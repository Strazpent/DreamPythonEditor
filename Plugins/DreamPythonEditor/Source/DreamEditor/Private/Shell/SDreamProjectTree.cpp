// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 2) The Project tree (Shell/SDreamProjectTree.h).
#include "Shell/SDreamProjectTree.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamFileWatch.h"
#include "Buffers/DreamScratch.h"
#include "CodeView/SDreamCodeView.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Persistence/DreamJournal.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamProjectTreeSettings.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamIcons.h"   // (icons)
#include "Shell/DreamProjectFiles.h"
#include "Shell/DreamTheme.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamProjectTreeLocal
{
	/** (icons) PO-06 F6: the icon before a row's label. False for the rows that have none (placeholders, "No project roots"). */
	bool NodeIcon(const FDreamProjectNode& Node, bool bExpanded, EDreamIcon& OutIcon)
	{
		using EKind = FDreamProjectNode::EKind;
		switch (Node.Kind)
		{
		case EKind::Group:
			OutIcon = Node.Key == TEXT("group:engine") ? EDreamIcon::Engine : EDreamIcon::ProjectPlugins;
			return true;
		case EKind::Scratches:
			OutIcon = EDreamIcon::Scratches;
			return true;
		case EKind::Root:
		case EKind::Folder:
		{
			const TSharedPtr<FDreamProjectNode> Parent = Node.Parent.Pin();
			if (Node.Kind == EKind::Folder && Parent.IsValid() && Parent->Kind == EKind::Scratches && Node.Name == TEXT("Samples"))
			{
				OutIcon = EDreamIcon::Samples;   // the seeded sample scripts (PO-05 scratch)
				return true;
			}
			OutIcon = bExpanded ? EDreamIcon::FolderOpen : EDreamIcon::FolderClosed;
			return true;
		}
		case EKind::File:
			OutIcon = FPaths::GetExtension(Node.Name).Equals(TEXT("py"), ESearchCase::IgnoreCase) ? EDreamIcon::FilePython : EDreamIcon::File;
			return true;
		case EKind::NewItem:
			OutIcon = Node.bNewFolder ? EDreamIcon::FolderClosed : EDreamIcon::FilePython;
			return true;
		default:
			return false;
		}
	}

	constexpr double MessageSeconds = 8.0;
	constexpr int32 MaxSavedFolders = 1000;
	// PO-05-A1: the groups' saved expansion, verbatim (not paths): Project Plugins starts expanded (its collapse is kept in
	// CollapsedRoots), Engine starts collapsed (its expansion is kept in ExpandedFolders).
	const TCHAR* const ProjectGroupSaved = TEXT("DreamGroup:ProjectPlugins");
	const TCHAR* const EngineGroupSaved = TEXT("DreamGroup:Engine");

	bool ContainsKey(const TArray<FString>& Paths, const FString& Key)
	{
		return Paths.ContainsByPredicate([&Key](const FString& P) { return FDreamProjectFiles::PathKey(P) == Key; });
	}

	bool RemoveKey(TArray<FString>& Paths, const FString& Key)
	{
		return Paths.RemoveAll([&Key](const FString& P) { return FDreamProjectFiles::PathKey(P) == Key; }) > 0;
	}

	FString LeafName(const FString& Path)
	{
		FString Leaf = FPaths::GetCleanFilename(Path);
		return Leaf.IsEmpty() ? Path : Leaf;   // a drive root ("C:/") names itself
	}

	/** A row whose idle background is the current-line colour while it is the active tab's file (PO-05: the active file
	 *  highlight); selection and hover keep the theme's row brushes. */
	class SDreamProjectRow : public STableRow<FDreamProjectNodePtr>
	{
	public:
		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner, TFunction<bool()> InIsActive)
		{
			IsActive = MoveTemp(InIsActive);
			STableRow<FDreamProjectNodePtr>::Construct(InArgs, InOwner);
		}

		virtual const FSlateBrush* GetBorder() const override
		{
			if (IsActive && !IsSelected() && !IsHovered() && IsActive())
			{
				return FDreamThemes::Brush(EDreamThemeSlot::CurrentLine);   // a brush whose tint follows the theme (A187)
			}
			return STableRow<FDreamProjectNodePtr>::GetBorder();
		}

	private:
		TFunction<bool()> IsActive;
	};
}

void SDreamProjectTree::Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab)
{
	using namespace DreamProjectTreeLocal;
	Tab = InTab;
	bActive = InArgs._StartActive;   // hidden at start: RefreshRoots below builds the rows but lists nothing (review p2 nit)
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	Files = MakeShared<FDreamProjectFiles>(
		[WeakThis](const FDreamFolderListing& Listing)
		{
			if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
			{
				Pinned->ApplyListing(Listing);
			}
		},
		[WeakThis](const TArray<FString>& Folders, const FString& Root, bool bAll)
		{
			if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
			{
				Pinned->HandleChanged(Folders, Root, bAll);
			}
		});
	NoRootsRow = MakeNode(FDreamProjectNode::EKind::NoRoots, FString(), FString(), nullptr);
	ProjectGroup = MakeNode(FDreamProjectNode::EKind::Group, FString(), LOCTEXT("ProjectTree.ProjectPlugins", "Project Plugins").ToString(), nullptr);
	ProjectGroup->Key = TEXT("group:project");
	EngineGroup = MakeNode(FDreamProjectNode::EKind::Group, FString(), LOCTEXT("ProjectTree.EngineGroup", "Engine").ToString(), nullptr);
	EngineGroup->Key = TEXT("group:engine");
	EngineGroup->bReadOnly = true;   // its entries and everything under them (MakeNode passes it down)

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			SAssignNew(TreeView, STreeView<FDreamProjectNodePtr>)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.ProjectTree")))
			.TreeViewStyle(&FDreamThemes::ListViewStyle())     // no list background of its own: the side bar's shows
			.ScrollBarStyle(&FDreamThemes::ScrollBarStyle())
			.SelectionMode(ESelectionMode::Single)
			.TreeItemsSource(&TopItems)
			.OnGenerateRow(this, &SDreamProjectTree::MakeRow)
			.OnGetChildren(this, &SDreamProjectTree::ChildrenOf)
			.OnExpansionChanged(this, &SDreamProjectTree::HandleExpansionChanged)
			.OnMouseButtonDoubleClick(this, &SDreamProjectTree::HandleDoubleClick)
			.OnMouseButtonClick(this, &SDreamProjectTree::HandleClick)
			.OnContextMenuOpening(this, &SDreamProjectTree::MakeContextMenu)
			.OnKeyDownHandler(this, &SDreamProjectTree::HandleTreeKey)
			.HighlightParentNodesForSelection(false)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			// The side bar's message line: failures of file operations, the delete confirm. One 1-px rule above it.
			SNew(SVerticalBox)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.Message")))
			.Visibility_Lambda([this]()
			{
				return !PendingDeletePath.IsEmpty() || (!Message.IsEmpty() && FPlatformTime::Seconds() < MessageUntil)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox).HeightOverride(1.f)[ SNew(SImage).Image(FDreamChrome::Brush(EDreamChromeSlot::Border)) ]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(6.f, 3.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FDreamChrome::Font(8))
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Message; })
					.ColorAndOpacity_Lambda([this]()
					{
						return FDreamChrome::Color(bMessageError ? EDreamChromeSlot::Warning : EDreamChromeSlot::MutedText);
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.ConfirmDelete")))
					.ButtonStyle(&FDreamChrome::FlatButton())
					.IsFocusable(false)
					.Visibility_Lambda([this]() { return PendingDeletePath.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.OnClicked_Lambda([this]() { ConfirmDelete(); return FReply::Handled(); })
					[
						SNew(STextBlock).Font(FDreamChrome::Font(8)).ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
						.Text(LOCTEXT("ProjectTree.ConfirmDelete", "Move to Recycle Bin"))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(&FDreamChrome::FlatButton())
					.IsFocusable(false)
					.Visibility_Lambda([this]() { return PendingDeletePath.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.OnClicked_Lambda([this]() { CancelDelete(); return FReply::Handled(); })
					[
						SNew(STextBlock).Font(FDreamChrome::Font(8)).ColorAndOpacity(FDreamChrome::ColorAttr(EDreamChromeSlot::Text))
						.Text(LOCTEXT("ProjectTree.CancelDelete", "Cancel"))
					]
				]
			]
		]
	];

	// Editor Preferences edits of Project Roots, Add Root / Remove Root and a reload of the roots' config (Reset to Defaults,
	// review p2 nit: UDreamProjectTreeSettings::OnRootsChanged), and the scratch folder moving or being made
	// (DreamScratch::OnFolderChanged), rebuild the top level.
	ProjectSettingsHandle = UDreamProjectTreeSettings::GetMutable()->OnSettingChanged().AddLambda(
		[WeakThis](UObject*, FPropertyChangedEvent& Event)
		{
			const FName Property = Event.GetMemberPropertyName();
			if (Property.IsNone() || Property == GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, ProjectRoots)
				|| Property == GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, bAddPluginPythonFolders))
			{
				if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
				{
					Pinned->RefreshRoots();
				}
			}
		});
	RootsChangedHandle = UDreamProjectTreeSettings::OnRootsChanged().AddSP(this, &SDreamProjectTree::RefreshRoots);
	ScratchFolderHandle = DreamScratch::OnFolderChanged().AddSP(this, &SDreamProjectTree::RefreshRoots);
	if (FDreamEditorModule::IsAvailable())
	{
		BufferOpenedHandle = FDreamEditorModule::Get().GetBufferManager().OnBufferOpened().AddSP(this, &SDreamProjectTree::HandleBufferOpened);
	}
	// PO-05-A1: plugins mounting or dismounting content (as Unreal's Python plugin follows them): rebuilt on the next tick.
	// Only a thread-safe flag is touched there (a mount may be reported off the game thread); Tick reads it.
	MountedHandle = FPackageName::OnContentPathMounted().AddLambda([Flag = AutoRootsDirty](const FString&, const FString&) { Flag->store(true); });
	DismountedHandle = FPackageName::OnContentPathDismounted().AddLambda([Flag = AutoRootsDirty](const FString&, const FString&) { Flag->store(true); });
	RefreshRoots();
}

