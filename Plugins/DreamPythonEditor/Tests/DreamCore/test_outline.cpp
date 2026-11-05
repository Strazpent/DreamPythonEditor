// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::BuildOutline, FindOutlineEntryAt and FilterOutline (NAV-02 / T-NAV-005 without a server, TDD §7.9.2;
// owner ruling PO-05 §3): nesting (classes, methods, nested functions), decorators folded in, async defs, `# %%` cells,
// defs inside strings and comments, continuation lines, tabs as indent, CRLF, an empty file, broken code (unclosed
// brackets and strings, bad dedents), the snapshot and text entry points agreeing, a random-edit run that checks the
// invariants after every edit, and a 1 MB file under 20 ms (best of three; DREAM_SKIP_TIMING=1 skips the gate).
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"
#include "test_util.h"

#include "dream/Outline.h"
#include "dream/TextBuffer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using dream::OutlineEntry;
using dream::OutlineKind;
using dream::OutlineMatch;
using dream_test_util::N;
using dream_test_util::U;

namespace
{
	const char* KindText(OutlineKind Kind)
	{
		switch (Kind)
		{
		case OutlineKind::Class: return "C";
		case OutlineKind::Function: return "f";
		case OutlineKind::Method: return "m";
		case OutlineKind::AsyncFunction: return "af";
		case OutlineKind::AsyncMethod: return "am";
		case OutlineKind::Cell: return "#";
		}
		return "?";
	}

	/** One line per entry: "<kind> <name> <line>:<column> <first>-<end> d<depth> p<parent>". */
	std::string Describe(const std::vector<OutlineEntry>& Entries)
	{
		std::string Out;
		char Buffer[128];
		for (const OutlineEntry& E : Entries)
		{
			std::snprintf(Buffer, sizeof(Buffer), " %d:%d %d-%d d%d p%d\n", E.Line, E.Column, E.FirstLine, E.EndLine, E.Depth, E.Parent);
			Out += KindText(E.Kind);
			Out += " ";
			Out += E.Name.empty() ? std::string("\"\"") : N(E.Name);
			Out += Buffer;
		}
		return Out;
	}

	std::string Outline(const char* Utf8)
	{
		return Describe(dream::BuildOutline(U(Utf8)));
	}

	bool SkipTiming()
	{
		const char* V = std::getenv("DREAM_SKIP_TIMING");
		return V != nullptr && *V != '\0' && *V != '0';
	}

	const char* const Nested =
		"import os\n"                                  // 0
		"\n"                                           // 1
		"class Outer:\n"                               // 2
		"    \"\"\"Doc.\"\"\"\n"                       // 3
		"    x = 1\n"                                  // 4
		"\n"                                           // 5
		"    class Inner:\n"                           // 6
		"        def method(self):\n"                  // 7
		"            def helper():\n"                  // 8
		"                return 1\n"                   // 9
		"            return helper()\n"                // 10
		"\n"                                           // 11
		"    def outer_method(self, a,\n"              // 12
		"                     b):\n"                   // 13
		"        pass\n"                               // 14
		"\n"                                           // 15
		"def top():\n"                                 // 16
		"    class Local:\n"                           // 17
		"        def m(self): pass\n"                  // 18
		"    def nested():\n"                          // 19
		"        pass\n"                               // 20
		"    return Local\n"                           // 21
		"\n"                                           // 22
		"y = top()\n";                                 // 23

	const char* const NestedExpected =
		"C Outer 2:6 2-14 d0 p-1\n"
		"C Inner 6:10 6-10 d1 p0\n"
		"m method 7:12 7-10 d2 p1\n"
		"f helper 8:16 8-9 d3 p2\n"
		"m outer_method 12:8 12-14 d1 p0\n"
		"f top 16:4 16-21 d0 p-1\n"
		"C Local 17:10 17-18 d1 p5\n"
		"m m 18:12 18-18 d2 p6\n"
		"f nested 19:8 19-20 d1 p5\n";

