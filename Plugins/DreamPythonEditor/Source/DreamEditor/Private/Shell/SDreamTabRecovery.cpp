// SPDX-License-Identifier: Apache-2.0
// SDreamTab's recovery actions (TDD §10.7.1-§10.7.3, §10.8; U26, U30, PER-03, PER-06), beside SDreamTab.cpp as
// SDreamTabToolbar.cpp is: per-buffer Restore (one undoable edit through codeview's grouped-edit API, dirty, nothing on
// disk), Restore as new tab, Compare (the M1 read-only unified diff of dream::Diff), Discard (the recovered text moves to
// local history as a "discarded" entry and only then resolves), the per-buffer pending bar, the Review panel, the conflict
// tab's Compare with disk and Save over, and Leave safe mode. Game thread; nothing here runs Python. Owner: persist, then
// instances, which adds the other namespaces (§10.5, §10.6, §10.1): their sections in Review (orphaned sessions, imported
// and compat journals, other storage roots, and a secondary's read-only view of the primary's tabs) and the secondary,
// moved-project, storage and n = 0 banners.
#include "Shell/SDreamTab.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/DreamUndoHistory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Persistence/DreamInstances.h"
#include "Persistence/DreamJournal.h"
#include "Persistence/DreamLocalHistory.h"
#include "Persistence/DreamRestoreGuard.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamRecoveryReview.h"
#include "Shell/SDreamRecoveryBanner.h"
#include "Shell/SDreamTabImpl.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"
#include "dream/TextBuffer.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamTabRecoveryLocal
{
	FDreamEditorModule& Module()
	{
		return FDreamEditorModule::Get();
	}

	dream::TextSnapshot ModelSnapshot(const FString& Text)
	{
		return dream::TextBuffer(DreamText::ToU16(DreamText::ToModelText(Text))).Snapshot();
	}

	FText RecoveredName(const FString& Display)
	{
		return FText::Format(LOCTEXT("Recovery.RecoveredTabName", "{0} (recovered)"), FText::FromString(Display));
	}

	/** Another open buffer on the same path: the disk version beside a "(recovered)" tab (§10.7.2). */
	TSharedPtr<FDreamBuffer> FindDiskTwin(const FDreamBuffer& Recovered)
	{
		if (!Recovered.HasPath())
		{
			return nullptr;
		}
		for (const TSharedRef<FDreamBuffer>& Other : Module().GetBufferManager().GetOpenBuffers())
		{
			if (Other->GetId() != Recovered.GetId() && Other->HasPath() && FPaths::IsSamePath(Other->GetPath(), Recovered.GetPath()))
			{
				return Other;
			}
		}
		return nullptr;
	}

	/** The disk text a "(recovered)" tab is compared with: the open disk tab's, else the file read now (under 1 MB, A40). */
	FString DiskTextOf(const FDreamBuffer& Recovered)
	{
		if (TSharedPtr<FDreamBuffer> Twin = FindDiskTwin(Recovered))
		{
			return Twin->GetTextAsString();
		}
		FString Text;
		if (Recovered.HasPath() && FFileHelper::LoadFileToString(Text, *Recovered.GetPath()))
		{
			return DreamText::ToModelText(Text);
		}
		return FString();
	}

	FText WhereOf(const FString& Path)
	{
		return Path.IsEmpty() ? LOCTEXT("Recovery.Untitled", "untitled") : FText::FromString(Path);
	}
}

using DreamTabRecoveryLocal::Module;

// ---------------------------------------------------------------------------------------------------------------------
// Per-buffer actions (§10.7.3)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::RestoreRecovered(const FGuid& BufferId, bool bAsNewTab)
{
	FDreamEditorModule& M = Module();
	if (Impl->bSafeMode && Impl->SafeModeSession.IsSet() && !Impl->PendingRecoveries.Contains(BufferId))
	{
		RestoreFromSkippedSession(BufferId, bAsNewTab);
		return;
	}
	TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(BufferId);
	if (!Buffer.IsValid() || !Buffer->HasPendingRecovery() || Impl->DiscardsInFlight.Contains(BufferId))
	{
		return;
	}
	if (M.GetRunService().IsRunActive())
	{
		ShowStatusMessage(LOCTEXT("Recovery.NotDuringRun", "Recovered text is restored when the run has ended."));   // RT-01
		return;
	}
	const FString Recovered = Buffer->GetPendingRecoveryText().GetValue();
	const dream::TextSnapshot Shown = Buffer->Snapshot();
	if (bAsNewTab)
	{
		// The recovered text in a new scratch tab; this buffer keeps the text it shows and becomes editable again. The tab opens
		// first (review r1): its BUF_OPEN is queued before RECOVERY_RESOLVED, whose batch also carries every live buffer's text,
		// so no kill between the two flushes can leave the recovery resolved and its text nowhere in the journal.
		// (scratch, PO-05) The new tab is a scratch file, scratch_N.py, labelled with its file name; its text is saved there
		// by the auto-save (the journal holds it until then).
		const bool bConflictTab = Buffer->GetKind() == EDreamBufferKind::Recovered;
		NewScratchTab(Recovered);
		Buffer->SetPendingRecovery(TOptional<FString>());
		M.GetJournal().ResolveRecovery(BufferId, TEXT("restored_as_new_tab"));
		ForgetPendingRecovery(BufferId);
		if (bConflictTab)
		{
			M.GetBufferManager().Close(BufferId);   // its text lives on in the new tab
		}
	}
	else
	{
		Buffer->SetPendingRecovery(TOptional<FString>());
		if (Buffer->GetKind() != EDreamBufferKind::Recovered)
		{
			// One undoable edit (U30): Ctrl+Z brings back the text shown before; dirty, nothing written to disk (PER-03).
			if (!FDreamUndoHistory::ReplaceAllText(*Buffer, Recovered, LOCTEXT("Recovery.RestoreUndoLabel", "Restore recovered text"),
				dream::EditOrigin::Command, /*bEvenIfReadOnly*/ true))
			{
				Buffer->SetPendingRecovery(Recovered);
				ShowStatusMessage(LOCTEXT("Recovery.RestoreRefused", "The recovered text could not be restored now; it is still waiting."));
				return;
			}
			// §10.4: the text the restore replaced joins local history as a "restore" entry.
			M.GetLocalHistory().AddEntry(BufferId, EDreamHistoryKind::Restore, Shown, Buffer->GetPath(), Buffer->GetDisplayName());
		}
		// A "(recovered)" tab already holds the recovered text: it becomes dirty and editable (§10.7.2).
		M.GetJournal().ResolveRecovery(BufferId, TEXT("restored"));
		ForgetPendingRecovery(BufferId);
		MigrateUntitledToScratchFile(BufferId);   // (scratch, PO-05) a restored Untitled-N becomes a scratch file now
	}
	if (Impl->CompareFor == BufferId)
	{
		Impl->CompareFor.Invalidate();
	}
	RefreshRecoveryUi();
	RebuildBanners();
	RefreshTabStrip();
}

