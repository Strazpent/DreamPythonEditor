// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::PyLexer (TDD §4.3): token kinds (T-LEX-002, T-LEX-004, T-LEX-005 shapes), line facts and states
// (T-LEX-006), long lines and bad input (T-LEX-009), and incremental against full relex on random edits (T-LEX-007;
// DREAM_FULL_SCALE=1 runs 100k edits per grammar), with the frontier left stale across edits as idle slicing leaves it.
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "GapArray.h"
#include "dream/PyLexer.h"

#include <cstdlib>
#include <string>
#include <vector>

using dream::LexGrammar;
using dream::LineFacts;
using dream::PyLexer;
using dream::TokenKind;
using dream_test_util::Rng;
using dream_test_util::U;

namespace
{
	bool FullScale()
	{
		const char* V = std::getenv("DREAM_FULL_SCALE");
		return V != nullptr && *V != '\0' && *V != '0';
	}

	struct Lexed
	{
		std::vector<std::vector<uint32_t>> Tokens;
		std::vector<LineFacts> Facts;
		dream::LexStateInfo End;
	};

	Lexed Lex(std::u16string_view Text, LexGrammar G = LexGrammar::Py311)
	{
		Lexed L;
		dream::LexStateId Id = 0;
		PyLexer::LexText(Text, G, L.Tokens, L.Facts, Id, L.End);
		return L;
	}

	TokenKind KindAt(const std::vector<uint32_t>& Toks, int32_t Col)
	{
		TokenKind K = TokenKind::Text;
		for (uint32_t T : Toks)
		{
			if (dream::TokenStart(T) > Col)
			{
				break;
			}
			K = dream::TokenKindOf(T);
		}
		return K;
	}

	/** Kind of the token at the column where Needle starts on line 0 (the first occurrence). */
	TokenKind KindOf(std::u16string_view Line, std::u16string_view Needle, LexGrammar G = LexGrammar::Py311, size_t From = 0)
	{
		const Lexed L = Lex(Line, G);
		const size_t At = Line.find(Needle, From);
		return At == std::u16string_view::npos ? TokenKind::Count : KindAt(L.Tokens[0], static_cast<int32_t>(At));
	}

	bool WellFormed(const std::vector<uint32_t>& Toks, size_t LineLen)
	{
		if (LineLen == 0)
		{
			return Toks.empty();
		}
		if (Toks.empty() || dream::TokenStart(Toks[0]) != 0)
		{
			return false;
		}
		for (size_t I = 1; I < Toks.size(); ++I)
		{
			if (dream::TokenStart(Toks[I]) <= dream::TokenStart(Toks[I - 1]))
			{
				return false;
			}
		}
		return dream::TokenStart(Toks.back()) < static_cast<int32_t>(std::max<size_t>(LineLen, 1))
			&& dream::TokenStart(Toks.back()) <= dream::LongLineColumns;
	}
}

TEST(Lexer_KeywordsNamesAndDefinitions)
{
	const std::u16string L = u"def foo(self, x=None, *a):  # note";
	CHECK(KindOf(L, u"def") == TokenKind::Keyword);
	CHECK(KindOf(L, u"foo") == TokenKind::FunctionName);
	CHECK(KindOf(L, u"(") == TokenKind::Bracket);
	CHECK(KindOf(L, u"self") == TokenKind::SelfParam);
	CHECK(KindOf(L, u"x=") == TokenKind::Text);
	CHECK(KindOf(L, u"=") == TokenKind::Operator);
	CHECK(KindOf(L, u"None") == TokenKind::KeywordConstant);
	CHECK(KindOf(L, u"*") == TokenKind::Operator);
	CHECK(KindOf(L, u"# note") == TokenKind::Comment);
	const std::u16string C = u"class Base(Exception): print(len(x)); x.print; cls";
	CHECK(KindOf(C, u"Base") == TokenKind::ClassName);
	CHECK(KindOf(C, u"Exception") == TokenKind::Builtin);
	CHECK(KindOf(C, u"print") == TokenKind::Builtin);
	CHECK(KindOf(C, u"len") == TokenKind::Builtin);
	CHECK(KindOf(C, u"print", LexGrammar::Py311, C.find(u"x.") + 2) == TokenKind::Text);   // attribute, not the builtin
	CHECK(KindOf(C, u"cls") == TokenKind::SelfParam);
	CHECK(KindOf(u"async def g(): await x", u"g") == TokenKind::FunctionName);
	CHECK(KindOf(u"x = True and not False", u"and") == TokenKind::Keyword);
	CHECK(PyLexer::IsKeyword(u"nonlocal"));
	CHECK(!PyLexer::IsKeyword(u"match"));
	CHECK(PyLexer::IsBuiltin(u"ZeroDivisionError"));
	CHECK(PyLexer::IsBuiltin(u"__import__"));
	CHECK(!PyLexer::IsBuiltin(u"self"));
}