	const char* const Cells =
		"\"\"\"Module doc\n"                           // 0
		"# %% not a cell (in a string)\n"              // 1
		"\"\"\"\n"                                     // 2
		"import x\n"                                   // 3
		"\n"                                           // 4
		"# %% Setup\n"                                 // 5
		"a = 1\n"                                      // 6
		"# trailing comment\n"                         // 7
		"\n"                                           // 8
		"#%%Load data  \n"                             // 9
		"def load():\n"                                // 10
		"    # %% indented, inside def\n"              // 11
		"    return 1\n"                               // 12
		"data = (\n"                                   // 13
		"# %% inside brackets\n"                       // 14
		")\n"                                          // 15
		"# %%\n"                                       // 16
		"class Model:\n"                               // 17
		"    pass\n";                                  // 18

	/** Checks the invariants every outline keeps, whatever the text. */
	bool CheckInvariants(const std::vector<OutlineEntry>& Entries, dream::LineNo LineCount)
	{
		bool bOk = true;
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			const OutlineEntry& E = Entries[I];
			bOk &= E.FirstLine <= E.Line && E.Line <= E.EndLine && E.EndLine < LineCount && E.FirstLine >= 0;
			bOk &= I == 0 || E.Line > Entries[I - 1].Line;
			bOk &= I == 0 || E.FirstLine > Entries[I - 1].Line;   // what FindOutlineEntryAt's early stop relies on
			bOk &= E.Parent < static_cast<int32_t>(I);
			if (E.IsCell())
			{
				bOk &= E.Parent == -1 && E.Depth == 0;
				continue;
			}
			bOk &= !E.Name.empty();
			if (E.Parent < 0)
			{
				bOk &= E.Depth == 0;
				bOk &= E.Kind != OutlineKind::Method && E.Kind != OutlineKind::AsyncMethod;
				continue;
			}
			const OutlineEntry& P = Entries[static_cast<size_t>(E.Parent)];
			bOk &= E.Depth == P.Depth + 1;
			bOk &= P.Line < E.Line;
			const bool bMethod = E.Kind == OutlineKind::Method || E.Kind == OutlineKind::AsyncMethod;
			if (E.Kind != OutlineKind::Class)
			{
				bOk &= bMethod == (P.Kind == OutlineKind::Class);
			}
			if (!P.IsCell())
			{
				bOk &= E.EndLine <= P.EndLine && E.IndentCols > P.IndentCols;
			}
		}
		return bOk;
	}
}

TEST(Outline_NestingClassesMethodsAndNestedFunctions)
{
	CHECK_EQ(Outline(Nested), std::string(NestedExpected));
}

TEST(Outline_DecoratorsFoldIntoTheirDef)
{
	const char* const Text =
		"import functools\n"                           // 0
		"\n"                                           // 1
		"@functools.lru_cache(\n"                      // 2
		"    maxsize=None)\n"                          // 3
		"# a comment between\n"                        // 4
		"\n"                                           // 5
		"@staticmethod\n"                              // 6
		"def cached(x):\n"                             // 7
		"    return x\n"                               // 8
		"\n"                                           // 9
		"class K:\n"                                   // 10
		"    @property\n"                              // 11
		"    def value(self):\n"                       // 12
		"        return 1\n"                           // 13
		"\n"                                           // 14
		"    @value.setter\n"                          // 15
		"    def value(self, v):\n"                    // 16
		"        pass\n"                               // 17
		"\n"                                           // 18
		"@decorator\n"                                 // 19
		"x = 1\n"                                      // 20
		"def plain():\n"                               // 21
		"    pass\n"                                   // 22
		"    @inner\n"                                 // 23
		"def wrong_indent():\n"                        // 24
		"    pass\n";                                  // 25
	CHECK_EQ(Outline(Text), std::string(
		"f cached 7:4 2-8 d0 p-1\n"
		"C K 10:6 10-17 d0 p-1\n"
		"m value 12:8 11-13 d1 p1\n"
		"m value 16:8 15-17 d1 p1\n"
		"f plain 21:4 21-23 d0 p-1\n"
		"f wrong_indent 24:4 24-25 d0 p-1\n"));
	// A decorated class, and a decorator run at the end of the text with nothing under it.
	CHECK_EQ(Outline("@dataclass\nclass P:\n    x: int\n@dangling\n"), std::string("C P 1:6 0-2 d0 p-1\n"));
}

