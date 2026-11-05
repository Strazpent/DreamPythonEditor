// SPDX-License-Identifier: Apache-2.0
// Dream.Persistence.Journal: a scratch buffer survives a session through the write-ahead journal (BUF-01, PER-01,
// §10.2-§10.3, T-PER-040). The round trip runs on a private storage root in the user temp folder with its own
// FDreamJournal, so the editor's own journal and session are untouched: open, scan, Reconcile, BUF_OPEN + SNAP, a typed
// DELTA, SESSION, a forced commit and the clean shutdown; then a second journal on the same root scans it and the
// recovered text restores the buffer with its identity. The second case closes a non-empty scratch tab in the Dream tab
// and brings it back with Reopen Closed Tab (Ctrl+Shift+T), which BUF_CLOSE keeps possible (§10.2.5). On private roots too:
// a safe-mode session carries the crashed session's buffers and its pending recovery into the next one (PER-01); a scan
// result a callback does not consume waits for the next request, and run snapshots before the reconcile are durable at
// once or held until it (PER-02, PER-09); commits made while the disk fails are replayed in order once it recovers, the
// plain text commits made meanwhile fold into the waiting requests instead of queueing each, and a run snapshot behind the
// failure fails at once with the reason (PER-08, P2-per-02); a damaged generation set aside by the real Win32 file layer
// is dated from the set-aside, so the 30-day retention keeps one last written long ago (P2-per-01); a run whose RUN_BEGIN
// was durable before a disk outage and that never ended is the one lost run after an unclean close, while the run that
// ended during the outage is not (its RUN_END replayed) (P3-per-02).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Persistence/DreamJournal.h"
#include "Shell/SDreamTab.h"

namespace DreamJournalSpec
{
	struct FState
	{
		FDreamStorageRoot Root;
		TSharedPtr<FDreamJournal> Journal;
		TSharedPtr<FDreamBuffer> Buffer;
		FString FinalText;
		FDateTime BeforeEditsUtc;
		bool bScanned = false;
		FDreamRecoveredSession Session;
	};

	FDreamStorageRoot MakePrivateRoot()
	{
		const FString Id = DreamTest::UniqueSuffix();
		FDreamStorageRoot Root;
		Root.Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("DreamTier4"), Id));
		Root.ProjectHash = FString::Printf(TEXT("DreamTier4-%s"), *Id.Left(6));
		return Root;
	}

	/** Opens a journal on Root and queues its recovery scan; the result lands in State. */
	bool OpenAndScan(FAutomationTestBase& Test, FState& State, const TCHAR* Which)
	{
		State.bScanned = false;
		State.Session = FDreamRecoveredSession();
		State.Journal = MakeShared<FDreamJournal>();
		EDreamInstanceRole Role = EDreamInstanceRole::Secondary;
		if (!State.Journal->Open(State.Root, Role) || !State.Journal->IsOpen())
		{
			Test.AddError(FString::Printf(TEXT("%s journal: Open failed on %s"), Which, *State.Root.Root));
			return false;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s journal is the primary instance of its root"), Which), Role == EDreamInstanceRole::Primary);
		TWeakPtr<FDreamJournal> Weak = State.Journal;
		FState* StatePtr = &State;
		State.Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([StatePtr, Weak](FDreamRecoveredSession& Session) -> bool
		{
			if (!Weak.IsValid())
			{
				return false;
			}
			StatePtr->Session = MoveTemp(Session);
			StatePtr->bScanned = true;
			return true;
		}));
		return true;
	}

	/** A scratch buffer the journal tracks, as the shell creates one (no buffer manager: the editor's journal never sees it). */
	TSharedRef<FDreamBuffer> MakeScratch(const FString& Purpose, const FString& Text)
	{
		TSharedRef<FDreamBuffer> Buffer = MakeShared<FDreamBuffer>(FGuid::NewGuid(), EDreamBufferKind::Scratch, DreamTest::ScratchName(Purpose));
		Buffer->ResetText(Text, dream::EditOrigin::Load);
		return Buffer;
	}

	FDreamRunId UniqueRunId()
	{
		return (static_cast<FDreamRunId>(FPlatformTime::Cycles64()) & 0xFFFFFFFFull) | (0x5u * 0x100000000ull);
	}

	void CloseJournal(FState& State)
	{
		if (State.Journal.IsValid())
		{
			if (State.Journal->IsOpen())
			{
				State.Journal->Shutdown();
			}
			State.Journal.Reset();
		}
	}

	dream::EditBatch AppendEdit(const FDreamBuffer& Buffer, const FString& Text)
	{
		dream::EditBatch Batch;
		Batch.Origin = dream::EditOrigin::Typing;
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = Buffer.GetModel().Length();
		Range.DeleteLen = 0;
		Range.Insert = std::u16string(DreamText::ToU16(Text));
		return Batch;
	}
}

