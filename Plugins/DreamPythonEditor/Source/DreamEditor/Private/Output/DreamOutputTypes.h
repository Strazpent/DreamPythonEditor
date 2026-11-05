// SPDX-License-Identifier: Apache-2.0
// Output types shared by the output store, its producers (_dream, FDreamOutputDevice, FDreamRunService) and the views
// (TDD §6.1-§6.4, D19, OUT-12). Header only; no bodies.
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"
#include "dream/OutputRing.h"

/** The stored line types and flags are DreamCore's (dream/OutputRing.h): Stdout..Evaluate, RunHeader, RunFooter, Echo;
 *  flags dream::OLF_*. The stored line struct is dream::LineView (text, LineInfo, link spans). */
using EDreamLineType = dream::LineType;

/** The _dream STREAM_* constants (§6.1, §12.1.1); each maps one to one onto dream::LineType 1..11. */
enum class EDreamStream : uint8
{
	Stdout = 1,
	Stderr = 2,
	Result = 3,
	Notice = 4,
	Info = 5,
	Warning = 6,
	Error = 7,
	Agent = 8,
	Debugger = 9,
	Logpoint = 10,
	Evaluate = 11,
};

inline dream::LineType ToLineType(EDreamStream Stream) { return static_cast<dream::LineType>(static_cast<uint8>(Stream)); }
inline bool IsValidStream(int32 Value) { return Value >= 1 && Value <= 11; }

/** Who produced a record (§6.2 origins). */
enum class EDreamOutputOrigin : uint8
{
	Python,         // _dream.Writer / _dream.write
	UnrealLog,      // the unreal.log* shims (RT-07)
	LogPython,      // captured by FDreamOutputDevice
	Dream,           // FDreamRunService: headers, footers, echo, Dream info lines
	Agent,          // 1.1
};

/** Raw-record kinds that are not text (zero-length control records through the same MPSC queue, §6.2). */
enum class EDreamRawKind : uint8
{
	Text,           // a write; split into lines on the ingest thread
	EchoLine,       // one input echo line, already a whole line (EchoLine, flags EchoFirst / ReplPrompt)
	BindRunId,      // BindRunId(Id, RunNumber): reaches ingest before any record of that entry
	RunHeader,      // BeginRunBlock
	RunFooter,      // EndRunBlock
	CaptureBegin,   // _dream.capture_begin: alive native ids in Text as decimal list (slice A choice)
	CaptureEnd,
	FlushPartials,  // commit the run's open game-thread partial lines before an echo, flagged Partial | EchoSplit (§6.1)
	Clear,          // FDreamOutputStore::Clear (applied in order with writes)
	StartupDone,    // (slice A choice) the first OnPythonInitialized: later LogPython lines are no "Editor startup" lines (§6.3)
};

/**
 * (slice A choice) What a RunHeader / RunFooter record carries to the ingest thread, built on the game thread from the
 * FDreamRunRecord: the run-log header and end lines (OUT-08, §6.2 format) and the LogDream summary (§6.3 table). The ring
 * line itself stays zero-length; the views render the header and footer from the record (§6.2 run blocks).
 */
struct FDreamRunBlockInfo
{
	FString RouteKey;                   // DreamRunText::RouteKey
	FString RunLogHeader;               // "# run #22 route=buffer source=... namespace=session start=... engine=... plugin=..."
	FString LogSummary;                 // "Run #22 · Untitled-3 · lines 12-20 · Session" / "Run #22 ok · 0.42 s"
	ELogVerbosity::Type SummaryVerbosity = ELogVerbosity::Log;
	FString OutcomeKey;                 // footer: the U2 outcome name ("ok", "not executed", ...)
	double DurationSeconds = 0.0;       // footer
	bool bAlwaysWriteRunLogs = true;    // UDreamUserSettings::bAlwaysWriteRunLogs at the header
	bool bReplPrompts = false;          // REPL and console runs: echo lines are written with >>> / ... (§6.2 format)
};

/** One producer record (§6.2). Built on any thread and pushed to the MPSC queue; about 1 µs per write. */
struct FDreamRawRecord
{
	EDreamRawKind Kind = EDreamRawKind::Text;
	FDreamRunId RunId = 0;               // 0 for device records: ingest attributes them by thread
	uint32 RunNumber = 0;               // control records carry the run number directly
	EDreamStream Stream = EDreamStream::Stdout;
	EDreamOutputOrigin Origin = EDreamOutputOrigin::Python;
	uint16 Flags = 0;                   // dream::OutputLineFlag bits known at the producer (EchoFirst, ReplPrompt, FromUnrealLog)
	uint32 NativeThreadId = 0;          // FPlatformTLS::GetCurrentThreadId()
	/** The writing thread's PyThreadState* as an opaque key, set by the Python producers (_dream.write, the Writers, the
	 *  log shims: the GIL is held there); 0 when unknown (FDreamOutputDevice, Dream's own lines). FDreamThreadTable keys a
	 *  thread by (NativeThreadId, ThreadState), so a native ID the OS reused for a new thread is detected (§6.1). */
	uint64 ThreadState = 0;
	int32 EchoLine = 0;                 // EchoLine records: 1-based buffer line
	double Time = 0.0;                  // FPlatformTime::Seconds()
	TArray<UTF8CHAR> Text;              // UTF-8, not terminated
	/** RunHeader and RunFooter records only (slice A choice). */
	TSharedPtr<const FDreamRunBlockInfo, ESPMode::ThreadSafe> Block;
};

