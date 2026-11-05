// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.Input: the input side of a supervised run (TDD §5.5.5, §5.6, RT-04, RT-05).
//   1. The pre-processor is registered while the run pumps and removed at finish (T-SUP-025); with the Run window as the
//      active modal a letter key passes to it, and Esc stops the run and swallows its key-up (T-SUP-027 subset: the
//      foreign-modal, menu and QueueOsKeys cases wait for the input bot, slice B). The events are synthetic FKeyEvents
//      given to the installed FDreamInputPreProcessor from a Slate pre-tick inside the pump, as Slate would.
//   2. The Stop command's chord (Shift+F5 unless the user rebound it) stops the run and is swallowed with its key-up
//      (T-SUP-028 subset).
//   3. The Off presentation (run.pump off, the kill switch): no watchdog posts and no pumps; the output appears at the end
//      (T-SUP-024). No Slate tick runs during such a run, so the Run window and the pre-processor are checked afterwards.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Commands/DreamInputPreProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Widgets/SWindow.h"

namespace DreamInputSpec
{
	struct FState
	{
		int32 RunNumber = 0;
		FDelegateHandle PreTickHandle;
		bool bChecked = false;
		bool bRegistered = false;
		bool bLetterPassed = false;
		bool bLetterUpPassed = false;
		bool bEscSwallowed = false;
		bool bEscUpSwallowed = false;
		bool bStopRequested = false;
		bool bShiftF5 = false;          // case 2: the probe sends Shift+F5 instead of the letter and Esc
		bool bChordSwallowed = false;
		bool bChordUpSwallowed = false;
		FString Diagnostic;             // what the Esc rule saw: the active modal, menus, the focused widget
		TSharedPtr<FDreamInputPreProcessor> PreProcessor;

		bool bSettingsSaved = false;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	FKeyEvent Key(const FKey& InKey, uint32 Character)
	{
		const FModifierKeysState NoMods;
		return FKeyEvent(InKey, NoMods, static_cast<uint32>(0), /*bInIsRepeat*/ false, Character, Character);
	}

	/** The Stop command's chord as this machine binds it (the editor's keyboard shortcuts are a user setting), else Shift+F5:
	 *  what FDreamInputPreProcessor's IsStopChord matches (P3-run-06). Left-hand modifiers. */
	FKeyEvent StopChordEvent()
	{
		FInputChord Chord(EModifierKey::Shift, EKeys::F5);
		if (FDreamCommands::IsRegistered())
		{
			if (const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(EDreamCommand::Stop))
			{
				const TSharedRef<const FInputChord> Primary = Info->GetActiveChord(EMultipleKeyBindingIndex::Primary);
				const TSharedRef<const FInputChord> Secondary = Info->GetActiveChord(EMultipleKeyBindingIndex::Secondary);
				if (Primary->IsValidChord())
				{
					Chord = *Primary;
				}
				else if (Secondary->IsValidChord())
				{
					Chord = *Secondary;
				}
			}
		}
		const FModifierKeysState Mods(Chord.NeedsShift(), false, Chord.NeedsControl(), false, Chord.NeedsAlt(), false,
			Chord.NeedsCommand(), false, false);
		return FKeyEvent(Chord.Key, Mods, static_cast<uint32>(0), /*bInIsRepeat*/ false, 0, 0);
	}

