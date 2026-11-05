// SPDX-License-Identifier: Apache-2.0
// The Python lifecycle and the only two ExecPythonCommandEx call sites (TDD §3.4-§3.6, §3.8, §12.3, D14, D16, RT-15, RT-17).
// Game thread unless marked. Includes no Python.h (DreamPythonFwd.h only), so any slice may include it.
// Bodies: DreamPython.cpp (lifecycle, helpers) and DreamPythonEntry.cpp (EnterRun and Boot, §2.5 rule 5) - bridge slice.
#pragma once

#include "CoreMinimal.h"
#include "Python/DreamPythonFwd.h"
#include "Run/DreamRunTypes.h"
#include "Templates/ValueOrError.h"

/** §3.5 states. */
enum class EDreamPythonState : uint8
{
	Unavailable,        // WITH_PYTHON is 0 or the plugin is missing
	Headless,           // commandlet, unattended or nullrhi
	Disabled,           // configured but not enabled
	WaitingForInit,     // enabled, startup scripts pending
	Cold,               // _dream exists; dream_editor not booted (deferred start, RT-17)
	Booting,
	Ready,
	BootFailed,
	ShuttingDown,
	ShutDown,
	Enabling,
};

/** §3.4 deferred-start triggers. */
enum class EDreamStartReason : uint8
{
	Focus,
	Keystroke,
	Timer,
	RunRequest,
	ConsoleExec,
	ConsoleCompletion,
	ApiCall,
};

/** The fixed helper table of §12.1.3 (INV-6). Slice A calls the first five. */
enum class EDreamHelper : uint8
{
	Prepare,            // _impl.runner.prepare(run_id, request) -> dict
	Discard,            // _impl.runner.discard(run_id)
	ClassifyInput,      // _impl.runner.classify_input(source, last_logical_indented) -> str
	EmergencyRestore,   // _impl.streams.emergency_restore()
	ResetSession,       // _impl.namespaces.reset_session() -> list[str]   (slice A choice: RT-15 Reset Session as a helper)
	OnBufferSaved,      // _impl.runner.on_buffer_saved(path)              (slice A choice: §5.3.2 linecache entry lifetime, R2)
	Shutdown,           // dream_editor._shutdown(generation)
};

enum class EDreamReplClassify : uint8
{
	Complete,
	Incomplete,
	Invalid,
};

/** A compile error from prepare, in buffer coordinates (§5.3.2; the "error" dict of DreamNativeModule.h). */
struct FDreamSyntaxError
{
	/** The exception type ("type" key): SyntaxError, IndentationError or TabError, or ValueError, RecursionError,
	 *  OverflowError for a non-syntax compile failure. The not executed block prints "<TypeName>: <Message>". */
	FString TypeName = TEXT("SyntaxError");
	FString Message;
	int32 Line = 0;                     // 1-based buffer line
	/** 1-based UTF-8 byte column in Text, always (the runner converts the parser's character offsets); convert to UTF-16
	 *  with dream/Utf.h or TextBuffer before indexing an FString. */
	int32 Offset = 0;
	/** Always greater than Offset: the end column (exclusive, same units), or end of line + 1 when the error spans lines. */
	int32 EndOffset = 0;
	/** The whole buffer line Line. */
	FString Text;
};

/** The prepare helper's result (§5.3.2). */
struct FDreamPrepareResult
{
	bool bOk = false;
	bool bStatementMode = false;
	TArray<FDreamStatementRange> Statements;   // statement mode only
	FDreamSyntaxError Error;                   // when !bOk and the failure is a compile error
	FString InternalError;                    // when !bOk because the helper itself failed (a Dream bug)
	int32 AliasesWritten = 0;                 // selection aliases prepare wrote under U19 (§5.3.8): bumps the session epoch
};

class DREAMEDITOR_API FDreamPython
{
public:
	FDreamPython();
	~FDreamPython();

	/** StartupModule (B1): detect headless, bind OnPythonInitialized (RegisterOnPythonInitialized) and OnPythonShutdown. No Python. */
	void Initialize();
	/** ShutdownModule: RemoveAll(this) on both delegates if PythonScriptPlugin is still loaded; touches no Python (§3.8). */
	void Shutdown();

