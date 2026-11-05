// SPDX-License-Identifier: Apache-2.0
// The DreamJournalWriter thread (TDD §10.2.1): one FRunnable that reads the namespace at open (§10.2.4), starts this
// session's generation after the reconcile (§10.2.3), turns game-thread snapshots into records through
// dream::journal::BatchPlanner, commits and flushes, marks tickets durable, and does the background work between commits:
// the checkpoint of a rotation in 4 MiB slices with its catch-up (§10.2.3 steps 1-2) and the local-history slices (§10.4).
// It touches no Python, Slate or UObject; text arrives as immutable dream::TextSnapshots. Slice-internal (FDreamJournal owns it).
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "Persistence/DreamHistoryTypes.h"
#include "Persistence/DreamJournal.h"
#include "dream/JournalFormat.h"
#include "dream/TextBuffer.h"

#include <atomic>

class FEvent;
class FDreamWin32FileOps;

namespace DreamJournal
{
	using FBufferPart = dream::journal::BufferPart;

	enum class ERequestKind : uint8
	{
		Scan,
		StartGeneration,        // the reconcile: the checkpoint that opens this session's generation
		Commit,
		Clean,
	};

	struct FRequest
	{
		ERequestKind Kind = ERequestKind::Commit;
		/** The records to commit (Commit, Clean) or the generation's starting state (StartGeneration: its buffers and
		 *  session). Commit.Ticket is the request's ticket. */
		dream::journal::CommitRequest Commit;
		/** StartGeneration: pending recoveries (RECOVERY_PENDING + SNAP with "pending", §10.7.1). */
		std::vector<std::pair<dream::journal::PendingInfo, std::string>> Pending;
		/** History entries this request adds once it is durable (§10.4: run, closed, save, recovered). A request that fails
		 *  drops them: a run whose snapshot failed never started. */
		TArray<FDreamHistoryNote> History;
	};
}

/** How a writer runs (§10.5, §10.6). */
struct FDreamJournalWriterOptions
{
	/** Where this session's history entries go: <root>/history, or a secondary's <sec>/pending-history (§10.5). */
	FString HistoryDir;
	/** The primary: its scan reads the root's other namespaces (orphans, §10.5) and moves to compat-* over a newer main. */
	bool bPrimary = true;
};

/** The namespace reads shared by the writer's scan and FDreamInstances' workers (DreamJournalWriter.cpp). Any thread. */
namespace DreamJournalScan
{
	/** The §10.5 lock content {schema, pid, process_start, host, plugin_version, namespace, utc} of this process. */
	DREAMEDITOR_API std::string MakeLockContent(const FString& Namespace);
	/** The names and sizes of a namespace's wal.*.sjl files (§10.1: a namespace written since it was read is never deleted). */
	DREAMEDITOR_API void RecordWalFiles(dream::journal::FileOps& Ops, const FString& Dir, TMap<FString, int64>& OutFiles);
	/**
	 * One namespace read (never modified: ReadOps should be an FDreamReadOnlyFileOps) into what Review offers: a non-empty
	 * scratch buffer, a file buffer whose text differs from the disk or whose file is gone, and every unresolved
	 * RECOVERY_PENDING; bAll (the live primary's tabs, §10.5 View its tabs) offers every open buffer. DiskOps reads the files
	 * the buffers name. Fills everything but Key, Source, Detail and NamespaceLock.
	 */
	DREAMEDITOR_API void ReadOrphan(dream::journal::FileOps& ReadOps, dream::journal::FileOps& DiskOps, const FString& Root,
		const FString& Namespace, bool bAll, FDreamOrphanNamespace& Out);
}

class DREAMEDITOR_API FDreamJournalWriter final : public FRunnable
{
public:
	FDreamJournalWriter(const FDreamStorageRoot& InRoot, const FString& InNamespace, int64 InSessionStartUtcMs,
		TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> InFence, FDreamHistorySink InHistorySink,
		const FDreamJournalWriterOptions& InOptions = FDreamJournalWriterOptions());
	virtual ~FDreamJournalWriter() override;

	/** Game thread: queue a request (SPSC: only the game thread produces). */
	void Push(TUniquePtr<DreamJournal::FRequest> Request);
	/** Any thread: queue background work (the history lane, §10.2.1). */
	void PostIo(FDreamIoSlice Work);

	// ---- Any thread (atomics, or a short lock never held across I/O) ----

