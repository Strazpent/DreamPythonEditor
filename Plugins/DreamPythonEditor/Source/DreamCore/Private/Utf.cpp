// SPDX-License-Identifier: Apache-2.0
// Dream core: UTF-8 / UTF-16 / code point helpers (TDD §4.1.2, §4.3.1, §6.1).
#include "dream/Utf.h"

#include <algorithm>
#include <iterator>

namespace dream::utf_tables
{
#include "XidTables.inc"

	template <size_t N>
	bool InRanges(const XidRange (&Ranges)[N], char32_t CodePoint)
	{
		const XidRange* End = Ranges + N;
		const XidRange* It = std::upper_bound(Ranges, End, CodePoint,
			[](char32_t Value, const XidRange& R) { return Value < R.First; });
		if (It == Ranges)
		{
			return false;
		}
		--It;
		return CodePoint >= It->First && CodePoint <= It->Last;
	}
}

namespace dream::utf_detail
{

	/** Units of the code point starting at Index (1 or 2), counting a lone surrogate as one unit. */
	inline size_t UnitsAt(std::u16string_view Text, size_t Index)
	{
		if (utf::IsHighSurrogate(Text[Index]) && Index + 1 < Text.size() && utf::IsLowSurrogate(Text[Index + 1]))
		{
			return 2;
		}
		return 1;
	}

	/** UTF-8 bytes needed for the code point at Index; a lone surrogate counts 3 (it is written as U+FFFD). */
	inline int32_t Utf8BytesAt(std::u16string_view Text, size_t Index, size_t& OutUnits)
	{
		const char32_t C = utf::DecodeAt(Text, Index, OutUnits);
		if (C < 0x80)
		{
			return 1;
		}
		if (C < 0x800)
		{
			return 2;
		}
		if (C < 0x10000)
		{
			return 3;
		}
		return 4;
	}

	bool Utf8ToUtf16Impl(std::string_view In, std::u16string& Out, std::vector<Offset>* Marks);
}

namespace dream::utf
{
	char32_t DecodeAt(std::u16string_view Text, size_t Index, size_t& OutUnits)
	{
		const char16_t C = Text[Index];
		if (IsHighSurrogate(C) && Index + 1 < Text.size() && IsLowSurrogate(Text[Index + 1]))
		{
			OutUnits = 2;
			return 0x10000 + ((static_cast<char32_t>(C) - 0xD800) << 10) + (static_cast<char32_t>(Text[Index + 1]) - 0xDC00);
		}
		OutUnits = 1;
		return C;
	}

	bool Utf8ToUtf16(std::string_view In, std::u16string& Out)
	{
		return utf_detail::Utf8ToUtf16Impl(In, Out, nullptr);
	}

	bool Utf8ToUtf16(std::string_view In, std::u16string& Out, std::vector<Offset>& OutReplacementAt)
	{
		return utf_detail::Utf8ToUtf16Impl(In, Out, &OutReplacementAt);
	}
}

namespace dream::utf_detail
{
	bool Utf8ToUtf16Impl(std::string_view In, std::u16string& Out, std::vector<Offset>* Marks)
	{
		// Table 3-7 of the Unicode standard; an invalid sequence becomes one U+FFFD per maximal subpart (TDD §4.13 step 5).
		constexpr char32_t ReplacementChar = utf::ReplacementChar;
		bool bClean = true;
		Out.reserve(Out.size() + In.size());
		const size_t Size = In.size();
		size_t I = 0;
		while (I < Size)
		{
			const uint8_t B0 = static_cast<uint8_t>(In[I]);
			if (B0 < 0x80)
			{
				Out.push_back(static_cast<char16_t>(B0));
				++I;
				continue;
			}
			int32_t Need = 0;
			uint8_t Lo = 0x80;
			uint8_t Hi = 0xBF;
			char32_t CodePoint = 0;
			if (B0 >= 0xC2 && B0 <= 0xDF)
			{
				Need = 1;
				CodePoint = B0 & 0x1Fu;
			}
			else if (B0 >= 0xE0 && B0 <= 0xEF)
			{
				Need = 2;
				CodePoint = B0 & 0x0Fu;
				if (B0 == 0xE0)
				{
					Lo = 0xA0;
				}
				else if (B0 == 0xED)
				{
					Hi = 0x9F;
				}
			}
			else if (B0 >= 0xF0 && B0 <= 0xF4)
			{
				Need = 3;
				CodePoint = B0 & 0x07u;
				if (B0 == 0xF0)
				{
					Lo = 0x90;
				}
				else if (B0 == 0xF4)
				{
					Hi = 0x8F;
				}
			}
			else
			{
				if (Marks != nullptr)
				{
					Marks->push_back(static_cast<Offset>(Out.size()));
				}
				Out.push_back(static_cast<char16_t>(ReplacementChar));
				bClean = false;
				++I;
				continue;
			}

			size_t J = I + 1;
			bool bOk = true;
			for (int32_t K = 0; K < Need; ++K, ++J)
			{
				if (J >= Size)
				{
					bOk = false;
					break;
				}
				const uint8_t B = static_cast<uint8_t>(In[J]);
				const uint8_t MinB = K == 0 ? Lo : 0x80;
				const uint8_t MaxB = K == 0 ? Hi : 0xBF;
				if (B < MinB || B > MaxB)
				{
					bOk = false;
					break;
				}
				CodePoint = (CodePoint << 6) | (B & 0x3Fu);
			}
			if (!bOk)
			{
				// The maximal subpart is the lead byte plus the valid continuation bytes before J.
				if (Marks != nullptr)
				{
					Marks->push_back(static_cast<Offset>(Out.size()));
				}
				Out.push_back(static_cast<char16_t>(ReplacementChar));
				bClean = false;
				I = J;
				continue;
			}
			if (CodePoint >= 0x10000)
			{
				const char32_t V = CodePoint - 0x10000;
				Out.push_back(static_cast<char16_t>(0xD800 + (V >> 10)));
				Out.push_back(static_cast<char16_t>(0xDC00 + (V & 0x3FF)));
			}
			else
			{
				Out.push_back(static_cast<char16_t>(CodePoint));
			}
			I = J;
		}
		return bClean;
	}
}

