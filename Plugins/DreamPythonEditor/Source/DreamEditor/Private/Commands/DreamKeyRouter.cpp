// SPDX-License-Identifier: Apache-2.0
// Key routing for Dream widgets (TDD §4.9.1, §4.9.3, §4.9.4, D8). dream::Keymap decides; this file turns FKeyEvents into
// strokes, builds the bindings from FDreamCommands' active chords and executes through IDreamCommandTarget, so a bound chord
// is consumed even when it cannot execute (we never route through FUICommandList::ProcessCommandBindings, §4.9.1).
#include "Commands/DreamKeyRouter.h"

#include "Commands/DreamNumpadEnter.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamKeyRouterLocal
{
	uint8 ModsOf(bool bShift, bool bCtrl, bool bAlt, bool bCmd)
	{
		uint8 Mods = dream::Mod_None;
		Mods |= bShift ? dream::Mod_Shift : 0;
		Mods |= bCtrl ? dream::Mod_Ctrl : 0;
		Mods |= bAlt ? dream::Mod_Alt : 0;
		Mods |= bCmd ? dream::Mod_Cmd : 0;
		return Mods;
	}

	bool IsLetterOrDigitKey(const FKey& Key)
	{
		static const FKey Digits[] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five,
			EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
		for (const FKey& Digit : Digits)
		{
			if (Key == Digit)
			{
				return true;
			}
		}
		const FString Name = Key.GetFName().ToString();
		return Name.Len() == 1 && Name[0] >= TEXT('A') && Name[0] <= TEXT('Z');
	}

	bool IsOemKey(const FKey& Key)
	{
		static const FKey Oem[] = { EKeys::Semicolon, EKeys::Equals, EKeys::Comma, EKeys::Underscore, EKeys::Hyphen,
			EKeys::Period, EKeys::Slash, EKeys::Tilde, EKeys::LeftBracket, EKeys::Backslash, EKeys::RightBracket,
			EKeys::Apostrophe, EKeys::Ampersand, EKeys::Asterix, EKeys::Caret, EKeys::Colon };
		for (const FKey& K : Oem)
		{
			if (Key == K)
			{
				return true;
			}
		}
		return false;
	}

	bool IsFunctionKey(const FKey& Key)
	{
		static const FKey Fn[] = { EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7, EKeys::F8,
			EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12 };
		for (const FKey& K : Fn)
		{
			if (Key == K)
			{
				return true;
			}
		}
		return false;
	}
}

FDreamKeyRouter& FDreamKeyRouter::Get()
{
	static FDreamKeyRouter Instance;
	return Instance;
}

dream::KeyCode FDreamKeyRouter::GetKeyCode(const FKey& Key) const
{
	if (!Key.IsValid())
	{
		return 0;
	}
	if (const dream::KeyCode* Found = KeyCodes.Find(Key))
	{
		return *Found;
	}
	const dream::KeyCode Code = static_cast<dream::KeyCode>(KeyCodes.Num() + 1);
	KeyCodes.Add(Key, Code);
	return Code;
}

dream::KeyClass FDreamKeyRouter::ClassifyKey(const FKey& Key)
{
	using namespace DreamKeyRouterLocal;
	if (Key == EKeys::Left || Key == EKeys::Right || Key == EKeys::Up || Key == EKeys::Down || Key == EKeys::Home
		|| Key == EKeys::End || Key == EKeys::PageUp || Key == EKeys::PageDown)
	{
		return dream::KeyClass::Navigation;
	}
	if (Key == EKeys::BackSpace || Key == EKeys::Delete || Key == EKeys::Tab || Key == EKeys::Enter || Key == EKeys::Escape)
	{
		return dream::KeyClass::Editing;
	}
	if (IsFunctionKey(Key))
	{
		return dream::KeyClass::Function;
	}
	if (IsLetterOrDigitKey(Key) || IsOemKey(Key))
	{
		return dream::KeyClass::LetterDigitOem;
	}
	return dream::KeyClass::Other;
}

