// SPDX-License-Identifier: Apache-2.0
// The code view's find and replace bar (EDT-09 M1, TDD §4.4).
#include "CodeView/SDreamFindBar.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "Styling/AppStyle.h"
#include "Shell/DreamChrome.h"  // (theme) the tab's flat buttons and toggles
#include "Shell/DreamTheme.h"   // (theme)
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamFindBarLocal
{
	bool IsLeftAltOnly(const FModifierKeysState& Mods)
	{
		return Mods.IsLeftAltDown() && !Mods.IsRightAltDown() && !Mods.IsControlDown() && !Mods.IsCommandDown() && !Mods.IsShiftDown();
	}

	bool IsPlain(const FModifierKeysState& Mods)
	{
		return !Mods.IsControlDown() && !Mods.IsAltDown() && !Mods.IsCommandDown();
	}
}

void SDreamFindBar::Construct(const FArguments& InArgs)
{
	OnQueryChanged = InArgs._OnQueryChanged;
	OnStep = InArgs._OnStep;
	OnReplace = InArgs._OnReplace;
	OnReplaceAll = InArgs._OnReplaceAll;
	OnClose = InArgs._OnClose;
	StatusText = InArgs._StatusText;
	IsStatusError = InArgs._IsStatusError;
	CanReplace = InArgs._CanReplace;

	auto Button = [](const FText& Label, const FText& Tooltip, FOnClicked OnClicked)   // (theme) the chrome's flat buttons
	{
		return SNew(SButton)
			.ButtonStyle(&FDreamChrome::FlatButton())   // (theme)
			.IsFocusable(false)
			.ToolTipText(Tooltip)
			.OnClicked(OnClicked)
			[
				SNew(STextBlock).Text(Label)
			];
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::ChromeBackground))   // (theme)
		.ForegroundColor(FDreamThemes::ColorAttr(EDreamThemeSlot::ChromeText))
		.Padding(FMargin(6.f, 3.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.MaxWidth(420.f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(FindBox, SEditableTextBox)
					.Style(&FDreamThemes::TextBoxStyle())   // (theme)
					.HintText(LOCTEXT("CodeView.FindHint", "Find"))
					.SelectAllTextWhenFocused(true)
					.ClearKeyboardFocusOnCommit(false)
					.RevertTextOnEscape(false)
					.OnKeyDownHandler(this, &SDreamFindBar::HandleFindKeyDown)
					.OnTextChanged_Lambda([this](const FText&) { NotifyQueryChanged(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f))
				[
					SNew(SBox)
					.MinDesiredWidth(90.f)
					[
						SNew(STextBlock)
						.Text(StatusText)
						.ColorAndOpacity_Lambda([this]()
						{
							return IsStatusError.Get(false) ? FDreamThemes::Color(EDreamThemeSlot::OutError) : FSlateColor::UseSubduedForeground();   // (theme)
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MakeToggle(LOCTEXT("CodeView.MatchCaseLabel", "Aa"), LOCTEXT("CodeView.MatchCaseTip", "Match case (Alt+C)"), &bMatchCase)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MakeToggle(LOCTEXT("CodeView.WholeWordLabel", "W"), LOCTEXT("CodeView.WholeWordTip", "Whole word (Alt+W)"), &bWholeWord)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MakeToggle(LOCTEXT("CodeView.RegexLabel", ".*"), LOCTEXT("CodeView.RegexTip", "Regular expression, Python syntax (Alt+R)"), &bRegex)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.f, 0.f, 0.f, 0.f))
				[
					Button(LOCTEXT("CodeView.FindPrevLabel", "↑"), LOCTEXT("CodeView.FindPrevTip", "Previous match (Shift+F3, Shift+Enter)"),
						FOnClicked::CreateLambda([this]() { OnStep.ExecuteIfBound(-1, false); return FReply::Handled(); }))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Button(LOCTEXT("CodeView.FindNextLabel", "↓"), LOCTEXT("CodeView.FindNextTip", "Next match (F3, Enter)"),
						FOnClicked::CreateLambda([this]() { OnStep.ExecuteIfBound(1, false); return FReply::Handled(); }))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Button(LOCTEXT("CodeView.ReplaceToggleLabel", "Replace"), LOCTEXT("CodeView.ReplaceToggleTip", "Show or hide the replace row (Ctrl+H)"),
						FOnClicked::CreateLambda([this]()
						{
							bShowReplace = !bShowReplace;
							return FReply::Handled();
						}))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Button(LOCTEXT("CodeView.FindCloseLabel", "×"), LOCTEXT("CodeView.FindCloseTip", "Close (Esc)"),
						FOnClicked::CreateLambda([this]() { OnClose.ExecuteIfBound(); return FReply::Handled(); }))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.f, 3.f, 0.f, 0.f))
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return bShowReplace ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.MaxWidth(420.f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(ReplaceBox, SEditableTextBox)
					.Style(&FDreamThemes::TextBoxStyle())   // (theme)
					.HintText(LOCTEXT("CodeView.ReplaceHint", "Replace (\\1, \\g<name>, $1 with Regex)"))
					.SelectAllTextWhenFocused(true)
					.ClearKeyboardFocusOnCommit(false)
					.RevertTextOnEscape(false)
					.OnKeyDownHandler(this, &SDreamFindBar::HandleReplaceKeyDown)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(SButton)
					.ButtonStyle(&FDreamChrome::FlatButton())   // (theme)
					.IsFocusable(false)
					.IsEnabled_Lambda([this]() { return CanReplace.Get(true); })
					.ToolTipText(LOCTEXT("CodeView.ReplaceOneTip", "Replace this match and go to the next (Enter in the replace box)"))
					.OnClicked_Lambda([this]() { OnReplace.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(LOCTEXT("CodeView.ReplaceOneLabel", "Replace"))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&FDreamChrome::FlatButton())   // (theme)
					.IsFocusable(false)
					.IsEnabled_Lambda([this]() { return CanReplace.Get(true); })
					.ToolTipText(LOCTEXT("CodeView.ReplaceAllTip", "Replace every match as one undo step (Ctrl+Alt+Enter in the replace box)"))
					.OnClicked_Lambda([this]() { OnReplaceAll.ExecuteIfBound(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text(LOCTEXT("CodeView.ReplaceAllLabel", "Replace All"))
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SDreamFindBar::MakeToggle(const FText& Label, const FText& Tooltip, bool* Flag)
{
	return SNew(SCheckBox)
		.Style(&FDreamChrome::FlatToggle())   // (theme) Selected while on, as the icon row's toggles
		.IsFocusable(false)
		.ToolTipText(Tooltip)
		.IsChecked_Lambda([Flag]() { return *Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Flag](ECheckBoxState State)
		{
			*Flag = State == ECheckBoxState::Checked;
			NotifyQueryChanged();
		})
		[
			SNew(SBox)
			.Padding(FMargin(5.f, 1.f))
			[
				SNew(STextBlock).Text(Label)
			]
		];
}

void SDreamFindBar::Open(bool bInShowReplace, const FString& Seed)
{
	bShowReplace = bInShowReplace;
	if (!Seed.IsEmpty())
	{
		SetFindText(Seed);
	}
}

void SDreamFindBar::FocusFindBox()
{
	if (FSlateApplication::IsInitialized() && FindBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(FindBox, EFocusCause::SetDirectly);
	}
}

FString SDreamFindBar::GetFindText() const
{
	return FindBox.IsValid() ? FindBox->GetText().ToString() : FString();
}

FString SDreamFindBar::GetReplaceText() const
{
	return ReplaceBox.IsValid() ? ReplaceBox->GetText().ToString() : FString();
}

void SDreamFindBar::SetFindText(const FString& Text)
{
	if (FindBox.IsValid() && GetFindText() != Text)
	{
		FindBox->SetText(FText::FromString(Text));
		NotifyQueryChanged();
	}
}

void SDreamFindBar::SetReplaceText(const FString& Text)
{
	if (ReplaceBox.IsValid())
	{
		ReplaceBox->SetText(FText::FromString(Text));
	}
}

void SDreamFindBar::SetMatchCase(bool bOn)
{
	if (bMatchCase != bOn)
	{
		bMatchCase = bOn;
		NotifyQueryChanged();
	}
}

void SDreamFindBar::SetRegex(bool bOn)
{
	if (bRegex != bOn)
	{
		bRegex = bOn;
		NotifyQueryChanged();
	}
}

void SDreamFindBar::SetWholeWord(bool bOn)
{
	if (bWholeWord != bOn)
	{
		bWholeWord = bOn;
		NotifyQueryChanged();
	}
}

void SDreamFindBar::NotifyQueryChanged()
{
	OnQueryChanged.ExecuteIfBound();
}

bool SDreamFindBar::HandleStepKey(const FKeyEvent& KeyEvent)
{
	// F3 / Shift+F3: the stock text box would run its own AdvanceSearch on them (SlateEditableTextLayout HandleKeyDown).
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	if (KeyEvent.GetKey() != EKeys::F3 || Mods.IsControlDown() || Mods.IsAltDown() || Mods.IsCommandDown())
	{
		return false;
	}
	OnStep.ExecuteIfBound(Mods.IsShiftDown() ? -1 : 1, /*bReturnFocus*/ false);
	return true;
}

bool SDreamFindBar::HandleToggleChord(const FKeyEvent& KeyEvent)
{
	if (!DreamFindBarLocal::IsLeftAltOnly(KeyEvent.GetModifierKeys()))
	{
		return false;
	}
	const FKey Key = KeyEvent.GetKey();
	if (Key == EKeys::C)
	{
		SetMatchCase(!bMatchCase);
		return true;
	}
	if (Key == EKeys::R)
	{
		SetRegex(!bRegex);
		return true;
	}
	if (Key == EKeys::W)
	{
		SetWholeWord(!bWholeWord);
		return true;
	}
	return false;
}

FReply SDreamFindBar::HandleFindKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const FKey Key = KeyEvent.GetKey();
	if (HandleToggleChord(KeyEvent) || HandleStepKey(KeyEvent))
	{
		return FReply::Handled();
	}
	if (!DreamFindBarLocal::IsPlain(Mods))
	{
		return FReply::Unhandled();
	}
	if (Key == EKeys::Enter)
	{
		OnStep.ExecuteIfBound(Mods.IsShiftDown() ? -1 : 1, /*bReturnFocus*/ true);
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		OnClose.ExecuteIfBound();
		return FReply::Handled();
	}
	if (Key == EKeys::Tab)
	{
		if (bShowReplace && ReplaceBox.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(ReplaceBox, EFocusCause::Navigation);
		}
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDreamFindBar::HandleReplaceKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FModifierKeysState& Mods = KeyEvent.GetModifierKeys();
	const FKey Key = KeyEvent.GetKey();
	if (HandleToggleChord(KeyEvent) || HandleStepKey(KeyEvent))
	{
		return FReply::Handled();
	}
	if (Key == EKeys::Enter && Mods.IsControlDown() && Mods.IsLeftAltDown() && !Mods.IsShiftDown())
	{
		OnReplaceAll.ExecuteIfBound();
		return FReply::Handled();
	}
	if (!DreamFindBarLocal::IsPlain(Mods))
	{
		return FReply::Unhandled();
	}
	if (Key == EKeys::Enter && !Mods.IsShiftDown())
	{
		OnReplace.ExecuteIfBound();
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		OnClose.ExecuteIfBound();
		return FReply::Handled();
	}
	if (Key == EKeys::Tab)
	{
		FocusFindBox();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
