// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::DiffSingleRange (TDD §4.6, A4) and dream::codec (§4.13: T-ENC-001..004, T-ENC-006 shapes), with the
// exception terminators mapped through edits (T-ENC-001 "after edits elsewhere").
#include "dream_test.h"
#include "test_util.h"

#include "dream/Diff.h"
#include "dream/TextBuffer.h"
#include "dream/TextCodec.h"

#include <string>

using dream::EditRange;
using dream_test_util::Rng;
using dream_test_util::U;
namespace codec = dream::codec;

namespace
{
	std::u16string ApplyRange(std::u16string Text, const EditRange& R)
	{
		Text.replace(static_cast<size_t>(R.At), static_cast<size_t>(R.DeleteLen), R.Insert);
		return Text;
	}

	std::string RoundTripBytes(std::string_view Bytes)
	{
		const codec::DecodeResult D = codec::Decode(Bytes);
		std::string Out;
		if (!codec::Encode(D.Text, D.Format, Out))
		{
			return "<encode failed>";
		}
		return Out;
	}
}

TEST(Diff_SingleRangeBasics)
{
	EditRange R = dream::DiffSingleRange(u"hello world", u"hello brave world");
	CHECK_EQ(R.At, 6);
	CHECK_EQ(R.DeleteLen, 0);
	CHECK(R.Insert == u"brave ");
	R = dream::DiffSingleRange(u"abc", u"abc");
	CHECK_EQ(R.At, 3);
	CHECK_EQ(R.DeleteLen, 0);
	CHECK(R.Insert.empty());
	R = dream::DiffSingleRange(u"aaaa", u"aa");
	CHECK_EQ(R.DeleteLen, 2);
	CHECK(ApplyRange(u"aaaa", R) == u"aa");
	R = dream::DiffSingleRange(u"", u"x\ny");
	CHECK_EQ(R.At, 0);
	CHECK(R.Insert == u"x\ny");
}

TEST(Diff_NeverSplitsSurrogatePairs)
{
	// Same high surrogate, different low ones: the range must start on the high half.
	const std::u16string A = U("x\xF0\x9F\x98\x80y");   // U+1F600
	const std::u16string B = U("x\xF0\x9F\x98\x81y");   // U+1F601
	const EditRange R = dream::DiffSingleRange(A, B);
	CHECK_EQ(R.At, 1);
	CHECK_EQ(R.DeleteLen, 2);
	CHECK(ApplyRange(A, R) == B);
	// A pair alone that differs only in its low half: the prefix backs off to the high half.
	const std::u16string C = U("\xF0\x9F\x98\x80");     // U+1F600: D83D DE00
	const std::u16string D = U("\xF0\x9F\x9A\x80");     // U+1F680: D83D DE80
	const EditRange R2 = dream::DiffSingleRange(C, D);
	CHECK(ApplyRange(C, R2) == D);
	CHECK_EQ(R2.At, 0);
	CHECK_EQ(R2.DeleteLen, 2);
	// Different high halves, same low half: the suffix must not start on the low half.
	const std::u16string E = U("\xF0\x90\x98\x80");     // U+10600: D801 DE00
	const EditRange R3 = dream::DiffSingleRange(C, E);
	CHECK(ApplyRange(C, R3) == E);
	CHECK_EQ(R3.At, 0);
	CHECK_EQ(R3.DeleteLen, 2);
}

TEST(Diff_RandomPairsReconstruct)
{
	Rng Rand(99);
	for (int I = 0; I < 2000; ++I)
	{
		std::u16string A;
		const int Len = static_cast<int>(Rand.Below(12));
		for (int K = 0; K < Len; ++K)
		{
			A.push_back(static_cast<char16_t>(u'a' + Rand.Below(3)));
		}
		std::u16string B = A;
		const size_t At = Rand.Below(B.size() + 1);
		B.insert(At, std::u16string(Rand.Below(3), u'b'));
		if (!B.empty() && Rand.OneIn(2))
		{
			B.erase(Rand.Below(B.size()), 1);
		}
		const EditRange R = dream::DiffSingleRange(A, B);
		CHECK(ApplyRange(A, R) == B);
		dream::TextBuffer TA(A);
		dream::TextBuffer TB(B);
		const EditRange R2 = dream::DiffSingleRange(TA.Snapshot(), TB.Snapshot());
		CHECK_EQ(R2.At, R.At);
		CHECK_EQ(R2.DeleteLen, R.DeleteLen);
	}
}

