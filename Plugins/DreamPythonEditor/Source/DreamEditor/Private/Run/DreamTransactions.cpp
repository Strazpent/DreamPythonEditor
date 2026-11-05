// SPDX-License-Identifier: Apache-2.0
// One undo step per run (TDD §5.9.1-§5.9.5, U3, U9, RT-08, RUN-07, RUN-22). Game thread. The run's transaction is a
// normal editor transaction titled "Dream: <tab> #N"; errors and cancels keep it (no rollback, U9).
#include "Run/DreamTransactions.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ITransaction.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Output/DreamOutputStore.h"
#include "Subsystems/ImportSubsystem.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamTransactionsPrivate
{
	constexpr int32 MaxDanglingCloses = 16;     // §5.9.2 (proposed)

	FText JoinNote(const FText& Existing, const FText& Addition)
	{
		if (Existing.IsEmpty())
		{
			return Addition;
		}
		return FText::Format(LOCTEXT("NoteJoin", "{0} · {1}"), Existing, Addition);
	}

	FText FormatMegabytes(SIZE_T Bytes)
	{
		FNumberFormattingOptions Options;
		Options.MaximumFractionalDigits = 0;
		return FText::AsNumber(static_cast<double>(Bytes) / (1024.0 * 1024.0), &Options);
	}

	/** "Dream: <tab> #N", "Dream: REPL #N", "Dream: Console #N", "Dream: <file> #N" (§5.9.1). */
	FText MakeTitle(const FDreamRunRecord& Rec)
	{
		FString Name = Rec.TabTitle;
		if (Name.IsEmpty())
		{
			Name = !Rec.FilePath.IsEmpty() ? FPaths::GetCleanFilename(Rec.FilePath) : Rec.Filename;
		}
		return FText::Format(LOCTEXT("TransactionTitle", "Dream: {0} #{1}"), FText::FromString(Name),
			FText::AsNumber(Rec.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping()));
	}

	void PushRunLine(int32 RunNumber, EDreamStream Stream, const FText& Text)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetOutputStore().PushDreamLine(static_cast<uint32>(RunNumber), Stream, Text.ToString());
		}
	}
}

struct FDreamTransactions::FImpl
{
	/** GEditor->Trans as captured at initialization: check 1 of §5.9.5 compares against it. */
	TWeakObjectPtr<UTransactor> StandardTrans;
	SIZE_T BudgetBytes = SIZE_T(256) * 1024 * 1024;

	TWeakObjectPtr<UTransBuffer> BoundBuffer;
	FDelegateHandle StateChangedHandle;
	FDelegateHandle BufferChangedHandle;
	FDelegateHandle MapOpenedHandle;
	FDelegateHandle PreDeleteHandle;
	FDelegateHandle PostImportHandle;
	FDelegateHandle ReimportHandle;
	FDelegateHandle RenamedHandle;

	// The open run's watch (one run at a time, D14(4)).
	bool bWatching = false;
	FGuid WatchedId;
	bool bCancelSeen = false;
	/** Our transaction's cancel was the last state event: an empty, inactive queue that follows is UTransBuffer::Reset's. */
	bool bAwaitReset = false;
	/** Our transaction ended (a 100% end, §5.9.4) and its record is in the queue: while it holds, outside GIsTransacting,
	 *  only UTransBuffer::Reset leaves the queue empty and inactive (a Begin's budget eviction leaves the new open entry; a
	 *  transient pop and a cancel put the evicted entries back), and a reset of an inactive buffer raises no cancel event.
	 *  UTransBuffer::Undo(bCanRedo false) empties it too, but broadcasts under GIsTransacting, where Reset never runs
	 *  (ensure(!GIsTransacting)); a script's Sequencer export does that after a Begin evicted our record (P4-run-01). That
	 *  undo clears the flag once our record is out of the queue, so a transient End on the queue it emptied (which inserts
	 *  nothing back) is not read as Reset either (P5-txo-01). */
	bool bOurEnded = false;
	/** §5.9.3: the undo buffer was reset during the run (EditorTransaction.cpp Reset: Cancel(0) when active, then Empty,
	 *  then OnUndoBufferChanged with ActiveCount 0), while our transaction was open or after its 100% end. Any reset, not
	 *  only a map open or an asset delete. */
	bool bResetSeen = false;
	bool bMapOpened = false;
	bool bAssetsDeleted = false;
	bool bForeignActiveAtBegin = false;
	std::atomic<bool> bUserCodeWindow{ false };
	std::atomic<int32> Imports{ 0 };
	std::atomic<int32> Renames{ 0 };

