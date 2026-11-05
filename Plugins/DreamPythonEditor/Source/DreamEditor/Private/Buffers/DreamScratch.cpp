// SPDX-License-Identifier: Apache-2.0
// (scratch, PO-05) The scratch folder, scratch_N.py names and the sample scripts (Buffers/DreamScratch.h).
#include "Buffers/DreamScratch.h"

#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamFileWatch.h"
#include "HAL/PlatformFileManager.h"
#include "Containers/SortedMap.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Persistence/DreamWin32FileOps.h"
#include "Run/DreamDeferral.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/DreamProjectFiles.h"   // (R-p1-2) the Project tree's roots
#include "Tasks/Task.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamScratchLocal
{
	/** scratch_N.py: N is at most this (a folder that holds them all refuses a new one). */
	constexpr int32 MaxNumber = 99999;
	/** The marker of a seeded folder that is not the user's own (-DreamScratchFolder or the test override): those folders
	 *  never touch the per-user bSamplesSeeded flag, so a test pass cannot keep the user's real folder from being seeded. */
	const TCHAR* const SeededMarker = TEXT(".dream-samples-seeded");

	FCriticalSection Lock;
	FString CachedFolder;           // under Lock
	bool bCacheValid = false;       // under Lock
	FString TestOverride;           // game thread (copied into the cache under Lock)
	bool bSettingsBound = false;    // game thread
	bool bComputing = false;        // game thread: the roots check reads the Project tree's roots, whose Scratches item asks
	// The writability probe of EnsureFolder: a success is remembered per folder per session (game thread).
	FString ProbedFolder;
	// The warnings said once (game thread).
	FString WarnedNotWritable;
	FString WarnedSettingValue;
	FString PendingSettingWarning;   // the refusal to show once (an FString: no global FText)
	// (scratch-p3-m1) The scratch files this editor made this session (keys), and those of them closed unused since.
	TSet<FString> CreatedThisSession;
	TSet<FString> ClosedUnused;
	std::atomic<bool> bSeeding { false };
	double LastFlagWriteSeconds = 0.0;

	FSimpleMulticastDelegate& FolderChanged()
	{
		static FSimpleMulticastDelegate Delegate;
		return Delegate;
	}

	FString Normalize(const FString& In)
	{
		if (In.IsEmpty())
		{
			return FString();
		}
		// (scratch-p2-m2) A UNC path keeps its leading "//": RemoveDuplicateSlashes would make "\\server\share" the current
		// drive's "/server/share".
		const bool bUnc = In.StartsWith(TEXT("\\\\")) || In.StartsWith(TEXT("//"));
		FString Out = FPaths::ConvertRelativePathToFull(In);
		FPaths::NormalizeFilename(Out);
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

	bool PathsEqual(const FString& A, const FString& B)
	{
#if PLATFORM_WINDOWS
		return A.Equals(B, ESearchCase::IgnoreCase);
#else
		return A.Equals(B, ESearchCase::CaseSensitive);
#endif
	}

	/** Normalized Path is Folder or lies under it. */
	bool IsUnder(const FString& Normalized, const FString& Folder)
	{
		if (Folder.IsEmpty() || Normalized.Len() < Folder.Len())
		{
			return false;
		}
		if (Normalized.Len() == Folder.Len())
		{
			return PathsEqual(Normalized, Folder);
		}
		return PathsEqual(Normalized.Left(Folder.Len()), Folder) && (Normalized[Folder.Len()] == TEXT('/') || Folder.EndsWith(TEXT("/")));
	}

	FString CommandLineFolder()
	{
		FString Value;
		if (FParse::Value(FCommandLine::Get(), TEXT("DreamScratchFolder="), Value) && !Value.IsEmpty())
		{
			return Value;
		}
		return FString();
	}

	bool IsOverridden()
	{
		return !TestOverride.IsEmpty() || !CommandLineFolder().IsEmpty();
	}

	FString DefaultFolder()
	{
		// FPlatformProcess::UserSettingsDir is %LOCALAPPDATA% on Windows (the storage root's and the themes' base, §10.1).
		const FString Base = FPlatformProcess::UserSettingsDir();
		return Normalize((Base.IsEmpty() ? FPlatformProcess::UserTempDir() : Base) / TEXT("DreamPythonEditor") / TEXT("Scratches"));
	}

	FText WhyNotAllowedImpl(const FString& Folder, bool bWithRoots);

	/** bChecked (game thread): the setting is fully guarded (R-p1-2) and a refused value warns once. Off the game thread a
	 *  cold cache gets every check but the Project tree roots' (they are game-thread state), and the caller does not cache it. */
	FString ComputeFolder(bool bChecked)
	{
		if (!TestOverride.IsEmpty())
		{
			return Normalize(TestOverride);
		}
		const FString FromCommandLine = CommandLineFolder();
		if (!FromCommandLine.IsEmpty())
		{
			return Normalize(FromCommandLine);
		}
		if (UObjectInitialized())
		{
			const FString Setting = UDreamUserSettings::Get()->ScratchFolder.Path.TrimStartAndEnd();
			if (!Setting.IsEmpty())
			{
				if (!bChecked)
				{
					return WhyNotAllowedImpl(Setting, /*bWithRoots*/ false).IsEmpty() ? Normalize(Setting) : DefaultFolder();
				}
				const FText Why = WhyNotAllowedImpl(Setting, /*bWithRoots*/ true);
				if (Why.IsEmpty())
				{
					return Normalize(Setting);
				}
				if (WarnedSettingValue != Setting)
				{
					WarnedSettingValue = Setting;
					PendingSettingWarning = FText::Format(LOCTEXT("Scratch.NotAllowed", "Scratch folder {0} is not allowed: {1}; the default folder is used"),
						FText::FromString(Setting), Why).ToString();
					UE_LOG(LogDream, Warning, TEXT("%s"), *PendingSettingWarning);
				}
			}
		}
		return DefaultFolder();
	}

	/** Recomputes the folder (game thread); broadcasts OnFolderChanged when the answer changed. */
	void Refresh()
	{
		TGuardValue<bool> Computing(bComputing, true);
		const FString New = ComputeFolder(/*bChecked*/ true);
		bool bChanged = false;
		{
			FScopeLock Guard(&Lock);
			bChanged = bCacheValid && !PathsEqual(CachedFolder, New);
			CachedFolder = New;
			bCacheValid = true;
		}
		if (bChanged)
		{
			ProbedFolder.Reset();
			UE_LOG(LogDream, Log, TEXT("Scratch folder is now %s"), *New);
			FolderChanged().Broadcast();
		}
	}

	/** The setting is followed from the first game-thread call on (Editor Preferences edits and the tests' guard restores both
	 *  broadcast OnSettingChanged). Nothing in StartupModule. */
	void BindSettings()
	{
		if (bSettingsBound || !IsInGameThread() || !UObjectInitialized())
		{
			return;
		}
		bSettingsBound = true;
		UDreamUserSettings::GetMutable()->OnSettingChanged().AddLambda([](UObject*, FPropertyChangedEvent& Event)
		{
			const FName Property = Event.GetMemberPropertyName();
			if (Property.IsNone() || Property == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ScratchFolder))
			{
				WarnedSettingValue.Reset();   // a new value is judged (and warned about) afresh
				Refresh();
				if (!PendingSettingWarning.IsEmpty() && FSlateApplication::IsInitialized())
				{
					// (scratch-p2 nit) An edit in Editor Preferences hears of its refusal at once, not at the next new tab.
					FNotificationInfo Info(FText::FromString(PendingSettingWarning));
					Info.ExpireDuration = 8.f;
					FSlateNotificationManager::Get().AddNotification(Info);
					PendingSettingWarning.Reset();
				}
			}
		});
	}

	/** N of "scratch_N.py" (case-insensitive), 0 when the name is not one. */
	int32 NumberOf(const FString& CleanName)
	{
		if (!CleanName.StartsWith(TEXT("scratch_"), ESearchCase::IgnoreCase) || !CleanName.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
		{
			return 0;
		}
		const FString Digits = CleanName.Mid(8, CleanName.Len() - 8 - 3);
		if (Digits.IsEmpty() || Digits.Len() > 5)
		{
			return 0;
		}
		for (const TCHAR Ch : Digits)
		{
			if (!FChar::IsDigit(Ch))
			{
				return 0;
			}
		}
		return FCString::Atoi(*Digits);
	}

	/** The copy itself (any thread): <Samples> is made, and every bundled sample missing there is copied, none overwritten. */
	int32 CopySamples(const FString& Bundled, const FString& Samples, FText& OutError, int32& OutFailed)
	{
		OutFailed = 0;
		TArray<FString> Names;
		if (!Bundled.IsEmpty())
		{
			IFileManager::Get().FindFiles(Names, *(Bundled / TEXT("*.py")), /*Files*/ true, /*Directories*/ false);
		}
		if (Names.Num() == 0)
		{
			OutError = FText::Format(LOCTEXT("Scratch.NoSamples", "no sample scripts were found in {0}"), FText::FromString(Bundled));
			return -1;
		}
		if (!IFileManager::Get().DirectoryExists(*Samples) && !IFileManager::Get().MakeDirectory(*Samples, /*Tree*/ true))
		{
			OutError = FText::Format(LOCTEXT("Scratch.SamplesFolder", "{0} could not be created"), FText::FromString(Samples));
			return -1;
		}
		Names.Sort();
		int32 Copied = 0;
		for (const FString& Name : Names)
		{
			const FString Target = Samples / Name;
			if (IFileManager::Get().FileExists(*Target))
			{
				continue;   // never overwritten: the user's edits to a sample stay
			}
			if (FDreamWin32FileOps::CopyNew(Bundled / Name, Target))
			{
				++Copied;
			}
			else
			{
				++OutFailed;
				UE_LOG(LogDream, Warning, TEXT("Could not copy the sample %s to %s"), *Name, *Samples);
			}
		}
		if (OutFailed > 0)
		{
			OutError = FText::Format(LOCTEXT("Scratch.SamplesFailedCount", "{0} {0}|plural(one=sample,other=samples) could not be copied"),
				FText::AsNumber(OutFailed));
		}
		return Copied;
	}
}

FString DreamScratch::GetFolder()
{
	using namespace DreamScratchLocal;
	BindSettings();
	{
		FScopeLock Guard(&Lock);
		if (bCacheValid)
		{
			return CachedFolder;
		}
	}
	if (!IsInGameThread() || bComputing)
	{
		return ComputeFolder(/*bChecked*/ false);   // not cached: the game thread computes the checked answer
	}
	FString Folder;
	{
		TGuardValue<bool> Computing(bComputing, true);
		Folder = ComputeFolder(/*bChecked*/ true);
	}
	FScopeLock Guard(&Lock);
	if (!bCacheValid)
	{
		CachedFolder = Folder;
		bCacheValid = true;
	}
	return CachedFolder;
}

bool DreamScratch::IsScratchPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.StartsWith(TEXT("<")))
	{
		return false;
	}
	return DreamScratchLocal::IsUnder(DreamScratchLocal::Normalize(Path), GetFolder());
}