SDreamProjectTree::~SDreamProjectTree()
{
	if (Files.IsValid())
	{
		Files->Shutdown();
	}
	if (UObjectInitialized())
	{
		UDreamProjectTreeSettings::GetMutable()->OnSettingChanged().Remove(ProjectSettingsHandle);
	}
	UDreamProjectTreeSettings::OnRootsChanged().Remove(RootsChangedHandle);
	DreamScratch::OnFolderChanged().Remove(ScratchFolderHandle);
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetBufferManager().OnBufferOpened().Remove(BufferOpenedHandle);
	}
	FPackageName::OnContentPathMounted().Remove(MountedHandle);
	FPackageName::OnContentPathDismounted().Remove(DismountedHandle);
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------------------------------------------------

FDreamProjectNodePtr SDreamProjectTree::MakeNode(FDreamProjectNode::EKind Kind, const FString& Path, const FString& Name,
	const FDreamProjectNodePtr& Parent) const
{
	FDreamProjectNodePtr Node = MakeShared<FDreamProjectNode>();
	Node->Kind = Kind;
	Node->Path = Path;
	Node->Key = Path.IsEmpty() ? FString() : FDreamProjectFiles::PathKey(Path);
	Node->Name = Name;
	Node->Parent = Parent;
	Node->Depth = Parent.IsValid() ? Parent->Depth + 1 : 0;
	Node->bRemote = Parent.IsValid() && Parent->bRemote;
	Node->bReadOnly = Parent.IsValid() && Parent->bReadOnly;
	if (Node->IsFolderLike())
	{
		Node->Placeholder = MakeShared<FDreamProjectNode>();
		Node->Placeholder->Kind = FDreamProjectNode::EKind::Loading;
		Node->Placeholder->Parent = Node;
		Node->Placeholder->Depth = Node->Depth + 1;
		Node->MoreRow = MakeShared<FDreamProjectNode>();
		Node->MoreRow->Kind = FDreamProjectNode::EKind::More;
		Node->MoreRow->Parent = Node;
		Node->MoreRow->Depth = Node->Depth + 1;
	}
	return Node;
}

FDreamProjectNodePtr SDreamProjectTree::FindNode(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	const FString Key = FDreamProjectFiles::PathKey(Path);
	TArray<TWeakPtr<FDreamProjectNode>> Found;
	FolderNodes.MultiFind(Key, Found);
	for (const TWeakPtr<FDreamProjectNode>& Weak : Found)
	{
		if (const FDreamProjectNodePtr Node = Weak.Pin())
		{
			return Node;
		}
	}
	// A file: among its folder's children.
	Found.Reset();
	FolderNodes.MultiFind(FDreamProjectFiles::PathKey(FPaths::GetPath(Path)), Found);
	for (const TWeakPtr<FDreamProjectNode>& Weak : Found)
	{
		if (const FDreamProjectNodePtr Folder = Weak.Pin())
		{
			for (const FDreamProjectNodePtr& Child : Folder->Children)
			{
				if (Child->Key == Key)
				{
					return Child;
				}
			}
		}
	}
	return nullptr;
}

void SDreamProjectTree::ForgetRows(const FDreamProjectNodePtr& Gone)
{
	// By parentage, not by path: a nested root or the scratch folder shown as a root too keeps its own rows.
	auto IsGone = [&Gone](FDreamProjectNodePtr Node)
	{
		for (; Node.IsValid(); Node = Node->Parent.Pin())
		{
			if (Node == Gone)
			{
				return true;
			}
		}
		return false;
	};
	TArray<TPair<FString, TWeakPtr<FDreamProjectNode>>> Drop;
	for (const TPair<FString, TWeakPtr<FDreamProjectNode>>& Entry : FolderNodes)
	{
		const FDreamProjectNodePtr Node = Entry.Value.Pin();
		if (!Node.IsValid() || IsGone(Node))
		{
			Drop.Add(Entry);
		}
	}
	for (const TPair<FString, TWeakPtr<FDreamProjectNode>>& Entry : Drop)
	{
		FolderNodes.RemoveSingle(Entry.Key, Entry.Value);
	}
}

FDreamProjectNodePtr SDreamProjectTree::FolderForActions(const FDreamProjectNodePtr& Node) const
{
	if (!Node.IsValid())
	{
		return nullptr;
	}
	if (Node->IsFolderLike())
	{
		return Node;
	}
	const FDreamProjectNodePtr Parent = Node->Parent.Pin();
	return Parent.IsValid() && Parent->IsFolderLike() ? Parent : nullptr;
}

void SDreamProjectTree::RefreshRoots()
{
	using namespace DreamProjectTreeLocal;
	if (!TreeView.IsValid())
	{
		return;
	}
	++RootsRefreshCount;
	AutoRootsDirty->store(false);
	// Review p3 nit: expansions set while the top level is half built would drop and register watches again (a CreateFileW on
	// the game thread each); UpdateWatches waits for the end of the rebuild.
	++RefreshingRoots;
	const TArray<FDreamProjectRoot> Roots = FDreamProjectFiles::ResolveRoots();
	const TArray<FDreamProjectRoot> Automatic = FDreamProjectFiles::ResolveAutomaticRoots();   // empty while the setting is off
	const UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::Get();
	TSet<FString> Collapsed;   // keys once, not a normalization per root and saved path (p1 m6)
	for (const FString& Saved : Session->CollapsedRoots)
	{
		Collapsed.Add(FDreamProjectFiles::PathKey(Saved));
	}
	TMap<FString, FDreamProjectNodePtr> Old;
	for (const FDreamProjectNodePtr& Item : TopItems)
	{
		if (Item->IsTopLevel())
		{
			Old.Add(Item->Key + (Item->Kind == FDreamProjectNode::EKind::Scratches ? TEXT("|scratches") : TEXT("")), Item);
		}
		else if (Item->Kind == FDreamProjectNode::EKind::Group)
		{
			for (const FDreamProjectNodePtr& Child : Item->Children)
			{
				Old.Add(Child->Key + TEXT("|auto"), Child);
			}
		}
	}
	const bool bProjectGroupWasShown = TopItems.Contains(ProjectGroup);
	const bool bEngineGroupWasShown = TopItems.Contains(EngineGroup);
	TopItems.Reset();
	TSet<FString> RootKeys;
	FDreamProjectNodePtr ScratchesNode;
	for (const FDreamProjectRoot& Root : Roots)
	{
		const FDreamProjectNode::EKind Kind = Root.bScratches ? FDreamProjectNode::EKind::Scratches : FDreamProjectNode::EKind::Root;
		const FString MapKey = FDreamProjectFiles::PathKey(Root.Path) + (Root.bScratches ? TEXT("|scratches") : TEXT(""));
		FDreamProjectNodePtr Node;
		if (const FDreamProjectNodePtr* Kept = Old.Find(MapKey))
		{
			Node = *Kept;
			Old.Remove(MapKey);
			if (Node->bRemote != Root.bRemote)
			{
				SetRemote(Node, Root.bRemote);   // a drive mapped or unmapped since: watched or not from now on (p2 nit)
				Node->RetryDelay = 0.0;
			}
			if (Node->bReadOnly != Root.bReadOnly)
			{
				SetReadOnly(Node, Root.bReadOnly);
			}
			if (Node->bListed || !Node->bListing)
			{
				RequestListing(Node);   // the settings changed: look at the folder again
			}
		}
		else
		{
			Node = MakeNode(Kind, Root.Path, Root.bScratches ? LOCTEXT("ProjectTree.Scratches", "Scratches").ToString() : LeafName(Root.Path), nullptr);
			Node->bRemote = Root.bRemote;
			Node->bReadOnly = Root.bReadOnly;   // a user root in the engine's folder (p3 nit); its rows inherit it
			FolderNodes.Add(Node->Key, Node);
			Node->bExists = true;   // until its first listing says otherwise
			// Each top-level item is listed once at once (one level), so a missing root says "(not found)" collapsed too.
			RequestListing(Node);
			TreeView->SetItemExpansion(Node, !Collapsed.Contains(Node->Key));
		}
		Node->bDefaultRoot = Root.bDefault;
		Node->Name = Root.bScratches ? LOCTEXT("ProjectTree.Scratches", "Scratches").ToString() : LeafName(Root.Path);
		RootKeys.Add(Node->Key);
		if (Root.bScratches)
		{
			ScratchesNode = Node;   // always last
		}
		else
		{
			TopItems.Add(Node);
		}
	}
	if (TopItems.Num() == 0)
	{
		TopItems.Add(NoRootsRow);   // "No project roots. Add one..." where the roots would be
	}

	// PO-05-A1: the plugins' Python folders, under their groups. One whose folder is not known to exist yet is asked about on
	// a worker and shown once the answer says it is there; one that does not exist is not shown.
	TSet<FString> SavedExpanded;
	for (const FString& Saved : Session->ExpandedFolders)
	{
		SavedExpanded.Add(FDreamProjectFiles::PathKey(Saved));
	}
	ProjectGroup->Children.Reset();
	EngineGroup->Children.Reset();
	TArray<FString> Unknown;
	for (const FDreamProjectRoot& Root : Automatic)
	{
		const FString Key = FDreamProjectFiles::PathKey(Root.Path);
		if (RootKeys.Contains(Key))
		{
			continue;   // the user's root (or the scratch folder) shows it, once
		}
		const bool* Exists = AutoExists.Find(Key);
		if (Exists == nullptr)
		{
			if (!AutoChecking.Contains(Key))
			{
				Unknown.Add(Root.Path);
			}
			continue;
		}
		if (!*Exists)
		{
			continue;
		}
		const FDreamProjectNodePtr& Group = Root.Group == EDreamRootGroup::Engine ? EngineGroup : ProjectGroup;
		FDreamProjectNodePtr Node;
		const FString MapKey = Key + TEXT("|auto");
		if (const FDreamProjectNodePtr* Kept = Old.Find(MapKey); Kept != nullptr && (*Kept)->Parent.Pin() == Group)
		{
			Node = *Kept;
			Old.Remove(MapKey);
			if (Node->bRemote != Root.bRemote)
			{
				SetRemote(Node, Root.bRemote);
			}
		}
		else
		{
			Node = MakeNode(FDreamProjectNode::EKind::Root, Root.Path, Root.Label, Group);   // read-only under Engine (MakeNode)
			Node->bAutomatic = true;
			Node->bRemote = Root.bRemote;
			FolderNodes.Add(Key, Node);
			if (SavedExpanded.Contains(Key))
			{
				TreeView->SetItemExpansion(Node, true);   // it lists itself on expand; the others wait until opened
			}
		}
		Node->Name = Root.Label;
		Group->Children.Add(Node);
	}
	if (Unknown.Num() > 0 && bActive)
	{
		for (const FString& Path : Unknown)
		{
			AutoChecking.Add(FDreamProjectFiles::PathKey(Path));
		}
		const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
		Files->CheckFolders(Unknown, [WeakThis](const TArray<FString>& Folders, const TArray<bool>& Exists)
		{
			if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
			{
				Pinned->ApplyChecks(Folders, Exists);
			}
		});
	}
	// A group shows while it holds an entry; one shown again gets its saved state (Project Plugins open, Engine closed at first).
	auto ShowGroup = [this, Session](const FDreamProjectNodePtr& Group, bool bWasShown, bool bOpenAtFirst, const TCHAR* Saved)
	{
		if (Group->Children.Num() == 0)
		{
			return;
		}
		TopItems.Add(Group);
		if (!bWasShown)
		{
			const bool bOpen = bOpenAtFirst ? !Session->CollapsedRoots.Contains(Saved) : Session->ExpandedFolders.Contains(Saved);
			TreeView->SetItemExpansion(Group, bOpen);
		}
	};
	ShowGroup(ProjectGroup, bProjectGroupWasShown, /*bOpenAtFirst*/ true, ProjectGroupSaved);
	ShowGroup(EngineGroup, bEngineGroupWasShown, /*bOpenAtFirst*/ false, EngineGroupSaved);
	if (ScratchesNode.IsValid())
	{
		TopItems.Add(ScratchesNode);
	}
	// Roots no longer configured (and plugin folders no longer mounted) leave the folder index; their rows go with TopItems.
	for (const TPair<FString, FDreamProjectNodePtr>& Gone : Old)
	{
		ForgetRows(Gone.Value);
	}
	--RefreshingRoots;   // the watches from the finished top level (an outer RefreshRoots, if any, sets them at its end)
	UpdateWatches();
	TreeView->RequestTreeRefresh();
}

