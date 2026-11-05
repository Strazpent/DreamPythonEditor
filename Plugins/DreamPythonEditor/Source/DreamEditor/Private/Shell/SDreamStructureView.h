// SPDX-License-Identifier: Apache-2.0
// (structure) The Structure section of the side bar (owner ruling PO-05 §3, NAV-02 moved up from M2, TDD §7.9.2 without a
// server): the outline of the ACTIVE tab only (dream::BuildOutline: classes, functions, methods, async defs, `# %%` cells),
// as a tree by depth with the kind's icon before each name ((icons) PO-06 F6 replaced the letters C / f / m / af / am / #,
// which stay the items' data). No title of its own (the side bar draws the "Structure" section title), PO-04's look.
//
// Behaviour:
//  - It follows the tab's active buffer and its edits by polling them in Tick (SDreamTab has no active-tab event), so a
//    hidden section does no work. An edit rebuilds the outline 300 ms after the last change, from a snapshot on a
//    UE::Tasks worker; the result is applied on the game thread only while the buffer is still at the snapshot's
//    version. A tab switch builds at once (on the game thread up to 256 Ki units, else on the worker).
//  - A click (or a double-click, which never toggles a parent), or Enter on the selected row, puts the caret on the
//    entry's name in the code view, scrolls it into view and focuses the code view (a stale entry's line is mapped
//    through the edits since its snapshot).
//  - The deepest entry that contains the caret is highlighted with the theme's current-line colour as the row's border,
//    unless the row is selected or hovered (checked at most every 100 ms).
//  - Typing while the tree has focus opens a filter row at the top: case-insensitive, matches keep their parents, Esc
//    clears it and closes the row. Esc with no filter is not handled here: it bubbles to the side bar, which gives focus
//    back to the code view.
//  - No tab, an empty tab, a tab whose path is not .py, .pyw or .pyi, or a Python text with no entry: one muted line,
//    "No structure".
//  - Colours are theme slots read at paint (bound getters and FDreamThemes brushes; UE 5.8 copies an FSlateColor's value,
//    so nothing caches one): a theme switch shows at once.
// Game thread. Body: SDreamStructureView.cpp (structure group). Hosted by the side bar's lower slot (sidebar group).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "dream/Outline.h"

class FDreamBuffer;
class ITableRow;
class SEditableTextBox;
class SDreamTab;
class STableViewBase;
template <typename ItemType> class STreeView;
/** One tree item: an outline entry as the tree shows it (SDreamStructureView.cpp). */
struct FDreamStructureItem;

class DREAMEDITOR_API SDreamStructureView final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamStructureView) {}
	SLATE_END_ARGS()

	/** SNew(SDreamStructureView, TabWeakPtr): the tab whose active buffer the section shows. */
	void Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab);
	virtual ~SDreamStructureView() override;

	/** The quiet time after the last edit before the outline is rebuilt (PO-05 §3). */
	static constexpr double RebuildDelaySeconds = 0.3;
	/** The caret highlight follows the caret at most this often. */
	static constexpr double CaretCheckSeconds = 0.1;
	/** A tab switch builds on the game thread up to this many UTF-16 units (about 5 ms), on the worker beyond. */
	static constexpr int64 SyncBuildMaxUnits = 256 * 1024;

	/** Focus Structure (sidebar): keyboard focus to the tree, its selection on the highlighted entry when it has none. */
	void FocusList();
	/** The widget that takes keyboard focus here (the tree): what Focus Structure focuses (SDreamSideBar::TryFocus). */
	TSharedPtr<SWidget> GetFocusWidget() const;
	/** Widget is this section's tree or its filter box (or inside it). */
	bool IsFocusWidget(const SWidget& Widget) const;

	// ---- Test hooks (Dream.Shell.Structure) ----

	/** One shown row, in tree order (the filter applied, collapsed parents' children left out). */
	struct FRowForTest
	{
		dream::OutlineKind Kind = dream::OutlineKind::Function;
		FString Name;
		int32 Line = 0;          // 0-based
		int32 Depth = 0;
		bool bMatch = true;      // false: shown only as the parent of a match
		bool bHighlighted = false;
		bool bSelected = false;
	};
	TArray<FRowForTest> GetRowsForTest() const;
	/** The outline as applied (every entry, no filter). */
	const std::vector<dream::OutlineEntry>& GetEntries() const { return Entries; }
	/** The "No structure" line is shown. */
	bool IsShowingNoStructure() const;
	/** The buffer and its model version the shown outline was built from (invalid / 0 for none). */
	FGuid GetShownBufferId() const { return ShownBufferId; }
	uint64 GetShownVersion() const { return ShownVersion; }
	/** Outlines applied since construction (each worker or game-thread build that reached the tree). */
	int32 GetAppliedCountForTest() const { return AppliedCount; }
	/** Worker builds started since construction, and whether one is in flight. */
	int32 GetWorkerBuildsForTest() const { return WorkerBuilds; }
	bool IsBuildPendingForTest() const { return Job.IsValid(); }
	/** The worker build in flight has finished (its result waits for the next poll). */
	bool IsBuildDoneForTest() const;
	/** While held, a finished worker result is not taken (a spec edits meanwhile to make it stale). */
	void SetHoldResultsForTest(bool bHold) { bHoldResultsForTest = bHold; }
	/** Worker results dropped because the buffer had moved past their version (a tab switch forgets a build uncounted). */
	int32 GetDroppedCountForTest() const { return DroppedCount; }
	/** The highlighted entry's index in GetEntries() (the deepest entry holding the caret, or its nearest shown parent),
	 *  or INDEX_NONE. Rows read it at paint. */
	int32 GetHighlightedEntry() const { return HighlightedEntry; }
	/** The filter row and its text. */
	bool IsFilterOpen() const { return bFilterOpen; }
	FString GetFilterText() const { return FilterText; }
	/** Clears and closes the filter row as Esc does (a spec's cleanup). */
	void ClearFilterForTest() { CloseFilter(); }
	/** The tree's row widget of entry EntryIndex while the tree has generated one (it is scrolled into the list), else
	 *  null: specs route real pointer events to it. */
	TSharedPtr<SWidget> GetRowWidgetForTest(int32 EntryIndex) const;
	/** Scrolls entry EntryIndex into the list (a short section shows only a few rows), so its row gets generated. False
	 *  when the entry is not shown (filtered out, or under a collapsed parent). */
	bool ScrollEntryIntoViewForTest(int32 EntryIndex);
	/** Expands or collapses entry EntryIndex as its chevron does. False when it has no shown children. */
	bool SetEntryExpandedForTest(int32 EntryIndex, bool bExpanded);

	// ---- SWidget ----

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	/** A printable character that bubbles up from the tree opens the filter row with it (or adds it to the filter). */
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;

