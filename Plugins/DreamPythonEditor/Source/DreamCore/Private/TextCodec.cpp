// SPDX-License-Identifier: Apache-2.0
// Dream core: file bytes <-> model text (TDD §4.13, BUF-06): BOM, PEP 263 cookie per tokenize.detect_encoding, strict
// decoding with U+FFFD markers, line-ending detection with per-line exceptions, and the byte-identical re-encode.
// Deferred (G1): progressive decoding in 4 MB slices for files over 10 MB and the mapped read-only source over 50 MB
// (§4.13); plain mode refuses files over 1 MB (A40).
#include "dream/TextCodec.h"

#include "dream/TextBuffer.h"
#include "dream/Utf.h"

#include <algorithm>
#include <array>
#include <utility>

namespace dream::codec_tables
{
#include "Cp1252Table.inc"
}

namespace dream::codec_detail
{
	using namespace dream::codec;

	constexpr size_t BinaryProbeBytes = 8u << 10;

	struct CookieScan
	{
		std::string Name;              // as written; empty when none
		bool bDecodeError = false;     // a scanned line is not UTF-8 ("invalid or missing encoding declaration")
	};

	/** One line as BytesIO.readline returns it: up to and including '\n'. */
	std::string_view ReadLine(std::string_view Bytes, size_t& Pos)
	{
		if (Pos >= Bytes.size())
		{
			return {};
		}
		const size_t Nl = Bytes.find('\n', Pos);
		const size_t End = Nl == std::string_view::npos ? Bytes.size() : Nl + 1;
		const std::string_view Line = Bytes.substr(Pos, End - Pos);
		Pos = End;
		return Line;
	}

	bool IsCookieNameChar(char C)
	{
		return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.';
	}

	bool IsPyBlankByte(char C)
	{
		return C == ' ' || C == '\t' || C == '\f';
	}

	/** tokenize.cookie_re: ^[ \t\f]*#.*?coding[:=][ \t]*([-\w.]+) (re.ASCII); '.' stops at '\n'. */
	bool MatchCookie(std::string_view Line, std::string& OutName)
	{
		size_t I = 0;
		while (I < Line.size() && IsPyBlankByte(Line[I]))
		{
			++I;
		}
		if (I >= Line.size() || Line[I] != '#')
		{
			return false;
		}
		const size_t Nl = Line.find('\n', I);
		const std::string_view Rest = Line.substr(I + 1, Nl == std::string_view::npos ? std::string_view::npos : Nl - I - 1);
		size_t From = 0;
		for (;;)
		{
			const size_t At = Rest.find("coding", From);
			if (At == std::string_view::npos)
			{
				return false;
			}
			size_t J = At + 6;
			From = At + 1;
			if (J >= Rest.size() || (Rest[J] != ':' && Rest[J] != '='))
			{
				continue;
			}
			++J;
			while (J < Rest.size() && (Rest[J] == ' ' || Rest[J] == '\t'))
			{
				++J;
			}
			const size_t NameStart = J;
			while (J < Rest.size() && IsCookieNameChar(Rest[J]))
			{
				++J;
			}
			if (J > NameStart)
			{
				OutName.assign(Rest.substr(NameStart, J - NameStart));
				return true;
			}
		}
	}

	/** tokenize.blank_re: ^[ \t\f]*(?:[#\r\n]|$). */
	bool MatchBlank(std::string_view Line)
	{
		size_t I = 0;
		while (I < Line.size() && IsPyBlankByte(Line[I]))
		{
			++I;
		}
		return I == Line.size() || Line[I] == '#' || Line[I] == '\r' || Line[I] == '\n';
	}

	bool IsValidUtf8(std::string_view Line)
	{
		std::u16string Scratch;
		return utf::Utf8ToUtf16(Line, Scratch);
	}

	/** tokenize.detect_encoding's find_cookie on line 1, then on line 2 when line 1 is blank or a comment. */
	CookieScan ScanCookie(std::string_view Bytes)
	{
		CookieScan Scan;
		size_t Pos = 0;
		std::string_view First = ReadLine(Bytes, Pos);
		if (First.substr(0, 3) == "\xEF\xBB\xBF")
		{
			First.remove_prefix(3);
		}
		if (First.empty())
		{
			return Scan;
		}
		if (!IsValidUtf8(First))
		{
			Scan.bDecodeError = true;
			return Scan;
		}
		if (MatchCookie(First, Scan.Name))
		{
			return Scan;
		}
		if (!MatchBlank(First))
		{
			return Scan;
		}
		const std::string_view Second = ReadLine(Bytes, Pos);
		if (Second.empty())
		{
			return Scan;
		}
		if (!IsValidUtf8(Second))
		{
			Scan.bDecodeError = true;
			return Scan;
		}
		MatchCookie(Second, Scan.Name);
		return Scan;
	}

