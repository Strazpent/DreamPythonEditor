// SPDX-License-Identifier: Apache-2.0
// Out-of-line DreamText helpers (Module/DreamText.h).
#include "Module/DreamText.h"

namespace DreamText
{
	FString ToModelText(const FString& Text)
	{
		int32 Index = INDEX_NONE;
		if (!Text.FindChar(TEXT('\r'), Index))
		{
			return Text;
		}
		FString Out;
		Out.Reserve(Text.Len());
		const int32 Len = Text.Len();
		for (int32 At = 0; At < Len; ++At)
		{
			const TCHAR Ch = Text[At];
			if (Ch == TEXT('\r'))
			{
				Out.AppendChar(TEXT('\n'));
				if (At + 1 < Len && Text[At + 1] == TEXT('\n'))
				{
					++At;   // CRLF is one terminator
				}
			}
			else
			{
				Out.AppendChar(Ch);
			}
		}
		return Out;
	}

	bool HexToGuid(const FString& Hex, FGuid& OutGuid)
	{
		return FGuid::Parse(Hex.TrimStartAndEnd(), OutGuid);
	}
}