void FDreamKeyRouter::Rebuild()
{
	using namespace DreamKeyRouterLocal;

	if (!ChordChangedHandle.IsValid())
	{
		ChordChangedHandle = FInputBindingManager::Get().RegisterUserDefinedChordChanged(
			FOnUserDefinedChordChanged::FDelegate::CreateLambda([this](const FUICommandInfo& Info)
			{
				if (Info.GetBindingContext() == FDreamCommands::ContextName)
				{
					Rebuild();   // §4.9.2: rebinding in Keyboard Shortcuts applies without a restart (T-KEY-005)
				}
			}));
	}

	std::vector<dream::Binding> Bindings;
	if (FDreamCommands::IsRegistered())
	{
		const FDreamCommands& Commands = FDreamCommands::Get();
		for (int32 Index = 1; Index < static_cast<int32>(EDreamCommand::Count); ++Index)
		{
			const EDreamCommand Command = static_cast<EDreamCommand>(Index);
			if (Command == EDreamCommand::OpenDream)
			{
				continue;   // global, through the main frame bindings (§11.6.1)
			}
			const TSharedPtr<FUICommandInfo> Info = Commands.GetInfo(Command);
			if (!Info.IsValid())
			{
				continue;
			}
			for (const EMultipleKeyBindingIndex Slot : { EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary })
			{
				const FInputChord& Chord = *Info->GetActiveChord(Slot);
				if (!Chord.IsValidChord())
				{
					continue;
				}
				dream::Binding Binding;
				Binding.Command = static_cast<dream::CommandId>(Index);
				Binding.Key = GetKeyCode(Chord.Key);
				Binding.Mods = ModsOf(Chord.bShift, Chord.bCtrl, Chord.bAlt, Chord.bCmd);
				Binding.RequiredContext = dream::Ctx_Focused;
				// Text commands act on the focused code view; in an output view Ctrl+Z must not undo the script (the consume
				// list still takes the key there).
				Binding.ExcludedContext = FDreamCommands::IsCodeViewCommand(Command) ? static_cast<uint32>(dream::Ctx_Output) : 0u;
				// (debug) R-q1-1: the debugger keys never fire from the side bar or an output view (the tree's F5 refreshes).
				if (Command == EDreamCommand::DebugRun || Command == EDreamCommand::ToggleBreakpoint || Command == EDreamCommand::DebugStepOver
					|| Command == EDreamCommand::DebugStepInto || Command == EDreamCommand::DebugStepOut)
				{
					Binding.ExcludedContext |= static_cast<uint32>(dream::Ctx_Output);
				}
				Bindings.push_back(Binding);
			}
		}
	}

	// Numpad Enter (G7): FInputChord cannot express it, so the binding exists only while the Win64 hooks are live (§4.9.2).
	// It never matches main Enter, and main Enter never falls back to running (§4.9.3).
	if (FDreamNumpadEnter::Get().IsAvailable())
	{
		dream::Binding Numpad;
		Numpad.Command = static_cast<dream::CommandId>(EDreamCommand::RunSelectionOrBuffer);
		Numpad.Key = GetKeyCode(EKeys::Enter);
		Numpad.Mods = dream::Mod_None;
		Numpad.bNumpadEnterOnly = true;
		Numpad.RequiredContext = dream::Ctx_Focused;
		Bindings.push_back(Numpad);
	}
	// (run, PO-06 R-q1-2, the owner's answer) Main Enter over a selection in an editable buffer view runs it (on by default).
	// A plain Enter binding matches Numpad Enter too, which runs the same command. Shift+Enter, every Enter without a
	// selection and Enter in a read-only view (a run in progress, a read-only file) stay as before.
	if (bMainEnterRunsSelection)
	{
		dream::Binding MainEnter;
		MainEnter.Command = static_cast<dream::CommandId>(EDreamCommand::RunSelectionOrBuffer);
		MainEnter.Key = GetKeyCode(EKeys::Enter);
		MainEnter.Mods = dream::Mod_None;
		MainEnter.RequiredContext = dream::Ctx_Focused | dream::Ctx_Editable | dream::Ctx_Buffer | dream::Ctx_HasSelection;
		MainEnter.ExcludedContext = dream::Ctx_Prompt | dream::Ctx_Output | dream::Ctx_PopupOpen;
		Bindings.push_back(MainEnter);
	}
	Keymap.SetBindings(MoveTemp(Bindings));

	// §4.9.4 consume list: keys a Dream view always handles, even read-only, so they never reach the Level Editor (D8).
	const uint8 Ctrl = dream::Mod_Ctrl;
	const uint8 CtrlShift = dream::Mod_Ctrl | dream::Mod_Shift;
	std::vector<dream::Chord> Consume =
	{
		{ GetKeyCode(EKeys::Z), Ctrl }, { GetKeyCode(EKeys::Y), Ctrl }, { GetKeyCode(EKeys::Z), CtrlShift },
		{ GetKeyCode(EKeys::C), Ctrl }, { GetKeyCode(EKeys::V), Ctrl }, { GetKeyCode(EKeys::X), Ctrl },
		{ GetKeyCode(EKeys::A), Ctrl }, { GetKeyCode(EKeys::D), Ctrl }, { GetKeyCode(EKeys::S), Ctrl },
		{ GetKeyCode(EKeys::W), Ctrl },
	};
	// (debug) R-q1-1: the debugger chords are excluded in Ctx_Output (the side bar, the output views), but they must not
	// bubble to Unreal there either (D8): consumed without a command. The tree reads FindCommand first, so its F5 refreshes.
	if (FDreamCommands::IsRegistered())
	{
		for (const EDreamCommand Command : { EDreamCommand::DebugRun, EDreamCommand::ToggleBreakpoint, EDreamCommand::DebugStepOver,
			EDreamCommand::DebugStepInto, EDreamCommand::DebugStepOut })
		{
			const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command);
			if (!Info.IsValid())
			{
				continue;
			}
			for (const EMultipleKeyBindingIndex Slot : { EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary })
			{
				const FInputChord& Chord = *Info->GetActiveChord(Slot);
				if (Chord.IsValidChord())
				{
					Consume.push_back({ GetKeyCode(Chord.Key), DreamKeyRouterLocal::ModsOf(Chord.bShift, Chord.bCtrl, Chord.bAlt, Chord.bCmd) });
				}
			}
		}
	}
	Keymap.SetConsumeList(MoveTemp(Consume));

	// §4.9.4 pass-through: these always bubble to Unreal.
	const uint8 Alt = dream::Mod_Alt;
	const uint8 Shift = dream::Mod_Shift;
	const uint8 CtrlAlt = dream::Mod_Ctrl | dream::Mod_Alt;
	PassThroughChords =
	{
		{ GetKeyCode(EKeys::Tab), Ctrl }, { GetKeyCode(EKeys::Tab), CtrlShift },
		{ GetKeyCode(EKeys::P), Ctrl }, { GetKeyCode(EKeys::B), Ctrl },
		{ GetKeyCode(EKeys::F1), dream::Mod_None }, { GetKeyCode(EKeys::F1), Alt }, { GetKeyCode(EKeys::F1), Shift },
		{ GetKeyCode(EKeys::O), Ctrl }, { GetKeyCode(EKeys::S), CtrlShift },
		{ GetKeyCode(EKeys::S), static_cast<uint8>(CtrlAlt | Shift) }, { GetKeyCode(EKeys::W), CtrlShift },
		{ GetKeyCode(EKeys::E), CtrlAlt }, { GetKeyCode(EKeys::K), CtrlAlt }, { GetKeyCode(EKeys::P), CtrlAlt },
		{ GetKeyCode(EKeys::Tilde), Ctrl }, { GetKeyCode(EKeys::Tilde), Alt },
		{ GetKeyCode(EKeys::P), Alt }, { GetKeyCode(EKeys::S), Alt },
		{ GetKeyCode(EKeys::Pause), dream::Mod_None },
	};
	Keymap.SetPassThrough(std::vector<dream::Chord>(PassThroughChords.GetData(), PassThroughChords.GetData() + PassThroughChords.Num()));
	Keymap.SetConsumeExtension(true);
}

