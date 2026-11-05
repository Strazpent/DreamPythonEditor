// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 section 2) The Project tree's roots, listings, watches and file operations (Shell/DreamProjectFiles.h).
#include "Shell/DreamProjectFiles.h"

#include "Buffers/DreamFileWatch.h"
#include "Buffers/DreamScratch.h"
#include "CoreGlobals.h"
#include "DirectoryWatcherModule.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IDirectoryWatcher.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Persistence/DreamJournal.h"
#include "Run/DreamDeferral.h"
#include "Settings/DreamProjectTreeSettings.h"
#include "Tasks/Task.h"
#include "UObject/UnrealType.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamProjectFilesLocal
{
	const FName DirectoryWatcherName(TEXT("DirectoryWatcher"));

	std::atomic<int32> MaxEntriesOverride { 0 };
	std::atomic<bool> bRecycleBinMissingForTest { false };
	std::atomic<int32> GameThreadListings { 0 };

	// Test overrides, game thread only (set by specs, read where the roots are resolved and operations start).
	bool bRootsOverride = false;
	TArray<FString> RootsOverride;
	FString RecycleSeamFolder;
	TArray<FString> RecycleSeamCalls;
	FString RemoteFolderForTest;   // paths under it count as network paths (p2 m2)
	FString DefaultRootForTest;    // the default root's stand-in (p3 ruling specs)
	struct FDriveType
	{
		bool bRemote = false;
		double CheckedAt = 0.0;
	};
	TMap<TCHAR, FDriveType> DriveTypes;   // by upper-case drive letter, under RemoteLock (review p3 nit)
	bool bSettingsSaveOff = false; // Add Root / Remove Root / the plugin folders toggle write no ini (a spec of the notifications)
	TOptional<bool> PluginFoldersOverride;   // PO-05-A1: Add plugin Python folders as a spec sets it
	FCriticalSection RemoteLock;

	FString Normalize(const FString& Path)
	{
		// A UNC root keeps its leading "//" (scratch-p2-m2, as FDreamFileWatch::PathKey): "\\server\share" must not become the
		// current drive's "/server/share", which would list the wrong folder and key it apart from its open files.
		const bool bUnc = Path.StartsWith(TEXT("\\\\")) || Path.StartsWith(TEXT("//"));
		FString Out = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Out);   // forward slashes, no trailing slash
		FPaths::CollapseRelativeDirectories(Out);
		FPaths::RemoveDuplicateSlashes(Out);
		if (bUnc && !Out.StartsWith(TEXT("//")))
		{
			Out = TEXT("/") + Out;
		}
		while (Out.Len() > 3 && Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	/** The roots' texts as the settings (or the test override) hold them. */
	TArray<FString> RootTexts()
	{
		if (bRootsOverride)
		{
			return RootsOverride;
		}
		TArray<FString> Out;
		for (const FDirectoryPath& Root : UDreamProjectTreeSettings::Get()->ProjectRoots)
		{
			Out.Add(Root.Path);
		}
		return Out;
	}

	void SetRootTexts(const TArray<FString>& Texts)
	{
		if (bRootsOverride)
		{
			RootsOverride = Texts;
			return;
		}
		UDreamProjectTreeSettings* Settings = UDreamProjectTreeSettings::GetMutable();
		Settings->ProjectRoots.Reset();
		for (const FString& Text : Texts)
		{
			Settings->ProjectRoots.AddDefaulted_GetRef().Path = Text;
		}
	}

	/** An open Editor Preferences page and the tree follow a change made outside it (Add Root, Remove Root): the engine's
	 *  property-changed notification, which an open details view listens to (UDeveloperSettings::PostEditChangeProperty
	 *  does not call its Super, so it would not reach one, review p2 nit), then the roots' own broadcast for the tree. The
	 *  test override changes no settings, so only the tree hears of it. */
	void NotifyRootsChanged()
	{
		if (!bRootsOverride)
		{
			UDreamProjectTreeSettings* Settings = UDreamProjectTreeSettings::GetMutable();
			FProperty* Property = FindFProperty<FProperty>(UDreamProjectTreeSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, ProjectRoots));
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Settings, Event);
		}
		UDreamProjectTreeSettings::OnRootsChanged().Broadcast();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The Recycle Bin (Windows): never a permanent delete
// ---------------------------------------------------------------------------------------------------------------------

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace DreamProjectFilesLocal
{
	FString NativePath(const FString& Path)
	{
		return Path.Replace(TEXT("/"), TEXT("\\"));
	}

	// The Windows 8 shell flags by value (shobjidl_core.h guards their names behind NTDDI_WIN8, which UE's minimum may not be).
	constexpr DWORD RecycleOnDeleteFlag = 0x00080000;        // FOFX_RECYCLEONDELETE
	constexpr DWORD EarlyFailureFlag = 0x00100000;           // FOFX_EARLYFAILURE
	constexpr DWORD DeleteRecycleIfPossibleFlag = 0x00000080; // TSF_DELETE_RECYCLE_IF_POSSIBLE

	/** The Recycle Bin of Path's drive: false where there is none (a network share, a drive with it turned off). */
	bool QueryRecycleBin(const FString& Path, int64* OutItems = nullptr)
	{
		SHQUERYRBINFO Info = {};
		Info.cbSize = sizeof(Info);
		const FString Native = NativePath(Path);
		if (FAILED(::SHQueryRecycleBinW(*Native, &Info)))
		{
			return false;
		}
		if (OutItems != nullptr)
		{
			*OutItems = static_cast<int64>(Info.i64NumItems);
		}
		return true;
	}

	/** Watches one IFileOperation delete: refuses any delete the shell could not send to the Recycle Bin (its flags lack
	 *  TSF_DELETE_RECYCLE_IF_POSSIBLE: too large for the bin, a bin set to remove at once), so nothing is ever destroyed and
	 *  no prompt is shown (review p1 m3). A stack object: its reference count does not own it. */
	class FRecycleSink final : public IFileOperationProgressSink
	{
	public:
		bool bRefused = false;
		bool bRecycled = false;
		HRESULT DeleteResult = S_OK;

		ULONG STDMETHODCALLTYPE AddRef() override { return ++Refs; }
		ULONG STDMETHODCALLTYPE Release() override { return --Refs; }
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Riid, void** Out) override
		{
			if (Riid == __uuidof(IUnknown) || Riid == __uuidof(IFileOperationProgressSink))
			{
				*Out = static_cast<IFileOperationProgressSink*>(this);
				AddRef();
				return S_OK;
			}
			*Out = nullptr;
			return E_NOINTERFACE;
		}
		HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD Flags, IShellItem*) override
		{
			if ((Flags & DeleteRecycleIfPossibleFlag) == 0)
			{
				bRefused = true;
				return E_ABORT;   // the shell would destroy it: stop before it does
			}
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT Result, IShellItem* NewlyCreated) override
		{
			DeleteResult = Result;
			bRecycled = SUCCEEDED(Result) && NewlyCreated != nullptr;   // the item's new place in the Recycle Bin
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
		HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
		HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
		HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

	private:
		ULONG Refs = 1;
	};

	/** The worker body of a move to the Recycle Bin (any thread but the game thread). */
	bool RecycleWithShell(const FString& Path, FText& OutError)
	{
		const FText Name = FText::FromString(FPaths::GetCleanFilename(Path));
		if (!QueryRecycleBin(FPaths::GetPath(Path)))
		{
			OutError = FText::Format(LOCTEXT("ProjectTree.NoRecycleBin", "Not deleted: the drive of {0} has no Recycle Bin, and Dream Python Editor never deletes for good."), Name);
			return false;
		}
		// IFileOperation is a COM object; a task thread has no apartment of its own.
		const HRESULT Com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		IFileOperation* Operation = nullptr;
		IShellItem* Item = nullptr;
		DWORD Cookie = 0;
		FRecycleSink Sink;
		BOOL bAborted = 0;
		HRESULT Result = ::CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&Operation));
		if (SUCCEEDED(Result))
		{
			Result = Operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | EarlyFailureFlag | RecycleOnDeleteFlag);
		}
		if (SUCCEEDED(Result))
		{
			const FString Native = NativePath(Path);
			Result = ::SHCreateItemFromParsingName(*Native, nullptr, IID_PPV_ARGS(&Item));
		}
		if (SUCCEEDED(Result))
		{
			Result = Operation->Advise(&Sink, &Cookie);
		}
		if (SUCCEEDED(Result))
		{
			Result = Operation->DeleteItem(Item, nullptr);
		}
		if (SUCCEEDED(Result))
		{
			Result = Operation->PerformOperations();
		}
		if (Operation != nullptr)
		{
			Operation->GetAnyOperationsAborted(&bAborted);
			if (Cookie != 0)
			{
				Operation->Unadvise(Cookie);
			}
			Operation->Release();
		}
		if (Item != nullptr)
		{
			Item->Release();
		}
		if (SUCCEEDED(Com))
		{
			::CoUninitialize();
		}
		if (Sink.bRefused)
		{
			OutError = FText::Format(LOCTEXT("ProjectTree.NotRecyclable", "Not deleted: {0} could not be recycled (too large for the Recycle Bin, or the bin deletes at once), and Dream Python Editor never deletes for good."), Name);
			return false;
		}
		if (FAILED(Result) || bAborted != 0 || FAILED(Sink.DeleteResult) || !Sink.bRecycled)
		{
			OutError = FText::Format(LOCTEXT("ProjectTree.RecycleFailed2", "Could not move {0} to the Recycle Bin (error 0x{1})."), Name,
				FText::FromString(FString::Printf(TEXT("%08X"), static_cast<uint32>(FAILED(Result) ? Result : Sink.DeleteResult))));
			return false;
		}
		return true;
	}
}

