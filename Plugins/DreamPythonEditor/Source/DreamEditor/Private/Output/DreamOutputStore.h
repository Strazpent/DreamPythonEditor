// SPDX-License-Identifier: Apache-2.0
// The output store (TDD §6.2, D19, OUT-01..OUT-09, OUT-12): MPSC ingest from any thread, the DreamOutput ingest thread
// (split, attribute, links, result cap, LogPython echo, run logs), and the game-thread drain into dream::OutputRing.
// Body: DreamOutputStore.cpp, DreamOutputIngest.cpp, DreamRunLog.cpp, DreamThreadTable.cpp (output slice).
//
// Threads: Push*, SetThreadLabel - any thread, lock-free, never block. Drain, BeginRunBlock, BindRunId, EndRunBlock,
// Clear, every query and the events - game thread. Ingest work runs on the DreamOutput thread only.
#pragma once

#include "CoreMinimal.h"
#include "Output/DreamOutputTypes.h"

class FDreamLogForwarder;

class DREAMEDITOR_API FDreamOutputStore
{
public:
	FDreamOutputStore();
	~FDreamOutputStore();

	/**
	 * Starts the DreamOutput thread. (slice A choice) Called at OnFEngineLoopInitComplete, because StartupModule may start no
	 * thread (B1, §3.4); records pushed earlier (the device registers in StartupModule) wait in the queue.
	 * RunLogRoot is <root>/RunLogs/<session start> (OUT-08); empty disables run logs until SetRunLogRoot.
	 */
	void Start(const FString& RunLogRoot);
	void SetRunLogRoot(const FString& RunLogRoot);
	/**
	 * Module shutdown: removes the drain ticker, flushes, stops and joins the ingest thread and the run-log pipe; safe to
	 * call twice (the destructor calls it). Lifetime order (CR-23): the views, the REPL strip and its history list unbind
	 * from the store in their destructors, and FDreamOutputDevice unregisters in its own, so the Dream tab widgets are
	 * destroyed and the device is removed from GLog and deleted before this runs; the store object is deleted after it.
	 */
	void Shutdown();
	/** OnHandleSystemError: echo everything still queued ignoring the cap, flush open run logs, wait at most 100 ms (§6.3). */
	void EmergencyFlush();

	// ---- Producers (any thread) ----

	void Push(FDreamRawRecord&& Record);
	/** _dream.write / Writer.write: UTF-8 already encoded (lone surrogates replaced). ThreadState is the caller's
	 *  PyThreadState* as an opaque key (the GIL is held there; FDreamRawRecord::ThreadState), 0 when unknown. */
	void PushText(FDreamRunId RunId, EDreamStream Stream, EDreamOutputOrigin Origin, const UTF8CHAR* Utf8, int32 Bytes,
		uint64 ThreadState = 0);
	/**
	 * A Dream line for a run block (compile-error blocks, internal errors, reload reports, notes), split into whole lines.
	 * Forwarding (§6.3): Error lines go to LogPython at Error, as a stock traceback does (T-OUT-018); Info, Warning and
	 * Notice lines go to LogDream. RunNumber 0 puts the line outside every run block (lost runs from recovery, A23).
	 */
	void PushDreamLine(uint32 RunNumber, EDreamStream Stream, const FString& Text);
	/** _dream.thread_label: names a native thread for the badges (D24). NativeThreadId is the OS thread ID
	 *  (threading.get_native_id(), FPlatformTLS::GetCurrentThreadId()). Any thread; the name is copied before the thread
	 *  table's write lock is taken, and lines written after the call carry it. */
	void SetThreadLabel(uint32 NativeThreadId, const FString& Name);

	// ---- Game thread: run blocks and input echo (FDreamRunService, _dream) ----

	/**
	 * At Starting (acceptance): queues a RunHeader record. The drain appends it to the ring as a zero-length line of type
	 * dream::LineType::RunHeader with LineInfo.RunNumber = RunNumber, and FDreamRunSpan::HeaderSeq is that line's sequence;
	 * the header text is rendered by the views from FDreamRunRecord (§6.2 run blocks). The ingest thread opens the run log
	 * and forwards the run's one LogDream header summary from the record as it is now (§6.3 table), so the run service logs
	 * no summary of its own.
	 */
	void BeginRunBlock(uint32 RunNumber);
	/** Before each entry into _run: maps the entry's run ID to its run number on the ingest thread. */
	void BindRunId(FDreamRunId Id, uint32 RunNumber);
	/**
	 * After the last entry returned or the run was abandoned, once the record's Outcome, Detail, EndSeconds, ExitCode and
	 * Txn are final (finish step 4, EndNotExecuted): commits the run's open partial lines, then a zero-length line of type
	 * dream::LineType::RunFooter with LineInfo.RunNumber = RunNumber. FDreamRunSpan::FooterSeq is that line's sequence; it
	 * stays 0 until the footer is drained. The ingest thread forwards the LogDream footer summary after the run's LogPython
	 * lines and closes the run log.
	 */
	void EndRunBlock(uint32 RunNumber);

	/**
	 * _dream.capture_begin / capture_end (§6.1 thread attribution). AliveNativeIds are the native IDs of
	 * threading.enumerate() at capture_begin: a worker thread outside that list that writes before CaptureEnd belongs to
	 * the run ("started during this run"); a listed thread's lines are Background. Game thread.
	 */
	void CaptureBegin(FDreamRunId Id, TConstArrayView<uint64> AliveNativeIds);
	void CaptureEnd(FDreamRunId Id);

