// SPDX-License-Identifier: Apache-2.0
// External changes (TDD §10.15 M1, BUF-04): directory watches, deferred stamp checks on a worker, self-write stamps.
#include "Buffers/DreamFileWatch.h"

#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamHash.h"
#include "DirectoryWatcherModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "IDirectoryWatcher.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamWin32FileOps.h"   // (scratch, p3-m2) IsRemotePath
#include "Run/DreamDeferral.h"
#include "Run/DreamRunService.h"
#include "Tasks/Task.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamFileWatchLocal
{
	const FName DirectoryWatcherName(TEXT("DirectoryWatcher"));

	/** The self-write stamps of saves in flight (§10.14 step 6): the saver's worker writes, the check worker reads. */
	struct FExpected
	{
		int64 Size = 0;
		FString Sha256Hex;
		double Seconds = 0.0;
	};

	FCriticalSection& ExpectedLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}

	TMap<FString, TArray<FExpected>>& ExpectedStamps()
	{
		static TMap<FString, TArray<FExpected>> Stamps;
		return Stamps;
	}

	/** Caller holds ExpectedLock. */
	void PruneExpected(double Now)
	{
		for (auto It = ExpectedStamps().CreateIterator(); It; ++It)
		{
			It->Value.RemoveAll([Now](const FExpected& E) { return Now - E.Seconds > FDreamFileWatch::ExpectedStampSeconds; });
			if (It->Value.Num() == 0)
			{
				It.RemoveCurrent();
			}
		}
	}

	FText RefusalText(dream::codec::Refusal Refused)
	{
		switch (Refused)
		{
		case dream::codec::Refusal::Binary:
			return LOCTEXT("Files.ChangedBinary", "it now looks binary (a NUL byte in its first 8 KB)");
		case dream::codec::Refusal::Utf32:
			return LOCTEXT("Files.ChangedUtf32", "it is now UTF-32, which is not supported");
		default:
			return LOCTEXT("Files.ChangedTooLarge", "it is now over 1 MB, which this version does not open");
		}
	}

	/** One path of a batch, as the game thread saw it when the batch started. */
	struct FItem
	{
		FGuid Buffer;
		FString Path;
		TOptional<FDreamBaseVersion> Base;
		bool bForceHash = false;
		bool bMissingOnce = false;
		int32 ReadAttempts = 0;
	};

	/** The worker part of one check (§10.15 Stamp). Any thread but the game thread; no Slate, no UObjects. */
	FDreamFileCheckResult Check(const FItem& Item, const FDateTime& NowUtc)
	{
		FDreamFileCheckResult Out;
		Out.Buffer = Item.Buffer;
		Out.Path = Item.Path;
		Out.QueuedBase = Item.Base;

		const FFileStatData Stat = IFileManager::Get().GetStatData(*Item.Path);
		if (!Stat.bIsValid || Stat.bIsDirectory)
		{
			Out.Outcome = EDreamFileCheck::Deleted;
			return Out;
		}
		Out.bReadOnlyOnDisk = Stat.bIsReadOnly;
		Out.NewBase.Size = Stat.FileSize;
		Out.NewBase.MtimeUtc = Stat.ModificationTime;
		if (Item.Base.IsSet())
		{
			Out.NewBase.Sha256Hex = Item.Base->Sha256Hex;
		}

		// Equal size and mtime mean no change (§10.15), except for a watcher event (always hashed: UE's file times have
		// one-second precision, so a same-size rewrite within the second would look unchanged) and a recent mtime.
		const bool bRecent = (NowUtc - Stat.ModificationTime).GetTotalSeconds() < FDreamFileWatch::RecentMtimeSeconds;
		if (!Item.bForceHash && !bRecent && Item.Base.IsSet() && Stat.FileSize == Item.Base->Size
			&& Stat.ModificationTime == Item.Base->MtimeUtc)
		{
			Out.Outcome = EDreamFileCheck::Unchanged;
			return Out;
		}
		if (Stat.FileSize > FDreamBufferManager::PlainModeMaxBytes)
		{
			Out.Outcome = EDreamFileCheck::Refused;
			Out.Error = RefusalText(dream::codec::Refusal::TooLarge);
			Out.NewBase.Sha256Hex.Reset();
			return Out;
		}

		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Item.Path, FILEREAD_Silent))
		{
			// A program may still hold the file while it writes it; the caller retries (or it was deleted meanwhile).
			Out.Outcome = IFileManager::Get().FileExists(*Item.Path) ? EDreamFileCheck::Unreadable : EDreamFileCheck::Deleted;
			return Out;
		}
		const std::string_view View(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
		Out.NewBase.Size = Bytes.Num();
		Out.NewBase.Sha256Hex = DreamHash::Sha256Hex(View);
		if (Item.Base.IsSet() && Out.NewBase.Size == Item.Base->Size && Out.NewBase.Sha256Hex == Item.Base->Sha256Hex)
		{
			Out.Outcome = Stat.ModificationTime == Item.Base->MtimeUtc ? EDreamFileCheck::Unchanged : EDreamFileCheck::MtimeOnly;
			return Out;
		}
		if (FDreamFileWatch::IsExpectedStamp(Item.Path, Out.NewBase.Size, Out.NewBase.Sha256Hex))
		{
			Out.Outcome = EDreamFileCheck::SelfWrite;
			return Out;
		}

		// An external change: decode on this worker (§4.13), apply on the game thread.
		dream::codec::DecodeResult Decoded = dream::codec::Decode(View, static_cast<uint64>(FDreamBufferManager::PlainModeMaxBytes));
		if (Decoded.Refused != dream::codec::Refusal::None)
		{
			Out.Outcome = EDreamFileCheck::Refused;
			Out.Error = RefusalText(Decoded.Refused);
			return Out;
		}
		Out.Outcome = EDreamFileCheck::Changed;
		Out.Text = DreamText::ToFString(Decoded.Text);
		Out.Format = MoveTemp(Decoded.Format);
		Out.bDecodedReadOnly = Decoded.bReadOnly;
		Out.DecodedReadOnlyReason = FText::FromString(DreamText::Utf8ToFString(Decoded.ReadOnlyReason));
		return Out;
	}
}

