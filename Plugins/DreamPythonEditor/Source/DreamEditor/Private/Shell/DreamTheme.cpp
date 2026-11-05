// SPDX-License-Identifier: Apache-2.0
// Dream themes (PO-04): the theme list, the current theme and its live application, the theme JSON (dream/ThemeFormat.h) and
// the user themes folder. The palettes are in DreamThemeTable.cpp.
//
// (theme R-notes, also in the report)
// - Live switching works without widget rebuilds where colours are read at paint: Color()/ColorAttr() read the current
//   theme each time, and Brush() returns one FSlateColorBrush per slot that is retinted in place, so a widget holding the
//   pointer paints the new colour on the next frame. FSlateColor cannot link to a shared colour in UE 5.8 (its
//   TSharedRef<FLinearColor> constructor copies), which is why nothing may store a colour value.
// - User themes live in <user settings dir>/Dream/Themes (%LOCALAPPDATA%\DreamPythonEditor\Themes), beside the per-engine storage roots
//   and shared by every engine version and project, since a theme is a personal preference. Only *.json directly in that
//   folder is read; Dream.Theme.Export (and View > Theme > Export Built-in Themes) writes the built-ins to its Built-in
//   subfolder, which the loader does not read. List() and Find() look at the folder at most once a second with one
//   directory enumeration (names, sizes, times) and only note a change; the reload itself (reads, apply, OnChanged) runs
//   on a deferred idle tick, never inside the call or a layout pass (review r6 m2, r7 m1). Per-frame readers (the Editor
//   Preferences combo) use FindLoaded, which touches no file.
// - A folder file must say "format": "dream-theme" (review r6 nit: another program's theme JSON is not loaded as a
//   mostly-default Dream theme); ImportJson stays lenient. A file is read with a bounded read (1 MB); an id with a control
//   character is replaced by the file name. A user theme's "base" may name a built-in only (no chains between user files,
//   so the load order never matters); a file whose id equals a built-in's is skipped with a warning (built-ins cannot be
//   replaced, only copied and renamed).
// - Start-up: a built-in ThemeId is selected in StartupModule (no I/O); a user ThemeId is resolved one deferred idle tick
//   later (B1), showing Darcula until then. An unknown ThemeId keeps Darcula and the setting as written (a theme file that
//   comes back is used again at the next start, or at the next reload, which resolves the setting again).
// - The setting is followed only when ThemeId itself changes (LastSeenSettingId, review r6 m1): other View toggles share
//   OnViewSettingsChanged and never move the theme. Unreal follows the editor's own theme (USlateThemeManager) while current.
// - The console commands and the export entry refuse while a run is active (RT-01).
#include "Shell/DreamTheme.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamLog.h"
#include "Module/DreamText.h"
#include "Run/DreamDeferral.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamThemeDetails.h"
#include "Shell/DreamThemeTable.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "dream/ThemeFormat.h"

static_assert(static_cast<int32>(EDreamThemeSlot::Count) == dream::theme::SlotCount,
	"EDreamThemeSlot and dream::theme's slot table must list the same slots in the same order");

namespace DreamThemeLocal
{
	constexpr int32 SlotCount = static_cast<int32>(EDreamThemeSlot::Count);
	/** A theme file larger than this is not read (dream::theme::Parse refuses a megabyte too). */
	constexpr int64 MaxThemeFileBytes = 1 << 20;
	/** List() and Find() list the themes folder at most this often. */
	constexpr double FolderCheckIntervalSeconds = 1.0;

	struct FState
	{
		TArray<TSharedRef<FDreamTheme>> BuiltIns;
		TArray<TSharedRef<FDreamTheme>> UserThemes;
		bool bUserThemesLoaded = false;
		TSharedPtr<FDreamTheme> Current;
		/** A user ThemeId seen at start-up, resolved at the first deferred idle tick. */
		FString PendingId;
		FTSTicker::FDelegateHandle PendingTicker;
		/** One brush per slot, allocated once and never freed (widgets keep the pointers). */
		TArray<TUniquePtr<FSlateColorBrush>> Brushes;
		TUniquePtr<FButtonStyle> FlatButton;
		TUniquePtr<FEditableTextBoxStyle> TextBox;
		TUniquePtr<FScrollBarStyle> ScrollBar;
		TUniquePtr<FTableRowStyle> TableRow;
		TUniquePtr<FTableViewStyle> ListView;
		TUniquePtr<FProgressBarStyle> ProgressBar;
		FSimpleMulticastDelegate OnChanged;
		FDelegateHandle SettingsHandle;
		FDelegateHandle EditorThemeHandle;
		FString UserDirOverride;
		/** UDreamUserSettings::ThemeId as last seen: only a change of it moves the theme. */
		FString LastSeenSettingId;
		/** The themes folder's *.json names, sizes and times at the last load. */
		FString FolderSignature;
		/** When List()/Find() last listed the folder (FPlatformTime seconds), and the reload a change scheduled. */
		double LastFolderCheckSeconds = 0.0;
		FTSTicker::FDelegateHandle ReloadTicker;
	};

