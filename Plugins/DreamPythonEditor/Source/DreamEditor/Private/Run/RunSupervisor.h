// SPDX-License-Identifier: Apache-2.0
// The RunSupervisor (TDD §5.4-§5.8, §12.3, D2, RV-01, RV-05): watchdog thread, pending call, GIL hand-off, guards,
// pump, cancellation, native-call reporting, and the presentation on the first pump. Body: RunSupervisor.cpp (+ the
// watchdog in RunSupervisorWatchdog.cpp), supervisor slice. This header includes no Python.h (rule 6): Python-typed
// state lives in the .cpp, which includes Compat/DreamPyCompat.h and Python/DreamNativeModule.h.
#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"
#include "Run/DreamRunTypes.h"

class SWidget;

/** BeginRun's parameters (§5.4.1, §5.5.1, §5.5.3). Built by FDreamRunService at entry. */
struct FDreamRunPolicy
{
	EDreamRunPresentation Presentation = EDreamRunPresentation::RunWindow; // UDreamUserSettings (policy is M4); Off = kill switch run.pump
	double ArmDelaySeconds = 0.3;       // ~300 ms for runs (RT-04); 1 s for task steps (M3)
	int32 RunNumber = 0;
	FString TabOrItem;                  // Run window title: "Dream · Run #22 · <tab or item>"
	FString Filename;                   // compile filename, for the §5.4.7 frame walk
	double CleanupInterruptSeconds = 10.0; // RT-06 (UDreamUserSettings::CleanupInterruptSeconds)

	/** The output area's cached geometry at entry, in desktop pixels (§5.5.3 step 4, PO-03). */
	FSlateRect OutputAreaRect;
	bool bOutputAreaVisible = false;
	bool bOutputAreaAtTop = true;       // layout.output_position: the window grows toward the code area
	float DpiScale = 1.f;
	TWeakPtr<SWidget> OutputAreaWidget; // AddModalWindow parent and the hand-off target
	TWeakPtr<SWidget> FocusToRestore;   // §5.5.2 "Focus returns to the widget that had it before"

	/**
	 * Called in every pending call that reaches §5.4.2 step 7 (game thread, GIL held, no Python may be called):
	 * the transaction size watch (FDreamTransactions::OnPump on the record's handle, §5.9.4), which writes TxnBytes and
	 * TxnBudget into Live for the Run window's undo meter, and the WS-07 snapshot copy. FDreamRunService supplies it.
	 * (Live is the supervisor's FDreamRunLiveStats; nothing else outside the supervisor writes it.)
	 */
	TFunction<void(FDreamRunLiveStats& Live)> OnPump;
};

class DREAMEDITOR_API FRunSupervisor
{
public:
	static constexpr double RedeliverySeconds = 0.250;   // D2
	static constexpr double AsyncChannelSeconds = 1.0;   // §5.7.5 condition 1
	static constexpr double NativeIntervalSeconds = 0.100; // §5.8
	static constexpr double DrainBudgetSeconds = 0.002;  // D22

	FRunSupervisor();
	~FRunSupervisor();

	/** Module shutdown: stop and join the watchdog (§3.8). Python is already finalized. */
	void Shutdown();

	// ---- Lifecycle (FDreamPython) ----

	/** HandleInitialized: set bPythonAlive under PostLock before any BeginRun (§3.5). GIL held. */
	void OnPythonInitialized(uint32 Generation);
	/** §3.8 step 2: invalidate the run, disarm, clear bPythonAlive under PostLock, release the GIL and wait for the
	 *  watchdog's bInPython to clear, re-take the GIL, clear and delete the watchdog's thread state (RV-01). GIL held. */
	void OnPythonShutdown();

	// ---- Run (FDreamRunService, game thread) ----

	/** Before ExecPythonCommandEx: CurrentRunId, ArmAt = now + ArmDelay, wake the watchdog (created lazily on the first run). */
	void BeginRun(FDreamRunId Id, FDreamRunPolicy&& Policy);
	/**
	 * After ExecPythonCommandEx returns, without the GIL (§5.7.7): CurrentRunId = 0, disarm, wait until bInPython is false;
	 * take the GIL, drop a pending async exception only when one is pending or user_code_done nulled one
	 * (FDreamPyCompat::ClearAsyncExc: a null SetAsyncExc on an empty state would arm 3.11's eval breaker for good),
	 * Py_MakePendingCalls, release; final output drain; freeze stats (and the last progress, OUT-02) into OutStats.
	 */
	void EndRun(FDreamRunId Id, FDreamRunStats& OutStats);

	/** Test hook (T-CAN-009, SUP-01): GT; takes the GIL; an asynchronous exception is pending on the game thread's state. */
	bool HasPendingAsyncExcForTest() const;
	/** Test hook (SUP-01): FDreamPyCompat::GetEmptyNullCallsForTest, the null SetAsyncExc calls on an empty state so far. */
	int32 GetEmptyAsyncExcClearsForTest() const;
	/** Test hook (SUP-01): FDreamPyCompat::GetBreakerDropsForTest, the deliveries ClearAsyncExc made so far (each drops the
	 *  eval breaker a pending exception or user_code_done's null call armed). */
	int32 GetAsyncExcBreakerDropsForTest() const;
	/** Test hook (SUP-01): GT, no run; arms the eval breaker with nothing pending and sets the flag user_code_done sets, so
	 *  the next EndRun takes ClearAsyncExc's placeholder path. False when it could not (no Python, a run, a pending one). */
	bool ArmAsyncExcBreakerForTest();
	/** (tests) Test hook, the PostSupervisorCall fixture of TDD §14.2 (T-SUP-033, T-DBG-029): Py_AddPendingCall with the
	 *  supervisor's own callback and Id (0 = the current run) from the calling thread, with no GIL hand-off. The caller holds
	 *  the GIL (a _dream_test call). False when there is no run to name or CPython's pending-call queue is full. */
	bool PostPendingCallForTest(FDreamRunId Id = 0);
	/** Finish step 3: remove the common protections, hand the live view over, tear the Run window down (§5.5.3). */
	void EndPresentation(FDreamRunId Id);

