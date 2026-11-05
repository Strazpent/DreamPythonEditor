// SPDX-License-Identifier: Apache-2.0
// Dream.Keys.Focus: keys through the real Slate path keep working after a tab closes (TDD §4.9.1, §4.9.4, D8, SHL-02,
// T-VIEW-018 subset). Ctrl+W goes through FSlateApplication::ProcessKeyDownEvent to the focused code view, closes the tab,
// and keyboard focus moves to the next tab's code view instead of falling out of Dream (where the next key would bubble to
// the Level Editor: H hides actors, Ctrl+S saves the level). Then a typed letter lands in the shown tab and Ctrl+D, which
// only the consume list handles, is consumed in Dream; a click on the tab strip's x closes the next tab and a click on the
// active tab's label keeps focus in its code view (P2-shl-02, A2-03), both as mouse events routed through Slate at the
// widgets' geometry; with the REPL prompt focused, a click on the icon row's Clear history keeps the caret there, also after
// a click on the REPL segment of the output tab row took focus, so a typed letter lands in the prompt and not in the script
// (P3-shl-01, P4-shl-02; the case turns View ▸ REPL Tab and REPL Prompt on, PO-04, and restores them); Ctrl+W closes again. Then the empty state, always (P2-shl-06): the other open tabs are closed
// (a scratch tab with text stays reopenable, §10.2.5) and the empty Dream tab keeps focus, Ctrl+T opens a scratch tab, and
// the closed tabs come back afterwards (Reopen Closed Tab for scratch text, the path for files). Only a dirty file tab,
// which a close would discard, keeps the case from running; the spec then says so.
// Every step checks focus first and stops when it left Dream, so no chord ever reaches the Level Editor.
// The docked leak suite with the input bot is Dream.Keys.G5 (T-KEY-009..013).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Framework/Application/SlateApplication.h"
#include "Module/DreamEditorModule.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamTab.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Text/STextBlock.h"