struct FDreamFileWatch::FBatch
{
	TArray<DreamFileWatchLocal::FItem> Items;
	TArray<FDreamFileCheckResult> Results;
	std::atomic<bool> bDone { false };
};

FDreamFileWatch::FDreamFileWatch(FDreamBufferManager& InManager, FOnResults InOnResults)
	: Manager(InManager)
	, OnResults(MoveTemp(InOnResults))
{
}

FDreamFileWatch::~FDreamFileWatch()
{
	Shutdown();
}

FString FDreamFileWatch::PathKey(const FString& Path)
{
	// (scratch-p2-m2) A UNC path keeps its leading "//", so its key never equals a drive-root path's.
	const bool bUnc = Path.StartsWith(TEXT("\\\\")) || Path.StartsWith(TEXT("//"));
	FString Key = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Key);
	FPaths::CollapseRelativeDirectories(Key);
	FPaths::RemoveDuplicateSlashes(Key);
	if (bUnc && !Key.StartsWith(TEXT("//")))
	{
		Key = TEXT("/") + Key;
	}
	while (Key.Len() > 3 && Key.EndsWith(TEXT("/")))
	{
		Key.LeftChopInline(1);
	}
#if PLATFORM_WINDOWS
	Key.ToLowerInline();
#endif
	return Key;
}

// ---- Self-write stamps (any thread) ----

void FDreamFileWatch::ExpectStamp(const FString& Path, int64 Size, const FString& Sha256Hex)
{
	using namespace DreamFileWatchLocal;
	const FString Key = PathKey(Path);
	const double Now = FPlatformTime::Seconds();
	FScopeLock Lock(&ExpectedLock());
	PruneExpected(Now);
	ExpectedStamps().FindOrAdd(Key).Add(FExpected{ Size, Sha256Hex, Now });
}

void FDreamFileWatch::ForgetStamp(const FString& Path, const FString& Sha256Hex)
{
	using namespace DreamFileWatchLocal;
	const FString Key = PathKey(Path);
	FScopeLock Lock(&ExpectedLock());
	if (TArray<FExpected>* Stamps = ExpectedStamps().Find(Key))
	{
		Stamps->RemoveAll([&Sha256Hex](const FExpected& E) { return E.Sha256Hex == Sha256Hex; });
		if (Stamps->Num() == 0)
		{
			ExpectedStamps().Remove(Key);
		}
	}
}

