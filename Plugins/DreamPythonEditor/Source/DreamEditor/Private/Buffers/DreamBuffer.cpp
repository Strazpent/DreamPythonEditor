// SPDX-License-Identifier: Apache-2.0
// One editable buffer (TDD §4.1, §4.13, §10.2, BUF-01, BUF-02, BUF-06): identity, file state, the dream::TextBuffer model and
// its incremental PyLexer, kept in step on every edit. Game thread.
#include "Buffers/DreamBuffer.h"

#include "Buffers/DreamScratch.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"
#include "Run/DreamDeferral.h"
#include "Tasks/Task.h"

#include <atomic>

#define LOCTEXT_NAMESPACE "Dream"

/** One BuildCompacted on a worker (§4.1.3): the snapshot it read, its result, and the done flag the game thread polls. */
struct FDreamBuffer::FCompactJob
{
	dream::TextSnapshot From;
	dream::TextSnapshot Built;
	std::atomic<bool> bDone { false };
};

FDreamBuffer::FDreamBuffer(const FGuid& InId, EDreamBufferKind InKind, const FString& InDisplayName)
	: Id(InId)
	, Kind(InKind)
	, DisplayName(InDisplayName)
	, Format(dream::codec::NewFileFormat())
{
	Lexer.Reset(Model.Snapshot());
	MarkClean();
}

FString FDreamBuffer::GetIdHex() const
{
	return DreamText::GuidToHex(Id);
}

dream::ChangeSet FDreamBuffer::ApplyEdit(const dream::EditBatch& Batch)
{
	dream::ChangeSet Changes = Model.Apply(Batch);
	if (!Changes.IsEmpty())
	{
		// Unedited lines keep their minority terminators on save (§4.13 step 6, T-ENC-001).
		dream::codec::MapEolExceptions(Format, Changes);
		Lexer.OnChange(Changes, Model.Snapshot());
		UpdateDirty();
		ScheduleCompaction();
		ChangedEvent.Broadcast(Changes);
	}
	return Changes;
}

void FDreamBuffer::ResetText(const FString& Text, dream::EditOrigin Origin)
{
	// The model is LF-only (§4.1.2): the codec normalizes files; text from other sources may still carry CR.
	const FString Normalized = DreamText::ToModelText(Text);
	dream::ChangeSet Changes = Model.SetText(DreamText::ToU16(Normalized), Origin);
	// A whole-text replace keeps no exception terminator (a recovered text differs from the disk file); a load sets the
	// file's Format right after (ApplyDecoded, AddRestored).
	dream::codec::MapEolExceptions(Format, Changes);
	Lexer.Reset(Model.Snapshot());
	UpdateDirty();
	if (!Changes.IsEmpty())
	{
		ScheduleCompaction();
		ChangedEvent.Broadcast(Changes);
	}
}

FString FDreamBuffer::GetTextAsString() const
{
	return DreamText::ToFString(Model.Snapshot().Text());
}

bool FDreamBuffer::IsDirty() const
{
	return bDirty || bDeletedOnDisk;
}

void FDreamBuffer::UpdateDirty()
{
	bool bNewDirty = false;
	if (Model.Version() != SavedVersion)
	{
		// (slice A choice) a compare only when the length matches, so undoing back to the saved text clears the marker.
		bNewDirty = Model.Length() != SavedLength || !Model.Snapshot().SameRepAs(SavedSnapshot);
		if (bNewDirty && Model.Length() == SavedLength)
		{
			bNewDirty = Model.Snapshot().Text() != SavedSnapshot.Text();
		}
	}
	if (bNewDirty != bDirty)
	{
		bDirty = bNewDirty;
		StateChangedEvent.Broadcast();
	}
}

void FDreamBuffer::MarkSaved(const FString& NewPath, const FDreamBaseVersion& NewBase)
{
	MarkSaved(NewPath, NewBase, Model.Snapshot());
}

void FDreamBuffer::MarkSaved(const FString& NewPath, const FDreamBaseVersion& NewBase, const dream::TextSnapshot& WrittenText)
{
	if (!NewPath.IsEmpty())
	{
		Path = NewPath;
		DisplayName = FPaths::GetCleanFilename(NewPath);
		if (Kind == EDreamBufferKind::Scratch || Kind == EDreamBufferKind::Recovered)
		{
			Kind = EDreamBufferKind::File;   // Save As promotes a scratch tab (BUF-01)
		}
		RefreshScratchFlag();   // (scratch) Save As into or out of the scratch folder
	}
	Base = NewBase;
	SavedSnapshot = WrittenText;
	SavedVersion = WrittenText.Version();
	SavedLength = WrittenText.Length();
	bDirty = false;
	bDeletedOnDisk = false;   // the file exists again: it holds the written (or reloaded) text (§10.15)
	UpdateDirty();
	StateChangedEvent.Broadcast();   // path, name, kind and base changed even when the dirty flag did not
}

void FDreamBuffer::MarkClean()
{
	SavedSnapshot = Model.Snapshot();
	SavedVersion = Model.Version();
	SavedLength = Model.Length();
	if (bDirty)
	{
		bDirty = false;
		StateChangedEvent.Broadcast();
	}
}

