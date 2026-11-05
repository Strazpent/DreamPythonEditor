// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::utf (TDD §4.1.2, §4.13 step 5, T-CORE-007, T-LEX-004 spot checks).
#include "dream_test.h"
#include "test_util.h"

#include "dream/Utf.h"

using dream_test_util::U;

namespace
{
	std::u16string Decode(std::string_view Bytes, bool& bClean)
	{
		std::u16string Out;
		bClean = dream::utf::Utf8ToUtf16(Bytes, Out);
		return Out;
	}

	size_t CountFffd(std::u16string_view S)
	{
		size_t N = 0;
		for (char16_t C : S)
		{
			N += C == 0xFFFD ? 1 : 0;
		}
		return N;
	}
}

TEST(Utf_DecodeAtPairsAndLoneSurrogates)
{
	const std::u16string S = U("a\xF0\x9F\x98\x80" "b");
	size_t Units = 0;
	CHECK_EQ(dream::utf::DecodeAt(S, 0, Units), U'a');
	CHECK_EQ(Units, 1u);
	CHECK_EQ(dream::utf::DecodeAt(S, 1, Units), static_cast<char32_t>(0x1F600));
	CHECK_EQ(Units, 2u);
	const std::u16string Lone = {static_cast<char16_t>(0xD800), u'x', static_cast<char16_t>(0xDC00)};
	CHECK_EQ(dream::utf::DecodeAt(Lone, 0, Units), static_cast<char32_t>(0xD800));
	CHECK_EQ(Units, 1u);
	CHECK_EQ(dream::utf::DecodeAt(Lone, 2, Units), static_cast<char32_t>(0xDC00));
}

TEST(Utf_StrictDecodeValid)
{
	bool bClean = false;
	const std::u16string S = Decode("a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80", bClean);
	CHECK(bClean);
	REQUIRE_EQ(S.size(), 5u);
	CHECK_EQ(S[1], static_cast<char16_t>(0xE9));
	CHECK_EQ(S[2], static_cast<char16_t>(0x4E2D));
	CHECK_EQ(S[3], static_cast<char16_t>(0xD83D));
	CHECK_EQ(S[4], static_cast<char16_t>(0xDE00));
}

TEST(Utf_StrictDecodeMaximalSubparts)
{
	struct Case
	{
		std::string_view Bytes;
		size_t Replacements;
		size_t Units;
	};
	const Case Cases[] = {
		{std::string_view("\xC0\xAF", 2), 2, 2},           // overlong lead, then a stray continuation
		{std::string_view("\xE0\x80\x80", 3), 3, 3},       // overlong 3-byte form
		{std::string_view("\xED\xA0\x80", 3), 3, 3},       // an encoded surrogate
		{std::string_view("\xF4\x90\x80\x80", 4), 4, 4},   // above U+10FFFF
		{std::string_view("\xE2\x82", 2), 1, 1},           // truncated: one maximal subpart
		{std::string_view("\xE2\x82\x41", 3), 1, 2},       // truncated, then 'A'
		{std::string_view("\xF0\x9F\x98x", 4), 1, 2},      // truncated 4-byte, then 'x'
		{std::string_view("\xFF", 1), 1, 1},
		{std::string_view("\x80\x80", 2), 2, 2},
	};
	for (const Case& C : Cases)
	{
		bool bClean = true;
		const std::u16string S = Decode(C.Bytes, bClean);
		CHECK(!bClean);
		CHECK_EQ(CountFffd(S), C.Replacements);
		CHECK_EQ(S.size(), C.Units);
	}
	std::u16string Out;
	std::vector<dream::Offset> Marks;
	CHECK(!dream::utf::Utf8ToUtf16(std::string_view("ab\xFF" "c\xEF\xBF\xBD" "d\xC3", 9), Out, Marks));
	REQUIRE_EQ(Marks.size(), 2u);   // the U+FFFD already in the input is not a marker
	CHECK_EQ(Marks[0], 2);
	CHECK_EQ(Marks[1], 6);
}

TEST(Utf_EncodeLoneSurrogateAsReplacement)
{
	const std::u16string S = {u'a', static_cast<char16_t>(0xD800), u'b'};
	std::string Out;
	dream::utf::Utf16ToUtf8(S, Out);
	CHECK_EQ(Out, std::string("a\xEF\xBF\xBD" "b"));
	std::string Cp;
	dream::utf::AppendUtf8(0x110000, Cp);
	CHECK_EQ(Cp, std::string("\xEF\xBF\xBD"));
}

TEST(Utf_ColumnConversions)
{
	// The engine's ast: in "é = 1; y = 2" y has col_offset 8 (UTF-8 bytes); the model column is 7.
	const std::u16string Line = U("\xC3\xA9 = 1; y = 2");
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Line, 8), 7);
	CHECK_EQ(dream::utf::Utf16ColToUtf8(Line, 7), 8);
	const std::u16string Cjk = U("\xE4\xB8\xADx");
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Cjk, 3), 1);
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Cjk, 2), 0);   // inside a character: its start
	const std::u16string Emoji = U("\xF0\x9F\x98\x80x");
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Emoji, 4), 2);
	CHECK_EQ(dream::utf::Utf16ColToCodepoint(Emoji, 2), 1);
	CHECK_EQ(dream::utf::CodepointColToUtf16(Emoji, 1), 2);
	CHECK_EQ(dream::utf::Utf16ColToUtf8(Emoji, 1), 0);   // splitting the pair counts up to its start
	const std::u16string Combining = U("e\xCC\x81x");
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Combining, 3), 2);
	CHECK_EQ(dream::utf::CodepointColToUtf16(Combining, 2), 2);
	// Stale columns clamp to the line end (§4.1.4).
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Line, 999), static_cast<int32_t>(Line.size()));
	CHECK_EQ(dream::utf::CodepointColToUtf16(Line, 999), static_cast<int32_t>(Line.size()));
	CHECK_EQ(dream::utf::Utf8ColToUtf16(Line, -5), 0);
}

TEST(Utf_IdentifierTablesMatchIsIdentifier)
{
	// Golden values from the engine's python.exe -I: chr(c).isidentifier(), ('a' + chr(c)).isidentifier().
	struct Golden
	{
		char32_t C;
		bool bStart;
		bool bContinue;
	};
	const Golden Cases[] = {
		{0x210C, true, true}, {0x2118, true, true}, {0xB7, false, true}, {0x30FB, false, false}, {0x2E2F, false, false},
		{0x1F600, false, false}, {0x4E2D, true, true}, {0x0301, false, true}, {0x0660, false, true}, {0xFF3F, false, true},
		{0x10FFFF, false, false}, {0xE0100, false, true}, {0x1D7CE, false, true}, {U'_', true, true}, {U'9', false, true},
		{U'$', false, false}, {0xD800, false, false},
	};
	for (const Golden& G : Cases)
	{
		CHECK_EQ(dream::utf::IsIdentifierStart(G.C), G.bStart);
		CHECK_EQ(dream::utf::IsIdentifierContinue(G.C), G.bContinue);
	}
	CHECK(dream::utf::IsIdentifier(u"_private1"));
	CHECK(dream::utf::IsIdentifier(U("\xC3\xA9t\xC3\xA9")));
	CHECK(!dream::utf::IsIdentifier(u"1x"));
	CHECK(!dream::utf::IsIdentifier(u""));
	CHECK(!dream::utf::IsIdentifier(u"a-b"));
}
