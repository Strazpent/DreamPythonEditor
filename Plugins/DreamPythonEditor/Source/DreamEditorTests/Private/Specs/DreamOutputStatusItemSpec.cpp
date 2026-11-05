// SPDX-License-Identifier: Apache-2.0
// Dream.Output.StatusItem: the WS-07 status-bar item's rows of T-UEI-002 that Dream.Output.Follow does not cover (TDD
// §11.6.2, U14 as amended by RV-16; STATUS "Open after the review rounds"; slice B item B93):
//   1. Running: inside a pump of a supervised run the item reads "Dream · Running <s> s · Run #N"; its
//      [Stop] path (FDreamRunService::RequestStopActive from the status bar) ends the run cancelled; then the item reads
//      "Dream · Run #N cancelled", and a click opens the tab's Output view at run N's block (its header selected).
//   2. After an ok run it reads "Dream · Idle · last run ok <s> s"; after a not executed run, "Dream · Run #N not executed",
//      and a click opens run N's block.
//   3. The item's tooltip is "Dream Python Editor: click to open".
//   4. (output, PO-04 r6 M1) With run headers off (the default) the click on an error selects the run's traceback, and
//      on a run that failed with no error line of its own (SystemExit(3)) its one outcome line.
// The item is built here (SNew) and clicked through its button (SButton::SimulateClick), the path a real click takes.
// Error opens the traceback: Dream.Output.Follow (ScrollToRunError). Task-mode Stop (M3) and Live (docked) Stop (M4) are
// later milestones.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/DreamOutputRows.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamStatusItem.h"
#include "Shell/SDreamTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SToolTip.h"

namespace DreamOutputStatusItemSpec
{
	/** The item's buttons in order: the state button (click to open), then [Stop]. */
	void FindButtons(const TSharedRef<SWidget>& Root, TArray<TSharedRef<SButton>>& Out)
	{
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (Child->GetType() == FName(TEXT("SButton")))
			{
				Out.Add(StaticCastSharedRef<SButton>(Child));
				continue;
			}
			FindButtons(Child, Out);
		}
	}

	/** U14's "Dream · Run #N <outcome>" for a run that did not end ok. */
	FString OutcomeText(int32 RunNumber, EDreamRunOutcome Outcome)
	{
		return FString::Printf(TEXT("Dream · Run #%s %s"), *FText::AsNumber(RunNumber).ToString(), *DreamRunText::OutcomeName(Outcome).ToString());
	}

	struct FState
	{
		int32 RunNumber = 0;
		FDelegateHandle PreTickHandle;
		bool bStopIssued = false;
		FString RunningText;
		TSharedPtr<SDreamStatusItem> Item;
		bool bSettingsSaved = false;
		bool bSavedRunPump = true;
		EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
	};

	/** Slate pre-tick inside a pump of our run (GT): read the item, then press its Stop path once. */
	void OnPreTick(FState& State, FRunSupervisor& Supervisor, FDreamRunService& RunService)
	{
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		if (State.bStopIssued || Supervisor.GetPumpDepth() <= 0 || Active == nullptr || Active->RunNumber != State.RunNumber
			|| FPlatformTime::Seconds() - Active->StartSeconds < 0.5)
		{
			return;
		}
		State.RunningText = SDreamStatusItem::GetStatusText().ToString();
		RunService.RequestStopActive(EDreamStopSource::StatusBarButton);
		State.bStopIssued = true;
	}
}

