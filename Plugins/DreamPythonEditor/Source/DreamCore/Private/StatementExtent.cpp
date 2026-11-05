// SPDX-License-Identifier: Apache-2.0
// Dream core: the statement extent (TDD §4.5 rules 1-6, RUN-02), the next-statement target (RUN-26, PO-02), selection
// lines and the REPL lexer pre-check (§6.5). Reads PyLexer line facts only; never calls Python.
#include "dream/StatementExtent.h"

#include "dream/Utf.h"

#include <algorithm>
#include <vector>

namespace dream::extent_detail
{
	/**
	 * The §4.5 terms over one text: continuation lines (StartsInString, StartsContinuation or a positive running
	 * bracket depth, clamped at 0 as in FoldModel), logical lines, statement starts and first tokens.
	 */
	class Model
	{
	public:
		Model(std::vector<LineFacts> InFacts, std::vector<std::u16string_view> InLines)
			: Facts(std::move(InFacts)), Lines(std::move(InLines))
		{
			const size_t N = Facts.size();
			Cont.assign(N, false);
			LogicalStartOf.assign(N, 0);
			int64_t Depth = 0;
			for (size_t L = 0; L < N; ++L)
			{
				const LineFacts& F = Facts[L];
				Cont[L] = L > 0 && (F.Has(LF_StartsInString) || F.Has(LF_StartsContinuation) || Depth > 0);
				LogicalStartOf[L] = Cont[L] ? LogicalStartOf[L - 1] : static_cast<LineNo>(L);
				Depth = std::max<int64_t>(0, Depth + F.BracketDelta);
			}
			EndDepth = Depth;
		}

		LineNo Count() const { return static_cast<LineNo>(Facts.size()); }
		bool IsCont(LineNo L) const { return Cont[static_cast<size_t>(L)]; }
		const LineFacts& FactsOf(LineNo L) const { return Facts[static_cast<size_t>(L)]; }
		int32_t Indent(LineNo L) const { return FactsOf(L).IndentCols; }
		bool IsBlankish(LineNo L) const { return FactsOf(L).Has(LF_Blank) || FactsOf(L).Has(LF_CommentOnly); }
		bool IsStatementStart(LineNo L) const { return !IsCont(L) && !IsBlankish(L); }
		LineNo LogicalStart(LineNo L) const { return LogicalStartOf[static_cast<size_t>(L)]; }
		int64_t DepthAtEnd() const { return EndDepth; }

		LineNo LogicalEnd(LineNo L) const
		{
			LineNo E = L;
			while (E + 1 < Count() && IsCont(E + 1))
			{
				++E;
			}
			return E;
		}

		bool EndsWithColon(LineNo Start) const { return FactsOf(LogicalEnd(Start)).Has(LF_EndsWithColon); }

		/** The first token's text: "@" for a decorator, else the identifier there (empty for anything else). */
		std::u16string_view Word(LineNo L, int32_t Skip = 0) const
		{
			const LineFacts& F = FactsOf(L);
			if (F.FirstTokenCol < 0)
			{
				return {};
			}
			const std::u16string_view Text = Lines[static_cast<size_t>(L)];
			size_t I = static_cast<size_t>(F.FirstTokenCol);
			for (int32_t K = 0;; ++K)
			{
				if (I >= Text.size())
				{
					return {};
				}
				if (Text[I] == u'@')
				{
					if (K == Skip)
					{
						return Text.substr(I, 1);
					}
					++I;
				}
				else
				{
					const size_t Begin = I;
					while (I < Text.size())
					{
						size_t Units = 1;
						if (!utf::IsIdentifierContinue(utf::DecodeAt(Text, I, Units)))
						{
							break;
						}
						I += Units;
					}
					if (K == Skip)
					{
						return Text.substr(Begin, I - Begin);
					}
					if (I == Begin)
					{
						return {};
					}
				}
				while (I < Text.size() && utf::IsPyBlank(Text[I]))
				{
					++I;
				}
			}
		}

