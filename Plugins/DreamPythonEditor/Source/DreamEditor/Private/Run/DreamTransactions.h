// SPDX-License-Identifier: Apache-2.0
// One undo step per run (TDD §5.9.1-§5.9.5, U3, U9, RT-08, RUN-07, RUN-22). Game thread. Body: DreamTransactions.cpp
// (bridge slice). Task steps and the walk-back (§5.9.6) are M3.
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"

/** What Undo Last Run would undo (§5.9.5 checks 4-5). */
struct FDreamUndoPreview
{
	FText Title;
	int32 RunNumber = 0;                // the run Undo Last Run picked ("Run #22 is partly undoable...", §5.9.5 check 4)
	int32 ObjectCount = 0;
	SIZE_T Bytes = 0;
	bool bPartial = false;              // check 4: needs the inline confirm
	bool bOverBudget = false;           // check 5: "This record is over the undo budget..."
};

class DREAMEDITOR_API FDreamTransactions
{
public:
	FDreamTransactions();
	~FDreamTransactions();

	/**
	 * §5.9.7's "OnPostEngineInit" step, called by the module at OnFEngineLoopInitComplete (ARCHITECTURE.md §6): the module
	 * loads at PostEngineInit, after FCoreDelegates::OnPostEngineInit has broadcast (LaunchEngineLoop.cpp:4835 then
	 * :4863), so a binding to that delegate would never fire. Captures the standard GEditor->Trans, reads
	 * [Undo] UndoBufferSize from GEditorPerProjectIni and binds UTransBuffer::OnTransactionStateChanged.
	 */
	void Initialize();
	void Shutdown();

	/**
	 * Entry step 6 (§5.9.2 Begin): runs without a transaction when IsTransactionActive() is already true (note "undo unavailable:
	 * another edit was in progress") or BeginTransaction returns INDEX_NONE; otherwise records the TransactionId and title.
	 * The run record's route default, the per-tab toggle and the U6 override decide whether it is called (FDreamRunService).
	 * Also starts counting import, reimport and asset-rename broadcasts for the run's user-code window (§5.9.2).
	 */
	FDreamTxnHandle BeginRunTransaction(const FDreamRunRecord& Rec);

	/** Every pump (§5.4.2 step 7): DataSize() of the open transaction; 50% warning; at 100% EndTransaction and Partial (§5.9.4). */
	void OnPump(FDreamTxnHandle& H);

	/** The run's user-code window opened or closed (from _dream.user_code_begin / user_code_done through FDreamRunService):
	 *  import, reimport and rename broadcasts count only inside it (§5.9.2). A second window of the same run (the
	 *  runner's sys.excepthook call, §5.3.7) counts into the same run; EndRunTransaction resets the counters. */
	void OnUserCodeWindow(bool bActive);

	/** Entry step 6a, for every run with or without its own transaction: records whether a foreign transaction is already
	 *  open, so the dangling check at End never closes another tool's transaction (§5.9.2), and restarts the import and
	 *  rename counters. FDreamRunService calls it before BeginRunTransaction. */
	void OnRunEntry();

	/** Finish step 2: End, the dangling check (at most 16), empty, transient, imports and renames notes, Cleared reset (§5.9.2-§5.9.3). */
	void EndRunTransaction(FDreamTxnHandle& H, FDreamRunRecord& Rec);

	/** §5.9.5 checks 0-5, in order; OutReason holds the refusal text. Callers go through FDreamRunService::CanUndoRun. */
	bool CanUndoLastRun(const FDreamRunRecord& Rec, FText& OutReason, FDreamUndoPreview& OutPreview) const;
	/** GEditor->UndoTransaction() and Status = Undone. Called only by FDreamRunService::UndoRun, which owns the mutable
	 *  history record and has confirmed a Partial record. */
	bool UndoLastRun(FDreamRunRecord& Rec);

	/** The [Undo] UndoBufferSize budget in bytes (default 256 MB). */
	SIZE_T GetBudgetBytes() const;
	/** Test hook (P3-run-03): the budget the next BeginRunTransaction gives its run, so a small record reaches 100% (§5.9.4).
	 *  Only Dream's watch changes; the engine's own eviction budget stays. */
	void SetBudgetBytesForTest(SIZE_T Bytes);

private:
	/** "changes to PIE objects are not undoable": the engine discards a transaction that holds a PIE object at End. */
	static void NotePieObjects(FDreamTxnHandle& H, const class FTransaction& Transaction);

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
