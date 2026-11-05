// SPDX-License-Identifier: Apache-2.0
// Dream.Keys.G5: the key-routing leak suite (TDD §4.16.1, D8, SHL-14). The fixture: the Level Editor with 20 actors, 3 of
// them selected, and the Dream tab where the layout docks it. The chords are generated at run time: every FDreamCommands
// chord (whatever the keymap binds now), the consume list (printable keys, Tab, arrows, Home/End, PgUp/PgDn, Delete,
// Backspace, Esc, Ctrl+Z/Y/Shift+Z/C/V/X/A/D/S/W) and the consume extension (unbound Ctrl, Ctrl+Shift and left-Ctrl+Alt
// chords on letters and digits). Each goes through FSlateApplication::ProcessKeyDownEvent / ProcessKeyCharEvent /
// ProcessKeyUpEvent to the focused Dream view, and after each chord the probes hold: actor count and names, the
// selection, the map package's dirty flag, no package saved (Ctrl+S on a file buffer saves the file, never the level),
// GEditor->Trans queue length and undo count (no TRANSACTION UNDO), PlayWorld, the Level Editor and Dream tabs alive, and
// FDreamRunService's undone-run count. The Dream side is put back after each chord (text, tabs, zoom, popups, focus); the
// user's clipboard is saved and restored around the pass. Pass-through chords are the control: Dream's two routing passes
// (RoutePre, RouteBubble) leave them Unhandled in every view context, so they reach Unreal (checked at the router, since
// sending them for real would open Unreal's own dialogs and browsers).
//   T-KEY-009 core set in a buffer view; T-KEY-010 the REPL prompt and the Output view; T-KEY-012 the set re-sent while
//   a run pumps (buffers unchanged); T-KEY-011 / T-TXN-010 the AltGr layouts, synthesized as Slate events with the
//   layout's characters (B-note: loading the real layouts would change the user's input-language list); T-TXN-011
//   Ctrl+Z in the code view after a run undoes text, not the run's transaction; T-KEY-020 Alt+Enter through SendInput.
//   T-REPL-005 the history popup; T-REPL-017 the prompt strip and the REPL tab.
// Not here (tests report): the floating-tab and PIE variants of T-KEY-010 and the Live (docked) pass (M4).
#include "Support/DreamTestUtil.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "CodeView/SDreamCodeView.h"
#include "Commands/DreamCommands.h"
#include "Commands/DreamKeyRouter.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Fixtures/DreamTestProbes.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Module/DreamEditorModule.h"
#include "Output/DreamReplHistory.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"
#include "Selection.h"
#include "Settings/DreamUserSettings.h"
#include "Shell/SDreamOutputArea.h"
#include "Shell/SDreamOutputView.h"
#include "Shell/SDreamReplStrip.h"
#include "Shell/SDreamTab.h"
#include "Support/DreamBotSteps.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsApplication.h"
#include "Windows/WindowsHWrapper.h"   // the clipboard format check (IsPlainTextClipboard)
#endif

namespace DreamKeysG5Spec
{
	enum class EKind : uint8 { Bound, Consume, Extension };

	struct FChord
	{
		FKey Key;
		bool bCtrl = false;
		bool bShift = false;
		bool bAlt = false;
		TCHAR Char = 0;
		EKind Kind = EKind::Consume;
		/** Checked at the router only (RoutePre runs the command on a recording target), never sent: Undo Last Run would undo
		 *  whatever run of an earlier spec is last in the editor's undo history and change the level the probes watch. */
		bool bRouteOnly = false;
		EDreamCommand Command = EDreamCommand::None;

		FString Describe() const
		{
			FString Text;
			Text += bCtrl ? TEXT("Ctrl+") : TEXT("");
			Text += bShift ? TEXT("Shift+") : TEXT("");
			Text += bAlt ? TEXT("Alt+") : TEXT("");
			Text += Key.GetDisplayName().ToString();
			if (Command != EDreamCommand::None)
			{
				Text += FString::Printf(TEXT(" (%s)"), *FDreamCommands::GetCommandName(Command).ToString());
			}
			return Text;
		}
		bool SameChord(const FKey& K, bool C, bool S, bool A) const { return Key == K && bCtrl == C && bShift == S && bAlt == A; }
	};

	struct FPassThrough
	{
		FKey Key;
		bool bCtrl = false;
		bool bShift = false;
		bool bAlt = false;
	};

	/** §4.9.4 pass-through list. */
	TArray<FPassThrough> PassThroughs()
	{
		return {
			{ EKeys::Tab, true, false, false }, { EKeys::Tab, true, true, false }, { EKeys::P, true, false, false }, { EKeys::B, true, false, false },
			{ EKeys::F1, false, false, false }, { EKeys::F1, false, false, true }, { EKeys::F1, false, true, false }, { EKeys::O, true, false, false },
			{ EKeys::S, true, true, false }, { EKeys::S, true, true, true }, { EKeys::W, true, true, false }, { EKeys::E, true, false, true },
			{ EKeys::K, true, false, true }, { EKeys::P, true, false, true }, { EKeys::Tilde, true, false, false }, { EKeys::Tilde, false, false, true },
			{ EKeys::P, false, false, true }, { EKeys::S, false, false, true }, { EKeys::Pause, false, false, false },
		};
	}

	bool IsPassThrough(const FKey& Key, bool bCtrl, bool bShift, bool bAlt)
	{
		for (const FPassThrough& P : PassThroughs())
		{
			if (P.Key == Key && P.bCtrl == bCtrl && P.bShift == bShift && P.bAlt == bAlt)
			{
				return true;
			}
		}
		return false;
	}