	/** RT-01: nothing that broadcasts OnChanged or writes files runs from a console command while a run is active. */
	bool RefuseDuringRun(const TCHAR* What)
	{
		if (FDreamDeferral::CanRunNow(TEXT("DreamTheme"), EDreamDeferPolicy::RefuseWhileExecuting, /*bNeedsPython=*/false))
		{
			return false;
		}
		UE_LOG(LogDream, Display, TEXT("%s: refused while a run is active (try again when it has ended)."), What);
		return true;
	}

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	void RetintBrushes()
	{
		FState& S = State();
		const FDreamTheme& Theme = *S.Current;
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			S.Brushes[Slot]->TintColor = FSlateColor(Theme.Colors[Slot]);
		}
		S.FlatButton->Hovered.TintColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeHover));
		S.FlatButton->Pressed.TintColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeSelected));

		FEditableTextBoxStyle& Box = *S.TextBox;
		auto Fill = [&Theme](FSlateBrush& Brush, EDreamThemeSlot FillSlot, EDreamThemeSlot OutlineSlot)
		{
			Brush.TintColor = FSlateColor(Theme.Get(FillSlot));
			Brush.OutlineSettings.Color = FSlateColor(Theme.Get(OutlineSlot));
		};
		Fill(Box.BackgroundImageNormal, EDreamThemeSlot::EditorBackground, EDreamThemeSlot::ChromeBorder);
		Fill(Box.BackgroundImageHovered, EDreamThemeSlot::EditorBackground, EDreamThemeSlot::ChromeHover);
		Fill(Box.BackgroundImageFocused, EDreamThemeSlot::EditorBackground, EDreamThemeSlot::Accent);
		Fill(Box.BackgroundImageReadOnly, EDreamThemeSlot::ChromeBackground, EDreamThemeSlot::ChromeBorder);
		Box.ForegroundColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText));
		Box.FocusedForegroundColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText));
		Box.ReadOnlyForegroundColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText));   // review r6 m4: readable read-only text

		const FSlateColor Thumb(Theme.Get(EDreamThemeSlot::ChromeHover));
		const FSlateColor ThumbActive(Theme.Get(EDreamThemeSlot::ChromeMutedText));
		for (FScrollBarStyle* Bar : { S.ScrollBar.Get(), &Box.ScrollBarStyle })
		{
			Bar->NormalThumbImage.TintColor = Thumb;
			Bar->HoveredThumbImage.TintColor = ThumbActive;
			Bar->DraggedThumbImage.TintColor = ThumbActive;
		}

		FTableRowStyle& Row = *S.TableRow;
		const FSlateColor Selection(Theme.Get(EDreamThemeSlot::Selection));
		const FSlateColor Hover(Theme.Get(EDreamThemeSlot::ChromeHover));
		for (FSlateBrush* Brush : { &Row.ActiveBrush, &Row.ActiveHoveredBrush, &Row.InactiveBrush, &Row.InactiveHoveredBrush,
			&Row.ActiveHighlightedBrush, &Row.InactiveHighlightedBrush })
		{
			Brush->TintColor = Selection;
		}
		Row.EvenRowBackgroundHoveredBrush.TintColor = Hover;
		Row.OddRowBackgroundHoveredBrush.TintColor = Hover;
		Row.ParentRowBackgroundHoveredBrush.TintColor = Hover;
		Row.SelectorFocusedBrush.OutlineSettings.Color = FSlateColor(Theme.Get(EDreamThemeSlot::Accent));
		Row.TextColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText));
		Row.SelectedTextColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText));

		S.ProgressBar->BackgroundImage.TintColor = FSlateColor(Theme.Get(EDreamThemeSlot::ChromeBorder));
	}

	void HandleViewSettingsChanged();

	/** The built-ins, the brushes and the flat button exist (no I/O); Darcula is current until something else is chosen. */
	void EnsureBuiltIns()
	{
		FState& S = State();
		if (S.BuiltIns.Num() > 0)
		{
			return;
		}
		DreamThemeTable::BuildBuiltIns(S.BuiltIns);
		check(S.BuiltIns.Num() > 0 && S.BuiltIns[0]->Id == DreamThemeTable::DarculaId);
		// EDreamThemeSlot and dream::theme's key table must agree slot for slot; the group boundaries catch a drift cheaply
		// (Dream.Theme.Json checks every slot).
		ensureMsgf(dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::ChromeBackground)) == "chrome.background"
			&& dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::OutputBackground)) == "output.background"
			&& dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::Caret)) == "editor.caret"
			&& dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::OutEcho)) == "output.echo"
			&& dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::SynNormal)) == "syntax.normal"
			&& dream::theme::SlotKey(static_cast<int>(EDreamThemeSlot::SynError)) == "syntax.error",
			TEXT("EDreamThemeSlot and dream/ThemeFormat.h list the slots in different orders"));
		S.Current = S.BuiltIns[0];
		S.Brushes.Reserve(SlotCount);
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			S.Brushes.Add(MakeUnique<FSlateColorBrush>(FLinearColor::Transparent));
		}
		// A flat icon button (PO-04 icon row): no fill at rest, a rounded hover and pressed fill, the parent's foreground.
		FButtonStyle Flat = FAppStyle::Get().HasWidgetStyle<FButtonStyle>(TEXT("SimpleButton"))
			? FAppStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("SimpleButton"))
			: FButtonStyle();
		Flat.SetNormal(FSlateNoResource())
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor::Transparent, 3.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor::Transparent, 3.f))
			.SetDisabled(FSlateNoResource())
			.SetNormalForeground(FSlateColor::UseForeground())
			.SetHoveredForeground(FSlateColor::UseForeground())
			.SetPressedForeground(FSlateColor::UseForeground())
			.SetDisabledForeground(FSlateColor::UseSubduedForeground())
			.SetNormalPadding(FMargin(3.f))
			.SetPressedPadding(FMargin(3.f, 4.f, 3.f, 2.f));
		S.FlatButton = MakeUnique<FButtonStyle>(Flat);
		// A text box (find bar, Go to line): rounded fill and outline, both retinted in place.
		FEditableTextBoxStyle Box = FAppStyle::Get().HasWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"))
			? FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"))
			: FEditableTextBoxStyle();
		const FSlateRoundedBoxBrush Rounded(FLinearColor::Transparent, 3.f, FLinearColor::Transparent, 1.f);
		Box.SetBackgroundImageNormal(Rounded)
			.SetBackgroundImageHovered(Rounded)
			.SetBackgroundImageFocused(Rounded)
			.SetBackgroundImageReadOnly(Rounded)
			.SetBackgroundColor(FSlateColor(FLinearColor::White));
		S.TextBox = MakeUnique<FEditableTextBoxStyle>(Box);

		// Scroll bars, list rows and progress bars: copies of the editor's styles with every coloured brush replaced by one
		// this file retints (the widgets keep pointers into these objects).
		const ISlateStyle& App = FAppStyle::Get();
		FScrollBarStyle Bar = App.HasWidgetStyle<FScrollBarStyle>(TEXT("ScrollBar")) ? App.GetWidgetStyle<FScrollBarStyle>(TEXT("ScrollBar")) : FScrollBarStyle();
		const FSlateRoundedBoxBrush ThumbBrush(FLinearColor::Transparent, 4.f);
		Bar.SetHorizontalBackgroundImage(FSlateNoResource())
			.SetVerticalBackgroundImage(FSlateNoResource())
			.SetHorizontalTopSlotImage(FSlateNoResource())
			.SetVerticalTopSlotImage(FSlateNoResource())
			.SetHorizontalBottomSlotImage(FSlateNoResource())
			.SetVerticalBottomSlotImage(FSlateNoResource())
			.SetNormalThumbImage(ThumbBrush)
			.SetHoveredThumbImage(ThumbBrush)
			.SetDraggedThumbImage(ThumbBrush);
		S.ScrollBar = MakeUnique<FScrollBarStyle>(Bar);
		S.TextBox->ScrollBarStyle = Bar;   // the multi-line boxes' own bars (the recovery diff), retinted with the others

		FTableRowStyle Row = App.HasWidgetStyle<FTableRowStyle>(TEXT("TableView.Row")) ? App.GetWidgetStyle<FTableRowStyle>(TEXT("TableView.Row")) : FTableRowStyle();
		const FSlateColorBrush Solid(FLinearColor::Transparent);
		Row.SetActiveBrush(Solid)
			.SetActiveHoveredBrush(Solid)
			.SetInactiveBrush(Solid)
			.SetInactiveHoveredBrush(Solid)
			.SetActiveHighlightedBrush(Solid)
			.SetInactiveHighlightedBrush(Solid)
			.SetEvenRowBackgroundBrush(FSlateNoResource())
			.SetOddRowBackgroundBrush(FSlateNoResource())
			.SetParentRowBackgroundBrush(FSlateNoResource())
			.SetEvenRowBackgroundHoveredBrush(Solid)
			.SetOddRowBackgroundHoveredBrush(Solid)
			.SetParentRowBackgroundHoveredBrush(Solid)
			.SetSelectorFocusedBrush(FSlateRoundedBoxBrush(FLinearColor::Transparent, 0.f, FLinearColor::Transparent, 1.f));
		S.TableRow = MakeUnique<FTableRowStyle>(Row);
		FTableViewStyle List = App.HasWidgetStyle<FTableViewStyle>(TEXT("ListView")) ? App.GetWidgetStyle<FTableViewStyle>(TEXT("ListView")) : FTableViewStyle();
		List.SetBackgroundBrush(FSlateNoResource());
		S.ListView = MakeUnique<FTableViewStyle>(List);

		FProgressBarStyle Progress = App.HasWidgetStyle<FProgressBarStyle>(TEXT("ProgressBar")) ? App.GetWidgetStyle<FProgressBarStyle>(TEXT("ProgressBar")) : FProgressBarStyle();
		Progress.SetBackgroundImage(FSlateColorBrush(FLinearColor::Transparent))
			.SetFillImage(FSlateColorBrush(FLinearColor::White));
		S.ProgressBar = MakeUnique<FProgressBarStyle>(Progress);
		RetintBrushes();

		// The setting is followed from the first use, also in a session without the style set (headless specs).
		if (const UDreamUserSettings* Settings = UDreamUserSettings::Get())
		{
			S.LastSeenSettingId = Settings->ThemeId;
		}
		if (!S.SettingsHandle.IsValid())
		{
			S.SettingsHandle = UDreamUserSettings::OnViewSettingsChanged().AddStatic(&HandleViewSettingsChanged);
		}
	}

	TSharedPtr<FDreamTheme> FindIn(const TArray<TSharedRef<FDreamTheme>>& Themes, const FString& Id)
	{
		for (const TSharedRef<FDreamTheme>& Theme : Themes)
		{
			if (Theme->Id.Equals(Id, ESearchCase::IgnoreCase))
			{
				return Theme;
			}
		}
		return nullptr;
	}

	dream::theme::Document ToDocument(const FDreamTheme& Theme)
	{
		dream::theme::Document Doc;
		Doc.Id = DreamText::ToUtf8(Theme.Id);
		Doc.Name = DreamText::ToUtf8(Theme.Name.ToString());
		Doc.Base = DreamText::ToUtf8(Theme.BaseId);
		Doc.bLight = Theme.bLight;
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			const FColor C = Theme.Colors[Slot].ToFColorSRGB();
			Doc.Set(Slot, dream::theme::Rgba{ C.R, C.G, C.B, C.A });
		}
		return Doc;
	}

	FString JoinUtf8(const std::vector<std::string>& Items)
	{
		FString Out;
		for (const std::string& Item : Items)
		{
			if (!Out.IsEmpty())
			{
				Out += TEXT(", ");
			}
			Out += DreamText::Utf8ToFString(Item);
		}
		return Out;
	}

	/**
	 * JSON to a theme. FallbackId names a file's theme when it has no "id". Missing and invalid colours come from the base
	 * (a built-in: "base", else Darcula, or GitHub Light for a light theme). OutNotes gets what was ignored (for a warning).
	 */
	bool HasControlCharacter(const FString& Text)
	{
		for (const TCHAR C : Text)
		{
			if (C < 0x20 || C == 0x7F)
			{
				return true;
			}
		}
		return false;
	}

	bool ImportImpl(const FString& Json, FDreamTheme& Out, FString* OutError, const FString& FallbackId, FString* OutNotes,
		bool bRequireFormat = false)
	{
		dream::theme::Document Doc;
		dream::theme::ParseReport Report;
		std::string ParseError;
		if (!dream::theme::Parse(DreamText::ToUtf8(Json), Doc, &Report, &ParseError))
		{
			if (OutError != nullptr)
			{
				*OutError = DreamText::Utf8ToFString(ParseError);
			}
			return false;
		}
		if (bRequireFormat && !Report.bHasFormat)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("it is not a Dream Python Editor theme (no \"format\": \"dream-theme\")");
			}
			return false;
		}
		FString Id = DreamText::Utf8ToFString(Doc.Id).TrimStartAndEnd();
		if (HasControlCharacter(Id))
		{
			Id.Reset();   // an id ends up in the ini (ThemeId) and in menus: the file name stands in for it
		}
		if (Id.IsEmpty())
		{
			Id = FallbackId.TrimStartAndEnd();
		}
		if (Id.IsEmpty())
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("the theme has no \"id\"");
			}
			return false;
		}

		EnsureBuiltIns();
		FState& S = State();
		const FString BaseId = DreamText::Utf8ToFString(Doc.Base).TrimStartAndEnd();
		TSharedPtr<FDreamTheme> Base = BaseId.IsEmpty() ? nullptr : FindIn(S.BuiltIns, BaseId);
		FString Notes;
		if (!BaseId.IsEmpty() && !Base.IsValid())
		{
			Notes += FString::Printf(TEXT("base \"%s\" is not a built-in theme; "), *BaseId);
		}
		if (!Base.IsValid())
		{
			Base = FindIn(S.BuiltIns, Doc.bLight ? DreamThemeTable::GitHubLightId : DreamThemeTable::DarculaId);
		}
		check(Base.IsValid());
		dream::theme::FillMissing(Doc, ToDocument(*Base));

		if (!Report.UnknownKeys.empty())
		{
			Notes += FString::Printf(TEXT("unknown keys ignored: %s; "), *JoinUtf8(Report.UnknownKeys));
		}
		if (!Report.InvalidValues.empty())
		{
			Notes += FString::Printf(TEXT("invalid values (taken from %s): %s; "), *Base->Id, *JoinUtf8(Report.InvalidValues));
		}
		if (OutNotes != nullptr)
		{
			*OutNotes = Notes.LeftChop(Notes.EndsWith(TEXT("; ")) ? 2 : 0);
		}

		Out = FDreamTheme();
		Out.Id = Id;
		const FString Name = DreamText::Utf8ToFString(Doc.Name).TrimStartAndEnd();
		Out.Name = FText::FromString(Name.IsEmpty() ? Id : Name);
		Out.bLight = Doc.bLight;
		Out.bBuiltIn = false;
		Out.BaseId = BaseId;
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			const dream::theme::Rgba& C = Doc.Colors[static_cast<size_t>(Slot)];
			Out.Colors[Slot] = FLinearColor::FromSRGBColor(FColor(C.R, C.G, C.B, C.A));
		}
		return true;
	}

	/** One file of the themes folder as one directory enumeration reports it. */
	struct FFolderFile
	{
		FString Name;
		int64 Size = 0;
		FDateTime Modified;
	};

	/** The folder's *.json files (not its subfolders), sorted by name, and a signature of names, sizes and times: one
	 *  directory enumeration, no file read. */
	TArray<FFolderFile> ScanFolder(const FString& Dir, FString& OutSignature)
	{
		TArray<FFolderFile> Files;
		IFileManager::Get().IterateDirectoryStat(*Dir, [&Files](const TCHAR* Path, const FFileStatData& Stat)
		{
			const FString Name = FPaths::GetCleanFilename(Path);
			if (!Stat.bIsDirectory && Name.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
			{
				Files.Add(FFolderFile{ Name, Stat.FileSize, Stat.ModificationTime });
			}
			return true;
		});
		Files.Sort([](const FFolderFile& A, const FFolderFile& B) { return A.Name < B.Name; });
		OutSignature = Dir;
		for (const FFolderFile& File : Files)
		{
			OutSignature += FString::Printf(TEXT("|%s:%lld:%lld"), *File.Name, File.Size, File.Modified.GetTicks());
		}
		return Files;
	}

	/** At most MaxThemeFileBytes of a file (a file that grew after the scan is refused, never read whole). */
	bool ReadBounded(const FString& Path, FString& OutText, FString& OutWhy)
	{
		const TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite));
		if (!Reader.IsValid())
		{
			OutWhy = TEXT("it cannot be read");
			return false;
		}
		const int64 Size = Reader->TotalSize();
		if (Size < 0 || Size > MaxThemeFileBytes)
		{
			OutWhy = TEXT("it is larger than 1 MB");
			return false;
		}
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(static_cast<int32>(Size));
		Reader->Serialize(Bytes.GetData(), Size);
		if (!Reader->Close() || Reader->IsError())
		{
			OutWhy = TEXT("it cannot be read");
			return false;
		}
		FFileHelper::BufferToString(OutText, Bytes.GetData(), Bytes.Num());   // UTF-8 (BOM or not), or UTF-16 with a BOM
		return true;
	}

	void LoadUserThemes()
	{
		FState& S = State();
		S.UserThemes.Reset();
		S.bUserThemesLoaded = true;
		S.LastFolderCheckSeconds = FPlatformTime::Seconds();
		const FString Dir = FDreamThemes::GetUserThemesDir();
		const TArray<FFolderFile> Files = ScanFolder(Dir, S.FolderSignature);
		for (const FFolderFile& File : Files)
		{
			const FString Path = FPaths::Combine(Dir, File.Name);
			if (File.Size > MaxThemeFileBytes)
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s skipped: it is larger than 1 MB."), *Path);
				continue;
			}
			FString Json;
			FString Why;
			if (!ReadBounded(Path, Json, Why))
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s skipped: %s."), *Path, *Why);
				continue;
			}
			TSharedRef<FDreamTheme> Theme = MakeShared<FDreamTheme>();
			FString Error;
			FString Notes;
			if (!ImportImpl(Json, *Theme, &Error, FPaths::GetBaseFilename(File.Name), &Notes, /*bRequireFormat=*/true))
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s skipped: %s."), *Path, *Error);
				continue;
			}
			if (FindIn(S.BuiltIns, Theme->Id).IsValid())
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s skipped: its id \"%s\" is a built-in theme's (give the copy another id)."), *Path, *Theme->Id);
				continue;
			}
			if (FindIn(S.UserThemes, Theme->Id).IsValid())
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s skipped: another file already has the id \"%s\"."), *Path, *Theme->Id);
				continue;
			}
			if (!Notes.IsEmpty())
			{
				UE_LOG(LogDream, Warning, TEXT("Theme file %s: %s."), *Path, *Notes);
			}
			Theme->SourcePath = Path;
			S.UserThemes.Add(Theme);
		}
		S.UserThemes.StableSort([](const TSharedRef<FDreamTheme>& A, const TSharedRef<FDreamTheme>& B)
		{
			return A->Name.CompareToCaseIgnored(B->Name) < 0;
		});
		UE_LOG(LogDream, Log, TEXT("Themes: %d user theme(s) from %s."), S.UserThemes.Num(), *Dir);
	}

	/** Loads the user themes the first time. Later calls list the folder at most once a second and, when it changed, only
	 *  schedule ReloadUserThemes on a deferred idle tick (it reads files, may apply a theme and broadcasts OnChanged, which
	 *  must not happen inside a caller's menu build or a layout pass). This call keeps returning the list it has. */
	void EnsureUserThemesLoaded()
	{
		FState& S = State();
		if (!S.bUserThemesLoaded)
		{
			LoadUserThemes();
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (S.ReloadTicker.IsValid() || Now - S.LastFolderCheckSeconds < FolderCheckIntervalSeconds)
		{
			return;
		}
		S.LastFolderCheckSeconds = Now;
		FString Signature;
		ScanFolder(FDreamThemes::GetUserThemesDir(), Signature);
		if (Signature != S.FolderSignature)
		{
			S.ReloadTicker = FDreamDeferral::AddTicker(TEXT("DreamThemeReload"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
				/*bNeedsPython=*/false, [](float)
				{
					State().ReloadTicker.Reset();
					FDreamThemes::ReloadUserThemes();
					return false;
				});
		}
	}

	TSharedPtr<FDreamTheme> FindMutable(const FString& Id)
	{
		EnsureBuiltIns();
		FState& S = State();
		if (TSharedPtr<FDreamTheme> BuiltIn = FindIn(S.BuiltIns, Id))
		{
			return BuiltIn;
		}
		EnsureUserThemesLoaded();
		return FindIn(S.UserThemes, Id);
	}

	/** Makes Theme current: brushes, button, style set, the setting (bPersist), then OnChanged. */
	void Apply(const TSharedRef<FDreamTheme>& Theme, bool bPersist, bool bBroadcast)
	{
		FState& S = State();
		S.Current = Theme;
		RetintBrushes();
		FDreamEditorStyle::ApplyTheme(*Theme);
		if (bPersist)
		{
			S.LastSeenSettingId = Theme->Id;
			UDreamUserSettings::SetThemeId(Theme->Id);   // saves this key; its OnViewSettingsChanged sees no ThemeId change
		}
		if (bBroadcast)
		{
			UE_LOG(LogDream, Log, TEXT("Theme: %s."), *Theme->Id);
			S.OnChanged.Broadcast();
		}
	}

	void ResolvePending()
	{
		FState& S = State();
		S.PendingTicker.Reset();
		const FString Id = MoveTemp(S.PendingId);
		S.PendingId.Reset();
		if (Id.IsEmpty() || Id.Equals(S.Current->Id, ESearchCase::IgnoreCase))
		{
			return;
		}
		if (const TSharedPtr<FDreamTheme> Theme = FindMutable(Id))
		{
			Apply(Theme.ToSharedRef(), /*bPersist=*/false, /*bBroadcast=*/true);
		}
		else
		{
			UE_LOG(LogDream, Warning, TEXT("Theme \"%s\" (UDreamUserSettings ThemeId) was not found in %s; showing %s."),
				*Id, *FDreamThemes::GetUserThemesDir(), *S.Current->Id);
		}
	}

	/** Every View setting broadcasts here (Editor Preferences, the View menu, SetThemeId); only a change of ThemeId acts. */
	void HandleViewSettingsChanged()
	{
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		FState& S = State();
		if (Settings == nullptr || !S.Current.IsValid() || Settings->ThemeId.Equals(S.LastSeenSettingId, ESearchCase::CaseSensitive))
		{
			return;
		}
		S.LastSeenSettingId = Settings->ThemeId;
		if (Settings->ThemeId.Equals(S.Current->Id, ESearchCase::IgnoreCase))
		{
			return;
		}
		if (!FDreamThemes::SetCurrent(Settings->ThemeId, /*bPersist=*/false))
		{
			UE_LOG(LogDream, Warning, TEXT("Theme \"%s\" is not a known theme; keeping %s."), *Settings->ThemeId, *S.Current->Id);
		}
	}

	/** The editor's own theme changed (Editor Preferences > Appearance): Unreal is read again when it is current. */
	void HandleEditorThemeChanged(FGuid)
	{
		FState& S = State();
		if (S.Current.IsValid() && S.Current->bBuiltIn && S.Current->Id == DreamThemeTable::UnrealId)
		{
			FDreamThemes::SetCurrent(DreamThemeTable::UnrealId, /*bPersist=*/false);   // rebuilds Unreal and applies it live
		}
	}

	/** Dream.Theme.Export [folder]: every built-in as <folder>/<Id>.json (default <user themes>/Built-in). */
	FAutoConsoleCommand ExportCommand(
		TEXT("Dream.Theme.Export"),
		TEXT("Writes every built-in Dream Python Editor theme as JSON to the folder given, or to <user themes folder>/Built-in."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (RefuseDuringRun(TEXT("Dream.Theme.Export")))
			{
				return;
			}
			FString Dir = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
			Dir.TrimQuotesInline();
			if (Dir.IsEmpty())
			{
				Dir = FPaths::Combine(FDreamThemes::GetUserThemesDir(), TEXT("Built-in"));
			}
			const int32 Written = FDreamThemes::ExportBuiltIns(Dir);
			UE_LOG(LogDream, Display, TEXT("Dream.Theme.Export: %d theme(s) written to %s."), Written, *Dir);
		}));

	/** Dream.Theme.Reload: reads the user themes folder again. */
	FAutoConsoleCommand ReloadCommand(
		TEXT("Dream.Theme.Reload"),
		TEXT("Reads the Dream user themes folder again."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (RefuseDuringRun(TEXT("Dream.Theme.Reload")))
			{
				return;
			}
			const int32 Count = FDreamThemes::ReloadUserThemes();
			UE_LOG(LogDream, Display, TEXT("Dream.Theme.Reload: %d user theme(s) in %s."), Count, *FDreamThemes::GetUserThemesDir());
		}));
}

