> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 02 — Script editors in other DCCs and game engines (non-Autodesk)

Research lane: in-application script editors outside Autodesk (Maya, MotionBuilder and 3ds Max are covered in a separate report). Target: a production Python script editor inside the Unreal Engine 5.8 editor that feels like Maya's Charcoal Script Editor crossed with PyCharm.

Date: 2026-10-28. Citations use the source IDs (e.g. `[H9]`) listed under **Sources**. **UNVERIFIED** marks a claim I could not confirm from a primary source in this pass. The shared web-search budget ran out near the end of the research, so the Substance Painter console, Clarisse, and some C4D Script Manager details stayed thin. They are flagged where they appear.

---

## Summary

1. **Godot is the only product here that ships a first-class code editor *and* debugger inside the engine, and its trick is process separation.** The editor launches the game as a separate OS process and passes `--remote-debug tcp://…` and the current `--breakpoints` list on the command line [G6][G7]. When a breakpoint hits, the *game* process blocks in a nested message loop that pumps OS events and answers `step`/`next`/`continue`/`evaluate` messages. The editor stays fully responsive [G8]. The flip side is that **breakpoints do not work in `@tool` scripts**, i.e. code running inside the editor process [G2]. That is exactly the situation of Unreal editor Python, which runs in-process on the game thread. Whatever debugger we ship has to resolve this tension explicitly (see Open questions).
2. **Godot's editor *is* the language server.** The GDScript LSP (completion, hover, definition, references, rename, signature help, document symbols) runs inside the editor process on port 6005, with a DAP debug adapter on 6006. External IDEs therefore get the same analysis as the built-in editor, but only while the editor is running [G4][G9][G10].
3. **Most DCC editors are thin.** Nuke's Script Editor, Katana's Python tab, Houdini's Python Shell and Source Editor, and Blender's Text Editor are all a single input/output split or a basic text buffer: Tab completion, a history list, run-selection, and little else [N1][N10][H2][B2]. The community repeatedly builds replacements: **KnobScripter** for Nuke [N5][N6], **EVE** and **Mad Coder** for Houdini (both 2025–2026) [H14][H16], and VS Code bridges for every app [N9][B12][C1][O10]. The demand for a "real" in-app editor is clearly unmet across the industry.
4. **The strongest UX ideas come from small tools.** KnobScripter has snippet placeholders (`$$`, `$$sel$$`, `$var$` prompts, `$_$` method receiver), editing code stored on node knobs with explicit Save/Reload, "Run in context", and per-file cursor/scroll memory [N6]. Gaffer's PythonEditor wraps each execution in one undo scope, evals single expressions to print their repr, inserts drag-and-drop code paths, streams output line-by-line, and now runs an MCP server that shares the editor namespace with AI assistants [GF1]. Blender has *Copy as Script* for console history, Python tooltips on every UI control, and completion that evaluates only attribute/index chains, never calls [B1][B4][B5][B7].
5. **Live-introspection completion is the DCC norm.** Blender, Nuke, Katana, Houdini's shell and Gaffer all complete against the live interpreter namespace. Stub packages are what *external* IDEs use: fake-bpy-module, houdini_stubs, Nuke Tools stubs, and Cinema 4D's connector setting completions to the connected app's API version [B15][H18][N9][C1]. Known pain point: huge single-file stubs exceed IDE indexing limits (`hou.py`, bpy stubs and PyCharm's `idea.max.intellisense.filesize`) [H10][B15].
6. **Interrupting long scripts is universally weak.** Houdini relies on cooperative `hou.InterruptableOperation`, which only checks Esc inside `updateProgress()` calls [H8]. Its shell maps Ctrl+C to `KeyboardInterrupt` and so loses Copy [H2]. Nuke offers only threading helpers [N2]. NVIDIA's Python server states that synchronous code cannot be interrupted [O9]. The Omniverse Script Editor warns before running `asyncio.run()` because it would block Kit's main thread [O3]. This is a real chance to differentiate.
7. **The external-bridge state of the art (2026)** is NVIDIA's Isaac Sim Python server. It is a localhost TCP server with a JSON envelope (`code`, `context`, `timeout`, `fire_and_forget`, optional `auth_token`), named isolated namespaces, and top-level `await`. Responses are structured (`status`, `output`, `result`, `traceback`, `ename`, `evalue`), and a matching VS Code extension consumes them [O9][O10]. That design is worth copying almost wholesale for an Unreal bridge.

---

## 1. SideFX Houdini

**Surfaces where Python lives** [H5]: Python Shell pane/window; Python Source Editor (edits `hou.session`); Python Panel pane (embedded PySide6/PyQt6 UIs); shelf tool scripts; HDA `PythonModule` and event-handler sections (OnCreated, OnLoaded, …); parameter expressions, callbacks and menu scripts; Python SOP/LOP nodes; startup scripts (`pythonrc.py`, `ready.py`, `uiready.py`, `123.py`, `456.py`); scene/node/asset event callbacks; and background callbacks via `hou.ui.addEventLoopCallback` or Qt timers.

### Python Shell
- A plain interactive REPL (`>>>`/`...` prompts) with `hou` pre-imported. Up/Down give history, Home/Ctrl+A and End/Ctrl+E move to the line ends, and Ctrl+D closes a floating shell [H2].
- **Ctrl+C sends `KeyboardInterrupt` instead of copying**; Copy is on the right-click menu [H2]. This is a deliberate trade-off that users trip over.
- **Drag a node from the network editor into the shell** and it pastes a `hou.node(...)` expression [H2].
- **Completion is live introspection.** Forum users describe a popup of members after `hou.`, and say it beats WingIDE's static completion [H11]. Users also report it does not complete through a call chain such as `hou.node('/obj').` unless the result is first stored in a variable (UNVERIFIED: forum report, thread not pinned down). That is consistent with completion that refuses to execute calls.
- Execution model: in-process, on the UI/main thread, with a single shared interpreter. Code in the shell, shelf tools and callbacks can all reach the same `hou.session` module [H1].

### Python Source Editor and `hou.session`
- Edits one per-scene module, `hou.session`, whose **source is saved inside the .hip file** [H1]. It can also be read and written programmatically (`hou.sessionModuleSource()`, `hou.setSessionModuleSource()`, `hou.appendSessionModuleSource()`), and those changes take effect without a re-import [H1].
- Long-standing complaints: false syntax errors from trailing comments or carriage returns, no duplicate-line shortcut, awkward indentation, and "weak" overall. Users recommend Sublime as an external editor [H17]. A 2026 third-party plugin author still calls Houdini's code-editing tools "poor" [H16].

### Python Panel (embedding PySide UIs)
- Interfaces are defined in `.pypanel` files found on `$HOUDINI_PYTHON_PANEL_PATH`; the last definition loaded wins [H4].
- Required hook: `onCreateInterface()` returns the root widget. Optional hooks: `onActivateInterface`/`onDeactivateInterface`, `onDestroyInterface`, hip-file before/after Clear/Load/Merge/Save, and `onNodePathChanged(node)` [H4].
- The docs warn not to keep `kwargs["paneTab"]` across calls because it may become invalid [H4]. This is a good model for "Python-defined tool tabs" with lifecycle hooks in Unreal (level load/save ≈ hip events).

### Shelf tools and HDA Python modules
- An HDA's `PythonModule` section is a real module stored *inside the asset*, reached via `node.hdaModule()` or the `hou.phm()` shortcut. Large code can be split into submodules, each stored in its own section [H6][H7].
- SideFX staff advise against using `hou.pwd()` inside a PythonModule and recommend treating it as a context-free standalone file [H19]. Lesson: code stored on assets still needs explicit, testable entry points.