	std::string Lower(std::string_view S)
	{
		std::string Out(S);
		for (char& C : Out)
		{
			if (C >= 'A' && C <= 'Z')
			{
				C = static_cast<char>(C - 'A' + 'a');
			}
		}
		return Out;
	}

	/** tokenize._get_normal_name. */
	std::string NormalCookieName(std::string_view Orig)
	{
		std::string Enc = Lower(Orig.substr(0, 12));
		std::replace(Enc.begin(), Enc.end(), '_', '-');
		auto StartsWith = [&Enc](std::string_view P) { return Enc.compare(0, P.size(), P) == 0; };
		if (Enc == "utf-8" || StartsWith("utf-8-"))
		{
			return "utf-8";
		}
		if (Enc == "latin-1" || Enc == "iso-8859-1" || Enc == "iso-latin-1" || StartsWith("latin-1-") || StartsWith("iso-8859-1-")
			|| StartsWith("iso-latin-1-"))
		{
			return "iso-8859-1";
		}
		return std::string(Orig);
	}

	/** encodings.normalize_encoding plus the lower-casing of codecs.lookup. */
	std::string NormalizeCodecName(std::string_view Name)
	{
		std::string Out;
		bool bPunct = false;
		for (char C : Name)
		{
			const bool bAlnum = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9');
			if (bAlnum || C == '.')
			{
				if (bPunct && !Out.empty())
				{
					Out.push_back('_');
				}
				Out.push_back(C >= 'A' && C <= 'Z' ? static_cast<char>(C - 'A' + 'a') : C);
				bPunct = false;
			}
			else
			{
				bPunct = true;
			}
		}
		return Out;
	}

	struct AliasEntry
	{
		std::string_view Alias;
		std::string_view Module;
	};

	// encodings/aliases.py entries whose codec is built in here, from the engine's 3.11.8 (TDD §4.13 step 5).
	inline constexpr AliasEntry Aliases[] = {
		{"1252", "cp1252"}, {"646", "ascii"}, {"8859", "latin_1"}, {"ansi_x3.4_1968", "ascii"}, {"ansi_x3.4_1986", "ascii"},
		{"ansi_x3_4_1968", "ascii"}, {"cp367", "ascii"}, {"cp65001", "utf_8"}, {"cp819", "latin_1"}, {"csascii", "ascii"},
		{"csisolatin1", "latin_1"}, {"ibm367", "ascii"}, {"ibm819", "latin_1"}, {"iso646_us", "ascii"}, {"iso8859", "latin_1"},
		{"iso8859_1", "latin_1"}, {"iso_646.irv_1991", "ascii"}, {"iso_8859_1", "latin_1"}, {"iso_8859_1_1987", "latin_1"},
		{"iso_ir_100", "latin_1"}, {"iso_ir_6", "ascii"}, {"l1", "latin_1"}, {"latin", "latin_1"}, {"latin1", "latin_1"},
		{"u16", "utf_16"}, {"u8", "utf_8"}, {"unicodebigunmarked", "utf_16_be"}, {"unicodelittleunmarked", "utf_16_le"},
		{"us", "ascii"}, {"us_ascii", "ascii"}, {"utf", "utf_8"}, {"utf16", "utf_16"}, {"utf8", "utf_8"}, {"utf8_ucs2", "utf_8"},
		{"utf8_ucs4", "utf_8"}, {"utf_16be", "utf_16_be"}, {"utf_16le", "utf_16_le"}, {"windows_1252", "cp1252"},
	};

	std::string_view FindAlias(std::string_view Name)
	{
		for (const AliasEntry& E : Aliases)
		{
			if (E.Alias == Name)
			{
				return E.Module;
			}
		}
		return {};
	}

	// ---- decoders -----------------------------------------------------------------------------------------

	struct Decoded
	{
		std::u16string Text;
		std::vector<Offset> Marks;
		bool bClean = true;
	};

	void Mark(Decoded& D)
	{
		D.Marks.push_back(static_cast<Offset>(D.Text.size()));
		D.Text.push_back(static_cast<char16_t>(utf::ReplacementChar));
		D.bClean = false;
	}