	/** Any thread (atomic). */
	EDreamPythonState GetState() const;
	/** Any thread. Incremented at every OnPythonInitialized (§3.5); the high 32 bits of every FDreamRunId. */
	uint32 GetGeneration() const;
	/**
	 * Any thread. +1 whenever Dream rewrites the selection aliases in the Session (U19, §5.3.8: prepare, the idle refresh
	 * after a selection change, the map-change release) and at Reset Session, which removes them; the live resolver's
	 * cached roots are keyed by it (§7.5.3, §12.3).
	 */
	uint32 GetSessionEpoch() const;
	/** Banner text for Unavailable, Disabled, BootFailed and ShutDown (§9.5 wording). */
	FText GetDisabledReason() const;
	bool IsReady() const { return GetState() == EDreamPythonState::Ready; }
	/** True in Cold, Booting, WaitingForInit and Enabling: a run request is accepted and waits (§3.5 table). */
	bool CanQueueRuns() const;

	/** RT-17 deferred-start trigger; idempotent; acts only in Cold. Arms the boot at the next OnEndFrame (FDreamRunService::ArmEndFrame). */
	void RequestStart(EDreamStartReason Reason);
	/** The banner's Retry (§3.5 BootFailed -> Booting): boots again at the next OnEndFrame when _dream exists. */
	void RetryBoot();
	/** Engine.Python.IsPythonInRestrictiveMode (SEC-07): runs are disabled with an explanation, Enable Python never offered. */
	bool IsRestrictiveMode() const;
	/** Any thread. The generation whose interpreter is alive (GIL-7 stamps); 0 before the first and after each shutdown. */
	static uint32 GetAliveGeneration();
	/** Enable Python (§3.5): only arms the next OnEndFrame, which calls ForceEnablePythonAtRuntime. */
	void RequestEnable();
	/** Reset Session (RT-15, §5.3.3): refused while a run is active; runs the helper at the next idle OnEndFrame. */
	bool RequestResetSession(FText& OutReason);

	// ---- Helpers (§12.1.3). GT; only from FDreamRunService's OnEndFrame handler or FDreamDeferral idle ticks (INV-6). ----
	// (slice A choice) Typed wrappers instead of §12.3's generic CallHelper(EDreamHelper, FDreamHelperArgs).

	/** Builds the request dict (DreamNativeModule.h "prepare request") and calls _impl.runner.prepare. Executes no user code. */
	FDreamPrepareResult Prepare(FDreamRunId Id, const FDreamRunRecord& Record, const FDreamRunRequest& Request);
	/** Starting aborted after a successful prepare (Stop before entry, failed snapshot or save). */
	void Discard(FDreamRunId Id);
	/** REPL Enter decision (§6.5). Error when Python is not Ready or the helper failed. */
	TValueOrError<EDreamReplClassify, FText> ClassifyInput(const FString& Source, bool bLastLogicalIndented);
	/** After ExecPythonCommandEx returned false (§5.2.4). */
	void EmergencyRestore();

	/**
	 * Ruling R2 (§5.3.2 entry lifetime): a buffer that was dirty and has a real path was saved. Game thread; any context,
	 * Slate handlers included, because it runs no Python itself: it queues Path for an FDreamDeferral idle tick
	 * (bNeedsPython) that calls _impl.runner.on_buffer_saved(path), which drops the runner's linecache entry for the file
	 * if it is still the tuple the runner wrote. Saves before the tick coalesce. A no-op when dream_editor is not booted
	 * (no run registered an entry); entries queued for a generation that ended are dropped.
	 */
	void NotifyBufferSaved(const FString& Path);

	/** Test hook (Dream.Run.Aliases): what FEditorDelegates::MapChange does, through the same deferred helper. */
	void SimulateMapChangeForTest();

	// ---- Live completion helpers (completion group, B60/B63; bodies: DreamPythonCompletion.cpp). GT, only from
	// FDreamDeferral idle ticks that need Python (INV-6); never from a Slate handler (D14(5)). False when Python is not Ready,
	// a run is active or a pump is on the stack, or the helper failed (the result's Error). They execute no user code (D6).
	/** _impl.resolver.complete(req) (§7.5.6). */
	bool ResolverComplete(const struct FDreamResolverRequest& Request, struct FDreamResolverResult& OutResult);
	/** _impl.resolver.prewarm(req): one slice of the post-run Session copy (RV-23, §6.6 step 5). */
	bool ResolverPrewarm(const struct FDreamResolverPrewarmRequest& Request, struct FDreamResolverPrewarmResult& OutResult);
	/** _impl.resolver.unreal_names(req): one slice of the unreal module's top-level names (FUnrealNameIndex). */
	bool ResolverUnrealNames(int32 Cursor, int64 BudgetNs, struct FDreamResolverNamesResult& OutResult);
	/** Any thread. Resolver helper calls made in this process (the T-REPL-013 probe). */
	static uint64 GetResolverCallCount();

