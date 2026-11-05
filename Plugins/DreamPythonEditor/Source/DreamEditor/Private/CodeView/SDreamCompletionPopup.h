// SPDX-License-Identifier: Apache-2.0
// The completion popup (TDD §7.6.2, D6, D8, REPL-03, INT-03) for the code view and the REPL prompt: SDreamCompletionPopup
// draws a merged list (kind, label, detail, the U4 badge, and a footer such as "Runtime results paused while code runs"),
// and FDreamCompletionController runs its lifecycle for one SDreamCodeView: triggers (an identifier character, '.', a quote
// in an unreal-aware string, the Complete chord Ctrl+Space; never Ctrl+Tab), the first answer from the C++ caches in the
// same keystroke and the resolver's answer at the next idle tick merged under the no-reorder rule, local re-filtering of a
// growing prefix, accept with Enter or Tab as ONE undo step "Completion" through the grouped-edit API (EDT-08), and closing
// on Esc, a caret that leaves the word, focus loss and a run start. Game thread; never calls Python (D14(5)): the resolver
// runs only from FDreamCompletionService's idle ticker. In the REPL prompt the controller sees keys before the prompt's own
// Enter (SDreamCodeView runs its HandleKeyDown before OnKeyHook), so Enter accepts a completion there (§6.5 first row).
// The popup is a Slate menu pushed without focus (T-KEY-007: popups are never focused); keys stay in the text.
// Completion group.
// Deferred (M2): completion mode Shell (CFG-05, popup only on Tab or Ctrl+Space); the type server's static items,
// completionItem/resolve and the 150 ms "Loading…" row for a slow server (T-MRG-023); signature help (Ctrl+Shift+Space)
// and hover (§7.7).
#pragma once

#include "CoreMinimal.h"
#include "Completion/DreamCompletionService.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "dream/CompletionMerge.h"

class IMenu;
class ITableRow;
class STableViewBase;
class SDreamCodeView;
struct FDreamRunRecord;
template <typename ItemType> class SListView;

/** One row of the popup (copied out of the merged list as display text). */
struct FDreamCompletionRow
{
	FText Label;
	FText Detail;
	FText Badge;
	FText KindGlyph;
	int32 Index = 0;
};

DECLARE_DELEGATE_OneParam(FDreamOnCompletionRowClicked, int32 /*Index*/);

class DREAMEDITOR_API SDreamCompletionPopup : public SCompoundWidget
{
public:
	static constexpr int32 VisibleRows = 12;

	SLATE_BEGIN_ARGS(SDreamCompletionPopup) {}
		SLATE_ARGUMENT(FSlateFontInfo, Font)
		SLATE_EVENT(FDreamOnCompletionRowClicked, OnRowClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Replaces the rows; Selected is the highlighted row (-1 none); Footer is empty or the status line. */
	void SetList(const dream::MergedList& List, int32 Selected, const FText& Footer);
	void SetSelected(int32 Index);
	int32 GetSelected() const { return Selected; }
	int32 GetNumRows() const { return Rows.Num(); }
	FText GetFooter() const { return Footer; }
	/** The label and badge texts of a row (tests). */
	FString GetRowLabel(int32 Index) const;
	FString GetRowBadge(int32 Index) const;

private:
	using FRowPtr = TSharedPtr<FDreamCompletionRow>;
	TSharedRef<ITableRow> MakeRow(FRowPtr Row, const TSharedRef<STableViewBase>& Owner);

	TArray<FRowPtr> Rows;
	TSharedPtr<SListView<FRowPtr>> List;
	FSlateFontInfo Font;
	FText Footer;
	int32 Selected = -1;
	FDreamOnCompletionRowClicked OnRowClicked;
};

class DREAMEDITOR_API FDreamCompletionController : public TSharedFromThis<FDreamCompletionController>
{
public:
	enum class EState : uint8
	{
		Closed,
		Requested,          // waiting for the first answer with items (§7.6.2 state diagram)
		Open,
	};

	static constexpr int32 MinChars = 1;                    // Completion.MinChars (§7.6.2, proposed)
	static constexpr int32 MaxItems = 1000;                 // (completion choice) longer merged lists are cut

	static TSharedRef<FDreamCompletionController> Create(const TSharedRef<SDreamCodeView>& View);
	~FDreamCompletionController();

	// ---- Hooks SDreamCodeView calls (each marked "(completion)" there) ----

