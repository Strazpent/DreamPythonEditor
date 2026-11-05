> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 01 - Maya Script Editor, Charcoal Editor 2, MotionBuilder and 3ds Max

Research lane: Autodesk DCC script editors (Maya native Script Editor, Charcoal Editor 2, MotionBuilder Python Editor, 3ds Max Scripting Listener and Editor).
Date: 2026-10-28. Sources are cited inline as `[ID]`, and the IDs resolve in the Sources section.

**Confidence notes**
- Several Autodesk forum and Packt pages returned HTTP 403. Where a claim comes only from a search-index excerpt of such a page, it is marked *(excerpt)*.
- Claims I could not confirm from any source are marked **UNVERIFIED**. They come from general knowledge or inference.
- The shared web-search budget ran out near the end of this pass. A few items that needed more searching are listed under Open questions instead.

---

## Summary

- **Charcoal Editor is not part of Maya.** The brief assumed Autodesk integrated Charcoal Editor into Maya. It did not. Charcoal Editor (2012) and Charcoal Editor 2 (2018) are commercial third-party plugins by Chris Zurbrigg, and CE2 costs USD 79 [S1][S28]. The plugin can take over Maya's Script Editor button through a preference [S18]. Maya's built-in Script Editor is still its own Qt widget pair, `cmdScrollFieldExecuter` (input) and `cmdScrollFieldReporter` (history) [M6][M7].
- **Maya's native editor has barely changed since 2009.** The 2009 help already describes MEL and Python tabs, numpad-Enter and Ctrl+Enter execution, Echo All, line numbers in errors, the stack trace window, suppress toggles, and save-on-exit tab persistence [M17]. Later additions are incremental:
  - command, object-path and tooltip completion, plus the Quick Help panel;
  - file-backed tabs with an unsaved marker and a reload prompt on external edits (documented by Maya 2018) [M10];
  - **Save All Script Tabs** and **Show Tabs and Spaces** (Maya 2022.2, 23 Sep 2021) [M19];
  - a separate **Tabs** menu and middle-click to close a tab (Maya 2025 docs) [M3].
  I found no Script Editor items in the Maya 2026 or 2027 What's New material.
- **CE2 is the reference for a "modern" in-DCC editor.** It has a VS Code-style shell: an Activity Bar, a Side Bar (Explorer with Workspace/Outline/Open Editors, Global Search, Quick Help), split editor groups, and Go to file/symbol/line [S11][S12][S14]. Its central behavioral rules:
  - **Ctrl+Enter** runs the selection, or the whole buffer when nothing is selected.
  - **Ctrl+Shift+Enter** runs the whole buffer.
  - **Ctrl+Alt+Enter** runs the current line with leading whitespace stripped.
  - **Code is never deleted on execute**, and the buffer is **saved before every execute** [S3][S4][S18].
  Other notable features: Pylint integration with Maya stubs [S21-S23], a "module state revert" that avoids `reload()` [S16], and output verbosity levels with custom highlight rules and a line cap [S6][S8]. Code folding only arrived in v2.10.1 (27 Aug 2026) [S5].
- **Execution model, common to the Autodesk tools:**
  - Everything runs on the main thread, so a long script freezes the UI.
  - Python tabs share one global namespace.
  - Output is captured by swapping `sys.stdout`. Output lines get language-specific prefixes (`// Error:` for MEL, `# Error:` for Python).
  - There is no built-in debugger. Developers attach PyCharm or VS Code through pydevd/debugpy, or send code over a socket (Maya `commandPort`, MotionBuilder telnet on 4242).
  - Cancelling is cooperative only, through `progressWindow(isInterruptable=True)` plus polling for Esc [M14][F14].
- **The main TD complaints:**
  - Lost or blank tabs after a crash [F1][F3].
  - Code vanishing on execute [F13].
  - A slow history pane once it holds many lines [F4].
  - Echo All being a MEL-only firehose that costs performance [S6][F6].
  - Autocomplete limited to what is already in `__main__` [S1].
  - No debugger [F15][I2].

---

## History

### Classic Maya Script Editor
- **By Maya 2009** the Script Editor already had a history pane (top) and an input pane (bottom) with MEL and Python tabs. Code ran with numeric-keypad Enter, Command > Execute, or by selecting text and pressing Ctrl+Enter. The toolbar had *Execute All* and *Execute (selected)* buttons. Other features [M17]:
  - Load, Source, Save and Save-to-Shelf.
  - Go to Line, Search and Replace, Indent and Unindent.
  - Echo All Commands, Line Numbers in Errors, Show Stack Trace, and suppress toggles.
  - MEL scripts saved on exit and restored on restart.
  - A dialog for Python `raw_input`.
- Python first appeared in Maya 8.5 (2007). **UNVERIFIED** in this pass.
- The `scriptEditorInfo` docs still say some flags "no longer work in the new Script Editor window" and point to the old "CommandWindow" [M8]. So the current Qt editor replaced an older Command Window at some point.
- **Maya 2017 docs** list Command Completion, Object Path Completion, Show Tooltip Help (Ctrl+Space when tooltips are off) and Show Quick Help *(excerpt of the 2017 menus page)*. The 2020 docs say the same [M18].
- **By Maya 2018**, *File > Open Script* opens a file-backed tab [M10]:
  - the tab is named after the file, and hovering shows the path;
  - an asterisk marks unsaved changes;
  - Save writes back to the file;
  - edits made outside Maya trigger a reload prompt, and deleting the file triggers a keep-or-close prompt;
  - loaded files are restored when Maya restarts.
- **Maya 2022.2 (23 Sep 2021)** added *File > Save All Script Tabs*, which forces a save so tabs survive a crash, and *Command > Show Tabs and Spaces* [M19][M1].
- **Maya 2023**: users reported that the Script Editor now keeps code after crashes. Maya 2020.2 was already dumping tabs to `%TEMP%` on crash [F2].
- **Maya 2025 docs** [M3]:
  - tab commands moved into a dedicated **Tabs** menu (previously under Command);
  - middle-clicking a tab closes it;
  - Ctrl+mouse-wheel zooms the font (Windows only);
  - custom tab icons via `tabIcon`/`tabIconIndex`.
- **Maya 2026 and 2027**: the menus-and-hotkeys page is the same as 2022.2–2025 [M1][M2]. I found no What's New entries for the Script Editor. One maya-usd release note mentions reducing message spam that slowed the Script Editor *(excerpt, maya-usd GitHub releases)*.

### Charcoal Editor (v1)
- **Developer:** Chris Zurbrigg, author of the Blue Pencil sketch plugin.
- **Release:** announced 1–13 Nov 2012 at an introductory price of CAD 39. Supported Maya 2011 SP1+ on Windows and Linux, and Maya 2012+ on OS X [S28][S29].
- **Implementation:** a native C++ plugin with no third-party dependencies [S29].
- **v1 features** [S29]:
  - MEL and Python tabs, split screen, movable tabs;
  - advanced find and replace;
  - auto-save on execute;
  - smart indent, autocomplete, matching word and brace highlights;
  - visible whitespace, tab-to-space conversion;
  - drag to and from the shelf;
  - execute selection or a single line;
  - an output window that highlights errors, warnings and results;
  - an outline view (classes, methods, procs), open-documents view and project tree.
