// SPDX-License-Identifier: Apache-2.0
// Go to line (Ctrl+G, NAV-03 M1): the line provider of the Go To palette's ':' prefix (TDD §7.9.3), as a small input box
// over the top of the code view until the palette lands (WS-06, M2). "L" or "L:C", 1-based; a line past the end goes to
// the last line. Enter jumps, Esc or a click elsewhere cancels; both give focus back to the code view.
// Game thread. Slice-internal (codeview).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SEditableTextBox;

/** Line and column are 0-based; Column is INDEX_NONE when the input named no column. */
DECLARE_DELEGATE_TwoParams(FDreamOnGoToLineAccept, int32 /*Line*/, int32 /*Column*/);

class DREAMEDITOR_API SDreamGoToLine final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamGoToLine) {}
		SLATE_EVENT(FDreamOnGoToLineAccept, OnAccept)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Clears the input and shows the caret position and the line count in the hint. */
	void Reset(int32 CaretLine, int32 CaretColumn, int32 LineCount);
	TSharedRef<SWidget> GetFocusTarget() const;
	/** The input box (specs type into it). */
	TSharedPtr<SEditableTextBox> GetInputBox() const { return Box; }
	/** Enter: jumps when the input is valid, else shows why under the box. */
	void Accept();

	/**
	 * Parses "L", "L:C" or ":L" (the palette's prefix), blanks ignored. L >= 1; a line past LineCount gives the last line.
	 * C >= 1 or absent. False with a reason for anything else.
	 */
	static bool Parse(const FString& Input, int32 LineCount, int32& OutLine, int32& OutColumn, FText& OutError);

private:
	FReply HandleBoxKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);

	TSharedPtr<SEditableTextBox> Box;
	FDreamOnGoToLineAccept OnAccept;
	FSimpleDelegate OnCancel;
	FText Hint;
	FText Error;
	int32 LineCount = 1;
};
