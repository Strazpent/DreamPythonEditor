// SPDX-License-Identifier: Apache-2.0
// (structure) The Structure section of the side bar (owner ruling PO-05 §3, NAV-02, TDD §7.9.2 without a server): the
// active tab's dream::BuildOutline as a tree, rebuilt 300 ms after the last edit on a worker, with the jump, the caret
// highlight and the filter row. See Shell/SDreamStructureView.h. Game thread (the worker reads only an immutable
// dream::TextSnapshot).
#include "Shell/SDreamStructureView.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamTheme.h"
#include "Tasks/Task.h"
#include "Widgets/Images/SImage.h"   // (icons)
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

struct FDreamStructureItem
{
	int32 Index = INDEX_NONE;       // into SDreamStructureView::Entries
	FText Name;
	FText Kind;
	bool bMatch = true;             // false: shown only because an entry under it matches the filter
	bool bShown = true;             // passes the filter
	/**
	 * The entry's path, one segment per level ("<buffer>|<kind>:<ordinal>:<name length>:<name>/..."): collapsed parents
	 * and the selection are kept by it across rebuilds. The ordinal tells same-kind, same-name siblings apart; the length
	 * prefix keeps the encoding unambiguous whatever a cell title holds ('#', ':', '/').
	 */
	FString Key;
	TArray<TSharedPtr<FDreamStructureItem>> Children;   // the shown children
};

/** One outline build on a worker: the snapshot it reads, its result, and the flag Poll reads on the game thread. */
struct SDreamStructureView::FBuildJob
{
	FGuid BufferId;
	uint64 Version = 0;
	dream::TextSnapshot Snapshot;
	std::vector<dream::OutlineEntry> Result;
	std::atomic<bool> bDone { false };
};

namespace DreamStructureViewPrivate
{
	FText KindText(dream::OutlineKind Kind)
	{
		switch (Kind)
		{
		case dream::OutlineKind::Class: return LOCTEXT("StructureKindClass", "C");
		case dream::OutlineKind::Function: return LOCTEXT("StructureKindFunction", "f");
		case dream::OutlineKind::Method: return LOCTEXT("StructureKindMethod", "m");
		case dream::OutlineKind::AsyncFunction: return LOCTEXT("StructureKindAsyncFunction", "af");
		case dream::OutlineKind::AsyncMethod: return LOCTEXT("StructureKindAsyncMethod", "am");
		case dream::OutlineKind::Cell: return LOCTEXT("StructureKindCell", "#");
		}
		return FText::GetEmpty();
	}

	/** (icons) PO-06 F6: the kind's icon, replacing the letter on screen (the letter stays the item's data). */
	EDreamIcon KindIcon(dream::OutlineKind Kind)
	{
		switch (Kind)
		{
		case dream::OutlineKind::Class: return EDreamIcon::Class;
		case dream::OutlineKind::Method: return EDreamIcon::Method;
		case dream::OutlineKind::AsyncFunction:
		case dream::OutlineKind::AsyncMethod: return EDreamIcon::Async;
		case dream::OutlineKind::Cell: return EDreamIcon::Cell;
		case dream::OutlineKind::Function:
		default: return EDreamIcon::Function;
		}
	}

	FText KindTooltip(dream::OutlineKind Kind)
	{
		switch (Kind)
		{
		case dream::OutlineKind::Class: return LOCTEXT("StructureTipClass", "Class");
		case dream::OutlineKind::Function: return LOCTEXT("StructureTipFunction", "Function");
		case dream::OutlineKind::Method: return LOCTEXT("StructureTipMethod", "Method");
		case dream::OutlineKind::AsyncFunction: return LOCTEXT("StructureTipAsyncFunction", "Async function");
		case dream::OutlineKind::AsyncMethod: return LOCTEXT("StructureTipAsyncMethod", "Async method");
		case dream::OutlineKind::Cell: return LOCTEXT("StructureTipCell", "Cell");
		}
		return FText::GetEmpty();
	}