#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace DreamProjectFilesLocal
{
	/** The worker body of Delete: the refusal hook, the specs' seam (R-p1-5), else the shell's Recycle Bin. */
	bool RecyclePath(const FString& Path, const FString& SeamFolder, FText& OutError)
	{
		const FText Name = FText::FromString(FPaths::GetCleanFilename(Path));
		if (bRecycleBinMissingForTest.load())
		{
			OutError = FText::Format(LOCTEXT("ProjectTree.NoRecycleBin", "Not deleted: the drive of {0} has no Recycle Bin, and Dream Python Editor never deletes for good."), Name);
			return false;
		}
		if (!SeamFolder.IsEmpty())
		{
			IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
			Platform.CreateDirectoryTree(*SeamFolder);
			const FString Target = SeamFolder / FString::Printf(TEXT("%s_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8), *FPaths::GetCleanFilename(Path));
			if (!Platform.MoveFile(*Target, *Path))
			{
				OutError = FText::Format(LOCTEXT("ProjectTree.RecycleFailed", "Could not move {0} to the Recycle Bin."), Name);
				return false;
			}
			return true;
		}
#if PLATFORM_WINDOWS
		return RecycleWithShell(Path, OutError);
#else
		OutError = LOCTEXT("ProjectTree.NoRecycleBinPlatform", "Not deleted: this platform has no Recycle Bin, and Dream Python Editor never deletes for good.");
		return false;
#endif
	}
}

struct FDreamProjectFiles::FListJob
{
	FString Folder;
	FString Key;
	bool bRemote = false;          // counted against the network budget (review p3 m1)
	int32 Max = FDreamProjectFiles::MaxEntries;
	FDreamFolderListing Result;
	std::atomic<bool> bDone { false };
};

struct FDreamProjectFiles::FCheckJob
{
	TArray<FString> Folders;
	TArray<bool> Exists;
	FOnChecked OnDone;
	std::atomic<bool> bDone { false };
};

struct FDreamProjectFiles::FOpJob
{
	EDreamProjectFileOp Op = EDreamProjectFileOp::NewFile;
	FString Path;
	FString Name;
	FString SeamFolder;           // the specs' recycle seam, copied when the job starts
	FDreamProjectFileResult Result;
	FOnDone OnDone;
	std::atomic<bool> bDone { false };
};

FDreamProjectFiles::FDreamProjectFiles(FOnListed InOnListed, FOnChanged InOnChanged)
	: OnListed(MoveTemp(InOnListed))
	, OnChanged(MoveTemp(InOnChanged))
{
}

FDreamProjectFiles::~FDreamProjectFiles()
{
	Shutdown();
}

void FDreamProjectFiles::Shutdown()
{
	if (bShutDown)
	{
		return;
	}
	bShutDown = true;
	SetWatchedFolders({});
	ListJobs.Reset();   // a worker still running keeps its job alive and its result is dropped
	ListAgain.Reset();
	ListQueue.Reset();
	RemoteQueue.Reset();
	QueuedKeys.Reset();
	Ops.Reset();
	CheckJobs.Reset();
	PendingChanges.Reset();
	PendingRescans.Reset();
	FDreamDeferral::RemoveTicker(Ticker);
	Ticker.Reset();
}

// ---- Roots and names ----

void UDreamProjectTreeSettings::MigrateOldRootsOnce()
{
	// (sidebar, R-p1-10) The first PO-05 build kept the roots in UDreamPerProjectUserSettings' section.
	static bool bDone = false;
	if (bDone || GConfig == nullptr)
	{
		return;
	}
	bDone = true;
	const TCHAR* const OldSection = TEXT("/Script/DreamEditor.DreamPerProjectUserSettings");
	TArray<FString> Old;
	GConfig->GetArray(OldSection, TEXT("ProjectRoots"), Old, GEditorPerProjectIni);
	if (Old.Num() == 0)
	{
		return;
	}
	UDreamProjectTreeSettings* Settings = GetMutable();
	if (Settings->ProjectRoots.Num() == 0)
	{
		for (const FString& Text : Old)
		{
			FString Path;
			if (FParse::Value(*Text, TEXT("Path="), Path) && !Path.IsEmpty())   // (Path="C:/...")
			{
				Settings->ProjectRoots.AddDefaulted_GetRef().Path = Path;
			}
		}
		UObject::FSaveConfigContext Context;
		Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, ProjectRoots));
		Settings->SaveConfig(Context);
	}
	GConfig->RemoveKey(OldSection, TEXT("ProjectRoots"), GEditorPerProjectIni);
	GConfig->Flush(/*bRemoveFromCache*/ false, GEditorPerProjectIni);
	UE_LOG(LogDream, Log, TEXT("Dream Python Editor moved %d Project Roots to their own settings section"), Old.Num());
}