TEST(Outline_AsyncFunctionsAndMethods)
{
	const char* const Text =
		"async def fetch(url):\n"                      // 0
		"    async with session() as s:\n"             // 1
		"        return [x async for x in s]\n"        // 2
		"\n"                                           // 3
		"class Client:\n"                              // 4
		"    async  def get(self):\n"                  // 5
		"        pass\n"                               // 6
		"    def sync(self):\n"                        // 7
		"        async def inner():\n"                 // 8
		"            pass\n"                           // 9
		"async = 3\n"                                  // 10
		"asyncdef = 4\n"                               // 11
		"async\tdef tabbed(): pass\n";                 // 12
	CHECK_EQ(Outline(Text), std::string(
		"af fetch 0:10 0-2 d0 p-1\n"
		"C Client 4:6 4-9 d0 p-1\n"
		"am get 5:15 5-6 d1 p1\n"
		"m sync 7:8 7-9 d1 p1\n"
		"af inner 8:18 8-9 d2 p3\n"
		"af tabbed 12:10 12-12 d0 p-1\n"));
}

TEST(Outline_CellsAreTopLevelParents)
{
	CHECK_EQ(Outline(Cells), std::string(
		"# Setup 5:0 5-7 d0 p-1\n"
		"# Load data 9:0 9-10 d0 p-1\n"
		"f load 10:4 10-12 d1 p1\n"
		"# indented, inside def 11:4 11-15 d0 p-1\n"
		"# \"\" 16:0 16-18 d0 p-1\n"
		"C Model 17:6 17-18 d1 p4\n"));
	// A cell header in a class body closes nothing: the method after it stays in the class.
	CHECK_EQ(Outline("class A:\n    def a(self): pass\n# %% B\n    def b(self): pass\nx = 1\n"), std::string(
		"C A 0:6 0-3 d0 p-1\n"
		"m a 1:8 1-1 d1 p0\n"
		"# B 2:0 2-4 d0 p-1\n"
		"m b 3:8 3-3 d1 p0\n"));
}

TEST(Outline_DefsInsideStringsAndCommentsMakeNoEntries)
{
	const char* const Text =
		"s = \"\"\"\n"                                 // 0
		"def fake_one():\n"                            // 1
		"    pass\n"                                   // 2
		"\"\"\"\n"                                     // 3
		"t = r'''class FakeTwo:\n"                     // 4
		"'''\n"                                        // 5
		"u = 'def fake_three(): \\\n"                  // 6
		"def fake_four(): pass'\n"                     // 7
		"v = \"def fake_five()\"  # def fake_six():\n" // 8
		"# def fake_seven():\n"                        // 9
		"w = f\"{x}\" b\"\\\"\" ; z = 1\n"             // 10
		"def real():\n"                                // 11
		"    \"\"\"Docstring with\n"                   // 12
		"    def fake_eight():\n"                      // 13
		"    \"\"\"\n"                                 // 14
		"    return 'class Fake9:'\n"                  // 15
		"q = Rb'''\n"                                  // 16
		"class FakeTen: pass\n"                        // 17
		"''' + 'x'\n";                                 // 18
	CHECK_EQ(Outline(Text), std::string("f real 11:4 11-15 d0 p-1\n"));
}

TEST(Outline_ContinuationLinesStartNoEntry)
{
	const char* const Text =
		"x = [\n"                                      // 0
		"    1,\n"                                     // 1
		"]\n"                                          // 2
		"def after_list():\n"                          // 3
		"    pass\n"                                   // 4
		"y = 1 + \\\n"                                 // 5
		"    2\n"                                      // 6
		"call(a,\n"                                    // 7
		"     def_value=1,\n"                          // 8
		"     class_=2)\n"                             // 9
		"def f(a,\n"                                   // 10
		"      b=(1,\n"                                // 11
		"         2)):\n"                              // 12
		"    pass\n"                                   // 13
		"total = foo(\n"                               // 14
		"    def broken(): pass\n"                     // 15
		"class AfterBroken:\n"                         // 16
		"    pass\n"                                   // 17
		"z = 1 + \\\n"                                 // 18
		"def joined(): pass\n"                         // 19
		"w = [i\n"                                     // 20
		"     async for i in it]\n";                   // 21
	CHECK_EQ(Outline(Text), std::string(
		"f after_list 3:4 3-4 d0 p-1\n"
		"f f 10:4 10-13 d0 p-1\n"
		"f broken 15:8 15-15 d0 p-1\n"
		"C AfterBroken 16:6 16-17 d0 p-1\n"
		"f joined 19:4 19-19 d0 p-1\n"));
}

