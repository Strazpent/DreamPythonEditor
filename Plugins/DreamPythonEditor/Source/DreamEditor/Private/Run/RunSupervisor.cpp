// SPDX-License-Identifier: Apache-2.0
// FRunSupervisor, game-thread side (TDD §5.4-§5.8, D2, RV-01, RV-05): the run bracket, the pending-call callback, guards
// and deferral, the pump with its adaptive rate, live stats, cancellation and native-call reporting, and the
// presentation (protections and the Run window) opened from the first pump. The watchdog thread is in
// RunSupervisorWatchdog.cpp. Python C-API use is allowed here (§2.5 rule 6) and always runs with the GIL held.
#include "Run/RunSupervisor.h"

#include "Run/RunSupervisorImpl.h"

#include "Compat/DreamPyCompat.h"
#include "Diagnostics/DreamTrace.h"
#include "Python/DreamGil.h"
#include "Python/DreamNativeModule.h"
#include "Python/DreamPythonInclude.h"

#include "Debug/DreamDebugger.h"      // (debug)
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Output/DreamOutputStore.h"
#include "Run/SDreamRunWindow.h"
#include "Run/DreamRunService.h"
#include "Run/DreamRunSourceText.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"

#include "FeedbackContextEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Misc/SlowTask.h"
#include "Misc/SlowTaskStack.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/StallDetector.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#include <string_view>

#define LOCTEXT_NAMESPACE "Dream"

// Insights counters next to the DreamPump and DreamRun channels (§5.4.6, §13.1, TEL-03; Diagnostics/DreamTrace.h).
TRACE_DECLARE_FLOAT_COUNTER(DreamPumpCostMs, TEXT("Dream.Pump/CostMs"));
TRACE_DECLARE_FLOAT_COUNTER(DreamPumpRateHz, TEXT("Dream.Pump/RateHz"));
TRACE_DECLARE_INT_COUNTER(DreamRunDeferrals, TEXT("Dream.Run/Deferrals"));

namespace DreamSupervisorPrivate
{
	// §5.4.6 (D2, D22): target 15 Hz, 20 Hz with output or Stopping…, 10 Hz floor while C̄·f stays within 10%, and the
	// hard limit 0.15/C̄ above 15 ms with a 2 Hz floor (proposed).
	constexpr double TargetHz = 15.0;
	constexpr double BoostHz = 20.0;
	constexpr double FloorHz = 10.0;
	constexpr double OverloadFloorHz = 2.0;
	constexpr double OverheadShare = 0.10;
	constexpr double HardCostSeconds = 0.015;
	constexpr double HardShare = 0.15;
	constexpr double EwmaAlpha = 0.2;

	constexpr double NativeNoteSeconds = 1.0;   // §5.8: the live note only for intervals over 1 s
	// §5.8 (B-note, chrome-run-r10-m1): a Stop press or key made during a native call waits in the OS queue and is read
	// by the first pump after the call returns (Dream.Supervisor.StopPress), so only a Stop that pump processes counts as
	// during the call. A press in a later pump came after the call.
	constexpr int32 StopAfterNativePumps = 1;
	constexpr int32 MaxFrames = 64;             // §5.4.7 frame walk (proposed)

	inline void* IdToArg(FDreamRunId Id)
	{
		return reinterpret_cast<void*>(static_cast<UPTRINT>(Id));
	}

	/** Frames of the dream_editor package (the runner, _run and the public helpers) are never shown (§5.4.7). */
	inline bool IsDreamPackagePath(std::string_view Path)
	{
		return Path.find("/dream_editor/") != std::string_view::npos || Path.find("\\dream_editor\\") != std::string_view::npos;
	}

	/** The vendored debugpy directory, which holds pydevd (§5.4.3, RV-10). */
	inline bool IsDebugpyPath(std::string_view Path)
	{
		return Path.find("_dream_vendor/debugpy") != std::string_view::npos || Path.find("_dream_vendor\\debugpy") != std::string_view::npos;
	}

	/** "json/decoder.py:337": the last two path components and the line (§5.4.7). */
	FString ShortFrameName(std::string_view Path, int32 Line)
	{
		FString Name = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Path.data()), static_cast<int32>(Path.size()));
		Name.ReplaceCharInline(TEXT('\\'), TEXT('/'));
		int32 Last = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Last) && Last > 0)
		{
			const int32 Prev = Name.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd, Last);
			if (Prev != INDEX_NONE)
			{
				Name.RightChopInline(Prev + 1);
			}
		}
		return FString::Printf(TEXT("%s:%d"), *Name, Line);
	}

	/** The first line of the last started statement (statement mode, §5.4.7); statement 1 before any started. */
	int32 StartedStatementFirstLine(const FDreamRunRecord& Rec)
	{
		if (Rec.Statements.Num() == 0)
		{
			return 0;
		}
		const int32 Index = FMath::Clamp(Rec.StatementsStarted - 1, 0, Rec.Statements.Num() - 1);
		return Rec.Statements[Index].FirstLine;
	}

	FText RunNumberText(int32 RunNumber)
	{
		return FText::AsNumber(RunNumber, &FNumberFormattingOptions::DefaultNoGrouping());
	}

	struct FPumpDepthScope
	{
		explicit FPumpDepthScope(std::atomic<int32>& InDepth)
			: Depth(InDepth)
		{
			Depth.fetch_add(1);
		}
		~FPumpDepthScope()
		{
			Depth.fetch_sub(1);
		}
		std::atomic<int32>& Depth;
	};
}

std::atomic<FRunSupervisor::FImpl*> FRunSupervisor::FImpl::Instance{nullptr};

FRunSupervisor::FImpl::FImpl(FRunSupervisor& InOwner)
	: Owner(InOwner)
{
	// §5.12: OnUserCancel only on the editor's own feedback context, as it was when the module loaded (PostEngineInit).
	if (GIsEditor && !IsRunningCommandlet())
	{
		EditorWarn = GWarn;
	}
}

FRunSupervisor::FImpl::~FImpl()
{
	StopWatchdog();
}

// =====================================================================================================================
// Public surface
// =====================================================================================================================

FRunSupervisor::FRunSupervisor()
	: Impl(MakeUnique<FImpl>(*this))
{
	FImpl::Instance.store(Impl.Get());
}

FRunSupervisor::~FRunSupervisor()
{
	Shutdown();
	FImpl* Expected = Impl.Get();
	FImpl::Instance.compare_exchange_strong(Expected, nullptr);
}

void FRunSupervisor::Shutdown()
{
	// §3.8 module shutdown: Python is already finalized; touch no Python, join the watchdog.
	Impl->CurrentRunId.store(0);
	Impl->bArmed.store(false);
	Impl->StopWatchdog();
	Impl->Protections.Remove();
	if (Impl->RunWindow.IsValid() && FSlateApplication::IsInitialized())
	{
		SDreamRunWindow::Teardown(Impl->RunWindow);
	}
	Impl->RunWindowContent.Reset();
	SDreamRunWindow::ReleaseKeptChildWindows();
}

void FRunSupervisor::OnPythonInitialized(uint32 Generation)
{
	// §3.5: before any BeginRun of this generation.
	FScopeLock Lock(&Impl->PostLock);
	Impl->Generation.store(Generation);
	Impl->bPythonAlive.store(true);
}

