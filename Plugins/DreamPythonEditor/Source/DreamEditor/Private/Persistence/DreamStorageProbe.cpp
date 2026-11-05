// SPDX-License-Identifier: Apache-2.0
// DreamStorageProbe (TDD §10.1). See DreamStorageProbe.h.
#include "Persistence/DreamStorageProbe.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Persistence/DreamWin32FileOps.h"

FDreamStorageProbe::FDreamStorageProbe()
	: Fence(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false))
{
	Ops = MakeUnique<FDreamWin32FileOps>(Fence);
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

FDreamStorageProbe::~FDreamStorageProbe()
{
	// Only reached when the thread was joined (or never started): an abandoned probe is leaked by its owner.
	if (Thread != nullptr)
	{
		StopAndJoin();
	}
	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

bool FDreamStorageProbe::Post(FWork Work)
{
	if (!Work || bStopRequested.load(std::memory_order_acquire))
	{
		return false;
	}
	{
		FScopeLock Lock(&StartLock);
		if (Thread == nullptr)
		{
			// TPri_Normal (architect ruling, as the journal writer): its jobs are short and the override probe gates a
			// user-visible decision inside a 2 s wait; below normal, a CPU-saturated start (shader or code compiles) could
			// miss the window and fall back (or, under -DreamStorageRoot, run without a journal).
			Thread = FRunnableThread::Create(this, TEXT("DreamStorageProbe"), 0, TPri_Normal);
			if (Thread == nullptr)
			{
				return false;
			}
		}
	}
	Pending.fetch_add(1, std::memory_order_acq_rel);
	Queue.Enqueue(MoveTemp(Work));
	WakeEvent->Trigger();
	return true;
}

bool FDreamStorageProbe::IsFenced() const
{
	return Fence->load(std::memory_order_acquire);
}

uint32 FDreamStorageProbe::Run()
{
	while (!bStopRequested.load(std::memory_order_acquire))
	{
		FWork Work;
		if (Queue.Dequeue(Work))
		{
			if (!IsFenced())
			{
				Work(*Ops);
			}
			Pending.fetch_sub(1, std::memory_order_acq_rel);
			continue;
		}
		WakeEvent->Wait(FTimespan::FromMilliseconds(500.0));
	}
	FWork Dropped;
	while (Queue.Dequeue(Dropped))
	{
		Pending.fetch_sub(1, std::memory_order_acq_rel);
	}
	bFinished.store(true, std::memory_order_release);
	return 0;
}

void FDreamStorageProbe::Stop()
{
	bStopRequested.store(true, std::memory_order_release);
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}

bool FDreamStorageProbe::Abandon(double GraceSeconds)
{
	// The fence first: a job blocked on a dead network share performs no further file operation once it returns.
	Fence->store(true, std::memory_order_release);
	Stop();
	FRunnableThread* Running = nullptr;
	{
		FScopeLock Lock(&StartLock);
		Running = Thread;
	}
	if (Running == nullptr)
	{
		return true;
	}
	const double Deadline = FPlatformTime::Seconds() + GraceSeconds;
	while (!bFinished.load(std::memory_order_acquire) && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	if (!bFinished.load(std::memory_order_acquire))
	{
		return false;   // abandoned (§10.1): never joined, the owner leaks this object
	}
	Running->WaitForCompletion();
	delete Running;
	FScopeLock Lock(&StartLock);
	Thread = nullptr;
	return true;
}

void FDreamStorageProbe::StopAndJoin()
{
	Fence->store(true, std::memory_order_release);
	Stop();
	FRunnableThread* Running = nullptr;
	{
		FScopeLock Lock(&StartLock);
		Running = Thread;
		Thread = nullptr;
	}
	if (Running != nullptr)
	{
		Running->WaitForCompletion();
		delete Running;
	}
}