	/**
	 * One input echo line (OUT-12), from FDreamRunService::BeginStatement inside run_statement. The stored line has type
	 * dream::LineType::Echo, the raw source line as its text (no '›', prompt or line-number decoration; the view draws
	 * those) and LineInfo.EchoLine = BufferLine; OLF_EchoFirst marks the first line echoed for a statement (lines already
	 * echoed by an earlier statement are not echoed again). The first echo of a statement also pushes FlushPartials so the
	 * run's open game-thread partial lines commit above it (§6.1). bReplPrompt sets OLF_ReplPrompt: the run log writes a
	 * prompt instead of the buffer line number, '>>>' on the statement's own first line (bPromptFirst, OLF_PromptFirst)
	 * and '...' on the others (§6.2); the history view draws no prompt (A226). Echo lines are never forwarded to the
	 * Unreal log.
	 */
	void PushEcho(FDreamRunId Id, int32 BufferLine, FStringView Text, bool bFirstOfStatement, bool bReplPrompt,
		bool bPromptFirst);

	// ---- Game thread: drain and clear ----

	/** Moves processed batches into the ring within BudgetSeconds; broadcasts OnLinesAppended once. Returns lines moved.
	 *  Called by an FTSTicker every idle frame (DREAM_TIMER_EXEMPT: C++ only) and by the pending call before each pump. */
	int32 Drain(double BudgetSeconds);

	/** Toolbar Clear, the context menu, clear_output() and Dream.ClearOutput (§6.4): ClearBefore(EndSeq) keeping the active
	 *  run's header; run logs, records and history untouched. Only queues a Clear record (never blocks), applied in queue
	 *  order, so a mid-run call lands at the next drain. */
	void Clear();

	// ---- Game thread: queries for the views ----

	const dream::OutputRing& GetRing() const;
	uint64 GetFirstSeq() const;
	uint64 GetEndSeq() const;
	bool GetLine(uint64 Seq, dream::LineView& Out) const;
	/** The thread name behind LineInfo.ThreadTag ("" for the game thread). */
	FString GetThreadName(uint32 ThreadTag) const;
	/** The full text of a truncated result line (§6.4 Show all), or empty when the blob was evicted. */
	FString GetResultBlob(uint64 Seq) const;
	/** The provisional open partial line of a run (shown as its block's last line, §6.1). */
	bool GetProvisionalLine(uint32 RunNumber, FString& OutText) const;

	const FDreamRunSpan* FindRunSpan(uint32 RunNumber) const;
	FString GetRunLogPath(uint32 RunNumber) const;
	/** Lines dropped before the ring's start, and the run numbers that lost lines (the eviction marker, §6.2). */
	uint64 GetEvictedLineCount() const;
	TConstArrayView<uint32> GetEvictedRunNumbers() const;

	/**
	 * Shared view visibility (§6.4 toggles, the toolbar's Echo input toggle, _dream.suppress with the §12.2 inversion done
	 * by the bridge). Game thread. SetVisibility broadcasts OnVisibilityChanged when the value changed, and every view of
	 * the store rebuilds at once, the Run window's live run view included, so suppress() inside a run shows at the next
	 * pump (T-OUT-043). Start reads bEcho from UDreamUserSettings::bEchoInput once; the toggle saves that setting itself.
	 */
	const FDreamOutputVisibility& GetVisibility() const;
	void SetVisibility(const FDreamOutputVisibility& Visibility);

	/** The forwarder the ingest thread uses (settings set the echo cap). */
	FDreamLogForwarder& GetForwarder();

	/** (slice A choice) The game-thread echo cap; the ingest thread applies it to the forwarder (which it alone touches).
	 *  It wins over UDreamProjectSettings::EchoCapLinesPerSecond until ClearEchoCap. */
	void SetEchoCap(int32 LinesPerSecond);
	/** Drops a SetEchoCap override: the project setting applies again at the next drain. Game thread. */
	void ClearEchoCap();

	/** The DreamOutput counters (§6.2 "Budgets", TEL-03), also traced to Insights as Dream.Output/*. Any thread. */
	FDreamOutputCounters GetCounters() const;

	/** The device whose startup ring the ingest thread fills (A31). FDreamOutputDevice calls it from its constructor and
	 *  destructor; any thread. */
	void SetOutputDevice(class FDreamOutputDevice* Device);

	/** The Warning and Error lines of the startup ring, once the first OnPythonInitialized has passed (the collapsed
	 *  "Editor startup" block, §6.3, T-OUT-037). Game thread. */
	const TArray<FDreamStartupLine>& GetStartupProblems() const;

	/** FPlatformTime::Seconds() when the store was constructed (dream::LineInfo::TimeMs counts from it, A12). */
	double GetSessionStartSeconds() const;

	DECLARE_EVENT_OneParam(FDreamOutputStore, FOnLinesAppended, const FDreamAppendInfo&);
	FOnLinesAppended& OnLinesAppended() { return LinesAppendedEvent; }
	DECLARE_EVENT(FDreamOutputStore, FOnVisibilityChanged);
	FOnVisibilityChanged& OnVisibilityChanged() { return VisibilityChangedEvent; }

	struct FImpl;

private:
	FOnLinesAppended LinesAppendedEvent;
	FOnVisibilityChanged VisibilityChangedEvent;

	TUniquePtr<FImpl> Impl;
};
