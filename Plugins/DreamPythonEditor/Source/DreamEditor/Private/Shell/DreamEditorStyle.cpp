// SPDX-License-Identifier: Apache-2.0
// The DreamEditorStyle style set (TDD §4.12, §6.4 colour tokens). PO-04: every colour is the current Dream theme's
// (Shell/DreamTheme.h). The Unreal theme holds exactly the slice-B values (SyntaxHighlight.SourceCode.* and FStyleColors,
// T-VIEW-014); Darcula is the default. ApplyTheme rewrites the colour keys and the two widget styles in place on a switch.
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"   // (icons)

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"

TSharedPtr<FSlateStyleSet> FDreamEditorStyle::StyleSet;

namespace DreamStyleLocal
{
	constexpr const TCHAR* StyleSetName = TEXT("DreamEditorStyle");
	constexpr const TCHAR* CodeTextKey = TEXT("DreamEditor.CodeText");

	/** The style keys the DreamStyleKeys colours and the older DreamEditor.Syntax.* keys map to. */
	struct FColorKey
	{
		const TCHAR* Key;
		EDreamThemeSlot Slot;
	};

	const FColorKey ColorKeys[] = {
		{ DreamStyleKeys::OutputStdout, EDreamThemeSlot::OutStdout },
		{ DreamStyleKeys::OutputStderr, EDreamThemeSlot::OutStderr },
		{ DreamStyleKeys::OutputResult, EDreamThemeSlot::OutResult },
		{ DreamStyleKeys::OutputInfo, EDreamThemeSlot::OutInfo },
		{ DreamStyleKeys::OutputWarning, EDreamThemeSlot::OutWarning },
		{ DreamStyleKeys::OutputError, EDreamThemeSlot::OutError },
		{ DreamStyleKeys::OutputNotice, EDreamThemeSlot::OutNotice },
		{ DreamStyleKeys::OutputEcho, EDreamThemeSlot::OutEcho },
		{ DreamStyleKeys::OutputHeader, EDreamThemeSlot::OutHeader },
		{ DreamStyleKeys::OutputFooter, EDreamThemeSlot::OutHeader },
		{ DreamStyleKeys::OutputLink, EDreamThemeSlot::OutLink },
		{ TEXT("Dream.Output.Agent"), EDreamThemeSlot::OutNotice },
		{ TEXT("Dream.Output.Debugger"), EDreamThemeSlot::OutNotice },
		{ TEXT("Dream.Output.Logpoint"), EDreamThemeSlot::OutWarning },
		{ TEXT("Dream.Output.Evaluate"), EDreamThemeSlot::OutResult },
		{ DreamStyleKeys::EditorBackground, EDreamThemeSlot::EditorBackground },
		{ DreamStyleKeys::EditorCurrentLine, EDreamThemeSlot::CurrentLine },
		{ DreamStyleKeys::EditorGutterText, EDreamThemeSlot::GutterText },
		{ DreamStyleKeys::EditorSelection, EDreamThemeSlot::Selection },
		{ DreamStyleKeys::EditorFindMatch, EDreamThemeSlot::FindMatch },
		{ DreamStyleKeys::EditorFindMatchCurrent, EDreamThemeSlot::FindCurrent },
		{ TEXT("DreamEditor.Syntax.Normal"), EDreamThemeSlot::SynNormal },
		{ TEXT("DreamEditor.Syntax.Operator"), EDreamThemeSlot::SynOperator },
		{ TEXT("DreamEditor.Syntax.Keyword"), EDreamThemeSlot::SynKeyword },
		{ TEXT("DreamEditor.Syntax.String"), EDreamThemeSlot::SynString },
		{ TEXT("DreamEditor.Syntax.Number"), EDreamThemeSlot::SynNumber },
		{ TEXT("DreamEditor.Syntax.Comment"), EDreamThemeSlot::SynComment },
		{ TEXT("DreamEditor.Syntax.Decorator"), EDreamThemeSlot::SynDecorator },
		{ TEXT("DreamEditor.Syntax.Error"), EDreamThemeSlot::SynError },
		{ TEXT("DreamEditor.Syntax.Builtin"), EDreamThemeSlot::SynBuiltin },
		{ TEXT("DreamEditor.Syntax.Self"), EDreamThemeSlot::SynSelf },
		{ TEXT("DreamEditor.Syntax.Definition"), EDreamThemeSlot::SynDefinition },
		{ TEXT("DreamEditor.Syntax.Docstring"), EDreamThemeSlot::SynDocstring },
		{ TEXT("DreamEditor.Syntax.Escape"), EDreamThemeSlot::SynEscape },
		{ TEXT("DreamEditor.Syntax.Cell"), EDreamThemeSlot::SynCell },
	};

