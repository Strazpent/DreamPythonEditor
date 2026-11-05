// SPDX-License-Identifier: Apache-2.0
// Dream core: the plain-mode edit builders (TDD §4.5 edit builders table; EDT-03 smart indent and Backspace to the
// indent stop, EDT-04 toggle comment and duplicate line, EDT-05 M1 pairing).
#include "dream/EditBuilders.h"

#include "dream/Utf.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace dream::editbuilders_detail
{
	size_t LeadingBlanks(std::u16string_view Line)
	{
		size_t I = 0;
		while (I < Line.size() && utf::IsPyBlank(Line[I]))
		{
			++I;
		}
		return I;
	}

	/** Visual columns of a run of text (tabs to the next multiple of TabSize). */
	int32_t Columns(std::u16string_view Text, int32_t TabSize)
	{
		const int32_t Tab = std::max(TabSize, 1);
		int32_t Cols = 0;
		for (char16_t C : Text)
		{
			Cols = C == u'\t' ? (Cols / Tab + 1) * Tab : Cols + 1;
		}
		return Cols;
	}

	bool IsCommentKind(TokenKind K)
	{
		return K == TokenKind::Comment || K == TokenKind::CellMarker || K == TokenKind::RegionMarker;
	}

	bool IsStringKind(TokenKind K)
	{
		return K == TokenKind::String || K == TokenKind::Docstring || K == TokenKind::StringEscape
			|| K == TokenKind::FStringSpec || K == TokenKind::FStringConversion;
	}

	bool IsQuote(char16_t C)
	{
		return C == u'\'' || C == u'"';
	}

	bool IsPrefixLetter(char16_t C)
	{
		return C == u'r' || C == u'R' || C == u'b' || C == u'B' || C == u'f' || C == u'F' || C == u'u' || C == u'U';
	}

	bool IsWordUnit(std::u16string_view Line, size_t At)
	{
		size_t Units = 1;
		return utf::IsIdentifierContinue(utf::DecodeAt(Line, At, Units));
	}

	/** One line with its (fresh) tokens. */
	struct LineCtx
	{
		LineNo Line = 0;
		Offset Start = 0;
		std::u16string Text;
		std::vector<uint32_t> Toks;
		LineFacts Facts;

		LineCtx(const TextSnapshot& Snapshot, PyLexer& Lexer, LineNo InLine)
			: Line(InLine)
			, Start(Snapshot.LineStart(InLine))
			, Text(Snapshot.LineText(InLine))
			, Facts(Lexer.Facts(InLine))
		{
			const std::span<const uint32_t> T = Lexer.Tokens(InLine);
			Toks.assign(T.begin(), T.end());
		}

		int32_t Len() const { return static_cast<int32_t>(Text.size()); }
		int32_t TokStart(size_t I) const { return TokenStart(Toks[I]); }
		int32_t TokEnd(size_t I) const { return I + 1 < Toks.size() ? TokenStart(Toks[I + 1]) : Len(); }
		TokenKind Kind(size_t I) const { return TokenKindOf(Toks[I]); }

		/** Not a comment and not a run of blanks (identifiers are Text tokens too). */
		bool IsSignificant(size_t I) const
		{
			const TokenKind K = Kind(I);
			if (IsCommentKind(K))
			{
				return false;
			}
			if (K != TokenKind::Text)
			{
				return true;
			}
			for (int32_t C = TokStart(I); C < TokEnd(I); ++C)
			{
				if (!utf::IsPyBlank(Text[static_cast<size_t>(C)]))
				{
					return true;
				}
			}
			return false;
		}

		/** Index of the token that holds column Col (Col < Len), or SIZE_MAX. */
		size_t TokenAt(int32_t Col) const
		{
			size_t Found = SIZE_MAX;
			for (size_t I = 0; I < Toks.size() && TokStart(I) <= Col; ++I)
			{
				Found = I;
			}
			return Found;
		}

		/** True when the caret at Col sits inside a string or a comment (auto-close and indent rules stay out). */
		bool InStringOrComment(int32_t Col) const
		{
			if (Col <= 0)
			{
				return Facts.Has(LF_StartsInString);
			}
			const size_t T = TokenAt(Col - 1);
			if (T == SIZE_MAX)
			{
				return false;
			}
			const TokenKind K = Kind(T);
			if (IsCommentKind(K))
			{
				return true;
			}
			if (K == TokenKind::Error)
			{
				// An unterminated single-quoted string is one Error token to the line end (§4.3.3).
				size_t P = static_cast<size_t>(TokStart(T));
				size_t Letters = 0;
				while (P < Text.size() && Letters < 2 && IsPrefixLetter(Text[P]))
				{
					++P;
					++Letters;
				}
				return P < Text.size() && IsQuote(Text[P]) && static_cast<int32_t>(P) < Col;
			}
			if (K == TokenKind::FStringBrace)
			{
				// After '{' the caret is in a field (code); after '}' it is back in the literal part.
				return Text[static_cast<size_t>(TokStart(T))] == u'}' && T + 1 < Toks.size() && IsStringKind(Kind(T + 1));
			}
			if (!IsStringKind(K))
			{
				return false;
			}
			if (TokEnd(T) > Col)
			{
				return true;
			}
			// The token ends at the caret: the string goes on when the next token is still part of it.
			if (T + 1 < Toks.size() && TokStart(T + 1) == Col)
			{
				const TokenKind N = Kind(T + 1);
				return IsStringKind(N) || N == TokenKind::FStringBrace;
			}
			// A triple-quoted string that stays open past the line end.
			return T + 1 == Toks.size() && Col == Len() && Facts.Has(LF_EndsInString);
		}
	};

	bool IsDedentKeyword(std::u16string_view Word)
	{
		return Word == u"return" || Word == u"pass" || Word == u"break" || Word == u"continue" || Word == u"raise";
	}

	std::u16string_view WordAt(std::u16string_view Line, int32_t Col)
	{
		if (Col < 0 || static_cast<size_t>(Col) >= Line.size())
		{
			return {};
		}
		size_t End = static_cast<size_t>(Col);
		while (End < Line.size() && IsWordUnit(Line, End))
		{
			++End;
		}
		return Line.substr(static_cast<size_t>(Col), End - static_cast<size_t>(Col));
	}

	/**
	 * The line whose indentation a continuation refers to: from Line, walk up over backslash continuations, lines
	 * that start in a string, and the lines whose brackets the Need unmatched closers at the caret close. (proposed)
	 * At most 1,000 lines up.
	 */
	LineNo ReferenceLine(PyLexer& Lexer, LineNo Line, int32_t Need)
	{
		LineNo L = Line;
		for (int32_t Steps = 0; L > 0 && Steps < 1000; ++Steps)
		{
			const LineFacts F = Lexer.Facts(L);
			if (Need <= 0 && !F.Has(LF_StartsContinuation) && !F.Has(LF_StartsInString))
			{
				break;
			}
			--L;
			Need = std::max(0, Need - Lexer.Facts(L).BracketDelta);
		}
		return L;
	}

	bool IsOpener(char16_t C)
	{
		return C == u'(' || C == u'[' || C == u'{';
	}

	char16_t CloserOf(char16_t C)
	{
		switch (C)
		{
		case u'(': return u')';
		case u'[': return u']';
		case u'{': return u'}';
		default: return C;
		}
	}

	BuiltEdit MakeSingle(Offset At, int64_t DeleteLen, std::u16string Insert, EditOrigin Origin, Offset Caret)
	{
		BuiltEdit E;
		E.Batch.Origin = Origin;
		E.Batch.Ranges.push_back(EditRange{At, DeleteLen, std::move(Insert)});
		E.Anchor = Caret;
		E.Caret = Caret;
		return E;
	}
}