	bool IsInside(const SWidget& Widget, const TSharedPtr<SWidget>& Root)
	{
		for (const SWidget* At = &Widget; At != nullptr; At = At->GetParentWidget().Get())
		{
			if (At == Root.Get())
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * A tree row whose border (its whole width) is the theme's current-line brush while its entry holds the caret and the
	 * row is neither selected nor hovered: selection and hover keep their own look (as SDreamProjectRow does for the
	 * active file). SBorder draws the brush with its tint, which follows the theme in place.
	 */
	class SRow final : public STableRow<TSharedPtr<FDreamStructureItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SRow) {}
			SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, const TWeakPtr<const SDreamStructureView>& InView,
			int32 InIndex)
		{
			View = InView;
			Index = InIndex;
			STableRow::Construct(STableRow::FArguments()
				.Style(&FDreamThemes::TableRowStyle())   // themed hover and selection, transparent otherwise
				.Padding(FMargin(0.f, 1.f))
				[
					InArgs._Content.Widget
				], Owner);
		}

		virtual const FSlateBrush* GetBorder() const override
		{
			const TSharedPtr<const SDreamStructureView> Pinned = View.Pin();
			if (Pinned.IsValid() && Index != INDEX_NONE && Pinned->GetHighlightedEntry() == Index && !IsSelected() && !IsHovered())
			{
				return FDreamThemes::Brush(EDreamThemeSlot::CurrentLine);
			}
			return STableRow::GetBorder();
		}

	private:
		TWeakPtr<const SDreamStructureView> View;
		int32 Index = INDEX_NONE;
	};
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------------------------------

void SDreamStructureView::Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab)
{
	Tab = InTab;
	const FSlateFontInfo Font = FDreamChrome::Font(9);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(2.f, 2.f, 2.f, 3.f))
		[
			SAssignNew(FilterBox, SEditableTextBox)
			.Visibility_Lambda([this]() { return bFilterOpen ? EVisibility::Visible : EVisibility::Collapsed; })
			.Style(&FDreamThemes::TextBoxStyle())   // themed fill, outline and text, retinted in place
			.Font(Font)
			.HintText(LOCTEXT("StructureFilterHint", "Filter"))
			.ClearKeyboardFocusOnCommit(false)
			.OnTextChanged(this, &SDreamStructureView::HandleFilterChanged)
			.OnKeyDownHandler(this, &SDreamStructureView::HandleFilterKeyDown)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(Tree, STreeView<FItemPtr>)
				.TreeViewStyle(&FDreamThemes::ListViewStyle())   // no panel brush of its own: the side bar shows through
				.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())
				.TreeItemsSource(&RootItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SDreamStructureView::MakeRow)
				.OnGetChildren(this, &SDreamStructureView::GetItemChildren)
				.OnExpansionChanged(this, &SDreamStructureView::HandleExpansionChanged)
				.OnMouseButtonClick(this, &SDreamStructureView::HandleRowClicked)
				// Bound, so a double-click on a parent jumps (again) instead of STableRow toggling its expansion.
				.OnMouseButtonDoubleClick(this, &SDreamStructureView::HandleRowClicked)
				.OnKeyDownHandler(this, &SDreamStructureView::HandleTreeKeyDown)
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(FMargin(8.f, 4.f))
			[
				SNew(STextBlock)
				.Visibility(EVisibility::HitTestInvisible)
				.Font(Font)
				.ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::MutedText))
				.Text_Lambda([this]()
				{
					if (IsShowingNoStructure())
					{
						return LOCTEXT("StructureNone", "No structure");
					}
					return (!Entries.empty() && RootItems.IsEmpty()) ? LOCTEXT("StructureNoMatches", "No matches") : FText::GetEmpty();
				})
			]
		]
	];
}

SDreamStructureView::~SDreamStructureView()
{
	// A worker still running keeps its job alive through its own reference and drops the result.
	Job.Reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Following the active tab
// ---------------------------------------------------------------------------------------------------------------------

void SDreamStructureView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	Poll(InCurrentTime);
}

TSharedPtr<FDreamBuffer> SDreamStructureView::GetActiveBuffer() const
{
	const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
	return Pinned.IsValid() ? Pinned->GetActiveBuffer() : nullptr;
}