TEST(Lexer_Numbers)
{
	struct Case
	{
		const char16_t* Text;
		TokenKind Kind;
	};
	const Case Cases[] = {
		{u"0x1F", TokenKind::Number}, {u"0o17", TokenKind::Number}, {u"0b101", TokenKind::Number}, {u"1_000", TokenKind::Number},
		{u"3.14", TokenKind::Number}, {u"1e10", TokenKind::Number}, {u"2j", TokenKind::Number}, {u".5", TokenKind::Number},
		{u"5.", TokenKind::Number}, {u"1E-3J", TokenKind::Number}, {u"00", TokenKind::Number}, {u"01.5", TokenKind::Number},
		{u"0x_1f", TokenKind::Number}, {u"1__0", TokenKind::Error}, {u"1_", TokenKind::Error}, {u"0_7", TokenKind::Error},
		{u"012", TokenKind::Error}, {u"1abc", TokenKind::Error}, {u"0x", TokenKind::Error}, {u"0b12", TokenKind::Error},
		{u"1e", TokenKind::Error}, {u"1e+", TokenKind::Error},
	};
	for (const Case& C : Cases)
	{
		const std::u16string Line = std::u16string(u"x = ") + C.Text;
		const Lexed L = Lex(Line);
		CHECK(KindAt(L.Tokens[0], 4) == C.Kind);
		CHECK(WellFormed(L.Tokens[0], Line.size()));
	}
	// "1if" is a number then a keyword in 3.11 (a DeprecationWarning, not an error).
	CHECK(KindOf(u"x = 1if y else 2", u"1") == TokenKind::Number);
	CHECK(KindOf(u"x = 1if y else 2", u"if") == TokenKind::Keyword);
}

TEST(Lexer_StringsPrefixesAndEscapes)
{
	CHECK(KindOf(u"x = 'a'", u"'a'") == TokenKind::String);
	CHECK(KindOf(u"x = rb\"y\"", u"rb") == TokenKind::String);
	CHECK(KindOf(u"x = U'z'", u"U'") == TokenKind::String);
	CHECK(KindOf(u"x = ur'w'", u"ur") == TokenKind::Text);    // invalid prefix: NAME + STRING, as in CPython
	CHECK(KindOf(u"x = ur'w'", u"'w'") == TokenKind::String);
	const std::u16string E = u"s = 'a\\nb\\x4' + '\\N{DASH}' + b'\\N{X}' + '\\q'";
	CHECK(KindOf(E, u"\\n") == TokenKind::StringEscape);
	CHECK(KindOf(E, u"\\x4") == TokenKind::Error);           // truncated \xXX escape
	CHECK(KindOf(E, u"\\N{DASH}") == TokenKind::StringEscape);
	CHECK(KindOf(E, u"\\N{X}") == TokenKind::String);        // bytes have no \N escape
	CHECK(KindOf(E, u"\\q") == TokenKind::String);           // not an escape
	CHECK(KindOf(u"r = r'\\n\\''", u"\\n") == TokenKind::String);
	const Lexed Unterminated = Lex(u"x = 'abc\ny = 1");
	CHECK(KindAt(Unterminated.Tokens[0], 4) == TokenKind::Error);
	CHECK(!Unterminated.Facts[1].Has(dream::LF_StartsInString));
	const Lexed Joined = Lex(u"x = 'abc\\\ndef'\ny");
	CHECK(Joined.Facts[1].Has(dream::LF_StartsInString));
	CHECK(KindAt(Joined.Tokens[1], 0) == TokenKind::String);
	CHECK(KindAt(Joined.Tokens[2], 0) == TokenKind::Text);
}

