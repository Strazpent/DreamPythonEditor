// SPDX-License-Identifier: Apache-2.0
// The built-in palettes (PO-04 "Palettes", hex sRGB). Darcula is PyCharm's built-in dark scheme with the exact values of
// the ruling's table. The other themes give seventeen colours each (background, output or panel, chrome, border, selection,
// text, muted, gutter text, keyword, string, number, function, builtin, comment, error, warning, result) plus an accent;
// the remaining slots are derived the same way for all of them (DeriveFromRow). Unreal is read from the editor at run time
// with exactly the slice-B colours.
#include "Shell/DreamThemeTable.h"

#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Styling/WidgetStyle.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamThemeTableLocal
{
	FColor Hex(uint32 Rgb)
	{
		return FColor(static_cast<uint8>((Rgb >> 16) & 0xFF), static_cast<uint8>((Rgb >> 8) & 0xFF), static_cast<uint8>(Rgb & 0xFF), 255);
	}

	/** A straight blend in sRGB space (the palette's own space), rounded to 8 bits. */
	FColor BlendSrgb(const FColor& A, const FColor& B, float T)
	{
		auto Channel = [T](uint8 X, uint8 Y)
		{
			return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(X) + (static_cast<float>(Y) - static_cast<float>(X)) * T), 0, 255));
		};
		return FColor(Channel(A.R, B.R), Channel(A.G, B.G), Channel(A.B, B.B), 255);
	}

	/** Moves a colour toward its own grey (Rec. 601 luma) by Amount. */
	FColor Desaturate(const FColor& C, float Amount)
	{
		const uint8 Luma = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(0.299f * C.R + 0.587f * C.G + 0.114f * C.B), 0, 255));
		return BlendSrgb(C, FColor(Luma, Luma, Luma, 255), Amount);
	}

	void SetSrgb(FDreamTheme& Theme, EDreamThemeSlot Slot, const FColor& C)
	{
		Theme.Set(Slot, FLinearColor::FromSRGBColor(C));
	}

	/** One row of the ruling's table, plus the accent (focus, the Run action, links). */
	struct FPaletteRow
	{
		const TCHAR* Id;
		bool bLight;
		uint32 Background, Panel, Chrome, Border, Selection, Text, Muted, GutterText, Keyword, String, Number, Function, Builtin,
			Comment, Error, Warning, Result, Accent;
	};

	/** (theme R-note) The derivation the ruling asks for: docstring between string and comment, decorator from function,
	 *  self as the keyword desaturated, the current line as the background lifted 4 % (toward white on a dark theme, black on
	 *  a light one), the gutter a third of the way from the background to the chrome, hover an eighth of the way from the
	 *  chrome to the text, find matches the warning colour over the background, stderr amber between warning and error. */
	void DeriveFromRow(FDreamTheme& T, const FPaletteRow& Row)
	{
		using S = EDreamThemeSlot;
		const FColor Bg = Hex(Row.Background);
		const FColor Chrome = Hex(Row.Chrome);
		const FColor Text = Hex(Row.Text);
		const FColor Muted = Hex(Row.Muted);
		const FColor Keyword = Hex(Row.Keyword);
		const FColor String = Hex(Row.String);
		const FColor Function = Hex(Row.Function);
		const FColor Comment = Hex(Row.Comment);
		const FColor ErrorC = Hex(Row.Error);
		const FColor WarningC = Hex(Row.Warning);
		const FColor Accent = Hex(Row.Accent);

		SetSrgb(T, S::ChromeBackground, Chrome);
		SetSrgb(T, S::ChromeBorder, Hex(Row.Border));
		SetSrgb(T, S::ChromeText, Text);
		SetSrgb(T, S::ChromeMutedText, Muted);
		SetSrgb(T, S::ChromeHover, BlendSrgb(Chrome, Text, 0.125f));
		SetSrgb(T, S::ChromeSelected, Bg);
		SetSrgb(T, S::Accent, Accent);
		SetSrgb(T, S::Danger, ErrorC);

		SetSrgb(T, S::OutputBackground, Hex(Row.Panel));
		SetSrgb(T, S::EditorBackground, Bg);
		SetSrgb(T, S::GutterBackground, BlendSrgb(Bg, Chrome, 0.35f));
		SetSrgb(T, S::GutterText, Hex(Row.GutterText));
		SetSrgb(T, S::CurrentLine, BlendSrgb(Bg, Row.bLight ? FColor::Black : FColor::White, 0.04f));
		SetSrgb(T, S::Selection, Hex(Row.Selection));
		SetSrgb(T, S::FindMatch, BlendSrgb(Bg, WarningC, 0.30f));
		SetSrgb(T, S::FindCurrent, BlendSrgb(Bg, WarningC, 0.50f));
		SetSrgb(T, S::Caret, Text);

		SetSrgb(T, S::OutEcho, Muted);
		SetSrgb(T, S::OutStdout, Text);
		SetSrgb(T, S::OutStderr, BlendSrgb(WarningC, ErrorC, 0.3f));
		SetSrgb(T, S::OutResult, Hex(Row.Result));
		SetSrgb(T, S::OutWarning, WarningC);
		SetSrgb(T, S::OutError, ErrorC);
		SetSrgb(T, S::OutNotice, Hex(Row.Number));
		SetSrgb(T, S::OutInfo, Muted);
		SetSrgb(T, S::OutLink, Accent);
		SetSrgb(T, S::OutHeader, Muted);

		SetSrgb(T, S::SynNormal, Text);
		SetSrgb(T, S::SynKeyword, Keyword);
		SetSrgb(T, S::SynBuiltin, Hex(Row.Builtin));
		SetSrgb(T, S::SynSelf, Desaturate(Keyword, 0.4f));
		SetSrgb(T, S::SynDefinition, Function);
		SetSrgb(T, S::SynDecorator, Function);
		SetSrgb(T, S::SynNumber, Hex(Row.Number));
		SetSrgb(T, S::SynString, String);
		SetSrgb(T, S::SynDocstring, BlendSrgb(String, Comment, 0.3f));
		SetSrgb(T, S::SynEscape, Keyword);
		SetSrgb(T, S::SynOperator, Text);
		SetSrgb(T, S::SynComment, Comment);
		SetSrgb(T, S::SynCell, Comment);
		SetSrgb(T, S::SynError, ErrorC);
		SetSrgb(T, S::SynWarning, WarningC);   // (lang) PO-06 F4: the row's warning colour
	}

	TSharedRef<FDreamTheme> MakeTheme(const TCHAR* Id, const FText& Name, bool bLight)
	{
		TSharedRef<FDreamTheme> Theme = MakeShared<FDreamTheme>();
		Theme->Id = Id;
		Theme->Name = Name;
		Theme->bLight = bLight;
		Theme->bBuiltIn = true;
		return Theme;
	}

	/** PyCharm's Darcula, every slot exact (PO-04 table). */
	TSharedRef<FDreamTheme> MakeDarcula()
	{
		using S = EDreamThemeSlot;
		TSharedRef<FDreamTheme> Theme = MakeTheme(DreamThemeTable::DarculaId, LOCTEXT("Theme.Darcula", "Darcula"), false);
		FDreamTheme& T = *Theme;
		SetSrgb(T, S::ChromeBackground, Hex(0x3C3F41));
		SetSrgb(T, S::ChromeBorder, Hex(0x2B2B2B));
		SetSrgb(T, S::ChromeText, Hex(0xBBBBBB));
		SetSrgb(T, S::ChromeMutedText, Hex(0x8C8C8C));
		SetSrgb(T, S::ChromeHover, Hex(0x4C5052));
		SetSrgb(T, S::ChromeSelected, Hex(0x2B2B2B));
		SetSrgb(T, S::Accent, Hex(0x4A88C7));
		SetSrgb(T, S::Danger, Hex(0xC75450));
		SetSrgb(T, S::OutputBackground, Hex(0x2B2B2B));
		SetSrgb(T, S::EditorBackground, Hex(0x2B2B2B));
		SetSrgb(T, S::GutterBackground, Hex(0x313335));
		SetSrgb(T, S::GutterText, Hex(0x606366));
		SetSrgb(T, S::CurrentLine, Hex(0x323232));
		SetSrgb(T, S::Selection, Hex(0x214283));
		SetSrgb(T, S::FindMatch, Hex(0x32593D));
		SetSrgb(T, S::FindCurrent, Hex(0x3C7D4A));
		SetSrgb(T, S::Caret, Hex(0xBBBBBB));
		SetSrgb(T, S::OutEcho, Hex(0x808080));
		SetSrgb(T, S::OutStdout, Hex(0xBBBBBB));
		SetSrgb(T, S::OutStderr, Hex(0xD9A343));
		SetSrgb(T, S::OutResult, Hex(0x6897BB));
		SetSrgb(T, S::OutWarning, Hex(0xBBB529));
		SetSrgb(T, S::OutError, Hex(0xFF6B68));
		SetSrgb(T, S::OutNotice, Hex(0x9876AA));
		SetSrgb(T, S::OutInfo, Hex(0x8C8C8C));
		SetSrgb(T, S::OutLink, Hex(0x589DF6));
		SetSrgb(T, S::OutHeader, Hex(0x8C8C8C));
		SetSrgb(T, S::SynNormal, Hex(0xA9B7C6));
		SetSrgb(T, S::SynKeyword, Hex(0xCC7832));
		SetSrgb(T, S::SynBuiltin, Hex(0x8888C6));
		SetSrgb(T, S::SynSelf, Hex(0x94558D));
		SetSrgb(T, S::SynDefinition, Hex(0xFFC66D));
		SetSrgb(T, S::SynDecorator, Hex(0xBBB529));
		SetSrgb(T, S::SynNumber, Hex(0x6897BB));
		SetSrgb(T, S::SynString, Hex(0x6A8759));
		SetSrgb(T, S::SynDocstring, Hex(0x629755));
		SetSrgb(T, S::SynEscape, Hex(0xCC7832));
		SetSrgb(T, S::SynOperator, Hex(0xA9B7C6));
		SetSrgb(T, S::SynComment, Hex(0x808080));
		SetSrgb(T, S::SynCell, Hex(0x808080));
		SetSrgb(T, S::SynError, Hex(0xBC3F3C));
		SetSrgb(T, S::SynWarning, Hex(0xBE9117));   // (lang) PO-06 F4: PyCharm Darcula's warning wave
		return Theme;
	}

	TSharedRef<FDreamTheme> MakeFromRow(const FPaletteRow& Row, const FText& Name)
	{
		TSharedRef<FDreamTheme> Theme = MakeTheme(Row.Id, Name, Row.bLight);
		DeriveFromRow(*Theme, Row);
		return Theme;
	}

	/** A source-code text style's colour, or Fallback when the app style lacks it (StarshipStyle.cpp:8853-8861). */
	FLinearColor StockSourceColor(const TCHAR* StockKey, const FColor& Fallback)
	{
		const ISlateStyle& App = FAppStyle::Get();
		if (App.HasWidgetStyle<FTextBlockStyle>(StockKey))
		{
			return App.GetWidgetStyle<FTextBlockStyle>(StockKey).ColorAndOpacity.GetSpecifiedColor();
		}
		return FLinearColor(Fallback);
	}
}

