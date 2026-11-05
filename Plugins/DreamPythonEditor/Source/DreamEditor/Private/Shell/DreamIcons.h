// SPDX-License-Identifier: Apache-2.0
// (icons) PO-06 F6: the editor's own icon set, the "dream" set. Hand-authored SVGs in <plugin>/Resources/Icons (written by
// Tools/Icons/dream_icons.py, which also renders Resources/Icon128.png), registered as FSlateVectorImageBrush entries of
// the DreamEditorStyle set under the keys "Dream.Icon.<Name>" (FDreamEditorStyle::Initialize calls Register; the files load
// at their first paint, D22). Body: DreamIcons.cpp. Owner: icons group.
//
// Style rules (every icon follows them; check new ones with `dream_icons.py render`):
//   * a 16 x 16 grid, art inside 1..15, crisp at 16 and 20 px;
//   * monochrome white glyphs: the caller tints them (SImage ColorAndOpacity) with TintAttr(Icon) or a theme colour of its
//     own, so each icon follows all ten themes. Two tones only: the glyph (opacity 1) and the "dream layer" (0.45);
//   * soft and rounded (every corner rounded, 1.5 px strokes with round caps), at most one dreamy motif per icon
//     (crescent moon, four-point sparkle, cloud), no letters or words;
//   * Brand is the only colour art (draw it untinted, white); BrandMono is the tintable brand glyph.
// Never put icons in the output history lines (PO-04 point 2 holds there). Game thread.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Styling/SlateColor.h"
#include "Textures/SlateIcon.h"

class FSlateStyleSet;
struct FSlateBrush;
struct FDreamTheme;

/** Every icon of the set. The style key is "Dream.Icon." + the enumerator's name (FDreamIcons::StyleKey). */
enum class EDreamIcon : uint8
{
	// Brand and tab
	Brand,              // colour: crescent moon, python curl, sparkle (tab, Tools/Window menus, level-editor toolbar)
	BrandMono,          // the same mark as a tintable glyph
	// The icon row and its menus
	Open,
	Save,
	Run,
	RunLine,
	Stop,
	Clear,
	UndoRun,
	Echo,
	LineNumbers,
	ChevronDown,
	Close,
	Plus,
	Recovered,
	// Project tree
	FolderClosed,
	FolderOpen,
	FilePython,
	File,
	Scratches,
	Samples,
	ProjectPlugins,
	Engine,
	ReadOnly,
	// Structure kinds
	Class,
	Function,
	Method,
	Async,
	Cell,
	// Severities (Problems, the inspection widget, banners) and the quick-fix bulb
	Error,
	Warning,
	Info,
	Lightbulb,
	// Debugger
	Breakpoint,
	BreakpointDisabled,
	CurrentLine,
	Continue,
	StepOver,
	StepInto,
	StepOut,
	DebugRun,
	// Windows
	RunWindow,
	PausedWindow,
	// Notifications
	Notify,
	Success,
	Count
};

/** The colour an icon is drawn in: a theme colour, or one of the dream accents (a dark-theme and a light-theme value). */
enum class EDreamIconTint : uint8
{
	None,           // untinted (white): Brand's own colours
	Text,           // ChromeText
	Muted,          // ChromeMutedText
	Accent,         // the theme's Accent (Run, debugger controls)
	Danger,         // the theme's Danger (Stop)
	Error,          // OutError (the error squiggle's family)
	Warning,        // OutWarning
	Keyword,        // SynKeyword (Structure: class)
	Definition,     // SynDefinition (function, method)
	Builtin,        // SynBuiltin (async)
	Comment,        // SynComment (cell)
	Moon,           // dream accents: gold, mint, lavender, sky, rose
	Mint,
	Lavender,
	Sky,
	Rose,
	Count
};

class DREAMEDITOR_API FDreamIcons
{
public:
	/** FDreamEditorStyle::Initialize: one vector brush per icon (16 px natural size; scale with DesiredSizeOverride). */
	static void Register(FSlateStyleSet& Style);

	/** "Dream.Icon.Run" etc. */
	static FName StyleKey(EDreamIcon Icon);
	/** "Run" etc. (the enumerator's name; tests and the preview label with it). */
	static const TCHAR* Name(EDreamIcon Icon);
	/** The SVG's file name without folder ("run.svg"). */
	static const TCHAR* FileName(EDreamIcon Icon);
	/** <plugin>/Resources/Icons. */
	static FString GetIconsDir();
	/** GetIconsDir() / FileName(Icon). */
	static FString FilePath(EDreamIcon Icon);

	/** The registered brush; null before FDreamEditorStyle::Initialize (a headless editor) or for Count. */
	static const FSlateBrush* Brush(EDreamIcon Icon);
	/** For menus, tabs and toolbars (FSlateIcon of the DreamEditorStyle set). */
	static FSlateIcon SlateIcon(EDreamIcon Icon);

	/** The tint the set was designed with for Icon (what the preview shows). */
	static EDreamIconTint DefaultTint(EDreamIcon Icon);
	/** A tint's colour in the current theme (read at paint; never cache it). */
	static FLinearColor TintColor(EDreamIconTint Tint);
	/** The same in a given theme (the preview sheet shows every theme at once). */
	static FLinearColor TintColor(EDreamIconTint Tint, const FDreamTheme& Theme);
	static FSlateColor Tint(EDreamIcon Icon);
	/** A bound getter of Tint(Icon): live across theme switches. */
	static TAttribute<FSlateColor> TintAttr(EDreamIcon Icon);
};