void SDreamTab::DiscardRecovered(const TArray<FGuid>& BufferIds)
{
	FDreamEditorModule& M = Module();
	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	for (const FGuid& Id : BufferIds)
	{
		if (Impl->bSafeMode && Impl->SafeModeSession.IsSet() && !Impl->PendingRecoveries.Contains(Id))
		{
			// Safe mode's Review: a buffer of the skipped session that is not open yet.
			FDreamRecoveredBuffer* Rec = Impl->SafeModeSession->Buffers.FindByPredicate([&Id](const FDreamRecoveredBuffer& B) { return B.Id == Id; });
			if (Rec == nullptr || Impl->DiscardsInFlight.Contains(Id))
			{
				continue;
			}
			Impl->DiscardsInFlight.Add(Id);
			M.GetLocalHistory().AddEntry(Id, EDreamHistoryKind::Discarded, DreamTabRecoveryLocal::ModelSnapshot(Rec->RecoveredText), Rec->Path,
				Rec->DisplayName, TOptional<int32>(), [WeakTab, Id](bool bKept)
				{
					if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
					{
						Tab->FinishDiscard(Id, bKept);
					}
				});
			continue;
		}
		TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(Id);
		if (!Buffer.IsValid() || !Buffer->HasPendingRecovery() || Impl->DiscardsInFlight.Contains(Id))
		{
			continue;
		}
		// §10.7.3 Discard "never deletes anything": the recovered text goes to local history first, and only once that entry
		// is durable does the recovery resolve; a text that could not be kept stays pending (P2-per-05).
		Impl->DiscardsInFlight.Add(Id);
		M.GetLocalHistory().AddEntry(Id, EDreamHistoryKind::Discarded, DreamTabRecoveryLocal::ModelSnapshot(Buffer->GetPendingRecoveryText().GetValue()),
			Buffer->GetPath(), Buffer->GetDisplayName(), TOptional<int32>(), [WeakTab, Id](bool bKept)
			{
				if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
				{
					Tab->FinishDiscard(Id, bKept);
				}
			});
	}
	RefreshRecoveryUi();
}

void SDreamTab::FinishDiscard(const FGuid& BufferId, bool bKept)
{
	Impl->DiscardsInFlight.Remove(BufferId);
	FDreamEditorModule& M = Module();
	if (Impl->bSafeMode && Impl->SafeModeSession.IsSet() && !Impl->PendingRecoveries.Contains(BufferId))
	{
		FDreamRecoveredBuffer* Rec = Impl->SafeModeSession->Buffers.FindByPredicate([&BufferId](const FDreamRecoveredBuffer& B) { return B.Id == BufferId; });
		if (Rec != nullptr && bKept)
		{
			if (Rec->bCounted)
			{
				M.GetJournal().ResolveRecovery(BufferId, TEXT("discarded"));
			}
			// Leave safe mode then restores this buffer as if nothing had been recovered: its disk or clean text.
			Rec->bCounted = false;
			Rec->bDiffersFromDisk = false;
			if (Rec->Kind == EDreamBufferKind::Scratch)
			{
				Rec->RecoveredText = Rec->CleanText.Get(FString());
			}
		}
		RefreshRecoveryUi();
		return;
	}
	TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(BufferId);
	if (!Buffer.IsValid() || !Buffer->HasPendingRecovery())
	{
		RefreshRecoveryUi();
		return;
	}
	if (!bKept)
	{
		UE_LOG(LogDream, Warning, TEXT("The recovered text of %s could not be kept in local history; it was not discarded."), *Buffer->GetDisplayName());
		// (chrome) A line of its own (a new id): it shows the banner again even when × hid an identical line (review r7).
		Impl->AddRecoveryLine(FText::Format(LOCTEXT("Recovery.DiscardNotKept", "Could not keep the recovered text of {0} in local history; it was not discarded."),
			FText::FromString(Buffer->GetDisplayName())));
		Impl->bShowRecoveryBanner = true;
		RebuildBanners();
		RefreshRecoveryUi();
		return;
	}
	Buffer->SetPendingRecovery(TOptional<FString>());
	M.GetJournal().ResolveRecovery(BufferId, TEXT("discarded"));
	ForgetPendingRecovery(BufferId);
	if (Impl->CompareFor == BufferId)
	{
		Impl->CompareFor.Invalidate();
	}
	if (Buffer->GetKind() == EDreamBufferKind::Recovered)
	{
		M.GetBufferManager().Close(BufferId);   // Discard closes the "(recovered)" tab (§10.7.2)
	}
	else
	{
		MigrateUntitledToScratchFile(BufferId);   // (scratch, PO-05) a resolved Untitled-N becomes a scratch file now
	}
	RefreshRecoveryUi();
	RebuildBanners();
	RefreshTabStrip();
}

void SDreamTab::ToggleRecoveryCompare(const FGuid& BufferId)
{
	Impl->CompareFor = Impl->CompareFor == BufferId && Impl->CompareSection.IsEmpty() ? FGuid() : BufferId;
	Impl->CompareSection.Reset();
	RefreshRecoveryUi();
}

void SDreamTab::ToggleRecoveryReview()
{
	Impl->bShowReview = !Impl->bShowReview;
	RefreshRecoveryUi();
}

bool SDreamTab::IsRecoveryReviewOpen() const
{
	return Impl->bShowReview;
}

TArray<FGuid> SDreamTab::GetPendingRecoveries() const
{
	return Impl->PendingRecoveries;
}

bool SDreamTab::IsInSafeMode() const
{
	return Impl->bSafeMode;
}

