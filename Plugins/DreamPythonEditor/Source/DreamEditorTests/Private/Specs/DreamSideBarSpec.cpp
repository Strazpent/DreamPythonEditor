// SPDX-License-Identifier: Apache-2.0
// (sidebar, PO-05 sections 1 and 2) Two attended specs.
// Dream.Shell.SideBar: the defaults (View > Project and View > Structure on, 240 px, 60 %); the side bar in the left slot of
// the outer splitter, full height beside the output and code areas and outside their splitter (an output move leaves it
// where it is); each View toggle shows its section and the other fills the side bar, both off collapse it with no strip
// and no gap; the width and the split saved per project (the ini on disk) and read back by a new side bar; Focus Project
// and Focus Structure, and Esc back to the code view; a press on either grip of the 1-px rule starts the drag; a live theme
// switch repaints the side bar (pixels).
// Dream.Shell.ProjectTree: roots (absolute, relative, missing: "(not found)", never created; from the settings too), the
// project's Content/Python first whenever it exists, with or without roots, once, not removable (p3 ruling; a stand-in
// folder, never the project's own), else the "No project roots" line, Scratches last; a missing root and a missing scratch folder that appear
// later fill in (R-p1-7); listing order and hiding; the listing cap; no listing on the game thread (counted); a routed
// double-click opens a file through the open route and focuses an open one; the active-file highlight; rename inline with
// the open tab following (a file, a file inside a renamed folder), and Enter on a bad name keeping the editor (p1 M2);
// delete after the confirm through the Recycle Bin seam (R-p1-5), refused without a bin, and an open tab's "deleted on
// disk" state; the real Recycle Bin only with -DreamRealRecycleTest; the DirectoryWatcher refresh, a hidden temp file
// ignored (p1 m10); Add Root / Remove Root; New Python File / New Folder; the context menus, Refresh, and no Rename or
// Delete on a root, the scratch folder or a folder holding one (p1 m9); review p2: the inline editor closing on a focus
// change made by code, on Esc in the tree and on hiding the section (m1); a new scratch file listed at once in a scratch
// folder that is not watched (m2); four listings at once with a queue, two network ones apart from them (p3 m1), and the
// backed-off looks at a missing network root (m3); a tree made hidden lists nothing, a reload of the roots' config reaches the tree, and Add Root / Remove Root send
// the property-changed notification an open preferences page needs (nits); PO-05-A1: the plugins' Python folders under
// Project Plugins and Engine, a mount point registered and removed, nothing changed under the engine, a user root equal to
// one shown once, and the live toggle from View, Editor Preferences and the tree's menu in step (no ini written). The
// plugin folders are off (a test override) in every other case, so their rows stay as before.
// The roots are a test override (FDreamProjectFiles::SetRootsForTest): no spec writes the user's Project Roots (p1 m7); the
// one check of roots from the settings sets them in memory and puts them back. Every spec saves the settings it changes and
// writes back only the keys that changed (A214): the two View keys, CollapsedRoots, ExpandedFolders, SideBarWidth and
// SideBarProjectFraction, the shown theme and the scratch folder override. The files live in
// DreamHost/Saved/DreamTests/<case>-<suffix>, removed afterwards; the user's real scratch folder and Recycle Bin are never
// touched in a routine pass.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Buffers/DreamScratch.h"
#include "CodeView/SDreamCodeView.h"
#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Run/DreamRunService.h"   // (editing) R-q1-1: F5 in the tree submits no run
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamProjectTreeSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamProjectTree.h"
#include "Shell/SDreamSideBar.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamChrome.h"
#include "Shell/DreamProjectFiles.h"
#include "Shell/DreamTheme.h"
#include "Types/ISlateMetaData.h"
#include "UObject/Package.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamSideBarSpec
{
	using FRow = SDreamProjectTree::FRowForTest;
	using EKind = FDreamProjectNode::EKind;

	const TCHAR* const ProjectSection = TEXT("/Script/DreamEditor.DreamPerProjectUserSettings");

	/** Everything these specs may change, as they found it (A214: restored key by key, only what changed). */
	struct FSaved
	{
		bool bSaved = false;
		bool bProject = true;
		bool bStructure = true;
		TArray<FString> Roots;              // UDreamProjectTreeSettings::ProjectRoots' paths (in memory; never written by a spec)
		TArray<FString> CollapsedRoots;
		TArray<FString> ExpandedFolders;
		float Width = 240.f;
		float Fraction = 0.6f;
		FString ShownTheme;
		EDreamOutputPosition Position = EDreamOutputPosition::Top;
	};

	TArray<FString> RootPaths()
	{
		TArray<FString> Out;
		for (const FDirectoryPath& Root : UDreamProjectTreeSettings::Get()->ProjectRoots)
		{
			Out.Add(Root.Path);
		}
		return Out;
	}

	/** The settings' roots in memory only (the one check of roots from the settings; the restore puts them back). */
	void SetRoots(const TArray<FString>& Paths)
	{
		UDreamProjectTreeSettings* Settings = UDreamProjectTreeSettings::GetMutable();
		Settings->ProjectRoots.Reset();
		for (const FString& Path : Paths)
		{
			Settings->ProjectRoots.AddDefaulted_GetRef().Path = Path;
		}
	}

	void Capture(FSaved& Saved)
	{
		const UDreamUserSettings* User = UDreamUserSettings::Get();
		const UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::Get();
		Saved.bProject = User->bShowProjectPanel;
		Saved.bStructure = User->bShowStructurePanel;
		Saved.Roots = RootPaths();
		Saved.CollapsedRoots = Session->CollapsedRoots;
		Saved.ExpandedFolders = Session->ExpandedFolders;
		Saved.Width = Session->SideBarWidth;
		Saved.Fraction = Session->SideBarProjectFraction;
		Saved.ShownTheme = FDreamThemes::GetCurrentId();
		Saved.Position = User->OutputPosition;
		Saved.bSaved = true;
	}

	/** Puts back what changed: the View keys through their setter, the per-project keys that differ (memory, and the ini key
	 *  by key), the theme shown, the output position; the side bar and the tree follow. */
	void Restore(FSaved& Saved, const TSharedPtr<SDreamTab>& Tab)
	{
		if (!Saved.bSaved || !UObjectInitialized())
		{
			return;
		}
		Saved.bSaved = false;
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, Saved.bProject);       // writes only a change
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, Saved.bStructure);
		UDreamPerProjectUserSettings* Session = UDreamPerProjectUserSettings::GetMutable();
		const bool bRoots = FDreamProjectFiles::HasRootsForTest() || RootPaths() != Saved.Roots;
		FDreamProjectFiles::SetRootsForTest(nullptr);
		if (RootPaths() != Saved.Roots)
		{
			SetRoots(Saved.Roots);   // memory only: nothing wrote them, so the ini still holds these
		}
		UObject::FSaveConfigContext Context;
		if (Session->CollapsedRoots != Saved.CollapsedRoots)
		{
			Session->CollapsedRoots = Saved.CollapsedRoots;
			Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, CollapsedRoots));
		}
		if (Session->ExpandedFolders != Saved.ExpandedFolders)
		{
			Session->ExpandedFolders = Saved.ExpandedFolders;
			Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, ExpandedFolders));
		}
		if (!FMath::IsNearlyEqual(Session->SideBarWidth, Saved.Width))
		{
			Session->SideBarWidth = Saved.Width;
			Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, SideBarWidth));
		}
		if (!FMath::IsNearlyEqual(Session->SideBarProjectFraction, Saved.Fraction))
		{
			Session->SideBarProjectFraction = Saved.Fraction;
			Context.PropertyNames.Add(GET_MEMBER_NAME_CHECKED(UDreamPerProjectUserSettings, SideBarProjectFraction));
		}
		if (Context.PropertyNames.Num() > 0)
		{
			Session->SaveConfig(Context);
		}
		if (!Saved.ShownTheme.IsEmpty() && FDreamThemes::GetCurrentId() != Saved.ShownTheme)
		{
			FDreamThemes::SetCurrent(Saved.ShownTheme, /*bPersist*/ false);
		}
		if (Tab.IsValid())
		{
			Tab->SetOutputPosition(Saved.Position);   // the setting is written only when it differs
			if (const TSharedPtr<SDreamSideBar> Side = Tab->GetSideBar())
			{
				Side->SetWidth(Saved.Width);
				Side->SetProjectFraction(Saved.Fraction);
				if (bRoots)
				{
					Side->GetProjectTree()->RefreshRoots();
				}
			}
		}
	}

	FString Key(const FString& Path)
	{
		return FDreamProjectFiles::PathKey(Path);
	}

	/** A fresh folder for one case: DreamHost/Saved/DreamTests/<Purpose>-<suffix>, normalized. */
	FString MakeTempRoot(const FString& Purpose)
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamTests") / FString::Printf(TEXT("%s-%s"), *Purpose, *DreamTest::UniqueSuffix()));
		FPaths::NormalizeDirectoryName(Dir);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	bool WriteText(const FString& Path, const FString& Text = FString())
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Removes a case's own temporary folder (the tests' data, made by the case). */
	void RemoveTempRoot(const FString& Dir)
	{
		if (!Dir.IsEmpty() && Dir.Contains(TEXT("/DreamTests/")))
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}
	}

	const FRow* FindRow(const TArray<FRow>& Rows, const FString& Path)
	{
		const FString K = Key(Path);
		return Rows.FindByPredicate([&K](const FRow& Row) { return !Row.Path.IsEmpty() && Key(Row.Path) == K; });
	}

	TArray<FString> LabelsAt(const TArray<FRow>& Rows, int32 Depth, const FString& UnderPath = FString())
	{
		TArray<FString> Out;
		for (const FRow& Row : Rows)
		{
			if (Row.Depth == Depth && (UnderPath.IsEmpty() || (!Row.Path.IsEmpty() && FDreamProjectFiles::IsUnder(Row.Path, UnderPath))))
			{
				Out.Add(Row.Label);
			}
		}
		return Out;
	}

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

	TArray<TSharedRef<SWidget>> FindTagged(const TSharedRef<SWidget>& Root, FName Tag)
	{
		TArray<TSharedRef<SWidget>> Found;
		Collect(Root, [Tag](const TSharedRef<SWidget>& W)
		{
			const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
			return Meta.IsValid() && Meta->Tag == Tag;
		}, Found);
		return Found;
	}

	FSlateRect RectOf(const SWidget& Widget)
	{
		const FGeometry& G = Widget.GetPaintSpaceGeometry();
		const FVector2D TopLeft(G.GetAbsolutePositionAtCoordinates(FVector2f(0.f, 0.f)));
		const FVector2D BottomRight(G.GetAbsolutePositionAtCoordinates(FVector2f(1.f, 1.f)));
		return FSlateRect(TopLeft, BottomRight);
	}

	/** A double-click at Widget's centre routed along its path, as Slate routes a platform double-click (down, up, double). */
	bool DoubleClick(const TSharedRef<SWidget>& Widget)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		const FVector2D At(Widget->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
		const uint32 User = App.GetUserIndexForMouse();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
		App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		App.RoutePointerUpEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		FWidgetPath Again;
		if (!DreamTest::MakePointerPath(Widget, Again))
		{
			return false;
		}
		App.RoutePointerDoubleClickEvent(Again, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		App.RoutePointerUpEvent(Again, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		return true;
	}

	/** A key as the platform delivers it to the focused widget (down, up), with Ctrl when asked. */
	bool PressKey(const FKey& Key, bool bCtrl, uint32 Character = 0)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods(false, false, bCtrl, false, false, false, false, false, false);
		const bool bHandled = App.ProcessKeyDownEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, Character, Character));
		App.ProcessKeyUpEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, Character, Character));
		return bHandled;
	}

	/** Typed text into the focused widget, one character event per character. */
	void TypeText(const FString& Text)
	{
		FSlateApplication& App = FSlateApplication::Get();
		for (const TCHAR Char : Text)
		{
			App.ProcessKeyCharEvent(FCharacterEvent(Char, FModifierKeysState(), App.GetUserIndexForKeyboard(), false));
		}
	}

	/** Esc as the platform delivers it to the focused widget. */
	bool PressEscape()
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FKeyEvent Down(EKeys::Escape, FModifierKeysState(), App.GetUserIndexForKeyboard(), false, 27, 27);
		const bool bHandled = App.ProcessKeyDownEvent(Down);
		App.ProcessKeyUpEvent(FKeyEvent(EKeys::Escape, FModifierKeysState(), App.GetUserIndexForKeyboard(), false, 27, 27));
		return bHandled;
	}

	bool FocusIsInside(const TSharedPtr<SWidget>& Widget)
	{
		const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
		for (TSharedPtr<SWidget> W = Focused; W.IsValid(); W = W->GetParentWidget())
		{
			if (W == Widget)
			{
				return true;
			}
		}
		return false;
	}

	FColor ThemeColor(EDreamThemeSlot Slot)
	{
		return FDreamThemes::LinearColor(Slot).ToFColorSRGB();
	}

	bool Near(const FColor& A, const FColor& B, int32 Tolerance = 3)
	{
		return FMath::Abs(int32(A.R) - int32(B.R)) <= Tolerance && FMath::Abs(int32(A.G) - int32(B.G)) <= Tolerance
			&& FMath::Abs(int32(A.B) - int32(B.B)) <= Tolerance;
	}

	FString Hex(const FColor& Color)
	{
		return FString::Printf(TEXT("#%02X%02X%02X"), Color.R, Color.G, Color.B);
	}

	/** The colour most points of a 9 x 9 grid over the fractions [Min, Max] of Widget's rect have, read back from its
	 *  window (FSlateApplication::TakeScreenshot draws the window and reads the widget's rect). False with a reason. */
	bool ModeColor(const TSharedRef<SWidget>& Widget, const FVector2f& Min, const FVector2f& Max, FColor& OutColor, FString& OutError)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = App.FindWidgetWindow(Widget);
		if (!Window.IsValid() || Window->IsWindowMinimized() || !Window->IsVisible())
		{
			OutError = TEXT("the widget is in no visible window");
			return false;
		}
		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!App.TakeScreenshot(Widget, Pixels, Size) || Size.X <= 0 || Size.Y <= 0 || Pixels.Num() < Size.X * Size.Y)
		{
			OutError = TEXT("TakeScreenshot returned no pixels");
			return false;
		}
		TMap<uint32, int32> Counts;
		for (int32 IY = 0; IY < 9; ++IY)
		{
			for (int32 IX = 0; IX < 9; ++IX)
			{
				const int32 X = FMath::Clamp(FMath::FloorToInt32(FMath::Lerp(Min.X, Max.X, IX / 8.f) * (Size.X - 1)), 0, Size.X - 1);
				const int32 Y = FMath::Clamp(FMath::FloorToInt32(FMath::Lerp(Min.Y, Max.Y, IY / 8.f) * (Size.Y - 1)), 0, Size.Y - 1);
				FColor Pixel = Pixels[Y * Size.X + X];
				Pixel.A = 255;
				++Counts.FindOrAdd(Pixel.ToPackedARGB());
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
		OutColor = FColor(Best);
		return true;
	}

	/** A press on Widget routed along its path; the mouse captor afterwards (the splitter, when the press starts a drag). */
	TSharedPtr<SWidget> PressAndGetCaptor(const TSharedRef<SWidget>& Widget)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return nullptr;
		}
		const FVector2D At(Widget->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
		const uint32 User = App.GetUserIndexForMouse();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()), false);
		App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		const TSharedPtr<FSlateUser> CursorUser = App.GetCursorUser();
		const TSharedPtr<SWidget> Captor = CursorUser.IsValid() ? CursorUser->GetCursorCaptor() : nullptr;
		App.RoutePointerUpEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		App.ReleaseAllPointerCapture();
		return Captor;
	}

	/** A settings object built by its C++ constructor alone (no archetype copy, no ini), as Dream.Shell.Chrome builds one. */
	template <typename TSettings>
	const TSettings* Fresh(const TCHAR* Name)
	{
		UClass* Class = TSettings::StaticClass();
		UObject* Raw = StaticAllocateObject(Class, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), Class, FName(Name)), RF_Transient);
		(*Class->ClassConstructor)(FObjectInitializer(Raw, nullptr, EObjectInitializerOptions::None));
		return CastChecked<TSettings>(Raw);
	}
}

