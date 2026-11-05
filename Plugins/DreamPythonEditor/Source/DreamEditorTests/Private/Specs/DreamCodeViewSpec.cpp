// SPDX-License-Identifier: Apache-2.0
// Dream.CodeView.*: the plain-mode code view of slice B (TDD §4.2, §4.4, §4.5, §4.8.3, §4.9, §7.9.3; EDT-03..EDT-05,
// EDT-08..EDT-10, NAV-03, CFG-08). Keys go through FSlateApplication::Process*Event to the focused code view, as the
// platform layer delivers them; Ctrl+wheel is routed along the text widget's path. Every step checks that keyboard focus is
// still in Dream before it sends a key, so no chord ever reaches the Level Editor.
//   Dream.CodeView.Undo       Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z on dream::UndoTree: typing groups (R2), the stock undo never
//                            runs, an edit group and a whole-text replace are one step each, a buffer without a view, the
//                            REPL prompt's own undo, and the history across a save (EDT-08, D8, §4.15 row (3)).
//   Dream.CodeView.SmartEdit  Enter indents after ':' and dedents after return, Backspace to the indent stop, auto-close
//                            pairs and type-over, Ctrl+D duplicate line; each one undo step (EDT-03, EDT-04, EDT-05).
//   Dream.CodeView.GoToLine   Ctrl+G: line, line:column, past the end, an invalid input, Esc (NAV-03 M1, §7.9.3).
//   Dream.CodeView.Zoom       Ctrl+= / Ctrl+- / Ctrl+0 and Ctrl+wheel in 10% steps within 50%..300%, and the Font size
//                            setting applied to open views at once (EDT-10).
//   Dream.CodeView.TabFocus   Ctrl+M: Tab indents, then moves keyboard focus out of the view, then indents again (CFG-08).
//   Dream.CodeView.Find       Ctrl+F / Ctrl+H: live count, case, regex with groups, F3 / Shift+F3, Replace, Replace All as one
//                            undo step, Esc (EDT-09 M1, §4.4).
//   Dream.CodeView.WholeText  a whole-text replace (SetTextNoUndo, a replace of more than 64 ranges, its undo) followed by
//                            Up / End / PageDown / Home in the same frame finds laid-out lines (no stale line views).
//   Dream.CodeView.FindWorker a pathological regex searches on a worker (no frame blocks; closing the bar cancels it), and a
//                            regex Replace All is built on a worker and applied as one undo step, and regex Replace
//                            one by one selects each next match from the worker's matches of the edited text (§4.4 Run).
//   Dream.CodeView.StickyColumn a caret placed before the view's first paint (a restored session, a link that opens at a
//                            column) keeps its column on Down (§4.5 sticky X).
//   Dream.CodeView.ReadOnlyKeys every editing chord (typing, Backspace, Delete, Enter, Tab, Ctrl+D, Ctrl+X, Ctrl+V,
//                            Ctrl+Backspace, ...) changes nothing on a read-only buffer and while a run pumps (A27, RT-01).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "CodeView/SDreamFindBar.h"
#include "CodeView/SDreamGoToLine.h"
#include "CodeView/DreamUndoHistory.h"
#include "Commands/DreamKeyRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Run/RunSupervisor.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SEditableTextBox.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // the clipboard format check (IsPlainTextClipboard)
#endif

namespace DreamCodeViewSpec
{
	FModifierKeysState Mods(bool bCtrl, bool bShift = false, bool bAlt = false)
	{
		return FModifierKeysState(bShift, false, bCtrl, false, bAlt, false, false, false, false);
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

	bool Chord(const FKey& Key, bool bCtrl, bool bShift = false, bool bAlt = false)
	{
		return Press(Key, Mods(bCtrl, bShift, bAlt));
	}

	/** Typed text, one character event each (no modifiers): what OnKeyChar sees for ordinary typing. */
	void TypeText(const FString& Text)
	{
		FSlateApplication& App = FSlateApplication::Get();
		for (const TCHAR Ch : Text)
		{
			App.ProcessKeyCharEvent(FCharacterEvent(Ch, FModifierKeysState(), App.GetUserIndexForKeyboard(), false));
		}
	}

	void Enter() { Press(EKeys::Enter, FModifierKeysState(), TCHAR(13)); }
	void Backspace() { Press(EKeys::BackSpace, FModifierKeysState(), TCHAR(8)); }
	void PressTab() { Press(EKeys::Tab, FModifierKeysState(), TCHAR(9)); }

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

	/** The first widget under Root (Root included) whose type is TypeName, depth first. */
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

	/** A wheel notch (or Delta notches) over the code view's text, routed along its path as Slate routes a platform wheel. */
	bool Wheel(SDreamCodeView& View, float Delta, bool bCtrl)
	{
		const TSharedPtr<SWidget> Text = FindByType(View.AsShared(), FName(TEXT("SDreamPlainText")));
		FWidgetPath Path;
		if (!Text.IsValid() || !DreamTest::MakePointerPath(Text.ToSharedRef(), Path))
		{
			return false;
		}
		FSlateApplication& App = FSlateApplication::Get();
		const FVector2D At(Text->GetPaintSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
		const FPointerEvent Event(App.GetUserIndexForMouse(), FSlateApplicationBase::CursorPointerIndex, At, At, TSet<FKey>(),
			EKeys::MouseWheelAxis, Delta, Mods(bCtrl));
		App.RouteMouseWheelOrGestureEvent(Path, Event, nullptr);
		return true;
	}

	FString Text(const TSharedPtr<FDreamBuffer>& Buffer)
	{
		return Buffer.IsValid() ? Buffer->GetTextAsString() : FString();
	}

	/** Empties and closes the buffers a spec opened, so nothing of them waits in Reopen Closed Tab. */
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

// =====================================================================================================================
// Dream.CodeView.Undo
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewUndoSpec, "Dream.CodeView.Undo", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString TempFile;
	bool RequireFocus(const TSharedPtr<SDreamCodeView>& View, const TCHAR* When)
	{
		return TestTrue(*FString::Printf(TEXT("%s: keyboard focus is in the code view"), When),
			View.IsValid() && DreamCodeViewSpec::FocusIsInside(View.ToSharedRef()));
	}
END_DEFINE_SPEC(FDreamCodeViewUndoSpec)

void FDreamCodeViewUndoSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		if (!TempFile.IsEmpty())
		{
			IFileManager::Get().Delete(*TempFile, false, true, true);
			TempFile.Reset();
		}
		Tab.Reset();
	});