TEST(Lexer_FStringsPy311)
{
	const std::u16string L = u"f'{x!r:>{w}}'";
	const Lexed X = Lex(L);
	CHECK(KindAt(X.Tokens[0], 0) == TokenKind::String);
	CHECK(KindAt(X.Tokens[0], 2) == TokenKind::FStringBrace);
	CHECK(KindAt(X.Tokens[0], 3) == TokenKind::Text);
	CHECK(KindAt(X.Tokens[0], 4) == TokenKind::FStringConversion);
	CHECK(KindAt(X.Tokens[0], 6) == TokenKind::Operator);
	CHECK(KindAt(X.Tokens[0], 7) == TokenKind::FStringSpec);
	CHECK(KindAt(X.Tokens[0], 8) == TokenKind::FStringBrace);
	CHECK(KindAt(X.Tokens[0], 9) == TokenKind::Text);
	CHECK(KindAt(X.Tokens[0], 10) == TokenKind::FStringBrace);
	CHECK(KindAt(X.Tokens[0], 11) == TokenKind::FStringBrace);
	CHECK(KindAt(X.Tokens[0], 12) == TokenKind::String);
	CHECK(KindOf(u"f'{{x}}'", u"x") == TokenKind::String);
	CHECK(KindOf(u"f'a}b'", u"}") == TokenKind::Error);                  // single '}' is not allowed
	CHECK(KindOf(u"f'{x # c}'", u"#") == TokenKind::Error);              // '#' in a 3.11 field
	CHECK(KindOf(u"f'{\"a\"}'", u"\"a\"") == TokenKind::String);          // another quote nests
	CHECK(KindOf(u"f'{len(x)}'", u"len") == TokenKind::Builtin);
	CHECK(KindOf(u"f'{x:{y:{z}}}'", u"{z") == TokenKind::Error);         // nested too deeply
	CHECK(KindOf(u"f'{x!=y}'", u"!=") == TokenKind::Operator);
	// The enclosing quote ends the f-string in 3.11; the open field becomes Error.
	const Lexed Same = Lex(u"f'{'a'}' + 1");
	CHECK(KindAt(Same.Tokens[0], 2) == TokenKind::Error);
	CHECK(KindAt(Same.Tokens[0], 3) == TokenKind::String);
	// A field spans lines inside a triple-quoted f-string.
	const Lexed Multi = Lex(u"f'''{\n  x + 1\n}'''\nz");
	CHECK(Multi.Facts[1].Has(dream::LF_StartsInString));
	CHECK(KindAt(Multi.Tokens[1], 4) == TokenKind::Operator);
	CHECK(KindAt(Multi.Tokens[2], 0) == TokenKind::FStringBrace);
	CHECK(!Multi.Facts[3].Has(dream::LF_StartsInString));
	// ...but not inside a single-quoted one.
	const Lexed Broken = Lex(u"f'{x\ny'");
	CHECK(KindAt(Broken.Tokens[0], 0) == TokenKind::Error);
	CHECK(!Broken.Facts[1].Has(dream::LF_StartsInString));
}

TEST(Lexer_FStringsPy312Plus)
{
	const LexGrammar G = LexGrammar::Py312Plus;
	const Lexed Same = Lex(u"f'{'a'}' + 1", G);
	CHECK(KindAt(Same.Tokens[0], 2) == TokenKind::FStringBrace);
	CHECK(KindAt(Same.Tokens[0], 3) == TokenKind::String);
	CHECK(KindAt(Same.Tokens[0], 6) == TokenKind::FStringBrace);
	CHECK(KindAt(Same.Tokens[0], 9) == TokenKind::Operator);
	CHECK(KindOf(u"f'{x # c\n}'", u"# c", G) == TokenKind::Comment);
	const Lexed Multi = Lex(u"f'{x +\n 1}'\nz", G);
	CHECK(Multi.Facts[1].Has(dream::LF_StartsInString));
	CHECK(!Multi.Facts[2].Has(dream::LF_StartsInString));
	// Depth 8 lexes; depth 9 is Error without a crash (T-LEX-003).
	std::u16string Depth8;
	std::u16string Depth9;
	for (int I = 0; I < 8; ++I)
	{
		Depth8 += u"f'{";
	}
	Depth8 += u"x";
	for (int I = 0; I < 8; ++I)
	{
		Depth8 += u"}'";
	}
	for (int I = 0; I < 9; ++I)
	{
		Depth9 += u"f'{";
	}
	const Lexed D8 = Lex(Depth8, G);
	const Lexed D9 = Lex(Depth9 + u"x", G);
	CHECK(KindAt(D8.Tokens[0], static_cast<int32_t>(Depth8.find(u'x'))) == TokenKind::Text);
	CHECK(KindAt(D9.Tokens[0], 24) == TokenKind::Error);
	CHECK(!D9.End.bInString);
}

