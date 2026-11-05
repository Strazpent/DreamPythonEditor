// SPDX-License-Identifier: Apache-2.0
// Run types shared by every slice (TDD §5.1.1, §5.2.3, §5.2.5, §5.9.7, §12.1.2). Game thread unless a field says otherwise.
// Bodies of the DreamRunText helpers are in DreamRunTypes.cpp (bridge slice).
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "dream/TextBuffer.h"

/** (Generation << 32) | EntrySerial (§12.1.2). Python sees the same integer. 0 = no run. */
using FDreamRunId = uint64;

namespace DreamRunId
{
	inline FDreamRunId Make(uint32 Generation, uint32 EntrySerial) { return (static_cast<uint64>(Generation) << 32) | EntrySerial; }
	inline uint32 GetGeneration(FDreamRunId Id) { return static_cast<uint32>(Id >> 32); }
	inline uint32 GetEntrySerial(FDreamRunId Id) { return static_cast<uint32>(Id & 0xFFFFFFFFu); }
}

/** Routes (§5.2.1). Slice A builds Selection, Line, Buffer, Repl, ConsoleExecutor, RunFile and Api. */
enum class EDreamRunRoute : uint8
{
	Selection,
	Line,
	Cell,
	Buffer,
	Repl,
	ConsoleExecutor,
	RunFile,
	Shelf,
	Snippet,
	Reload,
	ReloadOnSave,
	Revert,
	EntryPoint,
	TaskStep,
	Api,
	Bridge,     // 1.1
	Mcp,        // 1.1-1.2
};

/** The U2 states (§5.1.1). Display names come from DreamRunText::StateName, never from the enum names. */
enum class EDreamRunState : uint8
{
	Idle,
	Starting,
	Running,
	Stopping,
	Paused,
	TaskRunning,
};

/** The U2 outcomes (§5.1.1). The integer values are the _dream.report_outcome codes (DreamNativeModule.h). */
enum class EDreamRunOutcome : uint8
{
	None = 0,
	Ok = 1,
	Error = 2,
	Cancelled = 3,
	NotExecuted = 4,
	Lost = 5,
};

enum class EDreamNamespaceMode : uint8
{
	Session,
	Isolated,
	Agent,      // 1.2
};

/** (slice A choice) LiveDocked is M4; a LiveDocked setting runs with the Run window in slice A. */
enum class EDreamRunPresentation : uint8
{
	RunWindow,
	LiveDocked,
	Off,
};

enum class EDreamStopSource : uint8
{
	RunWindowButton,
	BannerButton,
	StatusBarButton,
	Esc,
	ShiftF5,
	PausedWindow,
	InterruptCleanup,
	Api,
};

/** Why a pending call skipped the pump (§5.4.3). Counted per run in FDreamRunStats. */
enum class EDreamDeferReason : uint8
{
	SavingPackage,
	GarbageCollecting,
	RoutingPostLoad,
	NativeReentry,
	OutsideUserCode,
	DebugpyFrame,       // M2 (vendored debugpy)
	SlateTicking,
	ProcessingInput,
	Count
};

/** (slice A choice) The Starting steps of §5.1.2 as an explicit state machine driven by FDreamRunService's OnEndFrame handler. */
enum class EDreamStartingStep : uint8
{
	Queued,             // accepted, waiting for the next OnEndFrame
	Prepare,            // step 2: runner.prepare (trust, step 1, is M4)
	AwaitSnapshot,      // step 3: journal ticket not yet durable
	AwaitSave,          // step 4: Run File save in flight
	Enter,              // step 6: transaction, BeginRun, ExecPythonCommandEx
	Done,
};

/** Transaction status (§5.9.7). (slice A choice) Declared here so FDreamRunRecord can hold it without a header cycle. */
enum class EDreamTxnStatus : uint8
{
	Off,
	NotStarted,
	Open,
	Recorded,
	Empty,
	Partial,
	Purged,
	CancelledByScript,
	Cleared,
	Undone,
};