	void DecodeBytes(std::string_view Bytes, Encoding Enc, Decoded& D)
	{
		switch (Enc)
		{
		case Encoding::Utf8:
			D.bClean = utf::Utf8ToUtf16(Bytes, D.Text, D.Marks);
			return;
		case Encoding::Latin1:
			D.Text.reserve(Bytes.size());
			for (char C : Bytes)
			{
				D.Text.push_back(static_cast<char16_t>(static_cast<uint8_t>(C)));
			}
			return;
		case Encoding::Ascii:
			D.Text.reserve(Bytes.size());
			for (char C : Bytes)
			{
				const uint8_t B = static_cast<uint8_t>(C);
				if (B < 0x80)
				{
					D.Text.push_back(static_cast<char16_t>(B));
				}
				else
				{
					Mark(D);
				}
			}
			return;
		case Encoding::Cp1252:
			D.Text.reserve(Bytes.size());
			for (char C : Bytes)
			{
				const uint8_t B = static_cast<uint8_t>(C);
				if (B >= 0x80 && B < 0xA0)
				{
					const char16_t U = codec_tables::Cp1252High[B - 0x80];
					if (U == 0)
					{
						Mark(D);
					}
					else
					{
						D.Text.push_back(U);
					}
				}
				else
				{
					D.Text.push_back(static_cast<char16_t>(B));
				}
			}
			return;
		case Encoding::Utf16LE:
		case Encoding::Utf16BE:
		{
			const bool bLE = Enc == Encoding::Utf16LE;
			const size_t Units = Bytes.size() / 2;
			D.Text.reserve(Units);
			for (size_t I = 0; I < Units; ++I)
			{
				const uint8_t A = static_cast<uint8_t>(Bytes[I * 2]);
				const uint8_t B = static_cast<uint8_t>(Bytes[I * 2 + 1]);
				const char16_t U = bLE ? static_cast<char16_t>(A | (B << 8)) : static_cast<char16_t>((A << 8) | B);
				D.Text.push_back(U);
			}
			// Lone surrogates are invalid UTF-16 (Python's strict decoder refuses them).
			for (size_t I = 0; I < D.Text.size(); ++I)
			{
				const char16_t U = D.Text[I];
				if (utf::IsHighSurrogate(U) && I + 1 < D.Text.size() && utf::IsLowSurrogate(D.Text[I + 1]))
				{
					++I;
				}
				else if (utf::IsHighSurrogate(U) || utf::IsLowSurrogate(U))
				{
					D.Text[I] = static_cast<char16_t>(utf::ReplacementChar);
					D.Marks.push_back(static_cast<Offset>(I));
					D.bClean = false;
				}
			}
			if (Bytes.size() % 2 != 0)
			{
				Mark(D);   // a truncated last unit
			}
			return;
		}
		}
	}

	/** Counts terminators, picks the dominant style, records exceptions and normalizes to LF; marks are remapped. */
	void NormalizeLineEndings(Decoded& D, FileFormat& Format)
	{
		const std::u16string& In = D.Text;
		size_t Lf = 0;
		size_t CrLf = 0;
		size_t Cr = 0;
		for (size_t I = 0; I < In.size(); ++I)
		{
			if (In[I] == u'\r')
			{
				if (I + 1 < In.size() && In[I + 1] == u'\n')
				{
					++CrLf;
					++I;
				}
				else
				{
					++Cr;
				}
			}
			else if (In[I] == u'\n')
			{
				++Lf;
			}
		}
		Format.Dominant = Eol::Lf;
		if (CrLf > Lf && CrLf >= Cr)
		{
			Format.Dominant = Eol::CrLf;
		}
		else if (Cr > Lf && Cr > CrLf)
		{
			Format.Dominant = Eol::Cr;
		}
		Format.bFinalNewline = In.empty() || In.back() == u'\n' || In.back() == u'\r';
		Format.Exceptions.clear();

		std::u16string Out;
		Out.reserve(In.size());
		size_t NextMark = 0;
		Offset Removed = 0;
		LineNo Line = 0;
		for (size_t I = 0; I < In.size(); ++I)
		{
			while (NextMark < D.Marks.size() && D.Marks[NextMark] == static_cast<Offset>(I))
			{
				D.Marks[NextMark] -= Removed;
				++NextMark;
			}
			const char16_t C = In[I];
			if (C != u'\r' && C != u'\n')
			{
				Out.push_back(C);
				continue;
			}
			Eol Style = Eol::Lf;
			if (C == u'\r')
			{
				if (I + 1 < In.size() && In[I + 1] == u'\n')
				{
					Style = Eol::CrLf;
					++I;
					++Removed;
				}
				else
				{
					Style = Eol::Cr;
				}
			}
			if (Style != Format.Dominant)
			{
				Format.Exceptions.push_back(EolException{Line, Style});
			}
			Out.push_back(u'\n');
			++Line;
		}
		Format.bMixed = !Format.Exceptions.empty();
		D.Text = std::move(Out);
	}

