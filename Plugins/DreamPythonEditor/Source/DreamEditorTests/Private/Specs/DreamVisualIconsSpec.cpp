// SPDX-License-Identifier: Apache-2.0
// (icons) Dream.Visual.Icons: the dream icon set's preview sheet (PO-06 F6). Every icon of FDreamIcons is drawn by Slate (its
// nanosvg rasterizer, the renderer the editor uses) in a small window of its own, tinted with its default tint, on the
// theme's editor background, and captured with FSlateApplication::TakeScreenshot to <project>/Saved/Dream/Screens:
//   icons-darcula-16.png, icons-darcula-20.png, icons-githublight-16.png, icons-githublight-20.png (one theme current at
//   a time, shown without saving it), and icons-themes-16.png (all ten built-in themes, one row each, colours read from
//   each theme directly). The window cancels the monitor's DPI scale, so 16 px is 16 screen pixels.
// The spec checks that every icon is registered under its key with a file that exists, that Icon128.png exists, and that
// every icon cell of the Darcula and GitHub Light sheets shows glyph pixels (so each SVG parsed and drew). The images are
// for people to read. Review q1 m5: a sheet bound to FDreamIcons::TintAttr is drawn once in Darcula and switched live to
// GitHub Light (icons-live-*.png); each probe icon must then be drawn in the new theme's tint, so a colour cached across a
// switch fails. It restores the theme it changed (FDreamThemes::SetCurrent with bPersist false; ThemeId on disk is
// never written).
#include "Support/DreamTestSteps.h"
#include "Support/DreamTestUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Shell/DreamEditorStyle.h"
#include "Shell/DreamIcons.h"
#include "Shell/DreamTheme.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamVisualIconsSpec
{
	constexpr int32 Columns = 9;
	const TCHAR* const SheetThemes[] = { TEXT("Darcula"), TEXT("GitHub Light") };

	FString ScreensDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Dream"), TEXT("Screens")));
	}

	FString FileTag(const FString& ThemeId)
	{
		return ThemeId.Replace(TEXT(" "), TEXT("")).ToLower();
	}

	/** One captured sheet: the window, the root to capture, each icon's image (for the per-cell pixel check). */
	struct FSheet
	{
		TSharedPtr<SWindow> Window;
		TSharedPtr<SWidget> Root;
		TArray<TPair<EDreamIcon, TSharedPtr<SWidget>>> Cells;
		FLinearColor Background = FLinearColor::Black;
		FString Name;
	};

	TSharedRef<SWindow> MakeWindow(const FVector2D& Size)
	{
		return SNew(SWindow)
			.ClientSize(Size)
			.ScreenPosition(FVector2D(60.0, 60.0))
			.AutoCenter(EAutoCenter::None)
			.SizingRule(ESizingRule::FixedSize)
			.CreateTitleBar(false)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.ActivationPolicy(EWindowActivationPolicy::Never)
			.FocusWhenFirstShown(false);
	}

	/** Content scaled by 1 / the window's DPI scale, so a 16 px icon is 16 captured pixels. */
	TSharedRef<SWidget> Unscaled(const TSharedRef<SWindow>& Window, const TSharedRef<SWidget>& Content)
	{
		const TWeakPtr<SWindow> Weak = Window;
		return SNew(SDPIScaler)
			.DPIScale(TAttribute<float>::CreateLambda([Weak]()
			{
				const TSharedPtr<SWindow> Pinned = Weak.Pin();
				const float Scale = Pinned.IsValid() ? Pinned->GetDPIScaleFactor() : 1.f;
				return Scale > 0.f ? 1.f / Scale : 1.f;
			}))
			[
				Content
			];
	}

	/** The sheet of one theme (the current one) at one size: a grid of every icon with its name. */
	void BuildSheet(FSheet& Sheet, float Size, bool bLiveTint = false)
	{
		const FDreamTheme& Theme = FDreamThemes::Current();
		Sheet.Background = Theme.Get(EDreamThemeSlot::EditorBackground);
		const float CellWidth = 120.f;
		const float CellHeight = Size + 22.f;
		const int32 Count = static_cast<int32>(EDreamIcon::Count);
		const int32 Rows = (Count + Columns - 1) / Columns;
		const FVector2D WindowSize(Columns * CellWidth + 24.0, Rows * CellHeight + 52.0);
		Sheet.Window = MakeWindow(WindowSize);

		TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(0.f));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const EDreamIcon Icon = static_cast<EDreamIcon>(Index);
			TSharedRef<SImage> Image = SNew(SImage)
				.Image(FDreamIcons::Brush(Icon))
				.DesiredSizeOverride(FVector2D(Size, Size))
				// Review q1 m5: the live sheet binds TintAttr (read at paint), the others take the colour once.
				.ColorAndOpacity(bLiveTint ? FDreamIcons::TintAttr(Icon) : TAttribute<FSlateColor>(FDreamIcons::Tint(Icon)));
			Sheet.Cells.Emplace(Icon, Image);
			Grid->AddSlot(Index % Columns, Index / Columns)
			[
				SNew(SBox).WidthOverride(CellWidth).HeightOverride(CellHeight)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[ Image ]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 3.f, 0.f, 0.f))
					[
						SNew(STextBlock)
						.Text(FText::AsCultureInvariant(FString(FDreamIcons::Name(Icon))))
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 7))
						.ColorAndOpacity(FSlateColor(Theme.Get(EDreamThemeSlot::ChromeMutedText)))
					]
				]
			];
		}

		TSharedRef<SWidget> Root = SNew(SBorder)
			.BorderImage(FDreamThemes::Brush(EDreamThemeSlot::EditorBackground))
			.Padding(FMargin(12.f, 8.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
				[
					SNew(STextBlock)
					.Text(FText::AsCultureInvariant(FString::Printf(TEXT("%s  %d px"), *Theme.Name.ToString(), FMath::RoundToInt(Size))))
					.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 9))
					.ColorAndOpacity(FSlateColor(Theme.Get(EDreamThemeSlot::ChromeText)))
				]
				+ SVerticalBox::Slot().AutoHeight()[ Grid ]
			];
		Sheet.Root = Root;
		Sheet.Window->SetContent(Unscaled(Sheet.Window.ToSharedRef(), Root));
	}

	/** All ten built-ins, one row each, at 16 px, colours read from each theme (no switch). */
	void BuildThemesSheet(FSheet& Sheet)
	{
		const int32 Count = static_cast<int32>(EDreamIcon::Count);
		TArray<TSharedRef<const FDreamTheme>> Themes;
		for (const TSharedRef<const FDreamTheme>& Theme : FDreamThemes::List())
		{
			if (Theme->bBuiltIn)
			{
				Themes.Add(Theme);
			}
		}
		const float Step = 22.f;
		Sheet.Window = MakeWindow(FVector2D(120.0 + Count * Step + 16.0, Themes.Num() * 28.0 + 8.0));
		TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
		for (const TSharedRef<const FDreamTheme>& Theme : Themes)
		{
			TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(112.f)
					[
						SNew(STextBlock)
						.Text(Theme->Name)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8))
						.ColorAndOpacity(FSlateColor(Theme->Get(EDreamThemeSlot::ChromeText)))
					]
				];
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const EDreamIcon Icon = static_cast<EDreamIcon>(Index);
				Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(Step).HAlign(HAlign_Center)
					[
						SNew(SImage)
						.Image(FDreamIcons::Brush(Icon))
						.DesiredSizeOverride(FVector2D(16.0, 16.0))
						.ColorAndOpacity(FSlateColor(FDreamIcons::TintColor(FDreamIcons::DefaultTint(Icon), *Theme)))
					]
				];
			}
			Column->AddSlot().AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor(FSlateColor(Theme->Get(EDreamThemeSlot::EditorBackground)))
				.Padding(FMargin(8.f, 6.f))
				[
					Row
				]
			];
		}
		Sheet.Root = Column;
		Sheet.Window->SetContent(Unscaled(Sheet.Window.ToSharedRef(), Column));
	}

	/** Captures Sheet.Root to Screens/<Name>.png. With Pixels, also returns the capture for the cell check. */
	bool Capture(FSheet& Sheet, FString& OutPath, FString& OutError, TArray<FColor>* OutPixels = nullptr, FIntVector* OutSize = nullptr)
	{
		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!Sheet.Root.IsValid() || !FSlateApplication::Get().TakeScreenshot(Sheet.Root.ToSharedRef(), Pixels, Size)
			|| Size.X <= 0 || Size.Y <= 0 || Pixels.Num() < Size.X * Size.Y)
		{
			OutError = FString::Printf(TEXT("%s: TakeScreenshot returned no pixels (%d x %d)"), *Sheet.Name, Size.X, Size.Y);
			return false;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		const FString Dir = ScreensDir();
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		OutPath = FPaths::Combine(Dir, Sheet.Name + TEXT(".png"));
		if (Png.Num() == 0 || !FFileHelper::SaveArrayToFile(Png, *OutPath))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
			return false;
		}
		if (OutPixels != nullptr && OutSize != nullptr)
		{
			*OutPixels = MoveTemp(Pixels);
			*OutSize = Size;
		}
		return true;
	}

	/** Pixels of the cell's image rect that differ clearly from the background (the glyph drew). */
	/** The cell pixel farthest from the background: a fully covered glyph pixel, so the icon's tint as drawn. */
	FColor StrongestGlyphPixel(const FSheet& Sheet, const TSharedPtr<SWidget>& Image, const TArray<FColor>& Pixels, const FIntVector& Size,
		const FColor& Bg)
	{
		const FGeometry& RootGeometry = Sheet.Root->GetPaintSpaceGeometry();
		const FGeometry& Geometry = Image->GetPaintSpaceGeometry();
		const FVector2D Min = FVector2D(Geometry.GetAbsolutePosition() - RootGeometry.GetAbsolutePosition());
		const FVector2D Extent = FVector2D(Geometry.GetAbsoluteSize());
		FColor Best = Bg;
		int32 BestDistance = -1;
		for (int32 Y = FMath::Max(0, FMath::FloorToInt(Min.Y)); Y < FMath::Min(Size.Y, FMath::CeilToInt(Min.Y + Extent.Y)); ++Y)
		{
			for (int32 X = FMath::Max(0, FMath::FloorToInt(Min.X)); X < FMath::Min(Size.X, FMath::CeilToInt(Min.X + Extent.X)); ++X)
			{
				const FColor& P = Pixels[Y * Size.X + X];
				const int32 Distance = FMath::Abs(P.R - Bg.R) + FMath::Abs(P.G - Bg.G) + FMath::Abs(P.B - Bg.B);
				if (Distance > BestDistance)
				{
					BestDistance = Distance;
					Best = P;
				}
			}
		}
		return Best;
	}

	int32 GlyphPixels(const FSheet& Sheet, const TSharedPtr<SWidget>& Image, const TArray<FColor>& Pixels, const FIntVector& Size)
	{
		const FGeometry& RootGeometry = Sheet.Root->GetPaintSpaceGeometry();
		const FGeometry& Geometry = Image->GetPaintSpaceGeometry();
		const FVector2D Min = FVector2D(Geometry.GetAbsolutePosition() - RootGeometry.GetAbsolutePosition());
		const FVector2D Extent = FVector2D(Geometry.GetAbsoluteSize());
		const FColor Bg = Sheet.Background.ToFColor(/*bSRGB*/ true);
		int32 Found = 0;
		for (int32 Y = FMath::FloorToInt(Min.Y); Y < FMath::CeilToInt(Min.Y + Extent.Y); ++Y)
		{
			for (int32 X = FMath::FloorToInt(Min.X); X < FMath::CeilToInt(Min.X + Extent.X); ++X)
			{
				if (X < 0 || Y < 0 || X >= Size.X || Y >= Size.Y)
				{
					continue;
				}
				const FColor& P = Pixels[Y * Size.X + X];
				if (FMath::Abs(P.R - Bg.R) + FMath::Abs(P.G - Bg.G) + FMath::Abs(P.B - Bg.B) > 60)
				{
					++Found;
				}
			}
		}
		return Found;
	}
}