namespace dream
{
	using namespace editbuilders_detail;

	EditBatch ToggleComment(const TextSnapshot& Snapshot, PyLexer& Lexer, LineRange Lines)
	{
		EditBatch Batch;
		Batch.Origin = EditOrigin::Command;
		const LineNo Count = Snapshot.LineCount();
		const LineNo First = std::clamp<LineNo>(std::min(Lines.First, Lines.Last), 0, Count - 1);
		const LineNo Last = std::clamp<LineNo>(std::max(Lines.First, Lines.Last), 0, Count - 1);
		Lexer.RelexThrough(Snapshot, Last);

		// Lines inside a string are left alone; blank lines neither count nor change.
		std::u16string Scratch;
		bool bAllCommented = true;
		bool bAny = false;
		size_t MinIndent = std::numeric_limits<size_t>::max();
		for (LineNo L = First; L <= Last; ++L)
		{
			if (Lexer.Facts(L).Has(LF_StartsInString))
			{
				continue;
			}
			const std::u16string_view Text = Snapshot.LineView(L, Scratch);
			const size_t Lead = LeadingBlanks(Text);
			if (Lead == Text.size())
			{
				continue;
			}
			bAny = true;
			MinIndent = std::min(MinIndent, Lead);
			if (Text[Lead] != u'#')
			{
				bAllCommented = false;
			}
		}
		if (!bAny)
		{
			return Batch;
		}

		for (LineNo L = First; L <= Last; ++L)
		{
			if (Lexer.Facts(L).Has(LF_StartsInString))
			{
				continue;
			}
			const std::u16string_view Text = Snapshot.LineView(L, Scratch);
			const size_t Lead = LeadingBlanks(Text);
			if (Lead == Text.size())
			{
				continue;
			}
			const Offset LineStart = Snapshot.LineStart(L);
			if (bAllCommented)
			{
				const int64_t Remove = (Lead + 1 < Text.size() && Text[Lead + 1] == u' ') ? 2 : 1;
				Batch.Ranges.push_back(EditRange{LineStart + static_cast<Offset>(Lead), Remove, std::u16string()});
			}
			else
			{
				Batch.Ranges.push_back(EditRange{LineStart + static_cast<Offset>(MinIndent), 0, std::u16string(u"# ")});
			}
		}
		return Batch;
	}

