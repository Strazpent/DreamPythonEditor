// SPDX-License-Identifier: Apache-2.0
// The Dream tab body (TDD §6.4 layout, §11.6.1, WS-01..WS-03, WS-05 subset, BUF-01..BUF-03; PO-04 point 1, the
// Maya-minimal chrome): a slim menu bar (File, Edit, History, Command, View, Help) and a row of small flat icon buttons,
// the slim banner lines, then a vertical SSplitter with the output area (top by default, 40%, min 120 px each) and the
// code area (the input tab row with the caret's line:col at its right, then the active code view), then the status line
// (run state with U2 names, namespace, cursor, journal age), shown only while View ▸ Status Line is on. Every colour is
// the current theme's (Shell/DreamChrome.h). Executes every buffer, run, output and View command.
// Game thread. Body: SDreamTab.cpp, SDreamTabToolbar.cpp, SDreamStatusStrip.cpp, SDreamEditorTabs.cpp (chrome),
// SDreamTabRecovery.cpp (persist), SDreamTabFiles.cpp (buffers: the file bars and the RUN-26 hint chip).
//
// Contract the specs read (Dream.Shell.TabLayout, Dream.Persistence.Journal; CR-35):
// - (sidebar, PO-05) Under the banners, an outer horizontal SSplitter (GetOuterSplitter) holds the side bar (GetSideBar:
//   Project above Structure; sized to its width, collapsed with both View toggles off) and the centre column.
// - The centre column is one vertical SSplitter (CentreSplitter) with exactly two slots: GetOutputArea() and the code area
//   (the input tab row and the active SDreamCodeView). Each slot has MinSize 120 (WS-02). Top puts the
//   output area in slot 0, Bottom in slot 1; a swap keeps each area's size value (WS-03).
// - The tab follows UDreamUserSettings::OnSettingChanged for OutputPosition (and bEchoInput): a PostEditChangeProperty on
//   the settings object moves the areas at once; SetOutputPosition also saves the setting.
// - NewScratchTab activates the new buffer. CloseBuffer and the CloseTab command never show a modal prompt: a non-empty
//   scratch buffer stays reopenable with Ctrl+Shift+T (§10.2.5).
#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/UIAction.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Commands/DreamKeyRouter.h"
#include "Run/DreamRunTypes.h"
#include "Settings/DreamUserSettings.h"

class FMenuBuilder;
class FDreamBuffer;
class SDockTab;
class SSplitter;
class SDreamCodeView;
class SDreamOutputArea;
class SDreamOutputView;
class SDreamSideBar;   // (sidebar)
class UObject;
struct FPropertyChangedEvent;
struct FDreamRecoveredSession;
enum class EDreamPythonState : uint8;
enum class EDreamRecoveryRowAction : uint8;

class DREAMEDITOR_API SDreamTab : public SCompoundWidget, public IDreamCommandTarget
{
public:
	SLATE_BEGIN_ARGS(SDreamTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SDockTab>& InOwnerTab);
	virtual ~SDreamTab() override;

	// ---- Buffers and editor tabs ----

	TSharedPtr<FDreamBuffer> GetActiveBuffer() const;
	TSharedPtr<SDreamCodeView> GetActiveCodeView() const;
	TSharedPtr<SDreamCodeView> FindCodeView(const FGuid& BufferId) const;
	void ActivateBuffer(const FGuid& BufferId);
	/** Ctrl+T, new_tab(): a scratch tab with an optional pre-allocated ID (§12.1.1 new_tab returns it before the tab exists).
	 *  (scratch, PO-05) Without a Name it is a scratch file, scratch_N.py in the scratch folder, saved as you type (the
	 *  journaled Untitled-N only when that folder cannot be written, with one warning); with a Name it is the journaled
	 *  in-memory scratch buffer of that name, as before (internal callers and the specs' DreamT4-* tabs). */
	TSharedRef<FDreamBuffer> NewScratchTab(const FString& Text = FString(), const FString& Name = FString(), const FGuid& Id = FGuid());
	/**
	 * Opens or focuses a real path or a "<dream:...#id>" pseudo file at a 1-based line and column (0 = keep): traceback
	 * links (§6.4), open() (§11.7), Dream.Open. A closed pseudo-file buffer offers Reopen tab; false when unresolved.
	 */
	bool OpenLocation(const FString& PathOrPseudoFile, int32 Line, int32 Column, const FGuid& PreallocatedId = FGuid());