	/** Chords that leave the code view on purpose or stop the probe from being meaningful, never sent: the run-level Stop
	 *  (no run here) is harmless and kept; nothing is excluded for its effect, only for the pass the set runs in. */
	/**
	 * True when the clipboard is empty or holds only text (the rule of Dream.CodeView.ReadOnlyKeys): FPlatformApplicationMisc
	 * saves and restores text only, so a clipboard with an image, files or rich text must not be touched (tests-r3 minor 4).
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

	/** bClipboardWritable false: the chords that write the clipboard (Ctrl+C, Ctrl+X) are left out (IsPlainTextClipboard). */
	TArray<FChord> GenerateChords(bool bDuringRun, bool bClipboardWritable = true)
	{
		TArray<FChord> Chords;
		auto Has = [&Chords](const FKey& K, bool C, bool S, bool A)
		{
			return Chords.ContainsByPredicate([&](const FChord& Ch) { return Ch.SameChord(K, C, S, A); });
		};
		// Every bound command chord, as the keymap binds it now.
		if (FDreamCommands::IsRegistered())
		{
			for (uint32 Index = 1; Index < static_cast<uint32>(EDreamCommand::Count); ++Index)
			{
				const EDreamCommand Command = static_cast<EDreamCommand>(Index);
				const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command);
				if (!Info.IsValid())
				{
					continue;
				}
				for (const EMultipleKeyBindingIndex Slot : { EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary })
				{
					const FInputChord& Chord = *Info->GetActiveChord(Slot);
					if (!Chord.IsValidChord() || Chord.NeedsCommand() || Has(Chord.Key, Chord.NeedsControl(), Chord.NeedsShift(), Chord.NeedsAlt()))
					{
						continue;
					}
					// During a run the Run window is the active modal: Esc and the Stop chord stop the run (covered by
					// Dream.Supervisor.Bot); the rest must change nothing.
					if (bDuringRun && Command == EDreamCommand::Stop)
					{
						continue;
					}
					FChord Ch;
					Ch.Key = Chord.Key;
					Ch.bCtrl = Chord.NeedsControl();
					Ch.bShift = Chord.NeedsShift();
					Ch.bAlt = Chord.NeedsAlt();
					Ch.Kind = EKind::Bound;
					Ch.Command = Command;
					// Route-only: Undo Last Run (it would undo an earlier spec's run), and the commands that open a native file
					// dialog, which is modal and blocks the game thread: Save As and Open always, Save during a run (its buffer is
					// the run's scratch tab, where Save means Save As). Outside runs Save goes to the file buffer and saves it.
					Ch.bRouteOnly = (!bDuringRun && Command == EDreamCommand::UndoLastRun) || Command == EDreamCommand::SaveAs
						|| Command == EDreamCommand::OpenFile || (bDuringRun && Command == EDreamCommand::Save)
						// (debug) PO-06 q1: F5 would start a Debug run and F9 set a breakpoint that a later run could pause on
						// (a blocking modal); routing still proves the chord is bound and consumed.
						|| Command == EDreamCommand::DebugRun || Command == EDreamCommand::ToggleBreakpoint
						|| Command == EDreamCommand::DebugStepOver || Command == EDreamCommand::DebugStepInto || Command == EDreamCommand::DebugStepOut;
					Chords.Add(Ch);
				}
			}
		}
		// The consume list: printable keys (a sample of each class), the navigation and editing keys, Esc, and the Ctrl list.
		const TCHAR* Letters = TEXT("aeqwz");
		for (const TCHAR* L = Letters; *L; ++L)
		{
			const FKey Key(*FString::Printf(TEXT("%c"), FChar::ToUpper(*L)));
			if (!Has(Key, false, false, false)) { Chords.Add({ Key, false, false, false, *L, EKind::Consume }); }
			if (!Has(Key, false, true, false)) { Chords.Add({ Key, false, true, false, FChar::ToUpper(*L), EKind::Consume }); }
		}
		Chords.Add({ EKeys::Seven, false, false, false, TEXT('7'), EKind::Consume });
		Chords.Add({ EKeys::Period, false, false, false, TEXT('.'), EKind::Consume });
		Chords.Add({ EKeys::SpaceBar, false, false, false, TEXT(' '), EKind::Consume });
		for (const FKey& Key : { EKeys::Tab, EKeys::Up, EKeys::Down, EKeys::Left, EKeys::Right, EKeys::Home, EKeys::End,
			EKeys::PageUp, EKeys::PageDown, EKeys::Delete, EKeys::BackSpace })
		{
			if (!Has(Key, false, false, false))
			{
				Chords.Add({ Key, false, false, false, 0, EKind::Consume });
			}
		}
		if (!bDuringRun && !Has(EKeys::Escape, false, false, false))
		{
			Chords.Add({ EKeys::Escape, false, false, false, 0, EKind::Consume });
		}
		const TArray<TPair<FKey, bool>> CtrlList = { { EKeys::Z, false }, { EKeys::Y, false }, { EKeys::Z, true }, { EKeys::C, false },
			{ EKeys::V, false }, { EKeys::X, false }, { EKeys::A, false }, { EKeys::D, false }, { EKeys::S, false }, { EKeys::W, false } };
		for (const TPair<FKey, bool>& Entry : CtrlList)
		{
			if (!bClipboardWritable && !Entry.Value && (Entry.Key == EKeys::C || Entry.Key == EKeys::X))
			{
				continue;
			}
			if (!Has(Entry.Key, true, Entry.Value, false))
			{
				Chords.Add({ Entry.Key, true, Entry.Value, false, 0, EKind::Consume });
			}
		}
		// The consume extension: unbound Ctrl, Ctrl+Shift and left-Ctrl+Alt chords on a letter or digit, unless pass-through.
		for (int32 Index = 0; Index < 36; ++Index)
		{
			const FKey Key = Index < 26 ? FKey(*FString::Printf(TEXT("%c"), TEXT('A') + Index))
				: TArray<FKey>{ EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine }[Index - 26];
			for (int32 Mods = 0; Mods < 3; ++Mods)
			{
				const bool bShift = Mods == 1;
				const bool bAlt = Mods == 2;
				if (IsPassThrough(Key, true, bShift, bAlt) || Has(Key, true, bShift, bAlt))
				{
					continue;
				}
				// A non-text clipboard is never touched: no Ctrl+C or Ctrl+X here either (the Output view copies on Ctrl+C
				// whatever is selected, nothing included).
				if (!bClipboardWritable && !bShift && !bAlt && (Key == EKeys::C || Key == EKeys::X))
				{
					continue;
				}
				Chords.Add({ Key, true, bShift, bAlt, 0, EKind::Extension });
			}
		}
		return Chords;
	}

	FModifierKeysState Mods(bool bCtrl, bool bShift, bool bAlt, bool bRightAlt = false)
	{
		return FModifierKeysState(bShift, false, bCtrl, false, bAlt, bRightAlt, false, false, false);
	}

	/** One chord through Slate's real input path; true when a widget handled the key-down. */
	bool Send(const FChord& Chord)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const uint32 User = App.GetUserIndexForKeyboard();
		const FModifierKeysState M = Mods(Chord.bCtrl, Chord.bShift, Chord.bAlt);
		const uint32 Char = Chord.Char;
		const bool bHandled = App.ProcessKeyDownEvent(FKeyEvent(Chord.Key, M, User, false, Char, Char));
		if (Char != 0 && !Chord.bCtrl && !Chord.bAlt)
		{
			App.ProcessKeyCharEvent(FCharacterEvent(static_cast<TCHAR>(Char), M, User, false));
		}
		App.ProcessKeyUpEvent(FKeyEvent(Chord.Key, M, User, false, Char, Char));
		return bHandled;
	}

	/** The level fixture: 20 transient actors, 3 selected; the map's dirty flag is put back after spawning and at the end. */
	struct FLevelFixture
	{
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<TWeakObjectPtr<AActor>> PreviousSelection;
		bool bMapWasDirty = false;
		bool bSpawned = false;

		static UWorld* World() { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; }

		bool Spawn(FAutomationTestBase& Test, const FString& Prefix)
		{
			UWorld* W = World();
			if (!Test.TestNotNull(TEXT("the editor world"), W))
			{
				return false;
			}
			bMapWasDirty = W->GetOutermost()->IsDirty();
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Selected = Cast<AActor>(*It))
				{
					PreviousSelection.Add(Selected);
				}
			}
			for (int32 Index = 0; Index < 20; ++Index)
			{
				FActorSpawnParameters Params;
				Params.Name = MakeUniqueObjectName(W->PersistentLevel, AStaticMeshActor::StaticClass(), FName(*FString::Printf(TEXT("%s_%02d"), *Prefix, Index)));
				Params.ObjectFlags = RF_Transient;
				AStaticMeshActor* Actor = W->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
					FTransform(FVector(200.0 * Index, 0.0, 100.0)), Params);
				if (Actor == nullptr)
				{
					Test.AddError(TEXT("could not spawn the G5 fixture actors"));
					return false;
				}
				Actor->SetActorLabel(Params.Name.ToString());
				Actors.Add(Actor);
			}
			bSpawned = true;
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			for (int32 Index = 0; Index < 3; ++Index)
			{
				GEditor->SelectActor(Actors[Index].Get(), /*bInSelected*/ true, /*bNotify*/ Index == 2);
			}
			W->GetOutermost()->SetDirtyFlag(false);
			return true;
		}

		void Destroy()
		{
			UWorld* W = World();
			if (!bSpawned || W == nullptr)
			{
				return;
			}
			GEditor->SelectNone(false, true);
			for (const TWeakObjectPtr<AActor>& Actor : Actors)
			{
				if (Actor.IsValid())
				{
					W->DestroyActor(Actor.Get());
				}
			}
			for (const TWeakObjectPtr<AActor>& Selected : PreviousSelection)
			{
				if (Selected.IsValid())
				{
					GEditor->SelectActor(Selected.Get(), true, false);
				}
			}
			GEditor->NoteSelectionChange();
			W->GetOutermost()->SetDirtyFlag(bMapWasDirty);
			Actors.Reset();
			bSpawned = false;
		}
	};

	/** What a leaked chord would change in Unreal (§4.16.1 probes). */
	/** UPackage::PackageMarkedDirtyEvent on the fixture's map package, counted while a G5 case runs (Prepare binds it). */
	int32 MapMarkedDirtyCount = 0;

	struct FProbes
	{
		TArray<FString> Actors;          // the fixture actors still alive, by label
		int32 LevelActors = 0;
		int32 Selected = 0;
		bool bMapDirty = false;
		int32 TransQueue = 0;
		int32 TransUndo = 0;
		bool bPlayWorld = false;
		bool bLevelEditorTab = false;
		bool bDreamTab = false;
		int32 UndoneRuns = 0;
		int32 SavedPackages = 0;
		/** MarkPackageDirty calls on the map package so far (UPackage::PackageMarkedDirtyEvent fires also when it is already
		 *  dirty): with the map dirty at the baseline (the engine re-dirties a never-saved /Temp map, AddSettle) the flag
		 *  cannot show a leak, the count can (review tests-r2 minor). */
		int32 MapMarkedDirty = 0;

		static FProbes Capture(const FLevelFixture& Level, int32 SavedPackages)
		{
			FProbes P;
			for (const TWeakObjectPtr<AActor>& Actor : Level.Actors)
			{
				if (Actor.IsValid() && IsValid(Actor.Get()))
				{
					P.Actors.Add(Actor->GetActorLabel());
				}
			}
			if (UWorld* W = FLevelFixture::World())
			{
				P.LevelActors = W->PersistentLevel ? W->PersistentLevel->Actors.Num() : 0;
				P.bMapDirty = W->GetOutermost()->IsDirty();
			}
			P.Selected = GEditor ? GEditor->GetSelectedActorCount() : 0;
			P.TransQueue = GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
			P.TransUndo = GEditor && GEditor->Trans ? GEditor->Trans->GetUndoCount() : 0;
			P.bPlayWorld = GEditor && GEditor->PlayWorld != nullptr;
			if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
			{
				P.bLevelEditorTab = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor")).GetLevelEditorTab().IsValid();
			}
			P.bDreamTab = FDreamEditorModule::Get().GetTab().IsValid();
			for (const FDreamRunRecord& Record : FDreamEditorModule::Get().GetRunService().GetHistory())
			{
				P.UndoneRuns += Record.Txn.Status == EDreamTxnStatus::Undone ? 1 : 0;
			}
			P.SavedPackages = SavedPackages;
			P.MapMarkedDirty = MapMarkedDirtyCount;
			return P;
		}

		/** The differences from Before, empty when every probe holds. */
		FString Diff(const FProbes& Before) const
		{
			TArray<FString> Out;
			if (Actors != Before.Actors) { Out.Add(FString::Printf(TEXT("fixture actors %d -> %d (deleted or renamed)"), Before.Actors.Num(), Actors.Num())); }
			if (LevelActors != Before.LevelActors) { Out.Add(FString::Printf(TEXT("level actors %d -> %d (duplicated or deleted)"), Before.LevelActors, LevelActors)); }
			if (Selected != Before.Selected) { Out.Add(FString::Printf(TEXT("selection %d -> %d"), Before.Selected, Selected)); }
			if (bMapDirty != Before.bMapDirty) { Out.Add(TEXT("the map package became dirty")); }
			else if (Before.bMapDirty && MapMarkedDirty != Before.MapMarkedDirty)
			{
				Out.Add(FString::Printf(TEXT("the map package was marked dirty %d time(s) while already dirty"), MapMarkedDirty - Before.MapMarkedDirty));
			}
			if (TransQueue != Before.TransQueue) { Out.Add(FString::Printf(TEXT("undo queue %d -> %d"), Before.TransQueue, TransQueue)); }
			if (TransUndo != Before.TransUndo) { Out.Add(FString::Printf(TEXT("undo count %d -> %d (TRANSACTION UNDO)"), Before.TransUndo, TransUndo)); }
			if (bPlayWorld != Before.bPlayWorld) { Out.Add(TEXT("PIE started or stopped")); }
			if (bLevelEditorTab != Before.bLevelEditorTab) { Out.Add(TEXT("the Level Editor tab closed")); }
			if (bDreamTab != Before.bDreamTab) { Out.Add(TEXT("the Dream tab closed")); }
			if (UndoneRuns != Before.UndoneRuns) { Out.Add(TEXT("Undo Last Run fired")); }
			if (SavedPackages != Before.SavedPackages) { Out.Add(FString::Printf(TEXT("%d package(s) saved"), SavedPackages - Before.SavedPackages)); }
			return FString::Join(Out, TEXT("; "));
		}
	};

	/** The IDreamCommandTarget the router check uses for the pass-through control: it records any execution. */
	class FRecordingTarget : public IDreamCommandTarget
	{
	public:
		virtual bool CanExecuteDreamCommand(EDreamCommand, FText&) const override { return true; }
		virtual void ExecuteDreamCommand(EDreamCommand Command) override { Executed.Add(Command); }
		virtual void ShowCommandRefusal(EDreamCommand, const FText&) override {}
		TArray<EDreamCommand> Executed;
	};

	enum class EView : uint8 { Buffer, Prompt, Output };

	/** The context mask the view's own key handling passes to FDreamKeyRouter. */
	uint32 ViewMask(EView View)
	{
		uint32 Mask = dream::Ctx_Focused;
		Mask |= View == EView::Buffer ? (dream::Ctx_Editable | dream::Ctx_Buffer) : 0u;
		Mask |= View == EView::Prompt ? (dream::Ctx_Editable | dream::Ctx_Prompt) : 0u;
		Mask |= View == EView::Output ? dream::Ctx_Output : 0u;
		return Mask;
	}

	const TCHAR* ViewName(EView View)
	{
		switch (View)
		{
		case EView::Buffer: return TEXT("buffer view");
		case EView::Prompt: return TEXT("REPL prompt");
		case EView::Output: return TEXT("Output view");
		}
		return TEXT("?");
	}

	/** Depth-first search for the first widget of Type under Root. */
	TSharedPtr<SWidget> FindByType(const TSharedRef<SWidget>& Root, FName Type)
	{
		if (Root->GetType() == Type)
		{
			return Root;
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindByType(Children->GetChildAt(Index), Type))
			{
				return Found;
			}
		}
		return nullptr;
	}

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

