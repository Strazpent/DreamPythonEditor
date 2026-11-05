// SPDX-License-Identifier: Apache-2.0
// (chrome) The look of the Maya-minimal chrome (PO-04 point 1): the menu bar, the icon row, the input tab row, the slim
// banners, the status line and the output area's frame. Every colour is a chrome slot of the current theme
// (FDreamThemes, Shell/DreamTheme.h), read at paint: attributes are bound getters, and the brushes and widget styles here are
// retinted in place when the theme changes, so widgets that hold their pointers follow a switch without a rebuild.
// (icons) The icons are the dream set's SVGs (Shell/DreamIcons.h, PO-06 F6), loaded at their first paint (D22).
// Game thread. Body: DreamChrome.cpp (chrome group).
#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Misc/Attribute.h"

struct FButtonStyle;
struct FCheckBoxStyle;
struct FComboButtonStyle;
struct FSlateBrush;
struct FSplitterStyle;

/** The chrome slots of a theme (restyle plan "Theme API": chrome background ... danger, and the two pane backgrounds). */
enum class EDreamChromeSlot : uint8
{
	Background,         // menu bar, icon row, tab rows, banners, status line
	Border,             // one-pixel separators, the splitter handle
	Text,
	MutedText,          // inactive tabs, line:col, hints
	Hover,
	Selected,           // the active input tab, a pressed button, a checked toggle
	Accent,             // Run, focus
	Danger,             // Stop while a run is active
	OutputBackground,   // the history pane
	EditorBackground,   // the input pane, the active input tab
	Warning,            // a warning banner's mark (the theme's output warning colour)
};

/** The chrome's icons: (icons) glyphs of the dream set (Shell/DreamIcons.h). */
enum class EDreamChromeIcon : uint8
{
	Open,
	Save,
	Run,
	RunLine,
	Stop,
	Clear,
	UndoRun,
	Echo,
	LineNumbers,
	Lock,
	Recovered,
	Close,
	Plus,
	ChevronDown,
	Count
};

class DREAMEDITOR_API FDreamChrome
{
public:
	/** The slot's colour in the current theme. */
	static FLinearColor Linear(EDreamChromeSlot Slot);
	static FSlateColor Color(EDreamChromeSlot Slot);
	/** A bound getter: the colour is read at every paint, so a theme switch shows at once. */
	static TAttribute<FSlateColor> ColorAttr(EDreamChromeSlot Slot);
	/** A solid brush of the slot (a pointer that never changes; its tint follows the theme). */
	static const FSlateBrush* Brush(EDreamChromeSlot Slot);

	/** Flat buttons: transparent, Hover on hover, Selected pressed; content inherits the foreground. */
	static const FButtonStyle& FlatButton();
	/** Flat toggles (an SCheckBox in ToggleButton mode): transparent, Hover on hover, Selected while checked. */
	static const FCheckBoxStyle& FlatToggle();
	/** The output area's tabs (an SCheckBox in ToggleButton mode): the checked tab takes the history's background. */
	static const FCheckBoxStyle& TabToggle();
	/** Flat combo buttons (menu titles, the Run split arrow): no down arrow, the flat button's states. */
	static const FComboButtonStyle& FlatCombo();
	/** The centre splitter: a Border-coloured handle, Accent while dragged or hovered. */
	static const FSplitterStyle& Splitter();

	static const FSlateBrush* Icon(EDreamChromeIcon Icon);
	/** The chrome's UI font (menu titles, tabs, banners, the status line): the editor's regular face. */
	static FSlateFontInfo Font(int32 Size = 9);

	/** True when the current theme is a light one (Solarized Light, GitHub Light, a light user theme). */
	static bool IsLightTheme();

	/** The themes for View ▸ Theme: (id, name) in list order; the current id; SetTheme applies and persists it. */
	static TArray<TPair<FString, FText>> ListThemes();
	static FString GetThemeId();
	static bool SetTheme(const FString& Id);

	/** Fires on the game thread after the theme changed and the chrome's brushes and styles were retinted. */
	static FSimpleMulticastDelegate& OnChanged();

private:
	static void EnsureInitialized();
	static void Retint();
};