void SDreamProjectTree::ApplyChecks(const TArray<FString>& Folders, const TArray<bool>& Exists)
{
	bool bChanged = false;
	for (int32 Index = 0; Index < Folders.Num() && Index < Exists.Num(); ++Index)
	{
		const FString Key = FDreamProjectFiles::PathKey(Folders[Index]);
		AutoChecking.Remove(Key);
		const bool* Was = AutoExists.Find(Key);
		bChanged = bChanged || Was == nullptr || *Was != Exists[Index];
		AutoExists.Add(Key, Exists[Index]);
	}
	if (bChanged)
	{
		RefreshRoots();
	}
}

void SDreamProjectTree::RecheckAutomaticRoots()
{
	if (!bActive)
	{
		return;
	}
	TArray<FString> Again;
	for (const FDreamProjectRoot& Root : FDreamProjectFiles::ResolveAutomaticRoots())
	{
		const FString Key = FDreamProjectFiles::PathKey(Root.Path);
		if (!AutoChecking.Contains(Key))
		{
			AutoChecking.Add(Key);
			Again.Add(Root.Path);
		}
	}
	if (Again.Num() == 0)
	{
		return;
	}
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	Files->CheckFolders(Again, [WeakThis](const TArray<FString>& Folders, const TArray<bool>& Exists)
	{
		if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
		{
			Pinned->ApplyChecks(Folders, Exists);
		}
	});
}