TEST(Outline_TabsAsIndent)
{
	const char* const Text =
		"class T:\n"                                   // 0
		"\tdef a(self):\n"                             // 1
		"\t\tpass\n"                                   // 2
		"        def b(self):\n"                       // 3  8 spaces: the same column as one tab
		"\t    pass\n"                                 // 4
		"\f\tdef c(self): pass\n"                      // 5  a form feed resets the column
		"  \tdef d(self): pass\n"                      // 6  two spaces and a tab: column 8
		"def e(): pass\n";                             // 7
	CHECK_EQ(Outline(Text), std::string(
		"C T 0:6 0-6 d0 p-1\n"
		"m a 1:5 1-2 d1 p0\n"
		"m b 3:12 3-4 d1 p0\n"
		"m c 5:6 5-5 d1 p0\n"
		"m d 6:7 6-6 d1 p0\n"
		"f e 7:4 7-7 d0 p-1\n"));
}

TEST(Outline_CrlfLinesGiveTheSameOutline)
{
	std::string Crlf;
	for (const char* P = Nested; *P != '\0'; ++P)
	{
		if (*P == '\n')
		{
			Crlf += '\r';
		}
		Crlf += *P;
	}
	CHECK_EQ(Outline(Crlf.c_str()), std::string(NestedExpected));
	std::string CellsCrlf;
	for (const char* P = Cells; *P != '\0'; ++P)
	{
		if (*P == '\n')
		{
			CellsCrlf += '\r';
		}
		CellsCrlf += *P;
	}
	CHECK_EQ(Outline(CellsCrlf.c_str()), Outline(Cells));
	// The cell title loses its CR too.
	CHECK_EQ(Outline("# %% Title\r\nx = 1\r\n"), std::string("# Title 0:0 0-1 d0 p-1\n"));
}

TEST(Outline_EmptyAndEntryFreeTexts)
{
	CHECK(dream::BuildOutline(std::u16string_view()).empty());
	CHECK(dream::BuildOutline(dream::TextSnapshot()).empty());
	CHECK(dream::BuildOutline(U("\n\n   \n\t\n")).empty());
	CHECK(dream::BuildOutline(U("x = 1\nprint(x)\n# just a comment\n")).empty());
	// While typing: a keyword with no name yet.
	CHECK(dream::BuildOutline(U("def")).empty());
	CHECK(dream::BuildOutline(U("def \n")).empty());
	CHECK(dream::BuildOutline(U("class (\n")).empty());
	CHECK(dream::BuildOutline(U("def(x): pass\n")).empty());
	CHECK(dream::BuildOutline(U("async def\n")).empty());
	CHECK(dream::BuildOutline(U("define = 1\nclassify = 2\n")).empty());
}

TEST(Outline_BrokenCodeStillListsEveryDef)
{
	// An unclosed bracket, then an unclosed triple-quoted string that runs to the end of the text.
	CHECK_EQ(Outline(
		"def ok():\n"                                  // 0
		"    return (1,\n"                             // 1
		"def next_one():\n"                            // 2
		"    pass\n"                                   // 3
		"s = \"\"\"unclosed\n"                         // 4
		"def after_string():\n"                        // 5
		"    pass\n"                                   // 6
		"class Last:\n"                                // 7
		"    pass\n"),                                 // 8
		std::string(
		"f ok 0:4 0-1 d0 p-1\n"
		"f next_one 2:4 2-3 d0 p-1\n"
		"f after_string 5:4 5-6 d0 p-1\n"
		"C Last 7:6 7-8 d0 p-1\n"));
	// A dedent to a column no block opened at: it closes what is deeper and keeps the rest.
	CHECK_EQ(Outline(
		"class A:\n"
		"        def m(self): pass\n"
		"    def n(self): pass\n"
		"  x = 1\n"
		"    def o(self): pass\n"),
		std::string(
		"C A 0:6 0-4 d0 p-1\n"
		"m m 1:12 1-1 d1 p0\n"
		"m n 2:8 2-2 d1 p0\n"
		"m o 4:8 4-4 d1 p0\n"));
	// An unmatched closing bracket is ignored.
	CHECK_EQ(Outline("x = 1)]}\ndef after_close(): pass\n"), std::string("f after_close 1:4 1-1 d0 p-1\n"));
	// Two unclosed triple-quoted strings of different kinds: two recoveries.
	CHECK_EQ(Outline("a = \"\"\"one\nb = '''two\ndef survivor(): pass\n"), std::string("f survivor 2:4 2-2 d0 p-1\n"));
	// An unclosed single-quoted string ends with its line.
	CHECK_EQ(Outline("s = 'open\ndef after_quote(): pass\n"), std::string("f after_quote 1:4 1-1 d0 p-1\n"));
	// A def with no body at the end, and a class whose body never came.
	CHECK_EQ(Outline("class Empty:\ndef sig(a,\n        b"), std::string(
		"C Empty 0:6 0-0 d0 p-1\n"
		"f sig 1:4 1-2 d0 p-1\n"));
}

