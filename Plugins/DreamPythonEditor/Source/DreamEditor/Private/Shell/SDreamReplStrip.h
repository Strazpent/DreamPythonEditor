// SPDX-License-Identifier: Apache-2.0
// The one REPL prompt strip below the output area's tab body (TDD §6.5, REPL-01, REPL-02 basic, REPL-06, RV-18, PO-01).
// An SDreamCodeView in prompt gutter mode over the "repl-input" buffer. Enter runs when complete (dream::ReplPreCheck, then
// classify_input at the next idle tick), Shift+Enter newline, Ctrl+Enter always runs, Up at an empty prompt or Ctrl+Up
// opens history, Esc clears (Ctrl+Z restores). Game thread; never calls Python from a key handler (D14(5)).
// FDreamPython::ClassifyInput is called only from an FDreamDeferral ticker with bNeedsPython true (INV-6); a helper error
// counts as "complete", so the input is submitted and the run reports the problem. Body: SDreamReplStrip.cpp (output slice).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Commands/DreamKeyRouter.h"
#include "Containers/Ticker.h"
#include "Output/DreamReplHistory.h"

class FDreamBuffer;
class IMenu;
class SDreamCodeView;

enum class EDreamReplSubmit : uint8
{
	Auto,               // Enter: the completeness decision of §6.5
	Force,              // Ctrl+Enter: submit even if incomplete (a SyntaxError ends not executed, RUN-16)
};

class DREAMEDITOR_API SDreamReplStrip : public SCompoundWidget, public IDreamCommandTarget
{
public:
	static constexpr int32 MaxAutoHeightLines = 12;     // proposed, §6.5
	static constexpr int32 TypeAheadMaxKeys = 64;

	SLATE_BEGIN_ARGS(SDreamReplStrip) {}
		/** Commands that are not prompt keys (Stop, Clear, zoom, tab commands) go to the owning tab. */
		SLATE_ARGUMENT(TWeakPtr<IDreamCommandTarget>, ForwardTarget)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Submits the prompt as a REPL run (route Repl, Session, auto-print, statement mode, transaction "Dream: REPL #N").
	 *  On acceptance the prompt clears into history (also for not executed); a refused submission keeps its text. */
	void Submit(EDreamReplSubmit Mode);
	void OpenHistory(const FString& Filter, EDreamHistoryMatch Match);
	void FocusPrompt();
	/** Puts a history entry in the prompt without running it, as one stock undo step (§6.5 popup and history list). */
	void PutInPrompt(const FString& Text);
	TSharedPtr<SDreamCodeView> GetPromptView() const { return PromptView; }

	// IDreamCommandTarget
	virtual bool CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const override;
	virtual void ExecuteDreamCommand(EDreamCommand Command) override;
	virtual void ShowCommandRefusal(EDreamCommand Command, const FText& Reason) override;

	/** Characters typed while a decision is pending reach here (the prompt is read-only then) and join the type-ahead. */
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	/** Tunnels before the prompt's own handling: a click maps its position through the stock text layout's line views,
	 *  so they are brought up to date first (RefreshPromptLayout). Never handles the click. */
	virtual FReply OnPreviewMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual ~SDreamReplStrip() override;

	/** True while a REPL Enter waits for classify_input (at most one idle tick, §6.5). */
	bool IsDecisionPending() const { return bClassifyPending; }

	/** Test hooks (PO-04, review r6 M2): the history popup opened on these entries (the history store is not read) and
	 *  returned, and closed again. */
	TSharedPtr<SWidget> OpenHistoryForTest(const TArray<FDreamHistoryEntry>& Entries);
	void CloseHistoryForTest() { CloseHistory(); }
	/** (chrome) Test hook (Dream.Shell.Chrome): the placeholder the prompt shows now (PO-04: "Python" when idle). */
	FText GetHintTextForTest() const { return GetHintText(); }

private:
	/** Opens the history popup on Entries (not empty) above the strip and focuses its list; returns the popup. */
	TSharedPtr<SWidget> OpenHistoryWith(const TArray<FDreamHistoryEntry>& Entries);
	/** The hard-wired prompt keys (§6.5 table), as SDreamCodeView's OnKeyHook. */
	FReply HandlePromptKey(const FKeyEvent& KeyEvent);

	/** The Enter decision of §6.5 on the last line (the flowchart). */
	void HandleEnter();
	void QueueClassify(const FString& Source);
	/** The FDreamDeferral body (bNeedsPython: idle, no pump, Python Ready, within the helper budget) that calls
	 *  FDreamPython::ClassifyInput and makes the decision; false when done. */
	bool TickClassify();
	/** The Python-free companion tick: ends the decision as a submission when Python cannot run or the boot takes longer
	 *  than 10 s, because the classify tick never runs while Python is not Ready; false when done. */
	bool TickClassifyWait();
	/** Unregisters both decision tickers (safe from inside either body). */
	void RemoveDecisionTickers();
	void FinishDecision(bool bSubmit);
	/** Replays the keys and characters typed while the decision was pending, in order. */
	void ReplayTypeAhead();
	void SubmitText(const FString& Text);
	/**
	 * (output choice) Lays the prompt's stock text out now, with a prepass of the prompt. The stock layout moves the caret
	 * by its line views (FSlateEditableTextLayout's vertical moves, line starts and ends, pages, clicks), and a whole-text
	 * set (SDreamCodeView::SetTextNoUndo, a whole-text sync) rebuilds them only at the next prepass: a key or click in the
	 * same frame, such as a replayed type-ahead key right after Submit's clear or two keys of one slow frame, would read
	 * the old text's views (an ensure, then an out-of-range index; crash UECC-...D711643D, 01:32).
	 */
	void RefreshPromptLayout();
	void InsertNewlineAndIndent();
	void ClearPromptUndoable();
	void CloseHistory();
	bool IsPromptReadOnly() const;
	FText GetHintText() const;
	FString GetPromptText() const;

	TSharedPtr<SDreamCodeView> PromptView;
	TWeakPtr<IDreamCommandTarget> ForwardTarget;
	bool bClassifyPending = false;
	/** Type-ahead (§6.5): key-downs as typed; characters as FKeyEvent with EKeys::Invalid and the character code. */
	TArray<FKeyEvent> TypeAhead;

	TSharedPtr<FDreamBuffer> Buffer;
	FString PendingSource;
	double PendingSince = 0.0;
	FTSTicker::FDelegateHandle ClassifyTicker;
	FTSTicker::FDelegateHandle ClassifyWaitTicker;
	TSharedPtr<IMenu> HistoryMenu;
	float LineHeight = 16.f;
};