- **Reception on tech-artists.org (2012–2016)** [S30]:
  - "a good replacement for the native maya script editor" but not a replacement for Eclipse, Wing or Sublime;
  - QuickHelp, which shows `maya.cmds` and `pymel.core` parameters, was praised;
  - the lack of a trial at launch was criticized.

### Charcoal Editor 2
- **Release history** [S5]:

| Version | Date | Change |
|---|---|---|
| 2.0.1 | 16 May 2018 | Initial release |
| 2.1.1 | Jun 2018 | Linux and macOS |
| 2.3.1 | Jan 2019 | Status-bar error and warning counts, `-openFile` flag |
| 2.3.2 | Feb 2019 | UTF-8 option |
| 2.4.1–2.4.2 | Sep 2019 | Python module-state revert, `dir()`/`help()` context items, insert-file-path |
| 2.7.1 | 30 Mar 2024 | Maya 2025 (Qt6), "native Maya autocomplete fallback" |
| 2.8.1 | 20 Apr 2026 | Maya 2027 |
| 2.9.1 | 2 Jul 2026 | New Python 3 highlighter, f-string autocomplete |
| 2.10.1 | 27 Aug 2026 | **Code folding**, output-suppression options |
| 2.10.2 | 7 Sep 2026 | Current version |

- **Builds** [S1]:
  - 2.10.2 for Maya 2025–2027;
  - 2.6.4 for Maya 2022–2024;
  - 2.6.1 for Maya 2018–2020;
  - 2.3.4 for Maya 2017, which needs Update 4 or 5 [S24].
  There is no "2021" build because Maya went from 2020 to 2022.
- **Distribution:** USD 79 through Gumroad, with an evaluation version and site licenses [S1].
- **Install:** a `.mll` (Windows) or `.bundle` (macOS) file copied into `Documents/maya/<ver>/plug-ins` and loaded or auto-loaded in the Plug-in Manager [S25].
- **What changed from v1 to v2** (inferred from the docs; Zurbrigg never published an explicit v1-vs-v2 list):
  - a VS Code-like shell: Activity Bar, Side Bar with Explorer (Workspace, Outline, Open Editors), Global Search and Quick Help, editor groups, Go-To palette, status bar [S11][S12][S14];
  - workspaces saved to disk [S9];
  - a Hotkey Manager [S15];
  - Pylint [S21];
  - forced imports for autocomplete [S17];
  - module-state revert [S16];
  - output verbosity levels and custom highlight rules [S6][S8].

---

## Feature inventory

### A. Maya native Script Editor (2026/2027)

| Feature | Behavior | Shortcut | Notes |
|---|---|---|---|
| Open | Windows > General Editors > Script Editor; Panels > Panel > Script Editor (dock it in a viewport panel); icon at the bottom right of the main window [M3] | none by default | The editor can live in a workspace panel, so it docks |
| Layout | History pane on top, input pane below. Toolbar buttons: Show History, Show Input, Show Both [M4] | | The panes share one splitter |
| Tabs | Each input tab is MEL or Python, chosen when the tab is created, with a language icon on the tab [M1][M3] | New **Ctrl+T**, Next **Ctrl+Tab**, Prev **Ctrl+Shift+Tab**, Close **Ctrl+W**, middle-click a tab to close it [M1][M3] | Tabs menu also has Rename Tab and Delete All Other Tabs. Right-click a tab, or use the marking menu, for New/Rename/Delete [M1][M5] |
| Syntax highlighting | MEL and Python commands are highlighted as you type [M1][M3] | | **UNVERIFIED** whether the history pane colors errors or warnings natively. No docs describe it or a way to customize its colors |
| Line numbers | Command > Show Line Numbers. The setting persists across sessions [M1][M13] | Toolbar toggle [M4] | |
| Whitespace display | Command > Show Tabs and Spaces (since 2022.2) [M1][M19] | | `cmdScrollFieldExecuter -showTabsAndSpaces` [M6] |
| Indentation | Command > Use Tabs for Indent. Tab stop defaults to 4. `tabsForIndent` defaults to on [M1][M6] | Edit > Indent Selection / Unindent Selection [M1] | No smart or auto indent is documented for Python |
| Auto-close braces | Inserts a closing `}` when Enter is pressed after `{`. On by default [M1][M6] | | Curly braces only, so it mostly helps MEL. It does not close parentheses or quotes |
| Command completion | Completes command names [M1] | **Ctrl+Space** when tooltip help is off [M1][M12] | Native completion only covers objects already in the main namespace [S1] |
| Object path completion | Completes DAG and node paths from the live scene [M1] | Ctrl+Space | Uses live scene introspection |
| Tooltip help | Shows completions and flags automatically as you type [M1] | | |
| Quick Help | Command > Show Quick Help opens a side panel that searches commands and lists their flags [M1] | Marking menu > Quick Help [M5] | **UNVERIFIED** that the flag lists come from the registered command syntax (`cmds.help`) [M12] |
| Command docs | Marking menu > Command Documentation opens the selected command's docs in a browser [M5] | | Context-sensitive docs lookup |
| Find and replace | Edit > Search and Replace, with direction and case options [M1] | Toolbar Search Down / Search Up [M4] | No regex is documented. The underlying flags are `searchMatchCase` and `searchWraps` [M6] |
| Go to line | Edit > Go to Line. Toolbar Go box [M1][M4] | | |
| Comment toggle | **Not present** in the documented menus [M1][M2] | | CE2, MotionBuilder and 3ds Max all have it |
| Folding, multi-cursor, bracket matching, snippets | None documented [M1][M6] | | Users asked for multi-cursor and multipane on the Maya Ideas forum [F5] *(excerpt)*. In practice the shelf serves as the snippet store |
| Execute | Runs the selection, or everything if nothing is selected [M6] | **Numpad Enter**, **Ctrl+Enter**, Command > Execute [M1][M3] | Main Enter inserts a newline |
| Execute All | Runs the whole tab regardless of selection [M6] | Toolbar button [M4] | `cmdScrollFieldExecuter -executeAll` |
| Execute current line | None | | CE2 adds this |
| Code removed after execute | **Classic behavior:** running with no selection clears the input. Select first, for example with Ctrl+A, to keep the code [F13] *(excerpt)* | | Users lose code to this. A 2017/2018 report describes paste-then-Ctrl+Enter wiping code that only Undo brought back [F7] *(excerpt)* |
| Font zoom | Ctrl + mouse wheel in the input or history pane, Windows only [M3] | | |
| History controls | Edit > Clear History, Clear Input, Clear All [M1] | Toolbar buttons [M4] | |
| Echo All Commands | Writes every MEL command Maya runs, including UI actions, to history [M1] | Toolbar toggle [M4] | MEL only. Users have asked for Python echo [F6] *(excerpt)* |
| Line numbers in errors | History > Line Numbers in Errors [M1] | | Python errors name the pseudo-file `<maya console>` [F11] *(excerpt)* |
| Show Stack Trace | Opens a **separate window** listing errors and line numbers in external script files. *Suppress Stack Window* sends that output to history instead [M1] | | Clickable error lines are not documented (**UNVERIFIED**; probably absent) |
| Output filters | Suppress Command Results (`// Result:`), Info, Warning (`// Warning:`), Error (`// Error:`) and Duplicate Variable messages [M1] | | Suppressed messages still show in the Help Line. `cmdScrollFieldReporter -filterSourceType mel/python` hides one language's output [M7] |
| Log to file | `scriptEditorInfo -historyFilename <f> -writeHistory true` appends history to a file [M8] | | `-clearHistoryFile` starts a fresh log |
| Load, Source, Save | Load Script puts a file's contents in the input. Source Script runs a file. Save Script saves the **selected** text [M1] | Toolbar [M4] | Sourced scripts must be re-sourced after edits [M1] |
| File-backed tabs | File > Open Script: tab named after the file, path tooltip, `*` when modified, Save writes back, reload prompt on external change, delete prompt, restored on restart [M10] | | Built on `fileChangedCommand` and `modificationChangedCommand` [M6] |
| Script to shelf | Select code, then **middle-drag it to the shelf**, or File > Save Script to Shelf [M11][M1] | | |
| Tab persistence | Tabs saved on quit and restored. *Save All Script Tabs* forces a save. On a crash that can write a backup scene, each tab is written next to that scene with a `-ScriptEditor-` suffix [M1] | | Forum reports [F1][F2][F3]: tabs live in `Documents/maya/<ver>/prefs/scriptEditorTemp/`; Maya 2020.2+ also writes `%LOCALAPPDATA%\Temp\<user>.<date>.<time>.ma-ScriptEditor-*` on crash; the backup procedure is `syncExecuterBackupFiles` in `scriptEditorPanel.mel` and can produce empty tabs if the editor is not visible. **UNVERIFIED:** the exact file names inside `scriptEditorTemp` |
| stdin | `input()` / `raw_input` opens a modal dialog [M1] | | |
| Marking menu | Right-click either pane. Both panes: New Tab, Prev, Next, Delete, Rename, Clear All. History pane: Copy, Select All, Clear History. Input pane: Execute, Quick Help, Command Documentation, Undo, Redo, Cut, Copy, Paste, Select All, Clear Input [M5] | RMB | |
| Scriptable editor | Every part is a MEL/Python UI command: `cmdScrollFieldExecuter` (execute, executeAll, storeContents, loadContents, search*, showLineNumbers, autoCloseBraces, commandCompletion, objectPathCompletion, filterKeyPress, dragCallback, and others) and `cmdScrollFieldReporter` (filterSourceType, echoAllCommands, lineNumbers, stackTrace, suppress*) [M6][M7] | | TDs can build their own editors from these widgets |