bool SDreamStructureView::IsPythonBuffer(const FDreamBuffer& Buffer)
{
	// A buffer with no path (Untitled-N, the REPL input) is Python; one with a path (a file, a scratch file, a recovered
	// copy of a file) is when its extension says so.
	if (!Buffer.HasPath())
	{
		return true;
	}
	const FString Extension = FPaths::GetExtension(Buffer.GetPath());
	return Extension.Equals(TEXT("py"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("pyw"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT("pyi"), ESearchCase::IgnoreCase);
}

void SDreamStructureView::Poll(double Now)
{
	const TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	if (!Buffer.IsValid() || !IsPythonBuffer(*Buffer))
	{
		const FGuid Id = Buffer.IsValid() ? Buffer->GetId() : FGuid();
		if (Id != SeenBufferId || ShownBufferId.IsValid() || !Entries.empty() || Job.IsValid())
		{
			Job.Reset();
			bRebuildDue = false;
			ClearOutline();
		}
		SeenBufferId = Id;
		SeenVersion = 0;
	}
	else
	{
		const FGuid Id = Buffer->GetId();
		const uint64 Version = Buffer->GetModel().Version();
		if (Id != SeenBufferId)
		{
			// A tab switch: build at once. A small text is built here, so the list never shows the other tab's entries.
			SeenBufferId = Id;
			SeenVersion = Version;
			bRebuildDue = false;
			Job.Reset();
			ClearOutline();
			StartBuild(Buffer.ToSharedRef(), Buffer->GetModel().Length() <= SyncBuildMaxUnits);
			CaretLine = INDEX_NONE;
		}
		else
		{
			if (Version != SeenVersion)
			{
				SeenVersion = Version;
				LastChangeSeconds = Now;
				bRebuildDue = true;
			}
			if (Job.IsValid() && !bHoldResultsForTest && Job->bDone.load(std::memory_order_acquire))
			{
				const TSharedPtr<FBuildJob, ESPMode::ThreadSafe> Done = MoveTemp(Job);
				Job.Reset();
				// Applied only while the buffer is still at the snapshot's version; a newer edit has armed another build.
				if (Done->BufferId == Id && Done->Version == Version)
				{
					ApplyOutline(Id, Version, MoveTemp(Done->Result));
				}
				else
				{
					++DroppedCount;
				}
			}
			if (bRebuildDue && !Job.IsValid() && Now - LastChangeSeconds >= RebuildDelaySeconds)
			{
				bRebuildDue = false;
				StartBuild(Buffer.ToSharedRef(), false);
			}
		}
	}

	// The caret highlight, at most every CaretCheckSeconds.
	if (Now >= NextCaretCheckSeconds)
	{
		NextCaretCheckSeconds = Now + CaretCheckSeconds;
		const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
		const TSharedPtr<SDreamCodeView> View = Pinned.IsValid() ? Pinned->GetActiveCodeView() : nullptr;
		const TSharedPtr<FDreamBuffer> ViewBuffer = View.IsValid() ? View->GetBuffer() : nullptr;
		const FGuid ViewId = ViewBuffer.IsValid() ? ViewBuffer->GetId() : FGuid();
		const int32 Line = View.IsValid() ? View->GetCaretLine() : INDEX_NONE;
		if (Line != CaretLine || ViewId != CaretBufferId)
		{
			CaretLine = Line;
			CaretBufferId = ViewId;
			UpdateHighlight();
		}
	}
}

void SDreamStructureView::StartBuild(const TSharedRef<FDreamBuffer>& Buffer, bool bSync)
{
	const FGuid Id = Buffer->GetId();
	const dream::TextSnapshot Snapshot = Buffer->Snapshot();
	if (bSync)
	{
		ApplyOutline(Id, Snapshot.Version(), dream::BuildOutline(Snapshot));
		return;
	}
	TSharedRef<FBuildJob, ESPMode::ThreadSafe> NewJob = MakeShared<FBuildJob, ESPMode::ThreadSafe>();
	NewJob->BufferId = Id;
	NewJob->Version = Snapshot.Version();
	NewJob->Snapshot = Snapshot;
	Job = NewJob;
	++WorkerBuilds;
	// Any thread: BuildOutline reads only the immutable snapshot (DreamCore: no Slate, no UObjects, no Python).
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [NewJob]()
	{
		NewJob->Result = dream::BuildOutline(NewJob->Snapshot);
		NewJob->Snapshot = dream::TextSnapshot();
		NewJob->bDone.store(true, std::memory_order_release);
	});
}

void SDreamStructureView::ApplyOutline(const FGuid& BufferId, uint64 Version, std::vector<dream::OutlineEntry>&& NewEntries)
{
	Entries = std::move(NewEntries);
	ShownBufferId = BufferId;
	ShownVersion = Version;
	const FString BufferKey = DreamText::GuidToHex(BufferId) + TEXT("|");
	TMap<FString, int32> SameNameCount;   // siblings with the same kind and name: ordinals 0, 1, 2, ... in source order
	AllItems.Reset(static_cast<int32>(Entries.size()));
	for (size_t I = 0; I < Entries.size(); ++I)
	{
		const dream::OutlineEntry& E = Entries[I];
		TSharedRef<FDreamStructureItem> Item = MakeShared<FDreamStructureItem>();
		const FString NameText = DreamText::ToFString(E.Name);
		Item->Index = static_cast<int32>(I);
		Item->Kind = DreamStructureViewPrivate::KindText(E.Kind);
		Item->Name = (E.IsCell() && E.Name.empty()) ? LOCTEXT("StructureUntitledCell", "(cell)") : FText::FromString(NameText);
		// A parent comes before its children, so its key is ready. Same-kind, same-name siblings (bare cells, a property's
		// getter and setter, @overload stubs) are told apart by their order among themselves: the ordinal is a numeric field
		// of its own, before the length-prefixed name, so no name can imitate another entry's key.
		const FString Parent = E.Parent >= 0 ? AllItems[E.Parent]->Key + TEXT("/") : BufferKey;
		const FString Sibling = FString::Printf(TEXT("%d:%d:"), static_cast<int32>(E.Kind), NameText.Len()) + NameText;
		const int32 Ordinal = SameNameCount.FindOrAdd(Parent + Sibling)++;
		Item->Key = Parent + FString::Printf(TEXT("%d:%d:%d:"), static_cast<int32>(E.Kind), Ordinal, NameText.Len()) + NameText;
		AllItems.Add(Item);
	}
	++AppliedCount;
	RebuildItems();
}

void SDreamStructureView::ClearOutline()
{
	Entries.clear();
	AllItems.Reset();
	RootItems.Reset();
	ShownBufferId.Invalidate();
	ShownVersion = 0;
	HighlightedEntry = INDEX_NONE;
	if (Tree.IsValid())
	{
		Tree->ClearSelection();
		Tree->RequestTreeRefresh();
	}
}

bool SDreamStructureView::IsBuildDoneForTest() const
{
	return Job.IsValid() && Job->bDone.load(std::memory_order_acquire);
}

bool SDreamStructureView::IsShowingNoStructure() const
{
	// While a large tab's first build runs on the worker, nothing is shown rather than "No structure".
	return Entries.empty() && !(Job.IsValid() && !ShownBufferId.IsValid());
}

// ---------------------------------------------------------------------------------------------------------------------
// Items, rows, expansion
// ---------------------------------------------------------------------------------------------------------------------

void SDreamStructureView::RebuildItems()
{
	if (!Tree.IsValid())
	{
		return;
	}
	TArray<FItemPtr> Selected = Tree->GetSelectedItems();
	const FString SelectedKey = (Selected.Num() == 1 && Selected[0].IsValid()) ? Selected[0]->Key : FString();

	const FString UntitledCell = LOCTEXT("StructureUntitledCell", "(cell)").ToString();   // a bare cell is found by its label
	const std::vector<dream::OutlineMatch> Matches = dream::FilterOutline(Entries, DreamText::ToU16(FilterText), DreamText::ToU16(UntitledCell));
	RootItems.Reset();
	for (const FItemPtr& Item : AllItems)
	{
		Item->Children.Reset();
	}
	for (int32 I = 0; I < AllItems.Num(); ++I)
	{
		const FItemPtr& Item = AllItems[I];
		Item->bShown = Matches[static_cast<size_t>(I)] != dream::OutlineMatch::Hidden;
		Item->bMatch = Matches[static_cast<size_t>(I)] == dream::OutlineMatch::Match;
		if (!Item->bShown)
		{
			continue;
		}
		const int32 Parent = Entries[static_cast<size_t>(I)].Parent;
		(Parent >= 0 ? AllItems[Parent]->Children : RootItems).Add(Item);
	}

	// While filtering every shown parent is open; otherwise the parents the user collapsed stay collapsed.
	bApplyingExpansion = true;
	Tree->ClearExpandedItems();
	FItemPtr Reselect;
	for (const FItemPtr& Item : AllItems)
	{
		if (!Item->Children.IsEmpty())
		{
			Tree->SetItemExpansion(Item, !FilterText.IsEmpty() || !CollapsedKeys.Contains(Item->Key));
		}
		if (!Reselect.IsValid() && !SelectedKey.IsEmpty() && Item->bShown && Item->Key == SelectedKey)
		{
			Reselect = Item;
		}
	}
	bApplyingExpansion = false;
	Tree->RequestTreeRefresh();
	if (Reselect.IsValid())
	{
		Tree->SetSelection(Reselect, ESelectInfo::Direct);
	}
	else
	{
		Tree->ClearSelection();
	}
	UpdateHighlight();
}

TSharedRef<ITableRow> SDreamStructureView::MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const FSlateFontInfo Font = FDreamChrome::Font(9);
	const TWeakPtr<FDreamStructureItem> WeakItem = Item;
	const dream::OutlineEntry* Entry = Entries.size() > static_cast<size_t>(Item->Index) ? &Entries[static_cast<size_t>(Item->Index)] : nullptr;
	const FText Tooltip = Entry != nullptr
		? FText::Format(LOCTEXT("StructureRowTip", "{0}, line {1}"), DreamStructureViewPrivate::KindTooltip(Entry->Kind), FText::AsNumber(Entry->Line + 1))
		: FText::GetEmpty();
	return SNew(DreamStructureViewPrivate::SRow, Owner, SharedThis(this), Item->Index)
		.ToolTipText(Tooltip)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 4.f, 0.f))
			[
				// (icons) PO-06 F6: the kind's icon in place of the letter, tinted at paint (a theme switch shows at once).
				SNew(SBox)
				.WidthOverride(16.f)
				.HeightOverride(16.f)
				[
					SNew(SImage)
					.Image(FDreamIcons::Brush(DreamStructureViewPrivate::KindIcon(Entry != nullptr ? Entry->Kind : dream::OutlineKind::Function)))
					.DesiredSizeOverride(FVector2D(16.0, 16.0))
					.ColorAndOpacity(FDreamIcons::TintAttr(DreamStructureViewPrivate::KindIcon(Entry != nullptr ? Entry->Kind : dream::OutlineKind::Function)))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Item->Name)
				.Font(Font)
				// Read at paint: a parent shown only for a match under it is muted, and a theme switch shows at once.
				.ColorAndOpacity_Lambda([WeakItem]()
				{
					const TSharedPtr<FDreamStructureItem> Pinned = WeakItem.Pin();
					return FDreamChrome::Color(Pinned.IsValid() && !Pinned->bMatch ? EDreamChromeSlot::MutedText : EDreamChromeSlot::Text);
				})
			]
		];
}

