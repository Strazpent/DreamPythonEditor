// SPDX-License-Identifier: Apache-2.0
// Dream core: UTF-8 / UTF-16 / code point helpers (TDD §4.1.2, §4.3.1, §6.1). Pure functions, any thread.
#pragma once

#include "dream/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace dream::utf
{
	inline constexpr char32_t ReplacementChar = 0xFFFD;

	inline bool IsHighSurrogate(char16_t C) { return C >= 0xD800 && C <= 0xDBFF; }
	inline bool IsLowSurrogate(char16_t C) { return C >= 0xDC00 && C <= 0xDFFF; }

	/**
	 * Decodes the code point that starts at Index. A lone surrogate decodes as itself (lexer and column math
	 * need to see it); OutUnits receives 1 or 2. Index must be < Text.size().
	 */
	DREAMCORE_API char32_t DecodeAt(std::u16string_view Text, size_t Index, size_t& OutUnits);

	/**
	 * Strict UTF-8 -> UTF-16. Overlong forms, encoded surrogates, values above U+10FFFF and truncated sequences
	 * become U+FFFD, one per maximal invalid subpart (TDD §4.13 step 5). Returns false when any replacement was made.
	 */
	DREAMCORE_API bool Utf8ToUtf16(std::string_view In, std::u16string& Out);

	/** The same, also appending the offset in Out of every U+FFFD it inserted (not of U+FFFD already in the input). */
	DREAMCORE_API bool Utf8ToUtf16(std::string_view In, std::u16string& Out, std::vector<Offset>& OutReplacementAt);

	/** UTF-16 -> UTF-8. Lone surrogates become U+FFFD (EF BF BD), as _dream.write requires (TDD §6.1). Appends to Out. */
	DREAMCORE_API void Utf16ToUtf8(std::u16string_view In, std::string& Out);

	/** Appends the UTF-8 encoding of one code point (a surrogate code point is written as U+FFFD). */
	DREAMCORE_API void AppendUtf8(char32_t CodePoint, std::string& Out);

	/**
	 * Column conversions on one line (no '\n' inside). Python's ast and co_positions count UTF-8 bytes (TDD §4.1.2);
	 * the model counts UTF-16 units. Out-of-range input clamps to the line end (TDD §4.1.4 "stale column").
	 */
	DREAMCORE_API int32_t Utf8ColToUtf16(std::u16string_view Line, int32_t ByteCol);
	DREAMCORE_API int32_t Utf16ColToUtf8(std::u16string_view Line, int32_t Col16);
	DREAMCORE_API int32_t CodepointColToUtf16(std::u16string_view Line, int32_t CodepointCol);
	DREAMCORE_API int32_t Utf16ColToCodepoint(std::u16string_view Line, int32_t Col16);

	/**
	 * Python identifier classes (XID_Start plus '_', XID_Continue) with the Unicode 14.0.0 tables of the engine's
	 * CPython 3.11.8, so IsIdentifier equals str.isidentifier() (TDD §4.3.1, T-LEX-004). The tables are generated
	 * into Private/ by dream_tools/gen_unicode_tables.py on the engine's python.exe -I.
	 */
	DREAMCORE_API bool IsIdentifierStart(char32_t CodePoint);
	DREAMCORE_API bool IsIdentifierContinue(char32_t CodePoint);
	DREAMCORE_API bool IsIdentifier(std::u16string_view Text);

	/** Python's str.isspace() subset that the tokenizer treats as indentation or separators: ' ', '\t', '\f'. */
	inline bool IsPyBlank(char16_t C) { return C == u' ' || C == u'\t' || C == u'\f'; }
}
