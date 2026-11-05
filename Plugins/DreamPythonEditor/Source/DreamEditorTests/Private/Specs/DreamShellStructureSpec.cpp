// SPDX-License-Identifier: Apache-2.0
// Dream.Shell.Structure (structure group; owner ruling PO-05 §3; NAV-02 moved up from M2, T-NAV-005 without a server, TDD
// §7.9.2): the Structure section shows the outline of the active tab only: nested classes, methods, async defs and `# %%`
// cells; it is rebuilt 300 ms after the last edit on a worker and a result older than the buffer is dropped; it follows a
// switch of the active tab at once; a click, a double-click (which never collapses a parent) or Enter jumps to the entry's
// name and focuses the code view; the deepest entry holding the caret is highlighted; collapsed parents and the selection
// survive a rebuild, same-named siblings apart; typing opens the filter row and Esc clears it; an empty, entry-free or
// non-Python tab shows "No structure". Clicks are real pointer events routed along the row's widget path, to rows the
// spec first scrolls into the list (a short section shows only a few).
// -DreamStructureSplit=<f> (for example 0.9) sets the side bar's Project / Structure split to f / 1-f for this spec, in
// memory only: the side bar's own split value and its setting are never written, and the splitter's bindings to it are
// put back after each case, so the user's layout is as it was.
// The view is the one the side bar hosts in the Dream tab when it is shown; otherwise the spec hosts its own
// SDreamStructureView over the same tab in an off-screen window. Buffers are in memory (FDreamBufferManager::CreateScratch,
// AddRestored for the non-Python tab), so nothing is written to disk.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Shell/SDreamSideBar.h"
#include "Shell/SDreamStructureView.h"
#include "Shell/SDreamTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SWindow.h"

namespace DreamShellStructureSpec
{
	// Entries: 0 # Setup, 1 C Outer, 2 C Inner, 3 m method, 4 f helper, 5 am fetch, 6 # Main, 7 af main.
	const TCHAR* const Sample =
		TEXT("# %% Setup\n")                     // 0
		TEXT("import os\n")                      // 1
		TEXT("\n")                               // 2
		TEXT("class Outer:\n")                   // 3
		TEXT("    class Inner:\n")               // 4
		TEXT("        def method(self):\n")      // 5
		TEXT("            def helper():\n")      // 6
		TEXT("                return 1\n")       // 7
		TEXT("            return helper()\n")    // 8
		TEXT("\n")                               // 9
		TEXT("    async def fetch(self):\n")     // 10
		TEXT("        pass\n")                   // 11
		TEXT("\n")                               // 12
		TEXT("# %% Main\n")                      // 13
		TEXT("@decorator\n")                     // 14
		TEXT("async def main():\n")              // 15
		TEXT("    pass\n")                       // 16
		TEXT("\n")                               // 17
		TEXT("x = 1\n");                         // 18

	const TCHAR* const SampleRows =
		TEXT("# Setup L1 d0 | C Outer L4 d1 | C Inner L5 d2 | m method L6 d3 | f helper L7 d4 | am fetch L11 d2 | ")
		TEXT("# Main L14 d0 | af main L16 d1");

	const TCHAR* KindText(dream::OutlineKind Kind)
	{
		switch (Kind)
		{
		case dream::OutlineKind::Class: return TEXT("C");
		case dream::OutlineKind::Function: return TEXT("f");
		case dream::OutlineKind::Method: return TEXT("m");
		case dream::OutlineKind::AsyncFunction: return TEXT("af");
		case dream::OutlineKind::AsyncMethod: return TEXT("am");
		case dream::OutlineKind::Cell: return TEXT("#");
		}
		return TEXT("?");
	}

	/** "<kind> <name> L<1-based line> d<depth>" per shown row, " | " between; a parent shown only for a match gets "(p)". */
	FString RowsText(const SDreamStructureView& View)
	{
		TArray<FString> Parts;
		for (const SDreamStructureView::FRowForTest& Row : View.GetRowsForTest())
		{
			Parts.Add(FString::Printf(TEXT("%s %s L%d d%d%s"), KindText(Row.Kind), *Row.Name, Row.Line + 1, Row.Depth, Row.bMatch ? TEXT("") : TEXT(" (p)")));
		}
		return FString::Join(Parts, TEXT(" | "));
	}