void SDreamProjectTree::UpdateWatches()
{
	if (RefreshingRoots > 0)
	{
		return;   // RefreshRoots sets them at its end (review p3 nit)
	}
	TArray<FString> Folders;
	TArray<FDreamProjectNodePtr> NewlyWatched;
	auto Consider = [&Folders, &NewlyWatched](const FDreamProjectNodePtr& Item, bool bWatch)
	{
		if (bWatch)
		{
			Folders.Add(Item->Path);
			if (!Item->bWatched && Item->bListed)
			{
				NewlyWatched.Add(Item);
			}
		}
		Item->bWatched = bWatch;
	};
	for (const FDreamProjectNodePtr& Item : TopItems)
	{
		if (Item->Kind == FDreamProjectNode::EKind::Group)
		{
			// PO-05-A1: a plugin's Python folder is watched only while it shows expanded (no watch for a closed group).
			const bool bOpen = TreeView->IsItemExpanded(Item);
			for (const FDreamProjectNodePtr& Child : Item->Children)
			{
				Consider(Child, bActive && bOpen && TreeView->IsItemExpanded(Child) && !Child->bRemote && Child->bExists && Child->bListed);
			}
			continue;
		}
		// Hidden section: nothing; network roots: never (R-p1-11), they refresh on expand, Refresh and the retry.
		Consider(Item, bActive && Item->IsTopLevel() && !Item->bRemote && Item->bExists && (Item->bListed || Item->bDefaultRoot));
	}
	Files->SetWatchedFolders(Folders);
	// A file made between a root's first listing and the start of its watch would be missed: list it once more now.
	for (const FDreamProjectNodePtr& Item : NewlyWatched)
	{
		if (!Item->bListing)
		{
			RequestListing(Item);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Listings
// ---------------------------------------------------------------------------------------------------------------------

void SDreamProjectTree::RequestListing(const FDreamProjectNodePtr& Node)
{
	if (!Node.IsValid() || !Node->IsFolderLike() || Node->Path.IsEmpty() || !bActive)
	{
		return;   // hidden: SetActive(true) lists again what the tree shows
	}
	Node->bListing = true;
	Files->RequestListing(Node->Path);
}

void SDreamProjectTree::ApplyListing(const FDreamFolderListing& Listing)
{
	TSet<FString> SavedExpanded;   // keys once per listing (p1 m6), not a normalization per child and saved path
	for (const FString& Saved : UDreamPerProjectUserSettings::Get()->ExpandedFolders)
	{
		SavedExpanded.Add(FDreamProjectFiles::PathKey(Saved));
	}
	TArray<TWeakPtr<FDreamProjectNode>> Targets;
	FolderNodes.MultiFind(FDreamProjectFiles::PathKey(Listing.Folder), Targets);
	bool bTopLevel = false;
	bool bAutomaticGone = false;
	for (const TWeakPtr<FDreamProjectNode>& Weak : Targets)
	{
		const FDreamProjectNodePtr Node = Weak.Pin();
		if (!Node.IsValid())
		{
			continue;
		}
		Node->bListing = false;
		bTopLevel = bTopLevel || Node->IsTopLevel() || Node->bAutomatic;
		if (!Listing.bExists && Node->bAutomatic)
		{
			AutoExists.Add(Node->Key, false);   // a plugin's Python folder that went is not shown (PO-05-A1)
			bAutomaticGone = true;
			continue;
		}
		if (!Listing.bExists)
		{
			// A root that is not there shows "(not found)" and is never created; a folder that went is dropped by its parent.
			Node->Children.Reset();
			Node->MoreCount = 0;
			Node->bListed = true;
			Node->bExists = false;
			if (Node->IsTopLevel())
			{
				// Its next look is timed from this answer, not from the request: an offline share that takes its timeout to say
				// "not there" is not asked again at once; a network one waits 2 s, 4 s, ... 60 s (review p2 m3).
				Node->RetryDelay = Node->bRemote ? FMath::Clamp(Node->RetryDelay * 2.0, RetrySeconds, MaxRemoteRetrySeconds) : RetrySeconds;
				Node->NextRetryAt = FPlatformTime::Seconds() + Node->RetryDelay;
			}
			continue;
		}
		Node->bExists = true;
		Node->RetryDelay = 0.0;
		TMap<FString, FDreamProjectNodePtr> Old;
		for (const FDreamProjectNodePtr& Child : Node->Children)
		{
			Old.Add(Child->Key, Child);
		}
		TArray<FDreamProjectNodePtr> Children;
		Children.Reserve(Listing.Entries.Num());
		for (const FDreamProjectEntry& Entry : Listing.Entries)
		{
			const FDreamProjectNode::EKind Kind = Entry.bDirectory ? FDreamProjectNode::EKind::Folder : FDreamProjectNode::EKind::File;
			const FString Key = FDreamProjectFiles::PathKey(Entry.Path);
			FDreamProjectNodePtr Child;
			if (const FDreamProjectNodePtr* Kept = Old.Find(Key); Kept != nullptr && (*Kept)->Kind == Kind)
			{
				Child = *Kept;
				Child->Name = Entry.Name;   // a case-only rename keeps the row
				Child->Path = Entry.Path;
				Old.Remove(Key);
			}
			else
			{
				Child = MakeNode(Kind, Entry.Path, Entry.Name, Node);
				if (Kind == FDreamProjectNode::EKind::Folder)
				{
					FolderNodes.Add(Key, Child);
				}
			}
			Children.Add(Child);
		}
		// Folders that went leave the index with everything under them.
		for (const TPair<FString, FDreamProjectNodePtr>& Gone : Old)
		{
			if (Gone.Value->Kind == FDreamProjectNode::EKind::Folder)
			{
				ForgetRows(Gone.Value);
			}
		}
		Node->Children = MoveTemp(Children);
		Node->MoreCount = Listing.MoreCount;
		Node->bListed = true;
		// The folders saved as expanded open again (each lists itself when it expands).
		for (const FDreamProjectNodePtr& Child : Node->Children)
		{
			if (Child->Kind == FDreamProjectNode::EKind::Folder && !TreeView->IsItemExpanded(Child) && SavedExpanded.Contains(Child->Key))
			{
				TreeView->SetItemExpansion(Child, true);
			}
			else if (Child->Kind == FDreamProjectNode::EKind::Folder && TreeView->IsItemExpanded(Child) && !Child->bListed && !Child->bListing)
			{
				RequestListing(Child);
			}
		}
	}
	if (bAutomaticGone)
	{
		RefreshRoots();   // rebuilds the groups and the watches
		return;
	}
	if (bTopLevel)
	{
		UpdateWatches();
	}
	TreeView->RequestTreeRefresh();
}

void SDreamProjectTree::HandleChanged(const TArray<FString>& Folders, const FString& Root, bool bAll)
{
	// The watcher's debounced news: the folders the tree has listed are listed again.
	TArray<FDreamProjectNodePtr> Again;
	for (const TPair<FString, TWeakPtr<FDreamProjectNode>>& Entry : FolderNodes)
	{
		const FDreamProjectNodePtr Node = Entry.Value.Pin();
		if (!Node.IsValid() || !Node->bListed || IsUnderClosedGroup(Node))
		{
			continue;   // a plugin folder under a collapsed group is looked at again when it opens (review p3 m2)
		}
		if (bAll ? FDreamProjectFiles::IsUnder(Node->Path, Root) : Folders.Contains(Entry.Key))
		{
			Again.AddUnique(Node);
		}
	}
	for (const FDreamProjectNodePtr& Node : Again)
	{
		RequestListing(Node);
	}
}

void SDreamProjectTree::HandleBufferOpened(const TSharedRef<FDreamBuffer>& Buffer)
{
	// PO-05: a new scratch file shows at once. The watcher brings it for a local scratch folder, but a network one is never
	// watched (R-p1-11), so the folder of every scratch file opened in a tab is listed again here (review p2 m2).
	if (!Buffer->IsScratchFile() || !Buffer->HasPath())
	{
		return;
	}
	TArray<TWeakPtr<FDreamProjectNode>> Targets;
	FolderNodes.MultiFind(FDreamProjectFiles::PathKey(FPaths::GetPath(Buffer->GetPath())), Targets);
	for (const TWeakPtr<FDreamProjectNode>& Weak : Targets)
	{
		const FDreamProjectNodePtr Node = Weak.Pin();
		if (Node.IsValid() && (Node->bListed || Node->IsTopLevel()))
		{
			RequestListing(Node);   // one listed already is listed once more after it (FDreamProjectFiles::RequestListing)
		}
	}
}

void SDreamProjectTree::SetRemote(const FDreamProjectNodePtr& Node, bool bRemote)
{
	Node->bRemote = bRemote;
	for (const FDreamProjectNodePtr& Child : Node->Children)
	{
		SetRemote(Child, bRemote);
	}
}

void SDreamProjectTree::SetReadOnly(const FDreamProjectNodePtr& Node, bool bReadOnly)
{
	Node->bReadOnly = bReadOnly;
	for (const FDreamProjectNodePtr& Child : Node->Children)
	{
		SetReadOnly(Child, bReadOnly);
	}
}

bool SDreamProjectTree::IsUnderClosedGroup(const FDreamProjectNodePtr& Node) const
{
	for (FDreamProjectNodePtr Parent = Node.IsValid() ? Node->Parent.Pin() : nullptr; Parent.IsValid(); Parent = Parent->Parent.Pin())
	{
		if (Parent->Kind == FDreamProjectNode::EKind::Group)
		{
			return !TreeView->IsItemExpanded(Parent);
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree view's delegates
// ---------------------------------------------------------------------------------------------------------------------

void SDreamProjectTree::ChildrenOf(FDreamProjectNodePtr Node, TArray<FDreamProjectNodePtr>& OutChildren)
{
	if (Node.IsValid() && Node->Kind == FDreamProjectNode::EKind::Group)
	{
		OutChildren.Append(Node->Children);   // the automatic roots RefreshRoots put there
		return;
	}
	if (!Node.IsValid() || !Node->IsFolderLike())
	{
		return;
	}
	if (EditMode == EEdit::NewItem && EditNode.IsValid() && EditNode->Parent.Pin() == Node)
	{
		OutChildren.Add(EditNode);   // the inline name editor heads its folder
	}
	if (!Node->bListed)
	{
		if (Node->bExists)
		{
			OutChildren.Add(Node->Placeholder);   // gives the folder its chevron until it is listed
		}
		return;
	}
	OutChildren.Append(Node->Children);
	if (Node->MoreCount > 0)
	{
		OutChildren.Add(Node->MoreRow);
	}
}

void SDreamProjectTree::HandleExpansionChanged(FDreamProjectNodePtr Node, bool bExpanded)
{
	if (Node.IsValid() && Node->Kind == FDreamProjectNode::EKind::Group)
	{
		if (bExpanded)
		{
			// Review p3 m2: its entries saved as expanded were not listed while it was closed: they open (and list) now.
			ReapplySavedExpansion(Node);
			for (const FDreamProjectNodePtr& Child : Node->Children)
			{
				if (TreeView->IsItemExpanded(Child) && !Child->bListed && !Child->bListing)
				{
					RequestListing(Child);
				}
			}
		}
		SaveExpansion(Node, bExpanded);
		UpdateWatches();   // an automatic root is watched only while it shows expanded
		return;
	}
	if (!Node.IsValid() || !Node->IsFolderLike())
	{
		return;
	}
	if (bExpanded && !Node->bListing && (Node->bExists || Node->IsTopLevel()) && (!Node->bListed || Node->bRemote) && !IsUnderClosedGroup(Node))
	{
		RequestListing(Node);   // unlisted, or a network folder (not watched: an expand is its refresh, R-p1-11)
	}
	SaveExpansion(Node, bExpanded);
	if (bExpanded && Node->bListed)
	{
		ReapplySavedExpansion(Node);   // its listed folders saved as open (a listing does the same, ApplyListing)
	}
	if (Node->bAutomatic)
	{
		UpdateWatches();
	}
}

void SDreamProjectTree::ReapplySavedExpansion(const FDreamProjectNodePtr& Node)
{
	TArray<FDreamProjectNodePtr> Open;
	TSet<FString> Saved;
	for (const FDreamProjectNodePtr& Child : Node->Children)
	{
		if ((Child->Kind == FDreamProjectNode::EKind::Folder || Child->bAutomatic) && !TreeView->IsItemExpanded(Child))
		{
			if (Saved.Num() == 0)
			{
				for (const FString& Path : UDreamPerProjectUserSettings::Get()->ExpandedFolders)
				{
					Saved.Add(FDreamProjectFiles::PathKey(Path));
				}
				Saved.Add(FString());   // a sentinel: built once even when nothing is saved
			}
			if (Saved.Contains(Child->Key))
			{
				Open.Add(Child);
			}
		}
	}
	for (const FDreamProjectNodePtr& Child : Open)
	{
		TreeView->SetItemExpansion(Child, true);   // HandleExpansionChanged lists it and opens its own saved folders
	}
}

void SDreamProjectTree::SaveExpansion(const FDreamProjectNodePtr& Node, bool bExpanded)
{
	using namespace DreamProjectTreeLocal;
	if (FDreamProjectFiles::IsSecondaryInstance())
	{
		return;   // the primary's session keeps its expansions (review p1 nit)
	}
	UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
	bool bChanged = false;
	if (Node->Kind == FDreamProjectNode::EKind::Group)
	{
		const bool bEngine = Node == EngineGroup;
		TArray<FString>& List = bEngine ? Settings->ExpandedFolders : Settings->CollapsedRoots;
		const TCHAR* const Saved = bEngine ? EngineGroupSaved : ProjectGroupSaved;
		const bool bKeep = bEngine ? bExpanded : !bExpanded;   // the state that is not the group's first one is kept
		if (!bKeep)
		{
			bChanged = List.Remove(Saved) > 0;
		}
		else if (!List.Contains(Saved))
		{
			List.Add(Saved);
			bChanged = true;
		}
	}
	else if (Node->IsTopLevel())
	{
		// Roots and the Scratches section start expanded: the collapsed ones are kept.
		if (bExpanded)
		{
			bChanged = RemoveKey(Settings->CollapsedRoots, Node->Key);
		}
		else if (!ContainsKey(Settings->CollapsedRoots, Node->Key))
		{
			Settings->CollapsedRoots.Add(Node->Path);
			bChanged = true;
		}
	}
	else if (Node->Kind == FDreamProjectNode::EKind::Folder || Node->bAutomatic)
	{
		// Folders, and automatic roots (they start collapsed), keep the expanded ones.
		if (!bExpanded)
		{
			bChanged = RemoveKey(Settings->ExpandedFolders, Node->Key);
		}
		else if (!ContainsKey(Settings->ExpandedFolders, Node->Key))
		{
			Settings->ExpandedFolders.Add(Node->Path);
			if (Settings->ExpandedFolders.Num() > MaxSavedFolders)
			{
				Settings->ExpandedFolders.RemoveAt(0, Settings->ExpandedFolders.Num() - MaxSavedFolders);
			}
			bChanged = true;
		}
	}
	if (bChanged)
	{
		// Saved with the session, as the output/code split is (SaveSessionState's 30 s debounce, and at shutdown).
		if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
		{
			Pinned->SaveSessionState(false);
		}
	}
}

void SDreamProjectTree::HandleDoubleClick(FDreamProjectNodePtr Node)
{
	Activate(Node);
}

void SDreamProjectTree::HandleClick(FDreamProjectNodePtr Node)
{
	if (Node.IsValid() && Node->Kind == FDreamProjectNode::EKind::NoRoots)
	{
		PromptAddRoot();   // "No project roots. Add one..." (a click opens the folder picker)
	}
}

void SDreamProjectTree::Activate(const FDreamProjectNodePtr& Node)
{
	if (!Node.IsValid())
	{
		return;
	}
	switch (Node->Kind)
	{
	case FDreamProjectNode::EKind::File:
		if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
		{
			// The existing open route: an open file is focused; binary, oversized or refused files show its messages.
			Pinned->OpenLocation(Node->Path, 0, 0);
		}
		break;
	case FDreamProjectNode::EKind::Root:
	case FDreamProjectNode::EKind::Scratches:
	case FDreamProjectNode::EKind::Folder:
		if (Node->IsTopLevel() && !Node->bExists)
		{
			RequestListing(Node);   // a missing root or scratch folder: look again now (R-p1-7)
			break;
		}
		TreeView->SetItemExpansion(Node, !TreeView->IsItemExpanded(Node));
		break;
	case FDreamProjectNode::EKind::NoRoots:
		PromptAddRoot();
		break;
	case FDreamProjectNode::EKind::Group:
		TreeView->SetItemExpansion(Node, !TreeView->IsItemExpanded(Node));
		break;
	default:
		break;
	}
}

FReply SDreamProjectTree::HandleTreeKey(const FGeometry& Geometry, const FKeyEvent& Key)
{
	const TArray<FDreamProjectNodePtr> Selected = TreeView->GetSelectedItems();
	const FDreamProjectNodePtr Node = Selected.Num() > 0 ? Selected[0] : nullptr;
	const FKey Pressed = Key.GetKey();
	if (EditNode.IsValid() && Pressed != EKeys::Escape)
	{
		return FReply::Unhandled();   // keys the name editor left alone change nothing in the tree while it is open
	}
	const bool bPlain = !Key.IsControlDown() && !Key.IsAltDown() && !Key.IsShiftDown();
	if (Pressed == EKeys::Enter && bPlain && Node.IsValid())
	{
		Activate(Node);
		return FReply::Handled();
	}
	if (Pressed == EKeys::F5 && bPlain)
	{
		// (editing) R-q1-1: F5 always refreshes while the tree has focus; no Dream command bound to F5 (DebugRun's default
		// included) reaches the side bar's key routing from here, so a refresh never runs user code.
		Refresh(Node.IsValid() ? Node->Path : FString());
		return FReply::Handled();
	}
	if (Pressed == EKeys::F2 && bPlain && Node.IsValid() && (Node->Kind == FDreamProjectNode::EKind::File || Node->Kind == FDreamProjectNode::EKind::Folder))
	{
		BeginRename(Node->Path);
		return FReply::Handled();
	}
	if (Pressed == EKeys::Delete && bPlain && Node.IsValid() && (Node->Kind == FDreamProjectNode::EKind::File || Node->Kind == FDreamProjectNode::EKind::Folder))
	{
		RequestDelete(Node->Path);
		return FReply::Handled();
	}
	if (Pressed == EKeys::Escape)
	{
		return OnKeyDown(Geometry, Key);
	}
	return FReply::Unhandled();
}

FReply SDreamProjectTree::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (EditNode.IsValid())
		{
			// The inline editor open while focus is in the tree (review p2 m1): Esc closes it first, focus stays in the tree.
			CancelEdit();
			bFocusTreeAfterEdit = true;
			return FReply::Handled();
		}
		if (!PendingDeletePath.IsEmpty())
		{
			CancelDelete();
			return FReply::Handled();
		}
		// Esc returns focus to the code view (PO-05 section 1).
		if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
		{
			if (const TSharedPtr<SDreamCodeView> View = Pinned->GetActiveCodeView())
			{
				View->FocusEditor();
				return FReply::Handled();
			}
			return FReply::Handled().SetUserFocus(Pinned.ToSharedRef(), EFocusCause::SetDirectly);
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SDreamProjectTree::FocusTree()
{
	if (!TreeView.IsValid())
	{
		return;
	}
	if (TreeView->GetNumItemsSelected() == 0 && TopItems.Num() > 0)
	{
		TreeView->SetSelection(TopItems[0], ESelectInfo::Direct);
	}
	FSlateApplication::Get().SetKeyboardFocus(TreeView, EFocusCause::SetDirectly);
}

TSharedPtr<SWidget> SDreamProjectTree::GetFocusWidget() const
{
	return TreeView;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------------------------------

FText SDreamProjectTree::LabelOf(const FDreamProjectNode& Node) const
{
	switch (Node.Kind)
	{
	case FDreamProjectNode::EKind::Loading:
		return LOCTEXT("ProjectTree.Loading", "…");
	case FDreamProjectNode::EKind::More:
		return FText::Format(LOCTEXT("ProjectTree.More", "… {0} more"), FText::AsNumber(Node.Parent.IsValid() ? Node.Parent.Pin()->MoreCount : 0));
	case FDreamProjectNode::EKind::NoRoots:
		return LOCTEXT("ProjectTree.NoRoots", "No project roots. Add one…");
	case FDreamProjectNode::EKind::Root:
		if (!Node.bExists)
		{
			return FText::Format(LOCTEXT("ProjectTree.NotFound", "{0} (not found)"), FText::FromString(Node.Path));
		}
		return FText::FromString(Node.Name);
	default:
		return FText::FromString(Node.Name);
	}
}

bool SDreamProjectTree::IsMuted(const FDreamProjectNode& Node) const
{
	switch (Node.Kind)
	{
	case FDreamProjectNode::EKind::File:
		return !FDreamProjectFiles::IsPythonFile(Node.Name);   // Python files in normal text, other files muted
	case FDreamProjectNode::EKind::Root:
		return !Node.bExists;
	case FDreamProjectNode::EKind::Loading:
	case FDreamProjectNode::EKind::More:
	case FDreamProjectNode::EKind::NoRoots:
		return true;
	default:
		return false;
	}
}

bool SDreamProjectTree::IsActiveFile(const FDreamProjectNode& Node) const
{
	return Node.Kind == FDreamProjectNode::EKind::File && !ActiveKey.IsEmpty() && Node.Key == ActiveKey;
}

TSharedRef<ITableRow> SDreamProjectTree::MakeRow(FDreamProjectNodePtr Node, const TSharedRef<STableViewBase>& Owner)
{
	using namespace DreamProjectTreeLocal;
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	const TWeakPtr<FDreamProjectNode> WeakNode = Node;
	TSharedRef<SDreamProjectRow> Row = SNew(SDreamProjectRow, Owner, [WeakThis, WeakNode]()
		{
			const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
			const FDreamProjectNodePtr N = WeakNode.Pin();
			return Pinned.IsValid() && N.IsValid() && Pinned->IsActiveFile(*N);
		})
		.Style(&FDreamThemes::TableRowStyle())
		.Padding(FMargin(0.f));

	TSharedPtr<SWidget> Content;
	if (Node == EditNode && EditMode != EEdit::None)
	{
		// The text is bound to EditText, so the engine's reload after an Enter shows what was typed, and focus stays in the box
		// after a refused name (ClearKeyboardFocusOnCommit off); only Esc or a click elsewhere ends the edit (review p1 M2).
		SAssignNew(EditBox, SEditableTextBox)
			.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("DreamSideBar.NameEditor")))
			.Style(&FDreamThemes::TextBoxStyle())
			.Font(FDreamChrome::Font(9))
			.Text_Lambda([WeakThis]()
			{
				const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
				return Pinned.IsValid() ? FText::FromString(Pinned->EditText) : FText::GetEmpty();
			})
			.OnTextChanged_Lambda([WeakThis](const FText& Text)
			{
				if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
				{
					Pinned->EditText = Text.ToString();
				}
			})
			.ClearKeyboardFocusOnCommit(false)
			.OnKeyDownHandler_Lambda([WeakThis](const FGeometry&, const FKeyEvent& Key)
			{
				const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
				if (Pinned.IsValid() && Key.GetKey() == EKeys::Escape)
				{
					Pinned->CancelEdit();
					Pinned->bFocusTreeAfterEdit = true;
					return FReply::Handled();
				}
				return FReply::Unhandled();
			})
			.HintText(Node->Kind == FDreamProjectNode::EKind::NewItem
				? (Node->bNewFolder ? LOCTEXT("ProjectTree.NewFolderHint", "Folder name") : LOCTEXT("ProjectTree.NewFileHint", "File name (.py is added)"))
				: FText::GetEmpty())
			.SelectAllTextWhenFocused(true)
			.OnTextCommitted_Lambda([WeakThis](const FText& Text, ETextCommit::Type Commit)
			{
				const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
				if (!Pinned.IsValid())
				{
					return;
				}
				if (Commit == ETextCommit::OnEnter)
				{
					Pinned->EditText = Text.ToString();
					Pinned->CommitEdit(Text.ToString());
				}
				else if (Commit == ETextCommit::OnUserMovedFocus)
				{
					Pinned->CancelEdit();   // a click or Tab elsewhere
				}
				// Default (focus set by code, or a window deactivating) and OnCleared: Tick decides on the next frame, once the
				// app's activation is known: the editor closes when focus went elsewhere in the active app, and stays across an
				// Alt-Tab (Slate gives it focus back when the window is activated again) (review p2 m1).
			});
		// A rebuilt editor row (RebuildList) has a new box: keyboard focus goes to it on the next tick.
		bFocusEditBox = true;
		bEditHadFocus = false;
		Content = EditBox;
	}
	else
	{
		Content = SNew(STextBlock)
			.Font(FDreamChrome::Font(9))
			.Text_Lambda([WeakThis, WeakNode]()
			{
				const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
				const FDreamProjectNodePtr N = WeakNode.Pin();
				return Pinned.IsValid() && N.IsValid() ? Pinned->LabelOf(*N) : FText::GetEmpty();
			})
			.ToolTipText_Lambda([WeakNode]()
			{
				const FDreamProjectNodePtr N = WeakNode.Pin();
				if (!N.IsValid() || N->Path.IsEmpty())
				{
					return N.IsValid() && N->Kind == FDreamProjectNode::EKind::NoRoots
						? LOCTEXT("ProjectTree.NoRootsTip", "Choose a folder to show here (Editor Preferences > Plugins > Dream Python Editor (this project) > Project Roots)")
						: FText::GetEmpty();
				}
				return FText::FromString(FPaths::ConvertRelativePathToFull(N->Path));
			})
			.ColorAndOpacity_Lambda([WeakThis, WeakNode]()
			{
				const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
				const FDreamProjectNodePtr N = WeakNode.Pin();
				const bool bMuted = Pinned.IsValid() && N.IsValid() && Pinned->IsMuted(*N);
				return FDreamChrome::Color(bMuted ? EDreamChromeSlot::MutedText : EDreamChromeSlot::Text);
			});
	}

	// In a tree, STableRow puts its own expander arrow before the content. (icons) PO-06 F6 (overriding PO-04's "chevrons
	// only" here): the row's icon before the label, read at paint (a folder opens and closes with its expansion, and the tint
	// follows a theme switch), and a small lock after a read-only engine entry.
	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox);
	EDreamIcon FirstIcon = EDreamIcon::File;
	if (NodeIcon(*Node, false, FirstIcon))
	{
		auto CurrentIcon = [WeakThis, WeakNode, FirstIcon]()
		{
			const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin();
			const FDreamProjectNodePtr N = WeakNode.Pin();
			EDreamIcon Icon = FirstIcon;
			if (Pinned.IsValid() && N.IsValid() && Pinned->TreeView.IsValid())
			{
				NodeIcon(*N, Pinned->TreeView->IsItemExpanded(N), Icon);
			}
			return Icon;
		};
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 4.f, 0.f))
		[
			SNew(SImage)
			.Image_Lambda([CurrentIcon]() { return FDreamIcons::Brush(CurrentIcon()); })
			.DesiredSizeOverride(FVector2D(16.0, 16.0))
			.ColorAndOpacity_Lambda([CurrentIcon]() { return FDreamIcons::Tint(CurrentIcon()); })
		];
	}
	Line->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)[ Content.ToSharedRef() ];
	if (Node->bReadOnly && Node->Kind == FDreamProjectNode::EKind::Root)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(SImage)
			.Image(FDreamIcons::Brush(EDreamIcon::ReadOnly))
			.DesiredSizeOverride(FVector2D(12.0, 12.0))
			.ColorAndOpacity(FDreamIcons::TintAttr(EDreamIcon::ReadOnly))
			.ToolTipText(LOCTEXT("ProjectTree.ReadOnlyTip", "Read-only: files open, nothing can be created, renamed or deleted here"))
		];
	}
	Row->SetContent(
		SNew(SBox)
		.VAlign(VAlign_Center)
		.Padding(FMargin(2.f, 1.f, 6.f, 1.f))
		[
			Line
		]);
	return Row;
}

