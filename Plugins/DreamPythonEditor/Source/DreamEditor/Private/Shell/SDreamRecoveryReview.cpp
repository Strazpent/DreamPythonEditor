// SPDX-License-Identifier: Apache-2.0
// The Recovery review panel and the pending bar (TDD §10.7.1-§10.7.3). Never modal, never focusable buttons (U26).
#include "Shell/SDreamRecoveryReview.h"

#include "Module/DreamText.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "dream/Diff.h"
#include "dream/TextBuffer.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRecoveryUiLocal
{
	dream::TextSnapshot ModelSnapshot(const FString& Text)
	{
		return dream::TextBuffer(DreamText::ToU16(DreamText::ToModelText(Text))).Snapshot();
	}

	TSharedRef<SWidget> MakeButton(const FDreamRecoveryAction& Action)
	{
		const FSimpleDelegate OnClick = Action.OnClick;
		return SNew(SButton)
			.Text(Action.Label)
			.ToolTipText(Action.Tooltip)
			.IsEnabled(Action.bEnabled)
			.IsFocusable(false)
			.OnClicked_Lambda([OnClick]() { OnClick.ExecuteIfBound(); return FReply::Handled(); });
	}
}

FString DreamRecoveryUi::UnifiedDiff(const FString& Old, const FString& New, const FString& OldLabel, const FString& NewLabel)
{
	const dream::TextSnapshot OldSnap = DreamRecoveryUiLocal::ModelSnapshot(Old);
	const dream::TextSnapshot NewSnap = DreamRecoveryUiLocal::ModelSnapshot(New);
	const dream::DiffResult Result = dream::Diff::Lines(OldSnap, NewSnap);
	dream::UnifiedOptions Options;
	Options.OldLabel = std::u16string(DreamText::ToU16(OldLabel));
	Options.NewLabel = std::u16string(DreamText::ToU16(NewLabel));
	return DreamText::ToFString(dream::Diff::FormatUnified(OldSnap, NewSnap, Result, Options));
}

void DreamRecoveryUi::CountLines(const FString& Old, const FString& New, int32& OutAdded, int32& OutRemoved)
{
	const dream::DiffResult Result = dream::Diff::Lines(DreamRecoveryUiLocal::ModelSnapshot(Old), DreamRecoveryUiLocal::ModelSnapshot(New));
	OutAdded = Result.AddedLines();
	OutRemoved = Result.RemovedLines();
}

TSharedRef<SWidget> DreamRecoveryUi::MakeDiffView(const FString& DiffText, float MaxHeight)
{
	return SNew(SBox)
		.MaxDesiredHeight(MaxHeight)
		[
			SNew(SMultiLineEditableTextBox)
			.Style(&FDreamThemes::TextBoxStyle())   // (theme)
			.IsReadOnly(true)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.Text(DiffText.IsEmpty() ? LOCTEXT("Recovery.NoDifference", "The texts are the same.") : FText::FromString(DiffText))
			.AutoWrapText(false)
		];
}

void SDreamRecoveryActionBar::Construct(const FArguments& InArgs)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(InArgs._Message)
			.AutoWrapText(true)
		];
	for (const FDreamRecoveryAction& Action : InArgs._Actions)
	{
		Row->AddSlot().AutoWidth().Padding(4.f, 0.f).VAlign(VAlign_Center)[ DreamRecoveryUiLocal::MakeButton(Action) ];
	}
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[ Row ];
	if (InArgs._DiffText.IsSet())
	{
		Body->AddSlot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)[ DreamRecoveryUi::MakeDiffView(InArgs._DiffText.GetValue()) ];
	}
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::OutWarning))   // (theme) PO-04: the theme's colours
		.Padding(FMargin(1.f))
		[
			SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme)
			.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
			.Padding(FMargin(8.f, 3.f))
			[
				Body
			]
		]
	];
}

