// SPDX-License-Identifier: Apache-2.0
// The breakpoint lane and the paused-line band (TDD §8.2.3, §8.3.5). Debug group (PO-06 F5).
#include "Debug/SDreamBreakpointLane.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Debug/DreamDebugger.h"
#include "Rendering/DrawElements.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"
#include "Styling/SlateStyle.h"

namespace DreamDebugPaintPrivate
{
	constexpr float LaneWidth = 16.f;

	const FSlateBrush* CircleBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 64.f);
		return &Brush;
	}
}

void DreamDebugPaint::DrawBreakpoint(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, const FVector2f& Origin, float Size, bool bHover)
{
	FLinearColor Color = FDreamThemes::LinearColor(EDreamThemeSlot::OutError);
	if (bHover)
	{
		Color.A = 0.35f;      // the dot a click would set (PyCharm's hover ghost)
	}
	// (icons) PO-06 F6: the dream set's Breakpoint glyph (a dot on a 16 px grid) in a square cell; the plain circle when the style
	// set is not up.
	if (const FSlateBrush* Icon = FDreamIcons::Brush(EDreamIcon::Breakpoint))
	{
		const float Box = FMath::Clamp(Size, 10.f, 16.f);
		const FVector2f IconCorner(Origin.X + (Size - Box) * 0.5f, Origin.Y + (Size - Box) * 0.5f);
		FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(FVector2f(Box, Box), FSlateLayoutTransform(IconCorner)),
			Icon, ESlateDrawEffect::None, Color);
		return;
	}
	const float Diameter = FMath::Clamp(Size * 0.62f, 6.f, 14.f);
	const FVector2f Corner(Origin.X + (Size - Diameter) * 0.5f, Origin.Y + (Size - Diameter) * 0.5f);
	FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(FVector2f(Diameter, Diameter), FSlateLayoutTransform(Corner)),
		DreamDebugPaintPrivate::CircleBrush(), ESlateDrawEffect::None, Color);
}

void DreamDebugPaint::DrawArrow(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, const FVector2f& Origin, float Size)
{
	// A right-pointing arrow: a shaft and a head, in the accent colour, over any breakpoint dot.
	const float W = FMath::Clamp(Size * 0.8f, 8.f, 16.f);
	const float X0 = Origin.X + (Size - W) * 0.5f;
	const float Y = Origin.Y + Size * 0.5f;
	const float Half = W * 0.38f;
	const FLinearColor Color = FDreamThemes::LinearColor(EDreamThemeSlot::Accent).CopyWithNewOpacity(1.f);
	// (icons) PO-06 F6: the dream set's CurrentLine arrow, same cell and colour; the drawn arrow when the style set is not up.
	if (const FSlateBrush* Icon = FDreamIcons::Brush(EDreamIcon::CurrentLine))
	{
		const float Box = FMath::Clamp(Size, 10.f, 16.f);
		const FVector2f IconCorner(Origin.X + (Size - Box) * 0.5f, Origin.Y + (Size - Box) * 0.5f);
		FSlateDrawElement::MakeBox(Out, LayerId, Geometry.ToPaintGeometry(FVector2f(Box, Box), FSlateLayoutTransform(IconCorner)),
			Icon, ESlateDrawEffect::None, Color);
		return;
	}
	TArray<FVector2f> Head;
	Head.Add(FVector2f(X0 + W * 0.45f, Y - Half));
	Head.Add(FVector2f(X0 + W, Y));
	Head.Add(FVector2f(X0 + W * 0.45f, Y + Half));
	Head.Add(FVector2f(X0 + W * 0.45f, Y - Half));
	FSlateDrawElement::MakeLines(Out, LayerId, Geometry.ToPaintGeometry(), Head, ESlateDrawEffect::None, Color, true, 2.f);
	TArray<FVector2f> Shaft;
	Shaft.Add(FVector2f(X0, Y));
	Shaft.Add(FVector2f(X0 + W * 0.6f, Y));
	FSlateDrawElement::MakeLines(Out, LayerId, Geometry.ToPaintGeometry(), Shaft, ESlateDrawEffect::None, Color, true, 3.f);
}

FLinearColor DreamDebugPaint::PausedBandColor()
{
	FLinearColor Accent = FDreamThemes::LinearColor(EDreamThemeSlot::Accent);
	Accent.A = 0.28f;
	return Accent;
}

// ---- SDreamBreakpointLane ---------------------------------------------------------------------------------------------

void SDreamBreakpointLane::Construct(const FArguments&, const TSharedRef<SDreamCodeView>& InView)
{
	View = InView;
}

