// SPDX-License-Identifier: Apache-2.0
// Dream core: the theme JSON format (PO-04, T-VIEW-015). Data only.
#include "dream/ThemeFormat.h"

#include "dream/Json.h"

#include <utility>

namespace dream::theme_detail
{
	/** Slot keys in EDreamThemeSlot order: chrome, panes, output line kinds, syntax kinds. */
	constexpr std::array<std::string_view, theme::SlotCount> SlotKeys = {
		"chrome.background",
		"chrome.border",
		"chrome.text",
		"chrome.textMuted",
		"chrome.hover",
		"chrome.selected",
		"chrome.accent",
		"chrome.danger",
		"output.background",
		"editor.background",
		"gutter.background",
		"gutter.text",
		"editor.currentLine",
		"editor.selection",
		"editor.findMatch",
		"editor.findCurrent",
		"editor.caret",
		"output.echo",
		"output.stdout",
		"output.stderr",
		"output.result",
		"output.warning",
		"output.error",
		"output.notice",
		"output.info",
		"output.link",
		"output.header",
		"syntax.normal",
		"syntax.keyword",
		"syntax.builtin",
		"syntax.self",
		"syntax.definition",
		"syntax.decorator",
		"syntax.number",
		"syntax.string",
		"syntax.docstring",
		"syntax.escape",
		"syntax.operator",
		"syntax.comment",
		"syntax.cell",
		"syntax.error",
		"syntax.warning",     // (lang) PO-06 F4: the warning squiggle
	};

	int HexDigit(char C)
	{
		if (C >= '0' && C <= '9')
		{
			return C - '0';
		}
		if (C >= 'a' && C <= 'f')
		{
			return C - 'a' + 10;
		}
		if (C >= 'A' && C <= 'F')
		{
			return C - 'A' + 10;
		}
		return -1;
	}

	bool HexByte(std::string_view Text, size_t At, uint8_t& Out)
	{
		const int Hi = HexDigit(Text[At]);
		const int Lo = HexDigit(Text[At + 1]);
		if (Hi < 0 || Lo < 0)
		{
			return false;
		}
		Out = static_cast<uint8_t>(Hi * 16 + Lo);
		return true;
	}

	void AppendHexByte(std::string& Out, uint8_t Byte)
	{
		constexpr const char* Digits = "0123456789ABCDEF";
		Out.push_back(Digits[Byte >> 4]);
		Out.push_back(Digits[Byte & 0x0F]);
	}

	/** The top-level keys this version reads; anything else is reported as unknown. */
	bool IsKnownTopKey(std::string_view Key)
	{
		return Key == "format" || Key == "version" || Key == "id" || Key == "name" || Key == "base" || Key == "light"
			|| Key == "colors";
	}

	/** A string field: missing leaves Out alone, a non-string is reported. */
	void ReadString(const json::Value& Root, std::string_view Key, std::string& Out, theme::ParseReport* Report)
	{
		const json::Value* Field = Root.Find(Key);
		if (Field == nullptr)
		{
			return;
		}
		if (!Field->IsString())
		{
			if (Report != nullptr)
			{
				Report->InvalidValues.push_back(std::string(Key) + ": not a string");
			}
			return;
		}
		Out = Field->AsString();
	}
}

namespace dream::theme
{
	std::string_view SlotKey(int Slot)
	{
		return Slot >= 0 && Slot < SlotCount ? theme_detail::SlotKeys[static_cast<size_t>(Slot)] : std::string_view();
	}

	int FindSlot(std::string_view Key)
	{
		for (int Slot = 0; Slot < SlotCount; ++Slot)
		{
			if (theme_detail::SlotKeys[static_cast<size_t>(Slot)] == Key)
			{
				return Slot;
			}
		}
		return -1;
	}

	bool ParseColor(std::string_view Text, Rgba& Out)
	{
		if ((Text.size() != 7 && Text.size() != 9) || Text[0] != '#')
		{
			return false;
		}
		Rgba Parsed;
		if (!theme_detail::HexByte(Text, 1, Parsed.R) || !theme_detail::HexByte(Text, 3, Parsed.G)
			|| !theme_detail::HexByte(Text, 5, Parsed.B))
		{
			return false;
		}
		Parsed.A = 255;
		if (Text.size() == 9 && !theme_detail::HexByte(Text, 7, Parsed.A))
		{
			return false;
		}
		Out = Parsed;
		return true;
	}

	std::string FormatColor(const Rgba& Color)
	{
		std::string Out;
		Out.reserve(9);
		Out.push_back('#');
		theme_detail::AppendHexByte(Out, Color.R);
		theme_detail::AppendHexByte(Out, Color.G);
		theme_detail::AppendHexByte(Out, Color.B);
		if (Color.A != 255)
		{
			theme_detail::AppendHexByte(Out, Color.A);
		}
		return Out;
	}