TEST(Outline_StringRecoveriesAreCapped)
{
	// Each `\"""` line opens a triple-quoted string that its escaped quote never lets close: every recovery rescans the
	// rest. MaxStringRecoveries (4) recoveries still reach a def after 4 such lines; after 5 the rest stays a string.
	REQUIRE(dream::MaxStringRecoveries == 4);
	CHECK_EQ(Outline("\\\"\"\"\n\\\"\"\"\n\\\"\"\"\n\\\"\"\"\ndef found(): pass\n"), std::string("f found 4:4 4-4 d0 p-1\n"));
	CHECK(dream::BuildOutline(U("\\\"\"\"\n\\\"\"\"\n\\\"\"\"\n\\\"\"\"\n\\\"\"\"\ndef lost(): pass\n")).empty());
	// 20,000 such lines: the cap keeps the build linear (it was quadratic, seconds on a tab switch).
	std::u16string Many;
	for (int Line = 0; Line < 20000; ++Line)
	{
		Many += u"x = \\\"\"\"\n";
	}
	const auto T0 = std::chrono::steady_clock::now();
	const std::vector<OutlineEntry> Entries = dream::BuildOutline(Many);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	CHECK(Entries.empty());
	std::printf("  20,000 unclosed-string lines: %.2f ms\n", Ms);
	if (!SkipTiming())
	{
		CHECK(Ms < 50.0);
	}
}

TEST(Outline_CellHeaderEndsADecoratorRun)
{
	// A decorator, then a cell header, then the def: the decorator does not fold over the cell, so the def starts after
	// the cell header and the caret search (which stops at the first entry starting past the line) stays exact.
	const std::vector<OutlineEntry> E = dream::BuildOutline(U("@dec\n# %% C\ndef f():\n    pass\n"));
	CHECK_EQ(Describe(E), std::string(
		"# C 1:0 1-3 d0 p-1\n"
		"f f 2:4 2-3 d1 p0\n"));
	CHECK_EQ(dream::FindOutlineEntryAt(E, 0), -1);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 1), 0);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 2), 1);
	// A cell header inside a class body sits between the class's methods in source order.
	const std::vector<OutlineEntry> C = dream::BuildOutline(U("class A:\n    def a(self): pass\n# %%\n    @property\n    def b(self): pass\n"));
	REQUIRE(C.size() == 4);
	CHECK_EQ(C[3].FirstLine, 3);
	CHECK_EQ(dream::FindOutlineEntryAt(C, 3), 3);
}

TEST(Outline_NonAsciiNames)
{
	// "def größe(): pass" and "class Ünïcode:" (UTF-16 columns), and a name with a supplementary-plane letter.
	CHECK_EQ(Outline("def gr\xC3\xB6\xC3\x9F" "e(): pass\nclass \xC3\x9Cn\xC3\xAF" "code:\n    def m(self): pass\n"), std::string(
		"f gr\xC3\xB6\xC3\x9F" "e 0:4 0-0 d0 p-1\n"
		"C \xC3\x9Cn\xC3\xAF" "code 1:6 1-2 d0 p-1\n"
		"m m 2:8 2-2 d1 p1\n"));
	CHECK_EQ(Outline("def \xF0\x9D\x90\x80x(): pass\n"), std::string("f \xF0\x9D\x90\x80x 0:4 0-0 d0 p-1\n"));
}

