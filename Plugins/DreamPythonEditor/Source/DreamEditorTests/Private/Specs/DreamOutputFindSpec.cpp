// SPDX-License-Identifier: Apache-2.0
// Dream.Output.Find: Find (Ctrl+F) and the soft-wrap toggle of the docked Output view (TDD §6.4 "Design: widget", slice B
// item B92). The search is dream::SearchEngine over a ring snapshot on a worker (§6.2: snapshots read lock-free).
//   1. Ctrl+F with the list focused opens the find bar and moves focus into it; a literal query counts the matches the
//      view shows (stdout and info lines, run 0); F3-style steps select each match in order and wrap; hiding Info drops
//      the info line's match from the count and showing it brings it back; Match case and a regex narrow the matches; a
//      bad pattern reports its error; while nothing new arrives no search pass starts and the rows are not made again
//      (review r1); lines pushed while the bar is open are searched by the next pass; after Clear output the bar finds
//      nothing and starts no pass at every tick; Esc in the box closes the bar, drops the matches and gives focus back to
//      the rows.
//   2. A match inside a statement's folded echo (OUT-12 Normal folding) is revealed: the step opens the fold and selects
//      the echo line.
//   3. With soft wrap off a long line's row is one line high; turning it on regenerates the rows and wraps it over several
//      lines, while a short line keeps its height.
#include "Support/DreamTestUtil.h"

#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamOutputStore.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamTab.h"

namespace DreamOutputFindSpec
{
	/** The first widget under Root that takes keyboard focus: the Output view's list (the bar is collapsed until Ctrl+F). */
	TSharedPtr<SWidget> FindFocusable(const TSharedRef<SWidget>& Root)
	{
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (!Child->GetVisibility().IsVisible())
			{
				continue;           // the collapsed find bar and what is inside it
			}
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

	void PressKey(const FKey& Key, bool bCtrl = false)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods(false, false, bCtrl, false, false, false, false, false, false);
		App.ProcessKeyDownEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, 0, 0));
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

	struct FState
	{
		FString Sfx;
		FString Needle;
		TArray<uint64> Steps;
		FDreamOutputVisibility SavedVisibility;
		bool bVisibilitySaved = false;
		bool bSavedWrap = false;
		uint64 ShortSeq = 0;
		uint64 LongSeq = 0;
		float ShortHeight = 0.f;
		float LongHeight = 0.f;
		int32 Passes = 0;
		int32 Rebuilds = 0;
		uint64 EndSeq = 0;
	};
}

