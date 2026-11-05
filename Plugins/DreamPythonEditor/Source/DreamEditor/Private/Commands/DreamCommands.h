// SPDX-License-Identifier: Apache-2.0
// Dream commands and the Charcoal default chords (TDD §4.9.2, §4.9.4, SPEC §9.3, D8, D25, U24). Context "DreamEditor",
// parent NAME_None (parentless), style set DreamEditorStyle. Chords persist in EditorKeyBindings.ini; Unreal's Keyboard
// Shortcuts page edits them. Game thread. Body: DreamCommands.cpp (shell slice).
// Execution never goes through FUICommandList::ProcessCommandBindings (§4.9.1): FDreamKeyRouter resolves keys with
// dream::Keymap and calls IDreamCommandTarget, so a bound chord is consumed even when it cannot execute.
#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

/**
 * Every Dream command. The value is the dream::CommandId used by dream::Keymap (0 = none). Values are not persisted (the ini
 * stores the stable names of DreamCommands.cpp), so a new command goes into its section, gets a name in CommandNames and a
 * UI_COMMAND_EXT in RegisterCommands, in the same order.
 * (slice A choice) The REPL prompt keys (Enter, Shift+Enter, Ctrl+Enter, Up, Ctrl+Up, Esc) are hard-wired in
 * SDreamReplStrip like the §4.9.4 primitives, not rebindable commands.
 * The text commands (Undo, Redo, DuplicateLine, Find, Replace, FindNext, FindPrevious, ToggleTabMovesFocus) act on the
 * focused code view: SDreamCodeView executes them itself before its command target sees them (SDreamCodeView.h), so a key in
 * the REPL prompt undoes the prompt, not the script.
 */
enum class EDreamCommand : uint32
{
	None = 0,

	// Run (§4.9.4, RUN-01, RUN-02, RUN-26)
	RunSelectionOrBuffer,   // Ctrl+Enter; Numpad Enter (Win64, G7)
	RunBuffer,              // Ctrl+Shift+Enter
	RunLine,                // Ctrl+Alt+Enter: the statement extent (§4.5)
	RunLineAdvance,         // Alt+Enter (RUN-26)
	RunFile,                // toolbar menu; no chord
	RunInSessionOrIsolated, // the split Run button's namespace choice (U6); no chord
	Stop,                   // Shift+F5 (Esc during a run is the pre-processor's RT-05 rule)
	UndoLastRun,            // Ctrl+Alt+Z, left Alt only (RT-16)
	ToggleOneUndoStepPerRun,// toolbar toggle (U3)
	ResetSession,           // RT-15; menu

	// Output (§6.4)
	ToggleEchoInput,        // toolbar toggle (OUT-12)
	ClearOutput,
	NextError,              // F4 (U13)
	PrevError,              // Shift+F4
	FocusRepl,
	MoveOutputToTop,        // WS-03; no default chord
	MoveOutputToBottom,
	ExpandEcho,             // OUT-12 echo folding (§6.4): the statement at the caret or the selected runs; no default chord
	FoldEcho,

	// Buffers (BUF-03)
	NewTab,                 // Ctrl+T
	OpenFile,               // menu; Ctrl+O is pass-through (§4.9.4)
	Save,                   // Ctrl+S
	SaveAs,                 // menu; Ctrl+Shift+S is pass-through
	CloseTab,               // Ctrl+W
	ReopenClosedTab,        // Ctrl+Shift+T
	NextTab,                // Ctrl+PageDown
	PreviousTab,            // Ctrl+PageUp

	// Editing (plain mode)
	GoToLine,               // Ctrl+G (NAV-03; Level Editor Regroup is shadowed while focused)
	ToggleComment,          // Ctrl+/
	ZoomIn,                 // Ctrl+=; Ctrl+wheel up (EDT-10)
	ZoomOut,                // Ctrl+-; Ctrl+wheel down
	ZoomReset,              // Ctrl+0
	Undo,                   // Ctrl+Z: text undo on dream::UndoTree (EDT-08, D8; shadows TRANSACTION UNDO while focused)
	Redo,                   // Ctrl+Y; Ctrl+Shift+Z
	DuplicateLine,          // Ctrl+D (EDT-04; shadows the Level Editor's Duplicate while focused)
	ToggleTabMovesFocus,    // Ctrl+M (CFG-08; shadows Move Selection to Current Level while focused)