void FRunSupervisor::OnPythonShutdown()
{
	check(IsInGameThread());
	// §3.8 step 2: invalidate the run and disarm; then clear bPythonAlive under PostLock, so no post is in flight.
	Impl->bUserCodeActive.store(false);
	Impl->bArmed.store(false);
	Impl->CurrentRunId.store(0);
	{
		FScopeLock Lock(&Impl->PostLock);
		Impl->bPythonAlive.store(false);
		Impl->bPendingOutstanding.store(false);
		Impl->OutstandingPostId.store(0);
		Impl->bChainedCall.store(false);
	}
	Impl->WakeWatchdog();
#if WITH_PYTHON
	{
		// RV-01: a watchdog blocked in PyEval_RestoreThread needs the GIL to leave; no timeout (§5.4.1).
		FDreamScopedGilRelease NoGil;
		Impl->WaitWhileInPython();
	}
	Impl->DeleteWatchdogThreadState();
#endif
	Impl->CancelClass.store(nullptr);
	Impl->GameThreadIdent.store(0);
	Impl->bHasMark = false;
	Impl->MarkThreadState = nullptr;
}

void FRunSupervisor::BeginRun(FDreamRunId Id, FDreamRunPolicy&& Policy)
{
	check(IsInGameThread());
	if (Impl->RunWindow.IsValid() || Impl->Protections.IsInstalled())
	{
		// A presentation left over from a run whose finish never reached step 3; never stack two modals.
		UE_LOG(LogDream, Warning, TEXT("Run #%d: the previous run's presentation was still up at entry; closing it."), Impl->Policy.RunNumber);
		EndPresentation(Impl->PresentationRunId);
	}

	Impl->ResetRunState();
	Impl->Policy = MoveTemp(Policy);
	if (!UDreamUserSettings::Get()->bRunPump)
	{
		Impl->Policy.Presentation = EDreamRunPresentation::Off;    // run.pump kill switch (§5.5.5, LRS-10)
	}

	const FTCHARToUTF8 Utf8(*Impl->Policy.Filename);
	Impl->FilenameUtf8.SetNumUninitialized(Utf8.Length());
	if (Utf8.Length() > 0)
	{
		FMemory::Memcpy(Impl->FilenameUtf8.GetData(), Utf8.Get(), Utf8.Length());
	}

	const double Now = FPlatformTime::Seconds();
	const bool bOff = Impl->Policy.Presentation == EDreamRunPresentation::Off;
	Impl->bOffPresentation.store(bOff);
	Impl->RunStartSeconds = Now;
	Impl->Live.State = EDreamRunState::Running;
	Impl->PresentationRunId = Id;
	Impl->CurrentRunId.store(Id);
	Impl->ArmAt.store(Now + FMath::Max(0.0, Impl->Policy.ArmDelaySeconds));

	// §5.5.5: Off arms nothing. Without Slate (headless, §5.5.1) there is nothing to pump either.
	if (!bOff && FSlateApplication::IsInitialized())
	{
		Impl->EnsureWatchdog();
		Impl->bArmed.store(Impl->Thread != nullptr);
		Impl->WakeWatchdog();
	}
}

void FRunSupervisor::EndRun(FDreamRunId Id, FDreamRunStats& OutStats)
{
	check(IsInGameThread());
	if (Id != Impl->CurrentRunId.load() && Id != Impl->PresentationRunId)
	{
		UE_LOG(LogDream, Warning, TEXT("EndRun for run id %llu, which is not the supervised run."), Id);
	}

	// §5.7.7 step 1: no GIL held here, so a watchdog inside a hand-off or the §5.7.5 channel finishes at once. The ID
	// is cleared under PostLock, so a post either finished before or sees no run; a hand-off that follows such a post
	// only takes and gives back the GIL, and the §5.7.5 channel re-checks CurrentRunId under the GIL.
	Impl->bUserCodeActive.store(false);
	Impl->bArmed.store(false);
	{
		FScopeLock Lock(&Impl->PostLock);
		Impl->CurrentRunId.store(0);
	}
	Impl->WakeWatchdog();
	Impl->WaitWhileInPython();

#if WITH_PYTHON
	// Step 2: clear a stale async exception and consume any stale pending call of ours now, at a safe point.
	if (Impl->bPythonAlive.load())
	{
		FDreamScopedGil Gil;
		PyThreadState* State = PyThreadState_Get();
		const uint64 GameTid = Impl->GameThreadIdent.load();
		if (GameTid == 0 || FDreamPyCompat::GetThreadId(State) == GameTid)
		{
			FDreamPyCompat::ClearAsyncExc(State, Impl->bAsyncExcBreakerArmed);
		}
		Impl->bAsyncExcBreakerArmed = false;
		if (Py_MakePendingCalls() < 0)
		{
			PyErr_Clear();
		}
	}
#endif
	{
		FScopeLock Lock(&Impl->PostLock);
		Impl->bPendingOutstanding.store(false);
		Impl->OutstandingPostId.store(0);
		Impl->bChainedCall.store(false);
	}

	// Step 3: final drain and the frozen stats.
	if (FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetOutputStore().Drain(DrainBudgetSeconds);
	}
	const double Now = FPlatformTime::Seconds();
	Impl->EndDeferral(Now);
	FDreamRunStats& Stats = Impl->Stats;
	Stats.Deliveries = Impl->Deliveries.load();
	Stats.StopRequestedAt = Impl->StopRequestedAt.load();
	if (Impl->bStopRequested.load() && Stats.StopEffectiveAt == 0.0)
	{
		Stats.StopEffectiveAt = Now;
	}
	{
		// OUT-02: the footer of a cancelled or failed run reports the progress it last reported ("312 / 1,204 completed").
		FScopeLock Lock(&Impl->ProgressLock);
		if (Impl->ProgressRunId == Id && Impl->ProgressDone >= 0)
		{
			Stats.ProgressDone = Impl->ProgressDone;
			Stats.ProgressTotal = Impl->ProgressTotal;
		}
	}
	// The entry counters belong to FDreamRunService (RT-07, §6.3); keep what it already wrote.
	const int32 LogOutputEntries = OutStats.LogOutputEntries;
	const int32 BypassLogPythonLines = OutStats.BypassLogPythonLines;
	OutStats = Stats;
	OutStats.LogOutputEntries = LogOutputEntries;
	OutStats.BypassLogPythonLines = BypassLogPythonLines;
}

bool FRunSupervisor::HasPendingAsyncExcForTest() const
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (Impl->bPythonAlive.load())
	{
		FDreamScopedGil Gil;
		return FDreamPyCompat::HasAsyncExc(PyThreadState_Get());
	}
#endif
	return false;
}

int32 FRunSupervisor::GetEmptyAsyncExcClearsForTest() const
{
#if WITH_PYTHON
	return FDreamPyCompat::GetEmptyNullCallsForTest();
#else
	return 0;
#endif
}

int32 FRunSupervisor::GetAsyncExcBreakerDropsForTest() const
{
#if WITH_PYTHON
	return FDreamPyCompat::GetBreakerDropsForTest();
#else
	return 0;
#endif
}

bool FRunSupervisor::ArmAsyncExcBreakerForTest()
{
	check(IsInGameThread());
#if WITH_PYTHON
	if (Impl->bPythonAlive.load() && Impl->CurrentRunId.load() == 0)
	{
		// What user_code_done leaves after it nulled a pending exception: the breaker armed, async_exc empty, the flag set.
		FDreamScopedGil Gil;
		PyThreadState* State = PyThreadState_Get();
		if (!FDreamPyCompat::HasAsyncExc(State))
		{
			FDreamPyCompat::SetAsyncExc(FDreamPyCompat::GetThreadId(State), nullptr);
			Impl->bAsyncExcBreakerArmed = true;
			return true;
		}
	}
#endif
	return false;
}

