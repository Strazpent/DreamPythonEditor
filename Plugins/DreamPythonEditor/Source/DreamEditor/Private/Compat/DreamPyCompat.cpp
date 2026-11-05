// SPDX-License-Identifier: Apache-2.0
// The CPython 3.11 compatibility layer (TDD §2.6, D15, RT-02, RV-05). The only file that reads thread-state fields
// (->gilstate_counter, ->cframe, ->thread_id) or compares version macros (§2.5 rule 7).
#include "Compat/DreamPyCompat.h"

#include "Runtime/Launch/Resources/Version.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

#if WITH_PYTHON

// §2.6.2: 1.0 is built for CPython 3.11 inside UE 5.8 only. A canary build for the next branch sets DREAM_COMPAT_CANARY.
#if !defined(DREAM_COMPAT_CANARY) || !DREAM_COMPAT_CANARY
static_assert(PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 11, "FDreamPyCompat reads CPython 3.11 thread-state fields (TDD §2.6.3).");
static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8, "FDreamPyCompat is written against UE 5.8 (TDD §2.6.2).");
#endif

namespace DreamPyCompatPrivate
{
	/** FDreamPyCompat::GetEmptyNullCallsForTest; written under the GIL. */
	std::atomic<int32> EmptyNullCalls{ 0 };
	/** FDreamPyCompat::GetBreakerDropsForTest; written under the GIL. */
	std::atomic<int32> BreakerDrops{ 0 };

	/** Parses the leading "major.minor" of Py_GetVersion() ("3.11.8 (tags/v3.11.8:...) [MSC v.1937 64 bit]"). */
	bool ParseMajorMinor(const char* Version, int32& OutMajor, int32& OutMinor)
	{
		OutMajor = 0;
		OutMinor = 0;
		if (Version == nullptr)
		{
			return false;
		}
		const char* Cursor = Version;
		bool bAnyMajor = false;
		while (*Cursor >= '0' && *Cursor <= '9')
		{
			OutMajor = OutMajor * 10 + (*Cursor - '0');
			bAnyMajor = true;
			++Cursor;
		}
		if (!bAnyMajor || *Cursor != '.')
		{
			return false;
		}
		++Cursor;
		bool bAnyMinor = false;
		while (*Cursor >= '0' && *Cursor <= '9')
		{
			OutMinor = OutMinor * 10 + (*Cursor - '0');
			bAnyMinor = true;
			++Cursor;
		}
		return bAnyMinor;
	}
}

bool FDreamPyCompat::CheckRuntime(FText& OutReason)
{
	const char* RuntimeVersion = Py_GetVersion();
	int32 Major = 0;
	int32 Minor = 0;
	if (!DreamPyCompatPrivate::ParseMajorMinor(RuntimeVersion, Major, Minor))
	{
		OutReason = FText::Format(LOCTEXT("RuntimeVersionUnreadable", "Dream Python Editor cannot read the Python version of this editor ({0}). Runs are disabled."),
			FText::FromString(FString(UTF8_TO_TCHAR(RuntimeVersion ? RuntimeVersion : ""))));
		return false;
	}
	if (Major != PY_MAJOR_VERSION || Minor != PY_MINOR_VERSION)
	{
		OutReason = FText::Format(LOCTEXT("RuntimeVersionMismatch", "This editor runs Python {0}.{1}, but Dream Python Editor was built for Python {2}. Runs are disabled; install the Dream build for this engine."),
			FText::AsNumber(Major, &FNumberFormattingOptions::DefaultNoGrouping()),
			FText::AsNumber(Minor, &FNumberFormattingOptions::DefaultNoGrouping()),
			FText::FromString(TEXT(PY_VERSION)));
		return false;
	}
	return true;
}

FDreamEntryMark FDreamPyCompat::CaptureEntryMark()
{
	FDreamEntryMark Mark;
	PyThreadState* ThreadState = PyThreadState_Get();
	Mark.ThreadState = ThreadState;
	Mark.GilCounter = ThreadState->gilstate_counter;
	Mark.CFrame = ThreadState->cframe;
	return Mark;
}

bool FDreamPyCompat::IsNativeReentered(const FDreamEntryMark& Mark)
{
	// §2.6.3: engine thunks enter Python through PyGILState_Ensure, which raises the game thread state's counter;
	// generators, sorted(key=...) and __init__ do not. The comparison is on the current thread state, as the rule says.
	const PyThreadState* ThreadState = PyThreadState_Get();
	return ThreadState->gilstate_counter > Mark.GilCounter;
}

