// SPDX-License-Identifier: Apache-2.0
// Dream.CodeView.Indent (editing, PO-06 F1): the code view's line selection and line indentation with real input.
//   TripleClick  a triple-click (press, release, the platform's double-click, release, press) selects the whole line with
//                its line break; dragging before the release extends by whole lines, down and up; the last line ends at
//                its end; a double-click still selects a word; a third press away from the double-click is an ordinary
//                click; Tab after a triple-click indents that line only.
//   Selection    Tab and Shift+Tab with any selection (part of one line, mid-line to mid-line, reversed, whole lines)
//                indent or dedent every line it touches by one unit; only leading whitespace changes, the selection keeps
//                its text, each change is one undo step; without a selection Tab inserts at the caret and Shift+Tab
//                dedents the caret's line; nothing to dedent makes no undo step (EDT-03, EDT-08).
// Keys go through FSlateApplication::Process*Event to the focused code view; mouse events are routed along the text
// widget's path as Slate routes platform input. Every step checks that keyboard focus is still in the code view.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Shell/SDreamTab.h"

namespace DreamCodeViewIndentSpec
{
	FModifierKeysState Mods(bool bCtrl, bool bShift = false)
	{
		return FModifierKeysState(bShift, false, bCtrl, false, false, false, false, false, false);
	}

	/** A key press as the platform layer delivers it: key-down, the character event when Character is set, key-up. */
	bool Press(const FKey& Key, const FModifierKeysState& State, TCHAR Character = 0)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const uint32 User = App.GetUserIndexForKeyboard();
		const bool bHandled = App.ProcessKeyDownEvent(FKeyEvent(Key, State, User, false, Character, Character));
		if (Character != 0)
		{
			App.ProcessKeyCharEvent(FCharacterEvent(Character, State, User, false));
		}
		App.ProcessKeyUpEvent(FKeyEvent(Key, State, User, false, Character, Character));
		return bHandled;
	}

	void PressTab() { Press(EKeys::Tab, FModifierKeysState(), TCHAR(9)); }
	void PressShiftTab() { Press(EKeys::Tab, Mods(false, true), TCHAR(9)); }
	void Undo() { Press(EKeys::Z, Mods(true)); }

	FString Text(const TSharedPtr<FDreamBuffer>& Buffer)
	{
		return Buffer.IsValid() ? Buffer->GetTextAsString() : FString();
	}

	bool FocusIsInside(const TSharedRef<SWidget>& Root)
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

	TSharedPtr<SWidget> FindByType(const TSharedRef<SWidget>& Root, const FName TypeName)
	{
		if (Root->GetType() == TypeName)
		{
			return Root;
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindByType(Children->GetChildAt(Index), TypeName))
			{
				return Found;
			}
		}
		return nullptr;
	}

	FDreamTextSelection Sel(int32 AnchorLine, int32 AnchorColumn, int32 CaretLine, int32 CaretColumn)
	{
		FDreamTextSelection Out;
		Out.AnchorLine = AnchorLine;
		Out.AnchorColumn = AnchorColumn;
		Out.CaretLine = CaretLine;
		Out.CaretColumn = CaretColumn;
		return Out;
	}

	/** The selected text with LF breaks (the stock widget returns its selection with the platform's line terminator). */
	FString Selected(const SDreamCodeView& View)
	{
		return View.GetSelectedText().Replace(TEXT("\r\n"), TEXT("\n"));
	}

	FString Describe(const FDreamTextSelection& S)
	{
		return FString::Printf(TEXT("(%d,%d)-(%d,%d)"), S.AnchorLine, S.AnchorColumn, S.CaretLine, S.CaretColumn);
	}

	/**
	 * The desktop position of (Line, Column) in the view, at the middle of the line's height. It moves the caret there to
	 * read its rectangle, so take every point a gesture needs before the gesture starts.
	 */
	bool PointAt(SDreamCodeView& View, int32 Line, int32 Column, FVector2D& Out)
	{
		View.SetCaret(Line, Column);
		const FSlateRect Rect = View.GetCaretScreenRect();
		if (Rect.GetSize().Y <= 0.0)
		{
			return false;
		}
		Out = FVector2D(Rect.Left + 2.0, (Rect.Top + Rect.Bottom) * 0.5);
		return true;
	}

	/** Pointer events routed along the code view's text widget path, as the platform layer delivers them. */
	struct FMouse
	{
		TSharedPtr<SWidget> Text;
		FVector2D Last = FVector2D::ZeroVector;

		explicit FMouse(SDreamCodeView& View)
			: Text(FindByType(View.AsShared(), FName(TEXT("SDreamPlainText"))))
		{
		}

		bool IsValid() const { return Text.IsValid(); }

		bool Route(TFunctionRef<void(FSlateApplication&, const FWidgetPath&, uint32, uint32)> Fn) const
		{
			FWidgetPath Path;
			if (!Text.IsValid() || !DreamTest::MakePointerPath(Text.ToSharedRef(), Path))
			{
				return false;
			}
			FSlateApplication& App = FSlateApplication::Get();
			Fn(App, Path, App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex);
			return true;
		}

		bool Move(const FVector2D& At, bool bButtonHeld)
		{
			TSet<FKey> Pressed;
			if (bButtonHeld)
			{
				Pressed.Add(EKeys::LeftMouseButton);
			}
			const FVector2D From = Last;
			Last = At;
			return Route([&](FSlateApplication& App, const FWidgetPath& Path, uint32 User, uint32 Pointer)
			{
				App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, From, Pressed, EKeys::Invalid, 0.f, FModifierKeysState()), false);
			});
		}

		bool Down(const FVector2D& At, const FModifierKeysState& State = FModifierKeysState())
		{
			Last = At;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			return Route([&](FSlateApplication& App, const FWidgetPath& Path, uint32 User, uint32 Pointer)
			{
				App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, State));
			});
		}

		bool DoubleClick(const FVector2D& At)
		{
			Last = At;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			return Route([&](FSlateApplication& App, const FWidgetPath& Path, uint32 User, uint32 Pointer)
			{
				App.RoutePointerDoubleClickEvent(Path, FPointerEvent(User, Pointer, At, At, Pressed, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			});
		}

		bool Up(const FVector2D& At)
		{
			Last = At;
			return Route([&](FSlateApplication& App, const FWidgetPath& Path, uint32 User, uint32 Pointer)
			{
				App.RoutePointerUpEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			});
		}

		/** Press, release, double-click, release: the first two clicks of a triple-click (the word is selected). */
		bool DoubleClickAt(const FVector2D& At)
		{
			return Move(At, false) && Down(At) && Up(At) && DoubleClick(At) && Up(At);
		}

		/** The whole triple-click but the last release (a drag can follow). */
		bool TriplePressAt(const FVector2D& At)
		{
			return DoubleClickAt(At) && Down(At);
		}
	};

	void CloseAll(TArray<FGuid>& Ids)
	{
		if (FDreamEditorModule::IsAvailable())
		{
			for (const FGuid& Id : Ids)
			{
				if (const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().FindById(Id))
				{
					if (Buffer->GetKind() == EDreamBufferKind::Scratch)
					{
						Buffer->ResetText(FString(), dream::EditOrigin::Load);
					}
				}
				DreamTest::CloseBuffer(Id);
			}
		}
		Ids.Reset();
	}
}

