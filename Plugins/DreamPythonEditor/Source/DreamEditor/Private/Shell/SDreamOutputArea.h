// SPDX-License-Identifier: Apache-2.0
// The output area (TDD §6.4 "The output area", WS-02, WS-03, RV-18, PO-01): the tab row Output · REPL · Problems, the tab
// body, and the one REPL prompt strip directly below the body (shown for Output and REPL, hidden for Problems). The Run
// window opens exactly over this widget's geometry (§5.5.3). Game thread. Body: SDreamOutputArea.cpp (chrome).
// (chrome) PO-04, the Maya-minimal look: the REPL and Problems tabs show only while View ▸ REPL Tab / Problems Tab are on,
// and with both off there is no tab row (the history alone; the Filter entries stay in History ▸ Show in Output). The
// prompt strip shows only while View ▸ REPL Prompt is on, or after Focus REPL until another focus home in the tab takes
// focus with the prompt empty. A hidden tab can still be made active from code (SetActiveTab); the View toggles that hide
// the active tab switch the area to Output.
#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Run/DreamRunTypes.h"

class FWidgetPath;
class IDreamCommandTarget;
class SBox;
class SHorizontalBox;
class SDreamOutputView;
class SDreamReplStrip;

enum class EDreamOutputTab : uint8
{
	Output = 0,
	Repl = 1,
	Problems = 2,
};

class DREAMEDITOR_API SDreamOutputArea : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamOutputArea)
		: _InitialTab(EDreamOutputTab::Output)
	{}
		/** The owning SDreamTab: the REPL strip forwards non-prompt commands to it, and both output views run bound chords
		 *  (F4, Shift+F5, Ctrl+Alt+Z, ...) through it while they have focus. */
		SLATE_ARGUMENT(TWeakPtr<IDreamCommandTarget>, CommandTarget)
		SLATE_ARGUMENT(EDreamOutputTab, InitialTab)
		SLATE_EVENT(FSimpleDelegate, OnActiveTabChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamOutputArea() override;

	void SetActiveTab(EDreamOutputTab Tab);
	EDreamOutputTab GetActiveTab() const { return ActiveTab; }

	TSharedPtr<SDreamOutputView> GetOutputView() const { return OutputView; }
	TSharedPtr<SDreamOutputView> GetReplView() const { return ReplView; }
	TSharedPtr<SDreamReplStrip> GetReplStrip() const { return ReplStrip; }
	/** The view the tab's F4 / Shift+F4 walk (U13) and Expand / Fold Echo act on: the REPL view while the REPL tab is
	 *  active, else the Output view (Problems has no walk of its own in slice A). Never null after Construct. */
	TSharedPtr<SDreamOutputView> GetActiveOutputView() const { return ActiveTab == EDreamOutputTab::Repl ? ReplView : OutputView; }

	/** Focus REPL (§6.5): selects the REPL tab when Problems is showing (Output while the REPL tab is hidden), shows the
	 *  prompt strip when View ▸ REPL Prompt is off, then focuses the prompt. */
	void FocusRepl();

	/** (chrome) The tab is in the tab row: Output always, REPL and Problems while their View toggles are on. */
	bool IsTabShown(EDreamOutputTab Tab) const;
	/** (chrome) The tab row is in the layout (REPL or Problems tab on). */
	bool IsTabRowShown() const;
	/** (chrome) The prompt strip is on: View ▸ REPL Prompt, or Focus REPL's reveal (the Problems tab still hides it). */
	bool IsPromptShown() const;
	/** (chrome) Ends Focus REPL's reveal: the strip hides again unless View ▸ REPL Prompt is on; a prompt holding text stays
	 *  unless bEvenIfTyped. */
	void EndPromptReveal(bool bEvenIfTyped);
	/** (chrome) Path's focused leaf is inside the REPL prompt. */
	bool IsPromptPath(const FWidgetPath& Path) const;
	/** (chrome) Follows the View settings (SDreamTab calls it on UDreamUserSettings::OnViewSettingsChanged, then checks where
	 *  keyboard focus is): the tab row is rebuilt only when the REPL or Problems toggle changed. */
	void ApplyViewSettings();

	/** The focused leaf of Path is a focus home here: in the REPL prompt, or the list of the Output, REPL, history or
	 *  Problems view; not a tab-row segment, a button or a link (SDreamTab::OnFocusChanging, P4-shl-02). */
	bool IsFocusHome(const FWidgetPath& Path) const;

	/**
	 * The view that takes over the Run window's live view in finish step 3 (§5.5.3 hand-off): the REPL view for a REPL run
	 * while the REPL tab is active, else the Output view; switches to Output when the active tab cannot show the run.
	 * Game thread; never null after Construct. The supervisor calls GetHandoffView(Route)->ApplyHandoff(
	 * LiveRunView->CaptureHandoff()) before it tears the Run window down.
	 */
	TSharedPtr<SDreamOutputView> GetHandoffView(EDreamRunRoute Route);

	/**
	 * The area's rect in desktop pixels and the DPI scale of its window, for the Run window (§5.5.3 step 4), read at
	 * BeginRun. False when the area is not on screen: a background tab (its content is in no window), a hidden or
	 * minimised window, or a zero-size area. Game thread.
	 */
	bool GetDesktopRect(FSlateRect& OutRect, float& OutDpiScale) const;

private:
	TSharedPtr<SDreamOutputView> OutputView;
	TSharedPtr<SDreamOutputView> ReplView;
	TSharedPtr<SDreamReplStrip> ReplStrip;
	TSharedPtr<SWidget> ProblemsView;
	EDreamOutputTab ActiveTab = EDreamOutputTab::Output;
	FSimpleDelegate OnActiveTabChanged;

	/** §5.5.3 "Tab": a run the active tab cannot show switches the area to Output at run start, before Python runs. */
	void HandleRunStateChanged(const FDreamRunRecord& Record);
	FText GetProblemsLabel() const;
	/** (chrome) The tab row from the View toggles; explicit visibilities, so a toggle applies within the same frame. */
	void RebuildTabRow();
	void RefreshPromptVisibility();

	TSharedPtr<SHorizontalBox> TabRow;
	TSharedPtr<SWidget> TabRowBox;
	TSharedPtr<SBox> PromptBox;
	bool bPromptRevealed = false;
	bool bRowReplTab = false;        // the toggles the tab row was built for
	bool bRowProblemsTab = false;

	TWeakPtr<IDreamCommandTarget> CommandTarget;
	TSharedPtr<SWidget> ReplHistoryList;     // the REPL tab's history list (§6.5 placement)
	FDelegateHandle RunStateHandle;
};
