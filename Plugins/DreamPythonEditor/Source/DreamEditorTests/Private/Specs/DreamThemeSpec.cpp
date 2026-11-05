// SPDX-License-Identifier: Apache-2.0
// Dream.Theme.*: the themes of owner ruling PO-04 (Shell/DreamTheme.h).
//   Dream.Theme.BuiltIns  the ten built-ins in menu order, each with every slot; Darcula's exact palette; each one loads
//                        (SetCurrent) and FDreamEditorStyle's getters, the brushes and the code text style follow it.
//   Dream.Theme.Default   Darcula is the default of a fresh profile (a settings object built without any config).
//   Dream.Theme.Json      T-VIEW-015's round trip: export -> import -> export is identical for every built-in, also through
//                        files (ExportBuiltIns, Dream.Theme.Export); every 8-bit sRGB value survives FLinearColor; a partial
//                        file takes missing and invalid colours from its base and ignores unknown keys; user themes are
//                        read lazily from the themes folder (broken files and built-in ids skipped, a removed current
//                        theme falls back to Darcula, a folder change is picked up by a deferred reload within a
//                        second or so, without an explicit Reload); folder files need
//                        "format", ids with control characters take the file name, files over 1 MB and subfolders
//                        are skipped; every slot's JSON key is in EDreamThemeSlot order.
//   Dream.Theme.Dropdown  (attended) the Editor Preferences dropdown: the customization is in the class layout map for
//                        UDreamUserSettings, a details view on the settings shows the combo with the setting's theme on
//                        the ThemeId row, and DreamThemeDetails::MakeThemeMenu (the menu the combo opens) has an entry for
//                        every theme, a user theme included.
//   Dream.Theme.Unreal    T-VIEW-014 for the Unreal theme: its syntax colours equal SyntaxHighlight.SourceCode.*, its
//                        chrome, pane and output colours are the slice-B FStyleColors.
//   Dream.Theme.LiveApply T-VIEW-015's live apply (attended): switching repaints the open code view at once in the new
//                        colours (pixels of the background, gutter band, current line, selection and a keyword's glyphs),
//                        through SetCurrent and through an Editor Preferences edit of ThemeId; the Run window of a
//                        pumping run is painted in the theme's chrome and history colours.
// Every spec restores the theme and UDreamUserSettings::ThemeId it found.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IDetailsView.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamText.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamTheme.h"
#include "Shell/DreamThemeDetails.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Styling/WidgetStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "dream/ThemeFormat.h"

namespace DreamThemeSpec
{
	const TCHAR* const BuiltInIds[] = { TEXT("Darcula"), TEXT("Dracula"), TEXT("Maya"), TEXT("Monokai"), TEXT("One Dark"), TEXT("Nord"),
		TEXT("Gruvbox Dark"), TEXT("Solarized Light"), TEXT("GitHub Light"), TEXT("Unreal") };

	/** PO-04's Darcula table, in EDreamThemeSlot order. */
	const uint32 Darcula[] = {
		0x3C3F41, 0x2B2B2B, 0xBBBBBB, 0x8C8C8C, 0x4C5052, 0x2B2B2B, 0x4A88C7, 0xC75450,                       // chrome
		0x2B2B2B, 0x2B2B2B, 0x313335, 0x606366, 0x323232, 0x214283, 0x32593D, 0x3C7D4A, 0xBBBBBB,             // panes
		0x808080, 0xBBBBBB, 0xD9A343, 0x6897BB, 0xBBB529, 0xFF6B68, 0x9876AA, 0x8C8C8C, 0x589DF6, 0x8C8C8C,   // output
		0xA9B7C6, 0xCC7832, 0x8888C6, 0x94558D, 0xFFC66D, 0xBBB529, 0x6897BB, 0x6A8759, 0x629755, 0xCC7832,   // syntax
		0xA9B7C6, 0x808080, 0x808080, 0xBC3F3C, 0xBE9117,   // (lang) PO-06 F4: syntax.warning
	};
	static_assert(UE_ARRAY_COUNT(Darcula) == static_cast<int32>(EDreamThemeSlot::Count), "one Darcula value per slot");

	constexpr int32 SlotCount = static_cast<int32>(EDreamThemeSlot::Count);

	FString Hex(const FColor& C)
	{
		return C.A == 255 ? FString::Printf(TEXT("#%02X%02X%02X"), C.R, C.G, C.B) : FString::Printf(TEXT("#%02X%02X%02X%02X"), C.R, C.G, C.B, C.A);
	}

	FColor Srgb(const FLinearColor& C)
	{
		return C.ToFColorSRGB();
	}

	int32 Distance(const FColor& A, const FColor& B)
	{
		return FMath::Abs(A.R - B.R) + FMath::Abs(A.G - B.G) + FMath::Abs(A.B - B.B);
	}

	bool Near(const FColor& A, const FColor& B, int32 Tolerance)
	{
		return FMath::Abs(A.R - B.R) <= Tolerance && FMath::Abs(A.G - B.G) <= Tolerance && FMath::Abs(A.B - B.B) <= Tolerance;
	}

	/** How far an anti-aliased glyph pixel P is from any blend of Ink over Bg (the segment Bg..Ink in sRGB space). */
	double BlendDistance(const FColor& P, const FColor& Bg, const FColor& Ink)
	{
		const FVector D(Ink.R - Bg.R, Ink.G - Bg.G, Ink.B - Bg.B);
		const FVector V(P.R - Bg.R, P.G - Bg.G, P.B - Bg.B);
		const double Len2 = D.SizeSquared();
		const double T = Len2 > 0.0 ? FMath::Clamp(FVector::DotProduct(V, D) / Len2, 0.0, 1.0) : 0.0;
		return (V - D * T).Size();
	}

	/** The current theme and the setting, restored by every spec. */
	struct FSaved
	{
		FString CurrentId;
		FString SettingId;

		void Capture()
		{
			CurrentId = FDreamThemes::GetCurrentId();
			SettingId = UDreamUserSettings::Get()->ThemeId;
		}

		void Restore() const
		{
			// The setting through its own path (SetThemeId writes this one key with SaveKey and broadcasts, so the theme's
			// last-seen id follows), then the theme that was current (they differ when ThemeId names a missing user theme).
			if (!SettingId.IsEmpty() && !UDreamUserSettings::Get()->ThemeId.Equals(SettingId, ESearchCase::CaseSensitive))
			{
				UDreamUserSettings::SetThemeId(SettingId);
			}
			if (!CurrentId.IsEmpty())
			{
				FDreamThemes::SetCurrent(CurrentId, /*bPersist=*/false);
			}
		}
	};

	/** Every STextBlock's text under Widget. */
	void CollectTexts(const TSharedRef<SWidget>& Widget, TArray<FString>& Out)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			Out.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			CollectTexts(Children->GetChildAt(Index), Out);
		}
	}

	/** The first widget under Widget tagged Tag (FTagMetaData). */
	TSharedPtr<SWidget> FindTagged(const TSharedRef<SWidget>& Widget, FName Tag)
	{
		const TSharedPtr<FTagMetaData> Meta = Widget->GetMetaData<FTagMetaData>();
		if (Meta.IsValid() && Meta->Tag == Tag)
		{
			return Widget;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindTagged(Children->GetChildAt(Index), Tag))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** How many of 16 points down a screenshot's right edge (left of a scroll bar) are Expected. */
	int32 RightEdgeMatches(const TArray<FColor>& Pixels, const FIntVector& Size, double Scale, const FColor& Expected, FColor& OutOther)
	{
		int32 Matching = 0;
		OutOther = Expected;
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const int32 X = FMath::Clamp(FMath::FloorToInt32(Size.X - 20.0 * Scale), 0, Size.X - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt32((Index + 0.5) * Size.Y / 16.0), 0, Size.Y - 1);
			const FColor P = Pixels[Y * Size.X + X];
			if (Near(P, Expected, 3))
			{
				++Matching;
			}
			else
			{
				OutOther = P;
			}
		}
		return Matching;
	}

	/** A fresh scratch folder under the automation transient folder. */
	FString MakeTempDir(const TCHAR* Purpose)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(),
			FString::Printf(TEXT("DreamThemes-%s-%s"), Purpose, *DreamTest::UniqueSuffix())));
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	bool WriteFile(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeBuiltInsSpec, "Dream.Theme.BuiltIns", DreamTest::AnyEditorFlags)
	DreamThemeSpec::FSaved Saved;
END_DEFINE_SPEC(FDreamThemeBuiltInsSpec)

