// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 2) The file side of the Project tree:
// - Roots: UDreamProjectTreeSettings::ProjectRoots (Editor Preferences > Plugins > Dream (this project) > Project Roots, its
//   own settings class, R-p1-10), a relative path resolved against the project folder, after <Project>/Content/Python,
//   which shows first whenever it exists (the /Game/ root on sys.path; p3 ruling). A missing root is reported, never
//   created. The Scratches section
//   (DreamScratch::GetFolder) always comes last. PO-05-A1: with Add plugin Python folders on, <content root>/Python of every
//   mounted content root (FPackageName::QueryRootContentPaths, as Unreal's Python plugin builds sys.path) is an automatic
//   root, grouped "Project Plugins" or "Engine"; whether each exists is asked on a worker (CheckFolders).
// - Listing: folders first, then files, each sorted case-insensitively; dotfiles, __pycache__, *.pyc and *.pyo hidden; at
//   most MaxEntries per folder with the rest counted. Each folder is listed by its own worker task (UE::Tasks), so a slow
//   or offline root holds up only its own rows (review p1 m2); at most MaxListingsInFlight local and, apart from them,
//   MaxRemoteListingsInFlight network listings run at once (review p2 m3, p3 m1); a finished listing is applied on the game
//   thread from an
//   FDreamDeferral ticker (DeferWhileExecuting: never while a run is active, as FDreamFileWatch does). Nothing is listed in
//   StartupModule (B1): the tree asks when it is shown.
// - Watching: one IDirectoryWatcher registration per existing local root (subtree and folders included). Network roots
//   (UNC paths, mapped network drives) are not watched (R-p1-11: no CreateFileW on the game thread, no watcher warning);
//   they refresh on expand, on the tree's Refresh and on its retry. The callback queues the parent folder of each added,
//   removed or renamed entry whose name is not hidden (an auto-save's ".<name>.dream-*.tmp" is not news, p1 m10);
//   WatchDebounceSeconds after the last event the owner is told which folders changed.
// - File operations (new file, new folder, rename, delete to the Recycle Bin): each on a worker task, the result on the
//   game thread through the same ticker. Delete is never permanent (review p1 m3): IFileOperation with
//   FOFX_RECYCLEONDELETE and a progress sink that aborts any delete the shell could not recycle (its PreDeleteItem flags
//   lack TSF_DELETE_RECYCLE_IF_POSSIBLE), so no "permanently delete?" prompt can appear and nothing is destroyed; a drive
//   with no Recycle Bin (SHQueryRecycleBinW fails) is refused before. IFileManager::Delete is never used for user files.
//   Specs replace the recycle call with a seam that moves the item into a folder of their own (R-p1-5).
// Game thread API except the static helpers marked "any thread". Owner: sidebar.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"

/** PO-05-A1: the group an automatic root shows under. */
enum class EDreamRootGroup : uint8
{
	None,
	ProjectPlugins,             // project plugins and any other mount outside the engine
	Engine,                     // /Engine/ (Engine/Content/Python) and engine plugins: read-only in the tree
};

/** One top-level item of the Project tree: a root, or the Scratches section; or an automatic plugin root (PO-05-A1). */
struct FDreamProjectRoot
{
	FString Path;               // normalized absolute, forward slashes, no trailing slash
	FString Entered;            // the setting's text ("" for the default root and the Scratches section)
	bool bExists = false;       // known only for the default root (stated); the others learn it from their first listing
	bool bDefault = false;      // <Project>/Content/Python, first whenever it exists; not removable (p3 ruling)
	bool bReadOnly = false;     // a user root in the engine's folder: nothing in it is changed by the tree (p3 nit)
	bool bScratches = false;    // the Scratches section (always last, never removable)
	bool bRemote = false;       // a network path: listed, never watched (R-p1-11)
	bool bAutomatic = false;    // PO-05-A1: <content root>/Python of a mounted content root
	EDreamRootGroup Group = EDreamRootGroup::None;
	FString Label;              // an automatic root's label: the plugin's friendly name, "Engine", or the mount's name
};

