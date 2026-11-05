// SPDX-License-Identifier: Apache-2.0
// The CPython compatibility layer (TDD §2.6, D15, RT-02, RV-05). Only Private/Compat reads CPython internals
// (->gilstate_counter, ->cframe, _Py* names) or compares version macros (§2.5 rule 7). Built per Python minor version.
// Includes Python.h, so only the §2.5 rule 6 folders include this header (the supervisor and the bridge do).
#pragma once

#include "CoreMinimal.h"
#include "Python/DreamPythonInclude.h"

#if WITH_PYTHON

/** The native re-entry mark (§2.6.3): captured at user_code_begin for the run (and at eval_begin, M3). */
struct FDreamEntryMark
{
	PyThreadState* ThreadState = nullptr;
	int32 GilCounter = 0;          // tstate->gilstate_counter (Engine/Source/ThirdParty/Python3/Win64/include/cpython/pystate.h:129)
	const void* CFrame = nullptr;  // tstate->cframe, diagnostics only (§2.6.3 "Why not cframe")
};

struct DREAMEDITOR_API FDreamPyCompat
{
	/** Runtime major.minor from Py_GetVersion() equals the compiled PY_VERSION (§2.6.2). False: no _dream, runs disabled. */
	static bool CheckRuntime(FText& OutReason);

	/** GT, GIL held. */
	static FDreamEntryMark CaptureEntryMark();

	/** GT, GIL held: PyThreadState_Get()->gilstate_counter > Mark.GilCounter (§2.6.3 rule). */
	static bool IsNativeReentered(const FDreamEntryMark& Mark);

	/**
	 * GIL held, any thread: ThreadState->gilstate_counter > Mark.GilCounter. The watchdog evaluates §5.7.5 condition 4 with
	 * it under the GIL it took on its own thread state, where PyThreadState_Get() is not the game thread's state; it passes
	 * Mark.ThreadState (the game thread's state, captured at user_code_begin). False for a null ThreadState.
	 */
	static bool IsNativeReenteredOn(PyThreadState* ThreadState, const FDreamEntryMark& Mark);

	/** PyErr_GetHandledException(): a new reference or null (RT-06; pyerrors.h:22). Caller wraps it in an FDreamPyRef. */
	static PyObject* GetHandledException();

	/** Any thread, no GIL needed to post (ceval.h:46). The watchdog then forces a GIL hand-off (GIL-4, RV-01). */
	static int AddPendingCall(int (*Fn)(void*), void* Arg);

	/**
	 * PyThreadState_SetAsyncExc (pystate.h:67 takes unsigned long). GIL held. ExcOrNull null clears. In 3.11 a null call
	 * still signals the interpreter-wide eval breaker (pending.async_exc), which only a real delivery drops again: clear
	 * through ClearAsyncExc, never with a null call on a state that holds nothing.
	 */
	static int SetAsyncExc(uint64 ThreadId, PyObject* ExcOrNull);

	/**
	 * Test hook (T-CAN-009, SUP-01): how many SetAsyncExc calls cleared a state that held nothing, the call that arms 3.11's
	 * eval breaker for good. GIL held at each count; read on the game thread.
	 */
	static int32 GetEmptyNullCallsForTest();
	/**
	 * Test hook (T-CAN-009, SUP-01): how many ClearAsyncExc calls saw their pending exception or placeholder delivered by the
	 * trivial evaluation, which drops the eval breaker. GIL held at each count; read on the game thread.
	 */
	static int32 GetBreakerDropsForTest();

	/** GIL held: ThreadState->async_exc is set (cpython/pystate.h:131), i.e. an asynchronous exception is still pending. */
	static bool HasAsyncExc(PyThreadState* ThreadState);

	/**
	 * GT, GIL held, ThreadState is the current thread's state, outside user code (§5.7.5, §5.7.7 step 2): drops a pending
	 * asynchronous exception and the eval breaker it armed. bBreakerArmed says a null SetAsyncExc already ran on this state
	 * (the breaker is set although async_exc is empty). A pending exception is delivered to a trivial evaluation, so
	 * CPython's own unsignal runs, and the error is cleared; nothing is touched when neither holds.
	 */
	static void ClearAsyncExc(PyThreadState* ThreadState, bool bBreakerArmed);

	/** The OS thread id CPython stores in a thread state (cpython/pystate.h:132), for SetAsyncExc. GIL held. */
	static uint64 GetThreadId(PyThreadState* ThreadState);
};

#endif // WITH_PYTHON
