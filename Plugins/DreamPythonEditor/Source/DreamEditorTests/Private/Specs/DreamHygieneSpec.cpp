// SPDX-License-Identifier: Apache-2.0
// Dream.Hygiene.Ledger: the tier-4 hygiene touches only buffers the tests made (reviews tests-r2-M1 and tests-r3, architect
// rulings on tests-M1). On the live tab, over two simulated passes, with the real classification: persist's
// SDreamTab::ApplyRecoveryForTest goes through ApplyRestore, so its opens are restore opens exactly as the startup restore's
// and Leave safe mode's are (SDreamTab::IsApplyingRestoreForTest):
//   pass A: a pending recovery with a developer's name brought back that way is not written to the ledger, and the per-test
//     close leaves its tab open; a scratch tab the test opens itself is in the ledger; a pending leftover with the tests'
//     scratch name counts as a test buffer and is in the ledger;
//   pass B: the pass-start choice over the live pending recoveries, with the ledger on disk, takes the leftover and never
//     the developer's recovery; the pass-start cleanup's own discard then resolves the leftover (its text becomes a
//     "discarded" local-history entry), closes its tab and prunes it from the ledger, and the developer's recovery is still
//     pending and open.
// The spec resolves its own pending state: Restore, then a close (both write the real local history's restore and closed
// entries, as a user's actions do). A kill in the middle leaves the "my notes" recovery pending for good: by design the
// hygiene never touches a buffer that is not a test's.
//
// Dream.Hygiene.SettingsGuard: the pass-level user-settings guard (review r6 m2) on a real key, EchoFoldLines: a value
// persisted and written back before the check is no change, also when the key was missing at the pass start; a value
// left changed is flagged once, with the name the check was given, and put back in memory, in the config cache and in
// EditorSettings.ini on disk; the run's baseline file exists while the run lasts; the same value only present instead of
// missing is put back without an Error; a key the test opened with DreamTest::AllowSettingChange is neither flagged nor put
// back (review r7 M1, m1, m2). A test that changes keys as the View toggles and a theme pick do (each writes its key to
// disk at once), then restores them the same way before it ends, leaves the file as it was and the guard logs nothing
// (review r8). A planted leftover baseline (a run that did not finish) with another EchoFoldLines is put back with an
// Error and re-applied while EditorSettings.ini's Dream section is unchanged since it, and only listed in a warning when the
// ini was written after it (review r8 m1, m3). A leak a killed test wrote with SaveKey after the baseline was taken is put
// back with an Error: the run's own baseline file follows this editor's writes (UDreamUserSettings::OnFileWritten, review
// r9 M1, r10 m1), and a copy of it resolves as a killed run's would (on bAlwaysWriteRunLogs, a key no test opens).
#include "Support/DreamTestUtil.h"
#include "Support/DreamTestLogCapture.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Module/DreamEditorModule.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Persistence/DreamJournal.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

BEGIN_DEFINE_SPEC(FDreamHygieneSpec, "Dream.Hygiene.Ledger", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> Planted;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamHygieneSpec)