void FDreamThemeBuiltInsSpec::Define()
{
	BeforeEach([this]() { Saved.Capture(); });
	AfterEach([this]() { Saved.Restore(); });

	It("lists the ten built-ins in menu order, Darcula first, built-ins before user themes", [this]()
	{
		const TArray<TSharedRef<const FDreamTheme>> Themes = FDreamThemes::List();
		TArray<FString> BuiltIns;
		bool bUserSeen = false;
		bool bOrderOk = true;
		for (const TSharedRef<const FDreamTheme>& Theme : Themes)
		{
			if (Theme->bBuiltIn)
			{
				bOrderOk &= !bUserSeen;
				BuiltIns.Add(Theme->Id);
			}
			else
			{
				bUserSeen = true;
			}
		}
		TestTrue(TEXT("built-ins come before user themes"), bOrderOk);
		TestEqual(TEXT("built-in count"), BuiltIns.Num(), static_cast<int32>(UE_ARRAY_COUNT(DreamThemeSpec::BuiltInIds)));
		for (int32 Index = 0; Index < BuiltIns.Num() && Index < static_cast<int32>(UE_ARRAY_COUNT(DreamThemeSpec::BuiltInIds)); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("built-in #%d"), Index), BuiltIns[Index], FString(DreamThemeSpec::BuiltInIds[Index]));
		}
		TestEqual(TEXT("FDreamThemes::GetDefaultId"), FDreamThemes::GetDefaultId(), FString(TEXT("Darcula")));
		const TSharedPtr<const FDreamTheme> Solarized = FDreamThemes::Find(TEXT("Solarized Light"));
		const TSharedPtr<const FDreamTheme> GitHub = FDreamThemes::Find(TEXT("github light"));   // ids match without case
		TestTrue(TEXT("Solarized Light and GitHub Light are light themes, Darcula is not"),
			Solarized.IsValid() && Solarized->bLight && GitHub.IsValid() && GitHub->bLight && !FDreamThemes::Find(TEXT("Darcula"))->bLight);
		TestFalse(TEXT("an unknown id is not found"), FDreamThemes::Find(TEXT("No Such Theme")).IsValid());
	});

	It("gives every built-in every slot: a finite colour, visible, written to its JSON", [this]()
	{
		for (const TCHAR* Id : DreamThemeSpec::BuiltInIds)
		{
			const TSharedPtr<const FDreamTheme> Theme = FDreamThemes::Find(Id);
			if (!TestTrue(*FString::Printf(TEXT("%s exists"), Id), Theme.IsValid()))
			{
				continue;
			}
			const FString Json = FDreamThemes::ExportJson(*Theme);
			int32 Bad = 0;
			FString BadSlots;
			for (int32 Slot = 0; Slot < DreamThemeSpec::SlotCount; ++Slot)
			{
				const FLinearColor& C = Theme->Colors[Slot];
				const bool bFinite = FMath::IsFinite(C.R) && FMath::IsFinite(C.G) && FMath::IsFinite(C.B) && FMath::IsFinite(C.A);
				if (!bFinite || C.A <= 0.f || C.A > 1.f)
				{
					++Bad;
					BadSlots += FString::Printf(TEXT(" %d"), Slot);
				}
			}
			TestEqual(*FString::Printf(TEXT("%s: slots without a usable colour (%s)"), Id, *BadSlots), Bad, 0);
			int32 Missing = 0;
			for (int32 Slot = 0; Slot < DreamThemeSpec::SlotCount; ++Slot)
			{
				const std::string_view Key = dream::theme::SlotKey(Slot);
				const FString Needle = FString::Printf(TEXT("\"%s\": \"#"), *DreamText::Utf8ToFString(Key));
				Missing += Json.Contains(Needle, ESearchCase::CaseSensitive) ? 0 : 1;
			}
			TestEqual(*FString::Printf(TEXT("%s: slot keys missing from its JSON"), Id), Missing, 0);
		}
	});

	It("holds PO-04's Darcula palette exactly", [this]()
	{
		const TSharedPtr<const FDreamTheme> Theme = FDreamThemes::Find(TEXT("Darcula"));
		if (!TestTrue(TEXT("Darcula exists"), Theme.IsValid()))
		{
			return;
		}
		for (int32 Slot = 0; Slot < DreamThemeSpec::SlotCount; ++Slot)
		{
			const uint32 Rgb = DreamThemeSpec::Darcula[Slot];
			const FColor Expected(static_cast<uint8>(Rgb >> 16), static_cast<uint8>((Rgb >> 8) & 0xFF), static_cast<uint8>(Rgb & 0xFF), 255);
			const FColor Actual = DreamThemeSpec::Srgb(Theme->Colors[Slot]);
			TestEqual(*FString::Printf(TEXT("Darcula slot %d"), Slot), DreamThemeSpec::Hex(Actual), DreamThemeSpec::Hex(Expected));
		}
	});

	It("follows ThemeId only when it changes: another View setting keeps a theme chosen without saving (review r6 m1)", [this]()
	{
		const FString Other = FDreamThemes::GetCurrentId() == TEXT("Nord") ? TEXT("Monokai") : TEXT("Nord");
		TestTrue(TEXT("SetCurrent without saving"), FDreamThemes::SetCurrent(Other, /*bPersist=*/false));
		UDreamUserSettings::OnViewSettingsChanged().Broadcast();   // what any View toggle does (no ini write here)
		TestEqual(TEXT("an unrelated View setting change keeps the theme"), FDreamThemes::GetCurrentId(), Other);
		const FString Changed = Other == TEXT("Nord") ? TEXT("Dracula") : TEXT("One Dark");
		UDreamUserSettings::SetThemeId(Changed);   // a real ThemeId change (saved; FSaved restores it)
		TestEqual(TEXT("a ThemeId change applies"), FDreamThemes::GetCurrentId(), Changed);
		UDreamUserSettings::OnViewSettingsChanged().Broadcast();
		TestEqual(TEXT("and stays after another View setting change"), FDreamThemes::GetCurrentId(), Changed);
	});

	It("loads every built-in: SetCurrent applies it and the style getters, brushes and code text style follow", [this]()
	{
		const FSlateBrush* EditorBrush = FDreamThemes::Brush(EDreamThemeSlot::EditorBackground);
		for (const TCHAR* Id : DreamThemeSpec::BuiltInIds)
		{
			if (!TestTrue(*FString::Printf(TEXT("SetCurrent(%s)"), Id), FDreamThemes::SetCurrent(Id, /*bPersist=*/false)))
			{
				continue;
			}
			const FDreamTheme& T = FDreamThemes::Current();
			TestEqual(TEXT("current id"), T.Id, FString(Id));
			TestEqual(*FString::Printf(TEXT("%s: GetCurrentId"), Id), FDreamThemes::GetCurrentId(), FString(Id));
			TestTrue(*FString::Printf(TEXT("%s: GetTokenColor(Keyword)"), Id),
				FDreamEditorStyle::GetTokenColor(dream::TokenKind::Keyword).GetSpecifiedColor() == T.Get(EDreamThemeSlot::SynKeyword));
			TestTrue(*FString::Printf(TEXT("%s: GetTokenColor(Docstring)"), Id),
				FDreamEditorStyle::GetTokenColor(dream::TokenKind::Docstring).GetSpecifiedColor() == T.Get(EDreamThemeSlot::SynDocstring));
			TestTrue(*FString::Printf(TEXT("%s: GetLineTypeColor(Error)"), Id),
				FDreamEditorStyle::GetLineTypeColor(dream::LineType::Error).GetSpecifiedColor() == T.Get(EDreamThemeSlot::OutError));
			TestTrue(*FString::Printf(TEXT("%s: GetLineTypeColor(RunFooter) is the header colour"), Id),
				FDreamEditorStyle::GetLineTypeColor(dream::LineType::RunFooter).GetSpecifiedColor() == T.Get(EDreamThemeSlot::OutHeader));
			TestTrue(*FString::Printf(TEXT("%s: Color(Accent)"), Id), FDreamThemes::Color(EDreamThemeSlot::Accent).GetSpecifiedColor() == T.Get(EDreamThemeSlot::Accent));
			TestTrue(*FString::Printf(TEXT("%s: ColorAttr reads the theme"), Id),
				FDreamThemes::ColorAttr(EDreamThemeSlot::OutLink).Get().GetSpecifiedColor() == T.Get(EDreamThemeSlot::OutLink));
			TestTrue(*FString::Printf(TEXT("%s: the editor brush is the same pointer, retinted"), Id),
				FDreamThemes::Brush(EDreamThemeSlot::EditorBackground) == EditorBrush
				&& EditorBrush->TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::EditorBackground));
			TestTrue(*FString::Printf(TEXT("%s: the flat button hover is ChromeHover"), Id),
				FDreamThemes::FlatButtonStyle().Hovered.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeHover));
			TestTrue(*FString::Printf(TEXT("%s: the text box outline is ChromeBorder"), Id),
				FDreamThemes::TextBoxStyle().BackgroundImageNormal.OutlineSettings.Color.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeBorder));
			TestTrue(*FString::Printf(TEXT("%s: the scroll bar thumb is ChromeHover, ChromeMutedText when dragged"), Id),
				FDreamThemes::ScrollBarStyle().NormalThumbImage.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeHover)
				&& FDreamThemes::ScrollBarStyle().DraggedThumbImage.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeMutedText));
			TestTrue(*FString::Printf(TEXT("%s: a selected list row is Selection, a hovered one ChromeHover"), Id),
				FDreamThemes::TableRowStyle().ActiveBrush.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::Selection)
				&& FDreamThemes::TableRowStyle().InactiveBrush.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::Selection)
				&& FDreamThemes::TableRowStyle().EvenRowBackgroundHoveredBrush.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeHover));
			TestTrue(*FString::Printf(TEXT("%s: the progress track is ChromeBorder"), Id),
				FDreamThemes::ProgressBarStyle().BackgroundImage.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::ChromeBorder));
			TestTrue(*FString::Printf(TEXT("%s: list views paint no background of their own"), Id),
				FDreamThemes::ListViewStyle().BackgroundBrush.DrawAs == ESlateBrushDrawType::NoDrawType);
			if (FSlateStyleRegistry::FindSlateStyle(FDreamEditorStyle::GetStyleSetName()) != nullptr)   // no style set when headless
			{
				const FTextBlockStyle& Code = FDreamEditorStyle::GetCodeTextStyle();
				TestTrue(*FString::Printf(TEXT("%s: the code text colour (caret) is Caret"), Id), Code.ColorAndOpacity.GetSpecifiedColor() == T.Get(EDreamThemeSlot::Caret));
				TestTrue(*FString::Printf(TEXT("%s: the code selection is Selection"), Id), Code.SelectedBackgroundColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::Selection));
				TestTrue(*FString::Printf(TEXT("%s: the style set's output link key"), Id),
					FDreamEditorStyle::Get().GetSlateColor(DreamStyleKeys::OutputLink).GetSpecifiedColor() == T.Get(EDreamThemeSlot::OutLink));
				TestTrue(*FString::Printf(TEXT("%s: the Run window's Stop is Danger"), Id),
					FDreamEditorStyle::Get().GetWidgetStyle<FButtonStyle>(DreamStyleKeys::RunWindowStopButton).Normal.TintColor.GetSpecifiedColor() == T.Get(EDreamThemeSlot::Danger));
			}
		}
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeDefaultSpec, "Dream.Theme.Default", DreamTest::AnyEditorFlags)
END_DEFINE_SPEC(FDreamThemeDefaultSpec)