bool FDreamFileWatch::IsExpectedStamp(const FString& Path, int64 Size, const FString& Sha256Hex)
{
	using namespace DreamFileWatchLocal;
	const FString Key = PathKey(Path);
	FScopeLock Lock(&ExpectedLock());
	PruneExpected(FPlatformTime::Seconds());
	const TArray<FExpected>* Stamps = ExpectedStamps().Find(Key);
	return Stamps != nullptr && Stamps->ContainsByPredicate([Size, &Sha256Hex](const FExpected& E)
	{
		return E.Size == Size && E.Sha256Hex == Sha256Hex;
	});
}

// ---- Watches (game thread) ----

void FDreamFileWatch::Sync()
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return;
	}
	TSet<FString> Files;
	TMap<FString, FString> WantedDirs;   // key -> directory as the buffer names it
	for (const TSharedRef<FDreamBuffer>& Buffer : Manager.GetOpenBuffers())
	{
		if (Buffer->GetKind() != EDreamBufferKind::File || !Buffer->HasPath())
		{
			continue;
		}
		Files.Add(PathKey(Buffer->GetPath()));
		const FString Dir = FPaths::GetPath(Buffer->GetPath());
		WantedDirs.Add(PathKey(Dir), Dir);
	}
	WatchedFiles = MoveTemp(Files);

	FDirectoryWatcherModule* Module = WantedDirs.Num() > 0
		? &FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(DreamFileWatchLocal::DirectoryWatcherName)
		: FModuleManager::GetModulePtr<FDirectoryWatcherModule>(DreamFileWatchLocal::DirectoryWatcherName);
	IDirectoryWatcher* Watcher = Module != nullptr ? Module->Get() : nullptr;

	for (auto It = Directories.CreateIterator(); It; ++It)
	{
		if (!WantedDirs.Contains(It->Key))
		{
			if (Watcher != nullptr && It->Value.Handle.IsValid())
			{
				Watcher->UnregisterDirectoryChangedCallback_Handle(It->Value.Dir, It->Value.Handle);
			}
			It.RemoveCurrent();
		}
	}
	for (const TPair<FString, FString>& Wanted : WantedDirs)
	{
		if (Directories.Contains(Wanted.Key))
		{
			continue;
		}
		FDirectory& Entry = Directories.Add(Wanted.Key);
		Entry.Dir = Wanted.Value;
		if (Watcher == nullptr)
		{
			continue;   // no watcher on this platform: focus return and run ends still rescan
		}
		if (FDreamWin32FileOps::IsRemotePath(Wanted.Value))
		{
			// (scratch-p3-m2) A share (a UNC path, a mapped network drive): the engine's watcher opens the folder with a
			// synchronous CreateFile on the game thread, which a slow share stalls. Not watched: focus return and run ends
			// recheck its files, as for a file system that does not notify.
			UE_LOG(LogDream, Verbose, TEXT("Dream Python Editor does not watch the network folder %s; it rechecks its files when the editor regains focus and after runs"),
				*Wanted.Value);
			continue;
		}
		const bool bRegistered = Watcher->RegisterDirectoryChangedCallback_Handle(Wanted.Value,
			IDirectoryWatcher::FDirectoryChanged::CreateSP(this, &FDreamFileWatch::HandleDirectoryChanged, Wanted.Key), Entry.Handle,
			IDirectoryWatcher::WatchOptions::IgnoreChangesInSubtree);
		if (!bRegistered)
		{
			UE_LOG(LogDream, Log, TEXT("Dream Python Editor could not watch %s for external changes; it rechecks its files when the editor regains focus"),
				*Wanted.Value);
			Entry.Handle.Reset();
		}
	}
	if (WantedDirs.Num() > 0)
	{
		EnsureBindings();
	}
}