FSimpleMulticastDelegate& DreamScratch::OnFolderChanged()
{
	return DreamScratchLocal::FolderChanged();
}

void DreamScratch::SetFolderForTest(const FString& Folder)
{
	check(IsInGameThread());
	DreamScratchLocal::TestOverride = Folder;
	GetFolder();   // a cache to compare against
	DreamScratchLocal::Refresh();
}

FString DreamScratch::GetFolderForTest()
{
	return DreamScratchLocal::TestOverride;
}

FText DreamScratch::WhyNotAllowed(const FString& Folder)
{
	return DreamScratchLocal::WhyNotAllowedImpl(Folder, /*bWithRoots*/ IsInGameThread());
}

FText DreamScratchLocal::WhyNotAllowedImpl(const FString& Folder, bool bWithRoots)
{
	const FString Raw = Folder.TrimStartAndEnd();
	if (Raw.IsEmpty() || FPaths::IsRelative(Raw))
	{
		return LOCTEXT("Scratch.WhyRelative", "it is not an absolute path");
	}
	const FString N = Normalize(Raw);
	// A drive root ("C:/") or a share root ("//server/share"): every file on it would save itself.
	const bool bDrive = N.Len() <= 3 && N.Len() >= 2 && FChar::IsAlpha(N[0]) && N[1] == TEXT(':');
	TArray<FString> Parts;
	N.ParseIntoArray(Parts, TEXT("/"), /*CullEmpty*/ true);
	const bool bShare = N.StartsWith(TEXT("//")) && Parts.Num() <= 2;
	if (bDrive || bShare || N == TEXT("/"))
	{
		return LOCTEXT("Scratch.WhyRoot", "it is a drive root");
	}
	// The profile folder, or a folder that holds it (C:/Users): every file of the user would save itself.
	const FString Profile = Normalize(FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE")));
	if (!Profile.IsEmpty() && IsUnder(Profile, N))
	{
		return LOCTEXT("Scratch.WhyProfile", "it is your user profile folder or holds it");
	}
	const FString Project = Normalize(FPaths::ProjectDir());
	if (!Project.IsEmpty() && IsUnder(Project, N))
	{
		return LOCTEXT("Scratch.WhyProject", "it is the project folder or holds it");
	}
	const FString Engine = Normalize(FPaths::EngineDir());
	if (!Engine.IsEmpty() && (IsUnder(N, Engine) || IsUnder(Engine, N)))
	{
		return LOCTEXT("Scratch.WhyEngine", "it is in the engine folder or holds it");
	}
	if (bWithRoots && IsInGameThread())
	{
		TGuardValue<bool> Computing(bComputing, true);   // the roots' Scratches item asks GetFolder: its answer skips the roots
		for (const FDreamProjectRoot& Root : FDreamProjectFiles::ResolveRoots())
		{
			if (!Root.bScratches && !Root.Path.IsEmpty() && IsUnder(Normalize(Root.Path), N))
			{
				return FText::Format(LOCTEXT("Scratch.WhyRoot2", "it is the Project tree root {0} or holds it"), FText::FromString(Root.Path));
			}
		}
	}
	return FText::GetEmpty();
}

FString DreamScratch::GetSamplesFolder()
{
	return GetFolder() / TEXT("Samples");
}

bool DreamScratch::EnsureFolder(FText& OutError, bool bReprobe)
{
	using namespace DreamScratchLocal;
	const FString Folder = GetFolder();
	if (!bReprobe && PathsEqual(ProbedFolder, Folder))
	{
		OutError = FText::GetEmpty();
		return true;   // only a success is remembered: a folder that was not writable is probed again next time
	}
	ProbedFolder.Reset();
	IFileManager& Files = IFileManager::Get();
	bool bCreated = false;
	if (!Files.DirectoryExists(*Folder))
	{
		if (!Files.MakeDirectory(*Folder, /*Tree*/ true))
		{
			OutError = FText::Format(LOCTEXT("Scratch.CannotCreate", "the folder could not be created ({0})"),
				FText::FromString(FDreamWin32FileOps::GetLastErrorText()));
			return false;
		}
		bCreated = true;
	}
	// Writable: a probe file is made and removed (a folder can exist read-only, or under a policy that refuses new files).
	FDreamWin32FileOps Ops;
	FDreamWin32FileOps::FScopedNoRetryWait NoWait;
	const FString Probe = Folder / FString::Printf(TEXT(".dream-probe-%08x.tmp"), static_cast<uint32>(FPlatformTime::Cycles64()));
	const std::string ProbeUtf8 = FDreamWin32FileOps::ToUtf8Path(Probe);
	dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
	const bool bWritable = Ops.CreateExclusive(ProbeUtf8, H);
	if (bWritable)
	{
		Ops.Close(H);
		Ops.Remove(ProbeUtf8);
		ProbedFolder = Folder;
		OutError = FText::GetEmpty();
	}
	else
	{
		OutError = FText::Format(LOCTEXT("Scratch.CannotWrite", "a file could not be created there ({0})"),
			FText::FromString(FDreamWin32FileOps::ToFString(Ops.LastError())));
	}
	if (bCreated)
	{
		FolderChanged().Broadcast();   // (R-p1-7) the folder exists now: the Project tree's Scratches section lists it
	}
	return bWritable;
}

bool DreamScratch::IsScratchFileName(const FString& Path)
{
	using namespace DreamScratchLocal;
	const FString Normalized = Normalize(Path);
	return NumberOf(FPaths::GetCleanFilename(Normalized)) > 0 && PathsEqual(FPaths::GetPath(Normalized), GetFolder());
}

void DreamScratch::NoteClosedUnused(const FString& Path)
{
	const FString Key = FDreamFileWatch::PathKey(Path);
	if (DreamScratchLocal::CreatedThisSession.Contains(Key))
	{
		DreamScratchLocal::ClosedUnused.Add(Key);
	}
}

bool DreamScratch::IsCommandLineFolder()
{
	return DreamScratchLocal::TestOverride.IsEmpty() && !DreamScratchLocal::CommandLineFolder().IsEmpty();
}

bool DreamScratch::WarnNotWritableOnce(const FText& Why)
{
	const FString Folder = GetFolder();
	if (DreamScratchLocal::PathsEqual(DreamScratchLocal::WarnedNotWritable, Folder))
	{
		return false;
	}
	DreamScratchLocal::WarnedNotWritable = Folder;
	UE_LOG(LogDream, Warning, TEXT("Scratch folder is not writable: %s (%s); new tabs are untitled and journaled until it is"), *Folder, *Why.ToString());
	return true;
}

FText DreamScratch::TakeSettingWarning()
{
	GetFolder();   // a refusal is found when the folder is computed
	const FText Warning = FText::FromString(DreamScratchLocal::PendingSettingWarning);
	DreamScratchLocal::PendingSettingWarning.Reset();
	return Warning;
}

void DreamScratch::ResetWarningsForTest()
{
	DreamScratchLocal::WarnedNotWritable.Reset();
	DreamScratchLocal::WarnedSettingValue.Reset();
	DreamScratchLocal::PendingSettingWarning.Reset();
}

FString DreamScratch::CreateNextFile(FText& OutError)
{
	using namespace DreamScratchLocal;
	if (!EnsureFolder(OutError))
	{
		return FString();
	}
	const FString Folder = GetFolder();
	const FDreamBufferManager* Buffers = FDreamEditorModule::IsAvailable() ? &FDreamEditorModule::Get().GetBufferManager() : nullptr;
	// Taken: a scratch file on disk, or a path an open or closed tab still holds (a file deleted while its tab is open). One
	// directory pass gathers the names and sizes (scratch-p3-m2: no stat per file, which a share would make slow).
	struct FExisting
	{
		FString Path;
		int64 Size = 0;
	};
	TSortedMap<int32, FExisting> Existing;
	FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryStat(*Folder, [&Existing, &Folder](const TCHAR* Name, const FFileStatData& Stat)
	{
		if (!Stat.bIsDirectory)
		{
			const FString Clean = FPaths::GetCleanFilename(Name);
			if (const int32 N = NumberOf(Clean); N > 0)
			{
				Existing.Add(N, FExisting{ Folder / Clean, Stat.FileSize });
			}
		}
		return true;
	});
	// (R-p1-8, scratch-p3-m1) Used again: only an empty scratch file this editor made and then closed unused this session (a
	// Ctrl+T closed untouched, a window opened with no tabs), and no buffer of this editor still needs (IsPathBusy). A file of
	// another editor or an earlier session is never taken: that editor may still have it open.
	for (const TPair<int32, FExisting>& Pair : Existing)
	{
		const FString Key = FDreamFileWatch::PathKey(Pair.Value.Path);
		if (Pair.Value.Size == 0 && ClosedUnused.Contains(Key) && (Buffers == nullptr || !Buffers->IsPathBusy(Pair.Value.Path)))
		{
			ClosedUnused.Remove(Key);
			return Pair.Value.Path;
		}
	}
	FDreamWin32FileOps Ops;
	FDreamWin32FileOps::FScopedNoRetryWait NoWait;
	for (int32 N = 1; N <= MaxNumber; ++N)
	{
		if (Existing.Contains(N))
		{
			continue;
		}
		const FString Path = Folder / FString::Printf(TEXT("scratch_%d.py"), N);
		if (Buffers != nullptr && Buffers->IsPathInUse(Path))
		{
			continue;
		}
		// CREATE_NEW: another editor (a second instance shares the per-user folder) that took the name first wins the race.
		const std::string Utf8 = FDreamWin32FileOps::ToUtf8Path(Path);
		dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
		if (Ops.CreateExclusive(Utf8, H))
		{
			Ops.Close(H);
			CreatedThisSession.Add(FDreamFileWatch::PathKey(Path));
			return Path;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			OutError = FText::Format(LOCTEXT("Scratch.CreateFailed", "{0} could not be created ({1})"), FText::FromString(Path),
				FText::FromString(FDreamWin32FileOps::ToFString(Ops.LastError())));
			ProbedFolder.Reset();   // the next new tab probes the folder again
			return FString();
		}
	}
	OutError = LOCTEXT("Scratch.Full", "the folder holds every scratch file name");
	return FString();
}

FString DreamScratch::GetBundledSamplesFolder()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamPythonEditor"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Samples")) : FString();
}

