// SPDX-License-Identifier: Apache-2.0
// Dream.G2.Cancel: the G2(1) cancellation shapes in the Run window presentation (TDD §5.15.1, D2, LRS-02, PERF-14).
//   T-CAN-001: `while True: pass`; a list comprehension over itertools.count(); sum() of a generator over it; recursion to
//     depth 900, repeated; a busy loop in __init__; loops calling unreal.SystemLibrary.get_engine_version() and the
//     EditorActorSubsystem's get_all_level_actors(); time.sleep(60). Each shape is stopped `Stops` times (200 unless
//     -DreamG2Stops=N) by the input bot after the first pump: an OS click on the Run window's Stop button and Shift+F5,
//     alternating, through SendInput. The time runs from the SendInput timestamp to the ScriptCancelled reaching user
//     code (a handler around the shape marks it and re-raises: an upper bound of the constructor time, §5.13); the
//     injection-to-request interval (the click or chord reaching RequestStop, Stats.StopRequestedAt) is reported apart.
//     Pass: p99 <= 250 ms per shape.
//   T-CAN-003: the swallowing shapes (`except: pass`, `except KeyboardInterrupt: pass`, `except BaseException: pass`
//     inside a loop). Pass: p99 <= 1.25 s.
//   T-CAN-004 (RT-06, LRS-12): Stop once a probe in the try body fired, with a 3 s finally and a 3 s __exit__: the
//     cleanup completes, its end marker is set, no ScriptCancelled is raised inside it, outcome cancelled.
// Every stopped run is followed by `x = 1`, which must end ok (the T-SUP-001 cycle; Dream.G2.Supervisor adds the rest).
// Synchronisation is on run-state probes and marks only (§14.2). Run logs are off for the cycles and restored after.
// (B-note) EditorActorSubsystem is fetched once with get_editor_subsystem: constructing one per iteration, as the TDD text
// reads, would create a transient UObject per call.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Fixtures/DreamTestLatch.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamTestPython.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"

namespace DreamG2CancelSpec
{
	struct FShape
	{
		FString Id;         // "while_true"
		FString TestId;     // "T-CAN-001"
		FString Prelude;    // definitions before the try (module level)
		FString Body;       // the shape, inside the try
		double BoundSeconds = 0.25;
	};

	FString Indent(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		FString Out;
		for (const FString& Line : Lines)
		{
			Out += TEXT("    ") + Line + TEXT("\n");
		}
		return Out;
	}

	/** The shape inside `try: ... except BaseException: mark; raise`, after the imports and the prelude. */
	FString ShapeSource(const FShape& Shape, const FString& Mark)
	{
		FString Source = TEXT("import _dream_test, itertools, time, unreal\n");
		if (!Shape.Prelude.IsEmpty())
		{
			Source += Shape.Prelude + TEXT("\n");
		}
		Source += TEXT("try:\n") + Indent(Shape.Body);
		Source += FString::Printf(TEXT("except BaseException:\n    _dream_test.mark('%s')\n    raise"), *Mark);
		return Source;
	}