#if PLATFORM_WINDOWS
	/** T-KEY-020 probe: counts WM_SYSCOMMAND SC_KEYMENU and WM_ENTERMENULOOP on the editor's windows. Never consumes. */
	struct FMenuLoopProbe final : public IWindowsMessageHandler
	{
		int32 KeyMenu = 0;
		int32 EnterMenuLoop = 0;
		virtual bool ProcessMessage(HWND, uint32 Msg, WPARAM WParam, LPARAM, int32&) override
		{
			if (Msg == WM_SYSCOMMAND && (WParam & 0xFFF0) == SC_KEYMENU) { ++KeyMenu; }
			if (Msg == WM_ENTERMENULOOP) { ++EnterMenuLoop; }
			return false;
		}
	};
#endif
}

BEGIN_DEFINE_SPEC(FDreamKeysG5Spec, "Dream.Keys.G5", DreamTest::AttendedFlags)
	TSharedPtr<SDreamTab> Tab;
	TArray<FGuid> BufferIds;
	FString Sfx;
	FString FilePath;
	FString FileText;
	FGuid FileBufferId;
	DreamKeysG5Spec::FLevelFixture Level;
	FString SavedClipboard;
	bool bClipboardSaved = false;
	int32 SavedPackages = 0;
	FDelegateHandle SavedHandle;
	FDelegateHandle MarkedDirtyHandle;
	TArray<FGuid> TabsBefore;
	/** Ctrl+M (CFG-08, B75) flips the router's session-wide Tab-moves-focus mode: put back after every chord and the pass. */
	bool bTabMovesFocusBefore = false;
	bool bTabModeSaved = false;
	bool Prepare();
	/** A few frames for the level to settle after the fixture cleared the map's dirty flag (see the definition). */
	void AddSettle(DreamTest::FDreamSteps& Steps);
	/** The file buffer the chords go to (Ctrl+S saves a file; on a scratch tab it opens a native Save As dialog). */
	bool OpenFileBuffer();
	/** Focuses View's target; false when it is not there. */
	bool FocusView(DreamKeysG5Spec::EView View);
	/** Puts the Dream side back after a chord: tabs, the file buffer, its text, zoom, popups, the prompt, the output tab. */
	void RestoreDream(DreamKeysG5Spec::EView View, const FString& PromptText);
	void DefineChordPass(DreamKeysG5Spec::EView View);
END_DEFINE_SPEC(FDreamKeysG5Spec)

bool FDreamKeysG5Spec::Prepare()
{
	Tab = DreamTest::RequireTab(*this);
	if (!Tab.IsValid())
	{
		return false;
	}
	bClipboardSaved = DreamKeysG5Spec::IsPlainTextClipboard();
	if (bClipboardSaved)
	{
		FPlatformApplicationMisc::ClipboardPaste(SavedClipboard);
	}
	else
	{
		AddInfo(TEXT("the clipboard holds more than text: it is left alone, so Ctrl+C and Ctrl+X are not sent in this case"));
	}
	SavedPackages = 0;
	SavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda([this](const FString&, UPackage*, FObjectPostSaveContext) { ++SavedPackages; });
	UPackage::PackageMarkedDirtyEvent.Remove(MarkedDirtyHandle);
	MarkedDirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([](UPackage* Package, bool /*bWasDirty*/)
	{
		const UWorld* World = DreamKeysG5Spec::FLevelFixture::World();
		if (World != nullptr && Package == World->GetOutermost())
		{
			++DreamKeysG5Spec::MapMarkedDirtyCount;
		}
	});
	if (!bTabModeSaved)
	{
		bTabMovesFocusBefore = FDreamKeyRouter::Get().IsTabMovesFocus();
		bTabModeSaved = true;
	}
	TabsBefore.Reset();
	for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
	{
		TabsBefore.Add(Buffer->GetId());
	}
	const TSharedPtr<SWindow> DreamWindow = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
	AddInfo(FString::Printf(TEXT("the Dream tab is %s"), DreamWindow.IsValid() && DreamWindow == FGlobalTabmanager::Get()->GetRootWindow()
		? TEXT("docked in the main frame with the Level Editor") : TEXT("in its own window")));
	return Level.Spawn(*this, TEXT("DreamG5_") + Sfx) && OpenFileBuffer();
}