// ---------------------------------------------------------------------------------------------------------------------
// The context menu and the actions
// ---------------------------------------------------------------------------------------------------------------------

TSharedPtr<SWidget> SDreamProjectTree::MakeContextMenu()
{
	const TArray<FDreamProjectNodePtr> Selected = TreeView->GetSelectedItems();
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);
	FillContextMenu(Menu, Selected.Num() > 0 ? Selected[0] : nullptr);
	return Menu.MakeWidget();
}

void SDreamProjectTree::FillContextMenu(FMenuBuilder& Menu, const FDreamProjectNodePtr& Node)
{
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	auto Add = [&Menu](const TCHAR* Name, const FText& Label, const FText& Tip, TFunction<void()> Run, bool bEnabled = true)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda(MoveTemp(Run)), FCanExecuteAction::CreateLambda([bEnabled]() { return bEnabled; })),
			NAME_None, EUserInterfaceActionType::Button, FName(Name));
	};
	auto AddRootEntry = [&Add, WeakThis]()
	{
		Add(TEXT("Dream.ProjectTree.AddRoot"), LOCTEXT("ProjectTree.AddRoot", "Add Root…"),
			LOCTEXT("ProjectTree.AddRootTip", "Choose a folder to show in the Project tree (saved for this project)"),
			[WeakThis]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->PromptAddRoot(); } });
	};
	auto RefreshEntry = [&Add, WeakThis](const FString& Path)
	{
		Add(TEXT("Dream.ProjectTree.Refresh"), LOCTEXT("ProjectTree.Refresh", "Refresh"),
			LOCTEXT("ProjectTree.RefreshTip", "List this folder and the folders shown under it again (F5); network folders are not watched"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->Refresh(Path); } });
	};
	if (!Node.IsValid() || Node->Kind == FDreamProjectNode::EKind::NoRoots || Node->Kind == FDreamProjectNode::EKind::Group)
	{
		AddRootEntry();   // the tree's background (and the plugin groups' headers)
		RefreshEntry(FString());
		// PO-05-A1 follow-up: the per-project setting, checkable here as in View > Side Bar and Editor Preferences.
		Menu.AddMenuEntry(LOCTEXT("ProjectTree.PluginFolders", "Plugin Python Folders"),
			LOCTEXT("ProjectTree.PluginFoldersTip", "Show the Python folders of the enabled engine and project plugins (saved for this project)"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { FDreamProjectFiles::SetAddPluginFolders(!FDreamProjectFiles::IsAddPluginFolders()); }),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]() { return FDreamProjectFiles::IsAddPluginFolders(); })),
			NAME_None, EUserInterfaceActionType::ToggleButton, FName(TEXT("Dream.ProjectTree.PluginFolders")));
		return;
	}
	const FString Path = Node->Path;
	if (Node->Kind == FDreamProjectNode::EKind::File)
	{
		Add(TEXT("Dream.ProjectTree.Open"), LOCTEXT("ProjectTree.Open", "Open"), LOCTEXT("ProjectTree.OpenTip", "Open in a Dream tab"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->Activate(P->FindNode(Path)); } });
		Menu.AddMenuSeparator();
	}
	const FDreamProjectNodePtr Folder = FolderForActions(Node);
	if (Folder.IsValid() && Folder->bExists && !Folder->bReadOnly)   // nothing is added under the engine (PO-05-A1)
	{
		const FString FolderPath = Folder->Path;
		Add(TEXT("Dream.ProjectTree.NewFile"), LOCTEXT("ProjectTree.NewFile", "New Python File…"),
			LOCTEXT("ProjectTree.NewFileTip", "A new .py file in this folder, opened in a tab"),
			[WeakThis, FolderPath]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->BeginNewItem(FolderPath, false); } });
		Add(TEXT("Dream.ProjectTree.NewFolder"), LOCTEXT("ProjectTree.NewFolder", "New Folder…"), LOCTEXT("ProjectTree.NewFolderTip", "A new folder in this folder"),
			[WeakThis, FolderPath]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->BeginNewItem(FolderPath, true); } });
		Menu.AddMenuSeparator();
	}
	if ((Node->Kind == FDreamProjectNode::EKind::File || Node->Kind == FDreamProjectNode::EKind::Folder) && !IsProtected(*Node))
	{
		Add(TEXT("Dream.ProjectTree.Rename"), LOCTEXT("ProjectTree.Rename", "Rename…"), LOCTEXT("ProjectTree.RenameTip", "Rename in place (F2); an open tab follows"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->BeginRename(Path); } });
		Add(TEXT("Dream.ProjectTree.Delete"), LOCTEXT("ProjectTree.Delete", "Delete"), LOCTEXT("ProjectTree.DeleteTip", "Move to the Recycle Bin, after a confirm (Delete)"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->RequestDelete(Path); } });
		Menu.AddMenuSeparator();
	}
	if (Folder.IsValid())
	{
		RefreshEntry(Folder->Path);
	}
	if (!Path.IsEmpty())
	{
		Add(TEXT("Dream.ProjectTree.Reveal"), LOCTEXT("ProjectTree.Reveal", "Reveal in Explorer"), LOCTEXT("ProjectTree.RevealTip", "Show it in the file browser"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->RevealInExplorer(Path); } }, Node->bExists);
		Add(TEXT("Dream.ProjectTree.CopyPath"), LOCTEXT("ProjectTree.CopyPath", "Copy Path"), LOCTEXT("ProjectTree.CopyPathTip", "Copy the full path to the clipboard"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->CopyPath(Path); } });
	}
	if (Node->Kind == FDreamProjectNode::EKind::Root && !Node->bAutomatic)   // automatic roots go only with the setting
	{
		Menu.AddMenuSeparator();
		Add(TEXT("Dream.ProjectTree.RemoveRoot"), LOCTEXT("ProjectTree.RemoveRoot", "Remove Root"),
			Node->bDefaultRoot ? LOCTEXT("ProjectTree.RemoveDefaultTip2", "The project's Content/Python always shows when it exists (Unreal's Python plugin imports from it)")
				: LOCTEXT("ProjectTree.RemoveRootTip", "Stop showing this folder (nothing on disk changes)"),
			[WeakThis, Path]() { if (const TSharedPtr<SDreamProjectTree> P = WeakThis.Pin()) { P->RemoveRoot(Path); } }, !Node->bDefaultRoot);
	}
	if (Node->IsTopLevel())
	{
		AddRootEntry();
	}
}

