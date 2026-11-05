// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::theme, the theme JSON format (PO-04 themes, T-VIEW-015): the slot key table (unique, in order, 42
// slots), colour parsing and formatting (#RRGGBB, #RRGGBBAA, case, bad input), the lossless round trip
// Serialize(Parse(Serialize(D))) == Serialize(D) over every 8-bit value in every channel, the lenient reader (unknown
// keys reported and ignored, invalid colours and missing slots left unset and filled from the base), and the hard
// failures (not JSON, not an object, a foreign "format", a bad "version").
#include "dream_test.h"

#include "dream/ThemeFormat.h"

#include <set>
#include <string>

using namespace dream::theme;

namespace
{
	/** A document with every slot set to a distinct colour derived from Seed. */
	Document MakeDoc(int Seed)
	{
		Document Doc;
		Doc.Id = "Test" + std::to_string(Seed);
		Doc.Name = "Test theme " + std::to_string(Seed);
		for (int Slot = 0; Slot < SlotCount; ++Slot)
		{
			const int V = Seed * 41 + Slot;
			Doc.Set(Slot, Rgba{ static_cast<uint8_t>(V * 7), static_cast<uint8_t>(V * 13 + 1), static_cast<uint8_t>(V * 29 + 2),
				static_cast<uint8_t>(Slot % 5 == 0 ? 128 + Slot : 255) });
		}
		return Doc;
	}
}

TEST(Theme_SlotKeysAreUniqueAndOrdered)
{
	CHECK_EQ(SlotCount, 42);
	std::set<std::string> Seen;
	for (int Slot = 0; Slot < SlotCount; ++Slot)
	{
		const std::string Key(SlotKey(Slot));
		CHECK(!Key.empty());
		CHECK(Seen.insert(Key).second);
		CHECK_EQ(FindSlot(Key), Slot);
	}
	CHECK_EQ(std::string(SlotKey(0)), std::string("chrome.background"));
	CHECK_EQ(std::string(SlotKey(7)), std::string("chrome.danger"));
	CHECK_EQ(std::string(SlotKey(16)), std::string("editor.caret"));
	CHECK_EQ(std::string(SlotKey(17)), std::string("output.echo"));
	CHECK_EQ(std::string(SlotKey(26)), std::string("output.header"));
	CHECK_EQ(std::string(SlotKey(27)), std::string("syntax.normal"));
	CHECK_EQ(std::string(SlotKey(40)), std::string("syntax.error"));
	CHECK_EQ(std::string(SlotKey(41)), std::string("syntax.warning"));   // (lang) PO-06 F4
	CHECK(SlotKey(-1).empty());
	CHECK(SlotKey(SlotCount).empty());
	CHECK_EQ(FindSlot("Chrome.Background"), -1);   // keys are case-sensitive
	CHECK_EQ(FindSlot("chrome"), -1);
	CHECK_EQ(FindSlot(""), -1);
}

TEST(Theme_ColorParseAndFormat)
{
	Rgba C;
	CHECK(ParseColor("#3C3F41", C));
	CHECK(C == (Rgba{ 0x3C, 0x3F, 0x41, 0xFF }));
	CHECK(ParseColor("#3c3f41", C));
	CHECK(C == (Rgba{ 0x3C, 0x3F, 0x41, 0xFF }));
	CHECK(ParseColor("#FFFFFF0B", C));
	CHECK(C == (Rgba{ 0xFF, 0xFF, 0xFF, 0x0B }));
	CHECK_EQ(FormatColor(Rgba{ 0x3C, 0x3F, 0x41, 0xFF }), std::string("#3C3F41"));
	CHECK_EQ(FormatColor(Rgba{ 0xFF, 0xFF, 0xFF, 0x0B }), std::string("#FFFFFF0B"));
	CHECK_EQ(FormatColor(Rgba{ 0, 0, 0, 0 }), std::string("#00000000"));

	const Rgba Before{ 1, 2, 3, 4 };
	for (const char* Bad : { "", "#", "3C3F41", "#3C3F4", "#3C3F411", "#3C3F41FF0", "#GG0000", "# 3C3F41", "#3C3F41 ", "rgb(1,2,3)", "#-12345" })
	{
		Rgba Out = Before;
		CHECK(!ParseColor(Bad, Out));
		CHECK(Out == Before);   // untouched on failure
	}

	// Every byte value survives format -> parse in every channel.
	for (int V = 0; V < 256; ++V)
	{
		const uint8_t B = static_cast<uint8_t>(V);
		for (const Rgba In : { Rgba{ B, 0, 0, 255 }, Rgba{ 0, B, 0, 255 }, Rgba{ 0, 0, B, 255 }, Rgba{ 9, 9, 9, B } })
		{
			Rgba Out;
			REQUIRE(ParseColor(FormatColor(In), Out));
			CHECK(Out == In);
		}
	}
}