TEST(Codec_UnEditedRoundTripsAreByteIdentical)
{
	const std::string Cases[] = {
		"print('x')\n",
		std::string("\xEF\xBB\xBF") + "a = 1\r\nb = 2\r\n",
		"a\rb\rc",
		"one\r\ntwo\nthree\r\nfour\rfive\r\n",              // mixed: CRLF dominant, LF and CR exceptions
		"no final newline",
		"",
		"\n\n\r\n",
		std::string("\xFF\xFE", 2) + std::string("a\0\r\0\n\0b\0", 8),     // UTF-16 LE with BOM
		std::string("\xFE\xFF", 2) + std::string("\0a\0\n\0\xE9", 6),      // UTF-16 BE with BOM
		std::string("# -*- coding: latin-1 -*-\ncaf\xE9\n"),
		std::string("#!/usr/bin/env python\n# vim: set fileencoding=cp1252 :\nx = '\x80'\n"),
	};
	for (const std::string& C : Cases)
	{
		const codec::DecodeResult D = codec::Decode(C);
		CHECK(D.Refused == codec::Refusal::None);
		CHECK(!D.bReadOnly);
		CHECK(D.Text.find(u'\r') == std::u16string::npos);
		CHECK_EQ(RoundTripBytes(C), C);
	}
	const codec::DecodeResult Mixed = codec::Decode("one\r\ntwo\nthree\r\nfour\rfive\r\n");
	CHECK(Mixed.Format.Dominant == codec::Eol::CrLf);
	CHECK(Mixed.Format.bMixed);
	REQUIRE_EQ(Mixed.Format.Exceptions.size(), 2u);
	CHECK_EQ(Mixed.Format.Exceptions[0].Line, 1);
	CHECK(Mixed.Format.Exceptions[0].Style == codec::Eol::Lf);
	CHECK(Mixed.Format.Exceptions[1].Style == codec::Eol::Cr);
	const codec::DecodeResult Latin = codec::Decode("# -*- coding: latin-1 -*-\ncaf\xE9\n");
	CHECK(Latin.Format.Enc == codec::Encoding::Latin1);
	CHECK_EQ(Latin.Format.CookieName, std::string("latin-1"));
	CHECK(Latin.Text == U("# -*- coding: latin-1 -*-\ncaf\xC3\xA9\n"));
	CHECK(!codec::Decode("no final newline").Format.bFinalNewline);
}

TEST(Codec_MixedEndingsSurviveEditsElsewhere)   // T-ENC-001: unedited exception lines keep their terminators
{
	const codec::DecodeResult D = codec::Decode("a\r\nb\r\nc\nd\r\n");
	REQUIRE_EQ(D.Format.Exceptions.size(), 1u);
	dream::TextBuffer B(D.Text);
	codec::FileFormat Format = D.Format;
	auto Edit = [&B, &Format](dream::Offset At, int64_t DeleteLen, std::u16string Insert) {
		dream::EditBatch Batch;
		Batch.Ranges.push_back(EditRange{At, DeleteLen, std::move(Insert)});
		codec::MapEolExceptions(Format, B.Apply(Batch));
	};
	auto Save = [&B, &Format]() {
		std::string Out;
		codec::Encode(B.Snapshot().Text(), Format, Out);
		return Out;
	};
	Edit(0, 0, u"new\n");   // a line above the exception
	CHECK_EQ(Save(), std::string("new\r\na\r\nb\r\nc\nd\r\n"));
	Edit(B.Snapshot().LineStart(3), 0, u"x");   // on the exception line, before its terminator
	CHECK_EQ(Save(), std::string("new\r\na\r\nb\r\nxc\nd\r\n"));
	Edit(B.Snapshot().LineStart(1), 2, u"");   // a line above it deleted
	CHECK_EQ(Save(), std::string("new\r\nb\r\nxc\nd\r\n"));
	// Enter at the end of the exception line, as plain mode reflects it (DiffSingleRange keeps the longest prefix): the
	// edited line keeps its terminator and the new line gets the dominant one.
	const std::u16string Before = B.Snapshot().Text();
	dream::EditBatch Enter;
	Enter.Ranges.push_back(dream::DiffSingleRange(Before, u"new\nb\nxc\n\nd\n"));
	codec::MapEolExceptions(Format, B.Apply(Enter));
	CHECK_EQ(Save(), std::string("new\r\nb\r\nxc\n\r\nd\r\n"));
	CHECK(Format.bMixed);
	Edit(B.Snapshot().LineEnd(2), 1, u"");   // its own terminator deleted: the exception goes
	CHECK_EQ(Save(), std::string("new\r\nb\r\nxc\r\nd\r\n"));
	CHECK(Format.Exceptions.empty());
	CHECK(!Format.bMixed);

	// Several ranges in one batch, each mapped in pre-edit coordinates.
	const codec::DecodeResult M = codec::Decode("1\n2\r\n3\n4\r\n5\n");
	REQUIRE_EQ(M.Format.Exceptions.size(), 2u);
	dream::TextBuffer B2(M.Text);
	codec::FileFormat F2 = M.Format;
	dream::EditBatch Two;
	Two.Ranges.push_back(EditRange{4, 2, u""});          // line "3" deleted
	Two.Ranges.push_back(EditRange{0, 0, u"a\nb\n"});    // two lines inserted at the top
	codec::MapEolExceptions(F2, B2.Apply(Two));
	std::string Saved;
	codec::Encode(B2.Snapshot().Text(), F2, Saved);
	CHECK_EQ(Saved, std::string("a\nb\n1\n2\r\n4\r\n5\n"));

	// A whole-text replace (load, recovered text) keeps none; an empty change keeps them all.
	codec::MapEolExceptions(F2, dream::ChangeSet());
	CHECK_EQ(F2.Exceptions.size(), 2u);
	codec::MapEolExceptions(F2, B2.SetText(u"1\n2\n", dream::EditOrigin::Load));
	CHECK(F2.Exceptions.empty());
}

