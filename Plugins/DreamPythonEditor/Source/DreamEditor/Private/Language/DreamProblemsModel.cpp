// SPDX-License-Identifier: Apache-2.0
// The Problems model (TDD §7.8.1, §7.8.2). See DreamProblemsModel.h.
#include "Language/DreamProblemsModel.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamRuntimeProblems.h"
#include "dream/TextBuffer.h"

namespace DreamProblemsLocal
{
	/** §7.8.2: which duplicate is kept. Lower wins. */
	int32 Priority(EDreamProblemSource Source, bool bSyntax)
	{
		if (bSyntax)
		{
			switch (Source)
			{
			case EDreamProblemSource::Parse: return 0;
			case EDreamProblemSource::Ruff: return 1;
			case EDreamProblemSource::DreamCheck: return 2;
			case EDreamProblemSource::TypeServer: return 3;
			case EDreamProblemSource::Runtime: return 4;
			case EDreamProblemSource::Syntax: return 5;      // (lang) the in-process check yields to every real parser
			}
		}
		switch (Source)
		{
		case EDreamProblemSource::TypeServer: return 0;
		case EDreamProblemSource::Ruff: return 1;
		case EDreamProblemSource::DreamCheck: return 2;
		case EDreamProblemSource::Parse: return 3;
		case EDreamProblemSource::Runtime: return 4;
		case EDreamProblemSource::Syntax: return 5;          // (lang)
		}
		return 6;
	}

	/** Moves Line (0-based, pre-edit) through Changes; false when an edit touched it. */
	bool MapLine(const dream::ChangeSet& Changes, int32& InOutLine)
	{
		int32 Delta = 0;
		for (const dream::ChangeRange& Range : Changes.Ranges)
		{
			const dream::LineNo LastTouched = Range.StartLine + Range.OldLines;
			if (InOutLine >= Range.StartLine && InOutLine <= LastTouched)
			{
				return false;
			}
			if (InOutLine > LastTouched)
			{
				Delta += Range.NewLines - Range.OldLines;
			}
		}
		InOutLine += Delta;
		return true;
	}
}

FDreamProblemsModel::FDreamProblemsModel() = default;

FDreamProblemsModel::~FDreamProblemsModel()
{
	UnbindRuntime();
}

void FDreamProblemsModel::BindRuntime(FDreamRuntimeProblems* InRuntime)
{
	UnbindRuntime();
	Runtime = InRuntime;
	if (Runtime != nullptr)
	{
		RuntimeHandle = Runtime->OnChanged().AddLambda([this]() { Broadcast(); });
	}
}

void FDreamProblemsModel::UnbindRuntime()
{
	if (Runtime != nullptr)
	{
		Runtime->OnChanged().Remove(RuntimeHandle);
	}
	Runtime = nullptr;
	RuntimeHandle.Reset();
}

void FDreamProblemsModel::Broadcast()
{
	++Revision;     // (lang) painters compare it before they re-read
	ChangedEvent.Broadcast();
}

bool FDreamProblemsModel::IsSyntaxHidden(const FGuid& Buffer, EDreamProblemSource Source) const
{
	// (lang) PO-06 F4: one syntax verdict per buffer, the one that read the newer text. dream-check (ast.parse) wins ties
	// and is the reference; the in-process check covers the time dream-check is starting, restarting, paused or behind.
	if (Source != EDreamProblemSource::DreamCheck && Source != EDreamProblemSource::Syntax)
	{
		return false;
	}
	const uint64* Local = AnalysedVersions.Find(FKey{ Buffer, EDreamProblemSource::Syntax });
	if (Local == nullptr)
	{
		return false;
	}
	const uint64* Check = AnalysedVersions.Find(FKey{ Buffer, EDreamProblemSource::DreamCheck });
	bool bCheckWins = Check != nullptr && *Check >= *Local;
	if (bCheckWins)
	{
		if (const TArray<FDreamProblem>* CheckItems = Sets.Find(FKey{ Buffer, EDreamProblemSource::DreamCheck }))
		{
			bCheckWins = !CheckItems->ContainsByPredicate([](const FDreamProblem& Item) { return Item.bStale; });
		}
	}
	return Source == EDreamProblemSource::Syntax ? bCheckWins : !bCheckWins;
}

void FDreamProblemsModel::NoteAnalysed(const FGuid& Buffer, EDreamProblemSource Source, uint64 Version)
{
	const bool bBefore = IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax);
	AnalysedVersions.Add(FKey{ Buffer, Source }, Version);
	if (IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax) != bBefore)
	{
		Broadcast();    // the verdict moved to the other source although neither set may change
	}
}