namespace dream::utf
{
	void AppendUtf8(char32_t CodePoint, std::string& Out)
	{
		if ((CodePoint >= 0xD800 && CodePoint <= 0xDFFF) || CodePoint > 0x10FFFF)
		{
			CodePoint = ReplacementChar;
		}
		if (CodePoint < 0x80)
		{
			Out.push_back(static_cast<char>(CodePoint));
		}
		else if (CodePoint < 0x800)
		{
			Out.push_back(static_cast<char>(0xC0 | (CodePoint >> 6)));
			Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
		}
		else if (CodePoint < 0x10000)
		{
			Out.push_back(static_cast<char>(0xE0 | (CodePoint >> 12)));
			Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
			Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
		}
		else
		{
			Out.push_back(static_cast<char>(0xF0 | (CodePoint >> 18)));
			Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F)));
			Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
			Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
		}
	}

	void Utf16ToUtf8(std::u16string_view In, std::string& Out)
	{
		Out.reserve(Out.size() + In.size());
		size_t I = 0;
		while (I < In.size())
		{
			const char16_t C = In[I];
			if (C < 0x80)
			{
				Out.push_back(static_cast<char>(C));
				++I;
				continue;
			}
			size_t Units = 1;
			AppendUtf8(DecodeAt(In, I, Units), Out);
			I += Units;
		}
	}

	int32_t Utf8ColToUtf16(std::u16string_view Line, int32_t ByteCol)
	{
		if (ByteCol <= 0)
		{
			return 0;
		}
		int32_t Bytes = 0;
		size_t I = 0;
		while (I < Line.size())
		{
			size_t Units = 1;
			const int32_t Len = utf_detail::Utf8BytesAt(Line, I, Units);
			if (Bytes + Len > ByteCol)
			{
				break;
			}
			Bytes += Len;
			I += Units;
		}
		return static_cast<int32_t>(I);
	}

	int32_t Utf16ColToUtf8(std::u16string_view Line, int32_t Col16)
	{
		if (Col16 <= 0)
		{
			return 0;
		}
		const size_t Limit = std::min(static_cast<size_t>(Col16), Line.size());
		int32_t Bytes = 0;
		size_t I = 0;
		while (I < Limit)
		{
			size_t Units = 1;
			const int32_t Len = utf_detail::Utf8BytesAt(Line, I, Units);
			if (I + Units > Limit)
			{
				break;  // Col16 splits a surrogate pair: count up to the pair's start
			}
			Bytes += Len;
			I += Units;
		}
		return Bytes;
	}

	int32_t CodepointColToUtf16(std::u16string_view Line, int32_t CodepointCol)
	{
		if (CodepointCol <= 0)
		{
			return 0;
		}
		size_t I = 0;
		int32_t Count = 0;
		while (I < Line.size() && Count < CodepointCol)
		{
			I += utf_detail::UnitsAt(Line, I);
			++Count;
		}
		return static_cast<int32_t>(I);
	}

	int32_t Utf16ColToCodepoint(std::u16string_view Line, int32_t Col16)
	{
		if (Col16 <= 0)
		{
			return 0;
		}
		const size_t Limit = std::min(static_cast<size_t>(Col16), Line.size());
		size_t I = 0;
		int32_t Count = 0;
		while (I < Limit)
		{
			const size_t Units = utf_detail::UnitsAt(Line, I);
			if (I + Units > Limit)
			{
				break;
			}
			I += Units;
			++Count;
		}
		return Count;
	}

	bool IsIdentifierStart(char32_t CodePoint)
	{
		if (CodePoint < 0x80)
		{
			return (CodePoint >= U'a' && CodePoint <= U'z') || (CodePoint >= U'A' && CodePoint <= U'Z') || CodePoint == U'_';
		}
		return utf_tables::InRanges(utf_tables::XidStartRanges, CodePoint);
	}

	bool IsIdentifierContinue(char32_t CodePoint)
	{
		if (CodePoint < 0x80)
		{
			return (CodePoint >= U'a' && CodePoint <= U'z') || (CodePoint >= U'A' && CodePoint <= U'Z') || CodePoint == U'_'
				|| (CodePoint >= U'0' && CodePoint <= U'9');
		}
		return utf_tables::InRanges(utf_tables::XidContinueRanges, CodePoint);
	}

	bool IsIdentifier(std::u16string_view Text)
	{
		if (Text.empty())
		{
			return false;
		}
		size_t I = 0;
		bool bFirst = true;
		while (I < Text.size())
		{
			size_t Units = 1;
			const char32_t C = DecodeAt(Text, I, Units);
			if (bFirst ? !IsIdentifierStart(C) : !IsIdentifierContinue(C))
			{
				return false;
			}
			bFirst = false;
			I += Units;
		}
		return true;
	}
}