BEGIN_DEFINE_SPEC(FDreamCodeViewIndentSpec, "Dream.CodeView.Indent", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	/** The view of Id with keyboard focus in it, or null with an error. */
	TSharedPtr<SDreamCodeView> FocusedView(const FGuid& Id, const TCHAR* When)
	{
		const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(Id) : nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: keyboard focus is in the code view"), When),
			View.IsValid() && DreamCodeViewIndentSpec::FocusIsInside(View.ToSharedRef())))
		{
			return nullptr;
		}
		return View;
	}
	bool ExpectSelection(const TCHAR* What, const SDreamCodeView& View, const FDreamTextSelection& Expected)
	{
		const FDreamTextSelection Actual = View.GetSelection();
		return TestTrue(*FString::Printf(TEXT("%s: selection %s, expected %s"), What, *DreamCodeViewIndentSpec::Describe(Actual),
			*DreamCodeViewIndentSpec::Describe(Expected)), Actual == Expected);
	}
	/** Opens a scratch tab with Text and focuses its code view; the steps after it find the view by Id. */
	TSharedPtr<FDreamBuffer> Open(DreamTest::FDreamSteps& Steps, const FString& Text, const TCHAR* Purpose)
	{
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, Text, Purpose) : nullptr;
		if (!Buffer.IsValid())
		{
			return nullptr;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this, Id]()
		{
			FDreamKeyRouter::Get().SetTabMovesFocus(false);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(0, 0);
			return true;
		});
		Steps.WaitFrames(3);   // laid out and painted: the caret rectangle and the pointer hit test read the line views
		return Buffer;
	}
