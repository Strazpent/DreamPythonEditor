// SPDX-License-Identifier: Apache-2.0
// The plain-mode text widget (TDD §4.15 row (3)): stock editing, IME and selection; no stock undo (EDT-08).
#include "CodeView/SDreamPlainText.h"

#include "Framework/Text/TextLayout.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // (editing) GetDoubleClickTime, SM_CXDOUBLECLK
#endif

namespace DreamPlainTextLocal
{
	/** (editing) The OS double-click limits, which a triple-click's third press must meet too: time in seconds, and the
	 *  half extents of the rectangle around the double-click it must land in (screen pixels). */
	void GetDoubleClickLimits(double& OutSeconds, FVector2f& OutSlop)
	{
#if PLATFORM_WINDOWS
		OutSeconds = static_cast<double>(::GetDoubleClickTime()) / 1000.0;
		OutSlop = FVector2f(FMath::Max(2.f, ::GetSystemMetrics(SM_CXDOUBLECLK) * 0.5f), FMath::Max(2.f, ::GetSystemMetrics(SM_CYDOUBLECLK) * 0.5f));
#else
		OutSeconds = 0.5;
		OutSlop = FVector2f(2.f, 2.f);
#endif
	}
}

void SDreamPlainText::DropStockUndo()
{
	SlateEditableTextLayout.ClearUndoStates();
}

bool SDreamPlainText::IsComposing() const
{
	return SlateEditableTextLayout.GetTextInputMethodContext()->IsComposing();
}

void SDreamPlainText::BeginProgrammaticEdit()
{
	// The depth goes up first: BeginEditTransaction reads IsTextReadOnly to decide whether to record the text before.
	++ProgrammaticDepth;
	SlateEditableTextLayout.BeginEditTransaction();
}

void SDreamPlainText::EndProgrammaticEdit()
{
	check(ProgrammaticDepth > 0);
	SlateEditableTextLayout.EndEditTransaction();
	--ProgrammaticDepth;
	DropStockUndo();
}

void SDreamPlainText::ReplaceRange(const FTextLocation& Start, const FTextLocation& End, const FString& Text, const FTextLayout& Layout)
{
	check(ProgrammaticDepth > 0);
	// FSlateEditableTextLayout::DeleteSelectedText (5.8): for a selection over several lines that ends at the end of its
	// line, it removes the end line whole and then joins the first line with the line after it, one break more than the
	// range holds. On the last line there is no line after it, so the result is exact there.
	const TArray<FTextLayout::FLineModel>& Lines = Layout.GetLineModels();
	const int32 EndLine = End.GetLineIndex();
	const bool bExtraBreak = Start.GetLineIndex() != EndLine && Lines.IsValidIndex(EndLine) && EndLine < Lines.Num() - 1
		&& Lines[EndLine].Text->Len() == End.GetOffset();
	const FString Insert = bExtraBreak ? Text + TEXT("\n") : Text;
	SlateEditableTextLayout.SelectText(Start, End);
	if (Insert.IsEmpty())
	{
		SlateEditableTextLayout.DeleteSelectedText();
	}
	else
	{
		SlateEditableTextLayout.InsertTextAtCursor(Insert);
	}
}

void SDreamPlainText::SetWholeText(const FString& Text, FTextLayout* Layout)
{
	const FVector2f Scroll = SlateEditableTextLayout.GetScrollOffset();
	const FText NewText = FText::FromString(Text);
	SlateEditableTextLayout.SetEditableText(NewText, /*bForce*/ true);
	// SetEditableText clears the line views (FTextLayout::ClearLines) and builds none: until the next prepass a vertical
	// move, Home/End or a click in this frame would read no line view (ensure at SlateEditableTextLayout.cpp:3119, then
	// LineViews[-1]). Lay the new text out now. (codeview; Windows delivers every key queued in a slow frame in one pump)
	if (Layout != nullptr)
	{
		Layout->UpdateIfNeeded();
	}
	SlateEditableTextLayout.SaveText(NewText);     // GetText (the bound attribute) follows
	SlateEditableTextLayout.SetScrollOffset(Scroll, GetCachedGeometry());
	DropStockUndo();
}

