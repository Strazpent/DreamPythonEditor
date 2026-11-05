// SPDX-License-Identifier: Apache-2.0
// Dream.Transactions.UndoLastRun: one undo step per run (RUN-07, U3, §5.9.1-§5.9.2) and Undo Last Run (RUN-22, §5.9.5).
// A buffer run whose two statements each move a fixture actor records exactly one transaction titled
// "Dream: <tab> #N"; FDreamRunService::UndoRun (what the toolbar and Ctrl+Alt+Z call) reverts both moves at once.
// The script calls modify() on the root component itself: SetActorLocation does not, so a script that wants its move
// undoable must (the engine's own gizmo does the same). More cases classify a lost transaction (§5.9.3): a reset of the
// undo history during the run reads Purged even from an empty history (P2-run-01), also after the record stopped at 100%
// (§5.9.4: a reset of an inactive buffer raises no cancel event, P3-run-03), but an undo without redo that empties the
// queue after that end is no reset and keeps Partial (P4-run-01), nor is a transient record popped on the queue it emptied
// (P5-txo-01); and a script that cancels the run's transaction and then
// records its own over a redo stack reads CancelledByScript, not Purged (A1-02).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Buffers/DreamBufferManager.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Run/DreamTransactions.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamOutputRows.h"

namespace DreamUndoRunSpec
{
	struct FState
	{
		TWeakObjectPtr<AStaticMeshActor> Actor;
		FString Label;
		int32 QueueBefore = 0;
		bool bSettingSaved = false;
		bool bSavedTransactionBuffer = true;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
		int32 RunNumber = 0;
		bool bResetDone = false;
		FDelegateHandle PreTickHandle;
		TOptional<SIZE_T> SavedBudget;
	};

	AStaticMeshActor* SpawnFixture(FAutomationTestBase& Test, FState& State)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		FActorSpawnParameters Params;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			Test.AddError(TEXT("could not spawn the fixture actor"));
			return nullptr;
		}
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Actor->SetActorLabel(State.Label);
		State.Actor = Actor;
		return Actor;
	}

	/** Pins the REPL route's One undo step per run and the pumping presentation; RestoreSettingsAndActor undoes it. */
	void PinSettings(FState& State)
	{
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		State.bSavedTransactionBuffer = Settings->bTransactionRepl;
		State.bSavedRunPump = Settings->bRunPump;
		State.SavedPresentation = Settings->RunPresentation;
		State.bSettingSaved = true;
		Settings->bTransactionRepl = true;
		Settings->bRunPump = true;
		Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
	}

	void RestoreSettingsAndActor(FState& State)
	{
		if (State.PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreTick().Remove(State.PreTickHandle);
			State.PreTickHandle.Reset();
		}
		if (State.SavedBudget.IsSet() && FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetTransactions().SetBudgetBytesForTest(State.SavedBudget.GetValue());
			State.SavedBudget.Reset();
		}
		if (AStaticMeshActor* Actor = State.Actor.Get())
		{
			if (UWorld* World = Actor->GetWorld())
			{
				World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ false);
			}
		}
		if (State.bSettingSaved)
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bTransactionRepl = State.bSavedTransactionBuffer;
			Settings->bRunPump = State.bSavedRunPump;
			Settings->RunPresentation = State.SavedPresentation;
		}
	}

	FString MakeScript(const FString& Sfx, const FString& Label)
	{
		const TArray<FString> Lines = {
			TEXT("import unreal"),
			FString::Printf(TEXT("def move_%s(x, y, z):"), *Sfx),
			TEXT("    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():"),
			FString::Printf(TEXT("        if actor.get_actor_label() == '%s':"), *Label),
			TEXT("            actor.root_component.modify()"),
			TEXT("            actor.set_actor_location(unreal.Vector(x, y, z), False, False)"),
			TEXT("            return True"),
			FString::Printf(TEXT("    raise LookupError('%s')"), *Label),
			FString::Printf(TEXT("move_%s(100.0, 200.0, 300.0)"), *Sfx),
			FString::Printf(TEXT("move_%s(400.0, 500.0, 600.0)"), *Sfx),
		};
		return FString::Join(Lines, TEXT("\n"));
	}
}