void SDreamTab::ForgetPendingRecovery(const FGuid& BufferId)
{
	if (Impl->PendingRecoveries.Remove(BufferId) == 0)
	{
		return;
	}
	Impl->RecoveryDetails.Remove(BufferId);
	if (Impl->PendingRecoveries.Num() == 0)
	{
		Impl->bConfirmDiscard = false;
	}
	// The line that counts the pending buffers follows their count and goes with the last one (P3-shl-05); the unclean-exit
	// line is past tense and stays, and with nothing pending the banner can be closed.
	// (chrome) The count line keeps its id, so a line hidden with × stays hidden after a buffer is handled (review r7).
	Impl->SetEarlierSessionLine(Impl->PendingRecoveries.Num() > 0
		? TOptional<FText>(FText::Format(LOCTEXT("RecoveredEarlier", "{0} recovered {0}|plural(one=buffer,other=buffers) from an earlier session {0}|plural(one=is,other=are) not restored yet."),
			FText::AsNumber(Impl->PendingRecoveries.Num())))
		: TOptional<FText>());
	Impl->bShowRecoveryBanner = Impl->bShowRecoveryBanner && Impl->RecoveryLines.Num() > 0;
	if (Impl->PendingRecoveries.Num() == 0 && !Impl->bSafeMode && Module().GetInstances().CountOfferedBuffers() == 0
		&& !Module().GetInstances().GetPrimaryView().IsSet())
	{
		Impl->bShowReview = false;   // nothing left to review (the other namespaces' sections keep it open, instances)
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Safe mode (§10.8)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::LeaveSafeMode()
{
	if (!Impl->bSafeMode)
	{
		return;
	}
	// §10.8 Leaving: pending = 0, the skipped restore now, the deferred services started (the module listens to the guard).
	Module().GetRestoreGuard().LeaveSafeMode();
	Impl->bSafeMode = false;
	Impl->bShowReview = false;
	if (Impl->SafeModeSession.IsSet())
	{
		FDreamRecoveredSession Session = MoveTemp(Impl->SafeModeSession.GetValue());
		Impl->SafeModeSession.Reset();
		Session.ReplInputText.Reset();   // ApplySafeMode already put the unsent prompt back (ADD3-01)
		ApplyRestore(MoveTemp(Session));
	}
	RebuildBanners();
	RefreshRecoveryUi();
}

void SDreamTab::RestoreFromSkippedSession(const FGuid& BufferId, bool bAsNewTab)
{
	FDreamEditorModule& M = Module();
	FDreamBufferManager& Buffers = M.GetBufferManager();
	if (!Impl->SafeModeSession.IsSet() || Buffers.FindById(BufferId).IsValid())
	{
		return;
	}
	FDreamRecoveredBuffer* Rec = Impl->SafeModeSession->Buffers.FindByPredicate([&BufferId](const FDreamRecoveredBuffer& B) { return B.Id == BufferId; });
	if (Rec == nullptr || Impl->DiscardsInFlight.Contains(BufferId))
	{
		return;
	}
	const FString Display = Rec->DisplayName.IsEmpty() ? FString(TEXT("Untitled")) : Rec->DisplayName;
	if (bAsNewTab)
	{
		NewScratchTab(Rec->RecoveredText);   // (scratch, PO-05) a scratch file
		if (Rec->bCounted)
		{
			M.GetJournal().ResolveRecovery(BufferId, TEXT("restored_as_new_tab"));
		}
		Rec->bCounted = false;
		Rec->bDiffersFromDisk = false;
		if (Rec->Kind == EDreamBufferKind::Scratch)
		{
			Rec->RecoveredText = Rec->CleanText.Get(FString());
		}
		RefreshRecoveryUi();
		return;
	}

	// §10.8: every buffer of the skipped session can be restored from Review: it opens as the startup restore would open it,
	// and a counted one takes its recovered text at once (the user chose Restore).
	TSharedPtr<FDreamBuffer> Buffer;
	FText Error;
	if (Rec->Kind == EDreamBufferKind::Scratch || Rec->Path.IsEmpty())
	{
		const FString Shown = Rec->bCounted ? Rec->CleanText.Get(FString()) : Rec->RecoveredText;
		Buffer = Buffers.AddRestored(BufferId, EDreamBufferKind::Scratch, Display, FString(), Shown, Rec->Format, TOptional<FDreamBaseVersion>());
		M.GetJournal().SetCleanText(BufferId, Buffer->Snapshot());
	}
	else if (Rec->bCounted && (Rec->bConflict || !FPaths::FileExists(Rec->Path)))
	{
		if (FPaths::FileExists(Rec->Path) && !Buffers.FindByPath(Rec->Path).IsValid())
		{
			Buffers.OpenFile(Rec->Path, Error);
		}
		const FString Name = Rec->Kind == EDreamBufferKind::Recovered ? Display : DreamTabRecoveryLocal::RecoveredName(Display).ToString();
		Buffer = Buffers.AddRestored(BufferId, EDreamBufferKind::Recovered, Name, Rec->Path, Rec->RecoveredText, Rec->Format, Rec->Base);
		Buffer->SetCleanText(FString());
	}
	else
	{
		Buffer = Buffers.OpenFile(Rec->Path, Error, BufferId);
		if (Buffer.IsValid() && !Rec->bCounted && Rec->bDiffersFromDisk)
		{
			const FString DiskText = Buffer->GetTextAsString();
			Buffer->ResetText(Rec->RecoveredText, dream::EditOrigin::Load);
			Buffer->SetCleanText(DiskText);
			if (Rec->bConflict && Buffer->IsScratchFile())
			{
				Buffers.HoldRestoredDiskVersion(BufferId, DiskText, Rec->Base);   // (scratch, p1-m2, p2-M1) the newer disk text waits behind its bar
			}
		}
	}
	if (!Buffer.IsValid())
	{
		ShowStatusMessage(FText::Format(LOCTEXT("Recovery.SkippedNotOpened", "Dream Python Editor could not open {0}: {1}"), FText::FromString(Display), Error));
		return;
	}
	ActivateBuffer(BufferId);
	if (Rec->bCounted)
	{
		Buffer->SetPendingRecovery(Rec->RecoveredText);
		Impl->PendingRecoveries.AddUnique(BufferId);
		Impl->RecoveryDetails.Add(BufferId, *Rec);
		Rec->bCounted = false;   // it is this session's pending recovery now
		RestoreRecovered(BufferId, false);   // (scratch) which turns a restored Untitled-N into a scratch file
	}
	else
	{
		MigrateUntitledToScratchFile(BufferId);   // (scratch, p1 nit) safe mode's per-buffer restore converts it too
	}
	RefreshRecoveryUi();
}

// ---------------------------------------------------------------------------------------------------------------------
// The conflict tab after Restore (§10.7.2): Compare with disk, Save over <name>...
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::SaveOverRecovered(const FGuid& BufferId, bool bConfirmed)
{
	FDreamEditorModule& M = Module();
	TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(BufferId);
	if (!Buffer.IsValid() || Buffer->GetKind() != EDreamBufferKind::Recovered || Buffer->HasPendingRecovery() || !Buffer->HasPath())
	{
		return;
	}
	if (!bConfirmed)
	{
		Impl->ConfirmSaveOver = BufferId;   // asks before it overwrites (§10.7.2)
		RefreshRecoveryUi();
		return;
	}
	Impl->ConfirmSaveOver.Invalidate();
	if (TSharedPtr<FDreamBuffer> Twin = DreamTabRecoveryLocal::FindDiskTwin(*Buffer))
	{
		if (Twin->IsDirty())
		{
			ShowStatusMessage(FText::Format(LOCTEXT("Recovery.SaveOverTwinDirty", "Not saved: {0} has unsaved changes in another tab."), FText::FromString(Twin->GetDisplayName())));
			RefreshRecoveryUi();
			return;
		}
		M.GetBufferManager().Close(Twin->GetId());   // the clean disk version: this tab becomes the file's buffer
	}
	const FString Path = Buffer->GetPath();
	Buffer->SetKind(EDreamBufferKind::File);
	Buffer->SetDisplayName(FPaths::GetCleanFilename(Path));
	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	M.GetBufferManager().SaveAsAsync(BufferId, Path, FDreamOnSaved::CreateLambda([WeakTab, Path](bool bOk, const FText& Error)
	{
		if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
		{
			Tab->ShowStatusMessage(bOk ? FText::Format(LOCTEXT("Recovery.SavedOver", "Saved over {0}"), FText::FromString(Path)) : Error);
			Tab->RefreshTabStrip();
			Tab->RefreshRecoveryUi();
		}
	}));
	RefreshTabStrip();
	RefreshRecoveryUi();
}

// ---------------------------------------------------------------------------------------------------------------------
// The pending bar and the Review panel
// ---------------------------------------------------------------------------------------------------------------------

namespace DreamTabRecoveryLocal
{
	/** (instances) The section title of another namespace (§10.5, §10.6, §10.1). */
	FText SectionTitle(const FDreamOrphanNamespace& Orphan)
	{
		const FText When = Orphan.EndUtc.GetTicks() > 0 ? FText::AsDateTime(Orphan.EndUtc, EDateTimeStyle::Short, EDateTimeStyle::Short)
			: LOCTEXT("Recovery.WhenUnknown", "at an unknown time");
		const FText Pid = Orphan.Pid > 0 ? FText::AsNumber(Orphan.Pid, &FNumberFormattingOptions::DefaultNoGrouping()) : LOCTEXT("Recovery.PidUnknown", "unknown");
		switch (Orphan.Source)
		{
		case EDreamOrphanSource::SecondaryInstance:
			return Orphan.bClean
				? FText::Format(LOCTEXT("Recovery.SectionSecondaryClean", "Tabs from a second editor instance (closed {0})"), When)
				: FText::Format(LOCTEXT("Recovery.SectionSecondary", "Another editor instance (PID {0}) that ended unexpectedly ({1})"), Pid, When);
		case EDreamOrphanSource::Compat:
			return FText::Format(LOCTEXT("Recovery.SectionCompat", "An older Dream build's session (format {0}, ended {1})"), FText::FromString(Orphan.Detail), When);
		case EDreamOrphanSource::Imported:
			return FText::Format(LOCTEXT("Recovery.SectionImported", "Imported journal {0} (ended {1})"), FText::FromString(Orphan.Namespace), When);
		case EDreamOrphanSource::OtherRoot:
			return FText::Format(LOCTEXT("Recovery.SectionOtherRoot", "Tabs from another storage folder (closed {0}): {1}"), When, FText::FromString(Orphan.Root));
		case EDreamOrphanSource::SameProject:
			return FText::Format(LOCTEXT("Recovery.SectionSameProject", "This project under another path (ended {0}): {1}"), When, FText::FromString(Orphan.Root));
		case EDreamOrphanSource::PrimaryView:
			return FText::Format(LOCTEXT("Recovery.SectionPrimary", "Tabs of the other editor (PID {0}), read-only: [Open a copy] makes a new scratch buffer"), Pid);
		default:
			return FText::Format(LOCTEXT("Recovery.SectionOther", "Journal {0} (ended {1})"), FText::FromString(Orphan.Namespace), When);
		}
	}

	/** What an orphan's recovered text is compared with: the file on disk (under 1 MB, A40), or nothing for untitled. */
	FString OrphanBaseText(const FDreamRecoveredBuffer& Rec)
	{
		FString Text;
		if (!Rec.Path.IsEmpty() && FFileHelper::LoadFileToString(Text, *Rec.Path))
		{
			return DreamText::ToModelText(Text);
		}
		return FString();
	}

	/** The bar's message for a buffer, empty when it shows none. */
	FText PendingBarMessage(const FDreamBuffer& Buffer, const FDreamRecoveredBuffer* Details, const FGuid& ConfirmSaveOver)
	{
		if (Buffer.HasPendingRecovery())
		{
			if (Buffer.GetKind() == EDreamBufferKind::Recovered)
			{
				return LOCTEXT("Recovery.BarConflict", "Not restored yet · Changed on disk after the journal's base version");
			}
			int32 Added = 0;
			int32 Removed = 0;
			if (Details != nullptr && (Details->LinesAdded > 0 || Details->LinesRemoved > 0))
			{
				Added = Details->LinesAdded;
				Removed = Details->LinesRemoved;
			}
			else
			{
				DreamRecoveryUi::CountLines(Buffer.GetTextAsString(), Buffer.GetPendingRecoveryText().GetValue(), Added, Removed);
			}
			return FText::Format(LOCTEXT("Recovery.BarPending", "Not restored yet · Recovered changes available (+{0} −{1} lines)"),
				FText::AsNumber(Added), FText::AsNumber(Removed));
		}
		if (Buffer.GetKind() == EDreamBufferKind::Recovered)
		{
			if (ConfirmSaveOver == Buffer.GetId())
			{
				return FText::Format(LOCTEXT("Recovery.BarConfirmSaveOver", "Overwrite {0} with this tab's text? The file on disk is replaced."),
					FText::FromString(Buffer.GetPath()));
			}
			return LOCTEXT("Recovery.BarRestoredConflict", "Recovered text, restored · the file on disk is unchanged until you save over it");
		}
		return FText::GetEmpty();
	}
}

TSharedPtr<SWidget> SDreamTab::BuildPendingBar(const TSharedRef<FDreamBuffer>& Buffer)
{
	const FDreamRecoveredBuffer* Details = Impl->RecoveryDetails.Find(Buffer->GetId());
	const FText Message = DreamTabRecoveryLocal::PendingBarMessage(*Buffer, Details, Impl->ConfirmSaveOver);
	if (Message.IsEmpty())
	{
		return nullptr;
	}
	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	const FGuid Id = Buffer->GetId();
	auto Act = [WeakTab](TFunction<void(SDreamTab&)> Body)
	{
		return FSimpleDelegate::CreateLambda([WeakTab, Body]()
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				Body(*Tab);
			}
		});
	};
	const bool bComparing = Impl->CompareFor == Id;
	const bool bConflictTab = Buffer->GetKind() == EDreamBufferKind::Recovered;
	const bool bBusy = Impl->DiscardsInFlight.Contains(Id);
	TArray<FDreamRecoveryAction> Actions;
	TOptional<FString> Diff;
	if (Buffer->HasPendingRecovery())
	{
		Actions.Add({ LOCTEXT("Recovery.BarRestore", "Restore"),
			LOCTEXT("Recovery.BarRestoreTip", "The recovered text as one undoable edit; Ctrl+Z brings back this text; nothing is written to disk"),
			Act([Id](SDreamTab& Tab) { Tab.RestoreRecovered(Id, false); }), !bBusy });
		Actions.Add({ bComparing ? LOCTEXT("Recovery.BarCloseDiff", "Close diff")
				: (bConflictTab ? LOCTEXT("Recovery.BarCompareDisk", "Compare with disk") : LOCTEXT("Recovery.BarCompare", "Compare")),
			LOCTEXT("Recovery.BarCompareTip", "A read-only unified diff against the recovered text"),
			Act([Id](SDreamTab& Tab) { Tab.ToggleRecoveryCompare(Id); }) });
		Actions.Add({ LOCTEXT("Recovery.BarDiscard", "Discard"),
			LOCTEXT("Recovery.BarDiscardTip", "Keeps this text; the recovered text moves to local history"),
			Act([Id](SDreamTab& Tab) { Tab.DiscardRecovered({ Id }); }), !bBusy });
		if (bComparing)
		{
			const FString Base = bConflictTab ? DreamTabRecoveryLocal::DiskTextOf(*Buffer) : Buffer->GetTextAsString();
			Diff = DreamRecoveryUi::UnifiedDiff(Base, Buffer->GetPendingRecoveryText().GetValue(),
				bConflictTab ? LOCTEXT("Recovery.DiffDisk", "on disk").ToString() : LOCTEXT("Recovery.DiffShown", "shown now").ToString(),
				LOCTEXT("Recovery.DiffRecovered", "recovered").ToString());
		}
	}
	else if (Impl->ConfirmSaveOver == Id)
	{
		Actions.Add({ LOCTEXT("Recovery.BarSaveOverConfirm", "Save over"), FText::GetEmpty(),
			Act([Id](SDreamTab& Tab) { Tab.SaveOverRecovered(Id, true); }) });
		Actions.Add({ LOCTEXT("Recovery.BarSaveOverCancel", "Cancel"), FText::GetEmpty(),
			Act([](SDreamTab& Tab) { Tab.Impl->ConfirmSaveOver.Invalidate(); Tab.RefreshRecoveryUi(); }) });
	}
	else
	{
		Actions.Add({ bComparing ? LOCTEXT("Recovery.BarCloseDiff2", "Close diff") : LOCTEXT("Recovery.BarCompareDisk2", "Compare with disk"),
			LOCTEXT("Recovery.BarCompareDiskTip", "A read-only unified diff of the file on disk against this tab"),
			Act([Id](SDreamTab& Tab) { Tab.ToggleRecoveryCompare(Id); }) });
		Actions.Add({ FText::Format(LOCTEXT("Recovery.BarSaveOver", "Save over {0}…"), FText::FromString(FPaths::GetCleanFilename(Buffer->GetPath()))),
			LOCTEXT("Recovery.BarSaveOverTip", "Writes this tab's text to the file on disk (asks first)"),
			Act([Id](SDreamTab& Tab) { Tab.SaveOverRecovered(Id, false); }) });
		if (bComparing)
		{
			Diff = DreamRecoveryUi::UnifiedDiff(DreamTabRecoveryLocal::DiskTextOf(*Buffer), Buffer->GetTextAsString(),
				LOCTEXT("Recovery.DiffDisk2", "on disk").ToString(), LOCTEXT("Recovery.DiffThisTab", "this tab").ToString());
		}
	}
	return SNew(SDreamRecoveryActionBar).Message(Message).Actions(Actions).DiffText(Diff);
}

