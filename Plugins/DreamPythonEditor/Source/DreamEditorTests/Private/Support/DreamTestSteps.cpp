// SPDX-License-Identifier: Apache-2.0
#include "Support/DreamTestSteps.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"   // (scratch) m8
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Persistence/DreamJournal.h"
#include "Python/DreamPython.h"
#include "Run/DreamRunService.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/DreamPerProjectUserSettings.h"   // (scratch) the per-project keys of the guard
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamTheme.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"

namespace DreamTest
{
	/** Owns a step list while it is polled; keeps itself alive until the last step has run and Done was executed. */
	class FDreamStepRunner : public TSharedFromThis<FDreamStepRunner>
	{
	public:
		FDreamStepRunner(FAutomationTestBase& InTest, TArray<FDreamSteps::FStep>&& InSteps, const FDoneDelegate& InDone, double InDeadline)
			: Test(InTest)
			, Steps(MoveTemp(InSteps))
			, Done(InDone)
			, DeadlineSeconds(InDeadline)
		{
		}

		void Start()
		{
			Self = AsShared();
			StartSeconds = FPlatformTime::Seconds();
			StepStartSeconds = StartSeconds;
			if (Tick(0.f))
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FDreamStepRunner::Tick));
			}
		}

	private:
		/** One poll pass: runs every step that completes this frame; true while a step waits for a later frame. */
		bool Tick(float /*DeltaTime*/)
		{
			while (Index < Steps.Num())
			{
				const double Now = FPlatformTime::Seconds();
				FDreamSteps::FStep& Step = Steps[Index];

				if (!bFailed && Now - StartSeconds > DeadlineSeconds)
				{
					Test.AddError(FString::Printf(TEXT("Dream Python Editor steps: deadline of %.0f s passed at step '%s'"), DeadlineSeconds, *Step.Name));
					bFailed = true;
				}
				if (bFailed && !Step.bAlways)
				{
					Advance(Now);
					continue;
				}

				const EStep Result = Step.Fn();
				if (Result == EStep::Wait)
				{
					if (!bFailed && Now - StepStartSeconds > Step.TimeoutSeconds)
					{
						Test.AddError(FString::Printf(TEXT("Dream Python Editor steps: '%s' timed out after %.1f s"), *Step.Name, Step.TimeoutSeconds));
						bFailed = true;
						Advance(Now);
						continue;
					}
					if (bFailed)
					{
						// A cleanup step never waits: it ran once.
						Advance(Now);
						continue;
					}
					return true;
				}
				if (Result == EStep::Fail)
				{
					if (!Test.HasAnyErrors())
					{
						Test.AddError(FString::Printf(TEXT("Dream Python Editor steps: '%s' failed"), *Step.Name));
					}
					bFailed = true;
				}
				Advance(Now);
			}

			Finish();
			return false;
		}

		void Advance(double Now)
		{
			++Index;
			StepStartSeconds = Now;
		}

		/** Tick returns false right after, which removes the ticker; the SP delegate pins this for the rest of the call. */
		void Finish()
		{
			Done.ExecuteIfBound();
			Self.Reset();
		}

		FAutomationTestBase& Test;
		TArray<FDreamSteps::FStep> Steps;
		FDoneDelegate Done;
		double DeadlineSeconds = StepsDeadlineSeconds;
		int32 Index = 0;
		double StartSeconds = 0.0;
		double StepStartSeconds = 0.0;
		bool bFailed = false;
		TSharedPtr<FDreamStepRunner> Self;
	};

	FDreamSteps::FDreamSteps(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}

	FDreamSteps& FDreamSteps::Poll(const FString& Name, TFunction<EStep()> Fn, double TimeoutSeconds)
	{
		Steps.Add(FStep{ Name, MoveTemp(Fn), TimeoutSeconds, false });
		return *this;
	}

	FDreamSteps& FDreamSteps::Do(const FString& Name, TFunction<bool()> Fn)
	{
		return Poll(Name, [Fn = MoveTemp(Fn)]() { return Fn() ? EStep::Next : EStep::Fail; });
	}

	FDreamSteps& FDreamSteps::Finally(const FString& Name, TFunction<void()> Fn)
	{
		Steps.Add(FStep{ Name, [Fn = MoveTemp(Fn)]() { Fn(); return EStep::Next; }, 1.0, true });
		return *this;
	}

	FDreamSteps& FDreamSteps::WaitFrames(int32 Frames)
	{
		TSharedRef<int32> Left = MakeShared<int32>(Frames);
		return Poll(FString::Printf(TEXT("wait %d frames"), Frames), [Left]()
		{
			return (*Left)-- <= 0 ? EStep::Next : EStep::Wait;
		}, 30.0);
	}

	void FDreamSteps::Run(const FDoneDelegate& Done)
	{
		TSharedRef<FDreamStepRunner> Runner = MakeShared<FDreamStepRunner>(Test, MoveTemp(Steps), Done, DeadlineSeconds);
		Runner->Start();
	}
}

// ---- Tier-4 hygiene ------------------------------------------------------------------------------------------------

namespace DreamTest
{
	namespace HygienePrivate
	{
		enum class EPhase : uint8 { NotStarted, WaitTab, Discarding, Done };
		EPhase Phase = EPhase::NotStarted;
		double StartedAt = 0.0;
		double DiscardedAt = 0.0;
		TArray<FGuid> Discarded;
		FTSTicker::FDelegateHandle Ticker;
		FDelegateHandle StartHandle;
		FDelegateHandle EndHandle;
		FDelegateHandle BeforeAllHandle;
		FDelegateHandle AfterAllHandle;
		FDelegateHandle FileWrittenHandle;
		FDelegateHandle OpenedHandle;
		TSet<FGuid> TabsAtTestStart;
		bool bInDreamTest = false;
		/** (scratch) Tabs the running test keeps open past its end (KeepTabOpenAfterTest). */
		TSet<FGuid> KeptOpen;
		/** (scratch, scratch-p1-m8) The session scratch folder the hooks set (no -DreamScratchFolder, no spec's override): a
		 *  pass started from the editor's automation window never writes into the user's real scratch folder. */
		bool bHooksSetScratchFolder = false;

		void EnsureTestScratchFolder()
		{
			if (DreamScratch::GetFolderForTest().IsEmpty() && !DreamScratch::IsCommandLineFolder())
			{
				DreamScratch::SetFolderForTest(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTests") / TEXT("Scratches")));
				bHooksSetScratchFolder = true;
			}
		}

		void ReleaseTestScratchFolder()
		{
			if (bHooksSetScratchFolder)
			{
				bHooksSetScratchFolder = false;
				DreamScratch::SetFolderForTest(FString());
			}
		}
		/** The ledger as the pass found it: buffers that Dream tests of earlier passes opened. */
		TSet<FGuid> Listed;
		/**
		 * Buffers a session restore opened (reviews tests-r2-M1, tests-r3 minor 1): every open inside SDreamTab::ApplyRestore
		 * (SDreamTab::IsApplyingRestoreForTest), which is the startup restore wherever it lands, Leave safe mode applying the
		 * skipped session, and persist's ApplyRecoveryForTest. They are the developer's tabs: never recorded in the ledger
		 * and never closed by the per-test close, unless they carry the tests' own scratch names.
		 */
		TSet<FGuid> RestoreOpened;

		// ---- The ledger (architect rulings on review tests-M1 and tests-r2-M1) ------------------------------------------
		// DreamHost's storage root is the developer's real one (no test profile), so a recovery of an interactive session
		// must never be touched. Every buffer a Dream test opens (after the restore) is appended at once, so a crash mid-test
		// keeps the record.

		FString LedgerPath()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("dream-tier4-buffers.txt"));
		}