TEST(Lexer_DocstringsCommentsAndMarkers)
{
	const Lexed L = Lex(u"\"\"\"Module doc.\n\nMore.\"\"\"\ndef f():\n    \"\"\"Doc.\"\"\"\n    'x'.join(y)\n    return 1\n# %% Cell two\n  #region tools\nx = 1  # %%\n#endregion\n");
	CHECK(KindAt(L.Tokens[0], 0) == TokenKind::Docstring);
	CHECK(KindAt(L.Tokens[2], 0) == TokenKind::Docstring);
	CHECK(KindAt(L.Tokens[4], 4) == TokenKind::Docstring);
	CHECK(KindAt(L.Tokens[5], 4) == TokenKind::String);                 // a string followed by code
	CHECK(KindAt(L.Tokens[7], 0) == TokenKind::CellMarker);
	CHECK(L.Facts[7].Has(dream::LF_CellMarker));
	CHECK(L.Facts[7].Has(dream::LF_CommentOnly));
	CHECK(KindAt(L.Tokens[8], 2) == TokenKind::RegionMarker);
	CHECK(L.Facts[8].Has(dream::LF_Region));
	CHECK(KindAt(L.Tokens[9], 7) == TokenKind::Comment);
	CHECK(KindAt(L.Tokens[10], 0) == TokenKind::RegionMarker);
	// "# %%" inside a string is not a header (§4.5 cells).
	const Lexed S = Lex(u"s = \"\"\"\n# %% not a cell\n\"\"\"");
	CHECK(KindAt(S.Tokens[1], 0) == TokenKind::String);
	CHECK(!S.Facts[1].Has(dream::LF_CellMarker));
	CHECK(!S.Facts[1].Has(dream::LF_CommentOnly));
}

TEST(Lexer_DecoratorsAndSoftKeywords)
{
	const std::u16string D = u"@app.route(\"/x\")";
	CHECK(KindOf(D, u"@") == TokenKind::Decorator);
	CHECK(KindOf(D, u"app") == TokenKind::Decorator);
	CHECK(KindOf(D, u".") == TokenKind::Decorator);
	CHECK(KindOf(D, u"route") == TokenKind::Decorator);
	CHECK(KindOf(D, u"(") == TokenKind::Bracket);
	CHECK(Lex(D).Facts[0].Has(dream::LF_Decorator));
	CHECK(KindOf(u"x = a @ b", u"@") == TokenKind::Operator);
	CHECK(KindOf(u"match command.split():", u"match") == TokenKind::SoftKeyword);
	CHECK(KindOf(u"    case [x, *rest] if x > 0:", u"case") == TokenKind::SoftKeyword);
	CHECK(KindOf(u"    case _:", u"_") == TokenKind::SoftKeyword);
	CHECK(KindOf(u"match = 1", u"match") == TokenKind::Text);
	CHECK(KindOf(u"match.group(1)", u"match") == TokenKind::Text);
	CHECK(KindOf(u"match(x)", u"match") == TokenKind::Text);              // no ':' at the end
	CHECK(KindOf(u"case(1)", u"case") == TokenKind::Text);
	CHECK(KindOf(u"for case in items:", u"case") == TokenKind::Text);
	CHECK(KindOf(u"print(_)", u"_") == TokenKind::Text);
}