TEST(Theme_RoundTripIsLossless)
{
	for (int Seed = 0; Seed < 8; ++Seed)
	{
		Document Doc = MakeDoc(Seed);
		Doc.bLight = (Seed % 2) == 1;
		if (Seed % 3 == 0)
		{
			Doc.Base = "Darcula";
		}
		if (Seed == 5)
		{
			Doc.Name = "Ünïcode \"quoted\" \\ name \xE3\x81\x82";   // escapes and non-ASCII survive
		}
		const std::string First = Serialize(Doc);
		Document Back;
		ParseReport Report;
		std::string Error;
		REQUIRE(Parse(First, Back, &Report, &Error));
		CHECK(Error.empty());
		CHECK(Report.UnknownKeys.empty());
		CHECK(Report.InvalidValues.empty());
		CHECK(Report.MissingSlots.empty());
		CHECK_EQ(Report.Version, FormatVersion);
		CHECK(Report.bHasFormat);   // Serialize names its format
		CHECK_EQ(Back.Id, Doc.Id);
		CHECK_EQ(Back.Name, Doc.Name);
		CHECK_EQ(Back.Base, Doc.Base);
		CHECK_EQ(Back.bLight, Doc.bLight);
		for (int Slot = 0; Slot < SlotCount; ++Slot)
		{
			CHECK(Back.Present[static_cast<size_t>(Slot)]);
			CHECK(Back.Colors[static_cast<size_t>(Slot)] == Doc.Colors[static_cast<size_t>(Slot)]);
		}
		CHECK_EQ(Serialize(Back), First);
	}
}

TEST(Theme_SerializeShape)
{
	Document Doc = MakeDoc(1);
	Doc.Id = "Darcula";
	Doc.Name = "Darcula";
	Doc.Set(0, Rgba{ 0x3C, 0x3F, 0x41, 0xFF });
	const std::string Text = Serialize(Doc);
	CHECK(Text.rfind("{\n  \"format\": \"dream-theme\",\n  \"version\": 1,\n  \"id\": \"Darcula\",\n  \"name\": \"Darcula\",\n  \"light\": false,\n  \"colors\": {\n    \"chrome.background\": \"#3C3F41\",", 0) == 0);
	CHECK(Text.find("\"base\"") == std::string::npos);   // written only when set
	CHECK(!Text.empty() && Text.back() == '\n');
	// Every slot key appears once, in slot order.
	size_t At = 0;
	for (int Slot = 0; Slot < SlotCount; ++Slot)
	{
		const size_t Found = Text.find("\"" + std::string(SlotKey(Slot)) + "\"", At);
		REQUIRE(Found != std::string::npos);
		At = Found + 1;
	}
}