BEGIN_DEFINE_SPEC(FDreamVisualIconsSpec, "Dream.Visual.Icons", DreamTest::AttendedFlags)
	TSharedPtr<DreamVisualIconsSpec::FSheet> Sheet;
	FString PreviousTheme;
	TArray<FString> Written;
END_DEFINE_SPEC(FDreamVisualIconsSpec)

void FDreamVisualIconsSpec::Define()
{
	It("registers every icon under Dream.Icon.<Name> with an SVG file that exists, and ships Icon128.png", [this]()
	{
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(FDreamEditorStyle::GetStyleSetName());
		if (!TestNotNull(TEXT("the DreamEditorStyle set is registered"), Style))
		{
			return;
		}
		TSet<FName> Keys;
		for (int32 Index = 0; Index < static_cast<int32>(EDreamIcon::Count); ++Index)
		{
			const EDreamIcon Icon = static_cast<EDreamIcon>(Index);
			const FName Key = FDreamIcons::StyleKey(Icon);
			TestEqual(*FString::Printf(TEXT("%s: the key"), FDreamIcons::Name(Icon)), Key.ToString(), FString(TEXT("Dream.Icon.")) + FDreamIcons::Name(Icon));
			TestFalse(*FString::Printf(TEXT("%s: the key is unique"), FDreamIcons::Name(Icon)), Keys.Contains(Key));
			Keys.Add(Key);
			const FSlateBrush* Brush = FDreamIcons::Brush(Icon);
			if (TestNotNull(*FString::Printf(TEXT("%s: a brush is registered"), FDreamIcons::Name(Icon)), Brush))
			{
				TestEqual(*FString::Printf(TEXT("%s: the brush is the set's SVG"), FDreamIcons::Name(Icon)), Brush->GetResourceName().ToString(),
					FDreamIcons::FilePath(Icon));
				TestEqual(*FString::Printf(TEXT("%s: 16 px natural size"), FDreamIcons::Name(Icon)), FVector2D(Brush->GetImageSize()), FVector2D(16.0, 16.0));
			}
			TestTrue(*FString::Printf(TEXT("%s: %s exists"), FDreamIcons::Name(Icon), *FDreamIcons::FilePath(Icon)),
				IFileManager::Get().FileExists(*FDreamIcons::FilePath(Icon)));
		}
		TestEqual(TEXT("the tab icon is the brand mark"), Style->GetBrush(DreamStyleKeys::TabIcon)->GetResourceName().ToString(),
			FDreamIcons::FilePath(EDreamIcon::Brand));
		const FString Icon128 = FPaths::Combine(FPaths::GetPath(FDreamIcons::GetIconsDir()), TEXT("Icon128.png"));
		TestTrue(*FString::Printf(TEXT("%s exists (the Plugins browser's icon)"), *Icon128), IFileManager::Get().FileExists(*Icon128));
	});

	LatentIt("follows a live theme switch: TintAttr icons are recoloured without a rebuild (review q1 m5)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamVisualIconsSpec;
		PreviousTheme = FDreamThemes::GetCurrentId();
		DreamTest::FDreamSteps Steps(*this);
		// The icons checked: one per tint family (a theme slot, the accent, a dream accent).
		const TArray<EDreamIcon> Probes = { EDreamIcon::Run, EDreamIcon::Error, EDreamIcon::Warning, EDreamIcon::Class, EDreamIcon::Scratches, EDreamIcon::Open };
		Steps.Do(TEXT("Darcula: open the live sheet once"), [this]()
		{
			if (!TestTrue(TEXT("Darcula is current"), FDreamThemes::SetCurrent(TEXT("Darcula"), /*bPersist*/ false)))
			{
				return false;
			}
			Sheet = MakeShared<FSheet>();
			Sheet->Name = TEXT("icons-live-darcula-16");
			BuildSheet(*Sheet, 16.f, /*bLiveTint*/ true);
			FSlateApplication::Get().AddWindow(Sheet->Window.ToSharedRef(), /*bShowImmediately*/ true);
			return true;
		});
		for (const TCHAR* ThemeId : SheetThemes)
		{
			const FString Id = ThemeId;
			if (Id != TEXT("Darcula"))
			{
				Steps.Do(FString::Printf(TEXT("switch to %s live (no rebuild)"), *Id), [this, Id]()
				{
					Sheet->Name = FString::Printf(TEXT("icons-live-%s-16"), *FileTag(Id));
					return TestTrue(*FString::Printf(TEXT("%s is current"), *Id), FDreamThemes::SetCurrent(Id, /*bPersist*/ false));
				});
			}
			Steps.WaitFrames(4);
			Steps.Do(FString::Printf(TEXT("%s: each probe is drawn in the current theme's tint"), *Id), [this, Id, Probes]()
			{
				FString Path;
				FString Error;
				TArray<FColor> Pixels;
				FIntVector Captured(0, 0, 0);
				if (!Capture(*Sheet, Path, Error, &Pixels, &Captured))
				{
					AddError(Error);
					return false;
				}
				Written.Add(Path);
				const FColor Bg = FDreamThemes::Current().Get(EDreamThemeSlot::EditorBackground).ToFColor(/*bSRGB*/ true);
				for (const TPair<EDreamIcon, TSharedPtr<SWidget>>& Cell : Sheet->Cells)
				{
					if (!Probes.Contains(Cell.Key))
					{
						continue;
					}
					const FColor Want = FDreamIcons::TintColor(FDreamIcons::DefaultTint(Cell.Key)).ToFColor(/*bSRGB*/ true);
					const FColor Got = StrongestGlyphPixel(*Sheet, Cell.Value, Pixels, Captured, Bg);
					const int32 Off = FMath::Max3(FMath::Abs(Got.R - Want.R), FMath::Abs(Got.G - Want.G), FMath::Abs(Got.B - Want.B));
					TestTrue(*FString::Printf(TEXT("%s: %s is drawn in its tint #%02X%02X%02X (strongest pixel #%02X%02X%02X)"), *Id,
						FDreamIcons::Name(Cell.Key), Want.R, Want.G, Want.B, Got.R, Got.G, Got.B), Off <= 12);
				}
				return true;
			});
		}
		Steps.Finally(TEXT("close the live sheet"), [this]()
		{
			if (Sheet.IsValid() && Sheet->Window.IsValid())
			{
				FSlateApplication::Get().RequestDestroyWindow(Sheet->Window.ToSharedRef());
			}
			Sheet.Reset();
		});
		Steps.Finally(TEXT("restore the theme"), [this]()
		{
			if (!PreviousTheme.IsEmpty())
			{
				FDreamThemes::SetCurrent(PreviousTheme, /*bPersist*/ false);
			}
			TestEqual(TEXT("the theme is restored"), FDreamThemes::GetCurrentId(), PreviousTheme);
		});
		Steps.Run(Done);
	});

	LatentIt("draws the whole set in Darcula and GitHub Light at 16 and 20 px, and all ten themes at 16 px", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamVisualIconsSpec;
		Written.Reset();
		PreviousTheme = FDreamThemes::GetCurrentId();
		DreamTest::FDreamSteps Steps(*this);
		for (const TCHAR* ThemeId : SheetThemes)
		{
			for (const float Size : { 16.f, 20.f })
			{
				const FString Id = ThemeId;
				const FString Name = FString::Printf(TEXT("icons-%s-%d"), *FileTag(Id), FMath::RoundToInt(Size));
				Steps.Do(FString::Printf(TEXT("open the %s sheet"), *Name), [this, Id, Size, Name]()
				{
					if (!TestTrue(*FString::Printf(TEXT("the %s theme is current"), *Id), FDreamThemes::SetCurrent(Id, /*bPersist*/ false)))
					{
						return false;
					}
					Sheet = MakeShared<FSheet>();
					Sheet->Name = Name;
					BuildSheet(*Sheet, Size);
					FSlateApplication::Get().AddWindow(Sheet->Window.ToSharedRef(), /*bShowImmediately*/ true);
					return true;
				});
				Steps.WaitFrames(5);
				Steps.Do(FString::Printf(TEXT("capture %s"), *Name), [this, Size]()
				{
					FString Path;
					FString Error;
					TArray<FColor> Pixels;
					FIntVector Captured(0, 0, 0);
					if (!Capture(*Sheet, Path, Error, &Pixels, &Captured))
					{
						AddError(Error);
						return false;
					}
					Written.Add(Path);
					AddInfo(FString::Printf(TEXT("wrote %s (%d x %d, DPI scale %.2f)"), *Path, Captured.X, Captured.Y, Sheet->Window->GetDPIScaleFactor()));
					const int32 Minimum = Size < 18.f ? 6 : 9;
					for (const TPair<EDreamIcon, TSharedPtr<SWidget>>& Cell : Sheet->Cells)
					{
						const int32 Found = GlyphPixels(*Sheet, Cell.Value, Pixels, Captured);
						TestTrue(*FString::Printf(TEXT("%s: %s draws (%d glyph pixels)"), *Sheet->Name, FDreamIcons::Name(Cell.Key), Found), Found >= Minimum);
					}
					return true;
				});
				Steps.Finally(FString::Printf(TEXT("close %s"), *Name), [this]()
				{
					if (Sheet.IsValid() && Sheet->Window.IsValid())
					{
						FSlateApplication::Get().RequestDestroyWindow(Sheet->Window.ToSharedRef());
					}
					Sheet.Reset();
				});
			}
		}
		Steps.Do(TEXT("open the all-themes sheet"), [this]()
		{
			Sheet = MakeShared<FSheet>();
			Sheet->Name = TEXT("icons-themes-16");
			BuildThemesSheet(*Sheet);
			FSlateApplication::Get().AddWindow(Sheet->Window.ToSharedRef(), /*bShowImmediately*/ true);
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("capture icons-themes-16"), [this]()
		{
			FString Path;
			FString Error;
			if (!Capture(*Sheet, Path, Error))
			{
				AddError(Error);
				return false;
			}
			Written.Add(Path);
			AddInfo(FString::Printf(TEXT("wrote %s"), *Path));
			return true;
		});
		Steps.Finally(TEXT("close the all-themes sheet"), [this]()
		{
			if (Sheet.IsValid() && Sheet->Window.IsValid())
			{
				FSlateApplication::Get().RequestDestroyWindow(Sheet->Window.ToSharedRef());
			}
			Sheet.Reset();
		});
		Steps.Finally(TEXT("restore the theme"), [this]()
		{
			if (!PreviousTheme.IsEmpty())
			{
				FDreamThemes::SetCurrent(PreviousTheme, /*bPersist*/ false);
			}
			TestEqual(TEXT("the theme is restored"), FDreamThemes::GetCurrentId(), PreviousTheme);
		});
		Steps.Run(Done);
	});
}