	TArray<FShape> CancelShapes()
	{
		TArray<FShape> Shapes;
		auto Add = [&Shapes](const TCHAR* Id, const TCHAR* TestId, const TCHAR* Prelude, const TCHAR* Body, double Bound)
		{
			Shapes.Add(FShape{ Id, TestId, Prelude, Body, Bound });
		};
		Add(TEXT("while_true"), TEXT("T-CAN-001"), TEXT(""), TEXT("while True: pass"), 0.25);
		Add(TEXT("listcomp_count"), TEXT("T-CAN-001"), TEXT(""), TEXT("[x for x in itertools.count() if x < 0]"), 0.25);
		Add(TEXT("sum_genexpr"), TEXT("T-CAN-001"), TEXT(""), TEXT("sum(1 for _ in itertools.count())"), 0.25);
		Add(TEXT("recursion_900"), TEXT("T-CAN-001"), TEXT("def g2_recurse(depth):\n    return g2_recurse(depth - 1) if depth else 0"),
			TEXT("while True: g2_recurse(900)"), 0.25);
		Add(TEXT("init_busy"), TEXT("T-CAN-001"), TEXT("class G2Busy:\n    def __init__(self):\n        while True:\n            pass"),
			TEXT("G2Busy()"), 0.25);
		Add(TEXT("engine_version"), TEXT("T-CAN-001"), TEXT(""), TEXT("while True: unreal.SystemLibrary.get_engine_version()"), 0.25);
		Add(TEXT("level_actors"), TEXT("T-CAN-001"), TEXT("g2_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)"),
			TEXT("while True: g2_actors.get_all_level_actors()"), 0.25);
		Add(TEXT("sleep_60"), TEXT("T-CAN-001"), TEXT(""), TEXT("time.sleep(60)"), 0.25);
		Add(TEXT("swallow_bare"), TEXT("T-CAN-003"), TEXT("g2_n = 0"),
			TEXT("while True:\n    try:\n        g2_n += 1\n    except:\n        pass"), 1.25);
		Add(TEXT("swallow_keyboardinterrupt"), TEXT("T-CAN-003"), TEXT(""),
			TEXT("for g2_i in itertools.count():\n    try:\n        sum(range(1000))\n    except KeyboardInterrupt:\n        pass"), 1.25);
		Add(TEXT("swallow_baseexception"), TEXT("T-CAN-003"), TEXT(""),
			TEXT("for g2_i in itertools.count():\n    try:\n        sum(range(1000))\n    except BaseException:\n        pass"), 1.25);
		return Shapes;
	}

	int32 LineCount(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		return Lines.Num();
	}

	/** The run/stop cycle machine one shape runs: run, bot Stop, finish, `x = 1`, finish; Count times. */
	struct FCycles
	{
		enum class EPhase : uint8 { Start, WaitRun, WaitBot, FollowUp, WaitFollowUp, Done };

		FShape Shape;
		FString Mark;
		int32 Count = 0;
		int32 Cycle = 0;
		EPhase Phase = EPhase::Start;
		double PhaseStart = 0.0;
		int32 RunNumber = 0;
		int32 FollowNumber = 0;
		TSharedPtr<FDreamBuffer> Buffer;
		TSharedPtr<FDreamBuffer> Follow;
		TSharedPtr<DreamTest::FDreamInputBotTicket> Ticket;
		TArray<double> Latency;
		TArray<double> InjectToRequest;
		int32 Failures = 0;
		int32 MarkMisses = 0;
		int32 Clicks = 0;
		int32 Chords = 0;
		int32 Drained = 0;      // stops the run swallowed for 10 s, ended by the bot's Interrupt cleanup drain
	};

	bool CycleFailed(FAutomationTestBase& Test, FCycles& C, const FString& Why)
	{
		Test.AddError(FString::Printf(TEXT("%s %s cycle %d: %s"), *C.Shape.TestId, *C.Shape.Id, C.Cycle + 1, *Why));
		return ++C.Failures < 3;
	}

