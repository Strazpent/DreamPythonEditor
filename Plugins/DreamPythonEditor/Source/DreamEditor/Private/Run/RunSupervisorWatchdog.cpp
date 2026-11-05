// SPDX-License-Identifier: Apache-2.0
// The DreamRunWatchdog thread (TDD §5.4.1, §5.7.5, GIL-4, RV-01): created lazily on the first run, parked on an event
// between runs; posts one pending call per period while user code is active, then forces a GIL hand-off on its own
// PyThreadState (one per Python generation) so the game thread notices the post even in a pure-Python loop; and from
// 1 s after the first delivery uses PyThreadState_SetAsyncExc only under the §5.7.5 gates. It touches no Slate, UObject
// or output store, and calls Python only inside the bInPython bracket after re-checking bPythonAlive.
#include "Run/RunSupervisor.h"

#include "Run/RunSupervisorImpl.h"

#include "Compat/DreamPyCompat.h"
#include "Python/DreamPythonInclude.h"

#include "Module/DreamLog.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"

class FRunSupervisor::FImpl::FWatchdog final : public FRunnable
{
public:
	explicit FWatchdog(FImpl& InImpl)
		: Impl(InImpl)
	{
	}

	virtual uint32 Run() override;

	virtual void Stop() override
	{
		Impl.bStopThread.store(true);
		if (Impl.WakeEvent != nullptr)
		{
			Impl.WakeEvent->Trigger();
		}
	}

private:
	static uint32 ToWaitMs(double Seconds)
	{
		return static_cast<uint32>(FMath::Clamp(FMath::CeilToDouble(Seconds * 1000.0), 1.0, 1000.0));
	}

	FImpl& Impl;
};

uint32 FRunSupervisor::FImpl::FWatchdog::Run()
{
	FDreamRunId LastId = 0;
	double NextPostAt = 0.0;
	while (!Impl.bStopThread.load())
	{
		const FDreamRunId Id = Impl.CurrentRunId.load();
		if (Id == 0 || !Impl.bArmed.load())
		{
			Impl.WakeEvent->Wait();                       // parked between runs (§5.4.1)
			continue;
		}
		if (Id != LastId)
		{
			LastId = Id;
			NextPostAt = 0.0;
		}

		// Armed at ArmAt (about 300 ms into the run, RT-04), then one tick per period P = 1/f (§5.4.6 sets f).
		const double Now = FPlatformTime::Seconds();
		const double Due = FMath::Max(Impl.ArmAt.load(), NextPostAt);
		if (Now < Due)
		{
			Impl.WakeEvent->Wait(ToWaitMs(Due - Now));
			continue;
		}
		NextPostAt = Now + Impl.PeriodSeconds.load();
		Impl.WatchdogTick(Id, Now);
	}
	return 0;
}

void FRunSupervisor::FImpl::EnsureWatchdog()
{
	check(IsInGameThread());
	if (Thread != nullptr)
	{
		return;
	}
	bStopThread.store(false);
	if (WakeEvent == nullptr)
	{
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	}
	Watchdog = new FWatchdog(*this);
	Thread = FRunnableThread::Create(Watchdog, TEXT("DreamRunWatchdog"), 0, TPri_AboveNormal);
	if (Thread == nullptr)
	{
		UE_LOG(LogDream, Error, TEXT("The DreamRunWatchdog thread could not be created; runs are not supervised (no Run window, no Stop)."));
		delete Watchdog;
		Watchdog = nullptr;
	}
}

void FRunSupervisor::FImpl::WakeWatchdog()
{
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}

