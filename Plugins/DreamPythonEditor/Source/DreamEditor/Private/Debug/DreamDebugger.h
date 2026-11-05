// SPDX-License-Identifier: Apache-2.0
// The in-editor debugger's C++ side (TDD §8.1-§8.5; DBG-01..DBG-05, DBG-08; debug group, PO-06 F5):
//   - FDreamBreakpoints' part: line breakpoints per buffer as dream::AnchorSet line anchors (they move with edits, §8.2.1),
//     persisted per file outside the file in <storage root>/state/breakpoints.json (§8.2.6, BUF-07), and the per-run
//     DebugConfig the runner gets with its prepare request (§8.1.5);
//   - the pause: the C++ pause rules (§8.4 rules 4-11), the Paused window's blocking modal loop (§8.3.1) with the
//     RunSupervisor's paused gate, requests drained from a clean point at the top of each modal-loop iteration (§8.3.3)
//     and the resume command.
// Game thread only. Includes no Python.h (rule 6): the Python half is Python/DreamNativeModule.cpp (_dream.pause and
// pause_context, which release the GIL around RunPause) and Python/DreamPythonDebug.cpp (handle_request).
// Not in this round (R-notes in the debug report): evaluate and watches, supervised evaluates, run to cursor, the
// foreign-tracer prompt, the engine-callback wrapper flag, the PIE-transition flag, child-window re-parenting.
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"
#include "Tasks/Task.h"
#include "dream/Json.h"

class FDreamBuffer;
class SDreamPausedWindow;
class SWindow;

/** TDD §8.1.5: the full list of pause refusals, in this order (debugger.py REFUSAL_* mirrors the values). */
enum class EDreamPauseRefusal : uint8
{
	None,
	NotDebugRun,
	NotGameThread,
	SlateTicking,
	EngineCallback,
	EngineBusy,
	PIETransition,
	Unattended,
	AlreadyPaused,
	WindowFailed,
	PolicyDisabled,
	InsidePump,
};

/** TDD §8.1.5 (Run to cursor is not in this round). */
enum class EDreamResumeKind : uint8
{
	Continue,
	StepOver,
	StepInto,
	StepOut,
	Stop,
};

struct FDreamResume
{
	EDreamPauseRefusal Refusal = EDreamPauseRefusal::None;
	EDreamResumeKind Kind = EDreamResumeKind::Continue;
	/** The stop was the uncaught-exception stop (§8.2.5): Stop returns "terminate" instead of raising. */
	bool bInspectOnly = false;
};

/** One user frame of a stop, innermost first (§8.1.6). */
struct FDreamDebugFrame
{
	int32 Id = 0;
	FString Name;
	FString Source;
	int32 Line = 0;
};

/** The stop the Paused window shows (the stop_json of _dream.pause). */
struct FDreamDebugStop
{
	FDreamRunId RunId = 0;
	int32 RunNumber = 0;
	FString Reason;             // breakpoint, step, exception, breakpoint(), entry
	FString Description;
	FString Source;             // the compile filename of the paused frame
	int32 Line = 0;             // 1-based
	bool bCanStep = true;
	TArray<FDreamDebugFrame> Frames;
};

DECLARE_MULTICAST_DELEGATE(FDreamOnBreakpointsChanged);

class DREAMEDITOR_API FDreamDebugger
{
public:
	static FDreamDebugger& Get();

	// ---- Breakpoints (DBG-01) ----

	/** The breakpoint lines of a buffer, 0-based, ascending, without duplicates. Loads the file's saved lines on first use. */
	TArray<int32> GetBreakpointLines(FDreamBuffer& Buffer);
	bool HasBreakpoint(FDreamBuffer& Buffer, int32 Line);
	/** F9 or a gutter click: toggles the line's breakpoint; returns the new state. Persists at once. */
	bool ToggleBreakpoint(FDreamBuffer& Buffer, int32 Line);
	void SetBreakpoint(FDreamBuffer& Buffer, int32 Line, bool bOn);
	void ClearBreakpoints(FDreamBuffer& Buffer);
	/** A breakpoint toggled in the Paused window's source view, keyed by the compile filename (an open buffer's, or a file
	 *  that is not open). Persists, and sends setBreakpoints to the paused run. */
	void ToggleBreakpointAtSource(const FString& Source, int32 Line);
	/** The lines of a source key (compile filename) as the Paused window's source view draws them, 1-based. */
	TArray<int32> GetSourceBreakpointLines(const FString& Source);
	/** Fires when any breakpoint changes (code views and the Paused window repaint). */
	FDreamOnBreakpointsChanged& OnBreakpointsChanged() { return BreakpointsChanged; }

	/** The DebugConfig of a Debug run (§8.1.5) as JSON text: every breakpoint of every open buffer, keyed for the runner
	 *  (the run's own buffer by RunFilename), and the saved breakpoints of files that are not open (DBG-02). */
	FString BuildDebugConfigJson(FDreamRunId RunId, const FGuid& RunBufferId, const FString& RunFilename);

	/** Where breakpoints persist (tests point it at a temporary file and back; empty restores the default). */
	void SetStorePathForTest(const FString& Path);
	FString GetStorePath() const;
	/** Writes the store now on the game thread (tests). */
	void FlushStoreForTest();

	// ---- The pause (§8.3, §8.4) ----