struct FDreamProjectEntry
{
	FString Name;
	FString Path;               // normalized absolute
	bool bDirectory = false;
};

/** What one folder holds, as a worker listed it. */
struct FDreamFolderListing
{
	FString Folder;             // as requested
	TArray<FDreamProjectEntry> Entries;   // folders first, then files; each group case-insensitive; at most MaxEntries
	int32 MoreCount = 0;        // entries past MaxEntries
	bool bExists = true;        // false: the folder is gone (or is a file now)
};

enum class EDreamProjectFileOp : uint8
{
	NewFile,
	NewFolder,
	Rename,
	Recycle,
};

/** The outcome of one file operation, on the game thread. */
struct FDreamProjectFileResult
{
	EDreamProjectFileOp Op = EDreamProjectFileOp::NewFile;
	bool bOk = false;
	FString Path;               // the folder (new file or folder) or the source path (rename, delete)
	FString NewPath;            // the created or renamed path
	FText Error;                // why it failed, for the side bar's message line
};

class DREAMEDITOR_API FDreamProjectFiles : public TSharedFromThis<FDreamProjectFiles>
{
public:
	static constexpr int32 MaxEntries = 5000;
	static constexpr double WatchDebounceSeconds = 0.25;
	/** Listings run as background-low tasks, at most this many local ones at once; the rest wait in a queue (review p2 m3). */
	static constexpr int32 MaxListingsInFlight = 4;
	/** Network folders have a budget of their own (review p3 m1): a dead share never holds a local listing back. */
	static constexpr int32 MaxRemoteListingsInFlight = 2;
	/** A drive letter's type (network or not) is asked again after this long (GetDriveTypeW, review p3 nit). */
	static constexpr double DriveTypeSeconds = 5.0;

	using FOnListed = TFunction<void(const FDreamFolderListing&)>;
	/** Folders whose entries changed on disk (debounced); bAll: the watcher lost track (rescan every shown folder under Root). */
	using FOnChanged = TFunction<void(const TArray<FString>& /*Folders*/, const FString& /*Root*/, bool /*bAll*/)>;
	using FOnDone = TFunction<void(const FDreamProjectFileResult&)>;
	/** CheckFolders' answer: Exists[i] for Folders[i]. */
	using FOnChecked = TFunction<void(const TArray<FString>& /*Folders*/, const TArray<bool>& /*Exists*/)>;

	FDreamProjectFiles(FOnListed InOnListed, FOnChanged InOnChanged);
	~FDreamProjectFiles();

	/** Unregisters the watches and the ticker; in-flight work is dropped (a running worker keeps its own data). */
	void Shutdown();

	// ---- Roots and names (static) ----

	/** The tree's top-level items: <Project>/Content/Python when it exists, then the roots (settings or the test override; one
	 *  equal to Content/Python is not repeated), then the Scratches section. Stats only the default root (the local project
	 *  folder), never a user root. */
	static TArray<FDreamProjectRoot> ResolveRoots();
	/** PO-05-A1: <content root>/Python of each mounted content root except /Game/ (Content/Python is the default root), with
	 *  its group and label (IPluginManager: the plugin's friendly name; an engine plugin or /Engine/ goes under Engine), sorted
	 *  by group and label. Path math only: whether each folder exists is for CheckFolders. Empty while Add plugin Python
	 *  folders is off. GT. */
	static TArray<FDreamProjectRoot> ResolveAutomaticRoots();
	/** Add plugin Python folders (the setting, or the test override). GT. */
	static bool IsAddPluginFolders();
	/** Sets it for this project: memory, its ini key alone, the property-changed notification an open Editor Preferences page
	 *  follows, then OnRootsChanged. View > Side Bar > Plugin Python Folders and the tree's background menu call it. GT. */
	static void SetAddPluginFolders(bool bOn);
	/** A setting's text as a path: relative paths against the project folder; normalized, no trailing slash. */
	static FString ResolveRootPath(const FString& Entered);
	/** <Project>/Content/Python, normalized (or the test's stand-in). */
	static FString DefaultRootPath();
	/** True when Path lies in the engine's folder (FPaths::EngineDir). Any thread. */
	static bool IsEnginePath(const FString& Path);
	/** The comparison key of a path (normalized, case-folded on Windows): FDreamFileWatch::PathKey. Any thread. */
	static FString PathKey(const FString& Path);
	/** True when Path is Folder or lies under it (keys compared). Any thread. */
	static bool IsUnder(const FString& Path, const FString& Folder);
	/** A UNC path or a path on a mapped network drive (GetDriveTypeW: DRIVE_REMOTE, cached per drive letter for
	 *  DriveTypeSeconds). Any thread. */
	static bool IsRemotePath(const FString& Path);
	/** Dotfiles, __pycache__, *.pyc and *.pyo are not listed. Any thread. */
	static bool IsHidden(const FString& Name, bool bDirectory);
	/** *.py, *.pyw and *.pyi show in normal text, other files muted. Any thread. */
	static bool IsPythonFile(const FString& Name);
	/** Empty when Name can be a file or folder name; else why not (empty, a separator, a reserved character, "." or ".."). */
	static FText ValidateName(const FString& Name);
	/** Lists Folder now (the worker's body). Any thread; a call on the game thread is counted (GetGameThreadListingsForTest). */
	static FDreamFolderListing ListFolder(const FString& Folder, int32 Max = MaxEntries);
	/** This editor is a secondary instance (FDreamJournal role): its side bar changes no saved view state (review p1 nit). GT. */
	static bool IsSecondaryInstance();

