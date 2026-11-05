// SPDX-License-Identifier: Apache-2.0
// Dream.Supervisor.Bot: Run-window input through the OS (TDD §5.6, §5.15.3, §4.16.1 RT-05, RT-04, RV-02, LRS-11), driven by
// the input bot with SendInput while the game thread is inside the run's pump or a modal loop (§14.2). Each case
// synchronises on run-state probes, marks and latches; the bot's safety net stops a run it could not stop.
//   T-SUP-029 mouse pairing: a button down outside the Run window and up on Stop fires no Stop and leaks no click to the
//     code view under the down; the wheel over the live run view scrolls it (follow off); (run group, A216) a down on
//     Stop stops the run although the button is released outside, and that up leaks nothing either.
//   T-SUP-027 / T-KEY-017: Esc after the Run window appears stops the run (source Esc), and Shift+F5 in another pass
//     (source Shift+F5); while the window is up GetActiveTopLevelRegularWindow() never returns it (RV-02).
//   T-SUP-027: Esc and Shift+F5 queued before the first pump (_dream_test.queue_os_keys from the run's first statement)
//     are dropped with the rest of the early input, and the run continues (RT-04).
//   T-SUP-027 / T-KEY-013 case 1: a script's Yes / No / Cancel dialog open over the Run window: Esc returns Cancel to
//     the script and the run continues to ok (the dialog is the _dream_test.show_modal fixture, see DreamTestModalDialog.h).
//   T-SUP-028 / T-KEY-013 case 4: Shift+F5 during that dialog: Stop is requested, the dialog stays until the script's
//     own Esc closes it, and the run ends cancelled after the dialog returns.
//   T-SUP-027: a menu open over the Run window: the first Esc closes only the menu, the next one stops the run.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Fixtures/DreamTestLatch.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamTestPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamSupervisorBotSpec
{
	/** Waits until Count more probe publishes happened (the pump or the modal loop processed what the bot sent). */
	bool WaitPublishes(DreamTest::FDreamInputBotContext& Bot, int32 Count, const FString& What)
	{
		const uint64 Start = DreamTest::FDreamTestProbes::GetSnapshot().Serial;
		return Bot.WaitFor([Start, Count](const DreamTest::FDreamProbeSnapshot& S) { return S.Serial >= Start + static_cast<uint64>(Count); },
			10.0, What);
	}

	bool WaitRunWindow(DreamTest::FDreamInputBotContext& Bot, int32 RunNumber)
	{
		return Bot.WaitFor([RunNumber](const DreamTest::FDreamProbeSnapshot& S)
		{
			return S.ActiveRunNumber == RunNumber && S.Modal == DreamTest::EDreamModalKind::RunWindow && S.StopButton.GetSize().X >= 1.f
				&& S.LiveRunView.GetSize().X >= 1.f;
		}, 15.0, FString::Printf(TEXT("run #%d's Run window"), RunNumber));
	}

	/** Bot-side safety net: Stop through the any-thread API so a failed case never leaves the run going. */
	void SafetyStop(DreamTest::FDreamInputBotContext& Bot, const TSharedRef<FDreamRunService>& Runs, FRunSupervisor* Supervisor)
	{
		if (!Bot.HasFailed())
		{
			return;
		}
		Runs->RequestStopActive(EDreamStopSource::Api);
		const double Deadline = FPlatformTime::Seconds() + 15.0;
		while (Runs->GetActiveRunId() != 0 && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (const FDreamRunId Id = Runs->GetActiveRunId())
		{
			Supervisor->RequestInterruptCleanup(Id);
		}
	}

	using FBotScript = TFunction<void(DreamTest::FDreamInputBotContext&, int32 /*RunNumber*/)>;
	using FCheck = TFunction<void(FAutomationTestBase&, const DreamTest::FRunResult&, const DreamTest::FDreamInputBotTicket&)>;

	struct FCaseState
	{
		TSharedPtr<DreamTest::FDreamInputBotTicket> Ticket;
		int32 ActiveRegularBefore = 0;
		FDelegateHandle PreTick;
		bool bMenuPushed = false;
		TSharedPtr<IMenu> Menu;
	};
}

BEGIN_DEFINE_SPEC(FDreamSupervisorBotSpec, "Dream.Supervisor.Bot", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Sfx;
	bool bSettingsSaved = false;
	bool bSavedRunPump = true;
	EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	bool Prepare();
	/** Opens Source in a scratch tab, submits it with the bot script running alongside, then runs Check. */
	void RunCase(const FDoneDelegate& Done, const FString& Purpose, const FString& Source, DreamSupervisorBotSpec::FBotScript Script,
		DreamSupervisorBotSpec::FCheck Check, bool bPushMenu = false, TFunction<void(SDreamTab&)> BeforeSubmit = nullptr);
END_DEFINE_SPEC(FDreamSupervisorBotSpec)

bool FDreamSupervisorBotSpec::Prepare()
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
	{
		return false;
	}
	if (!TestTrue(TEXT("the _dream_test fixture module exists"), DreamTestPython::IsModuleCreated()))
	{
		return false;
	}
	UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
	bSavedRunPump = Settings->bRunPump;
	SavedPresentation = Settings->RunPresentation;
	bSettingsSaved = true;
	Settings->bRunPump = true;
	Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
	return DreamTest::RequireEditorInFront(*this);
}

void FDreamSupervisorBotSpec::RunCase(const FDoneDelegate& Done, const FString& Purpose, const FString& Source,
	DreamSupervisorBotSpec::FBotScript Script, DreamSupervisorBotSpec::FCheck Check, bool bPushMenu, TFunction<void(SDreamTab&)> BeforeSubmit)
{
	if (!Prepare())
	{
		Done.Execute();
		return;
	}
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
	TSharedRef<DreamSupervisorBotSpec::FCaseState> State = MakeShared<DreamSupervisorBotSpec::FCaseState>();
	TSharedRef<FDreamRunService> Runs = FDreamEditorModule::Get().GetRunServiceRef();
	FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();

	DreamTest::FDreamSteps Steps(*this);
	DreamTest::AddWaitIdle(Steps);
	Steps.Do(TEXT("focus the script, submit the run and start the bot"), [this, Buffer, LineCount, Run, State, Script, Runs, Supervisor, bPushMenu, BeforeSubmit]()
	{
		FSlateApplication::Get().DismissAllMenus();
		Tab->ActivateBuffer(Buffer->GetId());
		if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
		{
			View->FocusEditor();
			DreamTest::FDreamTestProbes::Track(TEXT("CodeView"), View);
		}
		if (BeforeSubmit)
		{
			BeforeSubmit(*Tab);
		}
		DreamTest::FDreamTestProbes::PublishNow();
		State->ActiveRegularBefore = DreamTest::FDreamTestProbes::GetSnapshot().RunWindowWasActiveRegular;
		if (bPushMenu)
		{
			// A menu over the Run window, opened from a Slate tick inside the pump (the GIL is released there, §5.4.4).
			State->PreTick = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor](float)
			{
				if (State->bMenuPushed || Supervisor->GetPumpDepth() <= 0 || !Supervisor->IsRunWindowOpen())
				{
					return;
				}
				const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
				if (!Modal.IsValid())
				{
					return;
				}
				FSlateRect Rect;
				if (!DreamTest::FDreamTestProbes::GetDesktopRect(Modal, Rect))
				{
					return;
				}
				State->bMenuPushed = true;
				State->Menu = FSlateApplication::Get().PushMenu(Modal->GetContent(), FWidgetPath(),
					SNew(SBorder).Padding(12.f)[SNew(STextBlock).Text(LOCTEXT("Tests.Bot.Menu", "Dream test menu"))],
					FVector2D(Rect.Left + 24.f, Rect.Top + 24.f), FPopupTransitionEffect(FPopupTransitionEffect::None), /*bFocusImmediately*/ true);
			});
		}
		Run->SubmitSeconds = FPlatformTime::Seconds();
		Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, LineCount));
		if (Run->RunNumber <= 0)
		{
			return false;
		}
		const int32 Number = Run->RunNumber;
		State->Ticket = DreamTest::FDreamInputBot::Submit([Script, Number, Runs, Supervisor](DreamTest::FDreamInputBotContext& Bot)
		{
			Script(Bot, Number);
			DreamSupervisorBotSpec::SafetyStop(Bot, Runs, Supervisor);
		});
		return true;
	});
	DreamTest::AddWaitForRun(Steps, Purpose, Run, 90.0);
	Steps.Poll(TEXT("the bot's job"), [State]()
	{
		return DreamTest::IsTicketDone(State->Ticket.ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, 30.0);
	Steps.Do(TEXT("check the case"), [this, Run, State, Check, Purpose]()
	{
		DreamTest::ReportTicket(*this, State->Ticket.ToSharedRef(), Purpose);
		DreamTest::FDreamTestProbes::PublishNow();
		TestEqual(TEXT("the Run window was never the active top-level regular window (RV-02, T-KEY-017)"),
			DreamTest::FDreamTestProbes::GetSnapshot().RunWindowWasActiveRegular, State->ActiveRegularBefore);
		Check(*this, *Run, *State->Ticket);
		return true;
	});
	Steps.Finally(TEXT("untrack, remove the menu hook"), [State]()
	{
		DreamTest::FDreamTestProbes::Untrack(TEXT("CodeView"));
		if (State->PreTick.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreTick().Remove(State->PreTick);
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().DismissAllMenus();
		}
	});
	Steps.Run(Done);
}

void FDreamSupervisorBotSpec::Define()
{
	using namespace DreamSupervisorBotSpec;

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
		DreamTest::FDreamTestLatch::ForgetPrefix(TEXT("bot_"));
		Tab.Reset();
	});

	LatentIt("pairs mouse downs and ups: an up on Stop does not stop, a press on Stop stops released outside, nothing leaks, the wheel scrolls the live view (T-SUP-029)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		const FString Latch = TEXT("bot_pair_") + Sfx;
		const FString Source = FString::Printf(TEXT("import _dream_test\nfor i in range(300): print('pairing line', i)\n_dream_test.wait('%s')"), *Latch);
		TSharedRef<int32> CaretLine = MakeShared<int32>(-1);
		TSharedRef<int32> CaretColumn = MakeShared<int32>(-1);
		TSharedRef<FString> TextBefore = MakeShared<FString>();
		// The caret and text of the script before the run: a leaked click on the code view would move the caret.
		RunCase(Done, TEXT("BotPairing"), Source, [](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (!WaitRunWindow(Bot, Number) || !Bot.RequireForeground())
			{
				return;
			}
			DreamTest::FDreamProbeSnapshot S = DreamTest::FDreamTestProbes::GetSnapshot();
			const FSlateRect* Code = S.Tracked.Find(TEXT("CodeView"));
			void* const* CodeHwnd = S.TrackedHwnd.Find(TEXT("CodeView"));
			if (Code == nullptr || CodeHwnd == nullptr)
			{
				Bot.Fail(TEXT("the code view has no published geometry (is the Dream tab shown?)"));
				return;
			}
			// The code view's lower part, away from the Run window over the output area; a point there that no other
			// application's window covers (an always-on-top overlay would take the click instead).
			FVector2D Outside(Code->GetCenter().X, Code->Bottom - FMath::Min(40.f, Code->GetSize().Y * 0.25f));
			const FSlateRect Lower(Code->Left + 8.f, Code->Bottom - FMath::Min(120.f, Code->GetSize().Y * 0.4f), Code->Right - 8.f, Code->Bottom - 4.f);
			if (!DreamTest::FDreamInputBotContext::FindUncoveredPoint(Lower, *CodeHwnd, Outside))
			{
				Bot.Fail(TEXT("every point of the code view's lower part is covered by another window"));
				return;
			}
			// 1: down outside, up on Stop.
			if (!Bot.MoveToPoint(Outside, *CodeHwnd, TEXT("the code view")) || Bot.LeftDown() <= 0.0
				|| !Bot.MoveTo(S.StopButton, S.RunWindowHwnd, TEXT("Stop")) || Bot.LeftUp() <= 0.0
				|| !WaitPublishes(Bot, 6, TEXT("the pump to process the first pair")))
			{
				return;
			}
			if (DreamTest::FDreamTestProbes::GetSnapshot().bStopRequested)
			{
				Bot.Fail(TEXT("a down outside the Run window and an up on Stop requested Stop (T-SUP-029)"));
				return;
			}
			// 2: the wheel over the live run view scrolls it and leaves follow mode (PO-03, A59).
			S = DreamTest::FDreamTestProbes::GetSnapshot();
			if (!Bot.MoveTo(S.LiveRunView, S.RunWindowHwnd, TEXT("the live run view")) || Bot.Wheel(3) <= 0.0)
			{
				return;
			}
			if (!Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& P) { return !P.bLiveViewFollowing; }, 5.0,
				TEXT("the wheel to scroll the live run view out of follow mode")))
			{
				return;
			}
			// 3 (run group, A216): down on Stop, up outside. Stop acts on the press, so the run stops although the button is
			// released over the code view; the up leaks no click there (checked below, after the run). The wait covers the
			// press only (Stop requested or the run over); the up is not waited for.   (tests, review r10 nit)
			S = DreamTest::FDreamTestProbes::GetSnapshot();
			if (Bot.MoveTo(S.StopButton, S.RunWindowHwnd, TEXT("Stop")) && Bot.LeftDown() > 0.0
				&& Bot.MoveToPoint(Outside, *CodeHwnd, TEXT("the code view")) && Bot.LeftUp() > 0.0)
			{
				Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& P) { return P.bStopRequested || P.ActiveRunNumber == 0; }, 5.0,
					TEXT("the press on Stop to request Stop"));
			}
		}, [this, CaretLine, CaretColumn, TextBefore](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				Test.TestTrue(TEXT("the Stop came from the Run window's button"), Run.Record.StopSource == EDreamStopSource::RunWindowButton);
			}
			const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->GetActiveCodeView() : nullptr;
			int32 Line = -1;
			int32 Column = -1;
			if (View.IsValid())
			{
				View->GetCaret(Line, Column);
			}
			Test.TestTrue(*FString::Printf(TEXT("no click leaked to the code view: the caret is still %d:%d (now %d:%d)"), *CaretLine, *CaretColumn, Line, Column),
				Line == *CaretLine && Column == *CaretColumn);
			Test.TestTrue(TEXT("and its text is unchanged"), View.IsValid() && View->GetText() == *TextBefore);
		}, /*bPushMenu*/ false, [CaretLine, CaretColumn, TextBefore](SDreamTab& InTab)
		{
			if (const TSharedPtr<SDreamCodeView> View = InTab.GetActiveCodeView())
			{
				View->GetCaret(*CaretLine, *CaretColumn);
				*TextBefore = View->GetText();
			}
		});
	});

	LatentIt("stops on Esc sent through the OS after the Run window appears (T-SUP-027, T-KEY-017 Esc pass)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		RunCase(Done, TEXT("BotEsc"), TEXT("import time\nfor _ in range(1000): time.sleep(0.01)"),
			[](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (WaitRunWindow(Bot, Number) && Bot.RequireForeground())
			{
				Bot.Chord({ DreamTest::FDreamInputBot::VkEscape() });
			}
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				Test.TestTrue(TEXT("the record names Esc as the Stop source"), Run.Record.StopSource == EDreamStopSource::Esc);
			}
		});
	});

	LatentIt("stops on Shift+F5 sent through the OS with the Run window up (T-KEY-017 Shift+F5 pass, T-SUP-028)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		RunCase(Done, TEXT("BotShiftF5"), TEXT("import time\nfor _ in range(1000): time.sleep(0.01)"),
			[](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (WaitRunWindow(Bot, Number) && Bot.RequireForeground())
			{
				Bot.Chord({ DreamTest::FDreamInputBot::VkShift(), DreamTest::FDreamInputBot::VkF5() });
			}
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				Test.TestTrue(TEXT("the record names Shift+F5 as the Stop source"), Run.Record.StopSource == EDreamStopSource::ShiftF5);
			}
		});
	});

	LatentIt("drops Esc and Shift+F5 queued before the first pump and runs on (T-SUP-027, RT-04)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const FString Mark = TEXT("bot_queued_") + Sfx;
		const FString Source = FString::Printf(TEXT(
			"import _dream_test, time\n"
			"print('queued', _dream_test.queue_os_keys('esc,shift+f5'))\n"
			"for _ in range(150): time.sleep(0.01)\n"
			"_dream_test.mark('%s')"), *Mark);
		RunCase(Done, TEXT("BotQueued"), Source, [](DreamTest::FDreamInputBotContext&, int32) {},
			[Mark](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			Test.TestTrue(*FString::Printf(TEXT("QueueOsKeys saw both keys in the game thread's queue: %s"), *Run.Describe()),
				Run.HasLineContaining(dream::LineType::Stdout, TEXT("queued True")));
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
			{
				Test.TestTrue(TEXT("the run went on past the queued keys"), DreamTest::FDreamTestLatch::GetMark(Mark) > 0.0);
				Test.TestFalse(TEXT("no Stop was requested by the early keys"), Run.Record.bStopRequested);
				Test.TestTrue(TEXT("the run pumped (the keys were dispatched by its first pump)"), Run.Record.Stats.Pumps >= 1);
			}
		});
	});

	LatentIt("returns Cancel to a script's dialog on Esc and runs on (T-SUP-027, T-KEY-013 case 1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const FString Mark = TEXT("bot_dialog_") + Sfx;
		const FString Source = FString::Printf(TEXT(
			"import _dream_test, time\n"
			"for _ in range(60): time.sleep(0.01)\n"
			"_dream_test.mark('%s')\n"
			"bot_answer = _dream_test.show_modal('Dream Python Editor RT-05', 'Esc closes this dialog with Cancel')\n"
			"print('dialog', bot_answer)\n"
			"for _ in range(20): time.sleep(0.01)"), *Mark);
		RunCase(Done, TEXT("BotDialogEsc"), Source, [Mark](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (!WaitRunWindow(Bot, Number))
			{
				return;
			}
			if (!Bot.WaitFor([Mark](const DreamTest::FDreamProbeSnapshot& S)
				{
					return S.Modal == DreamTest::EDreamModalKind::Foreign && DreamTest::FDreamTestLatch::GetMark(Mark) > 0.0;
				}, 15.0, TEXT("the script's dialog")))
			{
				return;
			}
			FPlatformProcess::Sleep(0.3f);      // the shape of RT-05 case 1: the dialog has been up 300 ms
			if (Bot.RequireForeground())
			{
				Bot.Chord({ DreamTest::FDreamInputBot::VkEscape() });
				Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& S) { return S.Modal != DreamTest::EDreamModalKind::Foreign; }, 10.0,
					TEXT("Esc to close the dialog"));
			}
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Ok))
			{
				Test.TestTrue(*FString::Printf(TEXT("the dialog returned Cancel to the script: %s"), *Run.Describe()),
					Run.HasLineContaining(dream::LineType::Stdout, TEXT("dialog cancel")));
				Test.TestFalse(TEXT("Esc in the dialog did not stop the run"), Run.Record.bStopRequested);
			}
		});
	});

	LatentIt("stops on Shift+F5 during a script's dialog once the dialog returns (T-SUP-028, T-KEY-013 case 4)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		const FString Mark = TEXT("bot_dialogf5_") + Sfx;
		const FString Source = FString::Printf(TEXT(
			"import _dream_test, time\n"
			"for _ in range(60): time.sleep(0.01)\n"
			"_dream_test.mark('%s')\n"
			"bot_answer = _dream_test.show_modal('Dream Python Editor RT-05', 'Shift+F5 stops the run once this dialog closes')\n"
			"for _ in range(500): time.sleep(0.01)"), *Mark);
		RunCase(Done, TEXT("BotDialogShiftF5"), Source, [Mark](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (!WaitRunWindow(Bot, Number)
				|| !Bot.WaitFor([Mark](const DreamTest::FDreamProbeSnapshot& S)
					{
						return S.Modal == DreamTest::EDreamModalKind::Foreign && DreamTest::FDreamTestLatch::GetMark(Mark) > 0.0;
					}, 15.0, TEXT("the script's dialog")))
			{
				return;
			}
			FPlatformProcess::Sleep(0.3f);
			if (!Bot.RequireForeground() || Bot.Chord({ DreamTest::FDreamInputBot::VkShift(), DreamTest::FDreamInputBot::VkF5() }) <= 0.0)
			{
				return;
			}
			if (!Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& S) { return S.bStopRequested; }, 5.0,
				TEXT("Shift+F5 to request Stop inside the dialog's modal loop")))
			{
				return;
			}
			if (DreamTest::FDreamTestProbes::GetSnapshot().Modal != DreamTest::EDreamModalKind::Foreign)
			{
				Bot.Fail(TEXT("Shift+F5 closed the script's dialog; Dream Python Editor must leave it to the script (§5.12)"));
				return;
			}
			// The script's own Esc closes the dialog (the fixture closes itself after 20 s if this never arrives).
			if (Bot.RequireForeground())
			{
				Bot.Chord({ DreamTest::FDreamInputBot::VkEscape() });
			}
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				Test.TestTrue(TEXT("the record names Shift+F5 as the Stop source"), Run.Record.StopSource == EDreamStopSource::ShiftF5);
			}
		});
	});

	LatentIt("closes only a menu over the Run window on the first Esc and stops on the next (T-SUP-027 menu case)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		RunCase(Done, TEXT("BotMenuEsc"), TEXT("import time\nfor _ in range(1000): time.sleep(0.01)"),
			[](DreamTest::FDreamInputBotContext& Bot, int32 Number)
		{
			if (!WaitRunWindow(Bot, Number)
				|| !Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& S) { return S.bMenusVisible; }, 10.0, TEXT("the menu over the Run window"))
				|| !Bot.RequireForeground() || Bot.Chord({ DreamTest::FDreamInputBot::VkEscape() }) <= 0.0)
			{
				return;
			}
			if (!Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& S) { return !S.bMenusVisible; }, 5.0, TEXT("Esc to close the menu"))
				|| !WaitPublishes(Bot, 4, TEXT("a few pumps after the menu closed")))
			{
				return;
			}
			if (DreamTest::FDreamTestProbes::GetSnapshot().bStopRequested)
			{
				Bot.Fail(TEXT("the Esc that closed the menu also stopped the run (RT-05: a visible menu takes Esc)"));
				return;
			}
			Bot.Chord({ DreamTest::FDreamInputBot::VkEscape() });
		}, [](FAutomationTestBase& Test, const DreamTest::FRunResult& Run, const DreamTest::FDreamInputBotTicket&)
		{
			if (DreamTest::ExpectOutcome(Test, Run, EDreamRunOutcome::Cancelled))
			{
				Test.TestTrue(TEXT("the second Esc stopped the run"), Run.Record.StopSource == EDreamStopSource::Esc);
			}
		}, /*bPushMenu*/ true);
	});
}

#undef LOCTEXT_NAMESPACE
