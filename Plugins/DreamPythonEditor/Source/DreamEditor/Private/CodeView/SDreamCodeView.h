// SPDX-License-Identifier: Apache-2.0
// The code view: Plain mode (TDD §4.15 fallback row (3), EDT-15, A1), the facade every group uses for code text. A stock
// SMultiLineEditableText (SDreamPlainText) with FDreamPythonMarshaller (dream::PyLexer tokens -> runs), a line-number or prompt
// gutter, current-line highlight, the monospace font, Go to line (NAV-03), zoom (EDT-10) and Tab moves focus (CFG-08).
// Every stock edit is reflected into the buffer's dream::TextBuffer with dream::DiffSingleRange, so runs, journal and
// statement extents read one model. Game thread. Body: SDreamCodeView.cpp + DreamPythonMarshaller.cpp (codeview).
// Deferred (G1): the custom SCodeView of §4.8 replaces this widget behind the same facade if the G1 spike picks it.
//
// Commands: the view is the IDreamCommandTarget its keys resolve against. It executes the text commands itself
// (FDreamCommands::IsCodeViewCommand: Undo, Redo, DuplicateLine, Find, ...) and forwards every other command to its
// CommandTarget (the tab, or the REPL strip), so Ctrl+Z in the REPL prompt undoes the prompt.
#pragma once

#include "CoreMinimal.h"
#include "CodeView/DreamEditGroup.h"
#include "CodeView/DreamFindSearch.h"
#include "CodeView/DreamUndoHistory.h"
#include "Commands/DreamKeyRouter.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "dream/EditBuilders.h"
#include "dream/Types.h"

class FDreamBuffer;
class FDreamCodeViewDiagnostics;   // (lang)
class FDreamCompletionController;
class FDreamPythonMarshaller;
class FDreamUndoHistory;
class SScrollBar;
class SDreamFindBar;
class SDreamGoToLine;
class SDreamPlainText;
struct FDreamCodeViewFindTask;
class UObject;
struct FPropertyChangedEvent;
struct FTextLocation;
namespace dream { struct ChangeSet; }

enum class EDreamGutterMode : uint8
{
	LineNumbers,        // buffers
	Prompt,             // the REPL: '>>>' on the first line, '...' on the others (§4.14 prompt mode)
	None,
};

/** One laid-out line in the text widget's local space (gutter and current-line highlight). */
struct FDreamLineGeometry
{
	int32 Line = 0;             // 0-based model line
	float Top = 0.f;
	float Height = 0.f;
};

/** Runs before the key router (the REPL's hard-wired prompt keys, §6.5). Return Handled to stop routing. */
DECLARE_DELEGATE_RetVal_OneParam(FReply, FDreamOnCodeViewKey, const FKeyEvent&);

class DREAMEDITOR_API SDreamCodeView : public SCompoundWidget, public IDreamCommandTarget
{
public:
	SLATE_BEGIN_ARGS(SDreamCodeView)
		: _GutterMode(EDreamGutterMode::LineNumbers)
		, _ContextMask(0)
		, _FontSize(10)
		, _EnableFind(true)
	{}
		/** The model this view edits. Required. Its TextBuffer is updated on every stock OnTextChanged. */
		SLATE_ARGUMENT(TSharedPtr<FDreamBuffer>, Buffer)
		/** Executes bound chords (§4.9.4) that are not text commands; usually the owning SDreamTab, or the REPL strip. */
		SLATE_ARGUMENT(TWeakPtr<IDreamCommandTarget>, CommandTarget)
		SLATE_ARGUMENT(EDreamGutterMode, GutterMode)
		/** dream::ContextBit bits this view adds to every resolution (Ctx_Buffer, Ctx_Prompt, ...). Focused/Editable/
		 *  HasSelection/RunActive are computed per key. */
		SLATE_ARGUMENT(uint32, ContextMask)
		/** The Font size setting at construction; the view follows later changes of UDreamUserSettings::FontSize itself. */
		SLATE_ARGUMENT(int32, FontSize)
		/** The find bar (Ctrl+F, Ctrl+H). Only a line-number view has one: in the REPL prompt Find goes to CommandTarget. */
		SLATE_ARGUMENT(bool, EnableFind)
		/** Read-only while the attribute is true. Views bind it to "buffer read-only OR FDreamRunService::IsRunActive()". */
		SLATE_ATTRIBUTE(bool, IsReadOnly)
		SLATE_ATTRIBUTE(FText, HintText)
		SLATE_EVENT(FDreamOnCodeViewKey, OnKeyHook)
		/** After the TextBuffer was updated from an edit. */
		SLATE_EVENT(FSimpleDelegate, OnEdited)
		SLATE_EVENT(FSimpleDelegate, OnCaretMoved)
		/** First focus and every focus: FDreamPython::RequestStart(Focus) (RT-17, §4.9.3). */
		SLATE_EVENT(FSimpleDelegate, OnFocused)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamCodeView() override;

