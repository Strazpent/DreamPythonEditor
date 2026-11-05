// SPDX-License-Identifier: Apache-2.0
// The plain-mode gutter (TDD §4.8.2 subset, §4.14 prompt mode): 1-based line numbers with the caret line brighter, or the
// REPL prompts '>>>' / '...' outside the text. It reads the code view's laid-out lines each paint, so it follows scrolling
// and zoom without bookkeeping. Game thread. Slice-internal (codeview).
// Deferred (M2): the run lane, change bars (EDT-02) and ran-line marks (EDT-18).
#pragma once

#include "CoreMinimal.h"
#include "CodeView/SDreamCodeView.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"

class DREAMEDITOR_API SDreamGutter final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamGutter)
		: _Mode(EDreamGutterMode::LineNumbers)
	{}
		SLATE_ARGUMENT(EDreamGutterMode, Mode)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SDreamCodeView>& InView);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual bool ComputeVolatility() const override { return true; }

private:
	TWeakPtr<SDreamCodeView> View;
	EDreamGutterMode Mode = EDreamGutterMode::LineNumbers;
};