	UTransactor* GetTrans() const
	{
		return GEditor != nullptr ? GEditor->Trans.Get() : nullptr;
	}

	void OnTransactionStateChanged(const FTransactionContext& Context, ETransactionStateEventType Event)
	{
		if (!bWatching)
		{
			return;
		}
		// Reset broadcasts our cancel and then the emptied buffer with no state event between them; a script's own Begin, End
		// or Undo after its cancel broadcasts one (TransactionStarted, Finalized, UndoRedoStarted).
		const bool bOurCancel = Event == ETransactionStateEventType::TransactionCanceled && Context.TransactionId == WatchedId;
		bCancelSeen = bCancelSeen || bOurCancel;
		bAwaitReset = bOurCancel;
		// PostTransactionFinalized, not TransactionFinalized: a transient record's pop and its OnUndoBufferChanged come
		// between the two (EditorTransaction.cpp End), and a popped record is not ours to watch.
		if (Event == ETransactionStateEventType::PostTransactionFinalized && Context.TransactionId == WatchedId)
		{
			const UTransBuffer* Buffer = BoundBuffer.Get();
			bOurEnded = Buffer != nullptr && Buffer->FindTransactionIndex(WatchedId) != INDEX_NONE;
		}
	}

	void OnUndoBufferChanged()
	{
		if (!bWatching)
		{
			return;
		}
		UTransBuffer* Buffer = BoundBuffer.Get();
		// Under GIsTransacting only an undo without redo broadcasts (EditorTransaction.cpp Undo), never Reset
		// (ensure(!GIsTransacting)), so it is no reset (P4-run-01). Undo needs ActiveCount 0, so nothing is parked in
		// RemovedTransactions and the queue is the whole truth: once our record is gone, no later empty queue is its reset
		// (a transient End on the emptied queue, P5-txo-01).
		if (GIsTransacting)
		{
			bOurEnded = bOurEnded && Buffer != nullptr && Buffer->FindTransactionIndex(WatchedId) != INDEX_NONE;
			return;
		}
		// Cancel's own broadcast comes while ActiveCount > 0, so it never matches; Reset's comes after Empty with ActiveCount 0.
		if ((bAwaitReset || bOurEnded) && Buffer != nullptr && Buffer->GetQueueLength() == 0 && !Buffer->IsActive())
		{
			bResetSeen = true;
			bAwaitReset = false;
		}
	}
};

FDreamTransactions::FDreamTransactions()
	: Impl(MakeUnique<FImpl>())
{
}

FDreamTransactions::~FDreamTransactions() = default;

void FDreamTransactions::Initialize()
{
	check(IsInGameThread());
	if (GEditor == nullptr)
	{
		return;
	}
	Impl->StandardTrans = GEditor->Trans.Get();

	// The budget as the engine reads it (EditorServer.cpp: [Undo] UndoBufferSize in MB, default 256).
	int32 BufferMegabytes = 256;
	if (GConfig == nullptr || !GConfig->GetInt(TEXT("Undo"), TEXT("UndoBufferSize"), BufferMegabytes, GEditorPerProjectIni))
	{
		BufferMegabytes = 256;
	}
	Impl->BudgetBytes = SIZE_T(FMath::Max(BufferMegabytes, 1)) * 1024 * 1024;

	if (UTransBuffer* Buffer = Cast<UTransBuffer>(GEditor->Trans))
	{
		Impl->BoundBuffer = Buffer;
		Impl->StateChangedHandle = Buffer->OnTransactionStateChanged().AddRaw(Impl.Get(), &FImpl::OnTransactionStateChanged);
		Impl->BufferChangedHandle = Buffer->OnUndoBufferChanged().AddRaw(Impl.Get(), &FImpl::OnUndoBufferChanged);
	}
	Impl->MapOpenedHandle = FEditorDelegates::OnMapOpened.AddLambda([Watch = Impl.Get()](const FString&, bool)
	{
		Watch->bMapOpened = Watch->bWatching || Watch->bMapOpened;
	});
	Impl->PreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddLambda([Watch = Impl.Get()](const TArray<UObject*>&)
	{
		Watch->bAssetsDeleted = Watch->bWatching || Watch->bAssetsDeleted;
	});
	if (UImportSubsystem* Imports = GEditor->GetEditorSubsystem<UImportSubsystem>())
	{
		Impl->PostImportHandle = Imports->OnAssetPostImport.AddLambda([Watch = Impl.Get()](UFactory*, UObject*)
		{
			if (Watch->bUserCodeWindow.load())
			{
				++Watch->Imports;
			}
		});
		Impl->ReimportHandle = Imports->OnAssetReimport.AddLambda([Watch = Impl.Get()](UObject*)
		{
			if (Watch->bUserCodeWindow.load())
			{
				++Watch->Imports;
			}
		});
	}
	if (IAssetRegistry* Registry = IAssetRegistry::Get())
	{
		Impl->RenamedHandle = Registry->OnAssetRenamed().AddLambda([Watch = Impl.Get()](const FAssetData&, const FString&)
		{
			if (Watch->bUserCodeWindow.load())
			{
				++Watch->Renames;
			}
		});
	}
}

