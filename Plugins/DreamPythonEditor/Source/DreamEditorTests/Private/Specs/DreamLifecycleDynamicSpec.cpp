// SPDX-License-Identifier: Apache-2.0
// Dream.Lifecycle.Dynamic: Python disabled and re-enabled at runtime around runs (RUN-10's "re-captured after a Python
// restart", T-LIFE-006, T-LIFE-007, D16, ENV-05). It needs its own editor session with Engine.Python.IsPythonLifeCycleDynamic
// set (a read-only console variable, so only an ini value set at startup changes it); in the shared attended pass it only
// says how to run it, because ForceDisablePythonAtRuntime there would cycle Python under every later spec:
//   DreamHost\Scripts\run_tier4.bat --attended --filter Dream.Lifecycle.Dynamic
//       --args "-ini:Engine:[ConsoleVariables]:Engine.Python.IsPythonLifeCycleDynamic=1"
// (add -DreamLifeCycles=<n> to the --args for fewer than the 100 cycles of T-LIFE-006).
// Each cycle, from Disabled (the dynamic session starts without Python) or ShutDown:
//   Enable Python (FDreamPython::RequestEnable, the banner's action: ForceEnablePythonAtRuntime from OnEndFrame, T-LIFE-005)
//   with a REPL run submitted at once, which waits in Enabling and runs after the boot; it finds a fresh Session (no name
//   of the previous generation) and a fresh _dream (no attribute set on the previous one), and defines a name; a buffer
//   run `while True: pass` is stopped once its Run window is up (the watchdog's hand-off and the asynchronous
//   ScriptCancelled of this generation, T-LIFE-006); a REPL run reads the name back (the Session is shared across runs);
//   then ForceDisablePythonAtRuntime: Dream is ShutDown with its reason, _dream_test is gone, no Python reference outlived the
//   interpreter (the "still alive at Python shutdown" ensure of FDreamPython::HandleShutdown), and a run is refused with
//   the shut-down reason. Every cycle raises the generation by exactly one and fires Dream's OnInitialized and OnShutdown
//   once each (T-LIFE-007). The watchdog thread state's creation and deletion per generation have no observable outside
//   the supervisor beyond the Stop delivered in every generation (tests report).
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "HAL/IConsoleManager.h"
#include "IPythonScriptPlugin.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamPython.h"
#include "Python/DreamTestPython.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"

namespace DreamLifecycleDynamicSpec
{
	bool IsLifeCycleDynamic()
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Engine.Python.IsPythonLifeCycleDynamic"));
		return Var != nullptr && Var->GetBool();
	}

	/** What the cycles observe through FDreamPython's events (game thread). */
	struct FCounters
	{
		int32 Initialized = 0;
		int32 Shutdowns = 0;
		int32 ShutDownStates = 0;
		uint32 Generation = 0;
		FDelegateHandle InitializedHandle;
		FDelegateHandle ShutdownHandle;
		FDelegateHandle StateHandle;
	};

	const FString LeakNeedle = TEXT("Python references are still alive at Python shutdown");
}