### VEX / code parameter editors
- Wrangle snippet fields have syntax highlighting. A Houdini 16 user reported that autocompletion prompts for VEX already existed in Attribute Wrangles. UNVERIFIED: I only saw this forum report in search snippets, and it isn't in the docs.
- The built-in editor is weak enough that a paid replacement (**EVE**, Dec 2025: $19/yr or $30 perpetual) sells on autocomplete with argument order, tooltip help and errors, rainbow brackets, local-symbol parsing and `#include` awareness [H14][H15]. A free plugin (**Mad Coder**, Aug 2026) adds Python lint, format, type-check, autocomplete and a debug console [H16].
- **External editor:** set `EDITOR`/`VISUAL` in `houdini.env`, then use "Edit in External Editor" (Alt+E, or via the right-click Expression menu). Pain points: the hotkey only links to the field when hovering the code label, saves sometimes don't sync back, and `#include` symbols aren't known to Sublime or VS Code [H12]. MXE adds separate Python/VEX editor settings and uses a file watcher [H13].

### Long-running Python and interrupts
- `hou.InterruptableOperation(operation_name, long_operation_name, open_interrupt_dialog, timeout_ms)` is a context manager that shows status-line or dialog progress. **Esc is only checked when `updateProgress()`/`updateLongProgress()` is called**, and an interrupt raises `hou.OperationInterrupted`. A pure loop that never calls the progress methods cannot be interrupted by this mechanism [H8].
- The same calls let Houdini process events, which "keeps the app responsive on macOS" [H8]. In other words, the UI is otherwise blocked while Python runs.

### Remote access (hrpyc)
- `hrpyc.start_server()` is a thin wrapper over RPyC. It listens on port 18811 and **runs in a separate thread by default** (`use_thread=False` runs it on the main thread). Clients call `hrpyc.import_remote_module()` and get a proxied `hou` [H9].
- **No authentication**; the docs tell you to use a firewall. Proxies don't support `==`, and the connection closes when the connection object is garbage-collected [H9].

### External IDE autocomplete
- `hou.py` exceeds PyCharm's default IntelliSense size limit and has to be raised manually [H10]. Community stub generators such as `houdini_stubs` exist but have type-translation gaps [H18].

**Execution model:** in-process, main thread, shared interpreter. **Autocomplete:** live introspection in the shell; external editors need stubs. **Debugging:** none built in. **Persistence:** code can live *in the scene* (.hip) or *in assets* (HDA sections). **External bridge:** `$EDITOR` round-trip and RPyC. **Pain points:** weak Source Editor, unreliable external sync, cooperative-only interrupts, unauthenticated RPC.

---

## 2. Foundry Nuke (and Katana)

### Nuke Script Editor
- Two panes: **input below, output above**. Ctrl/Cmd+Return (or keypad Enter) runs the code; if there is a selection, only that runs and the text stays in the input [N1].
- By default, **successful statements are removed from the input**; a preference ("clear input window on successful script execution") turns this off. Invalid statements stay put and show an error in the output [N1].
- Previous/Next Script buttons step through history, and there's Clear history. Ctrl+Backspace clears the output. Tab/Shift+Tab indent. Show input only / output only toggles. "Echo all commands to output window" logs every Python command Nuke runs, including GUI-triggered ones [N1].
- **Tab completion** fills in a single match or pops up a list when there are several [N1]. Completion regressions happen: in Nuke 15.1v1 it stopped completing PySide2 modules [N3].
- Nuke 16 moved its built-in editor code to PySide6. The `nukescripts.blinkscripteditor` source shows line-number and completer classes [N4].
- **Threading:** run work in Python threads and marshal to the UI with `nuke.executeInMainThread` / `executeInMainThreadWithResult`; calling those *from* the main thread hangs Nuke [N2]. The Script Editor itself has no interrupt button (UNVERIFIED that none exists; none is documented in [N1]).

### KnobScripter (Adrian Pueyo) — why artists prefer it
KnobScripter (KS3, v3.2 of 23 Mar 2026; Nuke 11+ through 16–17, PySide6, Python 2/3; GPL-3.0 on GitHub) is a full Python/BlinkScript editor that runs as a floating window (Alt+Z on the Node Graph) or a dockable pane [N5][N6]. Its features, from the v3.2 user guide [N6]:

- **Three modes.**
  - *Script mode*: browse, create, duplicate and delete `.py` files in folder dropdowns (including `.nuke` and any custom path), or open a file in an external app. **Saving writes the file; otherwise a `.py.autosave` sidecar is written.**
  - *Node mode*: pick any node (even Root/Preferences), choose from a dropdown of its Python and callback knobs, and edit them. **Nothing changes until you press Save**, and Reload reverts. Modified knobs get a `*` marker.
  - *Blink mode*: a separate UI for BlinkScript `kernelSource` with "Save and Recompile". Because a bad kernel can crash Nuke, it can optionally auto-save a scratch `.blink` file on every compile.
- **Python output console**, equivalent to the default Script Editor's. KS2 hooked into Nuke's own Script Editor widgets: the v2.4 changelog mentions improving "how Nuke's Script Editor widgets are found" [N6]. **I found no "prompt-style console" or "SE mode" in the KS3 guide or README.** Those features may belong to forks or older versions (UNVERIFIED).
- **Run in context** (beta, v2.4+): runs knob code so that `nuke.thisNode()`/`thisKnob()` resolve to the node being edited rather than Root.
- **Snippets** (redesigned in v3.0), stored in plain text at `.nuke/KS3/Snippets.txt`. You type a shortcode after a separator and press Tab. Placeholder syntax:
  - `$$` sets the cursor position.
  - `$$text$$` inserts text pre-selected.
  - `$Var name$` prompts for a value once and substitutes it everywhere it appears.
  - `$_$` is replaced with the word before the dot (`node.cx` → `node.xpos()+node.screenWidth()/2`).
  - Snippets can be scoped per language (Python/Blink/All), and a default library ships with the tool. Snippet-editor changes only apply after "Save all snippets".
- **Code Gallery**: categorized, foldable reference code with title and description. Code can be inserted into the editor or turned into a snippet.
- **Editor helpers**:
  - Tab completion merges Nuke's completer with names defined in the current buffer.
  - Auto-closing brackets and quotes, and wrapping a selection.
  - **Parentheses wrapper**: select text, press Tab, type `help`, run, then Ctrl+Z.
  - Ctrl+Shift+D duplicates a line; Ctrl+Shift+Up/Down moves lines; `#` on a selection toggles comments; Ctrl+F opens find/replace.
  - Ctrl +/- changes font size in both editor and console.
  - Two highlight themes (Nuke default, Monokai).
  - A **Blink keyword hotbox** (double-click a keyword for alternatives with doc tooltips).
- **State memory**: remembers scroll, selection and cursor per knob or `.py` file, so a new KS window opens where you left off. A "Clear history" option resets it.
- Adoption: Nukepedia rates it 5/5 (6 ratings, ~4k downloads at fetch time) [N7].

Why artists prefer it, synthesized from the guide's stated goals and features [N6]: it removes the pain of editing Python *stored on nodes* (callbacks and buttons). It brings IDE ergonomics (snippets, line ops, find/replace) without leaving Nuke. And it keeps the familiar SE console. In short, it is "enough IDE" for daily work.

### Nuke external bridge
- **Nuke Tools** (VS Code, MIT) talks to the companion `nukeserversocket` plugin running inside Nuke. It runs the active file in Nuke, shows output in VS Code, ships Nuke/Hiero stubs, executes BlinkScript (creating or updating a BlinkScript node), and offers a preview Nodes Panel for editing Python knobs. It works with Houdini too. No debugger is mentioned [N9].