void FDreamHygieneSpec::Define()
{
	AfterEach([this]()
	{
		if (Tab.IsValid())
		{
			for (const FGuid& Id : Planted)
			{
				if (Tab->GetPendingRecoveries().Contains(Id))
				{
					Tab->RestoreRecovered(Id, /*bAsNewTab*/ false);
				}
			}
		}
		for (const FGuid& Id : Planted)
		{
			DreamTest::CloseBuffer(Id);
		}
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		Planted.Reset();
		BufferIds.Reset();
		Tab.Reset();
	});

	LatentIt("records and closes only buffers the tests made; a restored pending recovery survives two passes (reviews tests-r2-M1, r3)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		if (!FDreamEditorModule::Get().GetJournal().IsOpen())
		{
			AddWarning(TEXT("NOT RUN: this editor's journal is not open (a second instance?), so nothing can be pending or discarded"));
			Done.Execute();
			return;
		}
		struct FState
		{
			FGuid Developer;
			FGuid Leftover;
			FGuid Own;
			TSet<FGuid> TabsBefore;
			TArray<FGuid> Discarded;
		};
		TSharedRef<FState> State = MakeShared<FState>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("pass A: a restore brings back a developer's recovery and a test leftover; the test opens its own tab"), [this, State]()
		{
			State->TabsBefore = DreamTest::SnapshotOpenTabs();
			FDreamRecoveredSession Session;
			Session.bPreviousExitClean = false;
			Session.PreviousEndUtc = FDateTime::UtcNow();
			auto Add = [&Session](const FString& DisplayName, const FString& Text) -> FGuid
			{
				FDreamRecoveredBuffer& Rec = Session.Buffers.AddDefaulted_GetRef();
				Rec.Id = FGuid::NewGuid();
				Rec.Kind = EDreamBufferKind::Scratch;
				Rec.DisplayName = DisplayName;
				Rec.CleanText = FString();
				Rec.RecoveredText = Text;
				Rec.bCounted = true;
				Rec.LastEditUtc = Session.PreviousEndUtc;
				Session.SessionTabOrder.Add(Rec.Id);
				Session.CountedBuffers += 1;
				return Rec.Id;
			};
			const FString Sfx = DreamTest::UniqueSuffix();
			State->Developer = Add(FString::Printf(TEXT("my notes %s"), *Sfx), FString::Printf(TEXT("developer_work_%s = 1\n"), *Sfx));
			State->Leftover = Add(DreamTest::ScratchName(TEXT("HygieneLeftover")), FString::Printf(TEXT("leftover_%s = 1\n"), *Sfx));
			Planted = { State->Developer, State->Leftover };
			TestFalse(TEXT("no restore is being applied before"), SDreamTab::IsApplyingRestoreForTest());
			Tab->ApplyRecoveryForTest(MoveTemp(Session));       // through ApplyRestore: these are restore opens
			TestFalse(TEXT("nor after"), SDreamTab::IsApplyingRestoreForTest());
			const TSharedPtr<FDreamBuffer> Own = DreamTest::OpenScratch(*this, *Tab, TEXT("hygiene_own = 1"), TEXT("Hygiene"));
			if (!Own.IsValid())
			{
				return false;
			}
			State->Own = Own->GetId();
			BufferIds.Add(State->Own);
			const TArray<FGuid> Pending = Tab->GetPendingRecoveries();
			return TestTrue(TEXT("the developer's recovery is pending"), Pending.Contains(State->Developer))
				&& TestTrue(TEXT("the leftover is pending"), Pending.Contains(State->Leftover));
		});
		Steps.Do(TEXT("pass A: the ledger and the per-test close"), [this, State]()
		{
			TestFalse(TEXT("the restored developer's recovery is not in the ledger"), DreamTest::HygieneForTest::IsInLedger(State->Developer));
			TestTrue(TEXT("the restored leftover with the tests' scratch name is"), DreamTest::HygieneForTest::IsInLedger(State->Leftover));
			TestTrue(TEXT("the tab the test opened itself is in the ledger"), DreamTest::HygieneForTest::IsInLedger(State->Own));
			TestTrue(TEXT("the tests' scratch name marks a test buffer"), DreamTest::IsTestScratchName(DreamTest::ScratchName(TEXT("X"))));
			TestFalse(TEXT("a developer's name does not"), DreamTest::IsTestScratchName(TEXT("my notes")));
			// The end of a test: what it opened closes, the restore's tabs stay (the pending leftover is kept out of this call:
			// the pass-start cleanup resolves it before closing, which pass B runs below).
			TSet<FGuid> Before = State->TabsBefore;
			Before.Add(State->Leftover);
			DreamTest::CloseTabsOpenedSince(Before);
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			TestTrue(TEXT("the developer's restored tab is still open after the per-test close"), Buffers.FindById(State->Developer).IsValid());
			TestTrue(TEXT("and still pending"), Tab->GetPendingRecoveries().Contains(State->Developer));
			TestFalse(TEXT("the tab the test opened is closed"), Buffers.FindById(State->Own).IsValid());
			return true;
		});
		Steps.Do(TEXT("pass B: the pass-start choice, then its discard"), [this, State]()
		{
			const TArray<FGuid> Chosen = DreamTest::HygieneForTest::SelectAtPassStart(Tab->GetPendingRecoveries());
			TestFalse(TEXT("the developer's recovery would not be discarded"), Chosen.Contains(State->Developer));
			TestTrue(TEXT("the test leftover would be"), Chosen.Contains(State->Leftover));
			State->Discarded = DreamTest::HygieneForTest::BeginPassStartDiscard(*Tab);
			TestFalse(TEXT("the discard leaves the developer's recovery alone"), State->Discarded.Contains(State->Developer));
			return TestTrue(TEXT("the discard takes the leftover"), State->Discarded.Contains(State->Leftover));
		});
		Steps.Poll(TEXT("pass B: the leftover's text reaches local history and its recovery resolves"), [this, State]()
		{
			return Tab->GetPendingRecoveries().Contains(State->Leftover) ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 20.0);
		Steps.Do(TEXT("pass B: its tab closes, the ledger is pruned, the developer's recovery stays"), [this, State]()
		{
			DreamTest::HygieneForTest::FinishPassStartDiscard(*Tab, State->Discarded);
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			TestFalse(TEXT("the leftover's scratch tab is closed"), Buffers.FindById(State->Leftover).IsValid());
			TestFalse(TEXT("and it left the ledger"), DreamTest::HygieneForTest::IsInLedger(State->Leftover));
			TestTrue(TEXT("the developer's recovery is still pending"), Tab->GetPendingRecoveries().Contains(State->Developer));
			TestTrue(TEXT("and its tab is open"), Buffers.FindById(State->Developer).IsValid());
			TestFalse(TEXT("and it is not in the ledger"), DreamTest::HygieneForTest::IsInLedger(State->Developer));
			return true;
		});
		Steps.Run(Done);
	});
}