namespace DreamRecoveryUiLocal
{
	FText RowDetail(const FDreamRecoveryRow& Row)
	{
		const FText Changes = FText::Format(LOCTEXT("Recovery.RowChanges", "+{0} −{1} lines"),
			FText::AsNumber(Row.LinesAdded), FText::AsNumber(Row.LinesRemoved));
		const FText When = Row.LastEditUtc.GetTicks() > 0
			? FText::Format(LOCTEXT("Recovery.RowWhen", "last edit {0}"), FText::AsDateTime(Row.LastEditUtc, EDateTimeStyle::Short, EDateTimeStyle::Short))
			: FText::GetEmpty();
		return FText::Format(LOCTEXT("Recovery.RowDetail", "{0} · {1}{2}{3}"), Row.Where, Changes,
			Row.bConflict ? LOCTEXT("Recovery.RowConflict", " · changed on disk after the journal's base version") : FText::GetEmpty(),
			When.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("Recovery.RowWhenSep", " · {0}"), When));
	}

	TSharedRef<SHorizontalBox> RowLine(const FDreamRecoveryRow& Row, const TArray<FDreamRecoveryAction>& Actions)
	{
		TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock).Text(Row.Name)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(RowDetail(Row))
			];
		for (const FDreamRecoveryAction& Action : Actions)
		{
			Line->AddSlot().AutoWidth().Padding(4.f, 0.f).VAlign(VAlign_Center)[ MakeButton(Action) ];
		}
		return Line;
	}
}

