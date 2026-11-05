// SPDX-License-Identifier: Apache-2.0
// Dream themes (owner ruling PO-04 "Themes"): the colour set of the whole Dream tab (chrome, panes, gutter, current line,
// selection, find matches, caret, output line kinds, syntax kinds and the Run window). Built-ins: Darcula (the default),
// Dracula, Maya, Monokai, One Dark, Nord, Gruvbox Dark, Solarized Light, GitHub Light and Unreal (the editor's
// SyntaxHighlight.SourceCode.* and FStyleColors, the slice-B colours). User themes are JSON files (dream/ThemeFormat.h) in
// GetUserThemesDir(), read lazily (the first List()/Find() of an id that is not built in, or one deferred idle tick
// after start-up when UDreamUserSettings::ThemeId names one), never in StartupModule (B1).
// Body: DreamTheme.cpp (switching, JSON, user themes) and DreamThemeTable.cpp (the built-in palettes). Owner: theme group.
//
// Reading a colour: never cache the value. Read it at paint (FDreamThemes::Color inside OnPaint or a lambda), bind it
// (ColorAttr: a getter Slate polls), or use Brush(): one solid brush per slot whose pointer never changes and whose tint
// is retinted in place on a switch. A widget that must build a colour into its children (rows, text styles, a marshaller)
// subscribes to OnChanged(), which fires after the style set, the brushes and FlatButtonStyle() hold the new theme.
// FDreamEditorStyle's colour getters and DreamStyleKeys colours return the current theme too.
// Game thread only.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Styling/SlateColor.h"

struct FSlateBrush;
struct FButtonStyle;
struct FEditableTextBoxStyle;
struct FProgressBarStyle;
struct FScrollBarStyle;
struct FTableRowStyle;
struct FTableViewStyle;

/** The colour slots of a theme. The order is the JSON slot order of dream/ThemeFormat.h (static_assert in the body). */
enum class EDreamThemeSlot : uint8
{
	// Chrome: the menu bar, icon row, tab rows, banners, the Run window's frame
	ChromeBackground,
	ChromeBorder,
	ChromeText,
	ChromeMutedText,
	ChromeHover,
	ChromeSelected,     // the selected item and the active input tab
	Accent,             // focus, the Run action, progress
	Danger,             // Stop
	// Panes
	OutputBackground,   // the history (output) pane and the Run window's live view
	EditorBackground,   // the input (code) pane
	GutterBackground,
	GutterText,
	CurrentLine,
	Selection,
	FindMatch,
	FindCurrent,
	Caret,
	// Output line kinds (§6.4)
	OutEcho,
	OutStdout,
	OutStderr,
	OutResult,
	OutWarning,
	OutError,
	OutNotice,
	OutInfo,
	OutLink,
	OutHeader,          // run headers and footers
	// Syntax kinds (§4.12)
	SynNormal,
	SynKeyword,
	SynBuiltin,
	SynSelf,
	SynDefinition,
	SynDecorator,
	SynNumber,
	SynString,
	SynDocstring,
	SynEscape,
	SynOperator,
	SynComment,
	SynCell,
	SynError,
	SynWarning,         // (lang) PO-06 F4: warning squiggles, scroll-bar marks and the inspection widget
	Count
};

/** One theme. Colours are linear (FLinearColor), converted from the sRGB values of the palette or the file. */
struct FDreamTheme
{
	FString Id;
	FText Name;
	bool bLight = false;
	bool bBuiltIn = false;
	/** The theme a user file's missing slots came from (as written in the file; empty for built-ins and files without). */
	FString BaseId;
	/** The file a user theme was read from (empty for built-ins). */
	FString SourcePath;
	FLinearColor Colors[static_cast<int32>(EDreamThemeSlot::Count)];

	const FLinearColor& Get(EDreamThemeSlot Slot) const { return Colors[static_cast<int32>(Slot)]; }
	void Set(EDreamThemeSlot Slot, const FLinearColor& Color) { Colors[static_cast<int32>(Slot)] = Color; }
};

class DREAMEDITOR_API FDreamThemes
{
public:
	/** FDreamEditorStyle::Initialize (StartupModule): builds the built-ins and selects UDreamUserSettings::ThemeId when it is a
	 *  built-in (no I/O); a user theme id is resolved one deferred idle tick later. Registers the ThemeId dropdown of
	 *  Editor Preferences (Shell/DreamThemeDetails.h). */
	static void Initialize();
	static void Shutdown();

	/** The current theme. Do not keep the reference across a switch. */
	static const FDreamTheme& Current();
	static const FString& GetCurrentId();