void SDreamStructureView::GetItemChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren)
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SDreamStructureView::HandleExpansionChanged(FItemPtr Item, bool bExpanded)
{
	if (bApplyingExpansion || !Item.IsValid())
	{
		return;
	}
	if (FilterText.IsEmpty())
	{
		if (bExpanded)
		{
			CollapsedKeys.Remove(Item->Key);
		}
		else
		{
			CollapsedKeys.Add(Item->Key);
		}
	}
	if (bExpanded)
	{
		// STreeView forgets the expansion of rows under a collapsed parent when it linearizes; put back what the user left.
		bApplyingExpansion = true;
		ReapplyExpansionUnder(Item);
		bApplyingExpansion = false;
	}
	UpdateHighlight();   // a collapsed parent takes the highlight of an entry under it
}

void SDreamStructureView::ReapplyExpansionUnder(const FItemPtr& Item)
{
	for (const FItemPtr& Child : Item->Children)
	{
		if (!Child->Children.IsEmpty())
		{
			const bool bOpen = !FilterText.IsEmpty() || !CollapsedKeys.Contains(Child->Key);
			Tree->SetItemExpansion(Child, bOpen);
			if (bOpen)
			{
				ReapplyExpansionUnder(Child);
			}
		}
	}
}

bool SDreamStructureView::IsEntryShown(int32 Index) const
{
	if (!AllItems.IsValidIndex(Index) || !AllItems[Index]->bShown)
	{
		return false;
	}
	for (int32 Parent = Entries[static_cast<size_t>(Index)].Parent; Parent >= 0; Parent = Entries[static_cast<size_t>(Parent)].Parent)
	{
		if (!Tree.IsValid() || !Tree->IsItemExpanded(AllItems[Parent]))
		{
			return false;
		}
	}
	return true;
}