// (tests) The PostSupervisorCall fixture's hook (§14.2); see the header.
bool FRunSupervisor::PostPendingCallForTest(FDreamRunId Id)
{
#if WITH_PYTHON
	const FDreamRunId Target = Id != 0 ? Id : Impl->CurrentRunId.load();
	return Target != 0 && FDreamPyCompat::AddPendingCall(&FImpl::PendingCallTrampoline, DreamSupervisorPrivate::IdToArg(Target)) == 0;
#else
	return false;
#endif
}

void FRunSupervisor::EndPresentation(FDreamRunId Id)
{
	check(IsInGameThread());
	if (Impl->PresentationRunId == 0 && !Impl->RunWindow.IsValid() && !Impl->Protections.IsInstalled())
	{
		return;
	}
	if (Id != Impl->PresentationRunId)
	{
		UE_LOG(LogDream, Verbose, TEXT("EndPresentation for run id %llu; the presentation belongs to %llu."), Id, Impl->PresentationRunId);
	}

	const bool bHadUi = Impl->bPresentationStarted || Impl->RunWindow.IsValid();

	// Finish step 3 (§5.5.2, §5.5.3): protections off, the live view handed over, then teardown steps 1-4.
	Impl->Protections.Remove();
	Impl->HandOffLiveView();
	const bool bHadWindow = Impl->RunWindow.IsValid();
	bool bWindowWasForeground = false;
	if (bHadWindow)
	{
		bWindowWasForeground = SDreamRunWindow::Teardown(Impl->RunWindow);
	}
	Impl->RunWindowContent.Reset();

	// §5.5.2: focus returns to the widget that had it before, if it still exists. After a Run window, only when that window
	// held the OS foreground at teardown (review r6 m3): Slate focus on an inactive editor window activates it, which would
	// push a tool window the run made active (an unparented PySide window) behind the editor. The editor window restores
	// the focus it had (WidgetFocusedOnDeactivate) whenever it is activated again.
	if (bHadUi && FSlateApplication::IsInitialized() && (!bHadWindow || bWindowWasForeground))
	{
		if (const TSharedPtr<SWidget> Focus = Impl->Policy.FocusToRestore.Pin())
		{
			FSlateApplication::Get().SetKeyboardFocus(Focus, EFocusCause::SetDirectly);
		}
	}

	Impl->Live = FDreamRunLiveStats();
	Impl->Policy = FDreamRunPolicy();
	Impl->PresentationRunId = 0;
	Impl->bPresentationStarted = false;
	Impl->NotesSignature = 0;
}

bool FRunSupervisor::UserCodeBegin(FDreamRunId Id)
{
	check(IsInGameThread());
	if (Id == 0 || Id != Impl->CurrentRunId.load())
	{
		UE_LOG(LogDream, Warning, TEXT("user_code_begin for run id %llu, which is not the supervised run."), Id);
		return true;
	}
	// A run may open a second, short window after its first user_code_done: the runner calls a studio's replaced
	// sys.excepthook inside one so Stop works there (§5.3.7). Each window takes a fresh entry mark and reopens delivery
	// and the pump; the run's stats, its Stop state and its delivery clocks carry on unchanged.
	const bool bFirstWindow = Impl->UserCodeWindows++ == 0;
#if WITH_PYTHON
	// §2.6.3: the window's re-entry mark; the game thread's ident and the cancel class for the §5.7.5 channel.
	const FDreamEntryMark Mark = FDreamPyCompat::CaptureEntryMark();
	Impl->MarkThreadState = Mark.ThreadState;
	Impl->MarkGilCounter = Mark.GilCounter;
	Impl->MarkCFrame = Mark.CFrame;
	Impl->bHasMark = Mark.ThreadState != nullptr;
	Impl->GameThreadIdent.store(Mark.ThreadState ? FDreamPyCompat::GetThreadId(Mark.ThreadState) : 0);
	Impl->CancelClass.store(DreamNative::GetCancelClass());
#endif
	if (Impl->bStopRequested.load())
	{
		// Stop came first. In the first window the runner raises ScriptCancelled itself (DreamNativeModule.h), which counts
		// as a delivery; a later window is simply not opened (the runner skips the hook), so nothing is delivered.
		if (bFirstWindow)
		{
			Impl->CountDelivery(FPlatformTime::Seconds());
		}
		return false;
	}
	Impl->bUserCodeActive.store(true);
	return true;
}

void FRunSupervisor::UserCodeDone(FDreamRunId Id)
{
	check(IsInGameThread());
	// RT-06: delivery off and new posts stopped at once, without waiting for the watchdog (GIL-6); idempotent.
	Impl->bUserCodeActive.store(false);
#if WITH_PYTHON
	// §5.7.5: a stale ScriptCancelled must not reach the runner's handler. Only a pending one is nulled: a null call on an
	// empty state would arm the interpreter-wide eval breaker for the rest of the session (3.11). Nulling a real one also
	// leaves the breaker armed until a delivery, so EndRun step 2 drops it at a safe point (ClearAsyncExc).
	if (PyThreadState* State = PyThreadState_Get(); FDreamPyCompat::HasAsyncExc(State))
	{
		FDreamPyCompat::SetAsyncExc(FDreamPyCompat::GetThreadId(State), nullptr);
		Impl->bAsyncExcBreakerArmed = true;
	}
#endif
	if (Id == Impl->CurrentRunId.load() && Impl->bStopRequested.load() && Impl->Stats.StopEffectiveAt == 0.0)
	{
		Impl->Stats.StopEffectiveAt = FPlatformTime::Seconds();
	}
}

bool FRunSupervisor::RaiseCancelIfStopRequested(FDreamRunId Id)
{
	check(IsInGameThread());
	if (Id == 0 || Id != Impl->CurrentRunId.load() || !Impl->bStopRequested.load())
	{
		return false;
	}
	// run_statement step 1 (§5.3.7): a new instance, counted as a delivery.
	return Impl->RaiseCancel(FPlatformTime::Seconds(), static_cast<EDreamStopSource>(Impl->StopSource.load()));
}

void FRunSupervisor::RequestStop(FDreamRunId Id, EDreamStopSource Source)
{
	// §5.5.5: no Stop under the Off presentation.
	if (Id == 0 || Id != Impl->CurrentRunId.load() || Impl->bOffPresentation.load())
	{
		return;
	}
	if (!Impl->bStopRequested.load())
	{
		Impl->StopSource.store(static_cast<uint8>(Source));
		Impl->StopRequestedAt.store(FPlatformTime::Seconds());
		Impl->bStopRequested.store(true);
		Impl->WakeWatchdog();
	}
	if (IsInGameThread())
	{
		Impl->ApplyStopOnGameThread();
	}
	else
	{
		Impl->bStopUiPending.store(true);
	}
	// (debug, PO-06 F5) While paused, a run-level Stop also ends the pause with `terminate` (§8.3.3), so Shift+F5 through
	// the input pre-processor and an API Stop work. Deferred (M3): RequestStop(EvalId) for supervised evaluates (§5.7.2).
	if (IsInGameThread() && Impl->bPaused.load())
	{
		FDreamDebugger::Get().OnRunStopRequested();
	}
}

void FRunSupervisor::SetPaused(FDreamRunId Id, bool bPaused)
{
	check(IsInGameThread());
	if (Id == 0 || Id != Impl->CurrentRunId.load())
	{
		return;
	}
	Impl->bPaused.store(bPaused);
	if (bPaused)
	{
		Impl->bArmed.store(false);
		Impl->Live.State = EDreamRunState::Paused;
	}
	else
	{
		Impl->Live.State = Impl->bStopRequested.load() ? EDreamRunState::Stopping : EDreamRunState::Running;
		if (!Impl->bOffPresentation.load() && FSlateApplication::IsInitialized())
		{
			Impl->ArmAt.store(FPlatformTime::Seconds() + FMath::Max(0.0, Impl->Policy.ArmDelaySeconds));
			Impl->bArmed.store(Impl->Thread != nullptr);
			Impl->WakeWatchdog();
		}
	}
}

