// SPDX-License-Identifier: Apache-2.0
// Dream.Restart.Layout: T-GAP-019 (WS-03, WS-02, PO-01), one step per editor session of DreamHost/Scripts/run_restart.ps1,
// which starts each session with -DreamRestartStep=<n> -DreamRestartDir=<dir> on a fresh profile (the driver removes the
// layout keys first and restores both ini files after). Each step checks what the previous session left, changes the
// layout, writes <dir>/step<n>.txt (key=value lines: ok, errors, heights) and asks for a normal editor exit, so OnPreExit
// saves the session state as a user's exit does (an automation Quit would skip it).
//   1: the fresh profile shows the output area above the code area at exactly 40 % of the centre column, the prompt strip
//      at the bottom of the output area (when the REPL prompt is shown: PO-04 hides it at the defaults), both areas
//      >= 120 px; View > Output Position > Bottom (MoveOutputToBottom) moves it below the code area at once with the same
//      height.
//   2: after the restart it is still at the bottom with the same height; Move Output to Top (the palette is M2, so the
//      command) moves it back with the same height.
//   3: after the restart it is on top; a splitter drag with the input bot to the top end leaves it at 120 px.
//   4: after the restart the dragged height survived; a drag to the bottom end leaves the code area at 120 px.
//   5: after the restart that height survived.
// (scratch, PO-05, PER-02) Steps 1 and 2 also check that the open tabs come back: step 1 opens a file tab and a scratch
// tab (scratch_N.py), puts each one's caret and scroll somewhere, makes the scratch tab active after the file tab, and
// types into the scratch tab right before the exit (no auto-save can run in between: only the exit save writes it); step
// 2 finds both tabs in that order with the scratch tab active, each caret and first visible line as they were, and the
// scratch tab's text, typed last, in its file and clean (from the file, not only from the journal); it closes both.
// In the shared tier-4 pass (no -DreamRestartStep) the spec only says how to run it.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"            // (scratch) PER-02
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Settings/DreamPerProjectUserSettings.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "Containers/Ticker.h"

namespace DreamRestartLayoutSpec
{
	struct FLayout
	{
		bool bValid = false;
		bool bOutputOnTop = true;
		FSlateRect Output;          // desktop pixels
		FSlateRect Code;
		FSlateRect Strip;
		float OutputHeight = 0.f;   // Slate units
		float CodeHeight = 0.f;
		float Scale = 1.f;
	};

	/** The centre splitter's two children: the output area and the code area (whichever order the layout has). */
	FLayout Measure(SDreamTab& Tab)
	{
		FLayout L;
		const TSharedPtr<SDreamOutputArea> Area = Tab.GetOutputArea();
		if (!Area.IsValid())
		{
			return L;
		}
		TSharedPtr<SWidget> Splitter;
		TSharedPtr<SWidget> OutputSlot;
		for (TSharedPtr<SWidget> W = Area; W.IsValid(); W = W->GetParentWidget())
		{
			const TSharedPtr<SWidget> Parent = W->GetParentWidget();
			if (Parent.IsValid() && Parent->GetType() == FName(TEXT("SSplitter")))
			{
				Splitter = Parent;
				OutputSlot = W;
				break;
			}
		}
		if (!Splitter.IsValid() || Splitter->GetChildren()->Num() != 2)
		{
			return L;
		}
		FChildren* Children = Splitter->GetChildren();
		const TSharedRef<SWidget> First = Children->GetChildAt(0);
		const TSharedRef<SWidget> Second = Children->GetChildAt(1);
		const TSharedRef<SWidget> CodeSlot = First == OutputSlot ? Second : First;
		L.bOutputOnTop = First == OutputSlot;
		const FGeometry& OutGeo = OutputSlot->GetPaintSpaceGeometry();
		const FGeometry& CodeGeo = CodeSlot->GetPaintSpaceGeometry();
		L.OutputHeight = OutGeo.GetLocalSize().Y;
		L.CodeHeight = CodeGeo.GetLocalSize().Y;
		L.Scale = OutGeo.Scale;
		DreamTest::FDreamTestProbes::GetDesktopRect(OutputSlot, L.Output);
		DreamTest::FDreamTestProbes::GetDesktopRect(CodeSlot, L.Code);
		if (const TSharedPtr<SDreamReplStrip> Strip = Area->GetReplStrip())
		{
			DreamTest::FDreamTestProbes::GetDesktopRect(Strip, L.Strip);
		}
		L.bValid = L.OutputHeight > 0.f && L.CodeHeight > 0.f;
		return L;
	}

