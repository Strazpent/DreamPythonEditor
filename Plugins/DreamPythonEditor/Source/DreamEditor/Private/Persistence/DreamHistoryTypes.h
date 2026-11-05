// SPDX-License-Identifier: Apache-2.0
// Types shared by the journal writer and local history (TDD §10.2.1 background work, §10.4): the history entry kinds and
// entries the game thread sees, the note a journal commit carries into history, and the writer thread's background slice.
// Slice-internal (persist).
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "dream/TextBuffer.h"

#include <atomic>

namespace dream::journal
{
	class FileOps;
}

/** §10.4 entry kinds. */
enum class EDreamHistoryKind : uint8
{
	Save,               // every save (PER-04: ten saves, ten entries)
	Run,                // "Before run #14" after the pre-run snapshot is durable (§10.2.5 step 3); identical runs merge
	Reload,             // the text an external reload replaced (BUF-04)
	Closed,             // a non-empty scratch buffer closed (§10.2.5)
	Recovered,          // a recovered text left pending (§10.7.1)
	Discarded,          // a recovered text the user discarded (§10.7.3, U30)
	Restore,            // the text a restore replaced (recovery Restore, restore from history)
};

/** One history entry as the game thread sees it (§10.4 index entry). */
struct FDreamLocalHistoryEntry
{
	FString Id;                         // 32 hex
	FGuid Buffer;
	FString Path;                       // empty for scratch
	FString DisplayName;
	EDreamHistoryKind Kind = EDreamHistoryKind::Save;
	TArray<int32> Runs;                 // run numbers of a merged "run" entry
	FDateTime Utc;
	FString BlobSha256;
	int64 SizeBytes = 0;                // UTF-8 bytes of the text
	int32 Lines = 0;
};

/**
 * The time of a new history note, strictly increasing over the process (UTC ms, at least one above the last one handed out).
 * Notes reach the index out of order (a run's waits for its pre-run commit, an AddEntry is queued at once), and run-entry
 * merging decides "consecutive" by time (dream::history::PlanRunEntry), so two notes of one frame must not share a time.
 * Game thread (atomic anyway). Body: DreamLocalHistory.cpp.
 */
DREAMEDITOR_API int64 DreamHistoryNoteUtcMs();

/** What a journal commit adds to history once it is durable (§10.4 "after the commit that caused it"). */
struct FDreamHistoryNote
{
	FString BufferUuid;                 // 32 hex
	EDreamHistoryKind Kind = EDreamHistoryKind::Save;
	dream::TextSnapshot Text;
	FString Path;
	FString DisplayName;
	int32 RunNumber = 0;
	int64 UtcMs = 0;
};

/**
 * One unit of the journal writer's background work (§10.2.1): it runs on DreamJournalWriter after every due commit with the
 * writer's fenced FileOps and the session's fence, and returns true while it has more slices (it runs again after the next
 * commit drain). Each call should fit in about 50 ms. Work still queued when the writer stops runs until the shutdown budget
 * of §10.2.5 (2 s from OnPreExit) is spent, with bExiting set; what is left then is dropped.
 */
struct FDreamIoContext
{
	dream::journal::FileOps& Ops;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Fence;
	uint64 SessionId = 0;               // changes with every journal open (the writer thread and its FileOps)
	FString HistoryDir;                 // <root>/history of the journal's storage root
	/** The writer is stopping (review r1): the rest of the shutdown budget goes to the queued AddEntry work (a closed scratch
	 *  tab's entry is its only copy); retention, Clear and other long work stop at their next slice. */
	bool bExiting = false;
};
using FDreamIoSlice = TFunction<bool(FDreamIoContext& Context)>;

/** Writer thread: turns a durable commit's note into background history work (FDreamLocalHistory installs it). */
using FDreamHistorySink = TFunction<FDreamIoSlice(FDreamHistoryNote&& Note)>;