void FDreamKeysG5Spec::AddSettle(DreamTest::FDreamSteps& Steps)
{
	// Clearing the never-saved map's dirty flag (Level.Spawn) schedules the editor's unsaved-assets sync, which marks a /Temp
	// map dirty again one frame later while any side package is dirty (FUnsavedAssetsTracker -> EnumerateDirtyWorldPackages,
	// UnrealEd FileHelpers.cpp; traced by run, board 05:11). The per-chord baselines are taken after that settled, so the
	// engine's own step never reads as a leak of the first chord; a dirty map then still catches a save (which clears it).
	Steps.WaitFrames(5);
	Steps.Do(TEXT("the level settled after the fixture"), [this]()
	{
		const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World != nullptr && World->GetOutermost()->IsDirty())
		{
			AddInfo(TEXT("the never-saved map is dirty again after the fixture cleared it (the editor's unsaved-assets sync re-dirties a /Temp map); the probe still catches a save, which would clear it"));
		}
		return true;
	});
}

bool FDreamKeysG5Spec::OpenFileBuffer()
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Dream"), TEXT("G5")));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	FilePath = FPaths::Combine(Dir, FString::Printf(TEXT("g5_%s.py"), *Sfx));
	FileText = FString::Printf(TEXT("g5_hits_%s = globals().get('g5_hits_%s', 0) + 1"), *Sfx, *Sfx);
	if (!FFileHelper::SaveStringToFile(FileText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(FString::Printf(TEXT("could not write %s"), *FilePath));
		return false;
	}
	FText Error;
	const TSharedPtr<FDreamBuffer> Buffer = FDreamEditorModule::Get().GetBufferManager().OpenFile(FilePath, Error);
	if (!TestTrue(FString::Printf(TEXT("open %s: %s"), *FilePath, *Error.ToString()), Buffer.IsValid()))
	{
		return false;
	}
	FileBufferId = Buffer->GetId();
	Tab->ActivateBuffer(FileBufferId);
	return true;
}

bool FDreamKeysG5Spec::FocusView(DreamKeysG5Spec::EView View)
{
	FSlateApplication::Get().DismissAllMenus();
	if (View == DreamKeysG5Spec::EView::Buffer)
	{
		Tab->ActivateBuffer(FileBufferId);
		const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView();
		if (Code.IsValid())
		{
			Code->FocusEditor();
		}
		return Code.IsValid() && DreamKeysG5Spec::FocusIsInside(Code.ToSharedRef());
	}
	const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
	if (!Area.IsValid())
	{
		return false;
	}
	if (View == DreamKeysG5Spec::EView::Prompt)
	{
		Area->FocusRepl();
		const TSharedPtr<SDreamCodeView> Prompt = Area->GetReplStrip().IsValid() ? Area->GetReplStrip()->GetPromptView() : nullptr;
		return Prompt.IsValid() && DreamKeysG5Spec::FocusIsInside(Prompt.ToSharedRef());
	}
	Area->SetActiveTab(EDreamOutputTab::Output);
	const TSharedPtr<SDreamOutputView> Output = Area->GetOutputView();
	if (!Output.IsValid())
	{
		return false;
	}
	// The view's focus home is its list (SDreamOutputView::IsFocusList), its first focusable descendant.
	const TSharedPtr<SWidget> List = DreamKeysG5Spec::FindFocusable(Output.ToSharedRef());
	FSlateApplication::Get().SetKeyboardFocus(List.IsValid() ? List : StaticCastSharedPtr<SWidget>(Output), EFocusCause::SetDirectly);
	return DreamKeysG5Spec::FocusIsInside(Output.ToSharedRef());
}

void FDreamKeysG5Spec::RestoreDream(DreamKeysG5Spec::EView View, const FString& PromptText)
{
	FSlateApplication::Get().DismissAllMenus();
	if (bTabModeSaved)
	{
		FDreamKeyRouter::Get().SetTabMovesFocus(bTabMovesFocusBefore);
	}
	FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
	// Tabs the chord opened (Ctrl+T, Ctrl+Shift+T) go; the file buffer comes back when Ctrl+W closed it.
	for (const TSharedRef<FDreamBuffer>& Buffer : Tab->GetTabBuffers())
	{
		if (Buffer->GetId() != FileBufferId && !TabsBefore.Contains(Buffer->GetId()))
		{
			Buffers.Close(Buffer->GetId());
		}
	}
	if (!Buffers.FindById(FileBufferId).IsValid())
	{
		FText Error;
		if (const TSharedPtr<FDreamBuffer> Reopened = Buffers.OpenFile(FilePath, Error))
		{
			FileBufferId = Reopened->GetId();
		}
	}
	Tab->ActivateBuffer(FileBufferId);
	if (const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView())
	{
		Code->CloseGoToLine();
		Code->CloseFindBar();
		if (Code->GetText() != FileText)
		{
			Code->SetTextNoUndo(FileText);
		}
		if (Code->GetZoomPercent() != 100)
		{
			Tab->ExecuteDreamCommand(EDreamCommand::ZoomReset);
		}
	}
	const TSharedRef<FDreamBuffer> Repl = Buffers.GetReplInputBuffer();
	if (Repl->GetTextAsString() != PromptText)
	{
		if (const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea())
		{
			if (const TSharedPtr<SDreamCodeView> Prompt = Area->GetReplStrip().IsValid() ? Area->GetReplStrip()->GetPromptView() : nullptr)
			{
				Prompt->SetTextNoUndo(PromptText);
			}
		}
	}
	FocusView(View);
}

void FDreamKeysG5Spec::DefineChordPass(DreamKeysG5Spec::EView View)
{
	using namespace DreamKeysG5Spec;
	const FString Title = FString::Printf(TEXT("%s: every generated chord stays in Dream Python Editor and every probe holds (%s)"),
		View == EView::Buffer ? TEXT("T-KEY-009") : TEXT("T-KEY-010"), ViewName(View));
	LatentIt(Title, DreamTest::LatentTimeout(), [this, View](const FDoneDelegate& Done)
	{
		if (!Prepare() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		TSharedRef<TArray<FChord>> Chords = MakeShared<TArray<FChord>>(GenerateChords(/*bDuringRun*/ false, bClipboardSaved));
		TSharedRef<int32> Index = MakeShared<int32>(0);
		TSharedRef<int32> Phase = MakeShared<int32>(0);
		TSharedRef<FProbes> Before = MakeShared<FProbes>();
		TSharedRef<int32> Unhandled = MakeShared<int32>(0);
		TSharedRef<int32> Leaks = MakeShared<int32>(0);
		TSharedRef<int32> Waits = MakeShared<int32>(0);
		TSharedRef<FString> Prompt = MakeShared<FString>(FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString());
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		Steps.Do(TEXT("focus the view"), [this, View]()
		{
			return TestTrue(FString::Printf(TEXT("keyboard focus is in the %s"), ViewName(View)), FocusView(View));
		});
		Steps.Poll(TEXT("send every chord and check the probes"), [this, View, Chords, Index, Phase, Before, Unhandled, Leaks, Waits, Prompt]()
		{
			if (*Index >= Chords->Num())
			{
				return DreamTest::EStep::Next;
			}
			const FChord& Chord = (*Chords)[*Index];
			if (*Phase == 0 && Chord.bRouteOnly)
			{
				// Checked at the router with a recording target (see FChord::bRouteOnly): RoutePre handles it and runs the command.
				FRecordingTarget Target;
				const FKeyEvent Event(Chord.Key, Mods(Chord.bCtrl, Chord.bShift, Chord.bAlt), 0, false, 0, 0);
				const bool bPre = FDreamKeyRouter::Get().RoutePre(Event, ViewMask(View), Target).IsEventHandled();
				const bool bBubble = FDreamKeyRouter::Get().RouteBubble(Event, ViewMask(View)).IsEventHandled();
				if (!bPre && !bBubble)
				{
					++*Unhandled;
					AddError(FString::Printf(TEXT("%s in the %s: the router leaves it Unhandled, so it would bubble to Unreal (D8)"), *Chord.Describe(), ViewName(View)));
				}
				++*Index;
				return DreamTest::EStep::Wait;
			}
			if (*Phase == 0)
			{
				if (!DreamTest::IsIdle() || !FocusView(View))
				{
					return ++*Waits > 600 ? DreamTest::EStep::Fail : DreamTest::EStep::Wait;
				}
				*Waits = 0;
				*Before = FProbes::Capture(Level, SavedPackages);
				if (!Send(Chord))
				{
					++*Unhandled;
					AddError(FString::Printf(TEXT("%s in the %s: the key-down was not handled in Dream Python Editor, so it bubbled to Unreal (D8)"), *Chord.Describe(), ViewName(View)));
				}
				*Phase = 1;
				return DreamTest::EStep::Wait;
			}
			// A run chord submitted a run: wait for it, then a frame for any deferred effect.
			if (!DreamTest::IsIdle())
			{
				return ++*Waits > 1200 ? DreamTest::EStep::Fail : DreamTest::EStep::Wait;
			}
			if (*Phase == 1)
			{
				*Phase = 2;
				return DreamTest::EStep::Wait;
			}
			const FString Diff = FProbes::Capture(Level, SavedPackages).Diff(*Before);
			if (!Diff.IsEmpty())
			{
				++*Leaks;
				AddError(FString::Printf(TEXT("%s in the %s leaked: %s"), *Chord.Describe(), ViewName(View), *Diff));
			}
			RestoreDream(View, *Prompt);
			*Phase = 0;
			++*Index;
			return DreamTest::EStep::Wait;
		}, 150.0);
		Steps.Do(TEXT("report"), [this, View, Chords, Unhandled, Leaks]()
		{
			int32 Bound = 0, Consume = 0, Extension = 0;
			for (const FChord& Chord : *Chords)
			{
				Bound += Chord.Kind == EKind::Bound ? 1 : 0;
				Consume += Chord.Kind == EKind::Consume ? 1 : 0;
				Extension += Chord.Kind == EKind::Extension ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("%s: %d chords (%d bound, %d consume list, %d extension); %d unhandled, %d leaked"),
				ViewName(View), Chords->Num(), Bound, Consume, Extension, *Unhandled, *Leaks));
			return true;
		});
		Steps.Do(TEXT("the pass-through control stays Unhandled in Dream Python Editor (CFG-07)"), [this, View]()
		{
			const uint32 Mask = ViewMask(View);
			FRecordingTarget Target;
			int32 Consumed = 0;
			for (const FPassThrough& P : PassThroughs())
			{
				const FKeyEvent Event(P.Key, Mods(P.bCtrl, P.bShift, P.bAlt), 0, false, 0, 0);
				const bool bPre = FDreamKeyRouter::Get().RoutePre(Event, Mask, Target).IsEventHandled();
				const bool bBubble = FDreamKeyRouter::Get().RouteBubble(Event, Mask).IsEventHandled();
				if (bPre || bBubble)
				{
					++Consumed;
					AddError(FString::Printf(TEXT("the pass-through chord %s%s%s%s is consumed in the %s (%s)"), P.bCtrl ? TEXT("Ctrl+") : TEXT(""),
						P.bShift ? TEXT("Shift+") : TEXT(""), P.bAlt ? TEXT("Alt+") : TEXT(""), *P.Key.GetDisplayName().ToString(), ViewName(View),
						bPre ? TEXT("RoutePre") : TEXT("RouteBubble")));
				}
			}
			TestEqual(TEXT("no pass-through chord ran a Dream Python Editor command"), Target.Executed.Num(), 0);
			TestEqual(TEXT("pass-through chords consumed"), Consumed, 0);
			return true;
		});
		Steps.Run(Done);
	});
}

