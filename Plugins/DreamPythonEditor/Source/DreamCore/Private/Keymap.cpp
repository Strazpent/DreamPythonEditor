// SPDX-License-Identifier: Apache-2.0
// Dream core: key resolution (TDD §4.9.1 flowchart, §4.9.3 AltGr, §4.9.4 consume list, pass-through and extension).
#include "dream/Keymap.h"

#include <algorithm>
#include <utility>

namespace dream::keymap_detail
{
	bool ContextMatches(const Binding& B, uint32_t ContextMask)
	{
		return (ContextMask & B.RequiredContext) == B.RequiredContext && (ContextMask & B.ExcludedContext) == 0;
	}

	bool InList(const std::vector<Chord>& List, KeyCode Key, uint8_t Mods)
	{
		return std::find(List.begin(), List.end(), Chord{Key, Mods}) != List.end();
	}

	bool IsPrintableChar(char32_t C)
	{
		return C >= 0x20 && C != 0x7F;
	}
}

namespace dream
{
	Keymap::Keymap() = default;

	void Keymap::SetBindings(std::vector<Binding> InBindings)
	{
		Bindings = std::move(InBindings);
	}

	void Keymap::SetPassThrough(std::vector<Chord> InChords)
	{
		PassThrough = std::move(InChords);
	}

	void Keymap::SetConsumeList(std::vector<Chord> InChords)
	{
		ConsumeList = std::move(InChords);
	}

	void Keymap::SetConsumeExtension(bool bEnabled)
	{
		bConsumeExtension = bEnabled;
	}

	bool Keymap::IsPrintable(const Stroke& Key)
	{
		return keymap_detail::IsPrintableChar(Key.Char) && (Key.Mods & (Mod_Ctrl | Mod_Alt | Mod_Cmd)) == 0;
	}

	Resolution Keymap::Resolve(const Stroke& Key, uint32_t ContextMask, double NowSec, bool bComposing, bool bSubHandlerWants) const
	{
		(void)NowSec;   // Deferred (M2/M4): two-stroke sequences (Pending) and Shift Shift through OnKeyUp (A11)
		if (bComposing)
		{
			return Resolution{ResolveKind::ConsumeOnly, 0};   // the IME owns the key
		}
		// AltGr arrives as LeftCtrl+RightAlt: right Alt with a printable character is always text (RT-16, T-KEY-002).
		if (Key.bRightAlt && keymap_detail::IsPrintableChar(Key.Char))
		{
			return Resolution{ResolveKind::TextInput, 0};
		}
		if (bSubHandlerWants)
		{
			return Resolution{ResolveKind::ToSubHandler, 0};
		}
		for (auto It = Bindings.rbegin(); It != Bindings.rend(); ++It)
		{
			const Binding& B = *It;
			if (B.Key != Key.Key || B.Mods != Key.Mods || !keymap_detail::ContextMatches(B, ContextMask))
			{
				continue;
			}
			if (B.bNumpadEnterOnly && !Key.bNumpadEnter)
			{
				continue;   // the Numpad Enter binding never matches main Enter (G7)
			}
			return Resolution{ResolveKind::Command, B.Command};   // consumed even when it cannot execute (§4.9.4)
		}
		if (keymap_detail::InList(ConsumeList, Key.Key, Key.Mods))
		{
			return Resolution{ResolveKind::ConsumeOnly, 0};
		}
		if (IsPrintable(Key))
		{
			return Resolution{ResolveKind::TextInput, 0};
		}
		// Pass-through chords bubble even though their key class (Ctrl+Tab: Editing) would be consumed below.
		if (keymap_detail::InList(PassThrough, Key.Key, Key.Mods))
		{
			return Resolution{ResolveKind::Unhandled, 0};
		}
		if (Key.Class == KeyClass::Navigation || Key.Class == KeyClass::Editing)
		{
			return Resolution{ResolveKind::ConsumeOnly, 0};   // the hard-wired primitives (§4.9.4)
		}
		if (bConsumeExtension && Key.Class == KeyClass::LetterDigitOem && (Key.Mods & Mod_Ctrl) != 0 && (Key.Mods & Mod_Cmd) == 0)
		{
			const uint8_t Rest = static_cast<uint8_t>(Key.Mods & ~Mod_Ctrl);
			if (Rest == Mod_None || Rest == Mod_Shift || Rest == Mod_Alt)
			{
				return Resolution{ResolveKind::ConsumeOnly, 0};   // the proposed extension (§4.9.4)
			}
		}
		return Resolution{ResolveKind::Unhandled, 0};
	}

	CommandId Keymap::FindCommand(KeyCode Key, uint8_t Mods, uint32_t ContextMask) const
	{
		for (auto It = Bindings.rbegin(); It != Bindings.rend(); ++It)
		{
			if (It->Key == Key && It->Mods == Mods && !It->bNumpadEnterOnly && keymap_detail::ContextMatches(*It, ContextMask))
			{
				return It->Command;
			}
		}
		return 0;
	}
}