void FDreamThemes::Initialize()
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	FState& S = State();
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	const FString Wanted = Settings != nullptr ? Settings->ThemeId.TrimStartAndEnd() : FString();
	S.LastSeenSettingId = Settings != nullptr ? Settings->ThemeId : FString();
	if (const TSharedPtr<FDreamTheme> BuiltIn = Wanted.IsEmpty() ? nullptr : FindIn(S.BuiltIns, Wanted))
	{
		S.Current = BuiltIn;
	}
	else if (!Wanted.IsEmpty() && !S.PendingTicker.IsValid())
	{
		// B1: no file I/O in StartupModule; the user theme is read at the first idle tick.
		S.PendingId = Wanted;
		S.PendingTicker = FDreamDeferral::AddTicker(TEXT("DreamThemeStartup"), EDreamDeferPolicy::DeferWhileExecuting, 0.f,
			/*bNeedsPython=*/false, [](float)
			{
				ResolvePending();
				return false;
			});
	}
	Apply(S.Current.ToSharedRef(), /*bPersist=*/false, /*bBroadcast=*/false);
	if (!S.SettingsHandle.IsValid())
	{
		S.SettingsHandle = UDreamUserSettings::OnViewSettingsChanged().AddStatic(&HandleViewSettingsChanged);
	}
	if (!S.EditorThemeHandle.IsValid())
	{
		S.EditorThemeHandle = USlateThemeManager::Get().OnThemeChanged().AddStatic(&HandleEditorThemeChanged);   // review r6 m5
	}
	DreamThemeDetails::Register();   // the Editor Preferences dropdown of ThemeId
}