void FDreamBuffer::SetCleanText(const FString& CleanText)
{
	const dream::TextBuffer Clean(DreamText::ToU16(CleanText));
	SavedSnapshot = Clean.Snapshot();
	SavedVersion = 0;   // never a live version: dirty is decided by comparing texts
	SavedLength = SavedSnapshot.Length();
	UpdateDirty();
}

void FDreamBuffer::SetFileIdentity(const FString& NewPath, const TOptional<FDreamBaseVersion>& NewBase)
{
	Path = NewPath;
	Base = NewBase;
	RefreshScratchFlag();   // (scratch)
	StateChangedEvent.Broadcast();
}

bool FDreamBuffer::RefreshScratchFlag()
{
	// (scratch, PO-05) Only a File buffer saves itself: a "(recovered)" tab beside its disk file never overwrites it.
	const bool bNew = Kind == EDreamBufferKind::File && !Path.IsEmpty() && DreamScratch::IsScratchPath(Path);
	if (bNew == bScratchFile)
	{
		return false;
	}
	bScratchFile = bNew;
	return true;
}

void FDreamBuffer::SetReadOnly(bool bInReadOnly, const FText& Reason)
{
	if (bReadOnly == bInReadOnly && ReadOnlyReason.EqualTo(Reason))
	{
		return;
	}
	bReadOnly = bInReadOnly;
	ReadOnlyReason = bInReadOnly ? Reason : FText::GetEmpty();
	StateChangedEvent.Broadcast();
}

void FDreamBuffer::SetReadOnlyOnDisk(bool bInReadOnlyOnDisk)
{
	if (bReadOnlyOnDisk != bInReadOnlyOnDisk)
	{
		bReadOnlyOnDisk = bInReadOnlyOnDisk;
		StateChangedEvent.Broadcast();
	}
}

void FDreamBuffer::SetDeletedOnDisk(bool bInDeleted)
{
	if (bDeletedOnDisk != bInDeleted)
	{
		bDeletedOnDisk = bInDeleted;
		StateChangedEvent.Broadcast();   // IsDirty() follows the flag
	}
}

void FDreamBuffer::SetPendingRecovery(TOptional<FString> RecoveredText)
{
	const bool bWasPending = PendingRecoveryText.IsSet();
	PendingRecoveryText = MoveTemp(RecoveredText);
	if (PendingRecoveryText.IsSet() && !bWasPending)
	{
		// "Not restored yet" (§10.7.1, PER-03): typing is refused and Ctrl+S does nothing until Restore or Discard.
		bReadOnlyBeforeRecovery = bReadOnly;
		ReadOnlyReasonBeforeRecovery = ReadOnlyReason;
		bReadOnly = true;
		ReadOnlyReason = LOCTEXT("NotRestoredYet", "Not restored yet");
	}
	else if (!PendingRecoveryText.IsSet() && bWasPending)
	{
		bReadOnly = bReadOnlyBeforeRecovery;
		ReadOnlyReason = ReadOnlyReasonBeforeRecovery;
	}
	StateChangedEvent.Broadcast();
}

// ---------------------------------------------------------------------------------------------------------------------
// Compaction of the piece tree (TDD §4.1.3)
// ---------------------------------------------------------------------------------------------------------------------

void FDreamBuffer::ScheduleCompaction()
{
	LastChangeSeconds = FPlatformTime::Seconds();
	if (CompactTicker.IsValid() || !DoesSharedInstanceExist())
	{
		return;
	}
	// (B-note) One ticker per buffer, armed by the first change after a quiet period and gone once nothing is left to do:
	// it polls twice a second, so a buffer that is not being edited costs nothing. Deferred while a run is active, like
	// every applier of async results (§3.7), although an install would not change the text.
	const TWeakPtr<FDreamBuffer> Weak = AsShared();
	CompactTicker = FDreamDeferral::AddTicker(TEXT("DreamCompaction"), EDreamDeferPolicy::DeferWhileExecuting, 0.5f, false, [Weak](float) -> bool
	{
		const TSharedPtr<FDreamBuffer> Pinned = Weak.Pin();
		return Pinned.IsValid() && Pinned->TickCompaction();
	});
}

bool FDreamBuffer::TickCompaction()
{
	if (CompactJob.IsValid())
	{
		if (!CompactJob->bDone.load(std::memory_order_acquire))
		{
			return true;
		}
		const TSharedPtr<FCompactJob, ESPMode::ThreadSafe> Job = MoveTemp(CompactJob);
		CompactJob.Reset();
		// Refused (and nothing changes) when the buffer was edited after the snapshot: the next quiet period tries again.
		if (Model.InstallCompacted(Job->Built, Job->From.Version()))
		{
			++Compactions;
		}
	}
	if (FPlatformTime::Seconds() - LastChangeSeconds < CompactionIdleSeconds)
	{
		return true;   // still being edited
	}
	if (!Model.NeedsCompaction())
	{
		CompactTicker.Reset();
		return false;
	}
	TSharedRef<FCompactJob, ESPMode::ThreadSafe> Job = MakeShared<FCompactJob, ESPMode::ThreadSafe>();
	Job->From = Model.Snapshot();
	CompactJob = Job;
	// Any thread: BuildCompacted reads only the immutable snapshot (DreamCore, no Slate or UObjects).
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [Job]()
	{
		Job->Built = dream::TextBuffer::BuildCompacted(Job->From);
		Job->bDone.store(true, std::memory_order_release);
	});
	return true;
}

#undef LOCTEXT_NAMESPACE