	DreamTest::EStep Advance(FAutomationTestBase& Test, FCycles& C)
	{
		using DreamTest::EStep;
		const double Now = FPlatformTime::Seconds();
		switch (C.Phase)
		{
		case FCycles::EPhase::Start:
		{
			if (!DreamTest::IsIdle())
			{
				return EStep::Wait;
			}
			DreamTest::FDreamTestLatch::ClearMark(C.Mark);
			const DreamTest::EBotStop Method = (C.Cycle % 2 == 0) ? DreamTest::EBotStop::Click : DreamTest::EBotStop::ShiftF5;
			(Method == DreamTest::EBotStop::Click ? C.Clicks : C.Chords)++;
			C.RunNumber = DreamTest::Submit(Test, DreamTest::MakeBufferRequest(*C.Buffer, EDreamRunRoute::Buffer, 1, LineCount(C.Buffer->GetTextAsString())));
			if (C.RunNumber <= 0)
			{
				return EStep::Fail;
			}
			DreamTest::FBotStopOptions Options;
			Options.RunNumber = C.RunNumber;
			Options.Method = Method;
			C.Ticket = DreamTest::SubmitBotStop(Options);
			C.Phase = FCycles::EPhase::WaitRun;
			C.PhaseStart = Now;
			return EStep::Wait;
		}
		case FCycles::EPhase::WaitRun:
			if (!DreamTest::IsRunFinished(C.RunNumber))
			{
				// sleep_60 ends by itself after 60 s even when the bot failed; the others need the bot's safety net, and a
				// shape that swallows every delivery is drained by the bot after 10 s (FBotStopOptions::DrainAfterSeconds).
				if (Now - C.PhaseStart > 360.0)
				{
					Test.AddError(FString::Printf(TEXT("%s %s cycle %d: run #%d did not finish"), *C.Shape.TestId, *C.Shape.Id, C.Cycle + 1, C.RunNumber));
					return EStep::Fail;
				}
				return EStep::Wait;
			}
			C.Phase = FCycles::EPhase::WaitBot;
			C.PhaseStart = Now;
			return EStep::Wait;
		case FCycles::EPhase::WaitBot:
		{
			if (!DreamTest::IsTicketDone(C.Ticket.ToSharedRef()))
			{
				if (Now - C.PhaseStart > 30.0)
				{
					Test.AddError(TEXT("the input bot did not finish its Stop job"));
					return EStep::Fail;
				}
				return EStep::Wait;
			}
			const bool bBotOk = DreamTest::ReportTicket(Test, C.Ticket.ToSharedRef(), FString::Printf(TEXT("%s %s cycle %d"), *C.Shape.TestId, *C.Shape.Id, C.Cycle + 1));
			const FDreamRunRecord* Record = FDreamEditorModule::Get().GetRunService().FindRecord(C.RunNumber);
			const double* Injected = C.Ticket->Times.Find(TEXT("Injected"));
			bool bContinue = true;
			if (!bBotOk)
			{
				bContinue = ++C.Failures < 3;
			}
			else if (Record == nullptr || Record->Outcome != EDreamRunOutcome::Cancelled)
			{
				bContinue = CycleFailed(Test, C, FString::Printf(TEXT("run #%d ended %s (%s), not cancelled"), C.RunNumber,
					Record ? *DreamTest::OutcomeText(Record->Outcome) : TEXT("(no record)"), Record ? *Record->Detail : TEXT("")));
			}
			else if (Injected == nullptr || *Injected <= 0.0)
			{
				bContinue = CycleFailed(Test, C, TEXT("the bot reported no injection time"));
			}
			else
			{
				if (C.Ticket->Times.Contains(TEXT("Drained")))
				{
					++C.Drained;
				}
				const double MarkAt = DreamTest::FDreamTestLatch::GetMark(C.Mark);
				double End = MarkAt;
				if (End <= 0.0)
				{
					// The handler did not run (Stop landed between statements): the user_code_done time is the bound.
					++C.MarkMisses;
					End = Record->Stats.StopEffectiveAt;
				}
				C.Latency.Add(FMath::Max(0.0, End - *Injected));
				if (Record->Stats.StopRequestedAt > 0.0)
				{
					C.InjectToRequest.Add(FMath::Max(0.0, Record->Stats.StopRequestedAt - *Injected));
				}
			}
			C.Ticket.Reset();
			if (!bContinue)
			{
				return EStep::Fail;
			}
			C.Phase = FCycles::EPhase::FollowUp;
			return EStep::Wait;
		}
		case FCycles::EPhase::FollowUp:
			if (!DreamTest::IsIdle())
			{
				return EStep::Wait;
			}
			C.FollowNumber = DreamTest::Submit(Test, DreamTest::MakeBufferRequest(*C.Follow, EDreamRunRoute::Buffer, 1, 1));
			if (C.FollowNumber <= 0)
			{
				return EStep::Fail;
			}
			C.Phase = FCycles::EPhase::WaitFollowUp;
			C.PhaseStart = Now;
			return EStep::Wait;
		case FCycles::EPhase::WaitFollowUp:
		{
			if (!DreamTest::IsRunFinished(C.FollowNumber))
			{
				if (Now - C.PhaseStart > 20.0)
				{
					Test.AddError(FString::Printf(TEXT("the follow-up run #%d did not finish"), C.FollowNumber));
					return EStep::Fail;
				}
				return EStep::Wait;
			}
			const FDreamRunRecord* Record = FDreamEditorModule::Get().GetRunService().FindRecord(C.FollowNumber);
			if (Record == nullptr || Record->Outcome != EDreamRunOutcome::Ok)
			{
				if (!CycleFailed(Test, C, FString::Printf(TEXT("the next run `x = 1` (#%d) ended %s (%s), not ok (T-SUP-001)"), C.FollowNumber,
					Record ? *DreamTest::OutcomeText(Record->Outcome) : TEXT("(no record)"), Record ? *Record->Detail : TEXT(""))))
				{
					return EStep::Fail;
				}
			}
			if (++C.Cycle >= C.Count)
			{
				C.Phase = FCycles::EPhase::Done;
				return EStep::Next;
			}
			C.Phase = FCycles::EPhase::Start;
			return EStep::Wait;
		}
		case FCycles::EPhase::Done:
			return EStep::Next;
		}
		return EStep::Fail;
	}
}