bool FDreamPyCompat::IsNativeReenteredOn(PyThreadState* ThreadState, const FDreamEntryMark& Mark)
{
	// §5.7.5 condition 4: the same rule, read on a named thread state; the caller holds the GIL, which guards the field.
	return ThreadState != nullptr && ThreadState->gilstate_counter > Mark.GilCounter;
}

PyObject* FDreamPyCompat::GetHandledException()
{
	return PyErr_GetHandledException();
}

int FDreamPyCompat::AddPendingCall(int (*Fn)(void*), void* Arg)
{
	return Py_AddPendingCall(Fn, Arg);
}

int FDreamPyCompat::SetAsyncExc(uint64 ThreadId, PyObject* ExcOrNull)
{
	if (ExcOrNull == nullptr)
	{
		// SUP-01: a null call on a state whose async_exc is already empty arms the breaker and nothing ever drops it.
		for (PyThreadState* State = PyInterpreterState_ThreadHead(PyInterpreterState_Get()); State != nullptr; State = PyThreadState_Next(State))
		{
			if (static_cast<uint64>(State->thread_id) == ThreadId)
			{
				DreamPyCompatPrivate::EmptyNullCalls += State->async_exc == nullptr ? 1 : 0;
				break;
			}
		}
	}
	return PyThreadState_SetAsyncExc(static_cast<unsigned long>(ThreadId), ExcOrNull);
}

int32 FDreamPyCompat::GetEmptyNullCallsForTest()
{
	return DreamPyCompatPrivate::EmptyNullCalls.load();
}

int32 FDreamPyCompat::GetBreakerDropsForTest()
{
	return DreamPyCompatPrivate::BreakerDrops.load();
}

uint64 FDreamPyCompat::GetThreadId(PyThreadState* ThreadState)
{
	return ThreadState != nullptr ? static_cast<uint64>(ThreadState->thread_id) : 0;
}

bool FDreamPyCompat::HasAsyncExc(PyThreadState* ThreadState)
{
	return ThreadState != nullptr && ThreadState->async_exc != nullptr;
}

void FDreamPyCompat::ClearAsyncExc(PyThreadState* ThreadState, bool bBreakerArmed)
{
	if (ThreadState == nullptr || ThreadState != PyThreadState_Get() || (!bBreakerArmed && ThreadState->async_exc == nullptr))
	{
		return;
	}
	// 3.11 ceval.c: only eval_frame_handle_pending's async_exc branch runs UNSIGNAL_ASYNC_EXC. A breaker armed by a null
	// SetAsyncExc gets a placeholder to deliver; the RESUME of any code object then takes that branch (sup_review measure:
	// a stuck breaker costs 13-24% on every Python loop of the session).
	PyObject* SavedType = nullptr;
	PyObject* SavedValue = nullptr;
	PyObject* SavedTraceback = nullptr;
	PyErr_Fetch(&SavedType, &SavedValue, &SavedTraceback);
	if (ThreadState->async_exc == nullptr)
	{
		PyThreadState_SetAsyncExc(ThreadState->thread_id, PyExc_RuntimeError);
	}
	const bool bHeld = ThreadState->async_exc != nullptr;
	PyObject* Globals = PyDict_New();
	PyObject* Result = Globals != nullptr ? PyRun_StringFlags("None", Py_eval_input, Globals, Globals, nullptr) : nullptr;
	Py_XDECREF(Result);
	Py_XDECREF(Globals);
	PyErr_Clear();
	// Only the async branch of eval_frame_handle_pending empties async_exc, and it unsignals the breaker in the same step:
	// an exception held before the evaluation and gone after it was delivered, and the breaker dropped (T-CAN-009).
	DreamPyCompatPrivate::BreakerDrops += bHeld && ThreadState->async_exc == nullptr ? 1 : 0;
	if (ThreadState->async_exc != nullptr)
	{
		// Not delivered (no evaluation ran): at least never let it reach the next run.
		PyThreadState_SetAsyncExc(ThreadState->thread_id, nullptr);
	}
	PyErr_Restore(SavedType, SavedValue, SavedTraceback);
}

#endif // WITH_PYTHON

#undef LOCTEXT_NAMESPACE
