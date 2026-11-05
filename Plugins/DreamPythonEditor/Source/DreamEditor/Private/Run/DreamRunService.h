// SPDX-License-Identifier: Apache-2.0
// One queue, one start point, one run at a time (TDD §5.1, §5.2, INV-1..INV-7). Game thread unless marked.
// Body: DreamRunService.cpp (bridge slice). The only owner of an FCoreDelegates::OnEndFrame handler (§2.5 rule 9).
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"
#include "Templates/SharedPointer.h"

class FDreamPython;
struct FDreamUndoPreview;

class DREAMEDITOR_API FDreamRunService : public TSharedFromThis<FDreamRunService>
{
public:
	static constexpr int32 HistoryCapacity = 500;          // RUN-12
	static constexpr double SnapshotTimeoutSeconds = 2.0;  // §10.2.5 step 4 (proposed)

	FDreamRunService();
	~FDreamRunService();

	/** StartupModule. Binds nothing to OnEndFrame until the first ArmEndFrame. */
	void Initialize();
	/** ShutdownModule: removes the OnEndFrame handler; a run cannot be active (Python is finalized, §3.8). */
	void Shutdown();

	// ---- Requests (any route; Slate handlers included: Submit never runs Python, D14(1)) ----

	/**
	 * Accepts and numbers a request, or refuses it with a reason (not Idle, Python unavailable, D16 states).
	 * On acceptance: record in state Starting, FDreamOutputStore::BeginRunBlock(N), FDreamRuntimeProblems::OnRunStarted,
	 * OnRunStateChanged, and the one-shot OnEndFrame armed. In state Cold it calls FDreamPython::RequestStart(RunRequest).
	 */
	FDreamSubmitResult Submit(FDreamRunRequest&& Request);
	/** The same checks without submitting (console executor, API, menu enable state). */
	bool CanSubmit(EDreamRunRoute Route, FText& OutReason) const;
	/** Re-run the recorded snapshot of a run as a new numbered run with the same route options (RUN-12). */
	FDreamSubmitResult Rerun(int32 RunNumber);
	/** The run block's [Run without snapshot] after "Journal snapshot failed" (§10.2.5 step 4): Rerun with step 3 skipped;
	 *  FDreamAudit writes run.snapshot_skipped for the new run (§11.4, PER-09). */
	FDreamSubmitResult RerunWithoutSnapshot(int32 RunNumber);
	/** The footer's [Run in transaction] (RT-08, §5.9.4, U6): Rerun with the transaction override on, for a run that had
	 *  none (Run File's default is off). */
	FDreamSubmitResult RerunWithTransaction(int32 RunNumber);
	/** [Run in transaction] is offered for a finished, numbered run in the history that executed without a transaction. */
	bool CanRerunInTransaction(int32 RunNumber) const;

	/** Ignored unless Id is the active run. Starting: the run ends not executed. Running: FRunSupervisor::RequestStop. */
	bool RequestStop(FDreamRunId Id, EDreamStopSource Source);
	/**
	 * Stops whatever is active without knowing its id. The one path of every Stop control (the Run window's Stop, the
	 * pre-processor's Shift+F5 and Esc, WS-07, the toolbar): Starting ends not executed at the next OnEndFrame; Running
	 * forwards to FRunSupervisor::RequestStop(ActiveId, Source). Off the game thread only the supervisor's atomics move.
	 */
	bool RequestStopActive(EDreamStopSource Source);
	/** The Run window's Interrupt cleanup (RT-06): FRunSupervisor::RequestInterruptCleanup(ActiveId) for a Running run. */
	void RequestInterruptCleanup();

	// ---- State ----

	EDreamRunState GetState() const;
	/** True from entry to finish, while paused and during a task step: the RT-01 check of every ticker client. */
	bool IsRunActive() const;
	/** True only inside HandleEndFrame (§3.6 EnterRun assertion). */
	bool IsInEndFrameStart() const;
	/** +1 at every run end, Reset Session and Python restart (§7 caches). */
	uint32 GetRunGeneration() const;
	/** The active run's current entry id; 0 when none (_dream.current_run_id). Any thread (atomic). */
	FDreamRunId GetActiveRunId() const;
	/** The running run's route is agent or bridge (1.1), the only runs _dream.write accepts STREAM_AGENT for (§6.1). Any
	 *  thread (atomic); false between runs and for every 1.0 route. */
	bool IsActiveRunAgentOrBridge() const;

	/**
	 * B40 (A56, P2-run-02): whether a tab run of Route opens a transaction when the request carries no U6 override. A tab
	 * whose "One undo step per run" toggle the user set decides with it; an untouched tab follows the per-route setting it
	 * is seeded from (selection, line and cell runs bTransactionSelectionAndLine, buffer runs bTransactionBuffer); Run File
	 * keeps its own default ANDed with the toggle (SHL-05). Game thread.
	 */
	static bool ResolveTabTransaction(const struct FDreamBufferViewState& View, EDreamRunRoute Route);
	/**
	 * B40, what the tab's "One undo step per run" toggle shows, so it never claims what a run will not do (A56): its value
	 * once set (or without settings); untouched, the per-route setting when the selection/line/cell and buffer-run settings
	 * agree; unset (a mixed state) when they differ. Game thread.
	 */
	static TOptional<bool> TabToggleShown(const struct FDreamBufferViewState& View);
	/** B40: the toggle's tooltip, which names what buffer runs and selection, line and cell runs do while it is untouched. */
	static FText TabToggleTooltip(const struct FDreamBufferViewState& View);