void SDreamProjectTree::PromptAddRoot()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (Desktop == nullptr || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	FString Folder;
	if (Desktop->OpenDirectoryDialog(Parent, LOCTEXT("ProjectTree.AddRootTitle", "Add a Project Root").ToString(),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Folder))
	{
		AddRoot(Folder);
	}
}

bool SDreamProjectTree::AddRoot(const FString& Path)
{
	if (!FDreamProjectFiles::AddRoot(Path))
	{
		ShowMessage(FText::Format(LOCTEXT("ProjectTree.RootTwice", "{0} is a root already."), FText::FromString(Path)), false);
		return false;
	}
	RefreshRoots();
	return true;
}

bool SDreamProjectTree::RemoveRoot(const FString& Path)
{
	if (!FDreamProjectFiles::RemoveRoot(Path))
	{
		return false;
	}
	RefreshRoots();
	return true;
}

void SDreamProjectTree::BeginNewItem(const FString& Folder, bool bFolder)
{
	CancelEdit();
	CancelDelete();
	const FDreamProjectNodePtr FolderNode = FindNode(Folder);
	if (!FolderNode.IsValid() || !FolderNode->IsFolderLike() || !FolderNode->bExists)
	{
		return;
	}
	if (FolderNode->bReadOnly)
	{
		ShowMessage(ProtectedMessage(*FolderNode), true);   // PO-05-A1: nothing is added under the engine
		return;
	}
	EditMode = EEdit::NewItem;
	EditText.Reset();
	EditNode = MakeNode(FDreamProjectNode::EKind::NewItem, FolderNode->Path, FString(), FolderNode);
	EditNode->bNewFolder = bFolder;
	EditBox.Reset();
	bFocusEditBox = true;
	bEditHadFocus = false;
	TreeView->SetItemExpansion(FolderNode, true);
	TreeView->RequestTreeRefresh();
	TreeView->RequestScrollIntoView(EditNode);
}

void SDreamProjectTree::BeginRename(const FString& Path)
{
	CancelEdit();
	CancelDelete();
	const FDreamProjectNodePtr Node = FindNode(Path);
	if (!Node.IsValid())
	{
		return;
	}
	if (IsProtected(*Node))
	{
		ShowMessage(ProtectedMessage(*Node), true);
		return;
	}
	if (Node->Kind != FDreamProjectNode::EKind::File && Node->Kind != FDreamProjectNode::EKind::Folder)
	{
		return;
	}
	EditMode = EEdit::Rename;
	EditText = Node->Name;
	EditNode = Node;
	EditBox.Reset();
	bFocusEditBox = true;
	bEditHadFocus = false;
	// The row is built again with the name editor in it (next tick).
	bRebuildRows = true;
	TreeView->RequestScrollIntoView(Node);
}

