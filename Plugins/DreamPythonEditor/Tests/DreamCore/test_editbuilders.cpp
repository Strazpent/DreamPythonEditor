// SPDX-License-Identifier: Apache-2.0
// Tier 1: the edit builders of TDD §4.5 (T-CORE-025): smart indent on Enter (EDT-03), Backspace to the indent stop,
// auto-close pairing and type-over (EDT-05), duplicate line or selection (EDT-04) and indent detection. Cases are
// written with '|' for the caret and '<' '>' around a selection.
#include "dream_test.h"
#include "test_util.h"

#include "dream/EditBuilders.h"

#include <string>

using dream::BuiltEdit;
using dream::IndentStyle;
using dream::Offset;
using dream::PyLexer;
using dream::TextBuffer;

namespace
{
	struct Marked
	{
		std::u16string Text;
		Offset SelStart = 0;
		Offset SelEnd = 0;
	};

	Marked Parse(std::u16string_view In)
	{
		Marked M;
		bool bSel = false;
		for (char16_t C : In)
		{
			if (C == u'|')
			{
				M.SelStart = M.SelEnd = static_cast<Offset>(M.Text.size());
			}
			else if (C == u'<')
			{
				M.SelStart = static_cast<Offset>(M.Text.size());
				bSel = true;
			}
			else if (C == u'>' && bSel)
			{
				M.SelEnd = static_cast<Offset>(M.Text.size());
				bSel = false;
			}
			else
			{
				M.Text.push_back(C);
			}
		}
		return M;
	}

	/** Applies the edit and writes the result back with '|' at the caret (and '<' '>' for a selection). */
	std::u16string Show(const Marked& M, const BuiltEdit& E)
	{
		if (E.IsEmpty())
		{
			return u"<none>";
		}
		TextBuffer B(M.Text);
		if (!E.Batch.Ranges.empty())
		{
			const dream::ChangeSet C = B.Apply(E.Batch);
			if (C.IsEmpty())
			{
				return u"<invalid batch>";
			}
		}
		std::u16string Out = B.Snapshot().Text();
		if (E.Anchor == E.Caret)
		{
			Out.insert(static_cast<size_t>(E.Caret), u"|");
		}
		else
		{
			Out.insert(static_cast<size_t>(std::max(E.Anchor, E.Caret)), u">");
			Out.insert(static_cast<size_t>(std::min(E.Anchor, E.Caret)), u"<");
		}
		return Out;
	}

	std::u16string Enter(std::u16string_view In, const IndentStyle& Style = IndentStyle())
	{
		const Marked M = Parse(In);
		TextBuffer B(M.Text);
		PyLexer L;
		return Show(M, dream::SmartNewline(B.Snapshot(), L, M.SelStart, M.SelEnd, Style));
	}

	std::u16string Backspace(std::u16string_view In, const IndentStyle& Style = IndentStyle())
	{
		const Marked M = Parse(In);
		TextBuffer B(M.Text);
		PyLexer L;
		return Show(M, dream::SmartBackspace(B.Snapshot(), L, M.SelStart, Style));
	}

	std::u16string Type(std::u16string_view In, char16_t C)
	{
		const Marked M = Parse(In);
		TextBuffer B(M.Text);
		PyLexer L;
		return Show(M, dream::AutoClosePair(B.Snapshot(), L, M.SelStart, M.SelEnd, C));
	}

	std::u16string Duplicate(std::u16string_view In)
	{
		const Marked M = Parse(In);
		TextBuffer B(M.Text);
		return Show(M, dream::DuplicateLineOrSelection(B.Snapshot(), M.SelStart, M.SelEnd));
	}

	IndentStyle Detect(std::u16string_view Text)
	{
		TextBuffer B(Text);
		PyLexer L;
		return dream::DetectIndent(B.Snapshot(), L, IndentStyle());
	}
}

