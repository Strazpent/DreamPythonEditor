// SPDX-License-Identifier: Apache-2.0
// The Problems tab of the output area (TDD §7.8; OUT-11, INT-07, INT-09, INT-11, U13): the merged, deduplicated items of
// FDreamLanguageServices' Problems model (dream-check lint items, Parse items of a not executed run, runtime items), with the
// language-service health line ([Restart] and [View log] when paused), Problems > Clear runtime items, Enter or a double
// click to open an item, and a context menu with the item's quick fixes (one undo step, §7.8.3). Hints are not listed and
// stale items (a crashed server) are greyed. Game thread. lang group (B56).
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FDreamProblem;

/** A REPL or console frame has no tab: the output area scrolls to the run's echo line instead (OUT-05). */
DECLARE_DELEGATE_TwoParams(FDreamOnPromptLocation, int32 /*RunNumber*/, int32 /*Line*/);

class DREAMEDITOR_API SDreamProblemsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamProblemsView) {}
		SLATE_EVENT(FDreamOnPromptLocation, OnOpenPromptLocation)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDreamProblemsView() override;

	/** The tab label's count: the listed items (hints excluded). */
	int32 GetItemCount() const { return Items.Num(); }
	/** The listed items, in list order (tests). */
	TArray<FDreamProblem> GetItemsForTest() const;
	/** Widget is the list, the widget here that takes keyboard focus (SDreamTab's focus home, P4-shl-02). */
	bool IsFocusList(const SWidget& Widget) const { return List.IsValid() && List.Get() == &Widget; }
	/** Opens the item at Index as Enter would (tests). */
	void OpenItemForTest(int32 Index);

private:
	using FItem = TSharedPtr<FDreamProblem>;

	void Refresh();
	void OpenItem(const FItem& Item);
	void ShowFixes(const FItem& Item, const FVector2D& ScreenPosition);
	TSharedPtr<SWidget> MakeContextMenu();
	TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);
	FText GetSummaryText() const;

	TArray<FItem> Items;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TSharedPtr<SListView<FItem>> List;
	FDreamOnPromptLocation OnOpenPromptLocation;
	FDelegateHandle ChangedHandle;
};
