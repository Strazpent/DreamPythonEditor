// SPDX-License-Identifier: Apache-2.0
// Dream.Output.Selection: the selectable history of owner ruling PO-06 F3 (Shell/DreamOutputSelection.h, SDreamOutputView),
// driven with Slate mouse and key events routed along the widgets' paths (FSlateApplication::Route*), as Slate routes a
// platform pointer: the press tunnels through the view's surface, the drag and the release come back through its capture.
//   1. A press on one row and a drag to another selects exactly the characters between them (the points come from the
//      rows' own text blocks and the font measure, not from the view's mapping); the selection is painted from the press
//      point to the release point; Ctrl+C and the context menu's Copy copy it; Shift+click extends it; the view keeps
//      following new lines with the selection kept; a click without a drag drops it and selects the row as before.
//   2. A double-click selects a word, the press right after it the whole line, decorations included as drawn
//      ("# Result: 42 #"); Ctrl+A selects every shown row.
//   3. A click without a drag on a traceback link still opens its file at the line; a drag that starts on the link selects
//      instead and opens nothing.
//   4. The Run window's live view (bLiveRunView) selects by dragging too, and the hand-off carries the selection to the
//      docked view, where the context menu's Copy and its "Run #N" section act on it.
//   5. On a 100,000-line ring the view stays virtualized: Ctrl+A, copying every line, a drag and the frames while all of it
//      is selected stay fast.
// Each case shows every line type, leaves run headers as they are (off by default) and restores what it changed.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Rendering/SlateRenderer.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"
#include "Shell/DreamOutputRows.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // the clipboard format check (IsPlainTextClipboard)
#endif

namespace DreamOutputSelectionSpec
{
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

	/** The text blocks of a row widget, in order. */
	TArray<TSharedRef<STextBlock>> TextBlocks(const TSharedPtr<SWidget>& Row)
	{
		TArray<TSharedRef<SWidget>> Found;
		if (Row.IsValid())
		{
			Collect(Row.ToSharedRef(), [](const TSharedRef<SWidget>& Widget) { return Widget->GetType() == FName(TEXT("STextBlock")); }, Found);
		}
		TArray<TSharedRef<STextBlock>> Blocks;
		for (const TSharedRef<SWidget>& Widget : Found)
		{
			Blocks.Add(StaticCastSharedRef<STextBlock>(Widget));
		}
		return Blocks;
	}

	/** The desktop point 1 px right of the boundary before character Offset of a text block, half way down it: measured with
	 *  the block's own font, so it is independent of the view's hit testing. */
	FVector2D PointInText(const TSharedRef<STextBlock>& Text, int32 Offset)
	{
		const FGeometry& Geometry = Text->GetPaintSpaceGeometry();
		const float Scale = FMath::Max(0.01f, Geometry.Scale);
		const FString String = Text->GetText().ToString();
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const double X = Offset > 0 ? Measure->Measure(String.Left(Offset), Text->GetFont(), Scale).X / Scale : 0.0;
		return FVector2D(Geometry.LocalToAbsolute(FVector2D(X + 1.0, Geometry.GetLocalSize().Y * 0.5)));
	}

	FModifierKeysState Mods(bool bShift, bool bCtrl = false)
	{
		return FModifierKeysState(bShift, false, bCtrl, false, false, false, false, false, false);
	}

	TSet<FKey> LeftPressed()
	{
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		return Pressed;
	}