	LatentIt("drives Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z from UndoTree and never runs the stock undo (EDT-08, D8)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, FString(), TEXT("Undo")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("typing groups: a word, then a space and a word (R2)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!RequireFocus(View, TEXT("typing")))
			{
				return false;
			}
			TypeText(TEXT("abc def"));
			TestEqual(TEXT("the typed text reached the model"), Text(Buffer), FString(TEXT("abc def")));
			TestTrue(TEXT("Ctrl+Z is consumed"), Chord(EKeys::Z, true));
			TestEqual(TEXT("one Ctrl+Z undoes the second word with its space"), Text(Buffer), FString(TEXT("abc")));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("the second Ctrl+Z undoes the first word"), Text(Buffer), FString());
			TestTrue(TEXT("Ctrl+Z with nothing to undo is still consumed (D8)"), Chord(EKeys::Z, true));
			TestTrue(TEXT("Ctrl+Y is consumed"), Chord(EKeys::Y, true));
			TestEqual(TEXT("Ctrl+Y redoes the first word"), Text(Buffer), FString(TEXT("abc")));
			TestTrue(TEXT("Ctrl+Shift+Z is consumed"), Chord(EKeys::Z, true, true));
			TestEqual(TEXT("Ctrl+Shift+Z redoes the second word"), Text(Buffer), FString(TEXT("abc def")));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the caret follows the redo"), Column, 7);
			// The stock widget's own undo chords find nothing to undo (its states are dropped after every edit).
			Press(EKeys::BackSpace, Mods(false, false, true));
			TestEqual(TEXT("Alt+Backspace (the stock undo) changes nothing"), Text(Buffer), FString(TEXT("abc def")));
			return RequireFocus(View, TEXT("after the undo keys"));
		});
		Steps.Do(TEXT("an edit group and a whole-text replace are one undo step each (§4.2 R5)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!RequireFocus(View, TEXT("the edit group")))
			{
				return false;
			}
			FDreamEditGroup Group;
			Group.Label = NSLOCTEXT("DreamTests", "CodeView.TestGroup", "Test group");
			FDreamTextEdit& First = Group.Edits.AddDefaulted_GetRef();
			First.StartLine = 0;
			First.StartColumn = 0;
			First.EndLine = 0;
			First.EndColumn = 3;
			First.Text = TEXT("x");
			FDreamTextEdit& Second = Group.Edits.AddDefaulted_GetRef();
			Second.StartLine = 0;
			Second.StartColumn = 7;
			Second.EndLine = 0;
			Second.EndColumn = 7;
			Second.Text = TEXT("!\nnext");
			TestTrue(TEXT("ApplyEditGroup applies"), View->ApplyEditGroup(Group));
			TestEqual(TEXT("both ranges changed"), Text(Buffer), FString(TEXT("x def!\nnext")));
			FDreamEditGroup Bad;
			FDreamTextEdit& Out = Bad.Edits.AddDefaulted_GetRef();
			Out.StartLine = 5;
			Out.EndLine = 5;
			TestFalse(TEXT("a range outside the text is refused"), View->ApplyEditGroup(Bad));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z undoes the whole group"), Text(Buffer), FString(TEXT("abc def")));
			Chord(EKeys::Y, true);
			TestEqual(TEXT("one Ctrl+Y redoes it"), Text(Buffer), FString(TEXT("x def!\nnext")));
			TestTrue(TEXT("ReplaceAllText applies"), View->ReplaceAllText(TEXT("x def!\nchanged\nmore"),
				NSLOCTEXT("DreamTests", "CodeView.TestReload", "Reloaded from disk"), dream::EditOrigin::Reload));
			TestEqual(TEXT("the text is replaced"), Text(Buffer), FString(TEXT("x def!\nchanged\nmore")));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z undoes the replace"), Text(Buffer), FString(TEXT("x def!\nnext")));
			TestEqual(TEXT("the widget follows the undo of a multi-line replace"), View->GetShownText(), Text(Buffer));
			// ReplaceLines (toggle comment, indent, Restore) is one step too.
			View->ReplaceLines(0, 1, TEXT("# x def!\n# next"));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z undoes ReplaceLines"), Text(Buffer), FString(TEXT("x def!\nnext")));
			// A range over two lines that ends at the end of a line that is not the last (a stock delete quirk, SDreamPlainText).
			View->SetTextNoUndo(TEXT("a\nb\nc"));
			FDreamEditGroup Join;
			Join.Label = NSLOCTEXT("DreamTests", "CodeView.TestJoin", "Test join");
			FDreamTextEdit& JoinEdit = Join.Edits.AddDefaulted_GetRef();
			JoinEdit.StartLine = 0;
			JoinEdit.StartColumn = 1;
			JoinEdit.EndLine = 1;
			JoinEdit.EndColumn = 1;
			TestTrue(TEXT("the two-line delete applies"), View->ApplyEditGroup(Join));
			TestEqual(TEXT("the two-line delete removed '\\nb'"), Text(Buffer), FString(TEXT("a\nc")));
			TestEqual(TEXT("the widget shows it too"), View->GetShownText(), Text(Buffer));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("Ctrl+Z gives the line back"), Text(Buffer), FString(TEXT("a\nb\nc")));
			TestEqual(TEXT("the widget shows the line again"), View->GetShownText(), Text(Buffer));
			// A load starts a new root (§4.2): nothing to undo after it.
			View->SetTextNoUndo(TEXT("loaded"));
			TestFalse(TEXT("SetTextNoUndo leaves nothing to undo"), View->CanUndo());
			Chord(EKeys::Z, true);
			TestEqual(TEXT("Ctrl+Z after a load changes nothing"), Text(Buffer), FString(TEXT("loaded")));
			return RequireFocus(View, TEXT("after the edit group"));
		});
		Steps.Do(TEXT("a buffer without a code view takes an edit group too (a background tab's reload)"), [this]()
		{
			FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
			const TSharedRef<FDreamBuffer> Hidden = Buffers.CreateScratch(TEXT("one\ntwo"), DreamTest::ScratchName(TEXT("UndoHidden")));
			BufferIds.Add(Hidden->GetId());
			TestFalse(TEXT("the new buffer has no code view (views are made on activation)"), Tab->FindCodeView(Hidden->GetId()).IsValid());
			FDreamEditGroup Group;
			Group.Label = NSLOCTEXT("DreamTests", "CodeView.TestHidden", "Reloaded from disk");
			Group.Origin = dream::EditOrigin::Reload;
			FDreamTextEdit& Edit = Group.Edits.AddDefaulted_GetRef();
			Edit.StartLine = 1;
			Edit.StartColumn = 0;
			Edit.EndLine = 1;
			Edit.EndColumn = 3;
			Edit.Text = TEXT("2");
			TestTrue(TEXT("FDreamUndoHistory::ApplyEditGroup applies without a view"), FDreamUndoHistory::ApplyEditGroup(*Hidden, Group));
			TestEqual(TEXT("the hidden buffer changed"), Hidden->GetTextAsString(), FString(TEXT("one\n2")));
			const TSharedPtr<FDreamUndoHistory> History = FDreamUndoHistory::Find(*Hidden);
			if (!TestTrue(TEXT("the buffer has a history now"), History.IsValid()))
			{
				return false;
			}
			TestTrue(TEXT("the history can undo it"), History->CanUndo() && History->Undo());
			TestEqual(TEXT("the undo gives the text back"), Hidden->GetTextAsString(), FString(TEXT("one\ntwo")));
			return true;
		});
		Steps.Do(TEXT("Ctrl+Z in the REPL prompt undoes the prompt, not the script"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamReplStrip> Strip = Tab->GetOutputArea().IsValid() ? Tab->GetOutputArea()->GetReplStrip() : nullptr;
			const TSharedPtr<SDreamCodeView> Prompt = Strip.IsValid() ? Strip->GetPromptView() : nullptr;
			if (!TestTrue(TEXT("the REPL strip has a prompt"), Prompt.IsValid()))
			{
				return false;
			}
			const TSharedRef<FDreamBuffer> Repl = FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer();
			const FString PromptBefore = Repl->GetTextAsString();
			Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);
			if (!RequireFocus(Prompt, TEXT("the REPL prompt")))
			{
				return false;
			}
			const FString ScriptBefore = Text(Buffer);
			// The prompt may hold text from an earlier spec: type at its end.
			Prompt->SetCaret(Prompt->GetLineCount() - 1, MAX_int32);
			TypeText(TEXT("zz"));
			const FString Typed = Repl->GetTextAsString();
			Chord(EKeys::Z, true);
			const FString Undone = Repl->GetTextAsString();
			Repl->ResetText(PromptBefore, dream::EditOrigin::Load);   // the prompt as it was
			TestEqual(TEXT("the prompt got the typed text"), Typed, PromptBefore + TEXT("zz"));
			TestEqual(TEXT("Ctrl+Z in the prompt undid the prompt's typing"), Undone, PromptBefore);
			TestEqual(TEXT("the script is unchanged"), Text(Buffer), ScriptBefore);
			if (const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id))
			{
				View->FocusEditor();
			}
			return true;
		});
		Steps.Do(TEXT("the history survives a save; the save point follows it"), [this]()
		{
			TempFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamT4") /
				FString::Printf(TEXT("undo_%s.py"), *DreamTest::UniqueSuffix()));
			if (!TestTrue(TEXT("the test file is written"), FFileHelper::SaveStringToFile(FString(TEXT("a = 1\n")), *TempFile)))
			{
				return false;
			}
			if (!TestTrue(TEXT("the file opens in a tab"), Tab->OpenLocation(TempFile, 1, 1)))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> File = Tab->GetActiveBuffer();
			if (!TestTrue(TEXT("the file is the active buffer"), File.IsValid() && File->HasPath()))
			{
				return false;
			}
			BufferIds.Add(File->GetId());
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(File->GetId());
			if (!RequireFocus(View, TEXT("the file tab")))
			{
				return false;
			}
			TypeText(TEXT("x"));
			TestEqual(TEXT("typed into the file"), Text(File), FString(TEXT("xa = 1\n")));
			TestTrue(TEXT("the file is dirty"), File->IsDirty());
			Tab->ExecuteDreamCommand(EDreamCommand::Save);
			return true;
		});
		Steps.Poll(TEXT("the save finishes"), [this]()
		{
			const TSharedPtr<FDreamBuffer> File = Tab->GetActiveBuffer();
			return File.IsValid() && !File->IsDirty() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("Ctrl+Z after the save"), [this]()
		{
			const TSharedPtr<FDreamBuffer> File = Tab->GetActiveBuffer();
			const TSharedPtr<SDreamCodeView> View = File.IsValid() ? Tab->FindCodeView(File->GetId()) : nullptr;
			if (!RequireFocus(View, TEXT("after the save")))
			{
				return false;
			}
			const TSharedPtr<FDreamUndoHistory> History = View->GetUndoHistory();
			TestTrue(TEXT("the save marked the save point (R9)"), History.IsValid() && History->GetTree().IsAtSavePoint());
			Chord(EKeys::Z, true);
			TestEqual(TEXT("Ctrl+Z still undoes the typing after the save (EDT-08)"), Text(File), FString(TEXT("a = 1\n")));
			TestTrue(TEXT("the undone text is dirty"), File->IsDirty());
			TestFalse(TEXT("and away from the save point"), History->GetTree().IsAtSavePoint());
			Chord(EKeys::Y, true);
			TestEqual(TEXT("Ctrl+Y gives the saved text back"), Text(File), FString(TEXT("xa = 1\n")));
			TestFalse(TEXT("the saved text is clean again"), File->IsDirty());
			TestTrue(TEXT("at the save point again"), History->GetTree().IsAtSavePoint());
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.SmartEdit
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewSmartEditSpec, "Dream.CodeView.SmartEdit", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewSmartEditSpec)

void FDreamCodeViewSmartEditSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("indents on Enter, backspaces to the stop, pairs brackets and duplicates lines, each one undo step (EDT-03, EDT-04, EDT-05)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("def f():"), TEXT("Smart")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(0, 8);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Enter after ':' indents, Enter after return dedents (EDT-03)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			Enter();
			TestEqual(TEXT("def f():⏎ indents 4 spaces"), Text(Buffer), FString(TEXT("def f():\n    ")));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the caret is after the indentation"), Line == 1 && Column == 4);
			TypeText(TEXT("return 1"));
			Enter();
			TestEqual(TEXT("the line after return is dedented"), Text(Buffer), FString(TEXT("def f():\n    return 1\n")));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z removes the newline with its indentation (R3)"), Text(Buffer), FString(TEXT("def f():\n    return 1")));
			return true;
		});
		Steps.Do(TEXT("Backspace deletes to the indent stop (EDT-03)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			View->SetTextNoUndo(TEXT("if x:\n        y"));
			View->SetCaret(1, 8);
			Backspace();
			TestEqual(TEXT("eight spaces lose four"), Text(Buffer), FString(TEXT("if x:\n    y")));
			Backspace();
			TestEqual(TEXT("then the last four"), Text(Buffer), FString(TEXT("if x:\ny")));
			View->SetCaret(1, 1);
			Backspace();
			TestEqual(TEXT("outside the indentation one character goes"), Text(Buffer), FString(TEXT("if x:\n")));
			return true;
		});
		Steps.Do(TEXT("auto-close pairs and type-over (EDT-05)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			View->SetTextNoUndo(FString());
			TypeText(TEXT("print("));
			TestEqual(TEXT("'(' inserts the pair"), Text(Buffer), FString(TEXT("print()")));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the caret is between the pair"), Column, 6);
			TypeText(TEXT("'a"));
			TestEqual(TEXT("a quote pairs too"), Text(Buffer), FString(TEXT("print('a')")));
			TypeText(TEXT("')"));
			TestEqual(TEXT("typing the closers steps over them"), Text(Buffer), FString(TEXT("print('a')")));
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the caret is after the closing bracket"), Column, 10);
			TypeText(TEXT(" # ("));
			TestEqual(TEXT("nothing pairs in a comment"), Text(Buffer), FString(TEXT("print('a') # (")));
			View->SetTextNoUndo(FString());
			TypeText(TEXT("("));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z removes the whole pair (R5)"), Text(Buffer), FString());
			TypeText(TEXT("["));
			Backspace();
			TestEqual(TEXT("Backspace in an empty pair deletes both"), Text(Buffer), FString());
			TypeText(TEXT("f(("));
			TestEqual(TEXT("nested pairs"), Text(Buffer), FString(TEXT("f(())")));
			TypeText(TEXT("))"));
			TestEqual(TEXT("both closers are typed over"), Text(Buffer), FString(TEXT("f(())")));
			View->GetCaret(Line, Column);
			TestEqual(TEXT("the caret is after both"), Column, 5);
			return true;
		});
		Steps.Do(TEXT("Ctrl+D duplicates the line or the selection (EDT-04)"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("abc\nxyz"));
			View->SetCaret(0, 1);
			TestTrue(TEXT("Ctrl+D is consumed"), Chord(EKeys::D, true));
			TestEqual(TEXT("the caret line is copied below"), Text(Buffer), FString(TEXT("abc\nabc\nxyz")));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the caret moves to the copy"), Line == 1 && Column == 1);
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z removes the copy"), Text(Buffer), FString(TEXT("abc\nxyz")));
			// The undo removes a range that ends at the end of a line that is not the last one (a stock delete quirk).
			TestEqual(TEXT("the widget shows the model's text after the undo"), View->GetShownText(), Text(Buffer));
			FDreamTextSelection Selection;
			Selection.AnchorLine = 1;
			Selection.AnchorColumn = 0;
			Selection.CaretLine = 1;
			Selection.CaretColumn = 2;
			View->SetSelection(Selection);
			Chord(EKeys::D, true);
			TestEqual(TEXT("a selection is inserted again after itself"), Text(Buffer), FString(TEXT("abc\nxyxyz")));
			TestEqual(TEXT("the widget shows the model's text"), View->GetShownText(), Text(Buffer));
			return true;
		});
		Steps.Do(TEXT("Delete, Backspace and typing replace a selection over two lines as one step"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			// The selection ends at the end of a line that is not the last one: the stock delete would join "a" and "c".
			FDreamTextSelection Selection;
			Selection.AnchorLine = 0;
			Selection.AnchorColumn = 1;
			Selection.CaretLine = 1;
			Selection.CaretColumn = 1;
			View->SetTextNoUndo(TEXT("a\nb\nc"));
			View->SetSelection(Selection);
			Press(EKeys::Delete, FModifierKeysState());
			TestEqual(TEXT("Delete removes the selection only"), Text(Buffer), FString(TEXT("a\nc")));
			TestEqual(TEXT("the widget shows it"), View->GetShownText(), Text(Buffer));
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z brings it back"), Text(Buffer), FString(TEXT("a\nb\nc")));
			View->SetSelection(Selection);
			Backspace();
			TestEqual(TEXT("Backspace removes the selection only"), Text(Buffer), FString(TEXT("a\nc")));
			View->SetTextNoUndo(TEXT("a\nb\nc"));
			View->SetSelection(Selection);
			TypeText(TEXT("X"));
			TestEqual(TEXT("typing replaces the selection"), Text(Buffer), FString(TEXT("aX\nc")));
			TestEqual(TEXT("the widget shows the typed text"), View->GetShownText(), Text(Buffer));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the caret is after the typed character"), Line == 0 && Column == 2);
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.GoToLine
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewGoToLineSpec, "Dream.CodeView.GoToLine", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewGoToLineSpec)

void FDreamCodeViewGoToLineSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	It("parses line, line:column and the palette's ':' prefix (the Go To line provider)", [this]()
	{
		int32 Line = 0;
		int32 Column = 0;
		FText Error;
		TestTrue(TEXT("\"7\""), SDreamGoToLine::Parse(TEXT("7"), 10, Line, Column, Error) && Line == 6 && Column == INDEX_NONE);
		TestTrue(TEXT("\"3:5\""), SDreamGoToLine::Parse(TEXT(" 3:5 "), 10, Line, Column, Error) && Line == 2 && Column == 4);
		TestTrue(TEXT("\":4\""), SDreamGoToLine::Parse(TEXT(":4"), 10, Line, Column, Error) && Line == 3);
		TestTrue(TEXT("past the end goes to the last line"), SDreamGoToLine::Parse(TEXT("99"), 10, Line, Column, Error) && Line == 9);
		TestFalse(TEXT("\"0\" is refused"), SDreamGoToLine::Parse(TEXT("0"), 10, Line, Column, Error));
		TestFalse(TEXT("\"x\" is refused"), SDreamGoToLine::Parse(TEXT("x"), 10, Line, Column, Error));
		TestFalse(TEXT("\"2:\" is refused"), SDreamGoToLine::Parse(TEXT("2:"), 10, Line, Column, Error));
		TestFalse(TEXT("an empty input is refused"), SDreamGoToLine::Parse(TEXT(" "), 10, Line, Column, Error));
	});

	LatentIt("jumps on Ctrl+G and Enter, and gives focus back on Esc (NAV-03)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid()
			? DreamTest::OpenScratch(*this, *Tab, TEXT("a = 1\n    b = 2\nc = 3\nd = 4\ne = 5"), TEXT("GoTo")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Ctrl+G, 2, Enter"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+G is consumed"), Chord(EKeys::G, true));
			TestTrue(TEXT("the Go to line box is open"), View->IsGoToLineOpen());
			const TSharedPtr<SEditableTextBox> Box = View->GetGoToLine().IsValid() ? View->GetGoToLine()->GetInputBox() : nullptr;
			if (!TestTrue(TEXT("its input box has keyboard focus"), Box.IsValid() && FocusIsInside(Box.ToSharedRef())))
			{
				return false;
			}
			Box->SetText(FText::FromString(TEXT("2")));
			Press(EKeys::Enter, FModifierKeysState(), TCHAR(13));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the caret is on line 2 at its first non-blank column"), Line == 1 && Column == 4);
			TestFalse(TEXT("the box closed"), View->IsGoToLineOpen());
			return TestTrue(TEXT("focus is back in the code view"), View->HasEditorFocus());
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("line:column, past the end, an invalid input and Esc"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && View->HasEditorFocus()))
			{
				return false;
			}
			const TSharedPtr<SEditableTextBox> Box = View->GetGoToLine()->GetInputBox();
			Chord(EKeys::G, true);
			Box->SetText(FText::FromString(TEXT("4:3")));
			Press(EKeys::Enter, FModifierKeysState(), TCHAR(13));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("4:3 is line 4, column 3"), Line == 3 && Column == 2);
			Chord(EKeys::G, true);
			Box->SetText(FText::FromString(TEXT("99")));
			Press(EKeys::Enter, FModifierKeysState(), TCHAR(13));
			View->GetCaret(Line, Column);
			TestEqual(TEXT("past the end goes to the last line"), Line, 4);
			Chord(EKeys::G, true);
			Box->SetText(FText::FromString(TEXT("abc")));
			Press(EKeys::Enter, FModifierKeysState(), TCHAR(13));
			TestTrue(TEXT("an invalid input keeps the box open"), View->IsGoToLineOpen());
			Press(EKeys::Escape, FModifierKeysState(), TCHAR(27));
			TestFalse(TEXT("Esc closes it"), View->IsGoToLineOpen());
			int32 LineAfter = 0;
			View->GetCaret(LineAfter, Column);
			TestEqual(TEXT("Esc leaves the caret where it was"), LineAfter, 4);
			return TestTrue(TEXT("focus is back in the code view after Esc"), View->HasEditorFocus());
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.Zoom
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewZoomSpec, "Dream.CodeView.Zoom", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	int32 SavedFontSize = 10;
	void SetFontSetting(int32 Points)
	{
		UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
		Settings->FontSize = Points;
		FPropertyChangedEvent Event(FindFProperty<FProperty>(UDreamUserSettings::StaticClass(), GET_MEMBER_NAME_CHECKED(UDreamUserSettings, FontSize)));
		Settings->PostEditChangeProperty(Event);   // what Editor Preferences sends (no config write)
	}
