// SPDX-License-Identifier: Apache-2.0
// FDreamCommands: every slice-A command with the Charcoal default chords of TDD §4.9.4 (SPEC §9.3).
// Context "DreamEditor", parentless (D25), style set DreamEditorStyle; chords persist in EditorKeyBindings.ini (U24).
#include "Commands/DreamCommands.h"

#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "InputCoreTypes.h"
#include "Shell/DreamEditorStyle.h"

#define LOCTEXT_NAMESPACE "Dream"

const FName FDreamCommands::ContextName(TEXT("DreamEditor"));

namespace DreamCommandsLocal
{
	/** Stable names as stored in EditorKeyBindings.ini; index = EDreamCommand. RegisterCommands checks each against its info. */
	const TCHAR* const CommandNames[] =
	{
		TEXT(""),
		TEXT("RunSelectionOrBuffer"),
		TEXT("RunBuffer"),
		TEXT("RunLine"),
		TEXT("RunLineAdvance"),
		TEXT("RunFile"),
		TEXT("RunInSessionOrIsolated"),
		TEXT("Stop"),
		TEXT("UndoLastRun"),
		TEXT("ToggleOneUndoStepPerRun"),
		TEXT("ResetSession"),
		TEXT("ToggleEchoInput"),
		TEXT("ClearOutput"),
		TEXT("NextError"),
		TEXT("PrevError"),
		TEXT("FocusRepl"),
		TEXT("MoveOutputToTop"),
		TEXT("MoveOutputToBottom"),
		TEXT("ExpandEcho"),
		TEXT("FoldEcho"),
		TEXT("NewTab"),
		TEXT("OpenFile"),
		TEXT("Save"),
		TEXT("SaveAs"),
		TEXT("CloseTab"),
		TEXT("ReopenClosedTab"),
		TEXT("NextTab"),
		TEXT("PreviousTab"),
		TEXT("GoToLine"),
		TEXT("ToggleComment"),
		TEXT("ZoomIn"),
		TEXT("ZoomOut"),
		TEXT("ZoomReset"),
		TEXT("Undo"),
		TEXT("Redo"),
		TEXT("DuplicateLine"),
		TEXT("ToggleTabMovesFocus"),
		TEXT("Find"),
		TEXT("Replace"),
		TEXT("FindNext"),
		TEXT("FindPrevious"),
		TEXT("Complete"),
		TEXT("NextProblem"),
		TEXT("PrevProblem"),
		TEXT("QuickFix"),
		TEXT("NextProblemInFile"),      // (lang) PO-06 F4
		TEXT("PrevProblemInFile"),      // (lang)
		TEXT("DebugRun"),               // (debug) PO-06 F5
		TEXT("ToggleBreakpoint"),       // (debug)
		TEXT("DebugStepOver"),          // (debug)
		TEXT("DebugStepInto"),          // (debug)
		TEXT("DebugStepOut"),           // (debug)
		TEXT("ToggleReplTab"),
		TEXT("ToggleProblemsTab"),
		TEXT("ToggleReplPrompt"),
		TEXT("ToggleRunHeaders"),
		TEXT("ToggleStatusLine"),
		TEXT("ToggleLineNumbers"),
		TEXT("ToggleProjectPanel"),     // (sidebar)
		TEXT("ToggleStructurePanel"),   // (sidebar)
		TEXT("FocusProject"),           // (sidebar)
		TEXT("FocusStructure"),         // (sidebar)
		TEXT("TogglePluginPythonFolders"),   // (sidebar) PO-05-A1
		TEXT("OpenDream"),
	};
	static_assert(UE_ARRAY_COUNT(CommandNames) == static_cast<int32>(EDreamCommand::Count), "One name per EDreamCommand");
}

FDreamCommands::FDreamCommands()
	: TCommands<FDreamCommands>(
		ContextName,
		LOCTEXT("DreamCommandsContext", "Dream Python Editor"),
		NAME_None,                                   // parentless (D25): Level Editor chords never become ours
		FDreamEditorStyle::GetStyleSetName())
{
}