	std::u16string MakeIndent(int32_t Columns, const IndentStyle& Style)
	{
		std::u16string Out;
		Columns = std::max(Columns, 0);
		if (Style.bUseTabs)
		{
			const int32_t Tab = std::max(Style.TabSize, 1);
			Out.append(static_cast<size_t>(Columns / Tab), u'\t');
			Out.append(static_cast<size_t>(Columns % Tab), u' ');
		}
		else
		{
			Out.append(static_cast<size_t>(Columns), u' ');
		}
		return Out;
	}

	BuiltEdit SmartNewline(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset SelStart, Offset SelEnd, const IndentStyle& Style)
	{
		const Offset Length = Snapshot.Length();
		Offset Start = std::clamp<Offset>(std::min(SelStart, SelEnd), 0, Length);
		Offset End = std::clamp<Offset>(std::max(SelStart, SelEnd), 0, Length);
		const LineNo L = Snapshot.LineOf(Start);
		Lexer.RelexThrough(Snapshot, Snapshot.LineOf(End));
		const LineCtx Ctx(Snapshot, Lexer, L);
		const int32_t Col = static_cast<int32_t>(Start - Ctx.Start);
		const int32_t Lead = static_cast<int32_t>(LeadingBlanks(Ctx.Text));
		const std::u16string_view LineText(Ctx.Text);
		const int32_t Width = std::max(Style.Width, 1);
		// In a trailing comment the rules read the code before it; inside a string the line's indentation is kept.
		const size_t CommentTok = Col > 0 ? Ctx.TokenAt(Col - 1) : SIZE_MAX;
		const bool bInComment = CommentTok != SIZE_MAX && IsCommentKind(Ctx.Kind(CommentTok));
		const bool bInString = !bInComment && Ctx.InStringOrComment(Col);
		const int32_t RuleCol = bInComment ? Ctx.TokStart(CommentTok) : Col;
		const bool bBlankPrefix = !bInComment && Col <= Lead;

		std::u16string Indent;
		int32_t Cut = Col;   // where the replaced range starts on the caret line
		if (bInString)
		{
			Indent.assign(LineText.substr(0, static_cast<size_t>(std::min(Lead, Col))));
		}
		else if (bBlankPrefix)
		{
			Indent.assign(LineText.substr(0, static_cast<size_t>(Col)));
			Cut = 0;   // the blank prefix moves down: no trailing whitespace stays behind
		}
		else
		{
			while (!bInComment && Cut > 0 && utf::IsPyBlank(LineText[static_cast<size_t>(Cut - 1)]))
			{
				--Cut;
			}
			// The tokens before the caret: the last significant one, the bracket depth, the unclosed openers.
			int32_t Depth = 0;
			std::vector<size_t> Openers;
			size_t LastSig = SIZE_MAX;
			bool bSigAfter = false;
			for (size_t I = 0; I < Ctx.Toks.size(); ++I)
			{
				const TokenKind K = Ctx.Kind(I);
				const int32_t S = Ctx.TokStart(I);
				const bool bSig = Ctx.IsSignificant(I);
				if (S >= RuleCol)
				{
					bSigAfter = bSigAfter || bSig;
					continue;
				}
				if (K == TokenKind::Bracket)
				{
					const char16_t C = LineText[static_cast<size_t>(S)];
					if (IsOpener(C))
					{
						++Depth;
						Openers.push_back(I);
					}
					else
					{
						--Depth;
						if (!Openers.empty())
						{
							Openers.pop_back();
						}
					}
				}
				if (bSig)
				{
					LastSig = I;
				}
			}
			const bool bColon = LastSig != SIZE_MAX && Ctx.Kind(LastSig) == TokenKind::Operator
				&& LineText[static_cast<size_t>(Ctx.TokStart(LastSig))] == u':'
				&& Ctx.TokEnd(LastSig) - Ctx.TokStart(LastSig) == 1;
			const bool bBackslash = !bInComment && LineText[static_cast<size_t>(Cut - 1)] == u'\\';
			if (!Openers.empty())
			{
				// An unclosed bracket on this line: align after it when text follows it, else a hanging indent.
				const size_t Open = Openers.back();
				size_t Next = Open + 1;
				while (Next < Ctx.Toks.size() && Ctx.TokStart(Next) < RuleCol && Ctx.Kind(Next) == TokenKind::Text
					&& LeadingBlanks(LineText.substr(static_cast<size_t>(Ctx.TokStart(Next)),
						   static_cast<size_t>(Ctx.TokEnd(Next) - Ctx.TokStart(Next)))) == static_cast<size_t>(Ctx.TokEnd(Next) - Ctx.TokStart(Next)))
				{
					++Next;   // blanks after the opener
				}
				if (Next < Ctx.Toks.size() && Ctx.TokStart(Next) < std::min(Cut, RuleCol))
				{
					const int32_t AlignCols = Columns(LineText.substr(0, static_cast<size_t>(Ctx.TokStart(Next))), Style.TabSize);
					Indent.assign(LineText.substr(0, static_cast<size_t>(Lead)));
					Indent.append(static_cast<size_t>(std::max(0, AlignCols - Columns(Indent, Style.TabSize))), u' ');
				}
				else
				{
					Indent = MakeIndent(Columns(LineText.substr(0, static_cast<size_t>(Lead)), Style.TabSize) + Width, Style);
				}
			}
			else if (bBackslash)
			{
				// A backslash join continues the statement: one level more on its first line, the same after.
				const int32_t Base = Columns(LineText.substr(0, static_cast<size_t>(Lead)), Style.TabSize);
				Indent = MakeIndent(Ctx.Facts.Has(LF_StartsContinuation) ? Base : Base + Width, Style);
			}
			else
			{
				const LineNo Ref = ReferenceLine(Lexer, L, std::max(0, -Depth));
				const std::u16string RefText = Ref == L ? Ctx.Text : Snapshot.LineText(Ref);
				const int32_t RefLead = static_cast<int32_t>(LeadingBlanks(RefText));
				const int32_t RefCols = Columns(std::u16string_view(RefText).substr(0, static_cast<size_t>(RefLead)), Style.TabSize);
				const LineFacts RefFacts = Lexer.Facts(Ref);
				const bool bDedent = !bSigAfter && RefFacts.FirstTokenKind == TokenKind::Keyword
					&& IsDedentKeyword(WordAt(RefText, RefFacts.FirstTokenCol));
				if (bColon)
				{
					Indent = MakeIndent(RefCols + Width, Style);
				}
				else if (bDedent)
				{
					// One level fewer, down to the previous indent stop.
					Indent = MakeIndent(std::max(0, ((RefCols - 1) / Width) * Width), Style);
				}
				else if (Ref == L)
				{
					Indent.assign(LineText.substr(0, static_cast<size_t>(Lead)));
				}
				else
				{
					Indent.assign(std::u16string_view(RefText).substr(0, static_cast<size_t>(RefLead)));
				}
			}
		}

		if (!bInString && !bInComment)
		{
			// Blanks after the selection would sit between the new indentation and the moved text. With the caret in a
			// line's leading blanks they are the rest of that line's indentation and move down with its code (only a
			// blank-only rest of the line is dropped, as trailing whitespace).
			const LineNo EndLine = Snapshot.LineOf(End);
			const Offset EndLineStart = Snapshot.LineStart(EndLine);
			const Offset EndLineEnd = Snapshot.LineEnd(EndLine);
			std::u16string Head;
			Snapshot.CopyRange(EndLineStart, End - EndLineStart, Head);
			const bool bEndInIndent = LeadingBlanks(Head) == Head.size();   // End itself in its line's leading blanks
			std::u16string Tail;
			Snapshot.CopyRange(End, EndLineEnd - End, Tail);
			const size_t TailBlanks = LeadingBlanks(Tail);
			if (!bBlankPrefix || !bEndInIndent || TailBlanks == Tail.size())
			{
				End += static_cast<Offset>(TailBlanks);
			}
		}
		const Offset At = Ctx.Start + Cut;
		std::u16string Insert = u"\n";
		Insert += Indent;
		const Offset Caret = At + static_cast<Offset>(Insert.size());
		return MakeSingle(At, End - At, std::move(Insert), EditOrigin::Typing, Caret);
	}