	TSharedPtr<FDreamBuffer> GetBuffer() const { return Buffer; }
	/** (completion) This view's completion popup controller (CodeView/SDreamCompletionPopup.h, §7.6.2); specs drive it. */
	TSharedPtr<FDreamCompletionController> GetCompletion() const { return Completion; }

	// ---- Text ----

	/** The model's text (the widget always equals it). */
	FString GetText() const;
	/** The text the stock widget shows: GetText() after every sync (specs check that the two never differ). */
	FString GetShownText() const;
	/** Replaces the text with no undo step and drops the undo history (a load: origin Load starts a new root, §4.2). */
	void SetTextNoUndo(const FString& Text);
	/** Replaces lines [First, Last] (0-based, inclusive, without their final '\n') as ONE undo step; the caret ends after the
	 *  new text (toggle comment, indent, recovery Restore, the REPL's Esc clear, PER-03). Nothing when read-only. */
	void ReplaceLines(int32 FirstLine, int32 LastLine, const FString& NewText);
	/** Inserts at the caret (replacing the selection) like typed text (the REPL's Shift+Enter newline and indent). */
	void InsertAtCaret(const FString& Text);

	// ---- The grouped-edit API (EDT-08, §4.2 R5): FDreamUndoHistory's, plus this view's read-only state ----

	/** Group.Edits as ONE undo step with Group.Label (CodeView/DreamUndoHistory.h). False: nothing changed (a bad or
	 *  overlapping range, read-only without bEvenIfReadOnly, a run is active). */
	bool ApplyEditGroup(const FDreamEditGroup& Group);
	/** The whole text replaced by NewText through minimal edits as ONE undo step (reload, Restore, restore from history). */
	bool ReplaceAllText(const FString& NewText, const FText& Label, dream::EditOrigin Origin = dream::EditOrigin::Command,
		bool bEvenIfReadOnly = false);

	// ---- Text undo (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z; dream::UndoTree, EDT-08) ----

	bool CanUndo() const;
	bool CanRedo() const;
	void Undo();
	void Redo();
	TSharedPtr<FDreamUndoHistory> GetUndoHistory() const { return History; }

	// ---- Selection and caret ----

	FDreamTextSelection GetSelection() const;
	FString GetSelectedText() const;
	void SetSelection(const FDreamTextSelection& Selection);
	void SetCaret(int32 Line, int32 Column);         // clears the selection
	void GetCaret(int32& OutLine, int32& OutColumn) const;

	// ---- Scrolling and view ----

	/** Scrolls so Line is visible with Margin lines to the edges (§4.5: 3 lines for Run line and advance). */
	void ScrollToLine(int32 Line, int32 Margin = 3);
	int32 GetFirstVisibleLine() const;
	void SetFontSize(int32 Points);
	int32 GetFontSize() const { return FontSize; }
	/**
	 * Ctrl+= / Ctrl+- / Ctrl+0 and Ctrl+wheel (EDT-10): Delta +1 / -1 steps of 10% within 50%..300% of the Font size
	 * setting, 0 resets to 100%. The percentage is the buffer's view state (per tab, saved with the session).
	 */
	void Zoom(int32 Delta);
	int32 GetZoomPercent() const;
	/** Re-lexes and re-marshals the whole text (theme or grammar change). */
	void RefreshHighlighting();
	/** The indentation this buffer is written with (§4.5 indent detection at load; 4 spaces for new text, BUF-06). */
	const dream::IndentStyle& GetIndentStyle() const { return Indent; }

