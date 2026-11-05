// SPDX-License-Identifier: Apache-2.0
// Dream core, private: the regex engine behind dream::SearchEngine (TDD §4.4). A Python `re` parser (str patterns,
// 3.11 syntax) compiling to a small instruction program, and a backtracking matcher with an explicit stack, a capture
// undo log, a step limit and a stack limit. Pure functions over a UTF-16 text; any thread.
#pragma once

#include "dream/SearchEngine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::regex_detail
{
	/** Character tables generated from the engine's CPython (Private/SearchTables.inc). */
	bool IsWordCp(char32_t C);
	bool IsDigitCp(char32_t C);
	bool IsSpaceCp(char32_t C);
	/** IGNORECASE folding: two code points match when their folds are equal (Python's tolower plus _EXTRA_CASES). */
	char32_t FoldCp(char32_t C);
	/** Every code point with the same fold as C (C itself included); at least one. */
	void FoldMembers(char32_t C, std::vector<char32_t>& Out);
	char32_t UpperCp(char32_t C);
	char32_t LowerCp(char32_t C);
	/** _sre.unicode_tolower: what an IGNORECASE backreference compares (no _EXTRA_CASES, unlike FoldCp). */
	char32_t SreLowerCp(char32_t C);
	/** _sre.ascii_tolower: IGNORECASE folding under the ASCII flag. */
	inline char32_t AsciiLowerCp(char32_t C) { return (C >= U'A' && C <= U'Z') ? C + 32 : C; }

	/** Decodes the code point at I (a lone surrogate decodes as itself); Units gets 1 or 2. */
	char32_t DecodeAt(std::u16string_view T, size_t I, size_t& Units);
	/** The code point that ends at I (I > 0). */
	char32_t DecodeBefore(std::u16string_view T, size_t I, size_t& Units);

	enum CategoryBit : uint8_t
	{
		Cat_Word = 1 << 0,
		Cat_NotWord = 1 << 1,
		Cat_Digit = 1 << 2,
		Cat_NotDigit = 1 << 3,
		Cat_Space = 1 << 4,
		Cat_NotSpace = 1 << 5,
	};

	struct CharClass
	{
		std::vector<std::pair<char32_t, char32_t>> Ranges;
		uint8_t Categories = 0;
		bool bNegate = false;
		bool bIgnoreCase = false;
		bool bAscii = false;

		bool Matches(char32_t C) const;
	};

	enum class Op : uint8_t
	{
		Char,          // A = code point (folded when Flag_Set: ignore case; Flag_Ascii: ASCII-only folding, (?a))
		Any,           // bFlag = DOTALL
		Class,         // A = class index
		Split,         // try A first, then B
		Jmp,           // A
		Save,          // A = slot
		LineStart,     // ^ (MULTILINE)
		LineEnd,       // $ (MULTILINE)
		TextStart,     // \A
		TextEnd,       // \Z
		WordBoundary,  // \b (bFlag = ASCII)
		NotWordBoundary,
		Backref,       // A = group, Flag_Set = ignore case (lower() per code point, as _sre), Flag_Ascii = ASCII lower
		Look,          // A = sub-program, B = width (look-behind), Behind/Negate in Flags
		Atomic,        // A = sub-program
		MarkSet,       // A = slot: remember the position at a loop iteration's start
		MarkCheck,     // A = slot, B = exit: an iteration that consumed nothing ends the loop
		Match,
	};

	inline constexpr uint8_t Flag_Set = 1;
	inline constexpr uint8_t Flag_Behind = 2;
	inline constexpr uint8_t Flag_Negate = 4;
	inline constexpr uint8_t Flag_Ascii = 8;

	struct Inst
	{
		Op Code = Op::Match;
		uint8_t Flags = 0;
		int32_t A = 0;
		int32_t B = 0;
	};

	struct Program
	{
		std::vector<Inst> Code;
		std::vector<CharClass> Classes;
		int32_t Groups = 1;                                    // group 0 included
		int32_t Slots = 2;                                     // 2 per group, then loop marks
		std::vector<std::pair<std::u16string, int32_t>> Names; // named groups
		bool bCanMatchNewline = false;
		int32_t FirstChar = -1;                                // a code point every match starts with (case-sensitive), or -1
	};

	/** Compiles a Python `re` pattern (MULTILINE on). False with Err on a bad pattern. */
	bool CompileRegex(std::u16string_view Pattern, bool bIgnoreCase, Program& Out, SearchError& Err);

	/** A literal pattern compiled to the same program form (used for templates' group checks and captures). */
	void CompileLiteral(std::u16string_view Literal, bool bIgnoreCase, Program& Out);

	enum class Status : uint8_t
	{
		NoMatch,
		Matched,
		Limit,
		Cancelled,
	};

	/** Runs a program over a text. Reuse one per search: it keeps its stacks. */
	class Matcher
	{
	public:
		Matcher(const Program& InProg, std::u16string_view InText, const SearchLimits& InLimits);

		/**
		 * Tries a match starting exactly at Start. RejectEmpty: a match ending at Start is refused (finditer after an
		 * empty match). RequiredEnd >= 0: only a match ending there is accepted. Steps accumulate until ResetSteps.
		 * bWholeWord: the match may not touch a word character on either side, as a look-behind and a look-ahead for no word character around the pattern.
		 */
		Status MatchAt(Offset Start, bool bRejectEmpty, Offset RequiredEnd, Offset& OutEnd, bool bWholeWord = false);
		void ResetSteps() { Steps = 0; }
		/** Polled every 4,096 steps (tens of microseconds); a set token ends the attempt with Cancelled. */
		void SetCancel(const CancelToken* InCancel) { Cancel = InCancel; }
		/** Capture slots of the last match: [2g] start, [2g+1] end, -1 when the group did not take part. */
		const std::vector<Offset>& Slots() const { return Slot; }

	private:
		struct Frame
		{
			int32_t Pc;
			Offset Sp;
			size_t LogSize;
		};

		Status Exec(int32_t Pc, Offset Sp, Offset RequiredEnd, Offset RejectEmptyAt, int32_t Depth, Offset& OutEnd);
		void Unwind(size_t LogSize);

		const Program& Prog;
		std::u16string_view Text;
		SearchLimits Limits;
		uint64_t Steps = 0;
		bool bWhole = false;
		const CancelToken* Cancel = nullptr;
		std::vector<Offset> Slot;
		std::vector<std::pair<int32_t, Offset>> Log;
		std::vector<Frame> Stack;
	};
}

namespace dream
{
	/** The compiled form behind SearchEngine (Private/). */
	class CompiledQuery
	{
	public:
		SearchQuery Query;
		bool bLiteral = true;
		std::u16string Needle;            // literal queries: the text, or its folded code points as UTF-16 when ignoring case
		std::vector<char32_t> FoldedNeedle;
		regex_detail::Program Prog;       // regex queries (and literal ones, for captures)
	};
}