	FString Describe(const FLayout& L)
	{
		return FString::Printf(TEXT("output %s, %.1f / code %.1f Slate units (%.4f of the column)"), L.bOutputOnTop ? TEXT("on top") : TEXT("at the bottom"),
			L.OutputHeight, L.CodeHeight, L.OutputHeight / FMath::Max(1.f, L.OutputHeight + L.CodeHeight));
	}

	// ---- (scratch, PO-05, PER-02) the tabs that come back ----

	/** The text typed into the scratch tab right before step 1's exit. */
	const TCHAR* const LastEdit = TEXT("last_edit_before_exit = 1\n");

	FDreamBufferManager& Buffers()
	{
		return FDreamEditorModule::Get().GetBufferManager();
	}

	FString Lines(const TCHAR* Prefix, int32 Count)
	{
		FString Text;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Text += FString::Printf(TEXT("%s_%03d = %d\n"), Prefix, Index, Index);
		}
		return Text;
	}

	FString ReadText(const FString& Path)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path) ? Text.Replace(TEXT("\r\n"), TEXT("\n")) : FString(TEXT("<missing>"));
	}

	/** The first line at least half shown (the rule SDreamTab saves the scroll with, PER-02). */
	int32 FirstShownLine(const SDreamCodeView& View)
	{
		TArray<FDreamLineGeometry> Shown;
		View.GetVisibleLines(Shown);
		for (const FDreamLineGeometry& Line : Shown)
		{
			if (Line.Top + Line.Height * 0.5f >= 0.f)
			{
				return Line.Line;
			}
		}
		return Shown.Num() > 0 ? Shown.Last().Line : -1;
	}

	FString CaretOf(SDreamCodeView& View)
	{
		int32 Line = 0;
		int32 Column = 0;
		View.GetCaret(Line, Column);
		return FString::Printf(TEXT("%d:%d"), Line, Column);
	}

	TMap<FString, FString> ReadStep(const FString& Dir, int32 Step)
	{
		TMap<FString, FString> Values;
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *FPaths::Combine(Dir, FString::Printf(TEXT("step%d.txt"), Step)));
		for (const FString& Line : Lines)
		{
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Values.Add(Key, Value);
			}
		}
		return Values;
	}
}

