// SPDX-License-Identifier: Apache-2.0
// (chrome) FDreamChrome: the chrome slots of the current theme, the flat styles and the icons (PO-04 point 1; (icons) the dream set since PO-06).
#include "Shell/DreamChrome.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Misc/Paths.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"


#define LOCTEXT_NAMESPACE "Dream"

namespace DreamChromeLocal
{
	constexpr int32 SlotCount = static_cast<int32>(EDreamChromeSlot::Warning) + 1;
	constexpr float Radius = 3.f;

	struct FState
	{
		FSlateColorBrush Solid[SlotCount] = {
			FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White),
			FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White),
			FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White),
			FSlateColorBrush(FLinearColor::White), FSlateColorBrush(FLinearColor::White) };
		FButtonStyle Button;
		FCheckBoxStyle Toggle;
		FCheckBoxStyle Tab;
		FComboButtonStyle Combo;
		FSplitterStyle Splitter;
		TArray<TUniquePtr<FSlateBrush>> Icons;
		FSimpleMulticastDelegate Changed;
		FDelegateHandle ThemeHandle;
	};

	FState*& StatePtr()
	{
		static FState* State = nullptr;
		return State;
	}

	EDreamThemeSlot ThemeSlot(EDreamChromeSlot Slot)
	{
		switch (Slot)
		{
		case EDreamChromeSlot::Background: return EDreamThemeSlot::ChromeBackground;
		case EDreamChromeSlot::Border: return EDreamThemeSlot::ChromeBorder;
		case EDreamChromeSlot::Text: return EDreamThemeSlot::ChromeText;
		case EDreamChromeSlot::MutedText: return EDreamThemeSlot::ChromeMutedText;
		case EDreamChromeSlot::Hover: return EDreamThemeSlot::ChromeHover;
		case EDreamChromeSlot::Selected: return EDreamThemeSlot::ChromeSelected;
		case EDreamChromeSlot::Accent: return EDreamThemeSlot::Accent;
		case EDreamChromeSlot::Danger: return EDreamThemeSlot::Danger;
		case EDreamChromeSlot::OutputBackground: return EDreamThemeSlot::OutputBackground;
		case EDreamChromeSlot::Warning: return EDreamThemeSlot::OutWarning;
		case EDreamChromeSlot::EditorBackground:
		default: return EDreamThemeSlot::EditorBackground;
		}
	}

	/** (icons) PO-06 F6: the dream glyph of each chrome icon (Shell/DreamIcons.h). */
	EDreamIcon DreamIcon(EDreamChromeIcon Icon)
	{
		switch (Icon)
		{
		case EDreamChromeIcon::Open: return EDreamIcon::Open;
		case EDreamChromeIcon::Save: return EDreamIcon::Save;
		case EDreamChromeIcon::Run: return EDreamIcon::Run;
		case EDreamChromeIcon::RunLine: return EDreamIcon::RunLine;
		case EDreamChromeIcon::Stop: return EDreamIcon::Stop;
		case EDreamChromeIcon::Clear: return EDreamIcon::Clear;
		case EDreamChromeIcon::UndoRun: return EDreamIcon::UndoRun;
		case EDreamChromeIcon::Echo: return EDreamIcon::Echo;
		case EDreamChromeIcon::LineNumbers: return EDreamIcon::LineNumbers;
		case EDreamChromeIcon::Lock: return EDreamIcon::ReadOnly;
		case EDreamChromeIcon::Recovered: return EDreamIcon::Recovered;
		case EDreamChromeIcon::Close: return EDreamIcon::Close;
		case EDreamChromeIcon::Plus: return EDreamIcon::Plus;
		case EDreamChromeIcon::ChevronDown:
		default: return EDreamIcon::ChevronDown;
		}
	}

	FSlateRoundedBoxBrush Rounded(const FLinearColor& Fill)
	{
		return FSlateRoundedBoxBrush(Fill, Radius);
	}

	void Tint(FSlateBrush& Brush, const FLinearColor& Fill, const FLinearColor& Outline = FLinearColor::Transparent, float Width = 0.f)
	{
		Brush.TintColor = FSlateColor(Fill);
		if (Brush.DrawAs == ESlateBrushDrawType::RoundedBox)
		{
			Brush.OutlineSettings = FSlateBrushOutlineSettings(Radius, FSlateColor(Outline), Width);
		}
	}
}