TEST(EditBuilders_SmartNewline)   // EDT-03: "def f():⏎ indents 4 spaces"
{
	CHECK_EQ(Enter(u"def f():|"), std::u16string(u"def f():\n    |"));
	CHECK_EQ(Enter(u"    if x:|"), std::u16string(u"    if x:\n        |"));
	CHECK_EQ(Enter(u"    x = 1|"), std::u16string(u"    x = 1\n    |"));
	CHECK_EQ(Enter(u"x = 1   |"), std::u16string(u"x = 1\n|"));                       // no trailing blanks stay behind
	CHECK_EQ(Enter(u"a| b"), std::u16string(u"a\n|b"));                               // blanks after the caret go
	CHECK_EQ(Enter(u"ab<cd>ef"), std::u16string(u"ab\n|ef"));                         // replaces the selection
	CHECK_EQ(Enter(u"    |"), std::u16string(u"\n    |"));                            // the blank prefix moves down
	CHECK_EQ(Enter(u"|x"), std::u16string(u"\n|x"));
	// The caret in a line's leading blanks: its code moves down with its whole indentation (review r1 core-text-M1).
	CHECK_EQ(Enter(u"|    x"), std::u16string(u"\n|    x"));
	CHECK_EQ(Enter(u"  |  x"), std::u16string(u"\n  |  x"));
	CHECK_EQ(Enter(u"def f():\n|    return x"), std::u16string(u"def f():\n\n|    return x"));
	CHECK_EQ(Enter(u"def f():\n  |  return x"), std::u16string(u"def f():\n\n  |  return x"));
	CHECK_EQ(Enter(u"if a:\n    if b:\n        y = 1\n    |    z = 2"), std::u16string(u"if a:\n    if b:\n        y = 1\n\n    |    z = 2"));
	CHECK_EQ(Enter(u"  |  "), std::u16string(u"\n  |"));                              // a blank-only rest is trailing whitespace
	// A selection from the indentation into code: the blanks after it are not indentation and go (review r2).
	CHECK_EQ(Enter(u"x = 1\n    <foo> bar"), std::u16string(u"x = 1\n\n    |bar"));
	CHECK_EQ(Enter(u"x = 1\n  <  >  bar"), std::u16string(u"x = 1\n\n  |  bar"));     // one that ends in the blanks keeps them
	// Dedent after return, pass, break, continue, raise; not in the middle of the statement.
	CHECK_EQ(Enter(u"def f():\n    return x|"), std::u16string(u"def f():\n    return x\n|"));
	CHECK_EQ(Enter(u"        pass|"), std::u16string(u"        pass\n    |"));
	CHECK_EQ(Enter(u"    raise ValueError('x')|"), std::u16string(u"    raise ValueError('x')\n|"));
	CHECK_EQ(Enter(u"    break|"), std::u16string(u"    break\n|"));
	CHECK_EQ(Enter(u"    continue  # next|"), std::u16string(u"    continue  # next\n|"));
	CHECK_EQ(Enter(u"    return| x"), std::u16string(u"    return\n    |x"));
	CHECK_EQ(Enter(u"    passed = 1|"), std::u16string(u"    passed = 1\n    |"));      // a name, not the keyword
	// A colon inside brackets, strings or comments does not indent; a trailing comment does not hide one.
	CHECK_EQ(Enter(u"d = {'a':|"), std::u16string(u"d = {'a':\n     |"));              // aligned after the open brace
	CHECK_EQ(Enter(u"s = 'if x:|'"), std::u16string(u"s = 'if x:\n|'"));
	CHECK_EQ(Enter(u"# if x:|"), std::u16string(u"# if x:\n|"));
	CHECK_EQ(Enter(u"if x:  # why|"), std::u16string(u"if x:  # why\n    |"));
	CHECK_EQ(Enter(u"x = a[1:|]"), std::u16string(u"x = a[1:\n      |]"));             // aligned after '['
	// Brackets: aligned after the opener when text follows it, hanging otherwise; closing ones refer to the opener.
	CHECK_EQ(Enter(u"x = foo(a,|"), std::u16string(u"x = foo(a,\n        |"));
	CHECK_EQ(Enter(u"x = foo(|"), std::u16string(u"x = foo(\n    |"));
	CHECK_EQ(Enter(u"x = foo(\n    a,\n)|"), std::u16string(u"x = foo(\n    a,\n)\n|"));
	CHECK_EQ(Enter(u"    if (a and\n            b):|"), std::u16string(u"    if (a and\n            b):\n        |"));
	CHECK_EQ(Enter(u"    return foo(\n        a)|"), std::u16string(u"    return foo(\n        a)\n|"));
	// Backslash joins.
	CHECK_EQ(Enter(u"x = a + \\|"), std::u16string(u"x = a + \\\n    |"));
	CHECK_EQ(Enter(u"if a and \\\n   b:|"), std::u16string(u"if a and \\\n   b:\n    |"));
	// Inside a triple-quoted string the line's indentation is kept.
	CHECK_EQ(Enter(u"s = \"\"\"\n    text:|"), std::u16string(u"s = \"\"\"\n    text:\n    |"));
	// Tabs.
	IndentStyle Tabs;
	Tabs.bUseTabs = true;
	Tabs.TabSize = 4;
	CHECK_EQ(Enter(u"\tif x:|", Tabs), std::u16string(u"\tif x:\n\t\t|"));
	IndentStyle Two;
	Two.Width = 2;
	CHECK_EQ(Enter(u"  def f():|", Two), std::u16string(u"  def f():\n    |"));
	// One batch, one range: the newline and its indent are one undo group (§4.2 R3).
	TextBuffer B(u"def f():");
	PyLexer L;
	const BuiltEdit E = dream::SmartNewline(B.Snapshot(), L, 8, 8, IndentStyle());
	CHECK_EQ(E.Batch.Ranges.size(), 1u);
	CHECK_EQ(E.Batch.Origin, dream::EditOrigin::Typing);
}