	/** Broadcast on every state change, on the game thread. */
	DECLARE_EVENT_OneParam(FDreamPython, FOnStateChanged, EDreamPythonState /*NewState*/);
	FOnStateChanged& OnStateChanged() { return StateChangedEvent; }
	/** After _dream is created (each generation): the console executor registers here (§6.6). */
	FSimpleMulticastDelegate& OnInitialized() { return InitializedEvent; }
	/** In HandleShutdown, before refs are released: the console executor unregisters here (§6.6). */
	FSimpleMulticastDelegate& OnShutdown() { return ShutdownEvent; }

	/** Any thread: queue a decref for the game thread (GIL-7). Used by FDreamPyRef when destroyed without the GIL. */
	static void QueueDecref(FDreamPyObject* Object, uint32 Generation);

private:
	friend class FDreamRunService;
	friend class FDreamPyRef;

	/**
	 * INV-3: the one run entry. Asserts IsInGameThread, FDreamRunService::IsInEndFrameStart, not ticking, not processing
	 * input, no mouse captor; then ExecPythonCommandEx(EvaluateStatement, Flags None, "__import__('dream_editor')._run(<id>)").
	 * Returns ExecPythonCommandEx's result; fills the engine's CommandResult and the LogOutput size (RT-07 stat).
	 */
	bool EnterRun(FDreamRunId Id, FString& OutCommandResult, int32& OutLogOutputEntries);

	/**
	 * The INV-3 context checks of EnterRun, without entering: game thread, inside FDreamRunService's OnEndFrame handler,
	 * Slate not ticking, not processing input, no mouse captor. FDreamRunService re-arms instead of entering when false
	 * (§3.9 "EnterRun from a forbidden context": refuse, re-arm, report).
	 */
	static bool IsEntryContextValid(FString& OutWhy);

	/**
	 * Called first by FDreamRunService::HandleEndFrame: performs pending lifecycle work that must run at OnEndFrame (boot,
	 * Enable Python, Reset Session, decref queue). Returns true when Python is Ready afterwards.
	 * (slice A choice) FDreamRunService owns the one OnEndFrame handler (§2.5 rule 9); FDreamPython rides it.
	 */
	bool ProcessEndFrame();

	/**
	 * The second ExecPythonCommandEx call site: "__import__('dream_editor')._boot(globals(), <generation>)" (§3.4).
	 * Ruling R3: CommandResult "True" is Ready; a failed call is BootFailed with the last line of CommandResult in the
	 * banner (the ABI mismatch's ImportError text) and the whole text in LogDream; any other result is BootFailed with
	 * the "Dream did not start: <reason>" line _boot wrote to stderr (found in LogOutput; it names a shadowing package's
	 * folder, ENV-07) in the banner, else "returned <repr>; see LogPython".
	 */
	bool Boot();

	void HandleInitialized();           // §3.5: rebind shutdown, ++generation, CheckRuntime, create _dream, supervisor alive
	/** §3.5 initial state once Python is configured: Disabled when not available, or when the lifecycle is dynamic and
	 *  nothing force-enabled it (D16, "This project starts Python on demand."); else it stays WaitingForInit. */
	void DecideUnstartedState();
	void HandleConfigured();            // OnPythonConfigured after an early Initialize: DecideUnstartedState on the next tick
	void HandleShutdown();              // §3.8 steps 1-6

	// Selection aliases and the map-change release (§5.3.8, §3.7 "Alias refresh", §3.8 UObject rules, D3). Bodies in
	// DreamPythonAliases.cpp. The handlers run no Python: each queues one FDreamDeferral idle tick (Defer, needs Python).
	void BindEditorEvents();
	void UnbindEditorEvents();
	void HandleSelectionChanged(UObject* Selection);
	void HandleMapChange(uint32 MapChangeFlags);

	static void RegisterRef(uint32 Generation);
	static void UnregisterRef(uint32 Generation);

	FOnStateChanged StateChangedEvent;
	FSimpleMulticastDelegate InitializedEvent;
	FSimpleMulticastDelegate ShutdownEvent;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