TEST(Theme_LenientReaderFallsBackToBase)
{
	const std::string Json = R"({
  "id": "Mine",
  "comment": "made by hand",
  "colors": {
    "chrome.background": "#101010",
    "syntax.keyword": "#ff8800",
    "syntax.sparkle": "#123456",
    "output.error": "red",
    "output.result": 12
  }
})";
	Document Doc;
	ParseReport Report;
	std::string Error;
	REQUIRE(Parse(Json, Doc, &Report, &Error));
	CHECK_EQ(Doc.Id, std::string("Mine"));
	CHECK(Doc.Name.empty());
	CHECK(Doc.Base.empty());
	CHECK(!Doc.bLight);
	CHECK_EQ(Report.Version, FormatVersion);   // no "version": this version
	CHECK(!Report.bHasFormat);                 // no "format": a folder loader refuses it
	CHECK_EQ(Report.UnknownKeys.size(), static_cast<size_t>(2));
	CHECK(Report.UnknownKeys.size() == 2 && Report.UnknownKeys[0] == "comment" && Report.UnknownKeys[1] == "colors.syntax.sparkle");
	CHECK_EQ(Report.InvalidValues.size(), static_cast<size_t>(2));
	CHECK_EQ(Report.MissingSlots.size(), static_cast<size_t>(SlotCount - 2));

	const int Bg = FindSlot("chrome.background");
	const int Kw = FindSlot("syntax.keyword");
	const int Err = FindSlot("output.error");
	CHECK(Doc.Present[static_cast<size_t>(Bg)]);
	CHECK(Doc.Present[static_cast<size_t>(Kw)]);
	CHECK(!Doc.Present[static_cast<size_t>(Err)]);
	CHECK(Doc.Colors[static_cast<size_t>(Kw)] == (Rgba{ 0xFF, 0x88, 0x00, 0xFF }));

	const Document Base = MakeDoc(3);
	FillMissing(Doc, Base);
	for (int Slot = 0; Slot < SlotCount; ++Slot)
	{
		CHECK(Doc.Present[static_cast<size_t>(Slot)]);
		if (Slot != Bg && Slot != Kw)
		{
			CHECK(Doc.Colors[static_cast<size_t>(Slot)] == Base.Colors[static_cast<size_t>(Slot)]);
		}
	}
	CHECK(Doc.Colors[static_cast<size_t>(Bg)] == (Rgba{ 0x10, 0x10, 0x10, 0xFF }));

	// Wrong-typed fields are reported, not fatal; a newer version still loads what it knows.
	Document Typed;
	ParseReport TypedReport;
	REQUIRE(Parse(R"({"version": 7, "id": 3, "name": "N", "light": "yes", "colors": []})", Typed, &TypedReport));
	CHECK(Typed.Id.empty());
	CHECK_EQ(Typed.Name, std::string("N"));
	CHECK(!Typed.bLight);
	CHECK_EQ(TypedReport.Version, static_cast<int64_t>(7));
	CHECK_EQ(TypedReport.InvalidValues.size(), static_cast<size_t>(3));
	CHECK_EQ(TypedReport.MissingSlots.size(), static_cast<size_t>(SlotCount));
}

TEST(Theme_HardFailures)
{
	for (const char* Bad : { "", "not json", "[1,2]", "\"text\"", "{\"format\": \"vscode-theme\"}", "{\"format\": 1}",
		"{\"version\": 0}", "{\"version\": -2}", "{\"version\": 1.5}", "{\"version\": \"1\"}", "{\"colors\": {" })
	{
		Document Doc = MakeDoc(2);
		std::string Error;
		CHECK(!Parse(Bad, Doc, nullptr, &Error));
		CHECK(!Error.empty());
		CHECK(Doc.Id.empty());   // cleared on failure
		CHECK(!Doc.Present[0]);
	}
	// A megabyte of padding is not a theme.
	std::string Huge = "{\"id\": \"x\", \"name\": \"";
	Huge.append(static_cast<size_t>(1u << 20), 'a');
	Huge.append("\"}");
	Document Doc;
	CHECK(!Parse(Huge, Doc));
}

TEST(Theme_LegacyFormatNameReads)
{
	// (rename, A246) A theme exported before the product was renamed names the format "sumi-theme"; it still reads, and
	// a theme written now names "dream-theme".
	Document Doc;
	ParseReport Report;
	std::string Error;
	CHECK(Parse("{\"format\": \"sumi-theme\", \"version\": 1, \"id\": \"Old\", \"name\": \"Old\"}", Doc, &Report, &Error));
	CHECK(Error.empty());
	CHECK_EQ(Doc.Id, std::string("Old"));
	CHECK(Report.bHasFormat);
	CHECK(Serialize(Doc).find("\"format\": \"dream-theme\"") != std::string::npos);
}