FVector2D SDreamBreakpointLane::ComputeDesiredSize(float) const
{
	return FVector2D(DreamDebugPaintPrivate::LaneWidth, 0.0);
}

int32 SDreamBreakpointLane::LineAtLocalY(float LocalY) const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid())
	{
		return INDEX_NONE;
	}
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	for (const FDreamLineGeometry& Line : Lines)
	{
		if (LocalY >= Line.Top && LocalY < Line.Top + Line.Height)
		{
			return Line.Line;
		}
	}
	return INDEX_NONE;
}

FReply SDreamBreakpointLane::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	const int32 Line = LineAtLocalY(static_cast<float>(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()).Y));
	if (!Pinned.IsValid() || !Pinned->GetBuffer().IsValid() || Line == INDEX_NONE)
	{
		return FReply::Unhandled();
	}
	FDreamDebugger::Get().ToggleBreakpoint(*Pinned->GetBuffer(), Line);
	return FReply::Handled();
}

FReply SDreamBreakpointLane::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	HoverLine = LineAtLocalY(static_cast<float>(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()).Y));
	return FReply::Unhandled();
}

void SDreamBreakpointLane::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	HoverLine = INDEX_NONE;
	SLeafWidget::OnMouseLeave(MouseEvent);
}

int32 SDreamBreakpointLane::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	if (!Pinned.IsValid() || !Pinned->GetBuffer().IsValid())
	{
		return LayerId;
	}
	// The gutter's band, so the lane reads as part of it (PO-04: no separate chrome).
	const FSlateBrush* Band = FDreamThemes::Brush(EDreamThemeSlot::GutterBackground);
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Band, ESlateDrawEffect::None,
		Band->GetTint(InWidgetStyle) * InWidgetStyle.GetColorAndOpacityTint());
	++LayerId;

	FDreamDebugger& Debugger = FDreamDebugger::Get();
	FDreamBuffer& Buffer = *Pinned->GetBuffer();
	const TArray<int32> Breakpoints = Debugger.GetBreakpointLines(Buffer);
	int32 PausedLine = INDEX_NONE;
	Debugger.GetPausedLine(Buffer, PausedLine);
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	const float Width = static_cast<float>(AllottedGeometry.GetLocalSize().X);
	const float Height = static_cast<float>(AllottedGeometry.GetLocalSize().Y);
	for (const FDreamLineGeometry& Line : Lines)
	{
		if (Line.Top + Line.Height < 0.f || Line.Top > Height)
		{
			continue;
		}
		const float Cell = FMath::Min(Width, Line.Height);
		const FVector2f Origin((Width - Cell) * 0.5f, Line.Top + (Line.Height - Cell) * 0.5f);
		const bool bBreakpoint = Breakpoints.Contains(Line.Line);
		if (bBreakpoint || Line.Line == HoverLine)
		{
			DreamDebugPaint::DrawBreakpoint(OutDrawElements, LayerId, AllottedGeometry, Origin, Cell, !bBreakpoint);
		}
		if (Line.Line == PausedLine)
		{
			DreamDebugPaint::DrawArrow(OutDrawElements, LayerId + 1, AllottedGeometry, Origin, Cell);
		}
	}
	return LayerId + 2;
}

// ---- SDreamPausedLineBand ---------------------------------------------------------------------------------------------

void SDreamPausedLineBand::Construct(const FArguments&, const TSharedRef<SDreamCodeView>& InView)
{
	View = InView;
}

int32 SDreamPausedLineBand::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const TSharedPtr<SDreamCodeView> Pinned = View.Pin();
	int32 PausedLine = INDEX_NONE;
	if (!Pinned.IsValid() || !Pinned->GetBuffer().IsValid() || !FDreamDebugger::Get().GetPausedLine(*Pinned->GetBuffer(), PausedLine))
	{
		return LayerId;
	}
	TArray<FDreamLineGeometry> Lines;
	Pinned->GetVisibleLines(Lines);
	for (const FDreamLineGeometry& Line : Lines)
	{
		if (Line.Line == PausedLine)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(FVector2f(static_cast<float>(AllottedGeometry.GetLocalSize().X), Line.Height),
					FSlateLayoutTransform(FVector2f(0.f, Line.Top))),
				FDreamEditorStyle::Get().GetBrush(TEXT("DreamEditor.Editor.CurrentLineBrush")), ESlateDrawEffect::None,
				DreamDebugPaint::PausedBandColor());
			break;
		}
	}
	return LayerId + 1;
}