BEGIN_DEFINE_SPEC(FDreamOutputFindSpec, "Dream.Output.Find", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	FDreamOutputVisibility SavedVisibility;
	/** The docked Output view, shown; null with an error otherwise. */
	TSharedPtr<SDreamOutputView> RequireOutputView();
	/** Waits until the view's search passes have caught up with the ring. */
	void AddWaitSearched(DreamTest::FDreamSteps& Steps, const TCHAR* Name);
END_DEFINE_SPEC(FDreamOutputFindSpec)

TSharedPtr<SDreamOutputView> FDreamOutputFindSpec::RequireOutputView()
{
	const TSharedPtr<SDreamOutputView> View = Tab.IsValid() && Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetOutputView() : nullptr;
	if (!View.IsValid())
	{
		AddError(TEXT("the Dream tab has no Output view"));
	}
	return View;
}

void FDreamOutputFindSpec::AddWaitSearched(DreamTest::FDreamSteps& Steps, const TCHAR* Name)
{
	Steps.Poll(Name, [this]()
	{
		FDreamEditorModule::Get().GetOutputStore().Drain(0.005);
		const TSharedPtr<SDreamOutputView> View = RequireOutputView();
		if (!View.IsValid())
		{
			return DreamTest::EStep::Fail;
		}
		return View->IsFindSearching() ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
	}, 10.0);
}

void FDreamOutputFindSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		// (output) Every line type shown, Echo input included (the reveal case finds an echo line; a developer's Echo input
		// may be off), through the store as the Show menu sets it; bEchoInput is not written. Restored in AfterEach.
		FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
		SavedVisibility = Store.GetVisibility();
		FDreamOutputVisibility All = SavedVisibility;
		All.bStdout = All.bStderr = All.bResults = All.bInfo = All.bWarnings = All.bErrors = All.bEcho = All.bNotice = true;
		Store.SetVisibility(All);
	});

	AfterEach([this]()
	{
		if (FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetOutputStore().SetVisibility(SavedVisibility);
		}
		Tab.Reset();
	});

	LatentIt("finds, steps, filters and closes with Ctrl+F, F3 and Esc (B92)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputFindSpec;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		State->Needle = FString::Printf(TEXT("needle_%s"), *State->Sfx);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("show the Output view with three stdout matches and one info match between filler lines"), [this, State]()
		{
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			State->SavedVisibility = Store.GetVisibility();
			State->bVisibilitySaved = true;
			Store.SetVisibility(FDreamOutputVisibility());      // every type shown
			for (int32 Index = 0; Index < 40; ++Index)
			{
				Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("filler_%s %02d"), *State->Sfx, Index));
			}
			const TCHAR* Letters[] = { TEXT("A"), TEXT("B"), TEXT("C") };
			for (const TCHAR* Letter : Letters)
			{
				Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("%s %s"), *State->Needle, Letter));
				Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("filler_%s after %s"), *State->Sfx, Letter));
			}
			Store.PushDreamLine(0, EDreamStream::Info, FString::Printf(TEXT("%s D"), *State->Needle));
			return true;
		});
		Steps.WaitFrames(5);
		Steps.Do(TEXT("Ctrl+F with the list focused"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> List = View.IsValid() ? DreamOutputFindSpec::FindFocusable(View.ToSharedRef()) : nullptr;
			if (!View.IsValid() || !TestTrue(TEXT("the Output view has a focusable list"), List.IsValid()))
			{
				return false;
			}
			FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly);
			if (!TestTrue(TEXT("the list has keyboard focus"), List->HasKeyboardFocus()))
			{
				return false;
			}
			DreamOutputFindSpec::PressKey(EKeys::F, /*bCtrl*/ true);
			return TestTrue(TEXT("Ctrl+F opens the find bar"), View->IsFindOpen());
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("focus is in the find box"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
			TestTrue(*FString::Printf(TEXT("focus moved from the rows into the find box (%s)"), Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none")),
				Focused.IsValid() && Focused->GetType() == FName(TEXT("SEditableText")));
			return true;
		});
		Steps.Do(TEXT("search for the needle"), [this, State]()
		{
			RequireOutputView()->SetFindQuery(State->Needle, false, false, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the search pass ends"));
		Steps.Do(TEXT("count and step through the matches"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!TestEqual(*FString::Printf(TEXT("four matches (%s)"), *View->GetFindStatus().ToString()), View->GetFindMatchCount(), 4))
			{
				return false;
			}
			TestEqual(TEXT("the bar's count"), View->GetFindStatus().ToString(), FString(TEXT("4 results")));
			for (int32 Step = 0; Step < 5; ++Step)
			{
				if (!TestTrue(TEXT("a step finds a match"), View->FindStep(1)))
				{
					return false;
				}
				State->Steps.Add(View->GetCurrentFindSeq());
				TestTrue(*FString::Printf(TEXT("the current match's line is selected: \"%s\""), *View->GetSelectedText()),
					View->GetSelectedText().Contains(State->Needle));
			}
			const TSet<uint64> FirstFour(TArray<uint64>(State->Steps.GetData(), 4));
			TestEqual(TEXT("four steps visit four different matches"), FirstFour.Num(), 4);
			TestFalse(TEXT("every step has a current match"), FirstFour.Contains(0));
			TestEqual(TEXT("the fifth step wraps to the first match visited"), State->Steps[4], State->Steps[0]);
			View->FindStep(-1);
			TestEqual(TEXT("a step back returns to the fourth match visited"), View->GetCurrentFindSeq(), State->Steps[3]);
			TestTrue(*FString::Printf(TEXT("\"k of 4\" while a match is current (%s)"), *View->GetFindStatus().ToString()),
				View->GetFindStatus().ToString().EndsWith(TEXT(" of 4")));
			return true;
		});
		Steps.Do(TEXT("hide Info, then show it again"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			FDreamOutputVisibility NoInfo;
			NoInfo.bInfo = false;
			Store.SetVisibility(NoInfo);
			TestEqual(TEXT("with Info hidden the info line's match leaves the count (§6.4 filters)"), View->GetFindMatchCount(), 3);
			FDreamOutputVisibility WithInfo = NoInfo;
			WithInfo.bInfo = true;
			Store.SetVisibility(WithInfo);
			TestEqual(TEXT("with Info shown again it is back"), View->GetFindMatchCount(), 4);
			return true;
		});
		Steps.Do(TEXT("Match case with the upper-case needle"), [this, State]()
		{
			RequireOutputView()->SetFindQuery(State->Needle.ToUpper(), /*bMatchCase*/ true, false, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the case-sensitive pass ends"));
		Steps.Do(TEXT("check, then search case-insensitively"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			TestEqual(TEXT("Match case finds no upper-case needle"), View->GetFindMatchCount(), 0);
			TestEqual(TEXT("the bar says so"), View->GetFindStatus().ToString(), FString(TEXT("No results")));
			View->SetFindQuery(State->Needle.ToUpper(), false, false, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the case-insensitive pass ends"));
		Steps.Do(TEXT("check, then a regex"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			TestEqual(TEXT("without Match case the upper-case needle finds all four"), View->GetFindMatchCount(), 4);
			View->SetFindQuery(State->Needle + TEXT(" [AB]$"), false, /*bRegex*/ true, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the regex pass ends"));
		Steps.Do(TEXT("check, then a bad pattern"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			TestEqual(TEXT("the regex (Python re syntax) finds A and B"), View->GetFindMatchCount(), 2);
			View->SetFindQuery(State->Needle + TEXT("_("), false, true, false);
			TestEqual(TEXT("a bad pattern finds nothing"), View->GetFindMatchCount(), 0);
			TestFalse(TEXT("a bad pattern reports its error"), View->GetFindStatus().IsEmpty());
			TestFalse(TEXT("... and runs no search"), View->IsFindSearching());
			View->SetFindQuery(State->Needle, false, false, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the literal pass ends again"));
		auto Mark = [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			State->Passes = View->GetFindPassesForTest();
			State->Rebuilds = View->GetRowRebuildsForTest();
			State->EndSeq = FDreamEditorModule::Get().GetOutputStore().GetEndSeq();
			return true;
		};
		auto CheckQuiet = [this, State](const TCHAR* When)
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const uint64 EndSeq = FDreamEditorModule::Get().GetOutputStore().GetEndSeq();
			TestEqual(*FString::Printf(TEXT("%s: no new hit, so the rows were not made again"), When), View->GetRowRebuildsForTest(), State->Rebuilds);
			if (EndSeq == State->EndSeq)
			{
				TestEqual(*FString::Printf(TEXT("%s: no new line, so no search pass started"), When), View->GetFindPassesForTest(), State->Passes);
			}
			else
			{
				AddInfo(FString::Printf(TEXT("%s: %llu other lines arrived; the pass count is not checked"), When, EndSeq - State->EndSeq));
			}
			return true;
		};
		Steps.Do(TEXT("count the passes and row rebuilds"), Mark);
		Steps.WaitFrames(10);
		Steps.Do(TEXT("nothing new for ten frames"), [CheckQuiet]() { return CheckQuiet(TEXT("with nothing new")); });
		Steps.Do(TEXT("push two more matches while the bar is open"), [this, State]()
		{
			TestEqual(TEXT("four matches again"), RequireOutputView()->GetFindMatchCount(), 4);
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("%s E"), *State->Needle));
			Store.PushDreamLine(0, EDreamStream::Stdout, FString::Printf(TEXT("%s F"), *State->Needle));
			return true;
		});
		Steps.Poll(TEXT("the next pass finds the new lines"), [this]()
		{
			FDreamEditorModule::Get().GetOutputStore().Drain(0.005);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			return View.IsValid() && View->GetFindMatchCount() == 6 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 5.0);
		Steps.Do(TEXT("Clear output with the bar open"), []()
		{
			FDreamEditorModule::Get().GetOutputStore().Clear();
			return true;
		});
		AddWaitSearched(Steps, TEXT("the cleared ring is searched"));
		Steps.WaitFrames(3);
		Steps.Do(TEXT("count the passes and row rebuilds after the Clear"), [this, Mark]()
		{
			TestEqual(TEXT("after Clear output the bar finds nothing"), RequireOutputView()->GetFindMatchCount(), 0);
			return Mark();
		});
		Steps.WaitFrames(10);
		Steps.Do(TEXT("ten frames after the Clear"), [CheckQuiet]() { return CheckQuiet(TEXT("after the Clear")); });
		Steps.Do(TEXT("Esc in the find box"), [this]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			View->OpenFind();            // focus is in the box again
			DreamOutputFindSpec::PressKey(EKeys::Escape);
			TestFalse(TEXT("Esc in the box closes the bar"), View->IsFindOpen());
			TestEqual(TEXT("closing drops the matches"), View->GetFindMatchCount(), 0);
			const TSharedPtr<SWidget> List = DreamOutputFindSpec::FindFocusable(View.ToSharedRef());
			TestTrue(TEXT("focus is back on the rows"), List.IsValid() && List->HasKeyboardFocus());
			return true;
		});
		Steps.Finally(TEXT("close the bar, restore the toggles, follow again"), [this, State]()
		{
			if (State->bVisibilitySaved && FDreamEditorModule::IsAvailable())
			{
				FDreamEditorModule::Get().GetOutputStore().SetVisibility(State->SavedVisibility);
			}
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->CloseFind();
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("reveals a match inside a folded statement echo (B92, OUT-12)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Needle = FString::Printf(TEXT("fold_needle_%s"), *Sfx);
		// One statement whose echo is longer than the fold (Normal shows its first EchoFoldLines lines): the needle is last.
		const UDreamUserSettings* Settings = UDreamUserSettings::Get();
		const int32 FoldLines = FMath::Max(1, Settings ? Settings->EchoFoldLines : 6);
		FString Code = TEXT("if True:\n");
		for (int32 Index = 0; Index < FoldLines + 2; ++Index)
		{
			Code += FString::Printf(TEXT("    fold_%s_%d = %d\n"), *Sfx, Index, Index);
		}
		Code += FString::Printf(TEXT("    %s = 1\n"), *Needle);
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		DreamTest::AddRun(Steps, TEXT("a statement with a long echo"), [Code]()
		{
			return DreamTest::MakeReplRequest(Code);
		}, Run);
		Steps.Do(TEXT("search for the folded line"), [this, Run, Needle]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestTrue(*FString::Printf(TEXT("the needle line was echoed: %s"), *Run->Describe()), Run->HasLineContaining(dream::LineType::Echo, Needle)))
			{
				return false;
			}
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			View->OpenFind();
			View->SetFindQuery(Needle, false, false, false);
			return true;
		});
		AddWaitSearched(Steps, TEXT("the search pass ends"));
		Steps.Do(TEXT("step to the match"), [this, Needle]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!TestEqual(TEXT("the echo line is the one match"), View->GetFindMatchCount(), 1) || !TestTrue(TEXT("a step finds it"), View->FindStep(1)))
			{
				return false;
			}
			TestTrue(*FString::Printf(TEXT("the folded echo line is opened and selected: \"%s\""), *View->GetSelectedText()),
				View->GetSelectedText().Contains(Needle));
			return true;
		});
		Steps.Finally(TEXT("close the bar and follow again"), [this]()
		{
			if (Tab.IsValid() && Tab->GetOutputArea().IsValid())
			{
				if (const TSharedPtr<SDreamOutputView> View = Tab->GetOutputArea()->GetOutputView())
				{
					View->CloseFind();
					View->ScrollToEnd();
				}
			}
		});
		Steps.Run(Done);
	});

	LatentIt("wraps a long line with the soft-wrap toggle (B92)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		using namespace DreamOutputFindSpec;
		TSharedRef<FState> State = MakeShared<FState>();
		State->Sfx = DreamTest::UniqueSuffix();
		const FString Short = FString::Printf(TEXT("wrap_short_%s"), *State->Sfx);
		FString Long = FString::Printf(TEXT("wrap_long_%s"), *State->Sfx);
		while (Long.Len() < 4000)
		{
			Long += TEXT(" lorem ipsum dolor");
		}
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("soft wrap off; a short line and a long line at the end of the Output view"), [this, State, Short, Long]()
		{
			State->bSavedWrap = SDreamOutputView::IsSoftWrapEnabled();
			SDreamOutputView::SetSoftWrapEnabled(false);
			Tab->GetOutputArea()->SetActiveTab(EDreamOutputTab::Output);
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			if (!View.IsValid())
			{
				return false;
			}
			View->ScrollToEnd();
			FDreamOutputStore& Store = FDreamEditorModule::Get().GetOutputStore();
			Store.PushDreamLine(0, EDreamStream::Stdout, Short);
			Store.PushDreamLine(0, EDreamStream::Stdout, Long);
			return true;
		});
		Steps.WaitFrames(6);
		Steps.Do(TEXT("both rows are one line high"), [this, State, Short, Long]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			State->ShortSeq = DreamOutputFindSpec::FindSeq(Short);
			State->LongSeq = DreamOutputFindSpec::FindSeq(Long);
			if (!TestTrue(TEXT("both lines are in the ring"), State->ShortSeq != 0 && State->LongSeq != 0))
			{
				return false;
			}
			State->ShortHeight = View->GetRowHeightForTest(State->ShortSeq);
			State->LongHeight = View->GetRowHeightForTest(State->LongSeq);
			AddInfo(FString::Printf(TEXT("wrap off: short row %.1f px, long row %.1f px"), State->ShortHeight, State->LongHeight));
			if (!TestTrue(TEXT("both rows have widgets"), State->ShortHeight > 0.f && State->LongHeight > 0.f))
			{
				return false;
			}
			TestTrue(TEXT("unwrapped, the long line's row is one line high"), State->LongHeight < State->ShortHeight * 1.5f);
			SDreamOutputView::SetSoftWrapEnabled(true);
			View->ScrollToEnd();
			return true;
		});
		Steps.WaitFrames(6);
		Steps.Do(TEXT("the long row wraps, the short one does not"), [this, State]()
		{
			const TSharedPtr<SDreamOutputView> View = RequireOutputView();
			const float ShortHeight = View->GetRowHeightForTest(State->ShortSeq);
			const float LongHeight = View->GetRowHeightForTest(State->LongSeq);
			AddInfo(FString::Printf(TEXT("wrap on: short row %.1f px, long row %.1f px"), ShortHeight, LongHeight));
			TestTrue(TEXT("soft wrap is on"), SDreamOutputView::IsSoftWrapEnabled());
			TestTrue(TEXT("wrapped, the long line's row spans several lines"), LongHeight > State->ShortHeight * 2.f);
			TestTrue(TEXT("the short line's row keeps its height"), ShortHeight > 0.f && ShortHeight < State->ShortHeight * 1.5f);
			return true;
		});
		Steps.Finally(TEXT("restore the toggle and follow again"), [this, State]()
		{
			SDreamOutputView::SetSoftWrapEnabled(State->bSavedWrap);
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
}