void FDreamThemes::Shutdown()
{
	using namespace DreamThemeLocal;
	DreamThemeDetails::Unregister();
	FState& S = State();
	if (S.SettingsHandle.IsValid())
	{
		UDreamUserSettings::OnViewSettingsChanged().Remove(S.SettingsHandle);
		S.SettingsHandle.Reset();
	}
	if (S.EditorThemeHandle.IsValid() && UObjectInitialized())
	{
		USlateThemeManager::Get().OnThemeChanged().Remove(S.EditorThemeHandle);
	}
	S.EditorThemeHandle.Reset();
	if (S.PendingTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(S.PendingTicker);
		S.PendingTicker.Reset();
	}
	S.PendingId.Reset();
	if (S.ReloadTicker.IsValid())
	{
		FDreamDeferral::RemoveTicker(S.ReloadTicker);
		S.ReloadTicker.Reset();
	}
}

const FDreamTheme& FDreamThemes::Current()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().Current;
}

const FString& FDreamThemes::GetCurrentId()
{
	return Current().Id;
}

FSlateColor FDreamThemes::Color(EDreamThemeSlot Slot)
{
	return FSlateColor(LinearColor(Slot));
}

FLinearColor FDreamThemes::LinearColor(EDreamThemeSlot Slot)
{
	const int32 Index = static_cast<int32>(Slot);
	return Index >= 0 && Index < DreamThemeLocal::SlotCount ? Current().Colors[Index] : FLinearColor::Transparent;
}

