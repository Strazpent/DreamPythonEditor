// SPDX-License-Identifier: Apache-2.0
// The Editor Preferences dropdown of UDreamUserSettings::ThemeId (PO-04 "Where to pick one: ... Editor Preferences ▸
// Plugins ▸ Dream"). (theme R-note) A GetOptions meta function would need a UFUNCTION, and DreamEditor has none (rule 11,
// D29: no reflected API), so the dropdown is a detail customization of UDreamUserSettings that replaces only the ThemeId
// row's value widget with a combo of every theme (FDreamThemes::List: built-ins in menu order, then user themes). Choosing
// one sets the property through its handle, so Editor Preferences saves it and PostEditChangeProperty applies it live.
// Body: DreamThemeDetails.cpp. Game thread. Owner: theme group.
#pragma once

#include "CoreMinimal.h"

class IPropertyHandle;
class SWidget;

namespace DreamThemeDetails
{
	/** One entry of the dropdown. */
	struct FThemeOption
	{
		FString Id;
		FText Name;
		bool bBuiltIn = true;
		FString SourcePath;     // a user theme's file (the entry's tooltip)
	};

	/** Registers the customization with the PropertyEditor module when it is loaded (FDreamThemes::Initialize). */
	DREAMEDITOR_API void Register();
	DREAMEDITOR_API void Unregister();
	/** True when the PropertyEditor module's class layout map holds this customization for UDreamUserSettings. */
	DREAMEDITOR_API bool IsRegisteredForSettings();
	/** The one source of the dropdown's entries: every theme of FDreamThemes::List(), built-ins first. */
	DREAMEDITOR_API TArray<FThemeOption> GetThemeOptions();
	/** The dropdown's menu, built from GetThemeOptions(); choosing an entry sets Handle's value (no-op without a handle). */
	DREAMEDITOR_API TSharedRef<SWidget> MakeThemeMenu(const TSharedPtr<IPropertyHandle>& Handle);
	/** The FTagMetaData tag of the ThemeId row's combo button (specs find it in a details view). */
	DREAMEDITOR_API FName GetComboTag();
}
