// SPDX-License-Identifier: Apache-2.0
// Dream core: what Run current line (Ctrl+Alt+Enter, RUN-02) and Run line and advance (Alt+Enter, RUN-26) submit,
// and where the caret goes next (TDD §4.5 "Statement extent", PO-02); plus the REPL lexer pre-check (§6.5).
// Reads PyLexer line facts only; never calls Python. Game thread (it relexes the buffer's lexer).
#pragma once

#include "dream/PyLexer.h"

#include <optional>
#include <string_view>

namespace dream
{
	/**
	 * The statement extent of the caret line, per TDD §4.5 rules 1-6:
	 *  1. a blank or comment-only line that is not a continuation has none (nullopt);
	 *  2. S = the statement start of the logical line holding Caret (walk up over continuation lines: StartsInString,
	 *     StartsContinuation, or a positive running bracket depth); decorated definitions move S up over the decorator
	 *     logical lines directly above at the same indentation (blank and comment lines between them allowed);
	 *  3. a clause first token (elif/else/except/finally) moves S up to the previous statement start at the same
	 *     indentation whose first token is none of these; 'case' moves S up to the enclosing 'match';
	 *  4. while the logical line starts with '@' the extent runs on to the decorated def/class;
	 *  5. a logical line that opens a compound statement (first token if/while/for/try/with/def/class/async, or
	 *     EndsWithColon on its last physical line) takes its body (every line up to the next statement start with
	 *     indentation <= S's) and then each following clause at S's indentation with its body;
	 *  6. otherwise the single logical line.
	 * Trailing blank and comment-only lines are never part of an extent.
	 * Freshness: Lexer is relexed first (no budget). (slice A choice) It relexes through the end of the buffer rather
	 * than only the scanned lines, which is simpler and cheap for plain-mode buffers under 1 MB.
	 */
	DREAMCORE_API std::optional<LineRange> StatementExtent(const TextSnapshot& Snapshot, PyLexer& Lexer, LineNo Caret);

	/**
	 * The advance target after running Ran (TDD §4.5 "Next statement"): the first statement start after Ran.Last;
	 * while that target is a clause line of a compound statement H whose extent contains all of Ran, the target moves to
	 * the first statement start after H's extent. nullopt at the end of the buffer ("End of buffer").
	 */
	DREAMCORE_API std::optional<LineNo> NextStatementLine(const TextSnapshot& Snapshot, PyLexer& Lexer, LineRange Ran);

	/** First statement start at or after Line, for Run line and advance on a blank line (§4.5 rule 1); nullopt at the end. */
	DREAMCORE_API std::optional<LineNo> NextStatementStartFrom(const TextSnapshot& Snapshot, PyLexer& Lexer, LineNo Line);

	/** Column of the first non-blank unit of Line (the caret target, §4.5); the line length when the line is blank. */
	DREAMCORE_API int32_t FirstNonBlankColumn(const TextSnapshot& Snapshot, LineNo Line);

	/**
	 * The lines of a selection [SelStart, SelEnd): a selection that ends at column 0 of a line ends on the line before
	 * (§4.5 "With a selection", the Shift+Down case). An empty selection gives the caret line.
	 */
	DREAMCORE_API LineRange SelectionLines(const TextSnapshot& Snapshot, Offset SelStart, Offset SelEnd);

	/** REPL Enter pre-check result (§6.5 "Lexer pre-check"). It never says "complete"; only classify_input does. */
	enum class ReplPrecheck : uint8_t
	{
		Unknown,                 // ask classify_input at the next idle tick
		DefinitelyIncomplete,    // insert a newline and indent
	};

	/**
	 * "Definitely incomplete" only for: an open bracket; an open triple-quoted string; a trailing backslash; a decorator
	 * line; or a last line that starts with if/for/while/try/with/def/class/async/match and whose last token, ignoring a
	 * trailing comment, is ':'. Continuation clauses are left to classify_input (§6.5). Pure; any thread.
	 */
	DREAMCORE_API ReplPrecheck ReplPreCheck(std::u16string_view Source, LexGrammar Grammar = LexGrammar::Py311);

	/**
	 * classify_input's second argument: the first physical line of the last logical line has IndentCols > 0
	 * (§6.5, T-REPL-001). Pure; any thread.
	 */
	DREAMCORE_API bool LastLogicalLineIndented(std::u16string_view Source, LexGrammar Grammar = LexGrammar::Py311);
}
