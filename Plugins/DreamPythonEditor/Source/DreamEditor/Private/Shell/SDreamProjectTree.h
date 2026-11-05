// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 2; BUF-08 as amended) The Project section of the side bar: a tree of the project roots and, last,
// the Scratches section (DreamScratch::GetFolder). Roots come from Editor Preferences > Plugins > Dream (this project) >
// Project Roots (Shell/DreamProjectFiles.h); with none to show, one muted line "No project roots. Add one..." opens a folder
// picker. Folders list lazily when expanded, on a worker (FDreamProjectFiles), and refresh from DirectoryWatcher; folders
// first, then files, Python files in normal text and others muted, expand chevrons and ((icons) PO-06 F6) an icon per row (Shell/DreamIcons.h). The
// expanded folders are saved per project. A top-level item that is missing (a root not found, a scratch folder not made
// yet) is listed again while the tree is shown, so it fills in once the folder appears (review p1 M1): every RetrySeconds,
// a network one after 2 s, 4 s, ... up to MaxRemoteRetrySeconds (review p2 m3). A scratch file opened in a tab lists its
// folder again at once, watched or not (a network scratch folder, review p2 m2).
// Double-click or Enter opens a file through SDreamTab::OpenLocation (an open file is focused); the active tab's file is
// drawn on the current-line colour. The context menu: Open, New Python File..., New Folder..., Rename... (inline; an open
// tab follows), Delete (to the Recycle Bin after a confirm in the message line, never permanent), Refresh, Reveal in
// Explorer, Copy Path, Remove Root on a root, Add Root... on the background. Rename and Delete are refused on a root, the
// scratch folder and any folder that holds one (p1 m9). The inline editor closes on Esc (in it or in the tree), on a click
// elsewhere, and when code moves keyboard focus away while the editor app is active (a chord's command, the section
// hidden; an Alt-Tab keeps it, review p2 m1). Esc gives focus back to the code view. While its section is hidden the tree
// watches and lists nothing. PO-05-A1: with Add plugin Python folders on (Editor Preferences, View > Side Bar > Plugin Python
// Folders, the tree's background menu: one per-project setting, live), the Python folders of the enabled plugins follow the
// user's roots: a "Project Plugins" group (expanded at first) and an "Engine" group (collapsed at first; Engine/Content/Python
// and the engine plugins), each hidden while empty; a folder that does not exist is not shown (asked on a worker); mounts and
// dismounts rebuild them; a user root equal to one shows once, as the user's. Automatic entries have no Remove Root, and
// nothing under the Engine group (or in a user root in the engine's folder) is renamed, deleted or added to. An entry under
// a collapsed group is not listed until the group opens (review p3 m2). The project's Content/Python shows first whenever it
// exists (p3 ruling). Every colour is a theme slot read at paint (A187). Game
// thread. Owner: sidebar.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

#include <atomic>

class FDreamProjectFiles;
class SEditableTextBox;
class SDreamTab;
struct FDreamFolderListing;
struct FDreamProjectFileResult;

/** One row of the Project tree. */
struct FDreamProjectNode
{
	enum class EKind : uint8
	{
		Root,           // a project root (may be missing: "(not found)")
		Scratches,      // the Scratches section
		Folder,
		File,
		Loading,        // the placeholder child of a folder not listed yet (it gives the folder its chevron)
		More,           // "... n more" after MaxEntries
		NoRoots,        // "No project roots. Add one..."
		NewItem,        // the inline name editor of New Python File... / New Folder...
		Group,          // PO-05-A1: "Project Plugins" or "Engine", holding automatic roots
	};

	EKind Kind = EKind::File;
	FString Path;                       // normalized absolute; empty for placeholders
	FString Key;                        // FDreamProjectFiles::PathKey(Path)
	FString Name;                       // the label: the entry's name, a root's folder name
	TArray<TSharedPtr<FDreamProjectNode>> Children;
	TWeakPtr<FDreamProjectNode> Parent;
	TSharedPtr<FDreamProjectNode> Placeholder;   // Loading (and More) rows, kept so the tree keeps their identity
	TSharedPtr<FDreamProjectNode> MoreRow;
	bool bListed = false;               // a listing filled Children
	bool bListing = false;              // a listing is on its way
	bool bExists = true;                // false: a root that was not found
	bool bDefaultRoot = false;          // <Project>/Content/Python, first whenever it exists; not removable (p3 ruling)
	bool bRemote = false;               // under a network root: never watched, listed again on expand (R-p1-11)
	bool bWatched = false;              // a top-level item whose DirectoryWatcher registration is live
	bool bNewFolder = false;            // NewItem: a folder, else a Python file
	bool bAutomatic = false;            // PO-05-A1: a plugin's Python folder (a Root under a Group): no Remove Root
	bool bReadOnly = false;             // PO-05-A1: under the Engine group: no New, Rename or Delete (inherited by children)
	double RetryDelay = 0.0;            // a missing top-level item: seconds to its next look (0: not missing)
	double NextRetryAt = 0.0;           // FPlatformTime::Seconds() of that look
	int32 MoreCount = 0;
	int32 Depth = 0;

