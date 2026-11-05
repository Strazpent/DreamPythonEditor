// SPDX-License-Identifier: Apache-2.0
// Go to line (NAV-03 M1, TDD §7.9.3 ':' line provider) over the code view.
#include "CodeView/SDreamGoToLine.h"

#include "Input/Events.h"
#include "Styling/AppStyle.h"
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/StyleColors.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamGoToLineLocal
{
	/** Digits only, at least one. */
	bool ParsePositive(const FString& Text, int32& Out)
	{
		if (Text.IsEmpty() || Text.Len() > 9)
		{
			return false;
		}
		for (const TCHAR Ch : Text)
		{
			if (Ch < TEXT('0') || Ch > TEXT('9'))
			{
				return false;
			}
		}
		Out = FCString::Atoi(*Text);
		return Out >= 1;
	}
}

void SDreamGoToLine::Construct(const FArguments& InArgs)
{
	OnAccept = InArgs._OnAccept;
	OnCancel = InArgs._OnCancel;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme)
		.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
		.Padding(FMargin(8.f, 6.f))
		[
			SNew(SBox)
			.WidthOverride(360.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 0.f, 0.f, 4.f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CodeView.GoToLineTitle", "Go to Line"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(Box, SEditableTextBox)
					.Style(&FDreamThemes::TextBoxStyle())   // (theme)
					.HintText_Lambda([this]() { return Hint; })
					.SelectAllTextWhenFocused(true)
					.ClearKeyboardFocusOnCommit(false)
					.OnKeyDownHandler(this, &SDreamGoToLine::HandleBoxKeyDown)
					.OnTextChanged_Lambda([this](const FText&) { Error = FText::GetEmpty(); })
					.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type Commit)
					{
						// A click elsewhere ends it like Esc; Enter is handled as a key (HandleBoxKeyDown).
						if (Commit == ETextCommit::OnUserMovedFocus)
						{
							OnCancel.ExecuteIfBound();
						}
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.f, 4.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return Error; })
					.ColorAndOpacity(FDreamThemes::ColorAttr(EDreamThemeSlot::OutError))   // (theme)
					.Visibility_Lambda([this]() { return Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
			]
		]
	];
}

void SDreamGoToLine::Reset(int32 CaretLine, int32 CaretColumn, int32 InLineCount)
{
	LineCount = FMath::Max(1, InLineCount);
	Error = FText::GetEmpty();
	Hint = FText::Format(LOCTEXT("CodeView.GoToLineHint", "Line 1-{0}, or line:column (now {1}:{2})"),
		FText::AsNumber(LineCount, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::AsNumber(CaretLine + 1, &FNumberFormattingOptions::DefaultNoGrouping()),
		FText::AsNumber(CaretColumn + 1, &FNumberFormattingOptions::DefaultNoGrouping()));
	if (Box.IsValid())
	{
		Box->SetText(FText::GetEmpty());
	}
}

TSharedRef<SWidget> SDreamGoToLine::GetFocusTarget() const
{
	return Box.ToSharedRef();
}

bool SDreamGoToLine::Parse(const FString& Input, int32 InLineCount, int32& OutLine, int32& OutColumn, FText& OutError)
{
	FString Text = Input.TrimStartAndEnd();
	if (Text.StartsWith(TEXT(":")))
	{
		Text.RightChopInline(1);
		Text.TrimStartInline();
	}
	if (Text.IsEmpty())
	{
		OutError = LOCTEXT("CodeView.GoToLineEmpty", "Type a line number");
		return false;
	}
	FString LinePart = Text;
	FString ColumnPart;
	const bool bHasColumn = Text.Split(TEXT(":"), &LinePart, &ColumnPart);
	LinePart.TrimStartAndEndInline();
	ColumnPart.TrimStartAndEndInline();
	int32 Line = 0;
	int32 Column = 0;
	if (!DreamGoToLineLocal::ParsePositive(LinePart, Line))
	{
		OutError = FText::Format(LOCTEXT("CodeView.GoToLineBad", "\"{0}\" is not a line number"), FText::FromString(LinePart));
		return false;
	}
	if (bHasColumn && !DreamGoToLineLocal::ParsePositive(ColumnPart, Column))
	{
		OutError = FText::Format(LOCTEXT("CodeView.GoToColumnBad", "\"{0}\" is not a column number"), FText::FromString(ColumnPart));
		return false;
	}
	OutLine = FMath::Min(Line, FMath::Max(1, InLineCount)) - 1;
	OutColumn = bHasColumn ? Column - 1 : INDEX_NONE;
	return true;
}

void SDreamGoToLine::Accept()
{
	int32 Line = 0;
	int32 Column = INDEX_NONE;
	FText Reason;
	if (!Parse(Box.IsValid() ? Box->GetText().ToString() : FString(), LineCount, Line, Column, Reason))
	{
		Error = Reason;
		return;
	}
	OnAccept.ExecuteIfBound(Line, Column);
}

FReply SDreamGoToLine::HandleBoxKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	if (Mods.IsControlDown() || Mods.IsAltDown())
	{
		return FReply::Unhandled();
	}
	if (KeyEvent.GetKey() == EKeys::Enter)
	{
		Accept();
		return FReply::Handled();
	}
	if (KeyEvent.GetKey() == EKeys::Escape)
	{
		OnCancel.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
