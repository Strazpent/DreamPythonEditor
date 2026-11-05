// SPDX-License-Identifier: Apache-2.0
// Dream.G2.Supervisor: supervisor cases of the G2 suite and §5.15.3/§5.15.4 that need the §14.2 fixtures (Run window
// presentation). Synchronisation is on run-state probes, latches and marks; durations in a script are the shape.
//   T-SUP-016 (D2, RT-04): a run held on a latch released by the driver 400 ms after entry pumps first at 300 ms plus at
//     most one period and opens the Run window; released at 150 ms it never pumps or opens a window.
//   T-SUP-007 (G2(6)): Python Slate post-tick and ticker callbacks registered during a pumped run fire 0 times while it
//     runs, and a C++ FTSTicker probe shows the pump never ticks the core ticker.
//   T-SUP-005 (G2(4), PERF-13): a fixed CPU loop of about 10 s, supervised against Off: overhead <= 10 %, and the pump
//     rate stays >= 10 Hz unless the overhead would pass 15 % (B-note: the layout is the session's, not a dedicated
//     heavy layout with Sequencer).
//   T-SUP-002 subset (RT-01): user code calling TickCoreTicker(10) in a loop, stopped by the input bot: no crash, a Dream
//     ticker client is deferred (its counter grows, its body never runs during the run). The merge and proxy-mesh steps
//     need static-mesh fixtures this host project does not have (report).
//   T-SUP-003 subset (RT-02): CallPythonOverride in a loop (native code calling a Python override through ProcessEvent),
//     stopped by the input bot: deferrals for native re-entry, ScriptCancelled never raised inside the override,
//     delivered in user code after the call returns. The import hook and actor-delete hook steps need asset fixtures.
//   T-SUP-030 (§5.8, LRS-03): SleepNative(3) at line 12: the Run window shows "The last native call took 3.0 s (line 12)."
//     within one period of the call's return.
//   T-CAN-020 (LRS-03): Stop clicked during SleepNative(5) is delivered at its return, and the record carries the §5.8
//     "Stop took effect when the native call at line N returned" data.
//   T-SUP-004 (G2(3), RT-07, D19): print, then unreal.log / log_warning / log_error, then warnings.warn, each at 1,000
//     lines/s for 20 s (-DreamG2FloodSeconds=60 for the TDD's 60 s): the Run window's live view repaints at >= 10 Hz (Slate
//     ticks inside the pump), FPythonCommandEx::LogOutput stays empty, and every line is in the OutputStore.
//   T-SUP-033 (RT-02, RV-05, LRS-13): a re-run redefines an @unreal.uclass actor class with 10 instances in the level; the
//     reinstancing after the run's own code runs each new instance's _post_init, which calls PostSupervisorCall and loops
//     200 ms: each posted call records one OutsideUserCode deferral (the callback probe; one per post, so the watchdog
//     posted nothing after user_code_done), with zero pumps and zero deliveries, and the outcome is ok.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Fixtures/DreamTestLatch.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamTestPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamG2SupervisorSpec
{
	/** What a pre-tick probe inside the pumps of one run saw (game thread). */
	struct FPumpWatch
	{
		int32 RunNumber = 0;
		double FirstPumpAt = 0.0;
		double LastPumpAt = 0.0;
		double MaxPumpGap = 0.0;        // the longest interval between two Slate ticks inside the pump (T-SUP-004)
		bool bRunWindowSeen = false;
		int32 PumpTicks = 0;
		int64 CoreTicksAtFirstPump = -1;
		int64 CoreTicksAtLastPump = -1;
		double NativeNoteAt = 0.0;
		double NativeSeconds = 0.0;
		int32 NativeLine = 0;
		FString NativeText;
		FDelegateHandle PreTick;
		FTSTicker::FDelegateHandle CoreProbe;
		int64 CoreTicks = 0;
	};

	/** The text of every STextBlock under Root that contains Needle. */
	FString FindText(const TSharedRef<SWidget>& Root, const FString& Needle)
	{
		if (Root->GetType() == FName(TEXT("STextBlock")))
		{
			const FString Text = StaticCastSharedRef<STextBlock>(Root)->GetText().ToString();
			if (Text.Contains(Needle))
			{
				return Text;
			}
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			const FString Found = FindText(Children->GetChildAt(Index), Needle);
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
		return FString();
	}

	void Arm(const TSharedRef<FPumpWatch>& Watch, FRunSupervisor* Supervisor, FDreamRunService* Runs)
	{
		// The C++ FTSTicker probe (T-SUP-007): it ticks only when the core ticker does, never from Dream's pump.
		Watch->CoreProbe = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Watch](float)
		{
			++Watch->CoreTicks;
			return true;
		}));
		Watch->PreTick = FSlateApplication::Get().OnPreTick().AddLambda([Watch, Supervisor, Runs](float)
		{
			const FDreamRunRecord* Active = Runs->GetActiveRecord();
			if (Supervisor->GetPumpDepth() <= 0 || Active == nullptr || Active->RunNumber != Watch->RunNumber)
			{
				return;
			}
			const double Now = FPlatformTime::Seconds();
			if (Watch->FirstPumpAt == 0.0)
			{
				Watch->FirstPumpAt = Now;
				Watch->CoreTicksAtFirstPump = Watch->CoreTicks;
			}
			Watch->CoreTicksAtLastPump = Watch->CoreTicks;
			if (Watch->LastPumpAt > 0.0)
			{
				Watch->MaxPumpGap = FMath::Max(Watch->MaxPumpGap, Now - Watch->LastPumpAt);
			}
			Watch->LastPumpAt = Now;
			++Watch->PumpTicks;
			Watch->bRunWindowSeen = Watch->bRunWindowSeen || Supervisor->IsRunWindowOpen();
			const FDreamRunLiveStats& Live = Supervisor->GetLiveStats();
			if (Watch->NativeNoteAt == 0.0 && Live.LastNativeSeconds > 0.0)
			{
				Watch->NativeNoteAt = Now;
				Watch->NativeSeconds = Live.LastNativeSeconds;
				Watch->NativeLine = Live.LastNativeLine;
			}
			if (Watch->NativeText.IsEmpty() && Watch->NativeNoteAt > 0.0)
			{
				if (const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow())
				{
					Watch->NativeText = FindText(Modal->GetContent(), TEXT("The last native call took"));
				}
			}
		});
	}

	void Disarm(const TSharedRef<FPumpWatch>& Watch)
	{
		if (Watch->PreTick.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreTick().Remove(Watch->PreTick);
		}
		Watch->PreTick.Reset();
		if (Watch->CoreProbe.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Watch->CoreProbe);
			Watch->CoreProbe.Reset();
		}
	}

	using FBotScript = TFunction<void(DreamTest::FDreamInputBotContext&, int32)>;
	using FCheck = TFunction<void(FAutomationTestBase&, const DreamTest::FRunResult&, const FPumpWatch&, const DreamTest::FDreamInputBotTicket*)>;
}