struct FDreamTxnHandle
{
	EDreamTxnStatus Status = EDreamTxnStatus::Off;
	FGuid TransactionId;
	FText Title;                        // "Dream: <tab> #N" (§5.9.1)
	FText Note;                         // header note, e.g. "undo unavailable: another edit was in progress"
	SIZE_T LastSize = 0;
	SIZE_T Budget = 0;                  // [Undo] UndoBufferSize in bytes (§5.9.4)
	int32 QueueLengthAtBegin = 0;       // after Begin: the open entry is the last one (its TransactionId)
	int32 ObjectCount = 0;
	int32 DanglingClosed = 0;           // transactions the script left open and End closed (§5.9.2)
	bool bWarned50 = false;
	bool bPieObjects = false;
	bool bImported = false;
	bool bRenamed = false;
};

/** One top-level statement of a statement-mode run (RUN-27): 1-based buffer lines from prepare (§5.3.2). */
struct FDreamStatementRange
{
	int32 FirstLine = 0;
	int32 LastLine = 0;
};

/** Where an outcome landed (report_outcome's location, §5.3.9). */
struct FDreamRunLocation
{
	FString File;                       // real path or pseudo filename
	int32 Line = 0;                     // 1-based; 0 = none
	int32 Column = 0;                   // 0-based UTF-16 column on Line (a not executed compile error's caret, RUN-16)
};

/** A run request (§5.2.5). Built by SDreamTab, SDreamReplStrip, FDreamConsoleExecutor, console commands and _dream.enqueue_run. */
struct FDreamRunRequest
{
	EDreamRunRoute Route = EDreamRunRoute::Buffer;
	bool bDebug = false;                // M3

	FGuid BufferId;                     // invalid for REPL, console and file-only API runs
	FString TabTitle;                   // "Untitled-3", "rig_fix.py", "REPL", "Console"
	FString FilePath;                   // the real path when the buffer has one (or the Run File / API file)
	bool bDirty = false;
	/**
	 * Route Api with a file target (dream_editor.run(<path>), §12.2): Run File semantics whether or not the file is open in
	 * a tab (§5.2.1 API row, §5.3.4): Isolated by default, one exec, sys.path entry and sys.argv, a dirty open buffer saved
	 * at Starting step 4. A tab target (ID or name) leaves it false.
	 */
	bool bFileTarget = false;

	/** Code to compile. (run, PO-06 F2) A selection is exactly the selected characters (DreamRunSource::ExactSelection):
	 *  a fragment starts at SelectionStartColumn; an indented block keeps its indentation (RT-14, §5.3.2). */
	FString Text;
	/** 1-based buffer line of Text's first line. */
	int32 FirstLine = 1;
	/** Whole-buffer text for linecache. (slice A choice) Null: FDreamRunService fills it from SourceSnapshot. */
	TSharedPtr<const FString> BufferText;

	/**
	 * The snapshot Text was cut from (§5.2.3, §6.2 source snapshot, §10.2.5 step 1). Buffer routes pass the buffer's
	 * snapshot; REPL and console requests leave it empty and FDreamRunService makes one from Text, numbered from 1.
	 */
	dream::TextSnapshot SourceSnapshot;
	/** A selection that ends mid-line: its UTF-16 end column on the last line, so the echo stops there (§6.2). */
	TOptional<int32> SelectionEndColumn;
	/** (run, PO-06 F2) A selection that starts after code on its line: its UTF-16 start column on FirstLine. The runner
	 *  maps columns back to the buffer from it (tracebacks, carets, SyntaxError positions) and the echo starts there. */
	TOptional<int32> SelectionStartColumn;

	/**
	 * The compile filename (§5.3.2): the real path, or "<dream:Tab Name#id>" from FDreamBufferManager::GetCompileFilename.
	 * Empty for REPL and console routes: FDreamRunService sets "<dream:REPL#N>" / "<dream:Console#N>" at Submit.
	 */
	FString Filename;
	/** SHA-256 (lower-case hex) of the file's bytes when the request read the file from disk (Run File of a file that is
	 *  not open, DreamRunFile::ReadRunnableFile): the audit's code.sha256 for Run File (§11.4, T-AUD-002). */
	FString FileSha256;