bool FRunSupervisor::IsPaused() const
{
	return Impl->bPaused.load();
}

void FRunSupervisor::RequestInterruptCleanup(FDreamRunId Id)
{
	// RT-06 explicit choice: one delivery at the next pending call, regardless of the handling check (§5.7.6).
	if (Id != 0 && Id == Impl->CurrentRunId.load() && Impl->bStopRequested.load() && Impl->Deliveries.load() > 0)
	{
		Impl->bInterruptCleanup.store(true);
	}
}

bool FRunSupervisor::IsStopRequested(FDreamRunId Id) const
{
	return Id != 0 && Id == Impl->CurrentRunId.load() && Impl->bStopRequested.load();
}

bool FRunSupervisor::IsStopPendingForCurrent() const
{
	// §5.10.2: False under the Off presentation.
	return Impl->CurrentRunId.load() != 0 && Impl->bStopRequested.load() && !Impl->bOffPresentation.load();
}

bool FRunSupervisor::CheckCancel(bool bDeliver)
{
	const bool bPending = IsStopPendingForCurrent();
	if (!bDeliver || !bPending || !IsInGameThread())
	{
		return bPending;    // a worker decides for itself (§5.10.2); a plain check never counts a delivery
	}
	// §5.10.2: dream_editor.check_cancel() raises ScriptCancelled only where a delivery may land: inside the user-code
	// window, not from nested foreign Python inside our pump (§5.4.8), and with no §5.4.3 deferral condition. Otherwise
	// it returns True so the caller can return early (the deferral row, A19).
	if (Impl->PumpDepth.load() > 0)
	{
		return false;
	}
	EDreamDeferReason Reason = EDreamDeferReason::Count;
	if (Impl->FindDeferral(Reason))
	{
		return false;       // OutsideUserCode, SavingPackage, GarbageCollecting, RoutingPostLoad or NativeReentry
	}
	// The raise counts as a delivery, so the 250 ms re-delivery and the Interrupt cleanup clocks start from it.
	Impl->CountDelivery(FPlatformTime::Seconds());
	Impl->DeliveryLine = Impl->Live.Line;     // the swallow report's starting point (§5.7.4), as RaiseCancel keeps it
	return true;
}

void FRunSupervisor::SetProgress(FDreamRunId Id, int64 Done, TOptional<int64> Total, FStringView Message)
{
	if (Id == 0 || Id != Impl->CurrentRunId.load())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	FScopeLock Lock(&Impl->ProgressLock);
	Impl->ProgressRunId = Id;
	Impl->ProgressDone = Done;
	Impl->ProgressTotal = Total;
	if (!Message.Equals(Impl->ProgressMessage, ESearchCase::CaseSensitive))
	{
		Impl->ProgressMessage = FString(Message);
	}
	++Impl->ProgressSerial;
	// §5.10.1: a 16-entry ring, at most one sample every 100 ms, so a 1M-call loop stays cheap.
	if (Impl->ProgressRingCount == 0 || Now - Impl->ProgressLastRingAt >= 0.1)
	{
		Impl->ProgressRing[Impl->ProgressRingHead] = FImpl::FProgressSample{Now, Done};
		Impl->ProgressRingHead = (Impl->ProgressRingHead + 1) % FImpl::ProgressRingSize;
		Impl->ProgressRingCount = FMath::Min(Impl->ProgressRingCount + 1, FImpl::ProgressRingSize);
		Impl->ProgressLastRingAt = Now;
	}
}

int32 FRunSupervisor::GetPumpDepth() const
{
	return Impl->PumpDepth.load();
}

const FDreamRunLiveStats& FRunSupervisor::GetLiveStats() const
{
	return Impl->Live;
}

bool FRunSupervisor::IsRunWindowOpen() const
{
	return Impl->RunWindow.IsValid();
}

TSharedPtr<SDreamOutputView> FRunSupervisor::GetLiveRunViewForTest() const
{
	return Impl->RunWindowContent.IsValid() ? Impl->RunWindowContent->GetLiveRunView() : nullptr;
}

TSharedPtr<FDreamInputPreProcessor> FRunSupervisor::GetInputPreProcessorForTest() const
{
	return Impl->Protections.IsInstalled() ? Impl->Protections.GetPreProcessor() : nullptr;
}

FDreamRunId FRunSupervisor::GetCurrentRunId() const
{
	return Impl->CurrentRunId.load();
}

// =====================================================================================================================
// Run state
// =====================================================================================================================

void FRunSupervisor::FImpl::ResetRunState()
{
	Stats = FDreamRunStats();
	Live = FDreamRunLiveStats();
	bUserCodeActive.store(false);
	bPaused.store(false);       // (debug)
	bStopRequested.store(false);
	StopSource.store(0);
	bInterruptCleanup.store(false);
	Deliveries.store(0);
	StopRequestedAt.store(0.0);
	FirstDeliveryAt.store(0.0);
	LastDeliveryAt.store(0.0);
	LastCheckAt.store(0.0);
	bLastCheckHandling.store(false);
	bLastAddFailed.store(false);
	bStopUiPending.store(false);
	bLastDebugpyInnermost.store(false);
	bHasMark = false;
	MarkThreadState = nullptr;
	UserCodeWindows = 0;
	LastIntervalSeconds = 0.0;
	LastIntervalLine = 0;
	PostTime.store(0.0);
	HandOffWait.store(0.0);
	PeriodSeconds.store(1.0 / DreamSupervisorPrivate::TargetHz);
	AddFailures.store(0);
	CostEwma = 0.0;
	bHaveCost = false;
	PumpsSinceNative = -1;
	NativeEndedAt = 0.0;
	DeferStartAt = 0.0;
	DeliveryLine = 0;
	NotesSignature = 0;
	bPresentationStarted = false;
	ProgressCopiedSerial = 0;
	{
		FScopeLock Lock(&ProgressLock);
		ProgressRunId = 0;
		ProgressDone = -1;
		ProgressTotal.Reset();
		ProgressMessage.Reset();
		ProgressSerial = 0;
		ProgressRingCount = 0;
		ProgressRingHead = 0;
		ProgressLastRingAt = 0.0;
	}
}

const FDreamRunRecord* FRunSupervisor::FImpl::FindActiveRecord() const
{
	if (!FDreamEditorModule::IsAvailable())
	{
		return nullptr;
	}
	const FDreamRunRecord* Rec = FDreamEditorModule::Get().GetRunService().GetActiveRecord();
	return (Rec && Rec->RunNumber == Policy.RunNumber) ? Rec : nullptr;
}

EDreamRunPresentation FRunSupervisor::FImpl::EffectivePresentation() const
{
	// A28: Live (docked) Preview is M4; slice A runs it as the Run window.
	return Policy.Presentation == EDreamRunPresentation::Off ? EDreamRunPresentation::Off : EDreamRunPresentation::RunWindow;
}

void FRunSupervisor::FImpl::RecordStopDuringNative(double Seconds, int32 Line)
{
	// "Stop took effect when the native call at line 43 returned (12.4 s)" (§5.8): the first such interval of the run.
	if (!Stats.bStopDuringNative)
	{
		Stats.bStopDuringNative = true;
		Stats.StopNativeSeconds = Seconds;
		Stats.StopNativeLine = Line;
	}
}

