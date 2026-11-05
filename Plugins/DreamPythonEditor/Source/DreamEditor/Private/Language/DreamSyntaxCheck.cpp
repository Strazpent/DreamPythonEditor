// SPDX-License-Identifier: Apache-2.0
// The in-process syntax check (PO-06 F4). See DreamSyntaxCheck.h.
#include "Language/DreamSyntaxCheck.h"

#include "Module/DreamText.h"
#include "dream/PyLexer.h"
#include "dream/TextBuffer.h"

#include <string>
#include <string_view>
#include <vector>

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamSyntaxCheckLocal
{
	using dream::TokenKind;

	/** One bracket on the open stack. */
	struct FOpen
	{
		char16_t Char = 0;
		int32 Line = 0;
		int32 Column = 0;
	};

	/** One check: the lines, the lexer's view of them and the items found. */
	struct FChecker
	{
		std::u16string_view Text;
		std::vector<std::u16string_view> Lines;
		std::vector<std::vector<uint32_t>> Tokens;
		std::vector<dream::LineFacts> Facts;
		dream::LexStateInfo EndInfo;
		uint64 Version = 0;
		bool bLongLines = false;     // a line past dream::LongLineColumns: its tail has no tokens
		TArray<FDreamProblem> Items;

		bool Full() const { return Items.Num() >= FDreamSyntaxCheck::MaxItems; }

		void Add(int32 Line, int32 StartCol, int32 EndCol, const FText& Message, int32 EndLine = INDEX_NONE)
		{
			if (Full())
			{
				return;
			}
			FDreamProblem& Item = Items.AddDefaulted_GetRef();
			Item.Source = EDreamProblemSource::Syntax;
			Item.Severity = EDreamSeverity::Error;
			Item.Code = FDreamSyntaxCheck::Code;
			Item.Message = Message.ToString();
			Item.DedupClass = TEXT("syntax");
			Item.StartLine = Line;
			Item.StartColumn = FMath::Max(0, StartCol);
			Item.EndLine = EndLine == INDEX_NONE ? Line : EndLine;
			Item.EndColumn = FMath::Max(Item.StartColumn + (Item.EndLine == Line ? 1 : 0), EndCol);
			Item.DocVersion = Version;
		}

		char16_t CharAt(int32 Line, int32 Col) const
		{
			if (Line < 0 || Line >= static_cast<int32>(Lines.size()) || Col < 0 || Col >= static_cast<int32>(Lines[Line].size()))
			{
				return 0;
			}
			return Lines[Line][Col];
		}

		int32 LineLen(int32 Line) const
		{
			return Line >= 0 && Line < static_cast<int32>(Lines.size()) ? static_cast<int32>(Lines[Line].size()) : 0;
		}

		/** The end column of token Index on Line (the next token's start, or the line end). */
		int32 TokenEnd(int32 Line, size_t Index) const
		{
			const std::vector<uint32_t>& Toks = Tokens[Line];
			return Index + 1 < Toks.size() ? dream::TokenStart(Toks[Index + 1]) : LineLen(Line);
		}

		/** The token's text, at most 256 units (keywords, operators). */
		std::u16string_view TokenText(int32 Line, size_t Index) const
		{
			const int32 Start = dream::TokenStart(Tokens[Line][Index]);
			const int32 End = TokenEnd(Line, Index);
			if (Start >= End || Start >= LineLen(Line))
			{
				return {};
			}
			return Lines[Line].substr(static_cast<size_t>(Start), static_cast<size_t>(FMath::Min(End - Start, 256)));
		}

		void Run();
		void CheckErrorToken(int32 Line, size_t Index);
		void CheckStatements();
	};

	FString CharLabel(char16_t Char)
	{
		return FString::Printf(TEXT("%c"), static_cast<TCHAR>(Char));
	}

	/** The code point at Col of Text (a surrogate pair as one), its label and its length in UTF-16 units. */
	uint32 CodePointAt(std::u16string_view Text, int32 Col, FString& OutLabel, int32& OutUnits)
	{
		const char16_t High = Text[static_cast<size_t>(Col)];
		if (High >= 0xD800 && High <= 0xDBFF && static_cast<size_t>(Col + 1) < Text.size())
		{
			const char16_t Low = Text[static_cast<size_t>(Col + 1)];
			if (Low >= 0xDC00 && Low <= 0xDFFF)
			{
				OutLabel = FString::Printf(TEXT("%c%c"), static_cast<TCHAR>(High), static_cast<TCHAR>(Low));
				OutUnits = 2;
				return 0x10000u + ((static_cast<uint32>(High) - 0xD800u) << 10) + (static_cast<uint32>(Low) - 0xDC00u);
			}
		}
		OutLabel = CharLabel(High);
		OutUnits = 1;
		return High;
	}

	char16_t Matching(char16_t Opener)
	{
		return Opener == u'(' ? u')' : Opener == u'[' ? u']' : u'}';
	}

	bool IsQuote(char16_t C)
	{
		return C == u'\'' || C == u'"';
	}

	bool IsStringish(TokenKind Kind)
	{
		return Kind == TokenKind::String || Kind == TokenKind::Docstring || Kind == TokenKind::StringEscape || Kind == TokenKind::FStringSpec;
	}

	/** The statement keywords whose header must end with ':' (soft keywords match and case are left to dream-check). */
	bool IsCompoundKeyword(std::u16string_view Word)
	{
		static const std::u16string_view Words[] = { u"if", u"elif", u"else", u"for", u"while", u"def", u"class", u"try", u"except",
			u"finally", u"with" };
		for (const std::u16string_view Candidate : Words)
		{
			if (Word == Candidate)
			{
				return true;
			}
		}
		return false;
	}

	void FChecker::CheckErrorToken(int32 Line, size_t Index)
	{
		const int32 Col = dream::TokenStart(Tokens[Line][Index]);
		const int32 End = TokenEnd(Line, Index);
		const char16_t C = CharAt(Line, Col);
		const char16_t Next = CharAt(Line, Col + 1);
		const TokenKind Before = Index > 0 ? dream::TokenKindOf(Tokens[Line][Index - 1]) : TokenKind::Text;
		// An unterminated single-quoted string: the lexer turns the rest of the line from the literal's start (its prefix
		// letters or its quote) into one error token.
		int32 Probe = Col;
		while (Probe < LineLen(Line) && Probe - Col < 3 && FChar::IsAlpha(static_cast<TCHAR>(CharAt(Line, Probe))))
		{
			++Probe;
		}
		if (IsQuote(CharAt(Line, Probe)))
		{
			Add(Line, Col, LineLen(Line), FText::Format(LOCTEXT("Lang.SyntaxUnterminated", "unterminated string literal (detected at line {0})"),
				FText::AsNumber(Line + 1)));
			return;
		}
		if (C == u'\\')
		{
			if (IsStringish(Before) || (Next != 0 && IsStringish(Index + 1 < Tokens[Line].size() ? dream::TokenKindOf(Tokens[Line][Index + 1]) : TokenKind::Text)))
			{
				// CPython 3.11: a truncated \x, \u, \U or a malformed \N{...} is a decode error of the literal (review m3).
				const FText Message = Next == u'x' ? LOCTEXT("Lang.SyntaxEscapeX", "(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\xXX escape")
					: Next == u'u' ? LOCTEXT("Lang.SyntaxEscapeU", "(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\uXXXX escape")
					: Next == u'U' ? LOCTEXT("Lang.SyntaxEscapeU8", "(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\UXXXXXXXX escape")
					: Next == u'N' ? LOCTEXT("Lang.SyntaxEscapeN", "(unicode error) 'unicodeescape' codec can't decode bytes: malformed \\N character escape")
					: LOCTEXT("Lang.SyntaxEscape", "(unicode error) 'unicodeescape' codec can't decode bytes");
				Add(Line, Col, End, Message);
			}
			else
			{
				Add(Line, Col, Col + 1, LOCTEXT("Lang.SyntaxContinuation", "unexpected character after line continuation character"));
			}
			return;
		}
		if (FChar::IsDigit(static_cast<TCHAR>(C)) || (C == u'.' && FChar::IsDigit(static_cast<TCHAR>(Next))))
		{
			Add(Line, Col, End, LOCTEXT("Lang.SyntaxNumber", "invalid decimal literal"));
			return;
		}
		if (C == u'}')
		{
			Add(Line, Col, Col + 1, LOCTEXT("Lang.SyntaxFStringBrace", "f-string: single '}' is not allowed"));
			return;
		}
		if (C == u'#')
		{
			Add(Line, Col, Col + 1, LOCTEXT("Lang.SyntaxFStringHash", "f-string expression part cannot include '#'"));
			return;
		}
		if (C == u'{')
		{
			Add(Line, Col, Col + 1, LOCTEXT("Lang.SyntaxFStringExpecting", "f-string: expecting '}'"));
			return;
		}
		if (C == u'!')
		{
			const bool bLetter = FChar::IsAlpha(static_cast<TCHAR>(Next));
			Add(Line, Col, Col + (bLetter ? 2 : 1), bLetter
				? FText::Format(LOCTEXT("Lang.SyntaxFStringConversion", "f-string: invalid conversion character '{0}': expected 's', 'r', or 'a'"),
					FText::FromString(CharLabel(Next)))
				: LOCTEXT("Lang.SyntaxFStringConversionMissing", "f-string: missing conversion character"));
			return;
		}
		if (C == u')' || C == u']')
		{
			Add(Line, Col, Col + 1, FText::Format(LOCTEXT("Lang.SyntaxFStringUnmatched", "f-string: unmatched '{0}'"), FText::FromString(CharLabel(C))));
			return;
		}
		if (C != 0)
		{
			FString Label;
			int32 Units = 1;
			const uint32 CodePoint = CodePointAt(Lines[Line], Col, Label, Units);
			Add(Line, Col, Col + Units, FText::Format(LOCTEXT("Lang.SyntaxInvalidChar", "invalid character '{0}' (U+{1})"),
				FText::FromString(Label), FText::FromString(FString::Printf(TEXT("%04X"), CodePoint))));
		}
	}

	void FChecker::Run()
	{
		const int32 LineCount = static_cast<int32>(Lines.size());
		// 1. Error tokens and brackets, in text order. Brackets inside an f-string's replacement field balance within the
		//    field (the lexer marks a stray closer there as an error token), so they are left out of the stack.
		TArray<FOpen> Stack;
		int32 TokenErrors = 0;
		for (int32 Line = 0; Line < LineCount && !Full(); ++Line)
		{
			const std::vector<uint32_t>& Toks = Tokens[Line];
			int32 FieldDepth = 0;
			int32 SeenDelta = 0;        // the line's brackets the stack saw (a long line's tail has no tokens)
			for (size_t Index = 0; Index < Toks.size() && !Full(); ++Index)
			{
				const TokenKind Kind = dream::TokenKindOf(Toks[Index]);
				const int32 Col = dream::TokenStart(Toks[Index]);
				const char16_t C = CharAt(Line, Col);
				if (Kind == TokenKind::Error)
				{
					CheckErrorToken(Line, Index);
					++TokenErrors;
					continue;
				}
				if (Kind == TokenKind::FStringBrace)
				{
					FieldDepth = C == u'{' ? FieldDepth + 1 : FMath::Max(0, FieldDepth - 1);
					continue;
				}
				if (Kind != TokenKind::Bracket || FieldDepth > 0)
				{
					continue;
				}
				if (C == u'(' || C == u'[' || C == u'{')
				{
					Stack.Add(FOpen{ C, Line, Col });
					++SeenDelta;
					continue;
				}
				--SeenDelta;
				if (Stack.IsEmpty())
				{
					Add(Line, Col, Col + 1, FText::Format(LOCTEXT("Lang.SyntaxUnmatched", "unmatched '{0}'"), FText::FromString(CharLabel(C))));
					continue;
				}
				const FOpen Open = Stack.Pop();
				if (Open.Char != 0 && Matching(Open.Char) != C)
				{
					const FText Message = Open.Line == Line
						? FText::Format(LOCTEXT("Lang.SyntaxMismatch", "closing parenthesis '{0}' does not match opening parenthesis '{1}'"),
							FText::FromString(CharLabel(C)), FText::FromString(CharLabel(Open.Char)))
						: FText::Format(LOCTEXT("Lang.SyntaxMismatchLine", "closing parenthesis '{0}' does not match opening parenthesis '{1}' on line {2}"),
							FText::FromString(CharLabel(C)), FText::FromString(CharLabel(Open.Char)), FText::AsNumber(Open.Line + 1));
					Add(Line, Col, Col + 1, Message);
				}
			}
			// (review lang-q1-M1) Past column 20,000 the lexer keeps no tokens (§4.3.1 long lines) but still counts the line's
			// brackets in its facts: the tail settles the stack with placeholders, which never report anything themselves.
			if (LineLen(Line) > dream::LongLineColumns)
			{
				bLongLines = true;
				for (int32 Tail = Facts[Line].BracketDelta - SeenDelta; Tail != 0; Tail += Tail > 0 ? -1 : 1)
				{
					if (Tail > 0)
					{
						Stack.Add(FOpen{ 0, Line, dream::LongLineColumns });
					}
					else if (!Stack.IsEmpty())
					{
						Stack.Pop();
					}
				}
			}
		}
		// CPython stops at the first error: once the tokens are broken (an unterminated string swallowed a ')'), an opener
		// that looks unclosed is only a consequence of it (review lang-q1-m3).
		for (const FOpen& Open : Stack)
		{
			if (Open.Char == 0 || TokenErrors > 0)
			{
				continue;
			}
			Add(Open.Line, Open.Column, Open.Column + 1, FText::Format(LOCTEXT("Lang.SyntaxNeverClosed", "'{0}' was never closed"),
				FText::FromString(CharLabel(Open.Char))));
		}
		// 2. An unterminated triple-quoted string: the text ends inside it; the item sits where it starts.
		if (EndInfo.bInString && EndInfo.bTriple)
		{
			int32 StartLine = LineCount - 1;
			while (StartLine > 0 && Facts[StartLine].Has(dream::LF_StartsInString))
			{
				--StartLine;
			}
			int32 StartCol = 0;
			for (const uint32_t Tok : Tokens[StartLine])
			{
				if (IsStringish(dream::TokenKindOf(Tok)) && (dream::TokenKindOf(Tok) == TokenKind::String || dream::TokenKindOf(Tok) == TokenKind::Docstring))
				{
					StartCol = dream::TokenStart(Tok);   // the last literal that starts on the line is the open one
				}
			}
			Add(StartLine, StartCol, LineLen(StartLine), FText::Format(
				LOCTEXT("Lang.SyntaxUnterminatedTriple", "unterminated triple-quoted string literal (detected at line {0})"), FText::AsNumber(LineCount)));
		}
		// 3. Statements (missing ':' and indentation) only when the tokens are sound: with a bracket or token error the logical
		//    lines are unknown, and CPython reports the first error alone anyway.
		if (Items.IsEmpty() && !bLongLines)     // a long line's ':' may sit in its token-less tail
		{
			CheckStatements();
		}
		Items.Sort([](const FDreamProblem& A, const FDreamProblem& B)
		{
			return A.StartLine != B.StartLine ? A.StartLine < B.StartLine : A.StartColumn < B.StartColumn;
		});
	}

	void FChecker::CheckStatements()
	{
		const int32 LineCount = static_cast<int32>(Lines.size());
		TArray<int32> Indents;
		Indents.Add(0);
		bool bPendingBlock = false;     // the last logical line ended with ':' (a block must follow)
		int32 BlockLine = 0;
		FString BlockWhat;
		int32 Depth = 0;                // brackets open at the start of the physical line
		bool bIndentError = false;
		int32 Line = 0;
		while (Line < LineCount && !Full())
		{
			const dream::LineFacts& First = Facts[Line];
			const bool bLogicalStart = Depth == 0 && !First.Has(dream::LF_StartsInString) && !First.Has(dream::LF_StartsContinuation);
			if (!bLogicalStart || First.Has(dream::LF_Blank) || First.Has(dream::LF_CommentOnly))
			{
				Depth = FMath::Max(0, Depth + First.BracketDelta);
				++Line;
				continue;
			}
			// The logical line: from Line to the physical line where the brackets close and no string or join continues.
			int32 Last = Line;
			int32 D = FMath::Max(0, Depth + Facts[Last].BracketDelta);
			while (Last + 1 < LineCount && (D > 0 || Facts[Last].Has(dream::LF_EndsInString) || Facts[Last].Has(dream::LF_EndsWithBackslash)))
			{
				++Last;
				D = FMath::Max(0, D + Facts[Last].BracketDelta);
			}
			const int32 Indent = First.IndentCols;
			const int32 FirstCol = First.FirstTokenCol >= 0 ? First.FirstTokenCol : 0;

			// Indentation (the tokenizer's INDENT / DEDENT rules); after the first error the rest would only echo it.
			if (!bIndentError)
			{
				if (bPendingBlock)
				{
					if (Indent <= Indents.Last())
					{
						Add(Line, FirstCol, FMath::Max(FirstCol + 1, LineLen(Line)), FText::Format(
							LOCTEXT("Lang.SyntaxExpectedBlock", "expected an indented block after {0} on line {1}"),
							FText::FromString(BlockWhat), FText::AsNumber(BlockLine + 1)));
						bIndentError = true;
					}
					else
					{
						Indents.Add(Indent);
					}
				}
				else if (Indent > Indents.Last())
				{
					Add(Line, 0, FMath::Max(1, FirstCol), LOCTEXT("Lang.SyntaxUnexpectedIndent", "unexpected indent"));
					bIndentError = true;
				}
				else if (Indent < Indents.Last())
				{
					while (Indents.Num() > 1 && Indents.Last() > Indent)
					{
						Indents.Pop();
					}
					if (Indents.Last() != Indent)
					{
						Add(Line, 0, FMath::Max(1, FirstCol), LOCTEXT("Lang.SyntaxUnindent", "unindent does not match any outer indentation level"));
						bIndentError = true;
					}
				}
			}

			// The header keyword (async def / for / with count as their keyword) and whether a ':' closes the header.
			size_t FirstIndex = 0;
			const std::vector<uint32_t>& FirstToks = Tokens[Line];
			while (FirstIndex < FirstToks.size() && dream::TokenStart(FirstToks[FirstIndex]) < FirstCol)
			{
				++FirstIndex;
			}
			std::u16string_view Keyword;
			if (FirstIndex < FirstToks.size() && dream::TokenKindOf(FirstToks[FirstIndex]) == TokenKind::Keyword)
			{
				Keyword = TokenText(Line, FirstIndex);
				if (Keyword == u"async" && FirstIndex + 1 < FirstToks.size())
				{
					size_t NextIndex = FirstIndex + 1;
					while (NextIndex < FirstToks.size() && dream::TokenKindOf(FirstToks[NextIndex]) == TokenKind::Text)
					{
						++NextIndex;
					}
					Keyword = NextIndex < FirstToks.size() ? TokenText(Line, NextIndex) : std::u16string_view();
				}
			}
			const bool bHeader = !Keyword.empty() && IsCompoundKeyword(Keyword);
			bool bColon = false;
			int32 LambdaCount = 0;
			int32 LastSigLine = Line;
			int32 LastSigEnd = FirstCol + 1;
			int32 Inner = 0;
			for (int32 L = Line; L <= Last; ++L)
			{
				const std::vector<uint32_t>& Toks = Tokens[L];
				for (size_t Index = 0; Index < Toks.size(); ++Index)
				{
					const TokenKind Kind = dream::TokenKindOf(Toks[Index]);
					if (Kind == TokenKind::Comment || Kind == TokenKind::CellMarker || Kind == TokenKind::RegionMarker)
					{
						break;
					}
					const std::u16string_view Tok = TokenText(L, Index);
					if (Kind != TokenKind::Text || Tok.find_first_not_of(u" \t\f") != std::u16string_view::npos)
					{
						LastSigLine = L;     // identifiers are Text tokens too; only blanks are not significant
						LastSigEnd = TokenEnd(L, Index);
					}
					if (Kind == TokenKind::Bracket && !Tok.empty())
					{
						Inner += (Tok[0] == u'(' || Tok[0] == u'[' || Tok[0] == u'{') ? 1 : -1;
						continue;
					}
					if (Inner != 0)
					{
						continue;
					}
					if (Kind == TokenKind::Keyword && Tok == u"lambda")
					{
						++LambdaCount;
					}
					else if (Kind == TokenKind::Operator && !Tok.empty() && Tok[0] == u':' && (Tok.size() < 2 || Tok[1] != u'='))
					{
						if (LambdaCount > 0)
						{
							--LambdaCount;
						}
						else
						{
							bColon = true;
						}
					}
				}
			}
			// Trailing blanks are part of the last token's span; the mark goes right after the last visible character.
			while (LastSigEnd > 0 && LastSigEnd <= LineLen(LastSigLine) && (CharAt(LastSigLine, LastSigEnd - 1) == u' ' || CharAt(LastSigLine, LastSigEnd - 1) == u'\t'))
			{
				--LastSigEnd;
			}
			if (bHeader && !bColon)
			{
				Add(LastSigLine, FMath::Max(0, LastSigEnd - 1), LastSigEnd, LOCTEXT("Lang.SyntaxExpectedColon", "expected ':'"));
			}
			const bool bEndsWithColon = Facts[Last].Has(dream::LF_EndsWithColon);
			// A header without its ':' still opens a block, so the indented body is not reported again as an unexpected indent
			// (CPython stops at the first error).
			bPendingBlock = bEndsWithColon || (bHeader && !bColon);
			if (bPendingBlock)
			{
				BlockLine = Line;
				const FString Word = DreamText::ToFString(Keyword);
				BlockWhat = Word == TEXT("def") ? LOCTEXT("Lang.SyntaxFunctionDef", "function definition").ToString()
					: Word == TEXT("class") ? LOCTEXT("Lang.SyntaxClassDef", "class definition").ToString()
					: Word.IsEmpty() ? LOCTEXT("Lang.SyntaxStatement", "the ':'").ToString()
					: FText::Format(LOCTEXT("Lang.SyntaxKeywordStatement", "'{0}' statement"), FText::FromString(Word)).ToString();
			}
			Depth = 0;
			Line = Last + 1;
		}
		if (bPendingBlock && !bIndentError)
		{
			int32 End = FMath::Max(0, LineCount - 1);
			while (End > 0 && (Facts[End].Has(dream::LF_Blank) || Facts[End].Has(dream::LF_CommentOnly)))
			{
				--End;      // the mark goes after the header's text, not on the empty last line
			}
			Add(End, LineLen(End), LineLen(End) + 1, FText::Format(LOCTEXT("Lang.SyntaxExpectedBlockEof", "expected an indented block after {0} on line {1}"),
				FText::FromString(BlockWhat), FText::AsNumber(BlockLine + 1)));
		}
	}

	TArray<FDreamProblem> CheckU16(std::u16string_view Text, uint64 Version)
	{
		FChecker Checker;
		Checker.Text = Text;
		Checker.Version = Version;
		size_t Start = 0;
		for (size_t Index = 0; Index <= Text.size(); ++Index)
		{
			if (Index == Text.size() || Text[Index] == u'\n')
			{
				Checker.Lines.push_back(Text.substr(Start, Index - Start));
				Start = Index + 1;
			}
		}
		dream::LexStateId EndState = dream::NormalState;
		dream::PyLexer::LexText(Text, dream::LexGrammar::Py311, Checker.Tokens, Checker.Facts, EndState, Checker.EndInfo);
		if (Checker.Tokens.size() < Checker.Lines.size() || Checker.Facts.size() < Checker.Lines.size())
		{
			return {};      // never expected: the lexer gives one record per line
		}
		Checker.Run();
		return MoveTemp(Checker.Items);
	}
}

bool FDreamSyntaxCheck::Check(const dream::TextSnapshot& Snapshot, TArray<FDreamProblem>& Out)
{
	Out.Reset();
	if (Snapshot.Length() > MaxUnits)
	{
		return false;   // not checked: no verdict (review lang-q1-m2)
	}
	const std::u16string Text = Snapshot.Text();
	Out = DreamSyntaxCheckLocal::CheckU16(Text, Snapshot.Version());
	return true;
}

TArray<FDreamProblem> FDreamSyntaxCheck::CheckText(const FString& Text)
{
	FString Normalized = Text.Replace(TEXT("\r\n"), TEXT("\n"));
	return DreamSyntaxCheckLocal::CheckU16(DreamText::ToU16(Normalized), 0);
}

#undef LOCTEXT_NAMESPACE