### B. Charcoal Editor 2 (v2.10.x)

| Feature | Behavior | Shortcut | Notes |
|---|---|---|---|
| Launch | Windows > General Editors > Charcoal Editor 2; the Script Editor button; MEL `charcoalEditor2` [S3] | | Preference *Override Script Editor Button* sends Maya's button to CE2 [S18] |
| Buffers and files | **Temporary buffers** survive sessions, and their temp file is deleted when the buffer is closed. **File buffers** read and write real files. Language (MEL, Python or None) is set when the buffer is created or from the file extension [S3] | New **Ctrl+N** (language dialog) or double-click empty tab-bar space. Open **Ctrl+O**. Save **Ctrl+S** [S3] | Save on a temp buffer becomes Save As. File menu also has New MEL Buffer, New Python Buffer, Open Recent, Save All, Rename (renames the file on disk), Revert File, Close All, Save State [S9] |
| Tabs | Drag to reorder or move to another group. Optional close buttons. Lock tabs. Mini toolbar with run buttons. Mouse wheel switches tabs (2.2.1) [S4][S18][S5] | Go > Switch Editor / Switch Groups [S12] | Optional confirmation before closing a temp buffer [S18] |
| Split editing | Drag a tab to a group edge. An overlay previews the new group, and empty groups hide themselves. Toggle horizontal or vertical layout [S4][S11] | | Layout persists between sessions [S4] |
| Execute | Runs the selection, or the whole buffer if nothing is selected [S3][S13] | **Ctrl+Enter** | Selecting and executing a MEL variable prints its value [S4][S13] |
| Execute All | Runs the whole buffer regardless of selection [S3] | **Ctrl+Shift+Enter** | |
| Execute Line | Runs the line under the cursor with **leading whitespace stripped**, so indented Python lines run [S3][S4] | **Ctrl+Alt+Enter** | |
| Keep code | **Never deletes code on execute** [S1][S4] | | A headline difference from native Maya |
| Save on execute | Saves the buffer or file before running. On by default and can be turned off. *Save all on exit* is separate [S3][S18] | | This is CE2's crash-safety mechanism |
| Highlighting | MEL and Python, plus a new Python 3 highlighter in 2.9.1. The output pane is highlighted too [S1][S5] | | |
| Autocomplete | MEL and Python. Options: include keywords, include built-ins. **Forced Imports** is a semicolon-separated list of imports (for example `import maya.cmds as cmds;import pymel.core as pmc`) that are pre-imported so their members complete without importing them in the buffer. When a completion is a Maya command, it updates Quick Help. f-string completion (2.9.1). Falls back to Maya's native completion (2.7.1) [S17][S5] | Configurable triggers [S17] | Pitched as better than native completion, which only covers the main namespace [S1]. **UNVERIFIED:** whether completion is static parsing or live `dir()`; Forced Imports implies live introspection |
| Quick Help | Side Bar view that lists a MEL command's flags [S14] | View > Quick Help [S11] | |
| Error checking | A built-in minimal checker shows errors and some warnings. Pylint, if enabled, replaces it, configured with a pylintrc file and Maya stub files (`pylintStubs.zip`). For Maya 2022+, install with `mayapy -m pip install --user pylint` [S21][S22][S23][S17] | Command > Pylint submenu [S13] | The default hides style noise, which matters for adoption |
| Indentation | Smart indent, indent-to-bracket (2.1.8), indent guides, backspace to tab stop, tab size, replace tabs with spaces, auto-indent, all configurable per language [S17][S19][S26][S5] | | |
| Pairs | Inserts closing parentheses and quotes. Highlights matching braces and matching words (whole-word option); the word highlight also works in Output [S17][S4][S19] | | |
| Comments and lines | Edit > Comment submenu (toggle) and Edit > Line submenu (line utilities) [S10] | Not listed in the docs | **UNVERIFIED** default keys |
| Whitespace and wrap | Toggle Render Whitespaces, Toggle Word Wrap [S11] | | |
| On save | Trim trailing whitespace, ensure a final newline [S17] | | 2.10.2 fixed trimming that unfolded folds [S5] |
| Folding | Code folding (2.10.1) [S5] | | Arrived late |
| Scroll past end | Scroll beyond the last line (2.9.2, experimental) [S5] | | |
| Find and replace | Find and Replace dialogs. Find in Files and Replace in Files in the Side Bar, scoped to workspace folders with adjustable filters [S10][S14] | Select-and-find next/previous hotkeys (2.3.2) [S5] | |
| Go To | Go to File, Go to Symbol in File, Go to Line, all in one Go To dialog with filters [S12] | Not listed | Like a command palette |
| Explorer | Workspace (folder tree that can create files and folders), Outline (Python imports, classes and methods; MEL globals and procs; click to jump), Open Editors [S14] | | Workspaces can be saved and opened [S9] |
| Output verbosity | Echo All, Normal (default), Minimal. The docs warn that Echo All hurts performance. A preference disables Echo All at startup [S6][S7][S8] | Output menu | |
| Output styling | Errors red, warnings yellow, results green. Custom highlight rules [S6][S8] | | |
| Output limits | Buffer Size caps the lines kept; the oldest lines are dropped [S8] | | Prevents the native editor's slowdown |
| Output to file | Output > Write Output to File [S7] | | |
| Errors | Line numbers in errors, Show Stack Trace, Suppress Stack Window. Status bar error and warning counts (2.3.1) [S7][S5] | | **UNVERIFIED:** whether output error lines are clickable to jump to source |
| Suppression | Output-suppression options (2.10.1). Flags `-suppressErrors`, `-suppressInfo`, `-suppressResults`, `-suppressWarnings` on `charcoalEditor2` [S5][S20] | | |
| Introspection helpers | `dir()` and `help()` context-menu items (2.4.1), an Insert File Path dialog, `sys.path` and `os.environ` menu items (2.6.4), Command > Environment Variables prints Maya env vars [S5][S13] | | Small conveniences TDs use daily |
| Module state | Revert Module State removes every module loaded since CE2 opened. Revert on Execute does this automatically. Update Module State re-snapshots [S16] | Command > Python, context menu, or a custom hotkey | Replaces `reload()` chains. Caveat: modules imported at startup are never reverted [S16] |
| Shelf | Save Script to Shelf, using the selection if there is one. Drag and drop to the shelf [S9][S1] | | |
| Zoom | Increase, Decrease and Reset Font Size. Ctrl+wheel can be turned off [S11][S19] | | |
| Hotkeys | Hotkey Manager: several shortcuts per action, conflict display, restore defaults. Stored in `C:\Users\<user>\Documents\maya\<ver>\prefs\charcoalEditor2\charcoalEditor2_bindings.ini`, which can be copied between machines [S15] | | Default bindings other than execute are **not documented** |
| Layout and theming | Toggle the Code Editor, Output, Side Bar, Activity Bar and Status Bar. Move the Side Bar left or right. Output position. Dockable and workspace-aware. Fonts and colors [S11][S1] | | |
| Encoding | UTF-8 option (2.3.2). Windows hiDPI fix (2.6.4) [S5] | | |
| Scripting API | `charcoalEditor2 -clearOutput -openFile <path> -revertModuleState -suppress* -version` [S20] | | The editor can be driven from other tools |
| Multi-cursor | Not documented | | **UNVERIFIED**, probably absent |

