// SPDX-License-Identifier: Apache-2.0
// Dream.Output.Follow: follow mode of the docked Output view under a stream of lines (TDD §6.4, OUT-09, A11Y-01,
// T-OUT-030) and the Show toggles (OUT-03).
//   1. While lines keep arriving, a wheel scroll up over the list and Home with the list focused each leave follow mode:
//      the next lines wait below with the "n new lines" chip instead of pulling the view back down. Both are Slate input:
//      the wheel routed along the list's widget path (RouteMouseWheelOrGestureEvent, as ProcessMouseWheelOrGestureEvent
//      routes it after its hit test under the real OS cursor), Home through ProcessKeyDownEvent; Home scrolls at the list's
//      next tick, outside input processing (P2-out-01). The lines are Dream lines outside every run block (run 0).
//   2. Down from a selected line the stream scrolled above the view (End selected it, then more lines came) leaves follow
//      mode too: the list scrolls up to the next line at its tick (P3-out-03). Down from a selected line still on screen
//      keeps it, also in the frame right after a drain appended lines, before the list's tick clamps the offset that
//      drain's scroll to the bottom set to the row count (P4-shl-01).
//   3. Turning Info off hides a run's info line (unreal.log) and keeps its stdout and warning lines (OUT-03).
//   4. The WS-07 error click (ScrollToRunError) selects the run's traceback, not an earlier unreal.log_error line, and the
//      first line of the last error block when the error has no "Traceback" header (A2-02, T-UEI-002 subset).
#include "Support/DreamTestLogCapture.h"
#include "Support/DreamTestUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamOutputStore.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"

namespace DreamOutputFollowSpec
{
	/** The first widget under Root that takes keyboard focus: the Output view's list (the view itself does not). */
	TSharedPtr<SWidget> FindFocusable(const TSharedRef<SWidget>& Root)
	{
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (Child->SupportsKeyboardFocus())
			{
				return Child;
			}
			if (const TSharedPtr<SWidget> Found = FindFocusable(Child))
			{
				return Found;
			}
		}
		return nullptr;
	}

	void PressKey(const FKey& Key)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState NoMods;
		App.ProcessKeyDownEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
	}

	void PushLines(const FString& Tag, int32 From, int32 Count)
	{
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		for (int32 Index = From; Index < From + Count; ++Index)
		{
			Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("%s line %03d"), *Tag, Index));
		}
	}

	FVector2D CentreOf(const TSharedRef<SWidget>& Widget)
	{
		return FVector2D(Widget->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
	}
}