void FDreamFileWatch::EnsureBindings()
{
	if (bBound || !FDreamEditorModule::IsAvailable())
	{
		return;
	}
	bBound = true;
	// §10.15 "During a run": a re-stat of every open file after each run also catches changes the run made itself.
	RunFinishedHandle = FDreamEditorModule::Get().GetRunService().OnRunFinished().AddSP(this, &FDreamFileWatch::HandleRunFinished);
	// §10.15 Watching: focus returning to the editor queues every open file, for file systems that do not notify.
	if (FSlateApplication::IsInitialized())
	{
		ActivationHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddSP(this, &FDreamFileWatch::HandleActivationChanged);
	}
}

void FDreamFileWatch::HandleRunFinished(const FDreamRunRecord& Record)
{
	QueueAll(false);
}

void FDreamFileWatch::HandleActivationChanged(bool bActive)
{
	if (bActive)
	{
		QueueAll(false);
	}
}

void FDreamFileWatch::HandleDirectoryChanged(const TArray<FFileChangeData>& Changes, FString DirKey)
{
	// The watcher's callback runs on the game thread from the editor tick: it only queues (ARCHITECTURE §3).
	for (const FFileChangeData& Change : Changes)
	{
		if (Change.Action == FFileChangeData::FCA_RescanRequired)
		{
			for (const FString& File : WatchedFiles)
			{
				if (PathKey(FPaths::GetPath(File)) == DirKey)
				{
					Queue(File, /*bForceHash*/ true, EventDebounceSeconds);
				}
			}
			continue;
		}
		const FString Key = PathKey(Change.Filename);
		if (WatchedFiles.Contains(Key))
		{
			Queue(Key, /*bForceHash*/ true, EventDebounceSeconds);
		}
	}
}

void FDreamFileWatch::Queue(const FString& Path, bool bForceHash, double DelaySeconds)
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return;
	}
	const FString Key = PathKey(Path);
	const double Due = FPlatformTime::Seconds() + DelaySeconds;
	FQueued& Entry = Queued.FindOrAdd(Key);
	if (Entry.Path.IsEmpty())
	{
		Entry.Path = Path;
		Entry.DueSeconds = Due;
	}
	else
	{
		Entry.DueSeconds = FMath::Max(Entry.DueSeconds, Due);   // a new event restarts the quiet time
	}
	Entry.bForceHash = Entry.bForceHash || bForceHash;
	EnsureTicker();
}

void FDreamFileWatch::QueueAll(bool bForceHash)
{
	for (const TSharedRef<FDreamBuffer>& Buffer : Manager.GetOpenBuffers())
	{
		if (Buffer->GetKind() == EDreamBufferKind::File && Buffer->HasPath())
		{
			Queue(Buffer->GetPath(), bForceHash, 0.0);
		}
	}
}

void FDreamFileWatch::EnsureTicker()
{
	if (Ticker.IsValid() || bShutDown)
	{
		return;
	}
	// §3.7, RT-01: a ticker client that applies async results to buffers defers while a run is active; the queue waits, and
	// the first idle tick after the run starts the checks.
	TWeakPtr<FDreamFileWatch> Weak = AsShared();
	Ticker = FDreamDeferral::AddTicker(TEXT("DreamFileWatch"), EDreamDeferPolicy::DeferWhileExecuting, 0.f, false, [Weak](float) -> bool
	{
		const TSharedPtr<FDreamFileWatch> Pinned = Weak.Pin();
		return Pinned.IsValid() && Pinned->Tick();
	});
}

bool FDreamFileWatch::Tick()
{
	if (bShutDown)
	{
		Ticker.Reset();
		return false;
	}
	if (InFlight.IsValid())
	{
		if (!InFlight->bDone.load(std::memory_order_acquire))
		{
			return true;
		}
		FinishBatch();
	}
	StartBatch(FPlatformTime::Seconds());
	if (!InFlight.IsValid() && Queued.Num() == 0)
	{
		Ticker.Reset();
		return false;
	}
	return true;
}