void FDreamKeyRouter::SetMainEnterRunsSelection(bool bOn)
{
	if (bMainEnterRunsSelection != bOn)
	{
		bMainEnterRunsSelection = bOn;
		Rebuild();
	}
}

void FDreamKeyRouter::Shutdown()
{
	if (ChordChangedHandle.IsValid())
	{
		FInputBindingManager::Get().UnregisterUserDefinedChordChanged(ChordChangedHandle);
		ChordChangedHandle.Reset();
	}
	ConsumedDownKeys.Reset();
}

dream::Stroke FDreamKeyRouter::ToStroke(const FKeyEvent& KeyEvent) const
{
	const FModifierKeysState& Modifiers = KeyEvent.GetModifierKeys();
	const FKey Key = KeyEvent.GetKey();

	dream::Stroke Stroke;
	Stroke.Key = GetKeyCode(Key);
	// Ctrl+Alt chords use the left Alt only (RT-16, §4.9.3): Windows reports AltGr as LeftCtrl+RightAlt, so the right Alt
	// counts as Alt only without Ctrl (Right Alt+Enter is Run line and advance on a layout without AltGr, RUN-26).
	const bool bAlt = Modifiers.IsLeftAltDown() || (Modifiers.IsRightAltDown() && !Modifiers.IsControlDown());
	Stroke.Mods = DreamKeyRouterLocal::ModsOf(Modifiers.IsShiftDown(), Modifiers.IsControlDown(), bAlt, Modifiers.IsCommandDown());
	Stroke.bRightAlt = Modifiers.IsRightAltDown();
	Stroke.Char = static_cast<char32_t>(KeyEvent.GetCharacter());
	Stroke.Class = ClassifyKey(Key);
	if (Stroke.Class == dream::KeyClass::Other && Stroke.Char >= 0x20 && Stroke.Char != 0x7F)
	{
		Stroke.Class = dream::KeyClass::LetterDigitOem;   // layout-specific OEM keys (non-US keyboards)
	}
	Stroke.bRepeat = KeyEvent.IsRepeat();
	Stroke.bNumpadEnter = (Key == EKeys::Enter) && FDreamNumpadEnter::Get().IsCurrentEnterNumpad();
	return Stroke;
}