void FDreamChrome::EnsureInitialized()
{
	using namespace DreamChromeLocal;
	FState*& State = StatePtr();
	if (State != nullptr)
	{
		return;
	}
	State = new FState();

	const FMargin ButtonPadding(4.f, 2.f);
	State->Button = FButtonStyle()
		.SetNormal(FSlateNoResource())
		.SetHovered(Rounded(FLinearColor::White))
		.SetPressed(Rounded(FLinearColor::White))
		.SetDisabled(FSlateNoResource())
		.SetNormalPadding(ButtonPadding)
		.SetPressedPadding(ButtonPadding)
		.SetNormalForeground(FSlateColor::UseForeground())
		.SetHoveredForeground(FSlateColor::UseForeground())
		.SetPressedForeground(FSlateColor::UseForeground())
		.SetDisabledForeground(FSlateColor::UseSubduedForeground());

	State->Toggle = FCheckBoxStyle()
		.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateNoResource())
		.SetUncheckedHoveredImage(Rounded(FLinearColor::White))
		.SetUncheckedPressedImage(Rounded(FLinearColor::White))
		.SetCheckedImage(Rounded(FLinearColor::White))
		.SetCheckedHoveredImage(Rounded(FLinearColor::White))
		.SetCheckedPressedImage(Rounded(FLinearColor::White))
		.SetUndeterminedImage(Rounded(FLinearColor::White))
		.SetUndeterminedHoveredImage(Rounded(FLinearColor::White))
		.SetUndeterminedPressedImage(Rounded(FLinearColor::White))
		.SetPadding(ButtonPadding)
		.SetForegroundColor(FSlateColor::UseForeground())
		.SetHoveredForegroundColor(FSlateColor::UseForeground())
		.SetPressedForegroundColor(FSlateColor::UseForeground())
		.SetCheckedForegroundColor(FSlateColor::UseForeground())
		.SetCheckedHoveredForegroundColor(FSlateColor::UseForeground())
		.SetCheckedPressedForegroundColor(FSlateColor::UseForeground())
		.SetUndeterminedForegroundColor(FSlateColor::UseForeground());

	State->Tab = FCheckBoxStyle()
		.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateNoResource())
		.SetUncheckedHoveredImage(FSlateColorBrush(FLinearColor::White))
		.SetUncheckedPressedImage(FSlateColorBrush(FLinearColor::White))
		.SetCheckedImage(FSlateColorBrush(FLinearColor::White))
		.SetCheckedHoveredImage(FSlateColorBrush(FLinearColor::White))
		.SetCheckedPressedImage(FSlateColorBrush(FLinearColor::White))
		.SetPadding(FMargin(8.f, 2.f))
		.SetForegroundColor(FSlateColor::UseSubduedForeground())
		.SetHoveredForegroundColor(FSlateColor::UseForeground())
		.SetPressedForegroundColor(FSlateColor::UseForeground())
		.SetCheckedForegroundColor(FSlateColor::UseForeground())
		.SetCheckedHoveredForegroundColor(FSlateColor::UseForeground())
		.SetCheckedPressedForegroundColor(FSlateColor::UseForeground());

	// The engine's simple combo keeps its menu frame; the button face is ours.
	State->Combo = FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>(TEXT("SimpleComboButton"));
	State->Combo.SetButtonStyle(State->Button);
	State->Combo.SetContentPadding(FMargin(0.f));

	State->Splitter = FSplitterStyle()
		.SetHandleNormalBrush(FSlateColorBrush(FLinearColor::White))
		.SetHandleHighlightBrush(FSlateColorBrush(FLinearColor::White));

	for (int32 Index = 0; Index < static_cast<int32>(EDreamChromeIcon::Count); ++Index)
	{
		// (icons) the dream set's SVGs (16 px grid; callers scale with DesiredSizeOverride)
		const FString Path = FDreamIcons::FilePath(DreamIcon(static_cast<EDreamChromeIcon>(Index)));
		State->Icons.Add(MakeUnique<FSlateVectorImageBrush>(Path, FVector2f(16.f, 16.f)));
	}

	State->ThemeHandle = FDreamThemes::OnChanged().AddStatic(&FDreamChrome::Retint);
	Retint();
}