	BuiltEdit SmartBackspace(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset Caret, const IndentStyle& Style)
	{
		BuiltEdit None;
		if (Caret <= 0 || Caret > Snapshot.Length())
		{
			return None;
		}
		const LineNo L = Snapshot.LineOf(Caret);
		Lexer.RelexThrough(Snapshot, L);
		const LineCtx Ctx(Snapshot, Lexer, L);
		const int32_t Col = static_cast<int32_t>(Caret - Ctx.Start);
		if (Col <= 0)
		{
			return None;   // at a line start the default joins the lines
		}
		const std::u16string_view Text(Ctx.Text);
		const char16_t Before = Text[static_cast<size_t>(Col - 1)];
		const char16_t After = Col < Ctx.Len() ? Text[static_cast<size_t>(Col)] : u'\0';

		// An empty pair outside strings and comments goes in one step.
		if (IsOpener(Before) && After == CloserOf(Before))
		{
			const size_t T = Ctx.TokenAt(Col - 1);
			if (T != SIZE_MAX && Ctx.Kind(T) == TokenKind::Bracket && !Ctx.InStringOrComment(Col - 1))
			{
				return MakeSingle(Caret - 1, 2, std::u16string(), EditOrigin::Typing, Caret - 1);
			}
		}
		if (IsQuote(Before) && After == Before && (Col < 2 || Text[static_cast<size_t>(Col - 2)] != Before)
			&& (Col + 1 >= Ctx.Len() || Text[static_cast<size_t>(Col + 1)] != Before))
		{
			const size_t T = Ctx.TokenAt(Col - 1);
			if (T != SIZE_MAX && Ctx.Kind(T) == TokenKind::String && Ctx.TokStart(T) == Col - 1 && Ctx.TokEnd(T) == Col + 1)
			{
				return MakeSingle(Caret - 1, 2, std::u16string(), EditOrigin::Typing, Caret - 1);
			}
		}

		// In leading blanks: back to the previous indent stop, over spaces only.
		const int32_t Lead = static_cast<int32_t>(LeadingBlanks(Text));
		if (Col > Lead || Before != u' ')
		{
			return None;
		}
		const int32_t Width = std::max(Style.Width, 1);
		const int32_t Cols = Columns(Text.substr(0, static_cast<size_t>(Col)), Style.TabSize);
		const int32_t Target = ((Cols - 1) / Width) * Width;
		int32_t From = Col;
		int32_t At = Cols;
		while (From > 0 && At > Target && Text[static_cast<size_t>(From - 1)] == u' ')
		{
			--From;
			--At;
		}
		return MakeSingle(Ctx.Start + From, Col - From, std::u16string(), EditOrigin::Typing, Ctx.Start + From);
	}

