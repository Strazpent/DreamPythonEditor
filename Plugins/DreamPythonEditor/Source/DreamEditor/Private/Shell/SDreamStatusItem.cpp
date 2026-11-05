// SPDX-License-Identifier: Apache-2.0
// WS-07 (TDD §11.6.2, U14): the U14 texts, computed from FDreamRunService on paint.
#include "Shell/SDreamStatusItem.h"

#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamStatusItemLocal
{
	FText Seconds(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = Value < 10.0 ? 1 : 0;
		Options.MaximumFractionalDigits = Value < 10.0 ? 1 : 0;
		return FText::AsNumber(Value, &Options);
	}

	FText Progress(const FDreamRunLiveStats& Live)
	{
		if (Live.ProgressDone < 0)
		{
			return FText::GetEmpty();
		}
		return Live.ProgressTotal.IsSet()
			? FText::Format(LOCTEXT("ProgressOf", " · {0} / {1}"), FText::AsNumber(Live.ProgressDone), FText::AsNumber(Live.ProgressTotal.GetValue()))
			: FText::Format(LOCTEXT("ProgressOnly", " · {0}"), FText::AsNumber(Live.ProgressDone));
	}
}

void SDreamStatusItem::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
			.ContentPadding(FMargin(4.f, 0.f))
			.IsFocusable(false)
			.ToolTipText(LOCTEXT("StatusItemTip", "Dream Python Editor: click to open"))
			.OnClicked(this, &SDreamStatusItem::HandleClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(SImage)
					.Image(FDreamEditorStyle::Get().GetBrush(DreamStyleKeys::StatusIcon))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text_Static(&SDreamStatusItem::GetStatusText)
				]
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
			.ContentPadding(FMargin(4.f, 0.f))
			.IsFocusable(false)
			.Visibility_Lambda([]() { return SDreamStatusItem::IsStopVisible() ? EVisibility::Visible : EVisibility::Collapsed; })
			.ToolTipText(LOCTEXT("StatusStopTip", "Stop the active run"))
			.OnClicked(this, &SDreamStatusItem::HandleStopClicked)
			[
				SNew(STextBlock).Text(LOCTEXT("StatusStop", "[Stop]"))
			]
		]
	];
}

FText SDreamStatusItem::GetStatusText()
{
	using namespace DreamStatusItemLocal;
	if (!FDreamEditorModule::IsAvailable())
	{
		return LOCTEXT("DreamOnly", "Dream");
	}
	const FDreamEditorModule& Module = FDreamEditorModule::Get();
	const FDreamRunService& Runs = Module.GetRunService();
	const EDreamRunState State = Runs.GetState();
	const FDreamRunRecord* Active = Runs.GetActiveRecord();
	const FText StateName = DreamRunText::StateName(State);

	switch (State)
	{
	case EDreamRunState::Starting:
		// (run group, B32) §3.9: "Starting… waiting for the editor to become idle" after 1 s.
		return FText::Format(LOCTEXT("WsStarting", "Dream · {0} · Run #{1}"),
			Active ? DreamRunText::StartingStateText(*Active, FPlatformTime::Seconds()) : StateName, FText::AsNumber(Active ? Active->RunNumber : 0));
	case EDreamRunState::Running:
	case EDreamRunState::Paused:   // WS-07 has no visible Paused text (RV-16)
	{
		const FDreamRunLiveStats& Live = Module.GetSupervisor().GetLiveStats();
		const double Elapsed = Live.ElapsedSeconds > 0.0 ? Live.ElapsedSeconds
			: (Active ? FPlatformTime::Seconds() - Active->StartSeconds : 0.0);
		return FText::Format(LOCTEXT("WsRunning", "Dream · {0} {1} s · Run #{2}{3}"), DreamRunText::StateName(EDreamRunState::Running),
			Seconds(Elapsed), FText::AsNumber(Active ? Active->RunNumber : 0), Progress(Live));
	}
	case EDreamRunState::Stopping:
		return FText::Format(LOCTEXT("WsStopping", "Dream · {0} · Run #{1}"), StateName, FText::AsNumber(Active ? Active->RunNumber : 0));
	case EDreamRunState::TaskRunning:
		return FText::Format(LOCTEXT("WsTask", "Dream · {0}{1}"), StateName, Progress(Module.GetSupervisor().GetLiveStats()));
	case EDreamRunState::Idle:
	default:
		break;
	}

	const FDreamRunRecord* Last = Runs.GetLastFinished();
	if (Last == nullptr)
	{
		return FText::Format(LOCTEXT("WsIdleFresh", "Dream · {0}"), StateName);
	}
	if (Last->Outcome == EDreamRunOutcome::Ok)
	{
		const double Duration = FMath::Max(0.0, Last->EndSeconds - (Last->EnterSeconds > 0.0 ? Last->EnterSeconds : Last->StartSeconds));
		return FText::Format(LOCTEXT("WsIdle", "Dream · {0} · last run {1} {2} s"), StateName,
			DreamRunText::OutcomeName(EDreamRunOutcome::Ok), Seconds(Duration));
	}
	return FText::Format(LOCTEXT("WsOutcome", "Dream · Run #{0} {1}"), FText::AsNumber(Last->RunNumber), DreamRunText::OutcomeName(Last->Outcome));
}

bool SDreamStatusItem::IsStopVisible()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return false;
	}
	const EDreamRunState State = FDreamEditorModule::Get().GetRunService().GetState();
	return State == EDreamRunState::Running || State == EDreamRunState::Stopping || State == EDreamRunState::TaskRunning;
}

FReply SDreamStatusItem::HandleStopClicked()
{
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetRunService().RequestStopActive(EDreamStopSource::StatusBarButton);
	}
	return FReply::Handled();
}

FReply SDreamStatusItem::HandleClicked()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return FReply::Handled();
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	TSharedPtr<SDreamTab> Tab = Module.InvokeTab();
	const FDreamRunService& Runs = Module.GetRunService();
	const FDreamRunRecord* Last = Runs.GetState() == EDreamRunState::Idle ? Runs.GetLastFinished() : nullptr;
	if (!Tab.IsValid() || Last == nullptr)
	{
		return FReply::Handled();
	}
	if (Last->Outcome != EDreamRunOutcome::Ok)
	{
		if (TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea())
		{
			Area->SetActiveTab(EDreamOutputTab::Output);
			if (TSharedPtr<SDreamOutputView> View = Area->GetOutputView())
			{
				// U14, §11.6.2: an error opens the tab at the traceback (selected: the run's last "Traceback (most recent call
				// last):" line, else the first line of its last error block, A2-02); not executed and cancelled open the run
				// block (RV-16). The traceback's links then open the file.
				if (Last->Outcome != EDreamRunOutcome::Error || !View->ScrollToRunError(Last->RunNumber))
				{
					View->ScrollToRun(Last->RunNumber);
				}
			}
		}
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