void FDreamChrome::Retint()
{
	using namespace DreamChromeLocal;
	FState* State = StatePtr();
	if (State == nullptr)
	{
		return;
	}
	for (int32 Index = 0; Index < SlotCount; ++Index)
	{
		State->Solid[Index].TintColor = FSlateColor(Linear(static_cast<EDreamChromeSlot>(Index)));
	}
	const FLinearColor Hover = Linear(EDreamChromeSlot::Hover);
	const FLinearColor Selected = Linear(EDreamChromeSlot::Selected);
	const FLinearColor Accent = Linear(EDreamChromeSlot::Accent);
	Tint(State->Button.Hovered, Hover);
	Tint(State->Button.Pressed, Selected);
	// A checked toggle reads at a glance: the selected fill with a one-pixel accent outline.
	Tint(State->Toggle.UncheckedHoveredImage, Hover);
	Tint(State->Toggle.UncheckedPressedImage, Selected);
	Tint(State->Toggle.CheckedImage, Selected, Accent, 1.f);
	Tint(State->Toggle.CheckedHoveredImage, Hover, Accent, 1.f);
	Tint(State->Toggle.CheckedPressedImage, Selected, Accent, 1.f);
	Tint(State->Toggle.UndeterminedImage, Selected, Accent.CopyWithNewOpacity(0.5f), 1.f);
	Tint(State->Toggle.UndeterminedHoveredImage, Hover, Accent.CopyWithNewOpacity(0.5f), 1.f);
	Tint(State->Toggle.UndeterminedPressedImage, Selected, Accent.CopyWithNewOpacity(0.5f), 1.f);
	const FLinearColor OutputBackground = Linear(EDreamChromeSlot::OutputBackground);
	State->Tab.UncheckedHoveredImage.TintColor = FSlateColor(Hover);
	State->Tab.UncheckedPressedImage.TintColor = FSlateColor(Selected);
	State->Tab.CheckedImage.TintColor = FSlateColor(OutputBackground);
	State->Tab.CheckedHoveredImage.TintColor = FSlateColor(OutputBackground);
	State->Tab.CheckedPressedImage.TintColor = FSlateColor(OutputBackground);
	Tint(State->Combo.ButtonStyle.Hovered, Hover);
	Tint(State->Combo.ButtonStyle.Pressed, Selected);
	State->Splitter.HandleNormalBrush.TintColor = FSlateColor(Linear(EDreamChromeSlot::Border));
	State->Splitter.HandleHighlightBrush.TintColor = FSlateColor(Accent);
	State->Changed.Broadcast();
}

FLinearColor FDreamChrome::Linear(EDreamChromeSlot Slot)
{
	return FDreamThemes::LinearColor(DreamChromeLocal::ThemeSlot(Slot));
}

FSlateColor FDreamChrome::Color(EDreamChromeSlot Slot)
{
	return FSlateColor(Linear(Slot));
}

TAttribute<FSlateColor> FDreamChrome::ColorAttr(EDreamChromeSlot Slot)
{
	return TAttribute<FSlateColor>::CreateLambda([Slot]() { return FDreamChrome::Color(Slot); });
}

const FSlateBrush* FDreamChrome::Brush(EDreamChromeSlot Slot)
{
	EnsureInitialized();
	return &DreamChromeLocal::StatePtr()->Solid[FMath::Clamp(static_cast<int32>(Slot), 0, DreamChromeLocal::SlotCount - 1)];
}

const FButtonStyle& FDreamChrome::FlatButton()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Button;
}

const FCheckBoxStyle& FDreamChrome::FlatToggle()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Toggle;
}

const FCheckBoxStyle& FDreamChrome::TabToggle()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Tab;
}

const FComboButtonStyle& FDreamChrome::FlatCombo()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Combo;
}

const FSplitterStyle& FDreamChrome::Splitter()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Splitter;
}

const FSlateBrush* FDreamChrome::Icon(EDreamChromeIcon Icon)
{
	EnsureInitialized();
	const TArray<TUniquePtr<FSlateBrush>>& Icons = DreamChromeLocal::StatePtr()->Icons;
	const int32 Index = static_cast<int32>(Icon);
	return Icons.IsValidIndex(Index) ? Icons[Index].Get() : nullptr;
}

FSlateFontInfo FDreamChrome::Font(int32 Size)
{
	return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), Size);
}

bool FDreamChrome::IsLightTheme()
{
	return FDreamThemes::Current().bLight;
}

TArray<TPair<FString, FText>> FDreamChrome::ListThemes()
{
	TArray<TPair<FString, FText>> Out;
	for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
	{
		Out.Emplace(Theme->Id, Theme->Name);
	}
	return Out;
}

FString FDreamChrome::GetThemeId()
{
	return FDreamThemes::GetCurrentId();
}

bool FDreamChrome::SetTheme(const FString& Id)
{
	return FDreamThemes::SetCurrent(Id);
}

FSimpleMulticastDelegate& FDreamChrome::OnChanged()
{
	EnsureInitialized();
	return DreamChromeLocal::StatePtr()->Changed;
}

#undef LOCTEXT_NAMESPACE
