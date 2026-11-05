// SPDX-License-Identifier: Apache-2.0
// The output view (TDD §6.4, OUT-02, OUT-05, OUT-07, OUT-09, OUT-12, A11Y-05): a virtualized view of FDreamOutputStore
// with typed coloured lines, clickable traceback links, follow mode with the "↓ n new lines" chip, type toggles, Normal
// echo folding, Find (Ctrl+F) and the soft-wrap toggle. Game thread. Body: SDreamOutputView.cpp (output slice).
// (output, PO-04 point 2) The Maya-style history: muted echo without a gutter or a '>>> ' / '... ' prompt (A226),
// "# Result: … #", "# Warning: … #", tracebacks as "# " lines ending in "# Error: <Type>: <message> #", no per-line
// icons; run headers and footers (one muted line each, rendered from FDreamRunRecord) only with View ▸ Run headers; Re-run,
// Run in transaction, Undo this run and Copy in the context menu on any line of a run; colours from the current theme.
// (slice A choice, A33) Built on SListView rows over a dream::VisibleIndex, not on SCodeView's read-only line-source mode.
// Deferred (G1): the line-source mode comes with the custom SCodeView; the public surface below stays when it is swapped.
// (output, PO-06 F3) The history is selectable like a read-only text view (Shell/DreamOutputSelection.h): press and drag,
// double-click a word, triple-click a line, Shift+click, Ctrl+A, and Ctrl+C copies the selected characters as drawn; the
// Run window's live view selects too, and its selection carries over to the docked view at the hand-off.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Run/DreamRunTypes.h"

class FMenuBuilder;
class IDreamCommandTarget;
enum class EDreamOutputRowKind : uint8;

/** The Run window -> docked view hand-off (§5.5.3 "Hand-off"). */
struct FDreamOutputViewHandoff
{
	bool bFollowing = true;
	uint64 FirstVisibleSeq = 0;         // the live view's first visible line
	float FirstVisibleScreenY = 0.f;    // where it was drawn, in desktop pixels
	/** Echo folds the reader expanded in the live view ((run << 32) | statement), so the docked view shows the same block
	 *  (PO-03 "the same block, scrolled the same way"). */
	TArray<uint64> ExpandedFolds;
	/** (output, PO-06 F3) The live view's text selection by the stored lines its ends are on and the offsets in their drawn
	 *  text (0: none, or an end on a row that is no stored line), so the docked view shows it selected after the run. */
	uint64 SelectionAnchorSeq = 0;
	int32 SelectionAnchorOffset = 0;
	uint64 SelectionCaretSeq = 0;
	int32 SelectionCaretOffset = 0;
};

/** A "<dream:REPL#N>" / "<dream:Console#N>" link: scroll Output to run N's echo line Line (OUT-05). False: not shown. */
DECLARE_DELEGATE_RetVal_TwoParams(bool, FDreamOnOpenPromptLink, int32 /*RunNumber*/, int32 /*Line*/);