bool SDreamPlainText::CanCutText() const
{
	return SlateEditableTextLayout.CanExecuteCut();
}

void SDreamPlainText::CutText()
{
	SlateEditableTextLayout.CutSelectedTextToClipboard();
}

bool SDreamPlainText::CanCopyText() const
{
	return SlateEditableTextLayout.CanExecuteCopy();
}

void SDreamPlainText::CopyText()
{
	SlateEditableTextLayout.CopySelectedTextToClipboard();
}

bool SDreamPlainText::CanPasteText() const
{
	return SlateEditableTextLayout.CanExecutePaste();
}

void SDreamPlainText::PasteText()
{
	SlateEditableTextLayout.PasteTextFromClipboard();
}

bool SDreamPlainText::IsTextReadOnly() const
{
	if (ProgrammaticDepth > 0)
	{
		return false;
	}
	// UE 5.8 keeps the IsReadOnly argument in a TSlateNestedAttribute whose Get returns the value cached at the last
	// attribute update (a prepass), so right after a run starts, or while the Run window's pump ticks only its own window,
	// the stock edit paths would read "writable". The view's state is read live instead (A27, RT-01).
	return LiveReadOnly.IsSet() ? LiveReadOnly.Get(false) : SMultiLineEditableText::IsTextReadOnly();
}

FReply SDreamPlainText::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.IsControlDown())
	{
		return FReply::Unhandled();
	}
	return SMultiLineEditableText::OnMouseWheel(MyGeometry, MouseEvent);
}

// ---- (editing) PO-06 F1: triple-click selects the whole line, a drag after it extends by whole lines ----

FReply SDreamPlainText::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	bool bTriple = false;
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && LastDoubleClickSeconds >= 0.0 && OnSelectLines
		&& !MouseEvent.IsShiftDown() && !MouseEvent.IsControlDown() && !MouseEvent.IsAltDown() && !MouseEvent.IsCommandDown())
	{
		double Seconds = 0.0;
		FVector2f Slop;
		DreamPlainTextLocal::GetDoubleClickLimits(Seconds, Slop);
		const FVector2f Delta = FVector2f(MouseEvent.GetScreenSpacePosition()) - LastDoubleClickPosition;
		bTriple = FPlatformTime::Seconds() - LastDoubleClickSeconds <= Seconds
			&& FMath::Abs(Delta.X) <= Slop.X && FMath::Abs(Delta.Y) <= Slop.Y;
	}
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		LastDoubleClickSeconds = -1.0;   // a fourth press is an ordinary click again
		bSelectingLines = false;
	}
	// The stock press places the caret under the pointer, takes focus and captures the mouse (its drag selection starts).
	const FReply Reply = SMultiLineEditableText::OnMouseButtonDown(MyGeometry, MouseEvent);
	if (bTriple && Reply.IsEventHandled())
	{
		bSelectingLines = true;
		SelectLinesAnchor = FMath::Max(0, GetCursorLocation().GetLineIndex());
		OnSelectLines(SelectLinesAnchor, SelectLinesAnchor);
	}
	return Reply;
}

FReply SDreamPlainText::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// The stock drag moves the caret to the pointer (a character selection from the line mode's anchor); in line mode the
	// lines from the anchor line to the caret's line are selected over it at once.
	const FReply Reply = SMultiLineEditableText::OnMouseMove(MyGeometry, MouseEvent);
	if (bSelectingLines && Reply.IsEventHandled() && HasMouseCapture() && OnSelectLines)
	{
		OnSelectLines(SelectLinesAnchor, FMath::Max(0, GetCursorLocation().GetLineIndex()));
	}
	return Reply;
}

FReply SDreamPlainText::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bSelectingLines = false;
	}
	return SMultiLineEditableText::OnMouseButtonUp(MyGeometry, MouseEvent);
}

FReply SDreamPlainText::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FReply Reply = SMultiLineEditableText::OnMouseButtonDoubleClick(MyGeometry, MouseEvent);
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		// The platform sends the second press of a double-click as this event; a third press soon after is a triple-click.
		LastDoubleClickSeconds = FPlatformTime::Seconds();
		LastDoubleClickPosition = FVector2f(MouseEvent.GetScreenSpacePosition());
	}
	return Reply;
}