BEGIN_DEFINE_SPEC(FDreamG2CancelSpec, "Dream.G2.Cancel", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Sfx;
	bool bSettingsSaved = false;
	bool bSavedRunPump = true;
	bool bSavedRunLogs = true;
	EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	bool Prepare();
	void DefineShape(const DreamG2CancelSpec::FShape& Shape);
	void DefineCleanupCase(bool bExit);
END_DEFINE_SPEC(FDreamG2CancelSpec)

bool FDreamG2CancelSpec::Prepare()
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
	{
		return false;
	}
	if (!TestTrue(TEXT("the _dream_test fixture module exists (DreamEditorTests registers it at OnPythonInitialized)"), DreamTestPython::IsModuleCreated()))
	{
		return false;
	}
	// The kill switch off would leave the loops unstoppable (§5.5.5): force the supervised Run window, restore after.
	UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
	bSavedRunPump = Settings->bRunPump;
	bSavedRunLogs = Settings->bAlwaysWriteRunLogs;
	SavedPresentation = Settings->RunPresentation;
	bSettingsSaved = true;
	Settings->bRunPump = true;
	Settings->bAlwaysWriteRunLogs = false;
	Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
	return DreamTest::RequireEditorInFront(*this);
}

void FDreamG2CancelSpec::DefineShape(const DreamG2CancelSpec::FShape& Shape)
{
	const int32 Stops = FMath::Max(1, DreamTest::CommandLineInt(TEXT("DreamG2Stops"), 200));
	const double Budget = 60.0 + Stops * (Shape.BoundSeconds > 1.0 ? 6.0 : 4.0);
	LatentIt(FString::Printf(TEXT("%s %s: p99 <= %.0f ms from the SendInput Stop to ScriptCancelled over %d stops"),
		*Shape.TestId, *Shape.Id, Shape.BoundSeconds * 1000.0, Stops), FTimespan::FromSeconds(Budget + 30.0),
		[this, Shape, Stops, Budget](const FDoneDelegate& Done)
	{
		if (!Prepare())
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamG2CancelSpec::FCycles> C = MakeShared<DreamG2CancelSpec::FCycles>();
		C->Shape = Shape;
		C->Count = Stops;
		C->Mark = FString::Printf(TEXT("g2c_%s_%s"), *Shape.Id, *Sfx);
		C->Buffer = DreamTest::OpenScratch(*this, *Tab, DreamG2CancelSpec::ShapeSource(Shape, C->Mark), TEXT("G2-") + Shape.Id);
		C->Follow = C->Buffer.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("x = 1"), TEXT("G2-next")) : nullptr;
		if (!C->Follow.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(C->Buffer->GetId());
		BufferIds.Add(C->Follow->GetId());
		Tab->ActivateBuffer(C->Buffer->GetId());

		// T-SUP-001's foreign Python tick callback: registered between runs, it must never see a KeyboardInterrupt.
		const FString Foreign = FString::Printf(TEXT("g2_foreign_%s"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Register = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Unregister = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(Budget);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("register the foreign tick callback"), [Foreign]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT(
				"import unreal\n"
				"%s = {'calls': 0, 'interrupts': 0}\n"
				"def %s_tick(delta, _state=%s):\n"
				"    _state['calls'] += 1\n"
				"    try:\n"
				"        sum(range(50))\n"
				"    except KeyboardInterrupt:\n"
				"        _state['interrupts'] += 1\n"
				"%s_handle = unreal.register_slate_post_tick_callback(%s_tick)"), *Foreign, *Foreign, *Foreign, *Foreign, *Foreign));
		}, Register);
		Steps.Do(TEXT("the callback is registered"), [this, Register]()
		{
			return DreamTest::ExpectOutcome(*this, *Register, EDreamRunOutcome::Ok);
		});
		Steps.Poll(TEXT("the run/stop cycles"), [this, C]() { return DreamG2CancelSpec::Advance(*this, *C); }, Budget);
		DreamTest::AddRun(Steps, TEXT("read and unregister the foreign callback"), [Foreign]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT(
				"unreal.unregister_slate_post_tick_callback(%s_handle)\n"
				"print('foreign', %s['calls'], %s['interrupts'])"), *Foreign, *Foreign, *Foreign));
		}, Unregister);
		Steps.Do(TEXT("the foreign callback ran between runs and never saw ScriptCancelled (T-SUP-001)"), [this, Unregister]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Unregister, EDreamRunOutcome::Ok))
			{
				return true;
			}
			const TArray<DreamTest::FOutputLine> Out = Unregister->OfType(dream::LineType::Stdout);
			TArray<FString> Parts;
			if (Out.Num() > 0)
			{
				Out.Last().Text.ParseIntoArrayWS(Parts);
			}
			if (TestEqual(TEXT("the probe printed its counters"), Parts.Num(), 3))
			{
				TestTrue(TEXT("the foreign Python tick callback ran between the runs"), FCString::Atoi(*Parts[1]) > 0);
				TestEqual(TEXT("it never saw a KeyboardInterrupt (ScriptCancelled) (T-SUP-001, G2(2))"), FCString::Atoi(*Parts[2]), 0);
			}
			return true;
		});
		Steps.Do(TEXT("check the stop times"), [this, C]()
		{
			const double P99 = DreamTest::Percentile(C->Latency, 99.0);
			AddInfo(FString::Printf(TEXT("%s %s: SendInput to ScriptCancelled %s (%d clicks, %d Shift+F5; %d stops measured at user_code_done)"),
				*C->Shape.TestId, *C->Shape.Id, *DreamTest::DescribeMs(C->Latency), C->Clicks, C->Chords, C->MarkMisses));
			TestEqual(*FString::Printf(TEXT("%s %s: no run outlived its Stop by 10 s (the bot drained %d with Interrupt cleanup, §5.7.4)"),
				*C->Shape.TestId, *C->Shape.Id, C->Drained), C->Drained, 0);
			AddInfo(FString::Printf(TEXT("%s %s: SendInput to RequestStop (the injection-to-pre-processor interval) %s"),
				*C->Shape.TestId, *C->Shape.Id, *DreamTest::DescribeMs(C->InjectToRequest)));
			TestEqual(TEXT("every stop was measured"), C->Latency.Num(), C->Count);
			TestTrue(*FString::Printf(TEXT("p99 %.1f ms <= %.0f ms (%s, D2, PERF-14)"), P99 * 1000.0, C->Shape.BoundSeconds * 1000.0, *C->Shape.TestId),
				C->Latency.Num() > 0 && P99 <= C->Shape.BoundSeconds);
			return true;
		});
		Steps.Finally(TEXT("forget the marks; never leave the foreign callback registered"), [this, C, Foreign, Register, Unregister]()
		{
			DreamTest::FDreamTestLatch::ClearMark(C->Mark);
			if (Register->bFinished && !Unregister->bFinished && DreamTest::IsIdle())
			{
				// A failed cycle skipped the unregistration: queue it (the next spec waits for idle first).
				DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(TEXT(
					"import unreal\nunreal.unregister_slate_post_tick_callback(%s_handle)"), *Foreign)));
			}
		});
		Steps.Run(Done);
	});
}