	/** §8.4 rules 4-11 for a pause of RunId now (the _dream.pause_context answer). Game thread. */
	EDreamPauseRefusal CanPause(FDreamRunId RunId) const;
	/**
	 * _dream.pause's body, called WITHOUT the GIL (the bridge released it): re-checks CanPause, sets the supervisor's paused
	 * gate, opens the Paused window with AddModalWindow (blocking, §8.3.1), drains its requests at modal-loop iterations,
	 * and returns the resume command once the window closed.
	 */
	FDreamResume RunPause(FDreamRunId RunId, const FString& StopJson);
	bool IsPaused() const { return bPaused; }
	const FDreamDebugStop& GetStop() const { return Stop; }
	/** The paused line of a buffer's code (0-based) when the paused frame runs that buffer's code. */
	bool GetPausedLine(const FDreamBuffer& Buffer, int32& OutLine) const;

	/** Continue, Step over, Step into, Step out, Stop from the Paused window (or a test). Closes the window. Steps at an
	 *  exception stop continue (§8.2.5). Stop calls FDreamRunService::RequestStopActive first (Stop always works). */
	void RequestResume(EDreamResumeKind Kind);
	/** FRunSupervisor::RequestStop while paused (Shift+F5 through the input pre-processor, the API): resume with Stop. */
	void OnRunStopRequested();

	/** Queues a DAP-shaped request; the handler runs it at the next modal-loop iteration and calls OnResponse there
	 *  (§8.3.3). Response is the whole response object ({"success", "body", "message"}). */
	void QueueRequest(const FString& Command, dream::json::Value Arguments, TFunction<void(const dream::json::Value& Response)> OnResponse);
	/** Requests waiting or running. */
	int32 GetPendingRequestCount() const { return Pending.Num() + (bInRequest ? 1 : 0); }

	TSharedPtr<SWindow> GetPausedWindowForTest() const { return PausedWindow.Pin(); }
	TSharedPtr<SDreamPausedWindow> GetPausedContentForTest() const { return PausedContent.Pin(); }
	/** Tests (the G4 driver pattern, §8.9.1): called at each modal-loop iteration of a pause once no request is pending,
	 *  so a latent spec can act while the core ticker is stopped. */
	TFunction<void(FDreamDebugger&)> PausedTickForTest;
	/** Pauses opened so far in this session (tests). */
	int32 GetPauseCountForTest() const { return PauseCount; }

private:
	FDreamDebugger() = default;

	struct FBufferMarks
	{
		TArray<uint64> Anchors;     // dream::AnchorId line-start anchors on the buffer's TextBuffer
		bool bLoaded = false;
		FString Key;                // the store key when the marks were loaded (a Save As moves it)
		FDelegateHandle ChangedHandle;
		FDelegateHandle StateHandle;
	};

	FString KeyOf(const FDreamBuffer& Buffer) const;
	FBufferMarks& MarksOf(FDreamBuffer& Buffer);
	void LoadStore();
	void SaveStore();
	void SaveStoreImpl(bool bSync);
	/** Review q1 M1: the buffer-manager close hook and OnPreExit, once (again after a module restart). */
	void EnsureHooks();
	void OnBufferEdited(const FGuid& Id);
	void OnBufferStateChanged(const FGuid& Id);
	void HandleBufferClosed(const TSharedRef<FDreamBuffer>& Buffer);
	/** A debounced save (2 s after the last edit that moved a marker; FDreamDeferral, never during a run). */
	void ScheduleSave();
	/** OnPreExit: waits for the pending write, saves synchronously, and resets the static state (it outlives the module). */
	void OnPreExit();
	void NotifyChanged();
	/** Sends the source's current breakpoints to the paused run (setBreakpoints). */
	void SendBreakpointsForSource(const FString& Source, const TArray<int32>& Lines1Based);
	TSharedPtr<FDreamBuffer> FindBufferForSource(const FString& Source) const;

	void OnModalLoopTick(float DeltaTime);
	void CloseWindow();

	// Breakpoints
	TMap<FGuid, FBufferMarks> Marks;
	/** Saved lines (1-based) by key: a file's normalised path, or "buffer:<id hex>" for an untitled tab. */
	TMap<FString, TArray<int32>> Stored;
	bool bStoreLoaded = false;
	FString StorePathOverride;
	UE::Tasks::FTask PendingWrite;      // the last store write; writes chain so the newest lands last
	FDreamOnBreakpointsChanged BreakpointsChanged;

	// Pause
	bool bPaused = false;
	bool bShown = false;
	bool bResumeSet = false;
	bool bClosing = false;              // review q1 m3: the window is destroyed once per pause
	bool bSaveScheduled = false;
	void* HookedManager = nullptr;      // the FDreamBufferManager whose OnBufferClosed we listen to
	FDelegateHandle ClosedHandle;
	FDelegateHandle PreExitHandle;
	FDreamResume Resume;
	FDreamDebugStop Stop;
	TWeakPtr<SWindow> PausedWindow;
	TWeakPtr<SDreamPausedWindow> PausedContent;
	struct FRequest
	{
		FString Json;
		TFunction<void(const dream::json::Value&)> OnResponse;
	};
	TArray<FRequest> Pending;
	bool bInRequest = false;
	int32 NextSeq = 1;
	int32 PauseCount = 0;
	FVector2D SavedPosition = FVector2D::ZeroVector;
	FVector2D SavedSize = FVector2D::ZeroVector;
};
