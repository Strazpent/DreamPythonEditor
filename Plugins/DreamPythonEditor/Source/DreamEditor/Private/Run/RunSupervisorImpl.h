// SPDX-License-Identifier: Apache-2.0
// FRunSupervisor's state (TDD §5.4.1, §5.7.2), shared by RunSupervisor.cpp (game thread: pending call, pump, presentation)
// and RunSupervisorWatchdog.cpp (the DreamRunWatchdog thread). Supervisor slice internal; include it only from those two.
// It includes no Python.h (§2.5 rule 6): CPython types are the DreamPythonFwd.h forward declarations, and the .cpp files
// include Python through DreamPythonInclude.h.
//
// Threads: every std::atomic below is written on one side and read on the other with sequentially consistent
// operations (the default), because bInPython / bPythonAlive and the post flags are each "store one, load the other"
// handshakes (§5.4.1). Fields marked GT are touched by the game thread only.
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Python/DreamPythonFwd.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunProtections.h"

#include <atomic>

class FEvent;
class FFeedbackContext;
class FRunnableThread;
class SDreamRunWindow;
class SWindow;
struct FDreamRunRecord;

struct FRunSupervisor::FImpl
{
	explicit FImpl(FRunSupervisor& InOwner);
	~FImpl();

	FRunSupervisor& Owner;

	/** The one live supervisor, for the pending-call trampoline (Py_AddPendingCall passes only a void*). */
	static std::atomic<FImpl*> Instance;

	// ---- Lifecycle (§3.5, §3.8 step 2, RV-01) ----

	std::atomic<bool> bPythonAlive{false};
	/** Set by the watchdog before it re-checks bPythonAlive and makes any Python call; cleared when it is out. */
	std::atomic<bool> bInPython{false};
	std::atomic<uint32> Generation{0};
	/** Held around every post (§5.4.1 shutdown safety). Never held while taking the GIL (GIL-5). */
	FCriticalSection PostLock;

	// ---- Run identity and arming (GT writes, watchdog reads) ----

	std::atomic<FDreamRunId> CurrentRunId{0};
	std::atomic<bool> bArmed{false};
	/** (debug) The run is paused in the Paused window (§8.3.1 step 2): the step-1a gate. */
	std::atomic<bool> bPaused{false};
	std::atomic<double> ArmAt{0.0};
	std::atomic<bool> bOffPresentation{false};
	/** GT: the run whose presentation is up (kept after EndRun until EndPresentation). */
	FDreamRunId PresentationRunId = 0;
	/** GT. */
	FDreamRunPolicy Policy;
	/** GT: Policy.Filename as UTF-8, compared with co_filename in the frame walk (§5.4.7). */
	TArray<ANSICHAR> FilenameUtf8;
	/** GT: BeginRun time (elapsed time, U2). */
	double RunStartSeconds = 0.0;

	// ---- Posting (§5.4.1) ----

	/** At most one of our pending calls is outstanding, counting the one executing; cleared when the callback exits. */
	std::atomic<bool> bPendingOutstanding{false};
	/** The run ID of the outstanding call: a stale call of an older run never clears the newer run's flag. */
	std::atomic<FDreamRunId> OutstandingPostId{0};
	/** The outstanding call was queued by the fast chain (§5.7.3) on the game thread, not by a watchdog post: it re-delivers
	 *  without the 250 ms gap when no exception is being handled (DeliverIfDue, T-CAN-003, review r1 run-M1). */
	std::atomic<bool> bChainedCall{false};
	std::atomic<double> PostTime{0.0};
	std::atomic<double> HandOffWait{0.0};
	std::atomic<double> PeriodSeconds{1.0 / 15.0};
	std::atomic<int32> AddFailures{0};
	std::atomic<bool> bLastAddFailed{false};

	// ---- Cancellation (§5.7.2) ----

	std::atomic<bool> bUserCodeActive{false};
	std::atomic<bool> bStopRequested{false};
	std::atomic<uint8> StopSource{0};
	std::atomic<bool> bInterruptCleanup{false};
	std::atomic<int32> Deliveries{0};
	std::atomic<double> StopRequestedAt{0.0};
	std::atomic<double> FirstDeliveryAt{0.0};
	std::atomic<double> LastDeliveryAt{0.0};
	std::atomic<double> LastCheckAt{0.0};
	std::atomic<bool> bLastCheckHandling{false};
	/** RequestStop came from another thread: OnUserCancel and the Stopping… label wait for the next pending call. */
	std::atomic<bool> bStopUiPending{false};