	/** The current colour of a slot (read it at paint or in a lambda; never store it). */
	static FSlateColor Color(EDreamThemeSlot Slot);
	static FLinearColor LinearColor(EDreamThemeSlot Slot);
	/** A bound attribute that reads Color(Slot) each time Slate asks: live across switches without a rebuild. */
	static TAttribute<FSlateColor> ColorAttr(EDreamThemeSlot Slot);
	/** A solid brush of the slot's colour. The pointer stays valid for the session; the tint is updated on a switch. */
	static const FSlateBrush* Brush(EDreamThemeSlot Slot);
	/** A flat button: no fill, ChromeHover on hover, ChromeSelected pressed, the foreground inherited from the parent. */
	static const FButtonStyle& FlatButtonStyle();
	/** A text box: EditorBackground fill with a ChromeBorder outline (Accent when focused), ChromeText text. */
	static const FEditableTextBoxStyle& TextBoxStyle();
	/** Scroll bars (SScrollBar .Style, SListView / SScrollBox .ScrollBarStyle): no track, so the pane shows through; a
	 *  rounded ChromeHover thumb, ChromeMutedText while hovered or dragged. */
	static const FScrollBarStyle& ScrollBarStyle();
	/** List rows (STableRow .Style): transparent rows, ChromeHover under the mouse, Selection when selected (focused or
	 *  not), an Accent outline on the keyboard selector, ChromeText text. */
	static const FTableRowStyle& TableRowStyle();
	/** List views (SListView .ListViewStyle): no background of their own, so the pane's theme colour shows through (the
	 *  stock ListView style paints a dark panel brush, which a light theme cannot recolour). */
	static const FTableViewStyle& ListViewStyle();
	/** Progress bars: a ChromeBorder track; the fill is white, so FillColorAndOpacity gives its colour. */
	static const FProgressBarStyle& ProgressBarStyle();

	/** Every theme: the built-ins in their fixed order (Darcula first), then the user themes by name. Loads user themes the
	 *  first time (lazily, a few small files); later calls list the folder at most once a second and, when its files
	 *  changed, schedule ReloadUserThemes on a deferred idle tick (this call returns the list it has). */
	static TArray<TSharedRef<const FDreamTheme>> List();
	/** A theme by id (exact match), loading user themes first when Id is not a built-in. Null when unknown. */
	static TSharedPtr<const FDreamTheme> Find(const FString& Id);
	/** A theme by id among the built-ins and the user themes already loaded: no file access (for per-frame readers). */
	static TSharedPtr<const FDreamTheme> FindLoaded(const FString& Id);
	/**
	 * Makes Id current and applies it live (style set, brushes, OnChanged). bPersist writes UDreamUserSettings::ThemeId
	 * (UDreamUserSettings::SetThemeId saves it). False when Id is unknown (nothing changes). Selecting the current id again
	 * changes nothing, except that Unreal is rebuilt from the editor's colours first.
	 */
	static bool SetCurrent(const FString& Id, bool bPersist = true);
	/** After a switch: the style set, the brushes and FlatButtonStyle() already hold the new colours. */
	static FSimpleMulticastDelegate& OnChanged();

	/** The theme as canonical JSON (dream::theme::Serialize). */
	static FString ExportJson(const FDreamTheme& Theme);
	/**
	 * Reads a theme from JSON. Unknown keys are ignored, invalid or missing colours come from the base theme ("base", else
	 * Darcula, or GitHub Light when "light" is true), a missing name is the id. False with OutError for text that is not a
	 * theme (dream::theme::Parse's hard failures) or a theme without an id.
	 */
	static bool ImportJson(const FString& Json, FDreamTheme& OutTheme, FString* OutError = nullptr);
	/** Writes ExportJson of a known theme to Path (UTF-8, no BOM). */
	static bool ExportToFile(const FString& Id, const FString& Path, FString* OutError = nullptr);
	/** Exports every built-in as <Dir>/<Id>.json; returns how many were written. */
	static int32 ExportBuiltIns(const FString& Dir);

	/** <user settings dir>/Dream/Themes (%LOCALAPPDATA%\DreamPythonEditor\Themes on Windows), or the test override. */
	static FString GetUserThemesDir();
	/** Reads GetUserThemesDir()/*.json again (a file without "format": "dream-theme", or whose id is a built-in's, is
	 *  skipped). Returns the user theme count. Then UDreamUserSettings::ThemeId is resolved again when it is not the current
	 *  theme; otherwise a current user theme is re-applied from its new file, or Darcula shows when it is gone. */
	static int32 ReloadUserThemes();
	/** "Darcula" (PO-04: the default of a fresh profile). */
	static FString GetDefaultId();

	/** Test hook: read user themes from Dir instead (empty restores the default) and reload them. */
	static void SetUserThemesDirForTest(const FString& Dir);

	/** View > Theme > Open Themes Folder: creates GetUserThemesDir() when missing and shows it in the file browser. */
	static void OpenUserThemesFolder();
	/** View > Theme > Export Built-in Themes: every built-in to GetUserThemesDir()/Built-in, then shows that folder. Refused
	 *  (0) while a run is active. */
	static int32 ExportBuiltInsToUserFolder();
};
