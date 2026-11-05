// SPDX-License-Identifier: Apache-2.0
// Dream.Completion: live completion in the editor (TDD §7.5, §7.6, §6.6; completion group, slice B items B60-B63).
//  - ConsoleExecutor: T-REPL-013 and T-RES-021: "unreal.EditorAss" is answered from FUnrealNameIndex with the whole line
//    as each suggestion's Name, a single class match prefetches its members, a run's names complete on the first keystroke
//    after it (the post-run Session copy, RV-23), a miss is answered on the next keystroke, and GetSuggestedCompletions
//    never calls the resolver (D14(5)). The Cold-boot part of T-REPL-013 needs a fresh editor session (T-LIFE-003).
//  - CodeView: T-MRG-011 (Ctrl+Space opens, Ctrl+Tab never does) and T-MRG-024 (Enter and Tab accept as ONE undo step,
//    '(' and '.' never accept), through FSlateApplication's key path into the focused code view.
//  - Repl: T-MRG-015 ("unreal.EditorAssetLibrary.sa" offers save_asset) and §6.5's first row (Enter accepts, never runs).
//  - Runs: T-MRG-013 (a run start closes the popup) and T-RES-011 (while a run is active the caches answer, the footer says
//    "Runtime results paused while code runs" and the resolver is not called); the run ticks the core ticker from inside
//    (the _dream_test fixture), which is where these steps run.
//  - Resolver: T-RES-008 (a chain corpus within 5 ms at p95) and T-RES-024 (members of a live getter-fixture object are
//    listed and none of its BlueprintGetters runs, INT-02).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/SDreamCompletionPopup.h"
#include "Commands/DreamCommands.h"
#include "Completion/DreamCompletionService.h"
#include "Completion/DreamCompletionTypes.h"
#include "Fixtures/DreamTestGetterFixture.h"
#include "Fixtures/DreamTestLatch.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "Output/DreamConsoleExecutor.h"
#include "Python/DreamPython.h"
#include "Python/DreamTestPython.h"
#include "Run/DreamRunService.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"

namespace DreamCompletionSpec
{
	/** A key-down (and key-up) through FSlateApplication to the focused widget; true when the key-down was handled. */
	bool Press(const FKey& Key, bool bCtrl, uint32 Character = 0)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods(false, false, bCtrl, false, false, false, false, false, false);
		const bool bHandled = App.ProcessKeyDownEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, Character, Character));
		App.ProcessKeyUpEvent(FKeyEvent(Key, Mods, App.GetUserIndexForKeyboard(), false, Character, Character));
		return bHandled;
	}

	/** Ctrl+Space as Windows delivers it: the key-down, then a WM_CHAR space with Ctrl held, then the key-up. True when
	 *  Dream handled both the key-down and its key-up (§4.9.1: the key-up of a consumed key-down never reaches Unreal). */
	bool PressCtrlSpace()
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Ctrl(false, false, true, false, false, false, false, false, false);
		const bool bDown = App.ProcessKeyDownEvent(FKeyEvent(EKeys::SpaceBar, Ctrl, App.GetUserIndexForKeyboard(), false, TEXT(' '), TEXT(' ')));
		App.ProcessKeyCharEvent(FCharacterEvent(TEXT(' '), Ctrl, App.GetUserIndexForKeyboard(), false));
		const bool bUp = App.ProcessKeyUpEvent(FKeyEvent(EKeys::SpaceBar, Ctrl, App.GetUserIndexForKeyboard(), false, TEXT(' '), TEXT(' ')));
		return bDown && bUp;
	}

	/** A plain key-down and key-up; true when both were handled. */
	bool PressBoth(const FKey& Key, uint32 Character = 0)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState NoMods;
		const bool bDown = App.ProcessKeyDownEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, Character, Character));
		const bool bUp = App.ProcessKeyUpEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, Character, Character));
		return bDown && bUp;
	}

	/** A typed character with no key-down of interest (the text inserts on the character event). */
	void TypeChar(TCHAR Character)
	{
		FSlateApplication& App = FSlateApplication::Get();
		App.ProcessKeyCharEvent(FCharacterEvent(Character, FModifierKeysState(), App.GetUserIndexForKeyboard(), false));
	}

	FDreamCompletionService& Service()
	{
		return FDreamEditorModule::Get().GetCompletion();
	}

	/** The query a code view would send for Text with the caret at its end (one line, Session roots). */
	FDreamCompletionQuery MakeQuery(const FString& Text, dream::BufferKind Kind = dream::BufferKind::ConsoleExecutor)
	{
		FDreamCompletionQuery Query;
		Query.Chain = dream::ExtractCompletionChain(DreamText::ToU16(Text), Text.Len());
		Query.Kind = Kind;
		Query.Namespace = TEXT("session");
		return Query;
	}

	/** Labels of an answer's candidates. */
	TSet<FString> LabelsOf(const FDreamCompletionAnswer& Answer)
	{
		TSet<FString> Labels;
		if (Answer.Candidates.IsValid())
		{
			for (const dream::CompletionCandidate& Candidate : *Answer.Candidates)
			{
				Labels.Add(DreamText::Utf8ToFString(Candidate.Label));
			}
		}
		return Labels;
	}

	TArray<FString> SuggestionNames(const TArray<FConsoleSuggestion>& Suggestions)
	{
		TArray<FString> Names;
		for (const FConsoleSuggestion& Suggestion : Suggestions)
		{
			Names.Add(Suggestion.Name);
		}
		return Names;
	}

	/** A step that waits until the completion service has nothing queued (live requests, the prewarm, the index fill). */
	void AddWaitCompletionIdle(DreamTest::FDreamSteps& Steps, const FString& Why, double TimeoutSeconds = 30.0)
	{
		Steps.Poll(FString::Printf(TEXT("the completion service is idle (%s)"), *Why), []()
		{
			const FDreamCompletionService& Completion = Service();
			return !Completion.HasPendingWork() && Completion.IsUnrealIndexComplete() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, TimeoutSeconds);
	}

	/** Queues Query for the resolver and waits until the cache answers it completely. */
	void AddPrime(DreamTest::FDreamSteps& Steps, const FString& Text)
	{
		Steps.Do(FString::Printf(TEXT("ask the resolver for '%s'"), *Text), [Text]()
		{
			Service().RequestLive(MakeQuery(Text), FDreamOnCompletionAnswer());
			return true;
		});
		Steps.Poll(FString::Printf(TEXT("'%s' is cached"), *Text), [Text]()
		{
			FDreamCompletionAnswer Answer;
			return Service().QueryCached(MakeQuery(Text), Answer) && !Answer.bIncomplete && Answer.Num() > 0
				? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 20.0);
	}
}

BEGIN_DEFINE_SPEC(FDreamCompletionSpec, "Dream.Completion", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Sfx;
	/** Opens a scratch tab holding Text with the caret at its end and focuses its code view. */
	TSharedPtr<SDreamCodeView> OpenView(const FString& Text, const FString& Purpose);
	/** Sets a view's text (no undo step) with the caret at the end of its last line. */
	static void SetViewText(SDreamCodeView& View, const FString& Text);
END_DEFINE_SPEC(FDreamCompletionSpec)

TSharedPtr<SDreamCodeView> FDreamCompletionSpec::OpenView(const FString& Text, const FString& Purpose)
{
	const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, Text, Purpose);
	if (!Buffer.IsValid())
	{
		return nullptr;
	}
	BufferIds.Add(Buffer->GetId());
	const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Buffer->GetId());
	if (!TestTrue(TEXT("the scratch buffer has a code view"), View.IsValid())
		|| !TestTrue(TEXT("the code view has a completion controller"), View->GetCompletion().IsValid()))
	{
		return nullptr;
	}
	SetViewText(*View, Text);
	View->FocusEditor();
	return View;
}