### Katana Python tab (brief)
- The same input/output split as Nuke, run with Ctrl+Return; selection-only runs keep the input. History is Alt+Up/Down [N10].
- **Two completion modes** in preferences (`python > autoCompletionBehavior`): *Shell* (Tab, matches listed in the output) or *IDE* (a popup as you type) [N10]. It is a nice, cheap preference that serves both REPL purists and IDE users.

---

## 3. Blender

### Python Console
- Each console *region* gets its own `code.InteractiveConsole`, keyed by `hash(context.region)`; the console set is rebuilt when the window manager changes, e.g. after a file load [B3].
- stdout/stderr are redirected into StringIO buffers per execution, and stdin is disabled [B3].
- It pre-imports `mathutils` and `math` and provides convenience aliases `C` = `bpy.context` and `D` = `bpy.data` [B1][B3].
- **Autocomplete is live introspection** (Tab in current versions; older versions used Ctrl+Space, and the source docstring still says "Ctrl+Space") [B1][B4]:
  - Name, attribute and index completion run against the console namespace, and there is a separate import-statement completer [B4].
  - **Calltips**: if the line ends with `(`, it shows the signature and docstring [B4].
  - **Safety**: the regex only accepts names, dots and *closed bracket indexing*, so the expression that gets `eval`-ed to find members is an attribute/index chain, never a function call [B4][B5].
- History is Up/Down. Shift+Enter adds a line to history without running it. **Copy as Script (Shift+Ctrl+C)** copies the whole history as a runnable script, prefixed with the standard imports and the `C`/`D` aliases [B1][B3].

### Text Editor
- **Run Script** (Alt+P) runs a text block in a **fresh default namespace** with `__main__` backed up and restored, so it is *not* shared with the console [B6].
- On an exception it **moves the text cursor and selects the error range** [B6]. This has regressed in several versions: 2.93 stopped marking errors [B10], and a 4.1 report says it didn't jump to the line [B9].
- The editor's own message is the infamous "Python script failed, check the message in the system console". The traceback lives in the OS console or the Info editor [B11].
- **Register** (run the text when the .blend loads) and **Live Edit** (re-run on every change) are per-text toggles [B2].
- **Internal vs external text.** Internal text is stored in the .blend. External text is linked to a file on disk, and when that file changes outside Blender, **Resolve Conflict** offers *Reload from Disk / Make Internal / Ignore* [B2].
- **Templates** menu (Python and OSL) [B2].
- Autocomplete (Tab) offers **only words already present in the text** [B2]. That is a big gap compared to the console.
- Find & Replace has case matching, wrap-around, and **search across all text blocks** [B2].
- Other features: duplicate line (Ctrl+D), move lines (Shift+Ctrl+Up/Down), toggle comment (Ctrl+/), jump to line (Ctrl+J), margin column, line highlight, Ctrl+Wheel zoom [B2].

### Discoverability ("what's the Python for this?")
- **Python Tooltips** (`show_tooltips_python`) and **Developer Extras** (`show_developer_ui`) make every button's tooltip show its Python attribute or operator, and add context-menu links to the API docs [B7][B8].

