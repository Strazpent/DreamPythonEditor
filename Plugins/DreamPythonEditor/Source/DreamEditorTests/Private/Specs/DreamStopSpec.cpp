// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.Stop: Stop on `while True: pass` (the first G2(1) shape of T-CAN-001; D2, LRS-02). The loop never
// yields, so only the supervisor can end it: the watchdog's pending call and GIL hand-off reach the game thread, the
// first pump opens the Run window and ticks Slate (§5.4.4, §5.5.3), and Stop is requested from that Slate tick on the
// game thread, as a click on the Run window's Stop button is (§5.6). The run must end `cancelled` within 1.25 s of the
// request at the loop's line, with statement 3 never run.
// Synchronisation is on the pump-depth probe (§14.2), never on a sleep. A worker thread is the safety net for a broken
// pump: if no pump arrives within 8 s it requests Stop through FRunSupervisor's any-thread entry, so the editor is not
// left in the loop (the spec still fails).
// A second case takes ClearAsyncExc's placeholder path (SUP-01, T-CAN-009): with the eval breaker armed and nothing
// pending, as user_code_done leaves it after it nulled a stale ScriptCancelled, the next run's EndRun delivers a
// placeholder to a trivial evaluation and clears it; nothing reaches the run after, and no empty state is cleared.
#include "Support/DreamTestUtil.h"

#include "Async/Async.h"
#include "Buffers/DreamBuffer.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"

#include <atomic>

namespace DreamStopSpec
{
	constexpr double MaxStopSeconds = 1.25;         // the brief's bound for this shape (T-CAN-003's p99 bound)
	constexpr double SafetyStopSeconds = 8.0;       // no pump this long after entry: the pump is broken

	struct FState
	{
		int32 RunNumber = 0;
		FDelegateHandle PreTickHandle;
		double StopAtSeconds = 0.0;
		int32 PumpDepthAtStop = 0;
		bool bRunWindowOpenAtStop = false;
		bool bStopRequestedSeen = false;
		std::atomic<bool> bStopIssued{ false };
		std::atomic<bool> bWorkerStop{ false };
		std::atomic<bool> bDone{ false };
		TFuture<void> Safety;

		bool bSettingsSaved = false;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	/** Slate pre-tick: inside a pump of our run (GT, GIL released), request Stop once, as the Run window's button would. */
	void OnPreTick(FState& State, FRunSupervisor& Supervisor, const FDreamRunService& RunService)
	{
		if (State.bStopIssued || Supervisor.GetPumpDepth() <= 0)
		{
			return;
		}
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		const FDreamRunId Id = Supervisor.GetCurrentRunId();
		if (!Active || Active->RunNumber != State.RunNumber || Id == 0)
		{
			return;
		}
		State.PumpDepthAtStop = Supervisor.GetPumpDepth();
		State.bRunWindowOpenAtStop = Supervisor.IsRunWindowOpen();
		State.StopAtSeconds = FPlatformTime::Seconds();
		Supervisor.RequestStop(Id, EDreamStopSource::RunWindowButton);
		State.bStopRequestedSeen = Supervisor.IsStopRequested(Id);
		State.bStopIssued = true;
	}

	/** Worker thread: the safety net described above, through the any-thread APIs only (§3 threading table). */
	void RunSafetyNet(TSharedRef<FState> State, FRunSupervisor* Supervisor, FDreamRunService* RunService)
	{
		const double Begin = FPlatformTime::Seconds();
		FDreamRunId Id = 0;
		while (!State->bDone && (Id = RunService->GetActiveRunId()) == 0 && FPlatformTime::Seconds() - Begin < 60.0)
		{
			FPlatformProcess::Sleep(0.005f);
		}
		const double Active = FPlatformTime::Seconds();
		while (!State->bDone && !State->bStopIssued && FPlatformTime::Seconds() - Active < SafetyStopSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (State->bDone || State->bStopIssued || Id == 0)
		{
			return;
		}
		State->bWorkerStop = true;
		Supervisor->RequestStop(Id, EDreamStopSource::Api);
		const double Stopped = FPlatformTime::Seconds();
		while (!State->bDone && FPlatformTime::Seconds() - Stopped < 15.0)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (!State->bDone)
		{
			Supervisor->RequestInterruptCleanup(Id);
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamStopSpec, "Dream.Supervisor.Stop", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamStopSpec)

void FDreamStopSpec::Define()
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

	LatentIt("ends `while True: pass` cancelled within 1.25 s of Stop at the first pump", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("stop_%s = 'before'\nwhile True: pass\nstop_%s = 'after'"), *Sfx, *Sfx), TEXT("Stop"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();

		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		FDreamRunService* RunService = &Module.GetRunService();
		TSharedRef<DreamStopSpec::FState> State = MakeShared<DreamStopSpec::FState>();
		TSharedRef<DreamTest::FRunResult> Loop = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("arm the Stop probe and submit the loop"), [this, Buffer, State, Supervisor, RunService, Loop]()
		{
			// The run.pump kill switch off would leave the loop unstoppable (§5.5.5): force the supervised presentation.
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->SavedPresentation = Settings->RunPresentation;
			State->bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;

			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				DreamStopSpec::OnPreTick(*State, *Supervisor, *RunService);
			});

			Loop->SubmitSeconds = FPlatformTime::Seconds();
			Loop->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3));
			State->RunNumber = Loop->RunNumber;
			if (Loop->RunNumber <= 0)
			{
				return false;
			}
			State->Safety = Async(EAsyncExecution::Thread, [State, Supervisor, RunService]()
			{
				DreamStopSpec::RunSafetyNet(State, Supervisor, RunService);
			});
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the loop"), Loop, 90.0);