	// ---- Line commands (EDT-04) ----

	/** Ctrl+D: the caret line copied below it, or the selection inserted again after itself; one undo step. */
	void DuplicateLineOrSelection();

	void FocusEditor();
	bool HasEditorFocus() const;
	bool IsReadOnly() const;

	// ---- Go to line (Ctrl+G, NAV-03) ----

	void OpenGoToLine();
	/** Closes the Go to line box; focus goes back to the text when it was inside the box. */
	void CloseGoToLine();
	bool IsGoToLineOpen() const { return bGoToLineOpen; }
	TSharedPtr<SDreamGoToLine> GetGoToLine() const { return GoToLine; }

	// ---- Find and replace (Ctrl+F, Ctrl+H, F3, Shift+F3; EDT-09 M1) ----

	bool HasFindBar() const { return bFindEnabled; }
	/** Opens the bar (with the replace row for Ctrl+H) and focuses its find box; a one-line selection becomes the query. */
	void OpenFindBar(bool bReplace);
	/** Esc: closes the bar, clears the match marks; focus goes back to the text when it was in the bar. */
	void CloseFindBar();
	bool IsFindBarOpen() const { return bFindBarOpen; }
	/** F3 / Shift+F3 and the bar's arrows: selects the next or previous match from the selection, wrapping around. */
	void FindStep(int32 Direction, bool bFocusText);
	/** Replaces the selected match (one undo step) and selects the next one; selects the next match when none is selected. */
	void ReplaceCurrent();
	/** Replaces every match as one undo step (§4.4). */
	void ReplaceAll();
	TSharedPtr<SDreamFindBar> GetFindBar() const { return FindBar; }
	/**
	 * Specs only (it blocks): searches now if the query or the text changed since the last search, and waits for a regex
	 * search or Replace All on the worker to finish and applies it. Input never calls it (F3 and Replace wait for the worker).
	 */
	void UpdateFindNow();
	/** A regex search or Replace All runs on a worker now (§4.4 Run); the bar shows "Searching…" or "Replacing…". */
	bool IsFindBusy() const { return FindTask.IsValid(); }
	/** Find workers running in this process, any view (specs: closing the bar or a new query cancels them). Any thread. */
	static int32 GetFindWorkersRunning();
	/** Stock edits the view took back because it was read-only (never expected: the stock widget reads the same state). */
	static int32 GetReadOnlyTakeBacks();
	const TArray<FDreamFindMatch>& GetFindMatches() const { return FindMatches; }
	/** The index of the selected match, or INDEX_NONE. */
	int32 GetCurrentFindMatch() const { return CurrentFindMatch; }
	/** The bar's live count: "3 of 12", "12 results", "No results", or the pattern error. */
	FText GetFindStatus() const { return FindStatus; }

	// ---- Layout queries (gutter, current-line highlight; slice-internal callers) ----

	/** The lines that intersect the text widget's viewport, in its local space. */
	void GetVisibleLines(TArray<FDreamLineGeometry>& Out) const;
	/** The rectangles of the shown part of the model range [Start, End), one per visible line, in the text's local space. */
	void GetRangeRects(dream::Offset Start, dream::Offset End, TArray<FSlateRect>& Out) const;
	/** (lang) The same over visible lines the caller already has (GetVisibleLines once per paint, review lang-q1-m6). */
	void GetRangeRects(dream::Offset Start, dream::Offset End, TArray<FSlateRect>& Out, const TArray<FDreamLineGeometry>& Lines) const;
	/** The caret's rectangle in desktop space (§4.8.3: popups anchor to it, §7.6.2); empty when it is not shown. */
	FSlateRect GetCaretScreenRect() const;
	int32 GetLineCount() const;
	int32 GetCaretLine() const;
	FSlateFontInfo GetCodeFont() const;
	/** The context bits for a key in this view (§4.9.2): Focused, Editable, HasSelection, RunActive and the base mask. */
	uint32 ComputeContextMask() const;