		std::optional<LineNo> NextStatementStart(LineNo From) const
		{
			for (LineNo L = std::max<LineNo>(From, 0); L < Count(); ++L)
			{
				if (IsStatementStart(L))
				{
					return L;
				}
			}
			return std::nullopt;
		}

		/** The statement start of the logical line that ends above Before (blank and comment lines skipped). */
		std::optional<LineNo> PrevStatementStart(LineNo Before) const
		{
			for (LineNo L = Before - 1; L >= 0; --L)
			{
				if (IsCont(L) || !IsBlankish(L))
				{
					const LineNo S = LogicalStart(L);
					if (IsStatementStart(S))
					{
						return S;
					}
					L = S;
				}
			}
			return std::nullopt;
		}

		static bool IsOpenerWord(std::u16string_view W)
		{
			return W == u"if" || W == u"while" || W == u"for" || W == u"try" || W == u"with" || W == u"def" || W == u"class"
				|| W == u"async";
		}

		static bool IsClauseWord(std::u16string_view W)
		{
			return W == u"elif" || W == u"else" || W == u"except" || W == u"finally";
		}

		bool OpensCompound(LineNo S) const { return IsOpenerWord(Word(S)) || EndsWithColon(S); }
		bool IsMatchHeader(LineNo S) const { return Word(S) == u"match" && EndsWithColon(S); }

		/** The previous statement start with less indentation. */
		std::optional<LineNo> EnclosingHeader(LineNo S) const
		{
			std::optional<LineNo> P = PrevStatementStart(S);
			while (P && Indent(*P) >= Indent(S))
			{
				P = PrevStatementStart(*P);
			}
			return P;
		}

		bool IsCaseClause(LineNo S) const
		{
			if (Word(S) != u"case")
			{
				return false;
			}
			const std::optional<LineNo> H = EnclosingHeader(S);
			return H && IsMatchHeader(*H);
		}

		bool IsClauseLine(LineNo S) const { return IsClauseWord(Word(S)) || IsCaseClause(S); }

		/** Rule 3: the compound statement a clause line belongs to; nullopt when it has none. */
		std::optional<LineNo> ClauseOwner(LineNo S) const
		{
			if (IsClauseWord(Word(S)))
			{
				std::optional<LineNo> P = PrevStatementStart(S);
				while (P)
				{
					if (Indent(*P) < Indent(S))
					{
						return std::nullopt;
					}
					if (Indent(*P) == Indent(S) && !IsClauseWord(Word(*P)))
					{
						return OpensCompound(*P) ? P : std::nullopt;
					}
					P = PrevStatementStart(*P);
				}
				return std::nullopt;
			}
			if (IsCaseClause(S))
			{
				return EnclosingHeader(S);
			}
			return std::nullopt;
		}

		/** Drops trailing blank and comment-only lines (never one that starts inside a string), not below Floor. */
		LineNo TrimEnd(LineNo End, LineNo Floor) const
		{
			while (End > Floor && IsBlankish(End) && !FactsOf(End).Has(LF_StartsInString))
			{
				--End;
			}
			return End;
		}

		/** The end of the body of the logical line at H plus every following clause at H's indentation (rule 5). */
		LineNo CompoundEnd(LineNo H) const
		{
			const int32_t I = Indent(H);
			LineNo End = LogicalEnd(H);
			std::optional<LineNo> Next = DedentAfter(End, I);
			End = TrimEnd(Next ? *Next - 1 : Count() - 1, End);
			while (Next && Indent(*Next) == I && IsClauseWord(Word(*Next)))
			{
				const LineNo ClauseEnd = LogicalEnd(*Next);
				Next = DedentAfter(ClauseEnd, I);
				End = TrimEnd(Next ? *Next - 1 : Count() - 1, ClauseEnd);
			}
			return End;
		}

		/** The next statement start after line After whose indentation is <= Indent. */
		std::optional<LineNo> DedentAfter(LineNo After, int32_t IndentCols) const
		{
			for (LineNo L = After + 1; L < Count(); ++L)
			{
				if (IsStatementStart(L) && Indent(L) <= IndentCols)
				{
					return L;
				}
			}
			return std::nullopt;
		}