TAttribute<FSlateColor> FDreamThemes::ColorAttr(EDreamThemeSlot Slot)
{
	return TAttribute<FSlateColor>::Create(TAttribute<FSlateColor>::FGetter::CreateStatic(&FDreamThemes::Color, Slot));
}

const FSlateBrush* FDreamThemes::Brush(EDreamThemeSlot Slot)
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	const int32 Index = FMath::Clamp(static_cast<int32>(Slot), 0, SlotCount - 1);
	return State().Brushes[Index].Get();
}

const FButtonStyle& FDreamThemes::FlatButtonStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().FlatButton;
}

const FScrollBarStyle& FDreamThemes::ScrollBarStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().ScrollBar;
}

const FTableRowStyle& FDreamThemes::TableRowStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().TableRow;
}

const FTableViewStyle& FDreamThemes::ListViewStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().ListView;
}

const FProgressBarStyle& FDreamThemes::ProgressBarStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().ProgressBar;
}

const FEditableTextBoxStyle& FDreamThemes::TextBoxStyle()
{
	DreamThemeLocal::EnsureBuiltIns();
	return *DreamThemeLocal::State().TextBox;
}

TArray<TSharedRef<const FDreamTheme>> FDreamThemes::List()
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	EnsureUserThemesLoaded();
	FState& S = State();
	TArray<TSharedRef<const FDreamTheme>> Out;
	Out.Reserve(S.BuiltIns.Num() + S.UserThemes.Num());
	for (const TSharedRef<FDreamTheme>& Theme : S.BuiltIns)
	{
		Out.Add(Theme);
	}
	for (const TSharedRef<FDreamTheme>& Theme : S.UserThemes)
	{
		Out.Add(Theme);
	}
	return Out;
}