void FDreamCommands::RegisterCommands()
{
	Infos.Reset();
	Infos.SetNum(static_cast<int32>(EDreamCommand::Count));

	auto Slot = [this](EDreamCommand Command) -> TSharedPtr<FUICommandInfo>&
	{
		return Infos[static_cast<int32>(Command)];
	};

	const EModifierKey::Type Ctrl = EModifierKey::Control;
	const EModifierKey::Type CtrlShift = EModifierKey::FromBools(true, false, true, false);
	const EModifierKey::Type CtrlAlt = EModifierKey::FromBools(true, true, false, false);
	const EModifierKey::Type Alt = EModifierKey::Alt;
	const EModifierKey::Type Shift = EModifierKey::Shift;

	// Run (§4.9.4, RUN-01, RUN-02, RUN-26). Numpad Enter is not an FInputChord: FDreamKeyRouter adds it (G7, §4.9.2).
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunSelectionOrBuffer), "RunSelectionOrBuffer", "Run Selection or Buffer",
		"Runs exactly the selected text, or the whole buffer when nothing is selected", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Enter));   // (run) PO-06 F2 tooltip
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunBuffer), "RunBuffer", "Run Buffer",
		"Runs the whole buffer", EUserInterfaceActionType::Button, FInputChord(CtrlShift, EKeys::Enter));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunLine), "RunLine", "Run Current Line",
		"Runs the statement on the caret line", EUserInterfaceActionType::Button, FInputChord(CtrlAlt, EKeys::Enter));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunLineAdvance), "RunLineAdvance", "Run Line and Advance",
		"Runs the statement on the caret line and moves to the next statement", EUserInterfaceActionType::Button, FInputChord(Alt, EKeys::Enter));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunFile), "RunFile", "Run File",
		"Saves the file and runs it in an isolated namespace", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::RunInSessionOrIsolated), "RunInSessionOrIsolated", "Run in Isolated Namespace",
		"Runs this tab in a fresh namespace instead of the Session", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Stop), "Stop", "Stop",
		"Stops the active run", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F5));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::UndoLastRun), "UndoLastRun", "Undo Last Run",
		"Undoes the editor changes of the last run as one step", EUserInterfaceActionType::Button, FInputChord(CtrlAlt, EKeys::Z));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleOneUndoStepPerRun), "ToggleOneUndoStepPerRun", "One undo step per run",
		"Records each run of this tab as one undo step", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ResetSession), "ResetSession", "Reset Session",
		"Removes every name the Session gained since it started", EUserInterfaceActionType::Button, FInputChord());

	// Output (§6.4)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleEchoInput), "ToggleEchoInput", "Echo input",
		"Shows each statement's source before its output", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ClearOutput), "ClearOutput", "Clear",
		"Clears the output area; run logs and history are kept", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::NextError), "NextError", "Next Error",
		"Opens the next error location in Output", EUserInterfaceActionType::Button, FInputChord(EKeys::F4));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::PrevError), "PrevError", "Previous Error",
		"Opens the previous error location in Output", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F4));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FocusRepl), "FocusRepl", "Focus REPL",
		"Shows and focuses the REPL prompt", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::MoveOutputToTop), "MoveOutputToTop", "Move Output to Top",
		"Places the output area above the code area", EUserInterfaceActionType::RadioButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::MoveOutputToBottom), "MoveOutputToBottom", "Move Output to Bottom",
		"Places the output area below the code area", EUserInterfaceActionType::RadioButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ExpandEcho), "ExpandEcho", "Expand Echo",
		"Shows every echo line of the statement at the caret, or of the selected runs", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FoldEcho), "FoldEcho", "Fold Echo",
		"Folds the echo of the statement at the caret, or of the selected runs, to its first lines", EUserInterfaceActionType::Button, FInputChord());

	// Buffers (BUF-03)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::NewTab), "NewTab", "New Tab",
		"Opens a new scratch tab", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::T));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::OpenFile), "OpenFile", "Open File...",
		"Opens a Python file in a new tab", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Save), "Save", "Save",
		"Saves the active tab", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::S));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::SaveAs), "SaveAs", "Save As...",
		"Saves the active tab under a new path", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::CloseTab), "CloseTab", "Close Tab",
		"Closes the active tab", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::W));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ReopenClosedTab), "ReopenClosedTab", "Reopen Closed Tab",
		"Reopens the most recently closed tab", EUserInterfaceActionType::Button, FInputChord(CtrlShift, EKeys::T));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::NextTab), "NextTab", "Next Tab",
		"Activates the tab to the right", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::PageDown));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::PreviousTab), "PreviousTab", "Previous Tab",
		"Activates the tab to the left", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::PageUp));

	// Editing (plain mode)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::GoToLine), "GoToLine", "Go to Line...",
		"Moves the caret to a line", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::G));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleComment), "ToggleComment", "Toggle Comment",
		"Comments or uncomments the selected lines", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Slash));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ZoomIn), "ZoomIn", "Zoom In",
		"Increases the code font size", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Equals));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ZoomOut), "ZoomOut", "Zoom Out",
		"Decreases the code font size", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Hyphen));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ZoomReset), "ZoomReset", "Reset Zoom",
		"Resets the code font size to the Font size setting", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Zero));
	// Text undo in a Dream widget is always the text's, never the editor transaction's (D8, EDT-08).
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Undo), "Undo", "Undo",
		"Undoes the last text edit in this view", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Z));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Redo), "Redo", "Redo",
		"Redoes the last undone text edit in this view", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Y), FInputChord(CtrlShift, EKeys::Z));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::DuplicateLine), "DuplicateLine", "Duplicate Line",
		"Duplicates the caret line, or the selected lines, below", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::D));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleTabMovesFocus), "ToggleTabMovesFocus", "Tab Moves Focus",
		"Toggles whether Tab and Shift+Tab move keyboard focus out of the code view instead of indenting", EUserInterfaceActionType::ToggleButton,
		FInputChord(Ctrl, EKeys::M));

	// Find (EDT-09)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Find), "Find", "Find...",
		"Opens the find bar of the code view", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::F));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Replace), "Replace", "Replace...",
		"Opens the find bar with its replace row", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::H));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FindNext), "FindNext", "Find Next",
		"Selects the next match of the find bar's text", EUserInterfaceActionType::Button, FInputChord(EKeys::F3));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FindPrevious), "FindPrevious", "Find Previous",
		"Selects the previous match of the find bar's text", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F3));

	// Completion (§7.6.2, §4.9.4; completion group)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::Complete), "Complete", "Complete",
		"Shows the completion list at the caret", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::SpaceBar));

	// Problems (§7.8.2, §7.8.3; lang group)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::NextProblem), "NextProblem", "Next Problem",
		"Moves the caret to the next item in Problems", EUserInterfaceActionType::Button, FInputChord(EKeys::F8));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::PrevProblem), "PrevProblem", "Previous Problem",
		"Moves the caret to the previous item in Problems", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F8));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::QuickFix), "QuickFix", "Quick Fix...",
		"Shows the quick fixes for the problems on the caret line", EUserInterfaceActionType::Button, FInputChord(Ctrl, EKeys::Period));
	// (lang) PO-06 F4: PyCharm's F2 / Shift+F2, inside the active file (F8 / Shift+F8 keep walking the Problems list).
	UI_COMMAND_EXT(this, Slot(EDreamCommand::NextProblemInFile), "NextProblemInFile", "Next Problem in File",
		"Moves the caret to the next error or warning in this file", EUserInterfaceActionType::Button, FInputChord(EKeys::F2));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::PrevProblemInFile), "PrevProblemInFile", "Previous Problem in File",
		"Moves the caret to the previous error or warning in this file", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F2));

	// (debug) PO-06 F5: the debugger keys of TDD §8.3.5 (U21, U22); rebindable, read by the Paused window too.
	UI_COMMAND_EXT(this, Slot(EDreamCommand::DebugRun), "DebugRun", "Debug",
		"Debug-runs the selection, or the whole buffer when nothing is selected, stopping at breakpoints; continues when paused",
		EUserInterfaceActionType::Button, FInputChord(EKeys::F5));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleBreakpoint), "ToggleBreakpoint", "Toggle Breakpoint",
		"Sets or removes a breakpoint on the caret line", EUserInterfaceActionType::Button, FInputChord(EKeys::F9));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::DebugStepOver), "DebugStepOver", "Step Over",
		"Runs the paused line and stops at the next line of the same function", EUserInterfaceActionType::Button, FInputChord(EKeys::F10));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::DebugStepInto), "DebugStepInto", "Step Into",
		"Runs the paused line and stops at the first line of a function it calls", EUserInterfaceActionType::Button, FInputChord(EKeys::F11));
	UI_COMMAND_EXT(this, Slot(EDreamCommand::DebugStepOut), "DebugStepOut", "Step Out",
		"Runs to the end of the paused function and stops in its caller", EUserInterfaceActionType::Button, FInputChord(Shift, EKeys::F11));

	// View (chrome, PO-04): every part the Maya-minimal tab hides is one of these toggles away; unbound, rebindable.
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleReplTab), "ToggleReplTab", "REPL Tab",
		"Shows the REPL tab in the output area", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleProblemsTab), "ToggleProblemsTab", "Problems Tab",
		"Shows the Problems tab in the output area", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleReplPrompt), "ToggleReplPrompt", "REPL Prompt",
		"Shows the REPL prompt under the history", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleRunHeaders), "ToggleRunHeaders", "Run Headers",
		"Shows each run's header and footer line in the history", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleStatusLine), "ToggleStatusLine", "Status Line",
		"Shows the status line under the code area", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleLineNumbers), "ToggleLineNumbers", "Line Numbers",
		"Shows line numbers beside the code", EUserInterfaceActionType::ToggleButton, FInputChord());
	// (sidebar) PO-05: the side bar's sections, on by default; unbound and rebindable, so no chord can conflict.
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleProjectPanel), "ToggleProjectPanel", "Project",
		"Shows the Project tree in the side bar", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::ToggleStructurePanel), "ToggleStructurePanel", "Structure",
		"Shows the structure of the active tab in the side bar", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FocusProject), "FocusProject", "Focus Project",
		"Moves keyboard focus into the Project tree (Esc returns to the code)", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND_EXT(this, Slot(EDreamCommand::FocusStructure), "FocusStructure", "Focus Structure",
		"Moves keyboard focus into the Structure list (Esc returns to the code)", EUserInterfaceActionType::Button, FInputChord());
	// (sidebar) PO-05-A1: the same per-project setting as Editor Preferences > Plugins > Dream (this project).
	UI_COMMAND_EXT(this, Slot(EDreamCommand::TogglePluginPythonFolders), "TogglePluginPythonFolders", "Plugin Python Folders",
		"Shows the Python folders of the enabled engine and project plugins in the Project tree (saved for this project)",
		EUserInterfaceActionType::ToggleButton, FInputChord());

	// Global: mapped into the main frame bindings, unbound by default (§11.6.1: no free global chord exists)
	UI_COMMAND_EXT(this, Slot(EDreamCommand::OpenDream), "OpenDream", "Dream Python Editor",
		"Opens the Dream Python Editor", EUserInterfaceActionType::Button, FInputChord());

	for (int32 Index = 1; Index < Infos.Num(); ++Index)
	{
		ensureMsgf(Infos[Index].IsValid() && Infos[Index]->GetCommandName() == FName(DreamCommandsLocal::CommandNames[Index]),
			TEXT("Dream Python Editor command %d is not registered under its stable name"), Index);
	}
}