void FDreamThemeDefaultSpec::Define()
{
	It("is Darcula for a fresh profile (PO-04)", [this]()
	{
		// A settings object built by its C++ constructor alone, without the archetype copy and without any ini: what a
		// profile that never chose a theme reads.
		UClass* Class = UDreamUserSettings::StaticClass();
		UObject* Raw = StaticAllocateObject(Class, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), Class, FName(TEXT("DreamT4FreshSettings"))), RF_Transient);
		(*Class->ClassConstructor)(FObjectInitializer(Raw, nullptr, EObjectInitializerOptions::None));
		const UDreamUserSettings* Fresh = CastChecked<UDreamUserSettings>(Raw);
		TestEqual(TEXT("a fresh profile's ThemeId"), Fresh->ThemeId, FString(TEXT("Darcula")));
		TestEqual(TEXT("the theme default"), FDreamThemes::GetDefaultId(), FString(TEXT("Darcula")));
		const TArray<TSharedRef<const FDreamTheme>> Themes = FDreamThemes::List();
		TestTrue(TEXT("Darcula is the first theme"), Themes.Num() > 0 && Themes[0]->Id == TEXT("Darcula"));
		TestTrue(TEXT("Darcula is a dark built-in"), Themes.Num() > 0 && Themes[0]->bBuiltIn && !Themes[0]->bLight);

		// This profile: with no ThemeId in EditorSettings.ini the loaded setting is the C++ default.
		FString InIni;
		const bool bInIni = GConfig != nullptr
			&& GConfig->GetString(*Class->GetPathName(), TEXT("ThemeId"), InIni, GEditorSettingsIni);
		if (!bInIni)
		{
			TestEqual(TEXT("ThemeId of this profile (no ini key)"), UDreamUserSettings::Get()->ThemeId, FString(TEXT("Darcula")));
		}
		else
		{
			AddInfo(FString::Printf(TEXT("this profile chose ThemeId=%s in EditorSettings.ini"), *InIni));
		}
		TestFalse(TEXT("an empty id selects nothing"), FDreamThemes::SetCurrent(FString(), false));
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeJsonSpec, "Dream.Theme.Json", DreamTest::AnyEditorFlags)
	DreamThemeSpec::FSaved Saved;
	FString TempDir;
END_DEFINE_SPEC(FDreamThemeJsonSpec)