	/** Pointer events routed along Widget's path (DreamTest::MakePointerPath): with the mouse captured, a move and the
	 *  release go to the captor, as Slate routes them. */
	bool Press(const TSharedRef<SWidget>& Widget, const FVector2D& At, bool bShift = false)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		const uint32 User = App.GetUserIndexForMouse();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		App.RoutePointerMoveEvent(Path, FPointerEvent(User, Pointer, At, At, TSet<FKey>(), EKeys::Invalid, 0.f, Mods(bShift)), false);
		App.RoutePointerDownEvent(Path, FPointerEvent(User, Pointer, At, At, LeftPressed(), EKeys::LeftMouseButton, 0.f, Mods(bShift)));
		return true;
	}

	bool MoveTo(const TSharedRef<SWidget>& Widget, const FVector2D& At, bool bShift = false)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		App.RoutePointerMoveEvent(Path, FPointerEvent(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At, LeftPressed(),
			EKeys::Invalid, 0.f, Mods(bShift)), false);
		return true;
	}

	bool Release(const TSharedRef<SWidget>& Widget, const FVector2D& At, bool bShift = false)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		App.RoutePointerUpEvent(Path, FPointerEvent(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At, TSet<FKey>(),
			EKeys::LeftMouseButton, 0.f, Mods(bShift)));
		return true;
	}

	bool DoubleClick(const TSharedRef<SWidget>& Widget, const FVector2D& At)
	{
		FSlateApplication& App = FSlateApplication::Get();
		FWidgetPath Path;
		if (!DreamTest::MakePointerPath(Widget, Path))
		{
			return false;
		}
		App.RoutePointerDoubleClickEvent(Path, FPointerEvent(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At,
			LeftPressed(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		return Release(Widget, At);
	}

	/** A drag from one text point to another: press, two moves (the second past the drag distance) and release. */
	bool Drag(const TSharedRef<SWidget>& From, const FVector2D& FromAt, const TSharedRef<SWidget>& To, const FVector2D& ToAt)
	{
		return Press(From, FromAt) && MoveTo(From, FMath::Lerp(FromAt, ToAt, 0.5)) && MoveTo(To, ToAt) && Release(To, ToAt);
	}

	void PressKey(const FKey& Key, bool bCtrl)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState KeyMods = Mods(false, bCtrl);
		App.ProcessKeyDownEvent(FKeyEvent(Key, KeyMods, App.GetUserIndexForKeyboard(), false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, KeyMods, App.GetUserIndexForKeyboard(), false, 0, 0));
	}

	/** True when the clipboard is empty or holds only text, the only kind FPlatformApplicationMisc saves and restores (the
	 *  rule of Dream.Keys.G5, A177): a clipboard with an image, files or rich text is not touched. */
	bool IsPlainTextClipboard()
	{
#if PLATFORM_WINDOWS
		int32 TextFormats = 0;
		for (const UINT Format : { UINT(CF_TEXT), UINT(CF_UNICODETEXT), UINT(CF_OEMTEXT), UINT(CF_LOCALE) })
		{
			TextFormats += ::IsClipboardFormatAvailable(Format) ? 1 : 0;
		}
		return ::CountClipboardFormats() == TextFormats;
#else
		return false;
#endif
	}

	/** The sequence of the newest ring line whose text is Text, 0 when none. */
	uint64 FindSeq(const FString& Text)
	{
		const FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		dream::LineView View;
		for (uint64 Seq = Store.GetEndSeq(); Seq > Store.GetFirstSeq(); --Seq)
		{
			if (Store.GetLine(Seq - 1, View) && DreamText::Utf8ToFString(View.Text) == Text)
			{
				return Seq - 1;
			}
		}
		return 0;
	}

	/** A run's rows in the view: kinds, drawn texts and stored lines. */
	struct FRows
	{
		TArray<EDreamOutputRowKind> Kinds;
		TArray<FString> Texts;
		TArray<uint64> Seqs;

		int32 IndexOfSeq(uint64 Seq) const { return Seq != 0 ? Seqs.IndexOfByKey(Seq) : INDEX_NONE; }
		int32 IndexOfText(const FString& Text) const { return Texts.IndexOfByKey(Text); }
	};

	FRows RowsOf(const SDreamOutputView& View, int32 RunNumber)
	{
		FRows Rows;
		View.GetRowsForTest(RunNumber, Rows.Kinds, Rows.Texts, Rows.Seqs);
		return Rows;
	}

	struct FState
	{
		FString Sfx;
		TArray<FString> Lines;          // the pushed lines, in order
		TArray<int32> RowIndex;         // their rows in the run-0 order of GetRowsForTest
		FString Saved;
		bool bClipboardSaved = false;
		double Mark = 0.0;
		uint64 LastSeq = 0;
		FString Expected;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputSelectionSpec, "Dream.Output.Selection", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FDreamOutputVisibility SavedVisibility;
	/** The docked Output view, active; null with an error otherwise. */
	TSharedPtr<SDreamOutputView> ShowOutputView();
	/** Pushes Lines as run-0 stdout lines (Result lines when they start with "=") and waits until the view shows them. */
	void AddPushLines(DreamTest::FDreamSteps& Steps, TSharedRef<DreamOutputSelectionSpec::FState> State);
	/** The shown row widget of pushed line Line, and its text blocks; false with an error when it is not shown. */
	bool ShownLine(const DreamOutputSelectionSpec::FState& State, int32 Line, TSharedPtr<SWidget>& OutRow, TArray<TSharedRef<STextBlock>>& OutTexts);
	void SaveClipboard(DreamOutputSelectionSpec::FState& State);
	void RestoreClipboard(DreamOutputSelectionSpec::FState& State);
END_DEFINE_SPEC(FDreamOutputSelectionSpec)

TSharedPtr<SDreamOutputView> FDreamOutputSelectionSpec::ShowOutputView()
{
	const TSharedPtr<SDreamOutputArea> Area = Tab.IsValid() ? Tab->GetOutputArea() : nullptr;
	if (!TestTrue(TEXT("the output area"), Area.IsValid()))
	{
		return nullptr;
	}
	Area->SetActiveTab(EDreamOutputTab::Output);
	const TSharedPtr<SDreamOutputView> View = Area->GetOutputView();
	TestTrue(TEXT("the Output view"), View.IsValid());
	return View;
}

void FDreamOutputSelectionSpec::AddPushLines(DreamTest::FDreamSteps& Steps, TSharedRef<DreamOutputSelectionSpec::FState> State)
{
	Steps.Do(TEXT("push the lines and follow the Output view"), [this, State]()
	{
		const TSharedPtr<SDreamOutputView> View = ShowOutputView();
		if (!View.IsValid())
		{
			return false;
		}
		View->ClearTextSelection();
		View->ScrollToEnd();
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		for (const FString& Line : State->Lines)
		{
			const bool bResult = Line.StartsWith(TEXT("="));
			Store.PushDreamLine(0, bResult ? EDreamStream::Result : EDreamStream::Stdout, bResult ? Line.Mid(1) : Line);
		}
		return true;
	});
	Steps.Poll(TEXT("the view shows every pushed line"), [this, State]()
	{
		FDreamEditorModule::Get().GetOutputStore().Drain(0.01);
		const TSharedPtr<SDreamOutputView> View = ShowOutputView();
		if (!View.IsValid())
		{
			return DreamTest::EStep::Fail;
		}
		const DreamOutputSelectionSpec::FRows Rows = DreamOutputSelectionSpec::RowsOf(*View, 0);
		State->RowIndex.Reset();
		for (const FString& Line : State->Lines)
		{
			const FString Stored = Line.StartsWith(TEXT("=")) ? Line.Mid(1) : Line;
			const int32 Index = Rows.IndexOfSeq(DreamOutputSelectionSpec::FindSeq(Stored));
			if (Index == INDEX_NONE)
			{
				return DreamTest::EStep::Wait;
			}
			State->RowIndex.Add(Index);
		}
		return DreamTest::EStep::Next;
	});
	Steps.WaitFrames(5);
}

bool FDreamOutputSelectionSpec::ShownLine(const DreamOutputSelectionSpec::FState& State, int32 Line, TSharedPtr<SWidget>& OutRow,
	TArray<TSharedRef<STextBlock>>& OutTexts)
{
	const TSharedPtr<SDreamOutputView> View = ShowOutputView();
	OutRow = View.IsValid() && State.RowIndex.IsValidIndex(Line) ? View->GetRunRowWidgetForTest(0, State.RowIndex[Line]) : nullptr;
	OutTexts = DreamOutputSelectionSpec::TextBlocks(OutRow);
	const bool bShown = OutRow.IsValid() && !OutTexts.IsEmpty() && OutTexts[0]->GetPaintSpaceGeometry().GetLocalSize().X > 0.0;
	return TestTrue(*FString::Printf(TEXT("line %d (\"%s\") is shown and laid out"), Line, State.Lines.IsValidIndex(Line) ? *State.Lines[Line] : TEXT("?")), bShown);
}

void FDreamOutputSelectionSpec::SaveClipboard(DreamOutputSelectionSpec::FState& State)
{
	State.bClipboardSaved = DreamOutputSelectionSpec::IsPlainTextClipboard();
	if (State.bClipboardSaved)
	{
		FPlatformApplicationMisc::ClipboardPaste(State.Saved);
	}
}

void FDreamOutputSelectionSpec::RestoreClipboard(DreamOutputSelectionSpec::FState& State)
{
	if (State.bClipboardSaved)
	{
		FPlatformApplicationMisc::ClipboardCopy(*State.Saved);
		State.bClipboardSaved = false;
	}
}

void FDreamOutputSelectionSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		// Every line type shown, through the store as the Show menu sets it; bEchoInput is not written. Restored in AfterEach.
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		SavedVisibility = Store.GetVisibility();
		FDreamOutputVisibility All = SavedVisibility;
		All.bStdout = All.bStderr = All.bResults = All.bInfo = All.bWarnings = All.bErrors = All.bEcho = All.bNotice = true;
		Store.SetVisibility(All);
	});

	AfterEach([this]()
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
		}
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetOutputStore().SetVisibility(SavedVisibility);
		}
		if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
		{
			if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
			{
				View->ClearTextSelection();
				View->ScrollToEnd();
			}
		}
		Tab.Reset();
	});

	LatentIt("drags across rows and copies exactly the selected characters", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputSelectionSpec;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		for (int32 Index = 0; Index < 5; ++Index)
		{
			State->Lines.Add(FString::Printf(TEXT("sel_%s line_%d alpha beta"), *State->Sfx, Index));
		}
		DreamTest::FDreamSteps Steps(*this);
		AddPushLines(Steps, State);
		Steps.Do(TEXT("press in line 1, drag to line 3, release"), [this, State]()
		{
			TSharedPtr<SWidget> RowA;
			TSharedPtr<SWidget> RowB;
			TArray<TSharedRef<STextBlock>> TextsA;
			TArray<TSharedRef<STextBlock>> TextsB;
			if (!ShownLine(*State, 1, RowA, TextsA) || !ShownLine(*State, 3, RowB, TextsB))
			{
				return false;
			}
			const int32 From = 4;
			const int32 To = 7;
			if (!TestTrue(TEXT("the drag is routed"), Drag(TextsA[0], PointInText(TextsA[0], From), TextsB[0], PointInText(TextsB[0], To))))
			{
				return false;
			}
			State->Expected = State->Lines[1].Mid(From) + TEXT("\n") + State->Lines[2] + TEXT("\n") + State->Lines[3].Left(To);
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			TestTrue(TEXT("the drag selected text"), View->HasTextSelection());
			TestEqual(TEXT("exactly the characters from the press to the release"), View->GetSelectedText(), State->Expected);
			TestTrue(TEXT("the view still follows the newest line"), View->IsFollowing());
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the selection is painted from the press point to the release point"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			TSharedPtr<SWidget> RowA;
			TSharedPtr<SWidget> RowB;
			TArray<TSharedRef<STextBlock>> TextsA;
			TArray<TSharedRef<STextBlock>> TextsB;
			if (!ShownLine(*State, 1, RowA, TextsA) || !ShownLine(*State, 3, RowB, TextsB))
			{
				return false;
			}
			const TArray<FSlateRect> First = View->GetSelectionRectsForTest(0, State->RowIndex[1]);
			const TArray<FSlateRect> Middle = View->GetSelectionRectsForTest(0, State->RowIndex[2]);
			const TArray<FSlateRect> Last = View->GetSelectionRectsForTest(0, State->RowIndex[3]);
			const TArray<FSlateRect> After = View->GetSelectionRectsForTest(0, State->RowIndex[4]);
			if (!TestTrue(TEXT("the three rows paint a selection"), First.Num() > 0 && Middle.Num() > 0 && Last.Num() > 0))
			{
				return false;
			}
			TestEqual(TEXT("the row below paints none"), After.Num(), 0);
			// The first row's band starts at the press boundary, the last row's ends at the release boundary (1 px tolerance
			// for the 1 px the points sit right of their boundaries, plus rounding).
			const double PressX = PointInText(TextsA[0], 4).X - 1.0;
			const double ReleaseX = PointInText(TextsB[0], 7).X - 1.0;
			TestTrue(*FString::Printf(TEXT("the first band starts at the press boundary (%.1f vs %.1f)"), First[0].Left, PressX),
				FMath::Abs(First[0].Left - PressX) <= 2.0);
			TestTrue(*FString::Printf(TEXT("the last band ends at the release boundary (%.1f vs %.1f)"), Last[0].Right, ReleaseX),
				FMath::Abs(Last[0].Right - ReleaseX) <= 2.0);
			return true;
		});
		Steps.Do(TEXT("Ctrl+C and the context menu's Copy copy it"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!IsPlainTextClipboard())
			{
				AddInfo(TEXT("The clipboard holds more than text: the copies are checked through GetSelectedText only (A177)."));
				return true;
			}
			SaveClipboard(*State);
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
			TestTrue(*FString::Printf(TEXT("the press focused the rows (%s)"), Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none")),
				Focused.IsValid() && View->IsFocusList(*Focused));
			FPlatformApplicationMisc::ClipboardCopy(TEXT("before"));
			PressKey(EKeys::C, /*bCtrl*/ true);
			FString Copied;
			FPlatformApplicationMisc::ClipboardPaste(Copied);
			TestEqual(TEXT("Ctrl+C copies exactly the selected characters"), Copied, State->Expected);
			TestTrue(TEXT("the context menu offers Copy"), View->GetContextMenuLabelsForTest().Contains(TEXT("Copy")));
			FPlatformApplicationMisc::ClipboardCopy(TEXT("before"));
			TestTrue(TEXT("Copy runs"), View->InvokeContextMenuEntryForTest(TEXT("Copy")));
			FPlatformApplicationMisc::ClipboardPaste(Copied);
			TestEqual(TEXT("the context menu's Copy copies the selection"), Copied, State->Expected);
			RestoreClipboard(*State);
			return true;
		});
		Steps.Do(TEXT("Shift+click in line 4 extends the selection"), [this, State]()
		{
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 4, Row, Texts))
			{
				return false;
			}
			const FVector2D At = PointInText(Texts[0], 2);
			if (!TestTrue(TEXT("the Shift+click is routed"), Press(Texts[0], At, /*bShift*/ true) && Release(Texts[0], At, /*bShift*/ true)))
			{
				return false;
			}
			State->Expected = State->Lines[1].Mid(4) + TEXT("\n") + State->Lines[2] + TEXT("\n") + State->Lines[3] + TEXT("\n") + State->Lines[4].Left(2);
			TestEqual(TEXT("from the first press to the Shift+click"), ShowOutputView()->GetSelectedText(), State->Expected);
			return true;
		});
		Steps.Do(TEXT("push a line: following goes on and the selection stays"), [this, State]()
		{
			FDreamEditorModule::Get().GetOutputStore().PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("sel_%s later"), *State->Sfx));
			return true;
		});
		Steps.Poll(TEXT("the later line arrives"), [State]()
		{
			FDreamEditorModule::Get().GetOutputStore().Drain(0.01);
			return DreamOutputSelectionSpec::FindSeq(FString::Printf(TEXT("sel_%s later"), *State->Sfx)) != 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check, then click without a drag in line 0"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			TestTrue(TEXT("still following after the new line (OUT-09)"), View->IsFollowing());
			TestEqual(TEXT("the selection is the same"), View->GetSelectedText(), State->Expected);
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 0, Row, Texts))
			{
				return false;
			}
			const FVector2D At = PointInText(Texts[0], 3);
			if (!TestTrue(TEXT("the click is routed"), Press(Texts[0], At) && Release(Texts[0], At)))
			{
				return false;
			}
			TestFalse(TEXT("a click without a drag drops the text selection"), View->HasTextSelection());
			TestEqual(TEXT("and selects its row, copied raw as before (A209)"), View->GetSelectedText(), State->Lines[0]);
			return true;
		});
		Steps.Finally(TEXT("restore the clipboard and follow"), [this, State]()
		{
			RestoreClipboard(*State);
			FSlateApplication::Get().ReleaseAllPointerCapture();
		});
		Steps.Run(Done);
	});

	LatentIt("selects a word on a double-click, a line on a triple-click, and everything on Ctrl+A", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputSelectionSpec;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		State->Lines.Add(FString::Printf(TEXT("word_%s first.second(third)"), *State->Sfx));
		State->Lines.Add(FString::Printf(TEXT("=%s4242"), *State->Sfx));       // a result line: drawn "# Result: <sfx>4242 #"
		DreamTest::FDreamSteps Steps(*this);
		AddPushLines(Steps, State);
		Steps.Do(TEXT("double-click on 'second', then press again"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 0, Row, Texts))
			{
				return false;
			}
			const FString Text = Texts[0]->GetText().ToString();
			const int32 Second = Text.Find(TEXT("second"));
			const FVector2D At = PointInText(Texts[0], Second + 2);
			if (!TestTrue(TEXT("the first click and the double-click are routed"), Press(Texts[0], At) && Release(Texts[0], At) && DoubleClick(Texts[0], At)))
			{
				return false;
			}
			TestEqual(TEXT("a double-click selects the word"), View->GetSelectedText(), FString(TEXT("second")));
			if (!TestTrue(TEXT("the third press is routed"), Press(Texts[0], At) && Release(Texts[0], At)))
			{
				return false;
			}
			TestEqual(TEXT("a triple-click selects the whole line"), View->GetSelectedText(), State->Lines[0]);
			return true;
		});
		Steps.Do(TEXT("triple-click the result line: decorations as drawn"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 1, Row, Texts))
			{
				return false;
			}
			// The second text block is the result's own text ("# Result: " is the first).
			const TSharedRef<STextBlock> Text = Texts.Last();
			const FVector2D At = PointInText(Text, 2);
			if (!TestTrue(TEXT("the clicks are routed"), Press(Text, At) && Release(Text, At) && DoubleClick(Text, At) && Press(Text, At) && Release(Text, At)))
			{
				return false;
			}
			TestEqual(TEXT("the copy has the decorations as drawn"), View->GetSelectedText(), FString::Printf(TEXT("# Result: %s4242 #"), *State->Sfx));
			return true;
		});
		Steps.Do(TEXT("Ctrl+A selects every shown row"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
			if (!TestTrue(TEXT("the rows have keyboard focus after the clicks"), Focused.IsValid() && View->IsFocusList(*Focused)))
			{
				return false;
			}
			PressKey(EKeys::A, /*bCtrl*/ true);
			const FString All = View->GetSelectedText();
			TestTrue(TEXT("Ctrl+A selects text"), View->HasTextSelection());
			TestTrue(TEXT("from the first row"), All.Len() > 0 && All.Contains(State->Lines[0]));
			TestTrue(TEXT("to the last one, as drawn"), All.EndsWith(FString::Printf(TEXT("# Result: %s4242 #"), *State->Sfx)));
			return true;
		});
		Steps.Finally(TEXT("release the pointer"), []()
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
		});
		Steps.Run(Done);
	});

	LatentIt("still opens a link on a click without a drag, and selects on a drag from it", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputSelectionSpec;
		const TSharedPtr<FDreamBuffer> Target = DreamTest::OpenScratch(*this, *Tab, TEXT("first = 1\nsecond = 2\nthird = 3\n"), TEXT("SelLink"));
		if (!Target.IsValid())
		{
			Done.Execute();
			return;
		}
		const FGuid TargetId = Target->GetId();
		TSharedRef<FGuid> OtherId = MakeShared<FGuid>();
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		const FString Filename = FDreamEditorModule::Get().GetBufferManager().GetCompileFilename(*Target);
		State->Lines.Add(FString::Printf(TEXT("  File \"%s\", line 2, in link_%s"), *Filename, *State->Sfx));
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("open another tab"), [this, OtherId]()
		{
			const TSharedPtr<FDreamBuffer> Other = DreamTest::OpenScratch(*this, *Tab, TEXT("pass\n"), TEXT("SelOther"));
			*OtherId = Other.IsValid() ? Other->GetId() : FGuid();
			return Other.IsValid();
		});
		AddPushLines(Steps, State);
		Steps.Do(TEXT("click the link without a drag"), [this, State, TargetId]()
		{
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 0, Row, Texts))
			{
				return false;
			}
			TArray<TSharedRef<SWidget>> Links;
			Collect(Row.ToSharedRef(), [](const TSharedRef<SWidget>& Widget) { return Widget->GetType() == FName(TEXT("SHyperlink")); }, Links);
			if (!TestEqual(TEXT("the row has one link"), Links.Num(), 1) || !TestTrue(TEXT("the link is enabled (its file is open)"), Links[0]->IsEnabled()))
			{
				return false;
			}
			const FVector2D At(Links[0]->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
			if (!TestTrue(TEXT("the click is routed"), Press(Links[0], At) && Release(Links[0], At)))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			TestTrue(TEXT("the click opens the link's tab"), Active.IsValid() && Active->GetId() == TargetId);
			if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
			{
				int32 Line = 0;
				int32 Column = 0;
				Code->GetCaret(Line, Column);
				TestEqual(TEXT("at line 2 (0-based 1)"), Line, 1);
			}
			TestFalse(TEXT("a click selects no text"), ShowOutputView()->HasTextSelection());
			return true;
		});
		Steps.Do(TEXT("back to the other tab"), [this, OtherId]()
		{
			Tab->ActivateBuffer(*OtherId);
			return ShowOutputView().IsValid();
		});
		Steps.WaitFrames(4);
		Steps.Do(TEXT("drag from the link to the end of the row"), [this, State, OtherId]()
		{
			TSharedPtr<SWidget> Row;
			TArray<TSharedRef<STextBlock>> Texts;
			if (!ShownLine(*State, 0, Row, Texts))
			{
				return false;
			}
			TArray<TSharedRef<SWidget>> Links;
			Collect(Row.ToSharedRef(), [](const TSharedRef<SWidget>& Widget) { return Widget->GetType() == FName(TEXT("SHyperlink")); }, Links);
			if (!TestEqual(TEXT("the row still has its link"), Links.Num(), 1))
			{
				return false;
			}
			const FVector2D From(Links[0]->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
			const TSharedRef<STextBlock> Tail = Texts.Last();
			const FVector2D To = PointInText(Tail, Tail->GetText().ToString().Len()) + FVector2D(8.0, 0.0);
			if (!TestTrue(TEXT("the drag is routed"), Drag(Links[0], From, Tail, To)))
			{
				return false;
			}
			const FString Selected = ShowOutputView()->GetSelectedText();
			const FString Tip = FString::Printf(TEXT(", line 2, in link_%s"), *State->Sfx);
			TestTrue(*FString::Printf(TEXT("the drag selected the rest of the row from inside the link: \"%s\""), *Selected),
				Selected.EndsWith(Tip) && Selected.Len() > Tip.Len() && State->Lines[0].EndsWith(Selected));
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			TestTrue(TEXT("and opened nothing"), Active.IsValid() && Active->GetId() == *OtherId);
			return true;
		});
		Steps.Finally(TEXT("close the scratch tabs"), [TargetId, OtherId]()
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
			DreamTest::CloseBuffer(*OtherId);
			DreamTest::CloseBuffer(TargetId);
		});
		Steps.Run(Done);
	});

	LatentIt("selects in the Run window's live view and hands the selection to the docked view", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputSelectionSpec;
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString LineA = FString::Printf(TEXT("live_a_%s one"), *Sfx);
		const FString LineB = FString::Printf(TEXT("live_b_%s two"), *Sfx);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<TSharedPtr<SWindow>> Window = MakeShared<TSharedPtr<SWindow>>();
		TSharedRef<TSharedPtr<SDreamOutputView>> Live = MakeShared<TSharedPtr<SDreamOutputView>>();
		TSharedRef<FString> Expected = MakeShared<FString>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a REPL run with two lines"), [LineA, LineB]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('%s')\nprint('%s')"), *LineA, *LineB));
		}, Run);
		Steps.Do(TEXT("show the run in a live run view"), [this, Run, Window, Live]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			SAssignNew(*Live, SDreamOutputView).RunFilter(TOptional<int32>(Run->RunNumber)).bLiveRunView(true);
			*Window = SNew(SWindow)
				.Title(FText::AsCultureInvariant(TEXT("Dream.Output.Selection live view")))
				.ClientSize(FVector2D(640.0, 240.0))
				.SupportsMinimize(false)
				.SupportsMaximize(false)
				[
					Live->ToSharedRef()
				];
			FSlateApplication::Get().AddWindow(Window->ToSharedRef());
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("drag in the live view from line A to line B"), [this, Run, Live, LineA, LineB, Expected]()
		{
			const FRows Rows = RowsOf(**Live, Run->RunNumber);
			const int32 A = Rows.IndexOfText(LineA);
			const int32 B = Rows.IndexOfText(LineB);
			const TSharedPtr<SWidget> RowA = A != INDEX_NONE ? (*Live)->GetRunRowWidgetForTest(Run->RunNumber, A) : nullptr;
			const TSharedPtr<SWidget> RowB = B != INDEX_NONE ? (*Live)->GetRunRowWidgetForTest(Run->RunNumber, B) : nullptr;
			const TArray<TSharedRef<STextBlock>> TextsA = TextBlocks(RowA);
			const TArray<TSharedRef<STextBlock>> TextsB = TextBlocks(RowB);
			if (!TestTrue(*FString::Printf(TEXT("the live view shows both lines: %s"), *FString::Join(Rows.Texts, TEXT(" | "))), !TextsA.IsEmpty() && !TextsB.IsEmpty()))
			{
				return false;
			}
			if (!TestTrue(TEXT("the drag is routed"), Drag(TextsA[0], PointInText(TextsA[0], 0), TextsB[0], PointInText(TextsB[0], 6))))
			{
				return false;
			}
			// The rows from A to B as drawn (a statement's echo may sit between them), B up to its sixth character.
			TArray<FString> Between;
			for (int32 Index = A; Index <= B; ++Index)
			{
				Between.Add(Index == B ? Rows.Texts[Index].Left(6) : Rows.Texts[Index]);
			}
			*Expected = FString::Join(Between, TEXT("\n"));
			TestEqual(TEXT("the live view selects exactly the dragged characters"), (*Live)->GetSelectedText(), *Expected);
			return true;
		});
		Steps.Do(TEXT("hand off to the docked view"), [this, Run, Live, Expected]()
		{
			const FDreamOutputViewHandoff Handoff = (*Live)->CaptureHandoff();
			TestTrue(TEXT("the hand-off carries the selection"), Handoff.SelectionAnchorSeq != 0 && Handoff.SelectionCaretSeq != 0);
			const TSharedPtr<SDreamOutputView> Docked = ShowOutputView();
			if (!Docked.IsValid())
			{
				return false;
			}
			Docked->ApplyHandoff(Handoff);
			TestTrue(TEXT("the docked view has the selection"), Docked->HasTextSelection());
			TestEqual(TEXT("the same characters"), Docked->GetSelectedText(), *Expected);
			const TArray<FString> Labels = Docked->GetContextMenuLabelsForTest();
			TestTrue(*FString::Printf(TEXT("its context menu keeps Copy and the Run #N section: %s"), *FString::Join(Labels, TEXT(", "))),
				Labels.Contains(TEXT("Copy")) && Labels.Contains(TEXT("Re-run")));
			return true;
		});
		Steps.Finally(TEXT("close the live view's window"), [Window, Live]()
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
			if (Window->IsValid())
			{
				(*Window)->RequestDestroyWindow();
			}
			Window->Reset();
			Live->Reset();
		});
		Steps.Run(Done);
	});

	LatentIt("stays fast on a 100000-line ring", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputSelectionSpec;
		constexpr int32 Total = 100000;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		// Run-0 stdout lines go to LogPython under the echo cap (D19): its summaries are expected, in any number.
		AddExpectedMessagePlain(TEXT("lines not echoed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		DreamTest::FDreamSteps Steps(*this);
		Steps.Do(TEXT("push 100,000 lines"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ClearTextSelection();
			View->ScrollToEnd();
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			for (int32 Index = 0; Index < Total; ++Index)
			{
				Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("perf_%s %06d lorem ipsum dolor sit amet"), *State->Sfx, Index));
			}
			return true;
		});
		Steps.Poll(TEXT("the view has every line"), [State]()
		{
			FDreamEditorModule::Get().GetOutputStore().Drain(0.05);
			State->LastSeq = DreamOutputSelectionSpec::FindSeq(FString::Printf(TEXT("perf_%s %06d lorem ipsum dolor sit amet"), *State->Sfx, Total - 1));
			return State->LastSeq != 0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 60.0);
		Steps.WaitFrames(5);
		Steps.Do(TEXT("Ctrl+A and copy every line"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const int32 Shown = View->GetShownRowWidgetsForTest().Num();
			TestTrue(*FString::Printf(TEXT("virtualized: %d row widgets for over 100,000 rows"), Shown), Shown > 0 && Shown < 400);
			const TSharedPtr<SWidget> List = [&View]() -> TSharedPtr<SWidget>
			{
				TArray<TSharedRef<SWidget>> Found;
				DreamOutputSelectionSpec::Collect(View.ToSharedRef(), [&View](const TSharedRef<SWidget>& Widget) { return View->IsFocusList(*Widget); }, Found);
				return Found.IsEmpty() ? nullptr : TSharedPtr<SWidget>(Found[0]);
			}();
			if (!TestTrue(TEXT("the rows' list"), List.IsValid()))
			{
				return false;
			}
			FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly);
			const double Start = FPlatformTime::Seconds();
			PressKey(EKeys::A, /*bCtrl*/ true);
			const double SelectSeconds = FPlatformTime::Seconds() - Start;
			const double CopyStart = FPlatformTime::Seconds();
			const FString All = View->GetSelectedText();
			const double CopySeconds = FPlatformTime::Seconds() - CopyStart;
			int32 Breaks = 0;
			for (const TCHAR Char : All)
			{
				Breaks += Char == TEXT('\n') ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("Ctrl+A %.1f ms, copy of %d lines (%d characters) %.1f ms"), SelectSeconds * 1000.0, Breaks + 1, All.Len(), CopySeconds * 1000.0));
			TestTrue(TEXT("every line is selected"), View->HasTextSelection() && Breaks + 1 >= Total);
			TestTrue(*FString::Printf(TEXT("Ctrl+A takes under 500 ms (%.1f ms)"), SelectSeconds * 1000.0), SelectSeconds < 0.5);
			TestTrue(*FString::Printf(TEXT("copying every line takes under 3 s (%.1f ms)"), CopySeconds * 1000.0), CopySeconds < 3.0);
			State->Mark = FPlatformTime::Seconds();
			return true;
		});
		Steps.WaitFrames(20);
		Steps.Do(TEXT("frames with everything selected, then a drag"), [this, State]()
		{
			const double FrameSeconds = (FPlatformTime::Seconds() - State->Mark) / 20.0;
			AddInfo(FString::Printf(TEXT("%.1f ms per frame with 100,000 rows selected"), FrameSeconds * 1000.0));
			TestTrue(*FString::Printf(TEXT("a frame takes under 250 ms with every row selected (%.1f ms)"), FrameSeconds * 1000.0), FrameSeconds < 0.25);
			const TSharedPtr<SDreamOutputView> View = ShowOutputView();
			const FRows Rows = RowsOf(*View, 0);
			const int32 Last = Rows.IndexOfSeq(State->LastSeq);
			const TArray<TSharedRef<STextBlock>> TextsA = TextBlocks(Last >= 4 ? View->GetRunRowWidgetForTest(0, Last - 4) : nullptr);
			const TArray<TSharedRef<STextBlock>> TextsB = TextBlocks(Last >= 0 ? View->GetRunRowWidgetForTest(0, Last) : nullptr);
			if (!TestTrue(TEXT("the newest rows are shown"), !TextsA.IsEmpty() && !TextsB.IsEmpty()))
			{
				return false;
			}
			const double Start = FPlatformTime::Seconds();
			const bool bRouted = Drag(TextsA[0], PointInText(TextsA[0], 5), TextsB[0], PointInText(TextsB[0], 9));
			const double DragSeconds = FPlatformTime::Seconds() - Start;
			AddInfo(FString::Printf(TEXT("a drag over 5 rows (4 events) %.2f ms"), DragSeconds * 1000.0));
			TestTrue(TEXT("the drag is routed"), bRouted);
			TestTrue(*FString::Printf(TEXT("the drag's events take under 100 ms on the large ring (%.2f ms)"), DragSeconds * 1000.0), DragSeconds < 0.1);
			const FString Selected = View->GetSelectedText();
			int32 Breaks = 0;
			for (const TCHAR Char : Selected)
			{
				Breaks += Char == TEXT('\n') ? 1 : 0;
			}
			TestEqual(TEXT("the drag selects five rows"), Breaks, 4);
			return true;
		});
		Steps.Finally(TEXT("drop the selection and clear the large ring"), [this]()
		{
			FSlateApplication::Get().ReleaseAllPointerCapture();
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->ClearTextSelection();
				}
			}
			if (FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().Clear();
			}
		});
		Steps.Run(Done);
	});
}