void FRunSupervisor::FImpl::CountDelivery(double Now)
{
	LastDeliveryAt.store(Now);
	if (Deliveries.fetch_add(1) == 0)
	{
		FirstDeliveryAt.store(Now);
	}
}

void FRunSupervisor::FImpl::ApplyStopOnGameThread()
{
	check(IsInGameThread());
	Live.State = EDreamRunState::Stopping;
	// The Stop click or key waited in the OS queue for the native call that just ended (§5.8): Stop was first requested,
	// from input (an API Stop never queues there), in the first pump after the interval (StopAfterNativePumps).
	const bool bFromInput = static_cast<EDreamStopSource>(StopSource.load()) != EDreamStopSource::Api;
	if (bFromInput && PumpsSinceNative >= 0 && PumpsSinceNative < DreamSupervisorPrivate::StopAfterNativePumps
		&& StopRequestedAt.load() >= NativeEndedAt)
	{
		RecordStopDuringNative(LastIntervalSeconds, LastIntervalLine);
	}

	// §5.12: a user slow task's should_cancel() turns true, but only on the editor's own feedback context.
	const bool bSlowTaskOpen = GWarn != nullptr && GWarn->GetScopeStack().Num() > 0;
	if (bSlowTaskOpen && EditorWarn != nullptr && GWarn == EditorWarn)
	{
		static_cast<FFeedbackContextEditor*>(EditorWarn)->OnUserCancel();
	}

	// §5.12: inside a script's blocking dialog our pump does not run; say when Stop will act. Dream never closes it.
	if (!bSlowTaskOpen && PumpDepth.load() == 0 && !bPaused.load() && FSlateApplication::IsInitialized())   // (debug) the Paused window closes itself
	{
		const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
		if (Modal.IsValid() && Modal != RunWindow)
		{
			FNotificationInfo Info(LOCTEXT("Supervisor.StopInDialog", "Stop requested. It takes effect when this dialog closes."));
			Info.ExpireDuration = 5.f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}
}

// =====================================================================================================================
// The pending call (§5.4.2)
// =====================================================================================================================

int FRunSupervisor::FImpl::PendingCallTrampoline(void* Arg)
{
	FImpl* Self = Instance.load();
	const FDreamRunId Id = static_cast<FDreamRunId>(reinterpret_cast<UPTRINT>(Arg));
	return Self ? Self->OnPendingCall(Id) : 0;
}

void FRunSupervisor::FImpl::ClearOutstanding(FDreamRunId Id)
{
	FDreamRunId Expected = Id;
	if (OutstandingPostId.compare_exchange_strong(Expected, 0))
	{
		bChainedCall.store(false);
		bPendingOutstanding.store(false);
	}
}

bool FRunSupervisor::FImpl::RequeueFromGameThread(FDreamRunId Id)
{
#if WITH_PYTHON
	// §5.7.3 fast chain: the re-queued call is the one outstanding call; CPython runs it at the next eval-breaker check,
	// because a callback that returned -1 leaves the pending-call signal set (T-CAN-012).
	bChainedCall.store(true);
	if (FDreamPyCompat::AddPendingCall(&PendingCallTrampoline, DreamSupervisorPrivate::IdToArg(Id)) == 0)
	{
		return true;
	}
	bChainedCall.store(false);
#endif
	return false;
}

int FRunSupervisor::FImpl::OnPendingCall(FDreamRunId Id)
{
	// The flag is cleared when the callback exits, not when it is entered (§5.4.1); the fast chain keeps it.
	bool bKeepOutstanding = false;
	const bool bChained = OutstandingPostId.load() == Id && bChainedCall.exchange(false);
	ON_SCOPE_EXIT
	{
		if (!bKeepOutstanding)
		{
			ClearOutstanding(Id);
		}
	};

	// 1. Run-ID guard: a stale call of an earlier run or generation does nothing.
	if (Id == 0 || Id != CurrentRunId.load() || !IsInGameThread())
	{
		return 0;
	}
	// 1a. (debug, PO-06 F5) The Paused gate (§8.3.1 step 2): while the Paused window is open, nothing pumps or cancels.
	// Deferred (M3): the supervised-evaluate ID guard (§8.3.4).
	if (bPaused.load())
	{
		return 0;
	}

	// 3. Re-entrancy: never inside our own pump (CPython also refuses recursive pending calls, T-SUP-012). Checked
	// before the bookkeeping here because a call inside the pump would walk nested foreign frames into the live stats.
	if (PumpDepth.load() > 0)
	{
		return 0;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(Dream.Run.PendingCall, DreamRunChannel);   // §13.1 timing scope
	const double Now = FPlatformTime::Seconds();

	// 2. Bookkeeping: the line at this pump, live stats, the native interval (§5.4.7, §5.8).
	if (bStopUiPending.exchange(false))
	{
		ApplyStopOnGameThread();
	}
	const FFrameReport Frame = WalkFrames();
	UpdateLiveStats(Now, Frame);
	if (!bChained)
	{
		MeasureNativeInterval(Now);
	}

	// 4. Drain output (D22: 2 ms), safe in every state.
	int32 LinesMoved = 0;
	if (FDreamEditorModule::IsAvailable())
	{
		LinesMoved = FDreamEditorModule::Get().GetOutputStore().Drain(DrainBudgetSeconds);
	}

	// 5. Deferral (RT-02, RV-05): no pump and no delivery; the watchdog posts again next period.
	EDreamDeferReason Reason = EDreamDeferReason::Count;
	if (FindDeferral(Reason))
	{
		RecordDeferral(Reason, Now);
		return 0;
	}
	EndDeferral(Now);
	if (Frame.bDebugpy)
	{
		// §5.4.3: pydevd holds the game thread; keep pumping, never deliver (DeliverIfDue checks the same flag). The Run
		// window's "Paused in VS Code" note reads Live.bDebugpyPaused (UpdateLiveStats), not a deferral reason.
		++Stats.Deferrals[static_cast<int32>(EDreamDeferReason::DebugpyFrame)];
	}

	// 6. Cancellation, without pumping. A fast-chain call re-delivers at once when the last delivery was swallowed (§5.7.3).
	if (DeliverIfDue(Now, Frame, bChained))
	{
		bKeepOutstanding = RequeueFromGameThread(Id);
		return -1;
	}

	// 7. Transaction size watch and the WS-07 snapshot (FDreamRunService's hook, §5.9.4).
	if (Policy.OnPump)
	{
		Policy.OnPump(Live);
	}

	// 8. Pump, unless the presentation is Off.
	if (!bOffPresentation.load())
	{
		Pump(LinesMoved > 0);
	}

	// 9. A Stop processed during this pump is delivered now: latency is one pump, not one more period.
	if (Id == CurrentRunId.load() && DeliverIfDue(FPlatformTime::Seconds(), Frame))
	{
		bKeepOutstanding = RequeueFromGameThread(Id);
		return -1;
	}
	return 0;
}

// =====================================================================================================================
// Live stats (§5.4.7) and native intervals (§5.8)
// =====================================================================================================================

FRunSupervisor::FImpl::FFrameReport FRunSupervisor::FImpl::WalkFrames() const
{
	using namespace DreamSupervisorPrivate;
	FFrameReport Report;
#if WITH_PYTHON
	PyFrameObject* Frame = PyEval_GetFrame();                // borrowed
	Py_XINCREF(Frame);
	for (int32 Depth = 0; Frame != nullptr && Depth < MaxFrames; ++Depth)
	{
		PyCodeObject* Code = PyFrame_GetCode(Frame);          // new reference
		PyObject* FileObject = PyObject_GetAttrString(reinterpret_cast<PyObject*>(Code), "co_filename");
		Py_DECREF(Code);
		Py_ssize_t Length = 0;
		const char* Utf8 = FileObject ? PyUnicode_AsUTF8AndSize(FileObject, &Length) : nullptr;
		if (Utf8 == nullptr)
		{
			PyErr_Clear();
		}
		else
		{
			const std::string_view Path(Utf8, static_cast<size_t>(Length));
			if (!IsDreamPackagePath(Path))
			{
				const int32 Line = PyFrame_GetLineNumber(Frame);
				const bool bRunFile = Length == FilenameUtf8.Num()
					&& (Length == 0 || FMemory::Memcmp(Utf8, FilenameUtf8.GetData(), static_cast<SIZE_T>(Length)) == 0);
				if (!Report.bHaveInnermost)
				{
					Report.bHaveInnermost = true;
					Report.bInnermostIsRun = bRunFile;
					Report.bDebugpy = IsDebugpyPath(Path);
					if (!bRunFile)
					{
						Report.InnermostName = ShortFrameName(Path, Line);
					}
				}
				if (bRunFile)
				{
					Report.bFoundRunFrame = true;
					Report.RunLine = Line;
				}
			}
		}
		Py_XDECREF(FileObject);
		if (Report.bFoundRunFrame)
		{
			break;
		}
		PyFrameObject* Back = PyFrame_GetBack(Frame);         // new reference
		Py_DECREF(Frame);
		Frame = Back;
	}
	Py_XDECREF(Frame);
#endif
	return Report;
}

void FRunSupervisor::FImpl::UpdateLiveStats(double Now, const FFrameReport& Frame)
{
	using namespace DreamSupervisorPrivate;
	Live.ElapsedSeconds = Now - RunStartSeconds;

	const FDreamRunRecord* Rec = FindActiveRecord();
	const bool bStatementMode = Rec != nullptr && Rec->bStatementMode && Rec->Statements.Num() > 0;
	if (Frame.bFoundRunFrame)
	{
		// A statement's code object at its first instruction reports line 0: show that statement's first line.
		Live.Line = (Frame.RunLine <= 0 && bStatementMode) ? StartedStatementFirstLine(*Rec) : Frame.RunLine;
		Live.bBetweenStatements = false;
	}
	else if (bStatementMode)
	{
		// The call ran in the runner between two statements: the row shows the last started statement's range.
		Live.Line = StartedStatementFirstLine(*Rec);
		Live.bBetweenStatements = true;
	}
	Live.InnermostFrame = (Frame.bHaveInnermost && !Frame.bInnermostIsRun) ? Frame.InnermostName : FString();
	Live.bDebugpyPaused = Frame.bDebugpy;
	bLastDebugpyInnermost.store(Frame.bDebugpy);

	CopyProgress();
	CopySlowTask();

	// RT-06: Interrupt cleanup is offered once the run is still alive this long after the first delivery.
	const int32 DeliveryCount = Deliveries.load();
	Live.bShowInterruptCleanup = bStopRequested.load() && DeliveryCount > 0
		&& Now - FirstDeliveryAt.load() >= Policy.CleanupInterruptSeconds;
	if (Live.bCleanupRunning && DeliveryCount > 0)
	{
		Live.CleanupSeconds = Now - FirstDeliveryAt.load();
	}
}

void FRunSupervisor::FImpl::MeasureNativeInterval(double Now)
{
	// RunTime - PostTime - HandOffWait: the hand-off is never reported as native time (RV-01).
	const double Posted = PostTime.exchange(0.0);
	if (Posted <= 0.0)
	{
		return;
	}
	const double Gap = Now - Posted - HandOffWait.load();
	if (Gap < NativeIntervalSeconds)
	{
		return;
	}
	// The line is the frame at the call that ends the interval: the line of the call that returned.
	const int32 Line = Live.Line;
	Stats.NativeSeconds += Gap;
	if (Gap > Stats.LongestNativeSeconds)
	{
		Stats.LongestNativeSeconds = Gap;
		Stats.LongestNativeLine = Line;
	}
	LastIntervalSeconds = Gap;
	LastIntervalLine = Line;
	if (bStopRequested.load() && StopRequestedAt.load() >= Posted)
	{
		RecordStopDuringNative(Gap, Line);
	}
	PumpsSinceNative = 0;
	NativeEndedAt = Now;
	if (Gap > DreamSupervisorPrivate::NativeNoteSeconds)
	{
		Live.LastNativeSeconds = Gap;
		Live.LastNativeLine = Line;
	}
}

void FRunSupervisor::FImpl::CopyProgress()
{
	FScopeLock Lock(&ProgressLock);
	if (ProgressSerial == ProgressCopiedSerial)
	{
		return;
	}
	ProgressCopiedSerial = ProgressSerial;
	Live.ProgressDone = ProgressDone;
	Live.ProgressTotal = ProgressTotal;
	Live.ProgressMessage = ProgressMessage;
	Live.ProgressSamples = ProgressRingCount;
	Live.ProgressRate = 0.0;
	Live.ProgressEtaSeconds = -1.0;

	// §5.10.1: rate = Δi/Δt over the last 10 samples, shown with the ETA after 3 samples (U12).
	constexpr int32 RingSize = ProgressRingSize;
	if (ProgressRingCount >= 3)
	{
		const int32 Span = FMath::Min(ProgressRingCount, 10);
		const FProgressSample& Newest = ProgressRing[(ProgressRingHead - 1 + RingSize) % RingSize];
		const FProgressSample& Oldest = ProgressRing[(ProgressRingHead - Span + RingSize) % RingSize];
		const double Dt = Newest.Time - Oldest.Time;
		if (Dt > 0.0)
		{
			Live.ProgressRate = static_cast<double>(Newest.Done - Oldest.Done) / Dt;
			if (ProgressTotal.IsSet() && Live.ProgressRate > 0.0)
			{
				Live.ProgressEtaSeconds = FMath::Max(0.0, static_cast<double>(ProgressTotal.GetValue() - ProgressDone) / Live.ProgressRate);
			}
		}
	}
}

void FRunSupervisor::FImpl::CopySlowTask()
{
	// LRS-04: the user's slow-task progress (SlowTaskStack.h:14, SlowTask.h:147).
	Live.SlowTaskFraction = -1.f;
	Live.SlowTaskMessage = FText::GetEmpty();
	if (GWarn == nullptr)
	{
		return;
	}
	const FSlowTaskStack& Stack = GWarn->GetScopeStack();
	if (Stack.Num() > 0)
	{
		Live.SlowTaskFraction = Stack.GetProgressFraction(0);
		if (const FSlowTask* Top = Stack.Last())
		{
			Live.SlowTaskMessage = Top->GetCurrentMessage();
		}
	}
}

// =====================================================================================================================
// Guards (§5.4.3)
// =====================================================================================================================

bool FRunSupervisor::FImpl::FindDeferral(EDreamDeferReason& OutReason)
{
	if (UE::IsSavingPackage())
	{
		OutReason = EDreamDeferReason::SavingPackage;
		return true;
	}
	if (IsGarbageCollecting())
	{
		OutReason = EDreamDeferReason::GarbageCollecting;
		return true;
	}
	if (FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		OutReason = EDreamDeferReason::RoutingPostLoad;
		return true;
	}
	// The pump gate (RV-05): Python after user_code_done in the same entry, such as reinstancing (§5.3.10), never pumps.
	if (!bUserCodeActive.load())
	{
		OutReason = EDreamDeferReason::OutsideUserCode;
		return true;
	}
#if WITH_PYTHON
	bool bReentered = false;
	if (bHasMark)
	{
		FDreamEntryMark Mark;
		Mark.ThreadState = MarkThreadState;
		Mark.GilCounter = MarkGilCounter;
		Mark.CFrame = MarkCFrame;
		bReentered = FDreamPyCompat::IsNativeReentered(Mark);
	}
	if (bReentered)
	{
		OutReason = EDreamDeferReason::NativeReentry;
		return true;
	}
#endif
	return false;
}

void FRunSupervisor::FImpl::RecordDeferral(EDreamDeferReason Reason, double Now)
{
	++Stats.Deferrals[static_cast<int32>(Reason)];
	Live.LastDeferral = Reason;
	if (DeferStartAt <= 0.0)
	{
		DeferStartAt = Now;
	}
	int32 Total = 0;
	for (const int32 Count : Stats.Deferrals)
	{
		Total += Count;
	}
	TRACE_COUNTER_SET(DreamRunDeferrals, Total);
	DreamTrace::TraceDeferral(CurrentRunId.load(), static_cast<uint8>(Reason));
}

void FRunSupervisor::FImpl::EndDeferral(double Now)
{
	// §5.8: deferral time is summed separately; the footer reports long deferrals.
	if (DeferStartAt > 0.0)
	{
		Stats.DeferralSeconds += FMath::Max(0.0, Now - DeferStartAt);
		DeferStartAt = 0.0;
	}
	Live.LastDeferral = EDreamDeferReason::Count;
}

// =====================================================================================================================
// Cancellation (§5.7.3, §5.7.4)
// =====================================================================================================================

bool FRunSupervisor::FImpl::DeliverIfDue(double Now, const FFrameReport& Frame, bool bChained)
{
	if (!bStopRequested.load() || !bUserCodeActive.load() || Frame.bDebugpy)
	{
		return false;
	}
	bool bAnyHandled = false;
	const bool bHandling = IsHandlingOurs(&bAnyHandled);
	LastCheckAt.store(Now);
	bLastCheckHandling.store(bHandling);

	const bool bFirst = Deliveries.load() == 0;
	const bool bInterrupt = bInterruptCleanup.exchange(false);
	// Swallowed (D2): a watchdog post re-delivers 250 ms after the last delivery. A call the fast chain re-queued is due at
	// once only when no exception at all is being handled: the delivery before it was swallowed and this check lies
	// outside any handler, typically the loop's back-edge, where the re-delivery propagates (§5.7.3 fast chain, §5.7.8
	// "delivery 2 lands outside the protected range"; T-CAN-003). Delivering re-queues the chain again. A check that finds
	// ours handled ends it (RT-06). A foreign handled exception keeps the 250 ms rule: cleanup a finalizer runs while ours
	// still propagates (a suspended generator freed during the unwind runs its finally with GeneratorExit handled and no
	// __context__) cannot see ours, and must not be interrupted (RT-06, review r1 run-M1).
	const bool bRedeliver = !bHandling && ((bChained && !bAnyHandled) || Now - LastDeliveryAt.load() >= RedeliverySeconds);
	if (bFirst || bInterrupt || bRedeliver)
	{
		const EDreamStopSource Source = bInterrupt ? EDreamStopSource::InterruptCleanup : static_cast<EDreamStopSource>(StopSource.load());
		if (RaiseCancel(Now, Source))
		{
			return true;
		}
	}
	UpdateHandlingReport(Now, Frame, bHandling);
	return false;
}

bool FRunSupervisor::FImpl::RaiseCancel(double Now, EDreamStopSource Source)
{
#if WITH_PYTHON
	const int32 Delivery = Deliveries.load() + 1;
	PyObject* Exc = DreamNative::NewCancelledInstance(Policy.RunNumber, DreamRunText::StopSourceText(Source), Delivery);
	if (Exc == nullptr)
	{
		PyErr_Clear();
		UE_LOG(LogDream, Error, TEXT("Run #%d: ScriptCancelled could not be created (dream_editor not booted?); Stop was not delivered."), Policy.RunNumber);
		return false;
	}
	// A callback returning -1 must set an error first (07a Q1).
	PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(Exc)), Exc);
	Py_DECREF(Exc);
	CountDelivery(Now);
	DeliveryLine = Live.Line;
	return true;
#else
	return false;
#endif
}