void FDreamThemeJsonSpec::Define()
{
	BeforeEach([this]() { Saved.Capture(); });
	AfterEach([this]()
	{
		Saved.Restore();   // first, while a test theme the setting may name can still be found
		FDreamThemes::SetUserThemesDirForTest(FString());   // back to the real folder (read again)
		if (!TempDir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*TempDir, false, true);
			TempDir.Reset();
		}
	});

	It("exports, imports and exports every built-in to identical JSON (T-VIEW-015)", [this]()
	{
		for (const TCHAR* Id : DreamThemeSpec::BuiltInIds)
		{
			const TSharedPtr<const FDreamTheme> Theme = FDreamThemes::Find(Id);
			if (!TestTrue(*FString::Printf(TEXT("%s exists"), Id), Theme.IsValid()))
			{
				continue;
			}
			const FString First = FDreamThemes::ExportJson(*Theme);
			FDreamTheme Back;
			FString Error;
			if (!TestTrue(*FString::Printf(TEXT("%s: ImportJson (%s)"), Id, *Error), FDreamThemes::ImportJson(First, Back, &Error)))
			{
				continue;
			}
			TestEqual(*FString::Printf(TEXT("%s: id"), Id), Back.Id, Theme->Id);
			TestEqual(*FString::Printf(TEXT("%s: name"), Id), Back.Name.ToString(), Theme->Name.ToString());
			TestEqual(*FString::Printf(TEXT("%s: light"), Id), Back.bLight, Theme->bLight);
			TestEqual(*FString::Printf(TEXT("%s: export -> import -> export"), Id), FDreamThemes::ExportJson(Back), First);
			TestTrue(*FString::Printf(TEXT("%s: canonical text"), Id), First.StartsWith(TEXT("{\n  \"format\": \"dream-theme\",\n  \"version\": 1,")) && First.EndsWith(TEXT("}\n")));
		}
	});

	It("keys every EDreamThemeSlot with its JSON name, in slot order (review r6 nit)", [this]()
	{
		struct FKey
		{
			EDreamThemeSlot Slot;
			const TCHAR* Key;
		};
		const FKey Keys[] = {
			{ EDreamThemeSlot::ChromeBackground, TEXT("chrome.background") }, { EDreamThemeSlot::ChromeBorder, TEXT("chrome.border") },
			{ EDreamThemeSlot::ChromeText, TEXT("chrome.text") }, { EDreamThemeSlot::ChromeMutedText, TEXT("chrome.textMuted") },
			{ EDreamThemeSlot::ChromeHover, TEXT("chrome.hover") }, { EDreamThemeSlot::ChromeSelected, TEXT("chrome.selected") },
			{ EDreamThemeSlot::Accent, TEXT("chrome.accent") }, { EDreamThemeSlot::Danger, TEXT("chrome.danger") },
			{ EDreamThemeSlot::OutputBackground, TEXT("output.background") }, { EDreamThemeSlot::EditorBackground, TEXT("editor.background") },
			{ EDreamThemeSlot::GutterBackground, TEXT("gutter.background") }, { EDreamThemeSlot::GutterText, TEXT("gutter.text") },
			{ EDreamThemeSlot::CurrentLine, TEXT("editor.currentLine") }, { EDreamThemeSlot::Selection, TEXT("editor.selection") },
			{ EDreamThemeSlot::FindMatch, TEXT("editor.findMatch") }, { EDreamThemeSlot::FindCurrent, TEXT("editor.findCurrent") },
			{ EDreamThemeSlot::Caret, TEXT("editor.caret") }, { EDreamThemeSlot::OutEcho, TEXT("output.echo") },
			{ EDreamThemeSlot::OutStdout, TEXT("output.stdout") }, { EDreamThemeSlot::OutStderr, TEXT("output.stderr") },
			{ EDreamThemeSlot::OutResult, TEXT("output.result") }, { EDreamThemeSlot::OutWarning, TEXT("output.warning") },
			{ EDreamThemeSlot::OutError, TEXT("output.error") }, { EDreamThemeSlot::OutNotice, TEXT("output.notice") },
			{ EDreamThemeSlot::OutInfo, TEXT("output.info") }, { EDreamThemeSlot::OutLink, TEXT("output.link") },
			{ EDreamThemeSlot::OutHeader, TEXT("output.header") }, { EDreamThemeSlot::SynNormal, TEXT("syntax.normal") },
			{ EDreamThemeSlot::SynKeyword, TEXT("syntax.keyword") }, { EDreamThemeSlot::SynBuiltin, TEXT("syntax.builtin") },
			{ EDreamThemeSlot::SynSelf, TEXT("syntax.self") }, { EDreamThemeSlot::SynDefinition, TEXT("syntax.definition") },
			{ EDreamThemeSlot::SynDecorator, TEXT("syntax.decorator") }, { EDreamThemeSlot::SynNumber, TEXT("syntax.number") },
			{ EDreamThemeSlot::SynString, TEXT("syntax.string") }, { EDreamThemeSlot::SynDocstring, TEXT("syntax.docstring") },
			{ EDreamThemeSlot::SynEscape, TEXT("syntax.escape") }, { EDreamThemeSlot::SynOperator, TEXT("syntax.operator") },
			{ EDreamThemeSlot::SynComment, TEXT("syntax.comment") }, { EDreamThemeSlot::SynCell, TEXT("syntax.cell") },
			{ EDreamThemeSlot::SynError, TEXT("syntax.error") },
			{ EDreamThemeSlot::SynWarning, TEXT("syntax.warning") },   // (lang) PO-06 F4
		};
		static_assert(UE_ARRAY_COUNT(Keys) == static_cast<int32>(EDreamThemeSlot::Count), "one key per slot");
		for (const FKey& Key : Keys)
		{
			TestEqual(*FString::Printf(TEXT("slot %d"), static_cast<int32>(Key.Slot)),
				DreamText::Utf8ToFString(dream::theme::SlotKey(static_cast<int>(Key.Slot))), FString(Key.Key));
		}
	});

	It("keeps every 8-bit sRGB value through FLinearColor (the in-memory colour of an imported theme)", [this]()
	{
		int32 Lost = 0;
		for (int32 V = 0; V < 256; ++V)
		{
			const uint8 B = static_cast<uint8>(V);
			for (const FColor& In : { FColor(B, 0, 0, 255), FColor(0, B, 0, 255), FColor(0, 0, B, 255), FColor(7, 7, 7, B) })
			{
				if (FLinearColor::FromSRGBColor(In).ToFColorSRGB() != In)
				{
					++Lost;
				}
			}
		}
		TestEqual(TEXT("8-bit values that change through FromSRGBColor/ToFColorSRGB"), Lost, 0);
	});

	It("imports a partial theme: missing and invalid colours from its base, unknown keys ignored, hard failures refused", [this]()
	{
		const FString Json = TEXT("{\n  \"id\": \"DreamT4 Partial\",\n  \"base\": \"Nord\",\n  \"sparkle\": true,\n  \"colors\": {\n")
			TEXT("    \"chrome.background\": \"#101010\",\n    \"syntax.keyword\": \"#ff8800\",\n    \"syntax.sparkle\": \"#123456\",\n")
			TEXT("    \"output.error\": \"red\"\n  }\n}\n");
		FDreamTheme Theme;
		FString Error;
		if (!TestTrue(TEXT("ImportJson of a partial theme"), FDreamThemes::ImportJson(Json, Theme, &Error)))
		{
			AddError(Error);
			return;
		}
		const TSharedPtr<const FDreamTheme> Nord = FDreamThemes::Find(TEXT("Nord"));
		if (!TestTrue(TEXT("Nord exists"), Nord.IsValid()))
		{
			return;
		}
		TestEqual(TEXT("id"), Theme.Id, FString(TEXT("DreamT4 Partial")));
		TestEqual(TEXT("a missing name is the id"), Theme.Name.ToString(), FString(TEXT("DreamT4 Partial")));
		TestEqual(TEXT("base as written"), Theme.BaseId, FString(TEXT("Nord")));
		TestEqual(TEXT("chrome.background from the file"), DreamThemeSpec::Hex(DreamThemeSpec::Srgb(Theme.Get(EDreamThemeSlot::ChromeBackground))), FString(TEXT("#101010")));
		TestEqual(TEXT("syntax.keyword from the file"), DreamThemeSpec::Hex(DreamThemeSpec::Srgb(Theme.Get(EDreamThemeSlot::SynKeyword))), FString(TEXT("#FF8800")));
		int32 FromBase = 0;
		for (int32 Slot = 0; Slot < DreamThemeSpec::SlotCount; ++Slot)
		{
			const EDreamThemeSlot S = static_cast<EDreamThemeSlot>(Slot);
			if (S != EDreamThemeSlot::ChromeBackground && S != EDreamThemeSlot::SynKeyword
				&& DreamThemeSpec::Srgb(Theme.Colors[Slot]) == DreamThemeSpec::Srgb(Nord->Colors[Slot]))
			{
				++FromBase;
			}
		}
		TestEqual(TEXT("every other slot (output.error's invalid value included) comes from Nord"), FromBase, DreamThemeSpec::SlotCount - 2);

		// No base: a dark file falls back to Darcula, a light one to GitHub Light.
		FDreamTheme Light;
		TestTrue(TEXT("a light file without a base"), FDreamThemes::ImportJson(TEXT("{\"id\": \"L\", \"light\": true}"), Light));
		TestTrue(TEXT("its missing colours are GitHub Light's"),
			DreamThemeSpec::Srgb(Light.Get(EDreamThemeSlot::EditorBackground)) == DreamThemeSpec::Srgb(FDreamThemes::Find(TEXT("GitHub Light"))->Get(EDreamThemeSlot::EditorBackground)));
		FDreamTheme Dark;
		TestTrue(TEXT("a file whose base is not a built-in"), FDreamThemes::ImportJson(TEXT("{\"id\": \"D\", \"base\": \"Elsewhere\"}"), Dark));
		TestTrue(TEXT("its missing colours are Darcula's"),
			DreamThemeSpec::Srgb(Dark.Get(EDreamThemeSlot::SynKeyword)) == DreamThemeSpec::Srgb(FDreamThemes::Find(TEXT("Darcula"))->Get(EDreamThemeSlot::SynKeyword)));

		for (const TCHAR* Bad : { TEXT(""), TEXT("{ not json"), TEXT("[]"), TEXT("{\"format\": \"other\", \"id\": \"x\"}"), TEXT("{\"version\": 0, \"id\": \"x\"}"), TEXT("{\"colors\": {}}") })
		{
			FDreamTheme Refused;
			FString Why;
			TestFalse(*FString::Printf(TEXT("refuses %s"), Bad), FDreamThemes::ImportJson(Bad, Refused, &Why));
			TestFalse(*FString::Printf(TEXT("with a reason for %s"), Bad), Why.IsEmpty());
		}
	});

	LatentIt("reads user themes from the themes folder: format required, broken, oversized, nested and built-in-id files skipped, changes followed",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		AddExpectedMessagePlain(TEXT("Theme file "), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		AddExpectedMessagePlain(TEXT("is no longer in"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		TempDir = DreamThemeSpec::MakeTempDir(TEXT("User"));
		using DreamThemeSpec::WriteFile;
		const FString Format = TEXT("\"format\": \"dream-theme\", ");
		WriteFile(TempDir / TEXT("Mine.json"), TEXT("{\"format\": \"dream-theme\", \"version\": 1, \"id\": \"DreamT4 Mine\", \"name\": \"My theme\", \"base\": \"Nord\", ")
			TEXT("\"colors\": {\"chrome.background\": \"#101010\", \"syntax.keyword\": \"#FF8800\", \"syntax.sparkle\": \"#123456\"}}"));
		WriteFile(TempDir / TEXT("Light.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Light\", \"light\": true, \"colors\": {\"editor.background\": \"#FAFAFA\"}}"));
		WriteFile(TempDir / TEXT("noid.json"), TEXT("{") + Format + TEXT("\"name\": \"No id\"}"));
		WriteFile(TempDir / TEXT("ctrl.json"), TEXT("{") + Format + TEXT("\"id\": \"Bad\\u0007Id\"}"));                 // a control character in the id
		WriteFile(TempDir / TEXT("noformat.json"), TEXT("{\"id\": \"DreamT4 NoFormat\", \"colors\": {}}"));               // another program's JSON
		WriteFile(TempDir / TEXT("Darcula.json"), TEXT("{") + Format + TEXT("\"id\": \"Darcula\", \"colors\": {\"chrome.background\": \"#FF0000\"}}"));
		WriteFile(TempDir / TEXT("broken.json"), TEXT("{ not json"));
		WriteFile(TempDir / TEXT("zdup.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Mine\"}"));
		WriteFile(TempDir / TEXT("notes.txt"), TEXT("not a theme"));
		WriteFile(TempDir / TEXT("huge.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Huge\", \"name\": \"") + FString::ChrN((1 << 20) + 16, TEXT('a')) + TEXT("\"}"));
		IFileManager::Get().MakeDirectory(*(TempDir / TEXT("Built-in")), true);
		WriteFile(TempDir / TEXT("Built-in") / TEXT("Sub.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Sub\"}"));   // subfolders are not read

		FDreamThemes::SetUserThemesDirForTest(TempDir);
		TestEqual(TEXT("GetUserThemesDir follows the test folder"), FDreamThemes::GetUserThemesDir(), TempDir);
		auto UserIds = []()
		{
			TArray<FString> Ids;
			for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
			{
				if (!Theme->bBuiltIn)
				{
					Ids.Add(Theme->Id);
				}
			}
			Ids.Sort();
			return FString::Join(Ids, TEXT(", "));
		};
		TestEqual(TEXT("user themes (no format, broken, over 1 MB, nested, built-in id and duplicate skipped; noid and the control-character id named by their files)"),
			UserIds(), FString(TEXT("ctrl, DreamT4 Light, DreamT4 Mine, noid")));
		TestFalse(TEXT("the file over 1 MB is not read"), FDreamThemes::Find(TEXT("DreamT4 Huge")).IsValid());
		TestFalse(TEXT("a file in a subfolder is not read"), FDreamThemes::Find(TEXT("DreamT4 Sub")).IsValid());
		TestFalse(TEXT("a file without \"format\" is not read"), FDreamThemes::Find(TEXT("DreamT4 NoFormat")).IsValid());
		const TSharedPtr<const FDreamTheme> Darcula = FDreamThemes::Find(TEXT("Darcula"));
		TestTrue(TEXT("the built-in Darcula is untouched by a file with its id"),
			Darcula.IsValid() && Darcula->bBuiltIn && DreamThemeSpec::Hex(DreamThemeSpec::Srgb(Darcula->Get(EDreamThemeSlot::ChromeBackground))) == TEXT("#3C3F41"));
		const TSharedPtr<const FDreamTheme> Mine = FDreamThemes::Find(TEXT("DreamT4 Mine"));
		if (TestTrue(TEXT("DreamT4 Mine is found"), Mine.IsValid()))
		{
			TestEqual(TEXT("its name"), Mine->Name.ToString(), FString(TEXT("My theme")));
			TestTrue(TEXT("its file"), Mine->SourcePath.EndsWith(TEXT("Mine.json")));
			TestTrue(TEXT("its keyword from the file, its stdout from Nord"),
				DreamThemeSpec::Hex(DreamThemeSpec::Srgb(Mine->Get(EDreamThemeSlot::SynKeyword))) == TEXT("#FF8800")
				&& DreamThemeSpec::Srgb(Mine->Get(EDreamThemeSlot::OutStdout)) == DreamThemeSpec::Srgb(FDreamThemes::Find(TEXT("Nord"))->Get(EDreamThemeSlot::OutStdout)));
		}

		// A file added while the editor runs: List() lists the folder at most once a second and only notes the change; a
		// deferred tick reloads (never inside List()), so the new theme shows a moment later, without a Reload (r6 m2, r7 m1).
		WriteFile(TempDir / TEXT("Fresh.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Fresh\"}"));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Poll(TEXT("List() follows a new file after a deferred reload"), [UserIds]()
		{
			return UserIds() == TEXT("ctrl, DreamT4 Fresh, DreamT4 Light, DreamT4 Mine, noid") ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);

		// A user theme applies like a built-in; chosen and saved, then its file goes away: Reload shows Darcula and says so.
		Steps.Do(TEXT("choose and save DreamT4 Mine, then remove its file"), [this, Format]()
		{
			TestTrue(TEXT("SetCurrent(DreamT4 Mine), saved"), FDreamThemes::SetCurrent(TEXT("DreamT4 Mine"), /*bPersist=*/true));
			TestEqual(TEXT("current"), FDreamThemes::GetCurrentId(), FString(TEXT("DreamT4 Mine")));
			TestEqual(TEXT("Color() reads the user theme"), DreamThemeSpec::Hex(DreamThemeSpec::Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::ChromeBackground))), FString(TEXT("#101010")));
			IFileManager::Get().Delete(*(TempDir / TEXT("Mine.json")));
			IFileManager::Get().Delete(*(TempDir / TEXT("zdup.json")));   // its id would take Mine's place
			TestEqual(TEXT("ReloadUserThemes count"), FDreamThemes::ReloadUserThemes(), 4);
			TestEqual(TEXT("a removed current theme falls back to Darcula"), FDreamThemes::GetCurrentId(), FString(TEXT("Darcula")));
			TestEqual(TEXT("the setting still names it (used again if the file comes back)"), UDreamUserSettings::Get()->ThemeId, FString(TEXT("DreamT4 Mine")));
			// It comes back: the setting is resolved again by the reload that follows the folder change.
			DreamThemeSpec::WriteFile(TempDir / TEXT("Mine2.json"), TEXT("{") + Format + TEXT("\"id\": \"DreamT4 Mine\"}"));
			return true;
		});
		Steps.Poll(TEXT("the saved ThemeId is resolved again once its file is back"), []()
		{
			FDreamThemes::List();
			return FDreamThemes::GetCurrentId() == TEXT("DreamT4 Mine") ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Run(Done);
	});

	It("writes every built-in as a file (ExportBuiltIns, Dream.Theme.Export) that reads back identically", [this]()
	{
		TempDir = DreamThemeSpec::MakeTempDir(TEXT("Export"));
		const FString ApiDir = TempDir / TEXT("Api");
		TestEqual(TEXT("ExportBuiltIns count"), FDreamThemes::ExportBuiltIns(ApiDir), static_cast<int32>(UE_ARRAY_COUNT(DreamThemeSpec::BuiltInIds)));
		const FString ConsoleDir = TempDir / TEXT("Console");
		IConsoleManager::Get().ProcessUserConsoleInput(*FString::Printf(TEXT("Dream.Theme.Export %s"), *ConsoleDir), *GLog, nullptr);
		for (const TCHAR* Id : DreamThemeSpec::BuiltInIds)
		{
			for (const FString& Dir : { ApiDir, ConsoleDir })
			{
				FString Text;
				const FString Path = Dir / (FString(Id) + TEXT(".json"));
				if (!TestTrue(*FString::Printf(TEXT("%s was written"), *Path), FFileHelper::LoadFileToString(Text, *Path)))
				{
					continue;
				}
				TestEqual(*FString::Printf(TEXT("%s holds ExportJson"), *Path), Text, FDreamThemes::ExportJson(*FDreamThemes::Find(Id)));
				FDreamTheme Back;
				TestTrue(*FString::Printf(TEXT("%s imports"), *Path), FDreamThemes::ImportJson(Text, Back));
				TestEqual(*FString::Printf(TEXT("%s: file -> import -> export"), *Path), FDreamThemes::ExportJson(Back), Text);
			}
		}
		FString Error;
		TestFalse(TEXT("ExportToFile of an unknown id"), FDreamThemes::ExportToFile(TEXT("No Such Theme"), TempDir / TEXT("x.json"), &Error));
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeDropdownSpec, "Dream.Theme.Dropdown", DreamTest::AttendedFlags)
	DreamThemeSpec::FSaved Saved;
	FString TempDir;
	TSharedPtr<SWindow> Window;
END_DEFINE_SPEC(FDreamThemeDropdownSpec)

void FDreamThemeDropdownSpec::Define()
{
	BeforeEach([this]()
	{
		Saved.Capture();
		// One user theme, so the dropdown has to show user themes too.
		TempDir = DreamThemeSpec::MakeTempDir(TEXT("Dropdown"));
		DreamThemeSpec::WriteFile(TempDir / TEXT("Pick.json"), TEXT("{\"format\": \"dream-theme\", \"id\": \"DreamT4 Pick\", \"name\": \"DreamT4 Pick me\"}"));
		FDreamThemes::SetUserThemesDirForTest(TempDir);
	});
	AfterEach([this]()
	{
		if (Window.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
		}
		Window.Reset();
		Saved.Restore();
		FDreamThemes::SetUserThemesDirForTest(FString());
		IFileManager::Get().DeleteDirectory(*TempDir, false, true);
	});

	It("registers the customization for UDreamUserSettings and opens a menu of every theme, the user's included (review r6 M1)", [this]()
	{
		TestTrue(TEXT("the PropertyEditor class layout map holds the customization for UDreamUserSettings"), DreamThemeDetails::IsRegisteredForSettings());
		TArray<FString> Texts;
		DreamThemeSpec::CollectTexts(DreamThemeDetails::MakeThemeMenu(nullptr), Texts);
		int32 Missing = 0;
		for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
		{
			if (!Texts.Contains(Theme->Name.ToString()))
			{
				++Missing;
				AddError(FString::Printf(TEXT("the dropdown's menu has no entry for %s"), *Theme->Name.ToString()));
			}
		}
		TestEqual(TEXT("themes missing from the menu"), Missing, 0);
		TestTrue(TEXT("the menu lists the user theme"), Texts.Contains(TEXT("DreamT4 Pick me")));
	});

	LatentIt("shows the combo on the ThemeId row of a details view on the settings (review r6 M1)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("a details view on UDreamUserSettings, off screen"), [this]()
		{
			FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
			FDetailsViewArgs Args;
			Args.bAllowSearch = false;
			Args.bHideSelectionTip = true;
			Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
			const TSharedRef<IDetailsView> Details = PropertyEditor.CreateDetailView(Args);
			Details->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& Property)
			{
				return Property.Property.GetFName() == GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId);
			}));
			Details->SetObject(UDreamUserSettings::GetMutable());
			Window = SNew(SWindow)
				.ClientSize(FVector2D(640.0, 200.0))
				.ScreenPosition(FVector2D(-20000.0, -20000.0))
				.AutoCenter(EAutoCenter::None)
				.SizingRule(ESizingRule::FixedSize)
				.CreateTitleBar(false)
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				.ActivationPolicy(EWindowActivationPolicy::Never)
				.FocusWhenFirstShown(false)
				[
					Details
				];
			FSlateApplication::Get().AddWindow(Window.ToSharedRef(), /*bShowImmediately=*/true);
			return true;
		});
		Steps.WaitFrames(4);
		Steps.Poll(TEXT("the ThemeId row has the theme combo"), [this]()
		{
			const TSharedPtr<SWidget> Combo = Window.IsValid() ? DreamThemeSpec::FindTagged(Window.ToSharedRef(), DreamThemeDetails::GetComboTag()) : nullptr;
			if (!Combo.IsValid())
			{
				return DreamTest::EStep::Wait;
			}
			TArray<FString> Texts;
			DreamThemeSpec::CollectTexts(Combo.ToSharedRef(), Texts);
			const TSharedPtr<const FDreamTheme> Setting = FDreamThemes::Find(UDreamUserSettings::Get()->ThemeId);
			const FString Expected = Setting.IsValid() ? Setting->Name.ToString() : UDreamUserSettings::Get()->ThemeId;
			TestTrue(*FString::Printf(TEXT("the combo shows the setting's theme (%s; it shows %s)"), *Expected, *FString::Join(Texts, TEXT(", "))),
				Texts.Contains(Expected));
			return DreamTest::EStep::Next;
		}, 5.0);
		Steps.Finally(TEXT("close the window"), [this]()
		{
			if (Window.IsValid())
			{
				FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
				Window.Reset();
			}
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeUnrealSpec, "Dream.Theme.Unreal", DreamTest::AnyEditorFlags)
	DreamThemeSpec::FSaved Saved;
END_DEFINE_SPEC(FDreamThemeUnrealSpec)

void FDreamThemeUnrealSpec::Define()
{
	BeforeEach([this]() { Saved.Capture(); });
	AfterEach([this]() { Saved.Restore(); });

	It("takes its syntax colours from SyntaxHighlight.SourceCode.* exactly (T-VIEW-014)", [this]()
	{
		if (!TestTrue(TEXT("SetCurrent(Unreal)"), FDreamThemes::SetCurrent(TEXT("Unreal"), /*bPersist=*/false)))
		{
			return;
		}
		const FDreamTheme& T = FDreamThemes::Current();
		struct FRow
		{
			const TCHAR* Key;
			EDreamThemeSlot Slot;
			dream::TokenKind Kind;
		};
		const FRow Rows[] = {
			{ TEXT("SyntaxHighlight.SourceCode.Normal"), EDreamThemeSlot::SynNormal, dream::TokenKind::Text },
			{ TEXT("SyntaxHighlight.SourceCode.Operator"), EDreamThemeSlot::SynOperator, dream::TokenKind::Operator },
			{ TEXT("SyntaxHighlight.SourceCode.Keyword"), EDreamThemeSlot::SynKeyword, dream::TokenKind::Keyword },
			{ TEXT("SyntaxHighlight.SourceCode.String"), EDreamThemeSlot::SynString, dream::TokenKind::String },
			{ TEXT("SyntaxHighlight.SourceCode.Number"), EDreamThemeSlot::SynNumber, dream::TokenKind::Number },
			{ TEXT("SyntaxHighlight.SourceCode.Comment"), EDreamThemeSlot::SynComment, dream::TokenKind::Comment },
			{ TEXT("SyntaxHighlight.SourceCode.PreProcessorKeyword"), EDreamThemeSlot::SynDecorator, dream::TokenKind::Decorator },
			{ TEXT("SyntaxHighlight.SourceCode.Error"), EDreamThemeSlot::SynError, dream::TokenKind::Error },
		};
		const ISlateStyle& App = FAppStyle::Get();
		int32 Checked = 0;
		for (const FRow& Row : Rows)
		{
			if (!App.HasWidgetStyle<FTextBlockStyle>(Row.Key))
			{
				AddInfo(FString::Printf(TEXT("the app style has no %s (no editor style in this session)"), Row.Key));
				continue;
			}
			const FLinearColor Stock = App.GetWidgetStyle<FTextBlockStyle>(Row.Key).ColorAndOpacity.GetSpecifiedColor();
			TestTrue(*FString::Printf(TEXT("%s equals the theme slot"), Row.Key), T.Get(Row.Slot) == Stock);
			TestTrue(*FString::Printf(TEXT("%s equals GetTokenColor"), Row.Key), FDreamEditorStyle::GetTokenColor(Row.Kind).GetSpecifiedColor() == Stock);
			++Checked;
		}
		AddInfo(FString::Printf(TEXT("%d stock source-code colours compared"), Checked));
		TestTrue(TEXT("the caret is the Normal colour, as the stock text widget painted it in slice B"), T.Get(EDreamThemeSlot::Caret) == T.Get(EDreamThemeSlot::SynNormal));
	});

	It("uses the slice-B FStyleColors for chrome, panes and output", [this]()
	{
		if (!TestTrue(TEXT("SetCurrent(Unreal)"), FDreamThemes::SetCurrent(TEXT("Unreal"), /*bPersist=*/false)))
		{
			return;
		}
		const FDreamTheme& T = FDreamThemes::Current();
		struct FRow
		{
			const TCHAR* What;
			EDreamThemeSlot Slot;
			FLinearColor Expected;
		};
		const FRow Rows[] = {
			{ TEXT("chrome background = Panel"), EDreamThemeSlot::ChromeBackground, FStyleColors::Panel.GetSpecifiedColor() },
			{ TEXT("chrome text = Foreground"), EDreamThemeSlot::ChromeText, FStyleColors::Foreground.GetSpecifiedColor() },
			{ TEXT("accent = Primary"), EDreamThemeSlot::Accent, FStyleColors::Primary.GetSpecifiedColor() },
			{ TEXT("danger = Error"), EDreamThemeSlot::Danger, FStyleColors::Error.GetSpecifiedColor() },
			{ TEXT("editor background = Recessed"), EDreamThemeSlot::EditorBackground, FStyleColors::Recessed.GetSpecifiedColor() },
			{ TEXT("output background = Recessed"), EDreamThemeSlot::OutputBackground, FStyleColors::Recessed.GetSpecifiedColor() },
			{ TEXT("gutter text = AccentGray"), EDreamThemeSlot::GutterText, FStyleColors::AccentGray.GetSpecifiedColor() },
			{ TEXT("stdout = Foreground"), EDreamThemeSlot::OutStdout, FStyleColors::Foreground.GetSpecifiedColor() },
			{ TEXT("stderr = AccentOrange"), EDreamThemeSlot::OutStderr, FStyleColors::AccentOrange.GetSpecifiedColor() },
			{ TEXT("result = AccentBlue"), EDreamThemeSlot::OutResult, FStyleColors::AccentBlue.GetSpecifiedColor() },
			{ TEXT("warning = Warning"), EDreamThemeSlot::OutWarning, FStyleColors::Warning.GetSpecifiedColor() },
			{ TEXT("error = Error"), EDreamThemeSlot::OutError, FStyleColors::Error.GetSpecifiedColor() },
			{ TEXT("notice = AccentPurple"), EDreamThemeSlot::OutNotice, FStyleColors::AccentPurple.GetSpecifiedColor() },
			{ TEXT("info = AccentGray"), EDreamThemeSlot::OutInfo, FStyleColors::AccentGray.GetSpecifiedColor() },
			{ TEXT("header = ForegroundHeader"), EDreamThemeSlot::OutHeader, FStyleColors::ForegroundHeader.GetSpecifiedColor() },
			{ TEXT("link = AccentBlue"), EDreamThemeSlot::OutLink, FStyleColors::AccentBlue.GetSpecifiedColor() },
			{ TEXT("echo = #8C8C8C (PO-02)"), EDreamThemeSlot::OutEcho, FLinearColor::FromSRGBColor(FColor(140, 140, 140)) },
		};
		for (const FRow& Row : Rows)
		{
			TestTrue(Row.What, T.Get(Row.Slot) == Row.Expected);
		}
		TestTrue(TEXT("the muted text is the subdued foreground"),
			T.Get(EDreamThemeSlot::ChromeMutedText) == FWidgetStyle().SetForegroundColor(FStyleColors::Foreground.GetSpecifiedColor()).GetSubduedForegroundColor());
	});
}

// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamThemeLiveApplySpec, "Dream.Theme.LiveApply", DreamTest::AttendedFlags)
	DreamThemeSpec::FSaved Saved;
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	FDelegateHandle ChangedHandle;
	TSharedPtr<int32> ChangedCount;
	bool bLineNumbersWereOff = false;

	/** Paints the code view and checks its pixels against the current theme; OtherKeyword is the previous theme's. */
	void CheckPainted(const FString& Context, const FLinearColor& OtherKeyword);
	/** Selects the indent of line 2 and focuses the code view (the selection paints its colour only with focus). */
	void SelectIndent();
	/** Paints the history (output view) and checks that its empty right edge shows the theme's OutputBackground. */
	void CheckOutputPane(const FString& Context);
END_DEFINE_SPEC(FDreamThemeLiveApplySpec)

void FDreamThemeLiveApplySpec::CheckPainted(const FString& Context, const FLinearColor& OtherKeyword)
{
	using namespace DreamThemeSpec;
	const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(BufferId) : nullptr;
	if (!View.IsValid() || !FSlateApplication::IsInitialized())
	{
		AddError(FString::Printf(TEXT("%s: no code view"), *Context));
		return;
	}
	FSlateApplication& App = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = App.FindWidgetWindow(View.ToSharedRef());
	if (!Window.IsValid() || Window->IsWindowMinimized() || !Window->IsVisible())
	{
		AddError(FString::Printf(TEXT("%s: the code view's window is not shown"), *Context));
		return;
	}
	TArray<FColor> Pixels;
	FIntVector Size(0, 0, 0);
	if (!App.TakeScreenshot(View.ToSharedRef(), Pixels, Size) || Size.X < 80 || Size.Y < 60 || Pixels.Num() < Size.X * Size.Y)
	{
		AddError(FString::Printf(TEXT("%s: TakeScreenshot gave %d x %d"), *Context, Size.X, Size.Y));
		return;
	}
	const FVector2D Local = View->GetTickSpaceGeometry().GetLocalSize();
	const double Scale = Local.X > 0.0 ? static_cast<double>(Size.X) / Local.X : 1.0;
	auto At = [&Pixels, &Size](double X, double Y)
	{
		const int32 IX = FMath::Clamp(FMath::FloorToInt32(X), 0, Size.X - 1);
		const int32 IY = FMath::Clamp(FMath::FloorToInt32(Y), 0, Size.Y - 1);
		return Pixels[IY * Size.X + IX];
	};
	const FDreamTheme& T = FDreamThemes::Current();
	const FColor Bg = Srgb(T.Get(EDreamThemeSlot::EditorBackground));
	const FColor Gutter = Srgb(T.Get(EDreamThemeSlot::GutterBackground));
	constexpr int32 Tolerance = 3;

	// The background below the text, away from the scroll bars.
	const double BottomY = Size.Y - 18.0 * Scale;
	const FColor PxBg = At(Size.X - 30.0 * Scale, BottomY);
	TestTrue(*FString::Printf(TEXT("%s: editor background %s is %s"), *Context, *Hex(PxBg), *Hex(Bg)), Near(PxBg, Bg, Tolerance));
	// The gutter band at the bottom left, and where it ends (the text column's left edge, in pixels).
	const FColor PxGutter = At(1.0, BottomY);
	TestTrue(*FString::Printf(TEXT("%s: gutter band %s is %s"), *Context, *Hex(PxGutter), *Hex(Gutter)), Near(PxGutter, Gutter, Tolerance));
	int32 TextLeft = INDEX_NONE;
	for (int32 X = 0; X < Size.X / 2; ++X)
	{
		if (Near(At(X, BottomY), Bg, Tolerance) && !Near(At(X, BottomY), Gutter, 0))
		{
			TextLeft = X;
			break;
		}
	}
	if (TextLeft == INDEX_NONE)
	{
		AddError(FString::Printf(TEXT("%s: the gutter's right edge was not found"), *Context));
		return;
	}

	TArray<FDreamLineGeometry> Lines;
	View->GetVisibleLines(Lines);
	const FDreamLineGeometry* Line0 = Lines.FindByPredicate([](const FDreamLineGeometry& L) { return L.Line == 0; });
	const FDreamLineGeometry* Line1 = Lines.FindByPredicate([](const FDreamLineGeometry& L) { return L.Line == 1; });
	if (Line0 == nullptr || Line1 == nullptr)
	{
		AddError(FString::Printf(TEXT("%s: lines 1 and 2 are not laid out"), *Context));
		return;
	}
	// The caret line (line 2, after the selection) carries the current-line band; far right of its text.
	const FColor PxCurrent = At(Size.X - 30.0 * Scale, (Line1->Top + Line1->Height * 0.5) * Scale);
	const FColor Current = Srgb(T.Get(EDreamThemeSlot::CurrentLine));
	TestTrue(*FString::Printf(TEXT("%s: current line %s is %s"), *Context, *Hex(PxCurrent), *Hex(Current)), Near(PxCurrent, Current, Tolerance));

	// The selected indent of line 2 (no glyphs there): the selection colour, painted only while the text has focus.
	TArray<FSlateRect> Rects;
	View->GetRangeRects(10, 14, Rects);
	if (Rects.Num() == 1 && View->HasAnyUserFocusOrFocusedDescendants())
	{
		const FSlateRect& R = Rects[0];
		const FColor PxSel = At(TextLeft + (R.Left + R.Right) * 0.5 * Scale, (R.Top + R.Bottom) * 0.5 * Scale);
		const FColor Selection = Srgb(T.Get(EDreamThemeSlot::Selection));
		TestTrue(*FString::Printf(TEXT("%s: selection %s is %s"), *Context, *Hex(PxSel), *Hex(Selection)), Near(PxSel, Selection, Tolerance));
	}
	else
	{
		AddWarning(FString::Printf(TEXT("%s: selection pixels not checked (%d selection rects, keyboard focus %s)"), *Context, Rects.Num(),
			View->HasAnyUserFocusOrFocusedDescendants() ? TEXT("in the view") : TEXT("elsewhere")));
	}

	// "def" on line 1: its most inked pixel is nearer this theme's keyword colour than the previous theme's.
	Rects.Reset();
	View->GetRangeRects(0, 3, Rects);
	if (Rects.Num() != 1)
	{
		AddError(FString::Printf(TEXT("%s: no rect for the keyword"), *Context));
		return;
	}
	const FSlateRect& K = Rects[0];
	FColor Inked = Bg;
	for (double Y = K.Top * Scale; Y < K.Bottom * Scale; Y += 1.0)
	{
		for (double X = TextLeft + K.Left * Scale; X < TextLeft + K.Right * Scale; X += 1.0)
		{
			const FColor P = At(X, Y);
			if (Distance(P, Bg) > Distance(Inked, Bg))
			{
				Inked = P;
			}
		}
	}
	const FColor Keyword = Srgb(T.Get(EDreamThemeSlot::SynKeyword));
	const FColor Other = Srgb(OtherKeyword);
	const double ToKeyword = BlendDistance(Inked, Bg, Keyword);
	const double ToOther = BlendDistance(Inked, Bg, Other);
	TestTrue(*FString::Printf(TEXT("%s: the keyword's glyphs (%s) blend this theme's keyword %s over the background (%.1f), not the previous %s (%.1f)"),
		*Context, *Hex(Inked), *Hex(Keyword), ToKeyword, *Hex(Other), ToOther), ToKeyword < ToOther && Distance(Inked, Bg) > 30);
}

void FDreamThemeLiveApplySpec::CheckOutputPane(const FString& Context)
{
	using namespace DreamThemeSpec;
	const TSharedPtr<SDreamOutputArea> Area = Tab.IsValid() ? Tab->GetOutputArea() : nullptr;
	const TSharedPtr<SDreamOutputView> View = Area.IsValid() ? Area->GetOutputView() : nullptr;
	if (!View.IsValid() || !FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef()).IsValid())
	{
		AddWarning(FString::Printf(TEXT("%s: no history view shown; its pixels not checked"), *Context));
		return;
	}
	TArray<FColor> Pixels;
	FIntVector Size(0, 0, 0);
	if (!FSlateApplication::Get().TakeScreenshot(View.ToSharedRef(), Pixels, Size) || Size.X < 80 || Size.Y < 40 || Pixels.Num() < Size.X * Size.Y)
	{
		AddError(FString::Printf(TEXT("%s: TakeScreenshot of the history gave %d x %d"), *Context, Size.X, Size.Y));
		return;
	}
	const FVector2D Local = View->GetTickSpaceGeometry().GetLocalSize();
	const double Scale = Local.X > 0.0 ? static_cast<double>(Size.X) / Local.X : 1.0;
	const FColor Expected = Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::OutputBackground));
	// Sixteen points down the right edge, left of the scroll bar: text rarely reaches there, so most are the background.
	int32 Matching = 0;
	FColor Sample = Expected;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const int32 X = FMath::Clamp(FMath::FloorToInt32(Size.X - 20.0 * Scale), 0, Size.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32((Index + 0.5) * Size.Y / 16.0), 0, Size.Y - 1);
		const FColor P = Pixels[Y * Size.X + X];
		Matching += Near(P, Expected, 3) ? 1 : 0;
		if (!Near(P, Expected, 3))
		{
			Sample = P;
		}
	}
	TestTrue(*FString::Printf(TEXT("%s: the history's right edge shows OutputBackground %s (%d of 16 points; another was %s)"), *Context,
		*Hex(Expected), Matching, *Hex(Sample)), Matching >= 12);
}