void SDreamStructureView::UpdateHighlight()
{
	int32 New = INDEX_NONE;
	const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
	const TSharedPtr<SDreamCodeView> View = Pinned.IsValid() ? Pinned->GetActiveCodeView() : nullptr;
	const TSharedPtr<FDreamBuffer> ViewBuffer = View.IsValid() ? View->GetBuffer() : nullptr;
	if (ViewBuffer.IsValid() && ViewBuffer->GetId() == ShownBufferId && !Entries.empty())
	{
		New = dream::FindOutlineEntryAt(Entries, View->GetCaretLine());
		// An entry the filter hides or a collapsed parent covers gives the highlight to its nearest shown parent.
		while (New != INDEX_NONE && !IsEntryShown(New))
		{
			New = Entries[static_cast<size_t>(New)].Parent;
		}
	}
	if (New == HighlightedEntry)
	{
		return;
	}
	HighlightedEntry = New;
	if (New != INDEX_NONE && Tree.IsValid() && !Tree->HasKeyboardFocus())
	{
		Tree->RequestScrollIntoView(AllItems[New]);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Jump
// ---------------------------------------------------------------------------------------------------------------------

void SDreamStructureView::HandleRowClicked(FItemPtr Item)
{
	JumpTo(Item);
}

void SDreamStructureView::JumpTo(const FItemPtr& Item)
{
	const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
	if (!Pinned.IsValid() || !Item.IsValid() || !AllItems.IsValidIndex(Item->Index))
	{
		return;
	}
	const TSharedPtr<FDreamBuffer> Buffer = Pinned->GetActiveBuffer();
	const TSharedPtr<SDreamCodeView> View = Pinned->GetActiveCodeView();
	if (!Buffer.IsValid() || !View.IsValid() || Buffer->GetId() != ShownBufferId || View->GetBuffer() != Buffer)
	{
		return;
	}
	const dream::OutlineEntry& E = Entries[static_cast<size_t>(Item->Index)];
	int32 Line = E.Line;
	int32 Column = E.Column;
	const dream::TextBuffer& Model = Buffer->GetModel();
	if (Model.Version() != ShownVersion)
	{
		// Edited since this outline was built (the rebuild is at most 300 ms away): follow the line through the edits, and
		// find the name on it again when it moved.
		const dream::LineNo Mapped = Model.MapLine(ShownVersion, E.Line);
		Line = Mapped != dream::InvalidLine ? Mapped : FMath::Min(E.Line, Model.LineCount() - 1);
		const std::u16string Text = Buffer->Snapshot().LineText(Line);
		if (static_cast<size_t>(Column) + E.Name.size() > Text.size() || Text.compare(static_cast<size_t>(Column), E.Name.size(), E.Name) != 0)
		{
			const size_t Found = E.Name.empty() ? std::u16string::npos : Text.find(E.Name);
			const size_t FirstNonBlank = Text.find_first_not_of(u" \t\f");
			Column = static_cast<int32>(Found != std::u16string::npos ? Found : (FirstNonBlank != std::u16string::npos ? FirstNonBlank : 0));
		}
	}
	View->SetCaret(Line, Column);
	// Into view: a target below the shown lines lands near the top with its body under it (Run line and advance keeps 3
	// lines of margin, which would leave a def on the last lines); one above lands 3 lines from the top.
	TArray<FDreamLineGeometry> Visible;
	View->GetVisibleLines(Visible);
	const bool bInView = Visible.Num() > 0 && Line >= Visible[0].Line && Line <= Visible.Last().Line;
	if (!bInView)
	{
		const int32 Middle = Visible.Num() > 0 ? (Visible[0].Line + Visible.Last().Line) / 2 : 0;
		View->ScrollToLine(Line, Line >= Middle ? FMath::Max(3, Visible.Num() - 4) : 3);
	}
	View->FocusEditor();
	CaretLine = INDEX_NONE;   // the highlight follows on the next poll
	NextCaretCheckSeconds = 0.0;
}

SDreamStructureView::FItemPtr SDreamStructureView::GetJumpTarget() const
{
	if (Tree.IsValid())
	{
		const TArray<FItemPtr> Selected = Tree->GetSelectedItems();
		if (Selected.Num() == 1 && Selected[0].IsValid() && Selected[0]->bShown)
		{
			return Selected[0];
		}
	}
	for (const FItemPtr& Item : AllItems)
	{
		if (Item->bShown && Item->bMatch && IsEntryShown(Item->Index))
		{
			return Item;
		}
	}
	return nullptr;
}

bool SDreamStructureView::ScrollEntryIntoViewForTest(int32 EntryIndex)
{
	if (!Tree.IsValid() || !AllItems.IsValidIndex(EntryIndex) || !IsEntryShown(EntryIndex))
	{
		return false;
	}
	Tree->RequestScrollIntoView(AllItems[EntryIndex]);
	return true;
}

TSharedPtr<SWidget> SDreamStructureView::GetRowWidgetForTest(int32 EntryIndex) const
{
	if (!Tree.IsValid() || !AllItems.IsValidIndex(EntryIndex))
	{
		return nullptr;
	}
	const TSharedPtr<ITableRow> Row = Tree->WidgetFromItem(AllItems[EntryIndex]);
	return Row.IsValid() ? Row->AsWidget().ToSharedPtr() : nullptr;
}

bool SDreamStructureView::SetEntryExpandedForTest(int32 EntryIndex, bool bExpanded)
{
	if (!Tree.IsValid() || !AllItems.IsValidIndex(EntryIndex) || AllItems[EntryIndex]->Children.IsEmpty())
	{
		return false;
	}
	Tree->SetItemExpansion(AllItems[EntryIndex], bExpanded);   // OnExpansionChanged records it, as for a chevron click
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keys and the filter
// ---------------------------------------------------------------------------------------------------------------------

FReply SDreamStructureView::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	const TCHAR Ch = InCharacterEvent.GetCharacter();
	const bool bCtrl = InCharacterEvent.IsControlDown();
	const bool bAlt = InCharacterEvent.IsAltDown();
	// Control characters (Enter, Backspace, Esc, Tab) and chords stay with the key router; AltGr (Ctrl+Alt) types.
	if (Ch < 0x20 || Ch == 0x7F || bCtrl != bAlt || InCharacterEvent.IsCommandDown() || (Ch == TEXT(' ') && !bFilterOpen))
	{
		return FReply::Unhandled();
	}
	OpenFilter(FilterText + Ch);
	return FReply::Handled();
}

FReply SDreamStructureView::HandleTreeKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.IsControlDown() || KeyEvent.IsAltDown() || KeyEvent.IsCommandDown())
	{
		return FReply::Unhandled();
	}
	const FKey Key = KeyEvent.GetKey();
	if (Key == EKeys::Enter)
	{
		const FItemPtr Target = GetJumpTarget();
		if (Target.IsValid())
		{
			JumpTo(Target);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	if (Key == EKeys::Escape)
	{
		// With no filter Esc bubbles on: the side bar gives focus back to the code view.
		return CloseFilter() ? FReply::Handled() : FReply::Unhandled();
	}
	if (Key == EKeys::BackSpace && bFilterOpen)
	{
		OpenFilter(FilterText.LeftChop(1));
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SDreamStructureView::HandleFilterKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();
	if (Key == EKeys::Escape)
	{
		CloseFilter();
		return FReply::Handled();
	}
	if (Key == EKeys::Enter)
	{
		const FItemPtr Target = GetJumpTarget();
		if (Target.IsValid())
		{
			JumpTo(Target);
		}
		return FReply::Handled();
	}
	if (Key == EKeys::Down && Tree.IsValid())
	{
		const FItemPtr Target = GetJumpTarget();
		if (Target.IsValid() && Tree->GetSelectedItems().IsEmpty())
		{
			Tree->SetSelection(Target, ESelectInfo::OnNavigation);
			Tree->RequestScrollIntoView(Target);
		}
		FSlateApplication::Get().SetKeyboardFocus(Tree, EFocusCause::Navigation);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SDreamStructureView::HandleFilterChanged(const FText& Text)
{
	if (bSettingFilterText)
	{
		return;
	}
	FilterText = Text.ToString();
	RebuildItems();
}

void SDreamStructureView::OpenFilter(const FString& Text)
{
	bFilterOpen = true;
	FilterText = Text;
	if (FilterBox.IsValid())
	{
		bSettingFilterText = true;
		FilterBox->SetText(FText::FromString(Text));
		bSettingFilterText = false;
	}
	RebuildItems();
	if (FilterBox.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(FilterBox, EFocusCause::SetDirectly);
		FilterBox->GoTo(ETextLocation::EndOfDocument);
	}
}

bool SDreamStructureView::CloseFilter()
{
	if (!bFilterOpen)
	{
		return false;
	}
	const TSharedPtr<SWidget> Focused = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetKeyboardFocusedWidget() : nullptr;
	const bool bFocusInFilter = Focused.IsValid() && DreamStructureViewPrivate::IsInside(*Focused, FilterBox);
	bFilterOpen = false;
	FilterText.Empty();
	if (FilterBox.IsValid())
	{
		bSettingFilterText = true;
		FilterBox->SetText(FText::GetEmpty());
		bSettingFilterText = false;
	}
	RebuildItems();
	if (bFocusInFilter && Tree.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(Tree, EFocusCause::SetDirectly);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Focus and test hooks
// ---------------------------------------------------------------------------------------------------------------------

void SDreamStructureView::FocusList()
{
	if (!Tree.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	if (Tree->GetSelectedItems().IsEmpty() && AllItems.IsValidIndex(HighlightedEntry))
	{
		Tree->SetSelection(AllItems[HighlightedEntry], ESelectInfo::Direct);
		Tree->RequestScrollIntoView(AllItems[HighlightedEntry]);
	}
	FSlateApplication::Get().SetKeyboardFocus(Tree, EFocusCause::SetDirectly);
}

TSharedPtr<SWidget> SDreamStructureView::GetFocusWidget() const
{
	return Tree;
}

bool SDreamStructureView::IsFocusWidget(const SWidget& Widget) const
{
	return &Widget == Tree.Get() || DreamStructureViewPrivate::IsInside(Widget, FilterBox);
}

void SDreamStructureView::CollectShown(const FItemPtr& Item, int32 Depth, TArray<FRowForTest>& Out) const
{
	const dream::OutlineEntry& E = Entries[static_cast<size_t>(Item->Index)];
	FRowForTest& Row = Out.AddDefaulted_GetRef();
	Row.Kind = E.Kind;
	Row.Name = DreamText::ToFString(E.Name);
	Row.Line = E.Line;
	Row.Depth = Depth;
	Row.bMatch = Item->bMatch;
	Row.bHighlighted = Item->Index == HighlightedEntry;
	Row.bSelected = Tree.IsValid() && Tree->IsItemSelected(Item);
	if (Tree.IsValid() && Tree->IsItemExpanded(Item))
	{
		for (const FItemPtr& Child : Item->Children)
		{
			CollectShown(Child, Depth + 1, Out);
		}
	}
}

TArray<SDreamStructureView::FRowForTest> SDreamStructureView::GetRowsForTest() const
{
	TArray<FRowForTest> Rows;
	for (const FItemPtr& Item : RootItems)
	{
		CollectShown(Item, 0, Rows);
	}
	return Rows;
}

#undef LOCTEXT_NAMESPACE