	DecodeResult Finish(Decoded&& D, FileFormat Format, bool bReadOnly, std::string Reason)
	{
		DecodeResult R;
		NormalizeLineEndings(D, Format);
		R.Text = std::move(D.Text);
		R.ReplacementAt = std::move(D.Marks);
		R.Format = std::move(Format);
		if (!D.bClean && !bReadOnly)
		{
			bReadOnly = true;
			Reason = std::string("This file is not valid ") + EncodingName(R.Format.Enc)
				+ "; invalid bytes are shown as U+FFFD. Reopen with an encoding to edit it";
		}
		R.bReadOnly = bReadOnly;
		R.ReadOnlyReason = std::move(Reason);
		return R;
	}

	bool StartsWith(std::string_view Bytes, std::string_view Prefix)
	{
		return Bytes.substr(0, Prefix.size()) == Prefix;
	}

	/** Shared BOM and size checks; returns true when Out is final (refused). */
	bool CheckRefusals(std::string_view Bytes, uint64_t MaxBytes, DecodeResult& Out)
	{
		if (Bytes.size() > MaxBytes)
		{
			Out.Refused = Refusal::TooLarge;
			return true;
		}
		if (StartsWith(Bytes, std::string_view("\xFF\xFE\x00\x00", 4)) || StartsWith(Bytes, std::string_view("\x00\x00\xFE\xFF", 4)))
		{
			Out.Refused = Refusal::Utf32;
			return true;
		}
		return false;
	}

	bool HasNulInProbe(std::string_view Bytes)
	{
		return Bytes.substr(0, BinaryProbeBytes).find('\0') != std::string_view::npos;
	}

	bool EncodeUnit(char32_t C, Encoding Enc, std::string& Out)
	{
		switch (Enc)
		{
		case Encoding::Utf8:
			if (C >= 0xD800 && C <= 0xDFFF)
			{
				return false;
			}
			utf::AppendUtf8(C, Out);
			return true;
		case Encoding::Latin1:
			if (C > 0xFF)
			{
				return false;
			}
			Out.push_back(static_cast<char>(C));
			return true;
		case Encoding::Ascii:
			if (C > 0x7F)
			{
				return false;
			}
			Out.push_back(static_cast<char>(C));
			return true;
		case Encoding::Cp1252:
			if (C < 0x80 || (C >= 0xA0 && C <= 0xFF))
			{
				Out.push_back(static_cast<char>(C));
				return true;
			}
			for (int I = 0; I < 32; ++I)
			{
				if (codec_tables::Cp1252High[I] != 0 && codec_tables::Cp1252High[I] == C)
				{
					Out.push_back(static_cast<char>(0x80 + I));
					return true;
				}
			}
			return false;
		case Encoding::Utf16LE:
		case Encoding::Utf16BE:
		{
			if (C >= 0xD800 && C <= 0xDFFF)
			{
				return false;
			}
			char16_t Units[2];
			int N = 1;
			if (C >= 0x10000)
			{
				Units[0] = static_cast<char16_t>(0xD800 + ((C - 0x10000) >> 10));
				Units[1] = static_cast<char16_t>(0xDC00 + ((C - 0x10000) & 0x3FF));
				N = 2;
			}
			else
			{
				Units[0] = static_cast<char16_t>(C);
			}
			for (int I = 0; I < N; ++I)
			{
				const char Hi = static_cast<char>(Units[I] >> 8);
				const char Lo = static_cast<char>(Units[I] & 0xFF);
				if (Enc == Encoding::Utf16LE)
				{
					Out.push_back(Lo);
					Out.push_back(Hi);
				}
				else
				{
					Out.push_back(Hi);
					Out.push_back(Lo);
				}
			}
			return true;
		}
		}
		return false;
	}

