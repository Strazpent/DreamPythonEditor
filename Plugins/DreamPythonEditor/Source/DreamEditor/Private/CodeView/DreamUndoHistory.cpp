// SPDX-License-Identifier: Apache-2.0
// Text undo on dream::UndoTree and the grouped-edit API (TDD §4.2, §4.15 row (3), EDT-08).
#include "CodeView/DreamUndoHistory.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Run/DreamRunService.h"
#include "dream/Diff.h"

#include <algorithm>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamUndoHistoryLocal
{
	/** One history per buffer; an entry whose buffer is gone is purged at the next lookup. */
	TMap<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Registry()
	{
		static TMap<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>> Map;
		return Map;
	}

	bool IsRunActive()
	{
		return FDreamEditorModule::IsAvailable() && FDreamEditorModule::Get().GetRunService().IsRunActive();
	}

	dream::Offset ToOffset(const dream::TextSnapshot& Snapshot, int32 Line, int32 Column)
	{
		const dream::LineNo SafeLine = FMath::Clamp<dream::LineNo>(Line, 0, Snapshot.LineCount() - 1);
		return Snapshot.LineStart(SafeLine) + FMath::Clamp(Column, 0, Snapshot.LineLength(SafeLine));
	}

	dream::CaretRange ToCaret(const dream::TextSnapshot& Snapshot, const FDreamTextSelection& Selection)
	{
		dream::CaretRange Out;
		Out.Anchor = ToOffset(Snapshot, Selection.AnchorLine, Selection.AnchorColumn);
		Out.Active = ToOffset(Snapshot, Selection.CaretLine, Selection.CaretColumn);
		return Out;
	}

	FDreamTextSelection ToSelection(const dream::TextSnapshot& Snapshot, const dream::CaretRange& Caret)
	{
		const dream::Offset Length = Snapshot.Length();
		const dream::Offset Anchor = FMath::Clamp<dream::Offset>(Caret.Anchor, 0, Length);
		const dream::Offset Active = FMath::Clamp<dream::Offset>(Caret.Active, 0, Length);
		FDreamTextSelection Out;
		Out.AnchorLine = Snapshot.LineOf(Anchor);
		Out.AnchorColumn = static_cast<int32>(Anchor - Snapshot.LineStart(Out.AnchorLine));
		Out.CaretLine = Snapshot.LineOf(Active);
		Out.CaretColumn = static_cast<int32>(Active - Snapshot.LineStart(Out.CaretLine));
		return Out;
	}

	/** A line and column that exist in Snapshot (the end of a line counts; nothing past it). */
	bool IsValidPosition(const dream::TextSnapshot& Snapshot, int32 Line, int32 Column)
	{
		return Line >= 0 && Line < Snapshot.LineCount() && Column >= 0 && Column <= Snapshot.LineLength(Line);
	}

	dream::UndoKind KindOfOrigin(dream::EditOrigin Origin)
	{
		switch (Origin)
		{
		case dream::EditOrigin::Reload: return dream::UndoKind::Reload;
		case dream::EditOrigin::Ime:    return dream::UndoKind::Ime;
		case dream::EditOrigin::Typing: return dream::UndoKind::Typing;
		default:                       return dream::UndoKind::Command;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------

void FDreamUndoHistory::PurgeStale()
{
	// A history whose buffer is gone frees its memory here (and stops counting toward the process cap).
	for (auto It = DreamUndoHistoryLocal::Registry().CreateIterator(); It; ++It)
	{
		if (!It->Value.IsValid() || !It->Value->Buffer.IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

TSharedRef<FDreamUndoHistory> FDreamUndoHistory::For(const TSharedRef<FDreamBuffer>& Buffer)
{
	PurgeStale();
	TMap<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Map = DreamUndoHistoryLocal::Registry();
	if (const TSharedPtr<FDreamUndoHistory>* Found = Map.Find(&Buffer.Get()))
	{
		return Found->ToSharedRef();
	}
	TSharedRef<FDreamUndoHistory> History = MakeShared<FDreamUndoHistory>(Buffer);
	History->Bind();
	Map.Add(&Buffer.Get(), History);
	return History;
}

TSharedPtr<FDreamUndoHistory> FDreamUndoHistory::Find(const FDreamBuffer& Buffer)
{
	const TSharedPtr<FDreamUndoHistory>* Found = DreamUndoHistoryLocal::Registry().Find(&Buffer);
	return Found != nullptr && Found->IsValid() && (*Found)->Buffer.IsValid() ? *Found : nullptr;
}

FDreamUndoHistory::FDreamUndoHistory(const TSharedRef<FDreamBuffer>& InBuffer)
	: Buffer(InBuffer)
	, Tree(InBuffer->GetModel().Version())
	, Last(InBuffer->Snapshot())
{
	bWasDirty = InBuffer->IsDirty();
	if (bWasDirty)
	{
		Tree.ClearSavePoint();   // a restored dirty text: no node is the disk text
	}
}

FDreamUndoHistory::~FDreamUndoHistory()
{
	if (const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin())
	{
		Pinned->OnChanged().Remove(ChangedHandle);
		Pinned->OnStateChanged().Remove(StateChangedHandle);
	}
	if (RunStateHandle.IsValid() && FDreamEditorModule::IsAvailable())
	{
		FDreamEditorModule::Get().GetRunService().OnRunStateChanged().Remove(RunStateHandle);
	}
}

void FDreamUndoHistory::Bind()
{
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	check(Pinned.IsValid());
	ChangedHandle = Pinned->OnChanged().AddSP(this, &FDreamUndoHistory::HandleBufferChanged);
	StateChangedHandle = Pinned->OnStateChanged().AddSP(this, &FDreamUndoHistory::HandleBufferStateChanged);
	if (FDreamEditorModule::IsAvailable())
	{
		RunStateHandle = FDreamEditorModule::Get().GetRunService().OnRunStateChanged().AddSP(this, &FDreamUndoHistory::HandleRunStateChanged);
	}
}

// ---- The code view ----

void FDreamUndoHistory::AttachView(const TSharedRef<SDreamCodeView>& InView)
{
	View = InView;
}

void FDreamUndoHistory::DetachView(const SDreamCodeView* InView)
{
	if (View.Pin().Get() == InView || !View.IsValid())
	{
		View.Reset();
	}
}

void FDreamUndoHistory::SetNextEdit(dream::UndoKind Kind, const TOptional<dream::CaretRange>& Before, const TOptional<dream::CaretRange>& After)
{
	NextKind = Kind;
	NextBefore = Before;
	NextAfter = After;
	NextFrame = GFrameCounter;
	bHasNext = true;
}

void FDreamUndoHistory::CloseTypingGroup()
{
	Tree.CloseGroup();
}

void FDreamUndoHistory::BeginImeGroup()
{
	if (!bImeGroupOpen)
	{
		Tree.BeginGroup(dream::UndoKind::Ime);
		bImeGroupOpen = true;
	}
}

void FDreamUndoHistory::EndImeGroup()
{
	if (bImeGroupOpen)
	{
		bImeGroupOpen = false;
		Tree.EndGroup();
	}
}

TOptional<dream::CaretRange> FDreamUndoHistory::ViewCaret(const dream::TextSnapshot& Snapshot) const
{
	if (const TSharedPtr<SDreamCodeView> Pinned = View.Pin())
	{
		return DreamUndoHistoryLocal::ToCaret(Snapshot, Pinned->GetSelection());
	}
	return TOptional<dream::CaretRange>();
}

dream::Offset FDreamUndoHistory::MapOffset(const dream::ChangeSet& Changes, dream::Offset At, bool bStickRight)
{
	int64 Delta = 0;
	for (const dream::ChangeRange& Range : Changes.Ranges)
	{
		const dream::Offset RangeEnd = Range.At + Range.OldLen;
		if (At < Range.At || (At == Range.At && !bStickRight))
		{
			break;
		}
		if (At > RangeEnd || (At == RangeEnd && Range.OldLen > 0))
		{
			Delta += Range.NewLen - Range.OldLen;
			continue;
		}
		return Range.At + Delta + Range.NewLen;   // inside, or an insertion at At that goes before it
	}
	return At + Delta;
}

// ---- Recording ----

void FDreamUndoHistory::HandleBufferChanged(const dream::ChangeSet& Changes)
{
	using namespace DreamUndoHistoryLocal;
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}
	const dream::TextSnapshot Before = Last;
	const dream::TextSnapshot After = Pinned->Snapshot();
	const bool bHints = bHasNext && NextFrame == GFrameCounter;
	bHasNext = false;

	if (Changes.Origin == dream::EditOrigin::Load)
	{
		// A load (open, the REPL's clear after a submit, a restored session) is a new text nobody undoes into (§4.2).
		bImeGroupOpen = false;
		Tree.Reset(After.Version());
		if (Pinned->IsDirty())
		{
			Tree.ClearSavePoint();
		}
	}
	else if (bApplyingStep || Changes.Origin == dream::EditOrigin::Undo)
	{
		Tree.Record(Changes, Before, After, dream::UndoKind::Typing, FPlatformTime::Seconds());   // re-syncs the version
	}
	else
	{
		const dream::UndoKind Kind = bHints ? NextKind : KindOfOrigin(Changes.Origin);
		TOptional<dream::CaretRange> CaretBefore = bHints && NextBefore.IsSet() ? NextBefore : ViewCaret(Before);
		TOptional<dream::CaretRange> CaretAfter;
		if (bHints && NextAfter.IsSet())
		{
			CaretAfter = NextAfter;
		}
		else if (CaretBefore.IsSet())
		{
			// What the view will show: its selection mapped through the change (SDreamCodeView::SyncWidget).
			CaretAfter = dream::CaretRange{ MapOffset(Changes, CaretBefore->Anchor, false), MapOffset(Changes, CaretBefore->Active, false) };
		}
		const std::span<const dream::CaretRange> BeforeSpan = CaretBefore.IsSet()
			? std::span<const dream::CaretRange>(&CaretBefore.GetValue(), 1) : std::span<const dream::CaretRange>();
		const std::span<const dream::CaretRange> AfterSpan = CaretAfter.IsSet()
			? std::span<const dream::CaretRange>(&CaretAfter.GetValue(), 1) : std::span<const dream::CaretRange>();
		Tree.Record(Changes, Before, After, Kind, FPlatformTime::Seconds(), BeforeSpan, AfterSpan);
		EnforceProcessCap();
	}
	Last = After;
	ShowNotices();
	ModelChangedEvent.Broadcast(Changes, Before);
}

void FDreamUndoHistory::HandleBufferStateChanged()
{
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}
	const bool bDirty = Pinned->IsDirty();
	// R9: a save (or a reload, or an undo back to the saved text) makes the current node the save node and closes the
	// group; the history survives it. The model must be the text the history last recorded (FDreamBuffer::ApplyEdit sends
	// its state change before the change itself; that change is recorded next and is already at the save node then).
	if (!bDirty && (bWasDirty || !Tree.IsAtSavePoint()) && Pinned->GetModel().Version() == Last.Version())
	{
		Tree.MarkSavePoint();
	}
	bWasDirty = bDirty;
}

void FDreamUndoHistory::HandleRunStateChanged(const FDreamRunRecord& Record)
{
	// R8: the text a run of this buffer starts from closes the open group and marks RunBoundary(#N) (§10 maps a local
	// history run entry to it).
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (!Pinned.IsValid() || Record.BufferId != Pinned->GetId() || Record.RunNumber <= 0
		|| static_cast<uint32>(Record.RunNumber) == LastBoundaryRun || Record.SourceVersion != Pinned->GetModel().Version())
	{
		return;
	}
	LastBoundaryRun = static_cast<uint32>(Record.RunNumber);
	Tree.MarkRunBoundary(LastBoundaryRun);
}

void FDreamUndoHistory::ShowNotices()
{
	if (!Tree.WasTrimmed() && !Tree.WasReset())
	{
		return;
	}
	const FText Notice = Tree.WasReset() ? LOCTEXT("CodeView.UndoReset", "Undo history reset")
		: LOCTEXT("CodeView.UndoTrimmed", "Older undo history trimmed");
	if (Tree.WasReset())
	{
		UE_LOG(LogDream, Warning, TEXT("Dream: the undo history of %s was reset (its text changed without it)"),
			Buffer.IsValid() ? *Buffer.Pin()->GetDisplayName() : TEXT("a buffer"));
	}
	Tree.ClearNotices();
	if (const TSharedPtr<SDreamCodeView> Pinned = View.Pin())
	{
		Pinned->ShowCommandRefusal(EDreamCommand::None, Notice);
	}
}

void FDreamUndoHistory::EnforceProcessCap()
{
	PurgeStale();
	TMap<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Map = DreamUndoHistoryLocal::Registry();
	size_t Total = 0;
	for (const TPair<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Entry : Map)
	{
		Total += Entry.Value.IsValid() ? Entry.Value->Tree.RetainedBytes() : 0;
	}
	if (Total <= ProcessCapBytes)
	{
		return;
	}
	std::vector<dream::UndoTree*> Trees;
	for (const TPair<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Entry : Map)
	{
		if (Entry.Value.IsValid())
		{
			Trees.push_back(&Entry.Value->Tree);
		}
	}
	dream::EnforceUndoProcessCap(std::span<dream::UndoTree* const>(Trees.data(), Trees.size()), ProcessCapBytes);
	for (const TPair<const FDreamBuffer*, TSharedPtr<FDreamUndoHistory>>& Entry : Map)
	{
		if (Entry.Value.IsValid())
		{
			Entry.Value->ShowNotices();
		}
	}
}

// ---- Undo and redo ----

bool FDreamUndoHistory::CanUndo() const
{
	return Tree.CanUndo();
}

bool FDreamUndoHistory::CanRedo() const
{
	return Tree.CanRedo();
}

bool FDreamUndoHistory::Undo()
{
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (!Pinned.IsValid() || DreamUndoHistoryLocal::IsRunActive())
	{
		return false;
	}
	EndImeGroup();
	return ApplyStep(Tree.Undo(Pinned->Snapshot()), /*bUndo*/ true);
}

bool FDreamUndoHistory::Redo()
{
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (!Pinned.IsValid() || DreamUndoHistoryLocal::IsRunActive())
	{
		return false;
	}
	EndImeGroup();
	return ApplyStep(Tree.Redo(Pinned->Snapshot()), /*bUndo*/ false);
}

bool FDreamUndoHistory::ApplyStep(dream::UndoStep&& Step, bool bUndo)
{
	const TSharedPtr<FDreamBuffer> Pinned = Buffer.Pin();
	if (Step.Status == dream::UndoStatus::Reset)
	{
		ShowNotices();
		return false;
	}
	if (Step.Status != dream::UndoStatus::Applied || !Pinned.IsValid())
	{
		return false;
	}
	bApplyingStep = true;
	const dream::ChangeSet Changes = Pinned->ApplyEdit(Step.Batch);
	bApplyingStep = false;
	if (Changes.IsEmpty() && !Step.Batch.Ranges.empty())
	{
		UE_LOG(LogDream, Warning, TEXT("Dream: an undo step of %s did not apply to its text"), *Pinned->GetDisplayName());
		return false;
	}
	if (const TSharedPtr<SDreamCodeView> Pinned2 = View.Pin())
	{
		if (!Step.Carets.empty())
		{
			Pinned2->SetSelection(DreamUndoHistoryLocal::ToSelection(Pinned->Snapshot(), Step.Carets[0]));
		}
		if (!Step.Label.empty())
		{
			const FText Label = FText::FromString(DreamText::ToFString(Step.Label));
			Pinned2->ShowCommandRefusal(EDreamCommand::None, bUndo ? FText::Format(LOCTEXT("CodeView.UndidStep", "Undo: {0}"), Label)
				: FText::Format(LOCTEXT("CodeView.RedidStep", "Redo: {0}"), Label));
		}
	}
	return true;
}

// ---- The grouped-edit API ----

bool FDreamUndoHistory::ApplyEditGroup(FDreamBuffer& Buffer, const FDreamEditGroup& Group)
{
	using namespace DreamUndoHistoryLocal;
	const dream::TextSnapshot Snapshot = Buffer.Snapshot();
	dream::EditBatch Batch;
	Batch.Origin = Group.Origin;
	for (const FDreamTextEdit& Edit : Group.Edits)
	{
		if (!IsValidPosition(Snapshot, Edit.StartLine, Edit.StartColumn) || !IsValidPosition(Snapshot, Edit.EndLine, Edit.EndColumn))
		{
			return false;
		}
		const dream::Offset Start = ToOffset(Snapshot, Edit.StartLine, Edit.StartColumn);
		const dream::Offset End = ToOffset(Snapshot, Edit.EndLine, Edit.EndColumn);
		if (End < Start)
		{
			return false;
		}
		dream::EditRange& Range = Batch.Ranges.emplace_back();
		Range.At = Start;
		Range.DeleteLen = End - Start;
		Range.Insert = std::u16string(DreamText::ToU16(DreamText::ToModelText(Edit.Text)));
	}
	const dream::UndoKind Kind = Group.Origin == dream::EditOrigin::Reload ? dream::UndoKind::Reload : dream::UndoKind::Command;
	return ApplyBatchInternal(Buffer, Batch, Group.Label, Kind, Group.SelectionAfter, EAfter::EndOfLastEdit, Group.bEvenIfReadOnly);
}

bool FDreamUndoHistory::ReplaceAllText(FDreamBuffer& Buffer, const FString& NewText, const FText& Label, dream::EditOrigin Origin,
	bool bEvenIfReadOnly)
{
	const FString ModelText = DreamText::ToModelText(NewText);
	const dream::TextSnapshot Old = Buffer.Snapshot();
	if (Old.Length() == ModelText.Len() && Old.Text() == DreamText::ToU16(ModelText))
	{
		return true;
	}
	const dream::TextBuffer NewModel(DreamText::ToU16(ModelText));
	const dream::EditBatch Batch = dream::Diff::ToMinimalEdits(Old, NewModel.Snapshot(), Origin);
	const dream::UndoKind Kind = Origin == dream::EditOrigin::Reload ? dream::UndoKind::Reload : dream::UndoKind::Restore;
	return ApplyBatchInternal(Buffer, Batch, Label, Kind, TOptional<FDreamTextSelection>(), EAfter::Mapped, bEvenIfReadOnly);
}

bool FDreamUndoHistory::ApplyEditBatch(FDreamBuffer& Buffer, const dream::EditBatch& Batch, const FText& Label, dream::UndoKind Kind,
	const TOptional<FDreamTextSelection>& SelectionAfter, bool bEvenIfReadOnly)
{
	return ApplyBatchInternal(Buffer, Batch, Label, Kind, SelectionAfter, EAfter::EndOfLastEdit, bEvenIfReadOnly);
}

bool FDreamUndoHistory::ApplyBatchInternal(FDreamBuffer& Buffer, const dream::EditBatch& Batch, const FText& Label, dream::UndoKind Kind,
	const TOptional<FDreamTextSelection>& SelectionAfter, EAfter Default, bool bEvenIfReadOnly)
{
	using namespace DreamUndoHistoryLocal;
	if (IsRunActive() || (Buffer.IsReadOnly() && !bEvenIfReadOnly))
	{
		return false;   // RT-01: nothing applies while a run is active; a read-only buffer only when the caller says so
	}
	if (Batch.Ranges.empty())
	{
		return true;
	}
	if (!Buffer.GetModel().Validate(Batch))
	{
		return false;
	}
	const TSharedRef<FDreamUndoHistory> History = For(Buffer.AsShared());
	const dream::TextSnapshot Before = Buffer.Snapshot();

	// The caret after the group, in the text after it (the tree keeps it for Redo; the view shows it).
	TOptional<dream::CaretRange> After;
	if (SelectionAfter.IsSet())
	{
		dream::TextBuffer Preview(Before.Text());
		Preview.Apply(Batch);
		After = ToCaret(Preview.Snapshot(), SelectionAfter.GetValue());
	}
	else if (Default == EAfter::EndOfLastEdit)
	{
		std::vector<dream::EditRange> Sorted = Batch.Ranges;
		std::sort(Sorted.begin(), Sorted.end(), [](const dream::EditRange& A, const dream::EditRange& B) { return A.At < B.At; });
		int64 Delta = 0;
		for (size_t Index = 0; Index + 1 < Sorted.size(); ++Index)
		{
			Delta += static_cast<int64>(Sorted[Index].Insert.size()) - Sorted[Index].DeleteLen;
		}
		const dream::Offset End = Sorted.back().At + Delta + static_cast<dream::Offset>(Sorted.back().Insert.size());
		After = dream::CaretRange{ End, End };
	}

	const FString LabelText = Label.ToString();
	History->EndImeGroup();   // an edit group never joins a composition's group (R5, R6)
	History->Tree.BeginGroup(Kind, DreamText::ToU16(LabelText));
	History->SetNextEdit(Kind, TOptional<dream::CaretRange>(), After);
	const dream::ChangeSet Changes = Buffer.ApplyEdit(Batch);
	History->Tree.EndGroup();
	if (Changes.IsEmpty())
	{
		return false;
	}
	if (const TSharedPtr<SDreamCodeView> ViewPin = History->View.Pin())
	{
		if (After.IsSet())
		{
			ViewPin->SetSelection(ToSelection(Buffer.Snapshot(), After.GetValue()));
		}
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