void FDreamThemeLiveApplySpec::SelectIndent()
{
	const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(BufferId) : nullptr;
	if (!View.IsValid())
	{
		return;
	}
	FDreamTextSelection Selection;
	Selection.AnchorLine = 1;
	Selection.AnchorColumn = 0;
	Selection.CaretLine = 1;
	Selection.CaretColumn = 4;
	if (const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(View.ToSharedRef()))
	{
		Window->BringToFront();
	}
	View->FocusEditor();
	View->SetSelection(Selection);
}

void FDreamThemeLiveApplySpec::Define()
{
	BeforeEach([this]()
	{
		Saved.Capture();
		ChangedCount = MakeShared<int32>(0);
		TSharedPtr<int32> Counter = ChangedCount;
		ChangedHandle = FDreamThemes::OnChanged().AddLambda([Counter]() { ++*Counter; });
		// The gutter checks need line numbers (View > Line numbers, on by default): on for the spec, restored after.
		bLineNumbersWereOff = !UDreamUserSettings::Get()->GetViewSetting(EDreamViewSetting::LineNumbers);
		if (bLineNumbersWereOff)
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::LineNumbers, true);
		}
		Tab = DreamTest::RequireTab(*this);
		if (Tab.IsValid())
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("def f(x):\n    return x\n"), TEXT("Theme"));
			BufferId = Buffer.IsValid() ? Buffer->GetId() : FGuid();
		}
	});

	AfterEach([this]()
	{
		FDreamThemes::OnChanged().Remove(ChangedHandle);
		Saved.Restore();
		if (bLineNumbersWereOff)
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::LineNumbers, false);
			bLineNumbersWereOff = false;
		}
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	LatentIt("repaints the open code view at once in the new theme's colours (T-VIEW-015 live apply)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<FLinearColor> PreviousKeyword = MakeShared<FLinearColor>(FLinearColor::Black);

		Steps.Do(TEXT("select the indent of line 2"), [this, PreviousKeyword]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(BufferId) : nullptr;
			if (!View.IsValid())
			{
				AddError(TEXT("no Dream tab with a code view"));
				return false;
			}
			SelectIndent();
			*PreviousKeyword = FDreamThemes::LinearColor(EDreamThemeSlot::SynKeyword);
			return true;
		});
		Steps.WaitFrames(2);

		for (const TCHAR* Id : { TEXT("Solarized Light"), TEXT("Dracula") })
		{
			const FString ThemeId = Id;
			Steps.Do(FString::Printf(TEXT("switch to %s"), Id), [this, ThemeId, PreviousKeyword]()
			{
				*ChangedCount = 0;
				if (FDreamThemes::GetCurrentId() == ThemeId)
				{
					FDreamThemes::SetCurrent(TEXT("Monokai"), /*bPersist=*/false);   // start from something else
					*PreviousKeyword = FDreamThemes::LinearColor(EDreamThemeSlot::SynKeyword);
					*ChangedCount = 0;
				}
				if (!TestTrue(*FString::Printf(TEXT("SetCurrent(%s)"), *ThemeId), FDreamThemes::SetCurrent(ThemeId, /*bPersist=*/true)))
				{
					return false;
				}
				TestEqual(TEXT("OnChanged fired once"), *ChangedCount, 1);
				TestEqual(TEXT("SetCurrent persisted UDreamUserSettings::ThemeId"), UDreamUserSettings::Get()->ThemeId, ThemeId);
				SelectIndent();
				return true;
			});
			Steps.WaitFrames(3);
			Steps.Do(FString::Printf(TEXT("%s is painted"), Id), [this, ThemeId, PreviousKeyword]()
			{
				CheckPainted(ThemeId, *PreviousKeyword);
				CheckOutputPane(ThemeId);
				*PreviousKeyword = FDreamThemes::LinearColor(EDreamThemeSlot::SynKeyword);
				return true;
			});
		}

		Steps.Do(TEXT("Editor Preferences: ThemeId = Nord"), [this]()
		{
			*ChangedCount = 0;
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Settings->ThemeId = TEXT("Nord");
			FProperty* Property = FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, ThemeId));
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Settings->PostEditChangeProperty(Event);
			TestEqual(TEXT("the edited setting applies at once"), FDreamThemes::GetCurrentId(), FString(TEXT("Nord")));
			TestEqual(TEXT("OnChanged fired once"), *ChangedCount, 1);
			SelectIndent();
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Nord is painted"), [this, PreviousKeyword]()
		{
			CheckPainted(TEXT("Nord (Editor Preferences)"), *PreviousKeyword);
			return true;
		});
		Steps.Do(TEXT("selecting the current theme again changes nothing"), [this]()
		{
			*ChangedCount = 0;
			TestTrue(TEXT("SetCurrent(Nord) again"), FDreamThemes::SetCurrent(TEXT("Nord"), /*bPersist=*/false));
			TestEqual(TEXT("no OnChanged"), *ChangedCount, 0);
			TestFalse(TEXT("an unknown id changes nothing"), FDreamThemes::SetCurrent(TEXT("No Such Theme"), /*bPersist=*/false));
			TestEqual(TEXT("still Nord"), FDreamThemes::GetCurrentId(), FString(TEXT("Nord")));
			return true;
		});
		Steps.Finally(TEXT("restore the theme"), [this]() { Saved.Restore(); });
		Steps.Run(Done);
	});

	LatentIt("paints the Run window of a pumping run in the theme's chrome and history colours (review tests-r6 m11)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamThemeSpec;
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Loop = DreamTest::OpenScratch(*this, *Tab, TEXT("import dream_editor\nfor _ in range(250): dream_editor.sleep(0.01)"), TEXT("ThemeRun"));
		if (!Loop.IsValid())
		{
			Done.Execute();
			return;
		}
		struct FState
		{
			bool bSavedRunPump = true;
			EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
			bool bChanged = false;
			FDelegateHandle PostTick;
			bool bChecked = false;
			FString Error;
			int32 ChromeMatches = 0;
			FColor ChromeSample;
			int32 LiveMatches = 0;
			FColor LiveOther;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		State->bSavedRunPump = UDreamUserSettings::Get()->bRunPump;
		State->SavedPresentation = UDreamUserSettings::Get()->RunPresentation;
		const FGuid LoopId = Loop->GetId();
		const TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("Solarized Light, the Run window presentation, and a pumping run"), [this, State, Run, Loop]()
		{
			FDreamThemes::SetCurrent(TEXT("Solarized Light"), /*bPersist=*/false);
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();   // in memory only, restored by Finally
			State->bChanged = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			State->PostTick = FSlateApplication::Get().OnPostTick().AddLambda([State](float)
			{
				FRunSupervisor& Supervisor = FDreamEditorModule::Get().GetSupervisor();
				if (State->bChecked || Supervisor.GetPumpDepth() <= 0 || !Supervisor.IsRunWindowOpen())
				{
					return;
				}
				const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
				if (!Modal.IsValid())
				{
					return;
				}
				State->bChecked = true;
				TArray<FColor> Pixels;
				FIntVector Size(0, 0, 0);
				if (!FSlateApplication::Get().TakeScreenshot(Modal.ToSharedRef(), Pixels, Size) || Size.X < 40 || Size.Y < 40)
				{
					State->Error = TEXT("no screenshot of the Run window");
					return;
				}
				// The padding above the header row (inside the window's own 1-px outline): the theme's chrome background.
				const FColor Chrome = Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::ChromeBackground));
				for (int32 Index = 1; Index <= 8; ++Index)
				{
					const int32 X = FMath::Clamp(Size.X * Index / 9, 0, Size.X - 1);
					const FColor P = Pixels[FMath::Min(4, Size.Y - 1) * Size.X + X];
					if (Near(P, Chrome, 3))
					{
						++State->ChromeMatches;
					}
					else
					{
						State->ChromeSample = P;
					}
				}
				// The live run view: the theme's history background down its right edge.
				if (const TSharedPtr<SDreamOutputView> Live = Supervisor.GetLiveRunViewForTest())
				{
					TArray<FColor> LivePixels;
					FIntVector LiveSize(0, 0, 0);
					if (FSlateApplication::Get().TakeScreenshot(Live.ToSharedRef(), LivePixels, LiveSize) && LiveSize.X > 40 && LiveSize.Y > 16)
					{
						const FVector2D Local = Live->GetTickSpaceGeometry().GetLocalSize();
						const double Scale = Local.X > 0.0 ? static_cast<double>(LiveSize.X) / Local.X : 1.0;
						State->LiveMatches = RightEdgeMatches(LivePixels, LiveSize, Scale, Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::OutputBackground)), State->LiveOther);
					}
				}
			});
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Loop, EDreamRunRoute::Buffer, 1, 2));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumping run"), Run, 60.0);
		Steps.Do(TEXT("the Run window was painted in the theme"), [this, State, Run]()
		{
			using namespace DreamThemeSpec;
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			if (!TestTrue(TEXT("the Run window was up and checked during the run"), State->bChecked) || !State->Error.IsEmpty())
			{
				if (!State->Error.IsEmpty())
				{
					AddError(State->Error);
				}
				return true;
			}
			TestTrue(*FString::Printf(TEXT("the Run window's header padding is ChromeBackground %s (%d of 8 points; another was %s)"),
				*Hex(Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::ChromeBackground))), State->ChromeMatches, *Hex(State->ChromeSample)), State->ChromeMatches >= 6);
			TestTrue(*FString::Printf(TEXT("the live run view is OutputBackground %s (%d of 16 points; another was %s)"),
				*Hex(Srgb(FDreamThemes::LinearColor(EDreamThemeSlot::OutputBackground))), State->LiveMatches, *Hex(State->LiveOther)), State->LiveMatches >= 12);
			TestTrue(TEXT("the Stop button is the theme's Danger"),
				FDreamEditorStyle::Get().GetWidgetStyle<FButtonStyle>(DreamStyleKeys::RunWindowStopButton).Normal.TintColor.GetSpecifiedColor()
				== FDreamThemes::LinearColor(EDreamThemeSlot::Danger));
			return true;
		});
		Steps.Finally(TEXT("restore the run settings, the theme and the loop buffer"), [this, State, LoopId]()
		{
			if (State->PostTick.IsValid())
			{
				FSlateApplication::Get().OnPostTick().Remove(State->PostTick);
			}
			if (State->bChanged)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = State->bSavedRunPump;
				Settings->RunPresentation = State->SavedPresentation;
			}
			Saved.Restore();
			DreamTest::CloseBuffer(LoopId);
		});
		Steps.Run(Done);
	});
}
