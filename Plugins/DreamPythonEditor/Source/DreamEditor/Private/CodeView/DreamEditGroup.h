// SPDX-License-Identifier: Apache-2.0
// Text coordinates of the code view and the edit-group types of the grouped-edit API (TDD §4.2 R5, EDT-08): several
// replacements applied as ONE undo step with a label. FDreamUndoHistory (CodeView/DreamUndoHistory.h) and SDreamCodeView apply
// them. Plain structs, game thread. Owner: codeview.
#pragma once

#include "CoreMinimal.h"
#include "dream/TextBuffer.h"

/** A selection in model coordinates: 0-based lines, UTF-16 columns (the stock FTextLocation uses the same units). */
struct FDreamTextSelection
{
	int32 AnchorLine = 0;
	int32 AnchorColumn = 0;
	int32 CaretLine = 0;
	int32 CaretColumn = 0;

	bool IsEmpty() const { return AnchorLine == CaretLine && AnchorColumn == CaretColumn; }
	int32 FirstLine() const { return FMath::Min(AnchorLine, CaretLine); }
	int32 LastLine() const { return FMath::Max(AnchorLine, CaretLine); }
	bool operator==(const FDreamTextSelection& Other) const
	{
		return AnchorLine == Other.AnchorLine && AnchorColumn == Other.AnchorColumn && CaretLine == Other.CaretLine
			&& CaretColumn == Other.CaretColumn;
	}

	static FDreamTextSelection Caret(int32 Line, int32 Column)
	{
		FDreamTextSelection Out;
		Out.AnchorLine = Out.CaretLine = Line;
		Out.AnchorColumn = Out.CaretColumn = Column;
		return Out;
	}
};

/**
 * One replacement of an edit group: [Start, End) in 0-based lines and UTF-16 columns of the text BEFORE the group (the
 * units of FDreamTextSelection), replaced by Text. Lines in Text are joined with '\n'; a '\r' is dropped.
 */
struct FDreamTextEdit
{
	int32 StartLine = 0;
	int32 StartColumn = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	FString Text;
};

/**
 * Several replacements applied as ONE undo step with a label (EDT-08, §4.2 R5): reload from disk, Restore and restore from
 * history (U30), the SUM102 quick fix, a completion accept, Replace All, the line commands. One Ctrl+Z undoes all of it
 * and brings back the selection from before the group.
 */
struct FDreamEditGroup
{
	/** The undo step's name: "Reloaded from disk", "Restored from history", "Quick fix: …", "Completion". */
	FText Label;
	/** In the coordinates of the text before the group; they must not overlap; any order. */
	TArray<FDreamTextEdit> Edits;
	/** The selection after the group, in the text after it. Unset: a caret at the end of the last edit (by position). */
	TOptional<FDreamTextSelection> SelectionAfter;
	/** Reload for an external reload (the journal and the dirty rules read it); Command otherwise. */
	dream::EditOrigin Origin = dream::EditOrigin::Command;
	/** Applies to a read-only buffer too (a reload of a file that is read-only on disk, a restore). Never while a run is active. */
	bool bEvenIfReadOnly = false;
};