namespace DreamKeyFocusSpec
{
	/** One key press as the platform layer delivers it: key-down, then key-up, with real modifier states. */
	bool Press(const FKey& Key, bool bCtrl, uint32 Character)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState Mods(false, false, bCtrl, false, false, false, false, false, false);
		const FKeyEvent Down(Key, Mods, App.GetUserIndexForKeyboard(), /*bInIsRepeat*/ false, Character, Character);
		const bool bHandled = App.ProcessKeyDownEvent(Down);
		const FKeyEvent Up(Key, Mods, App.GetUserIndexForKeyboard(), /*bInIsRepeat*/ false, Character, Character);
		App.ProcessKeyUpEvent(Up);
		return bHandled;
	}

	/** A typed character: key-down, the character event, key-up. */
	void Type(const FKey& Key, TCHAR Character)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FModifierKeysState NoMods;
		App.ProcessKeyDownEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, Character, Character));
		App.ProcessKeyCharEvent(FCharacterEvent(Character, NoMods, App.GetUserIndexForKeyboard(), false));
		App.ProcessKeyUpEvent(FKeyEvent(Key, NoMods, App.GetUserIndexForKeyboard(), false, Character, Character));
	}

	/**
	 * A left click at the centre of Widget: a move (hover), the button down and the button up, routed along the widget's path
	 * as Slate routes a platform click (FSlateApplication::Route*, input processing, the mouse-down focus fallback included).
	 * Process*Event would hit-test the window under the real OS cursor instead (LocateWindowUnderMouse). False when the
	 * widget is not in a visible window.
	 */
	bool Click(const TSharedRef<SWidget>& Widget)
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
		return true;
	}

	/** The widget that has keyboard focus is Root or inside it. */
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

	/** Depth-first: every widget under Root (Root included) for which Match is true, in child order. */
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

	/** The text blocks of the editor tab strip (SDreamEditorTabs) whose text satisfies Match, in strip order. */
	TArray<TSharedRef<SWidget>> StripTexts(SDreamTab& Tab, TFunctionRef<bool(const FString&)> Match)
	{
		TArray<TSharedRef<SWidget>> Strips;
		Collect(Tab.AsShared(), [](const TSharedRef<SWidget>& W) { return W->GetType() == FName(TEXT("SDreamEditorTabs")); }, Strips);
		TArray<TSharedRef<SWidget>> Texts;
		if (Strips.Num() == 1)
		{
			Collect(Strips[0], [&Match](const TSharedRef<SWidget>& W)
			{
				return W->GetType() == FName(TEXT("STextBlock")) && Match(StaticCastSharedRef<STextBlock>(W)->GetText().ToString());
			}, Texts);
		}
		return Texts;
	}

	/** The text blocks anywhere in the Dream tab whose text is exactly Text, in child order. */
	TArray<TSharedRef<SWidget>> TextsInTab(SDreamTab& Tab, const FString& Text)
	{
		TArray<TSharedRef<SWidget>> Texts;
		Collect(Tab.AsShared(), [&Text](const TSharedRef<SWidget>& W)
		{
			return W->GetType() == FName(TEXT("STextBlock")) && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == Text;
		}, Texts);
		return Texts;
	}

	/** (chrome) The widgets in the Dream tab tagged Tag (FTagMetaData: the icon row's "DreamChrome.<command>" buttons), in child order. */
	TArray<TSharedRef<SWidget>> TaggedInTab(SDreamTab& Tab, const TCHAR* Tag)
	{
		TArray<TSharedRef<SWidget>> Out;
		Collect(Tab.AsShared(), [Tag](const TSharedRef<SWidget>& W)
		{
			const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>();
			return Meta.IsValid() && Meta->Tag == FName(Tag);
		}, Out);
		return Out;
	}

	/** (chrome) The View toggles this spec turns on (PO-04 hides them by default), and their values before. */
	struct FViewSaved
	{
		bool bSaved = false;
		bool bReplTab = false;
		bool bReplPrompt = false;
	};

	/** The label of the output tab row's segment named Text (a text block inside the segment's SCheckBox), or null. */
	TSharedPtr<SWidget> SegmentLabel(SDreamTab& Tab, const FString& Text)
	{
		for (const TSharedRef<SWidget>& Label : TextsInTab(Tab, Text))
		{
			for (TSharedPtr<SWidget> Widget = Label->GetParentWidget(); Widget.IsValid() && Widget != Tab.AsShared(); Widget = Widget->GetParentWidget())
			{
				if (Widget->GetType() == FName(TEXT("SCheckBox")))
				{
					return Label;
				}
			}
		}
		return nullptr;
	}

	/** The strip's x button of the tab at Index of SDreamTab::GetTabBuffers (its "×" text block's parent), or null. */
	TSharedPtr<SWidget> CloseButtonOf(SDreamTab& Tab, int32 Index)
	{
		const TArray<TSharedRef<SWidget>> Crosses = StripTexts(Tab, [](const FString& Text) { return Text.Len() == 1 && Text[0] == TCHAR(0x00D7); });
		return Crosses.IsValidIndex(Index) ? Crosses[Index]->GetParentWidget() : nullptr;
	}

	/** A tab the empty-state step closed, to bring back afterwards. */
	struct FClosedTab
	{
		FGuid Id;
		FString Path;           // a file tab reopens by path
		bool bReopenable = false;
	};

	int32 IndexOfTab(SDreamTab& Tab, const FGuid& Id)
	{
		return Tab.GetTabBuffers().IndexOfByPredicate([&Id](const TSharedRef<FDreamBuffer>& B) { return B->GetId() == Id; });
	}
}

