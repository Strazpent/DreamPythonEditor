// SPDX-License-Identifier: Apache-2.0
// The find and replace bar of the code view (EDT-09 M1, TDD §4.4): a find box with Match case (Alt+C), Regex (Alt+R) and
// Whole word (Alt+W) toggles, the live count, previous and next, and the replace row (Ctrl+H) with Replace and Replace
// All. The bar holds the query only; SDreamCodeView searches its buffer with dream::SearchEngine, selects matches and paints
// them. Keys in its boxes: Enter / Shift+Enter go to the next / previous match and give focus back to the text (§4.9.3
// "the find box takes focus; Esc or Enter returns it"), F3 / Shift+F3 step, Enter in the replace box replaces, Esc
// closes, Tab switches boxes; every other key bubbles to the code view, which runs the bound chords (Ctrl+F, Ctrl+S, ...).
// Game thread. Slice-internal (codeview).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SEditableTextBox;

/** Direction +1 / -1; bReturnFocus: Enter in the find box (focus goes back to the text). */
DECLARE_DELEGATE_TwoParams(FDreamOnFindStep, int32 /*Direction*/, bool /*bReturnFocus*/);

class DREAMEDITOR_API SDreamFindBar final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamFindBar) {}
		/** The find text or an option changed. */
		SLATE_EVENT(FSimpleDelegate, OnQueryChanged)
		SLATE_EVENT(FDreamOnFindStep, OnStep)
		SLATE_EVENT(FSimpleDelegate, OnReplace)
		SLATE_EVENT(FSimpleDelegate, OnReplaceAll)
		SLATE_EVENT(FSimpleDelegate, OnClose)
		/** "3 of 12", "12 results", "No results", or the pattern error. */
		SLATE_ATTRIBUTE(FText, StatusText)
		SLATE_ATTRIBUTE(bool, IsStatusError)
		/** Replace and Replace All are disabled while the text is read-only. */
		SLATE_ATTRIBUTE(bool, CanReplace)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows the replace row or not; Seed (a one-line selection) replaces the find text when not empty. */
	void Open(bool bShowReplace, const FString& Seed);
	void FocusFindBox();

	FString GetFindText() const;
	FString GetReplaceText() const;
	bool IsMatchCase() const { return bMatchCase; }
	bool IsRegex() const { return bRegex; }
	bool IsWholeWord() const { return bWholeWord; }
	bool IsReplaceShown() const { return bShowReplace; }

	// Specs and the code view set the query directly.
	void SetFindText(const FString& Text);
	void SetReplaceText(const FString& Text);
	void SetMatchCase(bool bOn);
	void SetRegex(bool bOn);
	void SetWholeWord(bool bOn);

	TSharedPtr<SEditableTextBox> GetFindBox() const { return FindBox; }
	TSharedPtr<SEditableTextBox> GetReplaceBox() const { return ReplaceBox; }

private:
	FReply HandleFindKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	FReply HandleReplaceKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	/** F3 / Shift+F3 in a box: the next or previous match. */
	bool HandleStepKey(const FKeyEvent& KeyEvent);
	/** Alt+C / Alt+R / Alt+W (left Alt only: AltGr types text, RT-16). */
	bool HandleToggleChord(const FKeyEvent& KeyEvent);
	TSharedRef<SWidget> MakeToggle(const FText& Label, const FText& Tooltip, bool* Flag);
	void NotifyQueryChanged();

	TSharedPtr<SEditableTextBox> FindBox;
	TSharedPtr<SEditableTextBox> ReplaceBox;
	FSimpleDelegate OnQueryChanged;
	FDreamOnFindStep OnStep;
	FSimpleDelegate OnReplace;
	FSimpleDelegate OnReplaceAll;
	FSimpleDelegate OnClose;
	TAttribute<FText> StatusText;
	TAttribute<bool> IsStatusError;
	TAttribute<bool> CanReplace;
	bool bMatchCase = false;
	bool bRegex = false;
	bool bWholeWord = false;
	bool bShowReplace = false;
};