FSimpleMulticastDelegate& UDreamProjectTreeSettings::OnRootsChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

void UDreamProjectTreeSettings::PostReloadConfig(FProperty* PropertyThatWasLoaded)
{
	Super::PostReloadConfig(PropertyThatWasLoaded);
	// Reset to Defaults on the Editor Preferences page empties the section and reloads it (SettingsSection.cpp): the tree
	// shows the roots as they are now (review p2 nit). Only on the game thread, where the tree listens.
	if (IsInGameThread())
	{
		OnRootsChanged().Broadcast();
	}
}

FString FDreamProjectFiles::PathKey(const FString& Path)
{
	return FDreamFileWatch::PathKey(Path);
}

bool FDreamProjectFiles::IsUnder(const FString& Path, const FString& Folder)
{
	const FString P = PathKey(Path);
	const FString F = PathKey(Folder);
	return P == F || (P.StartsWith(F) && P.Len() > F.Len() && (P[F.Len()] == TCHAR('/') || F.EndsWith(TEXT("/"))));
}

bool FDreamProjectFiles::IsRemotePath(const FString& Path)
{
	{
		FScopeLock Lock(&DreamProjectFilesLocal::RemoteLock);
		if (!DreamProjectFilesLocal::RemoteFolderForTest.IsEmpty() && IsUnder(Path, DreamProjectFilesLocal::RemoteFolderForTest))
		{
			return true;   // a spec's stand-in for a network folder (p2 m2)
		}
	}
	FString Full = Path.Replace(TEXT("\\"), TEXT("/"));
	if (Full.StartsWith(TEXT("//")))
	{
		return true;   // a UNC path
	}
#if PLATFORM_WINDOWS
	if (Full.Len() >= 2 && Full[1] == TCHAR(':'))
	{
		// A mapped network drive (asks the local drive table only), once per drive letter for a few seconds: the tree asks for
		// every root and listing, and a drive mapped or unmapped later is still seen (review p3 nit).
		const TCHAR Letter = FChar::ToUpper(Full[0]);
		const double Now = FPlatformTime::Seconds();
		FScopeLock Lock(&DreamProjectFilesLocal::RemoteLock);
		DreamProjectFilesLocal::FDriveType& Type = DreamProjectFilesLocal::DriveTypes.FindOrAdd(Letter);
		if (Type.CheckedAt <= 0.0 || Now - Type.CheckedAt > DriveTypeSeconds)
		{
			const TCHAR Root[] = { Letter, TCHAR(':'), TCHAR('\\'), TCHAR(0) };
			Type.bRemote = ::GetDriveTypeW(Root) == DRIVE_REMOTE;
			Type.CheckedAt = Now;
		}
		return Type.bRemote;
	}
#endif
	return false;
}

FString FDreamProjectFiles::ResolveRootPath(const FString& Entered)
{
	FString Path = Entered.TrimStartAndEnd().TrimQuotes();
	if (Path.IsEmpty())
	{
		return Path;
	}
	FPaths::NormalizeFilename(Path);
	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::Combine(FPaths::ProjectDir(), Path);   // relative roots resolve against the project folder
	}
	return DreamProjectFilesLocal::Normalize(Path);
}