---

## Execution model

### Maya
- **Language per tab.** Each tab runs MEL or Python. `cmdScrollFieldExecuter -sourceType` is set when the tab is created and cannot be changed later [M6][M1].
- **Execute versus Execute All.** `-execute` runs the selection or else everything. `-executeAll` ignores the selection [M6]. In the classic UI, running everything clears the input [F13] *(excerpt)*.
- **Shared namespace.**
  - All Python tabs share one global scope, so a module imported in one tab is available in every tab. MEL globals are shared the same way [F8].
  - Autodesk does not document this scope as `__main__`, but TDs treat it that way (**UNVERIFIED** in docs).
  - `if __name__ == "__main__":` behavior from the editor is a recurring confusion.
  - 3ds Max documents outright that `__name__` and encoding declarations do not work when code is evaluated from its Editor or Listener [X2].
- **Output capture.**
  - Maya replaces `sys.stdout` with a `maya.Output` object that writes to the Script Editor. The original stream (`sys.__stdout__`) still goes to the Output Window.
  - Under Python 3, writes to `sys.__stdout__` may need an explicit `flush()` [F10] *(excerpt)*.
  - Results print as `// Result: … //` for MEL and `# Result: … #` for Python. Warnings and errors use `// Warning:` and `// Error:` for MEL, `# Warning:` and `# Error:` for Python [M1][M20].
  - **UNVERIFIED:** whether native history lines are color-coded. CE2's own output pane colors errors red, warnings yellow and results green [S6].
- **Errors mapped to lines.**
  - *Line Numbers in Errors* adds line numbers to messages. Python errors from the editor name the pseudo-file `<maya console>` [M1][F11].
  - MEL parses the whole input despite syntax errors, while Python stops at the first one [M13].
  - *Show Stack Trace* opens a separate stack window for errors in external files, and *Suppress Stack Window* sends that output to history instead [M1].
  - The GUI exception formatter can be overridden through `maya.utils.formatGuiException`, whose private default is `maya.utils._guiExceptHook` [F12] *(excerpt)*.
- **Undo.**
  - The docs describe no automatic undo chunk for each Script Editor execution.
  - The documented tool is `cmds.undoInfo(openChunk=True)` / `closeChunk=True`, which should be wrapped in try/finally because the docs say "use with CAUTION" [M16].
  - Anecdotally, a loop of `move` calls run from the Script Editor undid in one step, while the same code run from a PySide button needed one undo per call [F9]. Treat the editor's undo grouping as **UNVERIFIED**.
- **Long-running scripts.**
  - Scripts run on the main thread, so Maya's UI freezes.
  - There is no generic way to break an infinite loop [F14] *(excerpt)*.
  - Cooperative cancel works through `progressWindow(isInterruptable=True)`, which shows "Hit ESC to Cancel", plus polling `isCancelled` [M14]. C++ plugins use `MComputation` with Esc [M15].
  - CE2 documents no interrupt feature.
- **stdin.** `input()` opens a modal dialog [M1].
- **Module reloading.** Natively you chain `importlib.reload()` calls. CE2's module-state revert drops every module imported since it opened [S16].
- **Logging.** `scriptEditorInfo -historyFilename/-writeHistory` echoes history to a file [M8]. CE2 has *Write Output to File* [S7].

### commandPort and external IDEs
- **`commandPort`** [M9]:
  - Naming: `-name ":7001"` opens a TCP socket on localhost. A bare name opens a Unix socket at `/tmp/<name>`. A leading `/` gives a full socket path.
  - `-sourceType mel|python` sets the language; the default is MEL.
  - `-echoOutput` sends all output back over the socket, not just the result. `-noreturn` sends results to the Script Editor and nothing back.
  - `-bufferSize` defaults to 4096. Longer commands close the connection, and longer results are replaced with an error.
  - `-prefix <cmd>` wraps every incoming write in that command.
  - `-securityWarning` and `-pickleOutput` are also available. I/O is UTF-8.
  - **Security:** INET ports have no authentication, so any local client can run `system()` with the user's rights. `-prefix` is the documented mitigation.
- **MayaCharm (PyCharm)**:
  - "Execute Selection" (**Alt+S**) and "Execute Document" (**Alt+A**) in the Run menu.
  - Ports set per Maya install; the plugin generates the `commandPort` code to paste into `userSetup.py`.
  - Maya instances appear in *Attach to Process*. Debugging through its run configuration was dropped as unreliable [I1].
  - Sends code over the command port. A "Maya Log" tool window shows Maya's output. Users asked for indentation normalization so code inside classes and functions could run [I2].