BEGIN_DEFINE_SPEC(FDreamG2SupervisorSpec, "Dream.G2.Supervisor", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Sfx;
	bool bSettingsSaved = false;
	bool bSavedRunPump = true;
	EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	bool Prepare(bool bNeedsBot);
	/** Runs Source (Buffer route) with the pump watch armed and an optional bot script, then Check. */
	void RunWatched(const FDoneDelegate& Done, const FString& Purpose, const FString& Source, DreamG2SupervisorSpec::FBotScript Script,
		DreamG2SupervisorSpec::FCheck Check, double TimeoutSeconds = 60.0);
END_DEFINE_SPEC(FDreamG2SupervisorSpec)

bool FDreamG2SupervisorSpec::Prepare(bool bNeedsBot)
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this)
		|| !TestTrue(TEXT("the _dream_test fixture module exists"), DreamTestPython::IsModuleCreated()))
	{
		return false;
	}
	UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
	if (!bSettingsSaved)
	{
		bSavedRunPump = Settings->bRunPump;
		SavedPresentation = Settings->RunPresentation;
		bSettingsSaved = true;
	}
	Settings->bRunPump = true;
	Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
	return !bNeedsBot || DreamTest::RequireEditorInFront(*this);
}

void FDreamG2SupervisorSpec::RunWatched(const FDoneDelegate& Done, const FString& Purpose, const FString& Source,
	DreamG2SupervisorSpec::FBotScript Script, DreamG2SupervisorSpec::FCheck Check, double TimeoutSeconds)
{
	const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Source, Purpose);
	if (!Buffer.IsValid())
	{
		Done.Execute();
		return;
	}
	BufferIds.Add(Buffer->GetId());
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines, false);
	const int32 LineCount = Lines.Num();
	TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
	TSharedRef<DreamG2SupervisorSpec::FPumpWatch> Watch = MakeShared<DreamG2SupervisorSpec::FPumpWatch>();
	TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();
	FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
	FDreamRunService* Runs = &FDreamEditorModule::Get().GetRunService();
	TSharedRef<FDreamRunService> RunsRef = FDreamEditorModule::Get().GetRunServiceRef();

	DreamTest::FDreamSteps Steps(*this);
	Steps.SetDeadline(TimeoutSeconds + 30.0);
	DreamTest::AddWaitIdle(Steps);
	Steps.Do(TEXT("arm the pump watch, submit, start the bot"), [this, Buffer, LineCount, Run, Watch, Ticket, Script, Supervisor, Runs, RunsRef]()
	{
		DreamG2SupervisorSpec::Arm(Watch, Supervisor, Runs);
		Run->SubmitSeconds = FPlatformTime::Seconds();
		Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LineCount));
		Watch->RunNumber = Run->RunNumber;
		if (Run->RunNumber <= 0)
		{
			return false;
		}
		if (Script)
		{
			const int32 Number = Run->RunNumber;
			*Ticket = DreamTest::FDreamInputBot::Submit([Script, Number, RunsRef, Supervisor](DreamTest::FDreamInputBotContext& Bot)
			{
				Script(Bot, Number);
				if (Bot.HasFailed())
				{
					RunsRef->RequestStopActive(EDreamStopSource::Api);
					const double Deadline = FPlatformTime::Seconds() + 15.0;
					while (RunsRef->GetActiveRunId() != 0 && FPlatformTime::Seconds() < Deadline)
					{
						FPlatformProcess::Sleep(0.01f);
					}
					if (const FDreamRunId Id = RunsRef->GetActiveRunId())
					{
						Supervisor->RequestInterruptCleanup(Id);
					}
				}
			});
		}
		return true;
	});
	DreamTest::AddWaitForRun(Steps, Purpose, Run, TimeoutSeconds);
	Steps.Poll(TEXT("the bot's job"), [Ticket]()
	{
		return !Ticket->IsValid() || DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, 30.0);
	Steps.Do(TEXT("check"), [this, Run, Watch, Ticket, Check, Purpose]()
	{
		DreamG2SupervisorSpec::Disarm(Watch);
		if (Ticket->IsValid())
		{
			DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), Purpose);
		}
		Check(*this, *Run, *Watch, Ticket->Get());
		return true;
	});
	Steps.Finally(TEXT("disarm"), [Watch]()
	{
		DreamG2SupervisorSpec::Disarm(Watch);
	});
	Steps.Run(Done);
}