### External workflow
- **Blender Development (Jacques Lucke's `blender_vscode`)**. It is "no longer in active feature development" but still takes bug reports [B12].
  - Features: *Blender: Start* (choose an executable, output goes to VS Code), breakpoint debugging, *Reload Addons* and `reloadOnSave`, *Run Script* into running sessions, script context comments (e.g. `#context.area: VIEW_3D`), and add-on/extension scaffolding via symlinks [B12].
  - Architecture: a **Flask/Werkzeug HTTP server in a daemon thread inside Blender** on a random port (49152–65535) plus a debugpy listener. Blender POSTs its ports to VS Code and blocks in `debugpy.wait_for_client()`, and incoming requests are dispatched to the main thread via `run_in_main_thread` [B13].
  - Known limits: running scripts can occasionally crash Blender; debugging is limited to workspace files unless `justMyCode` is turned off, which can destabilize Blender; reload works only when Blender was started from VS Code [B12][B14].
- **Stubs**: `fake-bpy-module` publishes PyPI packages per Blender version (2.78 → 5.2 plus `latest`), and PyCharm needs its IntelliSense size limit raised [B15]. A newer `blender-python-stubs` project targets mypy, pyright, basedpyright and ty type-checking [B16].

**Execution model:** in-process, main thread. Console namespaces are per region; Run Script uses a fresh namespace each time. **Debugging:** external only (debugpy via VS Code). **Pain points:** errors routed to the system console, flaky error-line jumping, word-only completion in the Text Editor, and an unmaintained bridge.

---

## 4. Godot (deep dive)

Docs are for Godot 4.7 stable [G1]; source is from `master` as of 2026-10.

### Built-in script editor features [G3]
- **Script list** with type icons, a filter box, `*` marking unsaved files, drag reordering, and middle-click close. It also lists open **documentation pages** as tabs.
- **Members outline** (alphabetical or source order, with a filter). For doc pages it shows a table of contents.
- **Code completion** (Enter/Tab; Tab replaces the text to the right of the caret).
- **Syntax checking** for GDScript and JSON, with **error and warning counts in the status bar**. Clicking a count lists the issues and jumps to them, and warnings can be ignored.
- Parsing runs after an idle delay (`text_editor/completion/idle_parse_delay`, in seconds); completion has its own delay setting [G12].
- **Find/Replace in Files** with a Search Results tab (Replace All cannot be undone).
- Bookmarks, code folding including named regions, a clickable minimap, multiple carets (Alt+Click), and Ctrl+D to select the next match.
- All shortcuts can be rebound under Editor Settings → Shortcuts.
- **Offline class reference** inside the editor, with search, history arrows and contextual help. Override icons link to the parent method's docs [G3].
- **User code gets docs too**: `##` doc comments become built-in help pages, with `@tutorial`, `@deprecated`, `@experimental` and BBCode cross-links, and exported variables show their docs as inspector tooltips [G5].

### Debugger [G1][G2]
- Breakpoints are set in the gutter and **persist across editor restarts even if the script wasn't saved**. The `breakpoint` keyword stores a breakpoint *in code*, so it travels through version control [G1].
- Stack Trace panel: frames, a Stack Variables tree (locals/members/globals), Step Into/Over, Break, Continue, and Skip-all-breakpoints. A green arrow in the gutter marks the paused line [G2].
- **Evaluator**: a REPL-ish field that evaluates expressions in the paused frame's scope. It is locked while the game is running [G2].
- Errors tab, Profiler, Visual Profiler, Network Profiler, Monitors (record even while closed), Video RAM, and Misc ("clicked Control") [G2].
- **Remote scene tree**: while the game runs, the Scene dock switches to *Remote* and you can inspect and edit live node properties [G1].
- Run options: sync scene/script changes to the running game, Keep Debug Server Open (for externally started sessions), Customize Run Instances, and remote deploy debugging [G1].
- **Limitation:** "Breakpoints don't work in tool scripts", i.e. `@tool` code executing inside the editor [G2]. The 3.x docs also said breakpoints didn't trigger in threads [G13]. The 4.x source has per-thread break handling (`threads_in_break`) [G8].

### Architecture: why the debugger doesn't freeze the editor
- `EditorRun` builds the command line for a **new OS process**. It passes `--path`, then `--remote-debug <server uri>`, then `--editor-pid`, then `--breakpoints <list>` collected from the editor, plus `--skip-breakpoints`/`--ignore-error-breaks` and debug-draw flags. It then calls `OS::create_instance` [G6].
- The editor side is an `EditorDebuggerServer` that listens on TCP (`network/debug/remote_host`/`remote_port`) and falls back to the next ports if busy [G7].
- In the game process, `RemoteDebugger::debug()` runs on a break. It sends `debug_enter`, then loops while connected: flush output, poll messages, handle `step`/`next`/`continue`/`breakpoint`/`evaluate`, and so on. When idle it sleeps 10 ms and **calls `DisplayServer::force_process_and_drop_events()` on the main thread so the paused game window doesn't go "Not Responding"** [G8].
- `evaluate` parses the expression with Godot's `Expression` class against the selected frame's instance [G8].
- **Script hot reload into the running game only happens during an idle poll**, never mid-frame or mid-break [G8].
- Net effect: the editor (UI, script editor, inspector) never blocks. Only the game process is suspended. The editor's own `@tool` code gets no debugger.

### LSP and DAP (external editor mode) [G4][G9][G10][G14]
- Editor Settings → Text Editor → External: an exec path plus flags with `{project}`, `{file}`, `{line}` and `{col}` placeholders (auto-detected for known editors since 4.5). There is also **Auto Reload Scripts on External Change** [G4].
- **The GDScript language server runs inside the editor process** (default port 6005) and is polled from the editor loop with a time budget [G10]. The text-document handler exposes `completion`, `resolve`, `hover`, `definition`, `declaration`, `signatureHelp`, `references`, `rename`/`prepareRename`, `documentSymbol`, `documentLink`, and a Godot-specific `nativeSymbol` for engine docs [G9].
- The **DAP** debug adapter (default port 6006) lets VS Code drive the same debugger [G4][G11]. Both require a running editor with the project open [G4].

**Lessons for Unreal:** (a) make one analysis engine serve both the built-in editor and an external IDE; (b) persist breakpoints per file outside the file, *and* honour an in-code `breakpoint()`; (c) the only way to get a non-freezing debugger is out-of-process execution. For in-editor code you either accept a frozen editor while paused (as Omniverse and C4D do with debugpy attach [O6][C1]) or pump a minimal event loop like Godot's game process does.

---

## 5. Brief survey

### NVIDIA Omniverse Kit — `omni.kit.window.script_editor` (2.0.17, 2026-08-24)
- **Tabs** ("Python tabs" created from the Tab menu). **All tabs share one namespace**, so imports and variables carry across them [O4][O5]. Run with the Run button or Ctrl+Enter; with a selection, only the selection runs [O4]. Open/Save/Save As exist, but the docs say they are non-operational on Linux [O4].
- **Snippets menu** fed by `snippetFolders` (default `${kit}/snippets` and `${shared_documents}/snippets`), giving studio-shareable snippet folders [O1][O2].
- Settings [O2]:
  - `executeInTempFile` (default true): scripts run from a temporary file. Presumably this is so tracebacks and `inspect` see a real file — UNVERIFIED rationale.
  - `executeOnReload`: re-run when the file reloads from disk.
  - `clearAfterExecute` (Maya-style), plus font, size and palette.
  - **`maxLogLines`** (1000): the output view keeps a bounded number of lines; it was added to fix a freeze when scripts log rapidly [O2][O3].
- Changelog highlights [O3]:
  - Fixed wrong error *line* when running a selection (1.7.7).
  - File watching moved from polling to `omni.client` stat subscriptions, and only for the active tab (2.0.4–2.0.6).
  - **Warns before running a script containing `asyncio.run()`** because it would block Kit's main thread (2.0.9).
  - The Open dialog checks files asynchronously (2.0.16).
- Autocomplete is not documented (UNVERIFIED either way).
- Debugging is external: *VS Code Link* / `omni.kit.debug.vscode` (ptvsd-based, attach from VS Code; "the app pauses while the breakpoint is hit") and the newer debugpy-based `omni.kit.debug.python`, which has wait-for-attach and programmatic breaks [O6][O7][O8].
- **Isaac Sim Python server + "Isaac Sim VS Code Edition"** [O9][O10][O11]:
  - Transport: TCP on 127.0.0.1:8226, carrying raw source *or* a JSON envelope (`code`, `auth_token`, `context`, `args`, `timeout`, `fire_and_forget`, or `introspect`). The client half-closes the socket to signal end of input.
  - Responses carry `status`, `output`, `result`, `traceback`, `ename`, `evalue`, plus `elapsed_seconds` for slow runs.
  - Globals persist across requests; **named contexts** give isolated namespaces; **top-level `await`** is supported.
  - Timeouts: async code is cancelled via `wait_for`, but **sync code cannot be interrupted** (a watchdog only reports the timeout).
  - Auth is optional and off by default.
  - The VS Code side offers Run / Run selection, an output panel, snippet browsing, templates, and attach configs. The docs note LLM agents can use the server too.

### Maxon Cinema 4D — Script Manager, Python console, VS Code connector
- The Script Manager is under Extensions → Script → Script Manager [C2]. I could not extract its feature list from the help page; script list, Execute, and shortcut/icon assignment are well known but UNVERIFIED here. `c4dpy` is a standalone interpreter with autocompletion and debugging for scripts [C3].
- **"Cinema 4D Connector" for VS Code** [C1]:
  - Cinema 4D runs a broadcasting "Code Exchanger" service (started from the Script Manager; built in since 2023.2), and VS Code toggles the connection from a "C4D X" status-bar button.
  - Commands: *Execute in Cinema 4D as a Script*, *Debug in Cinema 4D* (debugpy attach on localhost:5678; threaded plugin methods need `debugpy.trace_this_thread()`), *Load Script in Script Manager*, and *Load Cinema 4D Script Template*.
  - **"Send to IDE"** in the Script Manager pushes the active script to every connected IDE, and edits sync back.
  - **Python console output is mirrored into a "Cinema 4D" VS Code console.**
  - **Autocomplete is switched to the API version of the connected instance**, though not for unsaved files or the `maxon` package.
  - Limits: encrypted plugins and Python embedded in scene elements (Python Tag, Python Generator) can't be debugged.

### Adobe Substance 3D Painter / Designer
- **Painter**:
  - Python plugins live in `Documents/Adobe/Adobe Substance 3D Painter/python/plugins` (the top-level `plugins` folder is for JavaScript plugins) and implement `start_plugin()`/`close_plugin()` [SP2].
  - The Python menu toggles plugins and has "Reload Plugins Folder"; `substance_painter_plugins` manages plugins programmatically, and reloading means close → `importlib.reload` → start [SP1][SP3].
  - Users report path/reload bugs [SP4].
  - The in-app Python console's location and features are UNVERIFIED (search budget exhausted).
- **Designer** has a **Python Editor** window: Run all (F5), **Run Selection (Ctrl+Enter)**, Find & Replace (Ctrl+F), and console output [SD1]. Community threads dispute whether the `sd` API imports correctly inside it in 14.1.2 [SD2]. Version drift is a recurring pain.

### Unity — no built-in code editor
- C# editing is delegated entirely to the IDE chosen under Preferences → External Tools → **External Script Editor**: Visual Studio (Windows default), VS Code (macOS default) or Rider [U1].
- Unity **auto-generates and maintains `.sln`/`.csproj` files**; per-package project generation is optional [U1][U2]. The Rider package also exposes "an end-point for Rider to call different Unity APIs" [U2].
- "Assets → Open C# Project" opens the generated solution (well known; not shown on the fetched page — UNVERIFIED).
- Unity's editor-only **Python Scripting** package shipped a small Python Script Editor window. Support is scheduled to end as of Unity 6.1 [U3].
- Lesson: an engine can succeed with *zero* built-in editor if project generation and IDE integration are flawless. But Unity's audience is programmers, while Unreal Python users are largely TDs and artists who expect a Maya-style in-app editor.

### Gaffer (Image Engine / Cinesite) — PythonEditor
From source on `main` [GF1][GF2]:
- **Layout and namespace.** Output pane above an input `CodeWidget` with line numbers. The namespace preloads `Gaffer`, `GafferUI`, `IECore`, `imath`, and `root` (the script node).
- **Execution.**
  - It `ast.parse`s first so syntax errors render nicely.
  - A **single expression is `eval`-ed and its result printed**; anything else is `exec`-ed.
  - Execution is **wrapped in `Gaffer.UndoScope`**, so a whole run is one undo step, and runs inside the editor's current Gaffer context.
  - If nothing was selected, the input is cleared after success.
- **Output streaming**: stdout/stderr are flushed to the UI per newline, so progress shows while a script runs.
- **Drag and drop**: dropping nodes or plugs inserts `root['Node']['plug']` paths (lists for multi-selection); dropping data inserts its repr.
- **Completion**: `PythonCompleter` evaluates the object path in the live namespace and **skips `property` attributes to avoid side effects**. It also offers graph-child item completions.
- **MCP server toggle**: lets AI assistants execute Python, capture UI images, and capture image-node output. **It shares variables with the PythonEditor so the user and the AI "collaborate"**, and every tool call is echoed into the output pane. The UI carries an explicit caution that this means full machine access. Present in `main` as of Oct 2026; which shipped release includes it is UNVERIFIED.

### Clarisse
- Not researched in depth (search budget exhausted). Clarisse iFX has a Python/script editor, but no notable unique features were confirmed. UNVERIFIED; low priority.

---

## Comparison matrix

Legend: **Y** built-in · **P** partial/limited · **X** only via third-party, add-on or external IDE · **N** no · **?** not verified · n/a not applicable.
Autocomplete codes: **L** live-namespace introspection · **W** words in the buffer · **S** static analysis or stubs · **LSP** language server.

| Feature | Houdini (Shell / Src Ed / parm editors) | Nuke Script Editor | KnobScripter | Katana Python tab | Blender (Console / Text Ed) | Godot script editor | Omniverse Script Editor | Cinema 4D (+connector) | Substance (Designer / Painter) | Unity | Gaffer PythonEditor |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Multiple tabs / buffers | P (multiple shell panes; one `hou.session` buffer) [H2][H1] | N [N1] | P (file dropdowns, multiple panels) [N6] | ? | P (many text blocks; per-region consoles) [B2][B3] | Y (script list + doc tabs) [G3] | Y, shared namespace [O4][O5] | ? | ? | n/a | P (multiple editor panels) [GF1] |
| Syntax highlighting | Y | Y [N1] | Y, 2 themes [N6] | Y [N10] | Y [B2] | Y | Y (palettes) [O2] | ? | ? | n/a (IDE) | Y [GF1] |
| Autocomplete | L in shell; weak elsewhere [H11]; X EVE/Mad Coder [H14][H16] | L, Tab [N1] | L + W, Tab [N6] | L, Shell or IDE popup mode [N10] | L + calltips in console [B4]; W only in Text Ed [B2] | S / LSP [G3][G9] | ? | S in VS Code, matched to app version [C1] | ? | S via IDE [U1] | L, side-effect-safe [GF2] |
| Static analysis / lint as you type | X (Mad Coder) [H16] | N | N | N | N | Y, status-bar counts, jump [G3] | ? | X (VS Code) | ? | X (IDE) | N (syntax check on run) [GF1] |
| Debugger (breakpoints, step) | N built-in (? external) | N | N | N | X (VS Code + debugpy) [B12] | Y (out-of-process game; not for tool scripts) [G2][G8] | X (VS Code Link / debugpy attach) [O6][O8] | X (debugpy attach :5678) [C1] | ? | X (IDE) | N |
| Find / replace | ? | N (implied by KS) | Y [N6] | ? | Y, across all texts [B2] | Y, in files [G3] | ? | ? | Y (Designer) [SD1] | n/a | ? |
| Snippets / templates | ? | N | Y, rich placeholders [N6] | ? | P (Templates menu) [B2] | ? | Y (snippet folders) [O2] | P (templates in VS Code) [C1] | ? | n/a | N |
| Command history | Y (Up/Down) [H2] | Y (prev/next) [N1] | ? | Y (Alt+Up/Down) [N10] | Y + Copy as Script [B1] | N (Evaluator while paused only) [G2] | ? | ? | ? | n/a | ? |
| Run selection | ? | Y [N1] | Y [N6] | Y [N10] | ? | n/a | Y [O4] | ? | Y (Designer Ctrl+Enter) [SD1] | n/a | Y [GF1] |
| Error → source line / clickable traceback | ? | ? | ? | ? | P (cursor jump; regressions) [B6][B9] | Y [G3][G2] | P (error tracking; selection line-offset fix) [O1][O3] | X (VS Code) | ? | X (IDE) | P (line number in error heading) [GF1] |
| External IDE bridge | Y (`$EDITOR` Alt+E; hrpyc) [H12][H9] | X (Nuke Tools) [N9] | P (open in external app) [N6] | ? | Y (linked files + conflict resolve) [B2]; X (VS Code ext) [B12] | Y (exec placeholders, LSP, DAP) [G4] | Y (VS Code Link; Isaac server + VS Code ext) [O6][O10] | Y (Code Exchanger, Send to IDE, console mirror) [C1] | P (plugins folder + reload) [SP1] | Y (the only mode) [U1] | ? (MCP server) [GF1] |
| Persistence / crash recovery | P (code embedded in .hip / HDA) [H1][H6] | ? | Y (.py.autosave; cursor/scroll/selection memory) [N6] | ? | P (internal texts saved in .blend) [B2] | P (breakpoints persist across restarts) [G1] | P (files, watched; persistent settings) [O3] | ? | ? | n/a | ? |
| Interrupt long scripts | P (Ctrl+C KeyboardInterrupt in shell; cooperative Esc) [H2][H8] | N (threads + main-thread helpers) [N2] | N | ? | ? | Y (Break/Stop the game process) [G2] | N (sync not interruptible; `asyncio.run` warning) [O9][O3] | ? | ? | n/a | N |
| Docs lookup in editor | P (`help()`; separate help browser) | N | P (Blink keyword tooltips; wrap-in-`help`) [N6] | N | P (Python tooltips → API doc links) [B7] | Y (offline class ref, contextual help, user doc comments) [G3][G5] | ? | ? | ? | X (IDE) | N |
| Drag scene object → code | Y (node → `hou.node`) [H2] | ? | P (node picker) [N6] | ? | ? | ? (UNVERIFIED) | ? | ? | ? | n/a | Y (nodes/plugs → `root[...]`) [GF1] |
| One undo step per run | ? | ? | ? | ? | ? | n/a | ? | ? | ? | n/a | Y (`UndoScope`) [GF1] |
| Output flood protection | ? | ? | ? | ? | ? | ? | Y (`maxLogLines`) [O2] | ? | ? | n/a | ? |
| Code stored on scene objects / assets | Y (HDA sections, `hou.session`, parm code) [H5] | Y (Python/callback knobs) [N6] | Y, edits knobs with Save/Reload [N6] | ? | Y (internal texts, Register) [B2] | Y (built-in scripts in scenes, UNVERIFIED) | N | P (Python Tag/Generator) [C1] | N | N | N |

---

## Steal this (for the Unreal editor)

1. **One undo transaction per execution** (Gaffer `UndoScope` [GF1]). Wrap every Run in an editor transaction so Ctrl+Z reverts a whole script, not 400 property sets.
2. **Single-expression → print repr** (Gaffer `ast.parse` then eval/exec [GF1]). A buffer behaves like a REPL when you run one expression. It is cheap and delightful.
3. **Parse before running, and offset line numbers for selections** (Gaffer [GF1]; Omniverse 1.7.7 fix [O3]). Syntax errors never reach the interpreter, and tracebacks always point at buffer lines.
4. **Run buffers from a real (temp) file path** (Omniverse `executeInTempFile` [O2]). Tracebacks, `inspect.getsource`, and debugger breakpoint mapping all need a filename.
5. **Clickable tracebacks that jump to *and select* the error range** (Blender `python_script_error_jump_text` [B6]), with regression tests. Blender broke this repeatedly [B9][B10].
6. **Side-effect-safe live completion.** Evaluate only name/attribute/index chains, never calls (Blender [B4][B5]), and skip `property` getters (Gaffer [GF2]). Calling Unreal getters can load assets or tick systems.
7. **Calltips on `(`** with signature plus first docstring lines (Blender [B4]). Unreal's reflected docstrings make this nearly free.
8. **Pre-bound convenience aliases** in the console namespace (Blender `C`/`D` [B1]; Gaffer `root` [GF1]), e.g. `sel` for selected actors, `world`, `subsys`. They lower the barrier for artists.
9. **Copy history as a runnable script** (Blender Shift+Ctrl+C [B1]). It turns exploratory console sessions into tools in one keystroke.
10. **Drag Outliner actors / Content Browser assets / Details properties into the editor to insert code paths** (Houdini [H2], Gaffer [GF1]). This is the fastest bridge from "clicking" to "scripting".
11. **"Show Python" tooltips and "Copy Python path" on Details-panel properties** (Blender Python Tooltips [B7]). It answers "what is this called in Python?" without docs.
12. **KnobScripter snippet placeholders**: `$$` cursor, `$$sel$$` pre-selected text, `$Var$` prompt-once-substitute-everywhere, `$_$` receiver word [N6]. Plus **studio-shared snippet folders** (Omniverse `snippetFolders` [O2]). This is more expressive than VS Code snippets for artist workflows, and the folders deploy studio-wide.
13. **Edit code stored on objects with explicit Save/Reload and a modified marker** (KnobScripter node mode [N6]). For Unreal: Python in Editor Utility assets, Blueprint "Execute Python" nodes, data assets, and startup scripts.
14. **"Run in context"** (KnobScripter [N6]). Run a snippet as if called from its real context, e.g. with a chosen actor or asset bound to the names its callback would receive.
15. **Autosave sidecars plus per-file cursor/scroll/selection memory** (KnobScripter `.py.autosave` and state store [N6]). It gives crash recovery and lets you "jump back to where I was" at near-zero cost.
16. **Persist breakpoints outside the file, and honour in-code `breakpoint()`** (Godot [G1]). Gutter breakpoints survive restarts; code breakpoints survive version control.
17. **Status-bar error/warning counts from as-you-type analysis on an idle timer** (Godot `idle_parse_delay` [G3][G12]). Use a Python analyzer (e.g. a pyright-class checker against generated `unreal` stubs) in a worker process.
18. **One analysis engine shared by the built-in editor and external IDEs** (Godot's in-editor LSP [G9][G10]). Ship an Unreal-hosted LSP so VS Code and PyCharm users get the same live-aware completions, and keep the built-in and external experiences consistent.
19. **External editor exec template with `{file}` `{line}` `{col}` and auto-reload on external change** (Godot [G4]; Blender's Resolve Conflict reload/keep/ignore [B2]). This is the minimum respectful integration with users' own editors.
20. **Bidirectional "Send to IDE" plus console mirroring** (C4D Code Exchanger [C1]). Push the active tab to VS Code, sync edits back, and stream Unreal's Python output into the IDE console.
21. **Structured remote-exec protocol**: JSON envelope with `code` / `context` / `timeout` / `fire_and_forget` / `introspect`, named isolated namespaces, top-level `await`, structured results, and an auth token (Isaac Sim Python server [O9]). This is a proven design for the IDE bridge *and* for AI agents.
22. **Cooperative progress + cancel as a first-class API, surfaced as a Stop button** (Houdini `InterruptableOperation` [H8]). Unreal has slow-task primitives; the editor should auto-wrap runs and expose cancel. Pair it with an async-exception fallback for pure-Python loops.
23. **Bounded output view with batch trimming** (Omniverse `maxLogLines` [O2][O3]). Spammy `print` loops must never freeze the editor.
24. **Lint for editor-freezing patterns before running** (Omniverse warns on `asyncio.run()` [O3]). Warn on `time.sleep` loops, `asyncio.run`, and blocking `input()` on the game thread.
25. **Lifecycle hooks for Python-defined tool panels** (Houdini Python Panel `onCreate/Activate/Deactivate/Destroy`, hip-file events, `onNodePathChanged`; "don't hold the pane object" [H4]). Map these to level load/save and selection change for Python-built Unreal tabs.
26. **A completion-mode preference: Shell-style Tab vs. IDE-style popup** (Katana [N10]). It satisfies Maya-REPL users and PyCharm users with one setting.
27. **Optional MCP/agent bridge sharing the console namespace, with every agent action echoed in the output and an explicit full-access warning** (Gaffer [GF1]). By 2026 this is table stakes for a "modern" editor, and the echoing keeps it auditable.

---

## Avoid this

- **Errors that only say "check the system console"** (Blender [B11]). Every traceback must render in the editor's output with clickable frames.
- **Word-only completion in the main editor while the console has live completion** (Blender Text Editor [B2] vs console [B4]). The same completion engine must back every buffer.
- **Unreliable external-editor round trips**: hotkeys that only work when hovering a label, and saves that silently fail to sync back (Houdini [H12]). Use a file watcher with a visible "synced" indicator.
- **Ctrl+C meaning KeyboardInterrupt** (Houdini shell [H2]). Keep Ctrl+C as copy; use a dedicated Stop button or shortcut (e.g. Ctrl+Break/Esc while running).
- **Interrupts that only work at cooperative checkpoints**, with no indication of that (Houdini [H8]; Isaac "sync code cannot be interrupted" [O9]). Users press Stop and nothing happens. Combine cooperative checks with an injected async exception and say clearly when C++ is blocking.
- **Silent debugger gaps**: Godot's breakpoints simply don't fire in tool scripts [G2]. If some context (e.g. code run during asset import) can't be debugged, show it in the UI.
- **Unauthenticated remote-execution ports** (hrpyc: no auth [H9]; Isaac: auth off by default [O9]). Bind to localhost, require a per-session token, and never let a UI toggle expose 0.0.0.0 without a warning.
- **Unbounded output buffers** (Omniverse needed a 2026 fix for log-flood freezes [O3]).
- **Clearing the input after a successful run *by default* in tabbed buffers** (Nuke default [N1]). It is fine as Maya-compat opt-in for a scratch/REPL tab, but destructive in file-backed tabs.
- **Edits that need a separate "Save all" step to take effect** (KnobScripter's snippet editor [N6]). Apply on change, or at least warn on close.
- **One giant stub file** that exceeds IDE indexing limits (`hou.py`, bpy stubs and PyCharm's IntelliSense size cap [H10][B15]). Generate per-module `.pyi` packages.
- **Depending on an unmaintained community bridge** (blender_vscode is out of active feature development; its reload only works when Blender was started from VS Code [B12][B14]). Own and test the bridge.
- **Fragile code-in-scene editors with false syntax errors on trailing comments or whitespace** (Houdini Source Editor [H17]). Use the real Python parser, not ad-hoc preprocessing.
- **Menu items that exist but don't work on some platforms** (Omniverse Open/Save on Linux [O4]).
- **Completion regressions after Qt/Python upgrades** (Nuke 15.1 PySide2 completion [N3]). Keep a completion regression suite pinned per engine version.
- **HiDPI double-scaling in custom editor widgets** (EVE window oversized at 150% scaling [H14]). Use Slate DPI scaling consistently.

---

## Open questions

1. **Debugger pause semantics for in-editor Python.**
   - (a) Freeze the whole editor while paused: what debugpy attach does in Omniverse and C4D ("the app pauses" [O6][C1]); simplest.
   - (b) Pump a minimal Slate/OS event loop while paused, Godot-style [G8]: responsive but risky re-entrancy on the game thread.
   - (c) Offer an **out-of-process run mode** (headless `UnrealEditor-Cmd` / commandlet) for batch scripts, where a Godot-like non-freezing debugger is possible.
   - Probably all three, with (a) the default. Needs Unreal-internals input.
2. **Namespace policy.** Shared across tabs (Omniverse [O5]), per console (Blender [B3]), fresh per Run (Blender Text Editor [B6]), or named contexts (Isaac [O9])? Proposal: one shared "session" namespace by default, a per-tab "isolated" toggle, and a visible indicator of which is active.
3. **Where does static analysis run?** Bundle a language server (pyright-class or jedi) in a worker process against generated `unreal` stubs. Should Unreal host an LSP endpoint that *also* answers live-introspection queries for external IDEs (Godot model [G10])?
4. **Interrupt mechanism safety.** Is `PyThreadState_SetAsyncExc` (or `PyErr_SetInterrupt`) safe on Unreal's game thread mid-C++-call? Exceptions only surface on return to Python. Should Unreal's slow-task cancel flag be polled from a `sys.settrace`/`sys.monitoring` hook (Python 3.12+), and what does that cost?
5. **Code stored on assets** (KnobScripter knobs, Houdini HDA sections, Blender internal texts). Which Unreal containers should the editor open natively: Editor Utility Blueprints, Python stored in data assets, `init_unreal.py`? How does that interact with source control and cooking?
6. **Snippet format.** Adopt KnobScripter-style placeholders, VS Code snippet JSON (for portability), or both with a converter?
7. **AI/MCP bridge.** Ship it in v1 (Gaffer precedent [GF1]) or later? What permission model applies: per-session opt-in, read-only mode, and transaction-wrapped agent actions?
8. **Unity's lesson.** How much effort goes into the built-in editor vs. a flawless VS Code/PyCharm bridge? Given the TD/artist audience and the explicit Maya/Charcoal brief, the built-in editor should be first-class, but the bridge must be equally good.
9. **Remaining gaps from this pass.** Substance Painter console features, the Cinema 4D Script Manager's in-app feature set, Houdini's Python Shell completion internals, Godot drag-node-to-script behavior, Clarisse. All UNVERIFIED because the shared search budget ran out.

---

## Sources

**SideFX Houdini**
- H1 — hou.session (saved in .hip; Source Editor; `setSessionModuleSource`): https://www.sidefx.com/docs/houdini/hom/hou/session.html
- H2 — HOM introduction (Python Shell usage, Ctrl+C = KeyboardInterrupt, drag node into shell): https://www.sidefx.com/docs/houdini/hom/intro.html
- H3 — Python Panel pane: https://www.sidefx.com/docs/houdini/ref/panes/pythonpanel.html
- H4 — Python Panel Editor (.pypanel, hooks): https://www.sidefx.com/docs/houdini/ref/windows/pythonpaneleditor.html
- H5 — Locations of Python code in Houdini: https://www.sidefx.com/docs/houdini/hom/locations.html
- H6 — hou.HDAModule: https://www.sidefx.com/docs/houdini/hom/hou/HDAModule.html
- H7 — hou.phm: https://www.sidefx.com/ja/docs/houdini/hom/hou/phm.html
- H8 — hou.InterruptableOperation: https://www.sidefx.com/docs/houdini/hom/hou/InterruptableOperation.html
- H9 — Remote access / hrpyc: https://www.sidefx.com/docs/houdini/hom/rpc.html
- H10 — Forum: Autocompletion PyCharm Houdini (hou.py size limit, stubs): https://www.sidefx.com/forum/topic/74269/
- H11 — Forum (2013): Houdini shell completion vs. WingIDE: https://www.sidefx.com/forum/topic/29814/
- H12 — Forum: Artists' Guide for External Editor for Scripting in Houdini: https://www.sidefx.com/forum/topic/78429/
- H13 — MXE: Multiple External Editors for Houdini: https://github.com/peterprickarz/mxe
- H14 — Forum: EVE (Excellent VEX Editor), Dec 2025 / Jun 2026: https://www.sidefx.com/forum/topic/102715/
- H15 — Digital Production on EVE (2025-09-04): https://digitalproduction.com/2025/09/04/eve-turns-houdinis-vex-pane-into-a-rainbow-bracked-autocomplete-powerhouse/
- H16 — Forum: Mad Coder plugin (2026-08-21): https://www.sidefx.com/ja/forum/post/462977/
- H17 — Forum: Python Source Editor problems (H14 era): https://www.sidefx.com/forum/topic/38349/
- H18 — houdini_stubs: https://github.com/vincentullmann/houdini_stubs
- H19 — Forum: staff advice on hou.pwd() in PythonModule: https://www.sidefx.com/forum/post/380115

**Foundry Nuke / Katana**
- N1 — Using the Script Editor (Nuke): https://learn.foundry.com/nuke/content/comp_environment/script_editor/using_script_editor.html
- N2 — Nuke Python dev guide, Threading: https://learn.foundry.com/nuke/developers/latest/pythondevguide/threading.html
- N3 — Foundry bug ID 581730 (15.1 PySide2 completion): https://supportsandbox.foundry.com/hc/en-us/articles/32695793997330-ID-581730-Nuke-s-Script-Editor-no-longer-autofills-PySide2-modules
- N4 — nukescripts.blinkscripteditor source (Nuke 16.0): https://learn.foundry.com/nuke/developers/160/pythondevguide/_modules/nukescripts/blinkscripteditor.html
- N5 — KnobScripter GitHub: https://github.com/adrianpueyo/KnobScripter
- N6 — KnobScripter v3.2 User Guide (PDF, updated 2026-03-23): https://adrianpueyo.com/ks3-docs
- N7 — Nukepedia KnobScripter page: https://www.nukepedia.com/tools/python/ui/knobscripter/
- N8 — KnobScripter v3.1.0 release: https://github.com/adrianpueyo/KnobScripter/releases/tag/v3.1.0
- N9 — Nuke Tools (VS Code): https://github.com/sisoe24/Nuke-Tools/
- N10 — Katana: Using the Python Tab: https://learn.foundry.com/katana/Content/ug/script_editing/using_python_tab.html

**Blender**
- B1 — Manual source: Python Console: https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/editors/python_console.rst
- B2 — Manual source: Text Editor: https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/editors/text_editor.rst
- B3 — `_console_python.py` (InteractiveConsole per region, redirection, copy-as-script): https://github.com/blender/blender/blob/main/scripts/modules/_console_python.py
- B4 — `intellisense.py` (completion, calltips): https://github.com/blender/blender/blob/main/scripts/modules/_bl_console_utils/autocomplete/intellisense.py
- B5 — `complete_namespace.py`: https://github.com/blender/blender/blob/main/scripts/modules/_bl_console_utils/autocomplete/complete_namespace.py
- B6 — `bpy_interface_run.cc` (fresh namespace, error jump): https://github.com/blender/blender/blob/main/source/blender/python/intern/bpy_interface_run.cc
- B7 — Python API quickstart (Developer Extras, Python Tooltips): https://projects.blender.org/blender/blender/raw/branch/main/doc/python_api/rst/info_quickstart.rst
- B8 — Manual: Help System (Python tooltips): https://docs.blender.org/manual/en/4.5/getting_started/help.html
- B9 — Blender Artists: 4.1 Text Editor doesn't jump to error line: https://blenderartists.org/t/blender-4-1-text-editor-vs-jumping-to-line-with-error/1522964
- B10 — T88846: no longer marks error in text: https://developer.blender.org/T88846
- B11 — T84347: "check the message in the system console": https://developer.blender.org/T84347
- B12 — blender_vscode (Blender Development): https://github.com/JacquesLucke/blender_vscode
- B13 — blender_vscode communication.py: https://github.com/JacquesLucke/blender_vscode/blob/master/pythonFiles/include/blender_vscode/communication.py
- B14 — OSArch thread on blender_vscode reload behaviour: https://community.osarch.org/discussion/1650/vscode-and-jacquesluckes-blender-vscode
- B15 — fake-bpy-module: https://github.com/nutti/fake-bpy-module
- B16 — blender-python-stubs: https://git.autourdeminuit.com/autour_de_minuit/blender-python-stubs

**Godot**
- G1 — Overview of debugging tools (4.7 stable): https://docs.godotengine.org/en/stable/tutorials/scripting/debug/overview_of_debugging_tools.html
- G2 — Debugger panel: https://docs.godotengine.org/en/stable/tutorials/scripting/debug/debugger_panel.html
- G3 — Script editor: https://docs.godotengine.org/en/stable/tutorials/editor/script_editor.html
- G4 — Using an external text editor (LSP 6005, DAP 6006): https://docs.godotengine.org/en/stable/tutorials/editor/external_editor.html
- G5 — GDScript documentation comments: https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_documentation_comments.html
- G6 — `editor/run/editor_run.cpp`: https://github.com/godotengine/godot/blob/master/editor/run/editor_run.cpp
- G7 — `editor/debugger/editor_debugger_server.cpp`: https://github.com/godotengine/godot/blob/master/editor/debugger/editor_debugger_server.cpp
- G8 — `core/debugger/remote_debugger.cpp`: https://github.com/godotengine/godot/blob/master/core/debugger/remote_debugger.cpp
- G9 — `modules/gdscript/language_server/gdscript_text_document.cpp`: https://github.com/godotengine/godot/blob/master/modules/gdscript/language_server/gdscript_text_document.cpp
- G10 — `modules/gdscript/language_server/gdscript_language_protocol.cpp`: https://github.com/godotengine/godot/blob/master/modules/gdscript/language_server/gdscript_language_protocol.cpp
- G11 — GDQuest: Getting started with debugging in Godot: https://www.gdquest.com/library/how_to_use_debugger
- G12 — GDQuest editor settings guide (idle parse delay): https://school.gdquest.com/user_guides/useful_godot_editor_settings ; forum: https://forum.godotengine.org/t/can-you-adjust-the-time-before-godot-auto-parses-your-script-while-typing/4081
- G13 — Godot 3.6 debugging overview (threads limitation): https://docs.godotengine.org/en/3.6/tutorials/scripting/debug/overview_of_debugging_tools.html
- G14 — GDScriptLanguageProtocol class: https://docs.godotengine.org/en/stable/classes/class_gdscriptlanguageprotocol.html

**NVIDIA Omniverse / Isaac Sim**
- O1 — omni.kit.window.script_editor 2.0.17 Overview: https://docs.omniverse.nvidia.com/kit/docs/omni.kit.window.script_editor/2.0.17/Overview.html
- O2 — Settings: https://docs.omniverse.nvidia.com/kit/docs/omni.kit.window.script_editor/2.0.17/SETTINGS.html
- O3 — Changelog: https://docs.omniverse.nvidia.com/kit/docs/omni.kit.window.script_editor/2.0.17/CHANGELOG.html
- O4 — Script Editor user guide: https://docs.omniverse.nvidia.com/extensions/latest/ext_script-editor.html
- O5 — Isaac Sim: Omniverse Script Editor (shared namespace across tabs): https://docs.isaacsim.omniverse.nvidia.com/5.1.0/development_tools/omniverse_script_editor.html
- O6 — VS Code Link: https://docs.omniverse.nvidia.com/extensions/latest/ext_vs-code-link.html
- O7 — omni.kit.debug.vscode 0.1.6: https://docs.omniverse.nvidia.com/kit/docs/omni.kit.debug.vscode/0.1.6/Overview.html
- O8 — omni.kit.debug.python 0.2.3: https://docs.omniverse.nvidia.com/kit/docs/omni.kit.debug.python/0.2.3/index.html
- O9 — isaacsim.code_editor.python_server (6.2.0): https://docs.isaacsim.omniverse.nvidia.com/6.2.0/py/source/extensions/isaacsim.code_editor.python_server/docs/index.html
- O10 — Isaac Sim VS Code integration (6.2.0): https://docs.isaacsim.omniverse.nvidia.com/6.2.0/development_tools/vscode.html
- O11 — Isaac Sim Python Server page (6.0.1): https://docs.isaacsim.omniverse.nvidia.com/6.0.1/development_tools/python_server.html

**Maxon Cinema 4D**
- C1 — Visual Studio Code Integration Manual: https://developers.maxon.net/docs/py/2024_2_0/manuals/manual_py_vscode_integration.html
- C2 — Script Manager help page: https://help.maxon.net/c4d/2026/en-us/Content/html/5896.html
- C3 — Python in Cinema 4D (c4dpy): https://developers.maxon.net/docs/py/2023_1/manuals/introduction/python_in_c4d.html

**Adobe Substance 3D**
- SP1 — Painter: substance_painter_plugins module: https://helpx.adobe.com/substance-3d-painter-python/api/plugins/substance-painter-plugins.html
- SP2 — Painter: Create a Python plugin: https://experienceleague.adobe.com/en/docs/substance-3d-dev/painter-python/tutorials/create-a-python-plugin
- SP3 — Painter: Load external Python plugins: https://experienceleague.adobe.com/en/docs/substance-3d-dev/painter-python/tutorials/load-external-python-plugins
- SP4 — Painter community bug: reload plugins folder after path change: https://community.adobe.com/t5/substance-3d-painter-bugs/scripting-unable-to-reload-plugins-folder-via-python-after-updating-substance-painter-plugins-path/idi-p/13157202
- SD1 — Designer: Python Editor: https://experienceleague.adobe.com/en/docs/substance-3d-designer/using/scripting/python-editor
- SD2 — Designer community: 14.1.2 Python scripting issue: https://community.adobe.com/bug-reports-49/urgent-python-scripting-issue-in-substance-3d-designer-node-creation-connections-version-14-1-2-1187045

**Unity**
- U1 — Integrated development environment (IDE) support (Unity 6): https://docs.unity.com/en-us/engine/6000.0/manual/programming-environment/environment-and-tools/scripting-ide-support
- U2 — JetBrains Rider Editor package 3.0: https://docs.unity3d.com/Packages/com.unity.ide.rider@3.0/manual/index.html
- U3 — Python Scripting package 7.0: https://docs.unity3d.com/Packages/com.unity.scripting.python@7.0/manual/index.html

**Gaffer**
- GF1 — `python/GafferUI/PythonEditor.py` (main): https://github.com/GafferHQ/gaffer/blob/main/python/GafferUI/PythonEditor.py
- GF2 — `python/GafferUI/CodeWidget.py` (PythonCompleter): https://github.com/GafferHQ/gaffer/blob/main/python/GafferUI/CodeWidget.py