void FDreamKeysG5Spec::Define()
{
	using namespace DreamKeysG5Spec;

	BeforeEach([this]()
	{
		Sfx = DreamTest::UniqueSuffix();
	});

	AfterEach([this]()
	{
		if (Tab.IsValid())
		{
			RestoreDream(EView::Buffer, FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString());
		}
		DreamTest::CloseBuffer(FileBufferId);
		FileBufferId.Invalidate();
		for (const FGuid& Id : BufferIds)
		{
			DreamTest::CloseBuffer(Id);
		}
		BufferIds.Reset();
		if (!FilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*FilePath, false, true, true);
			FilePath.Reset();
		}
		Level.Destroy();
		if (bTabModeSaved)
		{
			FDreamKeyRouter::Get().SetTabMovesFocus(bTabMovesFocusBefore);
			bTabModeSaved = false;
		}
		UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
		UPackage::PackageMarkedDirtyEvent.Remove(MarkedDirtyHandle);
		MarkedDirtyHandle.Reset();
		if (bClipboardSaved)
		{
			FPlatformApplicationMisc::ClipboardCopy(*SavedClipboard);
			bClipboardSaved = false;
		}
		Tab.Reset();
	});

	DefineChordPass(EView::Buffer);
	DefineChordPass(EView::Prompt);
	DefineChordPass(EView::Output);

	LatentIt("T-KEY-012: the chord set sent while a run pumps changes no buffer and leaks nothing", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Loop = DreamTest::OpenScratch(*this, *Tab, TEXT("import dream_editor\nfor _ in range(600): dream_editor.sleep(0.01)"), TEXT("G5Run"));
		if (!Loop.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Loop->GetId());
		TSharedRef<TArray<FChord>> Chords = MakeShared<TArray<FChord>>(GenerateChords(/*bDuringRun*/ true, bClipboardSaved));
		TSharedRef<int32> Sent = MakeShared<int32>(0);
		TSharedRef<int32> RouteMisses = MakeShared<int32>(0);
		TSharedRef<bool> bSavedRunPump = MakeShared<bool>(UDreamUserSettings::Get()->bRunPump);
		TSharedRef<EDreamRunPresentationSetting> SavedPresentation = MakeShared<EDreamRunPresentationSetting>(UDreamUserSettings::Get()->RunPresentation);
		TSharedRef<FProbes> Before = MakeShared<FProbes>();
		TSharedRef<FString> LoopText = MakeShared<FString>(Loop->GetTextAsString());
		TSharedRef<FDelegateHandle> PreTick = MakeShared<FDelegateHandle>();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		FRunSupervisor* Supervisor = &FDreamEditorModule::Get().GetSupervisor();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		Steps.Do(TEXT("submit the pumping run; the chords go in from its pumps"), [this, Loop, Chords, Sent, RouteMisses, Before, PreTick, Run, Supervisor]()
		{
			// The chords go in from the Run window's pumps: force the supervised presentation (restored in Finally).
			UDreamUserSettings::GetMutable()->bRunPump = true;
			UDreamUserSettings::GetMutable()->RunPresentation = EDreamRunPresentationSetting::RunWindow;
			FocusView(EView::Buffer);
			*Before = FProbes::Capture(Level, SavedPackages);
			*PreTick = FSlateApplication::Get().OnPreTick().AddLambda([Chords, Sent, RouteMisses, Supervisor](float)
			{
				// A few chords per pump, from inside the pump (GT, GIL released), as keys arrive during a run.
				if (Supervisor->GetPumpDepth() <= 0 || !Supervisor->IsRunWindowOpen())
				{
					return;
				}
				for (int32 Count = 0; Count < 4 && *Sent < Chords->Num(); ++Count)
				{
					const FChord& Chord = (*Chords)[(*Sent)++];
					if (Chord.bRouteOnly)
					{
						// Checked at the router only (FChord::bRouteOnly); a native dialog would block the pump.
						FRecordingTarget Target;
						const FKeyEvent Event(Chord.Key, Mods(Chord.bCtrl, Chord.bShift, Chord.bAlt), 0, false, 0, 0);
						const uint32 Mask = ViewMask(EView::Buffer) | dream::Ctx_RunActive;
						if (!FDreamKeyRouter::Get().RoutePre(Event, Mask, Target).IsEventHandled() && !FDreamKeyRouter::Get().RouteBubble(Event, Mask).IsEventHandled())
						{
							++*RouteMisses;
						}
						continue;
					}
					Send(Chord);
				}
			});
			Run->RunNumber = DreamTest::Submit(*this, DreamTest::MakeBufferRequest(*Loop, EDreamRunRoute::Buffer, 1, 2));
			return Run->RunNumber > 0;
		});
		DreamTest::AddWaitForRun(Steps, TEXT("the pumping run"), Run, 60.0);
		Steps.Do(TEXT("check"), [this, Chords, Sent, RouteMisses, Before, LoopText, Loop, Run]()
		{
			TestEqual(TEXT("every chord was sent during the run"), *Sent, Chords->Num());
			TestEqual(TEXT("every route-only chord (Save, Save As, Open) is handled by the router during the run"), *RouteMisses, 0);
			DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok);
			TestEqual(TEXT("the running buffer is unchanged (T-KEY-012)"), Loop->GetTextAsString(), *LoopText);
			const TSharedPtr<FDreamBuffer> File = FDreamEditorModule::Get().GetBufferManager().FindById(FileBufferId);
			TestTrue(TEXT("the file buffer is unchanged"), File.IsValid() && File->GetTextAsString() == FileText);
			const FString Diff = FProbes::Capture(Level, SavedPackages).Diff(*Before);
			TestTrue(*FString::Printf(TEXT("every probe holds after the run's chords: %s"), *Diff), Diff.IsEmpty());
			return true;
		});
		Steps.Finally(TEXT("remove the hook, restore the run settings"), [PreTick, bSavedRunPump, SavedPresentation]()
		{
			UDreamUserSettings::GetMutable()->bRunPump = *bSavedRunPump;
			UDreamUserSettings::GetMutable()->RunPresentation = *SavedPresentation;
			if (PreTick->IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().OnPreTick().Remove(*PreTick);
			}
		});
		Steps.Run(Done);
	});

	LatentIt("T-KEY-011 / T-TXN-010: AltGr text on the German, French and Polish Programmers layouts, no Undo Last Run", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare())
		{
			Done.Execute();
			return;
		}
		struct FAltGr { FKey Key; TCHAR Char; const TCHAR* Layout; };
		const TArray<FAltGr> Cases = {
			{ EKeys::Q, TEXT('@'), TEXT("German 00000407") }, { EKeys::Seven, TEXT('{'), TEXT("German") }, { EKeys::Eight, TEXT('['), TEXT("German") },
			{ EKeys::Hyphen, TEXT('\\'), TEXT("German") }, { EKeys::Zero, TEXT('@'), TEXT("French 0000040C") }, { EKeys::Four, TEXT('{'), TEXT("French") },
			{ EKeys::Five, TEXT('['), TEXT("French") }, { EKeys::Eight, TEXT('\\'), TEXT("French") }, { EKeys::Z, TCHAR(0x017C), TEXT("Polish Programmers 00000415") },
			{ EKeys::X, TCHAR(0x017A), TEXT("Polish Programmers") },
		};
		DreamTest::FDreamSteps Steps(*this);
		AddSettle(Steps);
		for (const EView View : { EView::Buffer, EView::Prompt })
		{
			Steps.Do(FString::Printf(TEXT("AltGr in the %s"), ViewName(View)), [this, View, Cases]()
			{
				if (!TestTrue(TEXT("focus"), FocusView(View)))
				{
					return false;
				}
				const FProbes Before = FProbes::Capture(Level, SavedPackages);
				const TSharedPtr<SDreamCodeView> Target = View == EView::Buffer ? Tab->GetActiveCodeView()
					: Tab->GetOutputArea()->GetReplStrip()->GetPromptView();
				const FString TextBefore = Target->GetText();
				FString Typed;
				FSlateApplication& App = FSlateApplication::Get();
				const uint32 User = App.GetUserIndexForKeyboard();
				for (const FAltGr& Case : Cases)
				{
					// AltGr is left Ctrl + right Alt on Windows; the layout gives the character (§4.9.3, RT-16).
					const FModifierKeysState AltGr = Mods(/*Ctrl*/ true, false, false, /*RightAlt*/ true);
					App.ProcessKeyDownEvent(FKeyEvent(Case.Key, AltGr, User, false, Case.Char, Case.Char));
					App.ProcessKeyCharEvent(FCharacterEvent(Case.Char, AltGr, User, false));
					App.ProcessKeyUpEvent(FKeyEvent(Case.Key, AltGr, User, false, Case.Char, Case.Char));
					Typed.AppendChar(Case.Char);
				}
				// The dead keys: Shift+acute (German) or AltGr+7 (French) then Space gives a backtick.
				App.ProcessKeyDownEvent(FKeyEvent(EKeys::Equals, Mods(false, true, false), User, false, 0, 0));
				App.ProcessKeyUpEvent(FKeyEvent(EKeys::Equals, Mods(false, true, false), User, false, 0, 0));
				App.ProcessKeyDownEvent(FKeyEvent(EKeys::SpaceBar, Mods(false, false, false), User, false, TEXT('`'), TEXT('`')));
				App.ProcessKeyCharEvent(FCharacterEvent(TEXT('`'), Mods(false, false, false), User, false));
				App.ProcessKeyUpEvent(FKeyEvent(EKeys::SpaceBar, Mods(false, false, false), User, false, TEXT('`'), TEXT('`')));
				Typed.AppendChar(TEXT('`'));
				const FString TextAfter = Target->GetText();
				TestTrue(*FString::Printf(TEXT("the %s holds the AltGr characters \"%s\" (now \"%s\")"), ViewName(View), *Typed, *TextAfter),
					TextAfter.Contains(Typed, ESearchCase::CaseSensitive));
				const FString Diff = FProbes::Capture(Level, SavedPackages).Diff(Before);
				TestTrue(*FString::Printf(TEXT("no Undo Last Run and no leak (RT-16): %s"), *Diff), Diff.IsEmpty());
				Target->SetTextNoUndo(TextBefore);
				return true;
			});
		}
		Steps.Do(TEXT("AltGr+Z and AltGr+X with the level viewport focused (T-TXN-010)"), [this]()
		{
			FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			const TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport();
			const TSharedPtr<SWidget> Widget = Viewport.IsValid() ? FindByType(Viewport->AsWidget(), TEXT("SViewport")) : nullptr;
			if (!Widget.IsValid())
			{
				AddInfo(TEXT("no level viewport is shown; the viewport pass of T-TXN-010 is skipped"));
				return true;
			}
			FSlateApplication::Get().SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
			const FProbes Before = FProbes::Capture(Level, SavedPackages);
			FSlateApplication& App = FSlateApplication::Get();
			const uint32 User = App.GetUserIndexForKeyboard();
			for (const TPair<FKey, TCHAR>& Case : { TPair<FKey, TCHAR>(EKeys::Z, TCHAR(0x017C)), TPair<FKey, TCHAR>(EKeys::X, TCHAR(0x017A)) })
			{
				const FModifierKeysState AltGr = Mods(true, false, false, true);
				App.ProcessKeyDownEvent(FKeyEvent(Case.Key, AltGr, User, false, Case.Value, Case.Value));
				App.ProcessKeyUpEvent(FKeyEvent(Case.Key, AltGr, User, false, Case.Value, Case.Value));
			}
			const FProbes After = FProbes::Capture(Level, SavedPackages);
			TestEqual(TEXT("Undo Last Run never fires from the Level Editor (RT-16)"), After.UndoneRuns, Before.UndoneRuns);
			TestEqual(TEXT("no TRANSACTION UNDO"), After.TransUndo, Before.TransUndo);
			return true;
		});
		Steps.Run(Done);
	});

	LatentIt("T-TXN-011: Ctrl+Z in the code view after a run undoes text, not the run's transaction", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const FString Label = FString::Printf(TEXT("DreamG5_label_%s"), *Sfx);
		// The fixture actors are transient, which EditorActorSubsystem.get_all_level_actors leaves out: find it by path.
		const FString ActorName = Level.Actors[5]->GetPathName();
		TSharedRef<DreamTest::FRunResult> Run = MakeShared<DreamTest::FRunResult>();
		TSharedRef<FProbes> AfterRun = MakeShared<FProbes>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		DreamTest::AddRun(Steps, TEXT("a run that renames a fixture actor (one undo step)"), [ActorName, Label]()
		{
			return DreamTest::MakeReplRequest(FString::Printf(TEXT(
				"import unreal\n"
				"g5_actor = unreal.find_object(None, '%s')\n"
				"g5_actor.set_actor_label('%s')"), *ActorName, *Label));
		}, Run);
		Steps.Do(TEXT("type in the script, then Ctrl+Z"), [this, Run, AfterRun, Label]()
		{
			if (!DreamTest::ExpectOutcome(*this, *Run, EDreamRunOutcome::Ok)
				|| !TestEqual(TEXT("the run renamed the actor"), Level.Actors[5]->GetActorLabel(), Label))
			{
				return false;
			}
			if (!TestTrue(TEXT("focus in the script"), FocusView(EView::Buffer)))
			{
				return false;
			}
			const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView();
			Code->SetSelection(FDreamTextSelection{ 0, FileText.Len(), 0, FileText.Len() });
			FSlateApplication& App = FSlateApplication::Get();
			const uint32 User = App.GetUserIndexForKeyboard();
			App.ProcessKeyDownEvent(FKeyEvent(EKeys::Q, Mods(false, false, false), User, false, 'q', 'q'));
			App.ProcessKeyCharEvent(FCharacterEvent(TEXT('q'), Mods(false, false, false), User, false));
			App.ProcessKeyUpEvent(FKeyEvent(EKeys::Q, Mods(false, false, false), User, false, 'q', 'q'));
			if (!TestEqual(TEXT("the typed q is in the script"), Code->GetText(), FileText + TEXT("q")))
			{
				return false;
			}
			*AfterRun = FProbes::Capture(Level, SavedPackages);
			App.ProcessKeyDownEvent(FKeyEvent(EKeys::Z, Mods(true, false, false), User, false, 0, 0));
			App.ProcessKeyUpEvent(FKeyEvent(EKeys::Z, Mods(true, false, false), User, false, 0, 0));
			TestEqual(TEXT("Ctrl+Z undid the typing (text undo)"), Code->GetText(), FileText);
			const FProbes After = FProbes::Capture(Level, SavedPackages);
			TestEqual(TEXT("no TRANSACTION UNDO: the undo count is unchanged"), After.TransUndo, AfterRun->TransUndo);
			TestEqual(TEXT("the run's rename stands"), Level.Actors[5]->GetActorLabel(), Label);
			return true;
		});
		Steps.Run(Done);
	});