void FDreamTransactions::Shutdown()
{
	if (UTransBuffer* Buffer = Impl->BoundBuffer.Get())
	{
		Buffer->OnTransactionStateChanged().Remove(Impl->StateChangedHandle);
		Buffer->OnUndoBufferChanged().Remove(Impl->BufferChangedHandle);
	}
	FEditorDelegates::OnMapOpened.Remove(Impl->MapOpenedHandle);
	FEditorDelegates::OnAssetsPreDelete.Remove(Impl->PreDeleteHandle);
	if (GEditor != nullptr)
	{
		if (UImportSubsystem* Imports = GEditor->GetEditorSubsystem<UImportSubsystem>())
		{
			Imports->OnAssetPostImport.Remove(Impl->PostImportHandle);
			Imports->OnAssetReimport.Remove(Impl->ReimportHandle);
		}
	}
	if (IAssetRegistry* Registry = IAssetRegistry::Get())
	{
		Registry->OnAssetRenamed().Remove(Impl->RenamedHandle);
	}
	Impl->BoundBuffer.Reset();
}

SIZE_T FDreamTransactions::GetBudgetBytes() const
{
	return Impl->BudgetBytes;
}

void FDreamTransactions::SetBudgetBytesForTest(SIZE_T Bytes)
{
	Impl->BudgetBytes = Bytes;
}

void FDreamTransactions::OnUserCodeWindow(bool bActive)
{
	Impl->bUserCodeWindow = bActive;
}

void FDreamTransactions::OnRunEntry()
{
	check(IsInGameThread());
	// Every run, with or without its own transaction: the dangling check at End closes only what this run opened.
	Impl->bForeignActiveAtBegin = GEditor != nullptr && GEditor->IsTransactionActive();
	Impl->Imports = 0;
	Impl->Renames = 0;
}

void FDreamTransactions::NotePieObjects(FDreamTxnHandle& H, const FTransaction& Transaction)
{
	if (!H.bPieObjects && Transaction.ContainsPieObjects())
	{
		H.bPieObjects = true;
		H.Note = DreamTransactionsPrivate::JoinNote(H.Note, LOCTEXT("PieNotUndoable", "changes to PIE objects are not undoable"));
	}
}

