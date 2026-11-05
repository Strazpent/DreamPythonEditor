// SPDX-License-Identifier: Apache-2.0
// Runtime items in Problems (OUT-11, TDD §7.8.1 "Runtime (U13)" row). See DreamRuntimeProblems.h.
#include "Output/DreamRuntimeProblems.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Module/DreamEditorModule.h"
#include "dream/TextBuffer.h"

FDreamRuntimeProblems::FDreamRuntimeProblems() = default;

FDreamRuntimeProblems::~FDreamRuntimeProblems()
{
	Items.Reset();
	UnbindUnusedBuffers();
}

void FDreamRuntimeProblems::OnRunStarted(const FString& SourceKey)
{
	// §7.8.1: cleared by the next run started from the same source as the run that raised it, so an error raised in an
	// imported importer.py:88 by a run of Untitled-3 clears when Untitled-3 runs again.
	const int32 Removed = Items.RemoveAll([&SourceKey](const FDreamRuntimeProblem& Item) { return Item.SourceKey == SourceKey; });
	if (Removed > 0)
	{
		UnbindUnusedBuffers();
		ChangedEvent.Broadcast();
	}
}

void FDreamRuntimeProblems::AddRuntimeError(const FDreamRuntimeProblem& Problem)
{
	FDreamRuntimeProblem Item = Problem;
	if (Item.Message.Len() > MaxMessageChars)
	{
		Item.Message.LeftInline(MaxMessageChars - 1);
		Item.Message.AppendChar(TEXT('…'));
	}
	if (!Item.BufferId.IsValid() && FDreamEditorModule::IsAvailable())
	{
		const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().ResolveFilename(Item.File);
		if (Buffer.IsValid())
		{
			Item.BufferId = Buffer->GetId();
			BindBuffer(Buffer);
		}
	}
	Items.Add(MoveTemp(Item));
	ChangedEvent.Broadcast();
}

void FDreamRuntimeProblems::ClearRuntimeItems()
{
	if (Items.IsEmpty())
	{
		return;
	}
	Items.Reset();
	UnbindUnusedBuffers();
	ChangedEvent.Broadcast();
}

void FDreamRuntimeProblems::OnBufferEdited(const FGuid& BufferId, const dream::ChangeSet& Changes)
{
	bool bChanged = false;
	for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
	{
		FDreamRuntimeProblem& Item = Items[Index];
		if (Item.BufferId != BufferId || Item.Line <= 0)
		{
			continue;
		}
		// Ranges are in pre-edit coordinates of one version, so every comparison uses the item's pre-edit line.
		const dream::LineNo Line0 = Item.Line - 1;
		int32 Delta = 0;
		bool bEdited = false;
		for (const dream::ChangeRange& Range : Changes.Ranges)
		{
			const dream::LineNo LastTouched = Range.StartLine + Range.OldLines;
			if (Line0 >= Range.StartLine && Line0 <= LastTouched)
			{
				bEdited = true;
				break;
			}
			if (Line0 > LastTouched)
			{
				Delta += Range.NewLines - Range.OldLines;
			}
		}
		if (bEdited)
		{
			Items.RemoveAt(Index);
			bChanged = true;
		}
		else if (Delta != 0)
		{
			Item.Line += Delta;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		UnbindUnusedBuffers();
		ChangedEvent.Broadcast();
	}
}

void FDreamRuntimeProblems::BindBuffer(const TSharedPtr<FDreamBuffer>& Buffer)
{
	const FGuid Id = Buffer->GetId();
	if (FBound* Existing = BoundBuffers.Find(Id))
	{
		if (Existing->Buffer.IsValid())
		{
			return;
		}
		BoundBuffers.Remove(Id);
	}
	FBound Bound;
	Bound.Buffer = Buffer;
	Bound.Handle = Buffer->OnChanged().AddLambda([this, Id](const dream::ChangeSet& Changes) { OnBufferEdited(Id, Changes); });
	BoundBuffers.Add(Id, MoveTemp(Bound));
}

void FDreamRuntimeProblems::UnbindUnusedBuffers()
{
	for (auto It = BoundBuffers.CreateIterator(); It; ++It)
	{
		const FGuid Id = It->Key;
		const bool bUsed = Items.ContainsByPredicate([&Id](const FDreamRuntimeProblem& Item) { return Item.BufferId == Id; });
		if (bUsed && It->Value.Buffer.IsValid())
		{
			continue;
		}
		if (const TSharedPtr<FDreamBuffer> Buffer = It->Value.Buffer.Pin())
		{
			Buffer->OnChanged().Remove(It->Value.Handle);
		}
		It.RemoveCurrent();
	}
}
