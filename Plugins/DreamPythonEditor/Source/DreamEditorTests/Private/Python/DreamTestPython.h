// SPDX-License-Identifier: Apache-2.0
// The test-only Python module `_dream_test` (TDD §14.2 DreamTestLatch and the §5.15 fixtures that a run calls from Python).
// Repository only: DreamEditorTests never ships (§2.2). Created in sys.modules at every OnPythonInitialized (the lifecycle
// may be dynamic, RUN-10 / T-LIFE-006), like DreamEditor's own _dream; this header includes no Python.h (§2.5 rule 6).
//
//   wait(name)                    pumped wait on dream_editor.sleep(0.01) while the latch is held; ScriptCancelled on Stop
//   held(name) -> bool            the latch is held (never blocks; for a task step that polls and yields)
//   release(name)                 releases a latch (drivers usually call FDreamTestLatch::Release from C++)
//   mark(name) -> float           records FPlatformTime::Seconds() under name; the driver reads it (FDreamTestLatch)
//   now() -> float                FPlatformTime::Seconds(), the clock of every Dream timestamp
//   sleep_native(seconds)         SleepNative: a native call that holds the GIL for that long (§5.8, T-SUP-030, T-CAN-020)
//   tick_core_ticker(n=1) -> int  TickCoreTicker: n nested FTSTicker::GetCoreTicker().Tick calls (RT-01, T-SUP-002,
//                                 T-RUN-027); returns n
//   fill_pending_call_queue(seconds=1.0) -> int
//                                 FillPendingCallQueue: fills CPython's pending-call queue with no-op calls that re-queue
//                                 themselves until seconds have passed, so a post of Dream's finds it full (§5.7.5);
//                                 returns how many were queued at once
//   queue_os_keys(keys) -> bool   QueueOsKeys: SendInput of keys ("esc", "shift+f5", "esc,shift+f5") and a wait until
//                                 GetQueueStatus(QS_KEY) shows them in this (the game) thread's queue (T-SUP-027); Win64
//   probe(name) -> str            a run-state probe the game thread published (FDreamTestProbes): "run_state",
//                                 "run_number", "modal", "pump_depth"
//   show_modal(title, message) -> str
//                                 a foreign Yes / No / Cancel modal with its own modal loop (Fixtures/DreamTestModalDialog.h:
//                                 EditorDialog.show_message returns its default unopened under automation); "cancel" on Esc
//   post_supervisor_call(run_id=0) -> bool
//                                 PostSupervisorCall: Py_AddPendingCall with Dream's own pending callback and run_id (0: the
//                                 current run) from the calling thread, no GIL hand-off (FRunSupervisor::PostPendingCallForTest,
//                                 T-SUP-033); False when there is no run or the queue is full
#pragma once

#include "CoreMinimal.h"

namespace DreamTestPython
{
	/** DreamEditorTests StartupModule: creates _dream_test now when Python is initialized, and at every later initialization. */
	void Register();
	/** ShutdownModule. */
	void Unregister();
	/** True while _dream_test exists in the live interpreter (game thread). */
	bool IsModuleCreated();
}