	TSharedPtr<SDreamOutputArea> GetOutputArea() const { return OutputArea; }
	/** (chrome) The dock tab (DreamEditor.Main) this body is in, wherever the layout docked it. */
	TSharedPtr<SDockTab> GetOwnerTab() const { return OwnerTab.Pin(); }

	/** (sidebar, PO-05) The left side bar (Project above Structure), in the left slot of the outer horizontal splitter whose
	 *  right slot is the centre column (CentreSplitter: the output and code areas). */
	TSharedPtr<SDreamSideBar> GetSideBar() const;
	/** (sidebar) The outer splitter: slot 0 the side bar (sized to its width, collapsed with both sections off), slot 1 the
	 *  centre column. */
	TSharedPtr<SSplitter> GetOuterSplitter() const;

	/** WS-03: swaps the splitter slots at once, keeping each slot's size; saves the setting. */
	void SetOutputPosition(EDreamOutputPosition Position);

	/** A transient message in the status strip (refusal reasons, "End of buffer", "Wrapped to the first error"). */
	void ShowStatusMessage(const FText& Message);

	/** The widget to refocus after a run (§5.5.2). */
	TSharedPtr<SWidget> GetFocusToRestore() const;

	/** Applies the startup restore (§10.3): tabs from the ini or the journal SESSION, and shows the recovery banner. */
	void RestoreSession();

	/** Writes the clean-shutdown session to UDreamPerProjectUserSettings (§10.3): now, or with the 30 s debounce. */
	void SaveSessionState(bool bNow);

	/** Every open editor tab, in strip order. */
	TArray<TSharedRef<FDreamBuffer>> GetTabBuffers() const;

	// ---- (persist) Recovery actions, the Review panel and safe mode (§10.7.1-§10.7.3, §10.8): SDreamTabRecovery.cpp ----

	/** Restore (one undoable edit, dirty, nothing on disk) or Restore as new tab of one pending buffer (§10.7.3). */
	void RestoreRecovered(const FGuid& BufferId, bool bAsNewTab);
	/** Discard: the recovered texts move to local history as "discarded" entries, then resolve (§10.7.3, U30). */
	void DiscardRecovered(const TArray<FGuid>& BufferIds);
	/** Compare (M1): the read-only unified diff under the pending bar and the Review row; again closes it. */
	void ToggleRecoveryCompare(const FGuid& BufferId);
	void ToggleRecoveryReview();
	bool IsRecoveryReviewOpen() const;
	/** Leave safe mode (§10.8): pending = 0, the skipped restore applied now, the deferred services started. */
	void LeaveSafeMode();
	bool IsInSafeMode() const;
	/** The buffers waiting under "Not restored yet", in the order they were recovered. */
	TArray<FGuid> GetPendingRecoveries() const;
	/** Test hooks (Dream.Persistence.Recovery): a recovered session applied as the startup restore applies one, and safe mode
	 *  entered over one, on the editor's own buffers; the specs close what they open. */
	void ApplyRecoveryForTest(FDreamRecoveredSession&& Session);
	void EnterSafeModeForTest(FDreamRecoveredSession&& Session);
	/** (tests) True while ApplyRestore opens a session's buffers (the startup restore, Leave safe mode, the test hook
	 *  above): the tier-4 hygiene counts those opens as the developer's, never as a test's (DreamEditorTests). GT. */
	static bool IsApplyingRestoreForTest();
	FText GetPendingBarTextForTest() const;

	// ---- (buffers) Recent Files, the file bars and the RUN-26 hint chip: SDreamTabFiles.cpp ----

	/** File ▸ Recent Files (BUF-03 M1): opens or focuses Path; a file that no longer exists leaves the list with a status
	 *  message. False when nothing opened. */
	bool OpenRecentFile(const FString& Path);

	/** Test hooks (Dream.Buffers.*): the messages of the active buffer's file bars (§10.15 changed or deleted on disk, a failed
	 *  save, BUF-05 M1, BUF-06), top to bottom, and the labels of their buttons in order; InvokeFileBarActionForTest presses
	 *  button Index (false when there is none). */
	TArray<FText> GetFileBarMessagesForTest() const;
	TArray<FText> GetFileBarActionsForTest() const;
	bool InvokeFileBarActionForTest(int32 Index);
	/** The RUN-26 hint chip (§4.9.4) would show on this frame; its text with the chord as bound now. */
	bool IsRunLineHintShownForTest() const { return IsRunLineHintShown(); }
	FText GetRunLineHintTextForTest() const;
	/** A click on the chip: hidden for good in this project (T-KEY-018). */
	void ClickRunLineHintForTest() { DismissRunLineHint(); }