	bool Parse(std::string_view Utf8, Document& Out, ParseReport* OutReport, std::string* OutError)
	{
		Out = Document();
		if (OutReport != nullptr)
		{
			*OutReport = ParseReport();
		}
		auto Fail = [&](std::string Why)
		{
			Out = Document();
			if (OutError != nullptr)
			{
				*OutError = std::move(Why);
			}
			return false;
		};

		json::ParseOptions Options;
		Options.MaxBytes = 1u << 20;    // a theme is a few kilobytes; a megabyte is not a theme
		Options.MaxDepth = 8;
		json::Value Root;
		std::string JsonError;
		if (!json::Parse(Utf8, Root, &JsonError, Options))
		{
			return Fail("not valid JSON: " + JsonError);
		}
		if (!Root.IsObject())
		{
			return Fail("not a JSON object");
		}
		if (const json::Value* Format = Root.Find("format"))
		{
			if (!Format->IsString() || (Format->AsString() != FormatName && Format->AsString() != LegacyFormatName))
			{
				return Fail("\"format\" is not \"dream-theme\"");
			}
			if (OutReport != nullptr)
			{
				OutReport->bHasFormat = true;
			}
		}
		int64_t Version = FormatVersion;
		if (const json::Value* VersionField = Root.Find("version"))
		{
			if (!VersionField->IsInteger() || VersionField->AsInt() < 1)
			{
				return Fail("\"version\" is not a positive integer");
			}
			Version = VersionField->AsInt();
		}
		if (OutReport != nullptr)
		{
			OutReport->Version = Version;
		}

		theme_detail::ReadString(Root, "id", Out.Id, OutReport);
		theme_detail::ReadString(Root, "name", Out.Name, OutReport);
		theme_detail::ReadString(Root, "base", Out.Base, OutReport);
		if (const json::Value* Light = Root.Find("light"))
		{
			if (Light->GetKind() == json::Kind::Bool)
			{
				Out.bLight = Light->AsBool();
			}
			else if (OutReport != nullptr)
			{
				OutReport->InvalidValues.push_back("light: not true or false");
			}
		}
		for (const json::Value::Member& Member : Root.Members())
		{
			if (!theme_detail::IsKnownTopKey(Member.Key) && OutReport != nullptr)
			{
				OutReport->UnknownKeys.push_back(Member.Key);
			}
		}

		if (const json::Value* Colors = Root.Find("colors"))
		{
			if (!Colors->IsObject())
			{
				if (OutReport != nullptr)
				{
					OutReport->InvalidValues.push_back("colors: not an object");
				}
			}
			else
			{
				for (const json::Value::Member& Member : Colors->Members())
				{
					const int Slot = FindSlot(Member.Key);
					if (Slot < 0)
					{
						if (OutReport != nullptr)
						{
							OutReport->UnknownKeys.push_back("colors." + Member.Key);
						}
						continue;
					}
					Rgba Color;
					if (!Member.Val.IsString() || !ParseColor(Member.Val.AsString(), Color))
					{
						if (OutReport != nullptr)
						{
							OutReport->InvalidValues.push_back(Member.Key + ": not #RRGGBB or #RRGGBBAA");
						}
						continue;
					}
					Out.Set(Slot, Color);
				}
			}
		}
		if (OutReport != nullptr)
		{
			for (int Slot = 0; Slot < SlotCount; ++Slot)
			{
				if (!Out.Present[static_cast<size_t>(Slot)])
				{
					OutReport->MissingSlots.emplace_back(SlotKey(Slot));
				}
			}
		}
		return true;
	}

	std::string Serialize(const Document& Doc)
	{
		json::Value Root = json::Value::MakeObject();
		Root.Set("format", json::Value::MakeString(std::string(FormatName)));
		Root.Set("version", json::Value::MakeInt(FormatVersion));
		Root.Set("id", json::Value::MakeString(Doc.Id));
		Root.Set("name", json::Value::MakeString(Doc.Name));
		if (!Doc.Base.empty())
		{
			Root.Set("base", json::Value::MakeString(Doc.Base));
		}
		Root.Set("light", json::Value::MakeBool(Doc.bLight));
		json::Value Colors = json::Value::MakeObject();
		for (int Slot = 0; Slot < SlotCount; ++Slot)
		{
			Colors.Set(SlotKey(Slot), json::Value::MakeString(FormatColor(Doc.Colors[static_cast<size_t>(Slot)])));
		}
		Root.Set("colors", std::move(Colors));
		std::string Out = json::Serialize(Root, /*bPretty=*/true);
		Out.push_back('\n');
		return Out;
	}

	void FillMissing(Document& Doc, const Document& Base)
	{
		for (size_t Slot = 0; Slot < static_cast<size_t>(SlotCount); ++Slot)
		{
			if (!Doc.Present[Slot])
			{
				Doc.Colors[Slot] = Base.Colors[Slot];
				Doc.Present[Slot] = true;
			}
		}
	}
}
