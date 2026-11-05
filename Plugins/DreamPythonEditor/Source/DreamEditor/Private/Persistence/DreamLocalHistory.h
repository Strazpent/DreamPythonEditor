// SPDX-License-Identifier: Apache-2.0
// FDreamLocalHistory: the local per-project timeline of buffer texts (TDD §10.4 M1, D17, PER-04). Content-addressed blobs
// <root>/history/objects/<aa>/<sha256>.z (zlib in 1 MiB chunks behind the SUMIBLOB header, temp file then rename,
// deduplicated), and the index <root>/history/index.<g>.shi in the journal's framing and generations (dream/HistoryFormat.h).
// Entries come from saves, runs ("Before run #N", merged when the text did not change), reloads, closed scratch buffers,
// recovered and discarded texts and restores. Every byte of I/O runs on the journal writer thread, after the commit that
// caused it and in slices that yield to commits (§10.2.1); only the primary instance writes <root>/history (§10.5): a
// secondary's journal writer stages its entries (index and blobs) in <sec>/pending-history, which the primary merges with
// MergeFrom before it deletes that namespace (FDreamInstances). Retention (30 days, 500 MB, each buffer's
// newest entry and every entry under 24 h kept) runs 60 s after the journal opens and hourly. The timeline and diff UI
// is M2. Game-thread API; results come back on the game thread through FDreamDeferral (never inside a run, RT-01).
// Body: DreamLocalHistory.cpp (persist).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Persistence/DreamHistoryTypes.h"

class FDreamJournal;

class DREAMEDITOR_API FDreamLocalHistory
{
public:
	static constexpr double RetentionFirstDelaySeconds = 60.0;     // proposed, §10.4
	static constexpr double RetentionIntervalSeconds = 3600.0;     // proposed, §10.4

	/** Installs the journal's history sink (run, save, closed and recovered entries ride on its commits). */
	explicit FDreamLocalHistory(FDreamJournal& InJournal);
	~FDreamLocalHistory();
	FDreamLocalHistory(const FDreamLocalHistory&) = delete;
	FDreamLocalHistory& operator=(const FDreamLocalHistory&) = delete;

	/**
	 * Queues an entry: the text is hashed, stored as a blob unless one with its hash exists, and indexed. A "run" entry
	 * whose buffer's newest entry is a run entry with the same text joins its runs list; a "recovered" entry whose buffer's
	 * newest entry is the same recovered text is not added again. OnDone(true) on the game thread once the index record is
	 * durable; false when nothing could be written (no journal open, a disk error). A secondary's entries are staged (§10.5).
	 */
	void AddEntry(const FGuid& Buffer, EDreamHistoryKind Kind, const dream::TextSnapshot& Text, const FString& Path,
		const FString& DisplayName, TOptional<int32> RunNumber = TOptional<int32>(), TFunction<void(bool)> OnDone = nullptr);
	/** The entries of one buffer (an invalid FGuid: all of them), newest first. */
	void Query(const FGuid& Buffer, TFunction<void(TArray<FDreamLocalHistoryEntry>&&)> OnGameThread) const;
	/** The text of an entry; unset when its blob is missing or damaged. */
	void LoadText(const FDreamLocalHistoryEntry& Entry, TFunction<void(TOptional<FString>&&)> OnGameThread) const;
	/**
	 * Clear local history (§10.4): a CLEAR record, every blob deleted, the index rotated; journals are untouched. The UI that
	 * calls it (History panel menu, palette, with its inline confirm) is M2. OnGameThread(true) when it is done.
	 */
	void Clear(TFunction<void(bool)> OnGameThread);
	/**
	 * §10.5 Merge and §10.1 Import: another history folder (a secondary's <sec>/pending-history, a moved project's
	 * <oldroot>/history) joins this one: blobs deduplicated by SHA-256, entries by id. Nothing in the source is changed.
	 * OnGameThread(ok, entries merged). The primary's history only; Query and LoadText of a secondary see its staged entries.
	 */
	void MergeFrom(const FString& SourceHistoryDir, TFunction<void(bool, int32)> OnGameThread);
	/** §10.4 steps 1-4 now (the timer runs them 60 s after the journal opens and hourly): tombstones, sweep, rotation. */
	void EnforceRetention(TFunction<void(int32 /*Tombstoned*/, int32 /*Swept*/)> OnGameThread = nullptr);

	/**
	 * persist-r2-M2 (§10.11): true while an index file of this history cannot be read (another program holds it). History is
	 * then paused, not read only: its entries wait in the queue and are written once the index reads again (re-read every
	 * second); OnPausedChanged fires on the game thread when it starts and ends (the tab shows a notice meanwhile).
	 */
	bool IsPaused() const;
	FSimpleMulticastDelegate& OnPausedChanged() { return PausedChanged; }

	/** The module calls it when the journal opened, in every role: the result and pause ticker starts; the primary's call
	 *  also starts the retention timer (§10.5, bRunRetention). Game thread. */
	void OnJournalOpened(bool bRunRetention = true);
	/** ShutdownModule: the ticker goes; results that arrive later are dropped. */
	void Shutdown();

	// ---- Test hooks (Dream.Persistence.History, on a private root) ----

	/** Limits and a clock offset for the next retention runs (T-PER-010 at tier 4). */
	void SetRetentionForTest(int64 MaxAgeMs, int64 MaxBytes, int64 ClockOffsetMs);

	struct FStore;

private:
	void EnsureTicker() const;
	bool Post(FDreamIoSlice&& Work) const;
	bool Tick();

	FDreamJournal& Journal;
	TSharedPtr<FStore, ESPMode::ThreadSafe> Store;
	mutable FTSTicker::FDelegateHandle Ticker;
	double NextRetentionSeconds = 0.0;
	bool bRetentionScheduled = false;
	bool bPausedSeen = false;
	double NextPausedProbeSeconds = 0.0;
	FSimpleMulticastDelegate PausedChanged;
};