		/** Rules 4-6 from a statement start S. */
		LineRange ExtentFrom(LineNo S) const
		{
			LineNo H = S;
			while (Word(H) == u"@")
			{
				const std::optional<LineNo> Next = NextStatementStart(LogicalEnd(H) + 1);
				if (!Next)
				{
					return LineRange{S, TrimEnd(LogicalEnd(H), S)};
				}
				H = *Next;
			}
			if (OpensCompound(H))
			{
				return LineRange{S, CompoundEnd(H)};
			}
			return LineRange{S, TrimEnd(LogicalEnd(H), S)};
		}

		bool IsDefinitionStart(LineNo S) const
		{
			const std::u16string_view W = Word(S);
			return W == u"@" || W == u"def" || W == u"class" || (W == u"async" && Word(S, 1) == u"def");
		}

		std::optional<LineRange> Extent(LineNo Caret) const
		{
			if (Count() == 0)
			{
				return std::nullopt;
			}
			Caret = std::clamp<LineNo>(Caret, 0, Count() - 1);
			if (!IsCont(Caret) && IsBlankish(Caret))
			{
				return std::nullopt;   // rule 1
			}
			LineNo S = LogicalStart(Caret);   // rule 2
			if (!IsStatementStart(S))
			{
				return std::nullopt;
			}
			if (IsDefinitionStart(S))
			{
				for (;;)
				{
					const std::optional<LineNo> P = PrevStatementStart(S);
					if (!P || Word(*P) != u"@" || Indent(*P) != Indent(S))
					{
						break;
					}
					S = *P;
				}
			}
			if (IsClauseLine(S))   // rule 3
			{
				if (const std::optional<LineNo> Owner = ClauseOwner(S))
				{
					S = *Owner;
				}
			}
			return ExtentFrom(S);
		}

		std::optional<LineNo> NextAfter(LineRange Ran) const
		{
			std::optional<LineNo> T = NextStatementStart(Ran.Last + 1);
			while (T && IsClauseLine(*T))
			{
				const std::optional<LineNo> H = ClauseOwner(*T);
				if (!H)
				{
					break;
				}
				const LineRange E = ExtentFrom(*H);
				if (!(E.First <= Ran.First && Ran.Last <= E.Last))
				{
					break;
				}
				T = NextStatementStart(E.Last + 1);
			}
			return T;
		}

		/** The last statement start, for the REPL checks. */
		std::optional<LineNo> LastStatementStart() const
		{
			for (LineNo L = Count() - 1; L >= 0; --L)
			{
				if (IsStatementStart(L))
				{
					return L;
				}
			}
			return std::nullopt;
		}

	private:
		std::vector<LineFacts> Facts;
		std::vector<std::u16string_view> Lines;
		std::vector<bool> Cont;
		std::vector<LineNo> LogicalStartOf;
		int64_t EndDepth = 0;
	};

	Model FromLexer(const TextSnapshot& Snapshot, PyLexer& Lexer)
	{
		// Freshness (§4.5, A9): relex to the end of the buffer, so no fact is stale.
		Lexer.RelexThrough(Snapshot, Snapshot.LineCount() - 1);
		const LineNo N = Snapshot.LineCount();
		std::vector<LineFacts> Facts;
		std::vector<std::u16string_view> Lines;
		Facts.reserve(static_cast<size_t>(N));
		Lines.reserve(static_cast<size_t>(N));
		std::u16string Scratch;
		for (LineNo L = 0; L < N; ++L)
		{
			Facts.push_back(Lexer.Facts(L));
			Lines.push_back(Snapshot.LineView(L, Scratch));   // contiguous in slice A; the views live with Snapshot
		}
		return Model(std::move(Facts), std::move(Lines));
	}

