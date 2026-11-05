// SPDX-License-Identifier: Apache-2.0
// Dream core: bytes <-> model text for files (TDD §4.13, BUF-06). Pure functions on byte strings; DreamEditor does the I/O.
// Any thread (files over 1 MB decode on a worker, §4.13 step 1).
// (slice A choice) Built-in codecs utf-8, utf-8-sig, utf-16 (BOM), latin-1, ascii and cp1252 with Python's aliases;
// any other cookie opens read-only. Progressive (>10 MB) and mapped (>50 MB) loading are slice B; slice A refuses
// files over 50 MB to edit and the shell opens files only under 1 MB in plain mode (§4.15).
#pragma once

#include "dream/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	struct ChangeSet;
}

namespace dream::codec
{
	enum class Encoding : uint8_t
	{
		Utf8,
		Utf16LE,
		Utf16BE,
		Latin1,
		Ascii,
		Cp1252,
	};

	enum class Eol : uint8_t
	{
		Lf,
		CrLf,
		Cr,
	};

	/** A line whose terminator differs from the dominant style; kept so an unedited round trip is byte-identical (§4.13 step 6). */
	struct EolException
	{
		LineNo Line = 0;               // 0-based line in the decoded (LF-only) text
		Eol Style = Eol::Lf;
	};

	enum class Refusal : uint8_t
	{
		None,
		Binary,                        // NUL in the first 8 KB without a UTF-16 BOM (D23)
		Utf32,                         // UTF-32 BOM
		TooLarge,                      // over the caller's size limit
	};

	/** File metadata that a save re-emits (journal BUF_OPEN keeps encoding, bom and eol, §10.2.2). */
	struct FileFormat
	{
		Encoding Enc = Encoding::Utf8;
		bool bBom = false;
		Eol Dominant = Eol::Lf;
		bool bMixed = false;
		std::vector<EolException> Exceptions;
		bool bFinalNewline = true;     // the file ended with a terminator
		std::string CookieName;        // the PEP 263 name as written, empty when none
	};

	struct DecodeResult
	{
		Refusal Refused = Refusal::None;
		std::u16string Text;           // LF-only
		FileFormat Format;
		bool bReadOnly = false;        // invalid UTF-8, unsupported cookie, BOM/cookie conflict (§4.13 steps 4-5)
		std::string ReadOnlyReason;    // user-facing English, e.g. "Encoding x is not supported for editing"
		std::vector<Offset> ReplacementAt; // offsets of U+FFFD markers inserted for invalid input
	};

	/** Decodes file bytes: BOM, then the PEP 263 cookie on line 1 or 2, then UTF-8 (§4.13 step 4). */
	DREAMCORE_API DecodeResult Decode(std::string_view Bytes, uint64_t MaxBytes = 50ull << 20);

	/**
	 * "Reopen with encoding..." (§4.13 step 5, T-ENC-003): decodes Bytes with Enc, ignoring any cookie. A BOM that matches
	 * Enc is honoured (and recorded in Format); invalid input still gets U+FFFD markers and opens read-only.
	 */
	DREAMCORE_API DecodeResult DecodeWith(std::string_view Bytes, Encoding Enc, uint64_t MaxBytes = 50ull << 20);

	/**
	 * Maps a codec name as Python's codecs.lookup does for the built-in codecs (encodings.normalize_encoding, then the
	 * aliases table). False for any other codec, and for the BOM-dependent "utf-16". bOutSig is set for utf-8-sig.
	 * (Decode first applies tokenize's _get_normal_name to a cookie, which turns "utf-8-sig" into "utf-8".)
	 */
	DREAMCORE_API bool LookupEncoding(std::string_view Name, Encoding& OutEnc, bool& bOutSig);

	/** The PEP 263 cookie per tokenize.detect_encoding (§4.13 step 4), the name as written; empty when none. */
	DREAMCORE_API std::string DetectCookie(std::string_view Bytes);

	/** Defaults for a new file (BUF-06): UTF-8, no BOM, LF. */
	DREAMCORE_API FileFormat NewFileFormat();

	/**
	 * Encodes model text with Format, re-emitting the BOM, the dominant terminator and the exceptions (§4.13 Save).
	 * Returns false, with the first unrepresentable offset in OutBadOffset, when the encoding cannot hold a character.
	 */
	DREAMCORE_API bool Encode(std::u16string_view Text, const FileFormat& Format, std::string& Out, Offset* OutBadOffset = nullptr);

	/**
	 * Moves Format's exception terminators through one applied edit (§4.13 step 6, T-ENC-001), so an unedited line keeps
	 * its terminator when lines are inserted or deleted above it: a terminator the edit removed is dropped (that line gets
	 * the dominant style), one after the edit moves with its line. Call after every TextBuffer change of a buffer whose
	 * Format came from Decode; a whole-text replace (TextBuffer::SetText) drops them all. Updates bMixed.
	 * (slice A choice) Line numbers stand in for the LineStart anchors of §4.13; they could move onto LineStart anchors of
	 * the buffer's dream::AnchorSet (TextBuffer::Anchors) without changing the rule.
	 */
	DREAMCORE_API void MapEolExceptions(FileFormat& Format, const ChangeSet& Changes);

	/** Python-style codec names: "utf-8", "utf-8-sig", "utf-16-le", "latin-1", "ascii", "cp1252". */
	DREAMCORE_API const char* EncodingName(Encoding Enc);
}