// =====================================================================================================================
// Dream.Shell.SideBar
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamShellSideBarSpec, "Dream.Shell.SideBar", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	DreamSideBarSpec::FSaved Saved;
	TSharedPtr<SDreamSideBar> SideBar() const { return Tab.IsValid() ? Tab->GetSideBar() : nullptr; }
END_DEFINE_SPEC(FDreamShellSideBarSpec)

void FDreamShellSideBarSpec::Define()
{
	using namespace DreamSideBarSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Capture(Saved);
		if (Tab.IsValid())
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
				TEXT("class SideBarProbe:\n    def method(self):\n        return 1\n\ndef side_bar_probe():\n    pass\n"), TEXT("SideBar"));
			BufferId = Buffer.IsValid() ? Buffer->GetId() : FGuid();
		}
	});

	AfterEach([this]()
	{
		Restore(Saved, Tab);
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		Tab.Reset();
	});

	It("declares the side bar defaults: Project and Structure on, 240 px wide, the Project section 60 percent, no roots (PO-05)", [this]()
	{
		const UDreamUserSettings* User = Fresh<UDreamUserSettings>(TEXT("DreamT4FreshSideBarUser"));
		TestTrue(TEXT("View > Project is on by default (bShowProjectPanel)"), User->bShowProjectPanel);
		TestTrue(TEXT("View > Structure is on by default (bShowStructurePanel)"), User->bShowStructurePanel);
		TestTrue(TEXT("GetViewSetting(ProjectPanel) reads it"), User->GetViewSetting(EDreamViewSetting::ProjectPanel));
		TestTrue(TEXT("GetViewSetting(StructurePanel) reads it"), User->GetViewSetting(EDreamViewSetting::StructurePanel));
		TestFalse(TEXT("the PO-04 parts stay off (the side bar is the only exception)"), User->bShowStatusLine || User->bShowReplTab);
		const UDreamPerProjectUserSettings* Session = Fresh<UDreamPerProjectUserSettings>(TEXT("DreamT4FreshSideBarSession"));
		TestEqual(TEXT("the side bar is 240 px wide by default"), Session->SideBarWidth, SDreamSideBar::DefaultWidth);
		TestEqual(TEXT("the Project section takes 60 percent by default"), Session->SideBarProjectFraction, SDreamSideBar::DefaultProjectFraction);
		TestEqual(TEXT("the minimum width is 160 px"), SDreamSideBar::MinWidth, 160.f);
		// R-p1-10: the roots live alone in their own settings class, the only one on Plugins > Dream (this project); the
		// session state stays unregistered (so that page's Reset and Set as Default touch nothing else).
		const UDreamProjectTreeSettings* TreeSettings = Fresh<UDreamProjectTreeSettings>(TEXT("DreamT4FreshSideBarRoots"));
		TestEqual(TEXT("no project roots are set by default"), TreeSettings->ProjectRoots.Num(), 0);
		TestTrue(TEXT("the roots' class registers the Editor Preferences section"), TreeSettings->SupportsAutoRegistration());
		TestEqual(TEXT("as Plugins > DreamProject"), TreeSettings->GetSectionName(), FName(TEXT("DreamProject")));
		TestFalse(TEXT("the per-project session settings do not register"), Session->SupportsAutoRegistration());
		int32 Editable = 0;
		for (TFieldIterator<FProperty> It(UDreamProjectTreeSettings::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Editable += It->HasAllPropertyFlags(CPF_Edit | CPF_Config) ? 1 : 0;
		}
		TestEqual(TEXT("Project Roots and Add plugin Python folders are the page's editable properties"), Editable, 2);
		TestTrue(TEXT("plugin Python folders are added by default (PO-05-A1)"), TreeSettings->bAddPluginPythonFolders);
		TestEqual(TEXT("the roots' class keeps its own ini section"), UDreamProjectTreeSettings::StaticClass()->GetPathName(), FString(TEXT("/Script/DreamEditor.DreamProjectTreeSettings")));
	});

	LatentIt("puts the side bar left of the output and code areas, full height, outside their splitter (WS-02)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<FSlateRect> Before = MakeShared<FSlateRect>();
		Steps.Do(TEXT("both sections on, output on top"), [this]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar and a code view"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			Tab->SetOutputPosition(EDreamOutputPosition::Top);
			Tab->ActivateBuffer(BufferId);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the outer splitter holds the side bar, then the centre column"), [this, Before]()
		{
			const TSharedPtr<SSplitter> Outer = Tab->GetOuterSplitter();
			const TSharedPtr<SDreamSideBar> Side = SideBar();
			if (!TestTrue(TEXT("the tab has an outer splitter"), Outer.IsValid()))
			{
				return false;
			}
			TestTrue(TEXT("the outer splitter is horizontal"), Outer->GetOrientation() == Orient_Horizontal);
			TestEqual(TEXT("it has two slots"), Outer->GetChildren()->Num(), 2);
			TestTrue(TEXT("slot 0 is the side bar"), &Outer->GetChildren()->GetChildAt(0).Get() == Side.Get());
			// The centre splitter (output and code) is inside slot 1 and the side bar is not inside it.
			TSharedPtr<SWidget> Centre;
			for (TSharedPtr<SWidget> W = Tab->GetOutputArea(); W.IsValid(); W = W->GetParentWidget())
			{
				if (W->GetType() == FName(TEXT("SSplitter")) && W != Outer)
				{
					Centre = W;
					break;
				}
			}
			bool bCentreInSlot1 = false;
			for (TSharedPtr<SWidget> W = Centre; W.IsValid(); W = W->GetParentWidget())
			{
				bCentreInSlot1 = bCentreInSlot1 || W == Outer->GetChildren()->GetChildAt(1);
			}
			TestTrue(TEXT("the output/code splitter is in slot 1"), Centre.IsValid() && bCentreInSlot1);
			bool bSideInCentre = false;
			for (TSharedPtr<SWidget> W = Side; W.IsValid(); W = W->GetParentWidget())
			{
				bSideInCentre = bSideInCentre || W == Centre;
			}
			TestFalse(TEXT("the side bar is outside the output/code splitter"), bSideInCentre);
			// Painted: left of both areas, as tall as the centre column, as wide as its width.
			const FSlateRect SideRect = RectOf(*Side);
			const FSlateRect CentreRect = RectOf(Outer->GetChildren()->GetChildAt(1).Get());
			const FSlateRect OutputRect = RectOf(*Tab->GetOutputArea());
			const FSlateRect CodeRect = RectOf(*Tab->GetActiveCodeView());
			const float Scale = Side->GetPaintSpaceGeometry().Scale;
			TestTrue(*FString::Printf(TEXT("the side bar (right %.0f) is left of the output area (left %.0f)"), SideRect.Right, OutputRect.Left), SideRect.Right <= OutputRect.Left + 1.f);
			TestTrue(*FString::Printf(TEXT("and of the code area (left %.0f)"), CodeRect.Left), SideRect.Right <= CodeRect.Left + 1.f);
			TestEqual(TEXT("full height: its top is the centre column's"), SideRect.Top, CentreRect.Top, 1.5f);
			TestEqual(TEXT("full height: its bottom is the centre column's"), SideRect.Bottom, CentreRect.Bottom, 1.5f);
			TestEqual(TEXT("it is as wide as its width"), (SideRect.Right - SideRect.Left) / Scale, Side->GetWidth(), 1.5f);
			TestTrue(TEXT("Project above Structure"), RectOf(*Side->GetProjectSection()).Top < RectOf(*Side->GetStructureSection()).Top);
			// The section titles: small muted text, "Project" and "Structure".
			for (const TPair<const TCHAR*, const TCHAR*>& Title : { TPair<const TCHAR*, const TCHAR*>(TEXT("DreamSideBar.ProjectTitle"), TEXT("Project")),
				TPair<const TCHAR*, const TCHAR*>(TEXT("DreamSideBar.StructureTitle"), TEXT("Structure")) })
			{
				const TArray<TSharedRef<SWidget>> Found = FindTagged(Side.ToSharedRef(), FName(Title.Key));
				if (TestEqual(*FString::Printf(TEXT("one %s title"), Title.Value), Found.Num(), 1))
				{
					TestEqual(*FString::Printf(TEXT("it reads %s"), Title.Value), StaticCastSharedRef<STextBlock>(Found[0])->GetText().ToString(), FString(Title.Value));
				}
			}
			TestTrue(TEXT("the Structure section holds the structure view"),
				Side->GetStructureView().IsValid() && Side->GetStructureView()->GetType() == FName(TEXT("SDreamStructureView")));
			*Before = SideRect;
			Tab->SetOutputPosition(EDreamOutputPosition::Bottom);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("moving the output to the bottom leaves the side bar where it is"), [this, Before]()
		{
			const FSlateRect After = RectOf(*SideBar());
			TestEqual(TEXT("its left edge"), After.Left, Before->Left, 1.f);
			TestEqual(TEXT("its top"), After.Top, Before->Top, 1.f);
			TestEqual(TEXT("its width"), After.Right - After.Left, Before->Right - Before->Left, 1.f);
			TestEqual(TEXT("its bottom"), After.Bottom, Before->Bottom, 1.f);
			TestTrue(TEXT("the output area is below the code area now"), RectOf(*Tab->GetOutputArea()).Top > RectOf(*Tab->GetActiveCodeView()).Top);
			return true;
		});
		Steps.Finally(TEXT("restore"), [this]() { DreamSideBarSpec::Restore(Saved, Tab); });
		Steps.Run(Done);
	});

	LatentIt("shows each section with its View toggle, fills the side bar with the other, and collapses it with both off", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<float> CentreLeft = MakeShared<float>(0.f);
		Steps.Do(TEXT("both on"), [this]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			// View menu: the two toggles and the two focus commands.
			TSet<FName> Names;
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("View")))
			{
				Names.Add(Entry.Name);
			}
			for (const TCHAR* Name : { TEXT("ToggleProjectPanel"), TEXT("ToggleStructurePanel"), TEXT("FocusProject"), TEXT("FocusStructure") })
			{
				TestTrue(*FString::Printf(TEXT("View has %s"), Name), Names.Contains(FName(Name)));
			}
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("View > Project off: Structure fills the side bar"), [this]()
		{
			TestTrue(TEXT("the Project toggle shows on"), Tab->IsViewToggleOn(EDreamCommand::ToggleProjectPanel));
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleProjectPanel);
			TestFalse(TEXT("the command turned View > Project off"), Tab->IsViewToggleOn(EDreamCommand::ToggleProjectPanel));
			TestFalse(TEXT("and saved it (bShowProjectPanel)"), UDreamUserSettings::Get()->bShowProjectPanel);
			FString Disk;
			if (GConfig->GetString(TEXT("/Script/DreamEditor.DreamUserSettings"), TEXT("bShowProjectPanel"), Disk, GEditorSettingsIni))
			{
				TestEqual(TEXT("the config holds bShowProjectPanel=False"), Disk, FString(TEXT("False")));
			}
			TestFalse(TEXT("the Project section is hidden at once"), SideBar()->IsProjectShown());
			TestTrue(TEXT("Structure still shows"), SideBar()->IsStructureShown());
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Structure takes the whole side bar; then View > Structure off as well"), [this, CentreLeft]()
		{
			const FSlateRect Side = RectOf(*SideBar());
			const FSlateRect Structure = RectOf(*SideBar()->GetStructureSection());
			TestEqual(TEXT("the Structure section is as tall as the side bar"), Structure.Bottom - Structure.Top, Side.Bottom - Side.Top, 2.f);
			*CentreLeft = Side.Left;   // where the centre column starts once the side bar is gone
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleStructurePanel);
			TestFalse(TEXT("View > Structure is off"), UDreamUserSettings::Get()->bShowStructurePanel);
			TestFalse(TEXT("the side bar is collapsed with both sections off"), SideBar()->IsSideBarShown());
			TestTrue(TEXT("its visibility is Collapsed (no strip)"), SideBar()->GetVisibility() == EVisibility::Collapsed);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("no strip and no gap: the centre column starts where the side bar did"), [this, CentreLeft]()
		{
			const TSharedPtr<SSplitter> Outer = Tab->GetOuterSplitter();
			const FSlateRect CentreRect = RectOf(Outer->GetChildren()->GetChildAt(1).Get());
			const FSlateRect OuterRect = RectOf(*Outer);
			TestEqual(TEXT("the centre column's left edge is the outer splitter's"), CentreRect.Left, OuterRect.Left, 0.5f);
			TestEqual(TEXT("and where the side bar's was"), CentreRect.Left, *CentreLeft, 0.5f);
			TestEqual(TEXT("the centre column takes the whole width"), CentreRect.Right - CentreRect.Left, OuterRect.Right - OuterRect.Left, 0.5f);
			// The centre's grip next to the side bar's rule is gone too (it would eat clicks at the code's left edge).
			int32 ShownGrips = 0;
			for (const TSharedRef<SWidget>& Grip : FindTagged(Tab.ToSharedRef(), TEXT("DreamSideBar.Grip")))
			{
				bool bShown = true;
				for (TSharedPtr<SWidget> W = Grip; W.IsValid() && W != Tab; W = W->GetParentWidget())
				{
					bShown = bShown && W->GetVisibility().IsVisible();
				}
				ShownGrips += bShown ? 1 : 0;
			}
			TestEqual(TEXT("no side bar grip is shown"), ShownGrips, 0);
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleStructurePanel);
			TestTrue(TEXT("View > Structure on again shows the side bar"), SideBar()->IsSideBarShown() && SideBar()->IsStructureShown());
			TestFalse(TEXT("with Project still hidden"), SideBar()->IsProjectShown());
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleProjectPanel);
			TestTrue(TEXT("and View > Project on shows both"), SideBar()->IsProjectShown() && SideBar()->IsStructureShown());
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Structure off alone: Project fills the side bar"), [this]()
		{
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, false);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the Project section is as tall as the side bar"), [this]()
		{
			const FSlateRect Side = RectOf(*SideBar());
			const FSlateRect Project = RectOf(*SideBar()->GetProjectSection());
			TestEqual(TEXT("Project fills the side bar"), Project.Bottom - Project.Top, Side.Bottom - Side.Top, 2.f);
			return true;
		});
		Steps.Finally(TEXT("restore"), [this]() { DreamSideBarSpec::Restore(Saved, Tab); });
		Steps.Run(Done);
	});

	LatentIt("saves the width and the split per project and a new side bar reads them back", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("resize as the splitters do"), [this]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			// What the outer splitter's OnSlotResized and the section splitter's do.
			SideBar()->SetWidth(312.f);
			SideBar()->SetProjectFraction(0.7f);
			TestEqual(TEXT("the width is 312"), SideBar()->GetWidth(), 312.f);
			TestEqual(TEXT("the settings hold it"), UDreamPerProjectUserSettings::Get()->SideBarWidth, 312.f);
			TestEqual(TEXT("the settings hold the split"), UDreamPerProjectUserSettings::Get()->SideBarProjectFraction, 0.7f, 0.0001f);
			SideBar()->SetWidth(40.f);
			TestEqual(TEXT("a narrower drag stops at 160"), SideBar()->GetWidth(), SDreamSideBar::MinWidth);
			SideBar()->SetWidth(312.f);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("painted at the new width; saved with the session to the per-project ini"), [this]()
		{
			const FSlateRect Side = RectOf(*SideBar());
			TestEqual(TEXT("painted 312 wide"), (Side.Right - Side.Left) / SideBar()->GetPaintSpaceGeometry().Scale, 312.f, 1.5f);
			const FSlateRect Project = RectOf(*SideBar()->GetProjectSection());
			TestEqual(TEXT("the Project section takes about 70 percent of the height"), (Project.Bottom - Project.Top) / (Side.Bottom - Side.Top), 0.7f, 0.05f);
			// The debounced session save, now (as at shutdown), with the case's probe tab closed first so the saved session
			// lists only the developer's tabs (review p1 nit).
			DreamTest::CloseBuffer(BufferId);
			BufferId.Invalidate();
			Tab->SaveSessionState(/*bNow*/ true);
			FString Width;
			FString Fraction;
			TestTrue(TEXT("the ini holds SideBarWidth"), GConfig->GetString(DreamSideBarSpec::ProjectSection, TEXT("SideBarWidth"), Width, GEditorPerProjectIni));
			TestTrue(TEXT("the ini holds SideBarProjectFraction"), GConfig->GetString(DreamSideBarSpec::ProjectSection, TEXT("SideBarProjectFraction"), Fraction, GEditorPerProjectIni));
			TestEqual(TEXT("SideBarWidth is 312"), FCString::Atof(*Width), 312.f, 0.01f);
			TestEqual(TEXT("SideBarProjectFraction is 0.7"), FCString::Atof(*Fraction), 0.7f, 0.0001f);
			FString File;
			if (FFileHelper::LoadFileToString(File, *FPaths::ConvertRelativePathToFull(GEditorPerProjectIni)))
			{
				TestTrue(TEXT("and the file on disk does"), File.Contains(TEXT("SideBarWidth=312")));
			}
			// A side bar built now (a tab spawned again, or the next session) starts from them.
			const TSharedRef<SDreamSideBar> Again = SNew(SDreamSideBar, TWeakPtr<SDreamTab>(Tab));
			TestEqual(TEXT("a new side bar is 312 wide"), Again->GetWidth(), 312.f);
			TestEqual(TEXT("with the 70 percent split"), Again->GetProjectFraction(), 0.7f, 0.0001f);
			return true;
		});
		Steps.Finally(TEXT("restore the width, the split and the ini"), [this]()
		{
			const float Width = Saved.Width;
			DreamSideBarSpec::Restore(Saved, Tab);   // writes the two keys back (that key alone each, not the whole session)
			FString Disk;
			GConfig->GetString(DreamSideBarSpec::ProjectSection, TEXT("SideBarWidth"), Disk, GEditorPerProjectIni);
			TestEqual(TEXT("the ini holds the width found before"), FCString::Atof(*Disk), Width, 0.01f);
		});
		Steps.Run(Done);
	});

	LatentIt("never takes more width than the centre column leaves, and keeps the saved width for later", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<float> Wide = MakeShared<float>(0.f);
		Steps.Do(TEXT("a width saved for a wider window"), [this, Wide]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid() || !Tab->GetOuterSplitter().IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			const float Scale = Tab->GetOuterSplitter()->GetPaintSpaceGeometry().Scale;
			const FSlateRect Outer = RectOf(*Tab->GetOuterSplitter());
			*Wide = (Outer.Right - Outer.Left) / Scale + 200.f;   // wider than the whole splitter (a maximized window's width)
			SideBar()->SetWidth(*Wide);
			return true;
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("the centre column keeps its minimum; the saved width stays"), [this, Wide]()
		{
			const float Scale = Tab->GetOuterSplitter()->GetPaintSpaceGeometry().Scale;
			const FSlateRect Outer = RectOf(*Tab->GetOuterSplitter());
			const FSlateRect Centre = RectOf(Tab->GetOuterSplitter()->GetChildren()->GetChildAt(1).Get());
			const FSlateRect Side = RectOf(*SideBar());
			TestTrue(*FString::Printf(TEXT("the centre column keeps at least %.0f px (%.1f)"), SDreamSideBar::MinCentreWidth, (Centre.Right - Centre.Left) / Scale),
				(Centre.Right - Centre.Left) / Scale >= SDreamSideBar::MinCentreWidth - 1.5f);
			TestTrue(TEXT("the rule (the centre column's left edge) is inside the splitter"), Centre.Left < Outer.Right && Centre.Right <= Outer.Right + 1.f);
			TestEqual(TEXT("the side bar shows what is left"), (Side.Right - Side.Left) / Scale, SideBar()->GetShownWidth(), 1.5f);
			TestEqual(TEXT("the saved width is kept for a wider window"), SideBar()->GetWidth(), *Wide, 0.01f);
			TestEqual(TEXT("the settings hold the saved width"), UDreamPerProjectUserSettings::Get()->SideBarWidth, *Wide, 0.01f);
			return true;
		});
		Steps.Finally(TEXT("restore"), [this]() { DreamSideBarSpec::Restore(Saved, Tab); });
		Steps.Run(Done);
	});

	LatentIt("moves keyboard focus into Project and Structure, and Esc gives it back to the code view", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("focus the code; Focus Project with View > Project off"), [this]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid() || !Tab->FindCodeView(BufferId).IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar and a code view"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, false);
			Tab->ActivateBuffer(BufferId);
			Tab->GetActiveCodeView()->FocusEditor();
			FText Reason;
			TestTrue(TEXT("Focus Project can execute"), Tab->CanExecuteDreamCommand(EDreamCommand::FocusProject, Reason));
			Tab->ExecuteDreamCommand(EDreamCommand::FocusProject);
			TestTrue(TEXT("Focus Project shows the Project section"), UDreamUserSettings::Get()->bShowProjectPanel && SideBar()->IsProjectShown());
			return true;
		});
		Steps.Poll(TEXT("keyboard focus is in the Project tree"), [this]()
		{
			return DreamSideBarSpec::FocusIsInside(SideBar()->GetProjectTree()->GetFocusWidget()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0);
		Steps.Do(TEXT("code-view chords in the tree leave the script alone; Esc"), [this]()
		{
			// Review p1 m4: Ctrl+D (Duplicate Line) and Ctrl+Z (Undo) are the code view's own commands, not the tree's.
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(BufferId);
			const FString Before = Buffer.IsValid() ? Buffer->GetTextAsString() : FString();
			TestTrue(TEXT("Ctrl+D is taken in the side bar (consumed, not passed to the Level Editor)"), DreamSideBarSpec::PressKey(EKeys::D, true));
			DreamSideBarSpec::PressKey(EKeys::Z, true);
			TestTrue(TEXT("Ctrl+D and Ctrl+Z in the tree changed nothing in the active script"), Buffer.IsValid() && Buffer->GetTextAsString() == Before);
			TestTrue(TEXT("and focus stayed in the tree"), DreamSideBarSpec::FocusIsInside(SideBar()->GetProjectTree()->GetFocusWidget()));
			DreamSideBarSpec::PressEscape();
			TestTrue(TEXT("Esc gave focus back to the code view"), Tab->GetActiveCodeView()->HasEditorFocus());
			Tab->ExecuteDreamCommand(EDreamCommand::FocusStructure);
			return true;
		});
		Steps.Poll(TEXT("keyboard focus is in the Structure view"), [this]()
		{
			return DreamSideBarSpec::FocusIsInside(SideBar()->GetStructureView()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0);
		Steps.Do(TEXT("Esc in Structure"), [this]()
		{
			DreamSideBarSpec::PressEscape();
			TestTrue(TEXT("Esc with no filter gave focus back to the code view"), Tab->GetActiveCodeView()->HasEditorFocus());
			return true;
		});
		Steps.Finally(TEXT("restore"), [this]() { DreamSideBarSpec::Restore(Saved, Tab); });
		Steps.Run(Done);
	});

	It("starts a drag from a press on either grip beside the side bar's 1 px rules", [this]()
	{
		if (!Tab.IsValid() || !SideBar().IsValid() || !SideBar()->IsProjectShown() || !SideBar()->IsStructureShown())
		{
			AddInfo(TEXT("both sections must show for this check; skipped"));
			return;
		}
		const TArray<TSharedRef<SWidget>> Grips = FindTagged(Tab.ToSharedRef(), TEXT("DreamSideBar.Grip"));
		TestEqual(TEXT("four grips: two beside the side bar's edge, two beside the sections' rule"), Grips.Num(), 4);
		for (const TSharedRef<SWidget>& Grip : Grips)
		{
			const FVector2f Size = Grip->GetPaintSpaceGeometry().GetLocalSize();
			const bool bVertical = Size.X < Size.Y;
			TestEqual(TEXT("each grip is 2 px across"), bVertical ? Size.X : Size.Y, SDreamSideBar::GripPixels, 0.01f);
			const TSharedPtr<SWidget> Captor = DreamSideBarSpec::PressAndGetCaptor(Grip);
			const TSharedPtr<SSplitter> Expected = bVertical ? Tab->GetOuterSplitter() : SideBar()->GetSectionSplitter();
			TestTrue(*FString::Printf(TEXT("a press on a %s grip starts the %s splitter's drag (captor %s)"), bVertical ? TEXT("vertical") : TEXT("horizontal"),
				bVertical ? TEXT("outer") : TEXT("section"), Captor.IsValid() ? *Captor->GetTypeAsString() : TEXT("none")), Captor == Expected);
		}
	});

	LatentIt("follows a live theme switch: the side bar repaints in the new theme's colours", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		auto CheckPanel = [this](const TCHAR* Theme)
		{
			const TArray<TSharedRef<SWidget>> Panel = DreamSideBarSpec::FindTagged(SideBar().ToSharedRef(), TEXT("DreamSideBar.Panel"));
			if (!TestEqual(TEXT("one side bar panel"), Panel.Num(), 1))
			{
				return;
			}
			FColor Seen;
			FString Error;
			// The lower part of the Structure section: the panel colour (no rows down there for the short probe).
			if (!DreamSideBarSpec::ModeColor(Panel[0], FVector2f(0.1f, 0.85f), FVector2f(0.9f, 0.98f), Seen, Error))
			{
				AddWarning(FString::Printf(TEXT("%s: the side bar could not be read back (%s); its colours were not checked"), Theme, *Error));
				return;
			}
			const FColor Expected = DreamSideBarSpec::ThemeColor(EDreamThemeSlot::ChromeBackground);
			TestTrue(*FString::Printf(TEXT("%s: the side bar is the chrome background %s (%s)"), Theme, *DreamSideBarSpec::Hex(Expected), *DreamSideBarSpec::Hex(Seen)),
				DreamSideBarSpec::Near(Seen, Expected));
			// Review p1 m8 (c): the titles, the tree's text and the rule follow the switch too.
			auto ColorOf = [](const TSharedRef<SWidget>& Text) { return StaticCastSharedRef<STextBlock>(Text)->GetColorAndOpacity().GetSpecifiedColor().ToFColorSRGB(); };
			const FColor Muted = DreamSideBarSpec::ThemeColor(EDreamThemeSlot::ChromeMutedText);
			for (const TCHAR* Title : { TEXT("DreamSideBar.ProjectTitle"), TEXT("DreamSideBar.StructureTitle") })
			{
				const TArray<TSharedRef<SWidget>> Found = DreamSideBarSpec::FindTagged(SideBar().ToSharedRef(), FName(Title));
				TestTrue(*FString::Printf(TEXT("%s: %s is the muted text %s"), Theme, Title, *DreamSideBarSpec::Hex(Muted)),
					Found.Num() == 1 && DreamSideBarSpec::Near(ColorOf(Found[0]), Muted, 1));
			}
			const FColor Text = DreamSideBarSpec::ThemeColor(EDreamThemeSlot::ChromeText);
			const FColor Warning = DreamSideBarSpec::ThemeColor(EDreamThemeSlot::OutWarning);
			// The texts shown (a bound colour is evaluated when its widget is painted; the collapsed message line is not).
			const TSharedRef<SWidget> TreeWidget = SideBar()->GetProjectTree().ToSharedRef();
			TArray<TSharedRef<SWidget>> Labels;
			DreamSideBarSpec::Collect(TreeWidget, [&TreeWidget](const TSharedRef<SWidget>& W)
			{
				if (W->GetType() != FName(TEXT("STextBlock")))
				{
					return false;
				}
				for (TSharedPtr<SWidget> Up = W; Up.IsValid() && Up != TreeWidget; Up = Up->GetParentWidget())
				{
					if (!Up->GetVisibility().IsVisible())
					{
						return false;
					}
				}
				return true;
			}, Labels);
			int32 Off = 0;
			for (const TSharedRef<SWidget>& Label : Labels)
			{
				const FColor C = ColorOf(Label);
				const bool bOk = DreamSideBarSpec::Near(C, Text, 1) || DreamSideBarSpec::Near(C, Muted, 1) || DreamSideBarSpec::Near(C, Warning, 1);
				Off += bOk ? 0 : 1;
				if (!bOk)
				{
					AddInfo(FString::Printf(TEXT("%s: the text '%s' is %s (specified %d)"), Theme, *StaticCastSharedRef<STextBlock>(Label)->GetText().ToString(),
						*DreamSideBarSpec::Hex(C), StaticCastSharedRef<STextBlock>(Label)->GetColorAndOpacity().IsColorSpecified() ? 1 : 0));
				}
			}
			TestTrue(*FString::Printf(TEXT("%s: every Project tree text is the theme's text or muted text (%d texts, %d other)"), Theme, Labels.Num(), Off),
				Labels.Num() > 0 && Off == 0);
			const FColor Border = DreamSideBarSpec::ThemeColor(EDreamThemeSlot::ChromeBorder);
			const FColor Rule = FDreamChrome::Splitter().HandleNormalBrush.TintColor.GetSpecifiedColor().ToFColorSRGB();
			TestTrue(*FString::Printf(TEXT("%s: the rules are drawn in the border colour %s (%s)"), Theme, *DreamSideBarSpec::Hex(Border), *DreamSideBarSpec::Hex(Rule)),
				DreamSideBarSpec::Near(Rule, Border, 1));
			FColor RuleSeen;
			const TSharedPtr<SSplitter> Outer = Tab->GetOuterSplitter();
			const float Scale = Outer->GetPaintSpaceGeometry().Scale;
			const FSlateRect OuterRect = RectOf(*Outer);
			const FSlateRect SideRect = RectOf(*SideBar());
			const float W = FMath::Max(2.f, FMath::RoundToFloat(OuterRect.Right - OuterRect.Left));
			const float RuleX = (FMath::RoundToFloat(SideRect.Right - OuterRect.Left) + 0.25f) / (W - 1.f);   // the rule's pixel column
			if (FMath::IsNearlyEqual(Scale, 1.f) && DreamSideBarSpec::ModeColor(Outer.ToSharedRef(), FVector2f(RuleX, 0.3f), FVector2f(RuleX, 0.7f), RuleSeen, Error))
			{
				TestTrue(*FString::Printf(TEXT("%s: the painted rule beside the side bar is %s (%s)"), Theme, *DreamSideBarSpec::Hex(Border), *DreamSideBarSpec::Hex(RuleSeen)),
					DreamSideBarSpec::Near(RuleSeen, Border));
			}
			else
			{
				AddInfo(FString::Printf(TEXT("%s: the painted rule was not read (DPI scale %.2f): its brush colour was checked"), Theme, Scale));
			}
		};
		Steps.Do(TEXT("Darcula"), [this]()
		{
			if (!Tab.IsValid() || !SideBar().IsValid())
			{
				AddError(TEXT("no Dream tab with a side bar"));
				return false;
			}
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::StructurePanel, true);
			SideBar()->SetProjectFraction(0.4f);
			FDreamThemes::SetCurrent(TEXT("Darcula"), /*bPersist*/ false);   // shown, not saved
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Darcula painted"), [CheckPanel]() { CheckPanel(TEXT("Darcula")); return true; });
		Steps.Do(TEXT("GitHub Light, live"), []()
		{
			FDreamThemes::SetCurrent(TEXT("GitHub Light"), /*bPersist*/ false);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("GitHub Light painted"), [CheckPanel]()
		{
			CheckPanel(TEXT("GitHub Light"));
			return true;
		});
		Steps.Finally(TEXT("restore the theme and the split"), [this]() { DreamSideBarSpec::Restore(Saved, Tab); });
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.Shell.ProjectTree
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamShellProjectTreeSpec, "Dream.Shell.ProjectTree", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FGuid BufferId;
	DreamSideBarSpec::FSaved Saved;
	FString Temp;            // DreamHost/Saved/DreamTests/ProjectTree-<suffix>
	FString RootA;           // absolute
	FString RootB;           // given relative to the project folder
	FString RootBRelative;
	FString Missing;         // never created
	FString Scratches;       // the scratch folder override
	FString Seam;            // the Recycle Bin seam's folder (R-p1-5)
	TArray<FGuid> Opened;    // file tabs the case opened
	int32 GameThreadListingsAtStart = 0;
	FString ScratchFolderAtStart;   // DreamScratch's test folder as the case found it (put back, not cleared)

	TSharedPtr<SDreamProjectTree> Tree() const
	{
		const TSharedPtr<SDreamSideBar> Side = Tab.IsValid() ? Tab->GetSideBar() : nullptr;
		return Side.IsValid() ? Side->GetProjectTree() : nullptr;
	}
	/** Polls until the tree has nothing pending (listings, operations, expanded folders waiting). */
	void AddWaitTree(DreamTest::FDreamSteps& Steps, const FString& What, double Seconds = 10.0);
	TSharedPtr<FDreamBuffer> BufferAt(const FString& Path) const;
END_DEFINE_SPEC(FDreamShellProjectTreeSpec)

void FDreamShellProjectTreeSpec::AddWaitTree(DreamTest::FDreamSteps& Steps, const FString& What, double Seconds)
{
	Steps.Poll(FString::Printf(TEXT("the tree settles (%s)"), *What), [this]()
	{
		const TSharedPtr<SDreamProjectTree> T = Tree();
		return T.IsValid() && T->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, Seconds);
	Steps.WaitFrames(2);
}

TSharedPtr<FDreamBuffer> FDreamShellProjectTreeSpec::BufferAt(const FString& Path) const
{
	const FString K = DreamSideBarSpec::Key(Path);
	for (const TSharedRef<FDreamBuffer>& Buffer : FDreamEditorModule::Get().GetBufferManager().GetOpenBuffers())
	{
		if (Buffer->HasPath() && DreamSideBarSpec::Key(Buffer->GetPath()) == K)
		{
			return Buffer;
		}
	}
	return nullptr;
}

void FDreamShellProjectTreeSpec::Define()
{
	using namespace DreamSideBarSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Capture(Saved);
		Opened.Reset();
		Temp = MakeTempRoot(TEXT("ProjectTree"));
		RootA = Temp / TEXT("RootA");
		RootB = Temp / TEXT("RootB");
		Missing = Temp / TEXT("NotThere");
		Scratches = Temp / TEXT("Scratches");
		// RootA: two folders, three shown files, and the hidden kinds.
		WriteText(RootA / TEXT("Alpha") / TEXT("inner.py"), TEXT("def inner():\n    return 1\n"));
		IFileManager::Get().MakeDirectory(*(RootA / TEXT("beta")), true);
		IFileManager::Get().MakeDirectory(*(RootA / TEXT(".hidden_dir")), true);
		IFileManager::Get().MakeDirectory(*(RootA / TEXT("__pycache__")), true);
		WriteText(RootA / TEXT("zeta.py"), TEXT("zeta = 1\n"));
		WriteText(RootA / TEXT("Readme.txt"), TEXT("not Python\n"));
		WriteText(RootA / TEXT("a_module.py"), TEXT("def a():\n    pass\n"));
		WriteText(RootA / TEXT("cache.pyc"));
		WriteText(RootA / TEXT("old.pyo"));
		WriteText(RootA / TEXT(".dotfile"));
		WriteText(RootB / TEXT("b.py"), TEXT("b = 2\n"));
		WriteText(Scratches / TEXT("scratch_1.py"), TEXT("print('scratch')\n"));
		// RootB as the settings may hold it: relative to the project folder.
		RootBRelative = RootB;
		FPaths::MakePathRelativeTo(RootBRelative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Seam = Temp / TEXT("RecycleSeam");
		const TArray<FString> Roots = { RootA, RootBRelative, Missing };
		FDreamProjectFiles::SetRootsForTest(&Roots);     // the user's Project Roots are never read or written by a case (p1 m7)
		FDreamProjectFiles::SetRecycleSeamForTest(Seam); // routine passes never touch the user's Recycle Bin (R-p1-5)
		const bool bNoPluginFolders = false;
		FDreamProjectFiles::SetPluginFoldersForTest(&bNoPluginFolders);   // PO-05-A1 off unless a case turns it on (no setting)
		ScratchFolderAtStart = DreamScratch::GetFolderForTest();
		DreamScratch::SetFolderForTest(Scratches);       // the Scratches section shows the case's folder
		GameThreadListingsAtStart = FDreamProjectFiles::GetGameThreadListingsForTest();
		if (Tab.IsValid())
		{
			const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, TEXT("project_tree_probe = 1\n"), TEXT("ProjectTree"));
			BufferId = Buffer.IsValid() ? Buffer->GetId() : FGuid();
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProjectPanel, true);
			if (Tree().IsValid())
			{
				Tree()->RefreshRoots();
			}
		}
	});

	AfterEach([this]()
	{
		// Review p1 m8 (a): no folder of the case was listed on the game thread.
		TestEqual(TEXT("no listing ran on the game thread"), FDreamProjectFiles::GetGameThreadListingsForTest() - GameThreadListingsAtStart, 0);
		FDreamProjectFiles::SetMaxEntriesForTest(0);
		FDreamProjectFiles::SetRecycleBinMissingForTest(false);
		FDreamProjectFiles::SetRecycleSeamForTest(FString());
		FDreamProjectFiles::SetRemoteForTest(FString());
		FDreamProjectFiles::SetSettingsSaveOffForTest(false);
		FDreamProjectFiles::SetPluginFoldersForTest(nullptr);
		FDreamProjectFiles::SetDefaultRootForTest(FString());
		for (const FGuid& Id : Opened)
		{
			DreamTest::CloseBuffer(Id);
		}
		Opened.Reset();
		DreamTest::CloseBuffer(BufferId);
		BufferId.Invalidate();
		DreamScratch::SetFolderForTest(ScratchFolderAtStart);   // the folder a runner set before us stays (scratch's cross-area nit)
		Restore(Saved, Tab);
		if (Tree().IsValid())
		{
			Tree()->RefreshRoots();
		}
		RemoveTempRoot(Temp);
		Tab.Reset();
	});

	LatentIt("shows the roots from the settings, a missing one as not found, and Scratches last; lists off the game thread", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("the refresh queues the listings"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			Tree()->RefreshRoots();
			// Nothing is listed inside RefreshRoots: the roots' listings are queued for the worker and applied later.
			TestFalse(TEXT("right after the refresh the listings are still pending (no listing on the game thread)"), Tree()->IsIdleForTest());
			return true;
		});
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("the top level"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			const TArray<FString> Top = LabelsAt(Rows, 0);
			const FString Expected = FString::Join(TArray<FString>{ TEXT("RootA"), TEXT("RootB"), Missing + TEXT(" (not found)"), TEXT("Scratches") }, TEXT(" | "));
			TestEqual(TEXT("the roots in order, the missing one as not found, then Scratches"), FString::Join(Top, TEXT(" | ")), Expected);
			if (const FRow* B = FindRow(Rows, RootB))
			{
				TestEqual(TEXT("the relative root resolves against the project folder"), Key(B->Path), Key(RootB));
			}
			else
			{
				AddError(FString::Printf(TEXT("no row for the relative root %s"), *RootBRelative));
			}
			const FRow* Gone = FindRow(Rows, Missing);
			TestTrue(TEXT("the missing root is muted"), Gone != nullptr && Gone->bMuted);
			TestFalse(TEXT("and was not created"), IFileManager::Get().DirectoryExists(*Missing));
			const FRow* Scratch = FindRow(Rows, Scratches);
			TestTrue(TEXT("the Scratches section shows the scratch folder"), Scratch != nullptr && Scratch->Kind == EKind::Scratches);
			TestTrue(TEXT("with its file"), FindRow(Rows, Scratches / TEXT("scratch_1.py")) != nullptr);
			TestTrue(TEXT("roots start expanded"), FindRow(Rows, RootA) != nullptr && FindRow(Rows, RootA)->bExpanded);
			// The roots from the settings themselves (UDreamProjectTreeSettings, in memory only: nothing writes that key here).
			FDreamProjectFiles::SetRootsForTest(nullptr);
			SetRoots({ RootB });
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("roots from the settings"));
		Steps.Do(TEXT("the settings' root"), [this]()
		{
			const TArray<FString> Top = LabelsAt(Tree()->GetRowsForTest(), 0);
			TestEqual(TEXT("the settings' Project Roots are shown"), FString::Join(Top, TEXT(" | ")), FString(TEXT("RootB | Scratches")));
			SetRoots(Saved.Roots);   // back at once (the restore checks it too)
			// No roots and no Content/Python (a stand-in path that does not exist): the "No project roots" line.
			FDreamProjectFiles::SetDefaultRootForTest(Temp / TEXT("NoContent") / TEXT("Python"));
			const TArray<FString> None;
			FDreamProjectFiles::SetRootsForTest(&None);
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("no roots"));
		Steps.Do(TEXT("the no-roots line, then Scratches; then a Content/Python"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			const TArray<FString> Top = LabelsAt(Rows, 0);
			TestTrue(TEXT("one muted line offers to add a root"), Rows.Num() > 0 && Rows[0].Kind == EKind::NoRoots && Rows[0].bMuted);
			TestEqual(TEXT("it reads No project roots. Add one (ellipsis)"), Rows.Num() > 0 ? Rows[0].Label : FString(), FString(TEXT("No project roots. Add one\u2026")));
			TestEqual(TEXT("Scratches comes last"), Top.Num() > 0 ? Top.Last() : FString(), FString(TEXT("Scratches")));
			// The project's Content/Python, as a stand-in folder (the host project's own Content is never written).
			const FString Default = Temp / TEXT("Content") / TEXT("Python");
			DreamSideBarSpec::WriteText(Default / TEXT("game_tool.py"), TEXT("tool = 1\n"));
			FDreamProjectFiles::SetDefaultRootForTest(Default);
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("Content/Python, no roots"));
		auto RemoveRootEnabled = [this](const FString& Path)
		{
			bool bEnabled = false;
			for (const TPair<FName, bool>& Entry : Tree()->GetContextMenuForTest(Path))
			{
				bEnabled = Entry.Key == FName(TEXT("Dream.ProjectTree.RemoveRoot")) ? Entry.Value : bEnabled;
			}
			return bEnabled;
		};
		Steps.Do(TEXT("no roots: Content/Python first, no no-roots line"), [this, RemoveRootEnabled]()
		{
			const FString Default = Temp / TEXT("Content") / TEXT("Python");
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TestTrue(TEXT("Content/Python is the first row"), Rows.Num() > 0 && Key(Rows[0].Path) == Key(Default) && Rows[0].Kind == EKind::Root);
			TestFalse(TEXT("and the no-roots line is gone"), Rows.ContainsByPredicate([](const FRow& Row) { return Row.Kind == EKind::NoRoots; }));
			TestFalse(TEXT("it cannot be removed"), RemoveRootEnabled(Default));
			// Roots of the user's, one of them Content/Python itself.
			const TArray<FString> Roots = { RootA, Default };
			FDreamProjectFiles::SetRootsForTest(&Roots);
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("Content/Python with roots"));
		Steps.Do(TEXT("with roots: Content/Python still first, once"), [this, RemoveRootEnabled]()
		{
			const FString Default = Temp / TEXT("Content") / TEXT("Python");
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TestEqual(TEXT("Content/Python, the user's root, Scratches"), FString::Join(LabelsAt(Rows, 0), TEXT(" | ")), FString(TEXT("Python | RootA | Scratches")));
			TestEqual(TEXT("an equal user root is not repeated"), Rows.FilterByPredicate([&Default](const FRow& Row) { return !Row.Path.IsEmpty() && Key(Row.Path) == Key(Default); }).Num(), 1);
			TestFalse(TEXT("still not removable"), RemoveRootEnabled(Default));
			TestFalse(TEXT("Add Root of Content/Python adds nothing (it always shows)"), Tree()->AddRoot(Default));
			TestTrue(TEXT("Add Root of another folder adds it after the others (no Content/Python entry is kept for it, R7 retired)"), Tree()->AddRoot(RootB));
			TestEqual(TEXT("then"), FString::Join(LabelsAt(Tree()->GetRowsForTest(), 0), TEXT(" | ")), FString(TEXT("Python | RootA | RootB | Scratches")));
			return true;
		});
		Steps.Run(Done);
	});

	// (editing) R-q1-1: F5 is the tree's Refresh while the tree has focus, even with DebugRun bound to F5 by default.
	LatentIt("refreshes on F5 while the tree has focus and submits no run (R-q1-1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("F5 in the tree"), [this]()
		{
			if (!Tree().IsValid() || !Tab->GetActiveCodeView().IsValid())
			{
				AddError(TEXT("no Project tree or no code view"));
				return false;
			}
			FDreamRunService& Runs = FDreamEditorModule::Get().GetRunService();
			const int32 HistoryBefore = Runs.GetHistory().Num();
			Tree()->FocusTree();
			TestTrue(TEXT("zeta.py selected"), Tree()->SelectForTest(RootA / TEXT("zeta.py")));
			if (!TestTrue(TEXT("keyboard focus is in the tree"), DreamSideBarSpec::FocusIsInside(Tree()->GetFocusWidget())))
			{
				return false;
			}
			TestTrue(TEXT("F5 is handled"), DreamSideBarSpec::PressKey(EKeys::F5, false));
			// The listings of a refresh are queued for the worker and applied later (as RefreshRoots, above).
			TestFalse(TEXT("F5 refreshed: a listing is pending right after it"), Tree()->IsIdleForTest());
			TestNull(TEXT("no run is active"), Runs.GetActiveRecord());
			TestEqual(TEXT("no run was submitted"), Runs.GetHistory().Num(), HistoryBefore);
			TestTrue(TEXT("keyboard focus stays in the tree"), DreamSideBarSpec::FocusIsInside(Tree()->GetFocusWidget()));
			return true;
		});
		AddWaitTree(Steps, TEXT("after F5"));
		Steps.Do(TEXT("still no run after the refresh settled"), [this]()
		{
			TestTrue(TEXT("Dream Python Editor is idle (nothing started later from the key)"), DreamTest::IsIdle());
			TestNotNull(TEXT("zeta.py is still listed"), DreamSideBarSpec::FindRow(Tree()->GetRowsForTest(), RootA / TEXT("zeta.py")));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("lists folders first, then files, case-insensitively, hides dotfiles, pycache, pyc and pyo, and caps a folder", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("RootA's entries"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TestEqual(TEXT("folders first, then files, each case-insensitive; the hidden kinds left out"),
				FString::Join(LabelsAt(Rows, 1, RootA), TEXT(", ")), FString(TEXT("Alpha, beta, a_module.py, Readme.txt, zeta.py")));
			const FRow* Readme = FindRow(Rows, RootA / TEXT("Readme.txt"));
			const FRow* Zeta = FindRow(Rows, RootA / TEXT("zeta.py"));
			const FRow* Alpha = FindRow(Rows, RootA / TEXT("Alpha"));
			TestTrue(TEXT("another file is muted"), Readme != nullptr && Readme->bMuted);
			TestTrue(TEXT("a Python file is not"), Zeta != nullptr && !Zeta->bMuted);
			TestTrue(TEXT("a folder is not"), Alpha != nullptr && !Alpha->bMuted && Alpha->Kind == EKind::Folder);
			TestFalse(TEXT("folders start collapsed"), Alpha != nullptr && Alpha->bExpanded);
			Tree()->SetExpandedForTest(RootA / TEXT("Alpha"), true);
			return true;
		});
		AddWaitTree(Steps, TEXT("Alpha"));
		Steps.Do(TEXT("Alpha expanded, saved; then the listing cap"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			const FRow* Inner = FindRow(Rows, RootA / TEXT("Alpha") / TEXT("inner.py"));
			TestTrue(TEXT("Alpha lists inner.py when it expands"), Inner != nullptr && Inner->Depth == 2);
			TestTrue(TEXT("the expanded folder is saved for the project"),
				UDreamPerProjectUserSettings::Get()->ExpandedFolders.ContainsByPredicate([this](const FString& P) { return Key(P) == Key(RootA / TEXT("Alpha")); }));
			FDreamProjectFiles::SetMaxEntriesForTest(3);
			Tree()->RefreshRoots();   // the shown roots are listed again
			return true;
		});
		AddWaitTree(Steps, TEXT("capped"));
		Steps.Do(TEXT("three entries and the rest counted"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			// RootA has five entries, the others fewer than three: the one More row is RootA's.
			const FRow* More = Rows.FindByPredicate([](const FRow& Row) { return Row.Kind == EKind::More; });
			TestTrue(*FString::Printf(TEXT("a muted '(ellipsis) 2 more' row (%s)"), More ? *More->Label : TEXT("none")), More != nullptr && More->Label == TEXT("\u2026 2 more") && More->bMuted);
			TestEqual(TEXT("RootA shows its first three entries"), FString::Join(LabelsAt(Rows, 1, RootA), TEXT(", ")), FString(TEXT("Alpha, beta, a_module.py")));
			return true;
		});
		Steps.Finally(TEXT("cap back"), []() { FDreamProjectFiles::SetMaxEntriesForTest(0); });
		Steps.Run(Done);
	});

	LatentIt("opens a file on a double-click through the open route, focuses an open one, and highlights the active file", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("double-click zeta.py"), [this]()
		{
			const TSharedPtr<SWidget> Row = Tree().IsValid() ? Tree()->FindRowWidgetForTest(RootA / TEXT("zeta.py")) : nullptr;
			if (!TestTrue(TEXT("zeta.py has a row on screen"), Row.IsValid()))
			{
				return false;
			}
			TestTrue(TEXT("the double-click is routed"), DreamSideBarSpec::DoubleClick(Row.ToSharedRef()));
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("zeta.py is open and active, the code view has focus"), [this]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = BufferAt(RootA / TEXT("zeta.py"));
			if (!TestTrue(TEXT("zeta.py opened in a Dream tab"), Buffer.IsValid()))
			{
				return false;
			}
			Opened.Add(Buffer->GetId());
			TestTrue(TEXT("it is the active tab"), Tab->GetActiveBuffer() == Buffer);
			TestEqual(TEXT("with its file's text"), Buffer->GetTextAsString(), FString(TEXT("zeta = 1\n")));
			TestTrue(TEXT("the code view has keyboard focus"), Tab->GetActiveCodeView().IsValid() && Tab->GetActiveCodeView()->HasEditorFocus());
			Tab->ActivateBuffer(BufferId);   // another tab, then open zeta.py again
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the highlight follows the active tab; Enter on an open file focuses it"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();   // FindRow points into it
			const FRow* Zeta = FindRow(Rows, RootA / TEXT("zeta.py"));
			TestTrue(TEXT("with another tab active zeta.py is not highlighted"), Zeta != nullptr && !Zeta->bActive);
			int32 Before = 0;
			for (const TSharedRef<FDreamBuffer>& B : FDreamEditorModule::Get().GetBufferManager().GetOpenBuffers())
			{
				Before += B->HasPath() && Key(B->GetPath()) == Key(RootA / TEXT("zeta.py")) ? 1 : 0;
			}
			const TSharedPtr<SWidget> Row = Tree()->FindRowWidgetForTest(RootA / TEXT("zeta.py"));
			TestTrue(TEXT("a second double-click is routed"), Row.IsValid() && DreamSideBarSpec::DoubleClick(Row.ToSharedRef()));
			int32 After = 0;
			for (const TSharedRef<FDreamBuffer>& B : FDreamEditorModule::Get().GetBufferManager().GetOpenBuffers())
			{
				After += B->HasPath() && Key(B->GetPath()) == Key(RootA / TEXT("zeta.py")) ? 1 : 0;
			}
			TestEqual(TEXT("an open file is focused, not opened twice"), After, Before);
			TestTrue(TEXT("and it is active again"), Tab->GetActiveBuffer() == BufferAt(RootA / TEXT("zeta.py")));
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the active file's row is highlighted, the others not"), [this]()
		{
			int32 Active = 0;
			for (const FRow& Row : Tree()->GetRowsForTest())
			{
				Active += Row.bActive ? 1 : 0;
			}
			const TArray<FRow> RowsNow = Tree()->GetRowsForTest();   // FindRow points into it
			const FRow* Zeta = FindRow(RowsNow, RootA / TEXT("zeta.py"));
			TestTrue(TEXT("zeta.py is drawn on the current-line colour"), Zeta != nullptr && Zeta->bActive);
			TestEqual(TEXT("one row is highlighted"), Active, 1);
			// A file the open route refuses shows its message (binary).
			const FString Binary = RootA / TEXT("blob.py");
			TArray<uint8> Bytes = { 'a', 0, 'b', 0 };
			FFileHelper::SaveArrayToFile(Bytes, *Binary);
			Tab->OpenLocation(Binary, 0, 0);
			TestFalse(TEXT("a binary file is not opened"), BufferAt(Binary).IsValid());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("renames inline and the open tab follows, a file and a file inside a renamed folder", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<FGuid> Module = MakeShared<FGuid>();
		TSharedRef<FGuid> Inner = MakeShared<FGuid>();
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("open a_module.py and Alpha/inner.py; rename a_module.py with a bad name first"), [this, Module, Inner]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			TestTrue(TEXT("a_module.py opens"), Tab->OpenLocation(RootA / TEXT("a_module.py"), 0, 0));
			TestTrue(TEXT("inner.py opens"), Tab->OpenLocation(RootA / TEXT("Alpha") / TEXT("inner.py"), 0, 0));
			const TSharedPtr<FDreamBuffer> A = BufferAt(RootA / TEXT("a_module.py"));
			const TSharedPtr<FDreamBuffer> I = BufferAt(RootA / TEXT("Alpha") / TEXT("inner.py"));
			if (!A.IsValid() || !I.IsValid())
			{
				AddError(TEXT("the files did not open"));
				return false;
			}
			*Module = A->GetId();
			*Inner = I->GetId();
			Opened.Append({ A->GetId(), I->GetId() });
			Tree()->BeginRename(RootA / TEXT("a_module.py"));
			TestTrue(TEXT("the inline editor opens"), Tree()->IsEditingForTest());
			return true;
		});
		Steps.Poll(TEXT("the name editor takes keyboard focus"), [this]()
		{
			const TSharedPtr<SEditableTextBox> Box = Tree()->GetNameEditorForTest();
			return Box.IsValid() && DreamSideBarSpec::FocusIsInside(Box) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0);
		Steps.Do(TEXT("type a bad name and press Enter: the editor stays with what was typed"), [this]()
		{
			const TArray<TSharedRef<SWidget>> Editors = FindTagged(Tree().ToSharedRef(), TEXT("DreamSideBar.NameEditor"));
			TestEqual(TEXT("one name editor in the row"), Editors.Num(), 1);
			TestEqual(TEXT("it holds the name"), Tree()->GetNameEditorForTest()->GetText().ToString(), FString(TEXT("a_module.py")));
			DreamSideBarSpec::TypeText(TEXT("bad/name.py"));   // replaces the selected name
			DreamSideBarSpec::PressKey(EKeys::Enter, false, 13);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("still editing, still focused; a good name with Enter"), [this]()
		{
			// Review p1 M2: the engine follows an Enter commit with a focus change; neither may close the editor.
			TestTrue(TEXT("a name with a separator is refused, the editor stays"), Tree()->IsEditingForTest());
			const TSharedPtr<SEditableTextBox> Box = Tree()->GetNameEditorForTest();
			TestTrue(TEXT("and keeps the typed name"), Box.IsValid() && Box->GetText().ToString() == TEXT("bad/name.py"));
			TestTrue(TEXT("and keyboard focus"), Box.IsValid() && DreamSideBarSpec::FocusIsInside(Box));
			TestFalse(TEXT("with the reason in the message line"), Tree()->GetMessageForTest().IsEmpty());
			// Select all and type the good name, then Enter.
			DreamSideBarSpec::PressKey(EKeys::A, true);
			DreamSideBarSpec::TypeText(TEXT("renamed_module.py"));
			DreamSideBarSpec::PressKey(EKeys::Enter, false, 13);
			TestFalse(TEXT("a good name closes the editor"), Tree()->IsEditingForTest());
			return true;
		});
		Steps.Poll(TEXT("keyboard focus is back in the tree"), [this]()
		{
			return DreamSideBarSpec::FocusIsInside(Tree()->GetFocusWidget()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0);
		AddWaitTree(Steps, TEXT("rename"));
		Steps.Do(TEXT("the file moved and its tab followed"), [this, Module]()
		{
			const FString New = RootA / TEXT("renamed_module.py");
			TestTrue(TEXT("the new file exists"), IFileManager::Get().FileExists(*New));
			TestFalse(TEXT("the old one is gone"), IFileManager::Get().FileExists(*(RootA / TEXT("a_module.py"))));
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(*Module);
			if (TestTrue(TEXT("the tab is still open"), Buffer.IsValid()))
			{
				TestEqual(TEXT("its path is the new one"), Key(Buffer->GetPath()), Key(New));
				TestEqual(TEXT("its label is the new name"), Buffer->GetDisplayName(), FString(TEXT("renamed_module.py")));
				TestFalse(TEXT("and it is not deleted on disk"), Buffer->IsDeletedOnDisk());
			}
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TestTrue(TEXT("the tree shows the new name"), FindRow(Rows, New) != nullptr);
			TestTrue(TEXT("and not the old one"), FindRow(Rows, RootA / TEXT("a_module.py")) == nullptr);
			Tree()->BeginRename(RootA / TEXT("Alpha"));
			Tree()->CommitEdit(TEXT("Gamma"));
			return true;
		});
		AddWaitTree(Steps, TEXT("folder rename"));
		Steps.Do(TEXT("a tab inside the renamed folder followed"), [this, Inner]()
		{
			const FString New = RootA / TEXT("Gamma") / TEXT("inner.py");
			TestTrue(TEXT("the folder moved"), IFileManager::Get().FileExists(*New));
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(*Inner);
			TestTrue(TEXT("inner.py's tab now names Gamma/inner.py"), Buffer.IsValid() && Key(Buffer->GetPath()) == Key(New));
			return true;
		});
		// Review p1 m8 (b): the file watch would mark the old paths deleted about 0.65 s after the rename plus a worker pass, if
		// the tabs had not followed; watch them for 1.5 s.
		TSharedRef<double> WatchUntil = MakeShared<double>(0.0);
		Steps.Do(TEXT("watch the followed tabs"), [WatchUntil]() { *WatchUntil = FPlatformTime::Seconds() + 1.5; return true; });
		Steps.Poll(TEXT("for 1.5 s no followed tab is marked deleted on disk"), [this, Module, Inner, WatchUntil]()
		{
			for (const FGuid& Id : { *Module, *Inner })
			{
				const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Id);
				if (!Buffer.IsValid() || Buffer->IsDeletedOnDisk())
				{
					AddError(TEXT("a followed tab was marked deleted on disk (the file watch still knew its old path)"));
					return DreamTest::EStep::Fail;
				}
			}
			return FPlatformTime::Seconds() >= *WatchUntil ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Run(Done);
	});

	LatentIt("deletes through the Recycle Bin seam after the confirm, refuses without a bin, and an open tab gets the deleted state", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		const FString Victim = RootA / TEXT("victim.py");
		TSharedRef<FGuid> VictimTab = MakeShared<FGuid>();
		Steps.Do(TEXT("a file to delete, open in a tab"), [this, Victim, VictimTab]()
		{
			WriteText(Victim, TEXT("victim = True\n"));
			if (!Tab.IsValid() || !Tab->OpenLocation(Victim, 0, 0))
			{
				AddError(TEXT("victim.py did not open"));
				return false;
			}
			*VictimTab = BufferAt(Victim)->GetId();
			Opened.Add(*VictimTab);
			return true;
		});
		Steps.Poll(TEXT("the watcher shows victim.py"), [this, Victim]()
		{
			return Tree().IsValid() && FindRow(Tree()->GetRowsForTest(), Victim) != nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("without a Recycle Bin nothing is deleted"), [this, Victim]()
		{
			FDreamProjectFiles::SetRecycleBinMissingForTest(true);
			Tree()->RequestDelete(Victim);
			TestEqual(TEXT("the confirm is asked first"), Tree()->GetMessageForTest().ToString(), FString(TEXT("Move victim.py to the Recycle Bin?")));
			TestTrue(TEXT("nothing happened yet"), IFileManager::Get().FileExists(*Victim));
			Tree()->ConfirmDelete();
			return true;
		});
		AddWaitTree(Steps, TEXT("refused delete"));
		Steps.Do(TEXT("refused, kept; then Cancel"), [this, Victim]()
		{
			TestTrue(TEXT("the file is still there"), IFileManager::Get().FileExists(*Victim));
			TestTrue(*FString::Printf(TEXT("the message says why (%s)"), *Tree()->GetMessageForTest().ToString()),
				Tree()->GetMessageForTest().ToString().Contains(TEXT("no Recycle Bin")));
			FDreamProjectFiles::SetRecycleBinMissingForTest(false);
			Tree()->RequestDelete(Victim);
			Tree()->CancelDelete();
			TestTrue(TEXT("Cancel deletes nothing"), Tree()->GetPendingDeleteForTest().IsEmpty() && IFileManager::Get().FileExists(*Victim));
			Tree()->RequestDelete(Victim);
			Tree()->ConfirmDelete();
			return true;
		});
		AddWaitTree(Steps, TEXT("delete"));
		Steps.Do(TEXT("through the seam"), [this, Victim]()
		{
			TestFalse(TEXT("the file is gone from its folder"), IFileManager::Get().FileExists(*Victim));
			const TArray<FString> Calls = FDreamProjectFiles::GetRecycleSeamCallsForTest();
			TestTrue(TEXT("the recycle call went through the seam, once, for victim.py"), Calls.Num() == 1 && Key(Calls[0]) == Key(Victim));
			TArray<FString> Moved;
			IFileManager::Get().FindFiles(Moved, *(Seam / TEXT("*_victim.py")), /*Files*/ true, /*Directories*/ false);
			TestEqual(TEXT("the seam holds the file (nothing was destroyed)"), Moved.Num(), 1);
			TestEqual(TEXT("the message line says where it went"), Tree()->GetMessageForTest().ToString(), FString(TEXT("Moved victim.py to the Recycle Bin.")));
			return true;
		});
		Steps.Poll(TEXT("the open tab gets the deleted-on-disk state (the file watch)"), [VictimTab]()
		{
			const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(*VictimTab);
			return Buffer.IsValid() && Buffer->IsDeletedOnDisk() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Poll(TEXT("the row goes"), [this, Victim]()
		{
			return FindRow(Tree()->GetRowsForTest(), Victim) == nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Finally(TEXT("bin hook back"), []() { FDreamProjectFiles::SetRecycleBinMissingForTest(false); });
		Steps.Run(Done);
	});

	LatentIt("sends a file to the real Recycle Bin only when asked with -DreamRealRecycleTest", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// R-p1-5: routine passes never touch the user's Recycle Bin. The architect runs this case once at integration, with
		// -DreamRealRecycleTest, and removes its item from the bin by hand.
		if (!FParse::Param(FCommandLine::Get(), TEXT("DreamRealRecycleTest")))
		{
			AddWarning(TEXT("NOT RUN (needs -DreamRealRecycleTest; it leaves one real_recycle_<suffix>.py in the user's Recycle Bin)"));
			Done.Execute();
			return;
		}
		DreamTest::FDreamSteps Steps(*this);
		const FString Victim = RootA / FString::Printf(TEXT("real_recycle_%s.py"), *DreamTest::UniqueSuffix());
		TSharedRef<int64> Before = MakeShared<int64>(-1);
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("a file, the real bin"), [this, Victim, Before]()
		{
			FDreamProjectFiles::SetRecycleSeamForTest(FString());   // the real Recycle Bin, for this case only
			WriteText(Victim, TEXT("x = 1\n"));
			*Before = FDreamProjectFiles::GetRecycleBinItemCountForTest(RootA);
			Tree()->Refresh(RootA);
			return true;
		});
		Steps.Poll(TEXT("its row shows"), [this, Victim]()
		{
			return FindRow(Tree()->GetRowsForTest(), Victim) != nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("delete it"), [this, Victim]()
		{
			Tree()->RequestDelete(Victim);
			Tree()->ConfirmDelete();
			return true;
		});
		AddWaitTree(Steps, TEXT("the real delete"));
		Steps.Do(TEXT("in the Recycle Bin"), [this, Victim, Before]()
		{
			TestFalse(TEXT("the file left its folder"), IFileManager::Get().FileExists(*Victim));
			const int64 After = FDreamProjectFiles::GetRecycleBinItemCountForTest(RootA);
			TestTrue(*FString::Printf(TEXT("the Recycle Bin holds one more item (%lld -> %lld)"), *Before, After), *Before >= 0 && After >= *Before + 1);
			TestEqual(TEXT("the message line says where it went"), Tree()->GetMessageForTest().ToString(),
				FString::Printf(TEXT("Moved %s to the Recycle Bin."), *FPaths::GetCleanFilename(Victim)));
			AddInfo(FString::Printf(TEXT("left in the Recycle Bin: %s (remove it by hand)"), *Victim));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("refreshes from the directory watcher when files come and go outside Dream Python Editor", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		const FString Outside = RootA / TEXT("beta") / TEXT("made_outside.py");
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("expand beta; write a file there outside Dream Python Editor"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			Tree()->SetExpandedForTest(RootA / TEXT("beta"), true);
			TestTrue(TEXT("the tree watches its roots"), Tree()->GetFilesForTest()->GetWatchCountForTest() >= 2);
			return true;
		});
		AddWaitTree(Steps, TEXT("beta"));
		Steps.Do(TEXT("write"), [Outside]() { return DreamSideBarSpec::WriteText(Outside, TEXT("x = 1\n")); });
		Steps.Poll(TEXT("the new file shows"), [this, Outside]()
		{
			return FindRow(Tree()->GetRowsForTest(), Outside) != nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("remove it outside Dream Python Editor"), [Outside]()
		{
			return IFileManager::Get().Delete(*Outside, /*RequireExists*/ true, /*EvenReadOnly*/ false, /*Quiet*/ true);   // the case's own file
		});
		Steps.Poll(TEXT("its row goes"), [this, Outside]()
		{
			return FindRow(Tree()->GetRowsForTest(), Outside) == nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		// Review p1 m10: an auto-save's hidden temp file (".<name>.dream-*.tmp") coming and going lists nothing again.
		TSharedRef<int32> Listings = MakeShared<int32>(0);
		TSharedRef<double> QuietUntil = MakeShared<double>(0.0);
		AddWaitTree(Steps, TEXT("before the temp file"));
		Steps.Do(TEXT("a hidden temp file comes and goes"), [this, Listings, QuietUntil]()
		{
			*Listings = Tree()->GetFilesForTest()->GetListingsOfForTest(RootA);   // RootA's own (the missing root's retry lists too)
			const FString TempFile = RootA / TEXT(".zeta.py.dream-1234.tmp");
			DreamSideBarSpec::WriteText(TempFile, TEXT("x"));
			IFileManager::Get().Delete(*TempFile, false, false, true);
			*QuietUntil = FPlatformTime::Seconds() + 1.0;   // the watcher's events and the 0.25 s debounce
			return true;
		});
		Steps.Poll(TEXT("one quiet second"), [QuietUntil]() { return FPlatformTime::Seconds() >= *QuietUntil ? DreamTest::EStep::Next : DreamTest::EStep::Wait; }, 5.0);
		Steps.Do(TEXT("no listing started for it"), [this, Listings]()
		{
			TestEqual(TEXT("the hidden temp file started no listing of its folder"), Tree()->GetFilesForTest()->GetListingsOfForTest(RootA), *Listings);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("adds and removes roots for the project, makes files and folders inline, and offers the context menus", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		const FString RootC = Temp / TEXT("RootC");
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("Add Root"), [this, RootC]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			IFileManager::Get().MakeDirectory(*RootC, true);
			TestTrue(TEXT("Add Root adds a folder"), Tree()->AddRoot(RootC));
			TestFalse(TEXT("but not twice"), Tree()->AddRoot(RootC));
			// Under the test override the user's Project Roots are untouched, in memory and in the ini (p1 m7).
			TestFalse(TEXT("the user's Project Roots do not get it"), RootPaths().Contains(RootC));
			TArray<FString> Disk;
			GConfig->GetArray(TEXT("/Script/DreamEditor.DreamProjectTreeSettings"), TEXT("ProjectRoots"), Disk, GEditorPerProjectIni);
			TestFalse(TEXT("nor does the per-project ini"), FString::Join(Disk, TEXT(";")).Contains(TEXT("RootC")));
			return true;
		});
		AddWaitTree(Steps, TEXT("RootC"));
		Steps.Do(TEXT("RootC shows; Remove Root"), [this, RootC]()
		{
			TestTrue(TEXT("RootC is a top-level row"), FindRow(Tree()->GetRowsForTest(), RootC) != nullptr);
			TestTrue(TEXT("Remove Root removes it"), Tree()->RemoveRoot(RootC));
			TestFalse(TEXT("and not twice"), Tree()->RemoveRoot(RootC));
			TestTrue(TEXT("and from the tree"), FindRow(Tree()->GetRowsForTest(), RootC) == nullptr);
			TestTrue(TEXT("nothing on disk changed"), IFileManager::Get().DirectoryExists(*RootC));
			Tree()->BeginNewItem(RootA, /*bFolder*/ false);
			TestTrue(TEXT("New Python File opens the inline editor"), Tree()->IsEditingForTest());
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("name the new file"), [this]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TestTrue(TEXT("the editor row heads RootA"), Rows.ContainsByPredicate([](const FRow& Row) { return Row.Kind == EKind::NewItem; }));
			Tree()->CommitEdit(TEXT("made_here"));
			return true;
		});
		AddWaitTree(Steps, TEXT("new file"));
		Steps.Do(TEXT("made_here.py exists and opened; New Folder"), [this]()
		{
			const FString Made = RootA / TEXT("made_here.py");
			TestTrue(TEXT(".py was added and the file exists"), IFileManager::Get().FileExists(*Made));
			const TSharedPtr<FDreamBuffer> Buffer = BufferAt(Made);
			if (TestTrue(TEXT("the new file opened in a tab"), Buffer.IsValid()))
			{
				Opened.Add(Buffer->GetId());
				TestTrue(TEXT("as the active tab"), Tab->GetActiveBuffer() == Buffer);
			}
			Tree()->BeginNewItem(RootA, /*bFolder*/ true);
			Tree()->CommitEdit(TEXT("NewDir"));
			return true;
		});
		AddWaitTree(Steps, TEXT("new folder"));
		Steps.Do(TEXT("the folder exists; the context menus"), [this]()
		{
			TestTrue(TEXT("NewDir exists"), IFileManager::Get().DirectoryExists(*(RootA / TEXT("NewDir"))));
			const TArray<FRow> Rows = Tree()->GetRowsForTest();   // FindRow points into it
			const FRow* Dir = FindRow(Rows, RootA / TEXT("NewDir"));
			TestTrue(TEXT("and shows as a folder"), Dir != nullptr && Dir->Kind == EKind::Folder);
			auto Names = [this](const FString& Path)
			{
				TArray<FString> Out;
				for (const TPair<FName, bool>& Entry : Tree()->GetContextMenuForTest(Path))
				{
					Out.Add(Entry.Key.ToString().RightChop(FCString::Strlen(TEXT("Dream.ProjectTree."))) + (Entry.Value ? TEXT("") : TEXT("(off)")));
				}
				return FString::Join(Out, TEXT(","));
			};
			TestEqual(TEXT("a file's menu"), Names(RootA / TEXT("zeta.py")), FString(TEXT("Open,NewFile,NewFolder,Rename,Delete,Refresh,Reveal,CopyPath")));
			TestEqual(TEXT("a folder's menu"), Names(RootA / TEXT("beta")), FString(TEXT("NewFile,NewFolder,Rename,Delete,Refresh,Reveal,CopyPath")));
			TestEqual(TEXT("a root's menu"), Names(RootA), FString(TEXT("NewFile,NewFolder,Refresh,Reveal,CopyPath,RemoveRoot,AddRoot")));
			TestEqual(TEXT("a missing root's menu"), Names(Missing), FString(TEXT("Refresh,Reveal(off),CopyPath,RemoveRoot,AddRoot")));
			TestEqual(TEXT("the Scratches section's menu (never removable)"), Names(Scratches), FString(TEXT("NewFile,NewFolder,Refresh,Reveal,CopyPath,AddRoot")));
			TestEqual(TEXT("the background's menu"), Names(FString()), FString(TEXT("AddRoot,Refresh,PluginFolders")));
			// R-p1-11: network paths are told apart (listed, never watched).
			TestTrue(TEXT("a UNC path is a network path"), FDreamProjectFiles::IsRemotePath(TEXT("//server/share/scripts")));
			TestFalse(TEXT("a local root is not"), FDreamProjectFiles::IsRemotePath(RootA));
			// scratch-p2-m2: a UNC root keeps its leading "//" (never the current drive's /server/share), and so does its key.
			TestEqual(TEXT("a UNC root stays a UNC path"), FDreamProjectFiles::ResolveRootPath(TEXT("\\\\server\\share\\scripts\\")), FString(TEXT("//server/share/scripts")));
			TestTrue(TEXT("its key too"), FDreamProjectFiles::PathKey(TEXT("\\\\server\\share\\scripts")).StartsWith(TEXT("//")));
			TestTrue(TEXT("and a folder in it is under it"), FDreamProjectFiles::IsUnder(TEXT("//server/share/scripts/tools"), TEXT("\\\\server\\share\\scripts")));
			TestFalse(TEXT("but not under the drive's /server/share"), FDreamProjectFiles::IsUnder(TEXT("//server/share/scripts/tools"), TEXT("/server/share/scripts")));
			// Copy Path puts the full native path on the clipboard (the user's clipboard is put back).
			FString Before;
			FPlatformApplicationMisc::ClipboardPaste(Before);
			Tree()->CopyPath(RootA / TEXT("zeta.py"));
			FString Copied;
			FPlatformApplicationMisc::ClipboardPaste(Copied);
			FString Expected = RootA / TEXT("zeta.py");
			FPaths::MakePlatformFilename(Expected);
			TestEqual(TEXT("Copy Path copies the full path"), Copied, Expected);
			FPlatformApplicationMisc::ClipboardCopy(*Before);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("fills in a missing root and a missing scratch folder once they appear (R-p1-7)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// Review p1 M1: a top-level item found missing was never listed again. Both start missing here and appear later,
		// made behind the tree's back (no Add Root, no folder broadcast): the retry must find them.
		DreamTest::FDreamSteps Steps(*this);
		const FString LateRoot = Temp / TEXT("LateRoot");
		const FString LateScratch = Temp / TEXT("LateScratches");
		Steps.Do(TEXT("a root and a scratch folder that do not exist"), [this, LateRoot, LateScratch]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			const TArray<FString> Roots = { LateRoot };
			FDreamProjectFiles::SetRootsForTest(&Roots);
			DreamScratch::SetFolderForTest(LateScratch);   // broadcasts OnFolderChanged: the tree rebuilds its top level
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("missing items"));
		Steps.Do(TEXT("both missing; then both made"), [this, LateRoot, LateScratch]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			const FRow* Root = FindRow(Rows, LateRoot);
			TestTrue(TEXT("the root shows as not found"), Root != nullptr && Root->bMuted && Root->Label.EndsWith(TEXT("(not found)")));
			TestTrue(TEXT("Scratches shows with nothing under it"), FindRow(Rows, LateScratch) != nullptr && FindRow(Rows, LateScratch / TEXT("scratch_1.py")) == nullptr);
			TestFalse(TEXT("neither folder was created by the tree"), IFileManager::Get().DirectoryExists(*LateRoot) || IFileManager::Get().DirectoryExists(*LateScratch));
			DreamSideBarSpec::WriteText(LateRoot / TEXT("late.py"), TEXT("late = 1\n"));
			DreamSideBarSpec::WriteText(LateScratch / TEXT("scratch_1.py"), TEXT("print(1)\n"));
			return true;
		});
		Steps.Poll(TEXT("the retry lists both: the root's file and the scratch file show"), [this, LateRoot, LateScratch]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			const FRow* Root = FindRow(Rows, LateRoot);
			const bool bRoot = Root != nullptr && !Root->bMuted && FindRow(Rows, LateRoot / TEXT("late.py")) != nullptr;
			const bool bScratch = FindRow(Rows, LateScratch / TEXT("scratch_1.py")) != nullptr;
			return bRoot && bScratch ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0 * SDreamProjectTree::RetrySeconds + 2.0);
		Steps.Do(TEXT("and both are watched now"), [this]()
		{
			TestTrue(TEXT("the root and the scratch folder are watched once they exist"), Tree()->GetFilesForTest()->GetWatchCountForTest() >= 2);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("refuses Rename and Delete on a root, the scratch folder and a folder holding one (p1 m9)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// A root above the others: the case folder holds RootA (a root too), RootB (not a root) and Scratches (the scratch folder).
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("roots: the case folder and RootA"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			const TArray<FString> Roots = { Temp, RootA };
			FDreamProjectFiles::SetRootsForTest(&Roots);
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("nested roots"));
		Steps.Do(TEXT("the protected rows"), [this]()
		{
			auto HasEntry = [this](const FString& Path, const TCHAR* Name)
			{
				for (const TPair<FName, bool>& Entry : Tree()->GetContextMenuForTest(Path))
				{
					if (Entry.Key == FName(Name))
					{
						return true;
					}
				}
				return false;
			};
			TestTrue(TEXT("the scratch folder inside a root is protected"), Tree()->IsProtectedForTest(Scratches));
			TestTrue(TEXT("a configured root inside another root is protected"), Tree()->IsProtectedForTest(RootA));
			TestTrue(TEXT("a root itself is protected"), Tree()->IsProtectedForTest(Temp));
			TestFalse(TEXT("an ordinary folder is not"), Tree()->IsProtectedForTest(RootB));
			TestTrue(TEXT("an ordinary folder offers Rename"), HasEntry(RootB, TEXT("Dream.ProjectTree.Rename")));
			TestFalse(TEXT("the scratch folder offers no Delete"), HasEntry(Scratches, TEXT("Dream.ProjectTree.Delete")));
			Tree()->RequestDelete(Scratches);
			TestTrue(TEXT("Delete on the scratch folder is refused with a message"), Tree()->GetPendingDeleteForTest().IsEmpty() && !Tree()->GetMessageForTest().IsEmpty());
			Tree()->BeginRename(RootA);
			TestFalse(TEXT("Rename on a root is refused"), Tree()->IsEditingForTest());
			TestTrue(TEXT("and nothing moved"), IFileManager::Get().DirectoryExists(*Scratches) && IFileManager::Get().DirectoryExists(*RootA));
			return true;
		});
		Steps.Run(Done);
	});
	LatentIt("closes the inline editor when code moves keyboard focus away, and Esc in the tree closes it too (p2 m1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		// Review p2 m1: the engine reports a focus change made by code as ETextCommit::Default, which the editor ignored, so a
		// chord's command (Ctrl+T focusing a new tab) left it open and unfocused, and F2 / Enter / Delete in the tree did nothing.
		DreamTest::FDreamSteps Steps(*this);
		const FString Zeta = RootA / TEXT("zeta.py");
		auto AddWaitEditorFocus = [this, &Steps](const TCHAR* What)
		{
			Steps.Poll(What, [this]()
			{
				const TSharedPtr<SEditableTextBox> Box = Tree()->GetNameEditorForTest();
				return Box.IsValid() && DreamSideBarSpec::FocusIsInside(Box) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 3.0);
			Steps.WaitFrames(2);   // the tree's tick sees the editor focused once
		};
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("Rename zeta.py"), [this, Zeta]()
		{
			if (!Tree().IsValid() || !Tab->GetActiveCodeView().IsValid())
			{
				AddError(TEXT("no Project tree or no code view"));
				return false;
			}
			Tree()->BeginRename(Zeta);
			return TestTrue(TEXT("the inline editor opens"), Tree()->IsEditingForTest());
		});
		AddWaitEditorFocus(TEXT("the name editor takes keyboard focus"));
		Steps.Do(TEXT("code moves keyboard focus to the code view (as New Tab does)"), [this]()
		{
			AddInfo(FString::Printf(TEXT("the editor app is active: %s (the editor closes only then; an Alt-Tab keeps it)"),
				FSlateApplication::Get().IsActive() ? TEXT("yes") : TEXT("no")));
			Tab->GetActiveCodeView()->FocusEditor();
			return TestFalse(TEXT("the name editor lost keyboard focus"), DreamSideBarSpec::FocusIsInside(Tree()->GetNameEditorForTest()));
		});
		Steps.Poll(TEXT("the inline editor closes"), [this]()
		{
			return Tree()->IsEditingForTest() ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 2.0);
		Steps.Do(TEXT("focus stayed in the code view; F2 in the tree opens the editor again"), [this, Zeta]()
		{
			TestTrue(TEXT("keyboard focus stays where the code put it"), DreamSideBarSpec::FocusIsInside(Tab->GetActiveCodeView()));
			TestTrue(TEXT("nothing was renamed"), IFileManager::Get().FileExists(*Zeta));
			Tree()->FocusTree();
			TestTrue(TEXT("zeta.py selected"), Tree()->SelectForTest(Zeta));
			TestTrue(TEXT("F2 is handled"), DreamSideBarSpec::PressKey(EKeys::F2, false));
			return TestTrue(TEXT("F2 opens the inline editor again"), Tree()->IsEditingForTest());
		});
		AddWaitEditorFocus(TEXT("the name editor takes keyboard focus again"));
		Steps.Do(TEXT("focus into the tree by code and Esc at once: the editor closes"), [this]()
		{
			// The same frame, before the tree's tick: Esc reaches the tree with the editor still open.
			FSlateApplication::Get().SetKeyboardFocus(Tree()->GetFocusWidget(), EFocusCause::SetDirectly);
			TestTrue(TEXT("still editing before Esc"), Tree()->IsEditingForTest());
			TestTrue(TEXT("Esc is handled"), DreamSideBarSpec::PressEscape());
			TestFalse(TEXT("Esc in the tree closes the inline editor"), Tree()->IsEditingForTest());
			TestTrue(TEXT("and keyboard focus stays in the tree"), DreamSideBarSpec::FocusIsInside(Tree()->GetFocusWidget()));
			Tree()->BeginRename(RootA / TEXT("zeta.py"));
			return true;
		});
		AddWaitEditorFocus(TEXT("the name editor takes keyboard focus a third time"));
		Steps.Do(TEXT("View > Project off with the editor open"), [this]()
		{
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleProjectPanel);   // off (the AfterEach restore writes back only a change)
			TestFalse(TEXT("View > Project is off"), UDreamUserSettings::Get()->bShowProjectPanel);
			TestFalse(TEXT("hiding the section closes the inline editor"), Tree()->IsEditingForTest());
			Tab->ExecuteDreamCommand(EDreamCommand::ToggleProjectPanel);   // on again
			return true;
		});
		AddWaitTree(Steps, TEXT("shown again"));
		Steps.Do(TEXT("nothing renamed, the tree as before"), [this, Zeta]()
		{
			TestTrue(TEXT("zeta.py is still there"), IFileManager::Get().FileExists(*Zeta) && FindRow(Tree()->GetRowsForTest(), Zeta) != nullptr);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("lists a new scratch file at once in a scratch folder that is not watched (p2 m2)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		// Review p2 m2: a network scratch folder is never watched (R-p1-11), and the retry covers only missing items, so a new
		// scratch_N.py did not show until a re-expand. The case's scratch folder is made to count as a network folder.
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<FString> NewPath = MakeShared<FString>();
		Steps.Do(TEXT("the scratch folder counts as a network folder"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			FDreamProjectFiles::SetRemoteForTest(Scratches);
			// scratch's marker for an override folder already seeded: the samples stay out of this case's folder.
			DreamSideBarSpec::WriteText(Scratches / TEXT(".dream-samples-seeded"));
			Tree()->RefreshRoots();   // a kept item's network state is looked at again (p2 nit)
			return true;
		});
		AddWaitTree(Steps, TEXT("scratch folder"));
		Steps.Do(TEXT("not watched; a new scratch tab"), [this, NewPath]()
		{
			TestTrue(TEXT("Scratches is a network folder now"), Tree()->IsRemoteForTest(Scratches));
			TestFalse(TEXT("and not watched"), Tree()->IsWatchedForTest(Scratches));
			TestTrue(TEXT("its first file shows"), FindRow(Tree()->GetRowsForTest(), Scratches / TEXT("scratch_1.py")) != nullptr);
			const TSharedRef<FDreamBuffer> Buffer = Tab->NewScratchTab();
			Opened.Add(Buffer->GetId());
			*NewPath = Buffer->GetPath();
			TestTrue(TEXT("the new tab is a scratch file in the case's folder"), Buffer->IsScratchFile() && FDreamProjectFiles::IsUnder(*NewPath, Scratches));
			AddInfo(FString::Printf(TEXT("new scratch file %s"), *FPaths::GetCleanFilename(*NewPath)));
			return true;
		});
		Steps.Poll(TEXT("its row shows at once, with no watcher and no retry"), [this, NewPath]()
		{
			return !NewPath->IsEmpty() && FindRow(Tree()->GetRowsForTest(), *NewPath) != nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 1.5);
		Steps.Do(TEXT("still not watched"), [this]()
		{
			TestFalse(TEXT("the scratch folder is still not watched"), Tree()->IsWatchedForTest(Scratches));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("lists at most four folders at once, queues the rest, and backs off its looks at a missing network root (p2 m3)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		struct FQueueProbe
		{
			TSharedPtr<FDreamProjectFiles> Files;
			TSet<FString> Listed;
		};
		TSharedRef<FQueueProbe> Probe = MakeShared<FQueueProbe>();
		Steps.Do(TEXT("ten folders asked at once"), [this, Probe]()
		{
			TArray<FString> Folders;
			for (int32 Index = 0; Index < 10; ++Index)
			{
				Folders.Add(Temp / TEXT("Many") / FString::Printf(TEXT("F%d"), Index));
				IFileManager::Get().MakeDirectory(*Folders.Last(), true);
			}
			const TWeakPtr<FQueueProbe> WeakProbe = Probe;
			Probe->Files = MakeShared<FDreamProjectFiles>(
				[WeakProbe](const FDreamFolderListing& Listing)
				{
					if (const TSharedPtr<FQueueProbe> P = WeakProbe.Pin())
					{
						P->Listed.Add(FDreamProjectFiles::PathKey(Listing.Folder));
					}
				},
				[](const TArray<FString>&, const FString&, bool) {});
			for (const FString& Folder : Folders)
			{
				Probe->Files->RequestListing(Folder);
			}
			TestEqual(TEXT("four listings run at once"), Probe->Files->GetListingsInFlightForTest(), FDreamProjectFiles::MaxListingsInFlight);
			TestEqual(TEXT("six wait in the queue"), Probe->Files->GetListingsQueuedForTest(), 10 - FDreamProjectFiles::MaxListingsInFlight);
			Probe->Files->RequestListing(Folders.Last());
			TestEqual(TEXT("a queued folder asked again is queued once"), Probe->Files->GetListingsQueuedForTest(), 10 - FDreamProjectFiles::MaxListingsInFlight);
			return true;
		});
		Steps.Poll(TEXT("all ten listed"), [Probe]()
		{
			return Probe->Listed.Num() == 10 && Probe->Files->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("each once; four network folders and two local ones"), [this, Probe]()
		{
			TestEqual(TEXT("ten listings started, no more"), Probe->Files->GetListingsStartedForTest(), 10);
			// Review p3 m1: network folders have two slots of their own; local folders never wait behind them.
			const FString Net = Temp / TEXT("Many") / TEXT("Net");
			FDreamProjectFiles::SetRemoteForTest(Net);
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FString Folder = Net / FString::Printf(TEXT("R%d"), Index);
				IFileManager::Get().MakeDirectory(*Folder, true);
				Probe->Files->RequestListing(Folder);
			}
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const FString Folder = Temp / TEXT("Many") / FString::Printf(TEXT("L%d"), Index);
				IFileManager::Get().MakeDirectory(*Folder, true);
				Probe->Files->RequestListing(Folder);
			}
			TestEqual(TEXT("two network listings run"), Probe->Files->GetRemoteListingsInFlightForTest(), FDreamProjectFiles::MaxRemoteListingsInFlight);
			TestEqual(TEXT("two network folders wait"), Probe->Files->GetRemoteListingsQueuedForTest(), 4 - FDreamProjectFiles::MaxRemoteListingsInFlight);
			TestEqual(TEXT("both local folders run beside them"), Probe->Files->GetListingsInFlightForTest() - Probe->Files->GetRemoteListingsInFlightForTest(), 2);
			TestEqual(TEXT("no local folder waits"), Probe->Files->GetListingsQueuedForTest() - Probe->Files->GetRemoteListingsQueuedForTest(), 0);
			return true;
		});
		Steps.Poll(TEXT("all sixteen listed"), [Probe]()
		{
			return Probe->Listed.Num() == 16 && Probe->Files->IsIdleForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("the missing root counts as a network root"), [this, Probe]()
		{
			FDreamProjectFiles::SetRemoteForTest(FString());
			Probe->Files->Shutdown();
			Probe->Files.Reset();
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			TestEqual(TEXT("a missing local root is looked at again 2 s after each answer"), Tree()->GetRetryDelayForTest(Missing), SDreamProjectTree::RetrySeconds);
			FDreamProjectFiles::SetRemoteForTest(Missing);
			Tree()->RefreshRoots();
			TestTrue(TEXT("RefreshRoots looks at a kept root's network state again (p2 nit)"), Tree()->IsRemoteForTest(Missing));
			return true;
		});
		Steps.Poll(TEXT("the missing network root waits twice as long after its second miss"), [this]()
		{
			return Tree()->GetRetryDelayForTest(Missing) >= 2.0 * SDreamProjectTree::RetrySeconds ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 3.0 * SDreamProjectTree::RetrySeconds + 3.0);
		Steps.Do(TEXT("never past the cap; local again"), [this]()
		{
			TestTrue(TEXT("the wait stays within 60 s"), Tree()->GetRetryDelayForTest(Missing) <= SDreamProjectTree::MaxRemoteRetrySeconds);
			FDreamProjectFiles::SetRemoteForTest(FString());
			Tree()->RefreshRoots();
			TestFalse(TEXT("a local root again"), Tree()->IsRemoteForTest(Missing));
			return true;
		});
		Steps.Poll(TEXT("a missing local root goes back to 2 s"), [this]()
		{
			return FMath::IsNearlyEqual(Tree()->GetRetryDelayForTest(Missing), SDreamProjectTree::RetrySeconds) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Run(Done);
	});

	LatentIt("starts hidden without listing, follows a reload of the roots, and tells an open preferences page of Add Root (p2 nits)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		AddWaitTree(Steps, TEXT("roots"));
		Steps.Do(TEXT("a tree made hidden lists nothing until shown"), [this]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			const TSharedRef<SDreamProjectTree> Hidden = SNew(SDreamProjectTree, Tab).StartActive(false);
			TestFalse(TEXT("it starts inactive"), Hidden->IsActive());
			TestEqual(TEXT("no listing at start"), Hidden->GetFilesForTest()->GetListingsStartedForTest(), 0);
			TestEqual(TEXT("and no watch"), Hidden->GetFilesForTest()->GetWatchCountForTest(), 0);
			Hidden->SetActive(true);
			TestTrue(TEXT("shown, it lists its roots"), Hidden->GetFilesForTest()->GetListingsStartedForTest() + Hidden->GetFilesForTest()->GetListingsQueuedForTest() > 0);
			return true;
		});
		AddWaitTree(Steps, TEXT("after the hidden tree"));
		Steps.Do(TEXT("Reset to Defaults reloads the roots' config: the tree follows"), [this]()
		{
			// FSettingsSection::ResetDefaults empties the section and reloads it; the reload alone (memory = disk here, nothing
			// written) must reach the tree.
			const int32 Before = Tree()->GetRootsRefreshCountForTest();
			UDreamProjectTreeSettings::GetMutable()->ReloadConfig();
			TestTrue(TEXT("the tree rebuilt its top level on the reload"), Tree()->GetRootsRefreshCountForTest() > Before);
			TestEqual(TEXT("the reload read the same roots"), FString::Join(RootPaths(), TEXT(";")), FString::Join(Saved.Roots, TEXT(";")));
			return true;
		});
		Steps.Do(TEXT("Add Root and Remove Root on the settings (no ini written) notify an open details view"), [this]()
		{
			const FString RootC = Temp / TEXT("RootC");
			IFileManager::Get().MakeDirectory(*RootC, true);
			TArray<FString> DiskBefore;
			GConfig->GetArray(TEXT("/Script/DreamEditor.DreamProjectTreeSettings"), TEXT("ProjectRoots"), DiskBefore, GEditorPerProjectIni);
			int32 Notified = 0;
			const UObject* SettingsObject = UDreamProjectTreeSettings::Get();
			const FDelegateHandle Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
				[&Notified, SettingsObject](UObject* Object, FPropertyChangedEvent& Event)
				{
					if (Object == SettingsObject && Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, ProjectRoots))
					{
						++Notified;
					}
				});
			FDreamProjectFiles::SetRootsForTest(nullptr);
			FDreamProjectFiles::SetSettingsSaveOffForTest(true);
			const bool bAdded = Tree()->AddRoot(RootC);
			const bool bInMemory = RootPaths().Contains(RootC);
			const bool bRow = FindRow(Tree()->GetRowsForTest(), RootC) != nullptr;
			const int32 AfterAdd = Notified;
			const bool bRemoved = Tree()->RemoveRoot(RootC);
			const bool bGone = !RootPaths().Contains(RootC) && FindRow(Tree()->GetRowsForTest(), RootC) == nullptr;
			FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
			FDreamProjectFiles::SetSettingsSaveOffForTest(false);
			SetRoots(Saved.Roots);   // memory as found (the AfterEach restore does it too)
			const TArray<FString> Roots = { RootA, RootBRelative, Missing };
			FDreamProjectFiles::SetRootsForTest(&Roots);
			Tree()->RefreshRoots();
			TestTrue(TEXT("Add Root adds to the settings in memory"), bAdded && bInMemory);
			TestTrue(TEXT("the tree shows it at once"), bRow);
			TestEqual(TEXT("the property-changed notification a details view gets, once for Add Root"), AfterAdd, 1);
			TestTrue(TEXT("Remove Root takes it out again"), bRemoved && bGone);
			TestEqual(TEXT("and notifies once more"), Notified, 2);
			TArray<FString> DiskAfter;
			GConfig->GetArray(TEXT("/Script/DreamEditor.DreamProjectTreeSettings"), TEXT("ProjectRoots"), DiskAfter, GEditorPerProjectIni);
			TestEqual(TEXT("nothing was written to the per-project ini"), FString::Join(DiskAfter, TEXT(";")), FString::Join(DiskBefore, TEXT(";")));
			return true;
		});
		Steps.Run(Done);
	});
	LatentIt("shows the plugins' Python folders under Project Plugins and Engine, follows a mount and its removal, and changes nothing under the engine (PO-05-A1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		const FString MountRoot = TEXT("/DreamT4Mount/");
		const FString MountContent = Temp / TEXT("MountPlugin") / TEXT("Content");
		const FString MountPython = MountContent / TEXT("Python");
		const FString EnginePython = FPaths::ConvertRelativePathToFull(FPaths::EnginePluginsDir() / TEXT("Experimental") / TEXT("PythonScriptPlugin") / TEXT("Content") / TEXT("Python"));
		TSharedRef<FString> DreamPython = MakeShared<FString>();
		TSharedRef<bool> bMounted = MakeShared<bool>(false);
		Steps.Do(TEXT("plugin folders on (the case's value, no setting)"), [this, MountPython, DreamPython]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			DreamSideBarSpec::WriteText(MountPython / TEXT("mounted_tool.py"), TEXT("x = 1\n"));
			const bool bOn = true;
			FDreamProjectFiles::SetPluginFoldersForTest(&bOn);
			Tree()->RefreshRoots();
			for (const FDreamProjectRoot& Root : FDreamProjectFiles::ResolveAutomaticRoots())
			{
				if (Root.Label == TEXT("Dream Python Editor"))
				{
					*DreamPython = Root.Path;
				}
			}
			return TestFalse(TEXT("the Dream Python Editor plugin's own Python folder is an automatic root"), DreamPython->IsEmpty());
		});
		AddWaitTree(Steps, TEXT("plugin folders asked about on a worker"));
		TSharedRef<int32> EngineListings = MakeShared<int32>(0);
		Steps.Do(TEXT("the Engine group closed: an entry expanded under it"), [this, EnginePython, EngineListings]()
		{
			TestTrue(TEXT("Engine shows, closed"), Tree()->SetGroupExpandedForTest(/*bEngine*/ true, false));
			*EngineListings = Tree()->GetFilesForTest()->GetListingsOfForTest(EnginePython);
			return TestTrue(TEXT("the engine entry is expanded while its group is closed"), Tree()->SetExpandedForTest(EnginePython, true));
		});
		Steps.WaitFrames(3);
		AddWaitTree(Steps, TEXT("closed group"));
		Steps.Do(TEXT("not listed while closed (review p3 m2); the group opens"), [this, EnginePython, EngineListings]()
		{
			TestEqual(TEXT("no listing of an entry under a closed group"), Tree()->GetFilesForTest()->GetListingsOfForTest(EnginePython), *EngineListings);
			Tree()->SetGroupExpandedForTest(/*bEngine*/ true, true);
			return true;
		});
		AddWaitTree(Steps, TEXT("the group opened"));
		Steps.Do(TEXT("the groups, between the roots and Scratches"), [this, EnginePython, DreamPython, EngineListings]()
		{
			TestTrue(TEXT("opening the group lists its expanded entry"), Tree()->GetFilesForTest()->GetListingsOfForTest(EnginePython) > *EngineListings);
			TestTrue(TEXT("Project Plugins shows"), Tree()->SetGroupExpandedForTest(/*bEngine*/ false, true));
			TestTrue(TEXT("Engine shows"), Tree()->SetGroupExpandedForTest(/*bEngine*/ true, true));
			const TArray<FRow> Rows = Tree()->GetRowsForTest();
			TArray<FString> Top;
			for (const FRow& Row : Rows)
			{
				if (Row.Depth == 0)
				{
					Top.Add(Row.Kind == EKind::Group ? FString::Printf(TEXT("[%s]"), *Row.Label) : FPaths::GetCleanFilename(Row.Path));
				}
			}
			const FString Order = FString::Join(Top, TEXT(" | "));
			AddInfo(FString::Printf(TEXT("top level: %s"), *Order));
			TestTrue(TEXT("the user's roots, then Project Plugins, Engine, Scratches"), Order.EndsWith(TEXT("[Project Plugins] | [Engine] | Scratches")) && Order.StartsWith(TEXT("RootA | RootB")));
			const FRow* Dream = FindRow(Rows, *DreamPython);
			TestTrue(TEXT("Project Plugins holds Dream Python Editor's Python folder"), Dream != nullptr && Dream->Depth == 1 && Dream->Label == TEXT("Dream Python Editor"));
			const FRow* Python = FindRow(Rows, EnginePython);
			TestTrue(TEXT("Engine holds the Python Editor Script Plugin's folder"), Python != nullptr && Python->Depth == 1 && Python->Label == TEXT("Python Editor Script Plugin"));
			TestFalse(TEXT("Engine/Content/Python does not exist here, so no Engine entry for it"),
				Rows.ContainsByPredicate([](const FRow& Row) { return Row.Depth == 1 && Row.Label == TEXT("Engine"); }));
			auto Names = [this](const FString& Path)
			{
				TArray<FString> Out;
				for (const TPair<FName, bool>& Entry : Tree()->GetContextMenuForTest(Path))
				{
					Out.Add(Entry.Key.ToString().RightChop(FCString::Strlen(TEXT("Dream.ProjectTree."))));
				}
				return FString::Join(Out, TEXT(","));
			};
			TestEqual(TEXT("a project plugin's entry: no Remove Root"), Names(*DreamPython), FString(TEXT("NewFile,NewFolder,Refresh,Reveal,CopyPath")));
			TestEqual(TEXT("an engine entry: nothing added, renamed or removed"), Names(EnginePython), FString(TEXT("Refresh,Reveal,CopyPath")));
			TestTrue(TEXT("an engine entry is protected"), Tree()->IsProtectedForTest(EnginePython));
			TestTrue(TEXT("open the engine entry"), Tree()->SetExpandedForTest(EnginePython, true));
			return true;
		});
		AddWaitTree(Steps, TEXT("the engine entry listed"));
		Steps.Do(TEXT("nothing under the engine is renamed, deleted or added to"), [this, EnginePython]()
		{
			const FString File = EnginePython / TEXT("remote_execution.py");
			if (!TestTrue(TEXT("remote_execution.py shows under it"), FindRow(Tree()->GetRowsForTest(), File) != nullptr))
			{
				return false;
			}
			TestTrue(TEXT("a file under the engine is protected"), Tree()->IsProtectedForTest(File));
			Tree()->BeginRename(File);
			TestFalse(TEXT("Rename is refused"), Tree()->IsEditingForTest());
			TestTrue(TEXT("with the engine message"), Tree()->GetMessageForTest().ToString().Contains(TEXT("part of the engine")));
			Tree()->RequestDelete(File);
			TestTrue(TEXT("Delete is refused"), Tree()->GetPendingDeleteForTest().IsEmpty());
			Tree()->BeginNewItem(EnginePython, /*bFolder*/ false);
			TestFalse(TEXT("New Python File is refused"), Tree()->IsEditingForTest());
			TestTrue(TEXT("and the file is untouched"), IFileManager::Get().FileExists(*File));
			return true;
		});
		Steps.Do(TEXT("a content path mounted"), [MountRoot, MountContent, bMounted]()
		{
			(void)FPackageName::RegisterMountPoint(MountRoot, MountContent + TEXT("/"));
			*bMounted = true;
			return true;
		});
		Steps.Poll(TEXT("its Python folder shows under Project Plugins"), [this, MountPython]()
		{
			const TArray<FRow> Rows = Tree()->GetRowsForTest();   // FindRow points into it
			const FRow* Row = FindRow(Rows, MountPython);
			const bool bShown = Row != nullptr && Row->Depth == 1 && Row->Label == TEXT("DreamT4Mount");
			if (bShown)
			{
				AddInfo(FString::Printf(TEXT("the mount's folder: %s"), *Tree()->GetAutomaticStateForTest(MountPython)));
			}
			return bShown ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("and dismounted"), [MountRoot, MountContent, bMounted]()
		{
			FPackageName::UnRegisterMountPoint(MountRoot, MountContent + TEXT("/"));
			*bMounted = false;
			return true;
		});
		Steps.Poll(TEXT("it goes"), [this, MountPython]()
		{
			return FindRow(Tree()->GetRowsForTest(), MountPython) == nullptr ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("a user root equal to an automatic one shows once, as the user's"), [this, DreamPython]()
		{
			const TArray<FString> Roots = { RootA, *DreamPython };
			FDreamProjectFiles::SetRootsForTest(&Roots);
			Tree()->RefreshRoots();
			int32 Seen = 0;
			int32 Depth = -1;
			for (const FRow& Row : Tree()->GetRowsForTest())
			{
				if (Key(Row.Path) == Key(*DreamPython))
				{
					++Seen;
					Depth = Row.Depth;
				}
			}
			TestEqual(TEXT("once"), Seen, 1);
			TestEqual(TEXT("at the top level"), Depth, 0);
			return true;
		});
		Steps.Finally(TEXT("dismount if still mounted"), [MountRoot, MountContent, bMounted]()
		{
			if (*bMounted)
			{
				FPackageName::UnRegisterMountPoint(MountRoot, MountContent + TEXT("/"));
			}
		});
		Steps.Run(Done);
	});

	LatentIt("turns the plugin folders off and on live from the View menu, Editor Preferences and the tree's menu, all in step, writing no ini (PO-05-A1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		struct FToggleState
		{
			bool bBefore = true;
			bool bHadKey = false;
			FString DiskBefore;
		};
		TSharedRef<FToggleState> State = MakeShared<FToggleState>();
		auto GroupsShown = [this]()
		{
			return Tree()->GetRowsForTest().ContainsByPredicate([](const FRow& Row) { return Row.Kind == EKind::Group; });
		};
		auto ViewChecked = [this]()
		{
			for (const SDreamTab::FChromeEntry& Entry : Tab->GetMenuEntriesForTest(TEXT("View")))
			{
				if (Entry.Command == EDreamCommand::TogglePluginPythonFolders)
				{
					return Entry.Action.GetCheckState() == ECheckBoxState::Checked;
				}
			}
			AddError(TEXT("View has no Plugin Python Folders item"));
			return false;
		};
		auto MenuChecked = [this]()
		{
			return Tree()->GetContextMenuCheckForTest(FString(), TEXT("Dream.ProjectTree.PluginFolders")) == ECheckBoxState::Checked;
		};
		auto ExpectAll = [this, GroupsShown, ViewChecked, MenuChecked](const TCHAR* How, bool bOn)
		{
			TestEqual(*FString::Printf(TEXT("%s: the setting"), How), FDreamProjectFiles::IsAddPluginFolders(), bOn);
			TestEqual(*FString::Printf(TEXT("%s: the setting object"), How), UDreamProjectTreeSettings::Get()->bAddPluginPythonFolders, bOn);
			TestEqual(*FString::Printf(TEXT("%s: View > Side Bar > Plugin Python Folders"), How), ViewChecked(), bOn);
			TestEqual(*FString::Printf(TEXT("%s: the tree's background menu"), How), MenuChecked(), bOn);
			TestEqual(*FString::Printf(TEXT("%s: the plugin groups at once"), How), GroupsShown(), bOn);
		};
		auto EditInPreferences = [](bool bOn)
		{
			// What the Editor Preferences page does on a click: the value, then PostEditChangeProperty (its save is the page's).
			UDreamProjectTreeSettings* Settings = UDreamProjectTreeSettings::GetMutable();
			Settings->bAddPluginPythonFolders = bOn;
			FProperty* Property = FindFProperty<FProperty>(UDreamProjectTreeSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamProjectTreeSettings, bAddPluginPythonFolders));
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Settings->PostEditChangeProperty(Event);
		};
		Steps.Do(TEXT("the real setting, saving off; on"), [this, State]()
		{
			if (!Tree().IsValid())
			{
				AddError(TEXT("no Project tree"));
				return false;
			}
			State->bBefore = UDreamProjectTreeSettings::Get()->bAddPluginPythonFolders;
			State->bHadKey = GConfig->GetString(TEXT("/Script/DreamEditor.DreamProjectTreeSettings"), TEXT("bAddPluginPythonFolders"), State->DiskBefore, GEditorPerProjectIni);
			FDreamProjectFiles::SetPluginFoldersForTest(nullptr);
			FDreamProjectFiles::SetSettingsSaveOffForTest(true);
			FDreamProjectFiles::SetAddPluginFolders(true);
			Tree()->RefreshRoots();
			return true;
		});
		AddWaitTree(Steps, TEXT("plugin folders"));
		Steps.Do(TEXT("View menu off, then on"), [this, ExpectAll]()
		{
			ExpectAll(TEXT("at the start"), true);
			Tab->ExecuteDreamCommand(EDreamCommand::TogglePluginPythonFolders);
			ExpectAll(TEXT("View menu off"), false);
			Tab->ExecuteDreamCommand(EDreamCommand::TogglePluginPythonFolders);
			ExpectAll(TEXT("View menu on"), true);
			return true;
		});
		Steps.Do(TEXT("Editor Preferences off, then on"), [ExpectAll, EditInPreferences]()
		{
			EditInPreferences(false);
			ExpectAll(TEXT("Editor Preferences off"), false);
			EditInPreferences(true);
			ExpectAll(TEXT("Editor Preferences on"), true);
			return true;
		});
		Steps.Do(TEXT("the tree's menu off, then on"), [this, ExpectAll]()
		{
			TestTrue(TEXT("the background menu has the item"), Tree()->ExecuteContextMenuForTest(FString(), TEXT("Dream.ProjectTree.PluginFolders")));
			ExpectAll(TEXT("tree menu off"), false);
			Tree()->ExecuteContextMenuForTest(FString(), TEXT("Dream.ProjectTree.PluginFolders"));
			ExpectAll(TEXT("tree menu on"), true);
			return true;
		});
		Steps.Finally(TEXT("the setting as found, nothing written"), [this, State]()
		{
			UDreamProjectTreeSettings::GetMutable()->bAddPluginPythonFolders = State->bBefore;
			FDreamProjectFiles::SetSettingsSaveOffForTest(false);
			FString DiskAfter;
			const bool bHasKey = GConfig->GetString(TEXT("/Script/DreamEditor.DreamProjectTreeSettings"), TEXT("bAddPluginPythonFolders"), DiskAfter, GEditorPerProjectIni);
			TestTrue(TEXT("the per-project ini is as found"), bHasKey == State->bHadKey && DiskAfter == State->DiskBefore);
			const bool bOff = false;
			FDreamProjectFiles::SetPluginFoldersForTest(&bOff);
			if (Tree().IsValid())
			{
				Tree()->RefreshRoots();
			}
		});
		Steps.Run(Done);
	});
}