FString FDreamProjectFiles::DefaultRootPath()
{
	if (!DreamProjectFilesLocal::DefaultRootForTest.IsEmpty())
	{
		return DreamProjectFilesLocal::Normalize(DreamProjectFilesLocal::DefaultRootForTest);
	}
	return DreamProjectFilesLocal::Normalize(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Python")));
}

bool FDreamProjectFiles::IsEnginePath(const FString& Path)
{
	static const FString EngineDir = DreamProjectFilesLocal::Normalize(FPaths::EngineDir());
	return IsUnder(Path, EngineDir);
}

void FDreamProjectFiles::SetDefaultRootForTest(const FString& Path)
{
	DreamProjectFilesLocal::DefaultRootForTest = Path;
}

TArray<FDreamProjectRoot> FDreamProjectFiles::ResolveRoots()
{
	UDreamProjectTreeSettings::MigrateOldRootsOnce();
	TArray<FDreamProjectRoot> Roots;
	TSet<FString> Seen;
	// (p3 ruling) The project's Content/Python, the /Game/ root on sys.path, comes first whenever it exists, with or without
	// roots of the user's; an equal user root is not repeated. The project's own folder is local: one stat here is fine.
	const FString Default = DefaultRootPath();
	if (IFileManager::Get().DirectoryExists(*Default))
	{
		FDreamProjectRoot& Root = Roots.AddDefaulted_GetRef();
		Root.Path = Default;
		Root.bExists = true;
		Root.bDefault = true;
		Seen.Add(PathKey(Default));
	}
	for (const FString& Text : DreamProjectFilesLocal::RootTexts())
	{
		const FString Path = ResolveRootPath(Text);
		if (Path.IsEmpty() || Seen.Contains(PathKey(Path)))
		{
			continue;
		}
		Seen.Add(PathKey(Path));
		FDreamProjectRoot& Root = Roots.AddDefaulted_GetRef();
		Root.Path = Path;
		Root.Entered = Text;
		Root.bRemote = IsRemotePath(Path);
		Root.bReadOnly = IsEnginePath(Path);   // in the engine's folder: as read-only as the Engine group (p3 nit)
		// Not stated here: a root on an unreachable share could block the game thread. Its first listing says whether it exists.
	}
	FDreamProjectRoot& Scratches = Roots.AddDefaulted_GetRef();
	Scratches.Path = DreamProjectFilesLocal::Normalize(DreamScratch::GetFolder());
	Scratches.bScratches = true;
	Scratches.bRemote = IsRemotePath(Scratches.Path);
	return Roots;
}

TArray<FDreamProjectRoot> FDreamProjectFiles::ResolveAutomaticRoots()
{
	TArray<FDreamProjectRoot> Out;
	if (!IsAddPluginFolders())
	{
		return Out;
	}
	// The roots Unreal's Python plugin puts on sys.path (PythonScriptPlugin.cpp: QueryRootContentPaths, then
	// LongPackageNameToFilename(root) / "Python" for each): enabled plugins with content, /Engine/ and /Game/.
	TArray<FString> RootPaths;
	FPackageName::QueryRootContentPaths(RootPaths);
	TMap<FString, TSharedRef<IPlugin>> PluginsByMount;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
	{
		PluginsByMount.Add(Plugin->GetMountedAssetPath(), Plugin);
	}
	TSet<FString> Seen;
	for (const FString& RootPath : RootPaths)
	{
		if (RootPath == TEXT("/Game/"))
		{
			continue;   // <Project>/Content/Python: the default root
		}
		FString ContentDir;
		if (!FPackageName::TryConvertLongPackageNameToFilename(RootPath, ContentDir))
		{
			continue;
		}
		FDreamProjectRoot Root;
		Root.Path = DreamProjectFilesLocal::Normalize(ContentDir / TEXT("Python"));
		if (Root.Path.IsEmpty() || Seen.Contains(PathKey(Root.Path)))
		{
			continue;
		}
		Seen.Add(PathKey(Root.Path));
		Root.bAutomatic = true;
		Root.bRemote = IsRemotePath(Root.Path);
		if (RootPath == TEXT("/Engine/"))
		{
			Root.Group = EDreamRootGroup::Engine;
			Root.Label = LOCTEXT("ProjectTree.EngineContent", "Engine").ToString();
		}
		else if (const TSharedRef<IPlugin>* Plugin = PluginsByMount.Find(RootPath))
		{
			Root.Group = (*Plugin)->GetLoadedFrom() == EPluginLoadedFrom::Engine ? EDreamRootGroup::Engine : EDreamRootGroup::ProjectPlugins;
			Root.Label = (*Plugin)->GetFriendlyName().IsEmpty() ? (*Plugin)->GetName() : (*Plugin)->GetFriendlyName();
		}
		else
		{
			// A mount point no plugin owns (registered by code): grouped by where its folder is, named by the mount.
			Root.Group = IsEnginePath(Root.Path) ? EDreamRootGroup::Engine : EDreamRootGroup::ProjectPlugins;
			Root.Label = RootPath.Replace(TEXT("/"), TEXT(""));
		}
		Out.Add(MoveTemp(Root));
	}
	Out.Sort([](const FDreamProjectRoot& A, const FDreamProjectRoot& B)
	{
		if (A.Group != B.Group)
		{
			return A.Group < B.Group;   // Project Plugins, then Engine
		}
		const int32 Order = A.Label.Compare(B.Label, ESearchCase::IgnoreCase);
		return Order != 0 ? Order < 0 : A.Path < B.Path;
	});
	return Out;
}

bool FDreamProjectFiles::IsAddPluginFolders()
{
	if (DreamProjectFilesLocal::PluginFoldersOverride.IsSet())
	{
		return DreamProjectFilesLocal::PluginFoldersOverride.GetValue();
	}
	return UDreamProjectTreeSettings::Get()->bAddPluginPythonFolders;
}

void FDreamProjectFiles::SetAddPluginFolders(bool bOn)
{
	if (IsAddPluginFolders() == bOn)
	{
		return;
	}
	if (DreamProjectFilesLocal::PluginFoldersOverride.IsSet())
	{
		DreamProjectFilesLocal::PluginFoldersOverride = bOn;   // a spec's value: no setting, no ini
		UDreamProjectTreeSettings::OnRootsChanged().Broadcast();
		return;
	}
	UDreamProjectTreeSettings* Settings = UDreamProjectTreeSettings::GetMutable();
	Settings->bAddPluginPythonFolders = bOn;
	if (!DreamProjectFilesLocal::bSettingsSaveOff)
	{
		UObject::FSaveConfigContext Context;
		Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, bAddPluginPythonFolders));
		Settings->SaveConfig(Context);
	}
	// An open Editor Preferences page and the View menu read the value as it is now; the tree rebuilds on OnRootsChanged.
	FProperty* Property = FindFProperty<FProperty>(UDreamProjectTreeSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, bAddPluginPythonFolders));
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Settings, Event);
	UDreamProjectTreeSettings::OnRootsChanged().Broadcast();
}

bool FDreamProjectFiles::IsSecondaryInstance()
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return false;
	}
	const FDreamJournal& Journal = FDreamEditorModule::Get().GetJournal();
	return Journal.IsOpen() && Journal.GetRole() == EDreamInstanceRole::Secondary;
}