FText SDreamTab::GetPendingBarTextForTest() const
{
	TSharedPtr<FDreamBuffer> Buffer = GetActiveBuffer();
	return Buffer.IsValid() ? DreamTabRecoveryLocal::PendingBarMessage(*Buffer, Impl->RecoveryDetails.Find(Buffer->GetId()), Impl->ConfirmSaveOver)
		: FText::GetEmpty();
}

void SDreamTab::RefreshRecoveryUi()
{
	if (Impl->PendingBarHost.IsValid())
	{
		TSharedPtr<FDreamBuffer> Active = GetActiveBuffer();
		TSharedPtr<SWidget> Bar = Active.IsValid() ? BuildPendingBar(Active.ToSharedRef()) : nullptr;
		Impl->PendingBarHost->SetContent(Bar.IsValid() ? Bar.ToSharedRef() : SNullWidget::NullWidget);
	}
	if (!Impl->ReviewHost.IsValid())
	{
		return;
	}
	if (!Impl->bShowReview)
	{
		Impl->ReviewHost->SetContent(SNullWidget::NullWidget);
		return;
	}

	FDreamEditorModule& M = Module();
	TArray<FDreamRecoveryRow> Rows;
	FString CompareText;
	const bool bSkipped = Impl->bSafeMode && Impl->SafeModeSession.IsSet();
	for (const FGuid& Id : Impl->PendingRecoveries)
	{
		TSharedPtr<FDreamBuffer> Buffer = M.GetBufferManager().FindById(Id);
		if (!Buffer.IsValid() || !Buffer->HasPendingRecovery())
		{
			continue;
		}
		const FDreamRecoveredBuffer* Details = Impl->RecoveryDetails.Find(Id);
		FDreamRecoveryRow& Row = Rows.AddDefaulted_GetRef();
		Row.Id = Id;
		Row.Name = FText::FromString(Buffer->GetDisplayName());
		Row.Where = DreamTabRecoveryLocal::WhereOf(Buffer->GetPath());
		Row.bConflict = Buffer->GetKind() == EDreamBufferKind::Recovered || (Details != nullptr && Details->bConflict);
		Row.LastEditUtc = Details != nullptr ? Details->LastEditUtc : FDateTime();
		const FString Shown = Buffer->GetKind() == EDreamBufferKind::Recovered ? DreamTabRecoveryLocal::DiskTextOf(*Buffer) : Buffer->GetTextAsString();
		DreamRecoveryUi::CountLines(Shown, Buffer->GetPendingRecoveryText().GetValue(), Row.LinesAdded, Row.LinesRemoved);
		if (Impl->CompareFor == Id && Impl->CompareSection.IsEmpty())
		{
			CompareText = DreamRecoveryUi::UnifiedDiff(Shown, Buffer->GetPendingRecoveryText().GetValue(),
				LOCTEXT("Recovery.DiffShown2", "shown now").ToString(), LOCTEXT("Recovery.DiffRecovered2", "recovered").ToString());
		}
	}
	if (bSkipped)
	{
		// Safe mode (§10.8): every buffer of the skipped session, the ones with recovered changes first.
		for (const FDreamRecoveredBuffer& Rec : Impl->SafeModeSession->Buffers)
		{
			if (M.GetBufferManager().FindById(Rec.Id).IsValid())
			{
				continue;
			}
			FDreamRecoveryRow& Row = Rows.AddDefaulted_GetRef();
			Row.Id = Rec.Id;
			Row.Name = Rec.DisplayName.IsEmpty() ? LOCTEXT("Recovery.UntitledName", "Untitled") : FText::FromString(Rec.DisplayName);
			Row.Where = DreamTabRecoveryLocal::WhereOf(Rec.Path);
			Row.bConflict = Rec.bConflict;
			Row.LastEditUtc = Rec.LastEditUtc;
			Row.LinesAdded = Rec.bCounted ? Rec.LinesAdded : 0;
			Row.LinesRemoved = Rec.bCounted ? Rec.LinesRemoved : 0;
			if (Impl->CompareFor == Rec.Id && Impl->CompareSection.IsEmpty())
			{
				FString Shown = Rec.CleanText.Get(FString());
				if (Rec.Kind != EDreamBufferKind::Scratch && !Rec.Path.IsEmpty())
				{
					FFileHelper::LoadFileToString(Shown, *Rec.Path);
					Shown = DreamText::ToModelText(Shown);
				}
				CompareText = DreamRecoveryUi::UnifiedDiff(Shown, Rec.RecoveredText, LOCTEXT("Recovery.DiffDisk3", "on disk").ToString(),
					LOCTEXT("Recovery.DiffRecovered3", "recovered").ToString());
			}
		}
	}

	// (instances) One section per other namespace with buffers still offered, then the primary's tabs (read-only).
	TArray<FDreamRecoverySection> Sections;
	{
		const FDreamInstances& Instances = M.GetInstances();
		auto AddSection = [this, &Sections, &CompareText, &Instances](const FDreamOrphanNamespace& Orphan)
		{
			FDreamRecoverySection Section;
			Section.Key = Orphan.Key;
			Section.Title = DreamTabRecoveryLocal::SectionTitle(Orphan);
			Section.bReadOnly = Orphan.bReadOnly;
			for (const FDreamRecoveredBuffer& Rec : Orphan.Buffers)
			{
				if (!Orphan.bReadOnly && Instances.IsOrphanBufferResolved(Orphan.Key, Rec.Id))
				{
					continue;
				}
				FDreamRecoveryRow& Row = Section.Rows.AddDefaulted_GetRef();
				Row.Id = Rec.Id;
				Row.Name = Rec.DisplayName.IsEmpty() ? LOCTEXT("Recovery.UntitledName2", "Untitled") : FText::FromString(Rec.DisplayName);
				Row.Where = DreamTabRecoveryLocal::WhereOf(Rec.Path);
				Row.bConflict = Rec.bConflict;
				Row.LastEditUtc = Rec.LastEditUtc;
				Row.LinesAdded = Rec.LinesAdded;
				Row.LinesRemoved = Rec.LinesRemoved;
				if (Impl->CompareSection == Orphan.Key && Impl->CompareFor == Rec.Id)
				{
					CompareText = DreamRecoveryUi::UnifiedDiff(DreamTabRecoveryLocal::OrphanBaseText(Rec), Rec.RecoveredText,
						Rec.Path.IsEmpty() ? LOCTEXT("Recovery.DiffEmpty", "empty").ToString() : LOCTEXT("Recovery.DiffDisk4", "on disk").ToString(),
						LOCTEXT("Recovery.DiffRecovered4", "recovered").ToString());
				}
			}
			if (Section.Rows.Num() > 0)
			{
				Sections.Add(MoveTemp(Section));
			}
		};
		for (const FDreamOrphanNamespace& Orphan : Instances.GetOrphans())
		{
			AddSection(Orphan);
		}
		if (Instances.GetPrimaryView().IsSet())
		{
			AddSection(Instances.GetPrimaryView().GetValue());
		}
	}

	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	auto Row = [WeakTab](TFunction<void(SDreamTab&, const FGuid&)> Body)
	{
		return FDreamOnRecoveryRow::CreateLambda([WeakTab, Body](const FGuid& Id)
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				Body(*Tab, Id);
			}
		});
	};
	Impl->ReviewHost->SetContent(
		SNew(SDreamRecoveryReview)
		.Title(bSkipped ? LOCTEXT("Recovery.ReviewTitleSafe", "Recovery review · the session safe mode did not restore")
			: LOCTEXT("Recovery.ReviewTitle", "Recovery review · this session's recovered buffers"))
		.Rows(Rows)
		.CompareFor(Impl->CompareFor)
		.CompareText(CompareText)
		.OnRestore(Row([](SDreamTab& Tab, const FGuid& Id) { Tab.RestoreRecovered(Id, false); }))
		.OnRestoreAsNewTab(Row([](SDreamTab& Tab, const FGuid& Id) { Tab.RestoreRecovered(Id, true); }))
		.OnCompare(Row([](SDreamTab& Tab, const FGuid& Id) { Tab.ToggleRecoveryCompare(Id); }))
		.OnDiscard(Row([](SDreamTab& Tab, const FGuid& Id) { Tab.DiscardRecovered({ Id }); }))
		.OnClose_Lambda([WeakTab]()
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				Tab->ToggleRecoveryReview();
			}
		})
		.Sections(Sections)
		.CompareSection(Impl->CompareSection)
		.OnSectionAction_Lambda([WeakTab](const FString& Section, const FGuid& Id, EDreamRecoveryRowAction Action)
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				Tab->HandleOrphanAction(Section, Id, Action);
			}
		}));
}