	// ---- (lang) PO-06 F4: live problems (CodeView/SDreamDiagnostics.h) ----

	/** The view's problem painter: squiggles, tooltip, bulb, scroll-bar marks, inspection widget. Never null after Construct. */
	TSharedPtr<FDreamCodeViewDiagnostics> GetDiagnostics() const { return Diagnostics; }
	/** The model line and UTF-16 column under a desktop position; false off the text or before the first layout. */
	bool HitTestText(const FVector2D& ScreenPosition, int32& OutLine, int32& OutColumn) const;
	/** The text widget's geometry (GetVisibleLines and GetRangeRects are in its local space). */
	FGeometry GetTextGeometry() const;

	// ---- IDreamCommandTarget: text commands here, the rest to CommandTarget ----

	virtual bool CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const override;
	virtual void ExecuteDreamCommand(EDreamCommand Command) override;
	virtual void ShowCommandRefusal(EDreamCommand Command, const FText& Reason) override;

	// ---- SWidget ----

	/** The bubble pass of FDreamKeyRouter::RouteBubble for key-downs the stock widget left unhandled (consume list). */
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	/** Ctrl+wheel (bubbled up from the text, which does not scroll then) zooms (EDT-10). */
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent) override;

private:
	/** OnKeyDownHandler of the text widget: OnKeyHook, then FDreamKeyRouter::RoutePre, then the plain-mode primitives. */
	FReply HandleTextKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	/** OnKeyCharHandler of the text widget: auto-close pairing and type-over of a closer it inserted (EDT-05). */
	FReply HandleTextKeyChar(const FGeometry& Geometry, const FCharacterEvent& CharacterEvent);
	/** Enter and Backspace through the core edit builders (EDT-03); Unhandled when the stock primitive should run. */
	FReply HandleSmartEnter();
	FReply HandleSmartBackspace();
	void HandleTextChanged(const FText& NewText);
	void HandleCursorMoved(const FTextLocation& Location);
	/** The model changed without the stock widget (undo, an edit group, a builder's edit, a reload): FDreamUndoHistory
	 *  recorded it; the widget follows. */
	void HandleModelChanged(const dream::ChangeSet& Changes, const dream::TextSnapshot& Before);
	/** A batch as one undo group of this view (Replace, Replace All); Group gives the label and the selection after. */
	bool ApplyEditBatchGroup(const dream::EditBatch& Batch, const FDreamEditGroup& Group);
	/** The kind of the stock edit this key or menu action makes (§4.2 R1-R5); read by HandleTextChanged in this frame. */
	void SetPendingKind(dream::UndoKind Kind);
	/**
	 * Pushes one model change into the widget: its ranges inside one stock transaction (scroll and unchanged lines stay),
	 * or the whole text for a large change or when the widget missed a version. The selection becomes the old one mapped
	 * through the change; a caller with a selection of its own sets it right after.
	 */
	void SyncWidget(const dream::ChangeSet& Changes, const dream::TextSnapshot& Before, const dream::TextSnapshot& After);
	void MapPendingClosers(const dream::ChangeSet& Changes);
	/** Applies a core edit builder's result as one change of Kind and selects what it says (§4.2 R3, R5). False when empty. */
	bool ApplyBuiltEdit(const dream::BuiltEdit& Edit, dream::UndoKind Kind);
	/** The text has line views (it was laid out; the stock vertical moves and the preferred column read them). */
	bool HasLineViews() const;
	/** Takes the preferred column (sticky X, §4.5) from the caret again, keeping the selection. */
	void RefreshStickyX();
	/** Replaces the selection with Text through the model (Backspace, Delete, typing over it); false without a selection. */
	bool ReplaceSelection(const FString& Text, dream::UndoKind Kind);
	/** Ctrl+X, Shift+Delete, the menu's Cut: the selection to the clipboard, then deleted through the model (one step). */
	bool CutSelection();
	/** Ctrl+V, Shift+Insert, the menu's Paste over a selection: replaced through the model (one step). False without one. */
	bool PasteOverSelection();
	dream::Offset ToOffset(const dream::TextSnapshot& Snapshot, int32 Line, int32 Column) const;
	FDreamTextSelection ToSelection(const dream::TextSnapshot& Snapshot, dream::Offset Anchor, dream::Offset Caret) const;
	void SelectionOffsets(const dream::TextSnapshot& Snapshot, dream::Offset& OutStart, dream::Offset& OutEnd) const;
	void DetectIndentStyle();
	/** UDreamUserSettings::FontSize changed: the view follows at once, keeping its zoom (EDT-10). */
	void HandleSettingChanged(UObject* Settings, FPropertyChangedEvent& Event);
	/** (theme) FDreamThemes::OnChanged: the text style again (caret, selection) and a re-marshal (token colours, PO-04). */
	void HandleThemeChanged();
	/** Tab / Shift+Tab in plain mode: spaces to the next 4-column stop without a selection; with any selection, indent /
	 *  dedent every line it touches, keeping the selection on its text, one undo step; Shift+Tab alone dedents the caret's
	 *  line (EDT-03; (editing) PO-06 F1). */
	FReply HandleTab(bool bShift);
	/** (editing) PO-06 F1: selects lines AnchorLine..Line whole, line breaks included, the caret at Line's end of the
	 *  range (a triple-click, and the drag after it; SDreamPlainText::SetOnSelectLines). */
	void SelectWholeLines(int32 AnchorLine, int32 Line);
	/** The context menu: Undo and Redo of this view, then the stock clipboard entries. */
	TSharedPtr<SWidget> BuildContextMenu();
	/** Runs Command through CommandTarget (zoom from the wheel goes where the zoom chords go). */
	void ExecuteThroughTarget(EDreamCommand Command);
	/** A transient line in the owner's status strip. */
	void ShowStatus(const FText& Message);
	/** Re-marshals a moment after typing stops (paint-only timer: no run state, no Python). */
	void ScheduleHighlight();
	EActiveTimerReturnType HighlightTimer(double InCurrentTime, float InDeltaTime);
	FString GetLineText(int32 Line) const;
	FText ReadOnlyReason() const;
	void ApplyFontSize();
	/** The query of the bar as it is now. */
	FDreamFindQuery CurrentFindQuery() const;
	/** The bar's query changed: literals search at once, a regex 150 ms after typing stops (§4.4). */
	void HandleFindQueryChanged();
	/** Searches the buffer; bSelectFirst: select the first match from the anchor (incremental find while typing). */
	void RunFind(bool bSelectFirst);
	void ScheduleFind(double DelaySeconds, bool bSelectFirst);
	EActiveTimerReturnType FindTimer(double InCurrentTime, float InDeltaTime);
	/** A regex search of Query over the snapshot on a UE::Tasks worker (§4.4 Run); a bad pattern shows its error at once. */
	void StartFindTask(const FDreamFindQuery& Query, bool bSelectFirst);
	/** Replace All's enumeration and batch on a worker; applied as one undo step if the text did not change meanwhile. */
	void StartReplaceAllTask(const FDreamFindQuery& Query, const FString& Template, int32 Attempt);
	/** Sets the worker's CancelToken and forgets it (a new query, an edit, closing the bar, the destructor). */
	void CancelFindTask();
	/** The FDreamDeferral ticker that takes the worker's result on the game thread (outside runs, RT-01). */
	void StartFindPoll();
	/** True while the worker of Generation still runs; applies its result once it finished. */
	bool PollFindTask(uint32 Generation);
	/** Non-blocking: starts a search when the matches are stale; true when they answer the current query and text. */
	bool EnsureFindResults();
	/** The index of the match the selection is exactly on, or INDEX_NONE. */
	int32 FindMatchAtSelection() const;
	void SelectFindMatch(int32 Index);
	void UpdateFindStatus();

	TSharedPtr<FDreamBuffer> Buffer;
	TSharedPtr<SDreamPlainText> TextWidget;
	TSharedPtr<SScrollBar> VScrollBar;
	TSharedPtr<SScrollBar> HScrollBar;
	TSharedPtr<FDreamPythonMarshaller> Marshaller;
	TSharedPtr<SDreamGoToLine> GoToLine;
	TWeakPtr<IDreamCommandTarget> CommandTarget;
	TAttribute<bool> ReadOnlyAttribute;
	EDreamGutterMode GutterMode = EDreamGutterMode::LineNumbers;
	uint32 BaseContextMask = 0;
	int32 FontSize = 10;
	int32 SettingsFontSize = 10;
	float WheelZoomAccumulator = 0.f;
	bool bGoToLineOpen = false;

	// Find (EDT-09)
	TSharedPtr<SDreamFindBar> FindBar;
	bool bFindEnabled = false;
	bool bFindBarOpen = false;
	TArray<FDreamFindMatch> FindMatches;          // ascending, in the snapshot of FindVersion
	FDreamFindQuery FindQuery;                    // the query FindMatches answer
	uint64 FindVersion = 0;
	bool bFindCapped = false;
	int32 FindSkippedLines = 0;
	FText FindError;
	int32 CurrentFindMatch = INDEX_NONE;
	dream::Offset FindAnchor = 0;                 // incremental find starts here (the selection when the bar opened)
	FText FindStatus;
	double FindDueSeconds = 0.0;
	bool bFindSelectFirst = false;
	TSharedPtr<FActiveTimerHandle> FindTimerHandle;
	TSharedPtr<FDreamCodeViewFindTask> FindTask;  // the regex search or Replace All on a worker (§4.4 Run)
	uint32 FindGeneration = 0;                   // moves on when the task in flight becomes stale
	int32 PendingFindStep = 0;                   // F3 / Shift+F3 waiting for the worker's matches
	bool bPendingFindStepFocusText = false;      // and whether it gives focus back to the text (Enter in the find box)
	bool bStickyXStale = false;                  // the stock preferred column lags a caret or selection set by code
	FDreamOnCodeViewKey OnKeyHook;
	FSimpleDelegate OnEdited;
	FSimpleDelegate OnCaretMoved;
	FSimpleDelegate OnFocused;
	bool bSettingText = false;

	TAttribute<FText> HintText;
	dream::IndentStyle Indent;
	dream::TextSnapshot SyncedSnapshot;  // the model version the widget shows
	/** Closers that auto-close inserted and that typing the same character steps over (offsets in the model). */
	TArray<dream::Offset> PendingClosers;
	FString ReflectedText;              // the model text as of the last reflection (the diff's "old" side)
	bool bReflecting = false;           // this view is applying an edit to the buffer
	bool bHadFocus = false;
	bool bHighlightPending = false;
	double LastEditSeconds = 0.0;
	TSharedPtr<FActiveTimerHandle> HighlightTimerHandle;
	FDelegateHandle SettingChangedHandle;
	FDelegateHandle ThemeChangedHandle;   // (theme)

	// Undo (EDT-08)
	TSharedPtr<FDreamUndoHistory> History;
	TSharedPtr<FDreamCompletionController> Completion;   // (completion) the popup lifecycle (§7.6.2)
	TSharedPtr<FDreamCodeViewDiagnostics> Diagnostics;   // (lang) PO-06 F4: live problems
	FDelegateHandle ModelChangedHandle;
	FDreamTextSelection LastKnownSelection;      // the selection before the next stock edit (OnCursorMoved follows the edit)
	bool bEditCaretPending = false;             // the next OnCursorMoved is the edit's own, not a navigation (R4)
	dream::UndoKind PendingKind = dream::UndoKind::Typing;
	uint64 PendingKindFrame = 0;
	bool bSwallowBackspaceChar = false;         // a smart Backspace ran on the key-down; its WM_CHAR 8 deletes nothing
	bool bRoutingChildKey = false;              // OnKeyDown routes a key from the find bar or the Go to line box
};