bool FDreamProjectFiles::IsHidden(const FString& Name, bool bDirectory)
{
	if (Name.StartsWith(TEXT(".")))
	{
		return true;
	}
	if (bDirectory)
	{
		return Name.Equals(TEXT("__pycache__"), ESearchCase::IgnoreCase);
	}
	const FString Extension = FPaths::GetExtension(Name);
	return Extension.Equals(TEXT("pyc"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("pyo"), ESearchCase::IgnoreCase);
}

bool FDreamProjectFiles::IsPythonFile(const FString& Name)
{
	const FString Extension = FPaths::GetExtension(Name);
	return Extension.Equals(TEXT("py"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("pyw"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT("pyi"), ESearchCase::IgnoreCase);
}

FText FDreamProjectFiles::ValidateName(const FString& Name)
{
	if (Name.TrimStartAndEnd().IsEmpty())
	{
		return LOCTEXT("ProjectTree.NameEmpty", "Type a name.");
	}
	if (Name == TEXT(".") || Name == TEXT(".."))
	{
		return LOCTEXT("ProjectTree.NameDots", "That name is not allowed.");
	}
	for (const TCHAR Char : Name)
	{
		if (Char < 32 || FCString::Strchr(TEXT("/\\:*?\"<>|"), Char) != nullptr)
		{
			return LOCTEXT("ProjectTree.NameChars", "A name cannot hold / \\ : * ? \" < > | or control characters.");
		}
	}
	if (Name.EndsWith(TEXT(" ")) || Name.EndsWith(TEXT(".")))
	{
		return LOCTEXT("ProjectTree.NameEnd", "A name cannot end with a space or a dot.");
	}
	static const TCHAR* const Reserved[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"), TEXT("COM1"), TEXT("COM2"), TEXT("COM3"),
		TEXT("COM4"), TEXT("COM5"), TEXT("COM6"), TEXT("COM7"), TEXT("COM8"), TEXT("COM9"), TEXT("LPT1"), TEXT("LPT2"), TEXT("LPT3"),
		TEXT("LPT4"), TEXT("LPT5"), TEXT("LPT6"), TEXT("LPT7"), TEXT("LPT8"), TEXT("LPT9") };
	const FString Base = FPaths::GetBaseFilename(Name);
	for (const TCHAR* Word : Reserved)
	{
		if (Base.Equals(Word, ESearchCase::IgnoreCase))
		{
			return LOCTEXT("ProjectTree.NameReserved", "Windows reserves that name.");
		}
	}
	return FText::GetEmpty();
}

FDreamFolderListing FDreamProjectFiles::ListFolder(const FString& Folder, int32 Max)
{
	if (IsInGameThread())
	{
		++DreamProjectFilesLocal::GameThreadListings;   // must never happen (review p1 m8): the specs check it stays 0
	}
	FDreamFolderListing Out;
	Out.Folder = Folder;
	IFileManager& Files = IFileManager::Get();
	if (Folder.IsEmpty() || !Files.DirectoryExists(*Folder))
	{
		Out.bExists = false;
		return Out;
	}
	TArray<FDreamProjectEntry> Directories;
	TArray<FDreamProjectEntry> Plain;
	Files.IterateDirectory(*Folder, [&Directories, &Plain](const TCHAR* FilenameOrDirectory, bool bIsDirectory) -> bool
	{
		FString Path(FilenameOrDirectory);
		FPaths::NormalizeFilename(Path);
		FString Name = FPaths::GetCleanFilename(Path);
		if (!FDreamProjectFiles::IsHidden(Name, bIsDirectory))
		{
			FDreamProjectEntry& Entry = (bIsDirectory ? Directories : Plain).AddDefaulted_GetRef();
			Entry.Name = MoveTemp(Name);
			Entry.Path = MoveTemp(Path);
			Entry.bDirectory = bIsDirectory;
		}
		return true;
	});
	auto ByName = [](const FDreamProjectEntry& A, const FDreamProjectEntry& B)
	{
		const int32 Order = A.Name.Compare(B.Name, ESearchCase::IgnoreCase);
		return Order != 0 ? Order < 0 : A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
	};
	Directories.Sort(ByName);
	Plain.Sort(ByName);
	Out.Entries = MoveTemp(Directories);
	Out.Entries.Append(MoveTemp(Plain));
	const int32 Cap = FMath::Max(1, Max);
	if (Out.Entries.Num() > Cap)
	{
		Out.MoreCount = Out.Entries.Num() - Cap;
		Out.Entries.SetNum(Cap);
	}
	return Out;
}

bool FDreamProjectFiles::AddRoot(const FString& Path)
{
	UDreamProjectTreeSettings::MigrateOldRootsOnce();
	const FString Trimmed = Path.TrimStartAndEnd();
	const FString Key = PathKey(ResolveRootPath(Trimmed));
	if (Trimmed.IsEmpty() || Key.IsEmpty())
	{
		return false;
	}
	if (Key == PathKey(DefaultRootPath()))
	{
		return false;   // Content/Python always shows (p3 ruling): adding it would add nothing
	}
	TArray<FString> Texts = DreamProjectFilesLocal::RootTexts();
	for (const FString& Text : Texts)
	{
		const FString Resolved = ResolveRootPath(Text);
		if (!Resolved.IsEmpty() && PathKey(Resolved) == Key)
		{
			return false;
		}
	}
	// (p3 ruling) The default root no longer depends on the list being empty, so nothing is kept for it here (R7 retired).
	Texts.Add(Trimmed);
	DreamProjectFilesLocal::SetRootTexts(Texts);
	SaveRootsKey();
	DreamProjectFilesLocal::NotifyRootsChanged();
	return true;
}

bool FDreamProjectFiles::RemoveRoot(const FString& Path)
{
	UDreamProjectTreeSettings::MigrateOldRootsOnce();
	const FString Key = PathKey(Path);
	TArray<FString> Texts = DreamProjectFilesLocal::RootTexts();
	const int32 Removed = Texts.RemoveAll([&Key](const FString& Text)
	{
		const FString Resolved = ResolveRootPath(Text);
		return !Resolved.IsEmpty() && PathKey(Resolved) == Key;
	});
	if (Removed == 0)
	{
		return false;
	}
	DreamProjectFilesLocal::SetRootTexts(Texts);
	SaveRootsKey();
	DreamProjectFilesLocal::NotifyRootsChanged();
	return true;
}

void FDreamProjectFiles::SaveRootsKey()
{
	if (DreamProjectFilesLocal::bRootsOverride || DreamProjectFilesLocal::bSettingsSaveOff)
	{
		return;   // a spec's roots never reach the user's ini (review p1 m7)
	}
	UObject::FSaveConfigContext Context;
	Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, ProjectRoots));
	UDreamProjectTreeSettings::GetMutable()->SaveConfig(Context);
}

// ---- Work ----

void FDreamProjectFiles::RequestListing(const FString& Folder)
{
	check(IsInGameThread());
	if (bShutDown || Folder.IsEmpty())
	{
		return;
	}
	const FString Key = PathKey(Folder);
	if (ListJobs.ContainsByPredicate([&Key](const TSharedRef<FListJob, ESPMode::ThreadSafe>& Job) { return Job->Key == Key; }))
	{
		ListAgain.Add(Key, Folder);   // its result may predate the request: list once more after it
		return;
	}
	if (QueuedKeys.Contains(Key))
	{
		return;   // already waiting for a slot
	}
	// Review p2 m3: a Refresh of everything never floods the task pool. Review p3 m1: network folders wait for their own slots,
	// so four dead shares never keep Scratches or a local root on "...".
	const bool bRemote = IsRemotePath(Folder);
	if (InFlight(bRemote) >= (bRemote ? MaxRemoteListingsInFlight : MaxListingsInFlight))
	{
		Enqueue(Folder);
		EnsureTicker();
		return;
	}
	StartListing(Folder);
}

void FDreamProjectFiles::Enqueue(const FString& Folder)
{
	const FString Key = PathKey(Folder);
	if (QueuedKeys.Contains(Key))
	{
		return;
	}
	QueuedKeys.Add(Key);
	(IsRemotePath(Folder) ? RemoteQueue : ListQueue).Add(Folder);
}

int32 FDreamProjectFiles::InFlight(bool bRemote) const
{
	int32 Count = 0;
	for (const TSharedRef<FListJob, ESPMode::ThreadSafe>& Job : ListJobs)
	{
		Count += Job->bRemote == bRemote ? 1 : 0;
	}
	return Count;
}

void FDreamProjectFiles::StartListing(const FString& Folder)
{
	// One task per folder (review p1 m2): a slow or offline root holds up only its own rows.
	TSharedRef<FListJob, ESPMode::ThreadSafe> Job = MakeShared<FListJob, ESPMode::ThreadSafe>();
	Job->Folder = Folder;
	Job->Key = PathKey(Folder);
	Job->bRemote = IsRemotePath(Folder);
	Job->Max = GetMaxEntries();
	ListJobs.Add(Job);
	++ListingsStarted;
	++ListingsByKey.FindOrAdd(Job->Key);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job]()
	{
		Job->Result = FDreamProjectFiles::ListFolder(Job->Folder, Job->Max);
		Job->bDone.store(true, std::memory_order_release);
	}, UE::Tasks::ETaskPriority::BackgroundLow);   // review p2 m3: never ahead of the editor's own work
	EnsureTicker();
}