		TArray<FString> ReadLedgerLines()
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *LedgerPath());
			return Lines;
		}

		TSet<FGuid> ReadLedger()
		{
			TSet<FGuid> Ids;
			for (const FString& Line : ReadLedgerLines())
			{
				FGuid Id;
				if (FGuid::Parse(Line.TrimStartAndEnd(), Id))
				{
					Ids.Add(Id);
				}
			}
			return Ids;
		}

		void AppendToLedger(const FGuid& Id)
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(LedgerPath()), /*Tree*/ true);
			FFileHelper::SaveStringToFile(Id.ToString(EGuidFormats::Digits) + TEXT("\n"), *LedgerPath(),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
		}

		/** Drops the given entries; entries this pass appended meanwhile stay for the next pass. */
		void RemoveFromLedger(const TSet<FGuid>& Ids)
		{
			if (Ids.Num() == 0)
			{
				return;
			}
			TArray<FString> Kept;
			for (const FString& Line : ReadLedgerLines())
			{
				FGuid Id;
				if (FGuid::Parse(Line.TrimStartAndEnd(), Id) && !Ids.Contains(Id))
				{
					Kept.AddUnique(Id.ToString(EGuidFormats::Digits));
				}
			}
			if (Kept.Num() == 0)
			{
				IFileManager::Get().Delete(*LedgerPath(), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
				return;
			}
			FFileHelper::SaveStringArrayToFile(Kept, *LedgerPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		/** A buffer the tests made: opened by a Dream test after the restore, or carrying the tests' scratch name. */
		bool IsTestOpened(const FGuid& Id)
		{
			if (FDreamEditorModule::IsAvailable())
			{
				const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Id);
				if (Buffer.IsValid() && DreamTest::IsTestScratchName(Buffer->GetDisplayName()))
				{
					return true;
				}
			}
			return !RestoreOpened.Contains(Id);
		}

		void HandleBufferOpened(const TSharedRef<FDreamBuffer>& Buffer)
		{
			const FGuid Id = Buffer->GetId();
			const bool bTestName = DreamTest::IsTestScratchName(Buffer->GetDisplayName());
			if (SDreamTab::IsApplyingRestoreForTest())
			{
				RestoreOpened.Add(Id);
				// A test scratch the restore brought back (a leftover of an earlier pass) is still a test buffer.
				if (bTestName)
				{
					AppendToLedger(Id);
				}
				return;
			}
			// Open at the test start (a tab the test closed and reopened, say) is not the test's buffer.
			if (bInDreamTest && !TabsAtTestStart.Contains(Id) && !RestoreOpened.Contains(Id))
			{
				AppendToLedger(Id);
			}
		}

		void BindLedger()
		{
			if (OpenedHandle.IsValid() || !FDreamEditorModule::IsAvailable())
			{
				return;
			}
			OpenedHandle = FDreamEditorModule::Get().GetBufferManager().OnBufferOpened().AddStatic(&HandleBufferOpened);
		}

		/**
		 * Test-start activation (coordinator, PO-04 restyle follow-up): after every test the automation controller reports the
		 * result through FMessageLog("AutomationTestingLog").Open() (AutomationControllerManager.cpp,
		 * ReportAutomationResult), which invokes the nomad "MessageLog" tab and activates its window. The next
		 * SetKeyboardFocus into the Dream tab's window then goes through that window's native activation, which restores the
		 * focus the window had when it lost activation over the focus the test just set. So before each Dream test the Dream
		 * tab's window is activated now, and that restore happens here instead of inside the test: with the OS focus in this
		 * editor through SWindow::BringToFront (SetActiveWindow); without it through the window's native SetWindowFocus
		 * (::SetFocus), the very call the test's own SetKeyboardFocus would make, which activates the window within this
		 * thread and never takes the foreground from another application. Both send WM_ACTIVATE before they return; the test
		 * body runs from a later automation tick.
		 */
		void ActivateDreamWindow()
		{
			if (!FDreamEditorModule::IsAvailable() || !FSlateApplication::IsInitialized())
			{
				return;
			}
			const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab();
			if (!Tab.IsValid())
			{
				return;
			}
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
			// No check of Slate's active window here: it lags behind the deferred WM_ACTIVATE (review r6 m6), and activating
			// the window that is already active changes nothing.
			if (!Window.IsValid() || !Window->IsVisible() || Window->IsWindowMinimized() || FSlateApplication::Get().GetActiveModalWindow() != nullptr)
			{
				return;
			}
			if (FPlatformApplicationMisc::IsThisApplicationForeground())
			{
				Window->BringToFront(/*bForce*/ true);
			}
			else if (const TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow())
			{
				Native->SetWindowFocus();
			}
		}

		// ---- The user-settings guard (review r6 m2, review r7) -------------------------------------------------------------
		// UDreamUserSettings is config=EditorSettings: the user-global EditorSettings.ini that every UE 5.8 project of this
		// user shares. Each automation run takes its own baseline at its first Dream test (the snapshot is dropped on
		// OnBeforeAllTestsEvent and OnAfterAllTestsEvent): every config key read twice, never from the object in memory,
		// from the config cache (GConfig) and from EditorSettings.ini on disk. The baseline is also written to
		// Saved/DreamTests/settings-snapshot.json (with a stamp of the ini's Dream section, at each persist, at each test end
		// and after each of this editor's own writes of the file during a Dream test: UDreamUserSettings::OnFileWritten, review
		// r9 M1) and deleted at the end of the run; a file left by a run that did not finish (a crash, a killed pass) is compared at
		// the next first Dream test: while the Dream section is unchanged since, every key whose value differs is logged at
		// Error and put back; when the ini was written after it, the differences are only listed in a warning. At each Dream
		// test end the cache is checked (and put back) first, then the file:
		//   - a value changed in the cache: Error "user setting <Key> changed by <test>: <old> -> <new>" (the entry lands in
		//     that test's report; check_automation_report.py fails the pass on it), then the key is put back in memory, in
		//     the cache and on disk, and what follows it is re-applied (View settings, theme, output position);
		//   - only the presence or the text changed (a key written back with the default value): put back silently;
		//   - the disk still holds another value than the cache after the settled read (ReadSettledDiskFile: the file can
		//     trail this editor's own writes, review r8) and this editor's flush succeeded: a warning with its own text, once
		//     per value, nothing adopted (the disk baseline only ever takes a value that agrees with the cache, so a test's
		//     temporary value never becomes one); when this editor's own flush keeps failing, a warning that says so;
		//   - a key the test opened with AllowSettingChange: neither flagged nor put back; its new value becomes the baseline
		//     (and a baseline file left behind by that run does not check it).
		// Values are compared as values: the ini text is imported into the property and exported again, and a missing key
		// stands for the value of a fresh object (the C++ constructor alone, as the defaults specs build it).

		/** One key as one source holds it. */
		struct FSettingSource
		{
			bool bPresent = false;
			FString Value;              // the ini value (an array's lines joined with newlines)
			TArray<FString> Lines;      // an array property's ini lines

			bool SameText(const FSettingSource& Other) const
			{
				return bPresent == Other.bPresent && Value == Other.Value;
			}
		};

		struct FSettingSnapshot
		{
			FName Name;
			bool bArray = false;
			bool bAllowed = false;      // a test of this run opened it with AllowSettingChange: a baseline left behind skips it
			FSettingSource Cache;       // GConfig
			FSettingSource Disk;        // EditorSettings.ini on disk
			FString FreshText;          // the value of a fresh object, as ExportText writes it
			FString ForeignDisk;        // the disk value last reported as another editor's (said once per value)
		};
		TArray<FSettingSnapshot> SettingsAtPassStart;
		bool bSettingsSnapshotTaken = false;
		/** The keys AllowSettingChange opened for the running Dream test (cleared at each Dream test start and end). */
		TSet<FName> AllowedKeys;

		FString SettingsSection()
		{
			return UDreamUserSettings::StaticClass()->GetPathName();
		}

		FString SettingsFile()
		{
			return UDreamUserSettings::StaticClass()->GetConfigName();
		}

		FString SnapshotFilePath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTests") / TEXT("settings-snapshot.json"));
		}

		/** The saved EditorSettings.ini as it is on disk now (empty when there is none). */
		void ReadSettingsFileFromDisk(FConfigFile& Out)
		{
			if (FConfigBranch* Branch = GConfig->FindBranch(FName(*SettingsFile()), SettingsFile()))
			{
				if (!Branch->IniPath.IsEmpty() && IFileManager::Get().FileExists(*Branch->IniPath))
				{
					Out.Read(Branch->IniPath);
				}
			}
		}

		/** Every config key's value on an object built by the C++ constructor alone (no archetype copy, no ini). */
		TMap<FName, FString> FreshValues()
		{
			TMap<FName, FString> Out;
			UClass* Class = UDreamUserSettings::StaticClass();
			UObject* Raw = StaticAllocateObject(Class, GetTransientPackage(),
				MakeUniqueObjectName(GetTransientPackage(), Class, FName(TEXT("DreamT4GuardFreshSettings"))), RF_Transient);
			(*Class->ClassConstructor)(FObjectInitializer(Raw, nullptr, EObjectInitializerOptions::None));
			for (TFieldIterator<FProperty> It(Class); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Config))
				{
					FString Text;
					It->ExportText_InContainer(0, Text, Raw, Raw, Raw, PPF_SerializedAsImportText);
					Out.Add(It->GetFName(), Text);
				}
			}
			return Out;
		}

		/** Sets the property value at ValuePtr to what S stands for (a missing key: the fresh value). Import messages go
		 *  nowhere: a malformed value is whatever it imports to. */
		void ImportSourceInto(const FProperty* Property, const FSettingSource& S, const FString& FreshText, void* ValuePtr)
		{
			FOutputDeviceNull Quiet;
			if (!S.bPresent)
			{
				Property->ImportText_Direct(*FreshText, ValuePtr, nullptr, PPF_SerializedAsImportText, &Quiet);
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(Array, ValuePtr);
				Helper.EmptyValues();
				for (const FString& Line : S.Lines)
				{
					const int32 Index = Helper.AddValue();
					Array->Inner->ImportText_Direct(*Line, Helper.GetRawPtr(Index), nullptr, PPF_SerializedAsImportText, &Quiet);
				}
			}
			else
			{
				Property->ImportText_Direct(*S.Value, ValuePtr, nullptr, PPF_SerializedAsImportText, &Quiet);
			}
		}

		/** The value S stands for, as the property exports it. */
		FString EffectiveValue(const FProperty* Property, const FSettingSource& S, const FString& FreshText)
		{
			void* Value = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			Property->InitializeValue(Value);
			ImportSourceInto(Property, S, FreshText, Value);
			FString Text;
			Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_SerializedAsImportText);
			Property->DestroyValue(Value);
			FMemory::Free(Value);
			return Text;
		}

		bool ValueChanged(const FProperty* Property, const FSettingSource& Before, const FSettingSource& Now, const FString& FreshText)
		{
			return !Now.SameText(Before) && EffectiveValue(Property, Now, FreshText) != EffectiveValue(Property, Before, FreshText);
		}

		FString Shown(const FSettingSource& S)
		{
			return S.bPresent ? S.Value : FString(TEXT("(no key)"));
		}

		FSettingSnapshot ReadSetting(const FProperty* Property, const FConfigFile& DiskFile, const FString& FreshText)
		{
			FSettingSnapshot S;
			S.Name = Property->GetFName();
			S.bArray = Property->IsA<FArrayProperty>();
			S.FreshText = FreshText;
			const FString Section = SettingsSection();
			if (S.bArray)
			{
				S.Cache.bPresent = GConfig->GetArray(*Section, *Property->GetName(), S.Cache.Lines, SettingsFile()) > 0;
				S.Cache.Value = FString::Join(S.Cache.Lines, TEXT("\n"));
				S.Disk.bPresent = DiskFile.GetArray(*Section, *Property->GetName(), S.Disk.Lines) > 0;
				S.Disk.Value = FString::Join(S.Disk.Lines, TEXT("\n"));
			}
			else
			{
				S.Cache.bPresent = GConfig->GetString(*Section, *Property->GetName(), S.Cache.Value, SettingsFile());
				S.Disk.bPresent = DiskFile.GetString(*Section, *Property->GetName(), S.Disk.Value);
			}
			return S;
		}

		/** True when the file agrees with this editor's cache on every Dream key, as values. */
		bool DiskAgreesWithCache(const FConfigFile& DiskFile, const TMap<FName, FString>& Fresh)
		{
			for (TFieldIterator<FProperty> It(UDreamUserSettings::StaticClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Config))
				{
					const FString& FreshText = Fresh.FindRef(It->GetFName());
					const FSettingSnapshot S = ReadSetting(*It, DiskFile, FreshText);
					if (ValueChanged(*It, S.Cache, S.Disk, FreshText))
					{
						return false;
					}
				}
			}
			return true;
		}

		enum class ESettledDisk : uint8
		{
			Agrees,         // the file agrees with the cache
			Differs,        // this editor's flush succeeded and the file still says otherwise: another writer
			WriteFailed,    // this editor's own flush kept failing
		};

		/** EditorSettings.ini as it is on disk; when it disagrees with this editor's cache on a Dream key, the cache is written
		 *  to it and it is read again, with a backoff (10 ms doubling to 250 ms) for about 1 s (review r8, r8 m2). A burst of
		 *  setter writes can leave the file several writes behind the cache: under automation the ini is written in place,
		 *  and while another process holds it without share-write the open fails and SaveKey drops the result; the next
		 *  successful flush catches up. That flush is no write a SaveKey would not make: UE 5.8's SaveBranch never clears the
		 *  branch's Dirty, so once anything changed every flush of this editor rewrites the whole file from its cache. */
		TUniquePtr<FConfigFile> ReadSettledDiskFile(const TMap<FName, FString>& Fresh, ESettledDisk& OutResult)
		{
			TUniquePtr<FConfigFile> DiskFile = MakeUnique<FConfigFile>();
			ReadSettingsFileFromDisk(*DiskFile);
			OutResult = ESettledDisk::Agrees;
			if (DiskAgreesWithCache(*DiskFile, Fresh))
			{
				return DiskFile;
			}
			bool bFlushed = false;
			double Delay = 0.01;
			double Waited = 0.0;
			for (;;)
			{
				bFlushed = GConfig->Flush(/*bRemoveFromCache*/ false, SettingsFile());
				DiskFile = MakeUnique<FConfigFile>();
				ReadSettingsFileFromDisk(*DiskFile);
				if (DiskAgreesWithCache(*DiskFile, Fresh))
				{
					return DiskFile;
				}
				if (Waited >= 1.0)
				{
					break;
				}
				FPlatformProcess::Sleep(static_cast<float>(Delay));
				Waited += Delay;
				Delay = FMath::Min(Delay * 2.0, 0.25);
			}
			OutResult = bFlushed ? ESettledDisk::Differs : ESettledDisk::WriteFailed;
			return DiskFile;
		}

		TArray<FSettingSnapshot> ReadSettings()
		{
			TArray<FSettingSnapshot> All;
			const TMap<FName, FString> Fresh = FreshValues();
			ESettledDisk Settled = ESettledDisk::Agrees;
			const TUniquePtr<FConfigFile> DiskFile = ReadSettledDiskFile(Fresh, Settled);
			for (TFieldIterator<FProperty> It(UDreamUserSettings::StaticClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Config))
				{
					All.Add(ReadSetting(*It, *DiskFile, Fresh.FindRef(It->GetFName())));
				}
			}
			return All;
		}

		// The baseline on disk (review r7 m2, r8 m1).

		/** The Dream section of EditorSettings.ini as it is on disk: a CRC of its non-empty lines ("" when there is no file).
		 *  Kept with the baseline at each persist; a leftover baseline is restored only while the section is still the same. */
		FString DreamSectionStampOnDisk()
		{
			const FConfigBranch* Branch = GConfig->FindBranch(FName(*SettingsFile()), SettingsFile());
			TArray<FString> Lines;
			if (Branch == nullptr || Branch->IniPath.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *Branch->IniPath))
			{
				return FString();
			}
			const FString Header = FString::Printf(TEXT("[%s]"), *SettingsSection());
			bool bInSection = false;
			FString Joined;
			for (const FString& Raw : Lines)
			{
				const FString Line = Raw.TrimStartAndEnd();
				if (Line.StartsWith(TEXT("[")))
				{
					bInSection = Line == Header;
					continue;
				}
				if (bInSection && !Line.IsEmpty())
				{
					Joined += Line;
					Joined += TEXT("\n");
				}
			}
			return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Joined));
		}

		TSharedRef<FJsonObject> SourceToJson(const FSettingSource& S)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("present"), S.bPresent);
			Obj->SetStringField(TEXT("value"), S.Value);
			TArray<TSharedPtr<FJsonValue>> Lines;
			for (const FString& Line : S.Lines)
			{
				Lines.Add(MakeShared<FJsonValueString>(Line));
			}
			Obj->SetArrayField(TEXT("lines"), Lines);
			return Obj;
		}

		FSettingSource SourceFromJson(const TSharedPtr<FJsonObject>* Obj)
		{
			FSettingSource S;
			if (Obj != nullptr && Obj->IsValid())
			{
				(*Obj)->TryGetBoolField(TEXT("present"), S.bPresent);
				(*Obj)->TryGetStringField(TEXT("value"), S.Value);
				const TArray<TSharedPtr<FJsonValue>>* Lines = nullptr;
				if ((*Obj)->TryGetArrayField(TEXT("lines"), Lines))
				{
					for (const TSharedPtr<FJsonValue>& Line : *Lines)
					{
						FString Text;
						if (Line.IsValid() && Line->TryGetString(Text))
						{
							S.Lines.Add(Text);
						}
					}
				}
			}
			return S;
		}

		void PersistSnapshotTo(const TArray<FSettingSnapshot>& Snapshots, const FString& Path, const FString& Stamp)
		{
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (const FSettingSnapshot& S : Snapshots)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), S.Name.ToString());
				Obj->SetBoolField(TEXT("array"), S.bArray);
				Obj->SetBoolField(TEXT("allowed"), S.bAllowed);
				Obj->SetObjectField(TEXT("cache"), SourceToJson(S.Cache));
				Obj->SetObjectField(TEXT("disk"), SourceToJson(S.Disk));
				Obj->SetStringField(TEXT("fresh"), S.FreshText);
				Keys.Add(MakeShared<FJsonValueObject>(Obj));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("version"), 2);
			Root->SetStringField(TEXT("section"), SettingsSection());
			Root->SetStringField(TEXT("iniDreamStamp"), Stamp);
			Root->SetArrayField(TEXT("keys"), Keys);
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
			FJsonSerializer::Serialize(Root, Writer);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
			if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogDream, Warning, TEXT("tests' settings guard: could not write its baseline to %s"), *Path);
			}
		}

		/** The run's baseline and the Dream section's stamp as the file is now. */
		void PersistSnapshot()
		{
			PersistSnapshotTo(SettingsAtPassStart, SnapshotFilePath(), DreamSectionStampOnDisk());
		}

		void DeleteSnapshotFile(const FString& Path)
		{
			IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}

		/** A baseline file a run that did not finish left behind, and the stamp of the Dream section it was taken with ("" in a
		 *  file older than the stamp), or false. Read with TryGet*, so an older or damaged file logs no LogJson error. */
		bool LoadSnapshotFile(const FString& Path, TArray<FSettingSnapshot>& Out, FString& OutStamp)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("keys"), Keys))
			{
				UE_LOG(LogDream, Warning, TEXT("tests' settings guard: the baseline %s left by an earlier run cannot be read; deleted"), *Path);
				DeleteSnapshotFile(Path);
				return false;
			}
			Root->TryGetStringField(TEXT("iniDreamStamp"), OutStamp);
			for (const TSharedPtr<FJsonValue>& Key : *Keys)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				FString Name;
				if (!Key.IsValid() || !Key->TryGetObject(Obj) || Obj == nullptr || !(*Obj)->TryGetStringField(TEXT("name"), Name))
				{
					continue;
				}
				FSettingSnapshot& S = Out.AddDefaulted_GetRef();
				S.Name = FName(*Name);
				(*Obj)->TryGetBoolField(TEXT("array"), S.bArray);
				(*Obj)->TryGetBoolField(TEXT("allowed"), S.bAllowed);
				const TSharedPtr<FJsonObject>* Cache = nullptr;
				const TSharedPtr<FJsonObject>* Disk = nullptr;
				(*Obj)->TryGetObjectField(TEXT("cache"), Cache);
				(*Obj)->TryGetObjectField(TEXT("disk"), Disk);
				S.Cache = SourceFromJson(Cache);
				S.Disk = SourceFromJson(Disk);
				(*Obj)->TryGetStringField(TEXT("fresh"), S.FreshText);
			}
			return true;
		}

		// Putting a key back.

		/** What follows the key after a restore, as an edit in Editor Preferences or the View menu would move it: the
		 *  settings' OnSettingChanged (the tab follows OutputPosition and bEchoInput there), the theme, the View settings'
		 *  broadcast, the output position of the open tab (review r7 m4). */
		void ApplyRestoredSetting(FName Key)
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			if (FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), Key))
			{
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Settings->OnSettingChanged().Broadcast(Settings, Event);
			}
			const FName ThemeKey = GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId);
			if (Key == ThemeKey && !Settings->ThemeId.Equals(FDreamThemes::GetCurrentId(), ESearchCase::IgnoreCase))
			{
				FDreamThemes::SetCurrent(Settings->ThemeId, /*bPersist*/ false);
			}
			static const FName ViewKeys[] = {
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStatusLine),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplPrompt),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowReplTab),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProblemsTab),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowRunHeaders),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowLineNumbers),
				// (scratch, for sidebar, PO-05) the side bar's two View toggles follow a restore at once too.
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowProjectPanel),
				GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bShowStructurePanel) };
			for (const FName& View : ViewKeys)
			{
				if (Key == View)
				{
					UDreamUserSettings::OnViewSettingsChanged().Broadcast();
					break;
				}
			}
			if (Key == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition) && FDreamEditorModule::IsAvailable())
			{
				if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
				{
					Tab->SetOutputPosition(Settings->OutputPosition);
				}
			}
		}

		/** Puts one key back as Before's cache had it, in memory and in the cache, with one write: SaveKey for a scalar (its
		 *  checked write and retry, review r9 m1); for an array's lines or a missing key (the fresh value in memory), the
		 *  cache is set and flushed. A write that trails is caught by the next settled read. */
		void RestoreSetting(const FSettingSnapshot& Before)
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			const FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), Before.Name);
			if (Property == nullptr)
			{
				return;
			}
			ImportSourceInto(Property, Before.Cache, Before.FreshText, Property->ContainerPtrToValuePtr<void>(Settings));
			if (Before.Cache.bPresent && !Before.bArray)
			{
				UDreamUserSettings::SaveKey(Before.Name);
				return;
			}
			if (Before.Cache.bPresent)
			{
				GConfig->SetArray(*SettingsSection(), *Property->GetName(), Before.Cache.Lines, SettingsFile());
			}
			else
			{
				GConfig->RemoveKey(*SettingsSection(), *Property->GetName(), SettingsFile());
			}
			if (FConfigBranch* Branch = GConfig->FindBranch(FName(*SettingsFile()), SettingsFile()))
			{
				Branch->InMemoryFile.Dirty = true;
			}
			GConfig->Flush(/*bRemoveFromCache*/ false, SettingsFile());
		}

		/** A baseline left by a run that did not finish (Path), taken when the Dream section had Stamp. When the section is
		 *  still the same (EditorSettings.ini was not written since that run's last persist), every key whose value differs
		 *  now is logged at Error and put back; when it was written after (a user's change after the killed run, say), the
		 *  differences are listed in a warning and nothing is written (review r8 m1). A key that run's test had opened with
		 *  AllowSettingChange is left as it is. */
		void ResolveLeftoverSnapshot(const TArray<FSettingSnapshot>& Leftover, const FString& Stamp, const FString& Path)
		{
			FConfigFile DiskFile;
			ReadSettingsFileFromDisk(DiskFile);
			const TMap<FName, FString> Fresh = FreshValues();
			const bool bWrittenSince = Stamp.IsEmpty() || Stamp != DreamSectionStampOnDisk();
			TArray<TPair<FSettingSnapshot, FSettingSnapshot>> Differing;   // leftover, now
			for (FSettingSnapshot L : Leftover)
			{
				const FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), L.Name);
				if (L.bAllowed || Property == nullptr)
				{
					continue;
				}
				L.FreshText = Fresh.FindRef(L.Name);
				const FSettingSnapshot Now = ReadSetting(Property, DiskFile, L.FreshText);
				if (ValueChanged(Property, L.Cache, Now.Cache, L.FreshText))
				{
					Differing.Emplace(L, Now);
				}
				else if (!bWrittenSince && !Now.Cache.SameText(L.Cache))
				{
					RestoreSetting(L);   // the same value, only written differently or present instead of missing
				}
			}
			if (bWrittenSince)
			{
				if (Differing.Num() > 0)
				{
					TArray<FString> DifferenceLines;
					for (const TPair<FSettingSnapshot, FSettingSnapshot>& D : Differing)
					{
						DifferenceLines.Add(FString::Printf(TEXT("%s %s -> %s"), *D.Key.Name.ToString(), *Shown(D.Key.Cache), *Shown(D.Value.Cache)));
					}
					UE_LOG(LogDream, Warning, TEXT("tests' settings guard: the baseline %s left by an automation run that did not finish differs on %s, but EditorSettings.ini was written after it; nothing restored"),
						*Path, *FString::Join(DifferenceLines, TEXT(", ")));
				}
				return;
			}
			for (const TPair<FSettingSnapshot, FSettingSnapshot>& D : Differing)
			{
				UE_LOG(LogDream, Error, TEXT("user setting %s changed by an automation run that did not finish (its baseline %s): %s -> %s"),
					*D.Key.Name.ToString(), *Path, *Shown(D.Key.Cache), *Shown(D.Value.Cache));
				RestoreSetting(D.Key);
				ApplyRestoredSetting(D.Key.Name);
			}
		}

		// ---- (scratch, PO-05) The per-project keys the guard covers too ----------------------------------------------------
		// UDreamPerProjectUserSettings is DreamHost's own per-project ini, and most of it is session state that changes on purpose
		// during a pass (OpenTabs, ActiveTab, ...). The keys below are user choices: each Dream test end compares them with the
		// run's baseline (the exported value of the object in memory), logs the same Error line as a user setting and puts the
		// value back (in memory, then SaveConfig). Looked up by name, so a key that does not exist yet is skipped.
		const TCHAR* const PerProjectKeys[] = { TEXT("ProjectRoots"), TEXT("bAddPluginPythonFolders") };   // (sidebar) PO-05-A1, review p3 m3
		TMap<FName, FString> PerProjectAtPassStart;
		bool bPerProjectSnapshotTaken = false;

		/** The roots' settings object: sidebar's UDreamProjectTreeSettings (R-p1-10), found by name so this file builds
		 *  before and after it lands; else UDreamPerProjectUserSettings, which held them before. */
		UObject* PerProjectSettingsObject()
		{
			if (UClass* TreeSettings = FindObject<UClass>(nullptr, TEXT("/Script/DreamEditor.DreamProjectTreeSettings")))
			{
				return TreeSettings->GetDefaultObject();
			}
			return UDreamPerProjectUserSettings::GetMutable();
		}

		const FProperty* FindPerProjectProperty(const TCHAR* Key)
		{
			UObject* Settings = PerProjectSettingsObject();
			return Settings != nullptr ? FindFProperty<FProperty>(Settings->GetClass(), Key) : nullptr;
		}

		FString ExportPerProject(const FProperty* Property)
		{
			const UObject* Settings = PerProjectSettingsObject();
			FString Text;
			Property->ExportText_InContainer(0, Text, Settings, nullptr, nullptr, PPF_None);
			return Text;
		}

		void SnapshotPerProject()
		{
			if (bPerProjectSnapshotTaken || !UObjectInitialized())
			{
				return;
			}
			bPerProjectSnapshotTaken = true;
			PerProjectAtPassStart.Reset();
			for (const TCHAR* Key : PerProjectKeys)
			{
				if (const FProperty* Property = FindPerProjectProperty(Key))
				{
					PerProjectAtPassStart.Add(FName(Key), ExportPerProject(Property));
				}
			}
		}

		void CheckPerProject(const FString& TestName)
		{
			if (!bPerProjectSnapshotTaken || !UObjectInitialized())
			{
				return;
			}
			bool bRestored = false;
			for (TPair<FName, FString>& Pair : PerProjectAtPassStart)
			{
				const FProperty* Property = FindPerProjectProperty(*Pair.Key.ToString());
				if (Property == nullptr)
				{
					continue;
				}
				const FString Now = ExportPerProject(Property);
				if (Now == Pair.Value)
				{
					continue;
				}
				if (AllowedKeys.Contains(Pair.Key))
				{
					Pair.Value = Now;   // the test changed it on purpose and restores it some other way
					continue;
				}
				UE_LOG(LogDream, Error, TEXT("user setting %s changed by %s: %s -> %s"), *Pair.Key.ToString(), *TestName, *Pair.Value, *Now);
				UObject* Mutable = PerProjectSettingsObject();
				Property->ImportText_InContainer(*Pair.Value, Mutable, Mutable, PPF_None);
				bRestored = true;
			}
			if (bRestored)
			{
				PerProjectSettingsObject()->SaveConfig();
			}
		}

		void SnapshotSettings()
		{
			SnapshotPerProject();   // (scratch)
			if (bSettingsSnapshotTaken || GConfig == nullptr)
			{
				return;
			}
			const FString Path = SnapshotFilePath();
			TArray<FSettingSnapshot> Leftover;
			FString Stamp;
			if (LoadSnapshotFile(Path, Leftover, Stamp))
			{
				ResolveLeftoverSnapshot(Leftover, Stamp, Path);
				DeleteSnapshotFile(Path);
			}
			SettingsAtPassStart = ReadSettings();
			bSettingsSnapshotTaken = true;
			PersistSnapshot();
		}

		/** OnBeforeAllTestsEvent and OnAfterAllTestsEvent: the next run takes its own baseline. At the end of a run that took
		 *  one, its file goes too. */
		void ResetSettingsSnapshot(bool bEndOfRun)
		{
			if (bEndOfRun && bSettingsSnapshotTaken)
			{
				DeleteSnapshotFile(SnapshotFilePath());
			}
			SettingsAtPassStart.Reset();
			bSettingsSnapshotTaken = false;
			AllowedKeys.Reset();
			PerProjectAtPassStart.Reset();   // (scratch)
			bPerProjectSnapshotTaken = false;
		}

		void AllowKey(FName Key)
		{
			AllowedKeys.Add(Key);
			for (FSettingSnapshot& S : SettingsAtPassStart)
			{
				if (S.Name == Key && !S.bAllowed)
				{
					S.bAllowed = true;
					PersistSnapshot();   // a test that ends the editor itself (the restart driver's steps) leaves it marked
				}
			}
		}

		void CheckSettings(const FString& TestName)
		{
			CheckPerProject(TestName);   // (scratch)
			if (!bSettingsSnapshotTaken || GConfig == nullptr)
			{
				return;
			}
			// 1. The config cache first (review r8 nit): a value a test left changed is flagged and put back, a key only
			// written differently is put back silently, and a key the test opened (review r7 M1) takes its new value as the
			// baseline. It stays marked in the file, so a run that ends the editor itself (the restart driver's steps, whose
			// driver restores the key) leaves no baseline that would blame the next run for it.
			const FConfigFile NoDisk;
			for (FSettingSnapshot& Before : SettingsAtPassStart)
			{
				const FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), Before.Name);
				if (Property == nullptr)
				{
					continue;
				}
				const FSettingSnapshot Now = ReadSetting(Property, NoDisk, Before.FreshText);
				if (AllowedKeys.Contains(Before.Name))
				{
					Before.Cache = Now.Cache;
					continue;
				}
				if (ValueChanged(Property, Before.Cache, Now.Cache, Before.FreshText))
				{
					UE_LOG(LogDream, Error, TEXT("user setting %s changed by %s: %s -> %s"), *Before.Name.ToString(), *TestName,
						*Shown(Before.Cache), *Shown(Now.Cache));
					RestoreSetting(Before);
					ApplyRestoredSetting(Before.Name);
				}
				else if (!Now.Cache.SameText(Before.Cache))
				{
					RestoreSetting(Before);   // the same value, only written differently or present instead of missing (m1)
				}
			}

			// 2. Then the file, settled against the cache (review r8). It is compared with the cache, never with an earlier
			// disk read, and only a value that agrees with the cache becomes the disk baseline, so a test's temporary value can
			// never become one.
			TMap<FName, FString> Fresh;
			for (const FSettingSnapshot& S : SettingsAtPassStart)
			{
				Fresh.Add(S.Name, S.FreshText);
			}
			ESettledDisk Settled = ESettledDisk::Agrees;
			const TUniquePtr<FConfigFile> DiskFile = ReadSettledDiskFile(Fresh, Settled);
			if (Settled == ESettledDisk::WriteFailed)
			{
				UE_LOG(LogDream, Warning, TEXT("tests' settings guard: this editor's write of EditorSettings.ini failed for about 1 s after %s (another process holds the file?); the file on disk is not compared this time"), *TestName);
			}
			else
			{
				for (FSettingSnapshot& Before : SettingsAtPassStart)
				{
					const FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), Before.Name);
					if (Property == nullptr)
					{
						continue;
					}
					const FSettingSnapshot Now = ReadSetting(Property, *DiskFile, Before.FreshText);
					if (AllowedKeys.Contains(Before.Name))
					{
						Before.Disk = Now.Disk;
						continue;
					}
					if (ValueChanged(Property, Now.Cache, Now.Disk, Before.FreshText))
					{
						// This editor's flush succeeded and the file still holds another value: another writer (an editor of
						// another project saving the shared file) keeps it so (review r7 m3). Not this test's doing; said once
						// per value, nothing adopted.
						const FString OnDisk = EffectiveValue(Property, Now.Disk, Before.FreshText);
						if (Before.ForeignDisk != OnDisk)
						{
							UE_LOG(LogDream, Warning, TEXT("user setting %s differs on disk from this editor's config cache after %s: cache %s, disk %s (another editor saved EditorSettings.ini?); left as it is"),
								*Before.Name.ToString(), *TestName, *Shown(Now.Cache), *Shown(Now.Disk));
							Before.ForeignDisk = OnDisk;
						}
						continue;
					}
					Before.ForeignDisk.Reset();
					Before.Disk = Now.Disk;   // the file agrees with the cache, which is the baseline
				}
			}
			// At each test end: the baseline and the Dream section's stamp as the file is now (review r8 m1).
			PersistSnapshot();
		}

		/** UDreamUserSettings::OnFileWritten (review r9 M1): this editor wrote the settings file (SaveKey, its retry). While a
		 *  Dream test runs, the stamp kept with the baseline follows, so a leak a test wrote to disk is still "not written
		 *  since" for the next run if this one is killed now, and is put back with an Error. A write by another process or the
		 *  engine's own save path leaves the stamp stale: the next run then only warns (the safe direction). */
		void HandleSettingsFileWritten()
		{
			if (bInDreamTest && bSettingsSnapshotTaken)
			{
				PersistSnapshot();
			}
		}

		bool CanTouchTabs()
		{
			return FDreamEditorModule::IsAvailable() && !FDreamEditorModule::Get().GetRunService().IsRunActive();
		}

		/** The pass-start choice: the pending recoveries of test buffers (Ledger, or the tests' scratch name); the others. */
		TArray<FGuid> SelectTestPending(const TArray<FGuid>& Pending, const TSet<FGuid>& Ledger, TSet<FGuid>& OutOthers)
		{
			TArray<FGuid> Tests;
			for (const FGuid& Id : Pending)
			{
				bool bTest = Ledger.Contains(Id);
				if (!bTest && FDreamEditorModule::IsAvailable())
				{
					const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Id);
					bTest = Buffer.IsValid() && DreamTest::IsTestScratchName(Buffer->GetDisplayName());
				}
				if (bTest)
				{
					Tests.Add(Id);
				}
				else
				{
					OutOthers.Add(Id);
				}
			}
			return Tests;
		}

		/** The pass-start discard: the test buffers' pending recoveries go through the tab's Discard; the other pending
		 *  recoveries leave the ledger. Returns the discarded ids. */
		TArray<FGuid> BeginDiscard(SDreamTab& Tab, const TSet<FGuid>& Ledger, int32& OutOthers)
		{
			TSet<FGuid> Others;
			const TArray<FGuid> Tests = SelectTestPending(Tab.GetPendingRecoveries(), Ledger, Others);
			RemoveFromLedger(Others);
			OutOthers = Others.Num();
			if (Tests.Num() > 0)
			{
				Tab.DiscardRecovered(Tests);
			}
			return Tests;
		}

		/** After the discard: the resolved scratch tabs close, and the processed entries leave the ledger (a recovery still
		 *  pending stays for the next pass). Returns the tabs closed. */
		int32 FinishDiscard(SDreamTab& Tab, const TArray<FGuid>& Tests, const TSet<FGuid>& Ledger)
		{
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			int32 Closed = 0;
			for (const FGuid& Id : Tests)
			{
				const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindById(Id);
				if (Buffer.IsValid() && !Buffer->HasPendingRecovery() && Buffer->GetKind() == EDreamBufferKind::Scratch)
				{
					Closed += Buffers.Close(Id) ? 1 : 0;
				}
			}
			TSet<FGuid> Processed = Ledger;
			Processed.Append(Tests);
			for (const FGuid& Id : Tab.GetPendingRecoveries())
			{
				Processed.Remove(Id);
			}
			RemoveFromLedger(Processed);
			return Closed;
		}

		bool Tick(float)
		{
			const double Now = FPlatformTime::Seconds();
			if (!FDreamEditorModule::IsAvailable())
			{
				Phase = EPhase::Done;
				return false;
			}
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			if (Module.GetPython().GetState() == EDreamPythonState::Headless)
			{
				Phase = EPhase::Done;     // no tab, no restore in a headless session
				return false;
			}
			if (!CanTouchTabs())
			{
				return true;
			}
			const TSharedPtr<SDreamTab> Tab = Module.GetTab();
			if (Phase == EPhase::WaitTab)
			{
				// The restore is applied when the journal has reconciled it (or the journal is off); 20 s at most.
				const bool bRestored = !Module.GetJournal().IsOpen() || Module.GetJournal().IsReconciled();
				if (!Tab.IsValid() || (!bRestored && Now - StartedAt < 20.0))
				{
					if (!Tab.IsValid() && Now - StartedAt > 20.0)
					{
						Phase = EPhase::Done;   // the pass never opened the tab: nothing was restored either
						return false;
					}
					return true;
				}
				// Only the pending recoveries of buffers the tests made; the developer's own sessions share this storage
				// root and their recoveries stay as they are, and leave the ledger if anything ever put them there.
				int32 Others = 0;
				Discarded = BeginDiscard(*Tab, Listed, Others);
				if (Discarded.Num() == 0)
				{
					UE_LOG(LogDream, Display, TEXT("Dream tests: no pending recovery of a test buffer (restore %s; %d listed, %d other pending left alone)."),
						bRestored ? TEXT("reconciled") : TEXT("not reconciled after 20 s"), Listed.Num(), Others);
					RemoveFromLedger(Listed);
					Phase = EPhase::Done;
					return false;
				}
				UE_LOG(LogDream, Display, TEXT("Dream tests: discarding %d pending recoveries of test buffers (their texts go to local history); %d other pending left alone."),
					Discarded.Num(), Others);
				DiscardedAt = Now;
				Phase = EPhase::Discarding;
				return true;
			}
			if (Phase == EPhase::Discarding)
			{
				const TArray<FGuid> Still = Tab.IsValid() ? Tab->GetPendingRecoveries() : TArray<FGuid>();
				const bool bResolved = !Discarded.ContainsByPredicate([&Still](const FGuid& Id) { return Still.Contains(Id); });
				if (!bResolved && Now - DiscardedAt < 20.0)
				{
					return true;
				}
				const int32 Closed = Tab.IsValid() ? FinishDiscard(*Tab, Discarded, Listed) : 0;
				UE_LOG(LogDream, Display, TEXT("Dream tests: %s; closed %d of their scratch tabs."),
					bResolved ? TEXT("every pending recovery of a test buffer was discarded") : TEXT("some could not be discarded within 20 s"), Closed);
				Phase = EPhase::Done;
				return false;
			}
			return false;
		}
	}

	bool IsTestScratchName(const FString& DisplayName)
	{
		// ScratchName: "DreamT4-<purpose>-<4 lower-case hex digits>" (the first four of UniqueSuffix).
		if (!DisplayName.StartsWith(TEXT("DreamT4-"), ESearchCase::CaseSensitive) || DisplayName.Len() < 8 + 1 + 1 + 4)
		{
			return false;
		}
		const FString Tail = DisplayName.Right(5);
		if (Tail[0] != TEXT('-'))
		{
			return false;
		}
		for (int32 Index = 1; Index < Tail.Len(); ++Index)
		{
			const TCHAR C = Tail[Index];
			if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	TSet<FGuid> SnapshotOpenTabs()
	{
		TSet<FGuid> Ids;
		if (FDreamEditorModule::IsAvailable())
		{
			if (const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab())
			{
				for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
				{
					Ids.Add(Buffer->GetId());
				}
			}
		}
		return Ids;
	}

	void CloseTabsOpenedSince(const TSet<FGuid>& Before)
	{
		if (!HygienePrivate::CanTouchTabs())
		{
			return;
		}
		const TSharedPtr<SDreamTab> Tab = FDreamEditorModule::Get().GetTab();
		if (!Tab.IsValid())
		{
			return;
		}
		FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
		bool bClosed = false;
		for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
		{
			// The tabs the session restore opened stay, whenever the restore landed (review tests-r2-M1); (scratch) and so do
			// the ones the test keeps open on purpose (KeepTabOpenAfterTest).
			if (!Before.Contains(Buffer->GetId()) && HygienePrivate::IsTestOpened(Buffer->GetId()) && !HygienePrivate::KeptOpen.Contains(Buffer->GetId()))
			{
				bClosed |= Buffers.Close(Buffer->GetId());
			}
		}
		if (bClosed)
		{
			// Automation Quit skips the debounced session save: write the tab list now, so the next start does not try to
			// reopen a test's (deleted) file.
			Tab->SaveSessionState(/*bNow*/ true);
		}
	}

	void StartPassCleanup()
	{
		using namespace HygienePrivate;
		if (Phase != EPhase::NotStarted || !FDreamEditorModule::IsAvailable())
		{
			return;
		}
		Phase = EPhase::WaitTab;
		StartedAt = FPlatformTime::Seconds();
		Listed = ReadLedger();
		Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&HygienePrivate::Tick), 0.1f);
	}

	bool IsPassCleanupDone()
	{
		return HygienePrivate::Phase == HygienePrivate::EPhase::Done || HygienePrivate::Phase == HygienePrivate::EPhase::NotStarted;
	}

	void AllowSettingChange(FAutomationTestBase& Test, FName Key)
	{
		if (Test.GetTestFullName().StartsWith(TEXT("Dream.")))
		{
			HygienePrivate::AllowKey(Key);
		}
	}

	void KeepTabOpenAfterTest(const FGuid& Id)
	{
		HygienePrivate::KeptOpen.Add(Id);   // (scratch)
	}

	void RegisterHygieneHooks()
	{
		using namespace HygienePrivate;
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
		// Only Dream's tests trigger anything: another automation run in this editor never discards or closes a tab.
		StartHandle = Framework.OnTestStartEvent.AddLambda([](FAutomationTestBase* Test)
		{
			if (Test != nullptr && Test->GetTestFullName().StartsWith(TEXT("Dream.")))
			{
				AllowedKeys.Reset();
				EnsureTestScratchFolder();   // (scratch) m8, before anything a test opens
				SnapshotSettings();
				StartPassCleanup();
				BindLedger();
				ActivateDreamWindow();
				TabsAtTestStart = SnapshotOpenTabs();
				bInDreamTest = true;
			}
		});
		EndHandle = Framework.OnTestEndEvent.AddLambda([](FAutomationTestBase* Test)
		{
			if (Test != nullptr && Test->GetTestFullName().StartsWith(TEXT("Dream.")))
			{
				bInDreamTest = false;
				CloseTabsOpenedSince(TabsAtTestStart);
				TabsAtTestStart.Reset();
				KeptOpen.Reset();   // (scratch)
				CheckSettings(Test->GetTestFullName());
				AllowedKeys.Reset();
			}
		});
		// Each automation run takes its own settings baseline (review r7 M2).
		BeforeAllHandle = Framework.OnBeforeAllTestsEvent.AddLambda([]() { ResetSettingsSnapshot(/*bEndOfRun*/ false); });
		AfterAllHandle = Framework.OnAfterAllTestsEvent.AddLambda([]()
		{
			ResetSettingsSnapshot(/*bEndOfRun*/ true);
			ReleaseTestScratchFolder();   // (scratch) the editor's own folder again after the run
		});
		// The baseline's stamp follows this editor's own writes of the settings file (review r9 M1).
		FileWrittenHandle = UDreamUserSettings::OnFileWritten().AddLambda([]() { HandleSettingsFileWritten(); });
	}

	void UnregisterHygieneHooks()
	{
		using namespace HygienePrivate;
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
		Framework.OnTestStartEvent.Remove(StartHandle);
		Framework.OnTestEndEvent.Remove(EndHandle);
		Framework.OnBeforeAllTestsEvent.Remove(BeforeAllHandle);
		Framework.OnAfterAllTestsEvent.Remove(AfterAllHandle);
		UDreamUserSettings::OnFileWritten().Remove(FileWrittenHandle);
		if (OpenedHandle.IsValid() && FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetBufferManager().OnBufferOpened().Remove(OpenedHandle);
		}
		OpenedHandle.Reset();
		if (Ticker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
			Ticker.Reset();
		}
	}

	namespace HygieneForTest
	{
		bool IsInLedger(const FGuid& Id)
		{
			return HygienePrivate::ReadLedger().Contains(Id);
		}

		TArray<FGuid> SelectAtPassStart(const TArray<FGuid>& Pending)
		{
			TSet<FGuid> Others;
			return HygienePrivate::SelectTestPending(Pending, HygienePrivate::ReadLedger(), Others);
		}

		TArray<FGuid> BeginPassStartDiscard(SDreamTab& Tab)
		{
			int32 Others = 0;
			return HygienePrivate::BeginDiscard(Tab, HygienePrivate::ReadLedger(), Others);
		}

		int32 FinishPassStartDiscard(SDreamTab& Tab, const TArray<FGuid>& Discarded)
		{
			return HygienePrivate::FinishDiscard(Tab, Discarded, TSet<FGuid>());
		}

		bool CheckUserSettings(const FString& TestName)
		{
			HygienePrivate::CheckSettings(TestName);
			return HygienePrivate::bSettingsSnapshotTaken;
		}

		FString SettingsSnapshotPath()
		{
			return HygienePrivate::SnapshotFilePath();
		}

		void PlantLeftoverSnapshotForTest(const FString& Path, FName Key, const FString& Value, bool bStampCurrentIni)
		{
			using namespace HygienePrivate;
			TArray<FSettingSnapshot> Planted = ReadSettings();
			for (FSettingSnapshot& Snapshot : Planted)
			{
				Snapshot.bAllowed = false;
				if (Snapshot.Name == Key)
				{
					Snapshot.Cache.bPresent = true;
					Snapshot.Cache.Value = Value;
					Snapshot.Cache.Lines.Reset();
					Snapshot.Disk = Snapshot.Cache;
				}
			}
			PersistSnapshotTo(Planted, Path, bStampCurrentIni ? DreamSectionStampOnDisk() : FString(TEXT("written-after")));
		}

		FString SnapshotStampForTest(const FString& Path)
		{
			TArray<HygienePrivate::FSettingSnapshot> Snapshots;
			FString Stamp;
			HygienePrivate::LoadSnapshotFile(Path, Snapshots, Stamp);
			return Stamp;
		}

		FString DreamSectionStampForTest()
		{
			return HygienePrivate::DreamSectionStampOnDisk();
		}

		bool SettleSettingsFileForTest()
		{
			using namespace HygienePrivate;
			ESettledDisk Settled = ESettledDisk::Agrees;
			ReadSettledDiskFile(FreshValues(), Settled);
			return Settled == ESettledDisk::Agrees;
		}

		void ResolveLeftoverSnapshotForTest(const FString& Path)
		{
			using namespace HygienePrivate;
			TArray<FSettingSnapshot> Leftover;
			FString Stamp;
			if (LoadSnapshotFile(Path, Leftover, Stamp))
			{
				ResolveLeftoverSnapshot(Leftover, Stamp, Path);
			}
			DeleteSnapshotFile(Path);
		}
	}
}