void FDreamProblemsModel::ReplaceAnalysed(const FGuid& Buffer, EDreamProblemSource Source, uint64 Version, TArray<FDreamProblem>&& Items)
{
	const bool bBefore = IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax);
	AnalysedVersions.Add(FKey{ Buffer, Source }, Version);
	const bool bVerdictMoved = IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax) != bBefore;
	const uint64 RevisionBefore = Revision;
	ReplaceSource(Buffer, Source, MoveTemp(Items));
	if (bVerdictMoved && Revision == RevisionBefore)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::ForgetAnalysed(const FGuid& Buffer, EDreamProblemSource Source)
{
	const bool bBefore = IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax);
	AnalysedVersions.Remove(FKey{ Buffer, Source });
	if (IsSyntaxHidden(Buffer, EDreamProblemSource::Syntax) != bBefore)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::ReplaceSource(const FGuid& Buffer, EDreamProblemSource Source, TArray<FDreamProblem>&& Items)
{
	const FKey Key{ Buffer, Source };
	TArray<FDreamProblem>* Existing = Sets.Find(Key);
	if (Items.IsEmpty() && (Existing == nullptr || Existing->IsEmpty()))
	{
		return;
	}
	for (FDreamProblem& Item : Items)
	{
		Item.Buffer = Buffer;
		Item.Source = Source;
		Item.bStale = false;
	}
	if (Items.IsEmpty())
	{
		Sets.Remove(Key);
	}
	else
	{
		Sets.Add(Key, MoveTemp(Items));
	}
	Broadcast();
}