BEGIN_DEFINE_SPEC(FDreamRestartLayoutSpec, "Dream.Restart.Layout", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	int32 Step = 0;
	FString Dir;
	TArray<FString> Infos;
	/** Writes the step's result file and, a moment after the test ends, asks for a normal exit (OnPreExit saves the session). */
	void Finish(const TMap<FString, FString>& Values);
	/** A drag of the splitter handle by the input bot to the point Target (desktop pixels). */
	TSharedRef<DreamTest::FDreamInputBotTicket> Drag(const DreamRestartLayoutSpec::FLayout& L, float TargetY);
	/** (scratch, PER-02) Step 1's scratch tab: the exit types into it last. */
	FGuid Per2ScratchId;
	/** (scratch, PER-02) Step 1: the file tab and the scratch tab with their carets and scrolls, kept open past the test. */
	void AddPer2Setup(DreamTest::FDreamSteps& Steps, TSharedRef<TMap<FString, FString>> Values);
	/** (scratch, PER-02) Step 2: both are back as they were; then they are closed. */
	void AddPer2Check(DreamTest::FDreamSteps& Steps, const TMap<FString, FString>& Previous);
END_DEFINE_SPEC(FDreamRestartLayoutSpec)

void FDreamRestartLayoutSpec::Finish(const TMap<FString, FString>& Values)
{
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("ok=%d"), HasAnyErrors() ? 0 : 1));
	for (const TPair<FString, FString>& Pair : Values)
	{
		Lines.Add(Pair.Key + TEXT("=") + Pair.Value);
	}
	for (const FString& Info : Infos)
	{
		Lines.Add(TEXT("info=") + Info);
	}
	FFileHelper::SaveStringArrayToFile(Lines, *FPaths::Combine(Dir, FString::Printf(TEXT("step%d.txt"), Step)));
	// The spec object outlives the test: a strong reference would keep SDreamTab alive past Slate's shutdown in the normal
	// exit below, where FDreamPython's shutdown state change still reaches its banner rebuild.
	Tab.Reset();
	// After the automation framework has recorded the test (a second later), a normal exit, as a user's File > Exit.
	// (scratch, PER-02) Right before it, the last edit of step 1's scratch tab: only the exit save can write it.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([ScratchId = Per2ScratchId, StepFile = FPaths::Combine(Dir, FString::Printf(TEXT("step%d.txt"), Step))](float)
	{
		if (ScratchId.IsValid() && FDreamEditorModule::IsAvailable())
		{
			const TSharedPtr<SDreamTab> Live = FDreamEditorModule::Get().GetTab();
			const TSharedPtr<SDreamCodeView> View = Live.IsValid() ? Live->FindCodeView(ScratchId) : nullptr;
			if (View.IsValid())
			{
				// Typed at the caret as a user types (the view's own edit, so the caret moves past it); the caret at the
				// exit is what the next session must show.
				View->InsertAtCaret(DreamRestartLayoutSpec::LastEdit);
				FFileHelper::SaveStringToFile(TEXT("per02_scratch_caret_exit=") + DreamRestartLayoutSpec::CaretOf(*View) + TEXT("\n"), *StepFile,
					FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
			}
		}
		FPlatformMisc::RequestExit(/*Force*/ false, TEXT("Dream.Restart.Layout"));
		return false;
	}), 1.0f);
}

