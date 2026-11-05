// SPDX-License-Identifier: Apache-2.0
// Line numbers and REPL prompts for plain mode (TDD §4.8.2 subset, §4.14).
#include "CodeView/SDreamGutter.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"   // (theme)

namespace DreamGutterLocal
{
	constexpr float LeftPad = 6.f;
	constexpr float RightPad = 8.f;

	int32 DigitCount(int32 Value)
	{
		int32 Digits = 1;
		while (Value >= 10)
		{
			Value /= 10;
			++Digits;
		}
		return Digits;
	}
}

void SDreamGutter::Construct(const FArguments& InArgs, const TSharedRef<SDreamCodeView>& InView)
{
	View = InView;
	Mode = InArgs._Mode;
}

FVector2D SDreamGutter::ComputeDesiredSize(float) const
{
	TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid() || !FSlateApplication::IsInitialized() || Mode == EDreamGutterMode::None)
	{
		return FVector2D(0.0, 0.0);
	}
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FSlateFontInfo Font = Pinned->GetCodeFont();
	const FString Widest = Mode == EDreamGutterMode::Prompt
		? FString(TEXT(">>>"))
		: FString::ChrN(FMath::Max(2, DreamGutterLocal::DigitCount(Pinned->GetLineCount())), TEXT('0'));
	const FVector2D Size = Measure->Measure(Widest, Font);
	return FVector2D(Size.X + DreamGutterLocal::LeftPad + DreamGutterLocal::RightPad, Size.Y);
}

int32 SDreamGutter::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid() || Mode == EDreamGutterMode::None)
	{
		return LayerId;
	}

	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	const int32 CaretLine = Pinned->GetCaretLine();
	const FSlateFontInfo Font = Pinned->GetCodeFont();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	// (theme) PO-04: the gutter's own band and its numbers in the current theme, read at paint (a switch shows next frame).
	const FLinearColor Dim = FDreamThemes::LinearColor(EDreamThemeSlot::GutterText);
	const FLinearColor Bright = FDreamThemes::LinearColor(EDreamThemeSlot::ChromeText);
	const float Width = static_cast<float>(AllottedGeometry.GetLocalSize().X);
	const float Height = static_cast<float>(AllottedGeometry.GetLocalSize().Y);
	if (Mode == EDreamGutterMode::LineNumbers)
	{
		const FSlateBrush* Band = FDreamThemes::Brush(EDreamThemeSlot::GutterBackground);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Band, ESlateDrawEffect::None,
			Band->GetTint(InWidgetStyle) * InWidgetStyle.GetColorAndOpacityTint());
		++LayerId;
	}

	for (const FDreamLineGeometry& Line : Lines)
	{
		if (Line.Top + Line.Height < 0.f || Line.Top > Height)
		{
			continue;
		}
		FString Label;
		if (Mode == EDreamGutterMode::Prompt)
		{
			Label = Line.Line == 0 ? TEXT(">>>") : TEXT("...");
		}
		else
		{
			Label = FString::FromInt(Line.Line + 1);
		}
		const FVector2D TextSize = Measure->Measure(Label, Font);
		const float X = Mode == EDreamGutterMode::Prompt
			? DreamGutterLocal::LeftPad
			: Width - DreamGutterLocal::RightPad - static_cast<float>(TextSize.X);
		const bool bCurrent = Line.Line == CaretLine && Mode == EDreamGutterMode::LineNumbers;
		FSlateDrawElement::MakeText(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(static_cast<float>(TextSize.X), Line.Height), FSlateLayoutTransform(FVector2f(X, Line.Top))),
			Label, Font, ESlateDrawEffect::None, (bCurrent ? Bright : Dim) * InWidgetStyle.GetColorAndOpacityTint());
	}
	return LayerId + 1;
}