void FRunSupervisor::FImpl::StopWatchdog()
{
	if (Thread != nullptr)
	{
		Thread->Kill(true);                               // FWatchdog::Stop, then join (§3.8 module shutdown)
		delete Thread;
		Thread = nullptr;
	}
	delete Watchdog;
	Watchdog = nullptr;
	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

void FRunSupervisor::FImpl::WaitWhileInPython() const
{
	// The caller holds no GIL (GIL-6), so a watchdog inside a hand-off gets the GIL, gives it back and leaves at once.
	const double Start = FPlatformTime::Seconds();
	bool bLogged = false;
	while (bInPython.load())
	{
		FPlatformProcess::YieldThread();
		if (!bLogged && FPlatformTime::Seconds() - Start > 1.0)
		{
			bLogged = true;
			UE_LOG(LogDream, Warning, TEXT("Waiting for the DreamRunWatchdog to leave Python (over 1 s)."));
		}
	}
}

void FRunSupervisor::FImpl::WatchdogTick(FDreamRunId Id, double Now)
{
	// §5.4.1: posts only while the run is armed and user code is active; never under the Off presentation.
	// Deferred (M3): nothing is posted while Paused, except during a supervised evaluate, whose posts carry ActiveEvalId
	// instead of CurrentRunId (§8.3.4); task steps arm at 1 s through FDreamRunPolicy::ArmDelaySeconds (RT-09).
	if (!bUserCodeActive.load() || bOffPresentation.load())
	{
		return;
	}
	bool bPosted = false;
	bool bFull = false;
	if (!bPendingOutstanding.load())
	{
		bPosted = TryPost(Id, Now);
		bFull = !bPosted && bLastAddFailed.load();
	}
	if (bPosted)
	{
		HandOff(Id, /*bPosted=*/true, /*bAsyncChannel=*/false);
	}
	else if (bFull)
	{
		// The 32-slot queue is full of foreign calls (§5.14). The hand-off lets the game thread run them, and the
		// §5.7.5 channel, independent of the queue, is considered under its gates.
		HandOff(Id, /*bPosted=*/false, /*bAsyncChannel=*/AsyncChannelWanted(Now));
	}
}

bool FRunSupervisor::FImpl::TryPost(FDreamRunId Id, double Now)
{
#if WITH_PYTHON
	FScopeLock Lock(&PostLock);
	if (!bPythonAlive.load() || CurrentRunId.load() != Id || !bUserCodeActive.load())
	{
		return false;
	}
	bPendingOutstanding.store(true);
	OutstandingPostId.store(Id);
	bChainedCall.store(false);
	HandOffWait.store(0.0);
	PostTime.store(Now);
	if (FDreamPyCompat::AddPendingCall(&PendingCallTrampoline, reinterpret_cast<void*>(static_cast<UPTRINT>(Id))) == 0)
	{
		bLastAddFailed.store(false);
		return true;
	}
	// -1: queue full. Retry next period; the alternate channel may apply (§5.7.5 condition 3).
	OutstandingPostId.store(0);
	bPendingOutstanding.store(false);
	PostTime.store(0.0);
	bLastAddFailed.store(true);
	AddFailures.fetch_add(1);
#endif
	return false;
}

bool FRunSupervisor::FImpl::AsyncChannelWanted(double Now) const
{
	if (!bStopRequested.load() || Deliveries.load() == 0)
	{
		return false;
	}
	// (debug) review q1 m4: never while paused, where the game thread runs only the Paused window's requests (§8.3.1).
	if (bPaused.load())
	{
		return false;
	}
	// 1. At least 1 s since the first delivery (D2).
	if (Now - FirstDeliveryAt.load() < AsyncChannelSeconds)
	{
		return false;
	}
	// 2. Swallowed, not cleaning up (RT-06): the last check after the last delivery found no handling.
	if (bLastCheckHandling.load() || LastCheckAt.load() <= LastDeliveryAt.load())
	{
		return false;
	}
	// The debugpy row of §5.4.3 skips the channel; and never more often than the re-delivery period.
	if (bLastDebugpyInnermost.load() || Now - LastAsyncExcAt < RedeliverySeconds)
	{
		return false;
	}
	return true;                                          // 3. (the failed post) is the caller's condition
}

void FRunSupervisor::FImpl::HandOff(FDreamRunId Id, bool bPosted, bool bAsyncChannel)
{
#if WITH_PYTHON
	// Step 1: bInPython before the bPythonAlive re-check, so a shutdown either sees us or we see it (§5.4.1).
	bInPython.store(true);
	if (!bPythonAlive.load())
	{
		bInPython.store(false);
		return;
	}

	// Step 1a: our own thread state, created on this thread, so its thread_id is the watchdog's, never the game
	// thread's (cpython/pystate.h:132), and SetAsyncExc(GameThreadId, ...) can never match it.
	const uint32 Gen = Generation.load();
	PyThreadState* State = WatchdogState.load();
	if (State == nullptr || WatchdogStateGeneration.load() != Gen)
	{
		PyInterpreterState* Interpreter = PyInterpreterState_Main();
		State = Interpreter ? PyThreadState_New(Interpreter) : nullptr;
		WatchdogState.store(State);
		WatchdogStateGeneration.store(Gen);
	}

	if (State != nullptr)
	{
		// Step 2: if the game thread runs bytecode this waits one switch interval and asks it to drop the GIL; when it
		// takes the GIL back its eval breaker is recomputed on the main thread and our call runs (RV-01, C16).
		const double WaitStart = FPlatformTime::Seconds();
		PyEval_RestoreThread(State);
		if (bPosted)
		{
			HandOffWait.store(FMath::Min(FPlatformTime::Seconds() - WaitStart, 0.020));   // §5.8, capped at 20 ms
		}

		if (bAsyncChannel)
		{
			// §5.7.5 condition 4, under the GIL: depth 0 (RT-03), still in user code of the same run, the game thread
			// state's counter not above the run's entry mark (no native re-entry, read now on that state, not on ours),
			// and no debugpy frame at the last pending call. The mark fields are written by UserCodeBegin with the GIL
			// held, so the GIL orders them for this read. The instance it raises has run_number 0, which is ours.
			PyObject* CancelType = CancelClass.load();
			const uint64 GameTid = GameThreadIdent.load();
			bool bNoReentry = false;
			if (bHasMark && MarkThreadState != nullptr)
			{
				FDreamEntryMark Mark;
				Mark.ThreadState = MarkThreadState;
				Mark.GilCounter = MarkGilCounter;
				Mark.CFrame = MarkCFrame;
				bNoReentry = !FDreamPyCompat::IsNativeReenteredOn(MarkThreadState, Mark);
			}
			if (PumpDepth.load() == 0 && bUserCodeActive.load() && CurrentRunId.load() == Id && bNoReentry
				&& !bLastDebugpyInnermost.load() && CancelType != nullptr && GameTid != 0)
			{
				FDreamPyCompat::SetAsyncExc(GameTid, CancelType);
				LastAsyncExcAt = FPlatformTime::Seconds();
			}
		}

		// Step 3: give the GIL back at once.
		PyEval_SaveThread();
	}
	bInPython.store(false);
#endif
}

void FRunSupervisor::FImpl::DeleteWatchdogThreadState()
{
#if WITH_PYTHON
	// §3.8 step 2: GT, GIL held, and the watchdog is out of Python (bInPython false), so it cannot be using the state.
	if (PyThreadState* State = WatchdogState.exchange(nullptr))
	{
		PyThreadState_Clear(State);
		PyThreadState_Delete(State);
	}
	WatchdogStateGeneration.store(0);
#endif
}