TEST(EditBuilders_SmartBackspace)   // EDT-03 Backspace to the tab stop; EDT-05 empty pairs
{
	CHECK_EQ(Backspace(u"        |x"), std::u16string(u"    |x"));
	CHECK_EQ(Backspace(u"      |"), std::u16string(u"    |"));
	CHECK_EQ(Backspace(u"    |"), std::u16string(u"|"));
	CHECK_EQ(Backspace(u" |"), std::u16string(u"|"));
	CHECK_EQ(Backspace(u"  \t|"), std::u16string(u"<none>"));                         // a tab: the default deletes it
	CHECK_EQ(Backspace(u"x = 1  |"), std::u16string(u"<none>"));                      // not leading blanks
	CHECK_EQ(Backspace(u"|x"), std::u16string(u"<none>"));
	CHECK_EQ(Backspace(u"a\n|b"), std::u16string(u"<none>"));                         // a line start joins as usual
	CHECK_EQ(Backspace(u"x = (|)"), std::u16string(u"x = |"));
	CHECK_EQ(Backspace(u"x = [|]"), std::u16string(u"x = |"));
	CHECK_EQ(Backspace(u"x = {|}"), std::u16string(u"x = |"));
	CHECK_EQ(Backspace(u"x = '|'"), std::u16string(u"x = |"));
	CHECK_EQ(Backspace(u"x = \"|\""), std::u16string(u"x = |"));
	CHECK_EQ(Backspace(u"x = 'a(|)'"), std::u16string(u"<none>"));                    // inside a string
	CHECK_EQ(Backspace(u"x = '''|'"), std::u16string(u"<none>"));
	CHECK_EQ(Backspace(u"x = (|]"), std::u16string(u"<none>"));
	IndentStyle Two;
	Two.Width = 2;
	CHECK_EQ(Backspace(u"     |", Two), std::u16string(u"    |"));
}