void FDreamProjectFiles::SetWatchedFolders(const TArray<FString>& Folders)
{
	check(IsInGameThread());
	TMap<FString, FString> Wanted;
	if (!bShutDown)
	{
		for (const FString& Folder : Folders)
		{
			if (!Folder.IsEmpty() && !IsRemotePath(Folder))   // R-p1-11: network folders are never watched
			{
				Wanted.Add(PathKey(Folder), Folder);
			}
		}
	}
	FDirectoryWatcherModule* Module = Wanted.Num() > 0
		? &FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(DreamProjectFilesLocal::DirectoryWatcherName)
		: FModuleManager::GetModulePtr<FDirectoryWatcherModule>(DreamProjectFilesLocal::DirectoryWatcherName);
	IDirectoryWatcher* Watcher = Module != nullptr ? Module->Get() : nullptr;
	for (auto It = Watches.CreateIterator(); It; ++It)
	{
		if (!Wanted.Contains(It->Key))
		{
			if (Watcher != nullptr && It->Value.Handle.IsValid())
			{
				Watcher->UnregisterDirectoryChangedCallback_Handle(It->Value.Folder, It->Value.Handle);
			}
			It.RemoveCurrent();
		}
	}
	for (const TPair<FString, FString>& Entry : Wanted)
	{
		if (Watches.Contains(Entry.Key))
		{
			continue;
		}
		FWatch& Watch = Watches.Add(Entry.Key);
		Watch.Folder = Entry.Value;
		if (Watcher == nullptr)
		{
			continue;   // no watcher on this platform: the tree still lists on expand
		}
		// The subtree is watched too (no IgnoreChangesInSubtree), folders included, so an expanded subfolder refreshes.
		if (!Watcher->RegisterDirectoryChangedCallback_Handle(Entry.Value,
			IDirectoryWatcher::FDirectoryChanged::CreateSP(this, &FDreamProjectFiles::HandleDirectoryChanged, Entry.Key), Watch.Handle,
			IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges))
		{
			UE_LOG(LogDream, Log, TEXT("Dream's Project tree could not watch %s; it lists that folder again when it is expanded"), *Entry.Value);
			Watch.Handle.Reset();
		}
	}
}

int32 FDreamProjectFiles::GetWatchCountForTest() const
{
	int32 Count = 0;
	for (const TPair<FString, FWatch>& Watch : Watches)
	{
		Count += Watch.Value.Handle.IsValid() ? 1 : 0;
	}
	return Count;
}

void FDreamProjectFiles::HandleDirectoryChanged(const TArray<FFileChangeData>& Changes, FString RootKey)
{
	// The watcher calls this on the game thread from the editor tick: only queue (ARCHITECTURE section 3).
	if (bShutDown)
	{
		return;
	}
	bool bAny = false;
	for (const FFileChangeData& Change : Changes)
	{
		if (Change.Action == FFileChangeData::FCA_RescanRequired)
		{
			PendingRescans.Add(RootKey);
			bAny = true;
		}
		else if (Change.Action == FFileChangeData::FCA_Added || Change.Action == FFileChangeData::FCA_Removed)
		{
			// A modification changes no listing (a scratch tab's auto-save writes every second), nor does an entry the tree
			// hides (the auto-save's ".<name>.dream-*.tmp", __pycache__, p1 m10); an add, a remove or a rename (reported as
			// both) changes the parent folder's. A hidden folder's contents are not shown either.
			const FString Name = FPaths::GetCleanFilename(Change.Filename);
			if (IsHidden(Name, /*bDirectory*/ false) || Name.Equals(TEXT("__pycache__"), ESearchCase::IgnoreCase)
				|| Change.Filename.Contains(TEXT("/__pycache__/")) || Change.Filename.Contains(TEXT("\\__pycache__\\")))
			{
				continue;
			}
			PendingChanges.FindOrAdd(RootKey).Add(PathKey(FPaths::GetPath(Change.Filename)));
			bAny = true;
		}
	}
	if (bAny)
	{
		ChangesDueSeconds = FPlatformTime::Seconds() + WatchDebounceSeconds;   // each event restarts the quiet time
		EnsureTicker();
	}
}

void FDreamProjectFiles::NewFile(const FString& Folder, const FString& Name, FOnDone OnDone)
{
	StartOp(EDreamProjectFileOp::NewFile, Folder, Name, MoveTemp(OnDone));
}