	/** The Stop button of the Run window (mockup 02 callout 6): the theme's Danger with white text, the one control that
	 *  must stand out while the editor is blocked. Disabled ("Stopping…") is the theme's hover fill. */
	void FillStopButton(FButtonStyle& Style, const FDreamTheme& Theme)
	{
		const FLinearColor Danger = Theme.Get(EDreamThemeSlot::Danger);
		Style.Normal.TintColor = FSlateColor(Danger);
		Style.Hovered.TintColor = FSlateColor(FLinearColor::LerpUsingHSV(Danger, FLinearColor::White, 0.2f));
		Style.Pressed.TintColor = FSlateColor(Danger * 0.7f);
		Style.Disabled.TintColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeHover));
		Style.SetNormalForeground(FStyleColors::White)
			.SetHoveredForeground(FStyleColors::White)
			.SetPressedForeground(FStyleColors::White)
			.SetDisabledForeground(FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText)));
	}
}

void FDreamEditorStyle::Initialize()
{
	if (StyleSet.IsValid())
	{
		return;
	}

	using namespace DreamStyleLocal;
	StyleSet = MakeShared<FSlateStyleSet>(StyleSetName);
	FSlateStyleSet& S = *StyleSet;
	S.SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));

	// (icons) PO-06 F6: the dream icon set (Shell/DreamIcons.h, Resources/Icons/*.svg) under "Dream.Icon.*". The older keys
	// point at the same files: the tab, the Tools/Window menus and the level-editor toolbar button show the colour brand
	// mark; the status item its tintable glyph. Vector brushes load their file at first paint, so registration does no I/O
	// (D22).
	FDreamIcons::Register(S);
	const FVector2f Icon16(16.f, 16.f);
	S.Set(DreamStyleKeys::TabIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::Brand), Icon16));
	S.Set(DreamStyleKeys::StatusIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::BrandMono), Icon16));
	S.Set(DreamStyleKeys::RunIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::Run), Icon16));
	S.Set(DreamStyleKeys::StopIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::Stop), Icon16));
	S.Set(DreamStyleKeys::UndoRunIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::UndoRun), Icon16));
	S.Set(DreamStyleKeys::ClearIcon, new FSlateVectorImageBrush(FDreamIcons::FilePath(EDreamIcon::Clear), Icon16));

	// "Dimmed" output rows (§6.4) are a tint, not a theme colour; the current-line and find bands paint this white brush
	// with the theme's colour as the tint.
	S.Set(DreamStyleKeys::OutputBackgroundDim, FLinearColor(1.f, 1.f, 1.f, 0.55f));
	S.Set(TEXT("DreamEditor.Editor.CurrentLineBrush"), new FSlateColorBrush(FLinearColor::White));

	// The widget styles exist once; ApplyTheme rewrites their colours in place (widgets hold pointers into the set).
	S.Set(CodeTextKey, FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"))).SetFont(GetCodeFont(10)));
	S.Set(DreamStyleKeys::RunWindowStopButton, FButtonStyle(FAppStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("Button")))
		.SetNormal(FSlateRoundedBoxBrush(FLinearColor::White, 4.f))
		.SetHovered(FSlateRoundedBoxBrush(FLinearColor::White, 4.f))
		.SetPressed(FSlateRoundedBoxBrush(FLinearColor::White, 4.f))
		.SetDisabled(FSlateRoundedBoxBrush(FLinearColor::White, 4.f)));

	// The colour keys, then the theme from UDreamUserSettings::ThemeId (a built-in now; a user theme one idle tick later).
	for (const FColorKey& Entry : ColorKeys)
	{
		S.Set(Entry.Key, FSlateColor(FLinearColor::Transparent));
	}
	FSlateStyleRegistry::RegisterSlateStyle(S);
	FDreamThemes::Initialize();
}

void FDreamEditorStyle::Shutdown()
{
	FDreamThemes::Shutdown();
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		ensure(StyleSet.IsUnique());
		StyleSet.Reset();
	}
}

void FDreamEditorStyle::ApplyTheme(const FDreamTheme& Theme)
{
	using namespace DreamStyleLocal;
	if (!StyleSet.IsValid())
	{
		return;
	}
	FSlateStyleSet& S = *StyleSet;
	for (const FColorKey& Entry : ColorKeys)
	{
		S.Set(Entry.Key, FSlateColor(Theme.Get(Entry.Slot)));
	}
	// In place: the set keeps the same style objects, so pointers taken from GetWidgetStyle stay valid across switches.
	FTextBlockStyle& CodeText = const_cast<FTextBlockStyle&>(S.GetWidgetStyle<FTextBlockStyle>(CodeTextKey));
	CodeText.SetColorAndOpacity(FSlateColor(Theme.Get(EDreamThemeSlot::Caret)))
		.SetSelectedBackgroundColor(FSlateColor(Theme.Get(EDreamThemeSlot::Selection)));
	FillStopButton(const_cast<FButtonStyle&>(S.GetWidgetStyle<FButtonStyle>(DreamStyleKeys::RunWindowStopButton)), Theme);
}