- **MayaSublime**: separate MEL and Python ports (Python default 7002), Ctrl+Return to send. Output goes back to Sublime only if `receive_maya_output` is enabled. Advertises "Maya undo support" [I4].
- **MayaCode (VS Code)**: MEL highlighting, outline, completion with flag info, snippets. "Send Python/MEL Code to Maya" commands over `commandPort -name "localhost:7001" -sourceType "mel" -echoOutput`. Versions after 0.9 use one port for both languages [I3].
- **Port type matters.** Python sent to a MEL-typed port fails [I2]. Port and language mismatch is a common setup problem.

---

## MotionBuilder Python Editor

- **Layout** (Window > Python Editor) [B1]:
  - a toolbar;
  - an **interactive console**, an IDLE-like REPL with a `>>>` prompt, command history, completion and color-coded errors;
  - a tabbed **work area**.
- **Toolbar commands** [B1]: create, load, close and save scripts; execute; add script to scene; open the Python Tool Manager; launch the Telnet console; context help; Python Logger; clear console or work area; search console, work area or both; action buttons.
- **Shortcuts** [B2]:

| Key | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+O | Load |
| Ctrl+- | Close |
| Ctrl+S | Save |
| Ctrl+F2 | Save new |
| **F5** | Execute the highlighted code, or the whole script if nothing is highlighted |
| Ctrl+= | Add to scene |
| F10 | Tool Manager |
| F1 | Context help |
| Ctrl+F | Find |
| F3 | Find next |
| Ctrl+H | Replace |
| Ctrl+G | Go to line |
| Alt+P / Alt+N | Previous / next console history |
| Tab | Indent |
| Alt+C / Alt+U | Comment / uncomment |
| Ctrl+Space | Complete |

  These keys **override MotionBuilder's global hotkeys while the editor is active** [B2]. Shortcuts live in `PythonKeyboard.txt` under `bin\config\Python\Keyboard` *(excerpt)*.
- **Autocomplete** works only on objects that already exist: you instantiate the class (for example `x=FBModel(...)`) and run it before `x.` completes. Uninstantiated names give nothing [B3][B4].
- **Expression evaluation:** select a variable or expression in a tab and press Enter to print its value into the console history [B1].
- **Console color defaults:** comments red, strings green, keywords orange, output blue, reserved words purple [B3].
- **Preferences** [B5]:
  - syntax highlight, smart highlighting, current-line highlight, line numbers, auto-indent;
  - **Reload last scripts on startup**;
  - Show Close Button on Each Tab (2022+);
  - **File Status Auto-Detection**, which prompts or silently reloads files changed on disk;
  - Python startup folder; colors; font.
- An *Auto-Save Unnamed Scripts on Exit* preference, on by default, is reported but the version is **UNVERIFIED** *(excerpt)*.
- **Startup and remoting:**
  - `Documents\MB\<ver>\config\Python\pythonidelib_init.py` runs when the editor opens [B1].
  - `config/PythonStartup` and `bin/config/PythonStartup` run at application start; `MOTIONBUILDER_PYTHON_STARTUP` adds more paths [B9] *(excerpt)*.
  - The Python **telnet server on port 4242** needs both *Enable Server* and *Allow Network Connections* [B5][B8].
- **Python Tool Manager** (F10) lists tools from the PythonStartup folder. Reopening a tool through it keeps the tool's state, whereas re-running the script restarts it [B6].
- **Python Status Bar** (2024): a dockable bar that shows the latest Python output line, turns red on an error, and has a button that opens the editor [B7].

## 3ds Max: Scripting Listener, Scripting Editor and Python

- **Scripting Listener** (F11; Scripting > Scripting Listener; Mini Listener right-click) [X1]:
  - Only one instance at a time; it keeps its text when closed.
  - A **pink Macro Recorder pane** on top and a white output pane below. Running code from the recorder pane sends output to the output pane, so the recording stays clean.
  - **Python mode** is chosen with a radio button or by typing `>>>` and a space. Enter adds a newline and **Ctrl+Enter executes the multi-line block** [X2].
  - Can be installed as an extended viewport.
  - Colors and fonts come from MAXScript globals such as `listenerBackgroundColor` and `pythonPromptColor`.
  - *Auto Open Listener on Output* is a preference [X1].
- **Mini Listener:** a single-line view in the status bar. Its right-click menu re-runs earlier commands [X1].
- **Macro Recorder** [X6]:
  - Records most menu, toolbar, Create-panel and Modify-panel actions as MAXScript.
  - Options: explicit versus selection-relative object names, absolute versus relative transforms, explicit versus selection-relative sub-object sets, and whether to record panel switches, tool selections and menu items.
  - **Drag recorded text onto a toolbar to create a MacroScript button.**
  - Records MAXScript only, not Python.
- **Scripting Editor (formerly the MAXScript Editor)** [X4]:
  - **Scintilla-based**, configured through SciTE-style properties files (`MXS_Editor.properties` global, `MXS_EditorUser.properties` user override, `MXS_EditorAbbrev.properties`, `MAXScript.properties`, `python.properties`) [X3].
  - Tabs: 10 by default, up to 100 if configured. **Alt+1..0** jumps to the first 10 tabs and Ctrl+Tab cycles. `*` marks unsaved files. Middle- or right-click a tab for close, save and print.
  - Folding (Ctrl+Shift+Plus), line numbers, Go To (Ctrl+G).
  - Brace navigation and selection (Ctrl+B / Ctrl+Shift+B).
  - Bookmarks (Ctrl+F2 to set, F2 / Shift+F2 to move between them).
  - **Ctrl+right-click jumps to a definition.**
  - Regex find and replace, cloned and side-by-side views, custom status bars and context menus.
- **Editor execution** [X3][X2]:
  - **Ctrl+E** Evaluate All.
  - **Evaluate Line/Selection** is **Ctrl+Enter = NumPad Enter** in the 2025 Tools-menu page, but **Shift+Enter** in the 2026 Python page. **UNVERIFIED** which is current; it may differ by language mode.
  - F4 / Shift+F4 next / previous message.
  - Shift+F5 Clear Output; Ctrl+F6 switch between editor and output.
- **Completion and abbreviations:**
  - Document-word completion ("Complete Word").
  - An **auto-generated API keyword list since 3ds Max 2020.1**, controlled by `autocomplete.MAXScript.useautogeneratedapi=1`, or generated manually with `generateAPIList()` [X5].
  - Abbreviations such as `cd=CreateDialog` expand with Ctrl+Shift+A, and Ctrl+Shift+R opens a list of them [X7] *(excerpt)*.
- **Python execution** [X2]:
  - Editor in Python mode; Scripting > Run Script for anything that relies on `__name__` or an encoding declaration.
  - `3dsmax -U PythonHost script.py`; `3dsmaxbatch.exe`.
  - `.pyc` files run since 2022.2.
- **Debugging:** the official tutorial uses **debugpy** `listen(("localhost", 5678))` from the Listener, then VS Code "Python: Remote Attach". You must point `sys.executable` at the bundled `python.exe` for the duration of the `listen()` call [X8].
- **3ds Max 2026.1 additions:**
  - `systemTools.debugBreak()`;
  - control over call-stack capture depth on exceptions (`MXSCallstackCaptureDepth`, a `#numFrames:` argument);
  - Edit > Reset Zoom in the Listener [X9].