/** One line FDreamLogForwarder echoes to the Unreal log (§6.3). */
struct FDreamForwardLine
{
	FName Category;                     // LogPython or LogDream (FDreamLogForwarder::MapLine)
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Text;                       // the stored text: no "# Result: " prefix, no terminator
	uint32 RunNumber = 0;
	/** The stored type and flags: Result lines get the "# Result: " prefix in the log, and an OLF_EchoSplit line is held
	 *  and joined with the next line of the same Type and RunNumber, or forwarded alone at run end (§6.3 rule 1). */
	dream::LineType Type = dream::LineType::Stdout;
	uint16 Flags = 0;
};

/**
 * The DreamOutput counters (TDD §6.2 "Budgets": queue bytes, lines/s, drain ms, view-queue backlog, echo-suppressed lines
 * and dropped lines; TEL-03). The same values go to Unreal Insights as the counters Dream.Output/* (§13.1).
 * FDreamOutputStore::GetCounters reads them on any thread.
 */
struct FDreamOutputCounters
{
	int64 QueueBytes = 0;               // bytes waiting in the MPSC queue now
	int32 LinesPerSecond = 0;           // lines the ingest thread committed over its last 1 s window
	double DrainMs = 0.0;               // the last game-thread drain
	int64 ViewBacklogLines = 0;         // processed lines waiting in the view queue for a drain
	int64 EchoSuppressedLines = 0;      // lines the echo cap kept out of the Unreal log, session total (§6.3 rule 5)
	int64 DroppedLines = 0;             // writes dropped at the 64 MB queue cap plus lines the view did not get, session total
	int32 EchoCap = 0;                  // the cap the forwarder applies now, lines per second (0 = unlimited)
	int64 LinesTotal = 0;               // lines the ingest thread committed, session total
};

/** What a drain appended, for OnLinesAppended (§6.2). */
struct FDreamAppendInfo
{
	uint64 FirstSeq = 0;
	uint64 EndSeq = 0;                  // one past the last appended
	bool bCleared = false;              // a Clear was applied in this drain
	bool bEvicted = false;
	uint64 NewFirstSeq = 0;             // the ring's FirstSeq after this drain
};

/** A line of the startup ring (§6.3), read by the environment doctor (M4, §9.5) and the "Editor startup" block. */
struct FDreamStartupLine
{
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Text;
	double Time = 0.0;
};

/**
 * View visibility of the output kinds (§6.4 toggles, suppress(), Echo input). Shared by every view of the store, the Run
 * window's live run view included, so suppress(echo=True) in a run hides echo lines at the next pump (T-OUT-043).
 * The fields are visibilities (true shows); _dream.suppress speaks the §12.2 polarity (True suppresses) and inverts.
 */
struct FDreamOutputVisibility
{
	bool bResults = true;               // result lines
	bool bInfo = true;                  // info lines (unreal.log, LogPython info)
	bool bWarnings = true;
	bool bErrors = true;
	bool bEcho = true;                  // the Echo input toggle (OUT-12); default on
	bool bBackground = true;
	bool bStdout = true;
	bool bStderr = true;
	bool bNotice = true;

	bool operator==(const FDreamOutputVisibility&) const = default;
};

/** The header and footer span of one run block (§6.2 FRunSpan). Game thread (FDreamOutputStore::FindRunSpan). */
struct FDreamRunSpan
{
	/** The sequence of the run's zero-length RunHeader ring line (BeginRunBlock); after a Clear that kept the open run,
	 *  the sequence of the header line the Clear appended again. */
	uint64 HeaderSeq = 0;
	/** The sequence of the run's zero-length RunFooter ring line (EndRunBlock); 0 until that line is drained. */
	uint64 FooterSeq = 0;
	int32 LineCount = 0;
	int64 Bytes = 0;
	int32 DroppedLines = 0;
	FString RunLogPath;
	/** "Run log could not be written: <error>" (§6.7); empty while the log is fine. */
	FString RunLogError;
	/** LogPython lines FDreamOutputDevice captured for this run (the §6.3 bypass counter; footer warning above 10,000).
	 *  The footer uses the larger of this and FDreamRunStats::BypassLogPythonLines, so the run service may leave that 0. */
	int32 BypassLines = 0;
};