TSharedPtr<FUICommandInfo> FDreamCommands::GetInfo(EDreamCommand Command) const
{
	const int32 Index = static_cast<int32>(Command);
	return Infos.IsValidIndex(Index) ? Infos[Index] : TSharedPtr<FUICommandInfo>();
}

EDreamCommand FDreamCommands::FindCommand(const TSharedPtr<const FUICommandInfo>& Info) const
{
	if (Info.IsValid())
	{
		for (int32 Index = 1; Index < Infos.Num(); ++Index)
		{
			if (Infos[Index] == Info)
			{
				return static_cast<EDreamCommand>(Index);
			}
		}
	}
	return EDreamCommand::None;
}

FName FDreamCommands::GetCommandName(EDreamCommand Command)
{
	const int32 Index = static_cast<int32>(Command);
	if (Index <= 0 || Index >= static_cast<int32>(EDreamCommand::Count))
	{
		return NAME_None;
	}
	return FName(DreamCommandsLocal::CommandNames[Index]);
}

bool FDreamCommands::IsCodeViewCommand(EDreamCommand Command)
{
	switch (Command)
	{
	case EDreamCommand::Undo:
	case EDreamCommand::Redo:
	case EDreamCommand::DuplicateLine:
	case EDreamCommand::ToggleTabMovesFocus:
	case EDreamCommand::Find:
	case EDreamCommand::Replace:
	case EDreamCommand::FindNext:
	case EDreamCommand::FindPrevious:
		return true;
	default:
		return false;
	}
}

#undef LOCTEXT_NAMESPACE