void FDreamCompletionSpec::SetViewText(SDreamCodeView& View, const FString& Text)
{
	View.SetTextNoUndo(Text);
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"), false);
	const int32 Last = FMath::Max(0, Lines.Num() - 1);
	View.SetCaret(Last, Lines.IsValidIndex(Last) ? Lines[Last].Len() : 0);
}

void FDreamCompletionSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		Sfx = DreamTest::UniqueSuffix();
	});

	AfterEach([this]()
	{
		if (FDreamEditorModule::IsAvailable())
		{
			for (const FGuid& Id : BufferIds)
			{
				if (const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->FindCodeView(Id) : nullptr)
				{
					if (View->GetCompletion().IsValid())
					{
						View->GetCompletion()->Close();
					}
				}
				DreamTest::CloseBuffer(Id);
			}
		}
		BufferIds.Reset();
		DreamTest::FDreamTestLatch::ForgetPrefix(TEXT("Cmpl"));
		Tab.Reset();
	});

	Describe("ConsoleExecutor", [this]()
	{
		LatentIt("answers from the C++ caches with whole-line names and never calls the resolver (T-REPL-013, T-RES-021)", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const FString Tool = FString::Printf(TEXT("cmpl_tool_%s"), *Sfx);
			const FString Method = FString::Printf(TEXT("method_%s"), *Sfx);
			const FString Inner = FString::Printf(TEXT("inner_%s"), *Sfx);
			const FString Leaf = FString::Printf(TEXT("leaf_%s"), *Sfx);
			const FString Code = FString::Printf(TEXT(
				"class CmplInner_%s:\n"
				"    def %s(self):\n"
				"        return 1\n"
				"class CmplTool_%s:\n"
				"    def __init__(self):\n"
				"        self.%s = CmplInner_%s()\n"
				"    def %s(self):\n"
				"        return 2\n"
				"%s = CmplTool_%s()\n"), *Sfx, *Leaf, *Sfx, *Inner, *Sfx, *Method, *Tool, *Sfx);

			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			Steps.Do(TEXT("start from empty caches"), []() { Service().ResetForTest(); return true; });
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("the run that binds the tool"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
			Steps.Do(TEXT("the run is ok"), [this, Run]() { return DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok); });
			AddWaitCompletionIdle(Steps, TEXT("the post-run Session copy and the unreal index"));

			Steps.Do(TEXT("unreal.EditorAss: a hit within 1 ms, the whole line as Name, no Python"), [this]()
			{
				FDreamConsoleExecutor& Executor = FDreamEditorModule::Get().GetConsoleExecutor();
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				// The box asks on every keystroke: type "unreal.EditorAss" one character at a time and time each answer.
				const FString Line = TEXT("unreal.EditorAss");
				TArray<double> Times;
				TArray<double> Final;
				TArray<FConsoleSuggestion> Out;
				for (int32 Pass = 0; Pass < 7; ++Pass)
				{
					for (int32 Length = 8; Length <= Line.Len(); ++Length)
					{
						Out.Reset();
						const double Start = FPlatformTime::Seconds();
						Executor.GetSuggestedCompletions(*Line.Left(Length), Out);
						const double Elapsed = FPlatformTime::Seconds() - Start;
						Times.Add(Elapsed);
						if (Length == Line.Len())
						{
							Final.Add(Elapsed);
						}
					}
				}
				Times.Sort();
				Final.Sort();
				// T-REPL-013's measure is the answer to "unreal.EditorAss" itself (the earlier keystrokes are logged).
				const double Median = Final[Final.Num() / 2];
				AddInfo(FString::Printf(TEXT("T-REPL-013: unreal.EditorAss answered in %.3f ms (median of %d); all %d keystrokes: median %.3f ms, max %.3f ms; %d suggestions"),
					Median * 1000.0, Final.Num(), Times.Num(), Times[Times.Num() / 2] * 1000.0, Times.Last() * 1000.0, Out.Num()));
				const TArray<FString> Names = DreamCompletionSpec::SuggestionNames(Out);
				TestTrue(*FString::Printf(TEXT("suggests unreal.EditorAssetLibrary (UEI-08); got %d: %s"), Names.Num(),
					*FString::Join(TArrayView<const FString>(Names.GetData(), FMath::Min(Names.Num(), 10)), TEXT(", "))),
					Names.Contains(TEXT("unreal.EditorAssetLibrary")));
				TestTrue(*FString::Printf(TEXT("the hit takes at most 1 ms (median %.3f ms, §6.6 step 2)"), Median * 1000.0), Median <= 0.001);
				TestEqual(TEXT("GetSuggestedCompletions made no resolver call (D14(5), RV-23)"), FDreamPython::GetResolverCallCount(), Calls);
				return true;
			});

			Steps.Do(TEXT("the run's tool completes on the first keystroke after the run (RV-23)"), [this, Tool, Method]()
			{
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				TArray<FConsoleSuggestion> Out;
				FDreamEditorModule::Get().GetConsoleExecutor().GetSuggestedCompletions(*(Tool + TEXT(".")), Out);
				TestTrue(TEXT("tool. offers its method with the whole line as Name"),
					DreamCompletionSpec::SuggestionNames(Out).Contains(Tool + TEXT(".") + Method));
				TestEqual(TEXT("no resolver call"), FDreamPython::GetResolverCallCount(), Calls);
				return true;
			});

			Steps.Do(TEXT("a deeper chain misses first (queued for the resolver, no Python call)"), [this, Tool, Inner, Leaf]()
			{
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				TArray<FConsoleSuggestion> Out;
				FDreamEditorModule::Get().GetConsoleExecutor().GetSuggestedCompletions(*(Tool + TEXT(".") + Inner + TEXT(".")), Out);
				TestFalse(TEXT("the miss has no member of the inner object yet"),
					DreamCompletionSpec::SuggestionNames(Out).Contains(Tool + TEXT(".") + Inner + TEXT(".") + Leaf));
				TestEqual(TEXT("no resolver call on the input path"), FDreamPython::GetResolverCallCount(), Calls);
				return true;
			});
			AddWaitCompletionIdle(Steps, TEXT("the resolver served the miss"));
			Steps.Do(TEXT("the next keystroke has the results"), [this, Tool, Inner, Leaf]()
			{
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				TArray<FConsoleSuggestion> Out;
				FDreamEditorModule::Get().GetConsoleExecutor().GetSuggestedCompletions(*(Tool + TEXT(".") + Inner + TEXT(".")), Out);
				TestTrue(TEXT("tool.inner. offers the inner object's method"),
					DreamCompletionSpec::SuggestionNames(Out).Contains(Tool + TEXT(".") + Inner + TEXT(".") + Leaf));
				TestEqual(TEXT("no resolver call"), FDreamPython::GetResolverCallCount(), Calls);
				return true;
			});

			Steps.Do(TEXT("a single class match prefetches its members (§6.6 step 5)"), [this]()
			{
				TArray<FConsoleSuggestion> Out;
				FDreamEditorModule::Get().GetConsoleExecutor().GetSuggestedCompletions(TEXT("unreal.EditorAssetLibrary"), Out);
				return true;
			});
			AddWaitCompletionIdle(Steps, TEXT("the speculative prefetch"));
			Steps.Do(TEXT("typing '.' then hits the cache"), [this]()
			{
				FDreamCompletionAnswer Answer;
				TestTrue(TEXT("unreal.EditorAssetLibrary. is a cache hit"),
					DreamCompletionSpec::Service().QueryCached(DreamCompletionSpec::MakeQuery(TEXT("unreal.EditorAssetLibrary.")), Answer));
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				TArray<FConsoleSuggestion> Out;
				FDreamEditorModule::Get().GetConsoleExecutor().GetSuggestedCompletions(TEXT("unreal.EditorAssetLibrary.sa"), Out);
				TestTrue(TEXT("unreal.EditorAssetLibrary.sa offers save_asset"),
					DreamCompletionSpec::SuggestionNames(Out).Contains(TEXT("unreal.EditorAssetLibrary.save_asset")));
				TestTrue(TEXT("at most 100 suggestions (§6.6 step 3)"), Out.Num() <= 100);
				TestEqual(TEXT("no resolver call"), FDreamPython::GetResolverCallCount(), Calls);
				return true;
			});
			Steps.Run(Done);
		});
	});

	Describe("CodeView", [this]()
	{
		LatentIt("Ctrl+Space opens the list, Enter and Tab accept as one undo step, ( and . never accept, Ctrl+Tab never opens (T-MRG-011, T-MRG-024)",
			DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const FString Tool = FString::Printf(TEXT("cmpl_view_%s"), *Sfx);
			const FString Alpha = FString::Printf(TEXT("alpha_%s"), *Sfx);
			const FString Beta = FString::Printf(TEXT("beta_%s"), *Sfx);
			const FString Code = FString::Printf(TEXT(
				"class CmplView_%s:\n"
				"    def %s(self):\n"
				"        return 1\n"
				"    def %s(self):\n"
				"        return 2\n"
				"%s = CmplView_%s()\n"), *Sfx, *Alpha, *Beta, *Tool, *Sfx);
			const FString Typed = Tool + TEXT(".al");
			TSharedRef<TSharedPtr<SDreamCodeView>> ViewRef = MakeShared<TSharedPtr<SDreamCodeView>>();

			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("the run that binds the object"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
			Steps.Do(TEXT("open a scratch view"), [this, Typed, ViewRef]()
			{
				*ViewRef = OpenView(Typed, TEXT("Completion"));
				return ViewRef->IsValid();
			});
			Steps.WaitFrames(2);
			Steps.Do(TEXT("Ctrl+Space through Slate"), [this, Typed, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				if (!TestTrue(TEXT("the code view has keyboard focus"), View.HasEditorFocus()))
				{
					return false;
				}
				TestTrue(TEXT("Ctrl+Space and its key-up are Dream's (§4.9.1)"), PressCtrlSpace());
				TestEqual(TEXT("Ctrl+Space inserted nothing (its space is swallowed)"), View.GetText(), Typed);
				TestTrue(TEXT("Ctrl+Space opened or requested the list (T-MRG-011)"),
					View.GetCompletion()->GetState() != FDreamCompletionController::EState::Closed);
				return true;
			});
			Steps.Poll(TEXT("the list is open with the live answer"), [ViewRef]()
			{
				const TSharedPtr<FDreamCompletionController> Completion = (*ViewRef)->GetCompletion();
				return Completion->IsOpen() && !Completion->IsWaitingForLive() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 20.0);
			Steps.Do(TEXT("Enter accepts as one undo step"), [this, Tool, Alpha, Typed, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				const TSharedPtr<FDreamCompletionController> Completion = View.GetCompletion();
				const TArray<FString> Labels = Completion->GetLabels();
				TestTrue(*FString::Printf(TEXT("'al' ranks %s first; got %s"), *Alpha, *FString::Join(Labels, TEXT(", "))),
					Labels.Num() > 0 && Labels[0] == Alpha);
				const TSharedPtr<SDreamCompletionPopup> Popup = Completion->GetPopup();
				TestTrue(TEXT("the popup draws the runtime badge (U4)"), Popup.IsValid() && Popup->GetRowBadge(0) == TEXT("runtime"));
				TestTrue(TEXT("Enter and its key-up are the popup's"), DreamCompletionSpec::PressBoth(EKeys::Enter));
				TestEqual(TEXT("Enter inserted the selected item (T-MRG-024)"), View.GetText(), Tool + TEXT(".") + Alpha);
				TestTrue(TEXT("the popup closed after the accept"), Completion->GetState() == FDreamCompletionController::EState::Closed);
				TestTrue(TEXT("the accept is undoable"), View.CanUndo());
				View.Undo();
				TestEqual(TEXT("one Ctrl+Z brings back the typed prefix (one undo step, EDT-08)"), View.GetText(), Typed);
				return true;
			});
			Steps.Do(TEXT("Tab accepts the selected row"), [this, Tool, Beta, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				SetViewText(View, Tool + TEXT("."));
				const TSharedPtr<FDreamCompletionController> Completion = View.GetCompletion();
				PressCtrlSpace();
				if (!TestTrue(TEXT("a cached member list opens at once"), Completion->IsOpen()))
				{
					return false;
				}
				const int32 BetaIndex = Completion->GetLabels().IndexOfByKey(Beta);
				if (!TestTrue(TEXT("the member list has beta"), BetaIndex != INDEX_NONE))
				{
					return false;
				}
				Completion->SetSelected(BetaIndex);
				TestTrue(TEXT("Tab is the popup's"), DreamCompletionSpec::Press(EKeys::Tab, false));
				TestEqual(TEXT("Tab inserted the selected item (T-MRG-024)"), View.GetText(), Tool + TEXT(".") + Beta);
				return true;
			});
			// A typed character is decided once its edit is done (the auto-closed pair places the caret afterwards): each
			// check waits a frame.
			Steps.Do(TEXT("'(' with the list open"), [this, Typed, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				SetViewText(View, Typed);
				PressCtrlSpace();
				TestTrue(TEXT("the list is open"), View.GetCompletion()->IsOpen());
				DreamCompletionSpec::TypeChar(TEXT('('));
				return true;
			});
			Steps.WaitFrames(2);
			Steps.Do(TEXT("'(' never accepts and closes the list"), [this, Alpha, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				TestFalse(TEXT("'(' did not insert the item (no commit characters, §7.6.2)"), View.GetText().Contains(Alpha));
				TestTrue(TEXT("'(' closed the popup"), View.GetCompletion()->GetState() == FDreamCompletionController::EState::Closed);
				return true;
			});
			Steps.Do(TEXT("'.' with the list open"), [this, Typed, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				SetViewText(View, Typed);
				PressCtrlSpace();
				TestTrue(TEXT("the list is open again"), View.GetCompletion()->IsOpen());
				DreamCompletionSpec::TypeChar(TEXT('.'));
				return true;
			});
			Steps.WaitFrames(2);
			Steps.Do(TEXT("'.' never accepts; Esc closes"), [this, Typed, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				const TSharedPtr<FDreamCompletionController> Completion = View.GetCompletion();
				TestEqual(TEXT("'.' typed a dot and inserted no item"), View.GetText(), Typed + TEXT("."));
				Completion->Close();

				SetViewText(View, Typed);
				PressCtrlSpace();
				TestTrue(TEXT("Esc is the popup's"), DreamCompletionSpec::Press(EKeys::Escape, false));
				TestTrue(TEXT("Esc closed the popup"), Completion->GetState() == FDreamCompletionController::EState::Closed);
				TestEqual(TEXT("Esc left the text alone"), View.GetText(), Typed);
				return true;
			});
			Steps.Do(TEXT("typing an identifier character opens the list (§7.6.2 trigger)"), [this, Tool, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				SetViewText(View, Tool + TEXT("."));
				DreamCompletionSpec::TypeChar(TEXT('b'));
				return true;
			});
			Steps.WaitFrames(2);
			Steps.Do(TEXT("the typed trigger opened the cached list"), [this, Tool, Beta, ViewRef]()
			{
				SDreamCodeView& View = **ViewRef;
				const TSharedPtr<FDreamCompletionController> Completion = View.GetCompletion();
				TestEqual(TEXT("the b was typed"), View.GetText(), Tool + TEXT(".b"));
				const TArray<FString> Labels = Completion->GetLabels();
				TestTrue(*FString::Printf(TEXT("typing b opened the list with %s first; got %s"), *Beta, *FString::Join(Labels, TEXT(", "))),
					Completion->IsOpen() && Labels.Num() > 0 && Labels[0] == Beta);
				Completion->Close();
				return true;
			});
			Steps.Do(TEXT("Ctrl+Tab never opens completion (D8)"), [this, ViewRef]()
			{
				const TSharedPtr<FDreamCompletionController> Completion = (*ViewRef)->GetCompletion();
				FSlateApplication& App = FSlateApplication::Get();
				const FModifierKeysState Ctrl(false, false, true, false, false, false, false, false, false);
				// Straight to the controller: Ctrl+Tab passes through to Unreal (§4.9.4), where it would switch editor tabs.
				TestFalse(TEXT("Ctrl+Tab is not the Complete chord"),
					Completion->HandleKeyDown(FKeyEvent(EKeys::Tab, Ctrl, App.GetUserIndexForKeyboard(), false, 0, 0)));
				TestTrue(TEXT("nothing opened"), Completion->GetState() == FDreamCompletionController::EState::Closed);
				TestTrue(TEXT("Ctrl+Space is"),
					Completion->HandleKeyDown(FKeyEvent(EKeys::SpaceBar, Ctrl, App.GetUserIndexForKeyboard(), false, TEXT(' '), TEXT(' '))));
				Completion->Close();
				return true;
			});
			Steps.Run(Done);
		});
	});

	Describe("Repl", [this]()
	{
		LatentIt("unreal.EditorAssetLibrary.sa offers save_asset and Enter accepts instead of running (T-MRG-015, REPL-03)", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			const TSharedPtr<SDreamReplStrip> Strip = Area.IsValid() ? Area->GetReplStrip() : nullptr;
			const TSharedPtr<SDreamCodeView> Prompt = Strip.IsValid() ? Strip->GetPromptView() : nullptr;
			if (!TestTrue(TEXT("the REPL prompt exists"), Prompt.IsValid() && Prompt->GetCompletion().IsValid()))
			{
				Done.Execute();
				return;
			}
			const FString Typed = TEXT("unreal.EditorAssetLibrary.sa");
			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("import unreal"), []() { return DreamTest::MakeReplRequest(TEXT("import unreal")); }, Run, 60.0);
			AddWaitCompletionIdle(Steps, TEXT("the unreal index"));
			Steps.Do(TEXT("Focus REPL, then type into the prompt"), [this, Prompt, Typed]()
			{
				Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);   // shows the strip (REPL tab) and focuses the prompt (§6.5)
				SetViewText(*Prompt, Typed);
				return true;
			});
			Steps.WaitFrames(2);
			Steps.Do(TEXT("Ctrl+Space in the prompt"), [this, Prompt, Typed]()
			{
				if (!TestTrue(TEXT("the prompt has keyboard focus"), Prompt->HasEditorFocus()))
				{
					return false;
				}
				PressCtrlSpace();
				TestEqual(TEXT("Ctrl+Space inserted nothing"), Prompt->GetText(), Typed);
				return true;
			});
			Steps.Poll(TEXT("the list is open with the live answer"), [Prompt]()
			{
				const TSharedPtr<FDreamCompletionController> Completion = Prompt->GetCompletion();
				return Completion->IsOpen() && !Completion->IsWaitingForLive() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 20.0);
			Steps.Do(TEXT("save_asset is offered; Enter accepts it and runs nothing"), [this, Prompt, Strip]()
			{
				const TSharedPtr<FDreamCompletionController> Completion = Prompt->GetCompletion();
				const int32 Index = Completion->GetLabels().IndexOfByKey(TEXT("save_asset"));
				if (!TestTrue(*FString::Printf(TEXT("save_asset is offered (T-MRG-015); got %s"), *FString::Join(Completion->GetLabels(), TEXT(", "))),
					Index != INDEX_NONE))
				{
					return false;
				}
				Completion->SetSelected(Index);
				TestTrue(TEXT("Enter is the popup's in the REPL (§6.5 first row)"), DreamCompletionSpec::Press(EKeys::Enter, false));
				TestEqual(TEXT("Enter accepted the completion"), Prompt->GetText(), FString(TEXT("unreal.EditorAssetLibrary.save_asset")));
				TestTrue(TEXT("no run was submitted"), DreamTest::IsIdle());
				TestFalse(TEXT("no completeness decision is pending"), Strip->IsDecisionPending());
				return true;
			});
			Steps.Finally(TEXT("clear the prompt"), [Prompt]()
			{
				if (Prompt->GetCompletion().IsValid())
				{
					Prompt->GetCompletion()->Close();
				}
				Prompt->SetTextNoUndo(FString());
			});
			Steps.Run(Done);
		});
	});

	Describe("Runs", [this]()
	{
		LatentIt("a run start closes the popup; during the run the caches answer with the paused footer and no resolver call (T-MRG-013, T-RES-011)",
			DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this)
				|| !TestTrue(TEXT("the _dream_test fixture module exists"), DreamTestPython::IsModuleCreated()))
			{
				Done.Execute();
				return;
			}
			const FString Typed = TEXT("unreal.EditorAssetLibrary.");
			const FString Latch = FString::Printf(TEXT("CmplHold_%s"), *Sfx);
			const FString Mark = FString::Printf(TEXT("CmplLoop_%s"), *Sfx);
			const FString Code = FString::Printf(TEXT(
				"import _dream_test\n"
				"_dream_test.mark('%s')\n"
				"while _dream_test.held('%s'):\n"
				"    _dream_test.tick_core_ticker(1)\n"), *Mark, *Latch);
			TSharedRef<TSharedPtr<SDreamCodeView>> ViewRef = MakeShared<TSharedPtr<SDreamCodeView>>();
			TSharedRef<DreamTest::FRunResult> Held = MakeShared<DreamTest::FRunResult>();

			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			TSharedRef<DreamTest::FRunResult> Import = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("import unreal"), []() { return DreamTest::MakeReplRequest(TEXT("import unreal")); }, Import, 60.0);
			AddWaitCompletionIdle(Steps, TEXT("the unreal index"));
			AddPrime(Steps, Typed);
			Steps.Do(TEXT("open the popup in a scratch view"), [this, Typed, ViewRef]()
			{
				*ViewRef = OpenView(Typed, TEXT("CompletionRun"));
				if (!ViewRef->IsValid())
				{
					return false;
				}
				(*ViewRef)->GetCompletion()->Trigger(/*bExplicit*/ true);
				return TestTrue(TEXT("the cached list opens at once"), (*ViewRef)->GetCompletion()->IsOpen());
			});
			Steps.Do(TEXT("submit the held run: the popup closes (T-MRG-013)"), [this, Code, Latch, Held, ViewRef]()
			{
				DreamTest::FDreamTestLatch::Reset(Latch);
				Held->SubmitSeconds = FPlatformTime::Seconds();
				Held->RunNumber = DreamTest::Submit(*this, DreamTest::MakeReplRequest(Code));
				TestTrue(TEXT("the run start closed the popup"),
					(*ViewRef)->GetCompletion()->GetState() == FDreamCompletionController::EState::Closed);
				return Held->RunNumber > 0;
			});
			// The run ticks the core ticker from inside, so the next steps run while it is active.
			Steps.Poll(TEXT("the run reached its loop"), [Mark]()
			{
				return DreamTest::FDreamTestLatch::GetMark(Mark) > 0.0 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 30.0);
			Steps.Do(TEXT("Ctrl+Space during the run (T-RES-011)"), [this, Latch, ViewRef]()
			{
				const bool bActive = FDreamEditorModule::Get().GetRunService().IsRunActive();
				TestTrue(TEXT("a run is active"), bActive);
				const TSharedPtr<FDreamCompletionController> Completion = (*ViewRef)->GetCompletion();
				const uint64 Calls = FDreamPython::GetResolverCallCount();
				Completion->Trigger(/*bExplicit*/ true);
				TestTrue(TEXT("the cached list opens during the run"), Completion->IsOpen());
				TestTrue(TEXT("unreal.EditorAssetLibrary. completes from the caches"), Completion->GetLabels().Contains(TEXT("save_asset")));
				TestEqual(TEXT("the footer says runtime results are paused (§7.5.3)"), Completion->GetFooter().ToString(),
					FString(TEXT("Runtime results paused while code runs")));
				TestFalse(TEXT("nothing is queued for the resolver during the run"), Completion->IsWaitingForLive());
				TestEqual(TEXT("the resolver was not called (D6)"), FDreamPython::GetResolverCallCount(), Calls);
				Completion->Close();
				DreamTest::FDreamTestLatch::Release(Latch);
				return bActive;
			});
			DreamTest::AddWaitForRun(Steps, TEXT("the held run"), Held, 30.0);
			Steps.Do(TEXT("the held run is ok"), [this, Held]() { return DreamTest::ExpectOutcome(*this, *Held, EDreamRunOutcome::Ok); });
			Steps.Finally(TEXT("release the latch"), [Latch]() { DreamTest::FDreamTestLatch::Release(Latch); });
			Steps.Run(Done);
		});
	});

	Describe("Resolver", [this]()
	{
		LatentIt("answers a chain corpus within 5 ms at p95 (T-RES-008)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const FString Code = FString::Printf(TEXT(
				"import unreal, os, collections\n"
				"cmpl_corpus_%s = {'a': [1, 2, (3, 'x')], 'b': collections.OrderedDict(k=os)}\n"), *Sfx);
			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("the corpus roots"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
			AddWaitCompletionIdle(Steps, TEXT("the unreal index"));
			Steps.Do(TEXT("500 chains through the resolver"), [this]()
			{
				// The corpus: unreal.<Class>. for the index's classes, then Session chains with subscripts, calls and modules.
				TArray<FString> Corpus;
				FDreamCompletionAnswer Top;
				DreamCompletionSpec::Service().QueryCached(DreamCompletionSpec::MakeQuery(TEXT("unreal.")), Top);
				if (Top.Candidates.IsValid())
				{
					for (const dream::CompletionCandidate& Candidate : *Top.Candidates)
					{
						if (Candidate.Kind == dream::CompletionKind::Class && Corpus.Num() < 440)
						{
							Corpus.Add(TEXT("unreal.") + DreamText::Utf8ToFString(Candidate.Label) + TEXT("."));
						}
					}
				}
				const FString Root = FString::Printf(TEXT("cmpl_corpus_%s"), *Sfx);
				const TCHAR* Tails[] = { TEXT("."), TEXT("['a']."), TEXT("['a'][2]."), TEXT("['a'][2][1]."), TEXT("['b']."), TEXT("['b']['k']."),
					TEXT("['b']['k'].path."), TEXT(".keys()."), TEXT("['a'][-1]."), TEXT("['a'][0].") };
				while (Corpus.Num() < 500)
				{
					for (const TCHAR* Tail : Tails)
					{
						Corpus.Add(Root + Tail);
					}
					Corpus.Add(TEXT("os.path."));
					Corpus.Add(TEXT("collections."));
				}
				Corpus.SetNum(500);

				FDreamPython& Python = FDreamEditorModule::Get().GetPython();
				TArray<double> Times;
				int32 Failures = 0;
				for (const FString& Chain : Corpus)
				{
					FDreamResolverRequest Request;
					Request.Chain = dream::ExtractCompletionChain(DreamText::ToU16(Chain), Chain.Len());
					Request.Namespace = TEXT("session");
					Request.BudgetNs = FDreamCompletionService::LiveBudgetNs;
					Request.Generation = FDreamEditorModule::Get().GetRunService().GetRunGeneration();
					FDreamResolverResult Result;
					if (!Python.ResolverComplete(Request, Result) || (!Result.Error.IsEmpty() && Result.Error != TEXT("breaker")))
					{
						++Failures;
						continue;
					}
					Times.Add(Result.CallSeconds);
				}
				TestEqual(TEXT("every chain was answered"), Failures, 0);
				if (Times.Num() == 0)
				{
					return false;
				}
				Times.Sort();
				const double P95 = Times[FMath::Min(Times.Num() - 1, (Times.Num() * 95) / 100)];
				AddInfo(FString::Printf(TEXT("T-RES-008: %d chains, median %.2f ms, p95 %.2f ms, max %.2f ms"), Times.Num(),
					Times[Times.Num() / 2] * 1000.0, P95 * 1000.0, Times.Last() * 1000.0));
				TestTrue(*FString::Printf(TEXT("p95 within 5 ms (PERF-06, D6): %.2f ms"), P95 * 1000.0), P95 <= 0.005);
				return true;
			});
			Steps.Run(Done);
		});

		LatentIt("lists a live object's members without running its getters (T-RES-024, INT-02)", DreamTest::LatentTimeout(),
			[this](const FDoneDelegate& Done)
		{
			using namespace DreamCompletionSpec;
			if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
			{
				Done.Execute();
				return;
			}
			const FString Name = FString::Printf(TEXT("cmpl_getters_%s"), *Sfx);
			const FString Code = FString::Printf(TEXT(
				"import unreal\n"
				"%s = unreal.DreamTestGetterFixture()\n"
				"unreal.DreamTestGetterFixture.reset_getter_calls()\n"), *Name);
			DreamTest::FDreamSteps Steps(*this);
			DreamTest::AddWaitIdle(Steps);
			TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
			DreamTest::AddRun(Steps, TEXT("the getter fixture"), [Code]() { return DreamTest::MakeReplRequest(Code); }, Run, 60.0);
			Steps.Do(TEXT("the run is ok"), [this, Run]() { return DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok); });
			AddWaitCompletionIdle(Steps, TEXT("the post-run Session copy"));
			// 1,000 getter-backed properties may not list within one 5 ms call on a busy machine (the answer is then partial,
			// incomplete); the resolver is asked until an answer arrives, as the next keystroke would.
			TSharedRef<FDreamCompletionAnswer> Fixture = MakeShared<FDreamCompletionAnswer>();
			TSharedRef<bool> bAnswered = MakeShared<bool>(false);
			Steps.Poll(FString::Printf(TEXT("the resolver answers '%s.'"), *Name), [Name, Fixture, bAnswered]()
			{
				if (*bAnswered)
				{
					return DreamTest::EStep::Next;
				}
				if (Service().QueryCached(MakeQuery(Name + TEXT(".")), *Fixture) && Fixture->Num() > 0)
				{
					*bAnswered = true;
					return DreamTest::EStep::Next;
				}
				if (!Service().HasPendingWork())
				{
					Service().RequestLive(MakeQuery(Name + TEXT(".")), FDreamOnCompletionAnswer::CreateLambda(
						[Fixture, bAnswered](const FDreamCompletionAnswer& Answer)
						{
							if (Answer.Num() > 0)
							{
								*Fixture = Answer;
								*bAnswered = true;
							}
						}));
				}
				return DreamTest::EStep::Wait;
			}, 20.0);
			AddPrime(Steps, TEXT("unreal.StaticMeshActor."));
			Steps.Do(TEXT("members listed, getters never called"), [this, Fixture]()
			{
				const TSet<FString> Labels = DreamCompletionSpec::LabelsOf(*Fixture);
				TestTrue(TEXT("the fixture's getter-backed properties are listed"), Labels.Contains(TEXT("value0000")));
				if (Fixture->bIncomplete)
				{
					AddInfo(FString::Printf(TEXT("T-RES-024: the listing was partial within the 5 ms budget (%d names)"), Labels.Num()));
				}
				else
				{
					TestTrue(TEXT("a complete listing has the last property too"), Labels.Contains(TEXT("value0999")));
					TestTrue(TEXT("and its inherited functions"), Labels.Contains(TEXT("get_editor_property")));
				}
				FDreamCompletionAnswer Actor;
				DreamCompletionSpec::Service().QueryCached(DreamCompletionSpec::MakeQuery(TEXT("unreal.StaticMeshActor.")), Actor);
				const TSet<FString> ActorLabels = DreamCompletionSpec::LabelsOf(Actor);
				TestTrue(TEXT("StaticMeshActor lists its component property and inherited functions"),
					ActorLabels.Contains(TEXT("static_mesh_component")) && ActorLabels.Contains(TEXT("set_actor_location")));
				TestEqual(TEXT("no BlueprintGetter ran (counter 0, D6 E1)"), UDreamTestGetterFixture::GetGetterCalls(), static_cast<int64>(0));
				return true;
			});
			Steps.Run(Done);
		});
	});
}