void SDreamRecoveryReview::Construct(const FArguments& InArgs)
{
	Rows = InArgs._Rows;
	Sections = InArgs._Sections;
	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	for (const FDreamRecoveryRow& Row : Rows)
	{
		const FGuid Id = Row.Id;
		auto Bind = [Id](const FDreamOnRecoveryRow& Event)
		{
			return FSimpleDelegate::CreateLambda([Event, Id]() { Event.ExecuteIfBound(Id); });
		};
		const bool bComparing = InArgs._CompareFor == Id;
		TArray<FDreamRecoveryAction> Actions;
		Actions.Add({ LOCTEXT("Recovery.RowRestore", "Restore"),
			LOCTEXT("Recovery.RowRestoreTip", "The buffer takes the recovered text as one undoable edit; nothing is written to disk"),
			Bind(InArgs._OnRestore) });
		Actions.Add({ LOCTEXT("Recovery.RowRestoreNew", "Restore as new tab"),
			LOCTEXT("Recovery.RowRestoreNewTip", "Opens the recovered text in a new scratch tab and leaves this buffer as it is"),
			Bind(InArgs._OnRestoreAsNewTab) });
		Actions.Add({ bComparing ? LOCTEXT("Recovery.RowCloseDiff", "Close diff") : LOCTEXT("Recovery.RowCompare", "Compare"),
			LOCTEXT("Recovery.RowCompareTip", "A read-only unified diff of the text shown now against the recovered text"),
			Bind(InArgs._OnCompare) });
		Actions.Add({ LOCTEXT("Recovery.RowDiscard", "Discard"),
			LOCTEXT("Recovery.RowDiscardTip", "Keeps the text shown now; the recovered text moves to local history"),
			Bind(InArgs._OnDiscard) });

		List->AddSlot().AutoHeight().Padding(0.f, 2.f)[ DreamRecoveryUiLocal::RowLine(Row, Actions) ];
		if (bComparing && InArgs._CompareSection.IsEmpty())
		{
			List->AddSlot().AutoHeight().Padding(0.f, 2.f, 0.f, 6.f)[ DreamRecoveryUi::MakeDiffView(InArgs._CompareText) ];
		}
	}
	// (instances) One section per other namespace, after this session's buffers; nothing here is part of Restore all.
	for (const FDreamRecoverySection& Section : Sections)
	{
		List->AddSlot().AutoHeight().Padding(0.f, 8.f, 0.f, 2.f)
		[
			SNew(STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.Text(Section.Title)
			.AutoWrapText(true)
		];
		for (const FDreamRecoveryRow& Row : Section.Rows)
		{
			const FString Key = Section.Key;
			const FGuid Id = Row.Id;
			const FDreamOnRecoverySectionRow Event = InArgs._OnSectionAction;
			auto Bind = [Event, Key, Id](EDreamRecoveryRowAction Action)
			{
				return FSimpleDelegate::CreateLambda([Event, Key, Id, Action]() { Event.ExecuteIfBound(Key, Id, Action); });
			};
			const bool bComparing = InArgs._CompareFor == Id && InArgs._CompareSection == Key;
			TArray<FDreamRecoveryAction> Actions;
			if (Section.bReadOnly)
			{
				Actions.Add({ LOCTEXT("Recovery.RowOpenCopy", "Open a copy"),
					LOCTEXT("Recovery.RowOpenCopyTip", "A new scratch buffer with this text; the other editor's tab is not changed"),
					Bind(EDreamRecoveryRowAction::OpenCopy) });
			}
			else
			{
				Actions.Add({ LOCTEXT("Recovery.SectionRestore", "Restore"),
					LOCTEXT("Recovery.SectionRestoreTip", "Opens it in this session: a file takes the recovered text as one undoable edit; nothing is written to disk"),
					Bind(EDreamRecoveryRowAction::Restore) });
				Actions.Add({ LOCTEXT("Recovery.SectionRestoreNew", "Restore as new tab"),
					LOCTEXT("Recovery.SectionRestoreNewTip", "Opens the recovered text in a new scratch tab"),
					Bind(EDreamRecoveryRowAction::RestoreAsNewTab) });
				Actions.Add({ bComparing ? LOCTEXT("Recovery.SectionCloseDiff", "Close diff") : LOCTEXT("Recovery.SectionCompare", "Compare"),
					LOCTEXT("Recovery.SectionCompareTip", "A read-only unified diff of the file on disk (or nothing, for an untitled buffer) against the recovered text"),
					Bind(EDreamRecoveryRowAction::Compare) });
				Actions.Add({ LOCTEXT("Recovery.SectionDiscard", "Discard"),
					LOCTEXT("Recovery.SectionDiscardTip", "The recovered text moves to local history"),
					Bind(EDreamRecoveryRowAction::Discard) });
			}
			List->AddSlot().AutoHeight().Padding(8.f, 2.f, 0.f, 2.f)[ DreamRecoveryUiLocal::RowLine(Row, Actions) ];
			if (bComparing)
			{
				List->AddSlot().AutoHeight().Padding(8.f, 2.f, 0.f, 6.f)[ DreamRecoveryUi::MakeDiffView(InArgs._CompareText) ];
			}
		}
	}
	if (Rows.Num() == 0 && Sections.Num() == 0)
	{
		List->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Recovery.NothingPending", "Nothing is waiting to be restored."))
		];
	}

	const FSimpleDelegate OnClose = InArgs._OnClose;
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::Accent))   // (theme) PO-04: the theme's colours
		.Padding(FMargin(1.f))
		[
			SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme)
			.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
			.Padding(FMargin(8.f, 4.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.Text(InArgs._Title)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(&FDreamThemes::FlatButtonStyle())   // (theme)
						.IsFocusable(false)
						.ToolTipText(LOCTEXT("Recovery.CloseReview", "Close the review panel; nothing is resolved"))
						.OnClicked_Lambda([OnClose]() { OnClose.ExecuteIfBound(); return FReply::Handled(); })
						[
							SNew(STextBlock).Text(LOCTEXT("Recovery.CloseGlyph", "×"))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SBox)
					.MaxDesiredHeight(360.f)
					[
						SNew(SScrollBox)
						.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())   // (theme)
						+ SScrollBox::Slot()
						[
							List
						]
					]
				]
			]
		]
	];
}

#undef LOCTEXT_NAMESPACE
