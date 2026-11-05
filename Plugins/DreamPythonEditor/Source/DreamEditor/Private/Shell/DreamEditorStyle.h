// SPDX-License-Identifier: Apache-2.0
// The DreamEditorStyle style set (TDD §4.12, §6.4 colour tokens, A11Y-05). Registered in StartupModule; game thread.
// Body: DreamEditorStyle.cpp (theme group since PO-04). Every slice names styles through the keys below, never by string
// literal.
// (slice A choice) The code font is the engine's monospace face (FCoreStyle::GetDefaultFontStyle("Mono", Size)) with
// DroidSansFallback for CJK. Deferred (M1, needs the OFL font files): the JetBrains Mono composite font of §4.12, whose
// four OFL faces are not in the repository.
// PO-04: every colour here is the current theme's (Shell/DreamTheme.h; Darcula by default, Unreal = the slice-B colours of
// SyntaxHighlight.SourceCode.* and FStyleColors). GetTokenColor and GetLineTypeColor read the theme each call; the
// DreamStyleKeys colours, the code text style (caret = its colour, selection = its SelectedBackgroundColor) and the Run
// window's Stop button are rewritten in place on every switch (ApplyTheme), so a pointer to them stays valid.
#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Shell/DreamTheme.h"
#include "dream/PyLexer.h"
#include "dream/OutputRing.h"

class ISlateStyle;
struct FTextBlockStyle;

/** Style keys as TCHAR literals (no FName is built during static initialization). */
namespace DreamStyleKeys
{
	inline constexpr const TCHAR* TabIcon = TEXT("Dream.TabIcon");
	inline constexpr const TCHAR* StatusIcon = TEXT("Dream.StatusIcon");
	inline constexpr const TCHAR* RunIcon = TEXT("Dream.Run");
	inline constexpr const TCHAR* StopIcon = TEXT("Dream.Stop");
	inline constexpr const TCHAR* UndoRunIcon = TEXT("Dream.UndoRun");
	inline constexpr const TCHAR* ClearIcon = TEXT("Dream.Clear");
	/** FButtonStyle of the Run window's Stop (the theme's Danger, white text). */
	inline constexpr const TCHAR* RunWindowStopButton = TEXT("Dream.RunWindow.StopButton");

	// Output line colours (§6.4 table)
	inline constexpr const TCHAR* OutputStdout = TEXT("Dream.Output.Stdout");
	inline constexpr const TCHAR* OutputStderr = TEXT("Dream.Output.Stderr");       // amber, never the error colour
	inline constexpr const TCHAR* OutputResult = TEXT("Dream.Output.Result");
	inline constexpr const TCHAR* OutputInfo = TEXT("Dream.Output.Info");
	inline constexpr const TCHAR* OutputWarning = TEXT("Dream.Output.Warning");
	inline constexpr const TCHAR* OutputError = TEXT("Dream.Output.Error");
	inline constexpr const TCHAR* OutputNotice = TEXT("Dream.Output.Notice");
	inline constexpr const TCHAR* OutputEcho = TEXT("Dream.Output.Echo");
	inline constexpr const TCHAR* OutputHeader = TEXT("Dream.Output.Header");
	inline constexpr const TCHAR* OutputFooter = TEXT("Dream.Output.Footer");
	inline constexpr const TCHAR* OutputLink = TEXT("Dream.Output.Link");
	inline constexpr const TCHAR* OutputBackgroundDim = TEXT("Dream.Output.BackgroundDim");

	// Editor colours (DreamEditor.Editor.*)
	inline constexpr const TCHAR* EditorBackground = TEXT("DreamEditor.Editor.Background");
	inline constexpr const TCHAR* EditorCurrentLine = TEXT("DreamEditor.Editor.CurrentLine");
	inline constexpr const TCHAR* EditorGutterText = TEXT("DreamEditor.Editor.GutterText");
	inline constexpr const TCHAR* EditorSelection = TEXT("DreamEditor.Editor.Selection");
	/** Find matches in the code view (EDT-09): every match, and the selected one. */
	inline constexpr const TCHAR* EditorFindMatch = TEXT("DreamEditor.Editor.FindMatch");
	inline constexpr const TCHAR* EditorFindMatchCurrent = TEXT("DreamEditor.Editor.FindMatchCurrent");
}

class DREAMEDITOR_API FDreamEditorStyle
{
public:
	static void Initialize();           // StartupModule: register the set (font faces load lazily, D22)
	static void Shutdown();
	static const ISlateStyle& Get();
	static FName GetStyleSetName();     // "DreamEditorStyle"

	/** The monospace code font at a size in points (UDreamUserSettings::FontSize plus zoom, EDT-10). */
	static FSlateFontInfo GetCodeFont(int32 Size);
	/** Colour of a lexer token kind in the current theme (§4.3.1 kinds -> the theme's syntax slots). */
	static FSlateColor GetTokenColor(dream::TokenKind Kind);
	/** The theme slot of a lexer token kind. */
	static EDreamThemeSlot GetTokenSlot(dream::TokenKind Kind);
	/** Italic for Comment and Docstring, bold for Definition (§4.12 faces). */
	static FSlateFontInfo GetTokenFont(dream::TokenKind Kind, int32 Size);
	/** Colour of an output line type in the current theme (§6.4 rendering by type; headers and footers share OutHeader). */
	static FSlateColor GetLineTypeColor(dream::LineType Type);
	/** The theme slot of an output line type. */
	static EDreamThemeSlot GetLineTypeSlot(dream::LineType Type);
	/** The base text style the code view and the output view derive from: the code font, ColorAndOpacity = the theme's
	 *  Caret (the stock caret paints in the text style's colour; every token run has its own colour), SelectedBackgroundColor
	 *  = Selection. A code view copies it again (SetTextStyle) on FDreamThemes::OnChanged. */
	static const FTextBlockStyle& GetCodeTextStyle();

	/** FDreamThemes after a switch (and at Initialize): rewrites the style set's colours, the code text style and the Stop
	 *  button in place. No-op before Initialize (a headless editor has no style set). */
	static void ApplyTheme(const FDreamTheme& Theme);

private:
	static TSharedPtr<class FSlateStyleSet> StyleSet;
};