END_DEFINE_SPEC(FDreamCodeViewZoomSpec)

void FDreamCodeViewZoomSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		SavedFontSize = UDreamUserSettings::Get()->FontSize;
	});
	AfterEach([this]()
	{
		if (UDreamUserSettings::Get()->FontSize != SavedFontSize)
		{
			SetFontSetting(SavedFontSize);   // the user-global settings keep their values (tier-4 hygiene)
		}
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("zooms in 10% steps between 50% and 300% from the keys and Ctrl+wheel, and follows the Font size setting (EDT-10)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("zoom = 1"), TEXT("Zoom")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the zoom keys and Ctrl+wheel"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			const int32 Setting = UDreamUserSettings::Get()->FontSize;
			TestEqual(TEXT("a new tab starts at 100%"), View->GetZoomPercent(), 100);
			TestEqual(TEXT("100% is the Font size setting"), View->GetFontSize(), Setting);
			TestTrue(TEXT("Ctrl+= is consumed"), Chord(EKeys::Equals, true));
			TestEqual(TEXT("Ctrl+= zooms to 110%"), View->GetZoomPercent(), 110);
			Chord(EKeys::Hyphen, true);
			TestEqual(TEXT("Ctrl+- zooms back to 100%"), View->GetZoomPercent(), 100);
			if (!TestTrue(TEXT("the wheel event reached the text"), Wheel(*View, 1.f, true)))
			{
				return false;
			}
			TestEqual(TEXT("Ctrl+wheel up zooms in one step"), View->GetZoomPercent(), 110);
			Wheel(*View, -2.f, true);
			TestEqual(TEXT("two notches down zoom out two steps"), View->GetZoomPercent(), 90);
			Wheel(*View, -1.f, false);
			TestEqual(TEXT("the wheel without Ctrl does not zoom"), View->GetZoomPercent(), 90);
			Chord(EKeys::Zero, true);
			TestEqual(TEXT("Ctrl+0 resets to 100%"), View->GetZoomPercent(), 100);
			for (int32 Step = 0; Step < 30; ++Step)
			{
				Chord(EKeys::Equals, true);
			}
			TestEqual(TEXT("the zoom stops at 300%"), View->GetZoomPercent(), 300);
			TestEqual(TEXT("300% is three times the setting"), View->GetFontSize(), FMath::Min(Setting * 3, 160));
			for (int32 Step = 0; Step < 30; ++Step)
			{
				Chord(EKeys::Hyphen, true);
			}
			TestEqual(TEXT("the zoom stops at 50%"), View->GetZoomPercent(), 50);
			TestEqual(TEXT("the zoom is the tab's view state (saved with the session)"), Tab->GetActiveBuffer()->GetViewState().ZoomPercent, 50);
			Chord(EKeys::Zero, true);
			return true;
		});
		Steps.Do(TEXT("the Font size setting applies to the open view at once"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			Chord(EKeys::Equals, true);
			Chord(EKeys::Equals, true);
			TestEqual(TEXT("zoomed to 120%"), View->GetZoomPercent(), 120);
			SetFontSetting(SavedFontSize + 10);
			TestEqual(TEXT("the view follows the new setting and keeps its zoom"), View->GetFontSize(),
				FMath::RoundToInt((SavedFontSize + 10) * 1.2f));
			SetFontSetting(SavedFontSize);
			TestEqual(TEXT("and the setting back"), View->GetFontSize(), FMath::RoundToInt(SavedFontSize * 1.2f));
			Chord(EKeys::Zero, true);
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.TabFocus
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewTabFocusSpec, "Dream.CodeView.TabFocus", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewTabFocusSpec)

void FDreamCodeViewTabFocusSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		FDreamKeyRouter::Get().SetTabMovesFocus(false);
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("toggles with Ctrl+M whether Tab indents or moves focus (CFG-08)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("x"), TEXT("TabFocus")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
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
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Tab indents; Ctrl+M; Tab moves focus"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			PressTab();
			TestEqual(TEXT("with the toggle off, Tab indents"), Text(Buffer), FString(TEXT("    x")));
			TestTrue(TEXT("and focus stays in the view"), View->HasEditorFocus());
			TestTrue(TEXT("Ctrl+M is consumed"), Chord(EKeys::M, true));
			TestTrue(TEXT("Ctrl+M turned Tab moves focus on"), FDreamKeyRouter::Get().IsTabMovesFocus());
			PressTab();
			TestEqual(TEXT("with the toggle on, Tab does not indent"), Text(Buffer), FString(TEXT("    x")));
			TestFalse(TEXT("and keyboard focus left the code view"), FocusIsInside(View.ToSharedRef()));
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("Ctrl+M again: Tab indents again"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is back in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			Chord(EKeys::M, true);
			TestFalse(TEXT("Ctrl+M turned it off"), FDreamKeyRouter::Get().IsTabMovesFocus());
			View->SetCaret(0, 4);
			PressTab();
			TestEqual(TEXT("Tab indents again"), Text(Buffer), FString(TEXT("        x")));
			return TestTrue(TEXT("focus stays in the view"), View->HasEditorFocus());
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.Find
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewFindSpec, "Dream.CodeView.Find", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewFindSpec)

void FDreamCodeViewFindSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("finds with a live count, case, whole word and regex, steps with F3, replaces as one undo step (EDT-09)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const FString Original = TEXT("alpha beta\nAlpha gamma\nalpha_beta = 1\n");
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, Original, TEXT("Find")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view and select a word"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view with a find bar"), View.IsValid() && View->HasFindBar()))
			{
				return false;
			}
			View->FocusEditor();
			FDreamTextSelection Word;
			Word.CaretColumn = 5;
			View->SetSelection(Word);
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Ctrl+F seeds the query from the selection; case and whole word"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+F is consumed"), Chord(EKeys::F, true));
			const TSharedPtr<SDreamFindBar> Bar = View->GetFindBar();
			if (!TestTrue(TEXT("the find bar is open"), View->IsFindBarOpen() && Bar.IsValid()))
			{
				return false;
			}
			TestTrue(TEXT("its find box has keyboard focus"), Bar->GetFindBox().IsValid() && FocusIsInside(Bar->GetFindBox().ToSharedRef()));
			TestEqual(TEXT("the selection became the query"), Bar->GetFindText(), FString(TEXT("alpha")));
			View->UpdateFindNow();
			TestEqual(TEXT("three matches ignoring case"), View->GetFindMatches().Num(), 3);
			TestEqual(TEXT("the selected word is match 1"), View->GetCurrentFindMatch(), 0);
			TestEqual(TEXT("the live count"), View->GetFindStatus().ToString(), FString(TEXT("1 of 3")));
			Bar->SetMatchCase(true);
			View->UpdateFindNow();
			TestEqual(TEXT("two with Match case"), View->GetFindMatches().Num(), 2);
			Bar->SetWholeWord(true);
			View->UpdateFindNow();
			TestEqual(TEXT("one with Whole word too (alpha_beta is one word)"), View->GetFindMatches().Num(), 1);
			Bar->SetWholeWord(false);
			Bar->SetMatchCase(false);
			View->UpdateFindNow();
			TestEqual(TEXT("three again"), View->GetFindMatches().Num(), 3);
			return true;
		});
		Steps.Do(TEXT("F3 and Shift+F3 step through the matches from the find box"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			const TSharedPtr<SDreamFindBar> Bar = View->GetFindBar();
			if (!TestTrue(TEXT("focus is in the find box"), FocusIsInside(Bar->GetFindBox().ToSharedRef())))
			{
				return false;
			}
			TestTrue(TEXT("F3 is consumed"), Chord(EKeys::F3, false));
			TestEqual(TEXT("F3 selects match 2"), View->GetCurrentFindMatch(), 1);
			FDreamTextSelection Selection = View->GetSelection();
			TestTrue(TEXT("match 2 is 'Alpha' on line 2"), Selection.AnchorLine == 1 && Selection.AnchorColumn == 0 && Selection.CaretColumn == 5);
			Chord(EKeys::F3, false);
			Chord(EKeys::F3, false);
			TestEqual(TEXT("F3 wraps to match 1"), View->GetCurrentFindMatch(), 0);
			Chord(EKeys::F3, false, true);
			TestEqual(TEXT("Shift+F3 wraps back to match 3"), View->GetCurrentFindMatch(), 2);
			Bar->SetFindText(TEXT("gam"));
			TestEqual(TEXT("the count follows typing at once (literal)"), View->GetFindMatches().Num(), 1);
			TestEqual(TEXT("and the incremental find selected it"), View->GetCurrentFindMatch(), 0);
			Bar->SetFindText(TEXT("("));
			Bar->SetRegex(true);
			View->UpdateFindNow();
			TestFalse(TEXT("a bad regex shows an error"), View->GetFindStatus().IsEmpty() || View->GetFindMatches().Num() > 0);
			return true;
		});
		Steps.Do(TEXT("regex Replace All with a group is one undo step"), [this, Id, Buffer, Original]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			const TSharedPtr<SDreamFindBar> Bar = View->GetFindBar();
			Bar->SetFindText(TEXT("(\\w+)a(\\b)"));
			Bar->SetReplaceText(TEXT("\\1A"));
			View->UpdateFindNow();
			TestEqual(TEXT("the regex matches alpha, beta, Alpha, gamma, alpha_beta"), View->GetFindMatches().Num(), 5);
			View->ReplaceAll();
			TestTrue(TEXT("a regex Replace All runs on a worker"), View->IsFindBusy());
			View->UpdateFindNow();   // waits for the worker (specs only)
			TestEqual(TEXT("Replace All with the group"), Text(Buffer), FString(TEXT("alphA betA\nAlphA gammA\nalpha_betA = 1\n")));
			View->FocusEditor();
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z undoes Replace All"), Text(Buffer), Original);
			return true;
		});
		Steps.Do(TEXT("Replace replaces the selected match and selects the next; Esc closes"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View->HasEditorFocus()))
			{
				return false;
			}
			TestTrue(TEXT("Ctrl+H is consumed"), Chord(EKeys::H, true));
			const TSharedPtr<SDreamFindBar> Bar = View->GetFindBar();
			TestTrue(TEXT("Ctrl+H shows the replace row"), Bar->IsReplaceShown());
			Bar->SetRegex(false);
			Bar->SetFindText(TEXT("beta"));
			Bar->SetReplaceText(TEXT("delta"));
			View->UpdateFindNow();
			View->ReplaceCurrent();
			TestEqual(TEXT("the first Replace replaced the selected match"), Text(Buffer), FString(TEXT("alpha delta\nAlpha gamma\nalpha_beta = 1\n")));
			TestEqual(TEXT("and the next match is selected"), View->GetCurrentFindMatch(), 0);
			TestEqual(TEXT("one match is left"), View->GetFindMatches().Num(), 1);
			Bar->FocusFindBox();
			Press(EKeys::Escape, FModifierKeysState(), TCHAR(27));
			TestFalse(TEXT("Esc closes the bar"), View->IsFindBarOpen());
			return TestTrue(TEXT("and gives focus back to the text"), View->HasEditorFocus());
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.WholeText
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewWholeTextSpec, "Dream.CodeView.WholeText", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewWholeTextSpec)

void FDreamCodeViewWholeTextSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("moves the caret right after a whole-text replace in the same frame, before any layout pass", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		FString Long;
		for (int32 Line = 0; Line < 40; ++Line)
		{
			Long += FString::Printf(TEXT("line_%d = %d\n"), Line, Line);
		}
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, Long, TEXT("WholeText")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the code view, caret low in the long text"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(30, 4);
			return true;
		});
		Steps.WaitFrames(2);   // painted: the stock line views are those of the long text
		Steps.Do(TEXT("SetTextNoUndo, then Up, End, PageDown, Home and Down in the same frame"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			View->SetTextNoUndo(TEXT("a = 1\nb = 2\nc = 3"));
			// Through FSlateApplication's key path, as Windows delivers keys queued during a slow frame: no tick in between.
			Press(EKeys::Up, FModifierKeysState());
			Press(EKeys::End, FModifierKeysState());
			Press(EKeys::PageDown, FModifierKeysState());
			Press(EKeys::Home, FModifierKeysState());
			Press(EKeys::Down, FModifierKeysState());
			TestEqual(TEXT("the keys changed no text"), Text(Buffer), FString(TEXT("a = 1\nb = 2\nc = 3")));
			TestEqual(TEXT("the widget shows the new text"), View->GetShownText(), Text(Buffer));
			int32 Line = 0;
			int32 Column = 0;
			View->GetCaret(Line, Column);
			TestTrue(TEXT("the caret is inside the new text"), Line >= 0 && Line < 3 && Column >= 0 && Column <= 5);
			return true;
		});
		Steps.Do(TEXT("a replace with more than 64 ranges (set whole), then Up and End in the same frame"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			FString Before;
			FString After;
			for (int32 Line = 0; Line < 200; ++Line)
			{
				Before += FString::Printf(TEXT("x_%d = 0\n"), Line);
				After += Line % 2 == 0 ? FString::Printf(TEXT("x_%d = 0\n"), Line) : FString::Printf(TEXT("y_%d = 1\n"), Line);
			}
			View->SetTextNoUndo(Before);
			View->SetCaret(150, 2);
			TestTrue(TEXT("ReplaceAllText applies"), View->ReplaceAllText(After, NSLOCTEXT("DreamTests", "CodeView.TestWhole", "Reloaded from disk"),
				dream::EditOrigin::Reload));
			Press(EKeys::Up, FModifierKeysState());
			Press(EKeys::End, FModifierKeysState());
			TestEqual(TEXT("the replaced text is the model"), Text(Buffer), After);
			TestEqual(TEXT("the widget shows it"), View->GetShownText(), Text(Buffer));
			Chord(EKeys::Z, true);
			Press(EKeys::Down, FModifierKeysState());
			Press(EKeys::Home, FModifierKeysState());
			TestEqual(TEXT("one Ctrl+Z gives the text back"), Text(Buffer), Before);
			TestEqual(TEXT("the widget shows the undone text"), View->GetShownText(), Text(Buffer));
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.FindWorker
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewFindWorkerSpec, "Dream.CodeView.FindWorker", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
END_DEFINE_SPEC(FDreamCodeViewFindWorkerSpec)

void FDreamCodeViewFindWorkerSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		if (Tab.IsValid())
		{
			for (const FGuid& Id : BufferIds)
			{
				if (const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id))
				{
					View->CloseFindBar();
				}
			}
		}
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("runs a pathological regex on a worker: no frame blocks, and closing the bar cancels it (TDD 4-4 Run)",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		// About 7 ms per line at SearchEngine's step limit: tens of seconds for the whole search on one thread.
		FString Lines;
		for (int32 Line = 0; Line < 3000; ++Line)
		{
			Lines += TEXT("    return some_function_call_with_long_name(argument_one)\n");
		}
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, Lines, TEXT("FindWorker")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		struct FFrames
		{
			double Last = 0.0;
			double MaxGap = 0.0;
			double Start = 0.0;
			bool bSawBusy = false;
			int32 WorkersBefore = 0;
			int32 SteadyFrames = 0;
		};
		const TSharedRef<FFrames> Frames = MakeShared<FFrames>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		// A freshly started editor has long frames of its own (shaders, assets, the Python boot): measure only once the
		// frames are steady, so a long frame means the search blocked the game thread.
		Steps.Poll(TEXT("the editor runs steady frames (warm-up)"), [Frames]()
		{
			const double Now = FPlatformTime::Seconds();
			Frames->SteadyFrames = Frames->Last > 0.0 && Now - Frames->Last < 0.06 ? Frames->SteadyFrames + 1 : 0;
			Frames->Last = Now;
			return Frames->SteadyFrames >= 10 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 60.0);
		Steps.Do(TEXT("type the regex into the find bar"), [this, Id, Frames]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view with a find bar"), View.IsValid() && View->HasFindBar()))
			{
				return false;
			}
			Frames->WorkersBefore = SDreamCodeView::GetFindWorkersRunning();
			View->FocusEditor();
			View->OpenFindBar(false);
			View->GetFindBar()->SetRegex(true);
			View->GetFindBar()->SetFindText(TEXT("(\\w+\\s*)*="));
			Frames->Start = Frames->Last = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("frames keep coming while the worker searches (1.5 s)"), [this, Id, Frames]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			const double Now = FPlatformTime::Seconds();
			Frames->MaxGap = FMath::Max(Frames->MaxGap, Now - Frames->Last);
			Frames->Last = Now;
			Frames->bSawBusy |= View.IsValid() && View->IsFindBusy();
			return Now - Frames->Start < 1.5 ? DreamTest::EStep::Wait : DreamTest::EStep::Next;
		}, 10.0);
		Steps.Do(TEXT("the search ran on a worker without blocking a frame"), [this, Id, Frames]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the view is there"), View.IsValid()))
			{
				return false;
			}
			AddInfo(FString::Printf(TEXT("longest frame gap while the worker searched: %.1f ms"), Frames->MaxGap * 1000.0));
			TestTrue(TEXT("the search went to a worker"), Frames->bSawBusy);
			TestTrue(TEXT("it is still searching (tens of seconds of work)"), View->IsFindBusy()
				&& SDreamCodeView::GetFindWorkersRunning() > Frames->WorkersBefore);
			TestEqual(TEXT("the bar says so"), View->GetFindStatus().ToString(), FString(TEXT("Searching…")));
			TestTrue(TEXT("no frame was blocked for long (one line at the step limit costs about 7 ms)"), Frames->MaxGap < 0.25);
			View->CloseFindBar();
			TestFalse(TEXT("closing the bar forgets the worker"), View->IsFindBusy());
			Frames->Start = FPlatformTime::Seconds();
			return true;
		});
		Steps.Poll(TEXT("closing the bar cancelled the worker"), [this, Frames]()
		{
			if (SDreamCodeView::GetFindWorkersRunning() <= Frames->WorkersBefore)
			{
				AddInfo(FString::Printf(TEXT("the worker stopped %.0f ms after the bar closed"), (FPlatformTime::Seconds() - Frames->Start) * 1000.0));
				return DreamTest::EStep::Next;
			}
			return DreamTest::EStep::Wait;
		}, 3.0);
		Steps.Run(Done);
	});

	LatentIt("replaces regex matches one by one in order when the replacement is shorter than the match", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("x1 x22 x333"), TEXT("ReplaceOne")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("Ctrl+H with the regex x(\\d+) and the replacement \\1"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view with a find bar"), View.IsValid() && View->HasFindBar()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(0, 0);
			View->OpenFindBar(true);
			View->GetFindBar()->SetRegex(true);
			View->GetFindBar()->SetFindText(TEXT("x(\\d+)"));
			View->GetFindBar()->SetReplaceText(TEXT("\\1"));
			return true;
		});
		// Each Replace replaces the selected match; the worker's matches of the edited text then select the next one.
		const TCHAR* const Expected[] = { TEXT("x1"), TEXT("x22"), TEXT("x333") };
		const TCHAR* const TextAfter[] = { TEXT("1 x22 x333"), TEXT("1 22 x333"), TEXT("1 22 333") };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FString Want = Expected[Index];
			const FString After = TextAfter[Index];
			Steps.Poll(FString::Printf(TEXT("the worker's matches select %s"), *Want), [this, Id, Want]()
			{
				const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
				return View.IsValid() && !View->IsFindBusy() && View->GetSelectedText() == Want ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
			}, 10.0);
			Steps.Do(FString::Printf(TEXT("Replace %s"), *Want), [this, Id, Buffer, After]()
			{
				const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
				View->ReplaceCurrent();
				TestEqual(TEXT("the selected match was replaced"), Text(Buffer), After);
				return true;
			});
		}
		Steps.Poll(TEXT("no match is left"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			return View.IsValid() && !View->IsFindBusy() ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("every match was replaced, in order"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			TestEqual(TEXT("the text"), Text(Buffer), FString(TEXT("1 22 333")));
			TestEqual(TEXT("no match is left"), View->GetFindMatches().Num(), 0);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("builds a regex Replace All on a worker and applies it as one undo step", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		FString Before;
		FString Expected;
		for (int32 Line = 0; Line < 2000; ++Line)
		{
			Before += FString::Printf(TEXT("item_%d = %d\n"), Line, Line);
			Expected += FString::Printf(TEXT("thing_%d = %d\n"), Line, Line);
		}
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, Before, TEXT("ReplaceWorker")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("open the replace row with a regex and a group"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view with a find bar"), View.IsValid() && View->HasFindBar()))
			{
				return false;
			}
			View->FocusEditor();
			View->OpenFindBar(true);
			View->GetFindBar()->SetRegex(true);
			View->GetFindBar()->SetFindText(TEXT("item_(\\d+)"));
			View->GetFindBar()->SetReplaceText(TEXT("thing_\\1"));
			return true;
		});
		Steps.Poll(TEXT("the worker's matches arrive"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			return View.IsValid() && !View->IsFindBusy() && View->GetFindMatches().Num() == 2000 ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("Replace All"), [this, Id, Buffer, Before]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			View->ReplaceAll();
			TestTrue(TEXT("Replace All went to a worker"), View->IsFindBusy());
			TestEqual(TEXT("the bar says so"), View->GetFindStatus().ToString(), FString(TEXT("Replacing…")));
			TestEqual(TEXT("nothing is replaced before the worker's batch arrives"), Text(Buffer), Before);
			return true;
		});
		Steps.Poll(TEXT("the batch arrives and applies"), [this, Buffer, Expected]()
		{
			return Text(Buffer) == Expected ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 10.0);
		Steps.Do(TEXT("one Ctrl+Z undoes all of it"), [this, Id, Buffer, Before, Expected]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			TestEqual(TEXT("every item was replaced"), Text(Buffer), Expected);
			TestEqual(TEXT("the widget shows it"), View->GetShownText(), Text(Buffer));
			View->FocusEditor();
			if (!TestTrue(TEXT("focus is in the text"), View->HasEditorFocus()))
			{
				return false;
			}
			Chord(EKeys::Z, true);
			TestEqual(TEXT("one Ctrl+Z gives the text back"), Text(Buffer), Before);
			return true;
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.ReadOnlyKeys
// =====================================================================================================================

namespace DreamCodeViewSpec
{
	/** One editing chord as the platform delivers it (key-down, the character event when Char is set, key-up). */
	struct FEditChord
	{
		FKey Key;
		bool bCtrl = false;
		bool bShift = false;
		TCHAR Char = 0;
		const TCHAR* Name = TEXT("");
	};

	/** Every key that edits text in a code view: typing, the smart edits (B71), the selection paths, clipboard and undo. */
	TArray<FEditChord> EditingChords()
	{
		return {
			{ EKeys::X, false, false, TEXT('x'), TEXT("typing x") },
			{ EKeys::Nine, false, true, TEXT('('), TEXT("( (auto-close)") },
			{ EKeys::BackSpace, false, false, TCHAR(8), TEXT("Backspace") },
			{ EKeys::BackSpace, false, true, TCHAR(8), TEXT("Shift+Backspace") },
			{ EKeys::Delete, false, false, 0, TEXT("Delete") },
			{ EKeys::Enter, false, false, TCHAR(13), TEXT("Enter") },
			{ EKeys::Enter, false, true, TCHAR(13), TEXT("Shift+Enter") },
			{ EKeys::Tab, false, false, TCHAR(9), TEXT("Tab") },
			{ EKeys::Tab, false, true, TCHAR(9), TEXT("Shift+Tab") },
			{ EKeys::D, true, false, 0, TEXT("Ctrl+D") },
			{ EKeys::X, true, false, 0, TEXT("Ctrl+X") },
			{ EKeys::V, true, false, 0, TEXT("Ctrl+V") },
			{ EKeys::BackSpace, true, false, TCHAR(127), TEXT("Ctrl+Backspace") },
			{ EKeys::Delete, true, false, 0, TEXT("Ctrl+Delete") },
			{ EKeys::Delete, false, true, 0, TEXT("Shift+Delete") },
			{ EKeys::Insert, false, true, 0, TEXT("Shift+Insert") },
			{ EKeys::Slash, true, false, 0, TEXT("Ctrl+/") },
			{ EKeys::Z, true, false, 0, TEXT("Ctrl+Z") },
			{ EKeys::Y, true, false, 0, TEXT("Ctrl+Y") },
		};
	}

	/** FApp::HasFocus as when another application is in front (the Run window then opens without taking focus). */
	bool AppNeverHasFocus()
	{
		return false;
	}

	/** Sends every editing chord, each over Selection (two lines, ending at the end of a line that is not the last) and again
	 *  with a plain caret; returns how many reached the view with keyboard focus inside it. */
	int32 SendEditingChords(const TSharedRef<SDreamCodeView>& View, const FDreamTextSelection& Selection)
	{
		int32 Reached = 0;
		for (const FEditChord& Chord : EditingChords())
		{
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				if (Pass == 0)
				{
					View->SetSelection(Selection);
				}
				else
				{
					View->SetCaret(Selection.CaretLine, Selection.CaretColumn);
				}
				Reached += FocusIsInside(View) ? 1 : 0;
				Press(Chord.Key, Mods(Chord.bCtrl, Chord.bShift), Chord.Char);
			}
		}
		return Reached;
	}
}

namespace DreamCodeViewSpec
{
	/**
	 * True when the clipboard is empty or holds only text: FPlatformApplicationMisc saves and restores text only, so a
	 * clipboard with an image, files or rich text is left alone (Ctrl+V then pastes nothing the stock widget takes).
	 */
	bool IsPlainTextClipboard()
	{
#if PLATFORM_WINDOWS
		// Without opening the clipboard (an open would compete with the engine's own clipboard calls): every format on it
		// must be one of the text formats Windows synthesises from each other.
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
}

BEGIN_DEFINE_SPEC(FDreamCodeViewReadOnlyKeysSpec, "Dream.CodeView.ReadOnlyKeys", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString SavedClipboard;
	bool bClipboardSaved = false;
END_DEFINE_SPEC(FDreamCodeViewReadOnlyKeysSpec)

void FDreamCodeViewReadOnlyKeysSpec::Define()
{
	BeforeEach([this]()
	{
		Tab = DreamTest::RequireTab(*this);
		bClipboardSaved = DreamCodeViewSpec::IsPlainTextClipboard();
		if (bClipboardSaved)
		{
			FPlatformApplicationMisc::ClipboardPaste(SavedClipboard);
			FPlatformApplicationMisc::ClipboardCopy(TEXT("PASTED\nTEXT"));   // so Ctrl+V and Shift+Insert would change the text
		}
		else
		{
			AddInfo(TEXT("the clipboard holds more than text: it is left alone, so Ctrl+V and Shift+Insert paste whatever it holds"));
		}
	});
	AfterEach([this]()
	{
		if (bClipboardSaved)
		{
			FPlatformApplicationMisc::ClipboardCopy(*SavedClipboard);
			bClipboardSaved = false;
		}
		DreamCodeViewSpec::CloseAll(BufferIds);
		Tab.Reset();
	});

	LatentIt("refuses every editing chord on a read-only buffer (A27)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		const TSharedPtr<FDreamBuffer> Buffer = Tab.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("first\nsecond\nthird"), TEXT("ReadOnlyKeys")) : nullptr;
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus, type (so Ctrl+Z has a step), make the buffer read-only"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the scratch tab has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(2, 5);
			TypeText(TEXT("y"));
			Buffer->SetReadOnly(true, NSLOCTEXT("DreamTests", "CodeView.TestReadOnly", "Read-only for a test"));
			return TestTrue(TEXT("the view is read-only"), View->IsReadOnly());
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("every editing chord, over a two-line selection and at a caret"), [this, Id, Buffer]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			const FString Before = Text(Buffer);
			const int32 TakeBacksBefore = SDreamCodeView::GetReadOnlyTakeBacks();
			FDreamTextSelection Selection;
			Selection.AnchorLine = 0;
			Selection.AnchorColumn = 2;
			Selection.CaretLine = 1;
			Selection.CaretColumn = 6;
			const int32 Reached = SendEditingChords(View.ToSharedRef(), Selection);
			TestEqual(TEXT("the stock widget edited nothing (no edit had to be taken back)"), SDreamCodeView::GetReadOnlyTakeBacks(), TakeBacksBefore);
			TestEqual(TEXT("every chord reached the focused view"), Reached, EditingChords().Num() * 2);
			TestEqual(TEXT("the read-only buffer is unchanged"), Text(Buffer), Before);
			TestEqual(TEXT("the widget shows the unchanged text"), View->GetShownText(), Before);
			return true;
		});
		Steps.Finally(TEXT("writable again"), [Buffer]()
		{
			Buffer->SetReadOnly(false, FText::GetEmpty());
		});
		Steps.Run(Done);
	});

	LatentIt("refuses every editing chord while a run pumps (RT-01)", DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		using namespace DreamCodeViewSpec;
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Loop = DreamTest::OpenScratch(*this, *Tab, TEXT("import dream_editor\nfor _ in range(300): dream_editor.sleep(0.01)"), TEXT("KeysRun"));
		const TSharedPtr<FDreamBuffer> Target = Loop.IsValid() ? DreamTest::OpenScratch(*this, *Tab, TEXT("first\nsecond\nthird"), TEXT("KeysTarget")) : nullptr;
		if (!Loop.IsValid() || !Target.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Loop->GetId());
		BufferIds.Add(Target->GetId());
		const FGuid TargetId = Target->GetId();
		struct FState
		{
			FString TargetBefore;
			FString LoopBefore;
			bool bSent = false;
			bool bReadOnlyAtSend = false;
			int32 Reached = 0;
			int32 TakeBacks = 0;
			bool bSavedRunPump = false;
			EDreamRunPresentationSetting SavedPresentation = EDreamRunPresentationSetting::RunWindow;
			bool bSettingsChanged = false;   // Finally restores only what the submit step changed
			bool bFocusSeam = false;         // FApp's focus query is replaced until the Run window has opened
			FString FocusAtSend;
			FDelegateHandle PreTick;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		// The user's run settings, captured before any step can fail (the user-global section must keep its values).
		State->bSavedRunPump = UDreamUserSettings::Get()->bRunPump;
		State->SavedPresentation = UDreamUserSettings::Get()->RunPresentation;
		const TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("focus the target, type (so Ctrl+Z has a step)"), [this, TargetId]()
		{
			Tab->ActivateBuffer(TargetId);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(TargetId);
			if (!TestTrue(TEXT("the target has a code view"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			View->SetCaret(2, 5);
			TypeText(TEXT("y"));
			return true;
		});
		Steps.WaitFrames(1);
		Steps.Do(TEXT("submit the pumping run; the chords go in from its pump"), [this, TargetId, Target, Loop, State, Run]()
		{
			State->TargetBefore = Text(Target);
			State->LoopBefore = Text(Loop);
			UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
			State->bSettingsChanged = true;
			Settings->bRunPump = true;
			Settings->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			// With the editor in front, the Run window takes keyboard focus as it opens (RT-05, SDreamRunWindow::OpenModal), and
			// FSlateApplication::SetUserFocus refuses every widget outside the active modal window, so the target cannot be
			// focused from inside the pump. The Run window is opened as it opens when another application is in front
			// (FApp::HasFocus false: no activation, no focus), so keyboard focus stays in the target view and the chords reach
			// it through FSlateApplication's key path: the read-only defence must hold for a key that reaches a view during a run.
			FApp::SetHasFocusFunction(&DreamCodeViewSpec::AppNeverHasFocus);
			State->bFocusSeam = true;
			const TWeakPtr<SDreamTab> WeakTab = Tab;
			State->PreTick = FSlateApplication::Get().OnPreTick().AddLambda([WeakTab, TargetId, State](float)
			{
				const TSharedPtr<SDreamTab> PinnedTab = WeakTab.Pin();
				const FRunSupervisor& Supervisor = FDreamEditorModule::Get().GetSupervisor();
				if (State->bSent || !PinnedTab.IsValid() || Supervisor.GetPumpDepth() <= 0 || !Supervisor.IsRunWindowOpen())
				{
					return;
				}
				if (State->bFocusSeam)
				{
					FApp::SetHasFocusFunction(&FPlatformApplicationMisc::IsThisApplicationForeground);   // the Run window is open
					State->bFocusSeam = false;
				}
				const TSharedPtr<SDreamCodeView> View = PinnedTab->FindCodeView(TargetId);
				if (!View.IsValid())
				{
					return;
				}
				State->bSent = true;
				State->bReadOnlyAtSend = View->IsReadOnly();
				const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
				State->FocusAtSend = Focused.IsValid() ? Focused->GetTypeAsString() : FString(TEXT("nothing"));
				FDreamTextSelection Selection;
				Selection.AnchorLine = 0;
				Selection.AnchorColumn = 2;
				Selection.CaretLine = 1;
				Selection.CaretColumn = 6;
				const int32 TakeBacksBefore = SDreamCodeView::GetReadOnlyTakeBacks();
				State->Reached = DreamCodeViewSpec::SendEditingChords(View.ToSharedRef(), Selection);
				State->TakeBacks = SDreamCodeView::GetReadOnlyTakeBacks() - TakeBacksBefore;
			});
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Loop, EDreamRunRoute::Buffer, 1, 2));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumping run"), Run, 60.0);
		Steps.Do(TEXT("nothing changed"), [this, TargetId, Target, Loop, State, Run]()
		{
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestTrue(TEXT("the chords were sent from inside the run's pump"), State->bSent);
			TestTrue(TEXT("the target view was read-only then (a run is active)"), State->bReadOnlyAtSend);
			TestEqual(*FString::Printf(TEXT("every chord reached the focused target view (keyboard focus at the first pump: %s)"), *State->FocusAtSend),
				State->Reached, DreamCodeViewSpec::EditingChords().Num() * 2);
			TestEqual(TEXT("the target buffer is unchanged"), Text(Target), State->TargetBefore);
			TestEqual(TEXT("the stock widget edited nothing during the run (no edit had to be taken back)"), State->TakeBacks, 0);
			TestEqual(TEXT("the running buffer is unchanged"), Text(Loop), State->LoopBefore);
			if (const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(TargetId))
			{
				TestEqual(TEXT("the target widget shows the unchanged text"), View->GetShownText(), State->TargetBefore);
			}
			return true;
		});
		Steps.Finally(TEXT("remove the hook, restore FApp's focus query and the run settings"), [State]()
		{
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(State->PreTick);
			}
			if (State->bFocusSeam)
			{
				FApp::SetHasFocusFunction(&FPlatformApplicationMisc::IsThisApplicationForeground);
				State->bFocusSeam = false;
			}
			if (State->bSettingsChanged)
			{
				UDreamUserSettings* Settings = UDreamUserSettings::GetMutable();
				Settings->bRunPump = State->bSavedRunPump;
				Settings->RunPresentation = State->SavedPresentation;
			}
		});
		Steps.Run(Done);
	});
}

// =====================================================================================================================
// Dream.CodeView.StickyColumn
// =====================================================================================================================

BEGIN_DEFINE_SPEC(FDreamCodeViewStickyColumnSpec, "Dream.CodeView.StickyColumn", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString TempFile;
	static FString LongLines(int32 Count = 12)
	{
		FString Text;
		for (int32 Line = 0; Line < Count; ++Line)
		{
			Text += FString::Printf(TEXT("value_%03d = 'abcdefghijklmnopqrstuvwxyz'\n"), Line);
		}
		return Text;
	}
	/** Down twice through the real key path from a caret the view was made with: the column is kept (§4.5 sticky X). */
	bool CheckDownKeepsColumn(const FGuid& Id, int32 Line, int32 Column)
	{
		const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
		if (!TestTrue(TEXT("focus is in the code view"), View.IsValid() && DreamCodeViewSpec::FocusIsInside(View.ToSharedRef())))
		{
			return false;
		}
		int32 AtLine = 0;
		int32 AtColumn = 0;
		View->GetCaret(AtLine, AtColumn);
		TestTrue(*FString::Printf(TEXT("the caret starts at line %d, column %d (was %d, %d)"), Line + 1, Column + 1, AtLine + 1, AtColumn + 1),
			AtLine == Line && AtColumn == Column);
		DreamCodeViewSpec::Press(EKeys::Down, FModifierKeysState());
		View->GetCaret(AtLine, AtColumn);
		TestTrue(*FString::Printf(TEXT("Down keeps the column: line %d, column %d (was %d, %d)"), Line + 2, Column + 1, AtLine + 1, AtColumn + 1),
			AtLine == Line + 1 && AtColumn == Column);
		DreamCodeViewSpec::Press(EKeys::Down, FModifierKeysState());
		View->GetCaret(AtLine, AtColumn);
		TestTrue(*FString::Printf(TEXT("and again: line %d, column %d (was %d, %d)"), Line + 3, Column + 1, AtLine + 1, AtColumn + 1),
			AtLine == Line + 2 && AtColumn == Column);
		return true;
	}
END_DEFINE_SPEC(FDreamCodeViewStickyColumnSpec)

void FDreamCodeViewStickyColumnSpec::Define()
{
	BeforeEach([this]() { Tab = DreamTest::RequireTab(*this); });
	AfterEach([this]()
	{
		DreamCodeViewSpec::CloseAll(BufferIds);
		if (!TempFile.IsEmpty())
		{
			IFileManager::Get().Delete(*TempFile, false, true, true);
			TempFile.Reset();
		}
		Tab.Reset();
	});

	LatentIt("keeps the column of a caret restored before the view's first paint on Up and Down (PER-02)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		// A buffer whose view does not exist yet, with a saved caret (the session restore path): the view is made with it.
		// SDreamTab activates (and so makes the view of) a new buffer at once when no tab is active, as after an earlier spec
		// closed every tab: an active holder tab comes first, so the buffer below stays without a view until activated.
		const TSharedPtr<FDreamBuffer> Holder = DreamTest::OpenScratch(*this, *Tab, TEXT("# holder"), TEXT("StickyHolder"));
		if (!Holder.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Holder->GetId());
		const TSharedRef<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().CreateScratch(LongLines(), DreamTest::ScratchName(TEXT("Sticky")));
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("make the view with a restored caret at line 5, column 13, and focus it"), [this, Id, Buffer]()
		{
			if (!TestFalse(TEXT("the buffer has no code view yet"), Tab->FindCodeView(Id).IsValid()))
			{
				return false;
			}
			Buffer->GetViewState().CaretLine = 4;
			Buffer->GetViewState().CaretColumn = 12;
			Tab->ActivateBuffer(Id);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the view was made"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);   // painted once: the stock line views exist now
		Steps.Do(TEXT("Down, Down"), [this, Id]() { return CheckDownKeepsColumn(Id, 4, 12); });
		Steps.Run(Done);
	});

	LatentIt("leaves a view the user scrolled away where it is on a key that does not move up or down", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Holder = DreamTest::OpenScratch(*this, *Tab, TEXT("# holder"), TEXT("StickyHolder"));
		if (!Holder.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Holder->GetId());
		const TSharedRef<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().CreateScratch(LongLines(300), DreamTest::ScratchName(TEXT("StickyScroll")));
		BufferIds.Add(Buffer->GetId());
		const FGuid Id = Buffer->GetId();
		const TSharedRef<int32> FirstShown = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		Steps.Do(TEXT("make the view with a restored caret at line 5, column 13, and focus it"), [this, Id, Buffer]()
		{
			Buffer->GetViewState().CaretLine = 4;
			Buffer->GetViewState().CaretColumn = 12;
			Tab->ActivateBuffer(Id);
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the view was made"), View.IsValid()))
			{
				return false;
			}
			View->FocusEditor();
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("scroll the view away with the wheel"), [this, Id]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			if (!TestTrue(TEXT("the view is there"), View.IsValid()))
			{
				return false;
			}
			for (int32 Notch = 0; Notch < 20; ++Notch)
			{
				DreamCodeViewSpec::Wheel(*View, -1.f, /*bCtrl*/ false);
			}
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("press Ctrl alone (a key that reads no column)"), [this, Id, FirstShown]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			*FirstShown = View->GetFirstVisibleLine();
			if (!TestTrue(*FString::Printf(TEXT("the wheel scrolled the caret line out of view (first shown line %d)"), *FirstShown + 1), *FirstShown > 10))
			{
				return false;
			}
			if (!TestTrue(TEXT("focus is in the code view"), DreamCodeViewSpec::FocusIsInside(View.ToSharedRef())))
			{
				return false;
			}
			DreamCodeViewSpec::Press(EKeys::LeftControl, DreamCodeViewSpec::Mods(/*bCtrl*/ true));
			return true;
		});
		Steps.WaitFrames(3);
		Steps.Do(TEXT("the view did not jump back to the caret; Down still keeps the column"), [this, Id, FirstShown]()
		{
			const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(Id);
			TestEqual(TEXT("the first shown line after Ctrl"), View->GetFirstVisibleLine(), *FirstShown);
			return CheckDownKeepsColumn(Id, 4, 12);
		});
		Steps.Run(Done);
	});

	LatentIt("keeps the column of a link that opens a file at a line and column on Up and Down (traceback, Problems)", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		TempFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamT4") / FString::Printf(TEXT("sticky_%s.py"), *DreamTest::UniqueSuffix()));
		if (!TestTrue(TEXT("the test file is written"), FFileHelper::SaveStringToFile(LongLines(), *TempFile)))
		{
			Done.Execute();
			return;
		}
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		TSharedRef<FGuid> Id = MakeShared<FGuid>();
		Steps.Do(TEXT("open the file at line 5, column 13 (1-based), and focus it"), [this, Id]()
		{
			if (!TestTrue(TEXT("the file opens at the location"), Tab->OpenLocation(TempFile, 5, 13)))
			{
				return false;
			}
			const TSharedPtr<FDreamBuffer> File = Tab->GetActiveBuffer();
			if (!TestTrue(TEXT("the file is the active buffer"), File.IsValid() && File->HasPath()))
			{
				return false;
			}
			*Id = File->GetId();
			BufferIds.Add(*Id);
			if (const TSharedPtr<SDreamCodeView> View = Tab->FindCodeView(*Id))
			{
				View->FocusEditor();
			}
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Down, Down"), [this, Id]() { return CheckDownKeepsColumn(*Id, 4, 12); });
		Steps.Run(Done);
	});
}
