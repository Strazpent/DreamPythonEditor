// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

struct FSlateBrush;

namespace AnimTK
{
	/**
	 * Resolves an icon: an existing .png/.svg file path (cached per path, size and file time, so a re-captured thumbnail
	 * shows up) or an editor style brush name such as "Icons.Save". Returns nullptr when neither matches.
	 */
	const FSlateBrush* ResolveBrush(const FString& NameOrPath, const FVector2D& Size);

	void ClearBrushCache();
}