FDreamTxnHandle FDreamTransactions::BeginRunTransaction(const FDreamRunRecord& Rec)
{
	check(IsInGameThread());
	using namespace DreamTransactionsPrivate;

	FDreamTxnHandle H;
	H.Title = MakeTitle(Rec);
	H.Budget = Impl->BudgetBytes;
	H.Status = EDreamTxnStatus::NotStarted;

	Impl->bWatching = true;
	Impl->WatchedId.Invalidate();
	Impl->bCancelSeen = false;
	Impl->bAwaitReset = false;
	Impl->bOurEnded = false;
	Impl->bResetSeen = false;
	Impl->bMapOpened = false;
	Impl->bAssetsDeleted = false;
	Impl->Imports = 0;
	Impl->Renames = 0;

	UTransactor* Trans = Impl->GetTrans();
	if (GEditor == nullptr || Trans == nullptr)
	{
		H.Note = LOCTEXT("NoUndoBuffer", "undo unavailable: the editor has no undo buffer");
		return H;
	}
	// Nesting would merge the run into someone else's record (§5.9.2, proposed).
	if (GEditor->IsTransactionActive())
	{
		Impl->bForeignActiveAtBegin = true;
		H.Note = LOCTEXT("ForeignEditActive", "undo unavailable: another edit was in progress");
		return H;
	}
	const int32 Index = GEditor->BeginTransaction(TEXT("Dream"), H.Title, nullptr);
	if (Index == INDEX_NONE)
	{
		H.Note = LOCTEXT("CannotTransact", "undo unavailable: the editor cannot record undo while loading");
		return H;
	}
	// While active, the open transaction is the last entry (TransBuffer.h).
	H.QueueLengthAtBegin = Trans->GetQueueLength();
	if (const FTransaction* Open = Trans->GetTransaction(H.QueueLengthAtBegin - 1))
	{
		H.TransactionId = Open->GetContext().TransactionId;
	}
	Impl->WatchedId = H.TransactionId;
	H.Status = EDreamTxnStatus::Open;
	return H;
}

void FDreamTransactions::OnPump(FDreamTxnHandle& H)
{
	check(IsInGameThread());
	using namespace DreamTransactionsPrivate;
	if (H.Status != EDreamTxnStatus::Open || GEditor == nullptr || !GEditor->IsTransactionActive())
	{
		return;
	}
	UTransactor* Trans = Impl->GetTrans();
	const FTransaction* Open = Trans ? Trans->GetTransaction(Trans->GetQueueLength() - 1) : nullptr;
	if (Open == nullptr || Open->GetContext().TransactionId != H.TransactionId)
	{
		return;
	}
	// §5.9.4: DataSize() is linear in the record count; once per pump.
	H.LastSize = Open->DataSize();
	if (H.Budget == 0)
	{
		return;
	}
	if (!H.bWarned50 && H.LastSize * 2 >= H.Budget)
	{
		H.bWarned50 = true;
		const FText Warning = FText::Format(LOCTEXT("UndoHalf", "Undo record {0} MB of {1} MB. The next edit will evict older undo history."),
			FormatMegabytes(H.LastSize), FormatMegabytes(H.Budget));
		H.Note = JoinNote(H.Note, Warning);
	}
	if (H.LastSize >= H.Budget)
	{
		// 100%: stop recording now; the budget applies only at the next Begin, so this bounds memory (§5.9.4).
		NotePieObjects(H, *Open);
		GEditor->EndTransaction();
		H.Status = EDreamTxnStatus::Partial;
		H.Note = JoinNote(H.Note, LOCTEXT("PartlyUndoable", "Recording stopped at 100%: partly undoable"));
		if (GEditor->IsTransactionActive())
		{
			H.Note = JoinNote(H.Note, LOCTEXT("UserScopeOpen", "a transaction the script opened is still recording until it closes"));
		}
	}
}

