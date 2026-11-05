// SPDX-License-Identifier: Apache-2.0
// The code view's debugger marks (TDD §8.2.3 gutter glyphs, §8.3.5; DBG-01, DBG-03; debug group, PO-06 F5):
//   - SDreamBreakpointLane: a narrow lane left of the line numbers with a red dot per breakpoint line; a click toggles the
//     line's breakpoint (F9 does the same at the caret); the execution arrow on the paused line;
//   - SDreamPausedLineBand: the current-line band of the paused line, behind the text.
// Both read FDreamDebugger at paint, like the gutter reads the view's laid-out lines. Game thread.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"

class SDreamCodeView;

class DREAMEDITOR_API SDreamBreakpointLane final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamBreakpointLane) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SDreamCodeView>& InView);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual bool ComputeVolatility() const override { return true; }
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	/** The model line (0-based) under a local y, or INDEX_NONE (tests click through this). */
	int32 LineAtLocalY(float LocalY) const;

private:
	TWeakPtr<SDreamCodeView> View;
	int32 HoverLine = INDEX_NONE;
};

class DREAMEDITOR_API SDreamPausedLineBand final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamPausedLineBand) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SDreamCodeView>& InView);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
	virtual bool ComputeVolatility() const override { return true; }

private:
	TWeakPtr<SDreamCodeView> View;
};

namespace DreamDebugPaint
{
	/** The breakpoint dot (§8.2.3 "filled"), centred in a cell of Size at Origin (local paint space). */
	void DrawBreakpoint(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, const FVector2f& Origin, float Size, bool bHover);
	/** The execution arrow, centred in a cell of Size at Origin. */
	void DrawArrow(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, const FVector2f& Origin, float Size);
	/** The paused line's band colour: the theme's accent over the current-line colour. */
	FLinearColor PausedBandColor();
}