BEGIN_DEFINE_SPEC(FDreamKeyFocusSpec, "Dream.Keys.Focus", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	/** Focus is in the active code view; otherwise an error, and the caller stops before sending any key. */
	bool RequireFocusInActiveView(const TCHAR* When);
END_DEFINE_SPEC(FDreamKeyFocusSpec)

bool FDreamKeyFocusSpec::RequireFocusInActiveView(const TCHAR* When)
{
	const TSharedPtr<SDreamCodeView> View = Tab.IsValid() ? Tab->GetActiveCodeView() : nullptr;
	return TestTrue(*FString::Printf(TEXT("%s: keyboard focus is in the active code view"), When),
		View.IsValid() && DreamKeyFocusSpec::FocusIsInside(View.ToSharedRef()));
}

void FDreamKeyFocusSpec::Define()
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

	LatentIt("keeps keyboard focus in Dream Python Editor when Ctrl+W, the strip's x or a label click change tabs (SHL-02)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> First = DreamTest::OpenScratch(*this, *Tab, TEXT("first_tab = 1"), TEXT("FocusA"));
		const TSharedPtr<FDreamBuffer> Second = First.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("second_tab = 2"), TEXT("FocusB")) : nullptr;
		const TSharedPtr<FDreamBuffer> Third = Second.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("third_tab = 3"), TEXT("FocusC")) : nullptr;
		if (!Third.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(First->GetId());
		BufferIds.Add(Second->GetId());
		BufferIds.Add(Third->GetId());
		const FGuid FirstId = First->GetId();
		const FGuid SecondId = Second->GetId();
		const FGuid ThirdId = Third->GetId();

		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		TSharedRef<DreamKeyFocusSpec::FViewSaved> ViewSaved = MakeShared<DreamKeyFocusSpec::FViewSaved>();
		Steps.Do(TEXT("show the REPL tab and the REPL prompt (View toggles, PO-04)"), [ViewSaved]()
		{
			// (chrome) PO-04 hides the REPL tab and prompt by default; this case needs both.
			const UDreamUserSettings* Settings = UDreamUserSettings::Get();
			ViewSaved->bReplTab = Settings->bShowReplTab;
			ViewSaved->bReplPrompt = Settings->bShowReplPrompt;
			ViewSaved->bSaved = true;
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, true);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("focus the third tab's code view"), [this, ThirdId]()
		{
			Tab->ActivateBuffer(ThirdId);
			const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("the third tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return RequireFocusInActiveView(TEXT("before Ctrl+W"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Ctrl+W through Slate"), [this, ThirdId]()
		{
			if (!RequireFocusInActiveView(TEXT("Ctrl+W")))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+W was handled"), DreamKeyFocusSpec::Press(EKeys::W, /*bCtrl*/ true, 'w'));
			TestFalse(TEXT("the third tab is closed"), FDreamEditorModule::Get().GetBufferManager().FindById(ThirdId).IsValid());
			TestTrue(TEXT("the Dream tab itself is still open (CloseMajorTab never ran)"), FDreamEditorModule::Get().GetTab().IsValid());
			return RequireFocusInActiveView(TEXT("after Ctrl+W (SHL-02)"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("a letter is typed into the shown tab, and a key-down is consumed in Dream Python Editor"), [this]()
		{
			if (!RequireFocusInActiveView(TEXT("typing")))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			const int32 Before = Active.IsValid() ? Active->GetTextAsString().Len() : 0;
			DreamKeyFocusSpec::Type(EKeys::Q, TEXT('q'));
			TestTrue(TEXT("the typed q reached the shown tab's text"), Active.IsValid() && Active->GetTextAsString().Len() == Before + 1
				&& Active->GetTextAsString().Contains(TEXT("q"), ESearchCase::CaseSensitive));
			// Ctrl+D is on the consume list (D8) and Dream's DuplicateLine command (B71, EDT-04): Dream handles it and it never
			// reaches the Level Editor's Duplicate (T-VIEW-018). With the default keymap it duplicates the caret line; a user who
			// moved DuplicateLine off Ctrl+D leaves only the consume list, and then the text stays as it was.
			const TSharedPtr<FUICommandInfo> Duplicate = FDreamCommands::Get().GetInfo(EDreamCommand::DuplicateLine);
			bool bCtrlDDuplicates = false;
			for (int32 Slot = 0; Duplicate.IsValid() && Slot < static_cast<int32>(EMultipleKeyBindingIndex::NumChords); ++Slot)
			{
				const FInputChord& Chord = *Duplicate->GetActiveChord(static_cast<EMultipleKeyBindingIndex>(Slot));
				bCtrlDDuplicates |= Chord.Key == EKeys::D && Chord.bCtrl && !Chord.bShift && !Chord.bAlt && !Chord.bCmd;
			}
			const FString TextBefore = Active.IsValid() ? Active->GetTextAsString() : FString();
			TestTrue(TEXT("Ctrl+D's key-down is consumed in Dream Python Editor (consume list, T-VIEW-018)"), DreamKeyFocusSpec::Press(EKeys::D, /*bCtrl*/ true, 'd'));
			const FString TextAfter = Active.IsValid() ? Active->GetTextAsString() : FString();
			TArray<FString> LinesBefore;
			TArray<FString> LinesAfter;
			TextBefore.ParseIntoArrayLines(LinesBefore, false);
			TextAfter.ParseIntoArrayLines(LinesAfter, false);
			const bool bUnchanged = TextAfter == TextBefore;
			bool bDuplicated = LinesAfter.Num() == LinesBefore.Num() + 1 && LinesBefore.Num() > 0;
			if (bDuplicated)
			{
				// Removing one copy of a repeated line gives the old text back.
				bool bFound = false;
				for (int32 Index = 1; Index < LinesAfter.Num() && !bFound; ++Index)
				{
					if (LinesAfter[Index] == LinesAfter[Index - 1])
					{
						TArray<FString> Without = LinesAfter;
						Without.RemoveAt(Index);
						bFound = Without == LinesBefore;
					}
				}
				bDuplicated = bFound;
			}
			if (bCtrlDDuplicates)
			{
				TestTrue(*FString::Printf(TEXT("Ctrl+D duplicated the caret line (DuplicateLine, B71); was \"%s\", now \"%s\""),
					*TextBefore.ReplaceCharWithEscapedChar(), *TextAfter.ReplaceCharWithEscapedChar()), Active.IsValid() && bDuplicated);
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("Ctrl+D (not DuplicateLine's chord here) changed nothing; was \"%s\", now \"%s\""),
					*TextBefore.ReplaceCharWithEscapedChar(), *TextAfter.ReplaceCharWithEscapedChar()), Active.IsValid() && bUnchanged);
			}
			if (bDuplicated)
			{
				// The next steps compare texts: keep the buffer as it was.
				if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
				{
					View->SetTextNoUndo(TextBefore);
				}
			}
			return RequireFocusInActiveView(TEXT("after typing"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("close the shown tab with the strip's x"), [this, SecondId]()
		{
			if (!RequireFocusInActiveView(TEXT("the x click")))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			if (!TestTrue(TEXT("the second tab is the shown one"), Active.IsValid() && Active->GetId() == SecondId))
			{
				return false;
			}
			const TSharedPtr<SWidget> Close = DreamKeyFocusSpec::CloseButtonOf(*Tab, DreamKeyFocusSpec::IndexOfTab(*Tab, SecondId));
			if (!TestTrue(TEXT("the second tab's x button is in the strip"), Close.IsValid()))
			{
				return false;
			}
			if (!TestTrue(TEXT("the x button is in a visible window"), DreamKeyFocusSpec::Click(Close.ToSharedRef())))
			{
				return false;
			}
			TestFalse(TEXT("the x closed the second tab"), FDreamEditorModule::Get().GetBufferManager().FindById(SecondId).IsValid());
			return RequireFocusInActiveView(TEXT("after the x click (P2-shl-02)"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("click the active tab's label"), [this, FirstId]()
		{
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			if (!RequireFocusInActiveView(TEXT("the label click"))
				|| !TestTrue(TEXT("the first tab is the shown one"), Active.IsValid() && Active->GetId() == FirstId))
			{
				return false;
			}
			const FString Name = Active->GetDisplayName();
			const TArray<TSharedRef<SWidget>> Labels = DreamKeyFocusSpec::StripTexts(*Tab, [&Name](const FString& Text)
			{
				return Text == Name || Text == Name + TEXT(" *");
			});
			if (!TestEqual(TEXT("the first tab's label is in the strip once"), Labels.Num(), 1))
			{
				return false;
			}
			if (!TestTrue(TEXT("the label is in a visible window"), DreamKeyFocusSpec::Click(Labels[0])))
			{
				return false;
			}
			return RequireFocusInActiveView(TEXT("after a click on the active tab's label (A2-03)"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("a click on the icon row's Clear history keeps the caret in the REPL prompt"), [this]()
		{
			const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetReplStrip() : nullptr;
			const TSharedPtr<SDreamCodeView> Prompt = Strip.IsValid() ? Strip->GetPromptView() : nullptr;
			if (!TestTrue(TEXT("the REPL strip has a prompt"), Prompt.IsValid()))
			{
				return false;
			}
			Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);
			if (!TestTrue(TEXT("Focus REPL puts keyboard focus in the prompt"), DreamKeyFocusSpec::FocusIsInside(Prompt.ToSharedRef())))
			{
				return false;
			}
			// (chrome) The icon row's Clear history button, by its tag (PO-04: icons without labels).
			const TArray<TSharedRef<SWidget>> Clear = DreamKeyFocusSpec::TaggedInTab(*Tab, TEXT("DreamChrome.ClearOutput"));
			if (!TestEqual(TEXT("the icon row's Clear history button is in the tab once"), Clear.Num(), 1)
				|| !TestTrue(TEXT("Clear is in a visible window"), DreamKeyFocusSpec::Click(Clear[0])))
			{
				return false;
			}
			// The click focuses SDreamTab (the button takes no focus), which gives it back to the prompt; a key now must go there.
			if (!TestTrue(TEXT("keyboard focus is still in the REPL prompt after the click (P3-shl-01)"), DreamKeyFocusSpec::FocusIsInside(Prompt.ToSharedRef())))
			{
				return false;
			}
			// A segment of the output tab row takes focus on its click (SCheckBox). It is no focus home: the next click on
			// chrome still gives focus back to the prompt, not to the segment, where typed keys go nowhere (P4-shl-02).
			const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
			const EDreamOutputTab TabBefore = Area->GetActiveTab();
			const TSharedPtr<SWidget> Segment = DreamKeyFocusSpec::SegmentLabel(*Tab, TEXT("REPL"));
			if (!TestTrue(TEXT("the REPL segment's label is in the tab row"), Segment.IsValid())
				|| !TestTrue(TEXT("the segment is in a visible window"), DreamKeyFocusSpec::Click(Segment.ToSharedRef())))
			{
				return false;
			}
			const bool bSegmentTookFocus = !DreamKeyFocusSpec::FocusIsInside(Prompt.ToSharedRef());
			const bool bClearClicked = DreamKeyFocusSpec::Click(Clear[0]);
			Area->SetActiveTab(TabBefore);
			if (!TestTrue(TEXT("the segment click took focus from the prompt (the case under test)"), bSegmentTookFocus)
				|| !TestTrue(TEXT("Clear is in a visible window after the segment click"), bClearClicked)
				|| !TestTrue(TEXT("keyboard focus is back in the REPL prompt after the next click on chrome (P4-shl-02)"),
					DreamKeyFocusSpec::FocusIsInside(Prompt.ToSharedRef())))
			{
				return false;
			}
			const TSharedRef<FDreamBuffer> Repl = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer();
			const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
			const FString PromptBefore = Repl->GetTextAsString();
			const FString ScriptBefore = Active.IsValid() ? Active->GetTextAsString() : FString();
			DreamKeyFocusSpec::Type(EKeys::Q, TEXT('q'));
			const FString PromptAfter = Repl->GetTextAsString();
			Repl->ResetText(PromptBefore, dream::EditOrigin::Load);     // the prompt as it was
			TestTrue(TEXT("the typed q went into the prompt"), PromptAfter.Len() == PromptBefore.Len() + 1 && PromptAfter.Contains(TEXT("q"), ESearchCase::CaseSensitive));
			TestTrue(TEXT("and not into the script"), Active.IsValid() && Active->GetTextAsString() == ScriptBefore);
			if (const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView())
			{
				View->FocusEditor();
			}
			return RequireFocusInActiveView(TEXT("back in the script after the REPL step"));
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Ctrl+W again"), [this, FirstId]()
		{
			if (!RequireFocusInActiveView(TEXT("the second Ctrl+W")))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+W closes the next tab too (the keymap still applies)"), DreamKeyFocusSpec::Press(EKeys::W, /*bCtrl*/ true, 'w'));
			TestFalse(TEXT("the first tab is closed"), FDreamEditorModule::Get().GetBufferManager().FindById(FirstId).IsValid());
			TestTrue(TEXT("focus stays inside the Dream tab"), DreamKeyFocusSpec::FocusIsInside(Tab.ToSharedRef()));
			return true;
		});
		Steps.WaitFrames(2);
		TSharedRef<TArray<DreamKeyFocusSpec::FClosedTab>> Closed = MakeShared<TArray<DreamKeyFocusSpec::FClosedTab>>();
		TSharedRef<bool> bSkipped = MakeShared<bool>(false);
		Steps.Do(TEXT("reach the empty state: close the other tabs with Ctrl+W (P2-shl-06)"), [this, Closed, bSkipped]()
		{
			const TArray<TSharedRef<FDreamBuffer>> Others = Tab->GetTabBuffers();
			for (const TSharedRef<FDreamBuffer>& Other : Others)
			{
				// (scratch, PO-05, p1-m6) A scratch file whose auto-save is armed saves itself on close, so it is closed like a
				// scratch tab; one whose auto-save is paused (a bar is up) is a dirty file like any other.
				const bool bSavesItself = Other->IsScratchFile() && FDreamEditorModule::Get().GetBufferManager().IsScratchAutoSaveArmed(*Other);
				if (Other->GetKind() != EDreamBufferKind::Scratch && !bSavesItself && Other->IsDirty())
				{
					AddInfo(FString::Printf(TEXT("the dirty file tab %s is open: closing it would discard its edits, so the empty state is skipped"),
						*Other->GetDisplayName()));
					*bSkipped = true;
					return true;
				}
			}
			// Closed as a user would, with Ctrl+W from the focused code view, so the last close leaves focus where SDreamTab puts it.
			for (int32 Guard = 0; Tab->GetTabBuffers().Num() > 0 && Guard < 64; ++Guard)
			{
				const TSharedPtr<FDreamBuffer> Active = Tab->GetActiveBuffer();
				const TSharedPtr<SDreamCodeView> View = Tab->GetActiveCodeView();
				if (!Active.IsValid() || !View.IsValid())
				{
					break;
				}
				if (!DreamKeyFocusSpec::FocusIsInside(View.ToSharedRef()))
				{
					View->FocusEditor();
				}
				if (!RequireFocusInActiveView(TEXT("closing the other tabs")))
				{
					return false;
				}
				DreamKeyFocusSpec::FClosedTab Entry;
				Entry.Id = Active->GetId();
				// (scratch, p1-m6) A scratch file comes back with Reopen Closed Tab too: its ID and its text, not only the file's.
				Entry.Path = Active->GetKind() == EDreamBufferKind::Scratch || Active->IsScratchFile() ? FString() : Active->GetPath();
				Entry.bReopenable = Entry.Path.IsEmpty();
				DreamKeyFocusSpec::Press(EKeys::W, /*bCtrl*/ true, 'w');
				if (FDreamEditorModule::Get().GetBufferManager().FindById(Entry.Id).IsValid())
				{
					AddError(FString::Printf(TEXT("Ctrl+W did not close %s"), *Active->GetDisplayName()));
					return false;
				}
				Closed->Add(Entry);
			}
			return TestEqual(TEXT("no tab is left open"), Tab->GetTabBuffers().Num(), 0);
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the empty state keeps focus and Ctrl+T opens a scratch tab"), [this, bSkipped]()
		{
			if (*bSkipped)
			{
				return true;
			}
			// The empty state: the tab itself holds focus and routes keys; Ctrl+T, which its hint offers, opens a scratch tab.
			if (!TestTrue(TEXT("the empty Dream tab holds keyboard focus"), FSlateApplication::Get().GetKeyboardFocusedWidget() == Tab))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+T is handled in the empty state"), DreamKeyFocusSpec::Press(EKeys::T, /*bCtrl*/ true, 't'));
			const TSharedPtr<FDreamBuffer> Opened = Tab->GetActiveBuffer();
			// (scratch, PO-05) Ctrl+T makes a scratch file (scratch_N.py), or the journaled Untitled-N when the folder is not writable.
			if (TestTrue(TEXT("Ctrl+T opened a scratch tab"), Opened.IsValid() && (Opened->IsScratchFile() || Opened->GetKind() == EDreamBufferKind::Scratch)))
			{
				BufferIds.Add(Opened->GetId());
			}
			return true;
		});
		Steps.Finally(TEXT("restore the View toggles"), [ViewSaved]()   // (chrome)
		{
			if (ViewSaved->bSaved && UObjectInitialized())
			{
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, ViewSaved->bReplTab);
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, ViewSaved->bReplPrompt);
			}
		});
		Steps.Finally(TEXT("bring the closed tabs back"), [Closed]()
		{
			if (!FDreamEditorModule::IsAvailable())
			{
				return;
			}
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			for (const DreamKeyFocusSpec::FClosedTab& Entry : *Closed)
			{
				if (!Entry.Path.IsEmpty())
				{
					FText Error;
					Buffers.OpenFile(Entry.Path, Error);
				}
				else if (Entry.bReopenable)
				{
					Buffers.ReopenClosed(Entry.Id);
				}
			}
		});
		Steps.Run(Done);
	});
}
