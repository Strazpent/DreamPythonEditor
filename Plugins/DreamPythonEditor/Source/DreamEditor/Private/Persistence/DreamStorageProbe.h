// SPDX-License-Identifier: Apache-2.0
// DreamStorageProbe (TDD §10.1): the one Dream-owned FRunnable that does the storage work a game thread must never wait on
// without a bound: the storage.root_override probe (folders, owner.json, a probe file; the tab spawn waits at most 2 s),
// projects.json under its lock, the cross-root orphan scan and its deletes, the moved-project import copy, the secondary's
// 30 s poll of the primary's lock and the read-only view of the primary's namespace. Never UE::Tasks or GThreadPool,
// whose workers engine exit joins. At OnPreExit the thread is abandoned like the journal writer (§10.2.5): its FileOps
// fence is set, nothing joins it, and its code and state stay alive until the process exits.
// Jobs run in order; each gets the probe's fenced FileOps and must check IsFenced() before any static file helper.
// Thread: Post from any thread; jobs on DreamStorageProbe. Body: DreamStorageProbe.cpp (instances).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"

#include <atomic>

class FEvent;
class FRunnableThread;
class FDreamWin32FileOps;

class DREAMEDITOR_API FDreamStorageProbe final : public FRunnable
{
public:
	using FWork = TFunction<void(FDreamWin32FileOps& Ops)>;

	FDreamStorageProbe();
	virtual ~FDreamStorageProbe() override;

	/** Queues a job; the thread starts at the first call (never in StartupModule, B1). False once abandoned or stopped. */
	bool Post(FWork Work);
	/** True once Abandon or StopAndJoin ran: jobs should stop touching files. Any thread. */
	bool IsFenced() const;
	/**
	 * OnPreExit: sets the fence and asks the thread to stop. When it finishes within GraceSeconds it is joined and true is
	 * returned (the object may be deleted); otherwise the caller must leak the object, as the journal writer's is.
	 */
	bool Abandon(double GraceSeconds = 0.1);
	/** Tests and a runtime unload: the fence, then a join (only when no job can block for long). */
	void StopAndJoin();
	/** Jobs queued or running (tests wait for zero). */
	int32 GetPendingForTest() const { return Pending.load(std::memory_order_acquire); }

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	TQueue<FWork, EQueueMode::Mpsc> Queue;
	FEvent* WakeEvent = nullptr;
	FRunnableThread* Thread = nullptr;
	TUniquePtr<FDreamWin32FileOps> Ops;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Fence;
	std::atomic<bool> bStopRequested { false };
	std::atomic<bool> bFinished { false };
	std::atomic<int32> Pending { 0 };
	FCriticalSection StartLock;
};