void FDreamRestartLayoutSpec::AddPer2Setup(DreamTest::FDreamSteps& Steps, TSharedRef<TMap<FString, FString>> Values)
{
	using namespace DreamRestartLayoutSpec;
	TSharedRef<FGuid> FileId = MakeShared<FGuid>();
	Steps.Do(TEXT("PER-02: open a file tab at line 150 and scroll it"), [this, FileId]()
	{
		const FString FilePath = FPaths::Combine(Dir, TEXT("per02_file.py"));
		if (!TestTrue(TEXT("the file tab's file"), FFileHelper::SaveStringToFile(Lines(TEXT("file_line"), 300), *FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) || !TestTrue(TEXT("it opens"), Tab->OpenLocation(FilePath, 0, 0)))
		{
			return false;
		}
		const TSharedPtr<FDreamBuffer> File = Buffers().FindByPath(FilePath);
		const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
		if (!TestTrue(TEXT("the file tab is active"), File.IsValid() && View.IsValid() && Tab->GetActiveBuffer() == File))
		{
			return false;
		}
		*FileId = File->GetId();
		DreamTest::KeepTabOpenAfterTest(*FileId);
		View->SetCaret(150, 4);
		View->ScrollToLine(150, 3);
		return true;
	});
	Steps.WaitFrames(5);
	Steps.Do(TEXT("PER-02: record it; open a scratch tab at line 60 and scroll it"), [this, FileId, Values]()
	{
		const TSharedPtr<FDreamBuffer> File = Buffers().FindById(*FileId);
		const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(*FileId);
		if (!File.IsValid() || !View.IsValid())
		{
			AddError(TEXT("the file tab is gone"));
			return false;
		}
		Values->Add(TEXT("per02_file_id"), File->GetIdHex());
		Values->Add(TEXT("per02_file_path"), File->GetPath());
		Values->Add(TEXT("per02_file_caret"), CaretOf(*View));
		Values->Add(TEXT("per02_file_first"), FString::FromInt(FirstShownLine(*View)));
		const TSharedRef<FDreamBuffer> Scratch = Tab->NewScratchTab(Lines(TEXT("scratch_line"), 120));
		if (!TestTrue(*FString::Printf(TEXT("the new tab %s is a scratch file"), *Scratch->GetDisplayName()), Scratch->IsScratchFile()))
		{
			return false;
		}
		Per2ScratchId = Scratch->GetId();
		DreamTest::KeepTabOpenAfterTest(Per2ScratchId);
		const TSharedPtr<SDreamCodeView> ScratchView = Tab->GetActiveCodeView();
		if (!TestTrue(TEXT("the scratch tab is active"), ScratchView.IsValid() && Tab->GetActiveBuffer() == Scratch))
		{
			return false;
		}
		ScratchView->SetCaret(60, 2);
		ScratchView->ScrollToLine(60, 3);
		return true;
	});
	Steps.WaitFrames(5);
	Steps.Do(TEXT("PER-02: record the scratch tab, the order and the active tab"), [this, FileId, Values]()
	{
		const TSharedPtr<FDreamBuffer> Scratch = Buffers().FindById(Per2ScratchId);
		const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Per2ScratchId);
		if (!Scratch.IsValid() || !View.IsValid())
		{
			AddError(TEXT("the scratch tab is gone"));
			return false;
		}
		Values->Add(TEXT("per02_scratch_id"), Scratch->GetIdHex());
		Values->Add(TEXT("per02_scratch_path"), Scratch->GetPath());
		Values->Add(TEXT("per02_scratch_caret"), CaretOf(*View));
		Values->Add(TEXT("per02_scratch_first"), FString::FromInt(FirstShownLine(*View)));
		// The text the next session must find in the file: what the tab holds now with the edit typed at the caret right
		// before the exit.
		int32 CaretLine = 0;
		int32 CaretColumn = 0;
		View->GetCaret(CaretLine, CaretColumn);
		const dream::TextSnapshot Snapshot = Scratch->Snapshot();
		const int32 At = static_cast<int32>(Snapshot.LineStart(CaretLine)) + CaretColumn;
		const FString Now = Scratch->GetTextAsString();
		const FString Expected = Now.Left(At) + LastEdit + Now.Mid(At);
		TestTrue(TEXT("the expected text is kept for step 2"), FFileHelper::SaveStringToFile(Expected, *FPaths::Combine(Dir, TEXT("per02_scratch_expected.txt")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		TArray<FString> Order;
		for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
		{
			if (Buffer->GetId() == *FileId || Buffer->GetId() == Per2ScratchId)
			{
				Order.Add(Buffer->GetIdHex());
			}
		}
		TestEqual(TEXT("the file tab comes before the scratch tab"), Order.Num(), 2);
		Values->Add(TEXT("per02_order"), FString::Join(Order, TEXT(",")));
		Values->Add(TEXT("per02_active"), Tab->GetActiveBuffer().IsValid() ? Tab->GetActiveBuffer()->GetIdHex() : FString());
		Infos.Add(FString::Printf(TEXT("PER-02 tabs: file %s caret %s first %s; scratch %s caret %s first %s"), *Values->FindRef(TEXT("per02_file_path")),
			*Values->FindRef(TEXT("per02_file_caret")), *Values->FindRef(TEXT("per02_file_first")), *Scratch->GetPath(),
			*Values->FindRef(TEXT("per02_scratch_caret")), *Values->FindRef(TEXT("per02_scratch_first"))));
		return true;
	});
}

void FDreamRestartLayoutSpec::AddPer2Check(DreamTest::FDreamSteps& Steps, const TMap<FString, FString>& Previous)
{
	using namespace DreamRestartLayoutSpec;
	const FString FileHex = Previous.FindRef(TEXT("per02_file_id"));
	const FString ScratchHex = Previous.FindRef(TEXT("per02_scratch_id"));
	if (FileHex.IsEmpty() || ScratchHex.IsEmpty())
	{
		Steps.Do(TEXT("PER-02: step 1 recorded its tabs"), [this]() { AddError(TEXT("step 1 wrote no PER-02 tabs")); return false; });
		return;
	}
	auto Find = [](const FString& Hex) { return Buffers().FindByIdHex(Hex); };
	Steps.Poll(TEXT("PER-02: the session restore brings both tabs back"), [Find, FileHex, ScratchHex]()
	{
		return Find(FileHex).IsValid() && Find(ScratchHex).IsValid() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
	}, 30.0);
	Steps.Do(TEXT("PER-02: the scratch tab's file, the order and the active tab"), [this, Find, FileHex, ScratchHex, Previous]()
	{
		const TSharedPtr<FDreamBuffer> File = Find(FileHex);
		const TSharedPtr<FDreamBuffer> Scratch = Find(ScratchHex);
		const FString Expected = ReadText(FPaths::Combine(Dir, TEXT("per02_scratch_expected.txt")));
		TestEqual(TEXT("the file tab's path"), File->GetPath(), Previous.FindRef(TEXT("per02_file_path")));
		TestEqual(TEXT("the scratch tab's path"), Scratch->GetPath(), Previous.FindRef(TEXT("per02_scratch_path")));
		TestTrue(TEXT("it is a scratch file again"), Scratch->IsScratchFile());
		TestTrue(TEXT("with the last edit typed before the exit"), Scratch->GetTextAsString().Contains(LastEdit));
		TestEqual(TEXT("its whole text"), Scratch->GetTextAsString(), Expected);
		TestEqual(TEXT("the file holds it (the exit saved it)"), ReadText(Scratch->GetPath()), Expected);
		TestFalse(TEXT("so the tab is clean"), Scratch->IsDirty());
		TestFalse(TEXT("and nothing waits as a recovery"), Scratch->HasPendingRecovery());
		TArray<FString> Order;
		for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
		{
			if (Buffer->GetIdHex() == FileHex || Buffer->GetIdHex() == ScratchHex)
			{
				Order.Add(Buffer->GetIdHex());
			}
		}
		TestEqual(TEXT("the tab order"), FString::Join(Order, TEXT(",")), Previous.FindRef(TEXT("per02_order")));
		TestEqual(TEXT("the active tab"), Tab->GetActiveBuffer().IsValid() ? Tab->GetActiveBuffer()->GetIdHex() : FString(),
			Previous.FindRef(TEXT("per02_active")));
		return true;
	});
	// Each view's scroll comes back once it is laid out (SDreamTab::ScheduleRestoredScroll): the active one now, the file tab
	// when it is activated.
	struct FViewCheck
	{
		FString Hex;
		FString CaretKey;
		FString FirstKey;
		const TCHAR* What;
	};
	for (const FViewCheck& Check : { FViewCheck{ ScratchHex, TEXT("per02_scratch_caret"), TEXT("per02_scratch_first"), TEXT("scratch tab") },
		FViewCheck{ FileHex, TEXT("per02_file_caret"), TEXT("per02_file_first"), TEXT("file tab") } })
	{
		Steps.Do(FString::Printf(TEXT("PER-02: show the %s"), Check.What), [this, Find, Check]()
		{
			Tab->ActivateBuffer(Find(Check.Hex)->GetId());
			return true;
		});
		const int32 WantFirst = FCString::Atoi(*Previous.FindRef(Check.FirstKey));
		TSharedRef<double> Since = MakeShared<double>(0.0);
		// Up to 3 s for the scroll to land; the next step reports what it found either way.
		Steps.Poll(FString::Printf(TEXT("PER-02: the %s's scroll comes back"), Check.What), [this, Find, Check, WantFirst, Since]()
		{
			const double Now = FPlatformTime::Seconds();
			*Since = *Since == 0.0 ? Now : *Since;
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Find(Check.Hex)->GetId());
			const bool bThere = View.IsValid() && FirstShownLine(*View) == WantFirst;
			return bThere || Now - *Since > 3.0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(FString::Printf(TEXT("PER-02: the %s's caret and scroll"), Check.What), [this, Find, Check, Previous, WantFirst]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Find(Check.Hex)->GetId());
			if (!TestTrue(TEXT("its view"), View.IsValid()))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Buffer = Find(Check.Hex);
			Infos.Add(FString::Printf(TEXT("PER-02 %s restored view state: caret %d:%d, first visible line %d"), Check.What,
				Buffer->GetViewState().CaretLine, Buffer->GetViewState().CaretColumn, Buffer->GetViewState().FirstVisibleLine));
			// The scratch tab's caret at the exit (after the last edit moved it) when step 1 could record it; else as recorded.
			const FString ExitCaret = Previous.FindRef(Check.CaretKey + TEXT("_exit"));
			TestEqual(*FString::Printf(TEXT("the %s's caret"), Check.What), CaretOf(*View), ExitCaret.IsEmpty() ? Previous.FindRef(Check.CaretKey) : ExitCaret);
			TestEqual(*FString::Printf(TEXT("the %s's first visible line"), Check.What), FirstShownLine(*View), WantFirst);
			Infos.Add(FString::Printf(TEXT("PER-02 %s back: caret %s, first visible line %d"), Check.What, *CaretOf(*View), FirstShownLine(*View)));
			return true;
		});
	}
	Steps.Finally(TEXT("PER-02: close both tabs"), [Find, FileHex, ScratchHex]()
	{
		if (!FDreamEditorModule::IsAvailable())
		{
			return;
		}
		for (const FString& Hex : { FileHex, ScratchHex })
		{
			if (const TSharedPtr<FDreamBuffer> Buffer = Find(Hex))
			{
				DreamTest::CloseBuffer(Buffer->GetId());
			}
		}
	});
}

TSharedRef<DreamTest::FDreamInputBotTicket> FDreamRestartLayoutSpec::Drag(const DreamRestartLayoutSpec::FLayout& L, float TargetY)
{
	// The handle is the 2 px gap between the two areas; SSplitter hit-tests 6 px around it, and each area has an invisible
	// 2 px grip at the edge next to it (chrome R15). The press goes to the gap's middle.
	const float HandleY = L.bOutputOnTop ? (L.Output.Bottom + L.Code.Top) * 0.5f : (L.Code.Bottom + L.Output.Top) * 0.5f;
	const float X = L.Output.GetCenter().X;
	void* Hwnd = DreamTest::FDreamTestProbes::GetOsWindowHandle(FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef()));
	return DreamTest::FDreamInputBot::Submit([X, HandleY, TargetY, Hwnd](DreamTest::FDreamInputBotContext& Bot)
	{
		if (!Bot.RequireForeground() || !Bot.MoveToPoint(FVector2D(X, HandleY), Hwnd, TEXT("the splitter handle")))
		{
			return;
		}
		// SSplitter starts a resize on a press only over the handle its last OnMouseMove hovered: let Slate tick on the
		// moved cursor before the press, and say what the press will hit.
		const uint64 MovedAt = DreamTest::FDreamTestProbes::GetSnapshot().Serial;
		if (!Bot.WaitFor([MovedAt](const DreamTest::FDreamProbeSnapshot& S) { return S.Serial >= MovedAt + 2; }, 5.0, TEXT("Slate to tick on the moved cursor")))
		{
			return;
		}
		Bot.Note(FString::Printf(TEXT("under the cursor before the press at (%.0f, %.0f): %s"), X, HandleY,
			*DreamTest::FDreamTestProbes::GetSnapshot().WidgetUnderCursor));
		if (Bot.LeftDown() <= 0.0)
		{
			return;
		}
		// Small moves, each after Slate has ticked, so the splitter follows the drag as it does for a hand.
		for (int32 Index = 1; Index <= 10 && !Bot.HasFailed(); ++Index)
		{
			const uint64 Serial = DreamTest::FDreamTestProbes::GetSnapshot().Serial;
			const float Y = HandleY + (TargetY - HandleY) * Index / 10.f;
			Bot.MoveToPoint(FVector2D(X, Y), nullptr, TEXT("the drag"));
			Bot.WaitFor([Serial](const DreamTest::FDreamProbeSnapshot& S) { return S.Serial >= Serial + 2; }, 5.0, TEXT("Slate to tick"));
		}
		Bot.LeftUp();
	});
}

void FDreamRestartLayoutSpec::Define()
{
	LatentIt("T-GAP-019: the output area's position and height survive restarts, at exactly 40 % on a fresh profile (driver step)",
		FTimespan::FromSeconds(120.0), [this](const FDoneDelegate& Done)
	{
		Step = DreamTest::CommandLineInt(TEXT("DreamRestartStep"), 0);
		Dir = DreamTest::CommandLineString(TEXT("DreamRestartDir"));
		if (Step <= 0 || Dir.IsEmpty())
		{
			// Not run here: a driver-only spec must not read as passed in the shared pass (a warning, not an info).
			AddWarning(TEXT("NOT RUN (T-GAP-019 is driver-only): run powershell -ExecutionPolicy Bypass -File DreamHost\\Scripts\\run_restart.ps1 (it restarts the editor between steps)"));
			Done.Execute();
			return;
		}
		// Steps 1 and 2 persist OutputPosition on purpose for the next session; run_restart.ps1 backs the key up and puts
		// it back (A176), so the tests' settings guard leaves it alone in this test (review r7 M1).
		DreamTest::AllowSettingChange(*this, GET_MEMBER_NAME_CHECKED(UDreamUserSettings, OutputPosition));
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			Finish({});
			Done.Execute();
			return;
		}
		const TMap<FString, FString> Previous = DreamRestartLayoutSpec::ReadStep(Dir, Step - 1);
		TSharedRef<TMap<FString, FString>> Values = MakeShared<TMap<FString, FString>>();
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();
		TSharedRef<float> StartHeight = MakeShared<float>(0.f);
		DreamTest::FDreamSteps Steps(*this);
		Steps.WaitFrames(5);
		Steps.Do(TEXT("check what the previous session left"), [this, Previous, Values, StartHeight]()
		{
			const DreamRestartLayoutSpec::FLayout L = DreamRestartLayoutSpec::Measure(*Tab);
			if (!TestTrue(TEXT("the centre column is laid out"), L.bValid))
			{
				return false;
			}
			*StartHeight = L.OutputHeight;
			Infos.Add(FString::Printf(TEXT("step %d starts with %s"), Step, *DreamRestartLayoutSpec::Describe(L)));
			const float Total = L.OutputHeight + L.CodeHeight;
			const float Unit = 1.f / FMath::Max(1.f, L.Scale);
			if (Step == 1)
			{
				TestTrue(TEXT("the fresh profile has the output area above the code area (PO-01)"), L.bOutputOnTop);
				TestTrue(*FString::Printf(TEXT("at 40 %% of the centre column: %.4f (WS-02)"), L.OutputHeight / Total),
					FMath::Abs(L.OutputHeight - 0.4f * Total) <= 1.f + Unit);
				// The REPL prompt is hidden at the defaults since PO-04 (View > REPL Prompt); its place is checked when shown.
				if (UDreamUserSettings::Get()->bShowReplPrompt)
				{
					TestTrue(TEXT("the prompt strip is at the bottom of the output area, below its body"),
						L.Strip.GetSize().Y > 0.f && FMath::Abs(L.Strip.Bottom - L.Output.Bottom) <= 2.f && L.Strip.Top > L.Output.Top);
				}
				else
				{
					Infos.Add(TEXT("the REPL prompt is hidden (the PO-04 default), so the prompt strip's place is not checked"));
				}
				TestTrue(TEXT("the session state holds the 40 % default"), FMath::IsNearlyEqual(UDreamPerProjectUserSettings::Get()->OutputAreaFraction, 0.4f, 0.0001f));
			}
			else
			{
				const bool bWantTop = Previous.FindRef(TEXT("position")) == TEXT("top");
				const float WantHeight = FCString::Atof(*Previous.FindRef(TEXT("output_height")));
				TestTrue(TEXT("the previous session's step passed"), Previous.FindRef(TEXT("ok")) == TEXT("1"));
				TestEqual(TEXT("the output position survived the restart (WS-03)"), L.bOutputOnTop, bWantTop);
				TestTrue(*FString::Printf(TEXT("the output height survived the restart: %.1f, was %.1f (WS-02)"), L.OutputHeight, WantHeight),
					FMath::Abs(L.OutputHeight - WantHeight) <= 2.f);
			}
			TestTrue(TEXT("the output area is at least 120 px"), L.OutputHeight >= 120.f - Unit);
			TestTrue(TEXT("the code area is at least 120 px"), L.CodeHeight >= 120.f - Unit);
			return true;
		});
		Steps.Do(TEXT("change the layout"), [this, Ticket]()
		{
			const DreamRestartLayoutSpec::FLayout L = DreamRestartLayoutSpec::Measure(*Tab);
			switch (Step)
			{
			case 1: Tab->ExecuteDreamCommand(EDreamCommand::MoveOutputToBottom); break;
			case 2: Tab->ExecuteDreamCommand(EDreamCommand::MoveOutputToTop); break;
			case 3:
				if (DreamTest::RequireEditorInFront(*this))
				{
					*Ticket = Drag(L, L.Output.Top + 10.f);
				}
				break;
			case 4:
				if (DreamTest::RequireEditorInFront(*this))
				{
					*Ticket = Drag(L, L.Code.Bottom - 10.f);
				}
				break;
			default: break;
			}
			return true;
		});
		Steps.Poll(TEXT("the drag"), [Ticket]()
		{
			return !Ticket->IsValid() || DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 30.0);
		Steps.WaitFrames(4);
		Steps.Do(TEXT("check the change"), [this, Values, Ticket, StartHeight]()
		{
			if (Ticket->IsValid())
			{
				DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), TEXT("splitter drag"));
				for (const FString& Note : (*Ticket)->Notes)
				{
					Infos.Add(Note);
					AddInfo(Note);
				}
			}
			const DreamRestartLayoutSpec::FLayout L = DreamRestartLayoutSpec::Measure(*Tab);
			const float Unit = 1.f / FMath::Max(1.f, L.Scale);
			Infos.Add(FString::Printf(TEXT("step %d leaves %s"), Step, *DreamRestartLayoutSpec::Describe(L)));
			switch (Step)
			{
			case 1:
				TestFalse(TEXT("Bottom moved the output area below the code area at once"), L.bOutputOnTop);
				TestTrue(*FString::Printf(TEXT("with the same height: %.1f, was %.1f (WS-03)"), L.OutputHeight, *StartHeight), FMath::Abs(L.OutputHeight - *StartHeight) <= 2.f);
				break;
			case 2:
				TestTrue(TEXT("Move Output to Top moved it back"), L.bOutputOnTop);
				TestTrue(*FString::Printf(TEXT("with the same height: %.1f, was %.1f (WS-03)"), L.OutputHeight, *StartHeight), FMath::Abs(L.OutputHeight - *StartHeight) <= 2.f);
				break;
			case 3:
				TestTrue(*FString::Printf(TEXT("the drag to the top end stops the output area at 120 px (%.1f)"), L.OutputHeight),
					FMath::Abs(L.OutputHeight - 120.f) <= 1.f + Unit);
				break;
			case 4:
				TestTrue(*FString::Printf(TEXT("the drag to the bottom end stops the code area at 120 px (%.1f)"), L.CodeHeight),
					FMath::Abs(L.CodeHeight - 120.f) <= 1.f + Unit);
				break;
			default:
				break;
			}
			Values->Add(TEXT("position"), L.bOutputOnTop ? TEXT("top") : TEXT("bottom"));
			Values->Add(TEXT("output_height"), FString::SanitizeFloat(L.OutputHeight));
			Values->Add(TEXT("code_height"), FString::SanitizeFloat(L.CodeHeight));
			return true;
		});
		// (scratch, PO-05, PER-02) The open tabs come back across the restart (steps 1 and 2).
		if (Step == 1)
		{
			AddPer2Setup(Steps, Values);
		}
		else if (Step == 2)
		{
			AddPer2Check(Steps, Previous);
		}
		Steps.Finally(TEXT("write the result and exit normally"), [this, Values]()
		{
			Finish(*Values);
		});
		Steps.Run(Done);
	});
}