bool FDreamKeyRouter::IsPassThrough(const dream::Stroke& Stroke) const
{
	for (const dream::Chord& Chord : PassThroughChords)
	{
		if (Chord.Key == Stroke.Key && Chord.Mods == Stroke.Mods && !Stroke.bRightAlt)
		{
			return true;
		}
	}
	return false;
}

FReply FDreamKeyRouter::RoutePre(const FKeyEvent& KeyEvent, uint32 ContextMask, IDreamCommandTarget& Target)
{
	const dream::Stroke Stroke = ToStroke(KeyEvent);
	const dream::Resolution Resolution = Keymap.Resolve(Stroke, ContextMask, FPlatformTime::Seconds(), false, false);
	if (Resolution.Kind != dream::ResolveKind::Command || Resolution.Command == 0)
	{
		// AltGr text, primitives, printable keys and everything else: the stock handler runs (§4.15 row 3).
		return FReply::Unhandled();
	}

	const EDreamCommand Command = static_cast<EDreamCommand>(Resolution.Command);
	FText Reason;
	if (Target.CanExecuteDreamCommand(Command, Reason))
	{
		Target.ExecuteDreamCommand(Command);
	}
	else
	{
		Target.ShowCommandRefusal(Command, Reason);
	}
	ConsumedDownKeys.Add(KeyEvent.GetKey());
	return FReply::Handled();   // a bound chord is consumed even when it cannot execute (§4.9.4)
}

FReply FDreamKeyRouter::RouteBubble(const FKeyEvent& KeyEvent, uint32 ContextMask)
{
	const dream::Stroke Stroke = ToStroke(KeyEvent);
	if (IsPassThrough(Stroke) && Keymap.FindCommand(Stroke.Key, Stroke.Mods, ContextMask) == 0)
	{
		return FReply::Unhandled();
	}
	const dream::Resolution Resolution = Keymap.Resolve(Stroke, ContextMask, FPlatformTime::Seconds(), false, false);
	if (Resolution.Kind == dream::ResolveKind::Unhandled || Resolution.Kind == dream::ResolveKind::Pending)
	{
		return FReply::Unhandled();
	}
	ConsumedDownKeys.Add(KeyEvent.GetKey());
	return FReply::Handled();
}

bool FDreamKeyRouter::ShouldConsumeKeyUp(const FKeyEvent& KeyEvent) const
{
	// Key-ups of consumed key-downs are consumed too (§4.9.1). The set is cleared per key as its key-up arrives.
	return ConsumedDownKeys.Remove(KeyEvent.GetKey()) > 0;
}

FText FDreamKeyRouter::GetChordText(EDreamCommand Command) const
{
	if (!FDreamCommands::IsRegistered())
	{
		return FText::GetEmpty();
	}
	const TSharedPtr<FUICommandInfo> Info = FDreamCommands::Get().GetInfo(Command);
	if (!Info.IsValid())
	{
		return FText::GetEmpty();
	}
	FText Text = Info->GetInputText();
	if (Command == EDreamCommand::RunSelectionOrBuffer && FDreamNumpadEnter::Get().IsAvailable())
	{
		Text = Text.IsEmpty() ? LOCTEXT("NumpadEnterChord", "Num Enter")
			: FText::Format(LOCTEXT("ChordWithNumpadEnter", "{0}; Num Enter"), Text);
	}
	return Text;
}

#undef LOCTEXT_NAMESPACE