END_DEFINE_SPEC(FDreamCodeViewIndentSpec)

void FDreamCodeViewIndentSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
		}
		DreamCodeViewIndentSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("selects the whole line on a triple-click and extends by whole lines on a drag (PO-06 F1)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewIndentSpec;
		DreamTest::FDreamSteps Steps(*this);
		const TSharedPtr<FDreamBuffer> Buffer = Open(Steps, TEXT("alpha = 1\n    beta = 2\ngamma = 3\ndelta = 4"), TEXT("Triple"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Buffer->GetId();
		Steps.Do(TEXT("triple-click line 1, drag down to the last line, then up to line 0, release"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("triple-click"));
			if (!View.IsValid())
			{
				return false;
			}
			FMouse Mouse(*View);
			FVector2D OnBeta = FVector2D::ZeroVector, OnDelta = FVector2D::ZeroVector, OnAlpha = FVector2D::ZeroVector;
			if (!TestTrue(TEXT("the text widget is found"), Mouse.IsValid())
				|| !TestTrue(TEXT("the lines have caret rectangles"), PointAt(*View, 1, 6, OnBeta) && PointAt(*View, 3, 3, OnDelta)
					&& PointAt(*View, 0, 3, OnAlpha)))
			{
				return false;
			}
			TestTrue(TEXT("the triple press is routed"), Mouse.TriplePressAt(OnBeta));
			ExpectSelection(TEXT("the triple-click selects line 1 with its line break"), *View, Sel(1, 0, 2, 0));
			TestEqual(TEXT("the selected text is the line and its break"), Selected(*View), FString(TEXT("    beta = 2\n")));

			Mouse.Move(OnDelta, true);
			ExpectSelection(TEXT("a drag to the last line extends to its end"), *View, Sel(1, 0, 3, 9));
			TestEqual(TEXT("lines 1 to 3 are selected"), Selected(*View), FString(TEXT("    beta = 2\ngamma = 3\ndelta = 4")));

			Mouse.Move(OnAlpha, true);
			ExpectSelection(TEXT("a drag above the anchor line keeps line 1 whole and puts the caret at line 0's start"), *View, Sel(2, 0, 0, 0));
			TestEqual(TEXT("lines 0 and 1 are selected"), Selected(*View), FString(TEXT("alpha = 1\n    beta = 2\n")));

			Mouse.Up(OnAlpha);
			Mouse.Move(OnDelta, false);
			ExpectSelection(TEXT("a move after the release changes nothing"), *View, Sel(2, 0, 0, 0));
			FSlateApplication::Get().ReleaseAllPointerCapture();
			return true;
		});
		Steps.Do(TEXT("the last line, a double-click, and a third press elsewhere"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("last line"));
			if (!View.IsValid())
			{
				return false;
			}
			FMouse Mouse(*View);
			FVector2D OnDelta = FVector2D::ZeroVector, OnGamma = FVector2D::ZeroVector, OnAlpha = FVector2D::ZeroVector;
			if (!TestTrue(TEXT("the lines have caret rectangles"), PointAt(*View, 3, 2, OnDelta) && PointAt(*View, 2, 2, OnGamma)
				&& PointAt(*View, 0, 2, OnAlpha)))
			{
				return false;
			}
			Mouse.TriplePressAt(OnDelta);
			Mouse.Up(OnDelta);
			ExpectSelection(TEXT("the last line has no break: the selection ends at its end"), *View, Sel(3, 0, 3, 9));

			Mouse.DoubleClickAt(OnGamma);
			TestEqual(TEXT("a double-click still selects the word"), Selected(*View), FString(TEXT("gamma")));

			Mouse.Move(OnAlpha, false);
			Mouse.Down(OnAlpha);
			Mouse.Up(OnAlpha);
			TestTrue(TEXT("a third press away from the double-click is an ordinary click (no selection)"), View->GetSelection().IsEmpty());
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the ordinary click puts the caret on line 0"), Line, 0);

			// A fourth press right after a triple-click is an ordinary click too.
			Mouse.TriplePressAt(OnGamma);
			Mouse.Up(OnGamma);
			ExpectSelection(TEXT("triple-click on line 2"), *View, Sel(2, 0, 3, 0));
			Mouse.Down(OnGamma);
			Mouse.Up(OnGamma);
			TestTrue(TEXT("a fourth press clears the line selection"), View->GetSelection().IsEmpty());
			FSlateApplication::Get().ReleaseAllPointerCapture();
			return true;
		});
		Steps.Do(TEXT("Tab after a triple-click indents that line only, one undo step"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("Tab after triple-click"));
			if (!View.IsValid())
			{
				return false;
			}
			FMouse Mouse(*View);
			FVector2D OnGamma = FVector2D::ZeroVector;
			if (!TestTrue(TEXT("line 2 has a caret rectangle"), PointAt(*View, 2, 2, OnGamma)))
			{
				return false;
			}
			Mouse.TriplePressAt(OnGamma);
			Mouse.Up(OnGamma);
			FSlateApplication::Get().ReleaseAllPointerCapture();
			if (!FocusedView(Id, TEXT("after the triple-click")).IsValid())
			{
				return false;
			}
			PressTab();
			TestEqual(TEXT("only line 2 is indented"), Text(Buffer), FString(TEXT("alpha = 1\n    beta = 2\n    gamma = 3\ndelta = 4")));
			ExpectSelection(TEXT("line 2 stays selected whole, its new indent included"), *View, Sel(2, 0, 3, 0));
			PressShiftTab();
			TestEqual(TEXT("Shift+Tab takes it back"), Text(Buffer), FString(TEXT("alpha = 1\n    beta = 2\ngamma = 3\ndelta = 4")));
			ExpectSelection(TEXT("the line is still selected whole"), *View, Sel(2, 0, 3, 0));
			Undo();
			TestEqual(TEXT("one Ctrl+Z undoes the dedent"), Text(Buffer), FString(TEXT("alpha = 1\n    beta = 2\n    gamma = 3\ndelta = 4")));
			Undo();
			TestEqual(TEXT("a second Ctrl+Z undoes the indent"), Text(Buffer), FString(TEXT("alpha = 1\n    beta = 2\ngamma = 3\ndelta = 4")));
			TestEqual(TEXT("the widget shows the model's text"), View->GetShownText(), Text(Buffer));
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("indents and dedents every line any selection touches, keeping the selection, one undo step each (EDT-03, PO-06 F1)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewIndentSpec;
		DreamTest::FDreamSteps Steps(*this);
		const TSharedPtr<FDreamBuffer> Buffer = Open(Steps, TEXT("x = foo(1)"), TEXT("Indent"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid Id = Buffer->GetId();
		Steps.Do(TEXT("a selection inside one line"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("one line"));
			if (!View.IsValid())
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("x = foo(1)"));
			View->SetSelection(Sel(0, 4, 0, 7));
			PressTab();
			TestEqual(TEXT("Tab indents the line instead of replacing the selection"), Text(Buffer), FString(TEXT("    x = foo(1)")));
			ExpectSelection(TEXT("the selection moved with its text"), *View, Sel(0, 8, 0, 11));
			TestEqual(TEXT("the selected text is still foo"), Selected(*View), FString(TEXT("foo")));
			PressTab();
			TestEqual(TEXT("a second Tab indents again"), Text(Buffer), FString(TEXT("        x = foo(1)")));
			PressShiftTab();
			TestEqual(TEXT("Shift+Tab dedents one unit"), Text(Buffer), FString(TEXT("    x = foo(1)")));
			TestEqual(TEXT("the selected text is still foo after the dedent"), Selected(*View), FString(TEXT("foo")));
			Undo();
			TestEqual(TEXT("one Ctrl+Z undoes the dedent alone"), Text(Buffer), FString(TEXT("        x = foo(1)")));
			ExpectSelection(TEXT("the undo brings back the selection before the dedent"), *View, Sel(0, 12, 0, 15));
			Undo();
			Undo();
			TestEqual(TEXT("two more Ctrl+Z undo the two indents"), Text(Buffer), FString(TEXT("x = foo(1)")));
			TestFalse(TEXT("nothing more to undo"), View->CanUndo());
			return true;
		});
		Steps.Do(TEXT("a selection from mid-line to mid-line, forwards and reversed"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("mid-line"));
			if (!View.IsValid())
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("a = 1\nb = 2\nc = 3\nd = 4"));
			View->SetSelection(Sel(0, 2, 2, 3));
			PressTab();
			TestEqual(TEXT("the three touched lines are indented, the fourth is not"), Text(Buffer),
				FString(TEXT("    a = 1\n    b = 2\n    c = 3\nd = 4")));
			ExpectSelection(TEXT("both ends moved with their text"), *View, Sel(0, 6, 2, 7));
			TestEqual(TEXT("the widget shows the model's text"), View->GetShownText(), Text(Buffer));
			Undo();
			TestEqual(TEXT("one Ctrl+Z undoes all three lines"), Text(Buffer), FString(TEXT("a = 1\nb = 2\nc = 3\nd = 4")));
			ExpectSelection(TEXT("and brings the selection back"), *View, Sel(0, 2, 2, 3));

			View->SetSelection(Sel(3, 1, 1, 4));
			PressTab();
			TestEqual(TEXT("a reversed selection indents lines 1 to 3"), Text(Buffer), FString(TEXT("a = 1\n    b = 2\n    c = 3\n    d = 4")));
			ExpectSelection(TEXT("the reversed selection keeps its direction"), *View, Sel(3, 5, 1, 8));
			PressShiftTab();
			TestEqual(TEXT("Shift+Tab on the reversed selection dedents them"), Text(Buffer), FString(TEXT("a = 1\nb = 2\nc = 3\nd = 4")));
			ExpectSelection(TEXT("and the selection is back on its text"), *View, Sel(3, 1, 1, 4));
			return true;
		});
		Steps.Do(TEXT("whole lines, empty lines, mixed indents and nothing to dedent"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("whole lines"));
			if (!View.IsValid())
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("a\n\nb\nc"));
			View->SetSelection(Sel(0, 0, 3, 0));
			PressTab();
			TestEqual(TEXT("a selection ending at column 0 leaves that line; the empty line gets no whitespace"), Text(Buffer),
				FString(TEXT("    a\n\n    b\nc")));
			ExpectSelection(TEXT("the whole lines stay selected from column 0"), *View, Sel(0, 0, 3, 0));

			View->SetTextNoUndo(TEXT("    a\n  b\nc\n\tz\n      e"));
			View->SetSelection(Sel(0, 5, 4, 7));
			PressShiftTab();
			TestEqual(TEXT("each line loses one unit of what it has: four spaces, two spaces, nothing, a tab, four of six"),
				Text(Buffer), FString(TEXT("a\nb\nc\nz\n  e")));
			ExpectSelection(TEXT("the ends moved with their text"), *View, Sel(0, 1, 4, 3));
			Undo();
			TestEqual(TEXT("one Ctrl+Z restores every line"), Text(Buffer), FString(TEXT("    a\n  b\nc\n\tz\n      e")));

			View->SetTextNoUndo(TEXT("x = 1\ny = 2"));
			View->SetSelection(Sel(0, 1, 1, 2));
			PressShiftTab();
			TestEqual(TEXT("nothing to dedent changes nothing"), Text(Buffer), FString(TEXT("x = 1\ny = 2")));
			TestFalse(TEXT("and makes no undo step"), View->CanUndo());
			ExpectSelection(TEXT("the selection is kept"), *View, Sel(0, 1, 1, 2));
			return true;
		});
		Steps.Do(TEXT("no selection: Tab inserts at the caret, Shift+Tab dedents the caret's line"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = FocusedView(Id, TEXT("caret"));
			if (!View.IsValid())
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("ab"));
			View->SetCaret(0, 1);
			PressTab();
			TestEqual(TEXT("Tab mid-line inserts spaces to the next stop"), Text(Buffer), FString(TEXT("a   b")));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the caret is after the spaces"), Column, 4);

			View->SetTextNoUndo(TEXT("if x:\n        y = 1"));
			View->SetCaret(1, 11);
			PressShiftTab();
			TestEqual(TEXT("Shift+Tab mid-line dedents the caret's line"), Text(Buffer), FString(TEXT("if x:\n    y = 1")));
			ExpectSelection(TEXT("the caret stays on its character"), *View, FDreamTextSelection::Caret(1, 7));
			View->SetCaret(1, 2);
			PressShiftTab();
			TestEqual(TEXT("Shift+Tab with the caret in the indentation dedents too"), Text(Buffer), FString(TEXT("if x:\ny = 1")));
			ExpectSelection(TEXT("a caret inside the removed whitespace goes to column 0"), *View, FDreamTextSelection::Caret(1, 0));
			Undo();
			TestEqual(TEXT("each dedent is one undo step"), Text(Buffer), FString(TEXT("if x:\n    y = 1")));
			TestEqual(TEXT("the widget shows the model's text"), View->GetShownText(), Text(Buffer));
			return true;
		});
		Steps.Run(Done);
	});
}
