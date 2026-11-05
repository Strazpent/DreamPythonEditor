// SPDX-License-Identifier: Apache-2.0
// Source text for the Run window's Location row and the supervisor's swallow and cleanup reports (TDD §5.4.7, §5.7.4,
// PO-03), read in C++ from the run's source snapshot, never through Python. Lines are 1-based buffer lines; snapshot
// line 0 is buffer line 1 (FDreamRunService numbers REPL and console snapshots from 1 too). Game thread or any thread
// (a TextSnapshot is immutable). Supervisor slice internal. Body: DreamRunSourceText.cpp.
#pragma once

#include "CoreMinimal.h"
#include "dream/TextBuffer.h"

namespace DreamRunSource
{
	/** The block header that encloses a line, for the §5.7.4 reports. */
	enum class EHandlerKind : uint8
	{
		None,
		Except,     // an except clause: the script caught Stop ("The script caught Stop at line 57")
		Finally,    // cleanup: "cleanup is running (finally at line 88)"
		With,       // cleanup in __exit__
	};

	/** False for no snapshot (the default, version 0). FDreamRunService gives every run with text a snapshot, a file run
	 *  included (the §5.7.4 reports read it); the Location row hides a file run's source by its route (SDreamRunWindow). */
	DREAMEDITOR_API bool HasSource(const dream::TextSnapshot& Snapshot);

	/** The text of 1-based Line, or empty when out of range. */
	DREAMEDITOR_API FString GetLine(const dream::TextSnapshot& Snapshot, int32 Line);

	/** Leading whitespace stripped and cut at 80 characters with "…" (§5.4.7, proposed). */
	DREAMEDITOR_API FString ForLocation(const FString& LineText);

	/** The kind of a block header line ("except ...:", "finally:", "with ...:"), None for anything else. */
	DREAMEDITOR_API EHandlerKind ClassifyHeader(const FString& LineText);

	/**
	 * Where a handled ScriptCancelled is being handled, from the line executing at the check: Line itself when it is a
	 * finally / with / except header (an __exit__ call reports the with line), else the nearest enclosing header of those
	 * kinds found by walking outward through lower indentation. Returns the header's 1-based line, or 0 with None.
	 */
	DREAMEDITOR_API int32 FindEnclosingHandler(const dream::TextSnapshot& Snapshot, int32 Line, EHandlerKind& OutKind);

	/**
	 * The except clause that caught an exception raised at Line: the nearest enclosing "try:" (walking outward), then the
	 * first "except" header after Line at that try's indentation. Returns its 1-based line, or 0.
	 */
	DREAMEDITOR_API int32 FindCatchingExcept(const dream::TextSnapshot& Snapshot, int32 Line);

	/** (run, PO-06 F2) The exact characters a selection run executes, cut from the buffer snapshot. */
	struct FSelectionText
	{
		FString Text;                       // what the runner compiles (LF line breaks)
		int32 FirstLine = 1;                // 1-based buffer line of Text's first line
		int32 LastLine = 1;                 // 1-based buffer line of Text's last line
		TOptional<int32> StartColumn;       // UTF-16 column on FirstLine where Text starts; unset = column 0
		TOptional<int32> EndColumn;         // UTF-16 column on LastLine where Text ends; unset = the line end
	};

	/**
	 * (run, PO-06 F2) The selection [SelStart, SelEnd) as it runs: exactly the selected characters, not the whole lines it
	 * touches. A start preceded on its line by blanks only (the first non-blank character of an indented block, or a
	 * column inside the indentation) begins at the line start, so the block keeps its indentation and the runner's RT-14
	 * wrapper removes the common indentation at compile time (columns stay buffer columns). A start after code on its line
	 * is a fragment and begins there (StartColumn). A start at the end of a line with more lines selected begins at the
	 * next line. The end follows dream::SelectionLines (an end at column 0 ends on the line before). Pure; any thread.
	 */
	DREAMEDITOR_API FSelectionText ExactSelection(const dream::TextSnapshot& Snapshot, int64 SelStart, int64 SelEnd);

	/**
	 * (run, PO-06 F2) The text of 1-based buffer Line as far as a run executed it: cut at EndColumn when Line is LastLine
	 * and at StartColumn when Line is FirstLine (the echo and Re-run read it, §6.2).
	 */
	DREAMEDITOR_API FStringView CutRunLine(FStringView LineText, int32 Line, int32 FirstLine, int32 LastLine,
		const TOptional<int32>& StartColumn, const TOptional<int32>& EndColumn);
}