	EDreamNamespaceMode Namespace = EDreamNamespaceMode::Session;
	TOptional<bool> TransactionOverride;  // U6 "Run in / without transaction"; unset = the route default
	TArray<FString> Argv;               // Run File (RUN-14)
	FString Badge;
	double TimeoutSeconds = 0.0;
};

struct FDreamSubmitResult
{
	bool bAccepted = false;
	int32 RunNumber = 0;                // assigned at acceptance, so not executed runs are numbered (U7, U8)
	FText Reason;                       // why it was refused
};

/** Supervision statistics frozen into the record by EndRun (§5.2.3, §5.8). */
struct FDreamRunStats
{
	int32 Pumps = 0;
	double PumpSeconds = 0.0;
	double MaxPumpMs = 0.0;
	int32 Deferrals[static_cast<int32>(EDreamDeferReason::Count)] = {};
	double DeferralSeconds = 0.0;
	int32 Deliveries = 0;
	double StopRequestedAt = 0.0;       // FPlatformTime::Seconds, 0 = never
	double StopEffectiveAt = 0.0;
	double NativeSeconds = 0.0;         // sum of native intervals >= 100 ms
	double LongestNativeSeconds = 0.0;
	int32 LongestNativeLine = 0;
	bool bStopDuringNative = false;
	/** The native interval during which Stop was requested (set with bStopDuringNative): "Stop took effect when the native
	 *  call at line 43 returned (12.4 s)" (§5.8). Line 0 = not known. */
	double StopNativeSeconds = 0.0;
	int32 StopNativeLine = 0;
	bool bPumpOverload = false;
	/** The last dream_editor.progress(i, n) the run reported, frozen by EndRun (OUT-02 "312 / 1,204 completed"); -1 = none. */
	int64 ProgressDone = -1;
	TOptional<int64> ProgressTotal;
	int32 LogOutputEntries = 0;         // FPythonCommandEx::LogOutput.Num() at return (RT-07)
	int32 BypassLogPythonLines = 0;     // §6.3 bypass counter
};

/** One run (§5.2.3). Lives in FDreamRunService's 500-record ring. */
struct FDreamRunRecord
{
	int32 RunNumber = 0;
	TArray<FDreamRunId> RunIds;          // one per entry into _run (one for a plain run)
	EDreamRunRoute Route = EDreamRunRoute::Buffer;
	FString Badge;

	FGuid BufferId;
	FString TabTitle;
	FString FilePath;
	FString Filename;                   // the compile filename
	bool bFileTarget = false;           // FDreamRunRequest::bFileTarget (an API run of a file: Run File semantics)
	int32 FirstLine = 1;                // executed range, 1-based buffer lines
	int32 LastLine = 1;
	FString SourceSha256;               // of the executed text (UTF-8, what the runner compiles)
	int64 CodeBytes = 0;                // UTF-8 bytes of the executed text (audit code.bytes, §11.4)
	bool bDirty = false;                // the buffer had unsaved edits at Submit (audit source.dirty)
	/**
	 * SHA-256 of the file on disk (audit source.sha256 of a clean buffer, code.sha256 of Run File, §11.4): the clean
	 * buffer's base version, the bytes DreamRunFile read, or the base version after Run File's step-4 save. Empty: none.
	 */
	FString FileSha256;
	uint64 JournalTicket = 0;
	dream::TextSnapshot SourceSnapshot;  // echo and Location row source (§6.2, §5.4.7)
	uint64 SourceVersion = 0;           // SourceSnapshot.Version(), for MapLine on traceback links (§6.4)
	TOptional<int32> SelectionEndColumn;
	TOptional<int32> SelectionStartColumn;   // (run, PO-06 F2) FDreamRunRequest::SelectionStartColumn