TSharedPtr<const FDreamTheme> FDreamThemes::FindLoaded(const FString& Id)
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	const FString Trimmed = Id.TrimStartAndEnd();
	if (TSharedPtr<FDreamTheme> BuiltIn = FindIn(State().BuiltIns, Trimmed))
	{
		return BuiltIn;
	}
	return FindIn(State().UserThemes, Trimmed);
}

TSharedPtr<const FDreamTheme> FDreamThemes::Find(const FString& Id)
{
	return DreamThemeLocal::FindMutable(Id.TrimStartAndEnd());
}

bool FDreamThemes::SetCurrent(const FString& Id, bool bPersist)
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	FState& S = State();
	TSharedPtr<FDreamTheme> Theme = FindMutable(Id.TrimStartAndEnd());
	if (!Theme.IsValid())
	{
		return false;
	}
	S.PendingId.Reset();   // an explicit choice wins over the start-up resolution
	if (Theme->bBuiltIn && Theme->Id == DreamThemeTable::UnrealId)
	{
		// The editor's colours are user-editable: Unreal is read again whenever it is chosen.
		const int32 Index = S.BuiltIns.IndexOfByPredicate([](const TSharedRef<FDreamTheme>& T) { return T->Id == DreamThemeTable::UnrealId; });
		const TSharedRef<FDreamTheme> Fresh = DreamThemeTable::MakeUnreal();
		if (Index != INDEX_NONE)
		{
			S.BuiltIns[Index] = Fresh;
		}
		Apply(Fresh, bPersist, /*bBroadcast=*/true);
		return true;
	}
	if (S.Current == Theme)
	{
		if (bPersist)
		{
			UDreamUserSettings::SetThemeId(Theme->Id);
		}
		return true;
	}
	Apply(Theme.ToSharedRef(), bPersist, /*bBroadcast=*/true);
	return true;
}