void FDreamProblemsModel::ClearBuffer(const FGuid& Buffer)
{
	bool bChanged = false;
	for (auto It = Sets.CreateIterator(); It; ++It)
	{
		if (It->Key.Buffer == Buffer)
		{
			It.RemoveCurrent();
			bChanged = true;
		}
	}
	for (auto It = AnalysedVersions.CreateIterator(); It; ++It)     // (lang)
	{
		if (It->Key.Buffer == Buffer)
		{
			It.RemoveCurrent();
		}
	}
	if (bChanged)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::MarkSourceStale(EDreamProblemSource Source, double NowSeconds)
{
	bool bChanged = false;
	for (auto& Pair : Sets)
	{
		if (Pair.Key.Source != Source)
		{
			continue;
		}
		for (FDreamProblem& Item : Pair.Value)
		{
			bChanged |= !Item.bStale;
			Item.bStale = true;
		}
	}
	StaleSince.Add(Source, NowSeconds);
	if (bChanged)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::ExpireStale(double NowSeconds)
{
	bool bChanged = false;
	for (auto StaleIt = StaleSince.CreateIterator(); StaleIt; ++StaleIt)
	{
		if (NowSeconds - StaleIt->Value < StaleSeconds)
		{
			continue;
		}
		const EDreamProblemSource Source = StaleIt->Key;
		StaleIt.RemoveCurrent();
		for (auto It = Sets.CreateIterator(); It; ++It)
		{
			if (It->Key.Source != Source)
			{
				continue;
			}
			const int32 Removed = It->Value.RemoveAll([](const FDreamProblem& Item) { return Item.bStale; });
			bChanged |= Removed > 0;
			if (It->Value.IsEmpty())
			{
				if (Removed > 0)
				{
					AnalysedVersions.Remove(It->Key);   // (lang) review lang-q1-m2: a dropped set is no clean verdict
				}
				It.RemoveCurrent();
			}
		}
	}
	if (bChanged)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::OnBufferEdited(const FGuid& Buffer, const dream::ChangeSet& Changes)
{
	if (Changes.IsEmpty())
	{
		return;
	}
	bool bChanged = false;
	for (auto It = Sets.CreateIterator(); It; ++It)
	{
		if (It->Key.Buffer != Buffer)
		{
			continue;
		}
		const bool bParse = It->Key.Source == EDreamProblemSource::Parse;
		TArray<FDreamProblem>& Items = It->Value;
		for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
		{
			FDreamProblem& Item = Items[Index];
			int32 Start = Item.StartLine;
			if (!DreamProblemsLocal::MapLine(Changes, Start))
			{
				if (bParse)
				{
					Items.RemoveAt(Index);  // an edit of the line clears a Parse item (§7.8.1)
					bChanged = true;
				}
				continue;                   // a lint item stays where it is until the next publish replaces it
			}
			int32 End = Item.EndLine;
			if (!DreamProblemsLocal::MapLine(Changes, End))
			{
				End = Start;
			}
			if (Start != Item.StartLine || End != Item.EndLine)
			{
				Item.EndLine = FMath::Max(Start, End);
				Item.StartLine = Start;
				bChanged = true;
			}
		}
		if (Items.IsEmpty())
		{
			It.RemoveCurrent();
		}
	}
	if (bChanged)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::ClearParse(const FGuid& Buffer)
{
	if (Sets.Remove(FKey{ Buffer, EDreamProblemSource::Parse }) > 0)
	{
		Broadcast();
	}
}

void FDreamProblemsModel::ClearRuntimeItems()
{
	if (Runtime != nullptr)
	{
		Runtime->ClearRuntimeItems();       // broadcasts through the binding
	}
}

TArray<FDreamProblem> FDreamProblemsModel::GetItems(const FGuid& Buffer, EDreamProblemSource Source) const
{
	if (const TArray<FDreamProblem>* Items = Sets.Find(FKey{ Buffer, Source }))
	{
		return *Items;
	}
	return {};
}

TArray<FDreamProblem> FDreamProblemsModel::GetVisible() const
{
	return Dedup(Collect(nullptr, /*bHints*/ false));
}

TArray<FDreamProblem> FDreamProblemsModel::GetForBuffer(const FGuid& Buffer) const
{
	// (lang) PO-06 F4: one buffer, hints included; Dedup's order is by line and column within a buffer.
	return Buffer.IsValid() ? Dedup(Collect(&Buffer, /*bHints*/ true)) : TArray<FDreamProblem>();
}

TArray<FDreamProblem> FDreamProblemsModel::Collect(const FGuid* OnlyBuffer, bool bHints) const
{
	TArray<FDreamProblem> All;
	for (const auto& Pair : Sets)
	{
		if (OnlyBuffer != nullptr && Pair.Key.Buffer != *OnlyBuffer)
		{
			continue;
		}
		const bool bHideSyntax = IsSyntaxHidden(Pair.Key.Buffer, Pair.Key.Source);     // (lang) PO-06 F4
		for (const FDreamProblem& Item : Pair.Value)
		{
			if ((bHints || Item.Severity != EDreamSeverity::Hint) && !(bHideSyntax && Item.DedupClass == TEXT("syntax")))
			{
				All.Add(Item);
			}
		}
	}
	if (Runtime != nullptr)
	{
		for (const FDreamRuntimeProblem& R : Runtime->GetItems())
		{
			if (OnlyBuffer != nullptr && R.BufferId != *OnlyBuffer)
			{
				continue;
			}
			FDreamProblem Item;
			Item.Buffer = R.BufferId;
			Item.Source = EDreamProblemSource::Runtime;
			Item.Severity = EDreamSeverity::Error;
			FString Type;
			Item.Code = R.Message.Split(TEXT(": "), &Type, nullptr) ? Type : R.Message;
			Item.Message = R.Message;
			Item.DedupClass = FString::Printf(TEXT("runtime:%d:%s"), R.RunNumber, *Item.Code);
			Item.StartLine = Item.EndLine = FMath::Max(R.Line - 1, 0);
			Item.RunNumber = static_cast<uint32>(R.RunNumber);
			Item.RunSourceKey = R.SourceKey;
			Item.ModulePath = R.File;
			Item.File = R.File;
			All.Add(MoveTemp(Item));
		}
	}
	return All;
}

TArray<FDreamProblem> FDreamProblemsModel::Dedup(TArray<FDreamProblem>&& All)
{
	// Dedup: one item per (Buffer, StartLine, DedupClass); the kept one names the merged ones (§7.8.2).
	TMap<FString, int32> Winners;
	TArray<FDreamProblem> Out;
	for (FDreamProblem& Item : All)
	{
		const FString Where = Item.Buffer.IsValid() ? Item.Buffer.ToString() : Item.File;
		const FString Key = FString::Printf(TEXT("%s|%d|%s"), *Where, Item.StartLine, *Item.DedupClass);
		const bool bSyntax = Item.DedupClass == TEXT("syntax");
		if (int32* Found = Winners.Find(Key))
		{
			FDreamProblem& Kept = Out[*Found];
			if (DreamProblemsLocal::Priority(Item.Source, bSyntax) < DreamProblemsLocal::Priority(Kept.Source, bSyntax))
			{
				TArray<FString> Merged = MoveTemp(Kept.MergedFrom);
				Merged.AddUnique(SourceName(Kept.Source));
				Kept = MoveTemp(Item);
				Kept.MergedFrom = MoveTemp(Merged);
			}
			else
			{
				Kept.MergedFrom.AddUnique(SourceName(Item.Source));
			}
			continue;
		}
		Winners.Add(Key, Out.Num());
		Out.Add(MoveTemp(Item));
	}

	// Order: open buffers in tab order, then items of files that are not open.
	TMap<FGuid, int32> Order;
	if (FDreamEditorModule::IsAvailable())
	{
		int32 Index = 0;
		for (const TSharedRef<FDreamBuffer>& Buffer : FDreamEditorModule::Get().GetBufferManager().GetOpenBuffers())
		{
			Order.Add(Buffer->GetId(), Index++);
		}
	}
	Out.Sort([&Order](const FDreamProblem& A, const FDreamProblem& B)
	{
		const int32* OrderA = A.Buffer.IsValid() ? Order.Find(A.Buffer) : nullptr;
		const int32* OrderB = B.Buffer.IsValid() ? Order.Find(B.Buffer) : nullptr;
		const int32 RankA = OrderA != nullptr ? *OrderA : MAX_int32;
		const int32 RankB = OrderB != nullptr ? *OrderB : MAX_int32;
		if (RankA != RankB)
		{
			return RankA < RankB;
		}
		if (RankA == MAX_int32 && A.File != B.File)
		{
			return A.File < B.File;
		}
		if (A.Buffer != B.Buffer)
		{
			return A.Buffer.ToString() < B.Buffer.ToString();
		}
		if (A.StartLine != B.StartLine)
		{
			return A.StartLine < B.StartLine;
		}
		if (A.StartColumn != B.StartColumn)
		{
			return A.StartColumn < B.StartColumn;
		}
		return static_cast<uint8>(A.Severity) < static_cast<uint8>(B.Severity);
	});
	return Out;
}

int32 FDreamProblemsModel::FindNeighbour(const TArray<FDreamProblem>& Visible, const FGuid& Buffer, int32 Line, int32 Column, bool bForward,
	int32 CurrentIndex)
{
	if (Visible.IsEmpty())
	{
		return INDEX_NONE;
	}
	if (Visible.IsValidIndex(CurrentIndex) && Buffer.IsValid() && Visible[CurrentIndex].Buffer == Buffer &&
		Visible[CurrentIndex].StartLine == Line && Visible[CurrentIndex].StartColumn == Column)
	{
		return (CurrentIndex + (bForward ? 1 : Visible.Num() - 1)) % Visible.Num();
	}
	auto IsAfter = [&](const FDreamProblem& Item)
	{
		return Item.StartLine > Line || (Item.StartLine == Line && Item.StartColumn > Column);
	};
	auto IsBefore = [&](const FDreamProblem& Item)
	{
		return Item.StartLine < Line || (Item.StartLine == Line && Item.StartColumn < Column);
	};
	int32 First = INDEX_NONE;
	int32 Last = INDEX_NONE;
	for (int32 Index = 0; Index < Visible.Num(); ++Index)
	{
		if (Visible[Index].Buffer == Buffer && Buffer.IsValid())
		{
			First = First == INDEX_NONE ? Index : First;
			Last = Index;
		}
	}
	if (First == INDEX_NONE)
	{
		// The caret's buffer has no items: start at the list's ends.
		return bForward ? 0 : Visible.Num() - 1;
	}
	if (bForward)
	{
		for (int32 Index = First; Index <= Last; ++Index)
		{
			if (IsAfter(Visible[Index]))
			{
				return Index;
			}
		}
		return (Last + 1) % Visible.Num();
	}
	for (int32 Index = Last; Index >= First; --Index)
	{
		if (IsBefore(Visible[Index]))
		{
			return Index;
		}
	}
	return (First - 1 + Visible.Num()) % Visible.Num();
}

const TCHAR* FDreamProblemsModel::SourceName(EDreamProblemSource Source)
{
	switch (Source)
	{
	case EDreamProblemSource::Syntax: return TEXT("syntax check");     // (lang)
	case EDreamProblemSource::TypeServer: return TEXT("types");
	case EDreamProblemSource::Ruff: return TEXT("ruff");
	case EDreamProblemSource::DreamCheck: return TEXT("dream-check");
	case EDreamProblemSource::Parse: return TEXT("parse");
	case EDreamProblemSource::Runtime: return TEXT("runtime");
	}
	return TEXT("");
}

EDreamSeverity FDreamProblemsModel::SeverityFromLsp(int32 LspSeverity)
{
	switch (LspSeverity)
	{
	case 1: return EDreamSeverity::Error;
	case 2: return EDreamSeverity::Warning;
	case 3: return EDreamSeverity::Info;
	default: return EDreamSeverity::Hint;
	}
}