const ISlateStyle& FDreamEditorStyle::Get()
{
	check(StyleSet.IsValid());
	return *StyleSet;
}

FName FDreamEditorStyle::GetStyleSetName()
{
	return FName(DreamStyleLocal::StyleSetName);
}

FSlateFontInfo FDreamEditorStyle::GetCodeFont(int32 Size)
{
	// (slice A choice, A38) the engine's monospace face; DroidSansFallback covers CJK through the default composite.
	return FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), static_cast<float>(FMath::Clamp(Size, 4, 96)));
}

EDreamThemeSlot FDreamEditorStyle::GetTokenSlot(dream::TokenKind Kind)
{
	using dream::TokenKind;
	switch (Kind)
	{
	case TokenKind::Keyword:            return EDreamThemeSlot::SynKeyword;
	case TokenKind::SoftKeyword:        return EDreamThemeSlot::SynKeyword;
	case TokenKind::KeywordConstant:    return EDreamThemeSlot::SynKeyword;
	case TokenKind::Builtin:            return EDreamThemeSlot::SynBuiltin;
	case TokenKind::SelfParam:          return EDreamThemeSlot::SynSelf;
	case TokenKind::FunctionName:       return EDreamThemeSlot::SynDefinition;
	case TokenKind::ClassName:          return EDreamThemeSlot::SynDefinition;
	case TokenKind::Decorator:          return EDreamThemeSlot::SynDecorator;
	case TokenKind::Number:             return EDreamThemeSlot::SynNumber;
	case TokenKind::String:             return EDreamThemeSlot::SynString;
	case TokenKind::Docstring:          return EDreamThemeSlot::SynDocstring;
	case TokenKind::StringEscape:       return EDreamThemeSlot::SynEscape;
	case TokenKind::FStringBrace:       return EDreamThemeSlot::SynEscape;
	case TokenKind::FStringConversion:  return EDreamThemeSlot::SynEscape;
	case TokenKind::FStringSpec:        return EDreamThemeSlot::SynEscape;
	case TokenKind::Operator:           return EDreamThemeSlot::SynOperator;
	case TokenKind::Bracket:            return EDreamThemeSlot::SynOperator;
	case TokenKind::Comment:            return EDreamThemeSlot::SynComment;
	case TokenKind::CellMarker:         return EDreamThemeSlot::SynCell;
	case TokenKind::RegionMarker:       return EDreamThemeSlot::SynCell;
	case TokenKind::Error:              return EDreamThemeSlot::SynError;
	case TokenKind::Text:
	case TokenKind::Count:
	default:                            return EDreamThemeSlot::SynNormal;
	}
}

FSlateColor FDreamEditorStyle::GetTokenColor(dream::TokenKind Kind)
{
	return FDreamThemes::Color(GetTokenSlot(Kind));
}

FSlateFontInfo FDreamEditorStyle::GetTokenFont(dream::TokenKind Kind, int32 Size)
{
	// Deferred (M1, needs the OFL font files): italic Comment/Docstring and bold Definition faces (§4.12) need the
	// JetBrains Mono composite; the engine's Mono typeface has a single face, and a proportional italic would break column
	// alignment.
	(void)Kind;
	return GetCodeFont(Size);
}

EDreamThemeSlot FDreamEditorStyle::GetLineTypeSlot(dream::LineType Type)
{
	using dream::LineType;
	switch (Type)
	{
	case LineType::Stdout:      return EDreamThemeSlot::OutStdout;
	case LineType::Stderr:      return EDreamThemeSlot::OutStderr;
	case LineType::Result:      return EDreamThemeSlot::OutResult;
	case LineType::Notice:      return EDreamThemeSlot::OutNotice;
	case LineType::Info:        return EDreamThemeSlot::OutInfo;
	case LineType::Warning:     return EDreamThemeSlot::OutWarning;
	case LineType::Error:       return EDreamThemeSlot::OutError;
	case LineType::RunHeader:   return EDreamThemeSlot::OutHeader;
	case LineType::RunFooter:   return EDreamThemeSlot::OutHeader;
	case LineType::Echo:        return EDreamThemeSlot::OutEcho;
	case LineType::Agent:       return EDreamThemeSlot::OutNotice;   // M2+ line kinds borrow the nearest slot (theme R-note)
	case LineType::Debugger:    return EDreamThemeSlot::OutNotice;
	case LineType::Logpoint:    return EDreamThemeSlot::OutWarning;
	case LineType::Evaluate:    return EDreamThemeSlot::OutResult;
	default:                    return EDreamThemeSlot::OutStdout;
	}
}

FSlateColor FDreamEditorStyle::GetLineTypeColor(dream::LineType Type)
{
	return FDreamThemes::Color(GetLineTypeSlot(Type));
}

const FTextBlockStyle& FDreamEditorStyle::GetCodeTextStyle()
{
	return Get().GetWidgetStyle<FTextBlockStyle>(DreamStyleLocal::CodeTextKey);
}