	bool IsFolderLike() const { return Kind == EKind::Root || Kind == EKind::Scratches || Kind == EKind::Folder; }
	/** A user root, the default root or the Scratches section (not an automatic root, which sits under a Group). */
	bool IsTopLevel() const { return (Kind == EKind::Root && !bAutomatic) || Kind == EKind::Scratches; }
};

using FDreamProjectNodePtr = TSharedPtr<FDreamProjectNode>;

class DREAMEDITOR_API SDreamProjectTree : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SDreamProjectTree) : _StartActive(true) {}
		/** The Project section is shown at start: false lists and watches nothing until SetActive(true) (review p2 nit). */
		SLATE_ARGUMENT(bool, StartActive)
	SLATE_END_ARGS()

	/** A missing top-level item is listed again this long after its last look while the tree is shown (R-p1-7); a network one
	 *  waits twice as long after each miss, up to MaxRemoteRetrySeconds (review p2 m3). */
	static constexpr double RetrySeconds = 2.0;
	static constexpr double MaxRemoteRetrySeconds = 60.0;

	void Construct(const FArguments& InArgs, const TWeakPtr<SDreamTab>& InTab);
	virtual ~SDreamProjectTree() override;

	/** The Project section is shown (true) or hidden: hidden, the tree watches nothing and lists nothing; shown again, it
	 *  lists what it shows once more (review p1 nit). */
	void SetActive(bool bInActive);
	bool IsActive() const { return bActive; }

	/** The roots again from the settings and the scratch folder (kept rows keep their children and expansion). */
	void RefreshRoots();
	/** Keyboard focus into the tree (Focus Project); the first row is selected when nothing is. */
	void FocusTree();
	/** The tree's list widget (the one that takes keyboard focus). */
	TSharedPtr<SWidget> GetFocusWidget() const;

	// ---- Actions (the context menu, keys, tests) ----

	/** Opens a file row in a Dream tab (an open one is focused); toggles a folder; Add Root... on the "No project roots" line. */
	void Activate(const FDreamProjectNodePtr& Node);
	/** The folder picker, then AddRoot. */
	void PromptAddRoot();
	bool AddRoot(const FString& Path);
	bool RemoveRoot(const FString& Path);
	void BeginNewItem(const FString& Folder, bool bFolder);
	void BeginRename(const FString& Path);
	/** Commits the inline editor's name (Enter); an invalid name keeps the editor open with the reason in the message line. */
	void CommitEdit(const FString& Name);
	void CancelEdit();
	/** Asks in the message line before Delete; ConfirmDelete moves the item to the Recycle Bin. */
	void RequestDelete(const FString& Path);
	void ConfirmDelete();
	void CancelDelete();
	/** Refresh: lists the folder (a file's folder; "" = every top-level item) and the folders shown under it again. */
	void Refresh(const FString& Path);
	void RevealInExplorer(const FString& Path) const;
	void CopyPath(const FString& Path) const;
	/** The side bar's message line (failures, the delete confirm). bError: in the warning colour. */
	void ShowMessage(const FText& Message, bool bError);

	// ---- SWidget ----

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	// ---- Test hooks (Dream.Shell.ProjectTree, Dream.Shell.SideBar, Dream.Visual.Capture) ----

	struct FRowForTest
	{
		FString Label;                  // as drawn
		FString Path;
		FDreamProjectNode::EKind Kind = FDreamProjectNode::EKind::File;
		int32 Depth = 0;
		bool bExpanded = false;
		bool bActive = false;           // drawn on the current-line colour (the active tab's file)
		bool bMuted = false;            // drawn in the muted text colour
	};
	/** The rows as the tree shows them now: top-level items and the children of expanded folders, in order. */
	TArray<FRowForTest> GetRowsForTest() const;
	/** No listing or file operation pending, and no expanded folder waiting for its listing. */
	bool IsIdleForTest() const;
	bool SetExpandedForTest(const FString& Path, bool bExpanded);
	/** The row widget of Path while it is generated (on screen), for a routed click. */
	TSharedPtr<SWidget> FindRowWidgetForTest(const FString& Path) const;
	/** The names and enabled states of the context menu of Path ("" = the background). */
	TArray<TPair<FName, bool>> GetContextMenuForTest(const FString& Path);
	FText GetMessageForTest() const;
	FString GetPendingDeleteForTest() const { return PendingDeletePath; }
	bool IsEditingForTest() const { return EditNode.IsValid(); }
	/** The inline name editor while it is in the tree. */
	TSharedPtr<SEditableTextBox> GetNameEditorForTest() const { return EditBox; }
	/** Rename and Delete are refused for Path (a root, the scratch folder, or a folder that holds one). */
	bool IsProtectedForTest(const FString& Path) const;
	FDreamProjectFiles* GetFilesForTest() const { return Files.Get(); }
	/** Selects the row of Path (the case presses keys on it next). */
	bool SelectForTest(const FString& Path);
	/** The row of Path is a network folder (never watched) / its folder is watched now. */
	bool IsRemoteForTest(const FString& Path) const;
	bool IsWatchedForTest(const FString& Path) const;
	/** A missing top-level item's wait before its next look (0 when it is not missing). */
	double GetRetryDelayForTest(const FString& Path) const;
	/** RefreshRoots ran this many times (Reset to Defaults, Add Root, the scratch folder moving). */
	int32 GetRootsRefreshCountForTest() const { return RootsRefreshCount; }
	/** Expands or collapses a PO-05-A1 group (bEngine: the Engine group, else Project Plugins); false when it is not shown. */
	bool SetGroupExpandedForTest(bool bEngine, bool bExpanded);
	/** The check state of the context menu entry Name of Path ("" = the background). */
	ECheckBoxState GetContextMenuCheckForTest(const FString& Path, FName Name);
	/** Runs the context menu entry Name of Path ("" = the background) as a click would; false when it is not there. */
	bool ExecuteContextMenuForTest(const FString& Path, FName Name);
	/** PO-05-A1: what the tree knows of an automatic root's folder: "unknown", "checking", "exists" or "missing". */
	FString GetAutomaticStateForTest(const FString& Path) const;