	// ---- (scratch, PO-05) Scratch files and the sample scripts: SDreamTabFiles.cpp ----

	/** Help ▸ Restore Sample Scripts: the samples missing in <Scratch folder>/Samples are copied again (never over an existing
	 *  file), with a status message. Returns the number copied, -1 when the folder cannot be written. */
	int32 RestoreSampleScripts();
	/** Test hook: the "Scratch folder is not writable" warning shows again for the next fallback. */
	static void ResetScratchWarningForTest();

	// ---- (chrome) The menu bar, the icon row and the View toggles (PO-04 point 1): SDreamTabToolbar.cpp ----

	/** One entry of a chrome menu as the menu builds it: a command's entry, or a named action (Run in transaction, the
	 *  namespace radios, a theme, ...), with the action a click runs. */
	struct FChromeEntry
	{
		FName Name;                                 // the command's stable name, the entry's own ("Dream.Theme.Nord"), or none
		EDreamCommand Command = EDreamCommand::None;  // set for a command entry, and for a named entry that runs one
		FUIAction Action;                           // what a click runs: a named entry's own action, a command's mapping
	};
	/** Test hooks (Dream.Shell.Chrome): the entries of a menu ("File", "Edit", "History", "Command", "View", "Help"; the
	 *  split arrow "Run"; the submenus "RecentFiles", "ShowInOutput", "Theme", "OutputPosition"); the menu bar's titles
	 *  ("DreamChrome.Menu.File", ...). Every icon-row button carries the tag "DreamChrome.<command name>". */
	TArray<FChromeEntry> GetMenuEntriesForTest(FName Menu);
	/** Test hook: the texts the menu's widget shows (labels, and the chords beside them), for entries with no name. */
	TArray<FString> GetMenuLabelsForTest(FName Menu);
	TArray<FName> GetMenuTitlesForTest() const;
	/** A View toggle as the user sees it (ToggleReplTab ... ToggleLineNumbers; the REPL prompt counts Focus REPL's reveal). */
	bool IsViewToggleOn(EDreamCommand Command) const;
	/** The status line is in the layout (View ▸ Status Line). */
	bool IsStatusLineShown() const;
	/** The text at the right of the input tab row: the caret's line:col, and, while the status line is hidden, a health
	 *  warning (PO-04-R1: journal Degraded or off, Python not available, lint paused) in the warning colour, else a
	 *  transient message (refusal reasons, "End of buffer") muted. A click on a warning does its action. */
	FText GetCaretText() const;
	FText GetTabRowMessage() const;
	FText GetTabRowMessageTooltip() const;
	FSlateColor GetTabRowMessageColor() const;
	void ClickTabRowMessage();
	/** The health warning the tab row shows now (None while the status line is shown or everything is fine). */
	enum class EHealthWarning : uint8 { None, JournalDegraded, JournalOff, PythonUnavailable, LintPaused };
	EHealthWarning GetHealthWarning() const;
	/** A banner line closed with × comes back with View ▸ Show Hidden Notices, or when a new line or state appears (PO-04). */
	bool HasHiddenNotices() const;
	void ShowHiddenNotices();
	/** Test hooks (Dream.Shell.Chrome): the recovery banner's lines (a failed Discard adds one the same way), set back. */
	void AddRecoveryLineForTest(const FText& Line);
	void GetRecoveryLinesForTest(TArray<FText>& OutLines, int32& OutEarlierSessionLine, bool& bOutShown) const;
	void SetRecoveryLinesForTest(const TArray<FText>& Lines, int32 EarlierSessionLine, bool bShown);
	/** Test hook: the count line's update as restoring or discarding a pending buffer makes it (unset removes it). */
	void SetEarlierSessionLineForTest(const TOptional<FText>& Text);

	// ---- IDreamCommandTarget ----

	virtual bool CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const override;
	virtual void ExecuteDreamCommand(EDreamCommand Command) override;
	virtual void ShowCommandRefusal(EDreamCommand Command, const FText& Reason) override;

	// ---- SWidget: the tab routes keys that reach it, so they never leak to the Level Editor (§4.9.4, SHL-02) ----

