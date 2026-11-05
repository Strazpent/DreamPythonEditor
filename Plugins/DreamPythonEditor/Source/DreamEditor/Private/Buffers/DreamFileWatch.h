// SPDX-License-Identifier: Apache-2.0
// FDreamFileWatch (TDD §10.15 M1, BUF-04): notices when an open file changes outside Dream.
// - Watching: one IDirectoryWatcher registration per directory of an open file buffer, with IgnoreChangesInSubtree,
//   counted by the open files in it (Sync). The DirectoryWatcher ticks from the editor tick on the game thread; its
//   callbacks only queue paths (ARCHITECTURE §3). FCA_RescanRequired queues every open file of that directory. Focus
//   returning to the editor and the end of every run queue every open file (a re-stat also catches changes a run made).
// - Checking: a deferred ticker (FDreamDeferral, never while a run is active, RT-01) hands the due paths with each buffer's
//   base version to a worker task, which stats them and, when needed, reads, hashes and decodes them (§4.13); it never
//   touches Slate or UObjects. Equal size and mtime mean no change for a rescan; a watcher event always hashes, because
//   UE's file times have one-second precision (B-note). A hash equal to the base only updates the mtime; a hash equal to
//   an expected self-write (ExpectStamp, §10.14 step 6) is Dream's own save.
// - Results come back through another deferred tick and are handed to the owner (FDreamBufferManager), which reloads a
//   clean buffer as one undo group, shows the dirty or deleted bar and updates the lock badge.
// Game thread API except ExpectStamp / ForgetStamp (any thread). Owner: buffers.
#pragma once

#include "CoreMinimal.h"
#include "Buffers/DreamBuffer.h"
#include "Containers/Ticker.h"

class FDreamBufferManager;
struct FDreamRunRecord;

/** What a check found for one open file (§10.15 stamp rules). */
enum class EDreamFileCheck : uint8
{
	Unchanged,          // size and mtime equal (a rescan), or the bytes hash to the base
	MtimeOnly,          // the bytes hash to the base; only the mtime moved
	SelfWrite,          // the bytes are an expected stamp of Dream's own save (§10.14 step 6)
	Changed,            // an external change: Text, Format and NewBase hold the new disk version
	Refused,            // an external change Dream cannot load in plain mode (over 1 MB, binary, UTF-32): Error says why
	Deleted,            // the file is gone (confirmed by a second stat MissingConfirmSeconds later)
	Unreadable,         // the read failed (a sharing violation while another program writes): retried
};

struct FDreamFileCheckResult
{
	FGuid Buffer;
	FString Path;
	/** The buffer's base when the check was queued: a save that landed meanwhile makes the result stale. */
	TOptional<FDreamBaseVersion> QueuedBase;
	EDreamFileCheck Outcome = EDreamFileCheck::Unchanged;
	bool bReadOnlyOnDisk = false;
	FDreamBaseVersion NewBase;
	FString Text;
	dream::codec::FileFormat Format;
	bool bDecodedReadOnly = false;
	FText DecodedReadOnlyReason;
	FText Error;
};

class DREAMEDITOR_API FDreamFileWatch : public TSharedFromThis<FDreamFileWatch>
{
public:
	/** Quiet time after a watcher event before the path is checked: a program that writes in pieces finishes first. */
	static constexpr double EventDebounceSeconds = 0.15;
	/** A missing file is checked once more after this before it counts as deleted (an editor's delete-then-rename save). */
	static constexpr double MissingConfirmSeconds = 0.5;
	/** A failed read is retried this often, this far apart. */
	static constexpr int32 MaxReadAttempts = 4;
	static constexpr double ReadRetrySeconds = 0.25;
	/** A rescan trusts equal size and mtime only for files whose mtime is older than this (one-second mtime precision). */
	static constexpr double RecentMtimeSeconds = 3.0;
	/** An expected self-write stamp is forgotten after this long. */
	static constexpr double ExpectedStampSeconds = 60.0;

	using FOnResults = TFunction<void(TArray<FDreamFileCheckResult>&& /*Results*/)>;

	FDreamFileWatch(FDreamBufferManager& InManager, FOnResults InOnResults);
	~FDreamFileWatch();

	/** Registers and unregisters directory watches to match the manager's open file buffers. Game thread. */
	void Sync();
	/** Queues one open file for a check after DelaySeconds; bForceHash skips the size-and-mtime shortcut. Game thread. */
	void Queue(const FString& Path, bool bForceHash, double DelaySeconds = 0.0);
	/** Every open file buffer (focus returned, a run ended, a test). Game thread. */
	void QueueAll(bool bForceHash = false);
	/** Unregisters every watch and binding; queued and in-flight checks are dropped. Game thread. */
	void Shutdown();

	/** §10.14 step 6: the stamp a save is about to write to Path; any thread (the saver's worker). */
	static void ExpectStamp(const FString& Path, int64 Size, const FString& Sha256Hex);
	/** The save failed: its stamp is not coming. Any thread. */
	static void ForgetStamp(const FString& Path, const FString& Sha256Hex);
	/** Whether Path is expected to hold exactly these bytes (an unexpired ExpectStamp). Any thread. */
	static bool IsExpectedStamp(const FString& Path, int64 Size, const FString& Sha256Hex);

	/** The key every path comparison of the watch uses: normalized, case-folded on Windows. */
	static FString PathKey(const FString& Path);

	// ---- Test hooks (Dream.Buffers.*) ----

	/** Nothing is queued and no check is in flight. */
	bool IsIdleForTest() const;
	int32 GetWatchedDirectoryCountForTest() const { return Directories.Num(); }
	/** Checks handed to the worker so far, and self-writes recognised. */
	int32 GetChecksStartedForTest() const { return ChecksStarted; }
	int32 GetSelfWritesForTest() const { return SelfWrites; }

private:
	struct FQueued
	{
		FString Path;
		double DueSeconds = 0.0;
		bool bForceHash = false;
		bool bMissingOnce = false;
		int32 ReadAttempts = 0;
	};
	struct FDirectory
	{
		FString Dir;
		FDelegateHandle Handle;
	};
	struct FBatch;

	void HandleDirectoryChanged(const TArray<struct FFileChangeData>& Changes, FString DirKey);
	void HandleActivationChanged(bool bActive);
	void HandleRunFinished(const FDreamRunRecord& Record);
	void EnsureBindings();
	void EnsureTicker();
	bool Tick();
	void StartBatch(double Now);
	void FinishBatch();

	FDreamBufferManager& Manager;
	FOnResults OnResults;
	TMap<FString, FDirectory> Directories;          // by PathKey of the directory
	TSet<FString> WatchedFiles;                     // PathKey of every open file buffer
	TMap<FString, FQueued> Queued;                  // by PathKey
	TSharedPtr<FBatch, ESPMode::ThreadSafe> InFlight;
	FTSTicker::FDelegateHandle Ticker;
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle ActivationHandle;
	bool bBound = false;
	bool bShutDown = false;
	int32 ChecksStarted = 0;
	int32 SelfWrites = 0;
};