bool FRunSupervisor::FImpl::IsHandlingOurs(bool* bOutAnyHandled) const
{
	if (bOutAnyHandled != nullptr)
	{
		*bOutAnyHandled = false;
	}
#if WITH_PYTHON
	PyObject* Raw = FDreamPyCompat::GetHandledException();    // new reference or null (pyerrors.h:22)
	if (Raw == nullptr)
	{
		return false;
	}
	if (bOutAnyHandled != nullptr)
	{
		*bOutAnyHandled = true;
	}
	const FDreamPyRef Handled = FDreamPyRef::Steal(Raw);        // GIL-7
	return DreamNative::IsOurCancel(Handled.Get(), Policy.RunNumber);
#else
	return false;
#endif
}

void FRunSupervisor::FImpl::UpdateHandlingReport(double Now, const FFrameReport& Frame, bool bHandling)
{
	if (Deliveries.load() == 0)
	{
		return;
	}
	const FDreamRunRecord* Rec = FindActiveRecord();
	const dream::TextSnapshot* Source = (Rec && DreamRunSource::HasSource(Rec->SourceSnapshot)) ? &Rec->SourceSnapshot : nullptr;

	if (bHandling)
	{
		DreamRunSource::EHandlerKind Kind = DreamRunSource::EHandlerKind::None;
		int32 HeaderLine = 0;
		if (Source != nullptr && Frame.bFoundRunFrame && Live.Line > 0)
		{
			HeaderLine = DreamRunSource::FindEnclosingHandler(*Source, Live.Line, Kind);
		}
		if (Kind == DreamRunSource::EHandlerKind::Except)
		{
			// An except clause is handling it: the swallow report (§5.7.4), not the cleanup timer.
			Live.SwallowLine = HeaderLine;
			Live.bCleanupRunning = false;
		}
		else
		{
			// finally / __exit__: "Stopping… cleanup is running (finally at line 88), 4 s" (§5.8).
			Live.bCleanupRunning = true;
			Live.CleanupLine = HeaderLine > 0 ? HeaderLine : Live.Line;
			Live.CleanupSeconds = Now - FirstDeliveryAt.load();
		}
		return;
	}

	// Not handling after a delivery: the script caught Stop and went on; Dream keeps re-delivering (LRS-14).
	Live.bCleanupRunning = false;
	if (Source != nullptr && DeliveryLine > 0)
	{
		if (const int32 ExceptLine = DreamRunSource::FindCatchingExcept(*Source, DeliveryLine))
		{
			Live.SwallowLine = ExceptLine;
		}
	}
}