	virtual bool SupportsKeyboardFocus() const override;
	/** Remembers the last focus home inside the tab that took focus (a code view, a view's list), for OnFocusReceived. */
	virtual void OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent) override;
	/** Focus given to the tab itself goes back to the last focus home inside it (a code view, the REPL prompt, or the list of
	 *  the Output, REPL, history or Problems view; tracked in OnFocusChanging) while it is still shown, else on to the active
	 *  code view; the tab keeps it only in the empty state. */
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	/** Builds the request for RunSelectionOrBuffer, RunBuffer, RunLine and RunLineAdvance (§4.5, §5.2.1, RT-14) and moves the
	 *  caret when Submit accepts a RunLineAdvance (§4.5 "When the caret moves"). */
	void SubmitBufferRun(EDreamCommand Command);
	void HandleRunFinished(const FDreamRunRecord& Record);

	// SDreamTab.cpp
	void BuildLayout();
	/** (sidebar) The outer horizontal splitter: the side bar, then the centre splitter. */
	TSharedRef<SWidget> BuildSideBarLayout();
	void BindEvents();
	TSharedRef<SDreamCodeView> FindOrCreateView(const TSharedRef<FDreamBuffer>& Buffer);
	void RefreshTabStrip();
	void HandleBufferOpened(const TSharedRef<FDreamBuffer>& Buffer);
	void HandleBufferClosed(const TSharedRef<FDreamBuffer>& Buffer);
	void HandleSettingChanged(UObject* Settings, FPropertyChangedEvent& Event);
	void ApplyRestore(FDreamRecoveredSession&& Session);
	/** Safe mode (§10.8): no tabs restored, but the skipped session is carried by the journal and kept for Restore tabs. */
	void ApplySafeMode(FDreamRecoveredSession&& Session);
	void RestoreAllPending();
	void DiscardAllPending();
	void RebuildBanners();
	void RebuildBanners_PythonState(EDreamPythonState NewState);
	void CloseBuffer(const FGuid& BufferId);
	void CycleTab(int32 Direction);
	void OpenGoToLine();
	void ToggleCommentInActiveView();
	void SaveActive(bool bSaveAs);
	void OpenFileDialog();
	void UndoLastRunFromToolbar();
	/** The view F4, Shift+F4, Expand Echo and Fold Echo act on: the REPL view while the REPL tab is active, else Output. */
	TSharedPtr<SDreamOutputView> GetActiveOutputView() const;

	// SDreamTabRecovery.cpp (persist)
	void FinishDiscard(const FGuid& BufferId, bool bKept);
	/** One buffer of the skipped session restored from the safe-mode Review (§10.8: every buffer can be restored there). */
	void RestoreFromSkippedSession(const FGuid& BufferId, bool bAsNewTab);
	/** Save over <name>... of a restored "(recovered)" tab (§10.7.2): asks inline, then writes the disk file. */
	void SaveOverRecovered(const FGuid& BufferId, bool bConfirmed);
	/** The pending bar of the active buffer and the Review panel, rebuilt from the recovery state. */
	void RefreshRecoveryUi();
	TSharedPtr<SWidget> BuildPendingBar(const TSharedRef<FDreamBuffer>& Buffer);
	/** A buffer left the pending state (restored, discarded, closed): the banner's count line follows (P3-shl-05). */
	void ForgetPendingRecovery(const FGuid& BufferId);
	// (instances) SDreamTabRecovery.cpp: the other namespaces (§10.5, §10.6, §10.1): orphans and the primary's tabs in Review,
	// the secondary, moved-project, storage and n = 0 banners.
	void AddInstanceBanners();
	void HandleOrphanAction(const FString& Section, const FGuid& BufferId, EDreamRecoveryRowAction Action);
	/** The scan's orphans go to FDreamInstances (their ns.locks with them) and the cross-root scan starts (§10.1). */
	void HandOrphansToInstances(FDreamRecoveredSession& Session);
	void HandleInstancesChanged();

	// SDreamTabFiles.cpp (buffers)
	/** The active buffer's file bars, rebuilt from FDreamBufferManager's disk-change and save-failure state. */
	void RefreshFileBar();
	void HandleFileStateChanged(const TSharedRef<FDreamBuffer>& Buffer);
	/** Saves BufferId to Target (its own path when empty) with the status message of Save; bMakeWritable clears the target's
	 *  read-only attribute first (BUF-05 M1). */
	void SaveBufferTo(const FGuid& BufferId, const FString& Target, bool bMakeWritable);
	/** "Save as UTF-8" (BUF-06): the encoding becomes UTF-8 (the cookie line with it), then the save is retried. */
	void SaveAsUtf8(const FGuid& BufferId, const FString& Target);
	/** The code host with the RUN-26 chip layer over it (§4.9.4). */
	TSharedRef<SWidget> BuildCodeOverlay(const TSharedRef<SWidget>& CodeHostWidget);
	bool IsRunLineHintShown() const;
	FMargin GetRunLineHintPadding() const;
	/** The first RunLineAdvance or a click on the chip: never shown again in this project, saved at once (T-KEY-018). */
	void DismissRunLineHint();

	// SDreamTabFiles.cpp (scratch, PO-05)
	/** NewScratchTab without a name: a scratch file tab, else the journaled Untitled-N with the one warning. */
	TSharedRef<FDreamBuffer> NewScratchFileTab(const FString& Text, const FGuid& Id);
	/** Keyboard focus left a scratch file's code view (to another tab, the output, outside Dream): it saves now. */
	void HandleScratchFocusChange(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath);
	/** After the startup restore and its reconcile: every restored journaled Untitled-N that is not pending becomes a scratch
	 *  file; a pending one follows once its recovery is restored or discarded (PER-03). */
	void MigrateUntitledToScratchFiles(const FDreamRecoveredSession& Session);
	void MigrateUntitledToScratchFile(const FGuid& BufferId);
	/** (scratch, PER-02) Every shown code view's first visible line goes into its buffer's view state, so a scroll made with
	 *  the wheel (no caret move) is saved with the session too. Views never laid out keep the state they were restored with. */
	void RefreshScrollStates();
	/** (scratch, PER-02) A view made for a buffer whose view state has a scroll (a restored tab) scrolls back to that first
	 *  line once it is laid out, unless something moved its caret first (a traceback link). */
	void ScheduleRestoredScroll(const TSharedRef<FDreamBuffer>& Buffer, const TSharedRef<SDreamCodeView>& View, int32 RestoredFirstLine);

	// SDreamTabToolbar.cpp (chrome)
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildMenuBar();
	TSharedRef<SWidget> BuildIconRow();
	TSharedRef<SWidget> MakeMenu(void (SDreamTab::*Fill)(FMenuBuilder&));
	TSharedRef<SWidget> BuildRunMenu();
	TSharedRef<SWidget> BuildFileMenu();
	TSharedRef<SWidget> BuildEditMenu();
	TSharedRef<SWidget> BuildHistoryMenu();
	TSharedRef<SWidget> BuildCommandMenu();
	TSharedRef<SWidget> BuildViewMenu();
	TSharedRef<SWidget> BuildHelpMenu();
	/** The recovery lines now are all among those hidden with × (PO-04-R1). */
	bool IsRecoveryLineHidden() const;
	/** After a View toggle hid the widget that held keyboard focus: focus goes to the active code view. */
	void CheckFocusAfterViewChange(bool bFocusWasInside);
	void FillRunMenu(FMenuBuilder& Menu);
	void FillFileMenu(FMenuBuilder& Menu);
	void FillRecentFilesMenu(FMenuBuilder& Menu);
	void FillEditMenu(FMenuBuilder& Menu);
	void FillHistoryMenu(FMenuBuilder& Menu);
	void FillShowInOutputMenu(FMenuBuilder& Menu);
	/** The Fill function of a menu by its test name (GetMenuEntriesForTest, GetMenuLabelsForTest); null for none. */
	using FMenuFill = void (SDreamTab::*)(FMenuBuilder&);
	static FMenuFill FindMenuFillForTest(FName MenuName);
	void FillCommandMenu(FMenuBuilder& Menu);
	void FillViewMenu(FMenuBuilder& Menu);
	void FillThemeMenu(FMenuBuilder& Menu);
	void FillOutputPositionMenu(FMenuBuilder& Menu);
	void FillHelpMenu(FMenuBuilder& Menu);
	void BindMenuCommands();
	/** Flips a View toggle command's setting (saved, broadcast); the REPL prompt's off also ends Focus REPL's reveal. */
	void ToggleViewSetting(EDreamCommand Command);
	/** UDreamUserSettings::OnViewSettingsChanged: the status line follows (the output area and code views bind it too). */
	void HandleViewSettingsChanged();

	TWeakPtr<SDockTab> OwnerTab;
	TSharedPtr<SSplitter> CentreSplitter;
	TSharedPtr<SDreamOutputArea> OutputArea;
	TSharedPtr<SWidget> CodeArea;

	struct FImpl;
	TSharedPtr<FImpl> Impl;
};