void FDreamProjectFiles::NewFolder(const FString& Folder, const FString& Name, FOnDone OnDone)
{
	StartOp(EDreamProjectFileOp::NewFolder, Folder, Name, MoveTemp(OnDone));
}

void FDreamProjectFiles::Rename(const FString& Path, const FString& NewName, FOnDone OnDone)
{
	StartOp(EDreamProjectFileOp::Rename, Path, NewName, MoveTemp(OnDone));
}

void FDreamProjectFiles::Recycle(const FString& Path, FOnDone OnDone)
{
	StartOp(EDreamProjectFileOp::Recycle, Path, FString(), MoveTemp(OnDone));
}

void FDreamProjectFiles::StartOp(EDreamProjectFileOp Op, const FString& Path, const FString& Name, FOnDone OnDone)
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return;
	}
	TSharedRef<FOpJob, ESPMode::ThreadSafe> Job = MakeShared<FOpJob, ESPMode::ThreadSafe>();
	Job->Op = Op;
	Job->Path = Path;
	Job->Name = Name;
	Job->SeamFolder = DreamProjectFilesLocal::RecycleSeamFolder;
	Job->OnDone = MoveTemp(OnDone);
	Job->Result.Op = Op;
	Job->Result.Path = Path;
	Ops.Add(Job);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job]()
	{
		// Worker: file system only (no Slate, no UObjects).
		FDreamProjectFileResult& Result = Job->Result;
		IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
		auto Exists = [&Platform](const FString& P) { return Platform.FileExists(*P) || Platform.DirectoryExists(*P); };
		switch (Job->Op)
		{
		case EDreamProjectFileOp::NewFile:
		case EDreamProjectFileOp::NewFolder:
		{
			const FString Target = Job->Path / Job->Name;
			Result.NewPath = Target;
			if (Exists(Target))
			{
				Result.Error = FText::Format(LOCTEXT("ProjectTree.Exists", "{0} already exists."), FText::FromString(Job->Name));
			}
			else if (Job->Op == EDreamProjectFileOp::NewFolder)
			{
				Result.bOk = Platform.CreateDirectory(*Target);
				if (!Result.bOk)
				{
					Result.Error = FText::Format(LOCTEXT("ProjectTree.NewFolderFailed", "Could not create the folder {0}."), FText::FromString(Job->Name));
				}
			}
			else
			{
				// An empty file, never over one made meanwhile (nit: the check above and the write are not one step).
				Result.bOk = FFileHelper::SaveArrayToFile(TArray<uint8>(), *Target, &IFileManager::Get(), FILEWRITE_NoReplaceExisting);
				if (!Result.bOk)
				{
					Result.Error = FText::Format(LOCTEXT("ProjectTree.NewFileFailed", "Could not create {0}."), FText::FromString(Job->Name));
				}
			}
			break;
		}
		case EDreamProjectFileOp::Rename:
		{
			const FString Target = FPaths::GetPath(Job->Path) / Job->Name;
			Result.NewPath = Target;
			const bool bSameName = FPaths::GetCleanFilename(Job->Path).Equals(Job->Name, ESearchCase::CaseSensitive);
			const bool bCaseOnly = !bSameName && FDreamProjectFiles::PathKey(Target) == FDreamProjectFiles::PathKey(Job->Path);
			if (bSameName)
			{
				Result.bOk = true;   // nothing to do
			}
			else if (!Exists(Job->Path))
			{
				Result.Error = FText::Format(LOCTEXT("ProjectTree.RenameGone", "{0} is no longer there."), FText::FromString(FPaths::GetCleanFilename(Job->Path)));
			}
			else if (!bCaseOnly && Exists(Target))
			{
				Result.Error = FText::Format(LOCTEXT("ProjectTree.Exists", "{0} already exists."), FText::FromString(Job->Name));
			}
			else
			{
				Result.bOk = Platform.MoveFile(*Target, *Job->Path);   // MoveFileW never overwrites
				if (!Result.bOk)
				{
					Result.Error = FText::Format(LOCTEXT("ProjectTree.RenameFailed", "Could not rename {0} (it may be open in another program)."),
						FText::FromString(FPaths::GetCleanFilename(Job->Path)));
				}
			}
			break;
		}
		case EDreamProjectFileOp::Recycle:
		default:
			Result.bOk = DreamProjectFilesLocal::RecyclePath(Job->Path, Job->SeamFolder, Result.Error);
			break;
		}
		Job->bDone.store(true, std::memory_order_release);
	});
	EnsureTicker();
}

void FDreamProjectFiles::CheckFolders(const TArray<FString>& Folders, FOnChecked OnDone)
{
	check(IsInGameThread());
	if (bShutDown || Folders.Num() == 0)
	{
		return;
	}
	const TSharedRef<FCheckJob, ESPMode::ThreadSafe> Job = MakeShared<FCheckJob, ESPMode::ThreadSafe>();
	Job->Folders = Folders;
	Job->OnDone = MoveTemp(OnDone);
	CheckJobs.Add(Job);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job]()
	{
		IFileManager& FileManager = IFileManager::Get();
		Job->Exists.SetNumZeroed(Job->Folders.Num());
		for (int32 Index = 0; Index < Job->Folders.Num(); ++Index)
		{
			Job->Exists[Index] = FileManager.DirectoryExists(*Job->Folders[Index]);
		}
		Job->bDone.store(true, std::memory_order_release);
	}, UE::Tasks::ETaskPriority::BackgroundLow);
	EnsureTicker();
}