FSimpleMulticastDelegate& FDreamThemes::OnChanged()
{
	return DreamThemeLocal::State().OnChanged;
}

FString FDreamThemes::ExportJson(const FDreamTheme& Theme)
{
	return DreamText::Utf8ToFString(dream::theme::Serialize(DreamThemeLocal::ToDocument(Theme)));
}

bool FDreamThemes::ImportJson(const FString& Json, FDreamTheme& OutTheme, FString* OutError)
{
	return DreamThemeLocal::ImportImpl(Json, OutTheme, OutError, FString(), nullptr);
}

bool FDreamThemes::ExportToFile(const FString& Id, const FString& Path, FString* OutError)
{
	const TSharedPtr<const FDreamTheme> Theme = Find(Id);
	if (!Theme.IsValid())
	{
		if (OutError != nullptr)
		{
			*OutError = FString::Printf(TEXT("no theme has the id \"%s\""), *Id);
		}
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(ExportJson(*Theme), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		if (OutError != nullptr)
		{
			*OutError = FString::Printf(TEXT("%s cannot be written"), *Path);
		}
		return false;
	}
	return true;
}

int32 FDreamThemes::ExportBuiltIns(const FString& Dir)
{
	DreamThemeLocal::EnsureBuiltIns();
	int32 Written = 0;
	for (const TSharedRef<FDreamTheme>& Theme : DreamThemeLocal::State().BuiltIns)
	{
		FString Error;
		if (ExportToFile(Theme->Id, FPaths::Combine(Dir, Theme->Id + TEXT(".json")), &Error))
		{
			++Written;
		}
		else
		{
			UE_LOG(LogDream, Warning, TEXT("Theme export: %s."), *Error);
		}
	}
	return Written;
}

FString FDreamThemes::GetUserThemesDir()
{
	const FString& Override = DreamThemeLocal::State().UserDirOverride;
	if (!Override.IsEmpty())
	{
		return Override;
	}
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("DreamPythonEditor"), TEXT("Themes")));
}