// =====================================================================================================================
// The pump (§5.4.4) and the rate (§5.4.6)
// =====================================================================================================================

void FRunSupervisor::FImpl::Pump(bool bOutputArrived)
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& App = FSlateApplication::Get();
	// §5.4.3: never tick Slate from inside its own tick or from inside input routing (07b Q2(a)); delivery stays allowed.
	if (App.IsTicking())
	{
		++Stats.Deferrals[static_cast<int32>(EDreamDeferReason::SlateTicking)];
		Live.LastDeferral = EDreamDeferReason::SlateTicking;
		return;
	}
	if (App.IsProcessingInput())
	{
		++Stats.Deferrals[static_cast<int32>(EDreamDeferReason::ProcessingInput)];
		Live.LastDeferral = EDreamDeferReason::ProcessingInput;
		return;
	}

	const bool bHadWindow = RunWindow.IsValid();
	const double Start = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_ON_CHANNEL(Dream.Pump, DreamPumpChannel);       // §13.1 timing scope
		DreamSupervisorPrivate::FPumpDepthScope Depth(PumpDepth);   // raised before the GIL is released: the watchdog reads it under the GIL
		FSlateApplication::FScopedPreventDebuggingMode NoBreakpoints(LOCTEXT("Supervisor.Pump", "A Dream run is updating the editor"));
		SCOPE_STALL_DETECTOR_PAUSE();
