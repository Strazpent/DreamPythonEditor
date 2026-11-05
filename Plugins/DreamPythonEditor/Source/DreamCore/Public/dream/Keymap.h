// SPDX-License-Identifier: Apache-2.0
// Dream core: key resolution for Dream widgets (TDD §4.9.1 flowchart, §4.9.2, §4.9.4 consume list). Tier 1 (T-KEY-001..003).
// Game thread. DreamEditor turns an FKeyEvent into a Stroke and executes what Resolve returns.
// (slice A choice) Two-stroke sequences and Shift Shift (M2/M4) are not in slice A: Resolve never returns Pending
// and OnKeyUp is absent. KeyCode values are assigned by DreamEditor (Commands/DreamKeyRouter.h maps FKey to codes),
// and each Stroke carries its KeyClass so the core needs no key table.
#pragma once

#include "dream/Export.h"

#include <cstdint>
#include <vector>

namespace dream
{
	using KeyCode = uint32_t;          // 0 = none; values chosen by the host
	using CommandId = uint32_t;        // 0 = none; values chosen by the host (EDreamCommand in DreamEditor)

	enum KeyMods : uint8_t
	{
		Mod_None  = 0,
		Mod_Shift = 1 << 0,
		Mod_Ctrl  = 1 << 1,
		Mod_Alt   = 1 << 2,            // left Alt; a right Alt arrives as bRightAlt (AltGr, RT-16)
		Mod_Cmd   = 1 << 3,
	};

	/** What kind of key a stroke is, for the consume list and the extension rule (§4.9.4). */
	enum class KeyClass : uint8_t
	{
		Other,                         // unbound F-keys, Pause, ...
		LetterDigitOem,                // A-Z, 0-9, OEM punctuation: the extension applies to Ctrl chords on these
		Navigation,                    // arrows, Home, End, PageUp, PageDown
		Editing,                       // Backspace, Delete, Tab, Enter, Escape
		Function,                      // F1..F24
	};

	/** One key-down (§4.9.2). Char is the character the key would type (MapVirtualKey), 0 when none. */
	struct Stroke
	{
		KeyCode Key = 0;
		uint8_t Mods = Mod_None;
		char32_t Char = 0;
		KeyClass Class = KeyClass::Other;
		bool bRightAlt = false;
		bool bNumpadEnter = false;     // from FDreamNumpadEnter (G7); false when unknown
		bool bRepeat = false;
	};

	/** Context bits (§4.9.2). A binding acts only when (Mask & Required) == Required and (Mask & Excluded) == 0. */
	enum ContextBit : uint32_t
	{
		Ctx_Focused       = 1u << 0,
		Ctx_Editable      = 1u << 1,
		Ctx_Prompt        = 1u << 2,
		Ctx_Output        = 1u << 3,
		Ctx_PausedSource  = 1u << 4,
		Ctx_HasSelection  = 1u << 5,
		Ctx_MultiCaret    = 1u << 6,
		Ctx_SnippetActive = 1u << 7,
		Ctx_PopupOpen     = 1u << 8,
		Ctx_RunActive     = 1u << 9,
		Ctx_Paused        = 1u << 10,
		Ctx_TaskRunning   = 1u << 11,
		Ctx_Buffer        = 1u << 12,  // (slice A choice) a code-area buffer view has focus
	};

	struct Binding
	{
		CommandId Command = 0;
		KeyCode Key = 0;
		uint8_t Mods = Mod_None;
		bool bNumpadEnterOnly = false; // binds Numpad Enter only (G7); never matches main Enter
		uint32_t RequiredContext = 0;
		uint32_t ExcludedContext = 0;
	};

	/** A chord as data, for the pass-through and consume lists. */
	struct Chord
	{
		KeyCode Key = 0;
		uint8_t Mods = Mod_None;

		bool operator==(const Chord&) const = default;
	};

	enum class ResolveKind : uint8_t
	{
		Unhandled,                     // bubble to Unreal (pass-through, unbound F-keys, Alt+letter)
		Command,                       // execute Resolution.Command if it can; Handled either way (§4.9.4)
		Pending,                       // a sequence is pending (M2/M4; never in slice A)
		TextInput,                     // printable or AltGr text: let OnKeyChar insert it; Handled
		ConsumeOnly,                   // consume list or the extension: primitive or no-op; Handled
		ToSubHandler,                  // the prompt/popup/snippet sub-handler wants it; Handled by it
	};

	struct Resolution
	{
		ResolveKind Kind = ResolveKind::Unhandled;
		CommandId Command = 0;
	};

	class DREAMCORE_API Keymap
	{
	public:
		Keymap();

		/** Replaces the bindings (rebuilt at start and on RegisterUserDefinedChordChanged, §4.9.2). Later entries win ties. */
		void SetBindings(std::vector<Binding> InBindings);
		/** Chords that always bubble to Unreal (§4.9.4 pass-through row). */
		void SetPassThrough(std::vector<Chord> InChords);
		/** Chords consumed without an action (§4.9.4 consume list: Ctrl+Z/Y/C/V/X/A/D/S/W, Ctrl+Shift+Z, ...). */
		void SetConsumeList(std::vector<Chord> InChords);
		/** The proposed extension: unbound Ctrl, Ctrl+Shift or left-Ctrl+Alt chords on LetterDigitOem keys are consumed. */
		void SetConsumeExtension(bool bEnabled);

		/**
		 * The §4.9.1 resolution order: composing IME -> Handled (ConsumeOnly); right Alt with a printable char -> TextInput;
		 * sub-handler wants it -> ToSubHandler; bound for ContextMask -> Command; consume list, Navigation/Editing keys or
		 * printable -> ConsumeOnly or TextInput; pass-through -> Unhandled; extension -> ConsumeOnly; else Unhandled.
		 * Printable: Char >= 0x20, not 0x7F, with no Ctrl and no left Alt.
		 */
		Resolution Resolve(const Stroke& Key, uint32_t ContextMask, double NowSec, bool bComposing, bool bSubHandlerWants) const;

		/** The command bound to a chord in a context, or 0 (for menus and tooltips). */
		CommandId FindCommand(KeyCode Key, uint8_t Mods, uint32_t ContextMask) const;

		static bool IsPrintable(const Stroke& Key);

	private:
		std::vector<Binding> Bindings;
		std::vector<Chord> PassThrough;
		std::vector<Chord> ConsumeList;
		bool bConsumeExtension = true;
	};
}