void FDreamTransactions::EndRunTransaction(FDreamTxnHandle& H, FDreamRunRecord& Rec)
{
	check(IsInGameThread());
	using namespace DreamTransactionsPrivate;
	Impl->bUserCodeWindow = false;
	UTransactor* Trans = Impl->GetTrans();

	if (H.Status == EDreamTxnStatus::Open && GEditor != nullptr)
	{
		// The engine discards a transaction that holds a PIE object at End, edits and all (FTransaction::IsTransient), so
		// the note is read from the open record while it still exists.
		if (Trans != nullptr && H.TransactionId.IsValid())
		{
			const int32 OpenIndex = Trans->FindTransactionIndex(H.TransactionId);
			if (const FTransaction* Open = OpenIndex != INDEX_NONE ? Trans->GetTransaction(OpenIndex) : nullptr)
			{
				NotePieObjects(H, *Open);
			}
		}
		GEditor->EndTransaction();      // a no-op when a reset already purged it (ActiveCount 0)
	}

	// Dangling check (D8): close what the script opened and never closed, at most 16 times. Not when a foreign edit
	// was already open when the run entered (OnRunEntry): that transaction is not the script's to close.
	if (GEditor != nullptr && !Impl->bForeignActiveAtBegin)
	{
		int32 Closed = 0;
		while (GEditor->IsTransactionActive() && Closed < MaxDanglingCloses)
		{
			GEditor->EndTransaction();
			++Closed;
		}
		H.DanglingClosed = Closed;
		if (Closed > 0)
		{
			PushRunLine(Rec.RunNumber, EDreamStream::Warning,
				FText::Format(LOCTEXT("DanglingClosed", "Closed {0} {0}|plural(one=transaction,other=transactions) the script opened and never closed."), Closed));
		}
	}

	H.bImported = Impl->Imports.load() > 0;
	H.bRenamed = Impl->Renames.load() > 0;
	if (H.bImported)
	{
		H.Note = JoinNote(H.Note, LOCTEXT("ImportsNotUndoable", "imports and reimports in this run are not undoable"));
	}

	if (H.Status == EDreamTxnStatus::Open || H.Status == EDreamTxnStatus::Partial)
	{
		const int32 Index = Trans && H.TransactionId.IsValid() ? Trans->FindTransactionIndex(H.TransactionId) : INDEX_NONE;
		// §5.9.3 (slice A choice): Purged only on an observed reset: a map open, an asset delete, or UTransBuffer::Reset seen
		// while our transaction was open (our cancel, then the emptied buffer) or after its 100% end (the emptied buffer).
		// The queue's length is no evidence: it also counts a redo stack a script's Begin drops and the budget evictions of
		// the record a 100% end left over budget (A1-02, A1-01). An empty or PIE-only transaction that End popped reads
		// Empty below; once our cancel was seen without a reset, it is CancelledByScript.
		const bool bPurged = Impl->bMapOpened || Impl->bAssetsDeleted || Impl->bResetSeen;
		// A reset after a 100% end raises no cancel event for us, but the record is gone all the same.
		if (Impl->bCancelSeen || (Index == INDEX_NONE && bPurged))
		{
			// §5.9.3: a reset (map load, asset delete) purged it, or the script cancelled a nested scope.
			FText CanUndoReason;
			if (bPurged && Trans != nullptr)
			{
				Trans->CanUndo(&CanUndoReason);
			}
			H.Status = bPurged ? EDreamTxnStatus::Purged : EDreamTxnStatus::CancelledByScript;
			const FText Reason = bPurged
				? (CanUndoReason.IsEmpty() ? LOCTEXT("PurgedReason", "the undo history was reset during the run") : CanUndoReason)
				: LOCTEXT("CancelledReason", "the script cancelled the transaction");
			H.Note = JoinNote(H.Note, FText::Format(LOCTEXT("UndoUnavailable", "Undo is unavailable for this run: {0}."), Reason));
		}
		else if (Index == INDEX_NONE)
		{
			// The engine discards empty transactions: nothing was recorded and no Undo is offered (§5.9.2).
			if (H.Status == EDreamTxnStatus::Open)
			{
				H.Status = EDreamTxnStatus::Empty;
			}
		}
		else
		{
			if (H.Status == EDreamTxnStatus::Open)
			{
				H.Status = EDreamTxnStatus::Recorded;
			}
			if (const FTransaction* Recorded = Trans->GetTransaction(Index))
			{
				H.LastSize = Recorded->DataSize();
				TArray<UObject*> Objects;
				Recorded->GetTransactionObjects(Objects);
				H.ObjectCount = Objects.Num();
				NotePieObjects(H, *Recorded);
			}
		}
	}

	// Renames and moves are never undoable (§5.9.2): with a transaction, clear the history so no undo can split an asset
	// from its package. Cleared replaces Purged or CancelledByScript and suppresses that banner.
	if (H.bRenamed)
	{
		if (H.Status != EDreamTxnStatus::Off && GEditor != nullptr)
		{
			H.Status = EDreamTxnStatus::Cleared;
			GEditor->ResetTransaction(LOCTEXT("DreamAssetRename", "Dream: assets renamed"));
			H.Note = LOCTEXT("RenamesClearedNote", "asset renames and moves in this run are not undoable · undo history cleared");
		}
		else
		{
			H.Note = JoinNote(H.Note, LOCTEXT("RenamesNotUndoable", "asset renames and moves in this run are not undoable"));
		}
	}

	Impl->bWatching = false;
	Impl->bOurEnded = false;
	Impl->WatchedId.Invalidate();
	// A run without a transaction never calls BeginRunTransaction, so the counters restart here for the next run.
	Impl->Imports = 0;
	Impl->Renames = 0;
	Impl->bForeignActiveAtBegin = false;
	(void)Rec;
}