void DreamThemeTable::BuildBuiltIns(TArray<TSharedRef<FDreamTheme>>& Out)
{
	using namespace DreamThemeTableLocal;
	// Background / output or panel / chrome / border / selection / text / muted / gutter text / keyword / string / number /
	// function / builtin / comment / error / warning / result, then the accent (theme R-note: picked from each scheme's own
	// focus colour; Maya's is its selection blue).
	static const FPaletteRow Dracula{ TEXT("Dracula"), false, 0x282A36, 0x21222C, 0x191A21, 0x343746, 0x44475A, 0xF8F8F2, 0x8D93B8, 0x6272A4, 0xFF79C6, 0xF1FA8C, 0xBD93F9, 0x50FA7B, 0x8BE9FD, 0x6272A4, 0xFF5555, 0xFFB86C, 0xBD93F9, 0xBD93F9 };
	static const FPaletteRow Maya{ TEXT("Maya"), false, 0x2B2B2B, 0x262626, 0x444444, 0x373737, 0x5D5D5D, 0xC8C8C8, 0x9A9A9A, 0x707070, 0x7EB6E8, 0x9BCB7A, 0xE3B36A, 0xE2E2E2, 0xB9A0E0, 0xD08770, 0xF08080, 0xE5C07B, 0x9A9A9A, 0x5285A6 };
	static const FPaletteRow Monokai{ TEXT("Monokai"), false, 0x272822, 0x1E1F1C, 0x1E1F1C, 0x3E3D32, 0x49483E, 0xF8F8F2, 0xA59F85, 0x90908A, 0xF92672, 0xE6DB74, 0xAE81FF, 0xA6E22E, 0x66D9EF, 0x75715E, 0xF92672, 0xFD971F, 0xAE81FF, 0x66D9EF };
	static const FPaletteRow OneDark{ TEXT("One Dark"), false, 0x282C34, 0x21252B, 0x21252B, 0x181A1F, 0x3E4451, 0xABB2BF, 0x7F848E, 0x636D83, 0xC678DD, 0x98C379, 0xD19A66, 0x61AFEF, 0xE5C07B, 0x5C6370, 0xE06C75, 0xD19A66, 0x56B6C2, 0x61AFEF };
	static const FPaletteRow Nord{ TEXT("Nord"), false, 0x2E3440, 0x272C36, 0x3B4252, 0x434C5E, 0x4C566A, 0xD8DEE9, 0xA3ABBA, 0x616E88, 0x81A1C1, 0xA3BE8C, 0xB48EAD, 0x88C0D0, 0x8FBCBB, 0x616E88, 0xBF616A, 0xEBCB8B, 0xB48EAD, 0x88C0D0 };
	static const FPaletteRow Gruvbox{ TEXT("Gruvbox Dark"), false, 0x282828, 0x1D2021, 0x3C3836, 0x504945, 0x504945, 0xEBDBB2, 0xA89984, 0x7C6F64, 0xFB4934, 0xB8BB26, 0xD3869B, 0xFABD2F, 0x83A598, 0x928374, 0xFB4934, 0xFE8019, 0xD3869B, 0x83A598 };
	static const FPaletteRow SolarizedLight{ TEXT("Solarized Light"), true, 0xFDF6E3, 0xEEE8D5, 0xEEE8D5, 0xDDD6C1, 0xDDD6C1, 0x586E75, 0x839496, 0x93A1A1, 0x859900, 0x2AA198, 0xD33682, 0x268BD2, 0xB58900, 0x93A1A1, 0xDC322F, 0xCB4B16, 0x6C71C4, 0x268BD2 };
	static const FPaletteRow GitHubLight{ DreamThemeTable::GitHubLightId, true, 0xFFFFFF, 0xF6F8FA, 0xF6F8FA, 0xD0D7DE, 0xDDF4FF, 0x24292F, 0x57606A, 0x8C959F, 0xCF222E, 0x0A3069, 0x0550AE, 0x8250DF, 0x953800, 0x6E7781, 0xCF222E, 0x9A6700, 0x0550AE, 0x0969DA };

	Out.Reset();
	Out.Add(MakeDarcula());
	Out.Add(MakeFromRow(Dracula, LOCTEXT("Theme.Dracula", "Dracula")));
	Out.Add(MakeFromRow(Maya, LOCTEXT("Theme.Maya", "Maya")));
	Out.Add(MakeFromRow(Monokai, LOCTEXT("Theme.Monokai", "Monokai")));
	Out.Add(MakeFromRow(OneDark, LOCTEXT("Theme.OneDark", "One Dark")));
	Out.Add(MakeFromRow(Nord, LOCTEXT("Theme.Nord", "Nord")));
	Out.Add(MakeFromRow(Gruvbox, LOCTEXT("Theme.GruvboxDark", "Gruvbox Dark")));
	Out.Add(MakeFromRow(SolarizedLight, LOCTEXT("Theme.SolarizedLight", "Solarized Light")));
	Out.Add(MakeFromRow(GitHubLight, LOCTEXT("Theme.GitHubLight", "GitHub Light")));
	Out.Add(MakeUnreal());
}