	// ---- From _dream (bridge; GIL held, game thread) ----

	/**
	 * user_code_begin: bUserCodeActive = true, capture the window's entry mark. False when Stop was already requested; in
	 * the run's first window that counts as a delivery (the runner raises ScriptCancelled itself).
	 * A run may open a second window after its first user_code_done (the runner's call of a replaced sys.excepthook,
	 * §5.3.7): it takes a fresh entry mark and reopens delivery and the pump, and resets neither the run's stats nor its
	 * Stop state or delivery clocks; with Stop already requested it returns false and counts no delivery.
	 */
	bool UserCodeBegin(FDreamRunId Id);
	/** user_code_done: bUserCodeActive = false (stops new posts, GIL-6), clear the GT async exception. Idempotent; closes
	 *  whichever window is open. */
	void UserCodeDone(FDreamRunId Id);
	/**
	 * run_statement step 1: when Stop is requested for Id, sets ScriptCancelled as the Python error (a new instance,
	 * counted as a delivery) and returns true; the caller returns NULL to Python. GIL held.
	 */
	bool RaiseCancelIfStopRequested(FDreamRunId Id);

	// ---- Any thread (the atomics); the game-thread-only parts are deferred when called elsewhere ----

	/**
	 * Sets the atomics (any thread; §12.3). On the game thread (every slice-A Stop source: the pre-processor, the Run
	 * window, WS-07, Shift+F5) it also calls OnUserCancel on the captured editor GWarn when a user slow task is open
	 * (§5.12) and marks the live stats Stopping…; from another thread those two wait for the next pending call, because
	 * GWarn and Slate are game-thread objects. Delivery happens in the pending call (§5.4.2 step 9).
	 */
	void RequestStop(FDreamRunId Id, EDreamStopSource Source);
	/** RT-06 explicit choice: the next pending call delivers once regardless of the handling check (§5.7.6). Atomic. */
	void RequestInterruptCleanup(FDreamRunId Id);
	bool IsStopRequested(FDreamRunId Id) const;
	/** Stop pending for the calling context's run (the active run); false under the Off presentation. No side effects. */
	bool IsStopPendingForCurrent() const;
	/**
	 * _dream.check_cancel(deliver) (§5.10.2, A19, B34). Without bDeliver: IsStopPendingForCurrent(), no side effects, any
	 * thread. With bDeliver (dream_editor.check_cancel() on the game thread, about to raise ScriptCancelled): true only when
	 * Stop is pending, the caller is inside the user-code window and outside our pump (not nested foreign Python, §5.4.8),
	 * and no §5.4.3 deferral condition holds (saving a package, GC, PostLoad, native re-entry); that true counts as a
	 * delivery (Deliveries, FirstDeliveryAt, LastDeliveryAt), so the 250 ms re-delivery and the Interrupt cleanup clocks
	 * start from it. A false while Stop is pending is the deferral row: the caller returns True instead of raising. Off
	 * the game thread bDeliver changes nothing. GIL held when called from _dream.
	 */
	bool CheckCancel(bool bDeliver = false);
	/** progress(): latest sample with its timestamp in atomic slots (the pending call copies it into the live stats);
	 *  a 16-entry ring at most every 100 ms (§5.10.1). No-op outside a run. Never touches Slate. */
	void SetProgress(FDreamRunId Id, int64 Done, TOptional<int64> Total, FStringView Message);

	// ---- Game thread queries ----

	/**
	 * (debug, PO-06 F5) §8.3.1 step 2 and its resume: while paused the pending call returns at once (no pump, no
	 * cancellation, no SetAsyncExc: the step-1a gate) and the watchdog is disarmed; resuming re-arms it with the arm delay.
	 * The live state reads Paused meanwhile. Game thread (FDreamDebugger::RunPause).
	 */
	void SetPaused(FDreamRunId Id, bool bPaused);
	bool IsPaused() const;

	/** > 0 inside the pump (§5.4.4); FDreamDeferral and the pause refusal read it. */
	int32 GetPumpDepth() const;
	/** Live values for the Run window, WS-07 and the status strip (§5.4.7). */
	const FDreamRunLiveStats& GetLiveStats() const;
	/** True while the Run window is up (modal): the views keep their last frame (RV-16). */
	bool IsRunWindowOpen() const;
	FDreamRunId GetCurrentRunId() const;
	/** Test hook (VIS-02): the Run window's live run view while the window is up, else null. */
	TSharedPtr<class SDreamOutputView> GetLiveRunViewForTest() const;
	/** Test hook (T-SUP-025, T-SUP-027/028 subset): the input pre-processor the protections installed, else null. */
	TSharedPtr<class FDreamInputPreProcessor> GetInputPreProcessorForTest() const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
