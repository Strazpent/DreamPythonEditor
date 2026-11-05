// SPDX-License-Identifier: Apache-2.0
// Dream.Repl.SameFrameKeys: REPL prompt keys that arrive in the same frame as a change of the prompt's text (crash
// UECC-Windows-D711643D4444F769A8ED14821115EF0A, 01:32: FSlateEditableTextLayout::TranslateLocationVertical's ensure
// CurrentLineViewIndex != INDEX_NONE, then an out-of-range index). The stock text layout moves the caret by its line
// views, which a whole-text set rebuilds only at the next prepass; Windows delivers every key queued during a slow frame
// in one message pump, and the strip replays its type-ahead synchronously after a submission, so a user can get a caret
// key into that window. SDreamReplStrip lays the prompt out before such keys (RefreshPromptLayout). Every key goes through
// FSlateApplication::ProcessKeyDownEvent, and the keys of one step are one frame:
//   1. the prompt's text is replaced whole by three lines with the caret on the last; Up, Up: the caret is on line 1;
//   2. Ctrl+Enter submits two lines and clears the prompt, then Down: the caret is at the start of the empty prompt;
//   3. Enter on a complete line, then Down while the decision is pending: after the submission the replayed Down leaves
//      the caret at the start of the empty prompt;
//   4. a three-line history entry recalled from the popup with Enter, then Up, Up: the caret is on line 1.
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamReplHistory.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"

namespace DreamReplSameFrameSpec
{
	void Send(const FKey& Key, bool bCtrl = false)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods(false, false, bCtrl, false, false, false, false, false, false);
		App.ProcessKeyDownEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, 0, 0));
		App.ProcessKeyUpEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, 0, 0));
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

	struct FState
	{
		FString PromptBefore;
		bool bSaved = false;
	};
}

BEGIN_DEFINE_SPEC(FDreamReplSameFrameSpec, "Dream.Repl.SameFrameKeys", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	bool bSavedReplPrompt = false;
	TSharedPtr<SDreamReplStrip> Strip() const;
	TSharedPtr<SDreamCodeView> Prompt() const;
	/** Shows the REPL tab and puts keyboard focus in the prompt; false (with an error) when it cannot. */
	bool FocusPrompt();
	/** The prompt's text is Text, laid out (two frames), with the caret at its end; the prompt's text before is saved. */
	void AddSetPrompt(DreamTest::FDreamSteps& Steps, TSharedRef<DreamReplSameFrameSpec::FState> State, const FString& Text);
	void AddRestorePrompt(DreamTest::FDreamSteps& Steps, TSharedRef<DreamReplSameFrameSpec::FState> State);
END_DEFINE_SPEC(FDreamReplSameFrameSpec)

TSharedPtr<SDreamReplStrip> FDreamReplSameFrameSpec::Strip() const
{
	return Tab.IsValid() && Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetReplStrip() : nullptr;
}

TSharedPtr<SDreamCodeView> FDreamReplSameFrameSpec::Prompt() const
{
	const TSharedPtr<SDreamReplStrip> Repl = Strip();
	return Repl.IsValid() ? Repl->GetPromptView() : nullptr;
}

bool FDreamReplSameFrameSpec::FocusPrompt()
{
	FSlateApplication::Get().DismissAllMenus();
	const TSharedPtr<SDreamCodeView> View = Prompt();
	if (!TestTrue(TEXT("the Dream tab has a REPL prompt"), View.IsValid()))
	{
		return false;
	}
	Tab->GetOutputArea()->FocusRepl();
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
	if (!DreamReplSameFrameSpec::FocusIsInside(View.ToSharedRef()))
	{
		FString Chain;
		for (TSharedPtr<SWidget> Widget = Focused; Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			Chain += Widget->GetTypeAsString() + TEXT(" < ");
		}
		FWidgetPath Path;
		const bool bPath = FSlateApplication::Get().FindPathToWidget(View.ToSharedRef(), Path);
		AddError(FString::Printf(TEXT("keyboard focus is not in the prompt: focus chain %s; path to the prompt %d; output area %.0f x %.0f px"),
			*Chain, bPath ? 1 : 0, Tab->GetOutputArea()->GetCachedGeometry().GetLocalSize().X, Tab->GetOutputArea()->GetCachedGeometry().GetLocalSize().Y));
		return false;
	}
	return true;
}

void FDreamReplSameFrameSpec::AddSetPrompt(DreamTest::FDreamSteps& Steps, TSharedRef<DreamReplSameFrameSpec::FState> State, const FString& Text)
{
	int32 LineCount = 1;
	for (const TCHAR Char : Text)
	{
		LineCount += Char == TEXT('\n') ? 1 : 0;
	}
	// After an earlier spec's Run-window run another top-level window (the DreamHost layout's Message Log) can hold Slate
	// focus, and the main window's activation then takes back any focus set into it (chrome, board 19:16): the Dream tab's
	// window comes to the front first. (output, PO-04) BeforeEach also shows the REPL prompt strip (View ▸ REPL prompt):
	// both settle before the first focus (Dream.Shell.Chrome covers Focus REPL on a hidden strip).
	Steps.Do(TEXT("the Dream tab's window is in front"), [this]()
	{
		if (const TSharedPtr<SWindow> Window = Tab.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef()) : nullptr)
		{
			Window->BringToFront(/*bForce*/ true);
		}
		return true;
	});
	Steps.WaitFrames(3);
	Steps.Do(FString::Printf(TEXT("the prompt holds %d line(s), laid out"), LineCount), [this, State, Text]()
	{
		if (!State->bSaved)
		{
			State->PromptBefore = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString();
			State->bSaved = true;
		}
		if (!FocusPrompt())
		{
			return false;
		}
		Prompt()->SetTextNoUndo(Text);
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		Prompt()->SetCaret(FMath::Max(0, Lines.Num() - 1), Lines.Num() > 0 ? Lines.Last().Len() : 0);
		return true;
	});
	Steps.WaitFrames(2);
}