int32 FDreamThemes::ReloadUserThemes()
{
	using namespace DreamThemeLocal;
	EnsureBuiltIns();
	FState& S = State();
	LoadUserThemes();
	// The setting first (review r6 m1): a ThemeId whose file was missing is resolved again now that the folder was read.
	const UDreamUserSettings* Settings = UDreamUserSettings::Get();
	if (Settings != nullptr && S.Current.IsValid() && !Settings->ThemeId.Equals(S.Current->Id, ESearchCase::IgnoreCase))
	{
		TSharedPtr<FDreamTheme> Wanted = FindIn(S.BuiltIns, Settings->ThemeId);
		if (!Wanted.IsValid())
		{
			Wanted = FindIn(S.UserThemes, Settings->ThemeId);
		}
		if (Wanted.IsValid())
		{
			S.LastSeenSettingId = Settings->ThemeId;
			Apply(Wanted.ToSharedRef(), /*bPersist=*/false, /*bBroadcast=*/true);
			return S.UserThemes.Num();
		}
	}
	if (S.Current.IsValid() && !S.Current->bBuiltIn)
	{
		const TSharedPtr<FDreamTheme> Again = FindIn(S.UserThemes, S.Current->Id);
		if (Again.IsValid())
		{
			Apply(Again.ToSharedRef(), /*bPersist=*/false, /*bBroadcast=*/true);
		}
		else
		{
			UE_LOG(LogDream, Warning, TEXT("Theme \"%s\" is no longer in %s; showing %s."), *S.Current->Id, *GetUserThemesDir(), DreamThemeTable::DarculaId);
			Apply(S.BuiltIns[0], /*bPersist=*/false, /*bBroadcast=*/true);
		}
	}
	return S.UserThemes.Num();
}

FString FDreamThemes::GetDefaultId()
{
	return DreamThemeTable::DarculaId;
}

void FDreamThemes::SetUserThemesDirForTest(const FString& Dir)
{
	DreamThemeLocal::State().UserDirOverride = Dir;
	ReloadUserThemes();
}

void FDreamThemes::OpenUserThemesFolder()
{
	const FString Dir = GetUserThemesDir();
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	FPlatformProcess::ExploreFolder(*Dir);
}

int32 FDreamThemes::ExportBuiltInsToUserFolder()
{
	if (DreamThemeLocal::RefuseDuringRun(TEXT("Export Built-in Themes")))
	{
		return 0;
	}
	const FString Dir = FPaths::Combine(GetUserThemesDir(), TEXT("Built-in"));
	const int32 Written = ExportBuiltIns(Dir);
	UE_LOG(LogDream, Display, TEXT("Themes: %d built-in theme(s) written to %s."), Written, *Dir);
	if (Written > 0)
	{
		FPlatformProcess::ExploreFolder(*Dir);
	}
	return Written;
}