// ---------------------------------------------------------------------------------------------------------------------
// The other namespaces (instances, §10.5, §10.6, §10.1)
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::HandleInstancesChanged()
{
	RebuildBanners();
	RefreshRecoveryUi();
}

void SDreamTab::HandOrphansToInstances(FDreamRecoveredSession& Session)
{
	FDreamInstances& Instances = Module().GetInstances();
	if (Session.Orphans.Num() > 0)
	{
		Instances.AddSameRootOrphans(MoveTemp(Session.Orphans));
		Session.Orphans.Reset();
	}
	// §10.1: after the primary's reconcile (its first checkpoint is queued); a secondary never scans other roots.
	Instances.StartCrossRootScan();
}

void SDreamTab::HandleOrphanAction(const FString& Section, const FGuid& BufferId, EDreamRecoveryRowAction Action)
{
	FDreamEditorModule& M = Module();
	FDreamInstances& Instances = M.GetInstances();
	const TOptional<FDreamOrphanNamespace>& View = Instances.GetPrimaryView();
	const FDreamOrphanNamespace* Orphan = View.IsSet() && View->Key == Section ? &View.GetValue() : Instances.FindOrphan(Section);
	const FDreamRecoveredBuffer* Rec = Orphan != nullptr ? Orphan->Buffers.FindByPredicate([&BufferId](const FDreamRecoveredBuffer& B) { return B.Id == BufferId; }) : nullptr;
	if (Rec == nullptr)
	{
		return;
	}
	const FString Display = Rec->DisplayName.IsEmpty() ? FString(TEXT("Untitled")) : Rec->DisplayName;
	if (Action == EDreamRecoveryRowAction::Compare)
	{
		const bool bOpen = Impl->CompareFor == BufferId && Impl->CompareSection == Section;
		Impl->CompareFor = bOpen ? FGuid() : BufferId;
		Impl->CompareSection = bOpen ? FString() : Section;
		RefreshRecoveryUi();
		return;
	}
	if (Action == EDreamRecoveryRowAction::OpenCopy)
	{
		// §10.5 View its tabs: [Open a copy] makes a new scratch buffer; the other editor's namespace is never written.
		// (scratch, PO-05) The copy is a scratch file.
		NewScratchTab(Rec->RecoveredText);
		return;
	}
	if (Orphan->bReadOnly || Impl->DiscardsInFlight.Contains(BufferId))
	{
		return;
	}
	if (M.GetRunService().IsRunActive())
	{
		ShowStatusMessage(LOCTEXT("Recovery.OrphanNotDuringRun", "Recovered text is restored when the run has ended."));   // RT-01
		return;
	}
	// Copies: the orphan may be finalized (and removed) by the calls below.
	const FString Key = Section;
	const FDreamRecoveredBuffer Copy = *Rec;
	switch (Action)
	{
	case EDreamRecoveryRowAction::RestoreAsNewTab:
		NewScratchTab(Copy.RecoveredText);   // (scratch, PO-05) a scratch file
		Instances.ResolveOrphanBuffer(Key, BufferId, false);
		break;
	case EDreamRecoveryRowAction::Restore:
	{
		FDreamBufferManager& Buffers = M.GetBufferManager();
		if (Copy.Kind == EDreamBufferKind::Scratch || Copy.Path.IsEmpty())
		{
			// (scratch, PO-05) An untitled orphan comes back as a scratch file; a named one keeps its name.
			NewScratchTab(Copy.RecoveredText, Display.StartsWith(TEXT("Untitled"), ESearchCase::CaseSensitive) ? FString() : Display);
		}
		else if (Copy.bConflict || !FPaths::FileExists(Copy.Path))
		{
			// §10.7.2's rule for a text whose file changed or went: a "(recovered)" tab beside the disk version, dirty.
			TSharedRef<FDreamBuffer> Recovered = Buffers.AddRestored(FGuid::NewGuid(), EDreamBufferKind::Recovered,
				DreamTabRecoveryLocal::RecoveredName(FPaths::GetCleanFilename(Copy.Path)).ToString(), Copy.Path, Copy.RecoveredText, Copy.Format, Copy.Base);
			Recovered->SetCleanText(FString());
			ActivateBuffer(Recovered->GetId());
		}
		else
		{
			FText Error;
			TSharedPtr<FDreamBuffer> Buffer = Buffers.FindByPath(Copy.Path);
			if (!Buffer.IsValid())
			{
				Buffer = Buffers.OpenFile(Copy.Path, Error);
			}
			if (!Buffer.IsValid() || Buffer->HasPendingRecovery())
			{
				ShowStatusMessage(Buffer.IsValid() ? LOCTEXT("Recovery.OrphanFilePending", "That file has a recovered text of this session waiting; resolve it first.")
					: FText::Format(LOCTEXT("Recovery.OrphanFileRefused", "Dream Python Editor could not open {0}: {1}"), FText::FromString(Copy.Path), Error));
				return;
			}
			const dream::TextSnapshot Shown = Buffer->Snapshot();
			// One undoable edit (U30): Ctrl+Z brings back the text shown before; dirty, nothing written to disk (PER-03).
			if (!FDreamUndoHistory::ReplaceAllText(*Buffer, Copy.RecoveredText, LOCTEXT("Recovery.RestoreUndoLabel2", "Restore recovered text")))
			{
				ShowStatusMessage(LOCTEXT("Recovery.OrphanRefused", "The recovered text could not be restored now (the buffer is read-only)."));
				return;
			}
			M.GetLocalHistory().AddEntry(Buffer->GetId(), EDreamHistoryKind::Restore, Shown, Buffer->GetPath(), Buffer->GetDisplayName());
			ActivateBuffer(Buffer->GetId());
		}
		Instances.ResolveOrphanBuffer(Key, BufferId, false);
		break;
	}
	case EDreamRecoveryRowAction::Discard:
	{
		// §10.7.3 Discard: the text moves to local history first; it resolves only once that entry is durable.
		Impl->DiscardsInFlight.Add(BufferId);
		TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
		M.GetLocalHistory().AddEntry(BufferId, EDreamHistoryKind::Discarded, DreamTabRecoveryLocal::ModelSnapshot(Copy.RecoveredText), Copy.Path,
			Copy.DisplayName, TOptional<int32>(), [WeakTab, Key, BufferId](bool bKept)
			{
				TSharedPtr<SDreamTab> Tab = WeakTab.Pin();
				if (!Tab.IsValid() || !FDreamEditorModule::IsAvailable())
				{
					return;
				}
				Tab->Impl->DiscardsInFlight.Remove(BufferId);
				if (bKept)
				{
					FDreamEditorModule::Get().GetInstances().ResolveOrphanBuffer(Key, BufferId, true);
				}
				else
				{
					Tab->ShowStatusMessage(LOCTEXT("Recovery.OrphanDiscardNotKept", "The recovered text could not be kept in local history; it was not discarded."));
				}
				Tab->RefreshRecoveryUi();
			});
		break;
	}
	default:
		break;
	}
	if (Impl->CompareSection == Key && Impl->CompareFor == BufferId)
	{
		Impl->CompareFor.Invalidate();
		Impl->CompareSection.Reset();
	}
	RefreshRecoveryUi();
	RebuildBanners();
	RefreshTabStrip();
}

