// SPDX-License-Identifier: Apache-2.0
// Dream core: the outline of one Python text (NAV-02, TDD §7.9.2 "without a server"; owner ruling PO-05 §3): classes,
// functions, methods, async defs and `# %%` cells, nested by indentation, decorators folded into their def. One
// standalone pass over the text (its own small scanner for strings, comments, brackets and backslash joins), so it needs
// no PyLexer state and runs on any thread over an immutable TextSnapshot (the Structure view builds it on a worker).
// Pure functions; standard library only.
//
// The rules:
//  - An entry starts at a logical line whose first token is `class`, `def` or `async def` followed by a name; a line
//    inside a string or a comment, or a continuation line, never makes one. A missing name (while typing) makes none.
//  - Nesting: the entries whose indentation is >= a new logical line's are closed by it (tabs to the next multiple of 8,
//    as the tokenizer counts them). A def whose nearest open entry is a class is a Method (AsyncMethod); any other def is
//    a Function (AsyncFunction), nested under an enclosing def when there is one.
//  - Decorators: the '@' logical lines directly above a def or class at its indentation (blank and comment lines between
//    them allowed, but not a cell header, which ends the run) fold into it: FirstLine is the first decorator's line.
//  - Cells: a comment-only line at a logical-line start whose text after '#' and blanks begins with "%%" (PyLexer's
//    CellMarker rule; indented headers included) is a Cell named by the rest of the line, trimmed. A cell is always at
//    depth 0 and is the parent of the top-level entries after it, until the next cell. A cell header is a comment, so it
//    closes no class or def.
//  - EndLine: a def or class ends at the last line of its body: the last line of the last logical line inside it, or a
//    later comment-only line indented deeper than the def. A cell ends at its last non-blank line before the next cell.
//  - Broken code: a line whose first word can never continue an expression (def, class, async def, import, return,
//    pass, raise, with, while, try, except, finally, del, global, nonlocal, assert, break, continue, elif) starts a
//    logical line even inside unclosed brackets or after a backslash join; a triple-quoted string still open at the end
//    of the text is taken as closed at the end of its first line and the scan resumes on the next line, at most
//    MaxStringRecoveries times (escaped quotes can leave every line opening a string that never closes, and each
//    recovery rescans the tail); unmatched closing brackets are ignored; any dedent closes the entries indented at least
//    as deep.
#pragma once

#include "dream/TextBuffer.h"
#include "dream/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	enum class OutlineKind : uint8_t
	{
		Class,
		Function,
		Method,
		AsyncFunction,
		AsyncMethod,
		Cell,
	};

	/** How many times one build takes an unclosed triple-quoted string as closed and rescans the rest (see above). */
	inline constexpr int32_t MaxStringRecoveries = 4;

	/**
	 * One entry. Entries come in source order (Line strictly increasing), each parent before its children. That is not
	 * always a pre-order walk of the tree: a cell header inside a class body comes between the class's methods. An entry's
	 * FirstLine is after the previous entry's Line.
	 */
	struct OutlineEntry
	{
		OutlineKind Kind = OutlineKind::Function;
		/** The name after def or class; a cell's title (empty for a bare "# %%"). */
		std::u16string Name;
		/** 0-based line of the def or class keyword, or of the cell header. */
		LineNo Line = 0;
		/** UTF-16 column of the name (the caret target); a cell's '#'. */
		int32_t Column = 0;
		/** 0 at top level; a cell's top-level entries are at 1. */
		int32_t Depth = 0;
		/** Last line of the entry (inclusive, >= Line). */
		LineNo EndLine = 0;
		/** First line of the entry: the first decorator's line, else Line. */
		LineNo FirstLine = 0;
		/** Index of the parent entry, -1 at top level. */
		int32_t Parent = -1;
		/** The indentation columns of the def, class or cell line (tabs to multiples of 8). */
		int32_t IndentCols = 0;

		bool IsCell() const { return Kind == OutlineKind::Cell; }
		bool operator==(const OutlineEntry&) const = default;
	};

	/** The outline of a model text (LF-only lines). Any thread. */
	DREAMCORE_API std::vector<OutlineEntry> BuildOutline(const TextSnapshot& Snapshot);

	/** The outline of a standalone text; lines may end in "\n" or "\r\n" (a CR before the LF is ignored). Any thread. */
	DREAMCORE_API std::vector<OutlineEntry> BuildOutline(std::u16string_view Text);

	/**
	 * The entry that contains Line (FirstLine <= Line <= EndLine), the deepest one; among equally deep ones the one that
	 * starts last. -1 when none does.
	 */
	DREAMCORE_API int32_t FindOutlineEntryAt(const std::vector<OutlineEntry>& Entries, LineNo Line);

	enum class OutlineMatch : uint8_t
	{
		Hidden,
		Ancestor,          // shown because an entry under it matches
		Match,
	};

	/**
	 * The Structure filter (PO-05 §3): an entry matches when Query occurs in its name, ignoring case (Python's
	 * re.IGNORECASE folding); every ancestor of a match is shown too. A cell with no title is matched by UntitledCellName,
	 * the label the view shows for it. An empty query matches every entry. One result per entry, in order.
	 */
	DREAMCORE_API std::vector<OutlineMatch> FilterOutline(const std::vector<OutlineEntry>& Entries, std::u16string_view Query,
		std::u16string_view UntitledCellName = std::u16string_view());
}
