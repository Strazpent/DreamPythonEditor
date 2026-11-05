// SPDX-License-Identifier: Apache-2.0
// WS-05 subset: run state, namespace, cursor, journal age, and the transient message.
#include "Shell/SDreamStatusStrip.h"

#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Persistence/DreamJournal.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Shell/DreamChrome.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamStatusStripLocal
{
	FText Seconds(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = Value < 10.0 ? 1 : 0;
		Options.MaximumFractionalDigits = Value < 10.0 ? 1 : 0;
		return FText::AsNumber(Value, &Options);
	}
}

void SDreamStatusStrip::Construct(const FArguments& InArgs)
{
	const TAttribute<FText> Message = InArgs._Message;
	auto Separator = []()
	{
		return SNew(STextBlock).Font(FDreamChrome::Font(9)).Text(FText::FromString(TEXT("·"))).ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText));
	};
	const FSlateFontInfo Font = FDreamChrome::Font(9);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Border)) ]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
		SNew(SBorder)
		.BorderImage(FDreamChrome::Brush(EDreamChromeSlot::Background))
		.ForegroundColor(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
		.Padding(FMargin(8.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Font(Font).Text_Static(&SDreamStatusStrip::GetRunStateText)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f).VAlign(VAlign_Center)[ Separator() ]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Font(Font).Text(InArgs._NamespaceText)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.Padding(12.f, 0.f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(Font)
				.Text(Message)
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Accent))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Font(Font).Text(InArgs._CursorText).ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.f, 0.f).VAlign(VAlign_Center)[ Separator() ]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(Font)
				.Text_Static(&SDreamStatusStrip::GetJournalText)
				.ColorAndOpacity_Static(&SDreamStatusStrip::GetJournalColor)
			]
		]
		]
	];
}

FText SDreamStatusStrip::GetRunStateText()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return FText::GetEmpty();
	}
	const FDreamRunService& Runs = FDreamEditorModule::Get().GetRunService();
	const EDreamRunState State = Runs.GetState();
	const FText Name = DreamRunText::StateName(State);
	const FDreamRunRecord* Active = Runs.GetActiveRecord();
	if (State == EDreamRunState::Running && Active)
	{
		const double Elapsed = FPlatformTime::Seconds() - (Active->EnterSeconds > 0.0 ? Active->EnterSeconds : Active->StartSeconds);
		return FText::Format(LOCTEXT("RunningFmt", "{0} {1} s · Run #{2}"), Name, DreamStatusStripLocal::Seconds(Elapsed),
			FText::AsNumber(Active->RunNumber));
	}
	if (State != EDreamRunState::Idle && Active)
	{
		// (run group, B32) §3.9: after 1 s in Starting without an idle end of frame, "Starting… waiting for the editor
		// to become idle".
		const FText Shown = State == EDreamRunState::Starting ? DreamRunText::StartingStateText(*Active, FPlatformTime::Seconds()) : Name;
		return FText::Format(LOCTEXT("StateRunFmt", "{0} · Run #{1}"), Shown, FText::AsNumber(Active->RunNumber));
	}
	return Name;
}

FText SDreamStatusStrip::GetJournalText()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return FText::GetEmpty();
	}
	const FDreamJournal& Journal = FDreamEditorModule::Get().GetJournal();
	switch (Journal.GetHealth())
	{
	case EDreamJournalHealth::Disabled:
		return LOCTEXT("JournalOff", "Journal off");
	case EDreamJournalHealth::ReadOnly:
		return LOCTEXT("JournalReadOnly", "Journal read-only");
	default:
		break;
	}
	const FDateTime Last = Journal.GetLastDurableUtc();
	if (Last.GetTicks() == 0)
	{
		return LOCTEXT("JournalStarting", "Journal starting");
	}
	const double Age = FMath::Max(0.0, (FDateTime::UtcNow() - Last).GetTotalSeconds());
	if (Journal.GetHealth() == EDreamJournalHealth::Degraded)
	{
		return FText::Format(LOCTEXT("JournalRetrying", "Journal retrying · saved {0} s ago"), FText::AsNumber(FMath::FloorToInt(Age)));
	}
	return FText::Format(LOCTEXT("JournalAge", "Journal saved {0} s ago"), FText::AsNumber(FMath::FloorToInt(Age)));
}

FSlateColor SDreamStatusStrip::GetJournalColor()
{
	if (FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetJournal().GetHealth() == EDreamJournalHealth::Degraded)
	{
		return FStyleColors::Warning;   // the age turns amber while commits fail (§10.2.6 Degraded)
	}
	return FDreamChrome::Color(EDreamChromeSlot::MutedText);
}

#undef LOCTEXT_NAMESPACE