bool FDreamTransactions::CanUndoLastRun(const FDreamRunRecord& Rec, FText& OutReason, FDreamUndoPreview& OutPreview) const
{
	check(IsInGameThread());
	const FDreamTxnHandle& H = Rec.Txn;
	const FText RunNumber = FText::AsNumber(Rec.RunNumber, &FNumberFormattingOptions::DefaultNoGrouping());
	OutPreview = FDreamUndoPreview();
	OutPreview.Title = H.Title;
	OutPreview.RunNumber = Rec.RunNumber;

	// 0: a run that renamed or moved assets cleared the undo history.
	if (H.Status == EDreamTxnStatus::Cleared)
	{
		OutReason = FText::Format(LOCTEXT("Check0", "Run #{0} renamed or moved assets, so the undo history was cleared."), RunNumber);
		return false;
	}
	if (H.Status != EDreamTxnStatus::Recorded && H.Status != EDreamTxnStatus::Partial)
	{
		switch (H.Status)
		{
		case EDreamTxnStatus::Undone:
			OutReason = FText::Format(LOCTEXT("AlreadyUndone", "Run #{0} was already undone."), RunNumber);
			break;
		case EDreamTxnStatus::Purged:
		case EDreamTxnStatus::CancelledByScript:
			OutReason = H.Note.IsEmpty() ? FText::Format(LOCTEXT("NoUndoRecord", "Run #{0} has no undo record."), RunNumber) : H.Note;
			break;
		case EDreamTxnStatus::Empty:
			OutReason = FText::Format(LOCTEXT("NothingRecorded", "Run #{0} changed nothing that can be undone."), RunNumber);
			break;
		default:
			OutReason = FText::Format(LOCTEXT("RanWithoutTransaction", "Run #{0} ran without One undo step per run."), RunNumber);
			break;
		}
		return false;
	}
	// 1: the standard buffer (07e Q6).
	UTransactor* Trans = Impl->GetTrans();
	if (GEditor == nullptr || Trans == nullptr || Trans != Impl->StandardTrans.Get())
	{
		OutReason = LOCTEXT("Check1", "Another tool replaced the undo buffer (for example the AI Assistant).");
		return false;
	}
	// 2: no edit in progress.
	if (GEditor->IsTransactionActive())
	{
		OutReason = LOCTEXT("Check2", "An edit is in progress.");
		return false;
	}
	// 3: the top undoable entry is ours (id and title).
	const FTransaction* Top = Trans->GetTransaction(Trans->GetQueueLength() - Trans->GetUndoCount() - 1);
	if (Top == nullptr || Top->GetContext().TransactionId != H.TransactionId || !Top->GetTitle().EqualTo(H.Title))
	{
		OutReason = FText::Format(LOCTEXT("Check3", "Edits were made after Run #{0}. Use Edit ▸ Undo History."), RunNumber);
		return false;
	}
	// 4 and 5: the preview the caller shows (inline confirm for Partial, the over-budget warning).
	TArray<UObject*> Objects;
	Top->GetTransactionObjects(Objects);
	OutPreview.ObjectCount = Objects.Num();
	OutPreview.Bytes = Top->DataSize();
	OutPreview.bPartial = H.Status == EDreamTxnStatus::Partial;
	OutPreview.bOverBudget = Impl->BudgetBytes > 0 && OutPreview.Bytes >= Impl->BudgetBytes;
	OutReason = OutPreview.bOverBudget
		? LOCTEXT("Check5", "This record is over the undo budget. The next edit will evict it.")
		: FText::GetEmpty();
	return true;
}

bool FDreamTransactions::UndoLastRun(FDreamRunRecord& Rec)
{
	check(IsInGameThread());
	if (GEditor == nullptr || !GEditor->UndoTransaction())
	{
		return false;
	}
	Rec.Txn.Status = EDreamTxnStatus::Undone;
	UE_LOG(LogDream, Log, TEXT("Undo Last Run: %s undone."), *Rec.Txn.Title.ToString());
	return true;
}

#undef LOCTEXT_NAMESPACE