TEST(Outline_SnapshotAndTextAgree)
{
	dream::TextBuffer Buffer(U(Nested));
	CHECK_EQ(Describe(dream::BuildOutline(Buffer.Snapshot())), std::string(NestedExpected));
	// Edits split the text into pieces; the snapshot entry point reads lines across them.
	dream::EditBatch Insert;
	Insert.Ranges.push_back(dream::EditRange{ 0, 0, U("@decorate\nclass Added:\n    async def run(self): pass\n") });
	Buffer.Apply(Insert);
	dream::EditBatch Rename;
	const dream::Offset At = static_cast<dream::Offset>(Buffer.Snapshot().Text().find(u"def top"));
	Rename.Ranges.push_back(dream::EditRange{ At + 4, 3, U("renamed_top") });
	Buffer.Apply(Rename);
	const dream::TextSnapshot Snapshot = Buffer.Snapshot();
	CHECK(Snapshot.PieceCount() > 1);
	const std::vector<OutlineEntry> FromSnapshot = dream::BuildOutline(Snapshot);
	const std::vector<OutlineEntry> FromText = dream::BuildOutline(Snapshot.Text());
	CHECK(FromSnapshot == FromText);
	REQUIRE(FromSnapshot.size() == 11);
	CHECK_EQ(Describe({ FromSnapshot[0], FromSnapshot[1] }), std::string(
		"C Added 1:6 0-2 d0 p-1\n"
		"am run 2:14 2-2 d1 p0\n"));
	CHECK(FromSnapshot[7].Name == u"renamed_top");
}

TEST(Outline_EntryAtTheCaretIsTheDeepest)
{
	const std::vector<OutlineEntry> E = dream::BuildOutline(U(Nested));
	REQUIRE(E.size() == 9);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 0), -1);     // import
	CHECK_EQ(dream::FindOutlineEntryAt(E, 2), 0);      // class Outer
	CHECK_EQ(dream::FindOutlineEntryAt(E, 4), 0);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 9), 3);      // in helper
	CHECK_EQ(dream::FindOutlineEntryAt(E, 10), 2);     // back in method
	CHECK_EQ(dream::FindOutlineEntryAt(E, 11), 0);     // a blank line in Outer, after Inner
	CHECK_EQ(dream::FindOutlineEntryAt(E, 13), 4);     // the signature's continuation line
	CHECK_EQ(dream::FindOutlineEntryAt(E, 15), -1);    // between Outer and top
	CHECK_EQ(dream::FindOutlineEntryAt(E, 18), 7);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 21), 5);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 23), -1);
	CHECK_EQ(dream::FindOutlineEntryAt(E, 99), -1);
	CHECK_EQ(dream::FindOutlineEntryAt({}, 0), -1);

	const std::vector<OutlineEntry> C = dream::BuildOutline(U(Cells));
	CHECK_EQ(dream::FindOutlineEntryAt(C, 3), -1);     // before the first cell
	CHECK_EQ(dream::FindOutlineEntryAt(C, 6), 0);      // in cell Setup
	CHECK_EQ(dream::FindOutlineEntryAt(C, 12), 2);     // load, deeper than the indented cell around it
	CHECK_EQ(dream::FindOutlineEntryAt(C, 14), 3);
	CHECK_EQ(dream::FindOutlineEntryAt(C, 18), 5);

	// A decorator line belongs to its def.
	const std::vector<OutlineEntry> D = dream::BuildOutline(U("x = 1\n@dec\n# note\ndef f():\n    pass\n"));
	REQUIRE(D.size() == 1);
	CHECK_EQ(dream::FindOutlineEntryAt(D, 0), -1);
	CHECK_EQ(dream::FindOutlineEntryAt(D, 1), 0);
	CHECK_EQ(dream::FindOutlineEntryAt(D, 2), 0);
	// A comment indented into the body at its end belongs to the def; one at the def's own column does not.
	const std::vector<OutlineEntry> T = dream::BuildOutline(U("def f():\n    x = 1\n    # more\n# after\ny = 2\n"));
	REQUIRE(T.size() == 1);
	CHECK_EQ(T[0].EndLine, 2);
	CHECK_EQ(dream::FindOutlineEntryAt(T, 3), -1);
}