TEST(Codec_CookieDetectionFollowsTokenize)
{
	struct Case
	{
		std::string_view Bytes;
		std::string_view Cookie;
	};
	const Case Cases[] = {
		{"# -*- coding: utf-8 -*-\n", "utf-8"},
		{"# coding=latin-1\n", "latin-1"},
		{"#!/usr/bin/python\n# -*- coding: ascii -*-\n", "ascii"},
		{"\n# coding: cp1252\n", "cp1252"},                       // line 1 blank: line 2 is read
		{"x = 1\n# coding: latin-1\n", ""},                      // line 1 is code: line 2 is not read
		{"#!/x\n\n# coding: latin-1\n", ""},                     // only the first two lines count
		{"# codingX: latin-1\n", ""},
		{"# coding: \n", ""},
		{"# coding:  utf-8 \n", "utf-8"},
		{"  \t# vim: fileencoding=koi8-r\n", "koi8-r"},
		{"# coding: x coding: utf8\n", "x"},
		{"# coding:\xE2\x80\x8Bnone coding=latin1\n", "latin1"}, // a non-name after ':' lets the lazy match go on
		{"\xEF\xBB\xBF# coding: utf-8\n", "utf-8"},
		{"x = '''# coding: latin-1'''\n", ""},
	};
	for (const Case& C : Cases)
	{
		CHECK_EQ(codec::DetectCookie(C.Bytes), std::string(C.Cookie));
	}
	codec::Encoding E = codec::Encoding::Ascii;
	bool bSig = false;
	CHECK(codec::LookupEncoding("UTF8", E, bSig));
	CHECK(E == codec::Encoding::Utf8);
	CHECK(codec::LookupEncoding("utf-8-sig", E, bSig));
	CHECK(bSig);
	CHECK(codec::LookupEncoding("ISO-8859-1", E, bSig));
	CHECK(E == codec::Encoding::Latin1);
	CHECK(codec::LookupEncoding("Windows-1252", E, bSig));
	CHECK(E == codec::Encoding::Cp1252);
	CHECK(codec::LookupEncoding("us-ascii", E, bSig));
	CHECK(E == codec::Encoding::Ascii);
	CHECK(!codec::LookupEncoding("shift_jis", E, bSig));
	CHECK(!codec::LookupEncoding("utf-16", E, bSig));
}