void FDreamFileWatch::StartBatch(double Now)
{
	using namespace DreamFileWatchLocal;
	TArray<FItem> Items;
	for (auto It = Queued.CreateIterator(); It; ++It)
	{
		if (It->Value.DueSeconds > Now)
		{
			continue;
		}
		const FQueued Entry = It->Value;
		It.RemoveCurrent();
		const TSharedPtr<FDreamBuffer> Buffer = Manager.FindByPath(Entry.Path);
		if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::File)
		{
			continue;   // closed meanwhile
		}
		FItem& Item = Items.AddDefaulted_GetRef();
		Item.Buffer = Buffer->GetId();
		Item.Path = Buffer->GetPath();
		Item.Base = Buffer->GetBase();
		Item.bForceHash = Entry.bForceHash;
		Item.bMissingOnce = Entry.bMissingOnce;
		Item.ReadAttempts = Entry.ReadAttempts;
	}
	if (Items.Num() == 0)
	{
		return;
	}
	ChecksStarted += Items.Num();
	TSharedRef<FBatch, ESPMode::ThreadSafe> Batch = MakeShared<FBatch, ESPMode::ThreadSafe>();
	Batch->Items = MoveTemp(Items);
	InFlight = Batch;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Batch]()
	{
		const FDateTime NowUtc = FDateTime::UtcNow();
		for (const FItem& Item : Batch->Items)
		{
			Batch->Results.Add(Check(Item, NowUtc));
		}
		Batch->bDone.store(true, std::memory_order_release);
	});
}

void FDreamFileWatch::FinishBatch()
{
	const TSharedPtr<FBatch, ESPMode::ThreadSafe> Batch = MoveTemp(InFlight);
	InFlight.Reset();
	TArray<FDreamFileCheckResult> Deliver;
	const double Now = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Batch->Results.Num(); ++Index)
	{
		FDreamFileCheckResult& Result = Batch->Results[Index];
		const DreamFileWatchLocal::FItem& Item = Batch->Items[Index];
		if (Result.Outcome == EDreamFileCheck::Deleted && !Item.bMissingOnce)
		{
			// A save that deletes and renames, or a checkout, leaves a short gap: look once more before saying "deleted".
			FQueued& Again = Queued.FindOrAdd(PathKey(Item.Path));
			Again.Path = Item.Path;
			Again.DueSeconds = FMath::Max(Again.DueSeconds, Now + MissingConfirmSeconds);
			Again.bForceHash = true;
			Again.bMissingOnce = true;
			continue;
		}
		if (Result.Outcome == EDreamFileCheck::Unreadable && Item.ReadAttempts + 1 < MaxReadAttempts)
		{
			FQueued& Again = Queued.FindOrAdd(PathKey(Item.Path));
			Again.Path = Item.Path;
			Again.DueSeconds = FMath::Max(Again.DueSeconds, Now + ReadRetrySeconds);
			Again.bForceHash = true;
			Again.ReadAttempts = Item.ReadAttempts + 1;
			continue;
		}
		if (Result.Outcome == EDreamFileCheck::SelfWrite)
		{
			++SelfWrites;
		}
		Deliver.Add(MoveTemp(Result));
	}
	if (Deliver.Num() > 0 && OnResults)
	{
		OnResults(MoveTemp(Deliver));
	}
}

void FDreamFileWatch::Shutdown()
{
	if (bShutDown)
	{
		return;
	}
	bShutDown = true;
	Queued.Reset();
	InFlight.Reset();   // a worker still running keeps its batch alive and drops the result
	if (Ticker.IsValid())
	{
		FDreamDeferral::RemoveTicker(Ticker);
		Ticker.Reset();
	}
	if (FDirectoryWatcherModule* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(DreamFileWatchLocal::DirectoryWatcherName))
	{
		if (IDirectoryWatcher* Watcher = Module->Get())
		{
			for (const TPair<FString, FDirectory>& Entry : Directories)
			{
				if (Entry.Value.Handle.IsValid())
				{
					Watcher->UnregisterDirectoryChangedCallback_Handle(Entry.Value.Dir, Entry.Value.Handle);
				}
			}
		}
	}
	Directories.Reset();
	WatchedFiles.Reset();
	if (bBound)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetRunService().OnRunFinished().Remove(RunFinishedHandle);
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(ActivationHandle);
		}
		bBound = false;
	}
}

bool FDreamFileWatch::IsIdleForTest() const
{
	return Queued.Num() == 0 && !InFlight.IsValid();
}

#undef LOCTEXT_NAMESPACE