	// Find (EDT-09)
	Find,                   // Ctrl+F
	Replace,                // Ctrl+H (shadows the Level Editor's Show All while focused)
	FindNext,               // F3
	FindPrevious,           // Shift+F3

	// Completion (§7.6.2; completion group)
	Complete,               // Ctrl+Space, never Ctrl+Tab (D8; shadows Open Content Browser drawer while focused). The
	                        // code view's completion controller resolves it before RoutePre (CodeView/SDreamCompletionPopup.h)

	// Problems (§7.8.2, §7.8.3; lang group)
	NextProblem,            // F8 (U13; shadows PIE Possess or Eject Player while focused, U23)
	PrevProblem,            // Shift+F8
	QuickFix,               // Ctrl+. (U22): the quick fixes on the caret line
	NextProblemInFile,      // (lang) PO-06 F4: F2, PyCharm's Next Highlighted Error: the next problem of this file, wrapping
	PrevProblemInFile,      // (lang) Shift+F2 (both shadow the Level Editor's Rename while focused)

	// (debug) PO-06 F5, the debugger (TDD §8.3.5, U21, DBG-01..DBG-04). The step keys act in the Paused window, which
	// reads these chords; in the tab they are consumed and say "No Debug run is paused".
	// (run, with debug; review q1 nit) Shadowing: while a Dream view has keyboard focus, F10, F11 and Shift+F11 are Dream's
	// (the key router consumes bound chords, §4.9.4), so the Level Editor's F11 (immersive viewport) does not fire there;
	// it works as usual once focus is outside Dream. Rebinding these in Keyboard Shortcuts (Dream) frees the keys.
	DebugRun,               // F5 when idle: debug-run the selection or buffer (U21); Continue in the Paused window
	ToggleBreakpoint,       // F9: the caret line's breakpoint
	DebugStepOver,          // F10
	DebugStepInto,          // F11
	DebugStepOut,           // Shift+F11

	// View (chrome, PO-04): the Maya-minimal tab's View toggles (UDreamUserSettings view settings); no default chords
	ToggleReplTab,
	ToggleProblemsTab,
	ToggleReplPrompt,
	ToggleRunHeaders,
	ToggleStatusLine,
	ToggleLineNumbers,
	// (sidebar) PO-05: View ▸ Project and View ▸ Structure, and focus into either section (Esc returns); no default chords
	ToggleProjectPanel,
	ToggleStructurePanel,
	FocusProject,
	FocusStructure,
	TogglePluginPythonFolders,   // (sidebar) PO-05-A1: the per-project Add plugin Python folders setting, live

	// Global
	OpenDream,               // mapped into the main frame bindings; unbound by default (§11.6.1)

	Count
};

class DREAMEDITOR_API FDreamCommands : public TCommands<FDreamCommands>
{
public:
	static const FName ContextName;     // "DreamEditor"

	FDreamCommands();

	/** UI_COMMAND for every EDreamCommand with the Charcoal chords above. */
	virtual void RegisterCommands() override;

	/** The command info of a command (menus, tooltips, toolbar). */
	TSharedPtr<FUICommandInfo> GetInfo(EDreamCommand Command) const;
	/** The reverse map; None for an info that is not ours. */
	EDreamCommand FindCommand(const TSharedPtr<const FUICommandInfo>& Info) const;
	/** Stable name, as stored in EditorKeyBindings.ini ("RunSelectionOrBuffer", ...). */
	static FName GetCommandName(EDreamCommand Command);
	/** The text commands that the focused SDreamCodeView executes itself (see the enum's comment). Their chords are not bound
	 *  in the output views (dream::Ctx_Output), where Ctrl+Z must not undo the script; the consume list still holds there. */
	static bool IsCodeViewCommand(EDreamCommand Command);

private:
	TArray<TSharedPtr<FUICommandInfo>> Infos;   // indexed by EDreamCommand
};