TEST(Codec_ReadOnlyCasesAndRefusals)
{
	// Invalid UTF-8: U+FFFD at exact positions, read-only; reopening as Latin-1 and saving gives the bytes back.
	const std::string Bad = std::string("ok\r\n\xC3(x\xFF\n");
	const codec::DecodeResult D = codec::Decode(Bad);
	CHECK(D.bReadOnly);
	CHECK(!D.ReadOnlyReason.empty());
	REQUIRE_EQ(D.ReplacementAt.size(), 2u);
	CHECK_EQ(D.ReplacementAt[0], 3);   // after the CRLF became one LF
	CHECK_EQ(D.ReplacementAt[1], 6);
	CHECK_EQ(D.Text[3], static_cast<char16_t>(0xFFFD));
	const codec::DecodeResult L1 = codec::DecodeWith(Bad, codec::Encoding::Latin1);
	CHECK(!L1.bReadOnly);
	std::string Saved;
	CHECK(codec::Encode(L1.Text, L1.Format, Saved));
	CHECK_EQ(Saved, Bad);

	const codec::DecodeResult Unsupported = codec::Decode("# coding: shift_jis\nx\n");
	CHECK(Unsupported.bReadOnly);
	CHECK_EQ(Unsupported.ReadOnlyReason, std::string("Encoding shift_jis is not supported for editing"));
	const codec::DecodeResult BomConflict = codec::Decode("\xEF\xBB\xBF# coding: latin-1\n");
	CHECK(BomConflict.bReadOnly);
	CHECK_EQ(BomConflict.ReadOnlyReason, std::string("encoding problem: utf-8"));
	CHECK(codec::Decode("\xEF\xBB\xBF# coding: utf8\n").bReadOnly);     // tokenize compares the normal name
	CHECK(!codec::Decode("\xEF\xBB\xBF# coding: utf-8-sig\n").bReadOnly);
	CHECK(codec::Decode("# caf\xE9\n# coding: latin-1\n").bReadOnly);    // line 1 is not UTF-8

	CHECK(codec::Decode(std::string("ab\0cd", 5)).Refused == codec::Refusal::Binary);
	std::string LateNul(9000, 'a');
	LateNul.push_back('\0');
	const codec::DecodeResult Late = codec::Decode(LateNul);
	CHECK(Late.Refused == codec::Refusal::None);                          // a NUL after 8 KB loads
	CHECK_EQ(Late.Text.back(), u'\0');
	CHECK(codec::Decode(std::string("\xFF\xFE\0\0", 4)).Refused == codec::Refusal::Utf32);
	CHECK(codec::Decode(std::string("\xFF\xFE" "a\0", 4)).Refused == codec::Refusal::None);
	CHECK(codec::Decode("abc", 2).Refused == codec::Refusal::TooLarge);
}

TEST(Codec_EncodeBlocksUnrepresentable)
{
	codec::FileFormat Latin = codec::NewFileFormat();
	Latin.Enc = codec::Encoding::Latin1;
	std::string Out;
	dream::Offset Bad = -1;
	CHECK(!codec::Encode(U("ok\n\xE4\xB8\xAD"), Latin, Out, &Bad));
	CHECK_EQ(Bad, 3);
	const codec::FileFormat New = codec::NewFileFormat();
	CHECK(New.Enc == codec::Encoding::Utf8);
	CHECK(!New.bBom);
	CHECK(New.Dominant == codec::Eol::Lf);
	CHECK(codec::Encode(U("a\n\xE4\xB8\xAD"), New, Out));
	CHECK_EQ(Out, std::string("a\n\xE4\xB8\xAD"));
	codec::FileFormat Crlf = New;
	Crlf.Dominant = codec::Eol::CrLf;
	CHECK(codec::Encode(u"a\nb\n", Crlf, Out));
	CHECK_EQ(Out, std::string("a\r\nb\r\n"));
	codec::FileFormat Cp = New;
	Cp.Enc = codec::Encoding::Cp1252;
	CHECK(codec::Encode(U("\xE2\x82\xAC"), Cp, Out));                     // the euro sign is 0x80 in cp1252
	CHECK_EQ(Out, std::string("\x80"));
	CHECK(!codec::Encode(std::u16string(1, static_cast<char16_t>(0x81)), Cp, Out, &Bad));
	const std::u16string Lone(1, static_cast<char16_t>(0xD800));
	CHECK(!codec::Encode(Lone, New, Out, &Bad));
	CHECK_EQ(std::string(codec::EncodingName(codec::Encoding::Utf16BE)), std::string("utf-16-be"));
}