private:
	using FItemPtr = TSharedPtr<FDreamStructureItem>;
	struct FBuildJob;

	/** The per-frame poll: active buffer, version, quiet time, worker result, caret. */
	void Poll(double Now);
	/** Builds Buffer's current outline: on the game thread when bSync (applied at once), else on a worker. */
	void StartBuild(const TSharedRef<FDreamBuffer>& Buffer, bool bSync);
	/** Puts NewEntries in the tree (built from BufferId at Version). */
	void ApplyOutline(const FGuid& BufferId, uint64 Version, std::vector<dream::OutlineEntry>&& NewEntries);
	void ClearOutline();
	/** Rebuilds the shown items from Entries and the filter, keeping collapsed parents collapsed and the selection. */
	void RebuildItems();
	/** Recomputes the highlighted entry from the active code view's caret. */
	void UpdateHighlight();
	/** Entry Index is in the tree now: it passes the filter and no parent of it is collapsed. */
	bool IsEntryShown(int32 Index) const;

	TSharedRef<ITableRow> MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void GetItemChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren);
	void HandleExpansionChanged(FItemPtr Item, bool bExpanded);
	/** Sets the expansion of Item's shown descendants from CollapsedKeys (open while filtering). */
	void ReapplyExpansionUnder(const FItemPtr& Item);
	void HandleRowClicked(FItemPtr Item);
	FReply HandleTreeKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	FReply HandleFilterKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	void HandleFilterChanged(const FText& Text);
	/** Opens the filter row with Text and gives it keyboard focus, the caret at the end. */
	void OpenFilter(const FString& Text);
	/** Esc: clears and closes the filter row; false when it was not open. */
	bool CloseFilter();
	/** The selected item, else the first matching row in tree order. */
	FItemPtr GetJumpTarget() const;

	/** Puts the caret on the entry's name in the active code view, scrolls it into view and focuses the code view. */
	void JumpTo(const FItemPtr& Item);
	TSharedPtr<FDreamBuffer> GetActiveBuffer() const;
	static bool IsPythonBuffer(const FDreamBuffer& Buffer);
	void CollectShown(const FItemPtr& Item, int32 Depth, TArray<FRowForTest>& Out) const;

	TWeakPtr<SDreamTab> Tab;
	TSharedPtr<STreeView<FItemPtr>> Tree;
	TSharedPtr<SEditableTextBox> FilterBox;

	std::vector<dream::OutlineEntry> Entries;
	TArray<FItemPtr> AllItems;            // one per entry, same index
	TArray<FItemPtr> RootItems;           // the shown roots (after the filter)
	TSet<FString> CollapsedKeys;          // collapsed parents by item key, kept across rebuilds and tab switches
	FGuid ShownBufferId;
	uint64 ShownVersion = 0;
	int32 AppliedCount = 0;
	int32 WorkerBuilds = 0;
	int32 DroppedCount = 0;
	bool bHoldResultsForTest = false;
	int32 HighlightedEntry = INDEX_NONE;
	bool bApplyingExpansion = false;

	// The active buffer as last seen by Poll.
	FGuid SeenBufferId;
	uint64 SeenVersion = 0;
	double LastChangeSeconds = 0.0;
	bool bRebuildDue = false;
	TSharedPtr<FBuildJob, ESPMode::ThreadSafe> Job;

	// Caret highlight.
	FGuid CaretBufferId;
	int32 CaretLine = INDEX_NONE;
	double NextCaretCheckSeconds = 0.0;

	// Filter.
	bool bFilterOpen = false;
	FString FilterText;
	bool bSettingFilterText = false;
};
