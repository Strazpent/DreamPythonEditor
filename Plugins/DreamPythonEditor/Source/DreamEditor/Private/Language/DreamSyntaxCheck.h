// SPDX-License-Identifier: Apache-2.0
// The in-process syntax check (owner ruling PO-06 F4; TDD §7.3.6 "In process: parse check"; INT-07): a server-free pass over
// dream::PyLexer's tokens and line facts that finds the syntax errors a lexer can see, so they squiggle even while dream-check
// starts, restarts or is paused:
// - error tokens: unterminated strings, a bad escape, an invalid number or character, a character after a line
//   continuation, a single '}' in an f-string;
// - brackets: a closer with no opener, a closer of the wrong kind, an opener never closed;
// - a missing ':' after if / elif / else / for / while / def / class / try / except / finally / with (and async forms);
// - indentation: an unexpected indent, an unindent that matches no outer level, no indented block after a ':' line;
// - an unterminated triple-quoted string at the end of the text.
// The messages follow CPython 3.11's. Items are FDreamProblem with Source Syntax, Severity Error, Code "syntax-error" and
// dedup class "syntax"; ast.parse in dream-check stays the reference (FDreamProblemsModel::NoteAnalysed picks the verdict).
// Pure: no I/O, no Python, no shared state; any thread (FDreamLanguageServices runs it on a UE::Tasks worker). Body: DreamSyntaxCheck.cpp (lang).
#pragma once

#include "CoreMinimal.h"
#include "Language/DreamProblemsModel.h"

namespace dream { class TextSnapshot; }

class DREAMEDITOR_API FDreamSyntaxCheck
{
public:
	/** Texts longer than this (UTF-16 units) are not checked (dream-check still is, up to its 5 MB rule). */
	static constexpr int64 MaxUnits = 2 * 1024 * 1024;
	/** At most this many items per check (a broken file gives one cause and a few consequences, not hundreds). */
	static constexpr int32 MaxItems = 50;
	/** The items' code, as dream-check's syntax errors. */
	static constexpr const TCHAR* Code = TEXT("syntax-error");

	/**
	 * The syntax errors of Snapshot, by position; DocVersion is the snapshot's version. False when the text was not checked
	 * (over MaxUnits): then there is no verdict. Pure, so it runs on a worker (ruling R-q1-5); snapshots are immutable.
	 */
	static bool Check(const dream::TextSnapshot& Snapshot, TArray<FDreamProblem>& Out);
	/** The same over a plain text (tests). */
	static TArray<FDreamProblem> CheckText(const FString& Text);
};