void FDreamG2SupervisorSpec::Define()
{
	using namespace DreamG2SupervisorSpec;

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
			Settings->RunPresentation = SavedPresentation;
			bSettingsSaved = false;
		}
		DreamTest::FDreamTestLatch::ForgetPrefix(TEXT("g2s_"));
		Tab.Reset();
	});

	for (const int32 ReleaseMs : { 400, 150 })
	{
		LatentIt(FString::Printf(TEXT("T-SUP-016: a latch released %d ms after entry %s"), ReleaseMs,
			ReleaseMs >= 300 ? TEXT("pumps first at 300 ms plus at most one period, with the Run window") : TEXT("never pumps or opens a window")),
			DreamTest::LatentTimeout(), [this, ReleaseMs](const FDoneDelegate& Done)
		{
			if (!Prepare(/*bNeedsBot*/ false))
			{
				Done.Execute();
				return;
			}
			const FString Latch = FString::Printf(TEXT("g2s_first_%s_%d"), *Sfx, ReleaseMs);
			DreamTest::FDreamTestLatch::Reset(Latch);
			RunWatched(Done, TEXT("G2FirstPump"), FString::Printf(TEXT("import _dream_test\n_dream_test.wait('%s')"), *Latch),
				[Latch, ReleaseMs](DreamTest::FDreamInputBotContext& Bot, int32)
			{
				// The release time is the shape under test: the driver lets the run go ReleaseMs after it reached the latch.
				if (!Bot.WaitUntil([Latch]() { return DreamTest::FDreamTestLatch::WasReached(Latch); }, 15.0, TEXT("the run to reach the latch")))
				{
					return;
				}
				const double ReachedAt = FPlatformTime::Seconds();
				while (FPlatformTime::Seconds() - ReachedAt < ReleaseMs / 1000.0)
				{
					FPlatformProcess::SleepNoStats(0.0005f);
				}
				DreamTest::FDreamTestLatch::Release(Latch);
			}, [ReleaseMs](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch& Watch, const DreamTest::FDreamInputBotTicket*)
			{
				if (!DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
				{
					return;
				}
				if (ReleaseMs < 300)
				{
					Test.TestEqual(TEXT("the 150 ms run never pumped (T-SUP-016)"), Run.Record.Stats.Pumps, 0);
					Test.TestFalse(TEXT("and never opened the Run window"), Watch.bRunWindowSeen);
					return;
				}
				const double FirstPump = Watch.FirstPumpAt - Run.Record.EnterSeconds;
				// One period at the initial 15-20 Hz rate (T-SUP-017) is at most 1/15 s; the probe runs inside the pump's Slate tick.
				const double Bound = 0.300 + 1.0 / 15.0 + 0.020;
				Test.AddInfo(FString::Printf(TEXT("first pump %.1f ms after entry (bound %.1f ms), %d pumps"), FirstPump * 1000.0, Bound * 1000.0, Run.Record.Stats.Pumps));
				Test.TestTrue(TEXT("the run pumped"), Watch.FirstPumpAt > 0.0);
				Test.TestTrue(*FString::Printf(TEXT("the first pump came at 300 ms plus at most one period (%.1f ms)"), FirstPump * 1000.0),
					FirstPump >= 0.290 && FirstPump <= Bound);
				Test.TestTrue(TEXT("the Run window opened"), Watch.bRunWindowSeen);
			});
		});
	}

	LatentIt("T-SUP-007: Python Slate and ticker callbacks fire 0 times during a pumped run, and the pump never ticks the core ticker",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(false))
		{
			Done.Execute();
			return;
		}
		const FString Latch = FString::Printf(TEXT("g2s_cb_%s"), *Sfx);
		DreamTest::FDreamTestLatch::Reset(Latch);
		const FString Source = FString::Printf(TEXT(
			"import unreal, _dream_test\n"
			"g2s7 = {'slate': 0, 'ticker': 0}\n"
			"def g2s7_slate(delta):\n"
			"    g2s7['slate'] += 1\n"
			"def g2s7_tick(delta):\n"
			"    g2s7['ticker'] += 1\n"
			"    return True\n"
			"g2s7_h1 = unreal.register_slate_post_tick_callback(g2s7_slate)\n"
			"g2s7_h2 = unreal.register_ticker_callback(g2s7_tick)\n"
			"_dream_test.wait('%s')\n"
			"unreal.unregister_slate_post_tick_callback(g2s7_h1)\n"
			"unreal.unregister_ticker_callback(g2s7_h2)\n"
			"print('callbacks', g2s7['slate'], g2s7['ticker'])"), *Latch);
		RunWatched(Done, TEXT("G2Callbacks"), Source, [Latch](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			// Hold the run for twenty pumps with the callbacks registered, then let it finish.
			Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S) { return S.ActiveRunNumber == Number && S.PumpPublishes >= 20; }, 20.0,
				TEXT("twenty pumps"));
			DreamTest::FDreamTestLatch::Release(Latch);
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch& Watch, const DreamTest::FDreamInputBotTicket*)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
			{
				Test.TestTrue(*FString::Printf(TEXT("neither Python callback fired during the run (G2(6)): %s"), *Run.Describe()),
					Run.HasLineContaining(dream::LineType::Stdout, TEXT("callbacks 0 0")));
			}
			Test.TestTrue(TEXT("the run pumped"), Watch.PumpTicks >= 2);
			Test.TestEqual(TEXT("the core ticker did not tick between the first and the last pump (the pump never ticks it)"),
				Watch.CoreTicksAtLastPump, Watch.CoreTicksAtFirstPump);
		});
	});

	LatentIt("T-SUP-005: a 10 s CPU loop costs <= 10 % more supervised than with the pump off, at >= 10 Hz", FTimespan::FromSeconds(170.0),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare(false))
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamTest::FRunResult> Calibrate = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Off = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> On = MakeShared<DreamTest::FRunResult>();
		TSharedRef<int64> Iterations = MakeShared<int64>(0);
		auto Elapsed = [](const DreamTest::FRunResult& Run)
		{
			for (const DreamTest::FOutputLine& Line : Run.OfType(dream::LineType::Stdout))
			{
				if (Line.Text.StartsWith(TEXT("elapsed ")))
				{
					return FCString::Atod(*Line.Text.Mid(8));
				}
			}
			return -1.0;
		};
		auto Loop = [](int64 N)
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT(
				"import time\n"
				"g2s5_t = time.perf_counter()\n"
				"for g2s5_i in range(%lld):\n"
				"    pass\n"
				"print('elapsed', time.perf_counter() - g2s5_t)"), N));
		};
		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(160.0);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("pump off for the calibration and the baseline"), []() { UDreamUserSettings::GetMutable()->bRunPump = false; return true; });
		DreamTest::AddRun(Steps, TEXT("calibration"), [Loop]() { return Loop(5000000); }, Calibrate);
		Steps.Do(TEXT("size the loop to about 10 s"), [this, Calibrate, Iterations, Elapsed]()
		{
			const double Seconds = Elapsed(*Calibrate);
			if (!TestTrue(*FString::Printf(TEXT("the calibration printed its time: %s"), *Calibrate->Describe()), Seconds > 0.0))
			{
				return false;
			}
			*Iterations = static_cast<int64>(5000000.0 * 10.0 / Seconds);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the loop with the pump off"), [Loop, Iterations]() { return Loop(*Iterations); }, Off, 60.0);
		Steps.Do(TEXT("supervise the same loop"), []() { UDreamUserSettings::GetMutable()->bRunPump = true; return true; });
		DreamTest::AddRun(Steps, TEXT("the loop supervised"), [Loop, Iterations]() { return Loop(*Iterations); }, On, 60.0);
		Steps.Do(TEXT("compare"), [this, Off, On, Elapsed, Iterations]()
		{
			const double TOff = Elapsed(*Off);
			const double TOn = Elapsed(*On);
			if (!TestTrue(TEXT("both runs printed their times"), TOff > 0.0 && TOn > 0.0))
			{
				return false;
			}
			const double Overhead = (TOn - TOff) / TOff;
			const double Rate = On->Record.Stats.Pumps / FMath::Max(TOn, 0.001);
			AddInfo(FString::Printf(TEXT("%lld iterations: off %.3f s, supervised %.3f s, overhead %.1f %%, %d pumps (%.1f Hz), pump cost %.1f ms max"),
				*Iterations, TOff, TOn, Overhead * 100.0, On->Record.Stats.Pumps, Rate, On->Record.Stats.MaxPumpMs));
			TestTrue(*FString::Printf(TEXT("overhead %.1f %% <= 10 %% (PERF-13)"), Overhead * 100.0), Overhead <= 0.10);
			TestTrue(*FString::Printf(TEXT("the pump rate %.1f Hz stays >= 10 Hz unless the overhead would pass 15 %%"), Rate), Rate >= 10.0 || Overhead > 0.15);
			TestEqual(TEXT("the Off run never pumped"), Off->Record.Stats.Pumps, 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("T-SUP-002 subset: TickCoreTicker(10) in a loop, stopped by the bot, defers Dream's ticker clients (RT-01)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare(true))
		{
			Done.Execute();
			return;
		}
		// A Dream ticker client with work pending: its body must never run while the run is active (§3.7).
		const FName Client(*FString::Printf(TEXT("DreamTestDeferred_%s"), *Sfx));
		TSharedRef<int32> BodyDuringRun = MakeShared<int32>(0);
		TSharedRef<uint64> DeferredBefore = MakeShared<uint64>(FDreamDeferral::GetDeferredCount(Client));
		const FTSTicker::FDelegateHandle Handle = FDreamDeferral::AddTicker(Client, EDreamDeferPolicy::DeferWhileExecuting, 0.f, false,
			[BodyDuringRun](float)
		{
			if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().IsRunActive())
			{
				++*BodyDuringRun;
			}
			return true;
		});
		RunWatched(Done, TEXT("G2TickCoreTicker"), TEXT("import _dream_test\nwhile True:\n    _dream_test.tick_core_ticker(10)"),
			[](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S)
				{
					return S.ActiveRunNumber == Number && S.Modal == DreamTest::EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f;
				}, 15.0, TEXT("the Run window")) && Bot.RequireForeground())
			{
				const DreamTest::FDreamProbeSnapshot S = DreamTest::FDreamTestProbes::GetSnapshot();
				if (Bot.MoveTo(S.StopButton, S.RunWindowHwnd, TEXT("Stop")) && Bot.LeftClick() > 0.0)
				{
					// (run group, PO-04) Keep the cursor on Stop until the pump has processed the click: Slate reads the live cursor
					// for the queued mouse move, and SButton clicks only while hovered, so restoring the cursor first loses the click.
					Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& P) { return P.bStopRequested || P.ActiveRunNumber != Number; }, 15.0,
						TEXT("the click on Stop to request Stop"));
				}
			}
		}, [Client, Handle, BodyDuringRun, DeferredBefore](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch&, const DreamTest::FDreamInputBotTicket*)
		{
			FDreamDeferral::RemoveTicker(Handle);
			DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled);
			const uint64 Deferred = FDreamDeferral::GetDeferredCount(Client) - *DeferredBefore;
			Test.AddInfo(FString::Printf(TEXT("the test client was deferred %llu times during the run"), Deferred));
			Test.TestTrue(TEXT("the nested core ticks deferred Dream's ticker client (probe counter > 0)"), Deferred > 0);
			Test.TestEqual(TEXT("its body never ran while the run was active (RT-01)"), *BodyDuringRun, 0);
		});
	});

	LatentIt("T-SUP-003 subset: CallPythonOverride in a loop defers for native re-entry and delivers Stop only after the call returns (RT-02)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(true))
		{
			Done.Execute();
			return;
		}
		const FString Class = FString::Printf(TEXT("DreamG2Hook%s"), *Sfx);
		const FString Source = FString::Printf(TEXT(
			"import time, unreal\n"
			"g2h = {'inside': 0, 'raised': 0}\n"
			"@unreal.uclass()\n"
			"class %s(unreal.DreamTestOverrideFixture):\n"
			"    @unreal.ufunction(override=True)\n"
			"    def on_python_override(self, value):\n"
			"        g2h['inside'] += 1\n"
			"        try:\n"
			"            g2h_end = time.perf_counter() + 0.03\n"
			"            while time.perf_counter() < g2h_end:\n"
			"                pass\n"
			"        except BaseException:\n"
			"            g2h['raised'] += 1\n"
			"            raise\n"
			"g2h_obj = %s()\n"
			"while True:\n"
			"    g2h_obj.call_python_override(3)\n"
			"    g2h_user = time.perf_counter() + 0.03\n"
			"    while time.perf_counter() < g2h_user:\n"
			"        pass"), *Class, *Class);
		TSharedRef<DreamTest::FRunResult> Check = MakeShared<DreamTest::FRunResult>();
		RunWatched(Done, TEXT("G2Override"), Source, [](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S)
				{
					return S.ActiveRunNumber == Number && S.Modal == DreamTest::EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f
						&& S.PumpPublishes >= 4;
				}, 15.0, TEXT("the Run window")) && Bot.RequireForeground())
			{
				const DreamTest::FDreamProbeSnapshot S = DreamTest::FDreamTestProbes::GetSnapshot();
				if (Bot.MoveTo(S.StopButton, S.RunWindowHwnd, TEXT("Stop")) && Bot.LeftClick() > 0.0)
				{
					// (run group, PO-04) Keep the cursor on Stop until the pump has processed the click: Slate reads the live cursor
					// for the queued mouse move, and SButton clicks only while hovered, so restoring the cursor first loses the click.
					Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& P) { return P.bStopRequested || P.ActiveRunNumber != Number; }, 15.0,
						TEXT("the click on Stop to request Stop"));
				}
			}
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch&, const DreamTest::FDreamInputBotTicket*)
		{
			if (!DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				return;
			}
			const int32 Reentry = Run.Record.Stats.Deferrals[static_cast<int32>(EDreamDeferReason::NativeReentry)];
			Test.AddInfo(FString::Printf(TEXT("native re-entry deferrals %d, pumps %d, deliveries %d"), Reentry, Run.Record.Stats.Pumps, Run.Record.Stats.Deliveries));
			Test.TestTrue(TEXT("pending calls inside the override were deferred for native re-entry (RT-02)"), Reentry > 0);
			Test.TestTrue(*FString::Printf(TEXT("Stop landed in user code after the call returned (line %d is the loop, not the override)"), Run.Record.Location.Line),
				Run.Record.Location.Line >= 16 && Run.Record.Location.Line <= 20);
		}, 60.0);
	});

	LatentIt("T-SUP-030: SleepNative(3) at line 12 shows 'The last native call took 3.0 s (line 12).' within one period", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare(false))
		{
			Done.Execute();
			return;
		}
		const FString After = FString::Printf(TEXT("g2s_native_%s"), *Sfx);
		FString Source = TEXT("import _dream_test, time\nfor _ in range(60): time.sleep(0.01)\n");
		for (int32 Line = 3; Line <= 11; ++Line)
		{
			Source += FString::Printf(TEXT("# line %d\n"), Line);
		}
		Source += TEXT("_dream_test.sleep_native(3)\n");
		Source += FString::Printf(TEXT("_dream_test.mark('%s')\nfor _ in range(100): time.sleep(0.01)"), *After);
		RunWatched(Done, TEXT("G2SleepNative"), Source, nullptr,
			[After](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch& Watch, const DreamTest::FDreamInputBotTicket*)
		{
			if (!DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
			{
				return;
			}
			const double Returned = DreamTest::FDreamTestLatch::GetMark(After);
			Test.TestTrue(TEXT("the native call returned"), Returned > 0.0);
			Test.TestTrue(TEXT("the Run window reported the native call"), Watch.NativeNoteAt > 0.0);
			Test.AddInfo(FString::Printf(TEXT("note %.1f ms after the return: %.2f s at line %d, \"%s\""), (Watch.NativeNoteAt - Returned) * 1000.0,
				Watch.NativeSeconds, Watch.NativeLine, *Watch.NativeText));
			Test.TestTrue(TEXT("about 3.0 s"), FMath::Abs(Watch.NativeSeconds - 3.0) <= 0.15);
			Test.TestEqual(TEXT("at line 12"), Watch.NativeLine, 12);
			Test.TestTrue(TEXT("within one period of the return (§5.8)"), Watch.NativeNoteAt - Returned <= 1.0 / 10.0);
			Test.TestTrue(*FString::Printf(TEXT("the Run window's text reads \"The last native call took 3.0 s (line 12).\" (got \"%s\")"), *Watch.NativeText),
				Watch.NativeText.Contains(TEXT("3.0 s (line 12)")));
		});
	});

	LatentIt("T-CAN-020: Stop during SleepNative(5) is delivered at its return, with the native-call Stop record", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare(true))
		{
			Done.Execute();
			return;
		}