	/** Statement mode (RUN-27): ranges from prepare; counts written by _dream.run_statement before it returns. */
	bool bStatementMode = false;
	TArray<FDreamStatementRange> Statements;
	int32 StatementsStarted = 0;
	int32 StatementsReturned = 0;
	int32 LastEchoedLine = 0;           // each snapshot line is echoed at most once per run (§6.2)

	EDreamNamespaceMode Namespace = EDreamNamespaceMode::Session;
	bool bAutoPrint = true;

	double StartSeconds = 0.0;          // FPlatformTime::Seconds at acceptance
	double EnterSeconds = 0.0;          // at entry into _run
	/**
	 * FPlatformTime::Seconds() when the run finished (§5.2.4): after finish steps 1-3 (EndRun, the transaction end, the
	 * presentation teardown) and the outcome, immediately before EndRunBlock and the step 5 Idle / OnRunFinished broadcast.
	 * A not executed run sets it when its Starting step ended. 0 while the run is active.
	 */
	double EndSeconds = 0.0;
	FDateTime StartUtc;
	FDateTime EndUtc;

	EDreamRunState State = EDreamRunState::Starting;
	EDreamStartingStep StartingStep = EDreamStartingStep::Queued;
	EDreamRunOutcome Outcome = EDreamRunOutcome::None;
	/** "exited (code 3)", "stopped after statement 2 of 5", "ZeroDivisionError: division by zero" (an error: "<Type>: <first
	 *  line of str(exc)>", ruling R1), "SyntaxError: invalid syntax" (not executed), ... Shown as given in the footer. */
	FString Detail;
	/** Where it raised or landed (report_outcome). For a not executed compile error: the compile filename and the 1-based
	 *  buffer line of the SyntaxError (the shell moves the caret there). */
	FDreamRunLocation Location;
	TOptional<int32> ExitCode;          // SystemExit (RUN-18)
	bool bOutcomeReported = false;      // report_outcome was called
	bool bInternalError = false;        // ExecPythonCommandEx returned false (§5.2.4)
	FString CommandResult;              // the engine's traceback text when bInternalError

	FDreamTxnHandle Txn;
	FDreamRunStats Stats;
	bool bStopRequested = false;        // mirrors the supervisor flag for Starting and the record view
	EDreamStopSource StopSource = EDreamStopSource::RunWindowButton;
};

/** A run that was active when the previous session ended (§10.7.4). */
struct FDreamLostRun
{
	FDateTime PreviousSessionUtc;
	/** The journal RUN_BEGIN's run ID (0 when the reader did not pass it); the audit's run_id (§11.4, §12.1.2). */
	FDreamRunId RunId = 0;
	int32 RunNumber = 0;                // the previous session's number
	FGuid TabUuid;
	FString TabTitle;
	EDreamRunRoute Route = EDreamRunRoute::Buffer;
	FDateTime StartUtc;
	bool bPaused = false;
	FString RunLogPath;
};

/** Live values the presentations bind to (§5.4.7). Written on the game thread in the pending call; read by Slate. */
struct FDreamRunLiveStats
{
	EDreamRunState State = EDreamRunState::Idle;
	double ElapsedSeconds = 0.0;
	int32 Line = 0;                     // line at the last pump, 1-based, in the run's filename
	FString InnermostFrame;             // "json/decoder.py:337" when the innermost frame is elsewhere
	bool bBetweenStatements = false;    // the row shows the last started statement's range instead of one line
	int64 ProgressDone = -1;            // -1 = no progress reported
	TOptional<int64> ProgressTotal;     // unset = indeterminate (U12)
	FString ProgressMessage;
	double ProgressRate = 0.0;          // items per second, after 3 samples
	double ProgressEtaSeconds = -1.0;
	int32 ProgressSamples = 0;
	float SlowTaskFraction = -1.f;      // the user's slow task (LRS-04)
	FText SlowTaskMessage;
	SIZE_T TxnBytes = 0;
	SIZE_T TxnBudget = 0;
	int32 SwallowLine = 0;              // "The script caught Stop at line 57" (§5.7.4)
	bool bCleanupRunning = false;
	int32 CleanupLine = 0;
	double CleanupSeconds = 0.0;
	bool bShowInterruptCleanup = false; // Stopping... for 10 s since the first delivery (RT-06)
	double LastNativeSeconds = 0.0;
	int32 LastNativeLine = 0;
	/** Why the last pending call skipped the pump or the pump skipped Slate (§5.4.3); Count = none. */
	EDreamDeferReason LastDeferral = EDreamDeferReason::Count;
	/** The innermost frame at the last pending call is pydevd's (§5.4.3): the Run window's note "Paused in VS Code: Stop
	 *  takes effect when you continue in the IDE." It can first be true in M2, once debugpy is vendored. */
	bool bDebugpyPaused = false;
};

