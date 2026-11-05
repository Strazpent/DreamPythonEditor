// SPDX-License-Identifier: Apache-2.0
// The ticker-client deferral rule (TDD §3.7, RT-01). Game thread only. Body: DreamDeferral.cpp (bridge slice).
// Every Dream ticker, timer or delegate client that can call Python, touch run state, mutate buffers or apply async
// results registers through this gate; raw AddTicker / RegisterActiveTimer / OnEndFrame.Add live only here, in
// FDreamRunService, or under DREAM_TIMER_EXEMPT(paint-only) (§2.5 rule 9).
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** Marks a timer that touches no run state and calls no Python (caret blink, status paint); the §2.5 rule 9 scan reads it. */
#define DREAM_TIMER_EXEMPT(Reason)

enum class EDreamDeferPolicy : uint8
{
	DeferWhileExecuting,    // skip this tick and try again next time (the body is not called)
	RefuseWhileExecuting,   // the caller refuses its request (CanRunNow false)
};

class DREAMEDITOR_API FDreamDeferral
{
public:
	/**
	 * Registers an FTSTicker client whose Body runs only when CanRunNow(Client, Policy, bNeedsPython) holds. While it does
	 * not, Defer re-arms silently and counts a deferral. Body returns false to unregister, as an FTSTicker delegate does.
	 */
	static FTSTicker::FDelegateHandle AddTicker(FName Client, EDreamDeferPolicy Policy, float DelaySeconds,
		bool bNeedsPython, TFunction<bool(float)> Body);
	static void RemoveTicker(FTSTicker::FDelegateHandle Handle);
	/**
	 * ShutdownModule: removes every ticker still registered through AddTicker (a gated async result never runs once
	 * IsAvailable() is false and would stay registered), so a runtime unload leaves no Dream lambda in the core ticker.
	 */
	static void RemoveAllTickers();

	/** False while FDreamRunService::IsRunActive(), or FRunSupervisor::GetPumpDepth() > 0, or (bNeedsPython and not Ready). */
	static bool CanRunNow(FName Client, EDreamDeferPolicy Policy, bool bNeedsPython);

	/** Test hook (T-ENTRY-012). */
	static uint64 GetDeferredCount(FName Client);

	/** At most this much Python-needing helper work per idle frame, summed over clients (§3.7, proposed 5 ms). */
	static constexpr double IdlePythonBudgetSeconds = 0.005;
};