	/** The run's native re-entry mark (§2.6.3), captured in UserCodeBegin with the GIL held (a fresh mark for each
	 *  user-code window). Read under the GIL by the pending call and by the watchdog's §5.7.5 check. */
	FDreamPyThreadState* MarkThreadState = nullptr;
	int32 MarkGilCounter = 0;
	const void* MarkCFrame = nullptr;
	bool bHasMark = false;
	/** GT: user_code_begin calls of this run; the first window's refusal is a delivery, later windows' are not (CR-07). */
	int32 UserCodeWindows = 0;
	/** PyThread ident of the game thread and the registered cancel class (borrowed), captured in UserCodeBegin with the
	 *  GIL held, so the watchdog's §5.7.5 channel calls nothing else in Python to find them. */
	std::atomic<uint64> GameThreadIdent{0};
	std::atomic<FDreamPyObject*> CancelClass{nullptr};
	/** GT: user_code_done nulled a pending async exception, which leaves 3.11's eval breaker armed; EndRun drops it. */
	bool bAsyncExcBreakerArmed = false;
	/** What the last pending call found (the §5.7.5 condition 4 input the watchdog cannot compute itself). */
	std::atomic<bool> bLastDebugpyInnermost{false};

	// ---- Pump (§5.4.4, §5.4.6) ----

	std::atomic<int32> PumpDepth{0};
	double CostEwma = 0.0;             // GT
	bool bHaveCost = false;            // GT
	/**
	 * GT: pumps completed since the last native interval ended (-1: none this run), and when it ended. A Stop from input
	 * that the first pump after it processes (StopAfterNativePumps) was queued during the native call (§5.8, B-note).
	 */
	int32 PumpsSinceNative = -1;
	double NativeEndedAt = 0.0;
	/** GT: length and line of the last native interval (any length >= 100 ms), for FDreamRunStats::StopNative* (§5.8). */
	double LastIntervalSeconds = 0.0;
	int32 LastIntervalLine = 0;
	double DeferStartAt = 0.0;         // GT: start of the current run of RT-02 deferrals

	// ---- Statistics (GT) ----

	FDreamRunStats Stats;
	FDreamRunLiveStats Live;
	/** The run-file line where the last delivery landed, for the swallow report (§5.7.4). */
	int32 DeliveryLine = 0;

	// ---- Progress (§5.10.1; any thread under ProgressLock, copied on the GT) ----

	struct FProgressSample
	{
		double Time = 0.0;
		int64 Done = 0;
	};
	FCriticalSection ProgressLock;
	FDreamRunId ProgressRunId = 0;
	int64 ProgressDone = -1;
	TOptional<int64> ProgressTotal;
	FString ProgressMessage;
	uint64 ProgressSerial = 0;
	static constexpr int32 ProgressRingSize = 16;
	FProgressSample ProgressRing[ProgressRingSize];
	int32 ProgressRingCount = 0;
	int32 ProgressRingHead = 0;        // next write
	double ProgressLastRingAt = 0.0;
	uint64 ProgressCopiedSerial = 0;   // GT

	// ---- Presentation (GT, §5.5) ----

	FDreamRunProtections Protections;
	TSharedPtr<SWindow> RunWindow;
	TSharedPtr<SDreamRunWindow> RunWindowContent;
	bool bPresentationStarted = false;
	uint32 NotesSignature = 0;
	/** GWarn as captured at construction (the editor's FFeedbackContextEditor, §5.12). */
	FFeedbackContext* EditorWarn = nullptr;

	// ---- Watchdog thread (RunSupervisorWatchdog.cpp) ----

	/** The DreamRunWatchdog FRunnable, defined in RunSupervisorWatchdog.cpp and deleted in StopWatchdog. */
	class FWatchdog;
	FRunnable* Watchdog = nullptr;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	std::atomic<bool> bStopThread{false};
	/** The watchdog's own thread state for this generation (GIL-4). Created on the watchdog thread inside the
	 *  bInPython bracket; cleared and deleted by the game thread in OnPythonShutdown once bInPython is false. */
	std::atomic<FDreamPyThreadState*> WatchdogState{nullptr};
	std::atomic<uint32> WatchdogStateGeneration{0};

	// ---- Game thread (RunSupervisor.cpp) ----

	/** What the §5.4.7 frame walk found at a pending call. */
	struct FFrameReport
	{
		bool bFoundRunFrame = false;    // a frame whose co_filename is the run's filename
		int32 RunLine = 0;              // that frame's line (<= 0 at a statement's first instruction)
		bool bHaveInnermost = false;
		bool bInnermostIsRun = false;
		FString InnermostName;          // "json/decoder.py:337"
		bool bDebugpy = false;          // the innermost frame is under _dream_vendor/debugpy (§5.4.3)
	};