TEST(Outline_FilterKeepsParentsOfMatches)
{
	const std::vector<OutlineEntry> E = dream::BuildOutline(U(Nested));
	REQUIRE(E.size() == 9);
	auto Text = [&E](std::u16string_view Query)
	{
		std::string Out;
		const std::vector<OutlineMatch> M = dream::FilterOutline(E, Query);
		for (size_t I = 0; I < M.size(); ++I)
		{
			Out += M[I] == OutlineMatch::Match ? 'M' : M[I] == OutlineMatch::Ancestor ? 'a' : '.';
		}
		return Out;
	};
	// Outer Inner method helper outer_method top Local m nested
	CHECK_EQ(Text(u""), std::string("MMMMMMMMM"));
	CHECK_EQ(Text(u"METH"), std::string("aaM.M...."));
	CHECK_EQ(Text(u"local"), std::string(".....aM.."));
	CHECK_EQ(Text(u"zzz"), std::string("........."));
	CHECK_EQ(Text(u"e"), std::string("MMMMMa..M"));       // top is the parent of nested; Local and m have no e
	CHECK_EQ(Text(u"o"), std::string("MaM.MMM.."));       // Inner is the parent of method

	// Case folding beyond ASCII (Python's re.IGNORECASE folding).
	const std::vector<OutlineEntry> G = dream::BuildOutline(U("class Gr\xC3\xB6\xC3\x9F" "e:\n    def \xC3\x84pfel(self): pass\n"));
	REQUIRE(G.size() == 2);
	const std::vector<OutlineMatch> M = dream::FilterOutline(G, U("GR\xC3\x96"));
	CHECK(M[0] == OutlineMatch::Match && M[1] == OutlineMatch::Hidden);
	const std::vector<OutlineMatch> M2 = dream::FilterOutline(G, U("\xC3\xA4PF"));
	CHECK(M2[0] == OutlineMatch::Ancestor && M2[1] == OutlineMatch::Match);

	// Cells are parents like classes.
	const std::vector<OutlineEntry> C = dream::BuildOutline(U(Cells));
	const std::vector<OutlineMatch> MC = dream::FilterOutline(C, u"model");
	REQUIRE(MC.size() == 6);
	CHECK(MC[4] == OutlineMatch::Ancestor && MC[5] == OutlineMatch::Match && MC[0] == OutlineMatch::Hidden);
	// The bare cell (entry 4) is found by the label the view shows for it, and only through that label.
	const std::vector<OutlineMatch> MU = dream::FilterOutline(C, u"CELL", u"(cell)");
	CHECK(MU[4] == OutlineMatch::Match && MU[0] == OutlineMatch::Hidden && MU[3] == OutlineMatch::Hidden);
	CHECK(dream::FilterOutline(C, u"cell")[4] == OutlineMatch::Hidden);
	// "indented, inside def" is a titled cell: it is matched by its title, never by the label.
	CHECK(dream::FilterOutline(C, u"inside", u"(cell)")[3] == OutlineMatch::Match);
}

TEST(Outline_RandomEditsKeepTheInvariants)
{
	// Random edits that make and break strings, brackets, joins, indents, cells and defs: the outline stays well formed,
	// and the snapshot and text entry points agree.
	static const char16_t* const Pieces[] = {
		u"def ", u"class ", u"async def ", u"name", u"(", u")", u"[", u"]", u":", u"\n", u"\n    ", u"\n\t", u"    ",
		u"\"", u"'", u"\"\"\"", u"'''", u"\\", u"\\\n", u"#", u"# %% cell\n", u"@deco\n", u"pass\n", u"\r\n", u"x = 1\n",
		u"\n        def m(self): pass\n", u"f\"{", u"}\"", u"\f",
	};
	dream_test_util::Rng R(20261104);
	dream::TextBuffer Buffer(U(Nested));
	bool bAllOk = true;
	int Agreements = 0;
	for (int Step = 0; Step < 3000; ++Step)
	{
		const dream::TextSnapshot Before = Buffer.Snapshot();
		dream::EditBatch Batch;
		const dream::Offset At = static_cast<dream::Offset>(R.Below(static_cast<uint64_t>(Before.Length()) + 1));
		const int64_t Room = Before.Length() - At;
		const int64_t Delete = (R.OneIn(3) && Room > 0) ? static_cast<int64_t>(R.Below(static_cast<uint64_t>(std::min<int64_t>(Room, 12)) + 1)) : 0;
		std::u16string Insert = Pieces[R.Below(sizeof(Pieces) / sizeof(Pieces[0]))];
		std::erase(Insert, u'\r');   // the model is LF-only
		Batch.Ranges.push_back(dream::EditRange{ At, Delete, Insert });
		if (!Buffer.Validate(Batch))
		{
			continue;
		}
		Buffer.Apply(Batch);
		if (Buffer.Length() > 6000)
		{
			Buffer.SetText(U(Nested), dream::EditOrigin::Load);
		}
		const dream::TextSnapshot After = Buffer.Snapshot();
		const std::vector<OutlineEntry> Entries = dream::BuildOutline(After);
		if (!CheckInvariants(Entries, After.LineCount()))
		{
			bAllOk = false;
			std::fprintf(stderr, "invariants broken at step %d for:\n%s\n%s", Step, N(After.Text()).c_str(), Describe(Entries).c_str());
			break;
		}
		if (Step % 50 == 0)
		{
			Agreements += dream::BuildOutline(After.Text()) == Entries ? 1 : 0;
			CHECK(dream::BuildOutline(After.Text()) == Entries);
		}
	}
	CHECK(bAllOk);
	CHECK(Agreements > 0);
}