TSharedRef<FDreamTheme> DreamThemeTable::MakeUnreal()
{
	using namespace DreamThemeTableLocal;
	using S = EDreamThemeSlot;
	TSharedRef<FDreamTheme> Theme = MakeTheme(UnrealId, LOCTEXT("Theme.Unreal", "Unreal"), false);
	FDreamTheme& T = *Theme;

	// Syntax (T-VIEW-014): the stock source-code keys, the extra kinds derived from that palette exactly as slice B did.
	const FLinearColor Normal = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Normal"), FColor(189, 183, 107));
	const FLinearColor Operator = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Operator"), FColor(220, 220, 220));
	const FLinearColor Keyword = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Keyword"), FColor(86, 156, 214));
	const FLinearColor String = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.String"), FColor(214, 157, 133));
	const FLinearColor Number = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Number"), FColor(181, 206, 168));
	const FLinearColor Comment = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Comment"), FColor(87, 166, 74));
	const FLinearColor PreProcessor = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.PreProcessorKeyword"), FColor(188, 98, 171));
	const FLinearColor ErrorColor = StockSourceColor(TEXT("SyntaxHighlight.SourceCode.Error"), FColor(255, 0, 0));
	T.Set(S::SynNormal, Normal);
	T.Set(S::SynOperator, Operator);
	T.Set(S::SynKeyword, Keyword);
	T.Set(S::SynString, String);
	T.Set(S::SynNumber, Number);
	T.Set(S::SynComment, Comment);
	T.Set(S::SynDecorator, PreProcessor);
	T.Set(S::SynError, ErrorColor);
	T.Set(S::SynWarning, FStyleColors::Warning.GetSpecifiedColor());   // (lang) PO-06 F4
	T.Set(S::SynBuiltin, FLinearColor::LerpUsingHSV(Keyword, Number, 0.5f));
	T.Set(S::SynSelf, Keyword.Desaturate(0.35f));
	T.Set(S::SynDefinition, Normal);
	T.Set(S::SynDocstring, Comment.Desaturate(0.25f));
	T.Set(S::SynEscape, PreProcessor);
	T.Set(S::SynCell, Comment);

	// Chrome and panes: the FStyleColors the slice-B widgets used (Brushes.Panel, .Recessed, .Header; the subdued foreground).
	const FLinearColor Foreground = FStyleColors::Foreground.GetSpecifiedColor();
	const FLinearColor Subdued = FWidgetStyle().SetForegroundColor(Foreground).GetSubduedForegroundColor();   // UseSubduedForeground()
	T.Set(S::ChromeBackground, FStyleColors::Panel.GetSpecifiedColor());
	T.Set(S::ChromeBorder, FStyleColors::Background.GetSpecifiedColor());
	T.Set(S::ChromeText, Foreground);
	T.Set(S::ChromeMutedText, Subdued);
	T.Set(S::ChromeHover, FStyleColors::Hover.GetSpecifiedColor());
	T.Set(S::ChromeSelected, FStyleColors::Recessed.GetSpecifiedColor());
	T.Set(S::Accent, FStyleColors::Primary.GetSpecifiedColor());
	T.Set(S::Danger, FStyleColors::Error.GetSpecifiedColor());
	T.Set(S::OutputBackground, FStyleColors::Recessed.GetSpecifiedColor());
	T.Set(S::EditorBackground, FStyleColors::Recessed.GetSpecifiedColor());
	T.Set(S::GutterBackground, FStyleColors::Recessed.GetSpecifiedColor());
	T.Set(S::GutterText, FStyleColors::AccentGray.GetSpecifiedColor());
	T.Set(S::CurrentLine, FLinearColor(1.f, 1.f, 1.f, 0.045f));
	// The stock selection of a text style whose SelectedBackgroundColor is the foreground (FTextSelectionHighlighter), which
	// is what the slice-B code view showed: its foreground was the Normal colour.
	T.Set(S::Selection, (FLinearColor::White - Normal) * 0.5f + FLinearColor(-0.2f, -0.05f, 0.15f));
	T.Set(S::FindMatch, FLinearColor(1.f, 0.78f, 0.2f, 0.22f));
	T.Set(S::FindCurrent, FLinearColor(1.f, 0.62f, 0.1f, 0.55f));
	T.Set(S::Caret, Normal);   // the stock caret is the text style's colour

	// Output line kinds (§6.4 table as slice B had it; stderr is amber, never the error colour).
	T.Set(S::OutStdout, Foreground);
	T.Set(S::OutStderr, FStyleColors::AccentOrange.GetSpecifiedColor());
	T.Set(S::OutResult, FStyleColors::AccentBlue.GetSpecifiedColor());
	T.Set(S::OutInfo, FStyleColors::AccentGray.GetSpecifiedColor());
	T.Set(S::OutWarning, FStyleColors::Warning.GetSpecifiedColor());
	T.Set(S::OutError, FStyleColors::Error.GetSpecifiedColor());
	T.Set(S::OutNotice, FStyleColors::AccentPurple.GetSpecifiedColor());
	T.Set(S::OutEcho, FLinearColor::FromSRGBColor(FColor(140, 140, 140)));
	T.Set(S::OutHeader, FStyleColors::ForegroundHeader.GetSpecifiedColor());
	T.Set(S::OutLink, FStyleColors::AccentBlue.GetSpecifiedColor());
	return Theme;
}

#undef LOCTEXT_NAMESPACE