TEST(Lexer_LineFacts)
{
	const Lexed L = Lex(u"\t  x = foo(a, [b\n  \tc]):  # ends: here\nif x: pass\n\n   # only\ny = 1 + \\\n  2\n@dec\nz = {1:\n    }\n");
	CHECK_EQ(L.Facts[0].IndentCols, 10);
	CHECK_EQ(L.Facts[0].BracketDelta, 2);
	CHECK_EQ(L.Facts[0].FirstTokenCol, 3);
	CHECK(!L.Facts[0].Has(dream::LF_EndsWithColon));
	CHECK_EQ(L.Facts[1].IndentCols, 8);
	CHECK_EQ(L.Facts[1].BracketDelta, -2);
	CHECK(L.Facts[1].Has(dream::LF_EndsWithColon));                       // ':' at line depth <= 0, comment ignored
	CHECK(!L.Facts[2].Has(dream::LF_EndsWithColon));
	CHECK(L.Facts[3].Has(dream::LF_Blank));
	CHECK_EQ(L.Facts[3].FirstTokenCol, -1);
	CHECK(L.Facts[4].Has(dream::LF_CommentOnly));
	CHECK(L.Facts[4].FirstTokenKind == TokenKind::Comment);
	CHECK(L.Facts[5].Has(dream::LF_EndsWithBackslash));
	CHECK(L.Facts[6].Has(dream::LF_StartsContinuation));
	CHECK(L.Facts[7].Has(dream::LF_Decorator));
	CHECK(!L.Facts[8].Has(dream::LF_EndsWithColon));                     // the ':' is inside the dict on this line
	CHECK(!L.End.bInString);
	const Lexed Open = Lex(u"s = \"\"\"abc");
	CHECK(Open.End.bInString);
	CHECK(Open.End.bTriple);
	CHECK(Open.Facts[0].Has(dream::LF_EndsInString));
	const Lexed Fs = Lex(u"s = f'''{x");
	CHECK(Fs.End.bFString);
	CHECK_EQ(Fs.End.FrameDepth, 1);
	const Lexed Bs = Lex(u"x = 1 + \\");
	CHECK(Bs.End.bBackslashJoin);
}

TEST(Lexer_LongLinesAndBadInput)
{
	std::u16string Long = u"x = '";
	Long.append(30000, u'a');
	Long += u"' + 1";
	const Lexed L = Lex(Long + u"\ny = 2");
	CHECK(WellFormed(L.Tokens[0], Long.size()));
	CHECK_EQ(dream::TokenStart(L.Tokens[0].back()), dream::LongLineColumns);
	CHECK(dream::TokenKindOf(L.Tokens[0].back()) == TokenKind::Text);
	CHECK(!L.Facts[1].Has(dream::LF_StartsInString));                    // the state is still exact past the cut
	CHECK(KindAt(L.Tokens[1], 4) == TokenKind::Number);
	std::u16string Bad = u"a = ";
	Bad.push_back(static_cast<char16_t>(0xD800));
	Bad += u" $ ? ` ! ";
	Bad.push_back(0);
	Bad.push_back(static_cast<char16_t>(0xDC00));
	Bad += u" \u20AC";
	const Lexed B = Lex(Bad);
	CHECK(WellFormed(B.Tokens[0], Bad.size()));
	CHECK(KindAt(B.Tokens[0], 4) == TokenKind::Error);
	CHECK(KindAt(B.Tokens[0], 6) == TokenKind::Error);
	CHECK(KindAt(B.Tokens[0], static_cast<int32_t>(Bad.size()) - 1) == TokenKind::Error);
	CHECK(KindOf(u"x = 1 \\ y", u"\\") == TokenKind::Error);              // a backslash not at the line end
	CHECK(KindOf(U("\xC3\xA9t\xC3\xA9 = 1"), U("\xC3\xA9t\xC3\xA9")) == TokenKind::Text);
}

namespace
{
	/** A record that counts move assignments onto itself (MSVC's vector ignores a self-move; libc++ empties it). */
	struct SelfMoveProbe
	{
		int Value = -1;
		static int& SelfMoves()
		{
			static int Count = 0;
			return Count;
		}

		SelfMoveProbe() = default;
		explicit SelfMoveProbe(int V) : Value(V) {}
		SelfMoveProbe(const SelfMoveProbe&) = default;
		SelfMoveProbe(SelfMoveProbe&&) noexcept = default;
		SelfMoveProbe& operator=(const SelfMoveProbe&) = default;
		SelfMoveProbe& operator=(SelfMoveProbe&& Other) noexcept
		{
			if (this == &Other)
			{
				++SelfMoves();
			}
			Value = Other.Value;
			return *this;
		}
	};
}

TEST(Lexer_GapArrayNeverSelfMoves)   // review r2: the direct test of core-text-M2 (fails here with the guard removed)
{
	dream::lexer_detail::GapArray<SelfMoveProbe> A;
	std::vector<int> Oracle;
	A.resize(100);
	for (int I = 0; I < 100; ++I)
	{
		A[static_cast<size_t>(I)].Value = I;
		Oracle.push_back(I);
	}
	SelfMoveProbe::SelfMoves() = 0;
	int Next = 1000;
	const auto Splice = [&](size_t At, size_t Erase, size_t Insert) {
		A.Splice(At, Erase, Insert);
		Oracle.erase(Oracle.begin() + static_cast<std::ptrdiff_t>(At), Oracle.begin() + static_cast<std::ptrdiff_t>(At + Erase));
		Oracle.insert(Oracle.begin() + static_cast<std::ptrdiff_t>(At), Insert, -1);
		for (size_t K = 0; K < Insert; ++K)
		{
			A[At + K].Value = Next;
			Oracle[At + K] = Next++;
		}
	};
	const auto Same = [&] {
		bool bSame = A.size() == Oracle.size();
		for (size_t K = 0; bSame && K < Oracle.size(); ++K)
		{
			bSame = A[K].Value == Oracle[K];
		}
		return bSame;
	};
	Splice(50, 0, 1);    // the gap is empty after resize: moving it to 50 must not move records 0..49 onto themselves
	CHECK(Same());
	Splice(20, 2, 3);    // left, through a gap
	CHECK(Same());
	Splice(80, 4, 0);    // right
	CHECK(Same());
	Splice(80, 0, 0);    // in place
	CHECK(Same());
	A.resize(10);        // empty again
	Oracle.assign(10, -1);
	for (int I = 0; I < 10; ++I)
	{
		A[static_cast<size_t>(I)].Value = I;
		Oracle[static_cast<size_t>(I)] = I;
	}
	Splice(3, 1, 1);
	CHECK(Same());
	CHECK_EQ(SelfMoveProbe::SelfMoves(), 0);
}

TEST(Lexer_SplicesKeepCleanTokens)   // review r1 core-text-M2: a splice through an empty gap moves no record
{
	// After Reset the line array's gap is empty; the first line-count change splices there (and an insert that uses
	// up the gap empties it again). The clean lines above the edit must keep their tokens: they are never relexed.
	std::u16string Text;
	for (int I = 0; I < 1000; ++I)
	{
		Text += (I % 3 == 0) ? u"def f(x):  # c\n" : u"    return x + 'a'\n";
	}
	dream::TextBuffer B(Text);
	PyLexer Lx;
	Lx.Reset(B.Snapshot());
	Lx.Relex(B.Snapshot(), 0.0, 0, 0);
	const auto Check = [&] {
		std::vector<std::vector<uint32_t>> Full;
		PyLexer::LexFull(B.Snapshot(), LexGrammar::Py311, Full);
		bool bSame = Full.size() == static_cast<size_t>(Lx.LineCount());
		for (dream::LineNo L = 0; bSame && L < Lx.LineCount(); ++L)
		{
			const auto T = Lx.Tokens(L);
			bSame = std::vector<uint32_t>(T.begin(), T.end()) == Full[static_cast<size_t>(L)];
		}
		return bSame;
	};
	const auto Edit = [&](dream::LineNo Line, std::u16string Ins, int64_t Del) {
		dream::EditBatch E;
		E.Ranges.push_back(dream::EditRange{B.Snapshot().LineStart(Line), Del, std::move(Ins)});
		Lx.OnChange(B.Apply(E), B.Snapshot());
		CHECK(Lx.Frontier() <= Line);
		Lx.Relex(B.Snapshot(), 0.0, 0, 0);
	};
	Edit(500, u"x = 1\n", 0);          // the first splice: the gap is empty, records 0..500 must not move onto themselves
	CHECK(Check());
	Edit(200, u"y = 2\nz = 3\n", 0);   // the gap moves left
	CHECK(Check());
	Edit(800, u"", static_cast<int64_t>(B.Snapshot().LineStart(803) - B.Snapshot().LineStart(800)));   // and right, deleting
	CHECK(Check());
	Lx.Reset(B.Snapshot());           // an empty gap again
	Lx.Relex(B.Snapshot(), 0.0, 0, 0);
	Edit(10, u"a\nb\nc\n", 0);
	CHECK(Check());
}

TEST(Lexer_StatesInternAndInvalidationScope)
{
	std::u16string Text;
	for (int I = 0; I < 200; ++I)
	{
		Text += (I % 10 == 0) ? u"def f():\n" : u"    x = (1, 2)\n";
	}
	dream::TextBuffer B(Text);
	PyLexer Lx;
	Lx.Reset(B.Snapshot());
	CHECK_EQ(Lx.Frontier(), 0);
	Lx.Relex(B.Snapshot(), 0.0, 0, 0);
	CHECK_EQ(Lx.Frontier(), B.LineCount());
	CHECK(Lx.IsClean(150));
	CHECK_EQ(Lx.EndState(5), Lx.EndState(6));

	// An in-line edit that keeps the end state relexes one line.
	dream::EditBatch E;
	E.Ranges.push_back(dream::EditRange{B.Snapshot().LineStart(5) + 4, 0, u"yy"});
	Lx.OnChange(B.Apply(E), B.Snapshot());
	CHECK_EQ(Lx.Frontier(), 5);
	CHECK(!Lx.IsClean(5));
	CHECK_EQ(Lx.Relex(B.Snapshot(), 0.0, 0, 0), 1);

	// Opening and closing a triple quote before the cascade spreads stops within 2 lines of the close.
	dream::EditBatch Open;
	Open.Ranges.push_back(dream::EditRange{B.Snapshot().LineStart(20), 0, u"\"\"\""});
	Lx.OnChange(B.Apply(Open), B.Snapshot());
	dream::EditBatch Close;
	Close.Ranges.push_back(dream::EditRange{B.Snapshot().LineStart(21), 0, u"\"\"\""});
	Lx.OnChange(B.Apply(Close), B.Snapshot());
	const int32_t Lexed = Lx.Relex(B.Snapshot(), 0.0, 0, 0);
	CHECK(Lexed <= 4);
	CHECK(Lx.DescribeState(Lx.EndState(20)).bInString);
	CHECK(Lx.DescribeState(Lx.EndState(20)).bTriple);
	CHECK(!Lx.DescribeState(Lx.EndState(21)).bInString);
	std::vector<std::vector<uint32_t>> Full;
	PyLexer::LexFull(B.Snapshot(), LexGrammar::Py311, Full);
	for (dream::LineNo L = 0; L < B.LineCount(); ++L)
	{
		const auto T = Lx.Tokens(L);
		CHECK(std::vector<uint32_t>(T.begin(), T.end()) == Full[static_cast<size_t>(L)]);
	}
	CHECK(Lx.Tokens(-1).empty());
	CHECK(Lx.Tokens(100000).empty());
}

namespace
{
	const char16_t* const Fragments[] = {
		u"def f(x):\n", u"    return x\n", u"s = \"\"\"abc\n", u"\"\"\"\n", u"t = f'{x!r:>{w}}'\n", u"# %% cell\n",
		u"x = (1,\n", u"2)\n", u"'''", u"y = 'a\\\n", u"b'\n", u"@dec\n", u"class A:\n", u"match x:\n", u"    case _:\n",
		u"z = f\"\"\"{\n", u"}\"\"\"\n", u"u = rb'\\''\n", u"\\\n", u"if a: pass  # c:\n", u"v = {'k': [1, 2]}\n",
	};
	const char16_t Noise[] = {u'"', u'\'', u'{', u'}', u'#', u'\\', u'\n', u':', u'(', u')', u'f', u' ', u'x', u'1', u'\t'};

	void RandomEdits(LexGrammar G, uint64_t Seed, int Steps)
	{
		Rng R(Seed);
		std::u16string Start;
		for (int I = 0; I < 40; ++I)
		{
			Start += Fragments[R.Below(sizeof(Fragments) / sizeof(Fragments[0]))];
		}
		dream::TextBuffer B(Start);
		PyLexer Lx(G);
		Lx.Reset(B.Snapshot());
		Lx.Relex(B.Snapshot(), 0.0, 0, 0);
		int Mismatches = 0;
		for (int Step = 0; Step < Steps && Mismatches < 3; ++Step)
		{
			const int64_t Size = B.Length();
			dream::EditBatch E;
			dream::EditRange Range;
			Range.At = static_cast<int64_t>(R.Below(static_cast<uint64_t>(Size + 1)));
			switch (R.Below(3))
			{
			case 0:
				Range.Insert = Fragments[R.Below(sizeof(Fragments) / sizeof(Fragments[0]))];
				break;
			case 1:
				Range.DeleteLen = std::min<int64_t>(static_cast<int64_t>(R.Below(12)), Size - Range.At);
				break;
			default:
				Range.DeleteLen = std::min<int64_t>(static_cast<int64_t>(R.Below(2)), Size - Range.At);
				Range.Insert.push_back(Noise[R.Below(sizeof(Noise) / sizeof(Noise[0]))]);
				break;
			}
			if (Size > 6000)
			{
				// Inserts outweigh deletes: keep the text near a few hundred lines so a long run stays linear.
				Range = dream::EditRange{static_cast<int64_t>(R.Below(static_cast<uint64_t>(Size / 2))), Size / 3, std::u16string()};
			}
			E.Ranges.push_back(Range);
			const dream::ChangeSet C = B.Apply(E);
			Lx.OnChange(C, B.Snapshot());
			if (R.OneIn(4))
			{
				// Sometimes leave the frontier stale and relex only part of the text first (budgets, visible lines).
				const dream::LineNo Vis = static_cast<dream::LineNo>(R.Below(static_cast<uint64_t>(B.LineCount())));
				Lx.Relex(B.Snapshot(), 0.0001, Vis, Vis + 20);
			}
			if (R.OneIn(3))
			{
				// And sometimes keep it stale into the next edits (a cascade still running in idle slices when the user
				// types above or below the frontier); the comparison waits for a full pass.
				Lx.Relex(B.Snapshot(), 0.0001, 0, 0);
				continue;
			}
			Lx.Relex(B.Snapshot(), 0.0, 0, 0);
			std::vector<std::vector<uint32_t>> Full;
			PyLexer::LexFull(B.Snapshot(), G, Full);
			Lexed Facts = Lex(B.Snapshot().Text(), G);
			bool bSame = static_cast<size_t>(Lx.LineCount()) == Full.size();
			for (dream::LineNo L = 0; bSame && L < Lx.LineCount(); ++L)
			{
				const auto T = Lx.Tokens(L);
				std::u16string Scratch;
				bSame = std::vector<uint32_t>(T.begin(), T.end()) == Full[static_cast<size_t>(L)]
					&& WellFormed(Full[static_cast<size_t>(L)], B.Snapshot().LineView(L, Scratch).size());
				const LineFacts A = Lx.Facts(L);
				const LineFacts& F = Facts.Facts[static_cast<size_t>(L)];
				bSame = bSame && A.Flags == F.Flags && A.IndentCols == F.IndentCols && A.BracketDelta == F.BracketDelta
					&& A.FirstTokenCol == F.FirstTokenCol && A.FirstTokenKind == F.FirstTokenKind;
				if (!bSame)
				{
					std::fprintf(stderr, "  mismatch at step %d line %d\n", Step, L);
				}
			}
			CHECK(bSame);
			if (!bSame)
			{
				++Mismatches;
			}
		}
	}
}

TEST(Lexer_IncrementalEqualsFullPy311)   // T-LEX-007 (DREAM_FULL_SCALE=1: 100k edits)
{
	RandomEdits(LexGrammar::Py311, 2024, FullScale() ? 100000 : 1500);
}

TEST(Lexer_IncrementalEqualsFullPy312Plus)   // T-LEX-007 (DREAM_FULL_SCALE=1: 100k edits)
{
	RandomEdits(LexGrammar::Py312Plus, 77, FullScale() ? 100000 : 800);
}