TEST(Outline_OneMegabyteUnder20Ms)
{
	// A generated 1 MB module: per block a cell, a decorated class with a docstring, a multi-line signature, an async
	// method, a nested class, a function with a nested function and a string holding a fake def.
	std::u16string Text;
	int Blocks = 0;
	while (Text.size() < 1024 * 1024)
	{
		const std::string I = std::to_string(Blocks++);
		Text += U(
			"# %% Section " + I + "\n"
			"import os\n"
			"\n"
			"@decorator(arg=" + I + ")\n"
			"class Widget" + I + "(Base):\n"
			"    \"\"\"Docstring with def fake(): and class Fake: inside.\"\"\"\n"
			"    count = " + I + "\n"
			"\n"
			"    def __init__(self, a, b=(1,\n"
			"                             2)):\n"
			"        self.a = a  # def not_this():\n"
			"        self.text = f\"{a!r} def no\"\n"
			"\n"
			"    @property\n"
			"    async def fetch" + I + "(self):\n"
			"        data = [x for x in range(10)\n"
			"                if x % 2]\n"
			"        return data\n"
			"\n"
			"    class Meta:\n"
			"        ordering = ['-id']\n"
			"\n"
			"def helper_" + I + "(x):\n"
			"    def inner(y):\n"
			"        return y * 2\n"
			"    s = '''\n"
			"def in_string():\n"
			"    pass\n"
			"'''\n"
			"    return inner(x)\n"
			"\n");
	}
	dream::TextBuffer Buffer(Text);
	const dream::TextSnapshot Snapshot = Buffer.Snapshot();
	std::vector<OutlineEntry> Entries = dream::BuildOutline(Snapshot);
	REQUIRE_EQ(Entries.size(), static_cast<size_t>(Blocks) * 7);
	CHECK(CheckInvariants(Entries, Snapshot.LineCount()));
	CHECK_EQ(Describe({ Entries[0], Entries[1], Entries[2], Entries[3], Entries[4], Entries[5], Entries[6] }), std::string(
		"# Section 0 0:0 0-29 d0 p-1\n"
		"C Widget0 4:6 3-20 d1 p0\n"
		"m __init__ 8:8 8-11 d2 p1\n"
		"am fetch0 14:14 13-17 d2 p1\n"
		"C Meta 19:10 19-20 d2 p1\n"
		"f helper_0 22:4 22-29 d1 p0\n"
		"f inner 23:8 23-24 d2 p5\n"));
	if (SkipTiming())
	{
		std::printf("  (skipped: DREAM_SKIP_TIMING)\n");
		return;
	}
	double Best = 1e9;
	for (int Try = 0; Try < 3; ++Try)
	{
		const auto T0 = std::chrono::steady_clock::now();
		Entries = dream::BuildOutline(Snapshot);
		const auto T1 = std::chrono::steady_clock::now();
		const double Ms = std::chrono::duration<double, std::milli>(T1 - T0).count();
		Best = Ms < Best ? Ms : Best;
	}
	std::printf("  %zu units, %d lines, %zu entries: best of 3 %.2f ms (gate 20 ms)\n", Text.size(), Snapshot.LineCount(), Entries.size(), Best);
	CHECK(Best < 20.0);
}