BEGIN_DEFINE_SPEC(FDreamJournalSpec, "Dream.Persistence.Journal", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamJournalSpec)

void FDreamJournalSpec::Define()
{
	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	LatentIt("restores a scratch buffer's text and identity from the journal in the next session", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("open the first session's journal"), [this, State]()
		{
			IFileManager::Get().MakeDirectory(*State->Root.Root, /*Tree*/ true);
			return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("first"));
		});
		Steps.Poll(TEXT("first recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);

		Steps.Do(TEXT("journal a scratch buffer and a typed edit"), [this, State]()
		{
			TestEqual(TEXT("a fresh root recovers no buffer"), State->Session.Buffers.Num(), 0);
			// The first commit of a session waits for Reconcile, which the shell calls after its restore (§10.2.3, §10.3).
			State->Journal->Reconcile({});

			const FString Name = DreamTest::ScratchName(TEXT("Journal"));
			State->Buffer = MakeShared<FDreamBuffer>(FGuid::NewGuid(), EDreamBufferKind::Scratch, Name);
			State->Buffer->ResetText(TEXT("first = 1"), dream::EditOrigin::Load);
			State->BeforeEditsUtc = FDateTime::UtcNow();
			State->Journal->OnBufferOpened(State->Buffer.ToSharedRef());

			const dream::ChangeSet Change = State->Buffer->ApplyEdit(DreamJournalSpec::AppendEdit(*State->Buffer, TEXT("\nsecond = 'zwei ü'")));
			if (!TestFalse(TEXT("the typed edit applied"), Change.IsEmpty()))
			{
				return false;
			}
			State->FinalText = State->Buffer->GetTextAsString();
			State->Journal->OnBufferEdited(State->Buffer->GetId());
			const TArray<FGuid> Order = { State->Buffer->GetId() };
			State->Journal->SetSessionState(Order, State->Buffer->GetId());
			State->Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("a durable commit"), [State]()
		{
			return State->Journal->GetLastDurableUtc() >= State->BeforeEditsUtc ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("end the first session cleanly"), [this, State]()
		{
			TestTrue(TEXT("journal health Ok"), State->Journal->GetHealth() == EDreamJournalHealth::Ok);
			DreamJournalSpec::CloseJournal(*State);
			return true;
		});

		Steps.Do(TEXT("open the next session's journal"), [this, State]() { return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("second")); });
		Steps.Poll(TEXT("second recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);

		Steps.Do(TEXT("restore the scratch buffer"), [this, State]()
		{
			const FDreamRecoveredSession& Session = State->Session;
			TestFalse(TEXT("the journal is readable"), Session.bUnreadable);
			TestTrue(TEXT("the previous session ended cleanly"), Session.bPreviousExitClean);
			const FGuid Id = State->Buffer->GetId();
			const FDreamRecoveredBuffer* Recovered = Session.Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& Each) { return Each.Id == Id; });
			if (!Recovered)
			{
				AddError(FString::Printf(TEXT("the scratch buffer is not in the recovered session (%d buffers)"), Session.Buffers.Num()));
				return false;
			}
			TestTrue(TEXT("recovered as a scratch buffer"), Recovered->Kind == EDreamBufferKind::Scratch);
			TestEqual(TEXT("display name"), Recovered->DisplayName, State->Buffer->GetDisplayName());
			TestEqual(TEXT("the recovered text holds the typed edit"), Recovered->RecoveredText, State->FinalText);
			TestTrue(TEXT("a scratch buffer has no path"), Recovered->Path.IsEmpty());

			// The model-level restore the tab performs (FDreamBufferManager::AddRestored): same id, kind, name and text.
			TSharedRef<FDreamBuffer> Restored = MakeShared<FDreamBuffer>(Recovered->Id, Recovered->Kind, Recovered->DisplayName);
			Restored->ResetText(Recovered->RecoveredText, dream::EditOrigin::Load);
			TestEqual(TEXT("the restored buffer's text"), Restored->GetTextAsString(), State->FinalText);
			TestEqual(TEXT("the restored buffer keeps its journal uuid"), Restored->GetIdHex(), State->Buffer->GetIdHex());
			State->Journal->Reconcile({ Restored });
			return true;
		});

		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			DreamJournalSpec::CloseJournal(*State);
			if (!State->Root.Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("brings a closed non-empty scratch tab back with Reopen Closed Tab (T-PER-040)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Text = FString::Printf(TEXT("reopen_%s = 'kept'\nprint(reopen_%s)"), *Sfx, *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Text, TEXT("Reopen"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		const FString Name = Buffer->GetDisplayName();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("close the tab (Ctrl+W)"), [this, Id]()
		{
			Tab->ActivateBuffer(Id);
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::CloseTab, Reason))
			{
				AddError(FString::Printf(TEXT("Close Tab cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::CloseTab);
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("reopen it (Ctrl+Shift+T)"), [this, Id, Text, Name]()
		{
			TestFalse(TEXT("the closed tab has no code view"), Tab->FindCodeView(Id).IsValid());
			FText Reason;
			if (!Tab->CanExecuteDreamCommand(EDreamCommand::ReopenClosedTab, Reason))
			{
				AddError(FString::Printf(TEXT("Reopen Closed Tab cannot execute: %s"), *Reason.ToString()));
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::ReopenClosedTab);
			const TSharedPtr<FDreamBuffer> Reopened = Tab->GetActiveBuffer();
			if (!TestTrue(TEXT("Reopen Closed Tab activates a buffer"), Reopened.IsValid()))
			{
				return false;
			}
			BufferIds.Add(Reopened->GetId());
			TestEqual(TEXT("the reopened tab's text"), Reopened->GetTextAsString(), Text);
			TestEqual(TEXT("the reopened tab's name"), Reopened->GetDisplayName(), Name);
			TestTrue(TEXT("the reopened tab is a scratch tab"), Reopened->GetKind() == EDreamBufferKind::Scratch);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("carries a crashed session through safe mode into the next session, pending recovery included (PER-01)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		TSharedRef<TSharedPtr<FDreamBuffer>> FileBuffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		TSharedRef<FString> FilePath = MakeShared<FString>();
		TSharedRef<FString> EditedText = MakeShared<FString>();
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("session 1: a scratch buffer and a dirty file buffer"), [this, State, FileBuffer, FilePath]()
		{
			IFileManager::Get().MakeDirectory(*State->Root.Root, /*Tree*/ true);
			*FilePath = State->Root.Root / TEXT("carried.py");
			if (!FFileHelper::SaveStringToFile(FString(TEXT("disk = 1\n")), **FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				AddError(FString::Printf(TEXT("could not write %s"), **FilePath));
				return false;
			}
			return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("session 1"));
		});
		Steps.Poll(TEXT("session 1 scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("journal both buffers, then crash"), [this, State, FileBuffer, FilePath, EditedText]()
		{
			State->Journal->Reconcile({});
			State->Buffer = DreamJournalSpec::MakeScratch(TEXT("Carried"), TEXT("carried_scratch = 1"));
			TSharedRef<FDreamBuffer> File = MakeShared<FDreamBuffer>(FGuid::NewGuid(), EDreamBufferKind::File, TEXT("carried.py"));
			File->ResetText(TEXT("disk = 1\n"), dream::EditOrigin::Load);
			File->SetFileIdentity(*FilePath, TOptional<FDreamBaseVersion>());
			File->ApplyEdit(DreamJournalSpec::AppendEdit(*File, TEXT("edited = 2\n")));
			*EditedText = File->GetTextAsString();
			*FileBuffer = File;
			State->BeforeEditsUtc = FDateTime::UtcNow();
			State->Journal->OnBufferOpened(State->Buffer.ToSharedRef());
			State->Journal->OnBufferOpened(File);
			State->Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("session 1 commits are durable"), [State]()
		{
			return State->Journal->GetLastDurableUtc() >= State->BeforeEditsUtc ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("session 1 ends without CLEAN"), [State]()
		{
			State->Journal->CloseWithoutCleanForTest();
			State->Journal.Reset();
			return true;
		});

		Steps.Do(TEXT("session 2 (safe mode): scan"), [this, State]() { return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("session 2")); });
		Steps.Poll(TEXT("session 2 scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("session 2 restores nothing and exits cleanly"), [this, State, FileBuffer]()
		{
			const FDreamRecoveredSession& Session = State->Session;
			TestFalse(TEXT("session 1 ended unexpectedly"), Session.bPreviousExitClean);
			const FGuid FileId = (*FileBuffer)->GetId();
			const FDreamRecoveredBuffer* File = Session.Buffers.FindByPredicate([&FileId](const FDreamRecoveredBuffer& B) { return B.Id == FileId; });
			TestTrue(TEXT("the dirty file buffer is a counted recovery"), File != nullptr && File->bCounted);
			// What SDreamTab::ApplySafeMode does: no tab restored, the skipped session carried (§10.7.1, §10.8).
			State->Journal->Reconcile({}, &Session);
			DreamJournalSpec::CloseJournal(*State);
			return true;
		});

		Steps.Do(TEXT("session 3: scan"), [this, State]() { return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("session 3")); });
		Steps.Poll(TEXT("session 3 scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("session 3 still finds both buffers"), [this, State, FileBuffer, EditedText]()
		{
			const FDreamRecoveredSession& Session = State->Session;
			TestTrue(TEXT("session 2 ended cleanly"), Session.bPreviousExitClean);
			const FGuid ScratchId = State->Buffer->GetId();
			const FGuid FileId = (*FileBuffer)->GetId();
			const FDreamRecoveredBuffer* Scratch = Session.Buffers.FindByPredicate([&ScratchId](const FDreamRecoveredBuffer& B) { return B.Id == ScratchId; });
			const FDreamRecoveredBuffer* File = Session.Buffers.FindByPredicate([&FileId](const FDreamRecoveredBuffer& B) { return B.Id == FileId; });
			if (TestTrue(*FString::Printf(TEXT("the scratch buffer was carried through safe mode (%d buffers)"), Session.Buffers.Num()), Scratch != nullptr))
			{
				TestEqual(TEXT("its text"), Scratch->RecoveredText, State->Buffer->GetTextAsString());
			}
			if (TestTrue(TEXT("the file buffer was carried through safe mode"), File != nullptr))
			{
				TestTrue(TEXT("it is still a pending recovery"), File->bCounted);
				TestEqual(TEXT("with the edited text"), File->RecoveredText, *EditedText);
			}
			State->Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			DreamJournalSpec::CloseJournal(*State);
			IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});

	LatentIt("keeps an unconsumed scan for the next request, and treats run snapshots before the reconcile right (PER-02, PER-09)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FScan
		{
			int32 First = 0;
			int32 Second = 0;
			int32 Third = 0;
			uint64 FirstGeneration = MAX_uint64;
			uint64 SecondGeneration = MAX_uint64 - 1;
			FDreamJournalTicket Held;
			FDreamRunId HeldRun = 0;
		};
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		TSharedRef<FScan> Scan = MakeShared<FScan>();
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("open, and a scan callback that does not consume"), [this, State, Scan]()
		{
			IFileManager::Get().MakeDirectory(*State->Root.Root, /*Tree*/ true);
			State->Journal = MakeShared<FDreamJournal>();
			EDreamInstanceRole Role = EDreamInstanceRole::Secondary;
			if (!State->Journal->Open(State->Root, Role))
			{
				AddError(TEXT("Open failed"));
				return false;
			}
			// A tab closed before the result arrived (SDreamTab's callback returns false then).
			State->Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([Scan](FDreamRecoveredSession& Session) -> bool
			{
				++Scan->First;
				Scan->FirstGeneration = Session.SourceGeneration;
				return false;
			}));
			return true;
		});
		Steps.Poll(TEXT("the first delivery"), [Scan]() { return Scan->First > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("nobody waits for the scan: a run snapshot is durable at once"), [this, State, Scan]()
		{
			TestEqual(TEXT("the first callback ran once"), Scan->First, 1);
			const FDreamJournalTicket Ticket = State->Journal->SnapshotForRun(DreamJournalSpec::UniqueRunId(), 1, FGuid(), dream::TextSnapshot(),
				EDreamRunRoute::Repl, false);
			TestEqual(TEXT("a zero ticket (no reconcile is coming)"), Ticket.Seq, uint64(0));
			TestTrue(TEXT("durable at once"), State->Journal->IsDurable(Ticket));
			// The reopened tab asks again and consumes.
			State->Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([Scan](FDreamRecoveredSession& Session) -> bool
			{
				++Scan->Second;
				Scan->SecondGeneration = Session.SourceGeneration;
				return true;
			}));
			// A scan is awaited now: the snapshot is a real commit, held until the reconcile (§10.2.3, PER-09).
			Scan->HeldRun = DreamJournalSpec::UniqueRunId() + 1;
			Scan->Held = State->Journal->SnapshotForRun(Scan->HeldRun, 2, FGuid(), dream::TextSnapshot(), EDreamRunRoute::Repl, false);
			TestTrue(TEXT("a real ticket while a scan is awaited"), Scan->Held.Seq != 0);
			return true;
		});
		Steps.Poll(TEXT("the second delivery"), [Scan]() { return Scan->Second > 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.WaitFrames(5);
		Steps.Do(TEXT("the same result, and the held snapshot waits for the reconcile"), [this, State, Scan]()
		{
			TestEqual(TEXT("the kept result was delivered once more"), Scan->Second, 1);
			TestEqual(TEXT("the same scan (source generation)"), Scan->SecondGeneration, Scan->FirstGeneration);
			TestFalse(TEXT("the snapshot is not durable before the reconcile"), State->Journal->IsDurable(Scan->Held));
			State->Journal->Reconcile({});
			// A third request finds nothing: the result was consumed.
			State->Journal->RequestRecoveryScan(FDreamOnRecoveryScan::CreateLambda([Scan](FDreamRecoveredSession&) -> bool
			{
				++Scan->Third;
				return true;
			}));
			return true;
		});
		Steps.Poll(TEXT("the held snapshot is durable after the reconcile"), [State, Scan]()
		{
			return State->Journal->IsDurable(Scan->Held) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("a consumed result is not delivered again"), [this, State, Scan]()
		{
			TestEqual(TEXT("the third request got nothing"), Scan->Third, 0);
			State->Journal->MarkRunEnded(Scan->HeldRun, EDreamRunOutcome::Ok);
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			DreamJournalSpec::CloseJournal(*State);
			IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});

	LatentIt("replays commits made while the disk fails, in order, and fails a run snapshot behind the failure at once (PER-08)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		struct FOutage
		{
			TSharedPtr<FDreamBuffer> Closed;
			TSharedPtr<FDreamBuffer> Kept;
			FDreamJournalTicket Behind;
			FDreamRunId BehindRun = 0;
			FDreamJournalTicket After;
			FDreamRunId AfterRun = 0;
			FDreamJournalTicket Fence;
			FDreamRunId FenceRun = 0;
		};
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		TSharedRef<FOutage> Outage = MakeShared<FOutage>();
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("open the journal"), [this, State]()
		{
			IFileManager::Get().MakeDirectory(*State->Root.Root, /*Tree*/ true);
			return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("first"));
		});
		Steps.Poll(TEXT("first recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a durable buffer"), [State, Outage]()
		{
			State->Journal->Reconcile({});
			Outage->Closed = DreamJournalSpec::MakeScratch(TEXT("Outage"), TEXT("closed_later = 0"));
			State->BeforeEditsUtc = FDateTime::UtcNow();
			State->Journal->OnBufferOpened(Outage->Closed.ToSharedRef());
			State->Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("it is durable"), [State]()
		{
			return State->Journal->GetLastDurableUtc() >= State->BeforeEditsUtc ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the disk fails: the first commit"), [State, Outage]()
		{
			State->Journal->SetFailCommitsForTest(true);
			Outage->Closed->ApplyEdit(DreamJournalSpec::AppendEdit(*Outage->Closed, TEXT("\nedit_1 = 1")));
			State->Journal->OnBufferEdited(Outage->Closed->GetId());
			State->Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("the failed commit waits for the retry"), [State]()
		{
			return State->Journal->GetRetryQueueLengthForTest() == 1 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("more edits, a new buffer, a run snapshot"), [State, Outage]()
		{
			for (int32 Edit = 2; Edit <= 5; ++Edit)
			{
				Outage->Closed->ApplyEdit(DreamJournalSpec::AppendEdit(*Outage->Closed, FString::Printf(TEXT("\nedit_%d = %d"), Edit, Edit)));
				State->Journal->OnBufferEdited(Outage->Closed->GetId());
				State->Journal->ForceCommit();
			}
			Outage->Kept = DreamJournalSpec::MakeScratch(TEXT("OutageKept"), TEXT("kept = 1"));
			State->Journal->OnBufferOpened(Outage->Kept.ToSharedRef());
			Outage->BehindRun = DreamJournalSpec::UniqueRunId();
			Outage->Behind = State->Journal->SnapshotForRun(Outage->BehindRun, 3, Outage->Kept->GetId(), Outage->Kept->Snapshot(), EDreamRunRoute::Buffer, false);
			return true;
		});
		Steps.Poll(TEXT("the run snapshot behind the failure fails"), [State, Outage]()
		{
			FText Reason;
			return State->Journal->HasFailed(Outage->Behind, Reason) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the run ends not executed; the first buffer closes; the second is edited"), [this, State, Outage]()
		{
			TestTrue(TEXT("journal health Degraded"), State->Journal->GetHealth() == EDreamJournalHealth::Degraded);
			TestFalse(TEXT("the failed snapshot is not durable"), State->Journal->IsDurable(Outage->Behind));
			// The writer queued every request before it marked the snapshot's ticket: the four text commits folded into the
			// failed one, and only the BUF_OPEN and the RUN_BEGIN wait apart (1 + 4 + 2 = 7 without folding).
			TestEqual(TEXT("the plain text commits fold into the waiting requests (P2-per-02)"), State->Journal->GetRetryQueueLengthForTest(), 3);
			State->Journal->MarkRunEnded(Outage->BehindRun, EDreamRunOutcome::NotExecuted);
			State->Journal->OnBufferClosed(Outage->Closed->GetId(), TEXT("closed"));
			for (int32 Edit = 1; Edit <= 3; ++Edit)
			{
				Outage->Kept->ApplyEdit(DreamJournalSpec::AppendEdit(*Outage->Kept, FString::Printf(TEXT("\nkept_%d = %d"), Edit, Edit)));
				State->Journal->OnBufferEdited(Outage->Kept->GetId());
				State->Journal->ForceCommit();
			}
			// A REPL run's snapshot (RUN_BEGIN only) last: once its ticket failed, the writer has queued everything above.
			Outage->FenceRun = DreamJournalSpec::UniqueRunId() + 2;
			Outage->Fence = State->Journal->SnapshotForRun(Outage->FenceRun, 5, FGuid(), Outage->Kept->Snapshot(), EDreamRunRoute::Repl, false);
			return true;
		});
		Steps.Poll(TEXT("the last run snapshot behind the failure fails"), [State, Outage]()
		{
			FText Reason;
			return State->Journal->HasFailed(Outage->Fence, Reason) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the disk recovers"), [this, State, Outage]()
		{
			// RUN_END, BUF_CLOSE and the last RUN_BEGIN wait apart; the three text commits joined the newest (3 + 3 + 3 = 9
			// without folding).
			TestEqual(TEXT("the later text commits fold too (P2-per-02)"), State->Journal->GetRetryQueueLengthForTest(), 6);
			State->Journal->MarkRunEnded(Outage->FenceRun, EDreamRunOutcome::NotExecuted);
			State->Journal->SetFailCommitsForTest(false);
			return true;
		});
		Steps.Poll(TEXT("the retry succeeds"), [State]()
		{
			return State->Journal->GetHealth() == EDreamJournalHealth::Ok ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("a run snapshot after the recovery"), [State, Outage]()
		{
			Outage->AfterRun = DreamJournalSpec::UniqueRunId() + 1;
			Outage->After = State->Journal->SnapshotForRun(Outage->AfterRun, 4, Outage->Kept->GetId(), Outage->Kept->Snapshot(), EDreamRunRoute::Buffer, false);
			return true;
		});
		Steps.Poll(TEXT("it is durable"), [State, Outage]()
		{
			return State->Journal->IsDurable(Outage->After) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("end the session cleanly"), [State, Outage]()
		{
			State->Journal->MarkRunEnded(Outage->AfterRun, EDreamRunOutcome::Ok);
			DreamJournalSpec::CloseJournal(*State);
			return true;
		});
		Steps.Do(TEXT("open the next session's journal"), [this, State]() { return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("second")); });
		Steps.Poll(TEXT("second recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("every record of the outage reached the disk"), [this, State, Outage]()
		{
			const FDreamRecoveredSession& Session = State->Session;
			const FGuid ClosedId = Outage->Closed->GetId();
			const FGuid KeptId = Outage->Kept->GetId();
			TestTrue(TEXT("the session ended cleanly"), Session.bPreviousExitClean);
			TestTrue(TEXT("the BUF_CLOSE written during the outage was replayed"), Session.ClosedIds.Contains(ClosedId));
			TestFalse(TEXT("the closed buffer is not restored"), Session.Buffers.ContainsByPredicate([&ClosedId](const FDreamRecoveredBuffer& B) { return B.Id == ClosedId; }));
			const FDreamRecoveredBuffer* Kept = Session.Buffers.FindByPredicate([&KeptId](const FDreamRecoveredBuffer& B) { return B.Id == KeptId; });
			if (TestTrue(TEXT("the buffer opened during the outage was journaled (BUF_OPEN replayed)"), Kept != nullptr))
			{
				TestEqual(TEXT("with its newest text"), Kept->RecoveredText, Outage->Kept->GetTextAsString());
			}
			TestEqual(TEXT("no RUN_BEGIN is left without its RUN_END after the replay"), Session.LostRuns.Num(), 0);
			State->Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			if (State->Journal.IsValid())
			{
				State->Journal->SetFailCommitsForTest(false);
			}
			DreamJournalSpec::CloseJournal(*State);
			IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});

	LatentIt("reports a run that never ended as lost after a disk outage and an unclean close (P3-per-02)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FLost
		{
			TSharedPtr<FDreamBuffer> Buffer;
			FDreamRunId EndedRun = 0;
			FDreamRunId LostRun = 0;
			FDreamJournalTicket Ended;
			FDreamJournalTicket Lost;
		};
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		TSharedRef<FLost> Runs = MakeShared<FLost>();
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("open the journal"), [this, State]()
		{
			IFileManager::Get().MakeDirectory(*State->Root.Root, /*Tree*/ true);
			return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("first"));
		});
		Steps.Poll(TEXT("first recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("a buffer and two runs that begin before the outage"), [State, Runs]()
		{
			State->Journal->Reconcile({});
			Runs->Buffer = DreamJournalSpec::MakeScratch(TEXT("LostRun"), TEXT("lost = 0"));
			State->Journal->OnBufferOpened(Runs->Buffer.ToSharedRef());
			Runs->EndedRun = DreamJournalSpec::UniqueRunId();
			Runs->LostRun = Runs->EndedRun + 1;
			Runs->Ended = State->Journal->SnapshotForRun(Runs->EndedRun, 11, Runs->Buffer->GetId(), Runs->Buffer->Snapshot(), EDreamRunRoute::Buffer, false);
			Runs->Lost = State->Journal->SnapshotForRun(Runs->LostRun, 12, FGuid(), Runs->Buffer->Snapshot(), EDreamRunRoute::Repl, false);
			return true;
		});
		Steps.Poll(TEXT("both RUN_BEGIN records are durable"), [State, Runs]()
		{
			return State->Journal->IsDurable(Runs->Ended) && State->Journal->IsDurable(Runs->Lost) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the disk fails: one run ends and the buffer is edited during the outage"), [State, Runs]()
		{
			State->Journal->SetFailCommitsForTest(true);
			State->Journal->MarkRunEnded(Runs->EndedRun, EDreamRunOutcome::Ok);
			Runs->Buffer->ApplyEdit(DreamJournalSpec::AppendEdit(*Runs->Buffer, TEXT("\nduring_outage = 1")));
			State->Journal->OnBufferEdited(Runs->Buffer->GetId());
			State->Journal->ForceCommit();
			return true;
		});
		Steps.Poll(TEXT("the failed commits wait for the retry"), [State]()
		{
			return State->Journal->GetRetryQueueLengthForTest() >= 1 && State->Journal->GetHealth() == EDreamJournalHealth::Degraded
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the disk recovers"), [State]()
		{
			State->Journal->SetFailCommitsForTest(false);
			return true;
		});
		Steps.Poll(TEXT("the retry succeeds"), [State]()
		{
			return State->Journal->GetHealth() == EDreamJournalHealth::Ok && State->Journal->GetRetryQueueLengthForTest() == 0
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("the session ends without CLEAN while the other run never ended"), [State]()
		{
			State->Journal->CloseWithoutCleanForTest();
			State->Journal.Reset();
			return true;
		});
		Steps.Do(TEXT("open the next session's journal"), [this, State]() { return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("second")); });
		Steps.Poll(TEXT("second recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("the run that never ended is the one lost run"), [this, State, Runs]()
		{
			const FDreamRecoveredSession& Session = State->Session;
			TestFalse(TEXT("the session ended unexpectedly"), Session.bPreviousExitClean);
			if (TestEqual(TEXT("exactly one lost run: the RUN_END written during the outage was replayed for the other"), Session.LostRuns.Num(), 1))
			{
				const FDreamLostRun& Lost = Session.LostRuns[0];
				TestEqual(TEXT("its number is the run that never ended"), Lost.RunNumber, 12);
				TestTrue(TEXT("its route is the REPL"), Lost.Route == EDreamRunRoute::Repl);
				if (Lost.RunId != 0)
				{
					TestEqual(TEXT("its run ID is the RUN_BEGIN's"), static_cast<uint64>(Lost.RunId), static_cast<uint64>(Runs->LostRun));
				}
				else
				{
					AddInfo(TEXT("the reader passed no run ID for the lost run (FDreamLostRun::RunId 0)"));
				}
			}
			const FGuid BufferId = Runs->Buffer->GetId();
			const FDreamRecoveredBuffer* Kept = Session.Buffers.FindByPredicate([&BufferId](const FDreamRecoveredBuffer& B) { return B.Id == BufferId; });
			if (TestTrue(TEXT("the buffer survived the outage"), Kept != nullptr))
			{
				TestEqual(TEXT("with the edit made during the outage"), Kept->RecoveredText, Runs->Buffer->GetTextAsString());
			}
			State->Journal->Reconcile({});
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			if (State->Journal.IsValid())
			{
				State->Journal->SetFailCommitsForTest(false);
			}
			DreamJournalSpec::CloseJournal(*State);
			IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});

	LatentIt("dates a damaged generation from its set-aside, so the 30-day retention keeps it (P2-per-01, TDD 10.2.4)",   // (tests) ASCII test name
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<DreamJournalSpec::FState> State = MakeShared<DreamJournalSpec::FState>();
		State->Root = DreamJournalSpec::MakePrivateRoot();
		// GenerationFileName: wal.<16 hex digits>.sjl.
		const FString Damaged = State->Root.JournalDir(TEXT("main")) / TEXT("wal.0000000000000001.sjl");
		DreamTest::FDreamSteps Steps(*this);

		Steps.Do(TEXT("a damaged generation last written 40 days ago"), [this, State, Damaged]()
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Damaged), /*Tree*/ true);
			if (!TestTrue(TEXT("the damaged generation is written"), FFileHelper::SaveStringToFile(TEXT("garbage, no header"), *Damaged)))
			{
				return false;
			}
			IFileManager::Get().SetTimeStamp(*Damaged, FDateTime::UtcNow() - FTimespan::FromDays(40));
			if (!TestTrue(TEXT("its last write time is 40 days ago"), (FDateTime::UtcNow() - IFileManager::Get().GetTimeStamp(*Damaged)).GetTotalDays() > 39.0))
			{
				return false;
			}
			return DreamJournalSpec::OpenAndScan(*this, *State, TEXT("first"));
		});
		Steps.Poll(TEXT("recovery scan"), [State]() { return State->bScanned ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 10.0);
		Steps.Do(TEXT("this session's generation starts"), [this, State]()
		{
			TestTrue(TEXT("the scan found no usable generation"), State->Session.bUnreadable);
			State->Journal->Reconcile({});
			State->Buffer = DreamJournalSpec::MakeScratch(TEXT("Corrupt"), TEXT("after_corrupt = 1"));
			State->BeforeEditsUtc = FDateTime::UtcNow();
			State->Journal->OnBufferOpened(State->Buffer.ToSharedRef());
			State->Journal->ForceCommit();
			return true;
		});
		// The checkpoint is durable: StartGeneration ran, and its RemoveExpiredCorrupt with it.
		Steps.Poll(TEXT("the first commit is durable"), [State]()
		{
			return State->Journal->GetLastDurableUtc() >= State->BeforeEditsUtc ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("the damaged generation is kept, dated from the set-aside"), [this, Damaged]()
		{
			const FString Corrupt = Damaged + TEXT(".corrupt");
			if (!TestTrue(TEXT("wal.0000000000000001.sjl.corrupt survived the new generation's retention pass"), IFileManager::Get().FileExists(*Corrupt)))
			{
				return false;
			}
			const double Minutes = FMath::Abs((IFileManager::Get().GetTimeStamp(*Corrupt) - FDateTime::UtcNow()).GetTotalMinutes());
			TestTrue(*FString::Printf(TEXT("its last write time is the set-aside's, not 40 days ago (%.1f min off; FDreamWin32FileOps::Touch)"), Minutes),
				Minutes < 10.0);
			return true;
		});
		Steps.Finally(TEXT("remove the private root"), [State]()
		{
			DreamJournalSpec::CloseJournal(*State);
			IFileManager::Get().DeleteDirectory(*State->Root.Root, /*RequireExists*/ false, /*Tree*/ true);
		});
		Steps.Run(Done);
	});
}