#if PLATFORM_WINDOWS
		// (tests B-note) A window that reads no input for 5 s is "not responding" to Windows, which then shows a ghost window
		// in its place that takes the clicks; the click queued during SleepNative(5) would never reach the editor. Test
		// sessions turn ghosting off for the process (it cannot be turned back on; the tests module never ships).
		::DisableProcessWindowsGhosting();
#endif
		const FString Started = FString::Printf(TEXT("g2s_n5_%s"), *Sfx);
		const FString Source = FString::Printf(TEXT(
			"import _dream_test, time\n"
			"for _ in range(60): time.sleep(0.01)\n"
			"_dream_test.mark('%s')\n"
			"_dream_test.sleep_native(5)\n"
			"for _ in range(300): time.sleep(0.01)"), *Started);
		RunWatched(Done, TEXT("G2StopNative"), Source, [Started](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (!Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S)
				{
					return S.ActiveRunNumber == Number && S.Modal == DreamTest::EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f;
				}, 15.0, TEXT("the Run window")))
			{
				return;
			}
			// (run group, chrome-run-r10-m1) The StopPress shape: a check that may query an editor window (the window under a
			// point, the raise below) waits for the game thread, so a check made during the call would move the press to after
			// it. The checks run before the call; during it the bot only moves the cursor and calls SendInput.   (tests, r11)
			const DreamTest::FDreamProbeSnapshot S = DreamTest::FDreamTestProbes::GetSnapshot();
			FVector2D Point(S.StopButton.GetCenter());
			if (!Bot.RequireForeground())
			{
				return;
			}
			if (!Bot.FindUncoveredPointRaising(S.StopButton, S.RunWindowHwnd, Point, TEXT("Stop")))   // (tests, r11) raise fallback
			{
				Bot.Fail(TEXT("input bot: another window covers the Run window's Stop button"));
				return;
			}
			if (!Bot.WaitUntil([Started]() { return DreamTest::FDreamTestLatch::GetMark(Started) > 0.0; }, 10.0, TEXT("the native call to start")))
			{
				return;
			}
			FPlatformProcess::Sleep(0.5f);      // well inside the 5 s call (the shape)