/** U2 and SPEC wording for run states, outcomes, routes and stop sources (bridge slice, DreamRunTypes.cpp). */
namespace DreamRunText
{
	/** "Idle", "Starting", "Running", "Stopping…", "Paused", "Task running" (U2). */
	DREAMEDITOR_API FText StateName(EDreamRunState State);
	/** "ok", "error", "cancelled", "not executed", "lost" (U2). */
	DREAMEDITOR_API FText OutcomeName(EDreamRunOutcome Outcome);
	/** §3.9: a request this long in Starting without reaching an idle OnEndFrame shows the waiting text (proposed 1 s). */
	inline constexpr double StartingWaitNoticeSeconds = 1.0;
	/**
	 * §3.9 "OnEndFrame not reached (blocking modal or long slow task outside a run)": the run has been in Starting for
	 * StartingWaitNoticeSeconds and still waits for an idle end of frame (queued, about to prepare, or its entry refused
	 * or deferred by the RT-02 trio), not for its journal snapshot or Run File save.
	 */
	DREAMEDITOR_API bool IsWaitingForIdle(const FDreamRunRecord& Record, double NowSeconds);
	/** The state text of the status strip and WS-07 for a run in Starting: "Starting", or after 1 s of waiting
	 *  "Starting… waiting for the editor to become idle" (§3.9). */
	DREAMEDITOR_API FText StartingStateText(const FDreamRunRecord& Record, double NowSeconds);
	/** The route keys used in the prepare request, the journal RUN_BEGIN and the run log: "selection", "line", "buffer", "repl", "console", "run_file", "api", ... */
	DREAMEDITOR_API const TCHAR* RouteKey(EDreamRunRoute Route);
	/** ScriptCancelled.source text (§5.7.1, §12.2): "Stop button", "Esc", "Shift+F5", "status bar", "Interrupt cleanup", "API". */
	DREAMEDITOR_API const TCHAR* StopSourceText(EDreamStopSource Source);
	/** The default transaction setting of a route (§5.2.1 RT-08 column), before the user setting and the override. */
	DREAMEDITOR_API bool RouteTransactionDefault(EDreamRunRoute Route);
	/** Statement mode and auto-print are On for Selection, Line, Cell, Buffer, Repl, ConsoleExecutor in Session (§5.2.1),
	 *  and for an API run of a tab in Session; an API run of a file (bFileTarget) follows Run File: off (RUN-27). */
	DREAMEDITOR_API bool RouteUsesStatementMode(EDreamRunRoute Route, EDreamNamespaceMode Namespace, bool bFileTarget = false);
	/** Run File semantics (§5.3.4 sys.path entry and sys.argv, the save at Starting step 4): route RunFile, or an API run
	 *  of a file. */
	DREAMEDITOR_API bool HasRunFileSemantics(EDreamRunRoute Route, bool bFileTarget);
	/** The start of a not executed run's Detail for a failed journal commit and the 2 s timeout (§10.2.5 step 4). The
	 *  footer offers [Run without snapshot] for a Detail that starts with it (DreamOutputRows::IsSnapshotFailure). */
	inline constexpr TCHAR SnapshotFailedPrefix[] = TEXT("Journal snapshot failed");
}
