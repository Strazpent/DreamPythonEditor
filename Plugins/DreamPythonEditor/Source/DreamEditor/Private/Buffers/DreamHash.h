// SPDX-License-Identifier: Apache-2.0
// SHA-256 as lower-case hex for base versions (TDD §10.14) and the project hash (§10.1). Slice-internal, header only.
#pragma once

#include "CoreMinimal.h"
#include "Module/DreamText.h"
#include "dream/Checksum.h"

#include <string_view>

namespace DreamHash
{
	inline FString Sha256Hex(std::string_view Bytes)
	{
		dream::Sha256 Hasher;
		Hasher.Update(Bytes);
		const dream::Sha256Digest Digest = Hasher.Final();
		return DreamText::Utf8ToFString(dream::Sha256::ToHex(Digest));
	}
}
