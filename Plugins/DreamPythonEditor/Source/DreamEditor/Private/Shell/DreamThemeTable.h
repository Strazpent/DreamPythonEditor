// SPDX-License-Identifier: Apache-2.0
// The built-in theme palettes (PO-04 "Palettes"), internal to the theme group: DreamTheme.cpp builds its list from these.
// Body: DreamThemeTable.cpp. Game thread (Unreal reads the app style and the editor's colour table).
#pragma once

#include "CoreMinimal.h"
#include "Shell/DreamTheme.h"

namespace DreamThemeTable
{
	inline constexpr const TCHAR* DarculaId = TEXT("Darcula");
	inline constexpr const TCHAR* GitHubLightId = TEXT("GitHub Light");
	inline constexpr const TCHAR* UnrealId = TEXT("Unreal");

	/** Every built-in in menu order: Darcula, Dracula, Maya, Monokai, One Dark, Nord, Gruvbox Dark, Solarized Light,
	 *  GitHub Light, Unreal. */
	void BuildBuiltIns(TArray<TSharedRef<FDreamTheme>>& Out);
	/** The Unreal theme from the editor's current SyntaxHighlight.SourceCode.* styles and FStyleColors (rebuilt on demand,
	 *  since the editor's colours are user-editable). */
	TSharedRef<FDreamTheme> MakeUnreal();
}