#if PLATFORM_WINDOWS
	LatentIt("T-KEY-020: SendInput Alt+Enter runs the next statement three times, with no window menu, focus kept", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		if (!Prepare() || !DreamTest::RequireRunnablePython(*this) || !DreamTest::RequireEditorInFront(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab,
			FString::Printf(TEXT("g5_a_%s = 1\ng5_b_%s = 2\ng5_c_%s = 3"), *Sfx, *Sfx, *Sfx), TEXT("G5AltEnter"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		TSharedRef<FMenuLoopProbe> Probe = MakeShared<FMenuLoopProbe>();
		FWindowsApplication* WinApp = static_cast<FWindowsApplication*>(FSlateApplication::Get().GetPlatformApplication().Get());
		WinApp->AddMessageHandler(*Probe);
		TSharedRef<int32> FirstRun = MakeShared<int32>(0);
		TSharedRef<TSharedPtr<DreamTest::FDreamInputBotTicket>> Ticket = MakeShared<TSharedPtr<DreamTest::FDreamInputBotTicket>>();
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		Steps.Do(TEXT("caret on line 1, the Dream tab's window in front"), [this, Buffer]()
		{
			Tab->ActivateBuffer(Buffer->GetId());
			const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("the scratch tab's code view"), Code.IsValid()))
			{
				return false;
			}
			// OS keys go to the foreground window: make it the window that holds the code view (the Dream tab may float in
			// its own window), not just any editor window, whatever an earlier case left in front.
			void* Hwnd = DreamTest::FDreamTestProbes::GetOsWindowHandle(FSlateApplication::Get().FindWidgetWindow(Code.ToSharedRef()));
			if (!TestTrue(TEXT("the Dream tab's window is in front for SendInput"), DreamTest::FDreamInputBot::BringEditorToFront(Hwnd)))
			{
				return false;
			}
			Code->FocusEditor();
			Code->SetSelection(FDreamTextSelection{ 0, 0, 0, 0 });
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("the bot sends Alt+Enter three times"), [this, FirstRun, Ticket]()
		{
			const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView();
			if (!TestTrue(TEXT("keyboard focus is in the code view before the presses"), Code.IsValid() && FocusIsInside(Code.ToSharedRef())))
			{
				return false;
			}
			const FDreamRunRecord* Last = FDreamEditorModule::Get().GetRunService().GetLastFinished();
			*FirstRun = (Last ? Last->RunNumber : 0) + 1;
			const int32 First = *FirstRun;
			*Ticket = DreamTest::FDreamInputBot::Submit([First](DreamTest::FDreamInputBotContext& Bot)
			{
				if (!Bot.RequireForeground())
				{
					return;
				}
				for (int32 Press = 0; Press < 3 && !Bot.HasFailed(); ++Press)
				{
					Bot.Chord({ DreamTest::FDreamInputBot::VkAlt(), DreamTest::FDreamInputBot::VkReturn() });
					// The run starts at the next end of frame and ends a few frames later: wait for it, then for idle.
					const int32 Number = First + Press;
					Bot.WaitFor([Number](const DreamTest::FDreamProbeSnapshot& S) { return S.ActiveRunNumber >= Number; }, 10.0,
						FString::Printf(TEXT("Alt+Enter %d to start run #%d"), Press + 1, Number));
					Bot.WaitFor([](const DreamTest::FDreamProbeSnapshot& S) { return S.ActiveRunNumber == 0; }, 20.0, TEXT("the run to end"));
				}
			});
			return true;
		});
		Steps.Poll(TEXT("the bot's presses"), [Ticket]()
		{
			return DreamTest::IsTicketDone(Ticket->ToSharedRef()) ? DreamTest::EStep::Next : DreamTest::EStep::Wait;
		}, 90.0);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		Steps.Do(TEXT("check the runs, the caret and the menu probe"), [this, Buffer, FirstRun, Ticket, Probe]()
		{
			DreamTest::ReportTicket(*this, Ticket->ToSharedRef(), TEXT("T-KEY-020"));
			for (int32 Offset = 0; Offset < 3; ++Offset)
			{
				const FDreamRunRecord* Record = FDreamEditorModule::Get().GetRunService().FindRecord(*FirstRun + Offset);
				if (TestNotNull(*FString::Printf(TEXT("run #%d exists"), *FirstRun + Offset), Record))
				{
					TestEqual(TEXT("one statement per press"), Record->FirstLine, Offset + 1);
					TestEqual(TEXT("ok"), DreamTest::OutcomeText(Record->Outcome), DreamTest::OutcomeText(EDreamRunOutcome::Ok));
				}
			}
			int32 Line = -1, Column = -1;
			const TSharedPtr<SDreamCodeView> Code = Tab->GetActiveCodeView();
			if (Code.IsValid())
			{
				Code->GetCaret(Line, Column);
			}
			TestEqual(TEXT("the caret ends on the last statement"), Line, 2);
			TestTrue(TEXT("focus stays in the code view"), Code.IsValid() && FocusIsInside(Code.ToSharedRef()));
			TestEqual(TEXT("no WM_SYSCOMMAND SC_KEYMENU reached the editor's windows"), Probe->KeyMenu, 0);
			TestEqual(TEXT("no WM_ENTERMENULOOP"), Probe->EnterMenuLoop, 0);
			return true;
		});
		Steps.Finally(TEXT("remove the message probe"), [Probe]()
		{
			if (FSlateApplication::IsInitialized())
			{
				static_cast<FWindowsApplication*>(FSlateApplication::Get().GetPlatformApplication().Get())->RemoveMessageHandler(*Probe);
			}
		});
		Steps.Run(Done);
	});
#endif

	LatentIt("T-REPL-005: the history popup opens on Up, fills the prompt on Enter without running, and closes on Esc", DreamTest::LatentTimeout(),
		[this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid())
		{
			Done.Execute();
			return;
		}
		const FString EntryA = FString::Printf(TEXT("g5_hist_alpha_%s = 1"), *Sfx);
		const FString EntryB = FString::Printf(TEXT("g5_hist_beta_%s = 2"), *Sfx);
		TSharedRef<FString> PromptBefore = MakeShared<FString>(FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->GetTextAsString());
		TSharedRef<int32> LastRun = MakeShared<int32>(0);
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		AddSettle(Steps);
		Steps.Do(TEXT("two history entries and an empty prompt"), [this, EntryA, EntryB, LastRun]()
		{
			FDreamEditorModule::Get().GetReplHistory().Add(EntryA, EDreamHistorySource::Repl);
			FDreamEditorModule::Get().GetReplHistory().Add(EntryB, EDreamHistorySource::Repl);
			const FDreamRunRecord* Last = FDreamEditorModule::Get().GetRunService().GetLastFinished();
			*LastRun = Last ? Last->RunNumber : 0;
			if (!TestTrue(TEXT("focus in the prompt"), FocusView(EView::Prompt)))
			{
				return false;
			}
			Tab->GetOutputArea()->GetReplStrip()->GetPromptView()->SetTextNoUndo(FString());
			return true;
		});
		// A key that navigates the text needs the new text laid out first: the stock editable text layout has no line views
		// for it until the next tick (SlateEditableTextLayout ensures CurrentLineViewIndex, then indexes with -1).
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Up on an empty prompt opens the popup"), [this]()
		{
			FocusView(EView::Prompt);
			Send({ EKeys::Up, false, false, false, 0 });
			return TestTrue(TEXT("Up on an empty prompt opened the history popup (REPL-02)"), FSlateApplication::Get().AnyMenusVisible());
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Esc closes it"), [this]()
		{
			Send({ EKeys::Escape, false, false, false, 0 });
			return TestFalse(TEXT("Esc closed the popup"), FSlateApplication::Get().AnyMenusVisible());
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("two lines in the prompt, the caret on the second"), [this]()
		{
			FocusView(EView::Prompt);
			const TSharedPtr<SDreamCodeView> Prompt = Tab->GetOutputArea()->GetReplStrip()->GetPromptView();
			Prompt->SetTextNoUndo(TEXT("first = 1\nsecond = 2"));
			Prompt->SetSelection(FDreamTextSelection{ 1, 3, 1, 3 });
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Up with text moves the caret, not the popup"), [this]()
		{
			FocusView(EView::Prompt);
			const TSharedPtr<SDreamCodeView> Prompt = Tab->GetOutputArea()->GetReplStrip()->GetPromptView();
			Send({ EKeys::Up, false, false, false, 0 });
			int32 Line = -1, Column = -1;
			Prompt->GetCaret(Line, Column);
			TestFalse(TEXT("no popup with text in the prompt"), FSlateApplication::Get().AnyMenusVisible());
			return TestEqual(TEXT("Up moved the caret to line 1"), Line, 0);
		});
		Steps.Do(TEXT("a prefix of the first entry in the prompt"), [this, EntryA]()
		{
			const TSharedPtr<SDreamCodeView> Prompt = Tab->GetOutputArea()->GetReplStrip()->GetPromptView();
			const FString Prefix = EntryA.Left(EntryA.Find(TEXT("alpha")) + 5);
			Prompt->SetTextNoUndo(Prefix);
			Prompt->SetSelection(FDreamTextSelection{ 0, Prefix.Len(), 0, Prefix.Len() });
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("Ctrl+Up filters by the prompt text; Enter fills the prompt without running"), [this]()
		{
			FocusView(EView::Prompt);
			Send({ EKeys::Up, true, false, false, 0 });
			if (!TestTrue(TEXT("Ctrl+Up opened the filtered popup"), FSlateApplication::Get().AnyMenusVisible()))
			{
				return false;
			}
			return true;
		});
		Steps.WaitFrames(2);
		Steps.Do(TEXT("accept the entry"), [this, EntryA, LastRun]()
		{
			// The popup has keyboard focus on its list; Enter accepts the selected (first) entry.
			Send({ EKeys::Enter, false, false, false, TEXT('\r') });
			const TSharedPtr<SDreamCodeView> Prompt = Tab->GetOutputArea()->GetReplStrip()->GetPromptView();
			TestFalse(TEXT("the popup closed"), FSlateApplication::Get().AnyMenusVisible());
			TestEqual(TEXT("Enter filled the prompt with the filtered entry"), Prompt->GetText(), EntryA);
			TestTrue(TEXT("and ran nothing"), DreamTest::IsIdle() && (FDreamEditorModule::Get().GetRunService().GetLastFinished() == nullptr
				|| FDreamEditorModule::Get().GetRunService().GetLastFinished()->RunNumber == *LastRun));
			return true;
		});
		Steps.Finally(TEXT("restore the prompt"), [PromptBefore]()
		{
			FSlateApplication::Get().DismissAllMenus();
			FDreamEditorModule::Get().GetBufferManager().GetReplInputBuffer()->ResetText(*PromptBefore, dream::EditOrigin::Load);
		});
		Steps.Run(Done);
	});

	LatentIt("T-REPL-017: one prompt strip under Output and REPL, hidden under Problems; Focus REPL; the REPL tab shows only REPL runs",
		DreamTest::LatentTimeout(), [this](const FDoneDelegate& Done)
	{
		Tab = DreamTest::RequireTab(*this);
		if (!Tab.IsValid() || !DreamTest::RequireRunnablePython(*this))
		{
			Done.Execute();
			return;
		}
		const TSharedPtr<FDreamBuffer> Buffer = DreamTest::OpenScratch(*this, *Tab, FString::Printf(TEXT("print('g5 buffer run %s')"), *Sfx), TEXT("G5Strip"));
		if (!Buffer.IsValid())
		{
			Done.Execute();
			return;
		}
		BufferIds.Add(Buffer->GetId());
		const TSharedPtr<SDreamOutputArea> Area = Tab->GetOutputArea();
		TSharedRef<EDreamOutputTab> TabBefore = MakeShared<EDreamOutputTab>(Area->GetActiveTab());
		TSharedRef<DreamTest::FRunResult> ReplRun = MakeShared<DreamTest::FRunResult>();
		TSharedRef<DreamTest::FRunResult> BufferRun = MakeShared<DreamTest::FRunResult>();
		const FString Sx = Sfx;
		DreamTest::FDreamSteps Steps(*this);
		DreamTest::AddWaitIdle(Steps);
		// (chrome) PO-04 hides the REPL and Problems tabs and the prompt strip by default: this case checks the strip under
		// every tab (RV-18), so it turns the three View toggles on and restores them.
		struct FViewSaved
		{
			bool bSaved = false;
			bool bReplTab = false;
			bool bProblemsTab = false;
			bool bReplPrompt = false;
		};
		TSharedRef<FViewSaved> ViewSaved = MakeShared<FViewSaved>();
		Steps.Do(TEXT("show the REPL and Problems tabs and the REPL prompt (View toggles)"), [ViewSaved]()
		{
			const UDreamUserSettings* Settings = UDreamUserSettings::Get();
			ViewSaved->bReplTab = Settings->bShowReplTab;
			ViewSaved->bProblemsTab = Settings->bShowProblemsTab;
			ViewSaved->bReplPrompt = Settings->bShowReplPrompt;
			ViewSaved->bSaved = true;
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProblemsTab, true);
			UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, true);
			return true;
		});
		AddSettle(Steps);
		auto StripShown = [Area]()
		{
			// Shown means the strip and every widget between it and the output area are visible: the area collapses a box
			// around the strip for Problems, and a collapsed widget keeps the geometry of its last paint.
			const TSharedPtr<SDreamReplStrip> Strip = Area->GetReplStrip();
			if (!Strip.IsValid())
			{
				return false;
			}
			for (TSharedPtr<SWidget> W = Strip; W.IsValid() && W != StaticCastSharedPtr<SWidget>(Area); W = W->GetParentWidget())
			{
				if (!W->GetVisibility().IsVisible())
				{
					return false;
				}
			}
			return Strip->GetTickSpaceGeometry().GetLocalSize().Y > 0.f;
		};
		for (const EDreamOutputTab Which : { EDreamOutputTab::Output, EDreamOutputTab::Repl, EDreamOutputTab::Problems })
		{
			Steps.Do(TEXT("switch the output area"), [Area, Which]() { Area->SetActiveTab(Which); return true; });
			Steps.WaitFrames(2);
			Steps.Do(TEXT("check the strip"), [this, StripShown, Which]()
			{
				const bool bShown = StripShown();
				TestEqual(*FString::Printf(TEXT("the prompt strip is %s under tab %d (RV-18)"), Which == EDreamOutputTab::Problems ? TEXT("hidden") : TEXT("shown"),
					static_cast<int32>(Which)), bShown, Which != EDreamOutputTab::Problems);
				return true;
			});
		}
		Steps.Do(TEXT("Focus REPL from Problems"), [this, Area]()
		{
			Tab->ExecuteDreamCommand(EDreamCommand::FocusRepl);
			const TSharedPtr<SDreamCodeView> Prompt = Area->GetReplStrip()->GetPromptView();
			TestEqual(TEXT("Focus REPL selected the REPL tab"), static_cast<int32>(Area->GetActiveTab()), static_cast<int32>(EDreamOutputTab::Repl));
			return TestTrue(TEXT("and focused the prompt"), Prompt.IsValid() && FocusIsInside(Prompt.ToSharedRef()));
		});
		DreamTest::AddRun(Steps, TEXT("a REPL run"), [Sx]() { return DreamTest::MakeReplRequest(FString::Printf(TEXT("print('g5 repl run %s')"), *Sx)); }, ReplRun);
		DreamTest::AddRun(Steps, TEXT("a buffer run"), [Buffer]() { return DreamTest::MakeBufferRequest(*Buffer, EDreamRunRoute::Buffer, 1, 1); }, BufferRun);
		Steps.WaitFrames(3);
		Steps.Do(TEXT("check the views"), [this, Area, ReplRun, BufferRun]()
		{
			const TSharedPtr<SDreamOutputView> Output = Area->GetOutputView();
			const TSharedPtr<SDreamOutputView> Repl = Area->GetReplView();
			TestTrue(TEXT("Output shows the REPL run"), Output->GetLineSeqsForTest(ReplRun->RunNumber).Num() > 0);
			TestTrue(TEXT("Output shows the buffer run"), Output->GetLineSeqsForTest(BufferRun->RunNumber).Num() > 0);
			TestTrue(TEXT("the REPL tab shows the REPL run"), Repl->GetLineSeqsForTest(ReplRun->RunNumber).Num() > 0);
			TestEqual(TEXT("the REPL tab does not show the buffer run"), Repl->GetLineSeqsForTest(BufferRun->RunNumber).Num(), 0);
			TestTrue(TEXT("both views show the REPL run from the same store lines"), Repl->GetLineSeqsForTest(ReplRun->RunNumber)
				== Output->GetLineSeqsForTest(ReplRun->RunNumber));
			return true;
		});
		Steps.Finally(TEXT("restore the output tab"), [Area, TabBefore]()
		{
			Area->SetActiveTab(*TabBefore);
		});
		Steps.Finally(TEXT("restore the View toggles"), [ViewSaved]()
		{
			if (ViewSaved->bSaved && UObjectInitialized())
			{
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplTab, ViewSaved->bReplTab);
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ProblemsTab, ViewSaved->bProblemsTab);
				UDreamUserSettings::SetViewSetting(EDreamViewSetting::ReplPrompt, ViewSaved->bReplPrompt);
			}
		});
		Steps.Run(Done);
	});
}
