// SPDX-License-Identifier: Apache-2.0
// The stock text widget behind plain mode (TDD §4.15 row (3)): an SMultiLineEditableText whose own undo never runs, so
// "Ctrl+Z drives UndoTree" holds (EDT-08). SDreamCodeView owns it and keeps it equal to the buffer's model: user edits are
// reflected into the model from OnTextChanged; model changes (undo, redo, edit groups, a reload) are pushed back into the
// widget as range edits inside one stock transaction, or as one whole-text set for large groups.
// Game thread. Slice-internal (codeview).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/Text/SMultiLineEditableText.h"

class SDreamPlainText final : public SMultiLineEditableText
{
public:
	/**
	 * Drops the stock undo states (each one a whole-text copy, up to 99 of them). Called after every stock edit, so the stock
	 * Undo and Redo (FGenericCommands Ctrl+Z / Ctrl+Y, Alt+Backspace, Alt+Shift+Backspace, the stock context menu) always
	 * find nothing to apply, and the widget holds no undo memory of its own.
	 */
	void DropStockUndo();

	/** An IME composition is open (§4.10, plain mode: Epic's IME path). */
	bool IsComposing() const;

	/**
	 * Programmatic edits: between Begin and End the widget accepts edits even while the view is read-only (the model already
	 * changed: a reload into a file that is read-only on disk), and they form one stock transaction, so OnTextChanged fires
	 * once, at End. Nests.
	 */
	void BeginProgrammaticEdit();
	void EndProgrammaticEdit();
	/**
	 * Inside Begin/End: replaces [Start, End) with Text ('\n' line breaks). Layout is the widget's text layout (the
	 * marshaller's): the stock DeleteSelectedText removes one line break too many when a range over several lines ends at
	 * the end of a line that is not the last one, so the break is put back with the inserted text.
	 */
	void ReplaceRange(const FTextLocation& Start, const FTextLocation& End, const FString& Text, const FTextLayout& Layout);
	/** Replaces the whole text without OnTextChanged, keeping the scroll offset (the cursor stays where it is valid). */
	void SetWholeText(const FString& Text, FTextLayout* Layout);

	// The context menu's clipboard entries (SDreamCodeView builds the menu; its Undo and Redo are UndoTree's).
	bool CanCutText() const;
	void CutText();
	bool CanCopyText() const;
	void CopyText();
	bool CanPasteText() const;
	void PasteText();

	/** The read-only state every stock edit path checks, read live (the stock attribute is a cached value in 5.8). */
	void SetLiveReadOnly(TAttribute<bool> InReadOnly) { LiveReadOnly = MoveTemp(InReadOnly); }

	/**
	 * (editing) PO-06 F1: whole-line selection. A triple-click (a left press within the double-click time and distance of
	 * a double-click) calls Handler(Line, Line) for the clicked line, and every drag move until the release calls
	 * Handler(AnchorLine, LineUnderPointer); the owner selects those lines with their line breaks. Unset: no line mode.
	 */
	void SetOnSelectLines(TFunction<void(int32 AnchorLine, int32 Line)> Handler) { OnSelectLines = MoveTemp(Handler); }

	// SMultiLineEditableText
	virtual bool IsTextReadOnly() const override;
	/** Ctrl+wheel is not a scroll: it bubbles to SDreamCodeView, which zooms (EDT-10). */
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	// (editing) PO-06 F1: triple-click and the whole-line drag after it; everything else is the stock handling.
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	int32 ProgrammaticDepth = 0;
	TAttribute<bool> LiveReadOnly;
	// (editing) triple-click state
	TFunction<void(int32, int32)> OnSelectLines;
	double LastDoubleClickSeconds = -1.0;     // FPlatformTime::Seconds() of the last left double-click, -1 when none counts
	FVector2f LastDoubleClickPosition = FVector2f::ZeroVector;   // its screen-space position
	bool bSelectingLines = false;
	int32 SelectLinesAnchor = 0;
};
