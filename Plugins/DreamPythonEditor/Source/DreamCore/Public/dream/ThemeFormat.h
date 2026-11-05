// SPDX-License-Identifier: Apache-2.0
// Dream core: the theme JSON format (owner ruling PO-04 "Themes", T-VIEW-015). A theme is a flat set of sRGB colours, one
// per slot, plus an id, a display name, a light flag and the id of the base theme that supplies every slot the file does
// not give. Data only (SEC-06): no I/O; DreamEditor reads and writes the files (Shell/DreamTheme.cpp) and owns the built-in
// palettes. Any thread; values are plain data.
//
// The file:
//   { "format": "dream-theme", "version": 1, "id": "Darcula", "name": "Darcula", "base": "...", "light": false,
//     "colors": { "chrome.background": "#3C3F41", ... } }
// Colours are "#RRGGBB" or "#RRGGBBAA" (sRGB, hex digits in either case on input, upper case on output; the alpha pair is
// written only when it is not FF). "base" is written only when it is set. Every slot is always written, in slot order,
// so Serialize(Parse(Serialize(D))) == Serialize(D) (the lossless round trip of T-VIEW-015).
// Reading is lenient: an unknown key (top level or colour) is reported and ignored, a colour that does not parse is
// reported and left unset, a missing colour is left unset; the caller fills unset slots from the base theme
// (FillMissing). Only text that is not a JSON object, a "format" other than "dream-theme", or a "version" that is not a
// positive integer fail the parse.
#pragma once

#include "dream/Export.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream::theme
{
	/** The number of colour slots. The order is DreamEditor's EDreamThemeSlot order (static_assert there). */
	inline constexpr int SlotCount = 42;   // (lang) PO-06 F4: syntax.warning appended
	/** The format version this code writes; a reader accepts any positive version and ignores what it does not know. */
	inline constexpr int64_t FormatVersion = 1;
	inline constexpr std::string_view FormatName = "dream-theme";
	/** (rename, A246) The format name theme files written before the product was renamed carry; read like FormatName. */
	inline constexpr std::string_view LegacyFormatName = "sumi-theme";

	struct Rgba
	{
		uint8_t R = 0;
		uint8_t G = 0;
		uint8_t B = 0;
		uint8_t A = 255;

		bool operator==(const Rgba&) const = default;
	};

	struct Document
	{
		std::string Id;
		std::string Name;
		/** The theme unset slots come from; empty: the caller's default (Darcula for a dark theme, GitHub Light for light). */
		std::string Base;
		bool bLight = false;
		std::array<Rgba, SlotCount> Colors{};
		/** Parse sets a slot whose key was present with a valid colour; FillMissing sets the rest. Serialize ignores it. */
		std::array<bool, SlotCount> Present{};

		/** Sets a slot and marks it present. Out-of-range slots are ignored. */
		void Set(int Slot, Rgba Color)
		{
			if (Slot >= 0 && Slot < SlotCount)
			{
				Colors[static_cast<size_t>(Slot)] = Color;
				Present[static_cast<size_t>(Slot)] = true;
			}
		}
	};

	/** What a lenient parse ignored or could not use (for a log line; the theme still loads). */
	struct ParseReport
	{
		std::vector<std::string> UnknownKeys;       // top-level keys and colour keys this version does not know
		std::vector<std::string> InvalidValues;     // "key: why" for colours and fields of the wrong type
		std::vector<std::string> MissingSlots;      // colour keys absent from the file (they come from the base)
		int64_t Version = 0;
		/** The file names its format ("format": "dream-theme"). A loader of a themes folder requires it, so another
		 *  program's theme JSON (a VS Code theme) is not read as a mostly-default Dream theme. */
		bool bHasFormat = false;
	};

	/** The JSON key of a slot ("chrome.background"), or "" when Slot is out of range. */
	DREAMCORE_API std::string_view SlotKey(int Slot);
	/** The slot of a key (exact, case-sensitive), or -1. */
	DREAMCORE_API int FindSlot(std::string_view Key);

	/** "#RRGGBB" or "#RRGGBBAA", hex digits in either case, no spaces. */
	DREAMCORE_API bool ParseColor(std::string_view Text, Rgba& Out);
	/** "#RRGGBB" in upper case, or "#RRGGBBAA" when A is not 255. */
	DREAMCORE_API std::string FormatColor(const Rgba& Color);

	/** Parses a theme file (UTF-8). False (and Out cleared) only for the hard failures listed at the top of this file. */
	DREAMCORE_API bool Parse(std::string_view Utf8, Document& Out, ParseReport* OutReport = nullptr, std::string* OutError = nullptr);
	/** The canonical text: two-space indented, fixed key order, every slot, a trailing newline. */
	DREAMCORE_API std::string Serialize(const Document& Doc);
	/** Copies Base's colour into every slot of Doc that is not present, and marks it present. */
	DREAMCORE_API void FillMissing(Document& Doc, const Document& Base);
}