	bool IsDurable(uint64 Ticket) const;
	bool HasFailed(uint64 Ticket, FString& OutReason) const;
	bool IsCleanAcknowledged() const { return bCleanAcked.load(std::memory_order_acquire); }
	/** Test hook (Dream.Persistence.Journal): every commit fails as a full disk would, until it is turned off again. */
	void SetFailCommitsForTest(bool bFail) { bFailCommitsForTest.store(bFail, std::memory_order_release); }
	/** Test hook (P2-per-02): the requests waiting in the retry queue, as of the writer's last change to it. A ticket that
	 *  failed behind an earlier failure is marked only after this is updated. */
	int32 GetRetryQueueLengthForTest() const { return RetryQueueLength.load(std::memory_order_acquire); }
	/** Test hook: generations this writer rotated to, and the rotations it finished. */
	uint64 GetGenerationForTest() const { return CurrentGeneration.load(std::memory_order_acquire); }
	/** Test hook: the next rotation starts at the next commit, whatever the size and age rules say. */
	void ForceRotationForTest() { bForceRotationForTest.store(true, std::memory_order_release); }
	EDreamJournalHealth GetHealth() const { return static_cast<EDreamJournalHealth>(Health.load(std::memory_order_acquire)); }
	/** FDateTime ticks (UTC) of the last durable commit; 0 before the first. */
	int64 GetLastDurableUtcTicks() const { return LastDurableUtcTicks.load(std::memory_order_acquire); }
	/** Game thread: the recovery scan result, once (§10.3 step H). */
	bool TakeScanResult(FDreamRecoveredSession& Out);
	/** The namespace written: the one given, or compat-<format> once the scan moved there (§10.6). Any thread. */
	FString GetNamespace() const;
	/** The compat namespace's ns.lock the scan took (§10.6), handed to the caller once; 0 when there is none. */
	uint64 TakeCompatLock() { return CompatLock.exchange(0, std::memory_order_acq_rel); }
	/** Stops after the queued requests. DrainUntilSeconds (FPlatformTime): queued history work runs until then (review r1:
	 *  a just-closed scratch tab's entry); 0 drops it at once (a simulated crash, an abandoned writer). */
	void RequestStop(double DrainUntilSeconds = 0.0);

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override { RequestStop(); }

private:
	void Process(DreamJournal::FRequest& Request);
	void ProcessScan();
	/** §10.5: the root's other namespaces whose ns.lock is free, each read and its lock kept (a primary's scan only). */
	void ScanOrphans(TArray<FDreamOrphanNamespace>& Out);
	bool ProcessStartGeneration(DreamJournal::FRequest& Request);
	/** Commit or Clean: plan, write, flush, then apply the plan. On failure the request waits in the planner's retry queue.
	 *  True when it is durable. */
	bool CommitRequest(DreamJournal::FRequest& Request);
	bool CommitBatch(const std::vector<dream::journal::Record>& Batch);
	void AfterDurableCommit();
	void MarkTickets(const TArray<uint64>& Tickets, bool bOk, const FString& Reason);
	void RetryIfDue();
	/** One unit of background work: a rotation step, else one history slice. False when there was none. */
	bool DoBackgroundUnit();
	/** A history slice's FileOps retry wait (review r1): the due commits are served for that long instead of a sleep. */
	void ServeCommitsFor(float Seconds);
	FString LastErrorText() const;

	FDreamStorageRoot Root;
	FString Namespace;                            // guarded by SharedLock once the thread runs (the scan may move it, §10.6)
	FString Dir;
	FDreamJournalWriterOptions Options;
	int64 SessionStartUtcMs = 0;
	uint64 SessionId = 0;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Fence;
	TUniquePtr<FDreamWin32FileOps> Ops;
	TUniquePtr<dream::journal::Writer> Writer;
	dream::journal::BatchPlanner Planner;
	FDreamHistorySink HistorySink;

	TQueue<TUniquePtr<DreamJournal::FRequest>, EQueueMode::Spsc> Queue;
	TQueue<FDreamIoSlice, EQueueMode::Mpsc> IoQueue;
	FEvent* WakeEvent = nullptr;
	std::atomic<bool> bStopRequested { false };

	// Writer-thread state
	bool bGenerationStarted = false;
	TArray<TUniquePtr<DreamJournal::FRequest>> HeldUntilReconcile;
	TArray<FDreamIoSlice> Background;              // the front item is the one in progress
	bool bNeedsRetry = false;
	double NextRetrySeconds = 0.0;
	double NextRotationSeconds = 0.0;             // a failed rotation waits this long before the next attempt
	uint64 ScanGeneration = 0;
	/** The generation the scan applied in the namespace this session writes (persist-M2): nothing starts above an unused
	 *  newer one until it is set aside. */
	uint64 ScanUsedGeneration = 0;
	std::atomic<double> DrainUntil { 0.0 };
	bool bExitDrain = false;

	// Shared state
	std::atomic<uint8> Health { static_cast<uint8>(EDreamJournalHealth::Ok) };
	std::atomic<int64> LastDurableUtcTicks { 0 };
	std::atomic<bool> bCleanAcked { false };
	std::atomic<bool> bFailCommitsForTest { false };
	std::atomic<bool> bForceRotationForTest { false };
	std::atomic<int32> RetryQueueLength { 0 };
	std::atomic<uint64> CurrentGeneration { 0 };
	std::atomic<uint64> CompatLock { 0 };
	mutable FCriticalSection SharedLock;          // the ticket sets and ScanResult only; never held across I/O
	/** Pushed and not yet written (PER-09): an explicit set, not a high-water mark, so a request held until the reconcile
	 *  never reads durable because a later ticket was. */
	TSet<uint64> PendingTickets;
	uint64 IssuedTicket = 0;
	TMap<uint64, FString> FailedTickets;
	TOptional<FDreamRecoveredSession> ScanResult;
};