BEGIN_DEFINE_SPEC(FDreamUndoRunSpec, "Dream.Transactions.UndoLastRun", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamUndoRunSpec)

void FDreamUndoRunSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("records one undo step for a two-move run, and Undo Last Run reverts the actor to where it was", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		State->Label = FString::Printf(TEXT("DreamT4Actor_%s"), *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, DreamUndoRunSpec::MakeScript(Sfx, State->Label), TEXT("Undo"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		const int32 LastLine = Buffer->Snapshot().LineCount();
		TSharedRef<DreamTest::FRunResult> Move = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("place the fixture actor"), [this, State]()
		{
			// One undo step per run is the buffer route's default (RT-08); pin it for the spec.
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedTransactionBuffer = Settings->bTransactionBuffer;
			State->bSettingSaved = true;
			Settings->bTransactionBuffer = true;
			if (const TSharedPtr<FDreamBuffer> Live = FDreamEditorModule::Get().GetBufferManager().FindById(BufferId))
			{
				Live->GetViewState().bOneUndoStepPerRun = true;   // the tab's toggle decides its runs (§5.9.1)
			}

			UWorld* World = GEditor->GetEditorWorldContext().World();
			FActorSpawnParameters Params;
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (!Actor)
			{
				AddError(TEXT("could not spawn the fixture actor"));
				return false;
			}
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Actor->SetActorLabel(State->Label);
			State->Actor = Actor;
			// A redo stack (an earlier spec's undo) is dropped by the next Begin, so it does not count (TransBuffer.h).
			State->QueueBefore = GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount();
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the two moves"), [Buffer, LastLine]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LastLine);
		}, Move, 60.0);

		Steps.Do(TEXT("check one undo step"), [this, State, Move]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Move, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const FDreamRunRecord& Record = Move->Record;
			AStaticMeshActor* Actor = State->Actor.Get();
			if (!TestNotNull(TEXT("the fixture actor"), Actor))
			{
				return false;
			}
			TestEqual(TEXT("both moves ran"), Actor->GetActorLocation(), FVector(400.0, 500.0, 600.0), 0.01f);
			TestTrue(*FString::Printf(TEXT("the run's transaction is Recorded (status %d)"), static_cast<int32>(Record.Txn.Status)),
				Record.Txn.Status == EDreamTxnStatus::Recorded);
			TestEqual(TEXT("transaction title (§5.9.1)"), Record.Txn.Title.ToString(),
				FString::Printf(TEXT("Dream: %s #%d"), *Record.TabTitle, Record.RunNumber));
			TestEqual(TEXT("exactly one undo step for the whole run (RUN-07)"), GEditor->Trans->GetQueueLength(), State->QueueBefore + 1);
			TestFalse(TEXT("no transaction is left open (§5.9.2)"), GEditor->IsTransactionActive());
			return true;
		});

		Steps.Do(TEXT("Undo Last Run"), [this, State, Move]()
		{
			FDreamRunService& RunService = FDreamEditorModule::Get().GetRunService();
			FText Reason;
			FDreamUndoPreview Preview;
			if (!TestTrue(TEXT("CanUndoRun(0) offers the run"), RunService.CanUndoRun(0, Reason, Preview)))
			{
				AddError(FString::Printf(TEXT("Undo Last Run refused: %s"), *Reason.ToString()));
				return false;
			}
			TestEqual(TEXT("Undo Last Run names this run's transaction"), Preview.Title.ToString(), Move->Record.Txn.Title.ToString());
			TestFalse(TEXT("the record is not partial"), Preview.bPartial);
			if (!TestTrue(TEXT("UndoRun(0) succeeds"), RunService.UndoRun(0, /*bConfirmedPartial*/ false, Reason)))
			{
				AddError(FString::Printf(TEXT("UndoRun failed: %s"), *Reason.ToString()));
				return false;
			}
			AStaticMeshActor* Actor = State->Actor.Get();
			if (TestNotNull(TEXT("the fixture actor after undo"), Actor))
			{
				TestEqual(TEXT("Undo Last Run reverts both moves at once"), Actor->GetActorLocation(), FVector::ZeroVector, 0.01f);
			}
			const FDreamRunRecord* Record = RunService.FindRecord(Move->RunNumber);
			TestTrue(TEXT("the history record says Undone"), Record && Record->Txn.Status == EDreamTxnStatus::Undone);
			FDreamUndoPreview Again;
			TestFalse(TEXT("an undone run cannot be undone again"), RunService.CanUndoRun(Move->RunNumber, Reason, Again));
			return true;
		});

		Steps.Finally(TEXT("remove the fixture actor"), [State]()
		{
			if (AStaticMeshActor* Actor = State->Actor.Get())
			{
				if (UWorld* World = Actor->GetWorld())
				{
					World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ false);
				}
			}
			if (State->bSettingSaved)
			{
				UDreamUserSettings::GetMutable()->bTransactionBuffer = State->bSavedTransactionBuffer;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("ends a run that records nothing Empty with no undo clause, and Undo Last Run passes over it (RUN-01, RUN-07)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		State->Label = FString::Printf(TEXT("DreamT4Actor_%s"), *Sfx);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, DreamUndoRunSpec::MakeScript(Sfx, State->Label), TEXT("UndoEmpty"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		Buffer->GetViewState().bOneUndoStepPerRun = true;
		const int32 LastLine = Buffer->Snapshot().LineCount();
		TSharedRef<DreamTest::FRunResult> Move = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> ReadOnly = MakeShared<DreamTest::FRunResult>();
		TSharedRef<int32> QueueAfterMove = MakeShared<int32>(0);
		TSharedRef<int32> EmptyClearsBefore = MakeShared<int32>(0);
		TSharedRef<int32> BreakerDropsBefore = MakeShared<int32>(0);

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("place the fixture actor"), [this, State, EmptyClearsBefore, BreakerDropsBefore]()
		{
			*EmptyClearsBefore = FDreamEditorModule::Get().GetSupervisor().GetEmptyAsyncExcClearsForTest();
			*BreakerDropsBefore = FDreamEditorModule::Get().GetSupervisor().GetAsyncExcBreakerDropsForTest();
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedTransactionBuffer = Settings->bTransactionRepl;
			State->bSettingSaved = true;
			Settings->bTransactionRepl = true;      // the REPL route's default (RT-08), pinned: the read-only run opens one
			UWorld* World = GEditor->GetEditorWorldContext().World();
			FActorSpawnParameters Params;
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (!Actor)
			{
				AddError(TEXT("could not spawn the fixture actor"));
				return false;
			}
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Actor->SetActorLabel(State->Label);
			State->Actor = Actor;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the recorded moves"), [Buffer, LastLine]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LastLine);
		}, Move, 60.0);
		Steps.Do(TEXT("the moves were recorded"), [this, Move, QueueAfterMove]()
		{
			*QueueAfterMove = GEditor->Trans->GetQueueLength();
			return DreamTest::ExpectOutcome(*this, *Move, EDreamRunOutcome::Ok)
				&& TestTrue(TEXT("the moving run is Recorded"), Move->Record.Txn.Status == EDreamTxnStatus::Recorded);
		});
		DreamTest::AddRun(Steps, TEXT("a run that only prints"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("readonly_%s = 1\nprint('read only', readonly_%s)"), *Sfx, *Sfx));
		}, ReadOnly);
		Steps.Do(TEXT("check the empty run"), [this, ReadOnly, QueueAfterMove, EmptyClearsBefore, BreakerDropsBefore]()
		{
			if (!DreamTest::ExpectOutcome(*this, *ReadOnly, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const FDreamTxnHandle& Txn = ReadOnly->Record.Txn;
			TestTrue(*FString::Printf(TEXT("a run that changed nothing ends Empty, not Purged (status %d, note \"%s\")"),
				static_cast<int32>(Txn.Status), *Txn.Note.ToString()), Txn.Status == EDreamTxnStatus::Empty);
			TestTrue(TEXT("no note says the undo history was reset"), Txn.Note.IsEmpty());
			TestEqual(TEXT("the empty run left the undo queue as it was (REPL-06)"), GEditor->Trans->GetQueueLength(), *QueueAfterMove);
			FDreamEditorModule& Module = FDreamEditorModule::Get();
			const FString Footer = DreamOutputRows::FooterText(Module.GetRunService().FindRecord(ReadOnly->RunNumber),
				static_cast<uint32>(ReadOnly->RunNumber), Module.GetOutputStore()).ToString();
			TestFalse(*FString::Printf(TEXT("the footer has no transaction clause: %s"), *Footer),
				Footer.Contains(TEXT("undo")) || Footer.Contains(TEXT("purged")) || Footer.Contains(TEXT("transaction")));
			// SUP-01: neither user_code_done nor EndRun clears an empty state (that call arms 3.11's eval breaker for good).
			TestEqual(TEXT("no SetAsyncExc(null) on an empty thread state during the two runs (SUP-01)"),
				Module.GetSupervisor().GetEmptyAsyncExcClearsForTest(), *EmptyClearsBefore);
			TestEqual(TEXT("and no placeholder delivery: nothing armed the breaker in two plain runs (SUP-01)"),
				Module.GetSupervisor().GetAsyncExcBreakerDropsForTest(), *BreakerDropsBefore);
			TestFalse(TEXT("sanity: no asynchronous exception is pending after the runs"), Module.GetSupervisor().HasPendingAsyncExcForTest());
			return true;
		});
		Steps.Do(TEXT("Undo Last Run passes over the empty run"), [this, State, Move]()
		{
			FDreamRunService& RunService = FDreamEditorModule::Get().GetRunService();
			FText Reason;
			FDreamUndoPreview Preview;
			if (!TestTrue(TEXT("CanUndoRun(0) offers the recorded run"), RunService.CanUndoRun(0, Reason, Preview)))
			{
				AddError(FString::Printf(TEXT("Undo Last Run refused: %s"), *Reason.ToString()));
				return false;
			}
			TestEqual(TEXT("Undo Last Run picks the recorded run, not the newer empty one"), Preview.RunNumber, Move->RunNumber);
			if (!TestTrue(TEXT("UndoRun(0) succeeds"), RunService.UndoRun(0, /*bConfirmedPartial*/ false, Reason)))
			{
				AddError(FString::Printf(TEXT("UndoRun failed: %s"), *Reason.ToString()));
				return false;
			}
			if (AStaticMeshActor* Actor = State->Actor.Get())
			{
				TestEqual(TEXT("the moves are undone"), Actor->GetActorLocation(), FVector::ZeroVector, 0.01f);
			}
			return true;
		});
		Steps.Finally(TEXT("remove the fixture actor"), [State]()
		{
			if (AStaticMeshActor* Actor = State->Actor.Get())
			{
				if (UWorld* World = Actor->GetWorld())
				{
					World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ false);
				}
			}
			if (State->bSettingSaved)
			{
				UDreamUserSettings::GetMutable()->bTransactionRepl = State->bSavedTransactionBuffer;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("reads Purged when the undo history is reset during the run, also from an empty history (TDD 5.9.3, P2-run-01)",   // (tests) ASCII test name
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans)
		{
			AddError(TEXT("no transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		const FDreamRunService* RunService = &FDreamEditorModule::Get().GetRunService();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("empty the undo history and arm the reset"), [this, State, Supervisor, RunService]()
		{
			DreamUndoRunSpec::PinSettings(*State);
			// L == 0 before Begin: the queue cannot shrink below it, so only the observed reset can say Purged.
			GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Transactions: empty history")));
			TestEqual(TEXT("the undo history is empty before the run"), GEditor->Trans->GetQueueLength(), 0);
			// What a map change or a package reload does mid-run, from a Slate tick inside the run's pump (GIL released).
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				const FDreamRunRecord* Active = RunService->GetActiveRecord();
				if (State->bResetDone || Supervisor->GetPumpDepth() <= 0 || !Active || Active->RunNumber != State->RunNumber
					|| !GEditor->IsTransactionActive())
				{
					return;
				}
				State->bResetDone = true;
				GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Transactions: reset during the run")));
			});
			return true;
		});
		Steps.Do(TEXT("submit a run that pumps"), [this, State, Run, Sfx]()
		{
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(
				TEXT("import time as _t_%s\nfor _ in range(60):\n    _t_%s.sleep(0.025)\ndel _t_%s"), *Sfx, *Sfx, *Sfx)));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumping run"), Run, 60.0);
		Steps.Do(TEXT("check the purged run"), [this, State, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestTrue(TEXT("a pump of the run reset the undo history"), State->bResetDone))
			{
				return false;
			}
			const FDreamTxnHandle& Txn = Run->Record.Txn;
			TestTrue(*FString::Printf(TEXT("a reset during the run reads Purged (status %d, note \"%s\")"), static_cast<int32>(Txn.Status),
				*Txn.Note.ToString()), Txn.Status == EDreamTxnStatus::Purged);
			TestFalse(*FString::Printf(TEXT("the note does not blame the script: %s"), *Txn.Note.ToString()),
				Txn.Note.ToString().Contains(TEXT("cancelled")));
			return true;
		});
		Steps.Finally(TEXT("disarm"), [State]()
		{
			DreamUndoRunSpec::RestoreSettingsAndActor(*State);
		});
		Steps.Run(Done);
	});

	LatentIt("reads Purged when the undo history is reset after the record stopped at 100%, also from an empty history (P3-run-03)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		State->Label = FString::Printf(TEXT("DreamT4Actor_%s"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		const FDreamRunService* RunService = &FDreamEditorModule::Get().GetRunService();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("empty the undo history, a 1-byte budget, and arm the reset after the 100% end"), [this, State, Supervisor, RunService]()
		{
			DreamUndoRunSpec::PinSettings(*State);
			if (!DreamUndoRunSpec::SpawnFixture(*this, *State))
			{
				return false;
			}
			// L == 0 before Begin: the queue never shrinks below it, so only the observed reset can say Purged.
			GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Transactions: empty history")));
			FDreamTransactions& Transactions = FDreamEditorModule::Get().GetTransactions();
			State->SavedBudget = Transactions.GetBudgetBytes();
			Transactions.SetBudgetBytesForTest(1);     // the first pump after the move ends the record at 100% (Partial)
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				const FDreamRunRecord* Active = RunService->GetActiveRecord();
				if (State->bResetDone || Supervisor->GetPumpDepth() <= 0 || !Active || Active->RunNumber != State->RunNumber
					|| Active->Txn.Status != EDreamTxnStatus::Partial || GEditor->IsTransactionActive())
				{
					return;
				}
				// The record ended at 100% and nothing is open: a reset now cancels nothing (EditorTransaction.cpp Reset).
				State->bResetDone = true;
				GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Transactions: reset after the 100% end")));
			});
			return true;
		});
		Steps.Do(TEXT("submit a run that moves the actor, then pumps"), [this, State, Run, Sfx]()
		{
			const TArray<FString> Lines = {
				FString::Printf(TEXT("import unreal, time as _t_%s"), *Sfx),
				TEXT("for _a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():"),
				FString::Printf(TEXT("    if _a.get_actor_label() == '%s':"), *State->Label),
				TEXT("        _a.root_component.modify()"),
				TEXT("        _a.set_actor_location(unreal.Vector(1.0, 2.0, 3.0), False, False)"),
				TEXT("for _ in range(80):"),
				FString::Printf(TEXT("    _t_%s.sleep(0.025)"), *Sfx),
				FString::Printf(TEXT("del _a, _t_%s"), *Sfx),
			};
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Join(Lines, TEXT("\n"))));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the moving, pumping run"), Run, 60.0);
		Steps.Do(TEXT("check the purged run"), [this, State, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestTrue(TEXT("a pump after the 100% end reset the undo history"), State->bResetDone))
			{
				return false;
			}
			const FDreamTxnHandle& Txn = Run->Record.Txn;
			TestTrue(*FString::Printf(TEXT("a reset after a 100%% end reads Purged, not Partial (status %d, note \"%s\")"),
				static_cast<int32>(Txn.Status), *Txn.Note.ToString()), Txn.Status == EDreamTxnStatus::Purged);
			TestFalse(*FString::Printf(TEXT("the note does not blame the script: %s"), *Txn.Note.ToString()),
				Txn.Note.ToString().Contains(TEXT("cancelled")));
			FText Reason;
			FDreamUndoPreview Preview;
			TestFalse(TEXT("Undo Last Run refuses the purged run"), FDreamEditorModule::Get().GetRunService().CanUndoRun(Run->RunNumber, Reason, Preview));
			TestFalse(*FString::Printf(TEXT("and does not say edits were made after it: %s"), *Reason.ToString()),
				Reason.ToString().Contains(TEXT("Edits were made after")));
			return true;
		});
		Steps.Finally(TEXT("restore the budget, disarm, remove the fixture actor"), [State]()
		{
			DreamUndoRunSpec::RestoreSettingsAndActor(*State);
		});
		Steps.Run(Done);
	});

	LatentIt("keeps Partial when an undo without redo empties the queue after the 100% end, which is no reset, nor is a transient pop after it (P4-run-01, P5-txo-01)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		State->Label = FString::Printf(TEXT("DreamT4Actor_%s"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<bool> bUndone = MakeShared<bool>(false);
		TSharedRef<int32> QueueAfterUndo = MakeShared<int32>(-1);
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		const FDreamRunService* RunService = &FDreamEditorModule::Get().GetRunService();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("empty the undo history, a 1-byte budget, and arm the undo without redo after the 100% end"),
			[this, State, Supervisor, RunService, bUndone, QueueAfterUndo]()
		{
			DreamUndoRunSpec::PinSettings(*State);
			if (!DreamUndoRunSpec::SpawnFixture(*this, *State))
			{
				return false;
			}
			// L == 0 before Begin: the run's record is the only entry, so undoing it empties the queue.
			GEditor->ResetTransaction(FText::FromString(TEXT("Dream.Transactions: empty history")));
			FDreamTransactions& Transactions = FDreamEditorModule::Get().GetTransactions();
			State->SavedBudget = Transactions.GetBudgetBytes();
			Transactions.SetBudgetBytesForTest(1);     // the first pump after the move ends the record at 100% (Partial)
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService, bUndone, QueueAfterUndo](float)
			{
				const FDreamRunRecord* Active = RunService->GetActiveRecord();
				if (State->bResetDone || Supervisor->GetPumpDepth() <= 0 || !Active || Active->RunNumber != State->RunNumber
					|| Active->Txn.Status != EDreamTxnStatus::Partial || GEditor->IsTransactionActive())
				{
					return;
				}
				// What MovieSceneToolHelpers' spawnable restore does after its Begin evicted every older entry: an undo that
				// removes the last entry, broadcasting the emptied buffer under GIsTransacting (EditorTransaction.cpp Undo).
				State->bResetDone = true;
				*bUndone = GEditor->UndoTransaction(/*bCanRedo*/ false);
				// Then an empty, transient record: End pops it on the emptied queue and broadcasts the empty, inactive
				// buffer outside GIsTransacting, which is no reset either (P5-txo-01).
				GEditor->BeginTransaction(TEXT("Dream"), FText::FromString(TEXT("Dream.Transactions: empty")), nullptr);
				GEditor->EndTransaction();
				*QueueAfterUndo = GEditor->Trans->GetQueueLength();
			});
			return true;
		});
		Steps.Do(TEXT("submit a run that moves the actor, then pumps"), [this, State, Run, Sfx]()
		{
			const TArray<FString> Lines = {
				FString::Printf(TEXT("import unreal, time as _t_%s"), *Sfx),
				TEXT("for _a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():"),
				FString::Printf(TEXT("    if _a.get_actor_label() == '%s':"), *State->Label),
				TEXT("        _a.root_component.modify()"),
				TEXT("        _a.set_actor_location(unreal.Vector(1.0, 2.0, 3.0), False, False)"),
				TEXT("for _ in range(80):"),
				FString::Printf(TEXT("    _t_%s.sleep(0.025)"), *Sfx),
				FString::Printf(TEXT("del _a, _t_%s"), *Sfx),
			};
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Join(Lines, TEXT("\n"))));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the moving, pumping run"), Run, 60.0);
		Steps.Do(TEXT("check the partial run"), [this, State, Run, bUndone, QueueAfterUndo]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestTrue(TEXT("a pump after the 100% end undid the record without redo"), State->bResetDone && *bUndone)
				|| !TestEqual(TEXT("the undo emptied the queue and the transient record was popped"), *QueueAfterUndo, 0))
			{
				return false;
			}
			const FDreamTxnHandle& Txn = Run->Record.Txn;
			TestTrue(*FString::Printf(TEXT("an undo after a 100%% end reads Partial, not Purged (status %d, note \"%s\")"),
				static_cast<int32>(Txn.Status), *Txn.Note.ToString()), Txn.Status == EDreamTxnStatus::Partial);
			TestFalse(*FString::Printf(TEXT("the note does not say undo is unavailable: %s"), *Txn.Note.ToString()),
				Txn.Note.ToString().Contains(TEXT("Undo is unavailable")));
			FText Reason;
			FDreamUndoPreview Preview;
			TestFalse(TEXT("Undo Last Run refuses the run, whose record is gone"),
				FDreamEditorModule::Get().GetRunService().CanUndoRun(Run->RunNumber, Reason, Preview));
			TestTrue(*FString::Printf(TEXT("with check 3 (A1-01): %s"), *Reason.ToString()), Reason.ToString().Contains(TEXT("Edits were made after")));
			return true;
		});
		Steps.Finally(TEXT("restore the budget, disarm, remove the fixture actor"), [State]()
		{
			DreamUndoRunSpec::RestoreSettingsAndActor(*State);
		});
		Steps.Run(Done);
	});

	LatentIt("reads CancelledByScript when the script cancels the run's transaction and records its own over a redo stack (A1-02)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		if (!GEditor || !GEditor->Trans || !GEditor->GetEditorWorldContext().World())
		{
			AddError(TEXT("no editor world or transaction buffer"));
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamUndoRunSpec::FState> State = MakeShared<DreamUndoRunSpec::FState>();
		State->Label = FString::Printf(TEXT("DreamT4Actor_%s"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("two recorded moves, both undone (a redo stack)"), [this, State]()
		{
			DreamUndoRunSpec::PinSettings(*State);
			AStaticMeshActor* Actor = DreamUndoRunSpec::SpawnFixture(*this, *State);
			if (!Actor)
			{
				return false;
			}
			for (int32 Step = 1; Step <= 2; ++Step)
			{
				GEditor->BeginTransaction(FText::FromString(FString::Printf(TEXT("Dream.Transactions: move %d"), Step)));
				Actor->GetRootComponent()->Modify();
				Actor->SetActorLocation(FVector(10.0 * Step, 0.0, 0.0));
				GEditor->EndTransaction();
			}
			GEditor->UndoTransaction();
			GEditor->UndoTransaction();
			return TestTrue(TEXT("a redo stack is in place"), GEditor->Trans->GetUndoCount() >= 2);
		});
		DreamTest::AddRun(Steps, TEXT("cancel, then the script's own transaction"), [State]()
		{
			const TArray<FString> Lines = {
				TEXT("import unreal"),
				TEXT("unreal.SystemLibrary.cancel_transaction(0)"),
				TEXT("for _a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():"),
				FString::Printf(TEXT("    if _a.get_actor_label() == '%s':"), *State->Label),
				TEXT("        with unreal.ScopedEditorTransaction('Dream.Transactions: the script own move'):"),
				TEXT("            _a.root_component.modify()"),
				TEXT("            _a.set_actor_location(unreal.Vector(7.0, 8.0, 9.0), False, False)"),
				TEXT("del _a"),
			};
			return DreamTest::MakeReplRequest(FString::Join(Lines, TEXT("\n")));
		}, Run, 60.0);
		Steps.Do(TEXT("check the cancelled run"), [this, State, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			if (AStaticMeshActor* Actor = State->Actor.Get())
			{
				TestEqual(TEXT("the script's own move ran"), Actor->GetActorLocation(), FVector(7.0, 8.0, 9.0), 0.01f);
			}
			const FDreamTxnHandle& Txn = Run->Record.Txn;
			TestTrue(*FString::Printf(TEXT("a script cancel reads CancelledByScript, not Purged (status %d, note \"%s\")"),
				static_cast<int32>(Txn.Status), *Txn.Note.ToString()), Txn.Status == EDreamTxnStatus::CancelledByScript);
			TestTrue(*FString::Printf(TEXT("the note names the script's cancel: %s"), *Txn.Note.ToString()),
				Txn.Note.ToString().Contains(TEXT("cancelled")));
			return true;
		});
		Steps.Finally(TEXT("remove the fixture actor"), [State]()
		{
			DreamUndoRunSpec::RestoreSettingsAndActor(*State);
		});
		Steps.Run(Done);
	});
}