- **External editors:** MXSPyCOM is a COM bridge (`-f` runs a file, `-e` opens it in Max's editor, `-s` runs it silently with errors sent to the Listener) [X10].

---

## User sentiment

**What TDs value**
- **Persistent, unlimited, background-saved tabs**, execute-the-selection, and **one global scope across tabs**. A long-time Maya TD named these as the things missing in Blender [F8].
- **Echo All and the history pane as a learning tool:** watch the MEL a UI action produces, then script it [M1]. 3ds Max's Macro Recorder fills the same role, plus drag-to-toolbar [X6].
- **Quick Help and inline flag lists** for `maya.cmds` and PyMEL [S30].
- **Being inside the app**, with no bridge to set up. CE2 is praised as a strong replacement for the native editor: one TD said it "blows the script editor out of the water" [F2], while others say it is not a full IDE [S30].
- **Middle-drag to shelf** for turning snippets into tools instantly [M11].

**What TDs complain about**
- **Lost or blank tabs after crashes.**
  - Tabs were historically saved only on exit or when preferences were saved. Backups could come back empty, and the save procedure seemed to need the editor visible [F1] *(excerpt)*.
  - A third-party autosave tool exists because users lost work [F3].
  - Maya 2022.2 added *Save All Script Tabs*. Maya 2023 users report it is better, but restoring is still "an inconvenience" [F2].
  - Maya Ideas posts ask for a save button because "Maya hard crashes or hangs = script lost" [F5] *(excerpt)*.
- **Code vanishes on execute** unless it is selected first [F13] *(excerpt)*, plus the paste-then-execute accident [F7] *(excerpt)*. CE2 advertises "never deleting code on execute" as a feature [S1].
- **History pane slowdowns:** a large history makes the Script Editor slow to open, and users ask how to clear it from MEL without opening the editor [F4] *(excerpt)*. Echo All "can affect Maya's performance" [S6].
- **MEL-only echo:** there is a request for Python-formatted history output [F6] *(excerpt)*.
- **Weak autocomplete:** native completion only covers the main namespace [S1]. External IDEs need stub or skeleton files and break on chained expressions like `sel[0].` [S30][F15] *(excerpt)*. MotionBuilder only completes objects that already exist [B3].
- **No debugger.** TDs attach PyCharm or VS Code, and early Eclipse setups often failed to hit breakpoints [F15] *(excerpt)*. MayaCharm dropped its debug run configuration as unreliable [I1].
- **External-editor workflow gripes:** some TDs only use the Script Editor to read output and run code from Sublime, gvim or Eclipse over commandPort [S30]. MayaCharm users had to dedent snippets by hand [I2].
- **The Maya Ideas "modern redesigned script editor" thread** asks for multiple cursors, multi-pane views, tab/space replacement, plugins and customizable highlighters [F5] *(excerpt)*.
- **Python 3 migration friction:** old Python 2 `print` statements give a generic syntax error that points at `<maya console>` [F2][F11].
- **Third-party replacements stall:** Joe's Script Editor (JSE) is still "unstable pre-alpha" [I5].

---

## Steal this (for the Unreal editor)

1. **Ctrl+Enter runs the selection, or the whole buffer when nothing is selected; Ctrl+Shift+Enter runs everything; Ctrl+Alt+Enter runs the current line** (CE2), with Numpad Enter as an alias (Maya, Max). This is the muscle memory TDs bring.
2. **Never delete code on execute** (CE2). This removes Maya's oldest footgun.
3. **Dedent the selection or line before running it** (CE2 Execute Line; MayaCharm request). Lets TDs run a block from inside a function or class body.
4. **Select an expression and execute to print its value** (CE2 MEL variables; MotionBuilder select+Enter; Maya `# Result:`). The fastest inspection loop there is.
5. **Save every buffer continuously and on execute, to a journaled scratch folder, and restore tabs exactly** (CE2 save-on-execute; Maya *Save All Script Tabs*). Crash-proof tabs were the number-one complaint.
6. **Two kinds of tab: temp scratch buffers and file-backed tabs**, with `*` for unsaved changes, a path tooltip, a reload prompt on external changes, and a prompt when the file is deleted (Maya 2018+, CE2, MotionBuilder auto-detect). Covers both scratch work and real tool development.
7. **One shared interpreter namespace across tabs, plus a "Revert module state" command and an optional revert-on-execute** (Maya; CE2). Keeps the convenience of global scope while fixing stale-module bugs without `reload()` chains.
8. **Typed output lines (input echo, stdout, result, info, warning, error), color-coded, with a per-type suppress toggle and Echo All / Normal / Minimal verbosity** (Maya History menu; CE2). Lets users quiet the noise without losing errors.
9. **A bounded, virtualized output buffer with a line cap, and optional log-to-file** (CE2 Buffer Size and Write Output to File; Maya `historyFilename`). Avoids the "history makes the editor slow" problem.
10. **Clickable error lines that jump to the tab or file and line, plus next/previous-message keys** (Max F4 / Shift+F4; Maya line numbers in errors). Python tracebacks should be navigable, not just readable.
11. **Inline tracebacks in the output, not a separate modal stack window** (Maya's *Suppress Stack Window* exists for this reason). Fewer windows and better flow.
12. **Error and warning counters in the status bar, and a small global status strip that turns red on an error** (CE2 2.3.1; MotionBuilder Python Status Bar). Errors are visible even when the editor is hidden.
13. **Autocomplete that combines static stubs with live introspection of the session namespace, plus configurable "forced imports"** (CE2; Max's auto-generated API list). Never require users to run code before completion works, which is MotionBuilder's limitation.
14. **Quick Help / signature side panel and "open docs for the symbol under the cursor"** (Maya Quick Help and marking-menu Command Documentation; CE2 Quick Help; MotionBuilder F1). For Unreal this means `unreal.*` docstrings and links to the Python API docs.
15. **`dir()` and `help()` on the selection from the context menu, plus `sys.path` and `os.environ` inspectors and an Insert File Path dialog** (CE2). Cheap to build and used daily.
16. **Turn a selection into a tool by dragging it to a toolbar or menu** (Maya middle-drag to shelf; Max recorder drag to toolbar). In Unreal, drop it onto a toolbar or menu entry or a saved snippet.
17. **An action recorder that writes the Python equivalent of editor actions, with the recorder pane separate from the output pane** (Max Macro Recorder; Maya Echo All, which is MEL-only and expensive). The top learning feature, and Python echo was a missing request in Maya.
18. **Outline, Go to File/Symbol/Line palette, workspace folder tree, and find/replace in files** (CE2; Max jump-to-definition with Ctrl+right-click). The PyCharm feel inside the host app.
19. **Split editor groups, draggable tabs, and a layout that persists** (CE2; Max cloned views).
20. **Lint with a quiet default ruleset (errors and real warnings only) and optional full Pylint or Ruff with a project config and Unreal stubs** (CE2). Avoids style noise drowning out real errors.
21. **Whitespace rendering, per-language tab/space settings, trim trailing whitespace on save, ensure a final newline** (Maya 2022.2; CE2). Mixed indentation is a classic Python failure.
22. **Cooperative cancel (Esc or a Stop button) plus progress reporting from scripts** (Maya `progressWindow isInterruptable`). The minimum answer to a frozen editor.
23. **One undo transaction per execution, configurable and consistent across every execution route** (Maya's documented `undoInfo` chunking; the inconsistent anecdotal behavior is the warning). Users expect Ctrl+Z to undo a script in one step.
24. **A localhost-only, opt-in, authenticated remote-execution endpoint and a debugpy attach command** (Maya `commandPort`, MotionBuilder 4242 behind two toggles, Max debugpy on 5678, MayaCharm attach). Supports PyCharm and VS Code workflows without the `commandPort` security hole.
25. **Expose the editor as a scriptable API** for opening files, clearing output, toggling suppression and running buffers (Maya `cmdScrollField*`; `charcoalEditor2` flags). Studios automate their editors.

## Avoid this

- **Clearing the input after a non-selection execute.** It destroys work, and recovery depends on Undo [F13][F7].
- **Saving tabs only on clean exit, or only while the editor is visible, and writing crash backups to obscure places** (next to the scene file, `%TEMP%` with odd names) [M1][F1][F2].
- **An unbounded, non-virtualized history widget,** and an echo mode that costs performance by default [F4][S6].
- **Echo or recording output in a different language from the one users write** (Maya's MEL-only Echo All) [F6].
- **Autocomplete that only knows objects already executed** (MotionBuilder) or only `__main__` (native Maya) [B3][S1].
- **A separate stack-trace window,** and errors that name `<maya console>` instead of the tab name and line [M1][F11]. Give each tab a real pseudo-filename such as `<tab:MyScratch>`.
- **Breaking `__name__`, `__file__` or encoding declarations when code is run from the editor** (3ds Max documents this) [X2]. Set `__name__ == "__main__"` and a meaningful `__file__` for file-backed tabs.
- **Execute available only on Numpad Enter.** Many laptops have no numpad. Always provide Ctrl+Enter.
- **Inconsistent shortcuts across versions and docs** (3ds Max evaluate-selection is Shift+Enter in one page and Ctrl+Enter in another) [X2][X3].
- **Configuration buried in hand-edited properties files** (3ds Max's SciTE-style files) instead of a settings UI with search [X3].
- **Unauthenticated network command ports,** and language-typed ports that silently reject the other language [M9][I2].
- **Undo grouping that depends on how code was invoked** [F9].
- **Hotkey collisions with the host app.** Follow MotionBuilder's documented rule that editor keys override global keys only while the editor has focus [B2]. Avoid stealing Unreal's global bindings.
- **Shipping essential editor quality only through a paid third-party plugin.** Maya users pay USD 79 for CE2 to get what should be built in [S1].
- **A modal stdin dialog that blocks everything.** Maya's `input()` dialog is acceptable, but a non-blocking inline prompt would be better (design suggestion).

---

## Open questions

1. Native Maya: does Execute (no selection) also clear the input for **file-backed** tabs, or only for scratch tabs? (**UNVERIFIED**)
2. Native Maya: does the history pane color errors and warnings in 2025–2027, and can the colors be set? No docs found.
3. Native Maya: what are the exact file names and format inside `prefs/scriptEditorTemp/`, and how do tab labels and languages persist (optionVars in `userPrefs.mel`)? (**UNVERIFIED**)
4. Native Maya: is each Script Editor execution a single undo chunk for Python, for MEL, or for neither? It needs a hands-on test [F9].
5. Native Maya: does Python completion introspect members of imported modules (`cmds.` → list), or only top-level `__main__` names?
6. CE2: what are the default keybindings for comment toggle, line operations, Go To, Find in Files and Side Bar switching? The docs don't list them, so a trial install would be needed. Is the completion engine static or live? Are output error lines clickable?
7. MotionBuilder: which version added *Auto-Save Unnamed Scripts on Exit*? Does F5 execute in the console context too?
8. 3ds Max 2026/2027: is the current evaluate-selection key Shift+Enter or Ctrl+Enter, and does it differ between MAXScript and Python modes?
9. For our spec: should the Unreal equivalent of "Echo All" log editor transactions or console commands as Python? This overlaps the Unreal-internals lane.

---

## Sources

**Charcoal Editor / Zurbrigg**
- [S1] https://zurbrigg.com/charcoal-editor-2 (features, price, builds)
- [S2] https://zurbrigg.com/charcoal-editor-2-documentation
- [S3] https://zurbrigg.com/charcoal-editor-2-documentation/item/charcoal-editor-2-basics
- [S4] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-workflow-tips
- [S5] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-release-notes
- [S6] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-output
- [S7] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-output-menu
- [S8] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-output-preferences
- [S9] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-file-menu
- [S10] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-edit-menu
- [S11] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-view-menu
- [S12] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-go-menu
- [S13] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-command-menu
- [S14] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-side-bar
- [S15] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-hotkey-manager
- [S16] https://zurbrigg.com/charcoal-editor-2-documentation/item/avoiding-reload-with-module-states
- [S17] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-python-preferences
- [S18] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-general-preferences
- [S19] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-code-editor-preferences
- [S20] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-mel-commands
- [S21] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-pylint-overview
- [S22] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-installing-pylint
- [S23] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-maya-stub-files-for-pylint
- [S24] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-troubleshooting
- [S25] https://zurbrigg.com/charcoal-editor-2-documentation/item/installing-charcoal-editor-2
- [S26] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-tabs-and-spaces
- [S27] https://zurbrigg.com/charcoal-editor-2-documentation/item/ce2-mel-preferences
- [S28] https://www.cgchannel.com/2012/11/chris-zurbrigg-ships-charcoal-editor-for-maya
- [S29] https://lesterbanks.com/?p=18387 (CE1 feature list, 1 Nov 2012)
- [S30] https://www.tech-artists.org/t/anyone-tried-out-the-charcoal-editor-for-maya-by-zurbrigg/3289

**Autodesk Maya docs**
- [M1] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Scripting/files/GUID-B66020CC-1BBE-4EA1-B1DE-A288B044DD80.htm (menus and hotkeys, 2026)
- [M2] https://help.autodesk.com/cloudhelp/2027/ENU/Maya-Scripting/files/GUID-B66020CC-1BBE-4EA1-B1DE-A288B044DD80.htm (same page, 2027)
- [M3] https://help.autodesk.com/cloudhelp/2025/ENU/Maya-Scripting/files/GUID-7C861047-C7E0-4780-ACB5-752CD22AB02E.htm (open Script Editor, execution, tabs, zoom)
- [M4] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Scripting/files/GUID-7E1FCC98-A93E-4659-A746-F428BC63818B.htm (toolbar)
- [M5] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Scripting/files/GUID-E019B058-BA24-4362-9EF4-A8F614DFF0D3.htm (marking menu)
- [M6] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/cmdScrollFieldExecuter.html
- [M7] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/cmdScrollFieldReporter.html
- [M8] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/scriptEditorInfo.html
- [M9] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/commandPort.html
- [M10] https://help.autodesk.com/cloudhelp/2025/ENU/Maya-Scripting/files/GUID-43D4804F-0F7B-411C-B91A-0377603E611C.htm and https://help.autodesk.com/cloudhelp/2018/ENU/Maya-Scripting/files/GUID-43D4804F-0F7B-411C-B91A-0377603E611C.htm (file-backed tabs)
- [M11] https://help.autodesk.com/cloudhelp/2025/ENU/Maya-Scripting/files/GUID-527023AE-9FB5-4D01-8D29-075B1E6C4754.htm (shelf button)
- [M12] https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Scripting/files/GUID-19B8FB68-AF9B-43A2-84EC-5C7A3AE46CB2.htm (help on Python commands, Ctrl+Space)
- [M13] https://help.autodesk.com/cloudhelp/2016/ENU/Maya/files/GUID-1D52B6D8-4258-472D-838E-77FB66DB29B1.htm (error line numbers)
- [M14] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/progressWindow.html
- [M15] https://help.autodesk.com/cloudhelp/2026/ENU/MAYA-API-REF/cpp_ref/class_m_computation.html
- [M16] https://help.autodesk.com/cloudhelp/2018/ENU/Maya-Tech-Docs/CommandsPython/undoInfo.html
- [M17] https://download.autodesk.com/us/Maya/2009help/files/Basics_Windows_and_Editors_Script_editor.htm
- [M18] https://help.autodesk.com/cloudhelp/2020/ENU/Maya-Scripting/files/GUID-B66020CC-1BBE-4EA1-B1DE-A288B044DD80.htm
- [M19] https://www.cgchannel.com/2021/09/autodesk-ships-maya-2022-2/
- [M20] https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/error.html

**Forums and community** (*(excerpt)* = page returned 403 or was read only through a search-index excerpt)
- [F1] https://forums.autodesk.com/t5/maya-programming-forum/mel-command-save-contents-of-script-editor-tabs-and-mtk-shelf/td-p/9538713 *(excerpt)*
- [F2] https://www.tech-artists.org/t/maya-2023-general-discussion/14929
- [F3] https://community.highend3d.com/maya/script/script-editor-autosave-for-maya
- [F4] https://forums.autodesk.com/t5/maya-programming-forum/is-it-possible-to-clear-the-script-editor-output-without-opening/td-p/10476983 *(excerpt)*
- [F5] https://forums.autodesk.com/t5/maya-ideas/modern-redesigned-script-editor/idi-p/7963306 *(excerpt)*
- [F6] https://forums.autodesk.com/t5/maya-ideas/python-history-output/m-p/7963353 *(excerpt)*
- [F7] https://forums.autodesk.com/t5/maya-forum/script-editor-paste-gt-execute-bug-in-maya-2017-and-2018/m-p/7417092 *(excerpt)*
- [F8] https://devtalk.blender.org/t/longtime-maya-td-making-the-transition-scripting-workflow-questions/14045
- [F9] https://groups.google.com/g/python_inside_maya/c/cZs0oHYcKks/m/VCwGw5wIrd8J
- [F10] https://forums.autodesk.com/t5/maya-programming/maya-2022-and-sys-stdout-write/m-p/10206828 *(excerpt)*
- [F11] https://forums.autodesk.com/t5/maya-programming-forum/maya-2023-script-editor/m-p/11792357 *(excerpt)*
- [F12] https://tech-artists.org/t/rogue-usersetup-py-breaking-maya/17187 and https://www.tech-artists.org/t/maya-coding-conundrums/6139 *(excerpt)*
- [F13] https://subscription.packtpub.com/book/game-development/9781785283987/1/ch01lvl1sec04/running-code-from-the-script-editor *(excerpt)*
- [F14] https://forum.highend3d.com/t/how-to-interrupt-a-process-script-while-its-processing/697 *(excerpt)*
- [F15] https://www.tech-artists.org/t/ide-with-maya-autocomplete/1003 *(excerpt)*

**IDE bridges**
- [I1] https://github.com/cmcpasserby/MayaCharm
- [I2] https://www.tech-artists.org/t/maya-charm-maya-pycharm-connection/5000
- [I3] https://marketplace.visualstudio.com/items?itemName=saviof.mayacode
- [I4] https://packagecontrol.io/packages/MayaSublime
- [I5] https://github.com/j0yu/JSE

**MotionBuilder**
- [B1] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-B54F28EE-1261-4289-98F5-A9FBB4D59E02.htm
- [B2] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-42072BD9-ACD8-4E01-B53D-E1822BDF672A.htm
- [B3] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-6173FE36-3B52-4987-829A-5C0317AD56CE.htm
- [B4] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-0EFFE74C-9AA2-4376-A24A-E3EC42536502.htm
- [B5] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-B73DDA92-C674-4C47-9902-8C24AB288412.htm
- [B6] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-D99F0540-13A8-4B5B-A109-74BB0E75E2DC.htm
- [B7] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder/files/GUID-E7B7C6E3-B95B-4EDB-AEE4-E089EB689835.htm
- [B8] https://help.autodesk.com/cloudhelp/2018/ENU/MotionBuilder-SDK/files/GUID-82CF06D0-BC20-4F4A-A7E0-A61CDF2670D2.htm
- [B9] https://help.autodesk.com/cloudhelp/2024/ENU/MotionBuilder-SDK/Getting-Started/Directory-Structure.html *(excerpt)*

**3ds Max**
- [X1] https://help.autodesk.com/cloudhelp/2027/ENU/MAXScript-Help/files/MAXScript-Introduction/Accessing-MAXScript/GUID-81403EB7-94F8-4EAA-8026-1A495343AE06.html
- [X2] https://help.autodesk.com/cloudhelp/2026/ENU/MAXDEV-Python/files/MAXDEV_Python_executing_python_html.html
- [X3] https://help.autodesk.com/cloudhelp/2025/ENU/MAXScript-Help/files/MAXScript-Introduction/The-MAXScript-Editor/MAXScript-Editor-Menu-Options/GUID-029EF4FB-5822-4E33-85A6-BD140F8D26FD.html
- [X4] https://help.autodesk.com/cloudhelp/2025/ENU/MAXScript-Help/files/MAXScript-Introduction/The-MAXScript-Editor/GUID-A1C19386-A8B0-45F9-B4E5-37C2BA270E5E.html
- [X5] https://help.autodesk.com/cloudhelp/2022/ENU/MAXScript-Help/files/MAXScript-Introduction/The-MAXScript-Editor/GUID-96D3ABE3-32CA-491D-9CAD-0A0576346E54.html
- [X6] https://help.autodesk.com/cloudhelp/2025/ENU/MAXScript-Help/files/MAXScript-Introduction/Accessing-MAXScript/GUID-9A814C11-A966-4661-BE14-1A4F0193895E.html
- [X7] https://help.autodesk.com/cloudhelp/2027/ENU/MAXScript-Help/files/MAXScript-Introduction/The-MAXScript-Editor/MAXScript-Editor-Configuration/GUID-7358FB2E-182A-4BB8-8208-3E74D28FC205.html *(excerpt)*
- [X8] https://help.autodesk.com/cloudhelp/2025/ENU/MAXDEV-Python/files/tutorials/MAXDEV_Python_tutorials_attach_debugger_html.html
- [X9] https://help.autodesk.com/cloudhelp/2027/ENU/MAXScript-Help/files/What-is-New-in-MAXScript/What-was-New-in-MAXScript-in/GUID-56622940-9CCA-47BF-B1E2-375AAC5AAD23.html
- [X10] https://www.tech-artists.org/t/mxspycom-a-program-to-enable-editing-of-max-script-files-in-external-code-editors/5993 and https://github.com/techartorg/MXSPyCOM/releases
