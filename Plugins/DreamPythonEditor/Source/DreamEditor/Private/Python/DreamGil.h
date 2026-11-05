// SPDX-License-Identifier: Apache-2.0
// GIL helpers and the owned-reference type (TDD §3.2 GIL rules, §3.8 ownership). Include only from the §2.5 rule 6
// folders (it includes Python.h). Written from behaviour (§2.5 rule 2): the engine's FPyScopedGIL is private.
#pragma once

#include "Python/DreamPythonInclude.h"

#if WITH_PYTHON

/**
 * Takes the GIL for a scope with PyGILState_Ensure / PyGILState_Release (GIL-1). Non-copyable.
 * Game thread entries other than the pending call and the _dream functions (which already hold it) use this.
 * The watchdog never does: it takes the GIL only on its own thread state (GIL-4, RunSupervisor.cpp).
 */
class FDreamScopedGil
{
public:
	FDreamScopedGil()
		: State(PyGILState_Ensure())
	{
	}
	~FDreamScopedGil()
	{
		PyGILState_Release(State);
	}
	FDreamScopedGil(const FDreamScopedGil&) = delete;
	FDreamScopedGil& operator=(const FDreamScopedGil&) = delete;

private:
	PyGILState_STATE State;
};

/**
 * Releases the GIL for a scope (Py_BEGIN_ALLOW_THREADS as RAII). Used around exactly the three waits of GIL-3:
 * the Slate pump, AddModalWindow for the Paused window (M3), and the shutdown wait for the watchdog.
 */
class FDreamScopedGilRelease
{
public:
	FDreamScopedGilRelease()
		: Saved(PyEval_SaveThread())
	{
	}
	~FDreamScopedGilRelease()
	{
		PyEval_RestoreThread(Saved);
	}
	FDreamScopedGilRelease(const FDreamScopedGilRelease&) = delete;
	FDreamScopedGilRelease& operator=(const FDreamScopedGilRelease&) = delete;

private:
	PyThreadState* Saved;
};

/**
 * A strong reference that C++ holds (GIL-7, §3.8). Stamped with the Python generation and counted in FDreamPython's
 * registry; HandleShutdown releases every live ref and asserts the count is zero (§3.8 step 5). Destroying one without
 * the GIL queues the decref to the game thread (FDreamPython::QueueDecref). A ref from an older generation is never
 * decremented: it is leaked with an ensure, because its interpreter is gone.
 * (slice A choice) One non-template type; §3.8's TDreamPyRef<T> would add nothing over PyObject* here.
 */
class DREAMEDITOR_API FDreamPyRef
{
public:
	FDreamPyRef() = default;
	/** Takes ownership of a new reference (no incref). GIL held. */
	static FDreamPyRef Steal(PyObject* NewRef);
	/** Adds a reference (Py_XINCREF). GIL held. */
	static FDreamPyRef Borrow(PyObject* BorrowedRef);

	FDreamPyRef(const FDreamPyRef& Other);              // GIL held
	FDreamPyRef(FDreamPyRef&& Other);
	FDreamPyRef& operator=(const FDreamPyRef& Other);   // GIL held
	FDreamPyRef& operator=(FDreamPyRef&& Other);
	~FDreamPyRef();

	PyObject* Get() const { return Object; }
	explicit operator bool() const { return Object != nullptr; }
	uint32 GetGeneration() const { return Generation; }

	/** Gives up ownership and returns the reference (no decref). */
	PyObject* Release();
	/** Drops the reference now (GIL held) or queues it (no GIL). */
	void Reset();

private:
	PyObject* Object = nullptr;
	uint32 Generation = 0;
};

#endif // WITH_PYTHON