		Steps.Do(TEXT("check the Stop"), [this, State, Loop]()
		{
			TestFalse(TEXT("a pump arrived before the safety net fired (watchdog, pending call, GIL hand-off: RV-01)"), State->bWorkerStop.load());
			if (!TestTrue(TEXT("Stop was requested from a Slate tick inside the pump"), State->bStopIssued.load() && !State->bWorkerStop.load()))
			{
				DreamTest::ExpectOutcome(*this, *Loop, EDreamRunOutcome::Cancelled);
				return false;
			}
			TestTrue(TEXT("FRunSupervisor::IsStopRequested right after RequestStop"), State->bStopRequestedSeen);
			TestTrue(TEXT("the Run window is up at the first pump (RT-04, §5.5.3)"), State->bRunWindowOpenAtStop);
			if (!DreamTest::ExpectOutcome(*this, *Loop, EDreamRunOutcome::Cancelled))
			{
				return false;
			}

			const FDreamRunRecord& Record = Loop->Record;
			const double EndSeconds = Record.EndSeconds > 0.0 ? Record.EndSeconds : Loop->FinishSeenSeconds;
			const double StopToEnd = EndSeconds - State->StopAtSeconds;
			AddInfo(FString::Printf(TEXT("Stop to finish %.3f s (pump depth %d at the request, %d deliveries, %d pumps)"),
				StopToEnd, State->PumpDepthAtStop, Record.Stats.Deliveries, Record.Stats.Pumps));
			TestTrue(*FString::Printf(TEXT("cancelled within %.2f s of Stop (took %.3f s)"), DreamStopSpec::MaxStopSeconds, StopToEnd),
				StopToEnd >= 0.0 && StopToEnd <= DreamStopSpec::MaxStopSeconds);
			TestEqual(TEXT("the outcome lands at the loop's line"), Record.Location.Line, 2);
			TestEqual(TEXT("statements started: the loop was statement 2"), Record.StatementsStarted, 2);
			TestEqual(TEXT("statements returned: only statement 1"), Record.StatementsReturned, 1);
			TestTrue(TEXT("the supervisor delivered ScriptCancelled at least once"), Record.Stats.Deliveries >= 1);
			TestTrue(TEXT("the run pumped"), Record.Stats.Pumps >= 1);
			return true;
		});

		DreamTest::AddRun(Steps, TEXT("the probe"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print(stop_%s)"), *Sfx));
		}, Probe);
		Steps.Do(TEXT("check statement 3 never ran"), [this, Probe]()
		{
			if (DreamTest::ExpectOutcome(*this, *Probe, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the line after the loop never ran: %s"), *Probe->Describe()),
					Probe->HasLineContaining(dream::LineType::Stdout, TEXT("before")));
			}
			return true;
		});

		Steps.Finally(TEXT("disarm"), [State]()
		{
			State->bDone = true;
			if (State->PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTickHandle);
			}
			if (State->Safety.IsValid())
			{
				State->Safety.Wait();
			}
			if (State->bSettingsSaved)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = State->bSavedRunPump;
				Settings->RunPresentation = State->SavedPresentation;
			}
		});
		Steps.Run(Done);
	});

	LatentIt("drops an armed eval breaker at the next run's end and leaks nothing into later runs (SUP-01)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<int32> EmptyClears = MakeShared<int32>(0);
		TSharedRef<int32> BreakerDrops = MakeShared<int32>(0);
		TSharedRef<DreamTest::FRunResult> Boot = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Carrier = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> After = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		// The first run of a session boots dream_editor: arm only once Python is up.
		DreamTest::AddRun(Steps, TEXT("a first run"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("breaker_boot_%s = 1"), *Sfx));
		}, Boot);
		Steps.Do(TEXT("arm the breaker with nothing pending"), [this, EmptyClears, BreakerDrops]()
		{
			FRunSupervisor& Supervisor = FDreamEditorModule::Get().GetSupervisor();
			if (!TestTrue(TEXT("the breaker is armed (no run, nothing pending)"), Supervisor.ArmAsyncExcBreakerForTest()))
			{
				return false;
			}
			*EmptyClears = Supervisor.GetEmptyAsyncExcClearsForTest();
			*BreakerDrops = Supervisor.GetAsyncExcBreakerDropsForTest();
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the run whose end clears the breaker"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("breaker_carrier_%s = 2"), *Sfx));
		}, Carrier);
		Steps.Do(TEXT("check the carrier run"), [this, Carrier, EmptyClears, BreakerDrops]()
		{
			DreamTest::ExpectOutcome(*this, *Carrier, EDreamRunOutcome::Ok);
			FRunSupervisor& Supervisor = FDreamEditorModule::Get().GetSupervisor();
			TestFalse(TEXT("no asynchronous exception is pending after its EndRun"), Supervisor.HasPendingAsyncExcForTest());
			// The placeholder was delivered to the trivial evaluation, whose unsignal dropped the breaker (P3-run-01).
			TestEqual(TEXT("EndRun delivered the placeholder once, dropping the armed breaker (SUP-01)"),
				Supervisor.GetAsyncExcBreakerDropsForTest(), *BreakerDrops + 1);
			TestEqual(TEXT("neither user_code_done nor EndRun clears an empty state (SUP-01)"), Supervisor.GetEmptyAsyncExcClearsForTest(), *EmptyClears);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("a later run"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('after_breaker_%s')"), *Sfx));
		}, After);
		Steps.Do(TEXT("the placeholder never reached a later run"), [this, After, Sfx]()
		{
			if (DreamTest::ExpectOutcome(*this, *After, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the later run printed: %s"), *After->Describe()),
					After->HasLineContaining(dream::LineType::Stdout, TEXT("after_breaker_") + Sfx));
			}
			return true;
		});
		Steps.Run(Done);
	});
}
