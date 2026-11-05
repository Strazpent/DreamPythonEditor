// SPDX-License-Identifier: Apache-2.0
// FDreamPython's state, shared by DreamPython.cpp (lifecycle, helpers), DreamPythonEntry.cpp (EnterRun, Boot) and
// DreamPythonAliases.cpp (selection aliases, map change). Include only from those files. Game thread unless a field says
// otherwise.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Python/DreamGil.h"
#include "Python/DreamPython.h"

#include <atomic>

struct FDreamPython::FImpl
{
	/** Any thread (GetState). */
	std::atomic<EDreamPythonState> State{ EDreamPythonState::Unavailable };
	/** Any thread (GetGeneration); +1 at every OnPythonInitialized (§3.5). */
	std::atomic<uint32> Generation{ 0 };

	bool bHeadless = false;
	bool bModuleCreated = false;        // _dream exists in this generation, so a boot retry makes sense
	bool bBooted = false;               // _boot returned True in this generation
	bool bStartWanted = false;          // a deferred-start trigger arrived before Cold (§3.4)
	bool bBootPending = false;
	bool bEnablePending = false;
	bool bResetPending = false;
	FText FailureReason;                // BootFailed, a failed CheckRuntime, a failed Enable Python
	FDateTime ShutDownLocal;            // "Python was shut down at 14:03." (§9.5)
	FTSTicker::FDelegateHandle EnableTimeout;
	/** OnPythonConfigured, bound only when Initialize ran before Python was configured, and the tick that then decides. */
	FDelegateHandle ConfiguredHandle;
	FTSTicker::FDelegateHandle ConfiguredTicker;

	/** NotifyBufferSaved (R2): saved paths waiting for the idle tick, the generation they belong to, and the tick
	 *  (an FDreamDeferral ticker; removed in HandleShutdown and Shutdown). */
	TArray<FString> SavedPaths;
	uint32 SavedPathsGeneration = 0;
	FTSTicker::FDelegateHandle SavedPathsTicker;
	/** Removes the tick and forgets the waiting paths (DreamPython.cpp). */
	void CancelSavedPaths();

	/** GetSessionEpoch (U19, §7.5.3). Any thread. */
	std::atomic<uint32> SessionEpoch{ 0 };
	/** Editor events for the aliases (DreamPythonAliases.cpp) and their idle ticks, each queued at most once. */
	FDelegateHandle SelectionChangedHandle;
	FDelegateHandle SelectObjectHandle;
	FDelegateHandle SelectNoneHandle;
	FDelegateHandle MapChangeHandle;
	FTSTicker::FDelegateHandle AliasRefreshTicker;
	FTSTicker::FDelegateHandle MapChangeTicker;
	/** Removes both alias ticks (DreamPythonAliases.cpp). */
	void CancelAliasWork();

#if WITH_PYTHON
	// Helper callables, looked up on first use per generation (§3.8 "Helper callables"), released in HandleShutdown.
	FDreamPyRef PrepareFn;
	FDreamPyRef DiscardFn;
	FDreamPyRef ClassifyFn;
	FDreamPyRef EmergencyRestoreFn;
	FDreamPyRef ResetSessionFn;
	FDreamPyRef OnBufferSavedFn;
	FDreamPyRef AliasRefreshFn;          // _impl.aliases.refresh(run_number)
	FDreamPyRef AliasMapChangeFn;        // _impl.aliases.on_map_change()

	void ReleaseHelpers()
	{
		PrepareFn.Reset();
		DiscardFn.Reset();
		ClassifyFn.Reset();
		EmergencyRestoreFn.Reset();
		ResetSessionFn.Reset();
		OnBufferSavedFn.Reset();
		AliasRefreshFn.Reset();
		AliasMapChangeFn.Reset();
	}
#endif

	/** Every state change goes through here, on the game thread, so OnStateChanged sees each one once. */
	void SetState(FDreamPython& Owner, EDreamPythonState NewState)
	{
		if (State.exchange(NewState) != NewState)
		{
			Owner.StateChangedEvent.Broadcast(NewState);
		}
	}
};