#if WITH_PYTHON
		FDreamScopedGilRelease NoGil;                             // GIL-3: workers and nested foreign Python run (07b Q7)
#endif
		OnPumpBegin();
		// The slow-task recipe (FeedbackContextEditor.cpp:419-439) plus PumpMessages when no modal is active (07b Q6):
		// with a modal up, TickPlatform pumps the messages itself.
		if (!App.GetActiveModalWindow().IsValid())
		{
			App.PumpMessages();
		}
		const bool bWasTicking = App.IsTicking();
		App.Tick(ESlateTickType::All);
		if (!bWasTicking && GIsRHIInitialized)
		{
			ENQUEUE_RENDER_COMMAND(DreamPumpEndFrame)([](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndFrame();
			});
		}
		if (FSlateRenderer* Renderer = App.GetRenderer())
		{
			Renderer->Sync();                                    // runs queued AsyncTask(GameThread) work
		}
		// Never FTSTicker::Tick, GEngine->Tick, GC or OnEndFrame (§5.4.4): viewports do not render during a run (U11).
	}
	if (PumpsSinceNative >= 0)
	{
		++PumpsSinceNative;
	}

	const double Cost = FPlatformTime::Seconds() - Start;
	DreamTrace::TracePump(CurrentRunId.load(), Cost * 1000.0, 1.0 / FMath::Max(PeriodSeconds.load(), 1e-6), bOutputArrived);
	++Stats.Pumps;
	Stats.PumpSeconds += Cost;
	Stats.MaxPumpMs = FMath::Max(Stats.MaxPumpMs, Cost * 1000.0);
	if (!bHadWindow && RunWindow.IsValid())
	{
		// The pump that built the Run window also laid it out and painted it for the first time: one-time presentation
		// work, not the steady pump cost §5.4.6 adapts to, so it neither seeds the EWMA nor flags an overload.
		return;
	}
	AdaptRate(Cost, bOutputArrived);
}

void FRunSupervisor::FImpl::AdaptRate(double Cost, bool bOutputArrived)
{
	using namespace DreamSupervisorPrivate;
	CostEwma = bHaveCost ? (1.0 - EwmaAlpha) * CostEwma + EwmaAlpha * Cost : Cost;
	bHaveCost = true;

	double Hz = TargetHz;
	if (CostEwma > HardCostSeconds)
	{
		// Hard limit: below 10 Hz to hold 15% of wall time, floor 2 Hz; CI treats this as a G2(4) failure.
		Hz = FMath::Clamp(HardShare / CostEwma, OverloadFloorHz, FloorHz);
		if (!Stats.bPumpOverload)
		{
			UE_LOG(LogDream, Warning, TEXT("Run #%d: pump overload (%.1f ms per pump); the Run window updates at %.1f Hz."), Policy.RunNumber, CostEwma * 1000.0, Hz);
		}
		Stats.bPumpOverload = true;
	}
	else if (CostEwma * TargetHz > OverheadShare)
	{
		Hz = FMath::Max(FloorHz, OverheadShare / CostEwma);
	}
	else if ((bOutputArrived || bStopRequested.load()) && CostEwma * BoostHz <= OverheadShare)
	{
		Hz = BoostHz;
	}
	PeriodSeconds.store(1.0 / Hz);
	TRACE_COUNTER_SET(DreamPumpCostMs, Cost * 1000.0);
	TRACE_COUNTER_SET(DreamPumpRateHz, Hz);
}

// =====================================================================================================================
// Presentation (§5.5)
// =====================================================================================================================

void FRunSupervisor::FImpl::OnPumpBegin()
{
	// RT-04: the protections, the pre-processor first, exist before the first PumpMessages dispatches queued input.
	if (!bPresentationStarted)
	{
		bPresentationStarted = true;
		Protections.Install(Policy.RunNumber, EffectivePresentation());
	}
	OpenRunWindowIfPossible();
	SDreamRunWindow::ReparentKeptChildWindows();

	if (RunWindow.IsValid() && RunWindowContent.IsValid())
	{
		const uint32 Signature = RunWindowContent->GetNotesSignature();
		if (Signature != NotesSignature)
		{
			NotesSignature = Signature;
			SDreamRunWindow::GrowForNotes(RunWindow, *RunWindowContent, Policy);
		}
	}
}

void FRunSupervisor::FImpl::OpenRunWindowIfPossible()
{
	if (RunWindow.IsValid() || EffectivePresentation() == EDreamRunPresentation::Off)
	{
		return;
	}
	FSlateApplication& App = FSlateApplication::Get();
	// §5.5.1: no modal possible (renderer-less states): the common protections only.
	// §5.5.3 step 2: a foreign modal is up (a user slow task, §5.12): re-check at every pump.
	if (!App.CanAddModalWindow() || App.GetActiveModalWindow().IsValid())
	{
		return;
	}

	const TSharedRef<SDreamRunWindow> Content = SNew(SDreamRunWindow)
		.RunNumber(Policy.RunNumber)
		.TabOrItem(Policy.TabOrItem)
		.Stats(&Live)
		.OnStop(FSimpleDelegate::CreateLambda([]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetRunService().RequestStopActive(EDreamStopSource::RunWindowButton);
			}
		}))
		.OnInterruptCleanup(FSimpleDelegate::CreateLambda([]()
		{
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetRunService().RequestInterruptCleanup();
			}
		}));

	RunWindow = SDreamRunWindow::OpenModal(Policy, Content);
	if (RunWindow.IsValid())
	{
		RunWindowContent = Content;
		NotesSignature = Content->GetNotesSignature();
		Protections.SetRunWindow(RunWindow, Content->GetLiveRunView());
	}
}

void FRunSupervisor::FImpl::HandOffLiveView()
{
	if (!RunWindowContent.IsValid())
	{
		return;
	}
	const TSharedPtr<SDreamOutputView> LiveView = RunWindowContent->GetLiveRunView();
	const TSharedPtr<SWidget> AreaWidget = Policy.OutputAreaWidget.Pin();
	if (!LiveView.IsValid() || !AreaWidget.IsValid() || AreaWidget->GetType() != FName(TEXT("SDreamOutputArea")))
	{
		return;
	}
	EDreamRunRoute Route = EDreamRunRoute::Buffer;
	if (const FDreamRunRecord* Rec = FindActiveRecord())
	{
		Route = Rec->Route;
	}
	// The REPL view for a REPL run while the REPL tab is active, else the Output view (§5.5.3 "Hand-off").
	const TSharedPtr<SDreamOutputView> Target = StaticCastSharedPtr<SDreamOutputArea>(AreaWidget)->GetHandoffView(Route);
	if (Target.IsValid())
	{
		FDreamOutputViewHandoff Handoff = LiveView->CaptureHandoff();
		if (!Policy.bOutputAreaVisible && !Handoff.bFollowing)
		{
			// The window was centred because the area was not visible: its screen y means nothing to the area, so the
			// reading line goes to the view's top row (§5.5.3 "Not following", T-SUP-037).
			Handoff.FirstVisibleScreenY = static_cast<float>(Target->GetCachedGeometry().GetAbsolutePosition().Y);
		}
		Target->ApplyHandoff(Handoff);
	}
}

#undef LOCTEXT_NAMESPACE