	const FDreamRunRecord* GetActiveRecord() const;
	/** Newest last, at most HistoryCapacity. */
	TConstArrayView<FDreamRunRecord> GetHistory() const;
	const FDreamRunRecord* FindRecord(int32 RunNumber) const;
	const FDreamRunRecord* FindRecordByRunId(FDreamRunId Id) const;
	/** The last finished run (WS-07 "last run ok 0.4 s", status strip). */
	const FDreamRunRecord* GetLastFinished() const;

	/** Crash recovery (§10.7.4): adds a lost block and history entry; takes no run number. */
	void RecordLostRun(const FDreamLostRun& Lost);

	// ---- Undo Last Run (§5.9.5, RUN-22): toolbar, Ctrl+Alt+Z in Dream widgets, the run header's "Undo this run" ----

	/** RunNumber 0 = the newest finished run with a transaction. FDreamTransactions::CanUndoLastRun checks 0-5 on the record. */
	bool CanUndoRun(int32 RunNumber, FText& OutReason, FDreamUndoPreview& OutPreview) const;
	/**
	 * Re-runs the checks, then FDreamTransactions::UndoLastRun on the history record (status Undone) and broadcasts
	 * OnRunStateChanged for the footer chip. A Partial record needs bConfirmedPartial (check 4's inline confirm).
	 * Refused while a run is active.
	 */
	bool UndoRun(int32 RunNumber, bool bConfirmedPartial, FText& OutReason);

	// ---- Called from _dream (bridge, GIL held, game thread) ----

	/**
	 * run_statement steps 2-3: validates the active run and Index, records it as started, and pushes its echo lines
	 * (each snapshot line at most once per run, stopping at SelectionEndColumn on the last line, §6.2). False with
	 * OutError (and bOutIsValueError) when the run is not active or Index is out of range.
	 */
	bool BeginStatement(FDreamRunId Id, int32 Index, FString& OutError, bool& bOutIsValueError);
	/** run_statement step 5: records Index + 1 as returned. */
	void EndStatement(FDreamRunId Id, int32 Index);
	/** statement_counts: the record's (started, returned); (0, 0) outside statement mode. False when Id is not the active
	 *  entry (the caller raises RuntimeError). */
	bool GetStatementCounts(FDreamRunId Id, int32& OutStarted, int32& OutReturned) const;
	/** report_outcome. False when Id is not the active entry (the caller raises RuntimeError). */
	bool ReportOutcome(FDreamRunId Id, EDreamRunOutcome Outcome, const FString& Detail, const TOptional<FDreamRunLocation>& Location);
	/** enqueue_run: route Api; refused inside a run (INV-5). Returns the run number or 0 with OutError. */
	int32 EnqueueApiRun(const FString& Target, TOptional<EDreamNamespaceMode> Namespace, TOptional<bool> Transaction, FText& OutError);
	/** run_statement's checks without side effects: the active entry (else not a value error) and the prepared range. */
	bool ValidateStatement(FDreamRunId Id, int32 Index, FString& OutError, bool& bOutIsValueError) const;
	/** user_code_begin / user_code_done: the run's user-code window, for the import and rename counters (§5.9.2). */
	void OnUserCodeWindow(FDreamRunId Id, bool bActive);

	// ---- Called by FDreamPython ----

	/** Arms the one-shot OnEndFrame handler (boot, Enable Python, Reset Session ride it; slice A choice). */
	void ArmEndFrame();
	/** Reset Session ran (RT-15, §5.3.3): +1 run generation for the §7 caches. */
	void NotifySessionReset();
	/** A new Python generation was initialized (§3.5): +1 run generation. */
	void NotifyPythonRestarted();
	/** OnPythonShutdown while a run is active (§3.8 step 1, pathological): the run ends lost when control returns. */
	void NotifyPythonShutdownDuringRun();

	// ---- Events (game thread) ----

	DECLARE_EVENT_OneParam(FDreamRunService, FOnRunEvent, const FDreamRunRecord&);
	/** Every state change of the active run (Starting, Running, Stopping…, Idle). */
	FOnRunEvent& OnRunStateChanged() { return RunStateChangedEvent; }
	/** Finish step 5. The record has its outcome, detail, location and transaction info. */
	FOnRunEvent& OnRunFinished() { return RunFinishedEvent; }

private:
	/** The one-shot OnEndFrame handler: FDreamPython::ProcessEndFrame, then the Starting steps, entry and finish. */
	void HandleEndFrame();
	/** Advances the active request one Starting step; returns true when it entered or ended the run this frame. */
	bool AdvanceStarting(FDreamRunRecord& Rec);
	void EnterAndFinish(FDreamRunRecord& Rec);
	/** Finish steps 1-5 of §5.2.4. */
	void FinishRun(FDreamRunRecord& Rec, bool bExecOk, const FString& CommandResult);
	void EndNotExecuted(FDreamRunRecord& Rec, const FString& Detail, const TOptional<FDreamRunLocation>& Location);

	// The Starting steps of §5.1.2 (slice A choice A16); each returns true when it ended or entered the run.
	bool StepPrepare(FDreamRunRecord& Rec);
	bool StepAwaitSnapshot(FDreamRunRecord& Rec);
	bool StepAwaitSave(FDreamRunRecord& Rec);
	bool StepEnter(FDreamRunRecord& Rec);
	/** Python is not Ready at OnEndFrame: wait for the boot, or end the run not executed when Python cannot run. */
	void HandlePythonNotReady(FDreamRunRecord& Rec);
	/** Moves the finished active record into the history ring and broadcasts the finish events (§5.2.4 step 5). */
	void RetireActive();
	FDreamSubmitResult RerunInternal(int32 RunNumber, bool bSkipSnapshot, TOptional<bool> TransactionOverride);

	FOnRunEvent RunStateChangedEvent;
	FOnRunEvent RunFinishedEvent;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
