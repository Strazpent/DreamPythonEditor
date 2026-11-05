// SPDX-License-Identifier: Apache-2.0
// (scratch, PO-05) Scratch files and the sample scripts. Every case points the scratch folder at a private temp folder
// (DreamScratch::SetFolderForTest, put back in AfterEach), so no case writes into the user's real scratch folder; the
// tier-4 runner's -DreamScratchFolder covers everything else a pass opens.
//  - Dream.Buffers.ScratchFiles: a new unnamed tab is scratch_N.py, made empty at once with the lowest free N and labelled
//    with its file name (the + button, Ctrl+T and new_tab() alike; a named tab stays the journaled in-memory buffer); the
//    auto-save writes 1 s after the last edit and never later than 2 s after the oldest unsaved edit, on focus loss and
//    before a run; closing saves, asks nothing and keeps the file; a folder that cannot be written gives journaled
//    Untitled-N tabs with one warning; a restored Untitled-N becomes a scratch file with its ID (a pending one once its
//    recovery is restored); Save As writes the new file and leaves the scratch file as it was. Review p1: no text is lost
//    on a close during a save in flight or a refused close save (M1); no dirty mark while the auto-save is armed and one
//    history entry per 5 minutes of auto-saves (R-p1-3, R-p1-4); an empty scratch file is used again and making the folder
//    is announced (R-p1-7, R-p1-8); the folder setting's guard (R-p1-2); a restored tab's newer disk version (m2); a rename
//    during a save (m3).
//  - Dream.Buffers.Samples: the bundled samples are seeded into <folder>/Samples once, never over an existing file, and a
//    deleted one does not come back until Help > Restore Sample Scripts; a session-override folder keeps its own marker
//    and never touches the user's bSamplesSeeded; each sample has its two-line description and runs cleanly.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamFileWatch.h"
#include "Buffers/DreamScratch.h"
#include "CodeView/SDreamCodeView.h"
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
#include "Shell/SDreamEditorTabs.h"
#include "Shell/DreamProjectFiles.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

namespace DreamScratchFilesSpec
{
	FDreamBufferManager& Buffers()
	{
		return FDreamEditorModule::Get().GetBufferManager();
	}

