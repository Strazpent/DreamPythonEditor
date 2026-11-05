// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.FastChain (run group; TDD §5.7.3 fast chain, §5.7.8 "Stop with a bare except", T-CAN-003, D2): Stop on
//   for i in itertools.count():
//       try:
//           sum(range(1000))
//       except BaseException:
//           pass
// The game thread is nearly always inside sum()'s C code when a pending call arrives, so delivery 1 lands at the eval
// breaker check after the call, inside the try, and is swallowed. The fast-chain call the delivery re-queued runs at the
// next check, the loop's back-edge, outside the protected range: it must re-deliver there at once (not 250 ms after
// delivery 1), so the run ends cancelled within T-CAN-003's 1.25 s and the statement after the loop never runs (before
// the fix it ran on for minutes). Two Stop sources: a Slate tick inside a pump (as the Run window's Stop button) and the
// any-thread API entry from a worker while Python runs. A worker keeps the Interrupt cleanup flag set once 10 s have
// passed after Stop, so a regression drains the run instead of holding later specs (the spec still fails on the bound).
// A third case (review r1 run-M1) stops a loop inside a for over a generator with a finally: the unwind frees the
// generator and runs its finally with GeneratorExit handled; the chained call fires there and keeps the 250 ms rule, so
// there is exactly one delivery, the finally runs to its end and no "Exception ignored" line is written.
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

namespace DreamFastChainSpec
{
	constexpr double MaxStopSeconds = 1.25;         // T-CAN-003's bound for the swallowing shapes
	constexpr double DrainAfterSeconds = 10.0;      // after this, keep Interrupt cleanup set until the run ends
	constexpr double NoPumpSeconds = 8.0;           // no pump this long after entry: request Stop anyway

	enum class EStopFrom : uint8
	{
		PumpTick,       // FSlateApplication pre-tick inside our pump, game thread (the Run window's Stop button)
		WorkerApi,      // FRunSupervisor::RequestStop from a worker thread while Python runs (dream_editor API, Stop source Api)
	};

	struct FState
	{
		EStopFrom From = EStopFrom::PumpTick;
		int32 RunNumber = 0;
		FDelegateHandle PreTickHandle;
		std::atomic<double> StopAtSeconds{ 0.0 };
		std::atomic<bool> bStopIssued{ false };
		int32 PumpTicksSeen = 0;                        // GT: Slate pre-ticks inside our pumps
		std::atomic<bool> bApiArmed{ false };           // WorkerApi: the run has pumped a few times
		std::atomic<bool> bDrained{ false };
		std::atomic<bool> bDone{ false };
		TFuture<void> Worker;

		bool bSettingsSaved = false;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	/** Slate pre-tick (GT): inside a pump of our run, Stop once (PumpTick), or arm the worker's API Stop (WorkerApi). */
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
		if (State.From == EStopFrom::WorkerApi)
		{
			State.bApiArmed = ++State.PumpTicksSeen >= 3;      // the first pumps opened the Run window
			return;
		}
		State.StopAtSeconds = FPlatformTime::Seconds();
		Supervisor.RequestStop(Id, EDreamStopSource::RunWindowButton);
		State.bStopIssued = true;
	}