	/** First in the text's key-down handler, before OnKeyHook and the key router. True when the key was the popup's (Up,
	 *  Down, PageUp, PageDown, Enter, Tab, Esc while open) or the Complete chord (Ctrl+Space by default, §4.9.4). */
	bool HandleKeyDown(const FKeyEvent& KeyEvent);
	/** First in the view's key-up handler: true for the key-up of a key-down HandleKeyDown consumed (§4.9.1: the key-ups
	 *  of consumed key-downs are consumed too, so none reaches the Level Editor). */
	bool HandleKeyUp(const FKeyEvent& KeyEvent);
	/** First in the text's character handler. True swallows the character (the space of Ctrl+Space); otherwise it notes
	 *  the character the next edit inserts (the typing triggers). */
	bool OnCharTyped(const FCharacterEvent& CharacterEvent);
	/** After every model change the view shows (typing, a pair it auto-closed, undo, a reload). */
	void OnTextEdited();
	void OnCaretMoved();
	/** Focus left the text: closes unless it went into the popup (a click on a row). */
	void OnFocusLost(const FWidgetPath& NewFocusPath);
	/** The view is going away. */
	void Detach();

	// ---- State and commands (tests drive these too) ----

	/** Opens or refreshes the popup at the caret (bExplicit: Ctrl+Space, which also opens on an empty prefix). */
	void Trigger(bool bExplicit);
	/** Accepts row Index (the selection when INDEX_NONE) as one undo step "Completion"; false when nothing was inserted. */
	bool Accept(int32 Index = INDEX_NONE);
	void Close();
	EState GetState() const { return State; }
	bool IsOpen() const { return State == EState::Open; }
	/** A live request is queued for the resolver (its answer merges in at an idle tick). */
	bool IsWaitingForLive() const { return bWaitingLive; }
	const dream::MergedList& GetList() const { return List; }
	const dream::CompletionChain& GetChain() const { return Chain; }
	/** The labels of the merged list, in order (tests). */
	TArray<FString> GetLabels() const;
	int32 GetSelected() const { return List.Items.empty() ? INDEX_NONE : List.SelectedIndex; }
	void SetSelected(int32 Index);
	FText GetFooter() const;
	TSharedPtr<SDreamCompletionPopup> GetPopup() const { return Popup; }
	/** The popup is on screen (a menu window); false while the view is not shown, where only the list exists. */
	bool IsPopupShown() const { return Menu.IsValid(); }

private:
	FDreamCompletionController() = default;
	void Bind(const TSharedRef<SDreamCodeView>& View);

	/** The chain at the caret of the view (and the caret itself); false when the view has no buffer. */
	bool ReadChain(dream::CompletionChain& OutChain, int32& OutLine, int32& OutColumn) const;
	bool HandleKeyDownImpl(const FKeyEvent& KeyEvent);
	/** Re-merges the full candidate set with the current prefix (a new prefix: nothing pinned). */
	void Rebuild();
	void HandleLiveAnswer(const FDreamCompletionAnswer& Answer, FString Key);
	/** The typing decision after an edit, once its caret is final (a one-shot active timer on the view). */
	void ScheduleEvaluate();
	EActiveTimerReturnType HandleEvaluateTimer(double Now, float Delta);
	void EvaluateAfterEdit();
	void ShowOrUpdate();
	void ShowMenu();
	void DismissMenu();
	void HandleRowClicked(int32 Index);
	void HandleRunState(const FDreamRunRecord& Record);
	void HandleMenuDismissed(TSharedRef<IMenu> DismissedMenu);
	bool IsCompleteChord(const FKeyEvent& KeyEvent) const;
	dream::MergeContext MakeContext(int32 SelectedIndex) const;

	TWeakPtr<SDreamCodeView> View;
	TSharedPtr<SDreamCompletionPopup> Popup;
	TSharedPtr<IMenu> Menu;
	EState State = EState::Closed;

	dream::CompletionChain Chain;        // the chain the list answers
	FString ChainKey;                   // namespace + chain key of the live request
	int32 SessionLine = 0;              // the caret line when the list opened
	int32 LastColumn = 0;               // the caret column the list was last built for
	FDreamCompletionQuery Query;
	FDreamCandidateListPtr Live;         // the full live candidate set for Chain (unfiltered)
	bool bLiveIncomplete = false;
	bool bRootBound = false;
	bool bLivePaused = false;
	FString RuntimeType;
	dream::MergedList List;
	uint64 LiveTicket = 0;
	bool bWaitingLive = false;
	bool bExplicit = false;

	TCHAR LastTyped = 0;
	uint64 LastTypedFrame = MAX_uint64;
	uint64 LastEditFrame = MAX_uint64;
	TCHAR PendingTyped = 0;             // the character of the edit EvaluateAfterEdit decides on
	bool bEvaluatePending = false;
	bool bCompleteChordDown = false;    // the Complete chord's key-down ran: its WM_CHAR space is swallowed
	TSet<FKey> ConsumedKeys;            // keys whose key-down this controller consumed; their key-ups are consumed too
	bool bAccepting = false;
	FDelegateHandle RunStateHandle;
};