	BuiltEdit AutoClosePair(const TextSnapshot& Snapshot, PyLexer& Lexer, Offset SelStart, Offset SelEnd, char16_t Typed)
	{
		BuiltEdit None;
		const bool bBracket = IsOpener(Typed);
		if ((!bBracket && !IsQuote(Typed)) || SelStart != SelEnd || SelStart < 0 || SelStart > Snapshot.Length())
		{
			return None;
		}
		const Offset Caret = SelStart;
		const LineNo L = Snapshot.LineOf(Caret);
		Lexer.RelexThrough(Snapshot, L);
		const LineCtx Ctx(Snapshot, Lexer, L);
		const int32_t Col = static_cast<int32_t>(Caret - Ctx.Start);
		if (Ctx.InStringOrComment(Col))
		{
			return None;
		}
		const std::u16string_view Text(Ctx.Text);
		const char16_t Next = Col < Ctx.Len() ? Text[static_cast<size_t>(Col)] : u'\0';
		const bool bNextOk = Next == u'\0' || utf::IsPyBlank(Next) || std::u16string_view(u")]}:,;.=>").find(Next) != std::u16string_view::npos;
		if (bBracket)
		{
			if (!bNextOk)
			{
				return None;
			}
			std::u16string Pair{Typed, CloserOf(Typed)};
			return MakeSingle(Caret, 0, std::move(Pair), EditOrigin::Typing, Caret + 1);
		}

		// Quotes. The third quote of an empty string opens a triple-quoted pair.
		if (Col >= 2 && Text[static_cast<size_t>(Col - 1)] == Typed && Text[static_cast<size_t>(Col - 2)] == Typed
			&& (Col < 3 || Text[static_cast<size_t>(Col - 3)] != Typed))
		{
			const size_t T = Ctx.TokenAt(Col - 2);
			if (T != SIZE_MAX && Ctx.Kind(T) == TokenKind::String && Ctx.TokStart(T) <= Col - 2 && Ctx.TokEnd(T) == Col && bNextOk)
			{
				std::u16string Quotes(4, Typed);
				return MakeSingle(Caret, 0, std::move(Quotes), EditOrigin::Typing, Caret + 1);
			}
			return None;
		}
		if (!bNextOk || (Col >= 1 && Text[static_cast<size_t>(Col - 1)] == Typed))
		{
			return None;
		}
		if (Col >= 1 && IsWordUnit(Text, static_cast<size_t>(Col - 1)))
		{
			// Only a string prefix may stand right before the quote (f'', rb"", ...).
			int32_t W = Col;
			while (W > 0 && IsPrefixLetter(Text[static_cast<size_t>(W - 1)]) && Col - W < 3)
			{
				--W;
			}
			const std::u16string_view Prefix = Text.substr(static_cast<size_t>(W), static_cast<size_t>(Col - W));
			std::u16string Lower;
			for (char16_t C : Prefix)
			{
				Lower.push_back(C >= u'A' && C <= u'Z' ? static_cast<char16_t>(C + 32) : C);
			}
			const bool bPrefix = Lower == u"r" || Lower == u"u" || Lower == u"f" || Lower == u"b" || Lower == u"rb"
				|| Lower == u"br" || Lower == u"fr" || Lower == u"rf";
			if (!bPrefix || (W > 0 && IsWordUnit(Text, static_cast<size_t>(W - 1))))
			{
				return None;
			}
		}
		std::u16string Pair{Typed, Typed};
		return MakeSingle(Caret, 0, std::move(Pair), EditOrigin::Typing, Caret + 1);
	}