BEGIN_DEFINE_SPEC(FDreamLifecycleDynamicSpec, "Dream.Lifecycle.Dynamic", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	bool bSettingsSaved = false;
	bool bSavedRunPump = true;
	EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
END_DEFINE_SPEC(FDreamLifecycleDynamicSpec)

void FDreamLifecycleDynamicSpec::Define()
{
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
		Tab.Reset();
	});

	const int32 Cycles = FMath::Clamp(DreamTest::CommandLineInt(TEXT("DreamLifeCycles"), 100), 1, 1000);
	const double Budget = 60.0 + Cycles * 30.0;
	LatentIt(FString::Printf(TEXT("RUN-10 / T-LIFE-006: %d disable/enable cycles at runtime, each with runs around it"), Cycles),
		FTimespan::FromSeconds(Budget + 30.0), [this, Cycles, Budget](const FDoneDelegate& Done)
	{
		using namespace DreamLifecycleDynamicSpec;
		if (!IsLifeCycleDynamic())
		{
			AddWarning(TEXT("NOT RUN (RUN-10 / T-LIFE-006 need their own session): Engine.Python.IsPythonLifeCycleDynamic is off in this session, so nothing ran: this spec needs its own editor "
				"session (run_tier4.bat --attended --filter Dream.Lifecycle.Dynamic --args "
				"\"-ini:Engine:[ConsoleVariables]:Engine.Python.IsPythonLifeCycleDynamic=1\")."));
			Done.Execute();
			return;
		}
		IPythonScriptPlugin* Plugin = IPythonScriptPlugin::Get();
		FDreamEditorModule* Module = DreamTest::GetModule(*this);
		Tab = DreamTest::RequireTab(*this);
		if (Plugin == nullptr || Module == nullptr || !Tab.IsValid())
		{
			TestNotNull(TEXT("PythonScriptPlugin"), Plugin);
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Loop = DreamTest::OpenScratch(*this, *Tab, TEXT("while True: pass"), TEXT("LifeLoop"));
		if (!Loop.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Loop->GetId());
		// The Stop of each cycle waits for the Run window: force the supervised presentation, restore after.
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		bSavedRunPump = Settings->bRunPump;
		SavedPresentation = Settings->RunPresentation;
		bSettingsSaved = true;
		Settings->bRunPump = true;
		Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;

		TSharedRef<FCounters> Counters = MakeShared<FCounters>();
		FDreamPython& Python = Module->GetPython();
		Counters->Generation = Python.GetGeneration();
		Counters->InitializedHandle = Python.OnInitialized().AddLambda([Counters]() { ++Counters->Initialized; });
		Counters->ShutdownHandle = Python.OnShutdown().AddLambda([Counters]() { ++Counters->Shutdowns; });
		Counters->StateHandle = Python.OnStateChanged().AddLambda([Counters](EDreamPythonState State)
		{
			Counters->ShutDownStates += State == EDreamPythonState::ShutDown ? 1 : 0;
		});
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();

		DreamTest::FDreamSteps Steps(*this);
		Steps.SetDeadline(Budget);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("Python is off before the first cycle"), [this, Plugin]()
		{
			if (Plugin->IsPythonInitialized())
			{
				// An earlier spec of this session, or a startup script, enabled it: start from a shutdown.
				Plugin->ForceDisablePythonAtRuntime();
			}
			// A dynamic session that never started Python shows Disabled with Enable Python offered (T-LIFE-004); after a
			// shutdown it is ShutDown.
			const EDreamPythonState State = FDreamEditorModule::Get().GetPython().GetState();
			return TestTrue(*FString::Printf(TEXT("Dream Python Editor waits for Python to be enabled (state %s)"), DreamTest::PythonStateName(State)),
				State == EDreamPythonState::Disabled || State == EDreamPythonState::ShutDown);
		});

		for (int32 Cycle = 1; Cycle <= Cycles; ++Cycle)
		{
			const FString Name = FString::Printf(TEXT("life_%s"), *Sfx);
			const FString Tag = FString::Printf(TEXT("cycle %d"), Cycle);
			TSharedRef<DreamTest::FRunResult> Fresh = MakeShared<DreamTest::FRunResult>();
			TSharedRef<DreamTest::FRunResult> Stopped = MakeShared<DreamTest::FRunResult>();
			TSharedRef<DreamTest::FRunResult> Kept = MakeShared<DreamTest::FRunResult>();
			TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();

			Steps.Do(Tag + TEXT(": Enable Python, with a run submitted at once"), [this, Fresh, Name, Cycle, Tag]()
			{
				FDreamPython& Py = FDreamEditorModule::Get().GetPython();
				Py.RequestEnable();
				if (!TestTrue(*FString::Printf(TEXT("%s: Enable Python moves Dream Python Editor to Enabling (state %s)"), *Tag, DreamTest::PythonStateName(Py.GetState())),
					Py.GetState() == EDreamPythonState::Enabling))
				{
					return false;
				}
				// The run waits in Enabling and runs once OnPythonInitialized has made _dream and the boot is done (§3.5 table).
				Fresh->SubmitSeconds = FPlatformTime::Seconds();
				Fresh->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(TEXT(
					"import _dream\n"
					"print('fresh', '%s' in globals(), getattr(_dream, '_t4_life', None))\n"
					"%s = %d\n"
					"_dream._t4_life = %d"), *Name, *Name, Cycle, Cycle)));
				return Fresh->RunNumber > 0;
			});
			DreamTest::AddWaitForRun(Steps, Tag + TEXT(" fresh-Session run"), Fresh, 60.0);
			Steps.Do(Tag + TEXT(": a new generation with a fresh Session and _dream"), [this, Fresh, Counters, Cycle, Tag]()
			{
				const FDreamPython& Py = FDreamEditorModule::Get().GetPython();
				const uint32 Generation = Py.GetGeneration();
				TestEqual(*FString::Printf(TEXT("%s: the generation rose by exactly one"), *Tag), static_cast<int64>(Generation),
					static_cast<int64>(Counters->Generation) + 1);
				Counters->Generation = Generation;
				TestEqual(*FString::Printf(TEXT("%s: Dream's OnInitialized fired once per cycle (T-LIFE-007)"), *Tag), Counters->Initialized, Cycle);
				TestTrue(*FString::Printf(TEXT("%s: _dream_test was created again"), *Tag), DreamTestPython::IsModuleCreated());
				if (DreamTest::ExpectOutcome(*this, *Fresh, EDreamRunOutcome::Ok))
				{
					TestTrue(*FString::Printf(TEXT("%s: the Session holds no name of the previous generation and _dream is a new module (RUN-10): %s"),
						*Tag, *Fresh->Describe()), Fresh->HasLineContaining(dream::LineType::Stdout, TEXT("fresh False None")));
				}
				return true;
			});
			Steps.Do(Tag + TEXT(": a busy loop, stopped once its Run window is up"), [this, Loop, Stopped, Ticket]()
			{
				Stopped->SubmitSeconds = FPlatformTime::Seconds();
				Stopped->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Loop, EDreamRunRoute::Buffer, 1, 1));
				if (Stopped->RunNumber <= 0)
				{
					return false;
				}
				DreamTest::FBotStopOptions Options;
				Options.RunNumber = Stopped->RunNumber;
				Options.Method = DreamTest::EBotStop::Api;
				*Ticket = DreamTest::SubmitBotStop(Options);
				return true;
			});
			DreamTest::AddWaitForRun(Steps, Tag + TEXT(" stopped loop"), Stopped, 40.0);
			Steps.Poll(Tag + TEXT(": the stop job ends"), [this, Ticket, Stopped, Tag]()
			{
				if (!DreamTest::IsTicketDone(Ticket->ToSharedRef()))
				{
					return DreamTest::EStep::Wait;
				}
				DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), Tag + TEXT(" Stop"));
				DreamTest::ExpectOutcome(*this, *Stopped, EDreamRunOutcome::Cancelled);
				TestTrue(*FString::Printf(TEXT("%s: ScriptCancelled reached the game thread's run in this generation (%d deliveries)"), *Tag,
					Stopped->Record.Stats.Deliveries), Stopped->Record.Stats.Deliveries >= 1);
				return DreamTest::EStep::Next;
			}, 20.0);
			DreamTest::AddRun(Steps, Tag + TEXT(" Session read"), [Name]()
			{
				return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('kept', %s)"), *Name));
			}, Kept);
			Steps.Do(Tag + TEXT(": the Session is shared across the runs of a generation"), [this, Kept, Cycle, Tag]()
			{
				if (DreamTest::ExpectOutcome(*this, *Kept, EDreamRunOutcome::Ok))
				{
					TestTrue(*FString::Printf(TEXT("%s: a later run reads the name: %s"), *Tag, *Kept->Describe()),
						Kept->HasLineContaining(dream::LineType::Stdout, FString::Printf(TEXT("kept %d"), Cycle)));
				}
				return true;
			});
			Steps.Do(Tag + TEXT(": ForceDisablePythonAtRuntime"), [this, Plugin, Counters, Log, Cycle, Tag]()
			{
				Plugin->ForceDisablePythonAtRuntime();
				FDreamPython& Py = FDreamEditorModule::Get().GetPython();
				TestFalse(*FString::Printf(TEXT("%s: the interpreter is down"), *Tag), Plugin->IsPythonInitialized());
				TestTrue(*FString::Printf(TEXT("%s: Dream Python Editor is ShutDown (state %s)"), *Tag, DreamTest::PythonStateName(Py.GetState())),
					Py.GetState() == EDreamPythonState::ShutDown);
				TestTrue(*FString::Printf(TEXT("%s: its reason says so: %s"), *Tag, *Py.GetDisabledReason().ToString()),
					Py.GetDisabledReason().ToString().Contains(TEXT("shut down")));
				TestFalse(*FString::Printf(TEXT("%s: _dream_test went with the interpreter"), *Tag), DreamTestPython::IsModuleCreated());
				TestEqual(*FString::Printf(TEXT("%s: Dream's OnShutdown fired once per cycle (T-LIFE-007)"), *Tag), Counters->Shutdowns, Cycle);
				TestEqual(*FString::Printf(TEXT("%s: one ShutDown state per cycle"), *Tag), Counters->ShutDownStates, Cycle);
				TestEqual(*FString::Printf(TEXT("%s: no Python reference outlived the interpreter (T-LIFE-006 registry)"), *Tag),
					Log->CountContaining(TEXT("LogOutputDevice"), LeakNeedle) + Log->CountContaining(TEXT("LogDream"), LeakNeedle), 0);
				// A run while Python is down is refused with the reason (§3.5 table: ShutDown accepts no runs).
				FText Reason;
				TestFalse(*FString::Printf(TEXT("%s: a run is refused while Python is shut down"), *Tag),
					FDreamEditorModule::Get().GetRunService().CanSubmit(EDreamRunRoute::Repl, Reason));
				TestTrue(*FString::Printf(TEXT("%s: with the shut-down reason (%s)"), *Tag, *Reason.ToString()), Reason.ToString().Contains(TEXT("shut down")));
				return true;
			});
		}

		Steps.Do(TEXT("leave Python enabled"), [this]()
		{
			FDreamEditorModule::Get().GetPython().RequestEnable();
			return true;
		});
		Steps.Poll(TEXT("Python is back"), []()
		{
			const EDreamPythonState State = FDreamEditorModule::Get().GetPython().GetState();
			return State == EDreamPythonState::Cold || State == EDreamPythonState::Ready ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.Finally(TEXT("unbind the counters"), [Counters, Log]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamPython& Py = FDreamEditorModule::Get().GetPython();
				Py.OnInitialized().Remove(Counters->InitializedHandle);
				Py.OnShutdown().Remove(Counters->ShutdownHandle);
				Py.OnStateChanged().Remove(Counters->StateHandle);
			}
		});
		Steps.Run(Done);
	});
}