	/** Py_AddPendingCall target. Arg is the run ID. */
	static int PendingCallTrampoline(void* Arg);
	/** §5.4.2. GT, GIL held. Returns 0, or -1 with ScriptCancelled set. */
	int OnPendingCall(FDreamRunId Id);
	/** The callback exits: clear the outstanding flag when it belongs to this call's run. */
	void ClearOutstanding(FDreamRunId Id);
	/** Fast chain (§5.7.3): queue the next call before returning -1; false when the queue is full. */
	bool RequeueFromGameThread(FDreamRunId Id);
	/** RequestStop's game-thread part: Stopping…, OnUserCancel (§5.12), the blocking-modal toast. */
	void ApplyStopOnGameThread();
	/** The native interval during which Stop was requested (§5.8 footer): bStopDuringNative and StopNative*, first time only. */
	void RecordStopDuringNative(double Seconds, int32 Line);
	/** A delivery that raised no exception here (Stop before user code, a game-thread check_cancel()): the counters only. */
	void CountDelivery(double Now);
	void ResetRunState();

	FFrameReport WalkFrames() const;
	void UpdateLiveStats(double Now, const FFrameReport& Frame);
	void MeasureNativeInterval(double Now);
	void CopyProgress();
	void CopySlowTask();
	bool FindDeferral(EDreamDeferReason& OutReason);
	void RecordDeferral(EDreamDeferReason Reason, double Now);
	void EndDeferral(double Now);
	/**
	 * §5.7.3. True when ScriptCancelled was set as the Python error (the callback returns -1). bChained: this call was
	 * re-queued by the fast chain after a delivery, so it is due without the 250 ms gap when no exception at all is being
	 * handled (architect rulings on T-CAN-003 and review r1 run-M1: §5.7.3's fast-chain prose and the §5.7.8 sequence win
	 * over the pseudo-code's bDue); ours handled ends the chain, a foreign one keeps the gap, as a watchdog post does.
	 */
	bool DeliverIfDue(double Now, const FFrameReport& Frame, bool bChained = false);
	/** A new ScriptCancelled instance as the Python error, counted as a delivery. */
	bool RaiseCancel(double Now, EDreamStopSource Source);
	/** §5.7.4: the handled exception, or one in its context chain, is ours. bOutAnyHandled: some exception is handled. */
	bool IsHandlingOurs(bool* bOutAnyHandled = nullptr) const;
	/** The swallow report and the cleanup timer (§5.7.4, §5.8 table). */
	void UpdateHandlingReport(double Now, const FFrameReport& Frame, bool bHandling);
	/** §5.4.4 with the §5.4.6 measurement. GIL held on entry and exit, released inside. */
	void Pump(bool bOutputArrived);
	/** Inside the pump, GIL released, before PumpMessages: protections, the Run window, notes growth (§5.5.3). */
	void OnPumpBegin();
	void OpenRunWindowIfPossible();
	void AdaptRate(double Cost, bool bOutputArrived);
	/** Finish step 3 (§5.5.3 "Hand-off"): the area's view takes over the live view's follow state and scroll. */
	void HandOffLiveView();
	const FDreamRunRecord* FindActiveRecord() const;
	EDreamRunPresentation EffectivePresentation() const;

	// ---- Watchdog side (RunSupervisorWatchdog.cpp) ----

	void EnsureWatchdog();
	void WakeWatchdog();
	void StopWatchdog();
	/** Spins (no GIL held by the caller) until the watchdog is out of Python (§5.7.7 step 1, §3.8 step 2). */
	void WaitWhileInPython() const;
	/** One watchdog period for run Id (§5.4.1). Watchdog thread. */
	void WatchdogTick(FDreamRunId Id, double Now);
	/** Under PostLock: bPythonAlive check, flag, Py_AddPendingCall. Watchdog thread. */
	bool TryPost(FDreamRunId Id, double Now);
	/** §5.4.1 steps 1-3 (bPosted: record HandOffWait); with bAsyncChannel the §5.7.5 channel runs inside the same
	 *  bracket, under the GIL taken on the watchdog's own thread state. Watchdog thread. */
	void HandOff(FDreamRunId Id, bool bPosted, bool bAsyncChannel);
	/** §5.7.5 conditions 1-3 (atomics). Watchdog thread. */
	bool AsyncChannelWanted(double Now) const;
	/** GT, GIL held (OnPythonShutdown): PyThreadState_Clear and PyThreadState_Delete of the watchdog's state. */
	void DeleteWatchdogThreadState();

	double LastAsyncExcAt = 0.0;       // watchdog thread only
};