private:
	enum class EEdit : uint8 { None, NewItem, Rename };

	TSharedRef<ITableRow> MakeRow(FDreamProjectNodePtr Node, const TSharedRef<STableViewBase>& Owner);
	void ChildrenOf(FDreamProjectNodePtr Node, TArray<FDreamProjectNodePtr>& OutChildren);
	void HandleExpansionChanged(FDreamProjectNodePtr Node, bool bExpanded);
	void HandleDoubleClick(FDreamProjectNodePtr Node);
	void HandleClick(FDreamProjectNodePtr Node);
	TSharedPtr<SWidget> MakeContextMenu();
	void FillContextMenu(class FMenuBuilder& Menu, const FDreamProjectNodePtr& Node);
	FReply HandleTreeKey(const FGeometry& Geometry, const FKeyEvent& Key);

	void RequestListing(const FDreamProjectNodePtr& Node);
	void ApplyListing(const FDreamFolderListing& Listing);
	void HandleChanged(const TArray<FString>& Folders, const FString& Root, bool bAll);
	void HandleOpDone(const FDreamProjectFileResult& Result);
	/** A scratch file opened in a tab (a new scratch tab): its folder is listed again, watched or not (review p2 m2). */
	void HandleBufferOpened(const TSharedRef<class FDreamBuffer>& Buffer);
	/** bRemote for Node and every row under it (a root's network state is looked at again on RefreshRoots, p2 nit). */
	static void SetRemote(const FDreamProjectNodePtr& Node, bool bRemote);
	/** bReadOnly for Node and every row under it (a user root in the engine's folder, p3 nit). */
	static void SetReadOnly(const FDreamProjectNodePtr& Node, bool bReadOnly);
	/** Node sits under a plugin group that is collapsed: it is not listed until the group opens (review p3 m2). */
	bool IsUnderClosedGroup(const FDreamProjectNodePtr& Node) const;
	/** Node was expanded: its children saved as expanded open again. STreeView forgets the expansion of rows under a collapsed
	 *  parent, so a group or folder opened again would show them closed (and a plugin folder would never be listed). */
	void ReapplySavedExpansion(const FDreamProjectNodePtr& Node);
	/** PO-05-A1: CheckFolders' answer for automatic roots; the top level is rebuilt when an answer changed. */
	void ApplyChecks(const TArray<FString>& Folders, const TArray<bool>& Exists);
	/** PO-05-A1: asks again whether the automatic roots exist (Refresh of everything). */
	void RecheckAutomaticRoots();
	/** The protected-row message: under the engine, or a root / a folder holding one. */
	FText ProtectedMessage(const FDreamProjectNode& Node) const;
	void UpdateWatches();
	void SaveExpansion(const FDreamProjectNodePtr& Node, bool bExpanded);
	/** Open buffers at Old (or under it, for a folder) take the new path: the tab label, the journal META, the file watch. */
	void RetargetOpenBuffers(const FString& OldPath, const FString& NewPath) const;
	FDreamProjectNodePtr FindNode(const FString& Path) const;
	/** Drops Gone and every row under it (by its parents, not by path) from FolderNodes. */
	void ForgetRows(const FDreamProjectNodePtr& Gone);
	FDreamProjectNodePtr FolderForActions(const FDreamProjectNodePtr& Node) const;
	FDreamProjectNodePtr MakeNode(FDreamProjectNode::EKind Kind, const FString& Path, const FString& Name, const FDreamProjectNodePtr& Parent) const;
	FText LabelOf(const FDreamProjectNode& Node) const;
	bool IsMuted(const FDreamProjectNode& Node) const;
	bool IsActiveFile(const FDreamProjectNode& Node) const;
	/** A root, the scratch folder, or a folder that holds one: no Rename, no Delete (p1 m9). */
	bool IsProtected(const FDreamProjectNode& Node) const;
	void CollectRows(const FDreamProjectNodePtr& Node, TArray<FRowForTest>& Out) const;

	TWeakPtr<SDreamTab> Tab;
	TSharedPtr<FDreamProjectFiles> Files;
	TSharedPtr<STreeView<FDreamProjectNodePtr>> TreeView;
	TArray<FDreamProjectNodePtr> TopItems;
	/** Roots and folders by key (the listing targets); a folder shown twice (the scratch folder as a root too) has two rows. */
	TMultiMap<FString, TWeakPtr<FDreamProjectNode>> FolderNodes;
	FDreamProjectNodePtr NoRootsRow;
	// PO-05-A1: the plugin groups (kept, so the tree keeps their expansion), what the worker said about each automatic root's
	// folder (by key), the keys being asked now, and a mount or dismount seen since the last rebuild (any thread may set it).
	FDreamProjectNodePtr ProjectGroup;
	FDreamProjectNodePtr EngineGroup;
	TMap<FString, bool> AutoExists;
	TSet<FString> AutoChecking;
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> AutoRootsDirty = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);

	// Inline editing (New Python File..., New Folder..., Rename...)
	EEdit EditMode = EEdit::None;
	FDreamProjectNodePtr EditNode;                 // the NewItem row, or the row being renamed
	TSharedPtr<SEditableTextBox> EditBox;
	bool bFocusEditBox = false;
	bool bEditHadFocus = false;                    // the editor took keyboard focus once (losing it later closes it, p2 m1)
	bool bFocusTreeAfterEdit = false;              // a committed or cancelled edit gives keyboard focus back to the tree
	bool bRebuildRows = false;                     // rows built again on the next tick (never inside the editor's own commit)
	FString EditText;                              // the editor's text (kept when an invalid name keeps it open, p1 M2)

	bool bActive = true;
	int32 RootsRefreshCount = 0;
	int32 RefreshingRoots = 0;                     // inside RefreshRoots: the watches are set once at its end (p3 nit)
	FString PendingSelectPath;                     // selected once its row appears (a new or renamed item)

	// The message line
	FText Message;
	bool bMessageError = false;
	double MessageUntil = 0.0;
	FString PendingDeletePath;                     // the delete confirm is showing

	// The active tab's file (read once per frame for the rows)
	FString ActiveRawPath;
	FString ActiveKey;

	FDelegateHandle ProjectSettingsHandle;
	FDelegateHandle RootsChangedHandle;
	FDelegateHandle ScratchFolderHandle;
	FDelegateHandle BufferOpenedHandle;
	FDelegateHandle MountedHandle;
	FDelegateHandle DismountedHandle;
};