	Model FromText(std::u16string_view Source, LexGrammar Grammar, LexStateInfo& OutEndInfo)
	{
		std::vector<std::vector<uint32_t>> Tokens;
		std::vector<LineFacts> Facts;
		LexStateId EndState = NormalState;
		PyLexer::LexText(Source, Grammar, Tokens, Facts, EndState, OutEndInfo);
		std::vector<std::u16string_view> Lines;
		size_t From = 0;
		for (;;)
		{
			const size_t Nl = Source.find(u'\n', From);
			if (Nl == std::u16string_view::npos)
			{
				Lines.push_back(Source.substr(From));
				break;
			}
			Lines.push_back(Source.substr(From, Nl - From));
			From = Nl + 1;
		}
		return Model(std::move(Facts), std::move(Lines));
	}
}

namespace dream
{
	std::optional<LineRange> StatementExtent(const TextSnapshot& Snapshot, PyLexer& Lexer, LineNo Caret)
	{
		return extent_detail::FromLexer(Snapshot, Lexer).Extent(Caret);
	}

	std::optional<LineNo> NextStatementLine(const TextSnapshot& Snapshot, PyLexer& Lexer, LineRange Ran)
	{
		if (Ran.Last < Ran.First)
		{
			std::swap(Ran.First, Ran.Last);
		}
		return extent_detail::FromLexer(Snapshot, Lexer).NextAfter(Ran);
	}

	std::optional<LineNo> NextStatementStartFrom(const TextSnapshot& Snapshot, PyLexer& Lexer, LineNo Line)
	{
		return extent_detail::FromLexer(Snapshot, Lexer).NextStatementStart(Line);
	}

	int32_t FirstNonBlankColumn(const TextSnapshot& Snapshot, LineNo Line)
	{
		std::u16string Scratch;
		const std::u16string_view Text = Snapshot.LineView(Line, Scratch);
		size_t I = 0;
		while (I < Text.size() && utf::IsPyBlank(Text[I]))
		{
			++I;
		}
		return static_cast<int32_t>(I);
	}

	LineRange SelectionLines(const TextSnapshot& Snapshot, Offset SelStart, Offset SelEnd)
	{
		const Offset Lo = std::min(SelStart, SelEnd);
		const Offset Hi = std::max(SelStart, SelEnd);
		const LineNo First = Snapshot.LineOf(Lo);
		LineNo Last = Snapshot.LineOf(Hi);
		if (Hi > Lo && Last > First && Snapshot.LineStart(Last) == Hi)
		{
			--Last;   // a selection ending at column 0 ends on the line before (§4.5, Shift+Down)
		}
		return LineRange{First, Last};
	}

	ReplPrecheck ReplPreCheck(std::u16string_view Source, LexGrammar Grammar)
	{
		LexStateInfo EndInfo;
		const extent_detail::Model M = extent_detail::FromText(Source, Grammar, EndInfo);
		if (EndInfo.bInString || EndInfo.bBackslashJoin || M.DepthAtEnd() > 0)
		{
			return ReplPrecheck::DefinitelyIncomplete;   // open triple-quoted string, trailing backslash, open bracket
		}
		const std::optional<LineNo> Last = M.LastStatementStart();
		if (!Last)
		{
			return ReplPrecheck::Unknown;
		}
		const std::u16string_view W = M.Word(*Last);
		if (W == u"@")
		{
			return ReplPrecheck::DefinitelyIncomplete;
		}
		const bool bOpener = W == u"if" || W == u"for" || W == u"while" || W == u"try" || W == u"with" || W == u"def"
			|| W == u"class" || W == u"async" || W == u"match";
		if (bOpener && M.EndsWithColon(*Last))
		{
			return ReplPrecheck::DefinitelyIncomplete;
		}
		return ReplPrecheck::Unknown;
	}

	bool LastLogicalLineIndented(std::u16string_view Source, LexGrammar Grammar)
	{
		LexStateInfo EndInfo;
		const extent_detail::Model M = extent_detail::FromText(Source, Grammar, EndInfo);
		const std::optional<LineNo> Last = M.LastStatementStart();
		return Last && M.Indent(*Last) > 0;
	}
}