	/** A fresh private folder under the user temp dir (normalized like DreamScratch::GetFolder). */
	FString NewDir(const TCHAR* Purpose)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"),
			FString::Printf(TEXT("Scratch%s_%s"), Purpose, *DreamTest::UniqueSuffix())));
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

	/** The file's text as UTF-8 with LF ("<missing>" when it cannot be read). */
	FString ReadText(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return TEXT("<missing>");
		}
		return Text.Replace(TEXT("\r\n"), TEXT("\n"));
	}

	bool WriteText(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Appends Text at the end of the buffer as typing does (one model edit). */
	bool Type(FDreamBuffer& Buffer, const FString& Text)
	{
		dream::EditBatch Batch;
		Batch.Origin = dream::EditOrigin::Typing;
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = Buffer.GetModel().Length();
		Range.DeleteLen = 0;
		Range.Insert = std::u16string(DreamText::ToU16(Text));
		return !Buffer.ApplyEdit(Batch).IsEmpty();
	}

	TFunction<DreamTest::EStep()> Until(TFunction<bool()> Fn)
	{
		return [Fn]() { return Fn() ? DreamTest::EStep::Next : DreamTest::EStep::Wait; };
	}

	FString Join(const FString& Dir, const FString& Name)
	{
		return Dir / Name;
	}

	/** The six samples the plan names (PO-05 section 4). */
	const TCHAR* const SampleNames[] = { TEXT("cells.py"), TEXT("demo.py"), TEXT("long_run_stop.py"), TEXT("output_styles.py"),
		TEXT("structure_demo.py"), TEXT("undo_run.py") };
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.ScratchFiles
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamScratchFilesSpec, "Dream.Buffers.ScratchFiles", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	FString OtherDir;
	TArray<FGuid> BufferIds;
	int32 SavedUntitledNumber = 0;
	FString PreviousOverride;
	/** A new unnamed tab, recorded for the AfterEach close; null with an error when it is not a scratch file. */
	TSharedPtr<FDreamBuffer> NewScratchFile();
END_DEFINE_SPEC(FDreamScratchFilesSpec)

TSharedPtr<FDreamBuffer> FDreamScratchFilesSpec::NewScratchFile()
{
	const TSharedRef<FDreamBuffer> Buffer = Tab->NewScratchTab();
	BufferIds.Add(Buffer->GetId());
	if (!TestTrue(*FString::Printf(TEXT("the new tab %s is a scratch file"), *Buffer->GetDisplayName()), Buffer->IsScratchFile()))
	{
		return nullptr;
	}
	return Buffer;
}

void FDreamScratchFilesSpec::Define()
{
	using namespace DreamScratchFilesSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Dir = NewDir(TEXT("Files"));
		OtherDir = NewDir(TEXT("SaveAs"));
		PreviousOverride = DreamScratch::GetFolderForTest();
		// The samples are Dream.Buffers.Samples' business: this folder counts as seeded, so no copy runs on a worker while a
		// case ends and its folder goes.
		WriteText(Join(Dir, TEXT(".dream-samples-seeded")), FString());
		DreamScratch::SetFolderForTest(Dir);
		SDreamTab::ResetScratchWarningForTest();
		SavedUntitledNumber = UDreamPerProjectUserSettings::Get()->NextUntitledNumber;
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		// The close saves of scratch files finish on a worker; the folder goes after them.
		FDreamFileSaver::WaitForWorkerWrites(2.0);
		DreamScratch::SetFolderForTest(PreviousOverride);
		SDreamTab::ResetScratchWarningForTest();
		Buffers().SetFocusSavesForTest(true);
		UDreamPerProjectUserSettings::GetMutable()->NextUntitledNumber = SavedUntitledNumber;
		RemoveDir(Dir);
		RemoveDir(OtherDir);
		Dir.Reset();
		OtherDir.Reset();
		Tab.Reset();
	});

	It("makes scratch_N.py at once with the lowest free N, labelled with its file name (BUF-01, PO-05)", [this]()
	{
		if (!Tab.IsValid())
		{
			return;
		}
		TestEqual(TEXT("the test folder is the scratch folder"), DreamScratch::GetFolder(), Dir);
		TestTrue(TEXT("taken names are skipped: scratch_1.py"), WriteText(Join(Dir, TEXT("scratch_1.py")), TEXT("one = 1\n")));
		TestTrue(TEXT("and scratch_3.py"), WriteText(Join(Dir, TEXT("scratch_3.py")), TEXT("three = 3\n")));
		const TSharedPtr<FDreamBuffer> Second = NewScratchFile();
		if (!Second.IsValid())
		{
			return;
		}
		TestEqual(TEXT("the lowest free N is 2"), Second->GetPath(), Join(Dir, TEXT("scratch_2.py")));
		TestEqual(TEXT("the tab is labelled with the file name"), Second->GetDisplayName(), FString(TEXT("scratch_2.py")));
		TestTrue(TEXT("the file exists at once (so the Project tree shows it)"), IFileManager::Get().FileExists(*Second->GetPath()));
		TestEqual(TEXT("empty"), IFileManager::Get().FileSize(*Second->GetPath()), static_cast<int64>(0));
		TestTrue(TEXT("a File buffer"), Second->GetKind() == EDreamBufferKind::File);
		TestFalse(TEXT("clean"), Second->IsDirty());
		TestTrue(TEXT("it is the active tab"), Tab->GetActiveBuffer() == Second);
		TestEqual(TEXT("tracebacks and compile filenames use the real path"), Buffers().GetCompileFilename(*Second), Second->GetPath());
		TestFalse(TEXT("scratch files stay out of Recent Files"), Buffers().GetRecentFiles().Contains(Second->GetPath()));
		TestTrue(TEXT("IsScratchPath sees it"), DreamScratch::IsScratchPath(Second->GetPath()));
		TestTrue(TEXT("and its name"), DreamScratch::IsScratchFileName(Second->GetPath()));

		const TSharedPtr<FDreamBuffer> Fourth = NewScratchFile();
		TestTrue(TEXT("the next one is scratch_4.py"), Fourth.IsValid() && Fourth->GetPath() == Join(Dir, TEXT("scratch_4.py")));

		// The New Tab command (Ctrl+T, the + button, File > New) takes the same path.
		Tab->ExecuteDreamCommand(EDreamCommand::NewTab);
		const TSharedPtr<FDreamBuffer> Fifth = Tab->GetActiveBuffer();
		if (TestTrue(TEXT("New Tab opened a tab"), Fifth.IsValid() && Fifth != Fourth))
		{
			BufferIds.Add(Fifth->GetId());
			TestTrue(TEXT("New Tab makes scratch_5.py"), Fifth->IsScratchFile() && Fifth->GetPath() == Join(Dir, TEXT("scratch_5.py")));
		}

		// A file deleted while its tab is open keeps its name taken.
		IFileManager::Get().Delete(*Join(Dir, TEXT("scratch_2.py")), false, true, true);
		const TSharedPtr<FDreamBuffer> Sixth = NewScratchFile();
		TestTrue(TEXT("an open tab's deleted file still holds scratch_2.py: the next is scratch_6.py"),
			Sixth.IsValid() && Sixth->GetPath() == Join(Dir, TEXT("scratch_6.py")));

		// A named tab is still the journaled in-memory buffer (internal callers, the specs' DreamT4-* tabs).
		const TSharedRef<FDreamBuffer> Named = Tab->NewScratchTab(TEXT("named = 1\n"), DreamTest::ScratchName(TEXT("Named")));
		BufferIds.Add(Named->GetId());
		TestTrue(TEXT("a named tab is a Scratch buffer without a file"), Named->GetKind() == EDreamBufferKind::Scratch && !Named->HasPath() && !Named->IsScratchFile());
	});

	LatentIt("saves 1 s after the last edit and never later than 2 s after the oldest unsaved edit (PO-05)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		// (p1-m9) Timing only: a focus change or another application in front would save early, so those saves are off here
		// (the next case tests them).
		Buffers().SetFocusSavesForTest(false);
		struct FCase
		{
			double FirstEdit = 0.0;
			double LastEdit = 0.0;
			int32 Typed = 0;
			FString Expected;
		};
		TSharedRef<FCase> C = MakeShared<FCase>();
		auto Live = [Id]() { return Buffers().FindById(Id); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("one edit"), [this, C, Live]()
		{
			C->Expected = TEXT("quiet = 1\n");
			C->FirstEdit = C->LastEdit = FPlatformTime::Seconds();
			return TestTrue(TEXT("typed"), Type(*Live(), C->Expected));
		});
		Steps.Poll(TEXT("0.6 s later"), [C]() { return FPlatformTime::Seconds() - C->LastEdit >= 0.6 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; });
		Steps.Do(TEXT("not saved yet"), [this, Path, Id]()
		{
			TestEqual(TEXT("the file is still empty 0.6 s after the edit"), ReadText(Path), FString());
			TestEqual(TEXT("no save started"), Buffers().GetScratchSaveStatsForTest(Id).Saves, 0);
			return true;
		});
		// The file is written on a worker; the buffer turns clean when the result lands on the game thread a frame later.
		Steps.Poll(TEXT("the save lands"), Until([Path, C, Live]() { return ReadText(Path) == C->Expected && !Live()->IsDirty(); }), 5.0);
		Steps.Do(TEXT("1 s after the last edit"), [this, Id, Live, C]()
		{
			const FDreamBufferManager::FScratchSaveStatsForTest Stats = Buffers().GetScratchSaveStatsForTest(Id);
			const double After = Stats.LastSaveStartSeconds - C->LastEdit;
			TestEqual(TEXT("one save"), Stats.Saves, 1);
			TestTrue(TEXT("by the timer"), Stats.LastReason == FDreamBufferManager::EScratchSave::Timer);
			TestTrue(*FString::Printf(TEXT("it started %.2f s after the last edit (1.0 to 1.4)"), After), After >= 0.99 && After <= 1.4);
			TestFalse(TEXT("the buffer is clean"), Live()->IsDirty());
			return true;
		});
		// Typing every 0.3 s for 3 s: the quiet second never comes, so the 2 s bound saves.
		Steps.Do(TEXT("start typing steadily"), [C, Live]()
		{
			C->FirstEdit = C->LastEdit = FPlatformTime::Seconds();
			C->Typed = 1;
			C->Expected += TEXT("steady = 1\n");
			return Type(*Live(), TEXT("steady = 1\n"));
		});
		Steps.Poll(TEXT("type every 0.3 s for 3 s"), [C, Live]()
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - C->LastEdit >= 0.3)
			{
				const FString Line = FString::Printf(TEXT("steady = %d\n"), ++C->Typed);
				C->Expected += Line;
				Type(*Live(), Line);
				C->LastEdit = Now;
			}
			return Now - C->FirstEdit >= 3.0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Do(TEXT("the 2 s bound saved while typing"), [this, Id, C]()
		{
			const FDreamBufferManager::FScratchSaveStatsForTest Stats = Buffers().GetScratchSaveStatsForTest(Id);
			TestTrue(*FString::Printf(TEXT("a save started while typing (%d saves)"), Stats.Saves), Stats.Saves >= 2);
			const double FromOldest = Stats.LastFirstEditSeconds > 0.0 ? Stats.LastSaveStartSeconds - Stats.LastFirstEditSeconds : -1.0;
			TestTrue(*FString::Printf(TEXT("the first one %.2f s after the oldest unsaved edit (2.0 to 2.4)"), FromOldest),
				Stats.Saves == 2 ? FromOldest >= 1.99 && FromOldest <= 2.4 : true);
			TestTrue(TEXT("by the timer"), Stats.LastReason == FDreamBufferManager::EScratchSave::Timer);
			return true;
		});
		Steps.Poll(TEXT("the last edit lands within 1 s of the typing's end"), Until([Path, C, Live]() { return ReadText(Path) == C->Expected && !Live()->IsDirty(); }), 5.0);
		Steps.Do(TEXT("all of it is in the file"), [this, Live, C]()
		{
			TestFalse(TEXT("clean"), Live()->IsDirty());
			TestTrue(*FString::Printf(TEXT("it landed %.2f s after the last edit"), FPlatformTime::Seconds() - C->LastEdit),
				FPlatformTime::Seconds() - C->LastEdit < 2.5);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("saves when focus leaves its code view and before a run (PO-05)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString First = FString::Printf(TEXT("focus_%s = 1\n"), *Sfx);
		const FString Second = FString::Printf(TEXT("print('run_%s')\n"), *Sfx);
		auto Live = [Id]() { return Buffers().FindById(Id); };
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("type, then move focus to the tab itself"), [this, Live, First]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("the code view has focus"), View.IsValid() && (View->HasFocusedDescendants() || View->HasKeyboardFocus())))
			{
				return false;
			}
			Type(*Live(), First);
			FSlateApplication::Get().SetKeyboardFocus(Tab, EFocusCause::SetDirectly);
			return true;
		});
		Steps.Poll(TEXT("the focus-loss save lands"), Until([Path, First, Live]() { return ReadText(Path) == First && !Live()->IsDirty(); }), 5.0);
		Steps.Do(TEXT("it was the focus loss, not the timer"), [this, Id]()
		{
			const FDreamBufferManager::FScratchSaveStatsForTest Stats = Buffers().GetScratchSaveStatsForTest(Id);
			TestTrue(TEXT("saved on focus loss"), Stats.LastReason == FDreamBufferManager::EScratchSave::FocusLost);
			TestTrue(*FString::Printf(TEXT("at once (%.2f s after the edit)"), Stats.LastSaveStartSeconds - Stats.LastEditSeconds),
				Stats.LastSaveStartSeconds - Stats.LastEditSeconds < 0.9);
			return true;
		});
		// (p1-m9) Typed and submitted in one step, so the run starts well inside the auto-save's quiet second.
		Steps.Do(TEXT("type and run the tab at once"), [this, Live, Second, Run]()
		{
			Type(*Live(), Second);
			const TSharedPtr<FDreamBuffer> B = Live();
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*B, EDreamRunRoute::Buffer, 1, static_cast<int32>(B->GetModel().LineCount())));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the scratch tab"), Run);
		Steps.Poll(TEXT("the run-start save lands"), Until([Path, First, Second]() { return ReadText(Path) == First + Second; }), 5.0);
		Steps.Do(TEXT("it was saved before the run"), [this, Id, Run, Sfx]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestTrue(TEXT("the run printed"), Run->HasLineContaining(dream::LineType::Stdout, TEXT("run_") + Sfx));
			const FDreamBufferManager::FScratchSaveStatsForTest Stats = Buffers().GetScratchSaveStatsForTest(Id);
			// At Running (after its journal snapshot) as a rule; a run that took longer than the quiet second to start was saved
			// by the timer before it entered, which is "before the run" too.
			TestTrue(*FString::Printf(TEXT("saved before the run entered (reason %d, %.2f s before it)"), static_cast<int32>(Stats.LastReason),
				Run->Record.EnterSeconds - Stats.LastSaveStartSeconds),
				Stats.LastReason == FDreamBufferManager::EScratchSave::RunStart || Stats.LastSaveStartSeconds <= Run->Record.EnterSeconds);
			TestEqual(TEXT("the run's filename is the real path"), Run->Record.Filename, Buffers().FindById(Id)->GetPath());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("saves on close without asking and keeps the file; Reopen Closed Tab brings it back (PO-05)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		const FString Text = FString::Printf(TEXT("closed_%s = 1\n"), *DreamTest::UniqueSuffix());
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type and close at once"), [this, Id, Text]()
		{
			Type(*Buffers().FindById(Id), Text);
			Tab->ActivateBuffer(Id);
			Tab->ExecuteDreamCommand(EDreamCommand::CloseTab);
			TestFalse(TEXT("closed"), Buffers().FindById(Id).IsValid());
			TestFalse(TEXT("no modal prompt"), FSlateApplication::Get().GetActiveModalWindow().IsValid());
			return true;
		});
		Steps.Poll(TEXT("the close save lands"), Until([Path, Text]() { return ReadText(Path) == Text; }), 5.0);
		Steps.Do(TEXT("the file stays; Reopen Closed Tab"), [this, Id, Path, Text]()
		{
			TestTrue(TEXT("the file is kept"), IFileManager::Get().FileExists(*Path));
			const TSharedPtr<FDreamBuffer> Closed = Buffers().FindClosedByIdHex(DreamText::GuidToHex(Id));
			TestTrue(TEXT("the closed buffer took the saved state"), Closed.IsValid() && !Closed->IsDirty());
			Tab->ExecuteDreamCommand(EDreamCommand::ReopenClosedTab);
			const TSharedPtr<FDreamBuffer> Back = Buffers().FindById(Id);
			if (TestTrue(TEXT("Reopen Closed Tab brings it back"), Back.IsValid()))
			{
				TestEqual(TEXT("with its file"), Back->GetPath(), Path);
				TestTrue(TEXT("still a scratch file"), Back->IsScratchFile());
				TestEqual(TEXT("and its text"), Back->GetTextAsString(), Text);
			}
			return true;
		});
		Steps.Run(Done);
	});

	It("falls back to a journaled Untitled-N with one warning when the folder cannot be written (PO-05)", [this]()
	{
		if (!Tab.IsValid())
		{
			return;
		}
		// A folder below a file can never be made.
		const FString Blocker = Join(Dir, TEXT("blocker.txt"));
		TestTrue(TEXT("the blocking file"), WriteText(Blocker, TEXT("not a folder\n")));
		DreamScratch::SetFolderForTest(Blocker / TEXT("Scratches"));
		AddExpectedMessagePlain(TEXT("Scratch folder is not writable"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		const TSharedRef<FDreamBuffer> One = Tab->NewScratchTab();
		BufferIds.Add(One->GetId());
		const TSharedRef<FDreamBuffer> Two = Tab->NewScratchTab(TEXT("typing_never_fails = 1\n"));
		BufferIds.Add(Two->GetId());
		for (const TSharedRef<FDreamBuffer>& B : { One, Two })
		{
			TestTrue(*FString::Printf(TEXT("%s is a journaled scratch buffer"), *B->GetDisplayName()),
				B->GetKind() == EDreamBufferKind::Scratch && !B->HasPath() && B->GetDisplayName().StartsWith(TEXT("Untitled-")));
		}
		TestEqual(TEXT("the text is there"), Two->GetTextAsString(), FString(TEXT("typing_never_fails = 1\n")));
		TestTrue(TEXT("the active tab is the second"), Tab->GetActiveBuffer() == Two);
		// Writable again: the next new tab is a file.
		DreamScratch::SetFolderForTest(Dir);
		const TSharedPtr<FDreamBuffer> File = NewScratchFile();
		TestTrue(TEXT("a writable folder makes files again"), File.IsValid() && File->GetPath() == Join(Dir, TEXT("scratch_1.py")));
	});

	LatentIt("turns a restored Untitled-N into scratch_N.py with its ID; a pending one after its restore (PO-05)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		// An earlier session's journaled Untitled-N tabs, as the startup restore applies them: one restored at once, one still
		// pending (counted, with its clean text shown under Not restored yet).
		const int32 Base = FMath::Max(1, UDreamPerProjectUserSettings::Get()->NextUntitledNumber) + 50;
		FDreamRecoveredSession Session;
		Session.bPreviousExitClean = false;
		Session.PreviousEndUtc = FDateTime::UtcNow();
		const FString PlainText = FString::Printf(TEXT("migrated_%s = 1\n"), *DreamTest::UniqueSuffix());
		const FString CleanText = TEXT("pending = 0\n");
		const FString RecoveredText = FString::Printf(TEXT("pending_%s = 1\n"), *DreamTest::UniqueSuffix());
		FDreamRecoveredBuffer& Plain = Session.Buffers.AddDefaulted_GetRef();
		Plain.Id = FGuid::NewGuid();
		Plain.Kind = EDreamBufferKind::Scratch;
		Plain.DisplayName = FString::Printf(TEXT("Untitled-%d"), Base);
		Plain.RecoveredText = PlainText;
		Plain.LastEditUtc = Session.PreviousEndUtc;
		FDreamRecoveredBuffer& Pending = Session.Buffers.AddDefaulted_GetRef();
		Pending.Id = FGuid::NewGuid();
		Pending.Kind = EDreamBufferKind::Scratch;
		Pending.DisplayName = FString::Printf(TEXT("Untitled-%d"), Base + 1);
		Pending.CleanText = CleanText;
		Pending.RecoveredText = RecoveredText;
		Pending.bCounted = true;
		Pending.LastEditUtc = Session.PreviousEndUtc;
		Session.SessionTabOrder = { Plain.Id, Pending.Id };
		Session.CountedBuffers = 1;
		const FGuid PlainId = Plain.Id;
		const FGuid PendingId = Pending.Id;
		BufferIds.Append({ PlainId, PendingId });
		Tab->ApplyRecoveryForTest(MoveTemp(Session));

		auto IsMigrated = [](const FGuid& Id, const FString& Text)
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			return B.IsValid() && B->IsScratchFile() && !B->IsDirty() && ReadText(B->GetPath()) == Text;
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.Poll(TEXT("the restored Untitled-N becomes a scratch file"), Until([IsMigrated, PlainId, PlainText]() { return IsMigrated(PlainId, PlainText); }), 10.0);
		Steps.Do(TEXT("check it; the pending one waits"), [this, PlainId, PendingId, CleanText]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(PlainId);
			TestTrue(TEXT("the same ID, now a File buffer"), B.IsValid() && B->GetKind() == EDreamBufferKind::File);
			TestTrue(TEXT("labelled scratch_N.py"), B.IsValid() && DreamScratch::IsScratchFileName(B->GetPath())
				&& B->GetDisplayName() == FPaths::GetCleanFilename(B->GetPath()));
			const TSharedPtr<FDreamBuffer> P = Buffers().FindById(PendingId);
			TestTrue(TEXT("the pending one is still an untitled, pending buffer (PER-03)"), P.IsValid() && P->HasPendingRecovery()
				&& P->GetKind() == EDreamBufferKind::Scratch && !P->HasPath());
			TestEqual(TEXT("showing its clean text"), P.IsValid() ? P->GetTextAsString() : FString(), CleanText);
			Tab->RestoreRecovered(PendingId, /*bAsNewTab*/ false);
			return true;
		});
		Steps.Poll(TEXT("after its restore it becomes a scratch file too"), Until([IsMigrated, PendingId, RecoveredText]() { return IsMigrated(PendingId, RecoveredText); }), 10.0);
		Steps.Run(Done);
	});

	LatentIt("Save As writes the new file and leaves the scratch file as it was, a pending disk version included (PO-05, R-p1-1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString ScratchPath = Created->GetPath();
		const FString Target = Join(OtherDir, FString::Printf(TEXT("kept_%s.py"), *DreamTest::UniqueSuffix()));
		const FString Mine = TEXT("kept = 1\nkept = 2\n");
		const FString Theirs = TEXT("theirs = 1\n");
		TSharedRef<TOptional<bool>> Saved = MakeShared<TOptional<bool>>();
		DreamTest::FDreamSteps Steps(*this);
		// scratch-p1-M2: another program writes the scratch file while the tab holds unsaved text: the Keep mine / Reload bar.
		Steps.Do(TEXT("type, then change the file outside"), [this, Id, Mine, Theirs, ScratchPath]()
		{
			Type(*Buffers().FindById(Id), Mine);
			return TestTrue(TEXT("the outside write"), WriteText(ScratchPath, Theirs));
		});
		Steps.Poll(TEXT("the disk change is seen"), Until([Id]() { return Buffers().GetDiskChange(Id) != nullptr; }), 10.0);
		Steps.Do(TEXT("Save As"), [this, Id, Target, Saved]()
		{
			Buffers().SaveAsAsync(Id, Target, FDreamOnSaved::CreateLambda([Saved](bool bOk, const FText&) { *Saved = bOk; }));
			return true;
		});
		Steps.Poll(TEXT("the Save As lands"), Until([Saved]() { return Saved->IsSet(); }), 10.0);
		Steps.Do(TEXT("check both files and the tab"), [this, Id, Target, ScratchPath, Mine, Theirs, Saved]()
		{
			TestTrue(TEXT("saved"), Saved->Get(false));
			TestEqual(TEXT("the new file holds the tab's text"), ReadText(Target), Mine);
			TestEqual(TEXT("the scratch file keeps the other program's version (BUF-04)"), ReadText(ScratchPath), Theirs);
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			if (TestTrue(TEXT("the tab is still open with its ID"), B.IsValid()))
			{
				TestEqual(TEXT("retargeted to the new file"), B->GetPath(), FPaths::ConvertRelativePathToFull(Target));
				TestEqual(TEXT("labelled with its name"), B->GetDisplayName(), FPaths::GetCleanFilename(Target));
				TestFalse(TEXT("a file outside the scratch folder is no scratch file"), B->IsScratchFile());
				TestFalse(TEXT("clean"), B->IsDirty());
			}
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("never loses text closed during a save in flight: one write at a time, then exactly one more (scratch-p1-M1, p2-m3)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		const FString First = TEXT("first = 1\n");
		const FString Second = TEXT("second = 2\n");
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type, start a save, type more and close in one frame"), [this, Id, First, Second]()
		{
			Type(*Buffers().FindById(Id), First);
			TestTrue(TEXT("a save started"), Buffers().SaveScratchNow(Id, FDreamBufferManager::EScratchSave::Other));
			TestEqual(TEXT("one save started"), Buffers().GetSavesStartedForTest(Id), 1);
			Type(*Buffers().FindById(Id), Second);
			DreamTest::CloseBuffer(Id);
			TestFalse(TEXT("closed"), Buffers().FindById(Id).IsValid());
			// The fix: the close starts no second write while the first is in flight (the p1 code started one at once).
			TestEqual(TEXT("the close started no second save while one is in flight"), Buffers().GetSavesStartedForTest(Id), 1);
			TestTrue(TEXT("the first save is still in flight"), Buffers().IsSaveInFlight(Id));
			return true;
		});
		Steps.Poll(TEXT("the file gets the text typed after the first save"), Until([Path, First, Second]() { return ReadText(Path) == First + Second; }), 10.0);
		Steps.Poll(TEXT("and the closed buffer is clean"), Until([Id]()
		{
			const TSharedPtr<FDreamBuffer> Closed = Buffers().FindClosedByIdHex(DreamText::GuidToHex(Id));
			return Closed.IsValid() && !Closed->IsDirty() && !Buffers().IsSaveInFlight(Id);
		}), 10.0);
		Steps.Do(TEXT("exactly one more write after the first landed"), [this, Id]()
		{
			TestEqual(TEXT("two saves in all: the first, then the close save after it"), Buffers().GetSavesStartedForTest(Id), 2);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("keeps a closed scratch file's text in local history when its close save cannot run (scratch-p1-M1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		const FString Mine = FString::Printf(TEXT("mine_%s = 1\n"), *DreamTest::UniqueSuffix());
		const FString Theirs = TEXT("theirs = 1\n");
		AddExpectedMessagePlain(TEXT("could not be saved when its tab closed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		struct FCase
		{
			bool bQueried = false;
			TArray<FDreamLocalHistoryEntry> Entries;
			TOptional<FString> Loaded;
			bool bLoadAsked = false;
		};
		const TSharedRef<FCase> C = MakeShared<FCase>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type, then change the file outside"), [this, Id, Mine, Theirs, Path]()
		{
			Type(*Buffers().FindById(Id), Mine);
			return TestTrue(TEXT("the outside write"), WriteText(Path, Theirs));
		});
		Steps.Poll(TEXT("the disk change pauses the auto-save"), Until([Id]() { return Buffers().GetDiskChange(Id) != nullptr; }), 10.0);
		Steps.Do(TEXT("close the tab"), [this, Id]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			TestTrue(TEXT("its tab shows the dirty mark while the auto-save is paused (R-p1-4)"), B.IsValid() && SDreamEditorTabs::GetBadges(*B).bDirty);
			DreamTest::CloseBuffer(Id);
			return true;
		});
		Steps.Poll(TEXT("local history holds the closed text"), [this, Id, C, Mine]()
		{
			if (!C->bQueried)
			{
				C->bQueried = true;
				FDreamEditorModule::Get().GetLocalHistory().Query(Id, [C](TArray<FDreamLocalHistoryEntry>&& Entries) { C->Entries = MoveTemp(Entries); });
				return DreamTest::EStep::Wait;
			}
			const FDreamLocalHistoryEntry* Closed = C->Entries.FindByPredicate([](const FDreamLocalHistoryEntry& E) { return E.Kind == EDreamHistoryKind::Closed; });
			if (Closed == nullptr)
			{
				C->bQueried = false;   // ask again: the entry is written on the journal writer
				return DreamTest::EStep::Wait;
			}
			if (!C->bLoadAsked)
			{
				C->bLoadAsked = true;
				FDreamEditorModule::Get().GetLocalHistory().LoadText(*Closed, [C](TOptional<FString>&& Text) { C->Loaded = MoveTemp(Text); });
			}
			return C->Loaded.IsSet() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 15.0);
		Steps.Do(TEXT("check it"), [this, C, Mine, Theirs, Path]()
		{
			TestEqual(TEXT("the closed entry holds the tab's text"), C->Loaded.Get(FString()), Mine);
			TestEqual(TEXT("the other program's file is untouched (BUF-04)"), ReadText(Path), Theirs);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("shows no dirty mark while the auto-save is armed, and adds one history entry per 5 minutes of auto-saves (R-p1-3, R-p1-4)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		Buffers().SetFocusSavesForTest(false);
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		auto Live = [Id]() { return Buffers().FindById(Id); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("type"), [this, Live]()
		{
			Type(*Live(), TEXT("a = 1\n"));
			TestTrue(TEXT("dirty"), Live()->IsDirty());
			TestFalse(TEXT("but no dirty mark: the auto-save keeps it on disk"), SDreamEditorTabs::GetBadges(*Live()).bDirty);
			return true;
		});
		Steps.Poll(TEXT("the first auto-save lands"), Until([Live, Path]() { return !Live()->IsDirty() && ReadText(Path) == TEXT("a = 1\n"); }), 5.0);
		Steps.Do(TEXT("type again"), [Live]() { Type(*Live(), TEXT("b = 2\n")); return true; });
		Steps.Poll(TEXT("the second auto-save lands"), Until([Live, Path]() { return !Live()->IsDirty() && ReadText(Path) == TEXT("a = 1\nb = 2\n"); }), 5.0);
		Steps.Do(TEXT("one history entry for both"), [this, Id]()
		{
			const FDreamBufferManager::FScratchSaveStatsForTest Stats = Buffers().GetScratchSaveStatsForTest(Id);
			TestEqual(TEXT("two auto-saves"), Stats.Saves, 2);
			TestEqual(TEXT("one local-history entry (the first)"), Stats.HistoryEntries, 1);
			// A failed save shows the mark: the auto-save is paused behind the bar.
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("reuses only an empty scratch file this editor made and closed unused, and tells the tree when it makes the folder (R-p1-7, R-p1-8, p3-m1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FCase
		{
			FString Fresh;
			FString BusyPath;
			FString HeldPath;
			FGuid BusyId;
			FGuid HeldId;
		};
		const TSharedRef<FCase> C = MakeShared<FCase>();
		AddExpectedMessagePlain(TEXT("could not be saved when its tab closed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Not saved: the file is read-only"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("make, close unused, reuse; never another editor's file"), [this, C]()
		{
			if (!Tab.IsValid())
			{
				return false;
			}
			C->Fresh = Dir / TEXT("NotYet");
			DreamScratch::SetFolderForTest(C->Fresh);
			int32 Changes = 0;
			const FDelegateHandle Handle = DreamScratch::OnFolderChanged().AddLambda([&Changes]() { ++Changes; });
			const TSharedPtr<FDreamBuffer> One = NewScratchFile();
			DreamScratch::OnFolderChanged().Remove(Handle);
			TestTrue(TEXT("making the folder fired OnFolderChanged"), Changes >= 1);
			if (!One.IsValid())
			{
				return false;
			}
			const FString OnePath = One->GetPath();
			TestEqual(TEXT("scratch_1.py"), FPaths::GetCleanFilename(OnePath), FString(TEXT("scratch_1.py")));
			DreamTest::CloseBuffer(One->GetId());   // unused: the empty file stays
			TestTrue(TEXT("the empty file is kept"), IFileManager::Get().FileExists(*OnePath));
			const TSharedPtr<FDreamBuffer> Again = NewScratchFile();
			TestTrue(TEXT("the next new tab uses it again"), Again.IsValid() && Again->GetPath() == OnePath);
			// An empty scratch file this editor did not make (another editor's open tab, an earlier session's) is never taken,
			// however old.
			const FString Foreign = C->Fresh / TEXT("scratch_2.py");
			TestTrue(TEXT("another editor's empty scratch_2.py"), WriteText(Foreign, FString()));
			IFileManager::Get().SetTimeStamp(*Foreign, FDateTime::UtcNow() - FTimespan::FromHours(2.0));
			const TSharedPtr<FDreamBuffer> Three = NewScratchFile();
			TestTrue(TEXT("it is not reused: scratch_3.py"), Three.IsValid() && FPaths::GetCleanFilename(Three->GetPath()) == TEXT("scratch_3.py"));
			// p2-m1(a): a tab closed with text while its close save is still in flight (the file still empty).
			const TSharedPtr<FDreamBuffer> Busy = NewScratchFile();
			if (!Busy.IsValid())
			{
				return false;
			}
			C->BusyPath = Busy->GetPath();
			C->BusyId = Busy->GetId();
			Type(*Busy, TEXT("busy = 1\n"));
			DreamTest::CloseBuffer(Busy->GetId());
			TestTrue(TEXT("its close save is in flight"), Buffers().IsSaveInFlight(Busy->GetId()));
			TestTrue(TEXT("so its file is busy"), Buffers().IsPathBusy(C->BusyPath));
			const TSharedPtr<FDreamBuffer> Five = NewScratchFile();
			TestTrue(TEXT("and not reused"), Five.IsValid() && Five->GetPath() != C->BusyPath);
			// A closed holder with unsaved text: its close save fails (a read-only file), the text goes to local history.
			const TSharedPtr<FDreamBuffer> Held = NewScratchFile();
			if (!Held.IsValid())
			{
				return false;
			}
			C->HeldPath = Held->GetPath();
			C->HeldId = Held->GetId();
			TestTrue(TEXT("the file is made read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*C->HeldPath, true));
			Type(*Held, TEXT("held = 1\n"));
			DreamTest::CloseBuffer(Held->GetId());
			return true;
		});
		Steps.Poll(TEXT("both close saves land"), Until([C]()
		{
			return !DreamScratch::IsSeedingForTest() && !Buffers().IsSaveInFlight(C->BusyId) && !Buffers().IsSaveInFlight(C->HeldId)
				&& ReadText(C->BusyPath) == TEXT("busy = 1\n");
		}), 10.0);
		Steps.Do(TEXT("the dirty closed holder keeps its empty file"), [this, C]()
		{
			TestEqual(TEXT("the read-only file stays empty"), IFileManager::Get().FileSize(*C->HeldPath), static_cast<int64>(0));
			TestTrue(TEXT("its closed holder still has unsaved text: busy"), Buffers().IsPathBusy(C->HeldPath));
			const TSharedPtr<FDreamBuffer> Next = NewScratchFile();
			TestTrue(TEXT("it is not reused"), Next.IsValid() && Next->GetPath() != C->HeldPath && Next->GetPath() != C->BusyPath);
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*C->HeldPath, false);
			return true;
		});
		Steps.Run(Done);
	});

	It("refuses a Scratch folder setting that would make other files save themselves, and keeps UNC paths (R-p1-2, p2-m2)", [this]()
	{
		auto Refused = [this](const FString& Folder) { return !DreamScratch::WhyNotAllowed(Folder).IsEmpty(); };
		TestTrue(TEXT("a relative path"), Refused(TEXT("Scratches")));
		TestTrue(TEXT("a drive root"), Refused(TEXT("C:/")));
		TestTrue(TEXT("a drive root without its slash"), Refused(TEXT("C:")));
		TestTrue(TEXT("a share root"), Refused(TEXT("\\\\server\\share")));
		TestTrue(TEXT("a share root with a trailing slash"), Refused(TEXT("//server/share/")));
		TestFalse(*FString::Printf(TEXT("a folder on a share is allowed: %s"), *DreamScratch::WhyNotAllowed(TEXT("\\\\server\\share\\sub")).ToString()),
			Refused(TEXT("\\\\server\\share\\sub")));
		const FString Profile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		TestTrue(TEXT("the user profile folder"), Refused(Profile));
		TestTrue(TEXT("a folder that holds the user profile folder (C:/Users)"), Refused(FPaths::GetPath(FPaths::ConvertRelativePathToFull(Profile))));
		TestTrue(TEXT("the project folder"), Refused(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
		TestTrue(TEXT("a folder that holds the project"), Refused(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".."))));
		TestTrue(TEXT("the engine folder"), Refused(FPaths::ConvertRelativePathToFull(FPaths::EngineDir())));
		TestTrue(TEXT("a folder in the engine"), Refused(FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries"))));
		for (const FDreamProjectRoot& Root : FDreamProjectFiles::ResolveRoots())
		{
			if (!Root.bScratches)
			{
				TestTrue(*FString::Printf(TEXT("the Project tree root %s"), *Root.Path), Refused(Root.Path));
			}
		}
		TestFalse(*FString::Printf(TEXT("a private folder is allowed: %s"), *DreamScratch::WhyNotAllowed(Dir).ToString()), Refused(Dir));
		TestFalse(TEXT("a folder inside the project is allowed"), Refused(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MyScratches"))));

		// p2-m2: a UNC folder keeps its "//" (localhost, so the side bar's stat of the Scratches item answers at once; the folder is never made).
		DreamScratch::SetFolderForTest(TEXT("\\\\localhost\\DreamT4NoShare\\sub"));
		TestEqual(TEXT("a UNC scratch folder keeps its leading //"), DreamScratch::GetFolder(), FString(TEXT("//localhost/DreamT4NoShare/sub")));
		TestTrue(TEXT("a file in it is a scratch path"), DreamScratch::IsScratchPath(TEXT("\\\\localhost\\DreamT4NoShare\\sub\\scratch_1.py")));
		TestFalse(TEXT("the drive-root look-alike is not"), DreamScratch::IsScratchPath(TEXT("/localhost/DreamT4NoShare/sub/scratch_1.py")));
		TestTrue(TEXT("the watch's key keeps it too"), FDreamFileWatch::PathKey(TEXT("\\\\localhost\\DreamT4NoShare\\a.py")).StartsWith(TEXT("//")));
		DreamScratch::SetFolderForTest(Dir);
	});

	LatentIt("keeps a newer disk version behind its bar for a dirty scratch tab restored after a clean exit, through the watch's checks (scratch-p1-m2, p2-M1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString Path = Created->GetPath();
		const FString Disk = TEXT("disk = 1\n");
		const FString Journal = TEXT("journal = 1\n");
		// The journal wrote its text against the empty file's version; the file has changed outside since.
		const TOptional<FDreamBaseVersion> JournalBase = Created->GetBase();
		struct FCase
		{
			double Since = 0.0;
			int32 SavesBefore = 0;
		};
		const TSharedRef<FCase> C = MakeShared<FCase>();
		auto Live = [Id]() { return Buffers().FindById(Id); };
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the file changes outside"), [this, Path, Disk]() { return TestTrue(TEXT("the outside write"), WriteText(Path, Disk)); });
		// A clean tab reloads it: the buffer is now as ApplyRestore's OpenFile leaves it (the disk text, the disk's base).
		Steps.Poll(TEXT("the clean tab takes the disk version"), Until([Live, Disk]() { return Live()->GetTextAsString() == Disk && !Live()->IsDirty(); }), 10.0);
		Steps.Do(TEXT("as ApplyRestore leaves a dirty tab whose file changed"), [this, Id, Live, Disk, Journal, JournalBase, C]()
		{
			const TSharedPtr<FDreamBuffer> B = Live();
			B->ResetText(Journal, dream::EditOrigin::Load);
			B->SetCleanText(Disk);
			Buffers().HoldRestoredDiskVersion(Id, Disk, JournalBase);
			C->SavesBefore = Buffers().GetSavesStartedForTest(Id);
			return TestTrue(TEXT("the bar holds the disk version"), Buffers().GetDiskChange(Id) != nullptr);
		});
		// The watch looks again, as it does when the editor is activated and after every run.
		Steps.Do(TEXT("the watch checks every file (activation, run end)"), []() { Buffers().GetFileWatch().QueueAll(/*bForceHash*/ true); return true; });
		Steps.Poll(TEXT("its check lands"), Until([]() { return Buffers().GetFileWatch().IsIdleForTest(); }), 10.0);
		Steps.WaitFrames(3);
		Steps.Do(TEXT("and again, without the hash"), [C]() { Buffers().GetFileWatch().QueueAll(/*bForceHash*/ false); C->Since = FPlatformTime::Seconds(); return true; });
		Steps.Poll(TEXT("that check lands"), Until([]() { return Buffers().GetFileWatch().IsIdleForTest(); }), 10.0);
		Steps.Poll(TEXT("2.5 s of auto-save time"), [C]() { return FPlatformTime::Seconds() - C->Since > 2.5 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 5.0);
		Steps.Do(TEXT("the bar, the paused auto-save and the disk text all survive"), [this, Id, Path, Disk, Journal, Live, C]()
		{
			const FDreamDiskChange* Change = Buffers().GetDiskChange(Id);
			if (TestTrue(TEXT("the bar is still up"), Change != nullptr))
			{
				TestEqual(TEXT("holding the disk text"), Change->Text, Disk);
			}
			TestEqual(TEXT("the newer disk version is still on disk"), ReadText(Path), Disk);
			TestEqual(TEXT("the tab keeps the journal's text"), Live()->GetTextAsString(), Journal);
			TestTrue(TEXT("the tab shows the dirty mark"), SDreamEditorTabs::GetBadges(*Live()).bDirty);
			TestEqual(TEXT("no auto-save started"), Buffers().GetSavesStartedForTest(Id), C->SavesBefore);
			Buffers().KeepMine(Id);
			return true;
		});
		Steps.Poll(TEXT("Keep mine: the tab's text goes to the file"), Until([Path, Journal, Live]() { return ReadText(Path) == Journal && !Live()->IsDirty(); }), 5.0);
		Steps.Run(Done);
	});

	LatentIt("saves at the new path when the file is renamed while a save runs (scratch-p1-m3)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const TSharedPtr<FDreamBuffer> Created = Tab.IsValid() ? NewScratchFile() : nullptr;
		if (!Created.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Created->GetId();
		const FString OldPath = Created->GetPath();
		const FString NewPath = Dir / TEXT("renamed.py");
		const FString Text = TEXT("renamed = 1\n");
		DreamTest::FDreamSteps Steps(*this);
		// The tree renames on a worker and retargets the tab on the game thread later: here the save starts between the two.
		Steps.Do(TEXT("type; rename the file; save; retarget the tab as the Project tree does"), [this, Id, OldPath, NewPath, Text]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			Type(*B, Text);
			TestTrue(TEXT("renamed on disk"), IFileManager::Get().Move(*NewPath, *OldPath, /*Replace*/ false));
			TestTrue(TEXT("a save started (to the old path)"), Buffers().SaveScratchNow(Id, FDreamBufferManager::EScratchSave::Other));
			B->SetDisplayName(FPaths::GetCleanFilename(NewPath));
			B->SetFileIdentity(NewPath, B->GetBase());
			return true;
		});
		Steps.Poll(TEXT("the text reaches the new file"), Until([Id, NewPath, Text]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			return B.IsValid() && !B->IsDirty() && ReadText(NewPath) == Text;
		}), 10.0);
		Steps.Do(TEXT("the tab stays at the new path"), [this, Id, NewPath]()
		{
			const TSharedPtr<FDreamBuffer> B = Buffers().FindById(Id);
			TestEqual(TEXT("its path"), B->GetPath(), NewPath);
			TestTrue(TEXT("still a scratch file (it is in the folder)"), B->IsScratchFile());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("new_tab() makes a scratch file; new_tab(name=...) a named journaled tab (PO-05, TDD 12.1.1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Name = DreamTest::ScratchName(TEXT("NewTabNamed"));
		const FString Code = FString::Printf(TEXT(
			"import dream_editor\n"
			"print('unnamed=' + dream_editor.new_tab('api_%s = 1\\n'))\n"
			"print('named=' + dream_editor.new_tab('named_%s = 1\\n', name='%s'))\n"), *Sfx, *Sfx, *Name);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<FString> UnnamedHex = MakeShared<FString>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("new_tab twice"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run);
		Steps.Do(TEXT("the tabs appear at run end"), [this, Run, Sfx, Name, UnnamedHex]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			FString Unnamed;
			FString Named;
			for (const DreamTest::FOutputLine& Line : Run->OfType(dream::LineType::Stdout))
			{
				if (Line.Text.StartsWith(TEXT("unnamed=")))
				{
					Unnamed = Line.Text.RightChop(8);
				}
				else if (Line.Text.StartsWith(TEXT("named=")))
				{
					Named = Line.Text.RightChop(6);
				}
			}
			const TSharedPtr<FDreamBuffer> A = Buffers().FindByIdHex(Unnamed.TrimStartAndEnd());
			const TSharedPtr<FDreamBuffer> B = Buffers().FindByIdHex(Named.TrimStartAndEnd());
			if (A.IsValid())
			{
				BufferIds.Add(A->GetId());
			}
			if (B.IsValid())
			{
				BufferIds.Add(B->GetId());
			}
			if (TestTrue(*FString::Printf(TEXT("new_tab() opened a tab (%s)"), *Run->Describe()), A.IsValid()))
			{
				*UnnamedHex = A->GetIdHex();
				TestTrue(TEXT("a scratch file in the scratch folder"), A->IsScratchFile() && DreamScratch::IsScratchFileName(A->GetPath()));
				TestEqual(TEXT("with the text"), A->GetTextAsString(), FString::Printf(TEXT("api_%s = 1\n"), *Sfx));
			}
			if (TestTrue(TEXT("new_tab(name=...) opened a tab"), B.IsValid()))
			{
				TestTrue(TEXT("a named in-memory scratch buffer"), B->GetKind() == EDreamBufferKind::Scratch && !B->HasPath() && B->GetDisplayName() == Name);
			}
			return true;
		});
		Steps.Poll(TEXT("the unnamed tab's text reaches its file"), Until([UnnamedHex, Sfx]()
		{
			const TSharedPtr<FDreamBuffer> A = Buffers().FindByIdHex(*UnnamedHex);
			return A.IsValid() && ReadText(A->GetPath()) == FString::Printf(TEXT("api_%s = 1\n"), *Sfx);
		}), 5.0);
		Steps.Run(Done);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dream.Buffers.Samples
// ---------------------------------------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDreamSamplesSpec, "Dream.Buffers.Samples", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FString Dir;
	TArray<FGuid> BufferIds;
	FString PreviousOverride;
END_DEFINE_SPEC(FDreamSamplesSpec)

void FDreamSamplesSpec::Define()
{
	using namespace DreamScratchFilesSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Dir = NewDir(TEXT("Samples"));
		PreviousOverride = DreamScratch::GetFolderForTest();
		DreamScratch::SetFolderForTest(Dir);
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		FDreamFileSaver::WaitForWorkerWrites(2.0);
		DreamScratch::SetFolderForTest(PreviousOverride);
		RemoveDir(Dir);
		Dir.Reset();
		Tab.Reset();
	});

	It("bundles the six samples, each with its two-line description (PO-05)", [this]()
	{
		const FString Bundled = DreamScratch::GetBundledSamplesFolder();
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Bundled / TEXT("*.py")), true, false);
		Names.Sort();
		TArray<FString> Expected;
		for (const TCHAR* Name : SampleNames)
		{
			Expected.Add(Name);
		}
		TestEqual(*FString::Printf(TEXT("the samples in %s"), *Bundled), FString::Join(Names, TEXT(",")), FString::Join(Expected, TEXT(",")));
		for (const FString& Name : Names)
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *(Bundled / Name));
			TestTrue(*FString::Printf(TEXT("%s starts with a two-line description"), *Name),
				Lines.Num() > 3 && Lines[0].StartsWith(TEXT("# Dream sample: ")) && Lines[1].StartsWith(TEXT("# ")) && Lines[1].Len() > 4);
			TestTrue(*FString::Printf(TEXT("%s carries the SPDX line"), *Name), Lines.Num() > 2 && Lines[2] == TEXT("# SPDX-License-Identifier: Apache-2.0"));
		}
	});

	LatentIt("seeds the samples once, never over a file, and Restore Sample Scripts copies the missing ones (PO-05)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const bool bUserFlagBefore = UDreamUserSettings::Get()->bSamplesSeeded;
		const FString Samples = DreamScratch::GetSamplesFolder();
		TestEqual(TEXT("the Samples folder is under the scratch folder"), Samples, Dir / TEXT("Samples"));
		DreamScratch::SeedSamplesOnce();
		DreamTest::FDreamSteps Steps(*this);
		// (scratch-p1-m7) The copies run on a worker.
		Steps.Poll(TEXT("the seeding finishes"), Until([]() { return !DreamScratch::IsSeedingForTest(); }), 10.0);
		Steps.Do(TEXT("check the seeding and the restore"), [this, Samples, bUserFlagBefore]()
		{
		for (const TCHAR* Name : SampleNames)
		{
			TestEqual(*FString::Printf(TEXT("%s was copied as it is bundled"), Name), ReadText(Samples / Name),
				ReadText(DreamScratch::GetBundledSamplesFolder() / Name));
		}
		TestTrue(TEXT("an override folder keeps its own marker"), IFileManager::Get().FileExists(*(Dir / TEXT(".dream-samples-seeded"))));
		TestEqual(TEXT("and leaves the user's bSamplesSeeded alone"), UDreamUserSettings::Get()->bSamplesSeeded, bUserFlagBefore);

		const FString Edited = TEXT("# my own demo\n");
		TestTrue(TEXT("edit demo.py"), WriteText(Samples / TEXT("demo.py"), Edited));
		IFileManager::Get().Delete(*(Samples / TEXT("cells.py")), false, true, true);
		DreamScratch::SeedSamplesOnce();   // the marker is there: nothing starts
		TestFalse(TEXT("no second seeding"), DreamScratch::IsSeedingForTest());
		TestEqual(TEXT("seeding again overwrites nothing"), ReadText(Samples / TEXT("demo.py")), Edited);
		TestFalse(TEXT("and a deleted sample does not come back"), IFileManager::Get().FileExists(*(Samples / TEXT("cells.py"))));

		bool bInHelp = false;
		for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(FName(TEXT("Help"))))
		{
			bInHelp |= Entry.Name == FName(TEXT("Dream.RestoreSamples"));
		}
		TestTrue(TEXT("Help has Restore Sample Scripts"), bInHelp);
		TestEqual(TEXT("Restore Sample Scripts copies the one missing"), Tab->RestoreSampleScripts(), 1);
		TestTrue(TEXT("cells.py is back"), IFileManager::Get().FileExists(*(Samples / TEXT("cells.py"))));
		TestEqual(TEXT("demo.py keeps the user's edit"), ReadText(Samples / TEXT("demo.py")), Edited);
		TestEqual(TEXT("again: nothing is missing"), Tab->RestoreSampleScripts(), 0);
		TestEqual(TEXT("the user's bSamplesSeeded is still untouched"), UDreamUserSettings::Get()->bSamplesSeeded, bUserFlagBefore);
		return true;
		});
		Steps.Run(Done);
	});

	LatentIt("runs every sample cleanly (PO-05)", FTimespan::FromSeconds(240.0), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		FText Error;
		if (!TestTrue(TEXT("the samples are copied"), DreamScratch::CopyMissingSamples(Error) == static_cast<int32>(UE_ARRAY_COUNT(SampleNames))))
		{
			Done.Execute();
			return;
		}
		// output_styles.py logs on purpose: its warnings at Warning and its unreal.log_error line at Error (§6.3).
		AddExpectedMessagePlain(TEXT("unreal.log_error shows as an error"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedMessagePlain(TEXT("unreal.log_warning shows as a warning"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		// The warning's two lines (the UserWarning line and its source line) both hold this text.
		AddExpectedMessagePlain(TEXT("a Python warning shows as a warning block"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(220.0);
		DreamTest::AddWaitIdle(Steps);
		for (const TCHAR* NamePtr : SampleNames)
		{
			const FString Name = NamePtr;
			const FString Path = DreamScratch::GetSamplesFolder() / Name;
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			TSharedRef<FGuid> Id = MakeShared<FGuid>();
			Steps.Do(FString::Printf(TEXT("open %s"), *Name), [this, Path, Id]()
			{
				if (!TestTrue(*FString::Printf(TEXT("%s opens"), *Path), Tab->OpenLocation(Path, 0, 0)))
				{
					return false;
				}
				const TSharedPtr<FDreamBuffer> B = Buffers().FindByPath(Path);
				*Id = B.IsValid() ? B->GetId() : FGuid();
				BufferIds.Add(*Id);
				return TestTrue(TEXT("a scratch file (it is in the scratch folder)"), B.IsValid() && B->IsScratchFile());
			});
			DreamTest::AddRun(Steps, Name, [Id]()
			{
				const TSharedPtr<FDreamBuffer> B = Buffers().FindById(*Id);
				return DreamTest::MakeBufferRequest(*B, EDreamRunRoute::Buffer, 1, static_cast<int32>(B->GetModel().LineCount()));
			}, Run, 60.0);
			Steps.Do(FString::Printf(TEXT("%s ran cleanly"), *Name), [this, Run, Name]()
			{
				const bool bOk = DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
				TestTrue(*FString::Printf(TEXT("%s printed: %s"), *Name, *Run->Describe()), Run->OfType(dream::LineType::Stdout).Num() > 0
					|| Run->OfType(dream::LineType::Result).Num() > 0);
				if (Name == TEXT("undo_run.py") && bOk)
				{
					// The sample's own point: Undo Last Run takes the spawned light back, so the level stays as it was.
					FText Reason;
					const bool bUndone = FDreamEditorModule::Get().GetRunService().UndoRun(Run->RunNumber, /*bConfirmedPartial*/ true, Reason);
					TestTrue(*FString::Printf(TEXT("Undo Last Run takes undo_run.py back (%s)"), *Reason.ToString()), bUndone);
				}
				if (Name == TEXT("long_run_stop.py"))
				{
					TestTrue(TEXT("the long run reached its end"), Run->HasLineContaining(dream::LineType::Stdout, TEXT("long run done")));
				}
				if (Name == TEXT("structure_demo.py"))
				{
					TestTrue(TEXT("the async code ran"), Run->HasLineContaining(dream::LineType::Stdout, TEXT("square 0.25")));
				}
				return true;
			});
		}
		Steps.Run(Done);
	});
}