BEGIN_DEFINE_SPEC(FDreamSettingsGuardSpec, "Dream.Hygiene.SettingsGuard", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamSettingsGuardSpec)

void FDreamSettingsGuardSpec::Define()
{
	It("flags a user setting left changed, with the test's name, and puts it back; a value written back is no change (review r6 m2)", [this]()
	{
		const FName Key = GET_MEMBER_NAME_CHECKED(UDreamUserSettings, EchoFoldLines);
		const FString Section = UDreamUserSettings::StaticClass()->GetPathName();
		const FString ConfigFile = UDreamUserSettings::StaticClass()->GetConfigName();
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		const int32 Original = Settings->EchoFoldLines;
		const FString Probe = TEXT("Dream.Hygiene.SettingsGuard probe");

		// Persisted for a step and written back before the check, as a spec that persists a setting does: no change.
		Settings->EchoFoldLines = Original + 7;
		UDreamUserSettings::SaveKey(Key);
		Settings->EchoFoldLines = Original;
		UDreamUserSettings::SaveKey(Key);
		if (!TestTrue(TEXT("the guard has its pass snapshot"), DreamTest::HygieneForTest::CheckUserSettings(Probe)))
		{
			return;
		}

		// Left changed: flagged once, with the name, and put back.
		AddExpectedError(FString::Printf(TEXT("user setting EchoFoldLines changed by %s: "), *Probe), EAutomationExpectedErrorFlags::Contains, 1);
		Settings->EchoFoldLines = Original + 7;
		UDreamUserSettings::SaveKey(Key);
		DreamTest::HygieneForTest::CheckUserSettings(Probe);
		TestEqual(TEXT("the value in memory is put back"), Settings->EchoFoldLines, Original);
		FString Stored;
		if (GConfig->GetString(*Section, *Key.ToString(), Stored, ConfigFile))
		{
			TestEqual(TEXT("the config cache holds the value again"), FCString::Atoi(*Stored), Original);
		}
		else
		{
			AddInfo(TEXT("the config cache had no EchoFoldLines key at the pass start, and has none again"));
		}
		// And the file on disk, read as the guard reads it.
		const FConfigBranch* Branch = GConfig->FindBranch(FName(*ConfigFile), ConfigFile);
		if (TestTrue(TEXT("EditorSettings.ini has a saved file"), Branch != nullptr && !Branch->IniPath.IsEmpty()))
		{
			FConfigFile Disk;
			Disk.Read(Branch->IniPath);
			FString OnDisk;
			if (Disk.GetString(*Section, *Key.ToString(), OnDisk))
			{
				TestEqual(TEXT("EditorSettings.ini on disk holds the value again"), FCString::Atoi(*OnDisk), Original);
			}
			else
			{
				AddInfo(TEXT("EditorSettings.ini on disk had no EchoFoldLines key at the pass start, and has none again"));
			}
		}

		TestTrue(TEXT("the run's baseline is kept in Saved/DreamTests/settings-snapshot.json while the run lasts"),
			FPaths::FileExists(DreamTest::HygieneForTest::SettingsSnapshotPath()));

		// The same value, only missing instead of present: put back without an Error. A missing key stands for a fresh
		// object's value, and EchoFoldLines' C++ default is 6.
		FString Cached;
		if (GConfig->GetString(*Section, *Key.ToString(), Cached, ConfigFile) && FCString::Atoi(*Cached) == 6 && Settings->EchoFoldLines == 6)
		{
			GConfig->RemoveKey(*Section, *Key.ToString(), ConfigFile);
			GConfig->Flush(/*bRemoveFromCache*/ false, ConfigFile);
			DreamTest::HygieneForTest::CheckUserSettings(Probe);
			FString Back;
			TestTrue(TEXT("a key removed while it held the fresh value is written back, silently"),
				GConfig->GetString(*Section, *Key.ToString(), Back, ConfigFile) && FCString::Atoi(*Back) == 6);
		}
		else
		{
			AddInfo(TEXT("the presence-only case needs EchoFoldLines=6 in EditorSettings.ini; not run on this profile"));
		}

		// Opened by the test: neither flagged nor put back.
		DreamTest::AllowSettingChange(*this, Key);
		Settings->EchoFoldLines = Original + 5;
		UDreamUserSettings::SaveKey(Key);
		DreamTest::HygieneForTest::CheckUserSettings(GetTestFullName());
		TestEqual(TEXT("a key the test opened is not put back"), Settings->EchoFoldLines, Original + 5);
		Settings->EchoFoldLines = Original;
		UDreamUserSettings::SaveKey(Key);   // this spec's own restore; the key stays open until the test ends
	});

	It("logs nothing for a test that changes keys, writes them, and restores and writes them again before it ends (review r8)", [this]()
	{
		const FString Section = UDreamUserSettings::StaticClass()->GetPathName();
		const FString ConfigFile = UDreamUserSettings::StaticClass()->GetConfigName();
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		static const EDreamViewSetting Views[] = { EDreamViewSetting::ReplTab, EDreamViewSetting::ProblemsTab, EDreamViewSetting::ReplPrompt,
			EDreamViewSetting::RunHeaders, EDreamViewSetting::StatusLine, EDreamViewSetting::LineNumbers };
		static const TCHAR* const Keys[] = { TEXT("bShowReplTab"), TEXT("bShowProblemsTab"), TEXT("bShowReplPrompt"), TEXT("bShowRunHeaders"),
			TEXT("bShowStatusLine"), TEXT("bShowLineNumbers"), TEXT("ThemeId") };
		bool Before[UE_ARRAY_COUNT(Views)];
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Views); ++Index)
		{
			Before[Index] = Settings->GetViewSetting(Views[Index]);
		}
		const FString ThemeBefore = Settings->ThemeId;
		const FString OtherTheme = ThemeBefore == TEXT("GitHub Light") ? FString(TEXT("Darcula")) : FString(TEXT("GitHub Light"));
		DreamTest::FDreamTestLogCapture Log;

		// Each setter writes its key to EditorSettings.ini at once, as Shell.Chrome's persist case does.
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Views); ++Index)
		{
			UDreamUserSettings::SetViewSetting(Views[Index], !Before[Index]);
		}
		UDreamUserSettings::SetThemeId(OtherTheme);
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Views); ++Index)
		{
			UDreamUserSettings::SetViewSetting(Views[Index], Before[Index]);
		}
		UDreamUserSettings::SetThemeId(ThemeBefore);

		// A burst of setter writes can leave the saved file several writes behind the cache, without a log line; said here as
		// an info. The guard writes the cache and reads the file until it settles.
		const FConfigBranch* Branch = GConfig->FindBranch(FName(*ConfigFile), ConfigFile);
		if (!TestTrue(TEXT("EditorSettings.ini has a saved file"), Branch != nullptr && !Branch->IniPath.IsEmpty()))
		{
			return;
		}
		{
			FConfigFile Early;
			Early.Read(Branch->IniPath);
			TArray<FString> Trailing;
			for (const TCHAR* Key : Keys)
			{
				FString Cached, OnDisk;
				GConfig->GetString(*Section, Key, Cached, ConfigFile);
				Early.GetString(*Section, Key, OnDisk);
				if (Cached != OnDisk)
				{
					Trailing.Add(Key);
				}
			}
			if (Trailing.Num() > 0)
			{
				AddInfo(FString::Printf(TEXT("right after the restore the file on disk still trailed the cache on %s"), *FString::Join(Trailing, TEXT(", "))));
			}
		}
		DreamTest::HygieneForTest::CheckUserSettings(GetTestFullName());
		TestEqual(TEXT("the guard logs nothing about the user settings"), Log.CountContaining(FName(TEXT("LogDream")), TEXT("user setting ")), 0);
		// After the guard's check the file on disk holds the cache's values.
		FConfigFile Disk;
		Disk.Read(Branch->IniPath);
		for (const TCHAR* Key : Keys)
		{
			FString Cached, OnDisk;
			const bool bCached = GConfig->GetString(*Section, Key, Cached, ConfigFile);
			const bool bOnDisk = Disk.GetString(*Section, Key, OnDisk);
			TestTrue(*FString::Printf(TEXT("%s on disk is the config cache's after the check (cache %s, disk %s)"), Key,
				bCached ? *Cached : TEXT("(no key)"), bOnDisk ? *OnDisk : TEXT("(no key)")), bCached == bOnDisk && Cached == OnDisk);
		}
	});

	It("puts a leftover baseline's value back with an Error while the ini is unchanged since, and only warns when it was written after (review r8 m1, m3)", [this]()
	{
		const FName Key = GET_MEMBER_NAME_CHECKED(UDreamUserSettings, EchoFoldLines);
		const FString Section = UDreamUserSettings::StaticClass()->GetPathName();
		const FString ConfigFile = UDreamUserSettings::StaticClass()->GetConfigName();
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		const int32 Original = Settings->EchoFoldLines;
		const int32 Planted = Original + 3;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("DreamTests") / TEXT("settings-snapshot-planted.json");
		int32 Broadcasts = 0;
		const FDelegateHandle Handle = Settings->OnSettingChanged().AddLambda([&Broadcasts, Key](UObject*, FPropertyChangedEvent& Event)
		{
			Broadcasts += Event.GetPropertyName() == Key ? 1 : 0;
		});
		DreamTest::FDreamTestLogCapture Log;

		// The ini written after the leftover: the difference is listed, nothing is written.
		AddExpectedMessagePlain(TEXT("but EditorSettings.ini was written after it; nothing restored"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		DreamTest::HygieneForTest::PlantLeftoverSnapshotForTest(Path, Key, FString::FromInt(Planted), /*bStampCurrentIni*/ false);
		DreamTest::HygieneForTest::ResolveLeftoverSnapshotForTest(Path);
		TestEqual(TEXT("an ini written after the leftover keeps its value"), Settings->EchoFoldLines, Original);
		TestEqual(TEXT("and the warning lists EchoFoldLines"), Log.CountContaining(FName(TEXT("LogDream")), TEXT("differs on EchoFoldLines")), 1);
		TestEqual(TEXT("nothing is re-applied"), Broadcasts, 0);
		TestFalse(TEXT("the planted file is gone"), FPaths::FileExists(Path));

		// The ini unchanged since: an Error, the leftover's value back in memory, in the cache and on disk, re-applied.
		AddExpectedErrorPlain(TEXT("user setting EchoFoldLines changed by an automation run that did not finish"), EAutomationExpectedErrorFlags::Contains, 1);
		DreamTest::HygieneForTest::PlantLeftoverSnapshotForTest(Path, Key, FString::FromInt(Planted), /*bStampCurrentIni*/ true);
		DreamTest::HygieneForTest::ResolveLeftoverSnapshotForTest(Path);
		TestEqual(TEXT("the leftover's value is back in memory"), Settings->EchoFoldLines, Planted);
		FString Cached;
		TestTrue(TEXT("and in the config cache"), GConfig->GetString(*Section, *Key.ToString(), Cached, ConfigFile) && FCString::Atoi(*Cached) == Planted);
		TestTrue(TEXT("the file settles on the cache"), DreamTest::HygieneForTest::SettleSettingsFileForTest());
		if (const FConfigBranch* Branch = GConfig->FindBranch(FName(*ConfigFile), ConfigFile))
		{
			FConfigFile Disk;
			Disk.Read(Branch->IniPath);
			FString OnDisk;
			TestTrue(TEXT("and on disk"), Disk.GetString(*Section, *Key.ToString(), OnDisk) && FCString::Atoi(*OnDisk) == Planted);
		}
		TestTrue(TEXT("and re-applied through the settings' OnSettingChanged (ApplyRestoredSetting)"), Broadcasts >= 1);
		TestFalse(TEXT("the planted file is gone"), FPaths::FileExists(Path));

		Settings->OnSettingChanged().Remove(Handle);
		Settings->EchoFoldLines = Original;
		UDreamUserSettings::SaveKey(Key);   // this spec's own restore
	});

	LatentIt("puts back with an Error a leak a killed test wrote with SaveKey: the run's baseline file follows this editor's writes (review r9 M1, r10 m1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FState
		{
			bool bOriginal = false;
			bool bKeyBefore = false;     // the key was in the config before the leak (a fresh profile has none)
			FString TextBefore;          // its ini text then
			int32 Writes = 0;
			FDelegateHandle Handle;
		};
		TSharedRef<FState> State = MakeShared<FState>();
		// A key no test opens with AllowSettingChange: the run's baseline keeps an opened key marked (EchoFoldLines is opened
		// by the first case of this spec), and a baseline left behind does not check a marked key.
		const FName Key = GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bAlwaysWriteRunLogs);
		const FString Section = UDreamUserSettings::StaticClass()->GetPathName();
		const FString ConfigFile = UDreamUserSettings::StaticClass()->GetConfigName();
		// A copy of the run's own baseline file as a kill would leave it (the live file stays the run's).
		const FString Copy = FPaths::ProjectSavedDir() / TEXT("DreamTests") / TEXT("settings-snapshot-killed.json");
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a test's leak written with SaveKey while the run's guard watches"), [State, Key, Section, ConfigFile]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bOriginal = Settings->bAlwaysWriteRunLogs;
			State->bKeyBefore = GConfig->GetString(*Section, *Key.ToString(), State->TextBefore, ConfigFile);
			State->Handle = UDreamUserSettings::OnFileWritten().AddLambda([State]() { ++State->Writes; });
			Settings->bAlwaysWriteRunLogs = !State->bOriginal;
			UDreamUserSettings::SaveKey(Key);   // the leak; then the run is "killed": nothing restores it
			return true;
		});
		Steps.Poll(TEXT("the leak reaches the file (OnFileWritten)"), [State]()
		{
			return State->Writes > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("the run's baseline file carries the new stamp; the next run resolves a copy of it"), [this, State, Key, Section, ConfigFile, Copy]()
		{
			// The product branch: the guard's OnFileWritten handler re-persisted the run's baseline with the stamp of the file
			// this editor just wrote (the guard's handler was bound at module start-up, before this case's counter).
			const FString Live = DreamTest::HygieneForTest::SettingsSnapshotPath();
			const FString Stamp = DreamTest::HygieneForTest::DreamSectionStampForTest();
			TestEqual(TEXT("the run's baseline file holds the Dream section's stamp as this editor wrote it"), DreamTest::HygieneForTest::SnapshotStampForTest(Live), Stamp);
			if (!TestTrue(TEXT("a copy of the run's baseline file"), IFileManager::Get().Copy(*Copy, *Live) == COPY_OK))
			{
				return false;
			}
			// The baseline's text as the guard shows it ("(no key)" on a profile without the key), then the leak's.
			AddExpectedErrorPlain(FString::Printf(TEXT("user setting bAlwaysWriteRunLogs changed by an automation run that did not finish (its baseline %s): %s -> %s"),
				*Copy, State->bKeyBefore ? *State->TextBefore : TEXT("(no key)"), State->bOriginal ? TEXT("False") : TEXT("True")),
				EAutomationExpectedErrorFlags::Contains, 1);
			DreamTest::HygieneForTest::ResolveLeftoverSnapshotForTest(Copy);
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			TestEqual(TEXT("the value before the leak is back in memory"), Settings->bAlwaysWriteRunLogs, State->bOriginal);
			// The config as it was: the same text, or no key when there was none.
			FString Cached;
			const bool bCached = GConfig->GetString(*Section, *Key.ToString(), Cached, ConfigFile);
			TestEqual(TEXT("the config cache has the key exactly when it had it before the leak"), bCached, State->bKeyBefore);
			if (bCached && State->bKeyBefore)
			{
				TestEqual(TEXT("and the same text"), Cached, State->TextBefore);
			}
			TestTrue(TEXT("the file settles on the cache"), DreamTest::HygieneForTest::SettleSettingsFileForTest());
			if (const FConfigBranch* Branch = GConfig->FindBranch(FName(*ConfigFile), ConfigFile))
			{
				FConfigFile Disk;
				Disk.Read(Branch->IniPath);
				FString OnDisk;
				const bool bOnDisk = Disk.GetString(*Section, *Key.ToString(), OnDisk);
				TestEqual(TEXT("EditorSettings.ini on disk has the key exactly when it had it before the leak"), bOnDisk, State->bKeyBefore);
				if (bOnDisk && State->bKeyBefore)
				{
					TestEqual(TEXT("and the same text"), OnDisk, State->TextBefore);
				}
			}
			TestFalse(TEXT("the copy is gone"), FPaths::FileExists(Copy));
			return true;
		});
		Steps.Finally(TEXT("restore"), [State, Key, Copy]()
		{
			UDreamUserSettings::OnFileWritten().Remove(State->Handle);
			IFileManager::Get().Delete(*Copy, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			if (Settings->bAlwaysWriteRunLogs != State->bOriginal)
			{
				Settings->bAlwaysWriteRunLogs = State->bOriginal;
				UDreamUserSettings::SaveKey(Key);
			}
		});
		Steps.Run(Done);
	});
}
