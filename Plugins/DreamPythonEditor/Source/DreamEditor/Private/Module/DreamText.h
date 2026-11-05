// SPDX-License-Identifier: Apache-2.0
// FString <-> DreamCore UTF-16 conversions. TCHAR is UTF-16 on every platform UE 5.8 supports (wchar_t on Windows,
// char16_t on Linux and Mac, TDD §4.1.2), so the UTF-16 conversions are reinterpretations, not transcodes. Header only.
#pragma once

#include "CoreMinimal.h"
#include "Containers/StringConv.h"

#include <string>
#include <string_view>

static_assert(sizeof(TCHAR) == sizeof(char16_t), "Dream's model is UTF-16 and assumes a 16-bit TCHAR (TDD §4.1.2).");

namespace DreamText
{
	inline std::u16string_view ToU16(FStringView Str)
	{
		return std::u16string_view(reinterpret_cast<const char16_t*>(Str.GetData()), static_cast<size_t>(Str.Len()));
	}

	inline std::u16string_view ToU16(const FString& Str)
	{
		return ToU16(FStringView(Str));
	}

	inline FStringView ToView(std::u16string_view Str)
	{
		return FStringView(reinterpret_cast<const TCHAR*>(Str.data()), static_cast<int32>(Str.size()));
	}

	inline FString ToFString(std::u16string_view Str)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const TCHAR*>(Str.data()), static_cast<int32>(Str.size()));
	}

	/** UTF-8 (DreamCore output lines, journal heads) to FString. */
	inline FString Utf8ToFString(std::string_view Utf8)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Utf8.data()), static_cast<int32>(Utf8.size()));
	}

	inline std::string ToUtf8(FStringView Str)
	{
		const auto Converted = StringCast<UTF8CHAR>(Str.GetData(), Str.Len());
		return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
	}

	/** FGuid as 32 lower-case hex digits: buffer IDs, journal uuids, public API tab IDs. */
	inline FString GuidToHex(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::Digits).ToLower();
	}

	/** Model text from any source text: CRLF and lone CR become LF (the model is LF-only, TDD §4.1.2). Body: DreamText.cpp. */
	DREAMEDITOR_API FString ToModelText(const FString& Text);

	/** A 32-hex tab ID or a GUID in any FGuid format back to an FGuid; false when it is neither. */
	DREAMEDITOR_API bool HexToGuid(const FString& Hex, FGuid& OutGuid);
}