	/** Adds Path to the roots (as given; a path already listed, by key, is not added twice) and saves that key alone. An open
	 *  Editor Preferences page shows the change. False when Path is empty, already a root, or Content/Python (always shown).
	 *  Under the test override only the override changes. GT. */
	static bool AddRoot(const FString& Path);
	/** Removes the roots entry that resolves to Path and saves the key. False when no entry does (the implicit default root
	 *  cannot be removed: it is not in the list). GT. */
	static bool RemoveRoot(const FString& Path);
	/** Writes ProjectRoots alone to the per-project ini (nothing under the test override). GT. */
	static void SaveRootsKey();

	// ---- Work (game thread) ----

	/** Lists Folder on its own worker task (BackgroundLow, at most MaxListingsInFlight at once, the rest queued); the result
	 *  arrives through OnListed on a later frame. A folder already being listed or queued is listed once more after that. */
	void RequestListing(const FString& Folder);
	/** Watches exactly these existing local folders (subtrees included); others are unregistered; network folders are
	 *  skipped. */
	void SetWatchedFolders(const TArray<FString>& Folders);
	void NewFile(const FString& Folder, const FString& Name, FOnDone OnDone);
	void NewFolder(const FString& Folder, const FString& Name, FOnDone OnDone);
	void Rename(const FString& Path, const FString& NewName, FOnDone OnDone);
	/** Moves Path (a file or a folder) to the Recycle Bin; refused where the drive has none or the shell cannot recycle it. */
	void Recycle(const FString& Path, FOnDone OnDone);
	/** Asks whether each folder exists, on one worker task (BackgroundLow); the answer arrives through OnDone on a later frame
	 *  (PO-05-A1: the automatic roots, never stated on the game thread). */
	void CheckFolders(const TArray<FString>& Folders, FOnChecked OnDone);

	// ---- Test hooks (Dream.Shell.ProjectTree, Dream.Shell.SideBar, Dream.Visual.Capture) ----