#if PLATFORM_WINDOWS
			if (!::SetCursorPos(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y)))
			{
				Bot.Fail(TEXT("input bot: SetCursorPos onto Stop failed"));
				return;
			}
#endif
			const double Pressed = Bot.LeftClick();     // queued by the OS until the call returns and a pump runs
			Bot.Note(FString::Printf(TEXT("@Pressed=%.9f"), Pressed));
			// Stop acts on the press (A216), so the cursor may leave; the job still ends only once Stop is requested.
			Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& P) { return P.bStopRequested || P.ActiveRunNumber != Number; }, 15.0,
				TEXT("the queued press to request Stop once the native call returned"));
		}, [Started](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch&, const DreamTest::FDreamInputBotTicket* Ticket)
		{
			// (run group, chrome-run-r10-m1) The press itself came during the call, not after its return.
			const double Mark = DreamTest::FDreamTestLatch::GetMark(Started);
			const double* Pressed = Ticket != nullptr ? Ticket->Times.Find(FName(TEXT("Pressed"))) : nullptr;
			if (Pressed != nullptr)
			{
				Test.AddInfo(FString::Printf(TEXT("pressed %.2f s into the 5 s native call"), *Pressed - Mark));
			}
			Test.TestTrue(TEXT("the bot pressed Stop during the native call"), Pressed != nullptr && Mark > 0.0 && *Pressed > Mark && *Pressed - Mark < 4.5);
			if (!DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				return;
			}
			const FDreamRunStats& Stats = Run.Record.Stats;
			Test.AddInfo(FString::Printf(TEXT("Stop during native: %d, %.2f s at line %d"), Stats.bStopDuringNative ? 1 : 0, Stats.StopNativeSeconds, Stats.StopNativeLine));
			Test.TestTrue(TEXT("the record says Stop came during a native call (§5.8 footer)"), Stats.bStopDuringNative);
			Test.TestEqual(TEXT("the native call's line"), Stats.StopNativeLine, 4);
			Test.TestTrue(TEXT("about 5 s"), FMath::Abs(Stats.StopNativeSeconds - 5.0) <= 0.25);
			Test.TestTrue(TEXT("delivered at the return: the run ended after the native call"), Stats.StopEffectiveAt >= DreamTest::FDreamTestLatch::GetMark(Started) + 4.9);
		});
	});

	LatentIt("T-SUP-033: posts during the reinstancing after user code record OutsideUserCode deferrals, never pump or deliver (RT-02)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Prepare(/*bNeedsBot*/ false))
		{
			Done.Execute();
			return;
		}
		// The first definition and 10 instances in the level; the re-run redefines the class, so the plugin's reinstancing
		// (FPyWrapperTypeReinstancer::ProcessPending, after the statement returned and after user_code_done) constructs 10
		// new instances, each running _post_init: one PostSupervisorCall, then a 200 ms loop.
		const FString Class = FString::Printf(TEXT("DreamT033_%s"), *Sfx);
		const FString Mark = FString::Printf(TEXT("g2s_t033_%s"), *Sfx);
		const FString Posted = FString::Printf(TEXT("t033_posted_%s"), *Sfx);
		const FString Define = FString::Printf(TEXT(
			"import unreal\n"
			"@unreal.uclass()\n"
			"class %s(unreal.Actor):\n"
			"    def _post_init(self):\n"
			"        pass\n"
			"t033_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
			"print('spawned', sum(1 for i in range(10) if t033_sub.spawn_actor_from_class(%s, unreal.Vector(i * 100.0, 5000.0, 0.0))))"),
			*Class, *Class);
		const FString Redefine = FString::Printf(TEXT(
			"import _dream_test, time, unreal\n"
			"%s = []\n"
			"@unreal.uclass()\n"
			"class %s(unreal.Actor):\n"
			"    def _post_init(self):\n"
			"        if self.get_name().startswith('Default__'):\n"
			"            return\n"
			"        _dream_test.mark('%s')\n"
			"        %s.append(_dream_test.post_supervisor_call())\n"
			"        t033_end = time.perf_counter() + 0.2\n"
			"        while time.perf_counter() < t033_end:\n"
			"            pass"), *Posted, *Class, *Mark, *Posted);
		const FString Cleanup = FString::Printf(TEXT(
			"import unreal\n"
			"print('posted', len(%s), all(%s))\n"
			"t033_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n"
			"t033_gone = [a for a in t033_sub.get_all_level_actors() if a.get_class().get_name().startswith('%s')]\n"
			"print('instances', len(t033_gone))\n"
			"t033_sub.destroy_actors(t033_gone)\n"
			"del t033_gone"), *Posted, *Posted, *Class);
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Redefine, TEXT("G2T033"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		TArray<FString> Lines;
		Redefine.ParseIntoArrayLines(Lines, false);
		const int32 LineCount = Lines.Num();
		TSharedRef<DreamTest::FRunResult> Defined = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Rerun = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Removed = MakeShared<DreamTest::FRunResult>();
		TSharedRef<bool> bMapWasDirty = MakeShared<bool>(false);

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(150.0);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("remember the level's dirty flag"), [bMapWasDirty]()
		{
			const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			*bMapWasDirty = World != nullptr && World->GetOutermost()->IsDirty();
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the first definition and 10 actors"), [Define]() { return DreamTest::MakeReplRequest(Define); }, Defined, 60.0);
		Steps.Do(TEXT("10 instances are in the level"), [this, Defined]()
		{
			return DreamTest::ExpectOutcome(*this, *Defined, EDreamRunOutcome::Ok)
				&& TestTrue(*FString::Printf(TEXT("10 actors spawned: %s"), *Defined->Describe()), Defined->HasLineContaining(dream::LineType::Stdout, TEXT("spawned 10")));
		});
		Steps.Do(TEXT("forget the mark"), [Mark]() { DreamTest::FDreamTestLatch::ClearMark(Mark); return true; });
		DreamTest::AddRun(Steps, TEXT("the re-run that redefines the class"), [Buffer, LineCount]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LineCount);
		}, Rerun, 60.0);
		Steps.Do(TEXT("check the reinstancing run"), [this, Rerun, Mark]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Rerun, EDreamRunOutcome::Ok))
			{
				return true;
			}
			const FDreamRunStats& Stats = Rerun->Record.Stats;
			const int32 Outside = Stats.Deferrals[static_cast<int32>(EDreamDeferReason::OutsideUserCode)];
			const int32 PostInits = DreamTest::FDreamTestLatch::GetMarkCount(Mark);
			AddInfo(FString::Printf(TEXT("_post_init ran %d times after user code; OutsideUserCode deferrals %d, pumps %d, deliveries %d"),
				PostInits, Outside, Stats.Pumps, Stats.Deliveries));
			TestEqual(TEXT("the reinstancing ran _post_init on the 10 instances"), PostInits, 10);
			TestEqual(TEXT("each posted call ran inside _post_init and recorded one OutsideUserCode deferral; the watchdog posted nothing after user_code_done"),
				Outside, PostInits);
			TestEqual(TEXT("zero pumps (RV-05 pump gate)"), Stats.Pumps, 0);
			TestEqual(TEXT("zero deliveries"), Stats.Deliveries, 0);
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("read the posts and remove the actors"), [Cleanup]() { return DreamTest::MakeReplRequest(Cleanup); }, Removed, 60.0);
		Steps.Do(TEXT("every post was accepted"), [this, Removed]()
		{
			if (DreamTest::ExpectOutcome(*this, *Removed, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("10 posts, all accepted by CPython's queue: %s"), *Removed->Describe()),
					Removed->HasLineContaining(dream::LineType::Stdout, TEXT("posted 10 True")));
				TestTrue(TEXT("the 10 reinstanced actors were found and removed"), Removed->HasLineContaining(dream::LineType::Stdout, TEXT("instances 10")));
			}
			return true;
		});
		Steps.Finally(TEXT("restore the level's dirty flag; forget the mark"), [bMapWasDirty, Mark]()
		{
			DreamTest::FDreamTestLatch::ClearMark(Mark);
			if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			{
				World->GetOutermost()->SetDirtyFlag(*bMapWasDirty);
			}
		});
		Steps.Run(Done);
	});

	LatentIt(FString::Printf(TEXT("T-SUP-004: %d s each of print, unreal.log and warnings.warn at 1,000 lines/s: >= 10 Hz repaint, LogOutput 0, every line stored"),
		FMath::Clamp(DreamTest::CommandLineInt(TEXT("DreamG2FloodSeconds"), 20), 1, 600)),
		FTimespan::FromSeconds(3.0 * FMath::Clamp(DreamTest::CommandLineInt(TEXT("DreamG2FloodSeconds"), 20), 1, 600) + 150.0),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare(false))
		{
			Done.Execute();
			return;
		}
		// (tests B-note) 20 s per phase unless -DreamG2FloodSeconds=60 (the TDD's length), so the shared pass stays short.
		const int32 Seconds = FMath::Clamp(DreamTest::CommandLineInt(TEXT("DreamG2FloodSeconds"), 20), 1, 600);
		const FString Tag = Sfx;
		// unreal.log_error lines reach LogPython at Error through the forwarder's echo (§6.3); they are the shape here.
		AddExpectedErrorPlain(FString::Printf(TEXT("g2flood_l_%s"), *Tag), EAutomationExpectedErrorFlags::Contains, -1);
		const FString Source = FString::Printf(TEXT(
			"import time, unreal, warnings\n"
			"def g2f_paced(emit, seconds):\n"
			"    n = 0\n"
			"    start = time.perf_counter()\n"
			"    while True:\n"
			"        now = time.perf_counter()\n"
			"        if now - start >= seconds:\n"
			"            return n\n"
			"        due = int((now - start) * 1000)\n"
			"        while n < due:\n"
			"            emit(n)\n"
			"            n += 1\n"
			"g2f_logs = (unreal.log, unreal.log_warning, unreal.log_error)\n"
			"g2f_p = g2f_paced(lambda i: print('g2flood_p_%s', i), %d)\n"
			"g2f_l = g2f_paced(lambda i: g2f_logs[i %% 3]('g2flood_l_%s %%d' %% i), %d)\n"
			"with warnings.catch_warnings():\n"
			"    warnings.simplefilter('always')\n"
			"    g2f_w = g2f_paced(lambda i: warnings.warn('g2flood_w_%s %%d' %% i), %d)\n"
			"print('g2flood_counts', g2f_p, g2f_l, g2f_w)"), *Tag, Seconds, *Tag, Seconds, *Tag, Seconds);
		RunWatched(Done, TEXT("G2Flood"), Source, nullptr,
			[Tag, Seconds](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const FPumpWatch& Watch, const DreamTest::FDreamInputBotTicket*)
		{
			if (!DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
			{
				return;
			}
			int32 Printed = -1;
			int32 Logged = -1;
			int32 Warned = -1;
			int32 StoredPrints = 0;
			int32 StoredLogs = 0;
			int32 StoredWarnings = 0;
			const FString PrintNeedle = FString::Printf(TEXT("g2flood_p_%s "), *Tag);
			const FString LogNeedle = FString::Printf(TEXT("g2flood_l_%s "), *Tag);
			const FString WarnNeedle = FString::Printf(TEXT("UserWarning: g2flood_w_%s "), *Tag);
			for (const DreamTest::FOutputLine& Line : Run.Lines)
			{
				if (Line.Type == dream::LineType::Echo)
				{
					continue;
				}
				StoredPrints += Line.Text.StartsWith(PrintNeedle) ? 1 : 0;
				StoredLogs += Line.Text.Contains(LogNeedle) ? 1 : 0;
				StoredWarnings += Line.Text.Contains(WarnNeedle) ? 1 : 0;
				if (Line.Text.StartsWith(TEXT("g2flood_counts ")))
				{
					TArray<FString> Parts;
					Line.Text.ParseIntoArrayWS(Parts);
					if (Parts.Num() == 4)
					{
						Printed = FCString::Atoi(*Parts[1]);
						Logged = FCString::Atoi(*Parts[2]);
						Warned = FCString::Atoi(*Parts[3]);
					}
				}
			}
			const double Span = Watch.LastPumpAt - Watch.FirstPumpAt;
			const double Rate = Span > 0.0 ? (Watch.PumpTicks - 1) / Span : 0.0;
			Test.AddInfo(FString::Printf(TEXT("emitted %d prints, %d log lines, %d warnings over 3 x %d s; stored %d / %d / %d; %d pump repaints in %.1f s "
				"(%.1f Hz, longest gap %.0f ms); LogOutput %d"), Printed, Logged, Warned, Seconds, StoredPrints, StoredLogs, StoredWarnings,
				Watch.PumpTicks, Span, Rate, Watch.MaxPumpGap * 1000.0, Run.Record.Stats.LogOutputEntries));
			if (!Test.TestTrue(TEXT("the script printed its counts"), Printed > 0 && Logged > 0 && Warned > 0))
			{
				return;
			}
			Test.TestTrue(*FString::Printf(TEXT("about 1,000 lines/s were emitted (%d prints in %d s)"), Printed, Seconds), Printed >= Seconds * 900);
			Test.TestEqual(TEXT("Cmd.LogOutput.Num() == 0 (RT-07, D19)"), Run.Record.Stats.LogOutputEntries, 0);
			Test.TestEqual(TEXT("every print is in the OutputStore"), StoredPrints, Printed);
			Test.TestEqual(TEXT("every unreal.log, log_warning and log_error line is in the OutputStore"), StoredLogs, Logged);
			Test.TestEqual(TEXT("every warning is in the OutputStore"), StoredWarnings, Warned);
			Test.TestTrue(*FString::Printf(TEXT("the Run window's live view repainted at >= 10 Hz (%.1f Hz)"), Rate), Rate >= 10.0);
		}, 3.0 * Seconds + 90.0);
	});
}