class DREAMEDITOR_API SDreamOutputView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamOutputView)
		: _bLiveRunView(false)
	{}
		/** The REPL tab's view: only runs of this route (route = Repl, §6.5). Unset: every run. */
		SLATE_ARGUMENT(TOptional<EDreamRunRoute>, RouteFilter)
		/** The Run window's live run view: only this run number (§5.5.3). */
		SLATE_ARGUMENT(TOptional<int32>, RunFilter)
		/** No keyboard focus and no text selection (PO-03); the wheel, scrollbar, new-lines chip and fold chips still work. */
		SLATE_ARGUMENT(bool, bLiveRunView)
		/** Executes bound chords (F4, Shift+F5, Ctrl+Enter, ...) while the view has focus (§4.9.4); the owning tab. */
		SLATE_ARGUMENT(TWeakPtr<IDreamCommandTarget>, CommandTarget)
		/** REPL and console links go to the Output view (SDreamOutputArea binds it for both views); unbound: this view. */
		SLATE_EVENT(FDreamOnOpenPromptLink, OnOpenPromptLink)
	SLATE_END_ARGS()

	/**
	 * The "Show" toggles of §6.4 "Design: filters" (OUT-03): one per type plus background, read from and written to the
	 * store's shared visibility, so every view follows. Echo input runs EDreamCommand::ToggleEchoInput through EchoTarget,
	 * the same switch as the toolbar (persisted in bEchoInput). For the context menu, the View menu and the Filter button.
	 */
	static void AddShowMenuEntries(FMenuBuilder& Menu, TWeakPtr<IDreamCommandTarget> EchoTarget);

	void Construct(const FArguments& InArgs);
	virtual ~SDreamOutputView() override;

	// ---- Follow mode (OUT-09) ----

	bool IsFollowing() const;
	void ScrollToEnd();
	/** Lines appended below the viewport while not following (the chip count). */
	int32 GetNewLinesBelow() const;

	// ---- Navigation ----

	/** Scrolls to a run's header (expanding it) and selects it. With run headers hidden: the run's outcome line when it has
	 *  one (it ended other than ok with no error line, review r6 M1), else the first row the run shows. False when the run
	 *  has no rows in this view. */
	bool ScrollToRun(int32 RunNumber);
	/** Scrolls to and selects a run's traceback (WS-07 click, U14): its last "Traceback" line, else the first line of its
	 *  last block of error lines; false when it has no error line here. */
	bool ScrollToRunError(int32 RunNumber);
	/** <dream:REPL#N> links (§6.4): scrolls to run N's echo line Line and highlights it; falls back to the header. */
	bool ScrollToEchoLine(int32 RunNumber, int32 Line);

	/**
	 * F4 / Shift+F4 (U13): Direction +1 / -1 over link spans on error, stderr and warning lines of this view's runs, in
	 * output order; opens the target through FDreamEditorModule::OpenLocation; wraps. OutStatus gets "Wrapped to the first
	 * error", "Wrapped to the last error" or "No errors in output", and stays empty otherwise; the caller shows a non-empty
	 * status (SDreamTab::ShowStatusMessage). Game thread; call it on SDreamOutputArea::GetActiveOutputView().
	 */
	bool WalkErrors(int32 Direction, FText& OutStatus);

	// ---- Run window hand-off (§5.5.3) ----

	/** On the live run view: following, and the first visible line with its screen position in desktop pixels. */
	FDreamOutputViewHandoff CaptureHandoff() const;
	/** On the docked view from SDreamOutputArea::GetHandoffView: following stays following; otherwise the reader's line stays
	 *  where it was on screen, or at the nearest row this view shows. Game thread, before the Run window is torn down. */
	void ApplyHandoff(const FDreamOutputViewHandoff& Handoff);

	/** Rebuilds the visible index after a visibility, collapse or fold change (cheap; the store's events also trigger it). */
	void RebuildVisibleIndex();

	/** Ctrl+C: (output, PO-06 F3) the selected characters exactly as drawn, decorations included, rows joined with '
';
	 *  without a text selection, the selected rows as raw text, without decorations (§6.4 copy, A209). */
	FString GetSelectedText() const;

	// ---- (output, PO-06 F3) Text selection ----

	/** Characters are selected (a drag, a double- or triple-click, Shift+click or Ctrl+A). */
	bool HasTextSelection() const;
	/** Ctrl+A: every row this view shows, from the first character to the last. */
	void SelectAllText();
	/** Drops the text selection (the rows the list has selected stay selected). */
	void ClearTextSelection();

	// ---- Find (§6.4 widget: Ctrl+F, dream::SearchEngine over a ring snapshot, off the game thread) ----

	/** Ctrl+F in the view: shows the find bar above the rows (seeded with the selected line's text when it is one short
	 *  line) and focuses its box. Not in the live run view. */
	void OpenFind();
	/** Esc in the bar: hides it and drops the matches; focus goes back to the rows. */
	void CloseFind();
	bool IsFindOpen() const;
	/** Sets the query as the bar's box and toggles do (Python `re` syntax when bRegex), and starts a search pass. */
	void SetFindQuery(const FString& Text, bool bMatchCase, bool bRegex, bool bWholeWord);
	/** Enter / Shift+Enter in the box, F3 / Shift+F3: selects the next (+1) or previous (-1) match after the current one
	 *  (or after the selected row), wrapping, and reveals it (a collapsed block or a folded echo opens). False: none. */
	bool FindStep(int32 Direction);
	/** True while a search pass runs on the worker. */
	bool IsFindSearching() const;
	/** Matches this view shows (its type, route and run filters apply) and the current one's line, 0 when none. */
	int32 GetFindMatchCount() const;
	uint64 GetCurrentFindSeq() const;
	/** "3 of 12", "12 results", "No results", or the pattern's error, as the bar shows it. */
	FText GetFindStatus() const;

	// ---- Soft wrap (§6.4 widget: off by default) ----

	/** Every output view of the session, the Run window's live run view included, follows one toggle (Show menus). */
	static bool IsSoftWrapEnabled();
	static void SetSoftWrapEnabled(bool bEnabled);

	/** Widget is this view's list, the widget in it that takes keyboard focus (SDreamTab's focus home, P4-shl-02). */
	bool IsFocusList(const SWidget& Widget) const;

	/** Test hook (OUT-03): the store sequences of the line rows this view shows for a run, in order. */
	TArray<uint64> GetLineSeqsForTest(int32 RunNumber) const;
	/** Test hook (P3-out-03): no selected row and no selector row, as in a view nobody clicked since its rows were built
	 *  (the rows are rebuilt, so the list's selector names no row). Following stays following. */
	void ResetSelectionForTest();
	/** Test hook (T-UEI-002): the run of the selected header row (a run block opened by ScrollToRun), else 0. */
	int32 GetSelectedHeaderRunForTest() const;
	/** Test hook (soft wrap): the laid-out height of the row that shows a stored line, 0 when it has no widget now. */
	float GetRowHeightForTest(uint64 Seq) const;
	/** Test hooks (Find): the worker passes started, and the times the shown row widgets were made again. */
	int32 GetFindPassesForTest() const;
	int32 GetRowRebuildsForTest() const;
	/** Test hook (PO-04): the rows this view has for a run (0: the rows outside runs), in order: their kinds, their texts as
	 *  drawn (decorations included, DreamOutputRows::GetRowDisplayText) and the stored line each shows (0 for other rows). */
	void GetRowsForTest(int32 RunNumber, TArray<EDreamOutputRowKind>& OutKinds, TArray<FString>& OutTexts, TArray<uint64>& OutSeqs) const;
	/** Test hook (PO-04): the widgets the list made for the rows it shows now, and the widget of a run's row Index (the
	 *  order of GetRowsForTest; null when that row is not shown now). */
	TArray<TSharedRef<SWidget>> GetShownRowWidgetsForTest() const;
	TSharedPtr<SWidget> GetRunRowWidgetForTest(int32 RunNumber, int32 Index) const;
	/** Test hook (PO-04): selects a run's row Index (the order of GetRowsForTest) and scrolls it into view; false when the
	 *  run has no such row. */
	bool SelectRunRowForTest(int32 RunNumber, int32 Index);
	/** Test hook (PO-04): the labels of the context menu's plain entries on the selected rows (the menu a right-click opens),
	 *  and running one of them; false when there is no such entry or it cannot run now. */
	TArray<FString> GetContextMenuLabelsForTest() const;
	/** Test hook (PO-06 F3): the rectangles, in desktop space, that a run's row Index (the order of GetRowsForTest) paints
	 *  in the theme's Selection colour now; empty when nothing of it is selected or it is not shown. */
	TArray<FSlateRect> GetSelectionRectsForTest(int32 RunNumber, int32 Index) const;
	bool InvokeContextMenuEntryForTest(const FString& Label);

	virtual bool SupportsKeyboardFocus() const override;
	/** The consume-list pass for keys the list left unhandled (§4.9.4, D8), and the key-ups of consumed key-downs. */
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Context menu and palette Expand Echo / Fold Echo (§6.4): the selected statement, or every statement of the
	 *  selected runs; does nothing without a selection. Game thread. */
	void ExpandEcho(bool bExpand);

private:
	struct FImpl;
	TSharedPtr<FImpl> Impl;     // (slice A choice) shared, so row widgets can hold a weak reference to the view state
};
