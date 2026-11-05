// SPDX-License-Identifier: Apache-2.0
// The ticker-client deferral gate (TDD §3.7, RT-01, INV-6). Game thread only. User code can tick the core ticker mid-run
// and the pump ticks Slate mid-run, so every Dream client that can call Python, touch run state, mutate buffers or apply
// async results runs only when no run is active and no pump is on the stack.
#include "Run/DreamDeferral.h"

#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamPython.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"

namespace DreamDeferralPrivate
{
	TMap<FName, uint64>& DeferredCounts()
	{
		static TMap<FName, uint64> Counts;
		return Counts;
	}

	/** Python-needing helper time spent in the current frame, summed over clients (§3.7: at most 5 ms per idle frame). */
	uint64 GBudgetFrame = 0;
	double GBudgetSpentSeconds = 0.0;

	double& SpentThisFrame()
	{
		if (GBudgetFrame != GFrameCounter)
		{
			GBudgetFrame = GFrameCounter;
			GBudgetSpentSeconds = 0.0;
		}
		return GBudgetSpentSeconds;
	}

	void CountDeferral(FName Client)
	{
		++DeferredCounts().FindOrAdd(Client);
	}

	/** Every handle AddTicker returned that may still be registered, for RemoveAllTickers; expired ones are pruned. */
	TArray<FTSTicker::FDelegateHandle>& LiveHandles()
	{
		static TArray<FTSTicker::FDelegateHandle> Handles;
		return Handles;
	}
}

FTSTicker::FDelegateHandle FDreamDeferral::AddTicker(FName Client, EDreamDeferPolicy Policy, float DelaySeconds, bool bNeedsPython,
	TFunction<bool(float)> Body)
{
	check(IsInGameThread());
	FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Client, Policy, bNeedsPython, Body = MoveTemp(Body)](float DeltaSeconds) -> bool
		{
			if (!CanRunNow(Client, Policy, bNeedsPython))
			{
				DreamDeferralPrivate::CountDeferral(Client);
				return true;    // re-armed silently; the body runs at a later idle tick
			}
			if (!bNeedsPython)
			{
				return Body(DeltaSeconds);
			}
			double& Spent = DreamDeferralPrivate::SpentThisFrame();
			if (Spent >= IdlePythonBudgetSeconds)
			{
				DreamDeferralPrivate::CountDeferral(Client);
				return true;    // this frame's helper budget is used up
			}
			const double Start = FPlatformTime::Seconds();
			const bool bKeep = Body(DeltaSeconds);
			DreamDeferralPrivate::SpentThisFrame() += FPlatformTime::Seconds() - Start;
			return bKeep;
		}), DelaySeconds);

	TArray<FTSTicker::FDelegateHandle>& Live = DreamDeferralPrivate::LiveHandles();
	Live.RemoveAllSwap([](const FTSTicker::FDelegateHandle& Existing) { return !Existing.IsValid(); });
	Live.Add(Handle);
	return Handle;
}

void FDreamDeferral::RemoveTicker(FTSTicker::FDelegateHandle Handle)
{
	FTSTicker::RemoveTicker(Handle);
	// Once ShutdownModule has run RemoveAllTickers the list is empty and stays untouched: a widget destroyed later, even
	// during static destruction, must not reach it.
	if (Handle.IsValid() && FDreamEditorModule::IsAvailable())
	{
		// The core ticker drops a removed element at its next tick, so the handle stays valid until then: match it.
		DreamDeferralPrivate::LiveHandles().RemoveAllSwap([&Handle](const FTSTicker::FDelegateHandle& Existing)
		{
			return !Existing.IsValid() || Existing == Handle;
		});
	}
}

void FDreamDeferral::RemoveAllTickers()
{
	check(IsInGameThread());
	TArray<FTSTicker::FDelegateHandle> Live = MoveTemp(DreamDeferralPrivate::LiveHandles());
	DreamDeferralPrivate::LiveHandles().Reset();
	for (const FTSTicker::FDelegateHandle& Handle : Live)
	{
		FTSTicker::RemoveTicker(Handle);
	}
}

bool FDreamDeferral::CanRunNow(FName Client, EDreamDeferPolicy Policy, bool bNeedsPython)
{
	// Both policies use the same condition: a Defer client is skipped, a Refuse client's caller refuses (§3.7 table).
	(void)Client;
	(void)Policy;
	if (!IsInGameThread() || !FDreamEditorModule::IsAvailable())
	{
		return false;
	}
	FDreamEditorModule& Module = FDreamEditorModule::Get();
	if (Module.GetRunService().IsRunActive() || Module.GetSupervisor().GetPumpDepth() > 0)
	{
		return false;
	}
	return !bNeedsPython || Module.GetPython().IsReady();
}

uint64 FDreamDeferral::GetDeferredCount(FName Client)
{
	const uint64* Count = DreamDeferralPrivate::DeferredCounts().Find(Client);
	return Count != nullptr ? *Count : 0;
}