BEGIN_DEFINE_SPEC(FDreamOutputFollowSpec, "Dream.Output.Follow", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	/** The docked Output view, shown; null with an error otherwise. */
	TSharedPtr<SDreamOutputView> RequireOutputView();
END_DEFINE_SPEC(FDreamOutputFollowSpec)

TSharedPtr<SDreamOutputView> FDreamOutputFollowSpec::RequireOutputView()
{
	const TSharedPtr<SDreamOutputView> View = Tab.IsValid() && Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetOutputView() : nullptr;
	if (!View.IsValid())
	{
		AddError(TEXT("the Dream tab has no Output view"));
	}
	return View;
}

void FDreamOutputFollowSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
	});

	AfterEach([this]()
	{
		Tab.Reset();
	});

	LatentIt("leaves follow mode on a wheel scroll and on Home while lines stream in (T-OUT-030, P2-out-01)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString Tag = FString::Printf(TEXT("follow_%s"), *DreamTest::UniqueSuffix());
		TSharedRef<int32> Next = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("show the Output view, following, with more lines than fit"), [this, Tag, Next]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			DreamOutputFollowSpec::PushLines(Tag, *Next, 300);
			*Next += 300;
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("wheel up over the list"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("the view follows the stream before any scroll"), View->IsFollowing()))
			{
				return false;
			}
			FSlateApplication& App = FSlateApplication::Get();
			const TSharedPtr<SWidget> List = DreamOutputFollowSpec::FindFocusable(View.ToSharedRef());
			FWidgetPath Path;
			if (!TestTrue(TEXT("the Output view's list is in a visible window"), List.IsValid() && DreamTest::MakePointerPath(List.ToSharedRef(), Path)))
			{
				return false;
			}
			const FVector2D At = DreamOutputFollowSpec::CentreOf(List.ToSharedRef());
			const FPointerEvent Wheel(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At, TSet<FKey>(), EKeys::Invalid,
				/*WheelDelta*/ 3.f, FModifierKeysState());
			App.RouteMouseWheelOrGestureEvent(Path, Wheel, nullptr);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("more lines arrive after the wheel"), [Tag, Next]()
		{
			DreamOutputFollowSpec::PushLines(Tag, *Next, 20);
			*Next += 20;
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view held after the wheel"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestFalse(TEXT("a wheel scroll up leaves follow mode (OUT-09)"), View->IsFollowing());
			TestTrue(*FString::Printf(TEXT("the new lines wait below (%d)"), View->GetNewLinesBelow()), View->GetNewLinesBelow() > 0);
			View->ScrollToEnd();                // the chip's action
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("Home with the list focused"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFollowSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the Output view has a focusable list"), List.IsValid())
				|| !TestTrue(TEXT("the view follows again after Scroll to end"), View->IsFollowing()))
			{
				return false;
			}
			FSlateApplication& App = FSlateApplication::Get();
			App.SetKeyboardFocus(List, EFocusCause::SetDirectly);
			if (!TestTrue(TEXT("the list has keyboard focus"), List->HasKeyboardFocus()))
			{
				return false;
			}
			const FModifierKeysState NoMods;
			App.ProcessKeyDownEvent(FKeyEvent(EKeys::Home, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
			App.ProcessKeyUpEvent(FKeyEvent(EKeys::Home, NoMods, App.GetUserIndexForKeyboard(), false, 0, 0));
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("more lines arrive after Home"), [Tag, Next]()
		{
			DreamOutputFollowSpec::PushLines(Tag, *Next, 20);
			*Next += 20;
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view held after Home"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestFalse(TEXT("Home toward older lines leaves follow mode (A11Y-01, P2-out-01)"), View->IsFollowing());
			TestTrue(*FString::Printf(TEXT("the new lines wait below (%d)"), View->GetNewLinesBelow()), View->GetNewLinesBelow() > 0);
			return true;
		});
		Steps.Finally(TEXT("follow again"), [this]()
		{
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("leaves follow mode on Down from a selected line the stream scrolled above the view (P3-out-03)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString Tag = FString::Printf(TEXT("down_%s"), *DreamTest::UniqueSuffix());
		TSharedRef<int32> Next = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("show the Output view, following, with more lines than fit"), [this, Tag, Next]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			DreamOutputFollowSpec::PushLines(Tag, *Next, 300);
			*Next += 300;
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("End with the list focused selects the last line and keeps following"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFollowSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the Output view has a focusable list"), List.IsValid()))
			{
				return false;
			}
			FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly);
			if (!TestTrue(TEXT("the list has keyboard focus"), List->HasKeyboardFocus()))
			{
				return false;
			}
			DreamOutputFollowSpec::PressKey(EKeys::End);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the stream scrolls the selected line above the view"), [this, Tag, Next]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("End keeps follow mode"), View->IsFollowing())
				|| !TestTrue(TEXT("End selected a line"), !View->GetSelectedText().IsEmpty()))
			{
				return false;
			}
			DreamOutputFollowSpec::PushLines(Tag, *Next, 200);
			*Next += 200;
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("Down with the list focused"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFollowSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the view still follows the stream"), View->IsFollowing())
				|| !TestTrue(TEXT("the list still has keyboard focus"), List.IsValid() && List->HasKeyboardFocus()))
			{
				return false;
			}
			DreamOutputFollowSpec::PressKey(EKeys::Down);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("more lines arrive after Down"), [Tag, Next]()
		{
			DreamOutputFollowSpec::PushLines(Tag, *Next, 20);
			*Next += 20;
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view held after Down"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestFalse(TEXT("Down to a line above the view leaves follow mode (A11Y-01, P3-out-03)"), View->IsFollowing());
			TestTrue(*FString::Printf(TEXT("the new lines wait below (%d)"), View->GetNewLinesBelow()), View->GetNewLinesBelow() > 0);
			return true;
		});
		Steps.Finally(TEXT("follow again"), [this]()
		{
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("keeps follow mode on Down to a shown line in the frame after a drain appended lines (P4-shl-01)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString Tag = FString::Printf(TEXT("shown_%s"), *DreamTest::UniqueSuffix());
		TSharedRef<int32> Next = MakeShared<int32>(0);
		TSharedRef<uint64> EndSeqWanted = MakeShared<uint64>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("show the Output view, following, with more lines than fit"), [this, Tag, Next]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			DreamOutputFollowSpec::PushLines(Tag, *Next, 300);
			*Next += 300;
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("End with the list focused selects the last line and keeps following"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFollowSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the Output view has a focusable list"), List.IsValid()))
			{
				return false;
			}
			FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly);
			if (!TestTrue(TEXT("the list has keyboard focus"), List->HasKeyboardFocus()))
			{
				return false;
			}
			DreamOutputFollowSpec::PressKey(EKeys::End);
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("two more lines, so the selected line stays on screen"), [this, Tag, Next, EndSeqWanted]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("End keeps follow mode"), View->IsFollowing())
				|| !TestTrue(TEXT("End selected a line"), !View->GetSelectedText().IsEmpty()))
			{
				return false;
			}
			*EndSeqWanted = FDreamEditorModule::Get().GetOutputStore().GetEndSeq() + 2;
			DreamOutputFollowSpec::PushLines(Tag, *Next, 2);
			*Next += 2;
			return true;
		});
		// The steps poll after the store's drain ticker in the same core-ticker pass, so the list has not ticked since the
		// drain appended the lines and scrolled to the bottom: its desired offset is still the row count.
		Steps.Poll(TEXT("Down with the list focused, right after the drain"), [this, EndSeqWanted]()
		{
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			Store.Drain(0.005);
			if (Store.GetEndSeq() < *EndSeqWanted)
			{
				return DreamTest::EStep::Wait;
			}
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFollowSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the view still follows the stream"), View->IsFollowing())
				|| !TestTrue(TEXT("the list still has keyboard focus"), List.IsValid() && List->HasKeyboardFocus())
				|| !TestTrue(TEXT("the list has not ticked since the drain (the case under test)"),
					StaticCastSharedPtr<STableViewBase>(List)->IsPendingRefresh()))
			{
				return DreamTest::EStep::Fail;
			}
			DreamOutputFollowSpec::PressKey(EKeys::Down);
			return DreamTest::EStep::Next;
		});
		// The stream goes on, a drain every frame: a view that left follow mode is never found settled at the bottom again
		// (TickFollow), so it would hold with the new-lines chip.
		TSharedRef<int32> Streamed = MakeShared<int32>(0);
		Steps.Poll(TEXT("one more line every frame after Down"), [Tag, Next, Streamed]()
		{
			DreamOutputFollowSpec::PushLines(Tag, *Next, 1);
			++*Next;
			return ++*Streamed < 20 ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view still follows after Down"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			TestTrue(TEXT("Down to a line on screen keeps follow mode (OUT-09, P4-shl-01)"), View->IsFollowing());
			TestEqual(TEXT("no new lines wait below"), View->GetNewLinesBelow(), 0);
			return true;
		});
		Steps.Finally(TEXT("follow again"), [this]()
		{
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("selects the run's traceback on the error click, not an earlier log_error line (A2-02, T-UEI-002)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Early = FString::Printf(TEXT("early_%s"), *Sfx);
		const FString Incomplete = FString::Printf(TEXT("incomplete_%s = (1,"), *Sfx);
		// Both runs reach LogPython at Error (§6.3): the log_error line, the ZeroDivisionError traceback, the SyntaxError block.
		AddExpectedErrorPlain(Early, EAutomationExpectedErrorFlags::Contains, 0);
		DreamTest::ExpectPythonErrorBlock(*this, FString(), TEXT("ZeroDivisionError"), { TEXT("<dream:REPL#"), TEXT("1/0") });
		DreamTest::ExpectPythonErrorBlock(*this, FString(), TEXT("was never closed"), { Incomplete });
		TSharedRef<DreamTest::FDreamTestLogCapture> Log = MakeShared<DreamTest::FDreamTestLogCapture>();
		TSharedRef<DreamTest::FRunResult> Failing = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> Broken = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("an error after a log_error line and many prints"), [Early]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(
				TEXT("import unreal\nunreal.log_error('%s')\nfor _i in range(80):\n    print(_i)\n1/0"), *Early));
		}, Failing);
		Steps.Do(TEXT("the error click selects the traceback"), [this, Failing, Early]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Failing, EDreamRunOutcome::Error))
			{
				return false;
			}
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("ScrollToRunError finds the run's error"), View->ScrollToRunError(Failing->RunNumber)))
			{
				return false;
			}
			const FString Selected = View->GetSelectedText();
			TestTrue(*FString::Printf(TEXT("the traceback's header line is selected: \"%s\""), *Selected),
				Selected.StartsWith(TEXT("Traceback (most recent call last):")));
			TestFalse(TEXT("not the earlier log_error line (A2-02)"), Selected.Contains(Early));
			return true;
		});
		DreamTest::AddRun(Steps, TEXT("an error with no Traceback header"), [Incomplete]()
		{
			return DreamTest::MakeReplRequest(Incomplete);
		}, Broken);
		Steps.Do(TEXT("the error click selects the first line of the last error block"), [this, Broken]()
		{
			DreamTest::ExpectOutcome(*this, *Broken, EDreamRunOutcome::NotExecuted);
			FString FirstError;
			bool bAnyTraceback = false;
			for (const DreamTest::FOutputLine& Line : Broken->Lines)
			{
				if (Line.Type == dream::LineType::Error)
				{
					FirstError = FirstError.IsEmpty() ? Line.Text : FirstError;
					bAnyTraceback = bAnyTraceback || Line.Text.StartsWith(TEXT("Traceback"));
				}
			}
			if (!TestFalse(*FString::Printf(TEXT("the error block has no Traceback header: %s"), *Broken->Describe()), FirstError.IsEmpty() || bAnyTraceback))
			{
				return false;
			}
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid() || !TestTrue(TEXT("ScrollToRunError finds the run's error"), View->ScrollToRunError(Broken->RunNumber)))
			{
				return false;
			}
			TestEqual(TEXT("the block's first line is selected"), View->GetSelectedText().TrimEnd(), FirstError.TrimEnd());
			return true;
		});
		DreamTest::AddWaitForForwardedError(Steps, Log, TEXT("was never closed"));
		Steps.Finally(TEXT("follow again and release the log capture"), [this, Log]()
		{
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("hides only info lines when Info is turned off (OUT-03)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		// unreal.log_warning reaches LogPython at Warning; it is the line under test, not a failure.
		AddExpectedMessagePlain(FString::Printf(TEXT("warning_%s"), *Sfx), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<FDreamOutputVisibility> Saved = MakeShared<FDreamOutputVisibility>();
		TSharedRef<bool> bVisibilitySaved = MakeShared<bool>(false);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("stdout, info and warning lines"), [Sfx]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(
				TEXT("import unreal\nprint('stdout_%s')\nunreal.log('info_%s')\nunreal.log_warning('warning_%s')"), *Sfx, *Sfx, *Sfx));
		}, Run);
		Steps.Do(TEXT("turn Info off"), [this, Run, Saved, bVisibilitySaved]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok))
			{
				return false;
			}
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			*Saved = Store.GetVisibility();
			*bVisibilitySaved = true;
			FDreamOutputVisibility Hidden = *Saved;
			Hidden.bInfo = false;               // the Show menu's Info entry (SDreamOutputView::AddShowMenuEntries)
			Hidden.bStdout = Hidden.bWarnings = true;
			Store.SetVisibility(Hidden);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("check the rows"), [this, Run, Sfx]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			auto SeqOf = [&Run](dream::LineType Type, const FString& Needle) -> uint64
			{
				for (const DreamTest::FOutputLine& Line : Run->Lines)
				{
					if (Line.Type == Type && Line.Text.Contains(Needle))
					{
						return Line.Seq;
					}
				}
				return 0;
			};
			const uint64 StdoutSeq = SeqOf(dream::LineType::Stdout, TEXT("stdout_") + Sfx);
			const uint64 InfoSeq = SeqOf(dream::LineType::Info, TEXT("info_") + Sfx);
			const uint64 WarningSeq = SeqOf(dream::LineType::Warning, TEXT("warning_") + Sfx);
			if (!TestTrue(*FString::Printf(TEXT("the run has a stdout, an info and a warning line: %s"), *Run->Describe()),
				StdoutSeq != 0 && InfoSeq != 0 && WarningSeq != 0))
			{
				return false;
			}
			const TArray<uint64> Shown = View->GetLineSeqsForTest(Run->RunNumber);
			TestFalse(TEXT("the info line is hidden"), Shown.Contains(InfoSeq));
			TestTrue(TEXT("the stdout line stays"), Shown.Contains(StdoutSeq));
			TestTrue(TEXT("the warning line stays"), Shown.Contains(WarningSeq));
			return true;
		});
		Steps.Finally(TEXT("restore the visibility"), [Saved, bVisibilitySaved]()
		{
			if (*bVisibilitySaved && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().SetVisibility(*Saved);
			}
		});
		Steps.Run(Done);
	});
}
