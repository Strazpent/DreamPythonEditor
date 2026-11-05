// SPDX-License-Identifier: Apache-2.0
// Dream core: incremental Python 3.11 lexer for highlighting and line facts (TDD §4.3).
// Game thread only (it holds per-buffer line records); LexFull is a pure oracle usable anywhere.
#pragma once

#include "dream/TextBuffer.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace dream
{
	/** Token kinds (TDD §4.3.1). The theme key of each kind is in brackets there; DreamEditorStyle maps them (§4.12). */
	enum class TokenKind : uint8_t
	{
		Text = 0,          // whitespace and anything unclassified; [Normal]
		Keyword,
		SoftKeyword,       // match, case, _ in a match statement
		KeywordConstant,   // True, False, None
		Builtin,
		SelfParam,         // self, cls
		FunctionName,      // the name after def
		ClassName,         // the name after class
		Decorator,         // '@' and the dotted name after it
		Number,
		String,
		Docstring,
		StringEscape,
		FStringBrace,
		FStringConversion,
		FStringSpec,
		Operator,
		Bracket,
		Comment,
		CellMarker,        // '# %%' header (§4.5)
		RegionMarker,      // '# region' / '# endregion'
		Error,
		Count
	};

	enum class LexGrammar : uint8_t
	{
		Py311,             // the shipped 3.11.8 (default)
		Py312Plus,         // PEP 701 f-strings (1.0.x port, 3.13 canary)
	};

	/**
	 * A token is packed in 32 bits: 24-bit start column (UTF-16 units) and 8-bit kind (§4.3.3).
	 * Tokens of a line are sorted and partition it: token i runs from its start to the next token's start, the last
	 * one to the line end. Past column 20,000 the rest of the line is one Text token (§4.3.1 long lines).
	 */
	inline constexpr int32_t MaxTokenStart = (1 << 24) - 1;
	inline constexpr int32_t LongLineColumns = 20000;

	inline uint32_t PackToken(int32_t StartCol, TokenKind Kind)
	{
		return (static_cast<uint32_t>(StartCol) & 0x00FFFFFFu) | (static_cast<uint32_t>(Kind) << 24);
	}
	inline int32_t TokenStart(uint32_t Packed) { return static_cast<int32_t>(Packed & 0x00FFFFFFu); }
	inline TokenKind TokenKindOf(uint32_t Packed) { return static_cast<TokenKind>(Packed >> 24); }

	/** Line flags (§4.3.3 facts). */
	enum LineFlag : uint16_t
	{
		LF_Blank              = 1 << 0,  // only blanks
		LF_CommentOnly        = 1 << 1,  // only blanks and a comment
		LF_StartsInString     = 1 << 2,  // the line starts inside a triple-quoted string
		LF_StartsContinuation = 1 << 3,  // the previous line ended with a backslash join
		LF_CellMarker         = 1 << 4,
		LF_Region             = 1 << 5,
		LF_EndsWithColon      = 1 << 6,  // last significant token, ignoring a comment, is ':' at bracket depth 0 of the line
		LF_EndsInString       = 1 << 7,  // (slice A choice) the line ends inside an open triple-quoted string
		LF_EndsWithBackslash  = 1 << 8,  // (slice A choice) the line ends with a backslash join
		LF_Decorator          = 1 << 9,  // (slice A choice) first significant token is '@'
	};

	struct LineFacts
	{
		int32_t IndentCols = 0;          // leading blanks, tabs expanded to multiples of 8 as the tokenizer does
		int32_t BracketDelta = 0;        // opening minus closing brackets outside strings and comments
		uint16_t Flags = 0;              // LineFlag bits
		int32_t FirstTokenCol = -1;      // (slice A choice) column of the first significant token, -1 when none
		TokenKind FirstTokenKind = TokenKind::Text;

		bool Has(LineFlag F) const { return (Flags & F) != 0; }
	};

	/** Interned end-of-line lexer state (§4.3.3): equal states share an id; 0 is Normal. */
	using LexStateId = uint32_t;
	inline constexpr LexStateId NormalState = 0;

	/** What a state means, for the REPL pre-check and the statement extent (§6.5, §4.5). */
	struct LexStateInfo
	{
		bool bInString = false;          // inside an open string (only triple-quoted strings span lines)
		bool bTriple = false;
		bool bFString = false;
		bool bBackslashJoin = false;
		int32_t FrameDepth = 0;          // nested string frames (f-string fields), at most 8
	};

	class DREAMCORE_API PyLexer
	{
	public:
		explicit PyLexer(LexGrammar Grammar = LexGrammar::Py311);
		~PyLexer();
		PyLexer(PyLexer&& Other) noexcept;
		PyLexer& operator=(PyLexer&& Other) noexcept;
		PyLexer(const PyLexer&) = delete;
		PyLexer& operator=(const PyLexer&) = delete;

		LexGrammar GetGrammar() const;

		/** Drops all records and marks every line of Snapshot dirty (load, reload, grammar change). */
		void Reset(const TextSnapshot& Snapshot);

		/** Splices the line array for one ChangeSet and marks changed lines dirty (§4.3.3 step 1). */
		void OnChange(const ChangeSet& Changes, const TextSnapshot& After);

		/**
		 * Relexes dirty lines from the frontier, Priority lines first (visible lines, §4.3.3 step 4), and stops before the
		 * next line would pass BudgetMs (the slowest line of the call predicts it; the frontier still advances by at least
		 * one line); BudgetMs <= 0 means no budget. After a clean line whose end state is unchanged it jumps to the next
		 * dirty line. Returns the number of lines lexed.
		 */
		int32_t Relex(const TextSnapshot& Snapshot, double BudgetMs, LineNo PriorityFirst, LineNo PriorityLast);

		/** Relexes, without a budget, until every line up to and including Last is clean (run commands, §4.5 freshness). */
		void RelexThrough(const TextSnapshot& Snapshot, LineNo Last);

		/** First line that may hold stale tokens or facts; LineCount() when everything is clean. */
		LineNo Frontier() const;
		bool IsClean(LineNo Line) const;
		LineNo LineCount() const;

		/** Tokens of a line (possibly stale past the frontier; never invalid memory). Valid until the next mutation. */
		std::span<const uint32_t> Tokens(LineNo Line) const;
		LineFacts Facts(LineNo Line) const;
		LexStateId StartState(LineNo Line) const;
		LexStateId EndState(LineNo Line) const;
		LexStateInfo DescribeState(LexStateId State) const;

		/** Full lex of a snapshot without incremental state: the oracle for T-LEX-007 differential fuzzing. */
		static void LexFull(const TextSnapshot& Snapshot, LexGrammar Grammar, std::vector<std::vector<uint32_t>>& OutTokens);

		/** Full lex of a standalone text, with facts (REPL pre-check, tests). */
		static void LexText(std::u16string_view Text, LexGrammar Grammar, std::vector<std::vector<uint32_t>>& OutTokens,
			std::vector<LineFacts>& OutFacts, LexStateId& OutEndState, LexStateInfo& OutEndInfo);

		/** True for the 35 hard keywords of 3.11 (keyword.kwlist). */
		static bool IsKeyword(std::u16string_view Word);
		/** True for names in dir(builtins) of the shipped 3.11.8 (the generated table). */
		static bool IsBuiltin(std::u16string_view Word);

	private:
		struct Impl;
		std::unique_ptr<Impl> State;     // (slice A choice) pimpl keeps the line array (§4.3.3) private to Private/
	};
}