	/** No listing in flight or asked again, no operation and no debounce pending. */
	bool IsIdleForTest() const;
	/** The listing cap (MaxEntries); 0 restores it. */
	static void SetMaxEntriesForTest(int32 Max);
	static int32 GetMaxEntries();
	/** The roots come from Roots instead of the settings, and Add Root / Remove Root change only them; nothing is written to
	 *  the user's ini (review p1 m7). Null ends the override. */
	static void SetRootsForTest(const TArray<FString>* Roots);
	static bool HasRootsForTest();
	/** Add Root / Remove Root and the plugin folders toggle change the settings in memory and notify as usual but write no ini
	 *  (a spec checks the notifications an open preferences page gets, review p2 nit, PO-05-A1). */
	static void SetSettingsSaveOffForTest(bool bOff);
	/** Add plugin Python folders reads Value instead of the setting, and the toggle changes only it; null ends it. */
	static void SetPluginFoldersForTest(const bool* Value);
	/** Paths under Folder count as network paths (listed, never watched) until set back with an empty Folder (p2 m2). */
	static void SetRemoteForTest(const FString& Folder);
	/** Listings in flight and queued now (local and network); the network ones alone. */
	int32 GetListingsInFlightForTest() const { return ListJobs.Num(); }
	int32 GetListingsQueuedForTest() const { return ListQueue.Num() + RemoteQueue.Num(); }
	int32 GetRemoteListingsInFlightForTest() const { return InFlight(/*bRemote*/ true); }
	int32 GetRemoteListingsQueuedForTest() const { return RemoteQueue.Num(); }
	/** DefaultRootPath returns Path instead of <Project>/Content/Python until set back with an empty Path. */
	static void SetDefaultRootForTest(const FString& Path);
	/** Recycle reports "no Recycle Bin" whatever the drive has (the refusal branch), until set back. */
	static void SetRecycleBinMissingForTest(bool bMissing);
	/** R-p1-5: with a folder set, Recycle moves the item into it (a unique name) instead of the Recycle Bin and records the
	 *  call; empty ends the seam. */
	static void SetRecycleSeamForTest(const FString& Folder);
	static TArray<FString> GetRecycleSeamCallsForTest();
	/** The number of items in the Recycle Bin of Path's drive; -1 when it cannot be read (the opt-in real-recycle case). */
	static int64 GetRecycleBinItemCountForTest(const FString& Path);
	/** ListFolder calls made on the game thread since the editor started (must stay 0, review p1 m8). */
	static int32 GetGameThreadListingsForTest();
	/** Listings started by this object (a hidden temp file must start none, p1 m10). */
	int32 GetListingsStartedForTest() const { return ListingsStarted; }
	/** Listings of Folder started by this object. */
	int32 GetListingsOfForTest(const FString& Folder) const { return ListingsByKey.FindRef(PathKey(Folder)); }
	int32 GetWatchCountForTest() const;

private:
	struct FListJob;
	struct FOpJob;
	struct FCheckJob;
	struct FWatch
	{
		FString Folder;
		FDelegateHandle Handle;
	};

	void EnsureTicker();
	bool Tick();
	void StartListing(const FString& Folder);
	/** Queues Folder behind the others of its kind (local or network) unless it is queued already. */
	void Enqueue(const FString& Folder);
	/** Listings running now of one kind. */
	int32 InFlight(bool bRemote) const;
	void StartOp(EDreamProjectFileOp Op, const FString& Path, const FString& Name, FOnDone OnDone);
	void HandleDirectoryChanged(const TArray<struct FFileChangeData>& Changes, FString RootKey);

	FOnListed OnListed;
	FOnChanged OnChanged;
	TArray<TSharedRef<FListJob, ESPMode::ThreadSafe>> ListJobs;   // one per folder in flight
	TMap<FString, FString> ListAgain;                             // key -> folder: asked again while in flight
	TArray<FString> ListQueue;                                    // local folders waiting for a free listing slot
	TArray<FString> RemoteQueue;                                  // network folders waiting for one of their own slots
	TSet<FString> QueuedKeys;                                     // the keys in both queues (review p3 nit: no scan per request)
	TArray<TSharedRef<FOpJob, ESPMode::ThreadSafe>> Ops;
	TArray<TSharedRef<FCheckJob, ESPMode::ThreadSafe>> CheckJobs;
	TMap<FString, FWatch> Watches;                                // by PathKey of the root
	TMap<FString, TSet<FString>> PendingChanges;                  // root key -> folder keys changed since the last delivery
	TSet<FString> PendingRescans;                                 // root keys whose watcher asked for a rescan
	double ChangesDueSeconds = 0.0;
	FTSTicker::FDelegateHandle Ticker;
	int32 ListingsStarted = 0;
	TMap<FString, int32> ListingsByKey;
	bool bShutDown = false;
};