BEGIN_DEFINE_SPEC(FDreamOutputStatusItemSpec, "Dream.Output.StatusItem", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	bool bSavedRunHeaders = false;
	/** Clicks the item's state button; false when the item has none. */
	bool ClickItem(const TSharedRef<SDreamStatusItem>& Item);
	/** The run of the Output view's selected header row, after the click switched the area to Output. */
	int32 SelectedHeaderRun();
	/** Clears the Output view's selection, so a header selected afterwards was selected by the click. */
	void ClearSelection();
END_DEFINE_SPEC(FDreamOutputStatusItemSpec)

bool FDreamOutputStatusItemSpec::ClickItem(const TSharedRef<SDreamStatusItem>& Item)
{
	TArray<TSharedRef<SButton>> Buttons;
	DreamOutputStatusItemSpec::FindButtons(Item, Buttons);
	if (!TestTrue(TEXT("the item has its state button"), Buttons.Num() >= 1))
	{
		return false;
	}
	Buttons[0]->SimulateClick();
	return true;
}

int32 FDreamOutputStatusItemSpec::SelectedHeaderRun()
{
	const TSharedPtr<SDreamOutputArea> Area = Tab.IsValid() ? Tab->GetOutputArea() : nullptr;
	const TSharedPtr<SDreamOutputView> View = Area.IsValid() ? Area->GetOutputView() : nullptr;
	return View.IsValid() ? View->GetSelectedHeaderRunForTest() : 0;
}

void FDreamOutputStatusItemSpec::ClearSelection()
{
	const TSharedPtr<SDreamOutputArea> Area = Tab.IsValid() ? Tab->GetOutputArea() : nullptr;
	if (const TSharedPtr<SDreamOutputView> View = Area.IsValid() ? Area->GetOutputView() : nullptr)
	{
		View->ResetSelectionForTest();
	}
	if (Area.IsValid())
	{
		Area->SetActiveTab(EDreamOutputTab::Repl);     // the click must switch back to Output itself
	}
}

void FDreamOutputStatusItemSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		// (output, PO-04) The click selects the run's header row, which exists only with View ▸ Run headers on (off by
		// default; Dream.Output.MayaStyle covers the click with headers off). Restored in AfterEach.
		bSavedRunHeaders = UDreamUserSettings::Get()->bShowRunHeaders;
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, true);
	});

	AfterEach([this]()
	{
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, bSavedRunHeaders);
		if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ScrollToEnd();
			}
		}
		Tab.Reset();
	});

	LatentIt("reads Running in a pump, then cancelled, and the click opens the run block (T-UEI-002)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputStatusItemSpec;
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		FDreamRunService* RunService = &Module.GetRunService();
		TSharedRef<FState> State = MakeShared<FState>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		const FString Sfx = DreamTest::UniqueSuffix();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("build the item, arm the pre-tick probe and start a 6 s run"), [this, State, Supervisor, RunService, Run, Sfx]()
		{
			// A supervised presentation, so the run pumps and Slate ticks inside it (§5.5.3).
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSavedRunPump = Settings->bRunPump;
			State->SavedPresentation = Settings->RunPresentation;
			State->bSettingsSaved = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;

			State->Item = SNew(SDreamStatusItem);
			State->PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([State, Supervisor, RunService](float)
			{
				DreamOutputStatusItemSpec::OnPreTick(*State, *Supervisor, *RunService);
			});
			Run->SubmitSeconds = FPlatformTime::Seconds();
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(FString::Printf(
				TEXT("import time\nfor _st_%s in range(600):\n    time.sleep(0.01)"), *Sfx)));
			State->RunNumber = Run->RunNumber;
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the run"), Run, 60.0);
		Steps.Do(TEXT("check Running, the Stop path and the cancelled text, then click"), [this, State, Run]()
		{
			if (!TestTrue(TEXT("the pre-tick probe ran inside a pump of the run"), State->bStopIssued))
			{
				return false;
			}
			const FString RunTag = FString::Printf(TEXT("Run #%s"), *FText::AsNumber(Run->RunNumber).ToString());
			const FString RunningPrefix = FString::Printf(TEXT("Dream · %s "), *DreamRunText::StateName(EDreamRunState::Running).ToString());
			TestTrue(*FString::Printf(TEXT("the running text is \"Dream · Running <s> s · Run #N\": \"%s\""), *State->RunningText),
				State->RunningText.StartsWith(RunningPrefix) && State->RunningText.Contains(TEXT(" s · ") + RunTag));
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Cancelled))
			{
				return false;
			}
			TestEqual(TEXT("the cancelled text (U14)"), SDreamStatusItem::GetStatusText().ToString(),
				OutcomeText(Run->RunNumber, EDreamRunOutcome::Cancelled));

			ClearSelection();
			if (!ClickItem(State->Item.ToSharedRef()))
			{
				return false;
			}
			TestTrue(TEXT("the click shows the Output view"), Tab->GetOutputArea()->GetActiveTab() == EDreamOutputTab::Output);
			TestEqual(TEXT("the click opens the cancelled run's block (its header selected, RV-16)"), SelectedHeaderRun(), Run->RunNumber);
			return true;
		});
		Steps.Finally(TEXT("disarm and restore the settings"), [State]()
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
			State->Item.Reset();
		});
		Steps.Run(Done);
	});

	LatentIt("reads the idle and not executed texts, and the click opens the not executed block (T-UEI-002)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputStatusItemSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Incomplete = FString::Printf(TEXT("ne_%s = (1,"), *Sfx);
		// The compile error block reaches LogPython at Error (§6.3, A46): the line under test, not a failure.
		DreamTest::ExpectPythonErrorBlock(*this, FString(), TEXT("was never closed"), { Incomplete, TEXT("<dream:REPL#") });
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Ok = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Broken = MakeShared<DreamTest::FRunResult>();
		TSharedRef<TSharedPtr<SDreamStatusItem>> Item = MakeShared<TSharedPtr<SDreamStatusItem>>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("an ok run"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("ok_%s = 1"), *Sfx));
		}, Ok);
		Steps.Do(TEXT("check the idle text and the tooltip"), [this, Ok, Item]()
		{
			DreamTest::ExpectOutcome(*this, *Ok, EDreamRunOutcome::Ok);
			const FString IdlePrefix = FString::Printf(TEXT("Dream · %s · last run %s "), *DreamRunText::StateName(EDreamRunState::Idle).ToString(),
				*DreamRunText::OutcomeName(EDreamRunOutcome::Ok).ToString());
			const FString Text = SDreamStatusItem::GetStatusText().ToString();
			TestTrue(*FString::Printf(TEXT("the idle text after an ok run is \"Dream · Idle · last run ok <s> s\": \"%s\""), *Text),
				Text.StartsWith(IdlePrefix) && Text.EndsWith(TEXT(" s")));
			*Item = SNew(SDreamStatusItem);
			TArray<TSharedRef<SButton>> Buttons;
			DreamOutputStatusItemSpec::FindButtons(Item->ToSharedRef(), Buttons);
			const TSharedPtr<IToolTip> Tip = Buttons.Num() > 0 ? Buttons[0]->GetToolTip() : nullptr;
			// A text tooltip is a deferred one (FSlateApplication::MakeToolTip) whose widget is an SToolTip made on demand.
			const TSharedPtr<SWidget> TipWidget = Tip.IsValid() ? TSharedPtr<SWidget>(Tip->AsWidget()) : nullptr;
			if (TestTrue(TEXT("the state button has a text tooltip"), TipWidget.IsValid() && TipWidget->GetType() == FName(TEXT("SToolTip"))))
			{
				TestEqual(TEXT("the tooltip (TDD 11.6.2)"), StaticCastSharedPtr<SToolTip>(TipWidget)->GetTextTooltip().ToString(),
					FString(TEXT("Dream Python Editor: click to open")));
			}
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("a not executed run"), [Incomplete]()
		{
			return DreamTest::MakeReplRequest(Incomplete);
		}, Broken);
		Steps.Do(TEXT("check the not executed text, then click"), [this, Broken, Item]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Broken, EDreamRunOutcome::NotExecuted))
			{
				return false;
			}
			TestEqual(TEXT("the not executed text (U14)"), SDreamStatusItem::GetStatusText().ToString(),
				OutcomeText(Broken->RunNumber, EDreamRunOutcome::NotExecuted));
			ClearSelection();
			if (!ClickItem(Item->ToSharedRef()))
			{
				return false;
			}
			TestTrue(TEXT("the click shows the Output view"), Tab->GetOutputArea()->GetActiveTab() == EDreamOutputTab::Output);
			TestEqual(TEXT("the click opens the not executed run's block (its header selected, RV-16)"), SelectedHeaderRun(), Broken->RunNumber);
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, TEXT("was never closed"));
		Steps.Finally(TEXT("drop the item"), [Item]()
		{
			Item->Reset();
		});
		Steps.Run(Done);
	});

	LatentIt("with run headers off, the click selects the traceback, or the outcome line of a run with no error line (PO-04, r6 M1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::RunHeaders, false);      // PO-04's default; AfterEach restores
		const FString Sfx = DreamTest::UniqueSuffix();
		// The ZeroDivisionError block reaches LogPython at Error (§6.3) and each failed run one LogDream summary at Warning (A44).
		DreamTest::ExpectPythonErrorBlock(*this, FString(), TEXT("ZeroDivisionError: division by zero"), { TEXT("<dream:REPL#"), TEXT("1/0") });
		AddExpectedMessage(TEXT("^(LogDream: )?Run #\\d+ error .*$"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Failing = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Exited = MakeShared<DreamTest::FRunResult>();
		TSharedRef<TSharedPtr<SDreamStatusItem>> Item = MakeShared<TSharedPtr<SDreamStatusItem>>();
		const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
		auto SelectedText = [Area]() -> FString
		{
			const TSharedPtr<SDreamOutputView> View = Area.IsValid() ? Area->GetOutputView() : nullptr;
			return View.IsValid() ? View->GetSelectedText() : FString();
		};
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a run that raises"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("st_%s = 1\n1/0"), *Sfx));
		}, Failing);
		Steps.Do(TEXT("click: the traceback is selected"), [this, Failing, Item, SelectedText]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Failing, EDreamRunOutcome::Error))
			{
				return false;
			}
			*Item = SNew(SDreamStatusItem);
			ClearSelection();
			if (!ClickItem(Item->ToSharedRef()))
			{
				return false;
			}
			const FString Selected = SelectedText();
			TestTrue(*FString::Printf(TEXT("the click selects the run's traceback with headers off: \"%s\""), *Selected),
				Selected.StartsWith(TEXT("Traceback (most recent call last):")));
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, TEXT("ZeroDivisionError: division by zero"));
		DreamTest::AddRun(Steps, TEXT("a run that exits with code 3 and writes no line"), []()
		{
			return DreamTest::MakeReplRequest(TEXT("raise SystemExit(3)"));
		}, Exited);
		Steps.Do(TEXT("click: the run's outcome line is selected"), [this, Exited, Item, SelectedText]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Exited, EDreamRunOutcome::Error))
			{
				return false;
			}
			ClearSelection();
			if (!ClickItem(Item->ToSharedRef()))
			{
				return false;
			}
			const FDreamRunRecord* Record = FDreamEditorModule::Get().GetRunService().FindRecord(Exited->RunNumber);
			TestEqual(TEXT("ScrollToRunError finds no error row, so the click selects the outcome line (its raw text)"), SelectedText(),
				DreamOutputRows::RunOutcomeText(Record, static_cast<uint32>(Exited->RunNumber)));
			return true;
		});
		Steps.Finally(TEXT("drop the item"), [Item]()
		{
			Item->Reset();
		});
		Steps.Run(Done);
	});
}
