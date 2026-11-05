// SPDX-License-Identifier: Apache-2.0
#include "Fixtures/DreamTestLatch.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace DreamTest
{
	namespace
	{
		struct FLatchState
		{
			bool bReleased = false;
			bool bReached = false;
			int32 Waiters = 0;
		};

		struct FMarkState
		{
			double Seconds = 0.0;
			int32 Count = 0;
		};

		struct FStore
		{
			FCriticalSection Lock;
			TMap<FString, FLatchState> Latches;
			TMap<FString, FMarkState> Marks;
		};

		FStore& Store()
		{
			static FStore Instance;
			return Instance;
		}
	}

	void FDreamTestLatch::Release(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		S.Latches.FindOrAdd(Name).bReleased = true;
	}

	void FDreamTestLatch::Reset(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		FLatchState& Latch = S.Latches.FindOrAdd(Name);
		Latch.bReleased = false;
		Latch.bReached = false;
	}

	bool FDreamTestLatch::IsHeld(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		const FLatchState* Latch = S.Latches.Find(Name);
		return Latch == nullptr || !Latch->bReleased;
	}

	bool FDreamTestLatch::WasReached(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		const FLatchState* Latch = S.Latches.Find(Name);
		return Latch != nullptr && Latch->bReached;
	}

	bool FDreamTestLatch::IsWaiting(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		const FLatchState* Latch = S.Latches.Find(Name);
		return Latch != nullptr && Latch->Waiters > 0;
	}

	void FDreamTestLatch::BeginWait(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		FLatchState& Latch = S.Latches.FindOrAdd(Name);
		Latch.bReached = true;
		++Latch.Waiters;
	}

	void FDreamTestLatch::EndWait(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		if (FLatchState* Latch = S.Latches.Find(Name))
		{
			Latch->Waiters = FMath::Max(0, Latch->Waiters - 1);
		}
	}

	double FDreamTestLatch::Mark(const FString& Name)
	{
		const double Now = FPlatformTime::Seconds();
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		FMarkState& Mark = S.Marks.FindOrAdd(Name);
		Mark.Seconds = Now;
		++Mark.Count;
		return Now;
	}

	double FDreamTestLatch::GetMark(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		const FMarkState* Mark = S.Marks.Find(Name);
		return Mark ? Mark->Seconds : 0.0;
	}

	int32 FDreamTestLatch::GetMarkCount(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		const FMarkState* Mark = S.Marks.Find(Name);
		return Mark ? Mark->Count : 0;
	}

	void FDreamTestLatch::ClearMark(const FString& Name)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		S.Marks.Remove(Name);
	}

	void FDreamTestLatch::ForgetPrefix(const FString& Prefix)
	{
		FStore& S = Store();
		FScopeLock Lock(&S.Lock);
		for (auto It = S.Latches.CreateIterator(); It; ++It)
		{
			if (It.Key().StartsWith(Prefix) && It.Value().Waiters == 0)
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = S.Marks.CreateIterator(); It; ++It)
		{
			if (It.Key().StartsWith(Prefix))
			{
				It.RemoveCurrent();
			}
		}
	}
}