void SDreamProjectTree::CommitEdit(const FString& Name)
{
	if (!EditNode.IsValid() || EditMode == EEdit::None)
	{
		return;
	}
	FString Trimmed = Name.TrimStartAndEnd();
	if (EditMode == EEdit::NewItem && !EditNode->bNewFolder && !Trimmed.IsEmpty() && FPaths::GetExtension(Trimmed).IsEmpty())
	{
		Trimmed += TEXT(".py");   // New Python File...: ".py" is added when the name has no extension
	}
	const FText Error = FDreamProjectFiles::ValidateName(Trimmed);
	if (!Error.IsEmpty())
	{
		ShowMessage(Error, true);
		bFocusEditBox = true;   // the editor stays open with the name to fix
		return;
	}
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	auto OnDone = [WeakThis](const FDreamProjectFileResult& Result)
	{
		if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
		{
			Pinned->HandleOpDone(Result);
		}
	};
	const EEdit Mode = EditMode;
	const FDreamProjectNodePtr Node = EditNode;
	CancelEdit();
	bFocusTreeAfterEdit = true;   // the editor goes with its row: keyboard focus back to the tree, never to nothing
	if (Mode == EEdit::NewItem)
	{
		if (Node->bNewFolder)
		{
			Files->NewFolder(Node->Path, Trimmed, OnDone);
		}
		else
		{
			Files->NewFile(Node->Path, Trimmed, OnDone);
		}
	}
	else if (!Node->Name.Equals(Trimmed, ESearchCase::CaseSensitive))
	{
		Files->Rename(Node->Path, Trimmed, OnDone);
	}
}

void SDreamProjectTree::CancelEdit()
{
	if (EditMode == EEdit::None && !EditNode.IsValid())
	{
		return;
	}
	const bool bWasRename = EditMode == EEdit::Rename;
	EditMode = EEdit::None;
	EditNode.Reset();
	EditBox.Reset();
	bFocusEditBox = false;
	bEditHadFocus = false;
	if (TreeView.IsValid())
	{
		// The renamed row shows its label again; rebuilt on the next tick, never inside the editor's own commit callback.
		bRebuildRows = bRebuildRows || bWasRename;
		TreeView->RequestTreeRefresh();
	}
}

void SDreamProjectTree::RequestDelete(const FString& Path)
{
	CancelEdit();
	const FDreamProjectNodePtr Node = FindNode(Path);
	if (!Node.IsValid())
	{
		return;
	}
	if (IsProtected(*Node))
	{
		ShowMessage(ProtectedMessage(*Node), true);
		return;
	}
	if (Node->Kind != FDreamProjectNode::EKind::File && Node->Kind != FDreamProjectNode::EKind::Folder)
	{
		return;
	}
	PendingDeletePath = Node->Path;
	Message = FText::Format(LOCTEXT("ProjectTree.DeleteAsk", "Move {0} to the Recycle Bin?"), FText::FromString(Node->Name));
	bMessageError = false;
	MessageUntil = TNumericLimits<double>::Max();
}

void SDreamProjectTree::ConfirmDelete()
{
	if (PendingDeletePath.IsEmpty())
	{
		return;
	}
	const FString Path = MoveTemp(PendingDeletePath);
	PendingDeletePath.Reset();
	Message = FText::GetEmpty();
	const TWeakPtr<SDreamProjectTree> WeakThis = StaticCastSharedRef<SDreamProjectTree>(AsShared());
	Files->Recycle(Path, [WeakThis](const FDreamProjectFileResult& Result)
	{
		if (const TSharedPtr<SDreamProjectTree> Pinned = WeakThis.Pin())
		{
			Pinned->HandleOpDone(Result);
		}
	});
}

void SDreamProjectTree::CancelDelete()
{
	if (PendingDeletePath.IsEmpty())
	{
		return;
	}
	PendingDeletePath.Reset();
	Message = FText::GetEmpty();
	MessageUntil = 0.0;
}

void SDreamProjectTree::HandleOpDone(const FDreamProjectFileResult& Result)
{
	using namespace DreamProjectTreeLocal;
	if (!Result.bOk)
	{
		ShowMessage(Result.Error, true);
		return;
	}
	switch (Result.Op)
	{
	case EDreamProjectFileOp::NewFile:
		RequestListing(FindNode(Result.Path));
		PendingSelectPath = Result.NewPath;
		if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
		{
			Pinned->OpenLocation(Result.NewPath, 0, 0);   // the new file opens
		}
		break;
	case EDreamProjectFileOp::NewFolder:
		RequestListing(FindNode(Result.Path));
		PendingSelectPath = Result.NewPath;
		break;
	case EDreamProjectFileOp::Rename:
	{
		RetargetOpenBuffers(Result.Path, Result.NewPath);
		// Expanded folders under the old name keep their state under the new one (not in a secondary instance).
		if (!FDreamProjectFiles::IsSecondaryInstance())
		{
			UDreamPerProjectUserSettings* Settings = UDreamPerProjectUserSettings::GetMutable();
			const int32 OldLen = Result.Path.Len();
			for (FString& Saved : Settings->ExpandedFolders)
			{
				if (FDreamProjectFiles::IsUnder(Saved, Result.Path))
				{
					Saved = Result.NewPath + Saved.RightChop(OldLen);
				}
			}
		}
		RequestListing(FindNode(FPaths::GetPath(Result.Path)));
		PendingSelectPath = Result.NewPath;
		break;
	}
	case EDreamProjectFileOp::Recycle:
	default:
		RequestListing(FindNode(FPaths::GetPath(Result.Path)));
		ShowMessage(FText::Format(LOCTEXT("ProjectTree.Recycled", "Moved {0} to the Recycle Bin."), FText::FromString(FPaths::GetCleanFilename(Result.Path))), false);
		break;
	}
}

void SDreamProjectTree::RetargetOpenBuffers(const FString& OldPath, const FString& NewPath) const
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	FDreamBufferManager& Buffers = Module.GetBufferManager();
	const TArray<FString> Recent = Buffers.GetRecentFiles();
	const int32 OldKeyLen = FDreamProjectFiles::PathKey(OldPath).Len();
	bool bAny = false;
	for (const TSharedRef<FDreamBuffer>& Buffer : Buffers.GetOpenBuffers())
	{
		if (!Buffer->HasPath() || !FDreamProjectFiles::IsUnder(Buffer->GetPath(), OldPath))
		{
			continue;
		}
		const FString Before = Buffer->GetPath();
		const FString After = Before.Len() > OldKeyLen ? NewPath + Before.RightChop(OldKeyLen) : NewPath;   // a file in a renamed folder keeps its tail
		// The tab's name follows when it was the file name (a "(recovered)" tab keeps its own).
		if (Buffer->GetDisplayName() == FPaths::GetCleanFilename(Before))
		{
			Buffer->SetDisplayName(FPaths::GetCleanFilename(After));
		}
		Buffer->SetFileIdentity(After, Buffer->GetBase());   // same bytes: the base stays; broadcasts OnStateChanged (the tab label)
		Module.GetJournal().OnBufferMetaChanged(Buffer);     // the journal's META names the new path
		if (Recent.ContainsByPredicate([&Before](const FString& R) { return FDreamProjectFiles::PathKey(R) == FDreamProjectFiles::PathKey(Before); }))
		{
			Buffers.RemoveRecentFile(Before);
			Buffers.AddRecentFile(After);
		}
		bAny = true;
	}
	if (bAny)
	{
		Buffers.GetFileWatch().Sync();   // the watch follows the new directory (a result for the old path is dropped as renamed)
		if (const TSharedPtr<SDreamTab> Pinned = Tab.Pin())
		{
			Pinned->SaveSessionState(false);
		}
	}
}

void SDreamProjectTree::RevealInExplorer(const FString& Path) const
{
	FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(Path));
}

void SDreamProjectTree::CopyPath(const FString& Path) const
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::MakePlatformFilename(Full);
	FPlatformApplicationMisc::ClipboardCopy(*Full);
}

void SDreamProjectTree::ShowMessage(const FText& InMessage, bool bError)
{
	if (!PendingDeletePath.IsEmpty())
	{
		PendingDeletePath.Reset();   // a new message ends an open confirm
	}
	Message = InMessage;
	bMessageError = bError;
	MessageUntil = FPlatformTime::Seconds() + DreamProjectTreeLocal::MessageSeconds;
}

void SDreamProjectTree::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The active tab's file, once per frame for every row's highlight (a Save As or a rename moves it).
	const TSharedPtr<SDreamTab> Pinned = Tab.Pin();
	const TSharedPtr<FDreamBuffer> Active = Pinned.IsValid() ? Pinned->GetActiveBuffer() : nullptr;
	const FString& Raw = Active.IsValid() ? Active->GetPath() : FString();
	if (Raw != ActiveRawPath)
	{
		ActiveRawPath = Raw;
		ActiveKey = Raw.IsEmpty() ? FString() : FDreamProjectFiles::PathKey(Raw);
	}
	if (bRebuildRows && TreeView.IsValid())
	{
		bRebuildRows = false;
		TreeView->RebuildList();
	}
	if (AutoRootsDirty->load())
	{
		RefreshRoots();   // PO-05-A1: content mounted or dismounted since the last frame (a burst rebuilds once)
	}
	// R-p1-7: a top-level item found missing (a root not there yet, a scratch folder made later) is listed again while the
	// tree is shown (this Tick runs only then), RetryDelay after its last answer (ApplyListing: 2 s; a network one backs off
	// to 60 s, review p2 m3); its listing runs on a worker.
	if (bActive)
	{
		const double Now = FPlatformTime::Seconds();
		for (const FDreamProjectNodePtr& Item : TopItems)
		{
			if (Item->IsTopLevel() && !Item->bExists && !Item->bListing && Now >= Item->NextRetryAt)
			{
				RequestListing(Item);
			}
		}
	}
	// Review p2 m1: the engine reports a focus change made by code as ETextCommit::Default, which the editor ignores (it is
	// also what an Alt-Tab sends). Once the editor has had focus, losing it while the editor app is active and no menu is
	// open (the box's own context menu) closes it, so a chord's command (Ctrl+T focusing a new tab) never leaves it open.
	if (EditMode != EEdit::None && EditBox.IsValid() && !bFocusEditBox)
	{
		if (EditBox->HasKeyboardFocus() || EditBox->HasFocusedDescendants())
		{
			bEditHadFocus = true;
		}
		else if (bEditHadFocus && FSlateApplication::Get().IsActive() && !FSlateApplication::Get().AnyMenusVisible())
		{
			CancelEdit();   // focus stays where the code put it
		}
	}
	if (bFocusTreeAfterEdit && !bFocusEditBox && TreeView.IsValid() && !EditNode.IsValid())
	{
		bFocusTreeAfterEdit = false;
		FSlateApplication::Get().SetKeyboardFocus(TreeView, EFocusCause::SetDirectly);
	}
	if (bFocusEditBox && EditBox.IsValid())
	{
		bFocusEditBox = false;
		FSlateApplication::Get().SetKeyboardFocus(EditBox, EFocusCause::SetDirectly);
	}
	if (!PendingSelectPath.IsEmpty())
	{
		if (const FDreamProjectNodePtr Node = FindNode(PendingSelectPath))
		{
			TreeView->SetSelection(Node, ESelectInfo::Direct);
			TreeView->RequestScrollIntoView(Node);
			PendingSelectPath.Reset();
		}
	}
}

