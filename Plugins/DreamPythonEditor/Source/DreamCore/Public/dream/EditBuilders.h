// SPDX-License-Identifier: Apache-2.0
// Dream core: the plain-mode edit builders (TDD §4.5 edit builders table; EDT-03, EDT-04, EDT-05 M1 rows).
// (slice A choice) Free functions over a line range or one selection instead of dream::CaretSet methods (CaretSet is
// G1 work). Each builder reads PyLexer facts and tokens (it relexes through the lines it reads first) and returns one
// batch, so the caller records it as one undo group (§4.2 R3, R5). Game thread.
#pragma once

#include "dream/PyLexer.h"

namespace dream
{
	/** How one indentation level is written (EDT-03 per-language tabs/spaces; §4.5 indent detection). */
	struct IndentStyle
	{
		int32_t Width = 4;           // columns per level
		bool bUseTabs = false;       // write a level as tabs (and spaces for a remainder) instead of spaces
		int32_t TabSize = 4;         // columns of a '\t' in the editor's indentation math

		bool operator==(const IndentStyle&) const = default;
	};

	/**
	 * A built edit and where the selection goes after it, in post-edit offsets. An empty edit (no ranges and not
	 * bMoveOnly) means "nothing special": the caller does its default action (insert the key's character, delete one
	 * unit, ...). bMoveOnly: no text change, only the caret moves (type-over).
	 */
	struct BuiltEdit
	{
		EditBatch Batch;
		Offset Anchor = 0;
		Offset Caret = 0;
		bool bMoveOnly = false;

		bool IsEmpty() const { return Batch.Ranges.empty() && !bMoveOnly; }
	};

	/**
	 * Toggle comment (Ctrl+/, EDT-04): adds "# " at the minimum indentation of the non-blank lines of Lines, or removes
	 * one "# " (or "#") from each when all non-blank lines are already commented. Lines starting inside a string are
	 * left alone. Returns one batch (origin Command); empty when there is nothing to do.
	 */
	DREAMCORE_API EditBatch ToggleComment(const TextSnapshot& Snapshot, PyLexer& Lexer, LineRange Lines);

	/**
	 * Enter with smart indent (EDT-03): replaces the selection [SelStart, SelEnd) with '\n' plus the new line's
	 * indentation, one batch (origin Typing; §4.2 R3: the newline and its indent are one undo group). The indentation
	 * is the caret line's, plus one level after a ':' that ends the text before the caret outside strings and comments,
	 * minus one level after a line whose statement starts with return, pass, break, continue or raise. Inside an open
	 * bracket it is a hanging indent of one level, or aligned after the bracket when text follows it on that line;
	 * after a line that closes brackets opened above it (or a backslash continuation) the reference is the line that
	 * opened them. Inside a string or comment the caret line's indentation is kept. Blanks right before the caret and
	 * right after the selection are dropped (no trailing whitespace); Enter with the caret in a line's leading blanks
	 * moves the blanks before the caret down as the new indentation and keeps the blanks after it, so the code that
	 * moves down keeps its whole indentation (a blank-only rest of the line is dropped). Never empty. The caret lands
	 * after the indentation.
	 */
	DREAMCORE_API BuiltEdit SmartNewline(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset SelStart, Offset SelEnd,
		const IndentStyle& Style);

	/**
	 * Backspace with no selection (EDT-03, EDT-05): in leading blanks, deletes spaces back to the previous indent stop
	 * (a multiple of Style.Width columns; a tab or a non-space stops it); between an empty pair "()", "[]", "{}", "''"
	 * or '""' outside strings and comments, deletes both. Otherwise empty (the default one-unit delete applies).
	 */
	DREAMCORE_API BuiltEdit SmartBackspace(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset Caret, const IndentStyle& Style);

	/**
	 * Auto-close pairing (EDT-05 M1): typing '(', '[', '{', '\'' or '"' with an empty selection outside strings and
	 * comments inserts the pair and puts the caret between, when the next character is the line end, a blank or one
	 * of ")]}:,;.=>". A quote also needs no identifier character before it, unless that word is a string prefix
	 * (r, u, f, b, rb, br, fr, rf in any case); the third quote of an empty "''" or '""' makes a triple-quoted pair.
	 * Empty when the typed character is not paired (the caller inserts it). With a selection it is empty: wrapping the
	 * selection is EDT-05's M2 row. Origin Typing; one undo group (§4.2 R5).
	 */
	DREAMCORE_API BuiltEdit AutoClosePair(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset SelStart, Offset SelEnd,
		char16_t Typed);

	/**
	 * Type-over (EDT-05's M2 row; a primitive for the caller): when the character at Caret is the typed closer (')',
	 * ']', '}', '\'' or '"'), a move-only edit that steps over it. The caller uses it only for a closer that
	 * AutoClosePair inserted and that the caret has not left, as editors do; otherwise it inserts as usual.
	 */
	DREAMCORE_API BuiltEdit TypeOverClosing(const TextSnapshot& Snapshot, Offset Caret, char16_t Typed);

	/**
	 * Duplicate line or selection (Ctrl+D, EDT-04): with an empty selection, the caret line is copied below it and the
	 * caret moves to the same column of the copy; with a selection, the selected text is inserted again right after it
	 * and the copy is selected. One batch, origin Command.
	 */
	DREAMCORE_API BuiltEdit DuplicateLineOrSelection(const TextSnapshot& Snapshot, Offset SelStart, Offset SelEnd);

	/**
	 * Indent detection (§4.5 table): the dominant unit of the indented code lines (tabs against spaces, then the most
	 * common indentation step of 2, 3, 4 or 8 columns); Fallback when the text has too few indented lines or a tie.
	 */
	DREAMCORE_API IndentStyle DetectIndent(const TextSnapshot& Snapshot, PyLexer& Lexer, const IndentStyle& Fallback);

	/** The text of one indentation of Columns columns in Style (tabs first when Style.bUseTabs). */
	DREAMCORE_API std::u16string MakeIndent(int32_t Columns, const IndentStyle& Style);
}