void FDreamReplSameFrameSpec::AddRestorePrompt(DreamTest::FDreamSteps& Steps, TSharedRef<DreamReplSameFrameSpec::FState> State)
{
	Steps.Finally(TEXT("restore the prompt"), [State]()
	{
		FSlateApplication::Get().DismissAllMenus();
		if (State->bSaved && FDreamEditorModule::IsAvailable())
		{
			FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->ResetText(State->PromptBefore, dream::EditOrigin::Load);
		}
	});
}

void FDreamReplSameFrameSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		// (output, PO-04) The REPL prompt strip is off by default (View ▸ REPL prompt); these cases test its keys, not the
		// Focus REPL reveal, so the strip is shown and laid out before they focus it. Restored in AfterEach.
		bSavedReplPrompt = UDreamUserSettings::Get()->bShowReplPrompt;
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, true);
	});

	AfterEach([this]()
	{
		UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, bSavedReplPrompt);
		Tab.Reset();
	});

	LatentIt("moves the caret with two Up keys in the frame the text was replaced whole", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamReplSameFrameSpec::FState> State = MakeShared<DreamReplSameFrameSpec::FState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSetPrompt(Steps, State, FString());
		Steps.Do(TEXT("replace the text whole by three lines, caret on the last, then Up, Up in the same frame"), [this, Sfx]()
		{
			if (!FocusPrompt())
			{
				return false;
			}
			const TSharedPtr<SDreamCodeView> View = Prompt();
			View->SetTextNoUndo(FString::Printf(TEXT("sf_a_%s = 1\nsf_b = 2\nsf_c = 3"), *Sfx));
			View->SetCaret(2, 4);
			DreamReplSameFrameSpec::Send(EKeys::Up);
			DreamReplSameFrameSpec::Send(EKeys::Up);
			int32 Line = -1;
			int32 Column = -1;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("two Up keys from line 3 put the caret on line 1"), Line, 0);
			TArray<FString> Lines;
			View->GetText().ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
			TestTrue(*FString::Printf(TEXT("the caret's column is on that line (%d)"), Column),
				Lines.IsValidIndex(Line) && Column >= 0 && Column <= Lines[Line].Len());
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the prompt still edits"), [this]()
		{
			int32 Line = -1;
			int32 Column = -1;
			Prompt()->GetCaret(Line, Column);
			TestEqual(TEXT("the caret is still on line 1 after the next layout"), Line, 0);
			return true;
		});
		AddRestorePrompt(Steps, State);
		Steps.Run(Done);
	});

	LatentIt("keeps the caret on the cleared prompt with Down in the frame Ctrl+Enter submitted", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamReplSameFrameSpec::FState> State = MakeShared<DreamReplSameFrameSpec::FState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSetPrompt(Steps, State, FString::Printf(TEXT("sf_x_%s = 1\nsf_y_%s = 2"), *Sfx, *Sfx));
		Steps.Do(TEXT("Ctrl+Enter, then Down in the same frame"), [this, Run]()
		{
			if (!FocusPrompt())
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Enter, /*bCtrl*/ true);
			Run->RunNumber = DreamTest::GetActiveRunNumber();
			const TSharedPtr<SDreamCodeView> View = Prompt();
			if (!TestTrue(TEXT("Ctrl+Enter submitted and cleared the prompt"), Run->RunNumber > 0 && View->GetText().IsEmpty()))
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Down);
			int32 Line = -1;
			int32 Column = -1;
			View->GetCaret(Line, Column);
			TestTrue(*FString::Printf(TEXT("Down leaves the caret at the start of the empty prompt (line %d, column %d)"), Line + 1, Column),
				Line == 0 && Column == 0);
			return true;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the submitted run"), Run);
		Steps.Do(TEXT("check the run"), [this, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			return true;
		});
		AddRestorePrompt(Steps, State);
		Steps.Run(Done);
	});

	LatentIt("keeps the caret on the cleared prompt with a Down typed ahead of the Enter decision", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamReplSameFrameSpec::FState> State = MakeShared<DreamReplSameFrameSpec::FState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSetPrompt(Steps, State, FString::Printf(TEXT("sf_typeahead_%s = 3"), *Sfx));
		Steps.Do(TEXT("Enter on a complete line, then Down while the decision is pending"), [this]()
		{
			if (!FocusPrompt())
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Enter);
			if (!TestTrue(TEXT("Enter waits for classify_input (§6.5)"), Strip()->IsDecisionPending()))
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Down);   // type-ahead: replayed right after the submission clears the prompt
			return true;
		});
		Steps.Poll(TEXT("the decision submits and replays the Down"), [this, Run]()
		{
			if (Strip()->IsDecisionPending())
			{
				return DreamTest::EStep::Wait;
			}
			Run->RunNumber = DreamTest::GetActiveRunNumber();
			const TSharedPtr<SDreamCodeView> View = Prompt();
			int32 Line = -1;
			int32 Column = -1;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the line was submitted and the prompt cleared"), View->GetText().IsEmpty());
			TestTrue(*FString::Printf(TEXT("the replayed Down leaves the caret at the start of the empty prompt (line %d, column %d)"), Line + 1, Column),
				Line == 0 && Column == 0);
			return DreamTest::EStep::Next;
		}, 15.0);
		DreamTest::AddWaitIdle(Steps);
		AddRestorePrompt(Steps, State);
		Steps.Run(Done);
	});

	LatentIt("moves the caret with two Up keys in the frame a three-line history entry was recalled", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		TSharedRef<DreamReplSameFrameSpec::FState> State = MakeShared<DreamReplSameFrameSpec::FState>();
		const FString Sfx = DreamTest::UniqueSuffix();
		const FString Entry = FString::Printf(TEXT("for sf_i_%s in range(2):\n    sf_j = sf_i_%s\n    sf_k = sf_j"), *Sfx, *Sfx);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSetPrompt(Steps, State, FString());
		Steps.Do(TEXT("a three-line entry in history; Up on the empty prompt opens the popup"), [this, Entry]()
		{
			FDreamEditorModule::Get().GetReplHistory().Add(Entry, EDreamHistorySource::Repl);
			if (!FocusPrompt())
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Up);
			return TestTrue(TEXT("Up on an empty prompt opened the history popup"), FSlateApplication::Get().AnyMenusVisible());
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Enter recalls the newest entry, then Up, Up in the same frame"), [this, Entry]()
		{
			DreamReplSameFrameSpec::Send(EKeys::Enter);         // the popup's list has focus; its newest entry is selected
			const TSharedPtr<SDreamCodeView> View = Prompt();
			if (!TestEqual(TEXT("Enter put the entry in the prompt"), View->GetText(), Entry))
			{
				return false;
			}
			DreamReplSameFrameSpec::Send(EKeys::Up);
			DreamReplSameFrameSpec::Send(EKeys::Up);
			int32 Line = -1;
			int32 Column = -1;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("two Up keys from the entry's last line put the caret on line 1"), Line, 0);
			return true;
		});
		AddRestorePrompt(Steps, State);
		Steps.Run(Done);
	});
}