	void AppendTerminator(Eol Style, Encoding Enc, std::string& Out)
	{
		if (Style == Eol::CrLf || Style == Eol::Cr)
		{
			EncodeUnit(U'\r', Enc, Out);
		}
		if (Style == Eol::CrLf || Style == Eol::Lf)
		{
			EncodeUnit(U'\n', Enc, Out);
		}
	}
}

namespace dream::codec
{
	const char* EncodingName(Encoding Enc)
	{
		switch (Enc)
		{
		case Encoding::Utf8: return "utf-8";
		case Encoding::Utf16LE: return "utf-16-le";
		case Encoding::Utf16BE: return "utf-16-be";
		case Encoding::Latin1: return "latin-1";
		case Encoding::Ascii: return "ascii";
		case Encoding::Cp1252: return "cp1252";
		}
		return "utf-8";
	}

	bool LookupEncoding(std::string_view Name, Encoding& OutEnc, bool& bOutSig)
	{
		const std::string Norm = codec_detail::NormalizeCodecName(Name);
		std::string Module(codec_detail::FindAlias(Norm));
		if (Module.empty())
		{
			std::string Dotless = Norm;
			std::replace(Dotless.begin(), Dotless.end(), '.', '_');
			Module = std::string(codec_detail::FindAlias(Dotless));
		}
		if (Module.empty())
		{
			Module = Norm;
		}
		bOutSig = false;
		if (Module == "utf_8")
		{
			OutEnc = Encoding::Utf8;
			return true;
		}
		if (Module == "utf_8_sig")
		{
			OutEnc = Encoding::Utf8;
			bOutSig = true;
			return true;
		}
		if (Module == "utf_16_le")
		{
			OutEnc = Encoding::Utf16LE;
			return true;
		}
		if (Module == "utf_16_be")
		{
			OutEnc = Encoding::Utf16BE;
			return true;
		}
		if (Module == "latin_1")
		{
			OutEnc = Encoding::Latin1;
			return true;
		}
		if (Module == "ascii")
		{
			OutEnc = Encoding::Ascii;
			return true;
		}
		if (Module == "cp1252")
		{
			OutEnc = Encoding::Cp1252;
			return true;
		}
		return false;
	}

	std::string DetectCookie(std::string_view Bytes)
	{
		return codec_detail::ScanCookie(Bytes).Name;
	}

	FileFormat NewFileFormat()
	{
		return FileFormat();
	}

	DecodeResult Decode(std::string_view Bytes, uint64_t MaxBytes)
	{
		DecodeResult Out;
		if (codec_detail::CheckRefusals(Bytes, MaxBytes, Out))
		{
			return Out;
		}
		FileFormat Format;
		codec_detail::Decoded D;
		if (codec_detail::StartsWith(Bytes, "\xFF\xFE") || codec_detail::StartsWith(Bytes, "\xFE\xFF"))
		{
			Format.Enc = static_cast<uint8_t>(Bytes[0]) == 0xFF ? Encoding::Utf16LE : Encoding::Utf16BE;
			Format.bBom = true;
			codec_detail::DecodeBytes(Bytes.substr(2), Format.Enc, D);
			return codec_detail::Finish(std::move(D), std::move(Format), false, std::string());
		}

		const bool bUtf8Bom = codec_detail::StartsWith(Bytes, "\xEF\xBB\xBF");
		const std::string_view Body = bUtf8Bom ? Bytes.substr(3) : Bytes;
		if (codec_detail::HasNulInProbe(Body))
		{
			Out.Refused = Refusal::Binary;
			return Out;
		}
		Format.bBom = bUtf8Bom;
		const codec_detail::CookieScan Cookie = codec_detail::ScanCookie(Bytes);
		Format.CookieName = Cookie.Name;
		bool bReadOnly = false;
		std::string Reason;
		if (Cookie.bDecodeError)
		{
			bReadOnly = true;
			Reason = "invalid or missing encoding declaration";
		}
		else if (!Cookie.Name.empty())
		{
			const std::string Normal = codec_detail::NormalCookieName(Cookie.Name);
			Encoding Enc = Encoding::Utf8;
			bool bSig = false;
			const bool bKnown = LookupEncoding(Normal, Enc, bSig);
			if (!bKnown || Enc == Encoding::Utf16LE || Enc == Encoding::Utf16BE)
			{
				bReadOnly = true;
				Reason = "Encoding " + Cookie.Name + " is not supported for editing";
			}
			else if (bUtf8Bom && Normal != "utf-8")
			{
				// tokenize.detect_encoding compares the normal name: a BOM with any other cookie, even "utf8", is an error.
				bReadOnly = true;
				Reason = "encoding problem: utf-8";
			}
			else
			{
				Format.Enc = Enc;
			}
		}
		codec_detail::DecodeBytes(Body, Format.Enc, D);
		return codec_detail::Finish(std::move(D), std::move(Format), bReadOnly, std::move(Reason));
	}