void FDreamProjectFiles::EnsureTicker()
{
	if (Ticker.IsValid() || bShutDown)
	{
		return;
	}
	// RT-01, section 3.7: results that change the tree (and open buffers' paths) are applied only while no run is active.
	TWeakPtr<FDreamProjectFiles> Weak = AsShared();
	Ticker = FDreamDeferral::AddTicker(TEXT("DreamProjectFiles"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, /*bNeedsPython*/ false,
		[Weak](float) -> bool
		{
			const TSharedPtr<FDreamProjectFiles> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->Tick();
		});
}

bool FDreamProjectFiles::Tick()
{
	if (bShutDown)
	{
		Ticker.Reset();
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	if ((PendingChanges.Num() > 0 || PendingRescans.Num() > 0) && Now >= ChangesDueSeconds)
	{
		TMap<FString, TSet<FString>> Changes = MoveTemp(PendingChanges);
		TSet<FString> Rescans = MoveTemp(PendingRescans);
		PendingChanges.Reset();
		PendingRescans.Reset();
		// A copy: the owner may change the watched set from its handler.
		TArray<TPair<FString, FString>> Roots;
		for (const TPair<FString, FWatch>& Watch : Watches)
		{
			Roots.Emplace(Watch.Key, Watch.Value.Folder);
		}
		for (const TPair<FString, FString>& Root : Roots)
		{
			if (Rescans.Contains(Root.Key))
			{
				OnChanged({}, Root.Value, /*bAll*/ true);
			}
			else if (const TSet<FString>* Folders = Changes.Find(Root.Key))
			{
				OnChanged(Folders->Array(), Root.Value, /*bAll*/ false);
			}
			if (bShutDown)
			{
				return false;
			}
		}
	}
	// Each listing is delivered as soon as its own task is done.
	for (int32 Index = 0; Index < ListJobs.Num();)
	{
		if (!ListJobs[Index]->bDone.load(std::memory_order_acquire))
		{
			++Index;
			continue;
		}
		const TSharedRef<FListJob, ESPMode::ThreadSafe> Job = ListJobs[Index];
		ListJobs.RemoveAt(Index);
		OnListed(Job->Result);
		if (bShutDown)
		{
			return false;
		}
		FString Again;
		if (ListAgain.RemoveAndCopyValue(Job->Key, Again))
		{
			Enqueue(Again);   // behind the folders already waiting
		}
	}
	// Each kind fills its own free slots (review p3 m1).
	for (const bool bRemote : { false, true })
	{
		TArray<FString>& Queue = bRemote ? RemoteQueue : ListQueue;
		const int32 Max = bRemote ? MaxRemoteListingsInFlight : MaxListingsInFlight;
		while (Queue.Num() > 0 && InFlight(bRemote) < Max)
		{
			const FString Next = Queue[0];
			Queue.RemoveAt(0);
			QueuedKeys.Remove(PathKey(Next));
			StartListing(Next);
		}
	}
	for (int32 Index = 0; Index < Ops.Num();)
	{
		if (!Ops[Index]->bDone.load(std::memory_order_acquire))
		{
			++Index;
			continue;
		}
		const TSharedRef<FOpJob, ESPMode::ThreadSafe> Job = Ops[Index];
		Ops.RemoveAt(Index);
		if (Job->Op == EDreamProjectFileOp::Recycle && !Job->SeamFolder.IsEmpty() && Job->Result.bOk)
		{
			DreamProjectFilesLocal::RecycleSeamCalls.Add(Job->Path);   // R-p1-5: the specs' record of the seam's calls
		}
		if (Job->OnDone)
		{
			Job->OnDone(Job->Result);
		}
		if (bShutDown)
		{
			return false;
		}
	}
	for (int32 Index = 0; Index < CheckJobs.Num();)
	{
		if (!CheckJobs[Index]->bDone.load(std::memory_order_acquire))
		{
			++Index;
			continue;
		}
		const TSharedRef<FCheckJob, ESPMode::ThreadSafe> Job = CheckJobs[Index];
		CheckJobs.RemoveAt(Index);
		if (Job->OnDone)
		{
			Job->OnDone(Job->Folders, Job->Exists);
		}
		if (bShutDown)
		{
			return false;
		}
	}
	const bool bMore = ListJobs.Num() > 0 || ListQueue.Num() > 0 || RemoteQueue.Num() > 0 || Ops.Num() > 0 || CheckJobs.Num() > 0
		|| PendingChanges.Num() > 0 || PendingRescans.Num() > 0;
	if (!bMore)
	{
		Ticker.Reset();
	}
	return bMore;
}

// ---- Test hooks ----

bool FDreamProjectFiles::IsIdleForTest() const
{
	return ListJobs.Num() == 0 && ListAgain.Num() == 0 && ListQueue.Num() == 0 && RemoteQueue.Num() == 0 && Ops.Num() == 0
		&& CheckJobs.Num() == 0 && PendingChanges.Num() == 0 && PendingRescans.Num() == 0;
}

void FDreamProjectFiles::SetMaxEntriesForTest(int32 Max)
{
	DreamProjectFilesLocal::MaxEntriesOverride.store(FMath::Max(0, Max));
}

int32 FDreamProjectFiles::GetMaxEntries()
{
	const int32 Override = DreamProjectFilesLocal::MaxEntriesOverride.load();
	return Override > 0 ? Override : MaxEntries;
}

void FDreamProjectFiles::SetRootsForTest(const TArray<FString>* Roots)
{
	DreamProjectFilesLocal::bRootsOverride = Roots != nullptr;
	DreamProjectFilesLocal::RootsOverride = Roots != nullptr ? *Roots : TArray<FString>();
}

bool FDreamProjectFiles::HasRootsForTest()
{
	return DreamProjectFilesLocal::bRootsOverride;
}

void FDreamProjectFiles::SetSettingsSaveOffForTest(bool bOff)
{
	DreamProjectFilesLocal::bSettingsSaveOff = bOff;
}

void FDreamProjectFiles::SetPluginFoldersForTest(const bool* Value)
{
	DreamProjectFilesLocal::PluginFoldersOverride = Value != nullptr ? TOptional<bool>(*Value) : TOptional<bool>();
}

void FDreamProjectFiles::SetRemoteForTest(const FString& Folder)
{
	FScopeLock Lock(&DreamProjectFilesLocal::RemoteLock);
	DreamProjectFilesLocal::RemoteFolderForTest = Folder;
}

void FDreamProjectFiles::SetRecycleBinMissingForTest(bool bMissing)
{
	DreamProjectFilesLocal::bRecycleBinMissingForTest.store(bMissing);
}

void FDreamProjectFiles::SetRecycleSeamForTest(const FString& Folder)
{
	DreamProjectFilesLocal::RecycleSeamFolder = Folder;
	DreamProjectFilesLocal::RecycleSeamCalls.Reset();
}

TArray<FString> FDreamProjectFiles::GetRecycleSeamCallsForTest()
{
	return DreamProjectFilesLocal::RecycleSeamCalls;
}

int64 FDreamProjectFiles::GetRecycleBinItemCountForTest(const FString& Path)
{
#if PLATFORM_WINDOWS
	int64 Items = -1;
	return DreamProjectFilesLocal::QueryRecycleBin(Path, &Items) ? Items : -1;
#else
	return -1;
#endif
}

int32 FDreamProjectFiles::GetGameThreadListingsForTest()
{
	return DreamProjectFilesLocal::GameThreadListings.load();
}

#undef LOCTEXT_NAMESPACE
