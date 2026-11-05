// SPDX-License-Identifier: Apache-2.0
// Key routing for every Dream widget (TDD §4.9.1 event path, §4.9.3 AltGr, §4.9.4 consume list, D8). Game thread.
// Body: DreamKeyRouter.cpp (shell slice). Code views call Route from SMultiLineEditableText's OnKeyDownHandler, which
// runs before the stock handling (§4.15 row 3), so whatever Route handles never reaches the Level Editor or MainFrame.
#pragma once

#include "CoreMinimal.h"
#include "Commands/DreamCommands.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "dream/Keymap.h"

struct FKeyEvent;

/** Implemented by the widgets that execute commands: SDreamTab (buffers, run, output) and SDreamReplStrip (prompt). */
class IDreamCommandTarget
{
public:
	virtual ~IDreamCommandTarget() = default;
	/** False with a status-bar reason ("Run in progress", "No statement on this line") when it cannot execute now. */
	virtual bool CanExecuteDreamCommand(EDreamCommand Command, FText& OutReason) const = 0;
	virtual void ExecuteDreamCommand(EDreamCommand Command) = 0;
	/** Shows a refusal reason in the Dream status strip (§4.9.4: "the status bar says why"). */
	virtual void ShowCommandRefusal(EDreamCommand Command, const FText& Reason) = 0;
};

class DREAMEDITOR_API FDreamKeyRouter
{
public:
	static FDreamKeyRouter& Get();

	/**
	 * Builds the dream::Keymap from FDreamCommands' active chords, the consume list and pass-through list of §4.9.4.
	 * Called at module start and on FInputBindingManager::RegisterUserDefinedChordChanged (§4.9.2).
	 */
	void Rebuild();

	/** FKeyEvent -> dream::Stroke: key code, modifiers (Mod_Alt from the left Alt, or the right Alt without Ctrl, so AltGr
	 *  never forms a Ctrl+Alt chord), character, KeyClass, right Alt, repeat,
	 *  and bNumpadEnter from FDreamNumpadEnter::IsCurrentEnterNumpad() (Commands/DreamNumpadEnter.h, §4.9.3 G7). Rebuild adds
	 *  a bNumpadEnterOnly binding for RunSelectionOrBuffer only while FDreamNumpadEnter::IsAvailable() (FInputChord cannot
	 *  express Numpad Enter, §4.9.2); otherwise Numpad Enter is main Enter. */
	dream::Stroke ToStroke(const FKeyEvent& KeyEvent) const;

	/**
	 * (slice A choice) Plain mode routes a key-down in two passes, because the stock SMultiLineEditableText owns the editing
	 * primitives (arrows, Home/End, PgUp/PgDn, Backspace, Delete, Enter, Ctrl+C/V/X/A/Z/Y) and IME:
	 *
	 * RoutePre, from the text widget's OnKeyDownHandler (runs before the stock handling,
	 * Engine/Source/Runtime/Slate/Private/Widgets/Text/SMultiLineEditableText.cpp:771-783): a bound chord for ContextMask
	 * executes through Target when CanExecute, else Target shows the reason; Handled either way (§4.9.4). AltGr text and
	 * everything else return Unhandled, so the stock handler runs.
	 */
	FReply RoutePre(const FKeyEvent& KeyEvent, uint32 ContextMask, IDreamCommandTarget& Target);

	/**
	 * RouteBubble, from SDreamCodeView's own OnKeyDown, which sees only key-downs the stock widget left unhandled: the consume
	 * list (Ctrl+D, Ctrl+S, Ctrl+W, Esc, Tab, ...), printable keys and the proposed extension are Handled, so they never reach
	 * the Level Editor (C12, D8); pass-through chords and the rest stay Unhandled and bubble to Unreal.
	 */
	FReply RouteBubble(const FKeyEvent& KeyEvent, uint32 ContextMask);

	/** True when the key-up's key-down was consumed by either pass (§4.9.1: consumed key-downs consume their key-ups). */
	bool ShouldConsumeKeyUp(const FKeyEvent& KeyEvent) const;

	/** The chord text for menus and tooltips ("Ctrl+Enter"). */
	FText GetChordText(EDreamCommand Command) const;

	/**
	 * Ctrl+M, Tab moves focus (CFG-08, A11Y-01, §4.9.3): while on, Tab and Shift+Tab in a code view move keyboard focus to the
	 * next or previous widget instead of indenting. (codeview choice) One state for the editor session, off at start and not
	 * saved, as the VS Code toggle it copies: a stray Ctrl+M never survives a restart.
	 */
	bool IsTabMovesFocus() const { return bTabMovesFocus; }
	void SetTabMovesFocus(bool bOn) { bTabMovesFocus = bOn; }

	/**
	 * (run, PO-06 R-q1-2, the owner's import-bug fix: "Main Enter, make it run") Main Enter with a non-empty selection in an
	 * editable buffer view runs exactly the selection (RunSelectionOrBuffer, as Numpad Enter and Ctrl+Enter); without a
	 * selection main Enter stays a newline. Rebuild adds the binding (focused, editable buffer view with a selection; never
	 * the REPL prompt, an output view or an open completion popup). On by default; this constant is the default, and
	 * SetMainEnterRunsSelection turns it off for the session (no user setting yet).
	 */
	static constexpr bool bMainEnterRunsSelectionDefault = true;
	bool IsMainEnterRunsSelection() const { return bMainEnterRunsSelection; }
	/** Turns the binding on or off for this session (specs, and a later setting) and rebuilds the keymap. */
	void SetMainEnterRunsSelection(bool bOn);

	const dream::Keymap& GetKeymap() const { return Keymap; }

	/** ShutdownModule: unbinds the chord-changed listener. Idempotent. */
	void Shutdown();

	/** The host key code of an FKey (assigned on first use; stable for the session). */
	dream::KeyCode GetKeyCode(const FKey& Key) const;
	/** The §4.9.4 key class of an FKey. */
	static dream::KeyClass ClassifyKey(const FKey& Key);

private:
	/** True when the stroke is one of the §4.9.4 pass-through chords (checked before the consume rules). */
	bool IsPassThrough(const dream::Stroke& Stroke) const;

	dream::Keymap Keymap;
	FDelegateHandle ChordChangedHandle;
	mutable TSet<FKey> ConsumedDownKeys;
	mutable TMap<FKey, dream::KeyCode> KeyCodes;
	TArray<dream::Chord> PassThroughChords;
	bool bTabMovesFocus = false;
	bool bMainEnterRunsSelection = bMainEnterRunsSelectionDefault;   // (run, PO-06 R-q1-2)
};