	DecodeResult DecodeWith(std::string_view Bytes, Encoding Enc, uint64_t MaxBytes)
	{
		DecodeResult Out;
		if (codec_detail::CheckRefusals(Bytes, MaxBytes, Out))
		{
			return Out;
		}
		FileFormat Format;
		Format.Enc = Enc;
		std::string_view Body = Bytes;
		if (Enc == Encoding::Utf8 && codec_detail::StartsWith(Bytes, "\xEF\xBB\xBF"))
		{
			Format.bBom = true;
			Body.remove_prefix(3);
		}
		else if ((Enc == Encoding::Utf16LE && codec_detail::StartsWith(Bytes, "\xFF\xFE"))
			|| (Enc == Encoding::Utf16BE && codec_detail::StartsWith(Bytes, "\xFE\xFF")))
		{
			Format.bBom = true;
			Body.remove_prefix(2);
		}
		Format.CookieName = codec_detail::ScanCookie(Bytes).Name;
		codec_detail::Decoded D;
		codec_detail::DecodeBytes(Body, Enc, D);
		return codec_detail::Finish(std::move(D), std::move(Format), false, std::string());
	}

	bool Encode(std::u16string_view Text, const FileFormat& Format, std::string& Out, Offset* OutBadOffset)
	{
		Out.clear();
		Out.reserve(Text.size() + 16);
		if (Format.bBom)
		{
			if (Format.Enc == Encoding::Utf8)
			{
				Out.append("\xEF\xBB\xBF");
			}
			else if (Format.Enc == Encoding::Utf16LE)
			{
				Out.append("\xFF\xFE");
			}
			else if (Format.Enc == Encoding::Utf16BE)
			{
				Out.append("\xFE\xFF");
			}
		}
		size_t NextException = 0;
		LineNo Line = 0;
		size_t I = 0;
		while (I < Text.size())
		{
			if (Text[I] == u'\n')
			{
				while (NextException < Format.Exceptions.size() && Format.Exceptions[NextException].Line < Line)
				{
					++NextException;
				}
				Eol Style = Format.Dominant;
				if (NextException < Format.Exceptions.size() && Format.Exceptions[NextException].Line == Line)
				{
					Style = Format.Exceptions[NextException].Style;
				}
				codec_detail::AppendTerminator(Style, Format.Enc, Out);
				++Line;
				++I;
				continue;
			}
			size_t Units = 1;
			const char32_t C = utf::DecodeAt(Text, I, Units);
			if (!codec_detail::EncodeUnit(C, Format.Enc, Out))
			{
				if (OutBadOffset != nullptr)
				{
					*OutBadOffset = static_cast<Offset>(I);
				}
				return false;
			}
			I += Units;
		}
		return true;
	}

	void MapEolExceptions(FileFormat& Format, const ChangeSet& Changes)
	{
		if (Format.Exceptions.empty() || Changes.Ranges.empty())
		{
			return;
		}
		// Range R removes the terminators of lines [StartLine, StartLine + OldLines) and shifts every later one by
		// NewLines - OldLines; ranges come in ascending pre-edit order, so the first range past a line ends its walk.
		std::vector<EolException> Kept;
		Kept.reserve(Format.Exceptions.size());
		for (const EolException& Exception : Format.Exceptions)
		{
			LineNo Shift = 0;
			bool bRemoved = false;
			for (const ChangeRange& Range : Changes.Ranges)
			{
				if (Exception.Line < Range.StartLine)
				{
					break;
				}
				if (Exception.Line < Range.StartLine + Range.OldLines)
				{
					bRemoved = true;
					break;
				}
				Shift += Range.NewLines - Range.OldLines;
			}
			if (!bRemoved)
			{
				Kept.push_back(EolException{Exception.Line + Shift, Exception.Style});
			}
		}
		Format.Exceptions = std::move(Kept);
		Format.bMixed = !Format.Exceptions.empty();
	}
}