void SDreamProjectTree::SetActive(bool bInActive)
{
	if (bActive == bInActive)
	{
		return;
	}
	bActive = bInActive;
	if (!bActive)
	{
		CancelEdit();      // hidden with the inline editor open: it closes (review p2 m1)
		UpdateWatches();   // hidden: no watches (review p1 nit); listings are not started either
		return;
	}
	// Shown again: what the tree shows is listed again (it may have changed unwatched), and the watches come back.
	Refresh(FString());
	RefreshRoots();
}

void SDreamProjectTree::Refresh(const FString& Path)
{
	// Path: a folder, a file (its folder), or "" (every top-level item); the folders listed under it are listed again too.
	if (Path.IsEmpty())
	{
		RecheckAutomaticRoots();   // PO-05-A1: a plugin's Python folder made or removed since
	}
	FDreamProjectNodePtr Start = Path.IsEmpty() ? nullptr : FindNode(Path);
	if (Start.IsValid() && !Start->IsFolderLike())
	{
		Start = Start->Parent.Pin();
	}
	auto IsUnderStart = [&Start](FDreamProjectNodePtr Node)
	{
		for (; Node.IsValid(); Node = Node->Parent.Pin())
		{
			if (!Start.IsValid() || Node == Start)
			{
				return true;
			}
		}
		return false;
	};
	TArray<FDreamProjectNodePtr> Again;
	for (const TPair<FString, TWeakPtr<FDreamProjectNode>>& Entry : FolderNodes)
	{
		const FDreamProjectNodePtr Node = Entry.Value.Pin();
		if (Node.IsValid() && (Node->bListed || Node->IsTopLevel()) && IsUnderStart(Node) && !IsUnderClosedGroup(Node))
		{
			Again.AddUnique(Node);
		}
	}
	for (const FDreamProjectNodePtr& Node : Again)
	{
		if (!Node->bListing)
		{
			RequestListing(Node);
		}
	}
}

bool SDreamProjectTree::IsProtected(const FDreamProjectNode& Node) const
{
	if (Node.IsTopLevel() || Node.bAutomatic || Node.bReadOnly)
	{
		return true;   // a root, a plugin's Python folder, anything under the engine (PO-05-A1)
	}
	if (Node.Path.IsEmpty())
	{
		return false;
	}
	// The node is, or holds, a configured root or the scratch folder (a root above them, p1 m9).
	for (const FDreamProjectNodePtr& Item : TopItems)
	{
		if (Item->IsTopLevel() && FDreamProjectFiles::IsUnder(Item->Path, Node.Path))
		{
			return true;
		}
		if (Item->Kind == FDreamProjectNode::EKind::Group)
		{
			for (const FDreamProjectNodePtr& Child : Item->Children)
			{
				if (FDreamProjectFiles::IsUnder(Child->Path, Node.Path))
				{
					return true;   // a folder holding a plugin's Python folder
				}
			}
		}
	}
	return FDreamProjectFiles::IsUnder(DreamScratch::GetFolder(), Node.Path);
}

FText SDreamProjectTree::ProtectedMessage(const FDreamProjectNode& Node) const
{
	if (Node.bReadOnly)
	{
		return FText::Format(LOCTEXT("ProjectTree.EngineReadOnly", "{0} is part of the engine: the Project tree does not change it."),
			FText::FromString(Node.Name));
	}
	return FText::Format(LOCTEXT("ProjectTree.Protected", "{0} is a root or holds one (or the scratch folder): it is not renamed or deleted here."),
		FText::FromString(Node.Name));
}

bool SDreamProjectTree::IsProtectedForTest(const FString& Path) const
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	return Node.IsValid() && IsProtected(*Node);
}

// ---------------------------------------------------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------------------------------------------------

void SDreamProjectTree::CollectRows(const FDreamProjectNodePtr& Node, TArray<FRowForTest>& Out) const
{
	FRowForTest& Row = Out.AddDefaulted_GetRef();
	Row.Label = LabelOf(*Node).ToString();
	Row.Path = Node->Path;
	Row.Kind = Node->Kind;
	Row.Depth = Node->Depth;
	Row.bExpanded = (Node->IsFolderLike() || Node->Kind == FDreamProjectNode::EKind::Group) && TreeView->IsItemExpanded(Node);
	Row.bActive = IsActiveFile(*Node);
	Row.bMuted = IsMuted(*Node);
	if (Row.bExpanded)
	{
		TArray<FDreamProjectNodePtr> Children;
		const_cast<SDreamProjectTree*>(this)->ChildrenOf(Node, Children);
		for (const FDreamProjectNodePtr& Child : Children)
		{
			CollectRows(Child, Out);
		}
	}
}

TArray<SDreamProjectTree::FRowForTest> SDreamProjectTree::GetRowsForTest() const
{
	TArray<FRowForTest> Out;
	for (const FDreamProjectNodePtr& Item : TopItems)
	{
		CollectRows(Item, Out);
	}
	return Out;
}

bool SDreamProjectTree::IsIdleForTest() const
{
	if (!Files->IsIdleForTest() || AutoChecking.Num() > 0 || AutoRootsDirty->load())
	{
		return false;
	}
	for (const TPair<FString, TWeakPtr<FDreamProjectNode>>& Entry : FolderNodes)
	{
		const FDreamProjectNodePtr Node = Entry.Value.Pin();
		if (Node.IsValid() && (Node->bListing || (Node->bExists && !Node->bListed && TreeView->IsItemExpanded(Node) && !IsUnderClosedGroup(Node))))
		{
			return false;
		}
	}
	return true;
}

bool SDreamProjectTree::SetExpandedForTest(const FString& Path, bool bExpanded)
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	if (!Node.IsValid() || !Node->IsFolderLike())
	{
		return false;
	}
	TreeView->SetItemExpansion(Node, bExpanded);
	return true;
}

bool SDreamProjectTree::SetGroupExpandedForTest(bool bEngine, bool bExpanded)
{
	const FDreamProjectNodePtr& Group = bEngine ? EngineGroup : ProjectGroup;
	if (!TopItems.Contains(Group))
	{
		return false;
	}
	TreeView->SetItemExpansion(Group, bExpanded);
	return true;
}

ECheckBoxState SDreamProjectTree::GetContextMenuCheckForTest(const FString& Path, FName Name)
{
	FMenuBuilder Menu(true, nullptr);
	FillContextMenu(Menu, Path.IsEmpty() ? nullptr : FindNode(Path));
	for (const TSharedRef<const FMultiBlock>& Block : Menu.GetMultiBox()->GetBlocks())
	{
		if (Block->GetType() == EMultiBlockType::MenuEntry && Block->GetTutorialHighlightName() == Name)
		{
			return Block->GetDirectActions().GetCheckState();
		}
	}
	return ECheckBoxState::Undetermined;
}

bool SDreamProjectTree::ExecuteContextMenuForTest(const FString& Path, FName Name)
{
	FMenuBuilder Menu(true, nullptr);
	FillContextMenu(Menu, Path.IsEmpty() ? nullptr : FindNode(Path));
	for (const TSharedRef<const FMultiBlock>& Block : Menu.GetMultiBox()->GetBlocks())
	{
		if (Block->GetType() == EMultiBlockType::MenuEntry && Block->GetTutorialHighlightName() == Name)
		{
			return Block->GetDirectActions().Execute();
		}
	}
	return false;
}

FString SDreamProjectTree::GetAutomaticStateForTest(const FString& Path) const
{
	const FString Key = FDreamProjectFiles::PathKey(Path);
	if (AutoChecking.Contains(Key))
	{
		return TEXT("checking");
	}
	const bool* Exists = AutoExists.Find(Key);
	return Exists == nullptr ? FString(TEXT("unknown")) : FString(*Exists ? TEXT("exists") : TEXT("missing"));
}

bool SDreamProjectTree::SelectForTest(const FString& Path)
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	if (!Node.IsValid() || !TreeView.IsValid())
	{
		return false;
	}
	TreeView->SetSelection(Node, ESelectInfo::Direct);
	return true;
}

bool SDreamProjectTree::IsRemoteForTest(const FString& Path) const
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	return Node.IsValid() && Node->bRemote;
}

bool SDreamProjectTree::IsWatchedForTest(const FString& Path) const
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	return Node.IsValid() && Node->bWatched;
}

double SDreamProjectTree::GetRetryDelayForTest(const FString& Path) const
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	return Node.IsValid() ? Node->RetryDelay : -1.0;
}

TSharedPtr<SWidget> SDreamProjectTree::FindRowWidgetForTest(const FString& Path) const
{
	const FDreamProjectNodePtr Node = FindNode(Path);
	const TSharedPtr<ITableRow> Row = Node.IsValid() ? TreeView->WidgetFromItem(Node) : nullptr;
	return Row.IsValid() ? Row->AsWidget() : TSharedPtr<SWidget>();
}

TArray<TPair<FName, bool>> SDreamProjectTree::GetContextMenuForTest(const FString& Path)
{
	TArray<TPair<FName, bool>> Out;
	FMenuBuilder Menu(true, nullptr);
	FillContextMenu(Menu, Path.IsEmpty() ? nullptr : FindNode(Path));
	for (const TSharedRef<const FMultiBlock>& Block : Menu.GetMultiBox()->GetBlocks())
	{
		if (Block->GetType() == EMultiBlockType::MenuEntry)
		{
			Out.Emplace(Block->GetTutorialHighlightName(), Block->GetDirectActions().CanExecute());
		}
	}
	return Out;
}

FText SDreamProjectTree::GetMessageForTest() const
{
	return (!PendingDeletePath.IsEmpty() || FPlatformTime::Seconds() < MessageUntil) ? Message : FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE
