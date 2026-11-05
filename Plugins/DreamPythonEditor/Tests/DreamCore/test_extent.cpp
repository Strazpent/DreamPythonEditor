// SPDX-License-Identifier: Apache-2.0
// Tier 1: StatementExtent, NextStatementLine, SelectionLines (TDD §4.5, T-CORE-037, T-CORE-038, T-CORE-039 stale
// frontier), the REPL pre-check (§6.5, T-REPL-002 shape) and ToggleComment (EDT-04, T-CORE-025 subset).
#include "dream_test.h"

#include "dream/EditBuilders.h"
#include "dream/StatementExtent.h"

#include <optional>
#include <string>

using dream::LineNo;
using dream::LineRange;
using dream::PyLexer;
using dream::ReplPrecheck;

namespace
{
	const char16_t* const Golden =
		u"import os\n"                          // 0
		u"\n"                                   // 1
		u"for i in range(3):\n"                 // 2
		u"    print(i)\n"                       // 3
		u"    # comment in body\n"              // 4
		u"\n"                                   // 5
		u"else:\n"                              // 6
		u"    done = True\n"                    // 7
		u"# trailing comment\n"                 // 8
		u"\n"                                   // 9
		u"if a:\n"                              // 10
		u"    x = 1\n"                          // 11
		u"elif b:\n"                            // 12
		u"    x = 2\n"                          // 13
		u"else:\n"                              // 14
		u"    x = 3\n"                          // 15
		u"try:\n"                               // 16
		u"    risky()\n"                        // 17
		u"except ValueError as e:\n"            // 18
		u"    handle(e)\n"                      // 19
		u"finally:\n"                           // 20
		u"    cleanup()\n"                      // 21
		u"value = call(1,\n"                    // 22
		u"             2,\n"                    // 23
		u"             3)\n"                    // 24
		u"s = \"\"\"open\n"                     // 25
		u"still open\n"                         // 26
		u"\"\"\"\n"                             // 27
		u"total = 1 + \\\n"                     // 28
		u"    2\n"                              // 29
		u"@decorator_one\n"                     // 30
		u"@decorator_two(arg1,\n"               // 31
		u"               arg2)\n"               // 32
		u"def decorated():\n"                   // 33
		u"    return 1\n"                       // 34
		u"try: import json\n"                   // 35
		u"except ImportError: json = None\n"    // 36
		u"if c: a()\n"                          // 37
		u"else: b()\n"                          // 38
		u"match command:\n"                     // 39
		u"    case \"go\":\n"                   // 40
		u"        go()\n"                       // 41
		u"    case Point(x=0,\n"                // 42
		u"               y=0):\n"               // 43
		u"        origin()\n"                   // 44
		u"    case X: f()\n"                    // 45
		u"for case in items:\n"                 // 46
		u"    case(1)\n"                        // 47
		u"    case.cleanup()\n"                 // 48
		u"\n"                                   // 49
		u"# comment\n"                          // 50
		u"last = 1";                            // 51

	struct Fixture
	{
		dream::TextBuffer Buffer;
		PyLexer Lexer;

		explicit Fixture(std::u16string_view Text)
			: Buffer(Text)
		{
			Lexer.Reset(Buffer.Snapshot());   // frontier at line 0: every query relexes first
		}

		std::optional<LineRange> Extent(LineNo Caret) { return dream::StatementExtent(Buffer.Snapshot(), Lexer, Caret); }
		std::optional<LineNo> Next(LineNo First, LineNo Last) { return dream::NextStatementLine(Buffer.Snapshot(), Lexer, LineRange{First, Last}); }
	};

	bool Is(const std::optional<LineRange>& R, LineNo First, LineNo Last)
	{
		return R && R->First == First && R->Last == Last;
	}
}

TEST(Extent_GoldenCases)
{
	Fixture F(Golden);
	struct Case
	{
		LineNo Caret;
		LineNo First;   // -1: no statement
		LineNo Last;
	};
	const Case Cases[] = {
		{0, 0, 0}, {1, -1, -1}, {2, 2, 7}, {3, 3, 3}, {4, -1, -1}, {5, -1, -1}, {6, 2, 7}, {7, 7, 7}, {8, -1, -1},
		{10, 10, 15}, {11, 11, 11}, {12, 10, 15}, {14, 10, 15},
		{16, 16, 21}, {18, 16, 21}, {20, 16, 21}, {19, 19, 19},
		{22, 22, 24}, {23, 22, 24}, {24, 22, 24},
		{25, 25, 27}, {26, 25, 27}, {27, 25, 27},
		{28, 28, 29}, {29, 28, 29},
		{30, 30, 34}, {31, 30, 34}, {32, 30, 34}, {33, 30, 34}, {34, 34, 34},
		{35, 35, 36}, {36, 35, 36}, {37, 37, 38}, {38, 37, 38},
		{39, 39, 45}, {40, 39, 45}, {41, 41, 41}, {42, 39, 45}, {43, 39, 45}, {44, 44, 44}, {45, 39, 45},
		{46, 46, 48}, {47, 47, 47}, {48, 48, 48},
		{49, -1, -1}, {50, -1, -1}, {51, 51, 51},
	};
	for (const Case& C : Cases)
	{
		const std::optional<LineRange> R = F.Extent(C.Caret);
		if (C.First < 0)
		{
			CHECK(!R.has_value());
		}
		else
		{
			if (!Is(R, C.First, C.Last))
			{
				std::fprintf(stderr, "  caret %d: got %d..%d, want %d..%d\n", C.Caret, R ? R->First : -1, R ? R->Last : -1, C.First, C.Last);
			}
			CHECK(Is(R, C.First, C.Last));
		}
	}
}

TEST(Extent_NextStatementSkipsClauses)
{
	Fixture F(Golden);
	struct Case
	{
		LineNo First;
		LineNo Last;
		LineNo Next;   // -1: end of buffer
	};
	const Case Cases[] = {
		{0, 0, 2}, {2, 7, 10}, {3, 3, 10}, {7, 7, 10},
		{10, 15, 16}, {11, 11, 16}, {13, 13, 16}, {15, 15, 16},
		{16, 21, 22}, {17, 17, 22}, {19, 19, 22}, {21, 21, 22},
		{22, 24, 25}, {25, 27, 28}, {28, 29, 30}, {30, 34, 35}, {35, 35, 37}, {35, 36, 37}, {37, 37, 39},
		{41, 41, 46}, {44, 44, 46}, {39, 45, 46},
		{47, 47, 48}, {48, 48, 51}, {51, 51, -1},
	};
	for (const Case& C : Cases)
	{
		const std::optional<LineNo> N = F.Next(C.First, C.Last);
		if (C.Next < 0)
		{
			CHECK(!N.has_value());
		}
		else
		{
			if (!N || *N != C.Next)
			{
				std::fprintf(stderr, "  ran %d..%d: got %d, want %d\n", C.First, C.Last, N ? *N : -1, C.Next);
			}
			CHECK(N.has_value() && *N == C.Next);
		}
	}
	CHECK(dream::NextStatementStartFrom(F.Buffer.Snapshot(), F.Lexer, 1) == std::optional<LineNo>(2));
	CHECK(dream::NextStatementStartFrom(F.Buffer.Snapshot(), F.Lexer, 49) == std::optional<LineNo>(51));
	CHECK(dream::NextStatementStartFrom(F.Buffer.Snapshot(), F.Lexer, 23) == std::optional<LineNo>(25));
	CHECK(!dream::NextStatementStartFrom(F.Buffer.Snapshot(), F.Lexer, 52).has_value());
	CHECK_EQ(dream::FirstNonBlankColumn(F.Buffer.Snapshot(), 3), 4);
	CHECK_EQ(dream::FirstNonBlankColumn(F.Buffer.Snapshot(), 1), 0);
}

TEST(Extent_NestedWalkLandsAfterOuterTry)
{
	Fixture F(u"try:\n    if x:\n        a()\n    else:\n        b()\nexcept E:\n    d()\nafter()\n");
	CHECK(Is(F.Extent(1), 1, 4));
	CHECK(Is(F.Extent(5), 0, 6));
	CHECK(F.Next(2, 2) == std::optional<LineNo>(7));
	CHECK(F.Next(4, 4) == std::optional<LineNo>(7));
	CHECK(F.Next(6, 6) == std::optional<LineNo>(7));
	// Stepping through: from the try header's body, never landing on a clause.
	Fixture G(u"match v:\n    case 1:\n        one()\n    case 2:\n        two()\n    case X: f()\nend()\n");
	CHECK(G.Next(2, 2) == std::optional<LineNo>(6));
	CHECK(G.Next(4, 4) == std::optional<LineNo>(6));
	CHECK(Is(G.Extent(5), 0, 5));
}

TEST(Extent_SelectionLines)
{
	dream::TextBuffer B(u"a\nbb\nccc\ndddd\n");
	const dream::TextSnapshot S = B.Snapshot();
	CHECK(dream::SelectionLines(S, S.LineStart(1), S.LineStart(3)) == (LineRange{1, 2}));   // ends at column 0
	CHECK(dream::SelectionLines(S, S.LineStart(3), S.LineStart(1)) == (LineRange{1, 2}));   // reversed
	CHECK(dream::SelectionLines(S, S.LineStart(1) + 1, S.LineStart(2) + 1) == (LineRange{1, 2}));
	CHECK(dream::SelectionLines(S, S.LineStart(2), S.LineStart(2)) == (LineRange{2, 2}));   // empty: the caret line
	CHECK(dream::SelectionLines(S, S.LineStart(1), S.LineStart(2)) == (LineRange{1, 1}));
}

TEST(Extent_StaleFrontierAndEdits)
{
	Fixture F(Golden);
	const std::optional<LineRange> Before = F.Extent(33);
	// Open a string at the top, relex, then close it again: facts are recomputed before every query.
	dream::EditBatch Open;
	Open.Ranges.push_back(dream::EditRange{0, 0, u"\"\"\"\n"});
	F.Lexer.OnChange(F.Buffer.Apply(Open), F.Buffer.Snapshot());
	// Line 34 now sits in the string that the old line 27 opens, so its extent starts there.
	const std::optional<LineRange> Inside = F.Extent(34);
	CHECK(Inside.has_value() && Inside->First == 28);
	dream::EditBatch Close;
	Close.Ranges.push_back(dream::EditRange{0, 4, u""});
	F.Lexer.OnChange(F.Buffer.Apply(Close), F.Buffer.Snapshot());
	CHECK(F.Extent(33) == Before);
	PyLexer Fresh;
	CHECK(dream::StatementExtent(F.Buffer.Snapshot(), Fresh, 42) == F.Extent(42));
}

TEST(Repl_PreCheckAndIndentation)
{
	struct Case
	{
		const char16_t* Source;
		ReplPrecheck Want;
	};
	const Case Cases[] = {
		{u"for i in x:", ReplPrecheck::DefinitelyIncomplete},
		{u"x = (1,", ReplPrecheck::DefinitelyIncomplete},
		{u"@dec", ReplPrecheck::DefinitelyIncomplete},
		{u"s = '''abc", ReplPrecheck::DefinitelyIncomplete},
		{u"x = 1 + \\", ReplPrecheck::DefinitelyIncomplete},
		{u"match x:", ReplPrecheck::DefinitelyIncomplete},
		{u"class A:  # c: d", ReplPrecheck::DefinitelyIncomplete},
		{u"async def f():", ReplPrecheck::DefinitelyIncomplete},
		{u"while True:\n    # c", ReplPrecheck::DefinitelyIncomplete},
		{u"def f():\n pass", ReplPrecheck::Unknown},
		{u"else:", ReplPrecheck::Unknown},
		{u"case 1:", ReplPrecheck::Unknown},
		{u"if x: pass  # note:", ReplPrecheck::Unknown},
		{u"x=1\ny=2", ReplPrecheck::Unknown},
		{u"lambda:", ReplPrecheck::Unknown},
		{u"x = [\n    1,\n    2]", ReplPrecheck::Unknown},
		{u"", ReplPrecheck::Unknown},
		{u"d = {'k': 1}", ReplPrecheck::Unknown},
	};
	for (const Case& C : Cases)
	{
		CHECK(dream::ReplPreCheck(C.Source) == C.Want);
	}
	CHECK(dream::LastLogicalLineIndented(u"for i in range(3):\n    print(i)"));
	CHECK(dream::LastLogicalLineIndented(u"def f():\n pass"));
	CHECK(dream::LastLogicalLineIndented(u"for i in x:\n    print(i)\n"));
	CHECK(!dream::LastLogicalLineIndented(u"x = [\n    1,\n    2]"));
	CHECK(!dream::LastLogicalLineIndented(u"x = 1"));
	CHECK(!dream::LastLogicalLineIndented(u""));
}

namespace
{
	std::u16string Toggle(dream::TextBuffer& B, PyLexer& L, LineNo First, LineNo Last)
	{
		const dream::EditBatch E = dream::ToggleComment(B.Snapshot(), L, LineRange{First, Last});
		CHECK(E.Origin == dream::EditOrigin::Command);
		L.OnChange(B.Apply(E), B.Snapshot());
		return B.Snapshot().Text();
	}
}

TEST(EditBuilders_ToggleComment)
{
	dream::TextBuffer B(u"a = 1\n    b = 2\n\n    # c\nz");
	PyLexer L;
	L.Reset(B.Snapshot());
	CHECK(Toggle(B, L, 0, 3) == u"# a = 1\n#     b = 2\n\n#     # c\nz");
	CHECK(Toggle(B, L, 0, 3) == u"a = 1\n    b = 2\n\n    # c\nz");          // a second toggle restores the mix
	CHECK(Toggle(B, L, 1, 1) == u"a = 1\n    # b = 2\n\n    # c\nz");
	CHECK(Toggle(B, L, 1, 3) == u"a = 1\n    b = 2\n\n    c\nz");            // all commented: one '# ' each

	dream::TextBuffer S(u"s = \"\"\"\ninner\n\"\"\"\n");
	PyLexer SL;
	SL.Reset(S.Snapshot());
	CHECK(Toggle(S, SL, 0, 2) == u"# s = \"\"\"\ninner\n\"\"\"\n");         // lines inside the string are left alone
	dream::TextBuffer H(u"#x\n  #y\n");
	PyLexer HL;
	CHECK(Toggle(H, HL, 0, 1) == u"x\n  y\n");                               // "#" without a space
	dream::TextBuffer Blank(u"\n   \n");
	PyLexer BL;
	CHECK(dream::ToggleComment(Blank.Snapshot(), BL, LineRange{0, 1}).Ranges.empty());
}
