// SPDX-License-Identifier: Apache-2.0
// Dream.Visual.Capture: screenshots of the Dream tab and the Run window for the visual review against mockups 01 and 02
// (SPEC §9.1, §9.5; TDD §5.5.3, §6.4; WS-02, WS-03, PO-01, PO-03). A demo buffer (an import, a for loop that prints, a
// function definition, an expression statement, a deliberate NameError at the end) is walked with Alt+Enter twice through
// FDreamKeyRouter::RoutePre (the Dream.Run.LineAdvance path), run whole with Ctrl+Shift+Enter, and a REPL submission
// auto-prints a result; the tab is captured after each step, with the output area on top (the default) and at the bottom,
// plus the error block scrolled to its end, the Problems and REPL tabs, and the editor window (the WS-07 item).
// A long run (40 steps of dream_editor.progress and time.sleep(0.1)) is captured from inside the supervisor's pump: the
// Run window is a separate popup modal SWindow, drawn by Tick(All) in the pump, so a Slate post-tick hook on the game
// thread (GIL released, §5.4.4) takes it with FSlateApplication::TakeScreenshot, as Dream.Supervisor.Stop requests Stop.
// The PNGs go to <project>/Saved/Dream/Screens/. The spec checks that every capture was written, that the Run window was
// up during the long run and that its rect follows the output area (§5.5.3 step 4); the images are for people to read.
// (chrome) PO-04: the captures show the Maya-minimal layout (the View settings at their defaults: no tab row, prompt,
// status line or breadcrumb) in Darcula, then the same tab with every View part on, the Problems and REPL tabs with their
// toggles on, and a light theme (GitHub Light: the idle tab, every part on, and the Run window). The spec checks the
// Maya-minimal layout at its first capture and that each theme is current at its captures; it restores every View
// setting and the theme it changed. Review r6: the first capture is checked for the Maya-minimal structure (no breadcrumb,
// no language badge, the icon row, the gutter, no run header or footer rows) and, from its pixels, for Darcula's colours
// (editor and history #2B2B2B, the menu bar #3C3F41); the light captures for GitHub Light's history colour and the REPL
// prompt strip's ChromeBorder rules. The theme is shown without saving it (FDreamThemes::SetCurrent, bPersist false); the
// restore writes back only the keys that changed (the setters and UDreamUserSettings::SaveKey, never SaveConfig).
// (sidebar) PO-05: every capture shows the side bar (View > Project and View > Structure, on by default) with a capture
// root and a capture scratch folder under DreamHost/Saved/DreamTests/SideBarCapture (the roots are the test override
// FDreamProjectFiles::SetRootsForTest, so the user's Project Roots are never read or written; the scratch folder is the
// test override), and two more captures show it with a file of the root active, the caret in a method: its row
// highlighted in the Project tree and that method highlighted in Structure (R-p1-9): 14 in Darcula, 15 in GitHub Light,
// each checked for the side bar's chrome background and both highlights' current-line colour.
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"   // (sidebar)
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Settings/DreamPerProjectUserSettings.h"   // (sidebar)
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamProjectTree.h"   // (sidebar)
#include "Shell/SDreamSideBar.h"       // (sidebar)
#include "Shell/SDreamStructureView.h" // (sidebar) R-p1-9: the Structure highlight's pixel check
#include "Shell/DreamProjectFiles.h"   // (sidebar)
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamOutputRows.h"
#include "Shell/DreamTheme.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamVisualCaptureSpec
{
	/** Where the captures go: <project>/Saved/Dream/Screens. */
	FString ScreensDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Dream"), TEXT("Screens")));
	}

	/** (chrome) The pixels of the last tab capture. TakeScreenshot reads the tab's rect from its window's back buffer, in
	 *  window space: the rect starts at the tab's position minus the window's, truncated to whole pixels (RectMin), so a
	 *  point in Slate's absolute space maps to WindowPixel(point) - RectMin. */
	struct FTabPixels
	{
		TArray<FColor> Pixels;
		FIntPoint Size = FIntPoint::ZeroValue;   // not an FIntVector: its deprecated XYZ member warns in the spec's copy
		FVector2D WindowOrigin = FVector2D::ZeroVector;   // the window's position in Slate's absolute space
		FIntPoint RectMin = FIntPoint::ZeroValue;         // the capture rect's top left in window pixels
		float Scale = 1.f;
		FString Name;

		bool IsValid() const { return Size.X > 0 && Size.Y > 0 && Pixels.Num() >= Size.X * Size.Y; }

		/** The capture's column and row that hold the absolute point. */
		int32 Column(double AbsoluteX) const { return FMath::FloorToInt32(AbsoluteX - WindowOrigin.X) - RectMin.X; }
		int32 Row(double AbsoluteY) const { return FMath::FloorToInt32(AbsoluteY - WindowOrigin.Y) - RectMin.Y; }
		/** The first capture row whose pixel lies wholly at or below AbsoluteY. */
		int32 FirstRowBelow(double AbsoluteY) const { return FMath::CeilToInt32(AbsoluteY - WindowOrigin.Y - 0.01) - RectMin.Y; }

		bool AtPixel(int32 X, int32 Y, FColor& Out) const
		{
			if (!IsValid() || X < 0 || Y < 0 || X >= Size.X || Y >= Size.Y)
			{
				return false;
			}
			Out = Pixels[Y * Size.X + X];
			return true;
		}

		bool At(const FVector2D& Absolute, FColor& Out) const
		{
			return AtPixel(Column(Absolute.X), Row(Absolute.Y), Out);
		}

		/** The colour most points of a 9 x 9 grid over Widget's area (fractions Min..Max of its paint geometry) have. */
		FColor ModeOver(const SWidget& Widget, const FVector2f& Min, const FVector2f& Max) const
		{
			const FGeometry& Geometry = Widget.GetPaintSpaceGeometry();
			TMap<uint32, int32> Counts;
			for (int32 IY = 0; IY < 9; ++IY)
			{
				for (int32 IX = 0; IX < 9; ++IX)
				{
					const FVector2f Fraction(FMath::Lerp(Min.X, Max.X, IX / 8.f), FMath::Lerp(Min.Y, Max.Y, IY / 8.f));
					FColor Pixel;
					if (At(FVector2D(Geometry.GetAbsolutePositionAtCoordinates(Fraction)), Pixel))
					{
						++Counts.FindOrAdd(Pixel.ToPackedARGB());
					}
				}
			}
			uint32 Best = 0;
			int32 BestCount = -1;
			for (const TPair<uint32, int32>& Count : Counts)
			{
				if (Count.Value > BestCount)
				{
					Best = Count.Key;
					BestCount = Count.Value;
				}
			}
			return BestCount < 0 ? FColor(0, 0, 0, 0) : FColor(Best);
		}
	};

	bool Near(const FColor& A, const FColor& B, int32 Tolerance = 2)
	{
		return FMath::Abs(int32(A.R) - int32(B.R)) <= Tolerance && FMath::Abs(int32(A.G) - int32(B.G)) <= Tolerance
			&& FMath::Abs(int32(A.B) - int32(B.B)) <= Tolerance;
	}

	FString Hex(const FColor& Color)
	{
		return FString::Printf(TEXT("#%02X%02X%02X"), Color.R, Color.G, Color.B);
	}

	FColor ThemeColor(EDreamThemeSlot Slot)
	{
		return FDreamThemes::LinearColor(Slot).ToFColorSRGB();
	}

	/** (chrome) Root and every widget under it that Match accepts. */
	void Collect(const TSharedRef<SWidget>& Root, TFunctionRef<bool(const TSharedRef<SWidget>&)> Match, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Match(Root))
		{
			Out.Add(Root);
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			Collect(Children->GetChildAt(Index), Match, Out);
		}
	}

	TSharedPtr<SWidget> FindTagged(const TSharedRef<SWidget>& Root, FName Tag)
	{
		TArray<TSharedRef<SWidget>> Found;
		Collect(Root, [Tag](const TSharedRef<SWidget>& W)
		{
			const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
			return Meta.IsValid() && Meta->Tag == Tag;
		}, Found);
		return Found.Num() > 0 ? TSharedPtr<SWidget>(Found[0]) : nullptr;
	}

	/** Widget and every widget between it and Stop are visible. */
	bool IsShownUnder(const TSharedPtr<SWidget>& Widget, const TSharedPtr<SWidget>& Stop)
	{
		if (!Widget.IsValid())
		{
			return false;
		}
		for (TSharedPtr<SWidget> W = Widget; W.IsValid() && W != Stop; W = W->GetParentWidget())
		{
			if (!W->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		return true;
	}

	/** (chrome) What only a mis-encoded literal produces: a UTF-8 lead byte read as a sign-extended char (U+FFC2..U+FFF4)
	 *  followed by a continuation byte read the same way (U+FF80..U+FFBF), a C1 control, or U+FFFD. Fullwidth characters on
	 *  their own (a file named in fullwidth forms) are text, not damage (review r10). */
	bool FindMisencoded(const FString& Text, TCHAR& OutChar)
	{
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			const TCHAR Next = Index + 1 < Text.Len() ? Text[Index + 1] : TCHAR(0);
			if ((Char >= 0x80 && Char <= 0x9F) || Char == 0xFFFD || (Char >= 0xFFC2 && Char <= 0xFFF4 && Next >= 0xFF80 && Next <= 0xFFBF))
			{
				OutChar = Char;
				return true;
			}
		}
		return false;
	}

	/** (chrome) Every text block under Root and every label of the chrome's menus is free of mis-encoded characters. */
	void ExpectCleanTexts(FAutomationTestBase& Test, SDreamTab& Tab, const TCHAR* Where)
	{
		TArray<FString> Texts;
		TArray<TSharedRef<SWidget>> Blocks;
		Collect(Tab.AsShared(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("STextBlock")); }, Blocks);
		for (const TSharedRef<SWidget>& Block : Blocks)
		{
			Texts.Add(StaticCastSharedRef<STextBlock>(Block)->GetText().ToString());
		}
		for (const TCHAR* Menu : { TEXT("File"), TEXT("Edit"), TEXT("History"), TEXT("Command"), TEXT("View"), TEXT("Help"), TEXT("Run"),
			TEXT("RecentFiles"), TEXT("ShowInOutput"), TEXT("Theme"), TEXT("OutputPosition") })
		{
			Texts.Append(Tab.GetMenuLabelsForTest(FName(Menu)));
		}
		int32 Bad = 0;
		for (const FString& Text : Texts)
		{
			TCHAR Char = 0;
			if (FindMisencoded(Text, Char))
			{
				++Bad;
				Test.AddError(FString::Printf(TEXT("%s: the text \"%s\" holds U+%04X, a mis-encoded character (Slate draws it from the last-resort font)"),
					Where, *Text, static_cast<uint32>(Char)));
			}
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %d texts and menu labels checked for mis-encoded characters, %d found"), Where, Texts.Num(), Bad));
	}

	/** No run header or footer row for RunNumber in the history (run headers off, PO-04). */
	bool HasHeaderRows(SDreamTab& Tab, int32 RunNumber)
	{
		TArray<EDreamOutputRowKind> Kinds;
		TArray<FString> Texts;
		TArray<uint64> Seqs;
		Tab.GetOutputArea()->GetOutputView()->GetRowsForTest(RunNumber, Kinds, Texts, Seqs);
		return Kinds.Contains(EDreamOutputRowKind::Header) || Kinds.Contains(EDreamOutputRowKind::Footer);
	}

	/**
	 * Draws Widget's window and reads Widget's rect back (FSlateApplication::TakeScreenshot: PrivateDrawWindows, then a
	 * renderer readback that flushes the rendering commands), then writes it as an opaque PNG. Game thread. False with a
	 * reason when the widget is in no window or the readback is empty. Never asserts: TakeScreenshot generates the widget
	 * path with GeneratePathToWidgetChecked, so the widget's window is looked up first.
	 */
	bool SaveWidgetPng(const TSharedRef<SWidget>& Widget, const FString& Name, FString& OutPath, FString& OutError,
		TArray<FColor>* OutPixels = nullptr, FIntVector* OutSize = nullptr)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("Slate is not initialized");
			return false;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = App.FindWidgetWindow(Widget);
		if (!Window.IsValid())
		{
			OutError = FString::Printf(TEXT("%s: the widget is in no window (a background tab?)"), *Name);
			return false;
		}
		if (Window->IsWindowMinimized() || !Window->IsVisible())
		{
			OutError = FString::Printf(TEXT("%s: its window is minimised or hidden"), *Name);
			return false;
		}
		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!App.TakeScreenshot(Widget, Pixels, Size) || Size.X <= 0 || Size.Y <= 0 || Pixels.Num() < Size.X * Size.Y)
		{
			OutError = FString::Printf(TEXT("%s: TakeScreenshot returned no pixels (%d x %d, %d read)"), *Name, Size.X, Size.Y, Pixels.Num());
			return false;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;   // the back buffer's alpha is not coverage
		}
		if (OutPixels != nullptr && OutSize != nullptr)
		{
			*OutPixels = Pixels;
			*OutSize = Size;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		if (Png.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s: PNG compression failed"), *Name);
			return false;
		}
		const FString Dir = ScreensDir();
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		OutPath = FPaths::Combine(Dir, Name + TEXT(".png"));
		if (!FFileHelper::SaveArrayToFile(Png, *OutPath))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
			return false;
		}
		return true;
	}

	/** A chord as Slate delivers it to SDreamCodeView's OnKeyDownHandler, routed with a focused editable buffer's context. */
	FReply PressChord(SDreamTab& Tab, bool bCtrl, bool bShift, bool bAlt)
	{
		const FModifierKeysState Mods(bShift, false, bCtrl, false, bAlt, false, false, false, false);
		const FKeyEvent Event(EKeys::Enter, Mods, static_cast<uint32>(0), /*bInIsRepeat*/ false, /*InCharacterCode*/ 13, /*InKeyCode*/ 13);
		const uint32 Context = dream::Ctx_Focused | dream::Ctx_Editable | dream::Ctx_Buffer;
		return FDreamKeyRouter::Get().RoutePre(Event, Context, Tab);
	}

	/** What the in-pump capture saw. */
	struct FRunCapture
	{
		FString Name;                   // file name without extension
		double AtSeconds = 0.0;         // seconds after the submit
		bool bTaken = false;
		bool bWindowOpen = false;
		FString Path;
		FString Error;
		FVector2D WindowPos = FVector2D::ZeroVector;
		FVector2D WindowSize = FVector2D::ZeroVector;
		FSlateRect AreaRect;
		bool bAreaRect = false;
		bool bLiveView = false;         // the Run window's live run view at this pump (VIS-02)
		bool bLiveFollowing = false;
		int32 LiveNewLinesBelow = 0;
	};

	struct FLongRun
	{
		int32 RunNumber = 0;
		double SubmitSeconds = 0.0;
		bool bOutputAtTop = true;
		FDelegateHandle PostTickHandle;
		TArray<FRunCapture> Captures;
		int32 PumpsSeen = 0;
	};

	/** Slate post-tick inside a pump of our run (GT, GIL released): takes each due capture of the Run window once. */
	void OnPostTick(FLongRun& Long, FRunSupervisor& Supervisor, const FDreamRunService& RunService, const TWeakPtr<SDreamTab>& WeakTab)
	{
		if (Supervisor.GetPumpDepth() <= 0)
		{
			return;
		}
		const FDreamRunRecord* Active = RunService.GetActiveRecord();
		if (!Active || Active->RunNumber != Long.RunNumber)
		{
			return;
		}
		++Long.PumpsSeen;
		const double Elapsed = FPlatformTime::Seconds() - Long.SubmitSeconds;
		for (FRunCapture& Capture : Long.Captures)
		{
			if (Capture.bTaken || Elapsed < Capture.AtSeconds)
			{
				continue;
			}
			Capture.bTaken = true;
			Capture.bWindowOpen = Supervisor.IsRunWindowOpen();
			const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
			if (!Capture.bWindowOpen || !Modal.IsValid())
			{
				Capture.Error = TEXT("the Run window is not up at this pump");
				continue;
			}
			if (const TSharedPtr<SDreamOutputView> Live = Supervisor.GetLiveRunViewForTest())
			{
				Capture.bLiveView = true;
				Capture.bLiveFollowing = Live->IsFollowing();
				Capture.LiveNewLinesBelow = Live->GetNewLinesBelow();
			}
			Capture.WindowPos = Modal->GetPositionInScreen();
			Capture.WindowSize = Modal->GetSizeInScreen();
			if (const TSharedPtr<SDreamTab> Tab = WeakTab.Pin())
			{
				float Dpi = 1.f;
				Capture.bAreaRect = Tab->GetOutputArea().IsValid() && Tab->GetOutputArea()->GetDesktopRect(Capture.AreaRect, Dpi);
			}
			SaveWidgetPng(Modal.ToSharedRef(), Capture.Name, Capture.Path, Capture.Error);
		}
	}

	TArray<FString> DemoSource(const FString& Sfx)
	{
		return {
			TEXT("import math"),
			TEXT("for i in range(3):"),
			TEXT("    print(f\"square of {i} is {i * i}\")"),
			TEXT("def circle_area(r):"),
			TEXT("    return math.pi * r ** 2"),
			TEXT("circle_area(2.0)"),
			FString::Printf(TEXT("missing_%s + 1"), *Sfx),
		};
	}

	FString LongRunSource()
	{
		const TArray<FString> Lines = {
			TEXT("import time"),
			TEXT("import dream_editor"),
			TEXT("for i in range(40):"),
			TEXT("    dream_editor.progress(i, 40, f\"item {i}\")"),
			TEXT("    print(f\"step {i:02d}\")"),
			TEXT("    time.sleep(0.1)"),
			TEXT("print(\"long run done\")"),
		};
		return FString::Join(Lines, TEXT("\n"));
	}

	struct FSaved
	{
		bool bSaved = false;
		EDreamOutputPosition Position = EDreamOutputPosition::Top;
		bool bRunPump = true;
		EDreamRunPresentationSetting Presentation = EDreamRunPresentationSetting::RunWindow;
		// (chrome) PO-04: the View toggles and the theme the captures change.
		bool bViewValues[6] = {};
		FString ThemeId;                 // UDreamUserSettings::ThemeId (restored with SetThemeId only if it changed)
		FString CurrentThemeId;          // the theme shown at the start (differs when the setting names a missing theme)
		bool bEchoVisible = true;   // the store's echo visibility: the captures show the echo (restored after)
		// (sidebar) PO-05: the side bar's View keys and the per-project roots, shown with the capture folders.
		bool bProjectPanel = true;
		bool bStructurePanel = true;
		TArray<FString> ExpandedFolders;     // the tree saves expansions with the session; the captures' are taken back
		TArray<FString> CollapsedRoots;
		FString SideBarDir;              // DreamHost/Saved/DreamTests/SideBarCapture (made and removed by the spec)
		FString ScratchFolderBefore;     // DreamScratch's test folder as found (put back, not cleared; scratch's cross-area nit)
	};

	/** (sidebar) The capture root and scratch folder: a few scripts, a folder, a non-Python file, scratch files. */
	FString MakeSideBarFolders()
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamTests"), TEXT("SideBarCapture")));
		FPaths::NormalizeDirectoryName(Dir);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);   // the spec's own folder from an earlier pass
		const TPair<const TCHAR*, const TCHAR*> Files[] = {
			{ TEXT("scripts/tools/__init__.py"), TEXT("") },
			{ TEXT("scripts/tools/actors.py"), TEXT("import unreal\n") },
			{ TEXT("scripts/level_utils.py"), TEXT("def selected_actors():\n    return []\n") },
			{ TEXT("scripts/notes.txt"), TEXT("notes\n") },
			{ TEXT("scripts/structure_demo.py"), TEXT("import functools\n\n\nclass SideBarDemo:\n    \"\"\"Shows the Structure panel.\"\"\"\n\n    def __init__(self, name):\n        self.name = name\n\n    @functools.lru_cache\n    def area(self, r):\n        return 3.14159 * r * r\n\n    async def load(self):\n        return self.name\n\n\ndef circle_area(r):\n    def square(x):\n        return x * x\n    return 3.14159 * square(r)\n\n\n# %% run me\nprint(circle_area(2.0))\n") },
			{ TEXT("Scratches/scratch_1.py"), TEXT("print('scratch 1')\n") },
			{ TEXT("Scratches/scratch_2.py"), TEXT("print('scratch 2')\n") },
			{ TEXT("Scratches/Samples/demo.py"), TEXT("# demo\n") },
		};
		for (const TPair<const TCHAR*, const TCHAR*>& File : Files)
		{
			const FString Path = Dir / File.Key;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FFileHelper::SaveStringToFile(FString(File.Value), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		return Dir;
	}

	const EDreamViewSetting ViewSettings[] = { EDreamViewSetting::ReplTab, EDreamViewSetting::ProblemsTab, EDreamViewSetting::ReplPrompt,
		EDreamViewSetting::RunHeaders, EDreamViewSetting::StatusLine, EDreamViewSetting::LineNumbers };

	/** Every View part on (bAll) or the PO-04 defaults (everything off but line numbers). */
	void SetAllViewParts(bool bAll)
	{
		for (const EDreamViewSetting Setting : ViewSettings)
		{
			UDreamUserSettings::SetViewSetting(Setting, bAll || Setting == EDreamViewSetting::LineNumbers);
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamVisualCaptureSpec, "Dream.Visual.Capture", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	TArray<FString> Written;
	DreamVisualCaptureSpec::FTabPixels LastPixels;   // (chrome) the last tab capture, for the colour checks

	/** A step that captures the Dream tab body as Name.png; a failure is an error. */
	void AddCaptureTab(DreamTest::FDreamSteps& Steps, const FString& Name);
	/** (chrome) A step that sets the theme (live) and checks it is current. */
	void AddSetTheme(DreamTest::FDreamSteps& Steps, const FString& ThemeId);
	/** (sidebar) The last capture shows the side bar: the capture root, Scratches last, the active file's row highlighted,
	 *  an outline in Structure; the panel is the theme's chrome background and the highlight its current line. */
	void AddSideBarChecks(const FString& Dir, const TCHAR* Theme);
	/** Submits the long run in its scratch buffer with the post-tick hook armed and waits for it; Prefix names the PNGs. */
	void AddLongRun(DreamTest::FDreamSteps& Steps, const TSharedRef<DreamVisualCaptureSpec::FLongRun>& Long, const TSharedRef<FGuid>& LongBuffer,
		const FString& Prefix, bool bTop);
END_DEFINE_SPEC(FDreamVisualCaptureSpec)

void FDreamVisualCaptureSpec::AddCaptureTab(DreamTest::FDreamSteps& Steps, const FString& Name)
{
	Steps.WaitFrames(3);
	Steps.Do(FString::Printf(TEXT("capture %s"), *Name), [this, Name]()
	{
		if (!Tab.IsValid())
		{
			AddError(TEXT("no Dream tab to capture"));
			return false;
		}
		FString Path;
		FString Error;
		LastPixels = DreamVisualCaptureSpec::FTabPixels();
		FIntVector Size(0, 0, 0);
		if (!DreamVisualCaptureSpec::SaveWidgetPng(Tab.ToSharedRef(), Name, Path, Error, &LastPixels.Pixels, &Size))
		{
			AddError(FString::Printf(TEXT("capture %s: %s"), *Name, *Error));
			return false;
		}
		LastPixels.Size = FIntPoint(Size.X, Size.Y);
		// As TakeScreenshot placed the rect: the tab's position in its window, truncated.
		const TSharedPtr<SWindow> TabWindow = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
		LastPixels.WindowOrigin = TabWindow.IsValid() ? FVector2D(TabWindow->GetPositionInScreen()) : FVector2D::ZeroVector;
		const FVector2D TabPosition(Tab->GetPaintSpaceGeometry().AbsolutePosition);
		LastPixels.RectMin = FIntPoint(static_cast<int32>(TabPosition.X - LastPixels.WindowOrigin.X),
			static_cast<int32>(TabPosition.Y - LastPixels.WindowOrigin.Y));
		LastPixels.Scale = Tab->GetPaintSpaceGeometry().Scale;
		LastPixels.Name = Name;
		Written.Add(Path);
		AddInfo(FString::Printf(TEXT("wrote %s"), *Path));
		return true;
	});
}

void FDreamVisualCaptureSpec::AddSetTheme(DreamTest::FDreamSteps& Steps, const FString& ThemeId)
{
	Steps.Do(FString::Printf(TEXT("theme %s"), *ThemeId), [this, ThemeId]()
	{
		if (!TestTrue(*FString::Printf(TEXT("the %s theme exists"), *ThemeId), FDreamThemes::Find(ThemeId).IsValid()))
		{
			return false;
		}
		FDreamThemes::SetCurrent(ThemeId, /*bPersist*/ false);   // shown, not saved: ThemeId on disk stays the user's
		return TestEqual(TEXT("the theme is current"), FDreamThemes::GetCurrentId(), ThemeId);
	});
}

void FDreamVisualCaptureSpec::AddSideBarChecks(const FString& Dir, const TCHAR* Theme)
{
	using namespace DreamVisualCaptureSpec;
	const TSharedPtr<SDreamSideBar> Side = Tab.IsValid() ? Tab->GetSideBar() : nullptr;
	if (!TestTrue(*FString::Printf(TEXT("%s: the side bar shows"), Theme), Side.IsValid() && Side->IsProjectShown() && Side->IsStructureShown()))
	{
		return;
	}
	const TArray<SDreamProjectTree::FRowForTest> Rows = Side->GetProjectTree()->GetRowsForTest();
	TArray<FString> Top;
	const SDreamProjectTree::FRowForTest* ActiveRow = nullptr;
	for (const SDreamProjectTree::FRowForTest& Row : Rows)
	{
		if (Row.Depth == 0)
		{
			Top.Add(Row.Label);
		}
		ActiveRow = Row.bActive ? &Row : ActiveRow;
	}
	TestEqual(*FString::Printf(TEXT("%s: the capture root, then Scratches"), Theme), FString::Join(Top, TEXT(" | ")), FString(TEXT("scripts | Scratches")));
	TestTrue(*FString::Printf(TEXT("%s: the active file's row is highlighted"), Theme), ActiveRow != nullptr && ActiveRow->Label == TEXT("structure_demo.py"));
	TestTrue(*FString::Printf(TEXT("%s: Scratches lists its files"), Theme),
		Rows.ContainsByPredicate([](const SDreamProjectTree::FRowForTest& Row) { return Row.Label == TEXT("scratch_1.py"); }));
	TArray<TSharedRef<SWidget>> Outline;
	Collect(Side->GetStructureView().ToSharedRef(), [](const TSharedRef<SWidget>& W)
	{
		return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(TEXT("SideBarDemo"));
	}, Outline);
	TestTrue(*FString::Printf(TEXT("%s: Structure shows the active file's outline (SideBarDemo)"), Theme), Outline.Num() > 0);
	if (!TestTrue(*FString::Printf(TEXT("%s: the capture has pixels"), Theme), LastPixels.IsValid()))
	{
		return;
	}
	const FColor Chrome = ThemeColor(EDreamThemeSlot::ChromeBackground);
	const FColor Panel = LastPixels.ModeOver(*Side->GetStructureSection(), FVector2f(0.1f, 0.85f), FVector2f(0.9f, 0.97f));
	TestTrue(*FString::Printf(TEXT("%s: the side bar is the chrome background %s (%s)"), Theme, *Hex(Chrome), *Hex(Panel)), Near(Panel, Chrome));
	const FColor Line = ThemeColor(EDreamThemeSlot::CurrentLine);
	// structure-p2 n2: the OS pointer resting on a row gives it the hover brush, which wins over the highlight by design: such
	// a row is reported, not failed (the pointer is the user's; a synthetic move is undone by the next synthesized one).
	auto UnderPointer = [](const SWidget& Row)
	{
		return Row.IsHovered() || Row.GetTickSpaceGeometry().IsUnderLocation(FSlateApplication::Get().GetCursorPos());
	};
	if (const TSharedPtr<SWidget> Row = Side->GetProjectTree()->FindRowWidgetForTest(Dir / TEXT("scripts") / TEXT("structure_demo.py")))
	{
		const FColor Seen = LastPixels.ModeOver(*Row, FVector2f(0.7f, 0.2f), FVector2f(0.98f, 0.8f));
		if (UnderPointer(*Row))
		{
			AddInfo(FString::Printf(TEXT("%s: the active file's row is under the pointer (hover wins): %s, not checked against %s"), Theme, *Hex(Seen), *Hex(Line)));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the active file's row is the current-line colour %s (%s)"), Theme, *Hex(Line), *Hex(Seen)), Near(Seen, Line));
		}
	}
	// R-p1-9: the Structure entry holding the caret (SideBarDemo.area) is drawn on the current-line colour too: the band of
	// its row, right of its text and inside the Structure section.
	TArray<TSharedRef<SWidget>> AreaTexts;
	Collect(Side->GetStructureView().ToSharedRef(), [](const TSharedRef<SWidget>& W)
	{
		return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == TEXT("area");
	}, AreaTexts);
	if (TestEqual(*FString::Printf(TEXT("%s: Structure shows the method area once"), Theme), AreaTexts.Num(), 1))
	{
		// The row that holds the text (a table row: its type name ends in "Row"), sampled right of the text.
		TSharedPtr<SWidget> Row = AreaTexts[0];
		while (Row.IsValid() && !Row->GetTypeAsString().EndsWith(TEXT("Row")))
		{
			Row = Row->GetParentWidget();
		}
		if (TestTrue(*FString::Printf(TEXT("%s: the area entry has a row"), Theme), Row.IsValid()))
		{
			const FColor Seen = LastPixels.ModeOver(*Row, FVector2f(0.6f, 0.25f), FVector2f(0.9f, 0.75f));
			if (UnderPointer(*Row))
			{
				AddInfo(FString::Printf(TEXT("%s: Structure's area row is under the pointer (hover wins): %s, not checked against %s"), Theme, *Hex(Seen), *Hex(Line)));
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("%s: Structure's highlighted entry (area) is the current-line colour %s (%s, row %s %.0f x %.0f)"), Theme,
					*Hex(Line), *Hex(Seen), *Row->GetTypeAsString(), Row->GetPaintSpaceGeometry().GetLocalSize().X, Row->GetPaintSpaceGeometry().GetLocalSize().Y),
					Near(Seen, Line));
			}
		}
	}
}

void FDreamVisualCaptureSpec::AddLongRun(DreamTest::FDreamSteps& Steps, const TSharedRef<DreamVisualCaptureSpec::FLongRun>& Long,
	const TSharedRef<FGuid>& LongBuffer, const FString& Prefix, bool bTop)
{
	TSharedRef<DreamTest::FRunResult> Result = MakeShared<DreamTest::FRunResult>();
	Steps.Do(FString::Printf(TEXT("submit the long run (%s)"), *Prefix), [this, Long, LongBuffer, Prefix, bTop, Result]()
	{
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		FRunSupervisor* Supervisor = &Module.GetSupervisor();
		const FDreamRunService* RunService = &Module.GetRunService();
		const TSharedPtr<FDreamBuffer> Buffer = Module.GetBufferManager().FindById(*LongBuffer);
		if (!Buffer.IsValid())
		{
			AddError(TEXT("the long-run buffer is gone"));
			return false;
		}
		Tab->ActivateBuffer(*LongBuffer);
		Long->bOutputAtTop = bTop;
		Long->Captures.Reset();
		for (const TPair<const TCHAR*, double>& Due : { TPair<const TCHAR*, double>(TEXT("-early"), 1.2), TPair<const TCHAR*, double>(TEXT("-late"), 3.2) })
		{
			DreamVisualCaptureSpec::FRunCapture& Capture = Long->Captures.AddDefaulted_GetRef();
			Capture.Name = Prefix + Due.Key;
			Capture.AtSeconds = Due.Value;
		}
		const TWeakPtr<SDreamTab> WeakTab = Tab;
		Long->PostTickHandle = FSlateApplication::Get().OnPostTick().AddLambda([Long, Supervisor, RunService, WeakTab](float)
		{
			DreamVisualCaptureSpec::OnPostTick(*Long, *Supervisor, *RunService, WeakTab);
		});
		Long->SubmitSeconds = FPlatformTime::Seconds();
		Result->SubmitSeconds = Long->SubmitSeconds;
		Result->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, Buffer->Snapshot().LineCount()));
		Long->RunNumber = Result->RunNumber;
		return Result->RunNumber > 0;
	});
	DreamTest::AddWaitForRun(Steps, FString::Printf(TEXT("the long run (%s)"), *Prefix), Result, 60.0);
	Steps.Do(FString::Printf(TEXT("check the Run window captures (%s)"), *Prefix), [this, Long, Result, Prefix]()
	{
		if (Long->PostTickHandle.IsValid())
		{
			FSlateApplication::Get().OnPostTick().Remove(Long->PostTickHandle);
			Long->PostTickHandle.Reset();
		}
		DreamTest::ExpectOutcome(*this, *Result, EDreamRunOutcome::Ok);
		// §5.4.6: the pump that builds and first paints the Run window is not steady pump cost (VIS-04).
		TestFalse(*FString::Printf(TEXT("%s: no pump overload (max pump %.1f ms)"), *Prefix, Result->Record.Stats.MaxPumpMs),
			Result->Record.Stats.bPumpOverload);
		// The hand-off of a following live view leaves the Output view following (§5.5.3 "Following").
		if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
		{
			TestTrue(*FString::Printf(TEXT("%s: the Output view follows after the hand-off"), *Prefix), View->IsFollowing());
		}
		AddInfo(FString::Printf(TEXT("%s: %d pumps seen by the hook, %d pumps in the record, %.1f s"), *Prefix, Long->PumpsSeen,
			Result->Record.Stats.Pumps, Result->Record.EndSeconds - Result->Record.StartSeconds));
		for (const DreamVisualCaptureSpec::FRunCapture& Capture : Long->Captures)
		{
			if (!TestTrue(*FString::Printf(TEXT("%s: a pump at %.1f s took the capture"), *Capture.Name, Capture.AtSeconds), Capture.bTaken))
			{
				continue;
			}
			TestTrue(*FString::Printf(TEXT("%s: the Run window is up during the long run (RT-04, §5.5.3)"), *Capture.Name), Capture.bWindowOpen);
			// §5.5.3, PO-03: the live run view follows the run (its newest line at the bottom), with nothing waiting below.
			if (TestTrue(*FString::Printf(TEXT("%s: the Run window has a live run view"), *Capture.Name), Capture.bLiveView))
			{
				TestTrue(*FString::Printf(TEXT("%s: the live run view is following (VIS-02)"), *Capture.Name), Capture.bLiveFollowing);
				TestEqual(*FString::Printf(TEXT("%s: no new lines wait below the live run view"), *Capture.Name), Capture.LiveNewLinesBelow, 0);
			}
			if (!Capture.Error.IsEmpty() || Capture.Path.IsEmpty())
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Capture.Name, *Capture.Error));
				continue;
			}
			Written.Add(Capture.Path);
			AddInfo(FString::Printf(TEXT("wrote %s (window at %.0f,%.0f size %.0f x %.0f)"), *Capture.Path, Capture.WindowPos.X, Capture.WindowPos.Y,
				Capture.WindowSize.X, Capture.WindowSize.Y));
			if (Capture.bAreaRect)
			{
				// §5.5.3 step 4: the window covers the output area; shorter areas grow toward the code area, narrower ones
				// are 480 wide from the area's left edge.
				const FSlateRect& Area = Capture.AreaRect;
				AddInfo(FString::Printf(TEXT("%s: output area at %.0f,%.0f size %.0f x %.0f"), *Capture.Name, Area.Left, Area.Top,
					Area.Right - Area.Left, Area.Bottom - Area.Top));
				TestEqual(*FString::Printf(TEXT("%s: Run window left = output area left"), *Capture.Name), Capture.WindowPos.X, static_cast<double>(Area.Left), 2.0);
				TestTrue(*FString::Printf(TEXT("%s: Run window at least as wide as the output area"), *Capture.Name),
					Capture.WindowSize.X + 2.0 >= Area.Right - Area.Left);
				TestTrue(*FString::Printf(TEXT("%s: Run window at least as tall as the output area"), *Capture.Name),
					Capture.WindowSize.Y + 2.0 >= Area.Bottom - Area.Top);
				if (Long->bOutputAtTop)
				{
					TestEqual(*FString::Printf(TEXT("%s: top layout: the window's top is the area's top"), *Capture.Name), Capture.WindowPos.Y,
						static_cast<double>(Area.Top), 2.0);
				}
				else
				{
					TestEqual(*FString::Printf(TEXT("%s: bottom layout: the window's bottom is the area's bottom"), *Capture.Name),
						Capture.WindowPos.Y + Capture.WindowSize.Y, static_cast<double>(Area.Bottom), 2.0);
				}
			}
		}
		return true;
	});
	AddCaptureTab(Steps, Prefix + TEXT("-after"));
}

void FDreamVisualCaptureSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		Tab.Reset();
	});

	It("tells mis-encoded text from fullwidth names (review r11)", [this]()
	{
		// (chrome) The check the captures use: a UTF-8 section sign (C2 A7) read as sign-extended chars is damage; a file named in
		// fullwidth forms is text.
		TCHAR Char = 0;
		const TCHAR Damaged[] = { TCHAR(0xFFC2), TCHAR(0xFFA7), TCHAR('1'), TCHAR(0) };
		TestTrue(TEXT("U+FFC2 U+FFA7 (a sign-extended C2 A7) is mis-encoded"), DreamVisualCaptureSpec::FindMisencoded(FString(Damaged), Char));
		TestEqual(TEXT("and the lead is reported"), static_cast<uint32>(Char), 0xFFC2u);
		const TCHAR Fullwidth[] = { TCHAR(0x8A2D), TCHAR(0x8A08), TCHAR(0xFF08), TCHAR(0xFF12), TCHAR(0xFF09), TCHAR('.'), TCHAR('p'), TCHAR('y'), TCHAR(0) };
		TestFalse(TEXT("a fullwidth file name is text"), DreamVisualCaptureSpec::FindMisencoded(FString(Fullwidth), Char));
		const TCHAR Controls[] = { TCHAR('a'), TCHAR(0x0085), TCHAR(0) };
		TestTrue(TEXT("a C1 control is mis-encoded"), DreamVisualCaptureSpec::FindMisencoded(FString(Controls), Char));
		const TCHAR Replacement[] = { TCHAR(0xFFFD), TCHAR(0) };
		TestTrue(TEXT("U+FFFD is mis-encoded"), DreamVisualCaptureSpec::FindMisencoded(FString(Replacement), Char));
	});

	LatentIt("captures the Maya-minimal tab after line-advance, buffer and REPL runs, at Top and Bottom, every View part, a light theme, and the Run window during a long run",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		Written.Reset();
		const FString Sfx = DreamTest::UniqueSuffix();
		const TSharedPtr<FDreamBuffer> Demo = DreamTest::OpenScratch(*this, *Tab, FString::Join(DreamVisualCaptureSpec::DemoSource(Sfx), TEXT("\n")), TEXT("Demo"));
		const TSharedPtr<FDreamBuffer> LongBuf = DreamTest::OpenScratch(*this, *Tab, DreamVisualCaptureSpec::LongRunSource(), TEXT("LongRun"));
		if (!Demo.IsValid() || !LongBuf.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds = { Demo->GetId(), LongBuf->GetId() };
		const FGuid DemoId = Demo->GetId();
		TSharedRef<FGuid> LongId = MakeShared<FGuid>(LongBuf->GetId());

		// The buffer run's NameError reaches LogPython at Error (§6.3); declared before any run is submitted.
		const FString ErrorSummary = FString::Printf(TEXT("NameError: name 'missing_%s' is not defined"), *Sfx);
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		DreamTest::ExpectPythonErrorBlock(*this, DreamTest::MakeBufferRequest(*Demo, EDreamRunRoute::Buffer, 1, 7).Filename, ErrorSummary,
			{ FString::Printf(TEXT("missing_%s + 1"), *Sfx) });

		TSharedRef<DreamVisualCaptureSpec::FSaved> Saved = MakeShared<DreamVisualCaptureSpec::FSaved>();
		TSharedRef<DreamTest::FRunResult> Advance1 = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Advance2 = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Whole = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Repl = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamVisualCaptureSpec::FLongRun> LongTop = MakeShared<DreamVisualCaptureSpec::FLongRun>();
		TSharedRef<DreamVisualCaptureSpec::FLongRun> LongBottom = MakeShared<DreamVisualCaptureSpec::FLongRun>();
		TSharedRef<DreamVisualCaptureSpec::FLongRun> LongLight = MakeShared<DreamVisualCaptureSpec::FLongRun>();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("pin the layout and the presentation"), [this, Saved, DemoId]()
		{
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			Saved->Position = Settings->OutputPosition;
			Saved->bRunPump = Settings->bRunPump;
			Saved->Presentation = Settings->RunPresentation;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(DreamVisualCaptureSpec::ViewSettings); ++Index)
			{
				Saved->bViewValues[Index] = Settings->GetViewSetting(DreamVisualCaptureSpec::ViewSettings[Index]);
			}
			Saved->ThemeId = Settings->ThemeId;
			Saved->CurrentThemeId = FDreamThemes::GetCurrentId();
			// The echo always shows in the captures: the views' visibility only (the saved Echo setting is not touched).
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			FDreamOutputVisibility Visibility = Store.GetVisibility();
			Saved->bEchoVisible = Visibility.bEcho;
			Visibility.bEcho = true;
			Store.SetVisibility(Visibility);
			// (sidebar) PO-05: the side bar on (its defaults), with the capture root and scratch folder, in memory only.
			Saved->bProjectPanel = Settings->bShowProjectPanel;
			Saved->bStructurePanel = Settings->bShowStructurePanel;
			Saved->ExpandedFolders = UDreamPerProjectUserSettings::Get()->ExpandedFolders;
			Saved->CollapsedRoots = UDreamPerProjectUserSettings::Get()->CollapsedRoots;
			Saved->SideBarDir = DreamVisualCaptureSpec::MakeSideBarFolders();
			Saved->bSaved = true;
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			const TArray<FString> CaptureRoots = { Saved->SideBarDir / TEXT("scripts") };
			FDreamProjectFiles::SetRootsForTest(&CaptureRoots);   // the user's Project Roots are never read or written (p1 m7)
			const bool bNoPluginFolders = false;
			FDreamProjectFiles::SetPluginFoldersForTest(&bNoPluginFolders);   // PO-05-A1: the captures show the capture root only
			FDreamProjectFiles::SetDefaultRootForTest(Saved->SideBarDir / TEXT("NoContentPython"));   // and no project Content/Python
			Saved->ScratchFolderBefore = DreamScratch::GetFolderForTest();
			DreamScratch::SetFolderForTest(Saved->SideBarDir / TEXT("Scratches"));
			if (const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar())
			{
				Side->GetProjectTree()->RefreshRoots();
			}
			// (chrome) PO-04: the Maya-minimal default layout in Darcula.
			DreamVisualCaptureSpec::SetAllViewParts(false);
			FDreamThemes::SetCurrent(TEXT("Darcula"), /*bPersist*/ false);
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			// The setting first: SetOutputPosition saves the user-global EditorSettings.ini only when the position changes, and
			// a save now would write the pinned values above for every project (P2-tst-02).
			Settings->OutputPosition = EDreamOutputPosition::Top;
			Tab->SetOutputPosition(EDreamOutputPosition::Top);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			FDreamEditorModule::Get().GetOutputStore().Clear();
			Tab->ActivateBuffer(DemoId);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(DemoId);
			if (!View.IsValid() || Tab->GetActiveCodeView() != View)
			{
				AddError(TEXT("the demo buffer is not the active code view"));
				return false;
			}
			View->SetCaret(0, 0);
			View->FocusEditor();
			Tab->GetOutputArea()->EndPromptReveal(/*bEvenIfTyped*/ true);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("check the Maya-minimal layout (PO-04)"), [this]()
		{
			// (chrome, review r6) The structure of the Maya-minimal tab; its colours are checked from the capture's pixels.
			TArray<TSharedRef<SWidget>> Crumbs;
			DreamVisualCaptureSpec::Collect(Tab.ToSharedRef(), [](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(TEXT(" › scratch"));
			}, Crumbs);
			TestEqual(TEXT("no breadcrumb row"), Crumbs.Num(), 0);
			TestFalse(TEXT("no language badge on the input tabs"), DreamVisualCaptureSpec::FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamTab.Language"))).IsValid());
			for (const TCHAR* Icon : { TEXT("DreamChrome.OpenFile"), TEXT("DreamChrome.Save"), TEXT("DreamChrome.RunSelectionOrBuffer"),
				TEXT("DreamChrome.Stop"), TEXT("DreamChrome.ClearOutput") })
			{
				TestTrue(*FString::Printf(TEXT("the icon row shows %s"), Icon),
					DreamVisualCaptureSpec::IsShownUnder(DreamVisualCaptureSpec::FindTagged(Tab.ToSharedRef(), FName(Icon)), Tab));
			}
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				TArray<TSharedRef<SWidget>> Gutters;
				DreamVisualCaptureSpec::Collect(Code.ToSharedRef(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamGutter")); }, Gutters);
				TestTrue(TEXT("the gutter shows line numbers"), Gutters.Num() > 0 && DreamVisualCaptureSpec::IsShownUnder(Gutters[0], Code));
			}
			DreamVisualCaptureSpec::ExpectCleanTexts(*this, *Tab, TEXT("the Maya-minimal tab"));
			TestFalse(TEXT("no status line"), Tab->IsStatusLineShown());
			TestFalse(TEXT("no output tab row"), Tab->GetOutputArea()->IsTabRowShown());
			TestFalse(TEXT("no REPL prompt"), Tab->GetOutputArea()->IsPromptShown());
			TestEqual(TEXT("the menu bar has six menus"), Tab->GetMenuTitlesForTest().Num(), 6);
			TestFalse(TEXT("the caret's line:col is in the input tab row"), Tab->GetCaretText().IsEmpty());
			// (sidebar) PO-05: the side bar shows by default, Project above Structure.
			const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar();
			TestTrue(TEXT("the side bar shows the Project tree and the Structure view (PO-05)"), Side.IsValid() && Side->IsProjectShown() && Side->IsStructureShown());
			return true;
		});
		AddCaptureTab(Steps, TEXT("01-tab-top-idle"));
		Steps.Do(TEXT("Darcula's colours in the capture (review r6)"), [this]()
		{
			if (!TestTrue(TEXT("the capture has pixels"), LastPixels.IsValid()))
			{
				return false;
			}
			const FColor Background(0x2B, 0x2B, 0x2B);
			const FColor Chrome(0x3C, 0x3F, 0x41);
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				// The lower right of the code view: below the demo's seven lines, right of the gutter.
				const FColor Seen = LastPixels.ModeOver(*Code, FVector2f(0.55f, 0.6f), FVector2f(0.9f, 0.95f));
				TestTrue(*FString::Printf(TEXT("the editor is #2B2B2B (%s)"), *DreamVisualCaptureSpec::Hex(Seen)), DreamVisualCaptureSpec::Near(Seen, Background));
			}
			if (const TSharedPtr<SDreamOutputView> History = Tab->GetOutputArea()->GetOutputView())
			{
				const FColor Seen = LastPixels.ModeOver(*History, FVector2f(0.2f, 0.2f), FVector2f(0.9f, 0.9f));
				TestTrue(*FString::Printf(TEXT("the history is #2B2B2B (%s)"), *DreamVisualCaptureSpec::Hex(Seen)), DreamVisualCaptureSpec::Near(Seen, Background));
			}
			// The menu bar right of Help: the chrome background.
			const TSharedPtr<SWidget> Help = DreamVisualCaptureSpec::FindTagged(Tab.ToSharedRef(), FName(TEXT("DreamChrome.Menu.Help")));
			if (TestTrue(TEXT("the menu bar has Help"), Help.IsValid()))
			{
				const FGeometry& G = Help->GetPaintSpaceGeometry();
				const FVector2D Right(G.GetAbsolutePositionAtCoordinates(FVector2f(1.f, 0.5f)));
				const double TabRight = Tab->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(1.f, 0.f)).X;
				int32 Hits = 0;
				int32 Total = 0;
				FColor First;
				for (double X = Right.X + 12.0; X < TabRight - 12.0; X += 24.0)
				{
					FColor Pixel;
					if (LastPixels.At(FVector2D(X, Right.Y), Pixel))
					{
						First = Total == 0 ? Pixel : First;
						++Total;
						Hits += DreamVisualCaptureSpec::Near(Pixel, Chrome) ? 1 : 0;
					}
				}
				TestTrue(*FString::Printf(TEXT("the menu bar right of Help is #3C3F41 (%d of %d points; first %s)"), Hits, Total, *DreamVisualCaptureSpec::Hex(First)),
					Total > 0 && Hits * 10 >= Total * 9);
			}
			return true;
		});

		// Alt+Enter twice (RUN-26): the import, then the whole for loop.
		const TArray<TSharedRef<DreamTest::FRunResult>> Presses = { Advance1, Advance2 };
		for (int32 Index = 0; Index < Presses.Num(); ++Index)
		{
			const TSharedRef<DreamTest::FRunResult> Run = Presses[Index];
			Steps.Do(FString::Printf(TEXT("Alt+Enter %d"), Index + 1), [this, Run, Index]()
			{
				const FReply Reply = DreamVisualCaptureSpec::PressChord(*Tab, false, false, true);
				Run->SubmitSeconds = FPlatformTime::Seconds();
				Run->RunNumber = DreamTest::GetActiveRunNumber();
				if (!Reply.IsEventHandled() || Run->RunNumber <= 0)
				{
					AddError(FString::Printf(TEXT("Alt+Enter %d submitted no run"), Index + 1));
					return false;
				}
				return true;
			});
			DreamTest::AddWaitForRun(Steps, FString::Printf(TEXT("Alt+Enter run %d"), Index + 1), Run, Index == 0 ? 60.0 : 30.0);
			Steps.Do(FString::Printf(TEXT("check Alt+Enter run %d"), Index + 1), [this, Run]()
			{
				// (chrome, review r6) Run headers are off (PO-04): the history has no header or footer row for the run.
				TestFalse(*FString::Printf(TEXT("no run header or footer rows for run #%d"), Run->RunNumber),
					DreamVisualCaptureSpec::HasHeaderRows(*Tab, Run->RunNumber));
				return DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			});
			AddCaptureTab(Steps, FString::Printf(TEXT("%02d-tab-top-advance-%d"), Index + 2, Index + 1));
		}

		// Ctrl+Shift+Enter: the whole buffer, ending in the NameError.
		Steps.Do(TEXT("Ctrl+Shift+Enter"), [this, Whole]()
		{
			const FReply Reply = DreamVisualCaptureSpec::PressChord(*Tab, true, true, false);
			Whole->SubmitSeconds = FPlatformTime::Seconds();
			Whole->RunNumber = DreamTest::GetActiveRunNumber();
			if (!Reply.IsEventHandled() || Whole->RunNumber <= 0)
			{
				AddError(TEXT("Ctrl+Shift+Enter submitted no run"));
				return false;
			}
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the buffer run"), Whole);
		Steps.Do(TEXT("check the buffer run"), [this, Whole]()
		{
			if (DreamTest::ExpectOutcome(*this, *Whole, EDreamRunOutcome::Error))
			{
				TestEqual(TEXT("the NameError is on line 7"), Whole->Record.Location.Line, 7);
				TestTrue(TEXT("the buffer run is in statement mode (RUN-27)"), Whole->Record.bStatementMode);
			}
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, ErrorSummary);
		AddCaptureTab(Steps, TEXT("04-tab-top-buffer-error"));
		// The same block scrolled to its end (the "↓ n new lines" chip's action), so the traceback and footer show.
		Steps.Do(TEXT("scroll the output to its end"), [this]()
		{
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				// OUT-09: a view nobody scrolled keeps following, though the block is taller than the view (VIS-01).
				TestTrue(*FString::Printf(TEXT("after the buffer run the Output view is following (%d new lines below)"), View->GetNewLinesBelow()),
					View->IsFollowing());
				View->ScrollToEnd();
			}
			return true;
		});
		AddCaptureTab(Steps, TEXT("04b-tab-top-buffer-error-end"));
		Steps.Do(TEXT("show Problems (View ▸ Problems Tab on)"), [this]()
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProblemsTab, true);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Problems);
			return true;
		});
		AddCaptureTab(Steps, TEXT("04c-tab-top-problems"));
		Steps.Do(TEXT("back to Output (Problems tab off again)"), [this]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProblemsTab, false);
			return true;
		});

		// A REPL submission through the prompt strip (Ctrl+Enter path): echo (no prompt, A226) and # Result: (RUN-08, OUT-12).
		Steps.Do(TEXT("REPL: circle_area(1.5) with Ctrl+Enter"), [this, Repl]()
		{
			const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea()->GetReplStrip();
			if (!Strip.IsValid())
			{
				AddError(TEXT("no REPL prompt strip"));
				return false;
			}
			Strip->PutInPrompt(TEXT("circle_area(1.5)"));
			Strip->Submit(EDreamReplSubmit::Force);
			Repl->SubmitSeconds = FPlatformTime::Seconds();
			Repl->RunNumber = DreamTest::GetActiveRunNumber();
			if (Repl->RunNumber <= 0)
			{
				AddError(TEXT("the REPL strip submitted no run"));
				return false;
			}
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the REPL run"), Repl);
		Steps.Do(TEXT("check the REPL run"), [this, Repl]()
		{
			if (DreamTest::ExpectOutcome(*this, *Repl, EDreamRunOutcome::Ok))
			{
				TestTrue(*FString::Printf(TEXT("the REPL run auto-printed a result: %s"), *Repl->Describe()),
					Repl->HasLineContaining(dream::LineType::Result, TEXT("7.06")));
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					TestTrue(TEXT("after the REPL run the Output view is following (OUT-09)"), View->IsFollowing());
				}
			}
			return true;
		});
		AddCaptureTab(Steps, TEXT("05-tab-top-repl-result"));
		Steps.Do(TEXT("show the REPL tab (View ▸ REPL Tab and REPL Prompt on)"), [this]()
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, true);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Repl);
			return true;
		});
		AddCaptureTab(Steps, TEXT("05b-tab-top-repl-tab"));
		Steps.Do(TEXT("every View part on (REPL and Problems tabs, prompt, run headers, status line, line numbers)"), [this]()
		{
			DreamVisualCaptureSpec::SetAllViewParts(true);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ScrollToEnd();
			}
			return true;
		});
		AddCaptureTab(Steps, TEXT("05c-tab-top-all-parts"));
		Steps.Do(TEXT("every part's texts are clean"), [this]()
		{
			DreamVisualCaptureSpec::ExpectCleanTexts(*this, *Tab, TEXT("every View part on"));
			return true;
		});
		Steps.Do(TEXT("back to the Maya-minimal layout, scrolled to the end"), [this]()
		{
			DreamVisualCaptureSpec::SetAllViewParts(false);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				Code->FocusEditor();
			}
			Tab->GetOutputArea()->EndPromptReveal(/*bEvenIfTyped*/ true);
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ScrollToEnd();
			}
			return true;
		});

		// The long run with the output area on top: the Run window over the output area (PO-03).
		AddLongRun(Steps, LongTop, LongId, TEXT("06-runwindow-top"), true);

		// layout.output_position = bottom (WS-03): the same history below the code, then the long run again.
		Steps.Do(TEXT("move the output area to the bottom"), [this, DemoId]()
		{
			UDreamUserSettings::GetMutable()->OutputPosition = EDreamOutputPosition::Bottom;   // no save with the pinned values
			Tab->SetOutputPosition(EDreamOutputPosition::Bottom);
			Tab->ActivateBuffer(DemoId);
			return true;
		});
		AddCaptureTab(Steps, TEXT("07-tab-bottom-idle"));
		AddLongRun(Steps, LongBottom, LongId, TEXT("08-runwindow-bottom"), false);

		// The editor window that holds the tab, for the WS-07 status-bar item (§11.6.2) beside the docked tab.
		Steps.WaitFrames(3);
		Steps.Do(TEXT("capture the editor window"), [this]()
		{
			const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
			if (!Window.IsValid())
			{
				AddError(TEXT("the Dream tab is in no window"));
				return false;
			}
			FString Path;
			FString Error;
			if (!DreamVisualCaptureSpec::SaveWidgetPng(Window.ToSharedRef(), TEXT("09-editor-window"), Path, Error))
			{
				AddError(Error);
				return false;
			}
			Written.Add(Path);
			// The Level Editor's main window holds the status bar with WS-07, whether the tab is docked there or floats: a floating
			// tab adds the main frame as 10-main-frame; a docked one is in 09 already, and an older 10 from a floating run goes.
			const TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
			if (Root.IsValid() && Root != Window)
			{
				if (!DreamVisualCaptureSpec::SaveWidgetPng(Root.ToSharedRef(), TEXT("10-main-frame"), Path, Error))
				{
					AddError(Error);
					return false;
				}
				Written.Add(Path);
			}
			else
			{
				const FString Stale = FPaths::Combine(DreamVisualCaptureSpec::ScreensDir(), TEXT("10-main-frame.png"));
				if (IFileManager::Get().FileExists(*Stale) && IFileManager::Get().Delete(*Stale, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true))
				{
					AddInfo(TEXT("the tab is docked in the main frame (09 shows it): removed a 10-main-frame.png left by a floating-tab run"));
				}
			}
			return true;
		});

		// (chrome) PO-04 point 3: a light theme, the output area back on top.
		Steps.Do(TEXT("output area on top again"), [this, DemoId]()
		{
			UDreamUserSettings::GetMutable()->OutputPosition = EDreamOutputPosition::Top;   // no save with the pinned values
			Tab->SetOutputPosition(EDreamOutputPosition::Top);
			Tab->ActivateBuffer(DemoId);
			return true;
		});
		AddSetTheme(Steps, TEXT("GitHub Light"));
		Steps.Do(TEXT("a light theme says so"), [this]()
		{
			return TestTrue(TEXT("GitHub Light is a light theme"), FDreamChrome::IsLightTheme());
		});
		AddCaptureTab(Steps, TEXT("11-light-tab-top-idle"));
		Steps.Do(TEXT("light: the history is OutputBackground (review r6)"), [this]()
		{
			const TSharedPtr<SDreamOutputView> History = Tab->GetOutputArea()->GetOutputView();
			if (!TestTrue(TEXT("the capture has pixels and the history"), LastPixels.IsValid() && History.IsValid()))
			{
				return false;
			}
			const FColor Expected = DreamVisualCaptureSpec::ThemeColor(EDreamThemeSlot::OutputBackground);
			const FColor Seen = LastPixels.ModeOver(*History, FVector2f(0.2f, 0.2f), FVector2f(0.95f, 0.9f));
			TestTrue(*FString::Printf(TEXT("the history is GitHub Light's OutputBackground %s (%s)"), *DreamVisualCaptureSpec::Hex(Expected),
				*DreamVisualCaptureSpec::Hex(Seen)), DreamVisualCaptureSpec::Near(Seen, Expected));
			return true;
		});
		Steps.Do(TEXT("light: every View part on"), [this]()
		{
			DreamVisualCaptureSpec::SetAllViewParts(true);
			return true;
		});
		AddCaptureTab(Steps, TEXT("12-light-tab-top-all-parts"));
		Steps.Do(TEXT("light: the REPL prompt strip's rules are ChromeBorder (review r6)"), [this]()
		{
			const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea()->GetReplStrip();
			if (!TestTrue(TEXT("the capture has pixels and the prompt shows"), LastPixels.IsValid() && Strip.IsValid() && Tab->GetOutputArea()->IsPromptShown()))
			{
				return false;
			}
			const FColor Border = DreamVisualCaptureSpec::ThemeColor(EDreamThemeSlot::ChromeBorder);
			const FGeometry& G = Strip->GetPaintSpaceGeometry();
			const FVector2D TopLeft(G.GetAbsolutePositionAtCoordinates(FVector2f(0.f, 0.f)));
			const FVector2D BottomRight(G.GetAbsolutePositionAtCoordinates(FVector2f(1.f, 1.f)));
			const int32 X = LastPixels.Column((TopLeft.X + BottomRight.X) * 0.5);
			// The strip's own device rows: its first two from the top, its last two from the bottom (never a row outside it).
			// At DPI 1 the rule is exactly the first and the last row; off DPI 1 it may straddle a row edge and blend.
			const int32 FirstRow = LastPixels.FirstRowBelow(TopLeft.Y);
			const int32 LastRow = LastPixels.FirstRowBelow(BottomRight.Y) - 1;
			const bool bDpi1 = FMath::IsNearlyEqual(LastPixels.Scale, 1.f);
			auto RuleSeen = [this, &Border, X](std::initializer_list<int32> Rows, bool& bOutFound) -> FString
			{
				FString Seen;
				bOutFound = false;
				for (const int32 Row : Rows)
				{
					FColor Pixel;
					if (LastPixels.AtPixel(X, Row, Pixel))
					{
						bOutFound = bOutFound || DreamVisualCaptureSpec::Near(Pixel, Border);
						Seen += FString::Printf(TEXT("row %d %s "), Row, *DreamVisualCaptureSpec::Hex(Pixel));
					}
				}
				return Seen.IsEmpty() ? FString(TEXT("outside the capture")) : Seen;
			};
			if (!TestTrue(TEXT("the strip is at least four device rows tall"), LastRow - FirstRow >= 3))
			{
				return true;
			}
			bool bTop = false;
			bool bBottom = false;
			const FString TopSeen = bDpi1 ? RuleSeen({ FirstRow }, bTop) : RuleSeen({ FirstRow, FirstRow + 1 }, bTop);
			const FString BottomSeen = bDpi1 ? RuleSeen({ LastRow }, bBottom) : RuleSeen({ LastRow, LastRow - 1 }, bBottom);
			const FString TopWhat = FString::Printf(TEXT("the prompt strip's top rule is ChromeBorder %s (%s; DPI %.2f)"),
				*DreamVisualCaptureSpec::Hex(Border), *TopSeen, LastPixels.Scale);
			const FString BottomWhat = FString::Printf(TEXT("the prompt strip's bottom rule is ChromeBorder %s (%s; DPI %.2f)"),
				*DreamVisualCaptureSpec::Hex(Border), *BottomSeen, LastPixels.Scale);
			if (bDpi1)
			{
				TestTrue(*TopWhat, bTop);
				TestTrue(*BottomWhat, bBottom);
			}
			else
			{
				// A blended 1-unit rule is not a failure of the theme: said as a warning with the colours seen.
				if (!bTop)
				{
					AddWarning(TopWhat);
				}
				if (!bBottom)
				{
					AddWarning(BottomWhat);
				}
			}
			return true;
		});
		Steps.Do(TEXT("light: back to the Maya-minimal layout"), [this]()
		{
			DreamVisualCaptureSpec::SetAllViewParts(false);
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				Code->FocusEditor();
			}
			Tab->GetOutputArea()->EndPromptReveal(/*bEvenIfTyped*/ true);
			return true;
		});
		AddLongRun(Steps, LongLight, LongId, TEXT("13-light-runwindow-top"), true);

		// (sidebar) PO-05: the side bar with a root file active (its row highlighted, its outline in Structure), Scratches
		// below the root; Darcula, then GitHub Light.
		Steps.Do(TEXT("side bar: open structure_demo.py from the root, expand tools"), [this, Saved]()
		{
			const FString Path = Saved->SideBarDir / TEXT("scripts") / TEXT("structure_demo.py");
			if (!TestTrue(TEXT("structure_demo.py opens from the root"), Tab->OpenLocation(Path, 0, 0)))
			{
				return false;
			}
			if (const TSharedPtr<FDreamBuffer> Buffer = Tab->GetActiveBuffer())
			{
				BufferIds.Add(Buffer->GetId());
			}
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				Code->SetCaret(11, 8);   // inside SideBarDemo.area: Structure highlights that method (R-p1-9)
			}
			const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar();
			if (Side.IsValid())
			{
				Side->GetProjectTree()->SetExpandedForTest(Saved->SideBarDir / TEXT("scripts") / TEXT("tools"), true);
			}
			return Side.IsValid();
		});
		Steps.Poll(TEXT("side bar: the tree settles"), [this]()
		{
			const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar();
			return Side.IsValid() && Side->GetProjectTree()->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		AddSetTheme(Steps, TEXT("Darcula"));
		// The Structure view builds its outline 300 ms after a change and follows the caret, throttled: wait for its rows
		// (review p1 nit: frames were shorter than the debounce).
		Steps.Poll(TEXT("side bar: Structure shows the outline with the caret's method highlighted"), [this]()
		{
			const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar();
			const TSharedPtr<SDreamStructureView> View = Side.IsValid() ? StaticCastSharedPtr<SDreamStructureView>(Side->GetStructureView()) : nullptr;
			if (!View.IsValid())
			{
				return DreamTest::EStep::Wait;
			}
			for (const SDreamStructureView::FRowForTest& Row : View->GetRowsForTest())
			{
				if (Row.Name == TEXT("area") && Row.bHighlighted)
				{
					return DreamTest::EStep::Next;
				}
			}
			return DreamTest::EStep::Wait;
		}, 5.0);
		Steps.WaitFrames(3);
		AddCaptureTab(Steps, TEXT("14-sidebar-darcula"));
		Steps.Do(TEXT("side bar (Darcula): a root, Scratches and the highlight; the panel colours"), [this, Saved]()
		{
			AddSideBarChecks(Saved->SideBarDir, TEXT("Darcula"));
			return true;
		});
		AddSetTheme(Steps, TEXT("GitHub Light"));
		AddCaptureTab(Steps, TEXT("15-sidebar-light"));
		Steps.Do(TEXT("side bar (GitHub Light): the panel colours"), [this, Saved]()
		{
			AddSideBarChecks(Saved->SideBarDir, TEXT("GitHub Light"));
			return true;
		});

		Steps.Do(TEXT("list the captures"), [this]()
		{
			for (const FString& Path : Written)
			{
				TestTrue(*FString::Printf(TEXT("%s exists and is not empty"), *Path), IFileManager::Get().FileSize(*Path) > 0);
			}
			AddInfo(FString::Printf(TEXT("%d captures in %s"), Written.Num(), *DreamVisualCaptureSpec::ScreensDir()));
			return true;
		});

		Steps.Finally(TEXT("restore the layout and the presentation"), [this, Saved, LongTop, LongBottom, LongLight, Log]()
		{
			for (const TSharedRef<DreamVisualCaptureSpec::FLongRun>& Long : { LongTop, LongBottom, LongLight })
			{
				if (Long->PostTickHandle.IsValid() && FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().OnPostTick().Remove(Long->PostTickHandle);
					Long->PostTickHandle.Reset();
				}
			}
			if (Saved->bSaved)
			{
				// Only what changed is written back, key by key (never SaveConfig: the whole section would write another spec's
				// in-memory pins). The run settings and the position were pinned in memory only; a value that differs is put
				// back and its key written, which leaves the user's value on disk either way.
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				if (Settings->bRunPump != Saved->bRunPump)
				{
					Settings->bRunPump = Saved->bRunPump;
					UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, bRunPump));
				}
				if (Settings->RunPresentation != Saved->Presentation)
				{
					Settings->RunPresentation = Saved->Presentation;
					UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, RunPresentation));
				}
				if (Settings->OutputPosition != Saved->Position)
				{
					Settings->OutputPosition = Saved->Position;
					UDreamUserSettings::SaveKey(GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition));
				}
				if (Tab.IsValid())
				{
					Tab->SetOutputPosition(Saved->Position);    // the layout only: the setting already matches
				}
				// (chrome) The View toggles, the theme and the echo visibility as they were (the setters save their own keys).
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(DreamVisualCaptureSpec::ViewSettings); ++Index)
				{
					UDreamUserSettings::SetViewSetting(DreamVisualCaptureSpec::ViewSettings[Index], Saved->bViewValues[Index]);
				}
				// The ThemeId setting (the captures never saved it; written back only if something changed it), then the theme
				// shown at the start.
				if (Settings->ThemeId != Saved->ThemeId)
				{
					UDreamUserSettings::SetThemeId(Saved->ThemeId);
				}
				const FString ShowTheme = FDreamThemes::Find(Saved->ThemeId).IsValid() ? Saved->ThemeId : Saved->CurrentThemeId;
				if (!ShowTheme.IsEmpty() && FDreamThemes::GetCurrentId() != ShowTheme)
				{
					FDreamThemes::SetCurrent(ShowTheme, /*bPersist*/ false);
				}
				FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
				FDreamOutputVisibility Visibility = Store.GetVisibility();
				if (Visibility.bEcho != Saved->bEchoVisible)
				{
					Visibility.bEcho = Saved->bEchoVisible;
					Store.SetVisibility(Visibility);
				}
				// (sidebar) The side bar's View keys (the setter writes only a change), the roots in memory (never written), the
				// scratch folder override, and the capture folders.
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, Saved->bProjectPanel);
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, Saved->bStructurePanel);
				FDreamProjectFiles::SetRootsForTest(nullptr);
				FDreamProjectFiles::SetPluginFoldersForTest(nullptr);
				FDreamProjectFiles::SetDefaultRootForTest(FString());
				// The expansions the captures changed go back (the session save would have written them).
				UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::GetMutable();
				UObject::FSaveConfigContext Keys;
				if (Session->ExpandedFolders != Saved->ExpandedFolders)
				{
					Session->ExpandedFolders = Saved->ExpandedFolders;
					Keys.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, ExpandedFolders));
				}
				if (Session->CollapsedRoots != Saved->CollapsedRoots)
				{
					Session->CollapsedRoots = Saved->CollapsedRoots;
					Keys.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, CollapsedRoots));
				}
				if (Keys.PropertyNames.Num() > 0)
				{
					Session->SaveConfig(Keys);
				}
				DreamScratch::SetFolderForTest(Saved->ScratchFolderBefore);
				if (Tab.IsValid())
				{
					if (const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar())
					{
						Side->GetProjectTree()->RefreshRoots();
					}
				}
				for (const FGuid& Id : BufferIds)
				{
					DreamTest::CloseBuffer(Id);   // the root file's tab, before its folder goes
				}
				if (!Saved->SideBarDir.IsEmpty())
				{
					IFileManager::Get().DeleteDirectory(*Saved->SideBarDir, false, true);   // the spec's own capture folders
				}
			}
		});
		Steps.Run(Done);
	});
}