void SDreamTab::AddInstanceBanners()
{
	FDreamEditorModule& M = Module();
	FDreamInstances& Instances = M.GetInstances();
	const FDreamJournal& Journal = M.GetJournal();
	TWeakPtr<SDreamTab> WeakTab = SharedThis(this);
	auto Dismiss = [WeakTab](const FString& Id)
	{
		return FSimpleDelegate::CreateLambda([WeakTab, Id]()
		{
			if (TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				Tab->Impl->DismissedInstanceNotices.Add(Id);
				Tab->RebuildBanners();
			}
		});
	};
	auto OpenReview = [WeakTab](bool bViewPrimary)
	{
		return FSimpleDelegate::CreateLambda([WeakTab, bViewPrimary]()
		{
			TSharedPtr<SDreamTab> Tab = WeakTab.Pin();
			if (!Tab.IsValid() || !FDreamEditorModule::IsAvailable())
			{
				return;
			}
			if (bViewPrimary)
			{
				FDreamEditorModule::Get().GetInstances().RequestPrimaryView();
			}
			Tab->Impl->bShowReview = true;
			Tab->RefreshRecoveryUi();
		});
	};

	// §10.1, §10.11: an override that was rejected or did not answer, and the temp fallback: persistent until closed.
	const TArray<FText>& Notices = Instances.GetStorageNotices();
	for (int32 Index = 0; Index < Notices.Num(); ++Index)
	{
		const FString Id = FString::Printf(TEXT("storage-%d"), Index);
		if (!Impl->DismissedInstanceNotices.Contains(Id))
		{
			Impl->BannerHost->AddSlot().AutoHeight()
			[
				SNew(SDreamRecoveryBanner).Message(Notices[Index]).bShowActions(false).OnDismiss(Dismiss(Id))
			];
		}
	}

	// §10.5 "At tab spawn", held by a live instance.
	if (Journal.IsOpen() && Journal.GetRole() == EDreamInstanceRole::Secondary && !Impl->DismissedInstanceNotices.Contains(TEXT("secondary")))
	{
		const FText Message = Instances.GetPrimaryPid() > 0
			? FText::Format(LOCTEXT("Instances.Secondary", "Another Unreal Editor (PID {0}) has this project open in Dream Python Editor. This instance journals separately."),
				FText::AsNumber(Instances.GetPrimaryPid(), &FNumberFormattingOptions::DefaultNoGrouping()))
			: LOCTEXT("Instances.SecondaryNoPid", "Another Unreal Editor has this project open in Dream Python Editor. This instance journals separately.");
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(Message)
			.bShowActions(false)
			.bWarning(false)
			.ExtraActionText(LOCTEXT("Instances.ViewItsTabs", "View its tabs"))
			.OnExtraAction(OpenReview(true))
			.OnDismiss(Dismiss(TEXT("secondary")))
		];
	}
	// §10.5: the 30 s poll found the primary gone without CLEAN.
	if (Instances.HasPrimaryEndedUnexpectedly() && !Impl->DismissedInstanceNotices.Contains(TEXT("primary-ended")))
	{
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(LOCTEXT("Instances.PrimaryEnded", "The other editor instance ended unexpectedly."))
			.bShowActions(false)
			.ExtraActionText(LOCTEXT("Instances.ReviewItsBuffers", "Review its buffers"))
			.OnExtraAction(OpenReview(false))
			.OnDismiss(Dismiss(TEXT("primary-ended")))
		];
	}
	// §10.1 "Moved projects".
	for (const FDreamMovedProject& Moved : Instances.GetMovedProjects())
	{
		const int32 Days = Moved.LastSeenUtc.GetTicks() > 0 ? FMath::Max(0, static_cast<int32>((FDateTime::UtcNow() - Moved.LastSeenUtc).GetTotalDays())) : 0;
		const FString OldHash = Moved.OldHash;
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(FText::Format(LOCTEXT("Instances.Moved", "Dream Python Editor found a journal for {0} from {1} (last used {2} {2}|plural(one=day,other=days) ago)."),
				FText::FromString(Moved.Name), FText::FromString(FPaths::GetPath(Moved.OldProjectPath)), FText::AsNumber(Days)))
			.bShowActions(false)
			.bWarning(false)
			.ExtraActionText(LOCTEXT("Instances.Import", "Import"))
			.OnExtraAction_Lambda([OldHash]() { if (FDreamEditorModule::IsAvailable()) { FDreamEditorModule::Get().GetInstances().ImportMovedProject(OldHash); } })
			.SecondActionText(LOCTEXT("Instances.Ignore", "Ignore"))
			.OnSecondAction_Lambda([OldHash]() { if (FDreamEditorModule::IsAvailable()) { FDreamEditorModule::Get().GetInstances().IgnoreMovedProject(OldHash); } })
		];
	}
	// §10.7.1: n is 0 and Review lists only orphaned, imported or compat buffers.
	if (Impl->PendingRecoveries.Num() == 0 && Instances.CountOfferedBuffers() > 0 && !Impl->DismissedInstanceNotices.Contains(TEXT("orphans")))
	{
		Impl->BannerHost->AddSlot().AutoHeight()
		[
			SNew(SDreamRecoveryBanner)
			.Message(LOCTEXT("Instances.OrphansOnly", "Buffers from another editor instance or an imported journal can be recovered."))
			.bShowActions(false)
			.ExtraActionText(LOCTEXT("Instances.Review", "Review"))
			.OnExtraAction(OpenReview(false))
			.OnDismiss(Dismiss(TEXT("orphans")))
		];
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------------------------------------------------

void SDreamTab::ApplyRecoveryForTest(FDreamRecoveredSession&& Session)
{
	ApplyRestore(MoveTemp(Session));
}

void SDreamTab::EnterSafeModeForTest(FDreamRecoveredSession&& Session)
{
	Impl->bSafeMode = true;
	Module().GetRestoreGuard().EnterSafeMode(EDreamSafeModeReason::Switch);
	ApplySafeMode(MoveTemp(Session));
}

#undef LOCTEXT_NAMESPACE