	BuiltEdit TypeOverClosing(const TextSnapshot& Snapshot, Offset Caret, char16_t Typed)
	{
		BuiltEdit E;
		const bool bCloser = Typed == u')' || Typed == u']' || Typed == u'}' || IsQuote(Typed);
		if (!bCloser || Caret < 0 || Caret >= Snapshot.Length())
		{
			return E;
		}
		std::u16string Unit;
		Snapshot.CopyRange(Caret, 1, Unit);
		if (Unit.size() != 1 || Unit[0] != Typed)
		{
			return E;
		}
		E.bMoveOnly = true;
		E.Anchor = Caret + 1;
		E.Caret = Caret + 1;
		return E;
	}

	BuiltEdit DuplicateLineOrSelection(const TextSnapshot& Snapshot, Offset SelStart, Offset SelEnd)
	{
		const Offset Length = Snapshot.Length();
		const Offset Start = std::clamp<Offset>(std::min(SelStart, SelEnd), 0, Length);
		const Offset End = std::clamp<Offset>(std::max(SelStart, SelEnd), 0, Length);
		if (Start != End)
		{
			std::u16string Copy;
			Snapshot.CopyRange(Start, End - Start, Copy);
			BuiltEdit E = MakeSingle(End, 0, std::move(Copy), EditOrigin::Command, End + (End - Start));
			E.Anchor = End;
			return E;
		}
		const LineNo L = Snapshot.LineOf(Start);
		const Offset LineStart = Snapshot.LineStart(L);
		const Offset LineEnd = Snapshot.LineEnd(L);
		std::u16string Insert = u"\n";
		Snapshot.CopyRange(LineStart, LineEnd - LineStart, Insert);
		const Offset Caret = LineEnd + 1 + (Start - LineStart);
		return MakeSingle(LineEnd, 0, std::move(Insert), EditOrigin::Command, Caret);
	}