	/** A shown SDreamStructureView under Root (every ancestor visible, laid out with a size). */
	TSharedPtr<SDreamStructureView> FindShownView(const TSharedRef<SWidget>& Root)
	{
		if (!Root->GetVisibility().IsVisible())
		{
			return nullptr;
		}
		if (Root->GetType() == FName(TEXT("SDreamStructureView")))
		{
			const FVector2D Size = Root->GetTickSpaceGeometry().GetLocalSize();
			return (Size.X > 0.0 && Size.Y > 0.0) ? StaticCastSharedRef<SDreamStructureView>(Root).ToSharedPtr() : nullptr;
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SDreamStructureView> Found = FindShownView(Children->GetChildAt(Index)))
			{
				return Found;
			}
		}
		return nullptr;
	}

	bool FocusIsInside(const TSharedPtr<SWidget>& Root)
	{
		for (TSharedPtr<SWidget> Widget = FSlateApplication::Get().GetKeyboardFocusedWidget(); Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget == Root)
			{
				return true;
			}
		}
		return false;
	}

	void PressKey(const FKey& Key)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const uint32 User = App.GetUserIndexForKeyboard();
		App.ProcessKeyDownEvent(FKeyEvent(Key, FModifierKeysState(), User, false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, FModifierKeysState(), User, false, 0, 0));
	}

	/**
	 * A left click at Widget's centre (and with bDouble the double-click after it), routed along its widget path as Slate
	 * routes platform pointer events: move, down, up (, double-click, up). The Process* entry points hit-test under the
	 * real OS cursor, which a test cannot place (DreamTest::MakePointerPath).
	 * The row stays hovered afterwards (no pointer move leaves it), so its GetBorder() shows the hover brush, not the
	 * caret highlight, until the real cursor moves: check the highlight through GetHighlightedEntry, not pixels.
	 */
	bool Click(const TSharedPtr<SWidget>& Widget, bool bDouble)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!Widget.IsValid() || !DreamTest::MakePointerPath(Widget.ToSharedRef(), Path))
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
		if (bDouble)
		{
			FWidgetPath Again;
			if (!DreamTest::MakePointerPath(Widget.ToSharedRef(), Again))
			{
				return false;
			}
			App.RoutePointerDoubleClickEvent(Again, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			App.RoutePointerUpEvent(Again, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		}
		App.ReleaseAllPointerCapture();
		return true;
	}

	/** The row widget of entry Entry, generated and laid out with a size (null until then). */
	TSharedPtr<SWidget> LaidOutRow(const SDreamStructureView& View, int32 Entry)
	{
		const TSharedPtr<SWidget> Row = View.GetRowWidgetForTest(Entry);
		const FVector2D Size = Row.IsValid() ? FVector2D(Row->GetPaintSpaceGeometry().GetLocalSize()) : FVector2D::ZeroVector;
		return (Size.X > 0.0 && Size.Y > 0.0) ? Row : nullptr;
	}

	/** The caret of Buffer's code view as "line:column" (0-based). */
	FString CaretText(SDreamTab& Tab, const FDreamBuffer& Buffer)
	{
		const TSharedPtr<SDreamCodeView> Code = Tab.FindCodeView(Buffer.GetId());
		int32 Line = -1;
		int32 Column = -1;
		if (Code.IsValid())
		{
			Code->GetCaret(Line, Column);
		}
		return FString::Printf(TEXT("%d:%d"), Line, Column);
	}

	/** The nearest ancestor of Widget whose type is Type, or null. */
	TSharedPtr<SWidget> FindAncestor(const TSharedPtr<SWidget>& Widget, const TCHAR* Type)
	{
		for (TSharedPtr<SWidget> At = Widget.IsValid() ? Widget->GetParentWidget() : nullptr; At.IsValid(); At = At->GetParentWidget())
		{
			if (At->GetType() == FName(Type))
			{
				return At;
			}
		}
		return nullptr;
	}

	/** The index of Splitter's slot that holds Widget, or INDEX_NONE. */
	int32 SlotHolding(SSplitter& Splitter, TSharedPtr<SWidget> Widget)
	{
		for (; Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget->GetParentWidget().Get() != &Splitter)
			{
				continue;
			}
			FChildren* Children = Splitter.GetChildren();
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (&Children->GetChildAt(Index).Get() == Widget.Get())
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
		return INDEX_NONE;
	}

	void TypeText(const FString& Text)
	{
		FSlateApplication& App = FSlateApplication::Get();
		for (const TCHAR Ch : Text)
		{
			App.ProcessKeyCharEvent(FCharacterEvent(Ch, FModifierKeysState(), App.GetUserIndexForKeyboard(), false));
		}
	}
}

