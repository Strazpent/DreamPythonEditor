// SPDX-License-Identifier: Apache-2.0
// Dream.Buffers.*: the buffers half of the shell (slice B, group buffers). Files live in a private temp folder per case,
// removed afterwards; every buffer a case opens is closed again; settings a case changes are restored.
//  - Dream.Buffers.ExternalChange (T-PER-050, T-GAP-025, T-PER-017's reload entry, BUF-04): a clean file changed outside
//    Dream reloads within 2 s as one undo group with a `reload` history entry of the old text; Dream's own save raises no
//    external-change event, and its stamp goes once the save lands, so an external write of the saved bytes after another
//    external change still reloads (review shell-M1); a change made during a run applies only after the run; a dirty
//    buffer gets the pre-M2 bar [Keep mine] [Reload] (Keep mine then Save overwrites without a second bar; Reload is one
//    Ctrl+Z).
//  - Dream.Buffers.DeletedFile (T-PER-052): the deleted file's bar [Save] [Close]; Save recreates the file byte for byte
//    (BOM and CRLF kept); a file that comes back with the base bytes clears the bar.
//  - Dream.Buffers.SaveFailure (T-PER-047 subset, T-UEI-009 M1, BUF-05 M1, BUF-06): a share-deny handle on the target
//    (the original bytes stay, no temp file is left, the bar shows the OS error, Retry succeeds later); a read-only file
//    (the lock badge after a rescan, the bar offers only Make writable and Save As, Make writable saves); a character the
//    file's Latin-1 encoding cannot hold (the bar offers Save as UTF-8, which writes UTF-8 and rewrites the coding line;
//    after Ctrl+Z of that coding-line edit the save follows the latin-1 cookie again, review shell-M2); a UTF-16 file keeps
//    its encoding whatever its coding line says; a Latin-1 file whose coding line was deleted saves ASCII text as UTF-8
//    and offers Save as UTF-8 for other text (review r2).
//  - Dream.Buffers.Tabs (BUF-02 M1 badges as PO-04 draws them, BUF-03 M1 Recent Files, §6.4 Reopen tab): dirty "*", lock and recovered
//    badges and their widgets; Recent Files order, reopen and the missing-file case; a closed pseudo-file tab's link offers
//    Reopen tab and the click brings the buffer back with its ID.
//  - Dream.Buffers.RunLineHint (T-KEY-018's chip steps, RUN-26): the chip at the end of a statement-start caret line,
//    hidden while typing, on non-statement lines and while a popup is open; a click or the first RunLineAdvance hides it
//    for good.
//  - Dream.Buffers.Compaction (TDD 4.1.3, review r1 core-text minor): a buffer pushed past NeedsCompaction compacts after
//    the 2 s quiet period; text, version, dirty state, held snapshots and undo stay.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamFileWatch.h"
#include "Buffers/DreamHash.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/DreamUndoHistory.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLocalHistory.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Shell/SDreamEditorTabs.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamBuffersSpec
{
	FDreamBufferManager& Buffers()
	{
		return FDreamEditorModule::Get().GetBufferManager();
	}

	/** A fresh private folder under the user temp dir. */
	FString NewDir(const TCHAR* Purpose)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"),
			FString::Printf(TEXT("Buffers%s_%s"), Purpose, *DreamTest::UniqueSuffix())));
		FPaths::NormalizeDirectoryName(Dir);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	void RemoveDir(const FString& Dir)
	{
		if (Dir.IsEmpty())
		{
			return;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*"), /*Files*/ true, /*Directories*/ false);
		for (const FString& File : Files)
		{
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, false);
		}
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
	}

	bool WriteBytes(const FString& Path, const std::string& Bytes)
	{
		TArray<uint8> Data;
		Data.Append(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
		return FFileHelper::SaveArrayToFile(Data, *Path);
	}

	bool WriteUtf8(const FString& Path, const FString& Text)
	{
		return WriteBytes(Path, DreamText::ToUtf8(Text));
	}

	/** A UTF-16 LE file's bytes: the BOM, then each UTF-16 unit low byte first. */
	std::string Utf16LeWithBom(const FString& Text)
	{
		std::string Out("\xFF\xFE");
		for (const char16_t Unit : DreamText::ToU16(Text))
		{
			Out.push_back(static_cast<char>(Unit & 0xFF));
			Out.push_back(static_cast<char>(Unit >> 8));
		}
		return Out;
	}

	std::string ReadBytes(const FString& Path)
	{
		TArray<uint8> Data;
		if (!FFileHelper::LoadFileToArray(Data, *Path, FILEREAD_Silent))
		{
			return std::string("<unreadable>");
		}
		return std::string(reinterpret_cast<const char*>(Data.GetData()), static_cast<size_t>(Data.Num()));
	}

	FString ReadUtf8(const FString& Path)
	{
		return DreamText::Utf8ToFString(ReadBytes(Path));
	}

	/** "\xEF\xBB\xBF" style bytes as readable text for failure messages. */
	FString Escaped(const std::string& Bytes)
	{
		FString Out;
		for (const char C : Bytes)
		{
			const uint8 B = static_cast<uint8>(C);
			Out += (B >= 0x20 && B < 0x7F) ? FString::Chr(static_cast<TCHAR>(B)) : FString::Printf(TEXT("\\x%02X"), B);
		}
		return Out;
	}

	/** Replaces the whole text with one typed edit: the tab is dirty, its text differs from the disk. */
	bool SetText(FAutomationTestBase& Test, FDreamBuffer& Buffer, const FString& Text)
	{
		dream::EditBatch Batch;
		Batch.Origin = dream::EditOrigin::Typing;
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = 0;
		Range.DeleteLen = Buffer.GetModel().Length();
		Range.Insert = std::u16string(DreamText::ToU16(Text));
		const dream::ChangeSet Change = Buffer.ApplyEdit(Batch);
		return Test.TestFalse(TEXT("the edit applied"), Change.IsEmpty());
	}

	/** Opens Path in a tab, waits nothing; the buffer, or null with an error. */
	TSharedPtr<FDreamBuffer> OpenFile(FAutomationTestBase& Test, SDreamTab& Tab, const FString& Path)
	{
		if (!Tab.OpenLocation(Path, 0, 0))
		{
			Test.AddError(FString::Printf(TEXT("could not open %s in a tab"), *Path));
			return nullptr;
		}
		const TSharedPtr<FDreamBuffer> Buffer = Buffers().FindByPath(Path);
		if (!Buffer.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s is not open after OpenLocation"), *Path));
		}
		return Buffer;
	}

	FString DescribeBars(const SDreamTab& Tab)
	{
		TArray<FString> Parts;
		for (const FText& Message : Tab.GetFileBarMessagesForTest())
		{
			Parts.Add(FString::Printf(TEXT("\"%s\""), *Message.ToString()));
		}
		for (const FText& Label : Tab.GetFileBarActionsForTest())
		{
			Parts.Add(FString::Printf(TEXT("[%s]"), *Label.ToString()));
		}
		return Parts.Num() > 0 ? FString::Join(Parts, TEXT(" ")) : FString(TEXT("(no bar)"));
	}

	bool BarSays(const SDreamTab& Tab, const FString& Needle)
	{
		for (const FText& Message : Tab.GetFileBarMessagesForTest())
		{
			if (Message.ToString().Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}

	int32 FindAction(const SDreamTab& Tab, const FString& Label)
	{
		const TArray<FText> Labels = Tab.GetFileBarActionsForTest();
		for (int32 Index = 0; Index < Labels.Num(); ++Index)
		{
			if (Labels[Index].ToString() == Label)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool Press(FAutomationTestBase& Test, SDreamTab& Tab, const FString& Label)
	{
		const int32 Index = FindAction(Tab, Label);
		if (Index == INDEX_NONE)
		{
			Test.AddError(FString::Printf(TEXT("the file bar has no [%s]: %s"), *Label, *DescribeBars(Tab)));
			return false;
		}
		return Test.TestTrue(*FString::Printf(TEXT("[%s] ran"), *Label), Tab.InvokeFileBarActionForTest(Index));
	}

	void Collect(const TSharedRef<SWidget>& Root, TFunctionRef<bool(const TSharedRef<SWidget>&)> Match, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Match(Root))
		{
			Out.Add(Root);
		}
		if (FChildren* Children = Root->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				Collect(Children->GetChildAt(Index), Match, Out);
			}
		}
	}

	bool HasTag(const TSharedRef<SWidget>& Widget, const TCHAR* Tag)
	{
		const TSharedPtr<FTagMetaData> Meta = Widget->GetMetaData<FTagMetaData>();
		return Meta.IsValid() && Meta->Tag == FName(Tag);
	}

	/** The badge widget tagged Tag ("DreamTab.Dirty", ...) on the strip tab whose name label reads Name; null when none.
	 *  Bound attributes are cached per frame (TSlateAttribute), so callers read it a few frames after a change. */
	TSharedPtr<SWidget> FindTabBadge(SDreamTab& Tab, const FString& Name, const TCHAR* Tag)
	{
		TArray<TSharedRef<SWidget>> Strips;
		Collect(Tab.AsShared(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamEditorTabs")); }, Strips);
		for (const TSharedRef<SWidget>& Strip : Strips)
		{
			TArray<TSharedRef<SWidget>> Labels;
			Collect(Strip, [&Name](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == Name;
			}, Labels);
			for (const TSharedRef<SWidget>& Label : Labels)
			{
				const TSharedPtr<SWidget> Row = Label->GetParentWidget();   // the tab's SHorizontalBox: badges, name, close
				if (!Row.IsValid())
				{
					continue;
				}
				TArray<TSharedRef<SWidget>> Marks;
				Collect(Row.ToSharedRef(), [Tag](const TSharedRef<SWidget>& W) { return HasTag(W, Tag); }, Marks);
				if (Marks.Num() > 0)
				{
					return Marks[0];
				}
			}
		}
		return nullptr;
	}

	/** (chrome, tests review r6 m10) The strip tab's name label that reads Name; null when no tab shows that name. */
	TSharedPtr<SWidget> FindTabLabel(SDreamTab& Tab, const FString& Name)
	{
		TArray<TSharedRef<SWidget>> Strips;
		Collect(Tab.AsShared(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamEditorTabs")); }, Strips);
		for (const TSharedRef<SWidget>& Strip : Strips)
		{
			TArray<TSharedRef<SWidget>> Labels;
			Collect(Strip, [&Name](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == Name;
			}, Labels);
			if (Labels.Num() > 0)
			{
				return Labels[0];
			}
		}
		return nullptr;
	}

	/** (chrome) The widget right after the tab's name label in its row is the one tagged Tag (the dirty "*" follows the name). */
	bool IsRightAfterName(SDreamTab& Tab, const FString& Name, const TCHAR* Tag)
	{
		const TSharedPtr<SWidget> Label = FindTabLabel(Tab, Name);
		const TSharedPtr<SWidget> Row = Label.IsValid() ? Label->GetParentWidget() : nullptr;
		FChildren* Children = Row.IsValid() ? Row->GetChildren() : nullptr;
		for (int32 Index = 0; Children != nullptr && Index + 1 < Children->Num(); ++Index)
		{
			if (Children->GetChildAt(Index) == Label)
			{
				return HasTag(Children->GetChildAt(Index + 1), Tag);
			}
		}
		return false;
	}

	bool TabBadgeShown(SDreamTab& Tab, const FString& Name, const TCHAR* Tag)
	{
		const TSharedPtr<SWidget> Badge = FindTabBadge(Tab, Name, Tag);
		return Badge.IsValid() && Badge->GetVisibility().IsVisible();
	}

	FString TabBadgeText(SDreamTab& Tab, const FString& Name, const TCHAR* Tag)
	{
		const TSharedPtr<SWidget> Badge = FindTabBadge(Tab, Name, Tag);
		return Badge.IsValid() && Badge->GetType() == FName(TEXT("STextBlock")) ? StaticCastSharedPtr<STextBlock>(Badge)->GetText().ToString()
			: FString(TEXT("<no badge>"));
	}

	/** A failed save logs its "Not saved: ..." line at Warning (§10.14 step 5); the cases that fail a save on purpose expect it. */
	void ExpectSaveWarning(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessagePlain(TEXT("Not saved: "), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	}

	/** Polls Fn each frame; Next once it holds. */
	TFunction<DreamTest::EStep()> Until(TFunction<bool()> Fn)
	{
		return [Fn]() { return Fn() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; };
	}

	/** A step that lets the watch settle: no check queued or in flight for Seconds after it started. */
	void AddQuiet(DreamTest::FDreamSteps& Steps, const FString& Name, double Seconds)
	{
		TSharedRef<double> Start = MakeShared<double>(0.0);
		Steps.Poll(Name, [Start, Seconds]()
		{
			const double Now = FPlatformTime::Seconds();
			if (*Start == 0.0)
			{
				*Start = Now;
			}
			return Now - *Start >= Seconds && Buffers().GetFileWatch().IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, Seconds + 10.0);
	}

	/** Lets the directory watch of a just opened file arm (DirectoryWatcher adds requests at its next tick). */
	void AddArmWatch(DreamTest::FDreamSteps& Steps)
	{
		Steps.WaitFrames(5);
		AddQuiet(Steps, TEXT("the watch arms"), 0.3);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.ExternalChange
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersExternalSpec, "Dream.Buffers.ExternalChange", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	FString Path;
	FGuid BufferId;
	FDelegateHandle ReloadHandle;
	bool Prepare(const TCHAR* Purpose, const FString& Text);
END_DEFINE_SPEC(FDreamBuffersExternalSpec)

bool FDreamBuffersExternalSpec::Prepare(const TCHAR* Purpose, const FString& Text)
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid())
	{
		return false;
	}
	Dir = DreamBuffersSpec::NewDir(Purpose);
	Path = Dir / FString::Printf(TEXT("external_%s.py"), *DreamTest::UniqueSuffix());
	if (!TestTrue(TEXT("the fixture file was written"), DreamBuffersSpec::WriteUtf8(Path, Text)))
	{
		return false;
	}
	const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::OpenFile(*this, *Tab, Path);
	if (!Buffer.IsValid())
	{
		return false;
	}
	BufferId = Buffer->GetId();
	Path = Buffer->GetPath();
	Tab->ActivateBuffer(BufferId);
	return true;
}

void FDreamBuffersExternalSpec::Define()
{
	AfterEach([this]()
	{
		if (ReloadHandle.IsValid() && FDreamEditorModule::IsAvailable())
		{
			DreamBuffersSpec::Buffers().OnBufferReloaded().Remove(ReloadHandle);
		}
		ReloadHandle.Reset();
		DreamTest::CloseBuffer(BufferId);
		DreamBuffersSpec::RemoveDir(Dir);
		Dir.Reset();
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("reloads a clean file changed outside Dream Python Editor within 2 s as one undo group, with a reload history entry (T-PER-050, BUF-04)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const FString V1 = TEXT("value = 1\nkeep = 'same'\n");
		const FString V2 = TEXT("value = 2\nkeep = 'same'\nadded = True\n");
		if (!Prepare(TEXT("Clean"), V1))
		{
			Done.Execute();
			return;
		}
		struct FCase
		{
			double WrittenAt = 0.0;
			int32 ReloadsBefore = 0;
			bool bQueried = false;
			bool bHistoryDone = false;
			double NextQueryAt = 0.0;
			TOptional<FString> HistoryText;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("change the file outside Dream Python Editor"), [this, C, V2]()
		{
			C->ReloadsBefore = DreamBuffersSpec::Buffers().GetReloadCountForTest();
			C->WrittenAt = FPlatformTime::Seconds();
			return TestTrue(TEXT("the external write"), DreamBuffersSpec::WriteUtf8(Path, V2));
		});
		Steps.Poll(TEXT("the buffer shows the new text"), [this, V2]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			return Buffer.IsValid() && Buffer->GetTextAsString() == V2 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("check the latency and the clean state"), [this, C]()
		{
			const double Latency = FPlatformTime::Seconds() - C->WrittenAt;
			AddInfo(FString::Printf(TEXT("external change shown after %.0f ms"), Latency * 1000.0));
			TestTrue(*FString::Printf(TEXT("shown within 2 s while idle (BUF-04): %.0f ms"), Latency * 1000.0), Latency <= 2.0);
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			TestTrue(TEXT("the reloaded buffer is clean"), Buffer.IsValid() && !Buffer->IsDirty());
			TestEqual(TEXT("one reload"), DreamBuffersSpec::Buffers().GetReloadCountForTest(), C->ReloadsBefore + 1);
			TestEqual(TEXT("no bar for a clean reload"), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Do(TEXT("one Ctrl+Z restores the old text"), [this, V1]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(BufferId);
			if (!TestTrue(TEXT("the tab has its code view"), View.IsValid()) || !TestTrue(TEXT("the reload can be undone"), View->CanUndo()))
			{
				return false;
			}
			View->Undo();
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			TestEqual(TEXT("Ctrl+Z gives the text before the reload"), Buffer.IsValid() ? Buffer->GetTextAsString() : FString(), V1);
			TestTrue(TEXT("and the buffer differs from the disk now"), Buffer.IsValid() && Buffer->IsDirty());
			return true;
		});
		Steps.Poll(TEXT("the reload history entry holds the old text (T-PER-017)"), [this, C, V1]()
		{
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			if (!Module.GetJournal().IsOpen())
			{
				AddInfo(TEXT("the journal is not open in this editor (secondary instance): no local history to check"));
				return DreamTest::EStep::Next;
			}
			if (C->bHistoryDone)
			{
				if (!C->HistoryText.IsSet())
				{
					C->bHistoryDone = false;
					C->bQueried = false;     // not indexed yet: ask again in a moment
					C->NextQueryAt = FPlatformTime::Seconds() + 0.25;
					return DreamTest::EStep::Wait;
				}
				TestEqual(TEXT("the reload entry's text"), C->HistoryText.GetValue(), V1);
				return DreamTest::EStep::Next;
			}
			if (!C->bQueried && FPlatformTime::Seconds() >= C->NextQueryAt)
			{
				C->bQueried = true;
				Module.GetLocalHistory().Query(BufferId, [C](TArray<FDreamLocalHistoryEntry>&& Entries)
				{
					const FDreamLocalHistoryEntry* Reload = Entries.FindByPredicate([](const FDreamLocalHistoryEntry& E) { return E.Kind == EDreamHistoryKind::Reload; });
					if (Reload == nullptr || !FDreamEditorModule::IsAvailable())
					{
						C->bHistoryDone = true;
						return;
					}
					FDreamEditorModule::Get().GetLocalHistory().LoadText(*Reload, [C](TOptional<FString>&& Text)
					{
						C->HistoryText = MoveTemp(Text);
						C->bHistoryDone = true;
					});
				});
			}
			return DreamTest::EStep::Wait;
		}, 20.0);
		Steps.Run(Done);
	});

	LatentIt("detects an external write of bytes Dream Python Editor saved a moment ago, after another external change (review shell-M1, BUF-04)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// git commit, then checkout main, then checkout feature, all within the old 60 s stamp lifetime: the last write puts
		// back exactly the bytes Dream saved, and must count as an external change, not as Dream's own save.
		const FString Saved = TEXT("branch = 'feature'\n");
		const FString Other = TEXT("branch = 'main'\n");
		if (!Prepare(TEXT("Stamp"), TEXT("branch = 'start'\n")))
		{
			Done.Execute();
			return;
		}
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("edit and save"), [this, Buffer, Saved]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !DreamBuffersSpec::SetText(*this, *B, Saved))
			{
				return false;
			}
			Tab->ActivateBuffer(BufferId);
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("the landed save's stamp is gone"), [this, Saved]()
		{
			const std::string Bytes = DreamText::ToUtf8(Saved);
			TestFalse(TEXT("no expected self-write is left for the saved bytes"),
				FDreamFileWatch::IsExpectedStamp(Path, static_cast<int64>(Bytes.size()), DreamHash::Sha256Hex(Bytes)));
			return true;
		});
		DreamBuffersSpec::AddQuiet(Steps, TEXT("the watcher sees the save"), 1.0);
		Steps.Do(TEXT("another version arrives from outside"), [this, Other]() { return TestTrue(TEXT("written"), DreamBuffersSpec::WriteUtf8(Path, Other)); });
		Steps.Poll(TEXT("the clean buffer reloads it"), DreamBuffersSpec::Until([Buffer, Other]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && B->GetTextAsString() == Other; }), 10.0);
		Steps.Do(TEXT("the saved bytes come back from outside"), [this, Saved]() { return TestTrue(TEXT("written"), DreamBuffersSpec::WriteUtf8(Path, Saved)); });
		Steps.Poll(TEXT("they reload too (not taken for Dream's own save)"), DreamBuffersSpec::Until([Buffer, Saved]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && B->GetTextAsString() == Saved; }), 10.0);
		Steps.Do(TEXT("check"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("clean at the disk version"), B.IsValid() && !B->IsDirty());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("raises no external-change event for Dream's own save (T-PER-050, TDD 10.14 step 6)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)   // (tests) ASCII test name
	{
		const FString Saved = TEXT("saved_by_dream = 1\n");
		if (!Prepare(TEXT("Self"), TEXT("before = 0\n")))
		{
			Done.Execute();
			return;
		}
		TSharedRef<TPair<int32, int32>> Before = MakeShared<TPair<int32, int32>>(0, 0);
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("edit and save"), [this, Before, Saved]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			if (!Buffer.IsValid() || !DreamBuffersSpec::SetText(*this, *Buffer, Saved))
			{
				return false;
			}
			Before->Key = DreamBuffersSpec::Buffers().GetReloadCountForTest();
			Before->Value = DreamBuffersSpec::Buffers().GetDiskChangeCountForTest();
			Tab->ActivateBuffer(BufferId);
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until([this]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			return Buffer.IsValid() && !Buffer->IsDirty();
		}), 10.0);
		DreamBuffersSpec::AddQuiet(Steps, TEXT("the watcher sees the write"), 1.5);
		Steps.Do(TEXT("check that nothing reacted"), [this, Before, Saved]()
		{
			TestEqual(TEXT("the file holds the saved text"), DreamBuffersSpec::ReadUtf8(Path), Saved);
			TestEqual(TEXT("no reload"), DreamBuffersSpec::Buffers().GetReloadCountForTest(), Before->Key);
			TestEqual(TEXT("no external change"), DreamBuffersSpec::Buffers().GetDiskChangeCountForTest(), Before->Value);
			TestTrue(TEXT("no pending disk change"), DreamBuffersSpec::Buffers().GetDiskChange(BufferId) == nullptr);
			TestEqual(*FString::Printf(TEXT("no bar: %s"), *DreamBuffersSpec::DescribeBars(*Tab)), Tab->GetFileBarMessagesForTest().Num(), 0);
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			TestTrue(TEXT("the buffer stays clean"), Buffer.IsValid() && !Buffer->IsDirty());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("applies a change made during a run only after the run ends, within 2 s (T-PER-050, T-GAP-025, RT-01)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const FString V1 = TEXT("during = 1\n");
		const FString V2 = TEXT("during = 2\n");
		if (!DreamTest::RequireRunnablePython(*this) || !Prepare(TEXT("Run"), V1))
		{
			Done.Execute();
			return;
		}
		struct FCase
		{
			bool bReloadedDuringRun = false;
			double ReloadedAt = 0.0;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		const FGuid Id = BufferId;
		ReloadHandle = DreamBuffersSpec::Buffers().OnBufferReloaded().AddLambda([C, Id](const TSharedRef<FDreamBuffer>& Buffer)
		{
			if (Buffer->GetId() == Id)
			{
				C->bReloadedDuringRun |= FDreamEditorModule::Get().GetRunService().IsRunActive();
				C->ReloadedAt = FPlatformTime::Seconds();
			}
		});
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Code = FString::Join(TArray<FString>{
			FString::Printf(TEXT("import dream_editor as _se_%s"), *Sfx),
			FString::Printf(TEXT("with open(r'%s', 'w', encoding='utf-8', newline='') as _f_%s:"), *Path, *Sfx),
			FString::Printf(TEXT("    _f_%s.write('during = 2\\n')"), *Sfx),
			FString::Printf(TEXT("_se_%s.sleep(1.0)"), *Sfx),
			FString::Printf(TEXT("del _se_%s, _f_%s"), *Sfx, *Sfx),
		}, TEXT("\n"));
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamBuffersSpec::AddArmWatch(Steps);
		DreamTest::AddRun(Steps, TEXT("the run that rewrites the file"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run);
		Steps.Poll(TEXT("the change applies after the run"), [this, V2]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			return Buffer.IsValid() && Buffer->GetTextAsString() == V2 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("check when it applied"), [this, C, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestFalse(TEXT("nothing applied while the run was active (RT-01)"), C->bReloadedDuringRun);
			const double After = C->ReloadedAt - Run->Record.EndSeconds;
			AddInfo(FString::Printf(TEXT("applied %.0f ms after the run ended"), After * 1000.0));
			TestTrue(*FString::Printf(TEXT("applied within 2 s after the run (T-GAP-025): %.0f ms"), After * 1000.0), C->ReloadedAt > 0.0 && After <= 2.0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("gives a dirty buffer the [Keep mine] [Reload] bar; Keep mine then Save overwrites, Reload is one Ctrl+Z (TDD 10.15)",   // (tests) ASCII test name
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const FString Mine = TEXT("mine = 1\n");
		const FString Mine2 = TEXT("mine = 2\n");
		const FString Disk2 = TEXT("disk = 2\n");
		const FString Disk3 = TEXT("disk = 3\n");
		if (!Prepare(TEXT("Dirty"), TEXT("disk = 1\n")))
		{
			Done.Execute();
			return;
		}
		TSharedRef<int32> Changes = MakeShared<int32>(0);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("edit the tab, then change the file outside Dream Python Editor"), [this, Buffer, Mine, Disk2]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			return B.IsValid() && DreamBuffersSpec::SetText(*this, *B, Mine) && TestTrue(TEXT("the external write"), DreamBuffersSpec::WriteUtf8(Path, Disk2));
		});
		Steps.Poll(TEXT("the bar appears"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetDiskChange(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the bar"), [this, Buffer, Mine, Changes]()
		{
			Tab->ActivateBuffer(BufferId);
			TestTrue(*FString::Printf(TEXT("\"changed on disk.\": %s"), *DreamBuffersSpec::DescribeBars(*Tab)), DreamBuffersSpec::BarSays(*Tab, TEXT("changed on disk.")));
			TestTrue(TEXT("[Keep mine]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Keep mine")) != INDEX_NONE);
			TestTrue(TEXT("[Reload]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Reload")) != INDEX_NONE);
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestEqual(TEXT("the tab keeps its text"), B.IsValid() ? B->GetTextAsString() : FString(), Mine);
			*Changes = DreamBuffersSpec::Buffers().GetDiskChangeCountForTest();
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Keep mine"));
		});
		Steps.Do(TEXT("Keep mine, then Save"), [this, Buffer, Mine]()
		{
			TestTrue(TEXT("Keep mine clears the disk change"), DreamBuffersSpec::Buffers().GetDiskChange(BufferId) == nullptr);
			TestEqual(TEXT("and the bar"), Tab->GetFileBarMessagesForTest().Num(), 0);
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the tab stays dirty with its text"), B.IsValid() && B->IsDirty() && B->GetTextAsString() == Mine);
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		DreamBuffersSpec::AddQuiet(Steps, TEXT("the watcher sees the save"), 1.5);
		Steps.Do(TEXT("the save overwrote the file without a second bar"), [this, Mine, Changes]()
		{
			TestEqual(TEXT("the file holds the tab's text"), DreamBuffersSpec::ReadUtf8(Path), Mine);
			TestTrue(TEXT("no second bar"), DreamBuffersSpec::Buffers().GetDiskChange(BufferId) == nullptr);
			TestEqual(TEXT("no further external change"), DreamBuffersSpec::Buffers().GetDiskChangeCountForTest(), *Changes);
			return true;
		});
		Steps.Do(TEXT("edit again, then change the file again"), [this, Buffer, Mine2, Disk3]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			return B.IsValid() && DreamBuffersSpec::SetText(*this, *B, Mine2) && TestTrue(TEXT("the external write"), DreamBuffersSpec::WriteUtf8(Path, Disk3));
		});
		Steps.Poll(TEXT("the bar appears again"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetDiskChange(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("Reload"), [this]()
		{
			Tab->ActivateBuffer(BufferId);
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Reload"));
		});
		Steps.Do(TEXT("check the reload and its undo"), [this, Buffer, Mine2, Disk3]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestEqual(TEXT("Reload shows the disk text"), B.IsValid() ? B->GetTextAsString() : FString(), Disk3);
			TestTrue(TEXT("clean at the disk version"), B.IsValid() && !B->IsDirty());
			TestEqual(TEXT("no bar"), Tab->GetFileBarMessagesForTest().Num(), 0);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(BufferId);
			if (View.IsValid() && TestTrue(TEXT("Reload can be undone"), View->CanUndo()))
			{
				View->Undo();
				TestEqual(TEXT("one Ctrl+Z brings the tab's text back"), B.IsValid() ? B->GetTextAsString() : FString(), Mine2);
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.DeletedFile
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersDeletedSpec, "Dream.Buffers.DeletedFile", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	FString Path;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamBuffersDeletedSpec)

void FDreamBuffersDeletedSpec::Define()
{
	// A UTF-8 BOM and CRLF endings: Save recreates the file with the buffer's encoding and line endings (§10.15).
	static const std::string Original = std::string("\xEF\xBB\xBF") + "first = 1\r\nsecond = 2\r\n";

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			return;
		}
		Dir = DreamBuffersSpec::NewDir(TEXT("Deleted"));
		Path = Dir / FString::Printf(TEXT("deleted_%s.py"), *DreamTest::UniqueSuffix());
		if (TestTrue(TEXT("the fixture file was written"), DreamBuffersSpec::WriteBytes(Path, Original)))
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::OpenFile(*this, *Tab, Path))
			{
				BufferId = Buffer->GetId();
				Path = Buffer->GetPath();
				Tab->ActivateBuffer(BufferId);
			}
		}
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		DreamBuffersSpec::RemoveDir(Dir);
		Dir.Reset();
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("keeps a deleted file's tab open, dirty, with [Save] [Close]; Save recreates the file byte for byte (T-PER-052)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!BufferId.IsValid())
		{
			Done.Execute();
			return;
		}
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("delete the file outside Dream Python Editor"), [this]()
		{
			return TestTrue(TEXT("deleted"), IFileManager::Get().Delete(*Path));
		});
		Steps.Poll(TEXT("the buffer notices"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && B->IsDeletedOnDisk(); }), 10.0);
		Steps.Do(TEXT("check the deleted state and the bar"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the tab stays open and dirty"), B.IsValid() && B->IsDirty());
			TestEqual(TEXT("its text is kept"), B.IsValid() ? B->GetTextAsString() : FString(), FString(TEXT("first = 1\nsecond = 2\n")));
			Tab->ActivateBuffer(BufferId);
			TestTrue(*FString::Printf(TEXT("\"was deleted on disk.\": %s"), *DreamBuffersSpec::DescribeBars(*Tab)), DreamBuffersSpec::BarSays(*Tab, TEXT("was deleted on disk.")));
			TestTrue(TEXT("[Close]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Close")) != INDEX_NONE);
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Save"));
		});
		Steps.Poll(TEXT("Save recreates the file"), DreamBuffersSpec::Until([this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			return B.IsValid() && !B->IsDirty() && !B->IsDeletedOnDisk() && IFileManager::Get().FileExists(*Path);
		}), 10.0);
		Steps.Do(TEXT("check the bytes"), [this]()
		{
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("BOM and CRLF kept: %s"), *DreamBuffersSpec::Escaped(Bytes)), Bytes == Original);
			TestEqual(TEXT("the bar is gone"), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("clears the deleted state when the file comes back with the base bytes (TDD 10.15)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)   // (tests) ASCII test name
	{
		if (!BufferId.IsValid())
		{
			Done.Execute();
			return;
		}
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("delete the file outside Dream Python Editor"), [this]() { return TestTrue(TEXT("deleted"), IFileManager::Get().Delete(*Path)); });
		Steps.Poll(TEXT("the buffer notices"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && B->IsDeletedOnDisk(); }), 10.0);
		Steps.Do(TEXT("put the same bytes back"), [this]() { return TestTrue(TEXT("written"), DreamBuffersSpec::WriteBytes(Path, Original)); });
		Steps.Poll(TEXT("the deleted state clears"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDeletedOnDisk(); }), 10.0);
		Steps.Do(TEXT("check"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("clean again"), B.IsValid() && !B->IsDirty());
			Tab->ActivateBuffer(BufferId);
			TestEqual(*FString::Printf(TEXT("no bar: %s"), *DreamBuffersSpec::DescribeBars(*Tab)), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.SaveFailure
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersSaveFailureSpec, "Dream.Buffers.SaveFailure", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	FString Path;
	FGuid BufferId;
	TSharedPtr<IFileHandle> Holder;
	bool Prepare(const TCHAR* Purpose, const std::string& Bytes, const TCHAR* Extension);
END_DEFINE_SPEC(FDreamBuffersSaveFailureSpec)

bool FDreamBuffersSaveFailureSpec::Prepare(const TCHAR* Purpose, const std::string& Bytes, const TCHAR* Extension)
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid())
	{
		return false;
	}
	Dir = DreamBuffersSpec::NewDir(Purpose);
	Path = Dir / FString::Printf(TEXT("save_%s%s"), *DreamTest::UniqueSuffix(), Extension);
	if (!TestTrue(TEXT("the fixture file was written"), DreamBuffersSpec::WriteBytes(Path, Bytes)))
	{
		return false;
	}
	const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::OpenFile(*this, *Tab, Path);
	if (!Buffer.IsValid())
	{
		return false;
	}
	BufferId = Buffer->GetId();
	Path = Buffer->GetPath();
	Tab->ActivateBuffer(BufferId);
	return true;
}

void FDreamBuffersSaveFailureSpec::Define()
{
	AfterEach([this]()
	{
		Holder.Reset();
		DreamTest::CloseBuffer(BufferId);
		DreamBuffersSpec::RemoveDir(Dir);
		Dir.Reset();
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("keeps the target and the dirty buffer when the replace fails, shows the OS error with [Retry] [Save As...], and Retry saves later (T-PER-047)",   // (tests) ASCII test name
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(TEXT("Io"), "held = 1\n", TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		DreamBuffersSpec::ExpectSaveWarning(*this);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("hold the file without sharing, edit and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !DreamBuffersSpec::SetText(*this, *B, TEXT("held = 2\n")))
			{
				return false;
			}
			Holder = MakeShareable(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false));
			if (!TestTrue(TEXT("a share-deny handle on the target"), Holder.IsValid()))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save fails"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the failure"), [this, Buffer]()
		{
			const FDreamSaveFailureInfo* Failure = DreamBuffersSpec::Buffers().GetSaveFailure(BufferId);
			TestTrue(TEXT("an OS error"), Failure != nullptr && Failure->Kind == EDreamSaveFailure::Io);
			TestTrue(*FString::Printf(TEXT("\"Not saved: <OS error>\": %s"), *DreamBuffersSpec::DescribeBars(*Tab)), DreamBuffersSpec::BarSays(*Tab, TEXT("Not saved: ")));
			TestTrue(TEXT("[Retry]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Retry")) != INDEX_NONE);
			TestTrue(TEXT("[Save As…]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Save As…")) != INDEX_NONE);
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer stays dirty"), B.IsValid() && B->IsDirty());
			Holder.Reset();
			TestEqual(TEXT("the target is untouched"), DreamBuffersSpec::ReadUtf8(Path), FString(TEXT("held = 1\n")));
			TArray<FString> Left;
			IFileManager::Get().FindFiles(Left, *(Dir / TEXT("*.tmp")), /*Files*/ true, /*Directories*/ false);
			TestEqual(*FString::Printf(TEXT("no temp file is left: %s"), *FString::Join(Left, TEXT(", "))), Left.Num(), 0);
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Retry"));
		});
		Steps.Poll(TEXT("Retry saves"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("check the retry"), [this]()
		{
			TestEqual(TEXT("the file holds the new text"), DreamBuffersSpec::ReadUtf8(Path), FString(TEXT("held = 2\n")));
			TestTrue(TEXT("the failure is gone"), DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) == nullptr);
			TestEqual(TEXT("and its bar"), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("shows the lock for a read-only file and offers only Make writable and Save As; Make writable saves (T-UEI-009, BUF-05 M1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(TEXT("ReadOnly"), "locked = 1\n", TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		DreamBuffersSpec::ExpectSaveWarning(*this);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		DreamBuffersSpec::AddArmWatch(Steps);
		Steps.Do(TEXT("make the file read-only and rescan (focus return)"), [this]()
		{
			if (!TestTrue(TEXT("read-only now"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, true)))
			{
				return false;
			}
			DreamBuffersSpec::Buffers().GetFileWatch().QueueAll();
			return true;
		});
		Steps.Poll(TEXT("the lock badge shows"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && SDreamEditorTabs::GetBadges(*B).bLock; }), 10.0);
		Steps.Do(TEXT("only the lock changed; edit and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the text stays editable (§10.15 Read-only change)"), B.IsValid() && !B->IsReadOnly() && B->IsReadOnlyOnDisk());
			TestEqual(TEXT("no bar for an attribute change"), Tab->GetFileBarMessagesForTest().Num(), 0);
			if (!B.IsValid() || !DreamBuffersSpec::SetText(*this, *B, TEXT("locked = 2\n")))
			{
				return false;
			}
			Tab->ActivateBuffer(BufferId);
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save is refused"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the read-only bar"), [this]()
		{
			const FDreamSaveFailureInfo* Failure = DreamBuffersSpec::Buffers().GetSaveFailure(BufferId);
			TestTrue(TEXT("a read-only target"), Failure != nullptr && Failure->Kind == EDreamSaveFailure::ReadOnlyTarget);
			TestEqual(*FString::Printf(TEXT("Make writable and Save As only (Check out is M4): %s"), *DreamBuffersSpec::DescribeBars(*Tab)),
				Tab->GetFileBarActionsForTest().Num(), 2);
			TestTrue(TEXT("[Save As…]"), DreamBuffersSpec::FindAction(*Tab, TEXT("Save As…")) != INDEX_NONE);
			TestEqual(TEXT("the file is untouched"), DreamBuffersSpec::ReadUtf8(Path), FString(TEXT("locked = 1\n")));
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Make writable"));
		});
		Steps.Poll(TEXT("Make writable saves"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("check the save"), [this, Buffer]()
		{
			TestEqual(TEXT("the file holds the new text"), DreamBuffersSpec::ReadUtf8(Path), FString(TEXT("locked = 2\n")));
			TestFalse(TEXT("the file is writable now"), IFileManager::Get().IsReadOnly(*Path));
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("no lock badge"), B.IsValid() && !SDreamEditorTabs::GetBadges(*B).bLock);
			TestEqual(TEXT("no bar"), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("keeps a UTF-16 file UTF-16 on save although a coding line names latin-1 (review r2: the BOM wins, as at load)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const FString Original = TEXT("# -*- coding: latin-1 -*-\nx = 1\n");
		const FString Edited = TEXT("# -*- coding: latin-1 -*-\nx = 1\ny = '中'\n");
		if (!Prepare(TEXT("Utf16"), DreamBuffersSpec::Utf16LeWithBom(Original), TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("edit and save"), [this, Buffer, Original, Edited]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !TestTrue(TEXT("the UTF-16 file decodes"), B->GetTextAsString() == Original
				&& B->GetFormat().Enc == dream::codec::Encoding::Utf16LE))
			{
				return false;
			}
			if (!DreamBuffersSpec::SetText(*this, *B, Edited))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("still UTF-16 LE with its BOM"), [this, Buffer, Edited]()
		{
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("UTF-16 LE bytes with the BOM: %s"), *DreamBuffersSpec::Escaped(Bytes)), Bytes == DreamBuffersSpec::Utf16LeWithBom(Edited));
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer stays UTF-16 LE"), B.IsValid() && B->GetFormat().Enc == dream::codec::Encoding::Utf16LE && B->GetFormat().bBom);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("offers Save as UTF-8 when the coding line of a Latin-1 file with non-ASCII text is deleted (review r2, BUF-06)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		static const std::string Latin1 = "# -*- coding: latin-1 -*-\ns = 'caf\xE9'\n";
		if (!Prepare(TEXT("CookieGone"), Latin1, TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		DreamBuffersSpec::ExpectSaveWarning(*this);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("delete the coding line and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !DreamBuffersSpec::SetText(*this, *B, TEXT("s = 'café'\n")))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save is blocked"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the offer"), [this]()
		{
			const FDreamSaveFailureInfo* Failure = DreamBuffersSpec::Buffers().GetSaveFailure(BufferId);
			TestTrue(TEXT("an encoding refusal on line 1"), Failure != nullptr && Failure->Kind == EDreamSaveFailure::Encoding && Failure->BadLine == 1);
			TestTrue(*FString::Printf(TEXT("\"Save as UTF-8?\": %s"), *DreamBuffersSpec::DescribeBars(*Tab)), DreamBuffersSpec::BarSays(*Tab, TEXT("Save as UTF-8?")));
			TestTrue(TEXT("the file is untouched"), DreamBuffersSpec::ReadBytes(Path) == Latin1);
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Save as UTF-8"));
		});
		Steps.Poll(TEXT("Save as UTF-8 saves"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("UTF-8 bytes without a coding line"), [this, Buffer]()
		{
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("UTF-8: %s"), *DreamBuffersSpec::Escaped(Bytes)), Bytes == std::string("s = 'caf\xC3\xA9'\n"));
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer is UTF-8 now"), B.IsValid() && B->GetFormat().Enc == dream::codec::Encoding::Utf8);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("saves an ASCII-only Latin-1 file whose coding line was deleted as UTF-8, the same bytes (review r2, BUF-06)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(TEXT("CookieGoneAscii"), "# coding: latin-1\nx = 1\n", TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("delete the coding line and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !TestTrue(TEXT("a Latin-1 buffer"), B->GetFormat().Enc == dream::codec::Encoding::Latin1)
				|| !DreamBuffersSpec::SetText(*this, *B, TEXT("x = 1\n")))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("check"), [this, Buffer]()
		{
			TestTrue(TEXT("no refusal"), DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) == nullptr);
			TestTrue(TEXT("the ASCII bytes"), DreamBuffersSpec::ReadBytes(Path) == std::string("x = 1\n"));
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer is UTF-8 now, as a reader assumes without a coding line"), B.IsValid() && B->GetFormat().Enc == dream::codec::Encoding::Utf8);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("keeps the bytes and the coding line in agreement after Ctrl+Z of Save as UTF-8 (review shell-M2, BUF-06)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		static const std::string Latin1 = "# -*- coding: latin-1 -*-\ns = 'caf\xE9'\n";
		if (!Prepare(TEXT("Utf8Undo"), Latin1, TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		DreamBuffersSpec::ExpectSaveWarning(*this);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		auto Clean = [Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); };
		auto Undo = [this]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(BufferId);
			if (!TestTrue(TEXT("Ctrl+Z is possible"), View.IsValid() && View->CanUndo()))
			{
				return false;
			}
			View->Undo();
			return true;
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type a CJK character and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !DreamBuffersSpec::SetText(*this, *B, TEXT("# -*- coding: latin-1 -*-\ns = 'café'\nt = '中'\n")))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save is blocked"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("Save as UTF-8"), [this]() { return DreamBuffersSpec::Press(*this, *Tab, TEXT("Save as UTF-8")); });
		Steps.Poll(TEXT("the UTF-8 save completes"), DreamBuffersSpec::Until(Clean), 10.0);
		Steps.Do(TEXT("Ctrl+Z of the coding-line edit, then save"), [this, Buffer, Undo]()
		{
			if (!Undo())
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the coding line says latin-1 again"), B.IsValid() && B->GetTextAsString().StartsWith(TEXT("# -*- coding: latin-1 -*-\n")));
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save is blocked again"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the refusal, then Ctrl+Z of the CJK character and save"), [this, Undo]()
		{
			const FDreamSaveFailureInfo* Failure = DreamBuffersSpec::Buffers().GetSaveFailure(BufferId);
			TestTrue(TEXT("the latin-1 coding line cannot hold the CJK character: the UTF-8 offer again"),
				Failure != nullptr && Failure->Kind == EDreamSaveFailure::Encoding && Failure->BadLine == 3);
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("the UTF-8 file on disk is untouched: %s"), *DreamBuffersSpec::Escaped(Bytes)),
				Bytes == std::string("# -*- coding: utf-8 -*-\ns = 'caf\xC3\xA9'\nt = '\xE4\xB8\xAD'\n"));
			if (!Undo())
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save completes"), DreamBuffersSpec::Until(Clean), 10.0);
		Steps.Do(TEXT("the bytes follow the latin-1 coding line"), [this, Buffer]()
		{
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("Latin-1 bytes under the latin-1 cookie, as the file began: %s"), *DreamBuffersSpec::Escaped(Bytes)), Bytes == Latin1);
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer's encoding is Latin-1 again"), B.IsValid() && B->GetFormat().Enc == dream::codec::Encoding::Latin1);
			TestTrue(TEXT("no failure left"), DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) == nullptr);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("blocks a character the Latin-1 encoding cannot hold and offers Save as UTF-8, which rewrites the coding line (BUF-06, T-ENC-006)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(TEXT("Utf8"), "# -*- coding: latin-1 -*-\ns = 'caf\xE9'\n", TEXT(".py")))
		{
			Done.Execute();
			return;
		}
		DreamBuffersSpec::ExpectSaveWarning(*this);
		auto Buffer = [this]() { return DreamBuffersSpec::Buffers().FindById(BufferId); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type a CJK character and save"), [this, Buffer]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffer();
			if (!B.IsValid() || !TestEqual(TEXT("the Latin-1 file decodes"), B->GetTextAsString(), FString(TEXT("# -*- coding: latin-1 -*-\ns = 'café'\n"))))
			{
				return false;
			}
			if (!DreamBuffersSpec::SetText(*this, *B, TEXT("# -*- coding: latin-1 -*-\ns = 'café'\nt = '中'\n")))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save is blocked"), DreamBuffersSpec::Until([this]() { return DreamBuffersSpec::Buffers().GetSaveFailure(BufferId) != nullptr; }), 10.0);
		Steps.Do(TEXT("check the offer"), [this]()
		{
			const FDreamSaveFailureInfo* Failure = DreamBuffersSpec::Buffers().GetSaveFailure(BufferId);
			TestTrue(TEXT("an encoding refusal"), Failure != nullptr && Failure->Kind == EDreamSaveFailure::Encoding);
			TestEqual(TEXT("on line 3"), Failure != nullptr ? Failure->BadLine : 0, 3);
			TestTrue(*FString::Printf(TEXT("\"Save as UTF-8?\": %s"), *DreamBuffersSpec::DescribeBars(*Tab)), DreamBuffersSpec::BarSays(*Tab, TEXT("Save as UTF-8?")));
			TestTrue(TEXT("the file is untouched"), DreamBuffersSpec::ReadBytes(Path) == std::string("# -*- coding: latin-1 -*-\ns = 'caf\xE9'\n"));
			return DreamBuffersSpec::Press(*this, *Tab, TEXT("Save as UTF-8"));
		});
		Steps.Poll(TEXT("Save as UTF-8 saves"), DreamBuffersSpec::Until([Buffer]() { const TSharedPtr<FDreamBuffer> B = Buffer(); return B.IsValid() && !B->IsDirty(); }), 10.0);
		Steps.Do(TEXT("check the UTF-8 bytes"), [this, Buffer]()
		{
			const std::string Expected = "# -*- coding: utf-8 -*-\ns = 'caf\xC3\xA9'\nt = '\xE4\xB8\xAD'\n";
			const std::string Bytes = DreamBuffersSpec::ReadBytes(Path);
			TestTrue(*FString::Printf(TEXT("UTF-8 without BOM, the coding line rewritten: %s"), *DreamBuffersSpec::Escaped(Bytes)), Bytes == Expected);
			const TSharedPtr<FDreamBuffer> B = Buffer();
			TestTrue(TEXT("the buffer's encoding is UTF-8 now"), B.IsValid() && B->GetFormat().Enc == dream::codec::Encoding::Utf8);
			TestEqual(TEXT("no bar"), Tab->GetFileBarMessagesForTest().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.Tabs
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersTabsSpec, "Dream.Buffers.Tabs", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	TArray<FGuid> Opened;
	TArray<FString> SavedRecent;
	bool bRecentSaved = false;
END_DEFINE_SPEC(FDreamBuffersTabsSpec)

void FDreamBuffersTabsSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Dir = DreamBuffersSpec::NewDir(TEXT("Tabs"));
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : Opened)
		{
			DreamTest::CloseBuffer(Id);
		}
		Opened.Reset();
		if (bRecentSaved)
		{
			UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
			Settings->RecentFiles = SavedRecent;
			Settings->SaveConfig();
			bRecentSaved = false;
		}
		DreamBuffersSpec::RemoveDir(Dir);
		Dir.Reset();
		Tab.Reset();
	});

	LatentIt("shows the dirty, lock and recovered marks on their own tabs only and no language badge (BUF-02 M1, PO-04)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		struct FCase
		{
			FString ScratchName;
			FString TextName;
			FString LockedName;
			FString RecoveredName;
			FGuid ScratchId;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		const FString Sfx = DreamTest::UniqueSuffix();
		C->TextName = FString::Printf(TEXT("notes_%s.txt"), *Sfx);
		C->LockedName = FString::Printf(TEXT("locked_%s.py"), *Sfx);
		C->RecoveredName = DreamTest::ScratchName(TEXT("Recovered"));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open a scratch tab"), [this, C]()
		{
			const TSharedPtr<FDreamBuffer> Scratch = DreamTest::OpenScratch(*this, *Tab, TEXT("badge = 1"), TEXT("Badges"));
			if (!Scratch.IsValid())
			{
				return false;
			}
			Opened.Add(Scratch->GetId());
			C->ScratchId = Scratch->GetId();
			C->ScratchName = Scratch->GetDisplayName();
			const FDreamTabBadges Badges = SDreamEditorTabs::GetBadges(*Scratch);
			TestEqual(TEXT("a scratch tab is Python"), Badges.Language.ToString(), FString(TEXT("PY")));
			TestFalse(TEXT("clean"), Badges.bDirty);
			TestFalse(TEXT("no lock"), Badges.bLock);
			TestFalse(TEXT("not recovered"), Badges.bRecovered);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("its tab shows the plain name and nothing else; make it dirty"), [this, C]()
		{
			// (chrome) PO-04: no language badge on the tab (the badge data stays, GetBadges().Language). The tab is found first,
			// so the "no badge" checks below cannot pass on a missing tab.
			if (!TestTrue(TEXT("the scratch tab's name label is in the strip"), DreamBuffersSpec::FindTabLabel(*Tab, C->ScratchName).IsValid()))
			{
				return false;
			}
			TestFalse(TEXT("no language badge is painted (PO-04)"), DreamBuffersSpec::FindTabBadge(*Tab, C->ScratchName, TEXT("DreamTab.Language")).IsValid());
			TestFalse(TEXT("no dirty marker yet"), DreamBuffersSpec::TabBadgeShown(*Tab, C->ScratchName, TEXT("DreamTab.Dirty")));
			TestFalse(TEXT("no lock"), DreamBuffersSpec::TabBadgeShown(*Tab, C->ScratchName, TEXT("DreamTab.Lock")));
			TestFalse(TEXT("no recovered marker"), DreamBuffersSpec::TabBadgeShown(*Tab, C->ScratchName, TEXT("DreamTab.Recovered")));
			const TSharedPtr<FDreamBuffer> Scratch = DreamBuffersSpec::Buffers().FindById(C->ScratchId);
			return Scratch.IsValid() && DreamBuffersSpec::SetText(*this, *Scratch, TEXT("badge = 2"))
				&& TestTrue(TEXT("dirty after an edit"), SDreamEditorTabs::GetBadges(*Scratch).bDirty);
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the dirty marker follows; open a text file and a read-only file"), [this, C]()
		{
			TestTrue(TEXT("the dirty marker is painted on that tab"), DreamBuffersSpec::TabBadgeShown(*Tab, C->ScratchName, TEXT("DreamTab.Dirty")));
			TestEqual(TEXT("the dirty marker is a '*' (PO-04)"), DreamBuffersSpec::TabBadgeText(*Tab, C->ScratchName, TEXT("DreamTab.Dirty")),
				FString(TEXT("*")));
			TestTrue(TEXT("right after the name (PO-04)"), DreamBuffersSpec::IsRightAfterName(*Tab, C->ScratchName, TEXT("DreamTab.Dirty")));
			const FString TextPath = Dir / C->TextName;
			const FString LockedPath = Dir / C->LockedName;
			if (!DreamBuffersSpec::WriteUtf8(TextPath, TEXT("plain text\n")) || !DreamBuffersSpec::WriteUtf8(LockedPath, TEXT("locked = 1\n"))
				|| !TestTrue(TEXT("the read-only fixture"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*LockedPath, true)))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Text = DreamBuffersSpec::OpenFile(*this, *Tab, TextPath);
			const TSharedPtr<FDreamBuffer> Locked = DreamBuffersSpec::OpenFile(*this, *Tab, LockedPath);
			if (!Text.IsValid() || !Locked.IsValid())
			{
				return false;
			}
			Opened.Add(Text->GetId());
			Opened.Add(Locked->GetId());
			TestEqual(TEXT("another extension shows itself"), SDreamEditorTabs::GetBadges(*Text).Language.ToString(), FString(TEXT("TXT")));
			TestTrue(TEXT("a read-only file shows the lock"), SDreamEditorTabs::GetBadges(*Locked).bLock);
			TestFalse(TEXT("and stays editable"), Locked->IsReadOnly());
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("their tabs; add a (recovered) tab"), [this, C]()
		{
			if (!TestTrue(TEXT("the text file's and the read-only file's name labels are in the strip"),
				DreamBuffersSpec::FindTabLabel(*Tab, C->TextName).IsValid() && DreamBuffersSpec::FindTabLabel(*Tab, C->LockedName).IsValid()))
			{
				return false;
			}
			TestFalse(TEXT("no language badge on the text file's tab either (PO-04)"), DreamBuffersSpec::FindTabBadge(*Tab, C->TextName, TEXT("DreamTab.Language")).IsValid());
			TestFalse(TEXT("no lock on the writable file"), DreamBuffersSpec::TabBadgeShown(*Tab, C->TextName, TEXT("DreamTab.Lock")));
			TestTrue(TEXT("the lock is painted on the read-only file's tab"), DreamBuffersSpec::TabBadgeShown(*Tab, C->LockedName, TEXT("DreamTab.Lock")));
			TestFalse(TEXT("which is clean"), DreamBuffersSpec::TabBadgeShown(*Tab, C->LockedName, TEXT("DreamTab.Dirty")));
			const TSharedRef<FDreamBuffer> Recovered = DreamBuffersSpec::Buffers().AddRestored(FGuid::NewGuid(), EDreamBufferKind::Recovered,
				C->RecoveredName, FString(), TEXT("recovered = 1"), dream::codec::NewFileFormat(), TOptional<FDreamBaseVersion>());
			Opened.Add(Recovered->GetId());
			return TestTrue(TEXT("a (recovered) tab has the recovered marker"), SDreamEditorTabs::GetBadges(*Recovered).bRecovered);
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the recovered marker is painted on its tab only"), [this, C]()
		{
			TestTrue(TEXT("on the (recovered) tab"), DreamBuffersSpec::TabBadgeShown(*Tab, C->RecoveredName, TEXT("DreamTab.Recovered")));
			TestFalse(TEXT("not on the scratch tab"), DreamBuffersSpec::TabBadgeShown(*Tab, C->ScratchName, TEXT("DreamTab.Recovered")));
			return true;
		});
		Steps.Run(Done);
	});

	It("keeps Recent Files most recent first, opens one, and drops one that no longer exists (BUF-03 M1, T-GAP-002)", [this]()
	{
		if (!Tab.IsValid())
		{
			return;
		}
		FDreamBufferManager& Buffers = DreamBuffersSpec::Buffers();
		SavedRecent = UDreamPerProjectUserSettings::Get()->RecentFiles;
		bRecentSaved = true;
		const FString A = Dir / TEXT("recent_a.py");
		const FString B = Dir / TEXT("recent_b.py");
		if (!DreamBuffersSpec::WriteUtf8(A, TEXT("a = 1\n")) || !DreamBuffersSpec::WriteUtf8(B, TEXT("b = 1\n")))
		{
			AddError(TEXT("could not write the fixtures"));
			return;
		}
		const TSharedPtr<FDreamBuffer> BufA = DreamBuffersSpec::OpenFile(*this, *Tab, A);
		const TSharedPtr<FDreamBuffer> BufB = DreamBuffersSpec::OpenFile(*this, *Tab, B);
		if (!BufA.IsValid() || !BufB.IsValid())
		{
			return;
		}
		Opened.Add(BufA->GetId());
		Opened.Add(BufB->GetId());
		const FString PathA = BufA->GetPath();
		const FString PathB = BufB->GetPath();
		TArray<FString> Recent = Buffers.GetRecentFiles();
		TestTrue(TEXT("the last opened file is first"), Recent.Num() >= 2 && FPaths::IsSamePath(Recent[0], PathB) && FPaths::IsSamePath(Recent[1], PathA));

		Buffers.Close(BufA->GetId());
		Recent = Buffers.GetRecentFiles();
		TestTrue(TEXT("a closed file moves to the front"), Recent.Num() >= 1 && FPaths::IsSamePath(Recent[0], PathA));
		TestTrue(TEXT("Recent Files opens it again"), Tab->OpenRecentFile(PathA));
		const TSharedPtr<FDreamBuffer> Again = Buffers.FindByPath(PathA);
		TestTrue(TEXT("the file is open in a tab"), Again.IsValid());
		if (Again.IsValid())
		{
			Opened.Add(Again->GetId());
			TestTrue(TEXT("and it is the active tab"), Tab->GetActiveBuffer() == Again);
		}

		Buffers.Close(BufB->GetId());
		IFileManager::Get().Delete(*PathB);
		TestFalse(TEXT("a missing file does not open"), Tab->OpenRecentFile(PathB));
		Recent = Buffers.GetRecentFiles();
		TestFalse(TEXT("and leaves Recent Files"), Recent.ContainsByPredicate([&PathB](const FString& P) { return FPaths::IsSamePath(P, PathB); }));
		TestTrue(TEXT("never more than the cap"), Recent.Num() <= FDreamBufferManager::MaxRecentFiles);

		Buffers.ClearRecentFiles();
		TestEqual(TEXT("Clear Recent Files empties the list"), Buffers.GetRecentFiles().Num(), 0);
	});

	LatentIt("offers Reopen tab for a closed pseudo-file tab, and the click brings it back with its ID (TDD 6.4, B86)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)   // (tests) ASCII test name
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		FDreamBufferManager& Buffers = DreamBuffersSpec::Buffers();
		const FString Text = TEXT("first = 1\nsecond = 2\n");
		const TSharedPtr<FDreamBuffer> Scratch = DreamTest::OpenScratch(*this, *Tab, Text, TEXT("Reopen"));
		if (!Scratch.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Scratch->GetId();
		const FString Pseudo = Buffers.GetCompileFilename(*Scratch);
		TestTrue(*FString::Printf(TEXT("a pseudo filename: %s"), *Pseudo), Pseudo.StartsWith(TEXT("<dream:")));
		TestTrue(TEXT("a non-empty scratch tab is kept when it closes"), Buffers.Close(Id));
		TestFalse(TEXT("the pseudo file names no open tab"), Buffers.ResolveFilename(Pseudo).IsValid());
		const TSharedPtr<FDreamBuffer> Closed = Buffers.FindClosedByFilename(Pseudo);
		TestTrue(TEXT("but a closed one"), Closed.IsValid() && Closed->GetId() == Id);

		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		const FString Code = FString::Printf(TEXT("print('  File \"%s\", line 2')"), *Pseudo);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a run that prints a link to the closed tab"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run);
		Steps.Do(TEXT("show the Output view at its end"), [this]()
		{
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			if (!TestTrue(TEXT("the output area"), Area.IsValid()))
			{
				return false;
			}
			Area->SetActiveTab(EDreamOutputTab::Output);
			if (const TSharedPtr<SDreamOutputView> View = Area->GetOutputView())
			{
				View->ScrollToEnd();
			}
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("the link offers Reopen tab"), [this, Pseudo]()
		{
			TArray<TSharedRef<SWidget>> Links;
			DreamBuffersSpec::Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SHyperlink")); }, Links);
			int32 Matching = 0;
			int32 Enabled = 0;
			for (const TSharedRef<SWidget>& Link : Links)
			{
				TArray<TSharedRef<SWidget>> Texts;
				DreamBuffersSpec::Collect(Link, [&Pseudo](const TSharedRef<SWidget>& W)
				{
					return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Pseudo);
				}, Texts);
				if (Texts.Num() > 0)
				{
					++Matching;
					Enabled += Link->IsEnabled() ? 1 : 0;
				}
			}
			if (TestTrue(TEXT("the printed link is shown in the Output view"), Matching > 0))
			{
				TestEqual(TEXT("and enabled (Reopen tab, not the disabled closed-tab link)"), Enabled, Matching);
			}
			return true;
		});
		Steps.Do(TEXT("click it: the tab comes back with its ID and text"), [this, Id, Pseudo, Text]()
		{
			// What the link's click does (SDreamOutputView::OpenLink): FDreamEditorModule::OpenLocation at the frame's line.
			const FString Opened = FDreamEditorModule::Get().OpenLocation(Pseudo, 2, 0);
			TestEqual(TEXT("the reopened tab keeps its ID"), Opened, DreamText::GuidToHex(Id));
			const TSharedPtr<FDreamBuffer> Back = DreamBuffersSpec::Buffers().FindById(Id);
			if (!TestTrue(TEXT("the buffer is open again"), Back.IsValid()))
			{
				return false;
			}
			this->Opened.Add(Id);
			TestEqual(TEXT("with its text"), Back->GetTextAsString(), Text);
			TestEqual(TEXT("the pseudo filename names it again"), DreamBuffersSpec::Buffers().GetCompileFilename(*Back), Pseudo);
			TestTrue(TEXT("it is the active tab"), Tab->GetActiveBuffer() == Back);
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				TestEqual(TEXT("at the frame's line"), View->GetCaretLine(), 1);
			}
			return true;
		});
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.RunLineHint
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersHintSpec, "Dream.Buffers.RunLineHint", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	bool bSavedDismissed = false;
	bool bFlagSaved = false;
	TSharedPtr<IMenu> Menu;
	bool Prepare();
	TSharedPtr<SDreamCodeView> View() const { return Tab.IsValid() ? Tab->FindCodeView(BufferId) : nullptr; }
END_DEFINE_SPEC(FDreamBuffersHintSpec)

bool FDreamBuffersHintSpec::Prepare()
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid())
	{
		return false;
	}
	if (FDreamKeyRouter::Get().GetChordText(EDreamCommand::RunLineAdvance).IsEmpty())
	{
		AddError(TEXT("RunLineAdvance has no chord in this profile; the chip never shows then (T-KEY-018 needs the Charcoal default Alt+Enter)"));
		return false;
	}
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	bSavedDismissed = Settings->bRunLineAdvanceHintDismissed;
	bFlagSaved = true;
	Settings->bRunLineAdvanceHintDismissed = false;    // a fresh profile (T-KEY-018)
	// Line 0 a statement, line 1 blank, line 3 the continuation of the statement on line 2.
	const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("a = 1\n\nc = (1,\n     2)\n"), TEXT("Hint"));
	if (!Buffer.IsValid())
	{
		return false;
	}
	BufferId = Buffer->GetId();
	const TSharedPtr<SDreamCodeView> CodeView = View();
	if (!TestTrue(TEXT("the code view"), CodeView.IsValid()))
	{
		return false;
	}
	CodeView->FocusEditor();
	CodeView->SetCaret(0, 0);
	return true;
}

void FDreamBuffersHintSpec::Define()
{
	AfterEach([this]()
	{
		if (Menu.IsValid())
		{
			Menu->Dismiss();
			Menu.Reset();
		}
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		if (bFlagSaved)
		{
			UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
			Settings->bRunLineAdvanceHintDismissed = bSavedDismissed;
			Settings->SaveConfig();
			bFlagSaved = false;
		}
		Tab.Reset();
	});

	LatentIt("shows the chip at the end of a statement-start caret line, hides it while typing, elsewhere and under a popup, and a click hides it for good (T-KEY-018)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare())
		{
			Done.Execute();
			return;
		}
		auto Shown = [this]() { return Tab.IsValid() && Tab->IsRunLineHintShownForTest(); };
		TSharedRef<double> EditedAt = MakeShared<double>(0.0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Poll(TEXT("the chip shows on the fresh profile"), DreamBuffersSpec::Until(Shown), 10.0);
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check its text and place"), [this]()
		{
			const FString Text = Tab->GetRunLineHintTextForTest().ToString();
			const FString Chord = FDreamKeyRouter::Get().GetChordText(EDreamCommand::RunLineAdvance).ToString();
			TestTrue(*FString::Printf(TEXT("the text names the bound chord: %s"), *Text), Text.StartsWith(Chord) && Text.Contains(TEXT("runs this line and moves to the next")));
			TArray<TSharedRef<SWidget>> Chips;
			DreamBuffersSpec::Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return DreamBuffersSpec::HasTag(W, TEXT("DreamTab.RunLineHint")); }, Chips);
			const TSharedPtr<SDreamCodeView> CodeView = View();
			if (TestEqual(TEXT("one chip widget"), Chips.Num(), 1) && CodeView.IsValid())
			{
				const FSlateRect Caret = CodeView->GetCaretScreenRect();
				const FGeometry& Geometry = Chips[0]->GetCachedGeometry();
				const FVector2D ChipTopLeft = Geometry.GetAbsolutePosition();
				const double LineHeight = Caret.Bottom - Caret.Top;
				TestTrue(*FString::Printf(TEXT("right of the line's text (chip x %.0f, caret x %.0f)"), ChipTopLeft.X, Caret.Left), ChipTopLeft.X > Caret.Left + LineHeight);
				TestTrue(*FString::Printf(TEXT("on the caret line (chip y %.0f, caret y %.0f)"), ChipTopLeft.Y, Caret.Top), FMath::Abs(ChipTopLeft.Y - Caret.Top) <= LineHeight);
			}
			return true;
		});
		Steps.Do(TEXT("type on the line"), [this, EditedAt]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamBuffersSpec::Buffers().FindById(BufferId);
			if (!Buffer.IsValid())
			{
				return false;
			}
			dream::EditBatch Batch;
			Batch.Origin = dream::EditOrigin::Typing;
			dream::EditRange& Range = Batch.Ranges.emplace_back();
			Range.At = Buffer->Snapshot().LineEnd(0);
			Range.DeleteLen = 0;
			Range.Insert = u"2";
			Buffer->ApplyEdit(Batch);
			*EditedAt = FPlatformTime::Seconds();
			TestFalse(TEXT("hidden while typing"), Tab->IsRunLineHintShownForTest());
			return true;
		});
		Steps.Poll(TEXT("it comes back after a quiet second"), DreamBuffersSpec::Until(Shown), 10.0);
		Steps.Do(TEXT("not before the second passed"), [this, EditedAt]()
		{
			const double Quiet = FPlatformTime::Seconds() - *EditedAt;
			TestTrue(*FString::Printf(TEXT("at least 1 s after the edit: %.2f s"), Quiet), Quiet >= 0.95);
			View()->SetCaret(1, 0);
			TestFalse(TEXT("hidden on a blank line"), Tab->IsRunLineHintShownForTest());
			View()->SetCaret(3, 2);
			TestFalse(TEXT("hidden on a continuation line (not a statement start)"), Tab->IsRunLineHintShownForTest());
			View()->SetCaret(2, 0);
			TestTrue(TEXT("shown on the next statement start"), Tab->IsRunLineHintShownForTest());
			return true;
		});
		Steps.Do(TEXT("open a popup"), [this]()
		{
			Menu = FSlateApplication::Get().PushMenu(Tab.ToSharedRef(), FWidgetPath(), SNew(STextBlock).Text(FText::FromString(TEXT("popup"))),
				FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::None), /*bFocusImmediately*/ false);
			if (!TestTrue(TEXT("a popup menu"), Menu.IsValid()))
			{
				return false;
			}
			TestFalse(TEXT("hidden while a popup is open"), Tab->IsRunLineHintShownForTest());
			Menu->Dismiss();
			Menu.Reset();
			View()->FocusEditor();
			return true;
		});
		Steps.Poll(TEXT("shown again after the popup"), DreamBuffersSpec::Until(Shown), 10.0);
		Steps.Do(TEXT("click the chip"), [this]()
		{
			Tab->ClickRunLineHintForTest();
			TestFalse(TEXT("hidden after the click"), Tab->IsRunLineHintShownForTest());
			TestTrue(TEXT("for good: the flag is set"), UDreamPerProjectUserSettings::Get()->bRunLineAdvanceHintDismissed);
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("still hidden"), [this]()
		{
			TestFalse(TEXT("never again in this project"), Tab->IsRunLineHintShownForTest());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("hides the chip for good at the first RunLineAdvance (T-KEY-018, RUN-26)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!DreamTest::RequireRunnablePython(*this) || !Prepare())
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Poll(TEXT("the chip shows"), DreamBuffersSpec::Until([this]() { return Tab->IsRunLineHintShownForTest(); }), 10.0);
		Steps.Do(TEXT("RunLineAdvance"), [this, Run]()
		{
			Tab->ActivateBuffer(BufferId);
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::RunLineAdvance, Reason))
			{
				AddError(FString::Printf(TEXT("RunLineAdvance cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::RunLineAdvance);
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::GetActiveRunNumber();
			TestTrue(TEXT("the flag is set at acceptance"), UDreamPerProjectUserSettings::Get()->bRunLineAdvanceHintDismissed);
			return TestTrue(TEXT("a Line run was submitted"), Run->RunNumber > 0);
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the Line run"), Run);
		Steps.Do(TEXT("the chip never shows again"), [this, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			if (const TSharedPtr<SDreamCodeView> CodeView = View())
			{
				CodeView->FocusEditor();
				CodeView->SetCaret(0, 0);
			}
			TestFalse(TEXT("hidden for good"), Tab->IsRunLineHintShownForTest());
			return true;
		});
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.Compaction
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamBuffersCompactionSpec, "Dream.Buffers.Compaction", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> Opened;
END_DEFINE_SPEC(FDreamBuffersCompactionSpec)

void FDreamBuffersCompactionSpec::Define()
{
	AfterEach([this]()
	{
		for (const FGuid& Id : Opened)
		{
			DreamTest::CloseBuffer(Id);
		}
		Opened.Reset();
		Tab.Reset();
	});

	LatentIt("compacts a buffer pushed past NeedsCompaction within a few seconds of idle; text, version, snapshots and undo stay (TDD 4.1.3)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		struct FCase
		{
			TSharedPtr<FDreamBuffer> Buffer;
			TSharedPtr<FDreamUndoHistory> History;
			FString Expected;
			FString BeforeLast;
			uint64 Version = 0;
			bool bDirty = false;
			size_t PiecesBefore = 0;
			int32 CompactionsBefore = 0;
			double LastEditAt = 0.0;
			TOptional<dream::TextSnapshot> Held;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("push a background buffer past NeedsCompaction with scattered edits"), [this, C]()
		{
			// An active tab first, so the target buffer never gets a code view: thousands of edits would each re-sync the
			// stock text widget. The undo history records model changes with or without a view.
			const TSharedPtr<FDreamBuffer> Anchor = DreamTest::OpenScratch(*this, *Tab, TEXT("anchor = 1"), TEXT("CompactAnchor"));
			if (!Anchor.IsValid())
			{
				return false;
			}
			Opened.Add(Anchor->GetId());
			C->Buffer = DreamBuffersSpec::Buffers().CreateScratch(FString::ChrN(2000, TEXT('a')), DreamTest::ScratchName(TEXT("Compact")));
			Opened.Add(C->Buffer->GetId());
			C->History = FDreamUndoHistory::For(C->Buffer.ToSharedRef());
			C->CompactionsBefore = C->Buffer->GetCompactionsForTest();
			for (int32 Index = 0; Index < 3000; ++Index)
			{
				if (Index == 2999)
				{
					C->BeforeLast = C->Buffer->GetTextAsString();
				}
				const int64 Length = C->Buffer->GetModel().Length();
				dream::EditBatch Batch;
				Batch.Origin = dream::EditOrigin::Command;
				dream::EditRange& Range = Batch.Ranges.emplace_back();
				Range.At = static_cast<dream::Offset>((static_cast<int64>(Index) * 7919) % (Length + 1));
				Range.DeleteLen = 0;
				Range.Insert = (Index % 2) == 0 ? u"b" : u"c";
				if (C->Buffer->ApplyEdit(Batch).IsEmpty())
				{
					AddError(FString::Printf(TEXT("edit %d did not apply"), Index));
					return false;
				}
			}
			C->LastEditAt = FPlatformTime::Seconds();
			C->Expected = C->Buffer->GetTextAsString();
			C->Version = C->Buffer->GetModel().Version();
			C->bDirty = C->Buffer->IsDirty();
			C->PiecesBefore = C->Buffer->Snapshot().PieceCount();
			C->Held = C->Buffer->Snapshot();
			AddInfo(FString::Printf(TEXT("%llu pieces after 3000 scattered edits"), static_cast<uint64>(C->PiecesBefore)));
			return TestTrue(TEXT("the buffer needs compaction now"), C->Buffer->GetModel().NeedsCompaction());
		});
		Steps.Poll(TEXT("it compacts after the quiet period"), DreamBuffersSpec::Until([C]()
		{
			return C->Buffer->GetCompactionsForTest() > C->CompactionsBefore && !C->Buffer->GetModel().NeedsCompaction();
		}), 20.0);
		Steps.Do(TEXT("check the compacted buffer"), [this, C]()
		{
			const double After = FPlatformTime::Seconds() - C->LastEditAt;
			AddInfo(FString::Printf(TEXT("compacted %.2f s after the last edit"), After));
			TestTrue(*FString::Printf(TEXT("not before the 2 s quiet period: %.2f s"), After), After >= FDreamBuffer::CompactionIdleSeconds - 0.05);
			TestTrue(*FString::Printf(TEXT("within a few seconds of idle: %.2f s"), After), After <= 6.0);
			const size_t Pieces = C->Buffer->Snapshot().PieceCount();
			TestTrue(*FString::Printf(TEXT("the piece tree is compact (%llu pieces, %llu before)"), static_cast<uint64>(Pieces),
				static_cast<uint64>(C->PiecesBefore)), Pieces <= 2 && Pieces < C->PiecesBefore);
			TestTrue(TEXT("the text is unchanged"), C->Buffer->GetTextAsString() == C->Expected);
			TestEqual(TEXT("the version is unchanged"), C->Buffer->GetModel().Version(), C->Version);
			TestEqual(TEXT("the dirty state is unchanged"), C->Buffer->IsDirty(), C->bDirty);
			TestTrue(TEXT("a snapshot taken before still reads its text"), C->Held.IsSet() && DreamText::ToFString(C->Held->Text()) == C->Expected);
			if (TestTrue(TEXT("undo is still possible"), C->History.IsValid() && C->History->CanUndo()) && C->History->Undo())
			{
				TestTrue(TEXT("one undo step takes back the last edit"), C->Buffer->GetTextAsString() == C->BeforeLast);
				TestTrue(TEXT("and redo brings it back"), C->History->Redo() && C->Buffer->GetTextAsString() == C->Expected);
			}
			return true;
		});
		Steps.Run(Done);
	});
}