TEST(EditBuilders_AutoClosePairing)   // EDT-05 M1 pairing; nothing auto-closes inside strings or comments
{
	CHECK_EQ(Type(u"x = |", u'('), std::u16string(u"x = (|)"));
	CHECK_EQ(Type(u"x = |", u'['), std::u16string(u"x = [|]"));
	CHECK_EQ(Type(u"x = |", u'{'), std::u16string(u"x = {|}"));
	CHECK_EQ(Type(u"print(|)", u'"'), std::u16string(u"print(\"|\")"));
	CHECK_EQ(Type(u"f(|)", u'('), std::u16string(u"f((|))"));
	CHECK_EQ(Type(u"x = |abc", u'('), std::u16string(u"<none>"));                     // an identifier follows
	CHECK_EQ(Type(u"x = |", u'\''), std::u16string(u"x = '|'"));
	CHECK_EQ(Type(u"x = f|", u'\''), std::u16string(u"x = f'|'"));                    // string prefixes
	CHECK_EQ(Type(u"x = Rb|", u'"'), std::u16string(u"x = Rb\"|\""));
	CHECK_EQ(Type(u"x = don|", u'\''), std::u16string(u"<none>"));                    // a word, not a prefix
	CHECK_EQ(Type(u"x = elf|", u'\''), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"x = ''|", u'\''), std::u16string(u"x = '''|'''"));                // the third quote: a triple pair
	CHECK_EQ(Type(u"x = 'abc|", u'\''), std::u16string(u"<none>"));                   // closes an open string
	CHECK_EQ(Type(u"x = '|'", u'\''), std::u16string(u"<none>"));
	// Inside strings and comments: nothing.
	CHECK_EQ(Type(u"s = 'ab|c'", u'('), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"s = 'ab|'", u'['), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"s = \"a|\"", u'\''), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"# note |", u'('), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"x = 1  # |", u'"'), std::u16string(u"<none>"));
	CHECK_EQ(Type(u"s = \"\"\"\n  doc |", u'('), std::u16string(u"<none>"));          // an open triple-quoted string
	CHECK_EQ(Type(u"s = f'{x|}'", u'('), std::u16string(u"s = f'{x(|)}'"));           // an f-string field is code
	CHECK_EQ(Type(u"s = f'{x} |'", u'('), std::u16string(u"<none>"));                 // its literal part is not
	// A selection: wrapping is EDT-05's M2 row.
	CHECK_EQ(Type(u"x = <abc>", u'('), std::u16string(u"<none>"));
	// Other characters are not paired.
	CHECK_EQ(Type(u"x = |", u')'), std::u16string(u"<none>"));
}

TEST(EditBuilders_TypeOverClosing)
{
	TextBuffer B(u"f()");
	BuiltEdit E = dream::TypeOverClosing(B.Snapshot(), 2, u')');
	CHECK(E.bMoveOnly);
	CHECK(E.Batch.Ranges.empty());
	CHECK_EQ(E.Caret, 3);
	CHECK(dream::TypeOverClosing(B.Snapshot(), 2, u']').IsEmpty());
	CHECK(dream::TypeOverClosing(B.Snapshot(), 3, u')').IsEmpty());
	CHECK(dream::TypeOverClosing(B.Snapshot(), 1, u'(').IsEmpty());
}

TEST(EditBuilders_DuplicateLineOrSelection)   // EDT-04 Ctrl+D
{
	CHECK_EQ(Duplicate(u"a\nb|c\nd"), std::u16string(u"a\nbc\nb|c\nd"));
	CHECK_EQ(Duplicate(u"x|"), std::u16string(u"x\nx|"));
	CHECK_EQ(Duplicate(u"|"), std::u16string(u"\n|"));
	CHECK_EQ(Duplicate(u"a<bc>d"), std::u16string(u"abc<bc>d"));
	CHECK_EQ(Duplicate(u"<a\nb\n>c"), std::u16string(u"a\nb\n<a\nb\n>c"));
	TextBuffer B(u"x");
	CHECK_EQ(dream::DuplicateLineOrSelection(B.Snapshot(), 0, 0).Batch.Origin, dream::EditOrigin::Command);
}

TEST(EditBuilders_DetectIndent)   // §4.5 indent detection
{
	CHECK_EQ(Detect(u"def f():\n  if x:\n    y()\n  z()\n").Width, 2);
	CHECK_EQ(Detect(u"def f():\n    if x:\n        y()\n    z()\n").Width, 4);
	const IndentStyle T = Detect(u"def f():\n\tif x:\n\t\ty()\n\tz()\n");
	CHECK(T.bUseTabs);
	CHECK(!Detect(u"def f():\n    x()\n    y()\n").bUseTabs);
	CHECK(Detect(u"x = 1\n") == IndentStyle());                     // too few indented lines: the fallback
	// Continuation lines inside brackets and strings do not count.
	CHECK_EQ(Detect(u"x = foo(a,\n        b)\ndef f():\n  y()\n  if z:\n    w()\n").Width, 2);
	CHECK(dream::MakeIndent(6, T) == u"\t  ");
	CHECK(dream::MakeIndent(6, IndentStyle()) == u"      ");
}