	IndentStyle DetectIndent(const TextSnapshot& Snapshot, PyLexer& Lexer, const IndentStyle& Fallback)
	{
		const LineNo Count = Snapshot.LineCount();
		Lexer.RelexThrough(Snapshot, Count - 1);
		int32_t TabLines = 0;
		int32_t SpaceLines = 0;
		int32_t Steps[9] = {};
		int32_t Depth = 0;
		int32_t PrevCols = 0;
		std::u16string Scratch;
		for (LineNo L = 0; L < Count; ++L)
		{
			const LineFacts F = Lexer.Facts(L);
			const bool bContinuation = F.Has(LF_StartsInString) || F.Has(LF_StartsContinuation) || Depth > 0;
			Depth = std::max(0, Depth + F.BracketDelta);
			if (bContinuation || F.Has(LF_Blank) || F.Has(LF_CommentOnly))
			{
				continue;
			}
			const std::u16string_view Text = Snapshot.LineView(L, Scratch);
			const size_t Lead = LeadingBlanks(Text);
			int32_t Cols = 0;
			if (Lead > 0)
			{
				if (Text[0] == u'\t')
				{
					++TabLines;
				}
				else
				{
					++SpaceLines;
				}
				bool bSpacesOnly = true;
				for (size_t I = 0; I < Lead; ++I)
				{
					bSpacesOnly = bSpacesOnly && Text[I] == u' ';
				}
				Cols = bSpacesOnly ? static_cast<int32_t>(Lead) : -1;
			}
			if (Cols > PrevCols && Cols - PrevCols <= 8)
			{
				++Steps[Cols - PrevCols];
			}
			if (Cols >= 0)
			{
				PrevCols = Cols;
			}
		}
		IndentStyle Out = Fallback;
		if (TabLines + SpaceLines < 2)
		{
			return Out;
		}
		if (TabLines != SpaceLines)
		{
			Out.bUseTabs = TabLines > SpaceLines;
		}
		if (Out.bUseTabs)
		{
			Out.Width = std::max(Fallback.TabSize, 1);
			return Out;
		}
		int32_t Best = 0;
		int32_t BestCount = 0;
		bool bTie = false;
		for (int32_t Unit : {2, 3, 4, 8})
		{
			if (Steps[Unit] > BestCount)
			{
				Best = Unit;
				BestCount = Steps[Unit];
				bTie = false;
			}
			else if (Steps[Unit] == BestCount && BestCount > 0)
			{
				bTie = true;
			}
		}
		if (Best > 0 && !bTie)
		{
			Out.Width = Best;
		}
		return Out;
	}
}