int32 DreamScratch::CopyMissingSamples(FText& OutError, int32* OutFailed)
{
	int32 Failed = 0;
	if (IsInGameThread() && !EnsureFolder(OutError))
	{
		return -1;
	}
	const int32 Copied = DreamScratchLocal::CopySamples(GetBundledSamplesFolder(), GetSamplesFolder(), OutError, Failed);
	if (OutFailed != nullptr)
	{
		*OutFailed = Failed;
	}
	return Copied;
}

void DreamScratch::SeedSamplesOnce()
{
	using namespace DreamScratchLocal;
	const bool bOverridden = IsOverridden();
	const FString Marker = GetFolder() / SeededMarker;
	if (bSeeding.load() || (bOverridden ? IFileManager::Get().FileExists(*Marker) : UDreamUserSettings::Get()->bSamplesSeeded))
	{
		return;
	}
	FText Error;
	if (!EnsureFolder(Error))
	{
		return;
	}
	// (scratch-p1-m7) The copies run on a worker; the flag (or the override folder's marker) is written once every sample is
	// there, the flag on the game thread.
	struct FSeed
	{
		std::atomic<bool> bDone { false };
		int32 Copied = 0;
		int32 Failed = 0;
		FText Error;
	};
	const TSharedRef<FSeed, ESPMode::ThreadSafe> Seed = MakeShared<FSeed, ESPMode::ThreadSafe>();
	const FString Bundled = GetBundledSamplesFolder();
	const FString Samples = GetSamplesFolder();
	bSeeding.store(true);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Seed, Bundled, Samples, Marker, bOverridden]()
	{
		Seed->Copied = CopySamples(Bundled, Samples, Seed->Error, Seed->Failed);
		if (bOverridden && Seed->Copied >= 0 && Seed->Failed == 0)
		{
			FDreamWin32FileOps Ops;
			dream::journal::FileOps::Handle H = dream::journal::FileOps::InvalidHandle;
			if (Ops.CreateExclusive(FDreamWin32FileOps::ToUtf8Path(Marker), H))
			{
				Ops.Close(H);
			}
		}
		Seed->bDone.store(true, std::memory_order_release);
	});
	FDreamDeferral::AddTicker(TEXT("DreamSeedSamples"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, /*bNeedsPython*/ false,
		[Seed, Samples, bOverridden](float) -> bool
		{
			if (!Seed->bDone.load(std::memory_order_acquire))
			{
				return true;
			}
			bSeeding.store(false);
			if (Seed->Copied < 0 || Seed->Failed > 0)
			{
				UE_LOG(LogDream, Warning, TEXT("The sample scripts were not all copied to %s: %s"), *Samples, *Seed->Error.ToString());
				return false;   // tried again with the next new tab
			}
			UE_LOG(LogDream, Log, TEXT("Copied %d sample scripts to %s"), Seed->Copied, *Samples);
			if (!bOverridden && UObjectInitialized())
			{
				UDreamUserSettings::GetMutable()->bSamplesSeeded = true;
				UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bSamplesSeeded));
				LastFlagWriteSeconds = FPlatformTime::Seconds();
			}
			return false;
		});
}

bool DreamScratch::IsSeedingForTest()
{
	return DreamScratchLocal::bSeeding.load();
}

double DreamScratch::GetLastFlagWriteSecondsForTest()
{
	return DreamScratchLocal::LastFlagWriteSeconds;
}

#undef LOCTEXT_NAMESPACE