	/** Worker thread, any-thread APIs only (§3): the API Stop for EStopFrom::WorkerApi, and the drain for both. */
	void RunWorker(TSharedRef<FState> State, FRunSupervisor* Supervisor, FDreamRunService* RunService)
	{
		const double Begin = FPlatformTime::Seconds();
		FDreamRunId Id = 0;
		while (!State->bDone && (Id = RunService->GetActiveRunId()) == 0 && FPlatformTime::Seconds() - Begin < 60.0)
		{
			FPlatformProcess::Sleep(0.005f);
		}
		if (State->bDone || Id == 0)
		{
			return;
		}
		const double Entered = FPlatformTime::Seconds();
		if (State->From == EStopFrom::WorkerApi)
		{
			// Let the loop settle into the supervised run, then Stop between pumps, while the game thread runs Python.
			while (!State->bDone && !State->bApiArmed && FPlatformTime::Seconds() - Entered < NoPumpSeconds)
			{
				FPlatformProcess::Sleep(0.005f);
			}
			FPlatformProcess::Sleep(0.03f);
			if (State->bDone)
			{
				return;
			}
			State->StopAtSeconds = FPlatformTime::Seconds();
			Supervisor->RequestStop(Id, EDreamStopSource::Api);
			State->bStopIssued = true;
		}
		else
		{
			while (!State->bDone && !State->bStopIssued && FPlatformTime::Seconds() - Entered < NoPumpSeconds)
			{
				FPlatformProcess::Sleep(0.01f);
			}
			if (!State->bDone && !State->bStopIssued)
			{
				State->StopAtSeconds = FPlatformTime::Seconds();
				Supervisor->RequestStop(Id, EDreamStopSource::Api);     // no pump arrived: the spec fails on bStopIssued
			}
		}
		const double Stopped = FPlatformTime::Seconds();
		while (!State->bDone && FPlatformTime::Seconds() - Stopped < DrainAfterSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		const double DrainStart = FPlatformTime::Seconds();
		while (!State->bDone && FPlatformTime::Seconds() - DrainStart < 60.0)
		{
			State->bDrained = true;
			Supervisor->RequestInterruptCleanup(Id);     // each pending call then delivers; one lands outside the try
			FPlatformProcess::Sleep(0.02f);
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamFastChainSpec, "Dream.Supervisor.FastChain", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	void DefineCase(const TCHAR* Name, DreamFastChainSpec::EStopFrom From, bool bGeneratorCleanup = false);
END_DEFINE_SPEC(FDreamFastChainSpec)

void FDreamFastChainSpec::DefineCase(const TCHAR* Name, DreamFastChainSpec::EStopFrom From, bool bGeneratorCleanup)
{
	LatentIt(Name, DreamTest::LatentTimeout(), [this, From, bGeneratorCleanup](const FDoneDelegate& Done)
	{
		using namespace DreamFastChainSpec;
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		// The generator shape (review r1 run-M1): delivery 1 lands at the inner back-edge, outside any try, and propagates;
		// the unwind frees the suspended generator, whose finally runs with GeneratorExit handled and no __context__. The
		// chained call fires at len()'s check inside that finally and must not deliver there (RT-06).
		const FString Text = bGeneratorCleanup
			? FString::Printf(TEXT(
				"import itertools\n"
				"fc_%s = 'before'\n"
				"def fc_gen_%s():\n"
				"    try:\n"
				"        while True:\n"
				"            yield 1\n"
				"    finally:\n"
				"        len([])\n"
				"        print('fc-cleanup-%s', 'ran to its end')\n"
				"for _ in fc_gen_%s():\n"
				"    while True:\n"
				"        pass\n"
				"fc_%s = 'after'"), *Sfx, *Sfx, *Sfx, *Sfx, *Sfx)
			: FString::Printf(TEXT(
				"import itertools\n"
				"fc_%s = 'before'\n"
				"for i in itertools.count():\n"
				"    try:\n"
				"        sum(range(1000))\n"
				"    except BaseException:\n"
				"        pass\n"
				"fc_%s = 'after'"), *Sfx, *Sfx);
		const int32 LastLine = bGeneratorCleanup ? 13 : 8;
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Text, TEXT("FastChain"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();

		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		FDreamRunService* RunService = &Module.GetRunService();
		TSharedRef<FState> State = MakeShared<FState>();
		State->From = From;
		TSharedRef<DreamTest::FRunResult> Loop = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Probe = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("arm the Stop and submit the loop"), [this, Buffer, State, Supervisor, RunService, Loop, LastLine]()
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
				OnPreTick(*State, *Supervisor, *RunService);
			});
			Loop->SubmitSeconds = FPlatformTime::Seconds();
			Loop->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LastLine));
			State->RunNumber = Loop->RunNumber;
			if (Loop->RunNumber <= 0)
			{
				return false;
			}
			State->Worker = Async(EAsyncExecution::Thread, [State, Supervisor, RunService]()
			{
				RunWorker(State, Supervisor, RunService);
			});
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the loop"), Loop, 120.0);

		Steps.Do(TEXT("check the Stop"), [this, State, Loop, bGeneratorCleanup, Sfx]()
		{
			if (!TestTrue(TEXT("Stop was requested while the run was supervised"), State->bStopIssued.load()))
			{
				return false;
			}
			if (!DreamTest::ExpectOutcome(*this, *Loop, EDreamRunOutcome::Cancelled))
			{
				return false;
			}
			const FDreamRunRecord& Record = Loop->Record;
			const double EndSeconds = Record.EndSeconds > 0.0 ? Record.EndSeconds : Loop->FinishSeenSeconds;
			const double StopToEnd = EndSeconds - State->StopAtSeconds.load();
			AddInfo(FString::Printf(TEXT("Stop to finish %.3f s (%d deliveries, %d pumps, drained: %s)"), StopToEnd,
				Record.Stats.Deliveries, Record.Stats.Pumps, State->bDrained.load() ? TEXT("yes") : TEXT("no")));
			TestFalse(TEXT("the run ended without the Interrupt cleanup drain"), State->bDrained.load());
			TestTrue(*FString::Printf(TEXT("cancelled within %.2f s of Stop (took %.3f s)"), MaxStopSeconds, StopToEnd),
				StopToEnd >= 0.0 && StopToEnd <= MaxStopSeconds);
			if (bGeneratorCleanup)
			{
				// RT-06: one delivery; the generator's finally ran to its end; no delivery escaped it as "Exception ignored".
				TestEqual(TEXT("exactly one delivery: none inside the generator's finally"), Record.Stats.Deliveries, 1);
				TestTrue(*FString::Printf(TEXT("the generator's finally ran to its end: %s"), *Loop->Describe()),
					Loop->HasLineContaining(dream::LineType::Stdout, FString::Printf(TEXT("fc-cleanup-%s ran to its end"), *Sfx)));
				bool bIgnored = false;
				for (const DreamTest::FOutputLine& Line : Loop->Lines)
				{
					bIgnored |= Line.Text.Contains(TEXT("Exception ignored"));
				}
				TestFalse(*FString::Printf(TEXT("no 'Exception ignored' line: %s"), *Loop->Describe()), bIgnored);
				TestEqual(TEXT("statements started: the for loop was statement 4"), Record.StatementsStarted, 4);
				TestEqual(TEXT("statements returned: statements 1 to 3"), Record.StatementsReturned, 3);
				return true;
			}
			// Usually 2 (delivery 1 at the check after sum() inside the try, delivery 2 at the back-edge); 1 when the first
			// pending call happened to run at the back-edge itself, which is not an error.
			TestTrue(TEXT("the supervisor delivered ScriptCancelled"), Record.Stats.Deliveries >= 1);
			TestEqual(TEXT("statements started: the loop was statement 3"), Record.StatementsStarted, 3);
			TestEqual(TEXT("statements returned: statements 1 and 2"), Record.StatementsReturned, 2);
			TestTrue(*FString::Printf(TEXT("the outcome lands inside the loop (line %d)"), Record.Location.Line),
				Record.Location.Line >= 3 && Record.Location.Line <= 7);
			return true;
		});

		DreamTest::AddRun(Steps, TEXT("the probe"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print(fc_%s)"), *Sfx));
		}, Probe);
		Steps.Do(TEXT("check the statement after the loop never ran"), [this, Probe]()
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
			if (State->Worker.IsValid())
			{
				State->Worker.Wait();
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
}

void FDreamFastChainSpec::Define()
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

	DefineCase(TEXT("ends a swallowing loop with a call in its try within 1.25 s of a Stop from the Run window (T-CAN-003)"),
		DreamFastChainSpec::EStopFrom::PumpTick);
	DefineCase(TEXT("ends a swallowing loop with a call in its try within 1.25 s of an API Stop while Python runs (T-CAN-003)"),
		DreamFastChainSpec::EStopFrom::WorkerApi);
	DefineCase(TEXT("never re-delivers into a suspended generator's finally that the unwind runs (RT-06, review r1 run-M1)"),
		DreamFastChainSpec::EStopFrom::PumpTick, /*bGeneratorCleanup*/ true);
}