void FDreamG2CancelSpec::DefineCleanupCase(bool bExit)
{
	LatentIt(bExit ? TEXT("T-CAN-004: completes a 3 s __exit__ after Stop and raises nothing inside it (RT-06)")
		: TEXT("T-CAN-004: completes a 3 s finally after Stop and raises nothing inside it (RT-06)"),
		DreamTest::LatentTimeout(), [this, bExit](const FDoneDelegate& Done)
	{
		if (!Prepare())
		{
			Done.Execute();
			return;
		}
		const FString Probe = FString::Printf(TEXT("g2c_try_%s_%d"), *Sfx, bExit ? 1 : 0);
		const FString Finished = FString::Printf(TEXT("g2c_cleanup_%s_%d"), *Sfx, bExit ? 1 : 0);
		const FString Flag = FString::Printf(TEXT("g2c_raised_%s_%d"), *Sfx, bExit ? 1 : 0);
		FString Source = FString::Printf(TEXT("import _dream_test, time\n%s = False\n"), *Flag);
		const FString Cleanup = FString::Printf(TEXT(
			"try:\n"
			"    g2_end = time.perf_counter() + 3.0\n"
			"    while time.perf_counter() < g2_end:\n"
			"        pass\n"
			"except BaseException:\n"
			"    globals()['%s'] = True\n"
			"    raise\n"
			"_dream_test.mark('%s')\n"), *Flag, *Finished);
		if (bExit)
		{
			Source += TEXT("class G2Cleanup:\n    def __enter__(self):\n        return self\n    def __exit__(self, *exc):\n")
				+ DreamG2CancelSpec::Indent(DreamG2CancelSpec::Indent(Cleanup)) + TEXT("        return False\n");
			Source += FString::Printf(TEXT("with G2Cleanup():\n    _dream_test.mark('%s')\n    while True:\n        pass"), *Probe);
		}
		else
		{
			Source += FString::Printf(TEXT("try:\n    _dream_test.mark('%s')\n    while True:\n        pass\nfinally:\n"), *Probe)
				+ DreamG2CancelSpec::Indent(Cleanup);
			Source.RemoveFromEnd(TEXT("\n"));
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, bExit ? TEXT("G2-exit") : TEXT("G2-finally"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Check = MakeShared<DreamTest::FRunResult>();
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("submit the run and the bot's Stop after the try-body probe"), [this, Buffer, Run, Ticket, Probe, Finished]()
		{
			DreamTest::FDreamTestLatch::ClearMark(Probe);
			DreamTest::FDreamTestLatch::ClearMark(Finished);
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1,
				DreamG2CancelSpec::LineCount(Buffer->GetTextAsString())));
			if (Run->RunNumber <= 0)
			{
				return false;
			}
			DreamTest::FBotStopOptions Options;
			Options.RunNumber = Run->RunNumber;
			Options.Method = DreamTest::EBotStop::Click;
			Options.AwaitMark = Probe;
			*Ticket = DreamTest::SubmitBotStop(Options);
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the cleanup run"), Run, 60.0);
		Steps.Poll(TEXT("the bot's job"), [Ticket]()
		{
			return DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Do(TEXT("check the cleanup"), [this, Run, Ticket, Finished, Probe]()
		{
			DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), TEXT("T-CAN-004 Stop"));
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Cancelled);
			TestTrue(TEXT("the try-body probe fired before the Stop"), DreamTest::FDreamTestLatch::GetMark(Probe) > 0.0);
			const double FinishedAt = DreamTest::FDreamTestLatch::GetMark(Finished);
			TestTrue(TEXT("the cleanup ran to its end marker (RT-06: no delivery while handling)"), FinishedAt > 0.0);
			const double* Injected = (*Ticket)->Times.Find(TEXT("Injected"));
			if (Injected && FinishedAt > 0.0)
			{
				TestTrue(TEXT("the cleanup took its 3 s after the Stop"), FinishedAt - *Injected >= 2.9);
			}
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("read the raised flag"), [Flag]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('raised', %s)"), *Flag));
		}, Check);
		Steps.Do(TEXT("no ScriptCancelled was raised inside the cleanup"), [this, Check]()
		{
			if (DreamTest::ExpectOutcome(*this, *Check, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the cleanup saw no ScriptCancelled: %s"), *Check->Describe()),
					Check->HasLineContaining(dream::LineType::Stdout, TEXT("raised False")));
			}
			return true;
		});
		Steps.Run(Done);
	});
}

void FDreamG2CancelSpec::Define()
{
	BeforeEach([this]()
	{
		Sfx = DreamTest::UniqueSuffix();
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		if (bSettingsSaved)
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->bRunPump = bSavedRunPump;
			Settings->bAlwaysWriteRunLogs = bSavedRunLogs;
			Settings->RunPresentation = SavedPresentation;
			bSettingsSaved = false;
		}
		DreamTest::FDreamTestLatch::ForgetPrefix(TEXT("g2c_"));
		Tab.Reset();
	});

	for (const DreamG2CancelSpec::FShape& Shape : DreamG2CancelSpec::CancelShapes())
	{
		DefineShape(Shape);
	}
	DefineCleanupCase(/*bExit*/ false);
	DefineCleanupCase(/*bExit*/ true);
}