BEGIN_DEFINE_SPEC(FDreamShellStructureSpec, "Dream.Shell.Structure", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TSharedPtr<SDreamStructureView> View;
	TSharedPtr<SWindow> Window;
	TArray<FGuid> Opened;
	/** -DreamStructureSplit: the side bar's section splitter while this spec overrides its two slot values in memory. */
	TWeakPtr<SSplitter> SplitSplitter;
	TWeakPtr<SDreamSideBar> SplitSideBar;
	int32 SplitStructureSlot = INDEX_NONE;
	float SplitFractionBefore = 0.f;

	/** Sets the side bar's section split from -DreamStructureSplit=<f> (in memory only); nothing without the switch. */
	void ApplySplitOverride();
	/** Puts the splitter's slots back on the side bar's own split value (the bindings it builds them with). */
	void RestoreSplitOverride();

	/** A scratch buffer in memory with Text, made the active tab. */
	TSharedPtr<FDreamBuffer> OpenScratch(const FString& Text, const FString& Purpose);
	/** The view shows Buffer at its current version. */
	bool Shows(const TSharedPtr<FDreamBuffer>& Buffer) const;
	/** A step that waits until the view shows Buffer at its current version. */
	void AddWaitShows(DreamTest::FDreamSteps& Steps, const FString& Name, TSharedRef<TSharedPtr<FDreamBuffer>> Buffer, double TimeoutSeconds = 5.0);
END_DEFINE_SPEC(FDreamShellStructureSpec)

void FDreamShellStructureSpec::ApplySplitOverride()
{
	float Fraction = 0.f;
	if (!View.IsValid() || !FParse::Value(FCommandLine::Get(), TEXT("-DreamStructureSplit="), Fraction))
	{
		return;
	}
	const TSharedPtr<SWidget> SideBarWidget = DreamShellStructureSpec::FindAncestor(View, TEXT("SDreamSideBar"));
	TSharedPtr<SSplitter> Splitter;
	for (TSharedPtr<SWidget> At = View->GetParentWidget(); At.IsValid() && At != SideBarWidget; At = At->GetParentWidget())
	{
		if (At->GetType() == FName(TEXT("SSplitter")) && StaticCastSharedPtr<SSplitter>(At)->GetOrientation() == Orient_Vertical)
		{
			Splitter = StaticCastSharedPtr<SSplitter>(At);
			break;
		}
	}
	const int32 Slot = Splitter.IsValid() ? DreamShellStructureSpec::SlotHolding(*Splitter, View) : INDEX_NONE;
	if (!SideBarWidget.IsValid() || !Splitter.IsValid() || Splitter->GetChildren()->Num() != 2 || Slot == INDEX_NONE)
	{
		AddWarning(TEXT("-DreamStructureSplit: the Structure section is not in the side bar's two-slot splitter; the split is not changed"));
		return;
	}
	const TSharedPtr<SDreamSideBar> SideBar = StaticCastSharedPtr<SDreamSideBar>(SideBarWidget);
	Fraction = FMath::Clamp(Fraction, 0.1f, 0.9f);
	SplitSplitter = Splitter;
	SplitSideBar = SideBar;
	SplitStructureSlot = Slot;
	SplitFractionBefore = SideBar->GetProjectFraction();
	// Plain values for this case; SSplitter calls OnSlotResized only for a drag, so the side bar's value and setting stay.
	Splitter->SlotAt(Slot).SetSizeValue(1.f - Fraction);
	Splitter->SlotAt(1 - Slot).SetSizeValue(Fraction);
	AddInfo(FString::Printf(TEXT("-DreamStructureSplit: the Structure section gets %.0f %% of the side bar for this case"), (1.f - Fraction) * 100.f));
}

void FDreamShellStructureSpec::RestoreSplitOverride()
{
	const TSharedPtr<SSplitter> Splitter = SplitSplitter.Pin();
	const TSharedPtr<SDreamSideBar> SideBar = SplitSideBar.Pin();
	if (Splitter.IsValid() && SideBar.IsValid() && SplitStructureSlot != INDEX_NONE)
	{
		// The side bar builds the slots on its own value (Project: ProjectFraction, Structure: 1 - ProjectFraction).
		const TWeakPtr<SDreamSideBar> Weak = SideBar;
		auto Project = [Weak]()
		{
			const TSharedPtr<SDreamSideBar> Pinned = Weak.Pin();
			return Pinned.IsValid() ? Pinned->GetProjectFraction() : SDreamSideBar::DefaultProjectFraction;
		};
		Splitter->SlotAt(1 - SplitStructureSlot).SetSizeValue(TAttribute<float>::CreateLambda(Project));
		Splitter->SlotAt(SplitStructureSlot).SetSizeValue(TAttribute<float>::CreateLambda([Project]() { return 1.f - Project(); }));
		TestEqual(TEXT("-DreamStructureSplit: the side bar's own split value is unchanged"), SideBar->GetProjectFraction(), SplitFractionBefore);
	}
	SplitSplitter.Reset();
	SplitSideBar.Reset();
	SplitStructureSlot = INDEX_NONE;
}

TSharedPtr<FDreamBuffer> FDreamShellStructureSpec::OpenScratch(const FString& Text, const FString& Purpose)
{
	if (!Tab.IsValid())
	{
		return nullptr;
	}
	TSharedRef<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().CreateScratch(Text, DreamTest::ScratchName(Purpose));
	Opened.Add(Buffer->GetId());
	Tab->ActivateBuffer(Buffer->GetId());
	return Buffer;
}

bool FDreamShellStructureSpec::Shows(const TSharedPtr<FDreamBuffer>& Buffer) const
{
	return View.IsValid() && Buffer.IsValid() && View->GetShownBufferId() == Buffer->GetId()
		&& View->GetShownVersion() == Buffer->GetModel().Version();
}

void FDreamShellStructureSpec::AddWaitShows(DreamTest::FDreamSteps& Steps, const FString& Name, TSharedRef<TSharedPtr<FDreamBuffer>> Buffer, double TimeoutSeconds)
{
	Steps.Poll(Name, [this, Buffer]()
	{
		return Shows(*Buffer) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, TimeoutSeconds);
}

void FDreamShellStructureSpec::Define()
{
	using namespace DreamShellStructureSpec;

	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			return;
		}
		View = FindShownView(Tab.ToSharedRef());
		if (View.IsValid())
		{
			AddInfo(TEXT("using the Structure section the side bar hosts in the Dream tab"));
			ApplySplitOverride();
			return;
		}
		// No side bar shows one (not built yet, or View > Structure is off): the spec hosts its own over the same tab.
		View = SNew(SDreamStructureView, TWeakPtr<SDreamTab>(Tab));
		Window = SNew(SWindow)
			.ClientSize(FVector2D(260.0, 420.0))
			.ScreenPosition(FVector2D(-20000.0, -20000.0))
			.AutoCenter(EAutoCenter::None)
			.SizingRule(ESizingRule::FixedSize)
			.CreateTitleBar(false)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.ActivationPolicy(EWindowActivationPolicy::Never)
			.FocusWhenFirstShown(false)
			[
				View.ToSharedRef()
			];
		FSlateApplication::Get().AddWindow(Window.ToSharedRef(), /*bShowImmediately=*/true);
		AddInfo(TEXT("no shown Structure section in the Dream tab: hosted in an off-screen test window"));
	});

	AfterEach([this]()
	{
		RestoreSplitOverride();
		if (View.IsValid())
		{
			// The side bar's view outlives the spec: leave it as it was found.
			View->SetHoldResultsForTest(false);
			View->ClearFilterForTest();
		}
		for (const FGuid& Id : Opened)
		{
			DreamTest::CloseBuffer(Id);
		}
		Opened.Reset();
		if (Window.IsValid())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			Window.Reset();
		}
		View.Reset();
		Tab.Reset();
	});

	LatentIt("lists nested classes, methods, async defs and cells of the active tab (NAV-02, T-NAV-005)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		DreamTest::FDreamSteps Steps(*this);
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		Steps.Do(TEXT("open the sample"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return View.IsValid() && Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the view shows the sample"), Buffer);
		Steps.Do(TEXT("check the rows"), [this]()
		{
			TestEqual(TEXT("rows (kind, name, line, depth)"), DreamShellStructureSpec::RowsText(*View), FString(DreamShellStructureSpec::SampleRows));
			TestFalse(TEXT("\"No structure\" is not shown"), View->IsShowingNoStructure());
			const std::vector<dream::OutlineEntry>& Entries = View->GetEntries();
			if (TestEqual(TEXT("entries"), static_cast<int32>(Entries.size()), 8))
			{
				TestEqual(TEXT("main's decorator folds into it (first line)"), Entries[7].FirstLine, 14);
				TestEqual(TEXT("helper's name column"), Entries[4].Column, 16);
				TestEqual(TEXT("cell Setup ends before cell Main"), Entries[0].EndLine, 11);
			}
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("rebuilds 300 ms after the last edit and drops a result older than the buffer", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FState
		{
			TSharedPtr<FDreamBuffer> Buffer;
			int32 Edits = 0;
			double LastEdit = 0.0;
			double MaxGap = 0.0;
			double LastPoll = 0.0;
			int32 AppliedBefore = 0;
			int32 WorkersBefore = 0;
			int32 DroppedBefore = 0;
		};
		TSharedRef<FState> State = MakeShared<FState>();
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open a short buffer"), [this, Buffer, State]()
		{
			*Buffer = OpenScratch(TEXT("class A:\n    def a(self):\n        pass\n"), TEXT("Structure"));
			State->Buffer = *Buffer;
			return View.IsValid() && Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the first outline"), Buffer);
		// Three edits 100 ms apart: one rebuild, 300 ms after the last.
		Steps.Poll(TEXT("three edits 100 ms apart"), [this, State]()
		{
			const double Now = FPlatformTime::Seconds();
			if (State->LastPoll > 0.0)
			{
				State->MaxGap = FMath::Max(State->MaxGap, Now - State->LastPoll);
			}
			State->LastPoll = Now;
			if (State->Edits > 0 && View->GetAppliedCountForTest() != State->AppliedBefore && State->MaxGap < 0.25)
			{
				AddError(FString::Printf(TEXT("an outline was applied %.3f s after an edit, before the 300 ms quiet time"), Now - State->LastEdit));
				return DreamTest::EStep::Fail;
			}
			if (State->Edits > 0 && Now - State->LastEdit < 0.1)
			{
				return DreamTest::EStep::Wait;
			}
			const TSharedPtr<SDreamCodeView> Code = Tab->FindCodeView(State->Buffer->GetId());
			if (!Code.IsValid())
			{
				AddError(TEXT("the buffer has no code view"));
				return DreamTest::EStep::Fail;
			}
			if (State->Edits == 0)
			{
				State->AppliedBefore = View->GetAppliedCountForTest();
				State->WorkersBefore = View->GetWorkerBuildsForTest();
			}
			++State->Edits;
			FString Text = TEXT("class A:\n    def a(self):\n        pass\n");
			for (int32 Index = 1; Index <= State->Edits; ++Index)
			{
				Text += FString::Printf(TEXT("\ndef added_%d():\n    pass\n"), Index);
			}
			if (!Code->ReplaceAllText(Text, FText::GetEmpty()))
			{
				AddError(TEXT("ReplaceAllText refused the edit"));
				return DreamTest::EStep::Fail;
			}
			State->LastEdit = Now;
			return State->Edits == 3 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.Poll(TEXT("the rebuild lands 300 ms after the last edit"), [this, State]()
		{
			const double Now = FPlatformTime::Seconds();
			State->MaxGap = FMath::Max(State->MaxGap, Now - State->LastPoll);
			State->LastPoll = Now;
			if (!Shows(State->Buffer))
			{
				if (View->GetAppliedCountForTest() != State->AppliedBefore && State->MaxGap < 0.25)
				{
					AddError(TEXT("an intermediate outline was applied during the edits"));
					return DreamTest::EStep::Fail;
				}
				return DreamTest::EStep::Wait;
			}
			const double Quiet = Now - State->LastEdit;
			TestTrue(*FString::Printf(TEXT("applied %.3f s after the last edit (>= 0.3 s)"), Quiet), Quiet >= SDreamStructureView::RebuildDelaySeconds - 0.02);
			if (State->MaxGap < 0.25)
			{
				TestEqual(TEXT("one outline applied for the three edits"), View->GetAppliedCountForTest() - State->AppliedBefore, 1);
				TestEqual(TEXT("one worker build for the three edits"), View->GetWorkerBuildsForTest() - State->WorkersBefore, 1);
			}
			else
			{
				AddInfo(FString::Printf(TEXT("a frame took %.3f s during the edits: the one-build count is not checked"), State->MaxGap));
			}
			TestEqual(TEXT("rows after the edits"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("C A L1 d0 | m a L2 d1 | f added_1 L5 d0 | f added_2 L8 d0 | f added_3 L11 d0")));
			return DreamTest::EStep::Next;
		}, 10.0);
		// A worker result that the buffer has moved past is dropped, and the next build shows the newest text.
		Steps.Poll(TEXT("hold the next result, edit, wait for the worker"), [this, State]()
		{
			if (State->Edits == 3)
			{
				View->SetHoldResultsForTest(true);
				State->DroppedBefore = View->GetDroppedCountForTest();
				State->AppliedBefore = View->GetAppliedCountForTest();
				Tab->FindCodeView(State->Buffer->GetId())->ReplaceAllText(TEXT("def stale():\n    pass\n"), FText::GetEmpty());
				State->Edits = 4;
				return DreamTest::EStep::Wait;
			}
			return View->IsBuildDoneForTest() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("edit again while the result is held, then release it"), [this, State]()
		{
			Tab->FindCodeView(State->Buffer->GetId())->ReplaceAllText(TEXT("def fresh():\n    pass\nclass Newest:\n    pass\n"), FText::GetEmpty());
			View->SetHoldResultsForTest(false);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the stale result was dropped"), [this, State]()
		{
			TestEqual(TEXT("dropped results"), View->GetDroppedCountForTest() - State->DroppedBefore, 1);
			TestEqual(TEXT("nothing applied from the stale build"), View->GetAppliedCountForTest(), State->AppliedBefore);
			TestFalse(TEXT("the stale text's entry is not shown"), DreamShellStructureSpec::RowsText(*View).Contains(TEXT("stale")));
			return true;
		});
		AddWaitShows(Steps, TEXT("the newest text is shown"), Buffer);
		Steps.Do(TEXT("rows of the newest text"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(TEXT("f fresh L1 d0 | C Newest L3 d0")));
			return true;
		});
		Steps.Finally(TEXT("release the hold"), [this]()
		{
			if (View.IsValid())
			{
				View->SetHoldResultsForTest(false);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("follows a switch of the active tab at once", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> First = MakeShared<TSharedPtr<FDreamBuffer>>();
		TSharedRef<TSharedPtr<FDreamBuffer>> Second = MakeShared<TSharedPtr<FDreamBuffer>>();
		TSharedRef<int32> WorkersBefore = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open two tabs"), [this, First, Second]()
		{
			*First = OpenScratch(DreamShellStructureSpec::Sample, TEXT("StructureA"));
			*Second = OpenScratch(TEXT("def only_in_b():\n    pass\n"), TEXT("StructureB"));
			return First->IsValid() && Second->IsValid();
		});
		AddWaitShows(Steps, TEXT("the second tab's outline"), Second);
		Steps.Do(TEXT("switch to the first tab"), [this, First, WorkersBefore]()
		{
			TestEqual(TEXT("rows of the second tab"), DreamShellStructureSpec::RowsText(*View), FString(TEXT("f only_in_b L1 d0")));
			*WorkersBefore = View->GetWorkerBuildsForTest();
			Tab->ActivateBuffer((*First)->GetId());
			return true;
		});
		Steps.Poll(TEXT("the first tab's outline, built on the switch"), [this, First]()
		{
			return Shows(*First) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 2.0);
		Steps.Do(TEXT("rows of the first tab"), [this, WorkersBefore, Second]()
		{
			TestEqual(TEXT("rows of the first tab"), DreamShellStructureSpec::RowsText(*View), FString(DreamShellStructureSpec::SampleRows));
			TestEqual(TEXT("a small tab is built on the switch, not on a worker"), View->GetWorkerBuildsForTest(), *WorkersBefore);
			Tab->ActivateBuffer((*Second)->GetId());
			return true;
		});
		AddWaitShows(Steps, TEXT("back on the second tab"), Second, 2.0);
		Steps.Do(TEXT("rows of the second tab again"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(TEXT("f only_in_b L1 d0")));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("jumps to the entry on a click or Enter and focuses the code view", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the sample"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		Steps.Poll(TEXT("helper's row is laid out"), [this]()
		{
			View->ScrollEntryIntoViewForTest(4);   // a short section shows only a few rows
			return DreamShellStructureSpec::LaidOutRow(*View, 4).IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("click helper's row"), [this, Buffer]()
		{
			Tab->FindCodeView((*Buffer)->GetId())->SetCaret(18, 0);
			return TestTrue(TEXT("the click is routed"), DreamShellStructureSpec::Click(DreamShellStructureSpec::LaidOutRow(*View, 4), false));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the caret is on helper's name, the code view has focus, the row is selected"), [this, Buffer]()
		{
			TestEqual(TEXT("caret (0-based line:column)"), DreamShellStructureSpec::CaretText(*Tab, **Buffer), FString(TEXT("6:16")));
			TestTrue(TEXT("the code view has keyboard focus"), Tab->FindCodeView((*Buffer)->GetId())->HasEditorFocus());
			const TArray<SDreamStructureView::FRowForTest> Rows = View->GetRowsForTest();
			const int32 Selected = Rows.IndexOfByPredicate([](const SDreamStructureView::FRowForTest& Row) { return Row.bSelected; });
			TestTrue(TEXT("the clicked row is selected"), Rows.IsValidIndex(Selected) && Rows[Selected].Name == TEXT("helper"));
			return true;
		});
		// Enter on the selected row (helper's, from the click), with the caret moved away first.
		Steps.Do(TEXT("caret away, focus the tree, press Enter"), [this, Buffer]()
		{
			Tab->FindCodeView((*Buffer)->GetId())->SetCaret(0, 0);
			View->FocusList();
			if (!DreamShellStructureSpec::FocusIsInside(View->GetFocusWidget()))
			{
				AddError(TEXT("FocusList did not give the tree keyboard focus"));
				return false;
			}
			DreamShellStructureSpec::PressKey(EKeys::Enter);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Enter put the caret on helper's name"), [this, Buffer]()
		{
			TestEqual(TEXT("caret (0-based line:column)"), DreamShellStructureSpec::CaretText(*Tab, **Buffer), FString(TEXT("6:16")));
			TestTrue(TEXT("the code view has keyboard focus"), Tab->FindCodeView((*Buffer)->GetId())->HasEditorFocus());
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("jumps on a double-click and never collapses the parent it lands on", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the sample"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		Steps.Poll(TEXT("Inner's row is laid out"), [this]()
		{
			View->ScrollEntryIntoViewForTest(2);   // a short section shows only a few rows
			return DreamShellStructureSpec::LaidOutRow(*View, 2).IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("double-click Inner's row"), [this]()
		{
			return TestTrue(TEXT("the double-click is routed"), DreamShellStructureSpec::Click(DreamShellStructureSpec::LaidOutRow(*View, 2), true));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Inner is still open and the caret is on its name"), [this, Buffer]()
		{
			TestEqual(TEXT("rows (Inner keeps its children)"), DreamShellStructureSpec::RowsText(*View), FString(DreamShellStructureSpec::SampleRows));
			TestEqual(TEXT("caret (0-based line:column)"), DreamShellStructureSpec::CaretText(*Tab, **Buffer), FString(TEXT("4:10")));
			TestTrue(TEXT("the code view has keyboard focus"), Tab->FindCodeView((*Buffer)->GetId())->HasEditorFocus());
			// Nothing was recorded as collapsed: a rebuild keeps Inner open.
			const FString Text = FString(DreamShellStructureSpec::Sample) + TEXT("\ndef tail():\n    pass\n");
			return TestTrue(TEXT("the edit"), Tab->FindCodeView((*Buffer)->GetId())->ReplaceAllText(Text, FText::GetEmpty()));
		});
		AddWaitShows(Steps, TEXT("the rebuilt outline"), Buffer);
		Steps.Do(TEXT("Inner is open after the rebuild"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(DreamShellStructureSpec::SampleRows) + TEXT(" | f tail L21 d1"));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("tells same-named siblings apart: collapse, selection and the untitled cell label", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		// Entries: 0 C A, 1 m x, 2 C A, 3 m y, 4 # (bare), 5 f f, 6 # (bare), 7 f g.
		static const TCHAR* const Twins =
			TEXT("class A:\n    def x(self): pass\nclass A:\n    def y(self): pass\n# %%\ndef f(): pass\n# %%\ndef g(): pass\n");
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the twins"), [this, Buffer]()
		{
			*Buffer = OpenScratch(Twins, TEXT("StructureTwins"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		Steps.Do(TEXT("collapse the second class A and the second bare cell"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("C A L1 d0 | m x L2 d1 | C A L3 d0 | m y L4 d1 | #  L5 d0 | f f L6 d1 | #  L7 d0 | f g L8 d1")));
			TestTrue(TEXT("the second A collapses"), View->SetEntryExpandedForTest(2, false));
			TestTrue(TEXT("the second cell collapses"), View->SetEntryExpandedForTest(6, false));
			return true;
		});
		Steps.Poll(TEXT("the second A's row is laid out"), [this]()
		{
			View->ScrollEntryIntoViewForTest(2);   // a short section shows only a few rows
			return DreamShellStructureSpec::LaidOutRow(*View, 2).IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("click the second A (selects it), then edit"), [this, Buffer]()
		{
			TestTrue(TEXT("the click is routed"), DreamShellStructureSpec::Click(DreamShellStructureSpec::LaidOutRow(*View, 2), false));
			const FString Text = FString(Twins) + TEXT("x = 1\n");
			return TestTrue(TEXT("the edit"), Tab->FindCodeView((*Buffer)->GetId())->ReplaceAllText(Text, FText::GetEmpty()));
		});
		AddWaitShows(Steps, TEXT("the rebuilt outline"), Buffer);
		Steps.Do(TEXT("only the second A and the second cell are collapsed, the second A is still selected"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("C A L1 d0 | m x L2 d1 | C A L3 d0 | #  L5 d0 | f f L6 d1 | #  L7 d0")));
			const TArray<SDreamStructureView::FRowForTest> Rows = View->GetRowsForTest();
			const int32 Selected = Rows.IndexOfByPredicate([](const SDreamStructureView::FRowForTest& Row) { return Row.bSelected; });
			TestTrue(TEXT("the second A (line 3) is the selected row"), Rows.IsValidIndex(Selected) && Rows[Selected].Line == 2);
			// The untitled cells are found by the label they show, "(cell)".
			View->FocusList();
			if (!DreamShellStructureSpec::FocusIsInside(View->GetFocusWidget()))
			{
				AddError(TEXT("FocusList did not give the tree keyboard focus"));
				return false;
			}
			DreamShellStructureSpec::TypeText(TEXT("cell"));
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("the filter finds both bare cells by their label"), [this]()
		{
			TestEqual(TEXT("filter text"), View->GetFilterText(), FString(TEXT("cell")));
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(TEXT("#  L5 d0 | #  L7 d0")));
			View->ClearFilterForTest();
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("highlights the deepest entry that holds the caret", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the sample"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		// (line, expected entry): helper's body, a blank line in cell Setup, fetch, a decorator line, main, top-level code
		// in cell Main.
		static const int32 Cases[][2] = { { 7, 4 }, { 2, 0 }, { 11, 5 }, { 14, 7 }, { 16, 7 }, { 18, 6 }, { 9, 1 } };
		for (const auto& Case : Cases)
		{
			const int32 Line = Case[0];
			const int32 Expected = Case[1];
			Steps.Do(FString::Printf(TEXT("caret to line %d"), Line + 1), [this, Buffer, Line]()
			{
				Tab->FindCodeView((*Buffer)->GetId())->SetCaret(Line, 0);
				return true;
			});
			Steps.Poll(FString::Printf(TEXT("entry %d is highlighted"), Expected), [this, Expected, Line]()
			{
				if (View->GetHighlightedEntry() != Expected)
				{
					return DreamTest::EStep::Wait;
				}
				const TArray<SDreamStructureView::FRowForTest> Rows = View->GetRowsForTest();
				int32 Highlighted = 0;
				for (const SDreamStructureView::FRowForTest& Row : Rows)
				{
					Highlighted += Row.bHighlighted ? 1 : 0;
				}
				TestEqual(*FString::Printf(TEXT("one row highlighted for line %d"), Line + 1), Highlighted, 1);
				return DreamTest::EStep::Next;
			}, 2.0);
		}
		Steps.Do(TEXT("an empty tab highlights nothing"), [this]()
		{
			OpenScratch(FString(), TEXT("StructureEmpty"));
			return true;
		});
		Steps.Poll(TEXT("no highlight"), [this]()
		{
			return View->GetHighlightedEntry() == INDEX_NONE && View->IsShowingNoStructure() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 2.0);
		Steps.Run(Done);
	});

	LatentIt("works outside the side bar: a view of its own in a window follows the same tab", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		struct FOwn
		{
			TSharedPtr<SDreamStructureView> View;
			TSharedPtr<SWindow> Window;
		};
		TSharedRef<FOwn> Own = MakeShared<FOwn>();
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("host a second view in an off-screen window, open the sample"), [this, Own, Buffer]()
		{
			Own->View = SNew(SDreamStructureView, TWeakPtr<SDreamTab>(Tab));
			Own->Window = SNew(SWindow)
				.ClientSize(FVector2D(260.0, 420.0))
				.ScreenPosition(FVector2D(-20000.0, -20000.0))
				.AutoCenter(EAutoCenter::None)
				.SizingRule(ESizingRule::FixedSize)
				.CreateTitleBar(false)
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				.ActivationPolicy(EWindowActivationPolicy::Never)
				.FocusWhenFirstShown(false)
				[
					Own->View.ToSharedRef()
				];
			FSlateApplication::Get().AddWindow(Own->Window.ToSharedRef(), /*bShowImmediately=*/true);
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("StructureOwn"));
			return Buffer->IsValid();
		});
		Steps.Poll(TEXT("the window's view shows the sample"), [Own, Buffer]()
		{
			return Own->View->GetShownBufferId() == (*Buffer)->GetId() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("rows and a click"), [this, Own, Buffer]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*Own->View), FString(DreamShellStructureSpec::SampleRows));
			return true;
		});
		Steps.Poll(TEXT("main's row is laid out in the window"), [Own]()
		{
			Own->View->ScrollEntryIntoViewForTest(7);   // a short section shows only a few rows
			return DreamShellStructureSpec::LaidOutRow(*Own->View, 7).IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("click main's row"), [this, Own, Buffer]()
		{
			TestTrue(TEXT("the click is routed"), DreamShellStructureSpec::Click(DreamShellStructureSpec::LaidOutRow(*Own->View, 7), false));
			TestEqual(TEXT("caret (0-based line:column)"), DreamShellStructureSpec::CaretText(*Tab, **Buffer), FString(TEXT("15:10")));
			return true;
		});
		Steps.Finally(TEXT("close the window"), [Own]()
		{
			if (Own->Window.IsValid())
			{
				FSlateApplication::Get().RequestDestroyWindow(Own->Window.ToSharedRef());
			}
			Own->Window.Reset();
			Own->View.Reset();
		});
		Steps.Run(Done);
	});

	LatentIt("keeps a collapsed parent collapsed across a rebuild and gives it the highlight of an entry inside", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the sample, caret in helper"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		Steps.Do(TEXT("caret into helper"), [this, Buffer]()
		{
			Tab->FindCodeView((*Buffer)->GetId())->SetCaret(7, 0);
			return true;
		});
		Steps.Poll(TEXT("helper is highlighted"), [this]()
		{
			return View->GetHighlightedEntry() == 4 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 2.0);
		Steps.Do(TEXT("collapse Inner"), [this]()
		{
			TestTrue(TEXT("Inner collapses"), View->SetEntryExpandedForTest(2, false));
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("# Setup L1 d0 | C Outer L4 d1 | C Inner L5 d2 | am fetch L11 d2 | # Main L14 d0 | af main L16 d1")));
			TestEqual(TEXT("the collapsed Inner takes helper's highlight"), View->GetHighlightedEntry(), 2);
			return true;
		});
		Steps.Do(TEXT("edit below"), [this, Buffer]()
		{
			const FString Text = FString(DreamShellStructureSpec::Sample) + TEXT("\ndef tail():\n    pass\n");
			return TestTrue(TEXT("the edit"), Tab->FindCodeView((*Buffer)->GetId())->ReplaceAllText(Text, FText::GetEmpty()));
		});
		AddWaitShows(Steps, TEXT("the rebuilt outline"), Buffer);
		Steps.Do(TEXT("Inner is still collapsed and highlighted"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("# Setup L1 d0 | C Outer L4 d1 | C Inner L5 d2 | am fetch L11 d2 | # Main L14 d0 | af main L16 d1 | f tail L21 d1")));
			TestEqual(TEXT("highlight"), View->GetHighlightedEntry(), 2);
			TestTrue(TEXT("Inner expands"), View->SetEntryExpandedForTest(2, true));
			TestEqual(TEXT("helper takes the highlight back"), View->GetHighlightedEntry(), 4);
			TestEqual(TEXT("rows with Inner open again (method stays open under it)"), DreamShellStructureSpec::RowsText(*View),
				FString(DreamShellStructureSpec::SampleRows) + TEXT(" | f tail L21 d1"));
			// A collapse under a collapsed parent survives the parent's collapse and expansion (with frames between, so the
			// tree linearizes while Inner is collapsed and forgets the rows under it).
			TestTrue(TEXT("method collapses"), View->SetEntryExpandedForTest(3, false));
			TestTrue(TEXT("Inner collapses"), View->SetEntryExpandedForTest(2, false));
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("expand Inner: method is still collapsed"), [this]()
		{
			TestTrue(TEXT("Inner expands"), View->SetEntryExpandedForTest(2, true));
			TestEqual(TEXT("method is still collapsed and holds the highlight"), View->GetHighlightedEntry(), 3);
			TestFalse(TEXT("helper is not shown"), DreamShellStructureSpec::RowsText(*View).Contains(TEXT("helper")));
			TestTrue(TEXT("method expands"), View->SetEntryExpandedForTest(3, true));
			TestEqual(TEXT("helper is highlighted again"), View->GetHighlightedEntry(), 4);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("opens the filter on typing, keeps the parents of matches and clears it on Esc", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open the sample"), [this, Buffer]()
		{
			*Buffer = OpenScratch(DreamShellStructureSpec::Sample, TEXT("Structure"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the outline"), Buffer);
		Steps.Poll(TEXT("cell Setup holds the caret"), [this]()
		{
			return View->GetHighlightedEntry() == 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 2.0);
		Steps.Do(TEXT("focus the tree and type METH"), [this]()
		{
			View->FocusList();
			if (!DreamShellStructureSpec::FocusIsInside(View->GetFocusWidget()))
			{
				AddError(TEXT("FocusList did not give the tree keyboard focus"));
				return false;
			}
			const TArray<SDreamStructureView::FRowForTest> Rows = View->GetRowsForTest();
			TestTrue(TEXT("Focus Structure selects the highlighted entry when nothing is selected"), Rows.Num() > 0 && Rows[0].bSelected);
			DreamShellStructureSpec::TypeText(TEXT("METH"));
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the filter row shows the matches and their parents"), [this]()
		{
			TestTrue(TEXT("the filter row is open"), View->IsFilterOpen());
			TestEqual(TEXT("filter text"), View->GetFilterText(), FString(TEXT("METH")));
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
			TestTrue(TEXT("keyboard focus is in the filter row"), Focused.IsValid() && View->IsFocusWidget(*Focused) && !DreamShellStructureSpec::FocusIsInside(View->GetFocusWidget()));
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View),
				FString(TEXT("# Setup L1 d0 (p) | C Outer L4 d1 (p) | C Inner L5 d2 (p) | m method L6 d3")));
			DreamShellStructureSpec::TypeText(TEXT("zz"));
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("no match"), [this]()
		{
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString());
			TestFalse(TEXT("\"No structure\" is not shown for a filter with no match"), View->IsShowingNoStructure());
			DreamShellStructureSpec::PressKey(EKeys::Escape);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Esc cleared and closed the filter"), [this]()
		{
			TestFalse(TEXT("the filter row is closed"), View->IsFilterOpen());
			TestEqual(TEXT("filter text"), View->GetFilterText(), FString());
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(DreamShellStructureSpec::SampleRows));
			TestTrue(TEXT("keyboard focus is back in the tree"), DreamShellStructureSpec::FocusIsInside(View->GetFocusWidget()));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("shows No structure for an empty, an entry-free and a non-Python tab", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		TSharedRef<TSharedPtr<FDreamBuffer>> Buffer = MakeShared<TSharedPtr<FDreamBuffer>>();
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("an empty tab"), [this, Buffer]()
		{
			*Buffer = OpenScratch(FString(), TEXT("StructureEmpty"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the empty tab is shown"), Buffer);
		Steps.Do(TEXT("No structure for the empty tab"), [this, Buffer]()
		{
			TestTrue(TEXT("\"No structure\" is shown"), View->IsShowingNoStructure());
			TestEqual(TEXT("no rows"), View->GetRowsForTest().Num(), 0);
			*Buffer = OpenScratch(TEXT("x = 1\nprint(x)  # def not_this():\ns = 'class NotThis:'\n"), TEXT("StructureFlat"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the entry-free tab is shown"), Buffer);
		Steps.Do(TEXT("No structure for a tab without defs"), [this, Buffer]()
		{
			TestTrue(TEXT("\"No structure\" is shown"), View->IsShowingNoStructure());
			// A file tab that is not Python (a buffer in memory with a .json path; nothing is read or written): its text has
			// a def, and still nothing is listed.
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../Plugins/DreamPythonEditor/Tests/ci/lands_terms.json")));
			TSharedRef<FDreamBuffer> NotPython = FDreamEditorModule::Get().GetBufferManager().AddRestored(FGuid(), EDreamBufferKind::File,
				DreamTest::ScratchName(TEXT("StructureJson")), Path, TEXT("def looks_like_python():\n    pass\n"), dream::codec::NewFileFormat(),
				TOptional<FDreamBaseVersion>());
			Opened.Add(NotPython->GetId());
			Tab->ActivateBuffer(NotPython->GetId());
			*Buffer = NotPython;
			return true;
		});
		Steps.Poll(TEXT("No structure for the non-Python tab"), [this, Buffer]()
		{
			const bool bActive = Tab->GetActiveBuffer() == *Buffer;
			return bActive && View->IsShowingNoStructure() && !View->GetShownBufferId().IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 2.0);
		Steps.Do(TEXT("back to a Python tab"), [this, Buffer]()
		{
			TestEqual(TEXT("no rows"), View->GetRowsForTest().Num(), 0);
			*Buffer = OpenScratch(TEXT("def listed():\n    pass\n"), TEXT("StructureBack"));
			return Buffer->IsValid();
		});
		AddWaitShows(Steps, TEXT("the Python tab is shown"), Buffer);
		Steps.Do(TEXT("its entry is listed"), [this]()
		{
			TestFalse(TEXT("\"No structure\" is gone"), View->IsShowingNoStructure());
			TestEqual(TEXT("rows"), DreamShellStructureSpec::RowsText(*View), FString(TEXT("f listed L1 d0")));
			return true;
		});
		Steps.Run(Done);
	});
}