	/** Slate pre-tick inside a pump with the Run window up: the pre-processor's key decisions, then Esc as the Stop. */
	void OnPreTick(FState& State, FRunSupervisor& Supervisor, const FDreamRunService& RunService)
	{
		if (State.bChecked || Supervisor.GetPumpDepth() <= 0 || !Supervisor.IsRunWindowOpen())
		{
			return;
		}
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		const FDreamRunId Id = Supervisor.GetCurrentRunId();
		if (!Active || Active->RunNumber != State.RunNumber || Id == 0)
		{
			return;
		}
		State.bChecked = true;
		FSlateApplication& App = FSlateApplication::Get();
		State.PreProcessor = Supervisor.GetInputPreProcessorForTest();
		State.bRegistered = State.PreProcessor.IsValid() && App.FindInputPreProcessor(State.PreProcessor, EInputPreProcessorType::PreEngine) != INDEX_NONE;
		if (!State.PreProcessor.IsValid())
		{
			return;
		}
		if (State.bShiftF5)
		{
			// §4.9.4, §5.6.2: the run-level Stop chord stops and is swallowed in every context, its key-up too.
			const FKeyEvent Chord = StopChordEvent();
			State.bChordSwallowed = State.PreProcessor->HandleKeyDownEvent(App, Chord);
			State.bChordUpSwallowed = State.PreProcessor->HandleKeyUpEvent(App, Chord);
			State.bStopRequested = Supervisor.IsStopRequested(Id);
			return;
		}
		// §5.6.2: with a modal up (ours), other keys go to it; the pre-processor passes them and their key-ups.
		State.bLetterPassed = !State.PreProcessor->HandleKeyDownEvent(App, Key(EKeys::A, 'a'));
		State.bLetterUpPassed = !State.PreProcessor->HandleKeyUpEvent(App, Key(EKeys::A, 'a'));
		// §5.6.3, RT-05: our modal is active, no menu, no text entry focused: Esc stops, and its key-up is swallowed too.
		State.bEscSwallowed = State.PreProcessor->HandleKeyDownEvent(App, Key(EKeys::Escape, 27));
		State.bEscUpSwallowed = State.PreProcessor->HandleKeyUpEvent(App, Key(EKeys::Escape, 27));
		State.bStopRequested = Supervisor.IsStopRequested(Id);
		const TSharedPtr<SWindow> Modal = App.GetActiveModalWindow();
		const TSharedPtr<SWidget> Focused = App.GetKeyboardFocusedWidget();
		State.Diagnostic = FString::Printf(TEXT("modal \"%s\", menus %d, focus %s"), Modal.IsValid() ? *Modal->GetTitle().ToString() : TEXT("none"),
			App.AnyMenusVisible() ? 1 : 0, Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none"));
	}
}

BEGIN_DEFINE_SPEC(FDreamInputSpec, "Dream.Supervisor.Input", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
END_DEFINE_SPEC(FDreamInputSpec)

void FDreamInputSpec::Define()
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

	LatentIt("registers the pre-processor for the run, passes keys to the Run window and stops on Esc (T-SUP-025, RT-05)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			TEXT("import time\nfor _ in range(400):\n    time.sleep(0.025)"), TEXT("Input"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		FDreamRunService* RunService = &Module.GetRunService();
		TSharedRef<DreamInputSpec::FState> State = MakeShared<DreamInputSpec::FState>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("arm the probe and submit the loop"), [this, Buffer, State, Supervisor, RunService, Run]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->SavedPresentation = Settings->RunPresentation;
			State->bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			// RT-05 passes Esc to a focused editable text field (a script's dialog). An earlier spec may leave focus in one (the
			// Output Log's console box), so the run starts from the Dream code view, read-only while the run is active.
			FSlateApplication::Get().DismissAllMenus();
			Tab->ActivateBuffer(BufferId);
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				View->FocusEditor();
			}
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				DreamInputSpec::OnPreTick(*State, *Supervisor, *RunService);
			});
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the loop"), Run, 60.0);
		Steps.Do(TEXT("check the input decisions"), [this, State, Run]()
		{
			if (!TestTrue(TEXT("a pump with the Run window up ran the probe"), State->bChecked))
			{
				return false;
			}
			TestTrue(TEXT("the pre-processor is registered while the run pumps (T-SUP-025)"), State->bRegistered);
			TestTrue(TEXT("a letter key passes to the Run window (our modal)"), State->bLetterPassed);
			TestTrue(TEXT("its key-up passes too"), State->bLetterUpPassed);
			TestTrue(*FString::Printf(TEXT("Esc is swallowed (RT-05; %s)"), *State->Diagnostic), State->bEscSwallowed);
			TestTrue(TEXT("Esc's key-up is swallowed with it"), State->bEscUpSwallowed);
			TestTrue(TEXT("Esc requested Stop"), State->bStopRequested);
			if (DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Cancelled))
			{
				TestTrue(TEXT("the record names Esc as the Stop source"), Run->Record.StopSource == EDreamStopSource::Esc);
			}
			const bool bStillRegistered = State->PreProcessor.IsValid() && FSlateApplication::Get().FindInputPreProcessor(State->PreProcessor, EInputPreProcessorType::PreEngine) != INDEX_NONE;
			TestFalse(TEXT("the pre-processor is removed at finish (T-SUP-025)"), bStillRegistered);
			State->PreProcessor.Reset();
			return true;
		});
		Steps.Finally(TEXT("disarm"), [State]()
		{
			if (State->PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTickHandle);
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

	LatentIt("stops the run on Shift+F5 and swallows the chord (T-SUP-028 subset)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			TEXT("import time\nfor _ in range(400):\n    time.sleep(0.025)"), TEXT("ShiftF5"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		FDreamRunService* RunService = &Module.GetRunService();
		TSharedRef<DreamInputSpec::FState> State = MakeShared<DreamInputSpec::FState>();
		State->bShiftF5 = true;
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("arm the probe and submit the loop"), [this, Buffer, State, Supervisor, RunService, Run]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->SavedPresentation = Settings->RunPresentation;
			State->bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				DreamInputSpec::OnPreTick(*State, *Supervisor, *RunService);
			});
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 3));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the loop"), Run, 60.0);
		Steps.Do(TEXT("check the Stop chord"), [this, State, Run]()
		{
			if (!TestTrue(TEXT("a pump with the Run window up ran the probe"), State->bChecked))
			{
				return false;
			}
			TestTrue(TEXT("Shift+F5 is swallowed"), State->bChordSwallowed);
			TestTrue(TEXT("its key-up is swallowed with it"), State->bChordUpSwallowed);
			TestTrue(TEXT("Shift+F5 requested Stop"), State->bStopRequested);
			if (DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Cancelled))
			{
				TestTrue(TEXT("the record names Shift+F5 as the Stop source"), Run->Record.StopSource == EDreamStopSource::ShiftF5);
			}
			State->PreProcessor.Reset();
			return true;
		});
		Steps.Finally(TEXT("disarm"), [State]()
		{
			if (State->PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTickHandle);
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

	LatentIt("runs with no watchdog, pump, Run window or pre-processor under the Off presentation (T-SUP-024)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			TEXT("import time\nfor i in range(8):\n    print('off', i)\n    time.sleep(0.1)"), TEXT("Off"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferId = Buffer->GetId();
		TSharedRef<DreamInputSpec::FState> State = MakeShared<DreamInputSpec::FState>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("turn run.pump off"), [State]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->bSettingsSaved = true;
			Settings->bRunPump = false;
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("the 0.8 s loop"), [Buffer]()
		{
			return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 4);
		}, Run, 30.0);
		Steps.Do(TEXT("check the Off run"), [this, Run]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			const FDreamRunStats& Stats = Run->Record.Stats;
			TestEqual(TEXT("no pump ran (§5.5.5)"), Stats.Pumps, 0);
			// A watchdog post that reached the game thread either pumps or defers (Deferrals); neither may happen under Off.
			int32 Deferred = 0;
			for (const int32 Count : Stats.Deferrals)
			{
				Deferred += Count;
			}
			TestEqual(TEXT("no pending call arrived (no watchdog posts)"), Deferred, 0);
			TestFalse(TEXT("sanity: no Run window is up after the run"), FDreamEditorModule::Get().GetSupervisor().IsRunWindowOpen());
			TestFalse(TEXT("sanity: no pre-processor is installed after the run"), FDreamEditorModule::Get().GetSupervisor().GetInputPreProcessorForTest().IsValid());
			TestEqual(TEXT("every line is in the run's output"), Run->OfType(dream::LineType::Stdout).Num(), 8);
			return true;
		});
		Steps.Finally(TEXT("restore run.pump"), [State]()
		{
			if (State->bSettingsSaved)
			{
				UDreamUserSettings::GetMutable()->bRunPump = State->bSavedRunPump;
			}
		});
		Steps.Run(Done);
	});
}
