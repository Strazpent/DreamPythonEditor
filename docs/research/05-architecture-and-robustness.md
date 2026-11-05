> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 05 — Implementation Architecture Options and Production-Robustness Engineering

- **Project:** Production-grade Python script editor inside the Unreal Engine 5.8 editor (Charcoal/Maya Script Editor + PyCharm feel)
- **Lane:** Architecture trade-offs and failure-mode engineering (feature inventories live in sibling docs 01–04)
- **Date:** 2026-10-28
- **Status:** Research input for the product spec / technical design
- **Conventions:** Bracketed tags like [S12] point to the numbered Sources list at the end. **UNVERIFIED** marks a claim I could not confirm against a primary source during this research pass (usually reasoning from engine/CPython design, or a single secondary report). Every UNVERIFIED item that drives a design decision is also listed under Open questions as a spike to run.

---

## Summary

**Recommendation: Option E1. Build it in native Slate throughout, with a custom-painted, line-virtualized code view on top of an engine-independent editor core. Run language services out of process. Treat the external IDE bridge and debugpy as complements, not substitutes.**

- **UI surface.** All of it is Slate: nomad tabs, docking, toolbar, REPL, output view, find/replace and dialogs. Code buffers use our own `SCodeView` leaf widget. It paints only the visible lines, reuses Slate font shaping per line, and implements IME through Slate's `ITextInputMethodContext`. The stock `SMultiLineEditableText` plus syntax marshaller is acceptable only for small inputs, such as the one-line REPL prompt. Epic's own API docs say a syntax-highlighting marshaller forces a full `SetText` on every source change, "which is costly" [S4]. That is O(document) per keystroke.
- **Why not Monaco in CEF.** It gives the fastest route to editing features, but it brings documented focus and IME problems in UE's browser widget [S16][S17][S18], async key routing, and offscreen-rendering overhead [S19]. It also means a second tech stack whose Chromium version Epic controls. We rejected it for v1, but the core/renderer split keeps it possible later.
- **Why not PySide/Qt.** The window cannot dock into UE tabs, which is a hard miss against Maya parity. It needs a manual event pump outside Windows [S25] and has engine-version regressions such as UE-364137 [S29]. LGPL redistribution duties apply to PySide6, and QScintilla is GPL or commercial [S30][S31]. PySide versions also clash with other studio tools sharing the single interpreter.
- **Why external IDE alone is not enough.** It offers no live namespace and no in-editor output stream: Epic's remote-execution protocol returns output only when a command completes [S35]. It also gives no Maya-style select-and-run inside the editor. It is still a valuable complement for heavy refactoring and attach-debugging.

**Top robustness decisions**

1. **Write-ahead journal of every buffer, untitled ones included.** Writes are atomic, happen on a background thread, and run 1–2 s after the last edit. A forced snapshot is taken immediately before every Run, because scripts are the main cause of editor crashes. Crash recovery follows the VS Code hot-exit model [S62], and local history follows the IntelliJ model [S63]. Maya saves tabs on quit and can lose them in a crash [S64].
2. **Interrupting a runaway script without a trace function.** A watchdog thread uses `Py_AddPendingCall` [S39] to schedule a game-thread "pump" at bytecode boundaries. The pump repaints Slate under an input pre-processor that only lets Stop and scroll through. On Stop it raises a `BaseException` subclass in the script. `PyThreadState_SetAsyncExc` [S39] is the fallback. The cost is zero when the user does not cancel, and it does not conflict with debugpy, coverage or profilers. Epic documents that the editor UI blocks and does not tick while Python runs [S2][S3], so this is the core problem to solve.
3. **Output as a ring-buffered, virtualized store.** Lines go through a lock-free ingest queue, drained under a per-frame budget. The view never does an O(N) refresh per append. This is the same bug class that made UE's Message Log slow [S10] and the Output Log degrade with size [S9].
4. **Explicit execution semantics.** The default namespace is shared, like Maya's and UE's console (`PUBLIC` scope [S43]); per-tab and isolated runs are opt-in. Tracebacks keep correct line numbers through `linecache` registration, even when running a selection. Package reload purges and re-imports in dependency order, with unload hooks, rather than calling naive `importlib.reload` [S45]. Runs are wrapped in undo transactions [S47].
5. **Language server out of process and supervised.** Restarts use backoff, crashes are contained with Job Objects or a parent-death signal, and stale responses are dropped by document version. The editor generates its own split `.pyi` stub package, because the monolithic `unreal.py` (21–50+ MB) breaks Pylance/Pyright [S48][S49][S50]. Pyrefly (MIT, pip wheel) [S54] or basedpyright [S56] are the candidate servers.
6. **Two debuggers.** The in-editor debugger is bdb-based and runs a restricted nested Slate pump while paused. `FSlateApplication::EnterDebuggingMode` exists to "tick in place" [S42]. The second option is debugpy attach for external IDEs, where the editor freezes while paused, as in Maya [S58].
7. **A trust model like VS Code Workspace Trust** [S65], driven by studio policy files. The IDE bridge is loopback-only with a session token and HMAC; never extend Epic's unauthenticated multicast protocol [S35].

**Biggest risks**
- Building a code view of PyCharm quality in Slate takes a lot of work: text model, IME, accessibility, and many editing features.
- Typing latency in any in-process editor is bounded by the editor frame time. A heavy viewport means slow echo.
- Nested Slate pumping creates re-entrancy hazards.
- Native calls cannot be interrupted.
- Stub size defeats the language server.
- The coming Python 3.11 → 3.13 upgrade. Epic missed it for 5.8 and said it will land in main later [S1].

---

## Context and hard constraints (from evidence)

| Fact | Consequence | Source |
|---|---|---|
| UE 5.8 embeds Python 3.11 (3.11.8 per docs). Epic could not ship 3.13 for 5.8, and the next upgrade "should land in main" | Design for 3.11 now: no `sys.monitoring` (3.12+ [S40]), no PEP 701 f-strings. Abstract tracing and tokenization so a version flip is cheap | [S1] |
| "While the Unreal Editor is running a Python script, its UI becomes blocked" | Any feedback during a run (output, progress, Stop) must come from cooperative pumping inside the run | [S2] |
| "The editor doesn't tick during Python execution" (Python test docs; latent commands exist for this reason) | Async work must be latent or ticked. There is no free UI refresh | [S3] |
| One embedded interpreter shared by all studio tools (ShotGrid/AYON, Qt tools, init_unreal.py, etc.) | Never pollute or pin global packages. Namespace our helper package. Assume other tools register Slate tick callbacks | [S2][S33] |
| `ExecutePythonCommandEx` supports `PUBLIC`/`PRIVATE` file scopes and returns captured log output after execution | Fine for simple runs, but we need streaming output, line-offset compilation and interruption. Own the exec path | [S43][S44] |
| Plugin Python deps can be declared as `PythonRequirements` in `.uplugin` and are pip-installed (binary wheels only) to `Intermediate/PipInstall` with strict hashing | A supported shipping path for Python-side deps and for pip-distributed language servers (e.g., pyrefly) | [S33] |

---

## Part 1 — UI architecture options

### Evaluation criteria and weights

| # | Criterion | Weight | What "5" means |
|---|---|---|---|
| 1 | Native look and docking in UE tabs | 12 | Indistinguishable from editor panels. Docks, splits and saves layout with `FGlobalTabmanager` |
| 2 | Keyboard shortcut and focus integration | 14 | Synchronous key routing. We decide what bubbles to editor commands. No lost focus |
| 3 | IME and accessibility | 8 | CJK IME composition inline. Screen-reader exposure on par with the rest of the editor |
| 4 | High-DPI | 5 | Crisp at any Slate DPI scale, including per-monitor changes |
| 5 | Performance (large files, huge output) | 14 | 100k-line buffer and 1M-line output with no frame hitch |
| 6 | Cross-platform (Win/Mac/Linux editor) | 8 | Same code path on all three |
| 7 | Effort to reach PyCharm-grade features (higher = less effort) | 14 | Most editing/IDE features available or cheap |
| 8 | Shipping and licensing | 10 | No copyleft obligations, no large runtime downloads |
| 9 | Maintenance across engine upgrades | 15 | Depends only on stable public APIs. One tech stack |

**Gate (not weighted):** Does it deliver the in-editor, Maya-style workflow? That means a docked tab, live session namespace, select-and-run, and inline output.

---

### Option A — Native Slate

#### A1. Stock `SMultiLineEditableText` + syntax-highlighting marshaller

**How it works.** `SMultiLineEditableText` uses an `ITextLayoutMarshaller` to convert source text into styled runs. `FSyntaxHighlighterTextLayoutMarshaller` uses an `FSyntaxTokenizer` with ordered rules [S5][S8]. Epic uses this pattern for HLSL editing in `UnrealEd` and Niagara (`FHLSLSyntaxHighlighterMarshaller`, `FNiagaraHLSLSyntaxHighlighter`) [S6][S7].

**Evidence about scaling**
- The API docs for `ITextLayoutMarshaller::RequiresLiveUpdate` say that returning true "will cause SetText to be called every time the source text is changed, which is costly, but required for things like syntax highlighting" [S4]. So the stock highlighter retokenizes and rebuilds the whole layout on every keystroke.
- The Output Log, which Epic built on the same multi-line text stack (UNVERIFIED for 5.8 internals), has long-standing reports of editor slowdowns that grow with log size, and of freezes when viewing a large log [S9].
- Epic's own Slate log views have had O(N)-per-append refresh bugs. In 2026 the Message Log was made dramatically faster by deferring the full re-filter to draw time; the author measured roughly 2 million re-added messages per refresh by frame 200 [S10]. This is a warning about any view that rebuilds on append.
- The Epic HLSL/Niagara editors handle small snippets, so they are not evidence that the approach scales to multi-thousand-line files. I found no public Slate-native Python editor plugin that handles large files. The public in-editor Python editors I found are Qt-based [S27][S75].

**IME and accessibility.** IME is already implemented via `FSlateEditableTextLayout::FTextInputMethodContext` [S11], though there are still engine bugs, such as Japanese input in a MultiLineEditBox (UE-234820) [S13]. Screen-reader support exists in Slate (`Accessibility.Enable=1` plus the Slate Screen Reader plugin) [S14].

**Verdict.** Fastest to an MVP. But it caps out: there is no virtualization, the per-keystroke work is O(N), and gutters, folding, inline diagnostics and multi-cursor all have to be fought into a widget not designed for them. Use it only for the REPL input line and small dialogs.

#### A2. Custom-painted, line-virtualized Slate code view (`SCodeView`)

**How it works.** An `SLeafWidget` paints only visible lines plus overscan using Slate draw elements and the engine font cache and shaping. Shaping is cached per line, keyed by content and style hash. The data side is an engine-independent C++ core:
- a piece-table or rope text model with an undo tree;
- an incremental line-state tokenizer, plus optional tree-sitter-python for structure (folding, outline, smart selection; general knowledge);
- search, multi-cursor and an LSP client.

IME is implemented by registering our own `ITextInputMethodContext` with `ITextInputMethodSystem::RegisterContext` [S12]; Epic's implementation [S11] is the reference. Accessibility means exposing an accessible text widget.

**Pros**
- Total control of key routing. We are the focused leaf: we handle `OnKeyDown` and return `Handled` to stop bubbling into editor command lists, or let a chord through on purpose.
- Native docking and DPI.
- O(visible) paint and O(edited lines) tokenization.
- A single C++ stack and no runtime downloads.

**Cons**
- Largest build effort.
- IME is a known hard area in UE. A forum developer reported struggling to make a custom `ITextInputMethodContext` work (single report, UNVERIFIED), and Linux CJK IME has open problems up to 5.8 [S74].
- Accessibility is our job.
- Fallback: degrade to an A1 "plain mode" per buffer if IME regresses on a platform.

**Engine-upgrade exposure.** Low to moderate. We depend on public Slate paint, font and IME interfaces rather than private text-layout internals.

---

### Option B — Monaco or CodeMirror 6 inside UE's embedded browser (CEF)

**How it works.** A nomad tab hosts `SWebBrowser`, loading a bundled local page with Monaco or CM6.
- **C++→JS:** `ExecuteJavascript`.
- **JS→C++:** `BindUObject` exposes a `UObject` to JS [S15].
- **Language server:** Monaco would use `monaco-languageclient` over a WebSocket to an out-of-process Pyright (TypeFox ships a Python/Pyright example) [S23], or over the JS bridge to a C++-side LSP pump.

**Strengths**
- Monaco brings VS Code-class editing: multi-cursor, minimap, folding, find widget, diff editor and the LSP plumbing [S24].
- CM6 virtualizes rendering and is designed to avoid "performance cliffs for huge files or long lines" [S21][S22]. Its DOM-based design is screen-reader friendly in a normal browser [S21].

**Failure modes found (UE-specific)**
- **Focus.** Users report that keyboard focus lands on the browser's container widget rather than the browser, which needs a C++ fix [S17]. Epic added `SWebBrowserView::SetBrowserKeyboardFocus` to work around "a CEF bug that loses focus state on navigations" [S16].
- **IME.** UE-75056: "When using the Web Browser Widget, IME is disabled and you can not input non roman characters" [S18]. Current status UNVERIFIED (tracker returned 403).
- **Key routing.** In offscreen rendering the page decides asynchronously, in another process, whether it handled Ctrl+S, Ctrl+Z or F5. Slate must answer `Handled` or `Unhandled` synchronously. You end up keeping duplicate key maps in JS and C++. UNVERIFIED for UE's implementation, but inherent to OSR embedding.
- **Input lag and cost.** Coherent Labs notes that CEF's renderer-to-host IPC "can add several milliseconds of lag", and that DirectX integration needs hardware acceleration to perform well [S19]. Every repaint of the editor surface becomes a texture upload into Slate. Multiple browser instances multiply process and memory cost [S19]. Startup adds CEF subprocess spin-up on first open (UNVERIFIED magnitude, likely hundreds of ms to seconds).
- **Accessibility.** In windowless mode the page's accessibility tree is not the OS's view of the Slate window, so screen readers likely see nothing. UNVERIFIED.
- **High-DPI.** The browser's device scale factor must track Slate DPI. Blurry-CEF reports exist in other OSR hosts. UNVERIFIED for UE 5.8.
- **Maintenance.** Epic controls the CEF and Chromium version. Third-party CEF plugins warn they cannot co-exist with the built-in WebBrowser plugin [S19]. You maintain TypeScript and C++ plus a bridge protocol, and re-test on every engine bump.
- **No real-world precedent found.** Searches for Monaco or CodeMirror embedded in an Unreal editor plugin found none. Absence of evidence is weak, but it means we would be pioneering.

**Verdict.** Highest feature velocity and lowest feature effort. Highest integration risk: keys, IME, focus and lifecycle. Keep it as a possible future "advanced buffer" renderer behind the core/renderer split. Do not make it v1.

---

### Option C — PySide6/Qt window parented into UE

**How it works.** Create a `QApplication`, then call `unreal.parent_external_window_to_slate(int(widget.winId()))` and drive Qt from a Slate tick. The public Maya-style editor for UE is built this way: leixingyu/unrealScriptEditor, packaged as a plugin by Hannes Delbeke [S27][S75][S77]. It runs code through `execute_python_command_ex(..., EXECUTE_FILE, PUBLIC)` and captures output from the returned log entries, so output appears only after completion [S77].

**Failure modes found**
- **Event loop.** On Windows, Qt mostly works because UE's Win32 message pump dispatches Qt's messages. ShotGrid's tk-unreal says: "On other platforms than Windows, we need to process the Qt events otherwise UIs are 'frozen'. We use a slate tick callback" [S25]. The UnrealEnginePython docs warn never to call `app.exec_()` because it hijacks the engine loop [S78]. Typing in a Qt window is therefore paced by the editor frame anyway.
- **Docking and stacking.** The window floats and cannot dock in UE tab wells. "Won't stay on top of the Unreal main window" [S28]. The Windows title bar stays white, and frameless variants lose resize [S26]. `unreal_qt` needs a periodic re-parent of orphan widgets every 0.3 s [S76].
- **Crashes and regressions.**
  - Creating a widget before your own `QApplication` crashes the editor [S28].
  - Connecting to the editor tick "has a habit of crashing too" [S28].
  - UE 5.7 broke drag-and-drop from PySide windows (UE-364137); the workaround needs an engine source change [S29].
- **Licensing and shipping.** PySide6 is LGPLv3. Redistributing it carries LGPL obligations [S31], and Qt's docs say commercial-license holders should not use the pip community wheels [S32]. QScintilla, the best Qt code widget, is GPLv3 or commercial and binds to PyQt, not PySide [S30]. The pure-PySide path is `QPlainTextEdit` plus a Python `QSyntaxHighlighter`, which runs on the game thread. Wheels must match UE's Python ABI and must be re-validated at the 3.13 upgrade [S1].
- **Studio conflicts.** One interpreter, one PySide. Pipeline tools (ShotGrid via tk-unreal [S25]; AYON ships a PySide pre-install hook, UNVERIFIED detail) bring their own PySide, so version clashes are likely. UNVERIFIED frequency.

**Strengths.** Mature IME and accessibility in Qt, decent high-DPI, and Python-level hackability for TDs.

**Verdict.** Proven for a small editor. Wrong foundation for a product that must dock, ship cleanly and survive engine upgrades.

---

### Option D — External IDE only (VS Code / PyCharm + UE remote execution)

**How it works.** VS Code's "Unreal Engine Python" extension (Nils Söderman; the repo moved to Codeberg, GitHub archived 2026-05-10) provides:
- execute selection or file (Ctrl+Enter);
- code-completion setup;
- docs generated from the running editor;
- debugger attach.

It requires UE Remote Execution [S34].

**Why it isn't enough for the Maya-style workflow**
- **No live session.** Completions come from static stubs, not from the objects you created three commands ago.
- **No streaming output.** Epic's protocol (`ping`/`pong`/`open_connection` over UDP multicast `239.0.0.1:6766`, then `command`/`command_result` over TCP, command endpoint `127.0.0.1:6776`) returns one result per command [S35]. Long runs show nothing until they finish.
- **Context switching.** You alt-tab away from the editor and its selection context.
- **Security.** The protocol has no authentication, only a magic string and random UUIDs [S35].
- **Debugging freezes the editor.** When paused, the host freezes "until you resume", as with Maya [S58][S59].

**Why it's a valuable complement.** It gives users world-class editing, refactoring, VCS and test runners for package development, plus attach debugging. Our plugin should make external IDEs first-class through a secure bridge (see 2.7) and shared stubs (2.5).

---

### Option E — Hybrids

- **E1 (recommended): Native Slate everywhere.** That means A2 for buffers and A1 for the REPL line, plus:
  - an out-of-process LSP;
  - in-process live-namespace completion, time-boxed;
  - a native output store;
  - an in-editor bdb debugger with a restricted nested pump;
  - a debugpy attach mode;
  - a secure external-IDE bridge, which reuses D.

  The editor core (text model, tokenizer, LSP client, search) is a UE-independent C++ library. That allows fast unit tests and fuzzing, and leaves room for another renderer.
- **E2: Slate shell plus Monaco buffer.** Native tabs, output and REPL, but each buffer is a CEF view. Faster to rich editing, but it inherits B's key, IME and focus risks exactly where users type most.
- **E3: Qt editor plus Slate output.** Combines C's docking and licensing problems with a split UI. Not recommended.

---

### Scoring table (1–5 per criterion; weighted score out of 100)

| Criterion (weight) | A1 Stock Slate | A2 Custom Slate view | B Monaco/CM6 in CEF | C PySide6/Qt | D External IDE only | **E1 Slate + LSP + bridge** | E2 Slate shell + Monaco |
|---|---|---|---|---|---|---|---|
| Native look and docking (12) | 5 | 5 | 3 | 2 | 1 | **5** | 4 |
| Keys and focus integration (14) | 4 | 5 | 2 | 3 | 5 | **5** | 3 |
| IME and accessibility (8) | 4 | 3 | 2 | 4 | 5 | **3** | 2 |
| High-DPI (5) | 5 | 5 | 3 | 3 | 5 | **5** | 3 |
| Performance, big files/logs (14) | 2 | 4 | 4 | 3 | 4 | **4** | 4 |
| Cross-platform (8) | 5 | 5 | 3 | 2 | 5 | **5** | 3 |
| Effort to PyCharm-grade, higher = easier (14) | 2 | 2 | 5 | 3 | 4 | **3** | 4 |
| Shipping and licensing (10) | 5 | 5 | 4 | 2 | 4 | **5** | 4 |
| Maintenance across upgrades (15) | 4 | 4 | 2 | 2 | 4 | **4** | 2 |
| **Weighted score / 100** | **75.8** | **82.6** | **63.0** | **52.6** | **79.8** | **85.4** | **65.4** |
| **Gate: in-editor Maya-style workflow** | Yes | Yes | Yes | Partial (floats) | **No (fails)** | **Yes** | Yes |

Notes on scoring:
- D scores well on raw editor quality because it *is* VS Code or PyCharm, but it fails the gate, so its score is not comparable.
- E1 beats A2 on effort because the language server and external IDE carry the deepest intelligence and refactoring features. That way we do not re-implement PyCharm inside Slate.
- IME and accessibility (3) is E1's weakest cell and the reason for the Phase 0 IME spike.

### Recommendation and rationale

**Adopt E1.** The rationale, in order of weight:

1. **Maintenance.** At 15% it is the most heavily weighted criterion: a studio tool must survive every engine upgrade. Slate paint, font, IME and tab APIs are stable public surfaces. CEF and Qt are third-party runtimes whose versions are not ours to pick (Epic for CEF, interpreter ABI for PySide) [S1][S29].
2. **Keyboard ownership.** The Maya and PyCharm feel is mostly keyboard: Ctrl+Enter, Ctrl+/, multi-cursor, completion popups. Only native Slate gives synchronous, per-chord control over what reaches editor commands. B is asynchronous and C lives in a separate window.
3. **Performance is achievable natively,** if we avoid the stock marshaller path [S4] and design views with virtualization and budgets (see 2.3 and 2.9).
4. **Feature depth comes from outside the widget.** The LSP gives diagnostics, hover, go-to-definition, rename and formatting. Runtime introspection gives live completion. The external IDE handles heavy refactoring. The Slate view must be an excellent editor, not an IDE.

**Layered architecture (E1)**

```
+-------------------------- UnrealEditor process ---------------------------------+
|  ScriptEditor (Editor module, C++)                  game thread unless noted     |
|   Slate shell: nomad tabs, toolbar, REPL line (A1), output view, find, trust UI  |
|   SCodeView (A2) <---- EditorCore (UE-independent C++ lib: piece table, undo,    |
|                         incremental tokenizer, folding, search, LSP client)      |
|   ExecutionService: compile/exec (C API + helper pkg), namespaces, linecache,    |
|                     transactions, stdout/stderr capture -> OutputStore (ring)    |
|   RunSupervisor: watchdog thread + Py_AddPendingCall pump + cancel injection     |
|   Journal: snapshot on GT (O(1) COW) -> background writer thread                 |
|   DebugService: bdb in-editor (restricted nested pump) | debugpy attach mode     |
|   RuntimeIntrospection: live-namespace completion (<=5 ms time box, no getattr   |
|                         on properties)                                           |
+-----------------------------------+-------------------------+------------------+
          stdio JSON-RPC (I/O thread) |                         | loopback + token + HMAC
              +-----------------------v--------+      +---------v------------------+
              | LSP subprocess (pyrefly or     |      | External IDE (VS Code /    |
              | basedpyright) + split .pyi     |      | PyCharm) bridge, debugpy   |
              | stub package for `unreal`      |      +----------------------------+
              +--------------------------------+
```

**Phase 0 spikes (exit criteria)**

1. **`SCodeView` prototype.**
   - Typing into a 100k-line file: model+tokenize+invalidate ≤ 2 ms p95, paint ≤ 2 ms p95 at 4K.
   - Inline IME composition works for MS-IME Japanese, Microsoft Pinyin and Korean on Windows, plus macOS Kotoeri.
   - Linux IBus is best-effort [S74].
   - Fallback if any of this fails: A1 plain mode per buffer.
2. **Run supervisor prototype on 5.8.** `while True: pass` and a tight list-comprehension loop can be stopped within 250 ms. Output repaints at ≥ 10 Hz during a 60 s run. No crash in 1,000 cancel cycles.
3. **Stub plus LSP.** Split `.pyi` generated for a large project. Server RSS ≤ 1.5 GB and completion p95 ≤ 400 ms. If not met, ship in-process completion only for `unreal` and use the LSP for user code.
4. **Paused-debugger nested pump.** Stepping through 100 breakpoints with the debugger panel interactive and the rest of the editor input-blocked. No re-entrancy asserts.

---

## Part 2 — Production robustness engineering

### 2.1 Never losing work

**Failure modes**
- An editor crash loses unsaved and untitled tabs. Maya saves Script Editor tabs on quit. Its "Save All Script Tabs" command exists precisely so tabs survive an unexpected exit, and users still report tabs coming back empty after crashes because the process dies before writing [S64].
- A torn write corrupts the session file.
- Two editor instances fight over one session.
- A file changes on disk because of a P4 sync, git checkout or another tool.
- A user regrets an edit that was saved hours ago.

**Prior art**
- **VS Code hot exit** periodically backs up dirty files, including untitled ones, and restores them on the next launch regardless of the `files.hotExit` setting [S62].
- **IntelliJ Local History** keeps per-file revisions for 5 working days by default, with size limits, stored locally. It is not a VCS replacement [S63].

**Design**
- **Journal everything.** Every buffer gets a stable UUID. Session state goes in `session.json`: tab order, path, encoding, EOL, cursor, scroll, dirty flag, and the on-disk hash and mtime at load. Content goes in `buffers/<uuid>.txt`. Store both under a per-user, per-project directory *outside the project tree*, such as `%LOCALAPPDATA%/UnrealEngine/ScriptEditor/<projectHash>/`, so Perforce never sees them.
- **When to write.** Debounce 1–2 s after the last edit. Force a write every 30 s while dirty, on focus loss, on tab close, on editor shutdown and **synchronously before every Run.** A pre-run snapshot exists because user scripts are the most common way to crash the editor.
- **How to write.**
  - Take the snapshot on the game thread; it is O(1) with an immutable piece table.
  - Serialize and write on a background thread: temp file, flush, then atomic rename (`ReplaceFileW` or POSIX `rename`).
  - Keep the previous generation and a checksum header, to recover from torn writes.
  - No work happens at crash time. Crash handlers are not a safe place for I/O.
- **Recovery.** A lock file holds PID and start time. An absent clean-shutdown marker means a crash. On the next open, show a non-modal banner: "Recovered 7 buffers from the session that ended unexpectedly", with Restore all, Review or Discard. Never overwrite disk files during recovery; restored buffers come back dirty. Orphaned sessions from other instances appear in the review list.
- **Local history.** Snapshot on every save, every Run ("Before run 14:03 — Tab 'rig_fix'") and every external reload.
  - Store content-addressed and deduped by hash, compressed with an engine-bundled codec.
  - Retention: 7 working days or 200 MB per project, configurable.
  - The UI is a timeline with diffs and "Restore as new tab".
- **File changed on disk.** Use the engine's directory watcher, plus a hash/mtime re-check on focus regain.
  - **Clean buffer:** reload in place as an undoable edit, so Ctrl+Z restores what you had.
  - **Dirty buffer:** show a conflict bar with Compare, Keep mine and Reload.
  - Also handle delete, rename and read-only flips (P4).
  - Preserve BOM, encoding and line endings per file. Defaults: UTF-8 without BOM and LF, configurable per studio.
- **Never use pickle** for session or history metadata (JSON only), so a tampered session file cannot execute code.

### 2.2 Long-running and runaway scripts

**Failure modes**
- The game thread blocks, so the UI freezes and no output appears until the run ends [S2][S3].
- `while True` cannot be stopped, so the user kills the editor and loses work.
- A Stop that works only between UE calls feels broken.
- User code's bare `except:` swallows `KeyboardInterrupt`.

**Mechanisms evaluated (CPython 3.11)**

| Mechanism | Works from other thread? | Reaches pure-Python loops? | Cost when idle | Notes |
|---|---|---|---|---|
| `PyErr_SetInterruptEx(SIGINT)` | Yes (async-signal-safe, no GIL) | Only if a Python SIGINT handler is installed; "if the given signal isn't handled by Python... it will be ignored" | 0 | Embedders often skip signal handlers (`Py_InitializeEx(0)`). Whether UE installs one is UNVERIFIED. Installing `default_int_handler` ourselves may clash with console Ctrl+C handling in `-Cmd` runs [S38][S39] |
| `PyThreadState_SetAsyncExc(tid, exc)` | Yes, but needs the GIL | Yes, at the next eval-breaker check | 0 | Needs a C extension ("to prevent naive misuse"). Its thread can get the GIL during pure-Python loops (switch interval), but not while the game thread holds the GIL in C++ [S39] |
| `Py_AddPendingCall(fn)` | Yes (no GIL, no thread state) | Yes, runs "on a bytecode boundary" in the main interpreter thread with the GIL; `fn` may raise to abort | 0 | Not timely during system calls or blocking C calls. Stable ABI [S39] |
| `sys.settrace` / `PyEval_SetTrace` line checks | n/a (per thread) | Yes | High: line events on every line, and the 3.11 adaptive interpreter loses speed | Conflicts with debugpy, coverage and our debugger. 3.12+ `sys.monitoring` is cheaper [S40] |
| Cooperative `unreal.ScopedSlowTask` (`should_cancel`, delayed dialog) | n/a | Only if the script calls it | 0 | Epic's documented progress and cancel pattern [S41] |

**Design: the RunSupervisor**
1. **Defer the run to a clean point.** Do not execute inside the button's Slate input handler. Queue the run and start it from a top-level ticker callback, so any nested pumping is one level deep.
2. **Watchdog thread.** It is armed at run start. After 250 ms it calls `Py_AddPendingCall(Pump)` every ~100 ms.
3. **`Pump` runs on the game thread** at a bytecode boundary, with a re-entrancy guard. It:
   - drains the output queue into the OutputStore;
   - calls a **Slate-only tick** (no world tick, no GC, no core ticker) at up to ~15 Hz;
   - registers an input pre-processor for the duration of the run.

   The pre-processor swallows all input except the Stop button, Esc, output scroll and the run-status overlay. That prevents the user from editing buffers, starting another run or deleting assets mid-script. This uses the same idea as slow-task and modal loops: `FSlateApplication` exposes a modal-loop tick event [S42].
4. **Stop** sets a flag. The next `Pump` returns -1 with `ScriptCancelled` set. `ScriptCancelled` is a `BaseException` subclass, so `except Exception:` does not catch it.
   - If the run is still alive 500 ms later, re-inject every 250 ms, which defeats bare `except:` loops.
   - The final fallback is `PyThreadState_SetAsyncExc`.
   - The user is never told "stopped" until the call actually returns.
5. **Native code honesty.** If no pump has run for over 2 s, the overlay cannot update, because the game thread is inside a C++ call or a system call. The watchdog records this and the post-run report says "Script spent 41 s inside native calls; cancellation applies when they return." Document that `time.sleep()` and `Thread.join()` on the game thread delay cancellation [S39 warning].
6. **No auto-kill.** Optional per-studio soft limits ("warn after 60 s") only show UI. Killing the interpreter is not survivable.
7. **Cooperative APIs for authors.**
   - `script_editor.progress(i, n, msg)` and `script_editor.check_cancel()`.
   - Interop with `unreal.ScopedSlowTask`: our Stop also sets its cancel.
   - `@script_editor.task` runs a generator one step per editor tick, in the spirit of Epic's latent-command scheduler [S3], so long batch jobs keep the editor live.
8. **Open risk.** During the pump, other tools' Slate post-tick Python callbacks may run in the middle of the user's script. Mitigation: a setting "Run Slate callbacks during script pump" (default off), if PythonScriptPlugin lets us suppress them. UNVERIFIED (see Open questions).

### 2.3 Output at scale

**Failure modes**
- The log view slows as it grows, or freezes when shown [S9].
- O(N) refresh per append [S10].
- Per-line widget creation.
- Unbounded memory.
- Echoing 100k lines into UE's Output Log slows the editor even if our view is fine.

**Design**
- **Ingest.** A C++ `sys.stdout`/`sys.stderr` writer pushes to a lock-free MPSC queue, so writes from Python background threads are safe. It is chained to the previous stream so `LogPython` still works.
  - An optional `FOutputDevice` captures UE log categories during the run.
  - Lines are split, tagged by stream or severity, run ID and timestamp, and SGR colour codes are parsed into style spans. Other ANSI escapes are stripped.
- **Store.** A chunked ring buffer.
  - Default cap: 200k lines or 64 MB, whichever comes first, per output pane. When truncating, say so ("… 1,204,551 earlier lines dropped — saved to Saved/Logs/ScriptEditor/run-0142.log").
  - Store text as UTF-8 in 64 KB chunks with a line-offset index. Styles are run-length spans.
  - Full logs of runs over the cap spill to disk.
- **Drain budget.** At most 2 ms per frame on the game thread. Coalesce, then mark the view dirty once per frame, never per line.
- **View.** A custom virtualized leaf widget with fixed line height. It paints only visible rows, so there are no per-line widgets.
  - Auto-follow when at the bottom. Hold position and show "↓ 3,421 new lines" otherwise.
  - Selection and copy work across chunks.
  - Soft-wrap is computed lazily for visible lines only.
- **Filtering and search.** Severity, source (stdout, stderr, UE category), run ID, and a regex.
  - Filtering runs on a background thread, producing a filtered index. The view swaps to it atomically, which is the deferred-refresh lesson from [S10].
  - Incremental search highlights matches in the viewport first.
- **Clickable tracebacks.** Recognize `File "<path>", line N`, our pseudo-filenames (`<sedit:tab-uuid>`), and UE log `file(line)` patterns. Clicking opens or focuses the tab at that line.
- **Echo policy.** Mirror to the Output Log by default, but rate-limit. Above 2k lines/s, collapse to "[ScriptEditor] 48,000 lines suppressed; see Script Editor output".
- **Per-run grouping.** Collapsible run headers ("▶ Run 142 · rig_fix.py · 3.2 s · exit: ScriptCancelled") make giant sessions navigable.

### 2.4 Execution semantics

**Namespaces**
- **Default "Session" namespace**, shared by all tabs and the REPL. This matches Maya and UE's console (`PUBLIC` = "the shared locals and globals dict as used by the console" [S43]). Users rely on variables persisting between runs.
- **Opt-in "Tab namespace"**, one dict per tab, persisting across that tab's runs.
- **Opt-in "Isolated run"**, a fresh dict per run like `PRIVATE` [S43]. It is the default for "Run File" on saved files under trusted script roots.
- The current namespace mode is always visible in the tab's run button tooltip and status bar.

**Dunder names**
- `__name__ == "__main__"` for all buffer runs, so `if __name__ == "__main__":` works.
- `__file__` is set only for saved files, to the real path. It is unset for untitled buffers, because a fake path breaks `os.path.dirname(__file__)` silently.
- Source is registered in `linecache` under `<sedit:tab-uuid>` so tracebacks, `inspect` and our debugger show source lines.

**Run selection with correct line numbers.** Parse with `ast`, shift with `ast.increment_lineno` to the selection's start line, and compile with the buffer's filename. A traceback then points at the right line in the tab. If the last statement is an expression, echo its `repr` through `sys.displayhook` (Maya and Jupyter feel).

**sys.path**
- Never permanently modified by a run.
- "Run File" temporarily prepends the file's directory, like `python script.py`, and restores it afterwards.
- A read-only "Environment" panel shows `sys.path`, the `UE_PYTHONPATH` contribution [S2], loaded studio packages with versions and file locations, and whether isolation mode is on [S36].

**Reloading studio packages.** `importlib.reload` keeps the old module dict and does not rebind `from x import y`. Instances keep old classes, and extension modules "may fail in arbitrary ways" [S45]. IPython's autoreload patches code objects but still fails on property/method changes and creates non-identical Enums [S46]. Our approach:
- "Reload package…" builds the dependency graph from `sys.modules`, restricted to modules whose `__file__` is under the chosen roots.
- It calls each module's optional `__sedit_unload__()` hook in reverse dependency order. The hook unregisters Slate tick callbacks, delegates, menus and Qt widgets.
- It deletes the modules from `sys.modules` and re-imports the top-level entry points.
- It refuses to touch `unreal`, builtins and C extensions.
- It reports objects that still reference old modules (gc referrers, sampled) as "stale references" warnings.

**Undo**
- Wrap each Run in `unreal.ScopedEditorTransaction("Script Editor: <tab>")` [S47], so one Ctrl+Z undoes a script's undoable edits. This is on by default, with a per-tab toggle.
- Document that imports and file operations are not undoable [S2].
- Show a one-time tip that very large batch scripts may want it off, because transactions snapshot every modified object.
- Rollback on exception is opt-in. Whether `cancel()` reverts changes or only stops recording is UNVERIFIED and must be tested before we promise rollback.

**Threading guard.** Detect UObject access from non-game Python threads where feasible, and print a warning with a link to docs.

### 2.5 Out-of-process language services

**Failure modes**
- The LSP crashes, hangs or leaks memory.
- Orphaned server processes survive an editor crash.
- Stale completions apply to a changed document.
- The monolithic `unreal.py` stub defeats analysis:
  - Pylance refuses files over 50 MB, at 52,986,705 bytes in a large project; this is Epic issue UE-119549, open since 2021 [S48]. The cap may come from the VS Code host rather than Pyright core [S52].
  - Pyright's hard-coded code-complexity limit stops analysing a ~24 MB stub, and the maintainers closed that report "by design" [S49].
  - PyCharm runs low on memory with the 21 MB / 400k-line UE 5.1 stub [S50]. Epic's UE4 docs needed `idea.max.intellisense.filesize` raised [S80].
  - Pyright can hit Node heap exhaustion on huge files [S53].

**Design**
- **Server choice.** The server runs behind an adapter, so we can swap it per studio policy.
  - **Pyrefly**: MIT, 1.0 stable, `pip install pyrefly`, Rust binary with no Node [S54]. It ships through `PythonRequirements` [S33].
  - **basedpyright**: MIT [S56]. Mature Pyright semantics, but a Node runtime is required (bundled or pip-provided; UNVERIFIED which).
  - **ty**: MIT, still beta and 0.0.x [S55]. Watch it.
  - **jedi-language-server**: pure Python on UE's bundled interpreter. A fallback for minimal installs (UNVERIFIED performance on our stubs).
- **Generate a split stub package.** Emit `unreal/__init__.pyi` plus per-module `.pyi` files (by owning C++ module or plugin), with base classes ordered before subclasses. This follows Pyright maintainers' advice to split [S49] and the community splitter precedent [S50]. Generate from the engine's Python stub output or a reflection walk. Cache by engine version, plugin set and project-module hash, and regenerate in the background on change. Exclude the original `Intermediate/PythonStub/unreal.py` [S51] from analysis. The same package feeds the external IDEs (D), so everyone benefits.
- **Supervision**
  - Spawn on first buffer open, not at editor launch.
  - Talk stdio JSON-RPC on a dedicated I/O thread. The game thread only posts and receives messages through queues.
  - Restart with backoff: 1 s, 2 s, 4 s … capped at 60 s. After 5 crashes in 3 minutes, stop and show "Language features paused — [Restart] [View log]". Editing and running are never affected.
  - Use a Windows Job Object (kill-on-close), `PR_SET_PDEATHSIG` on Linux, and a parent-PID watch on macOS, so the server dies with the editor.
  - Cap memory with the server's own flag where available. Kill and restart if it stays over the cap for more than 60 s.
- **Timeouts and staleness**
  - Completion: send `$/cancelRequest` after 1.5 s. Hover: 2 s.
  - Every response carries the document version. Discard results for older versions.
  - Diagnostics are debounced 400 ms after typing stops.
- **Runtime completion, in process.** Live-namespace names come from `dir()` and type-level inspection. Time-boxed to ≤ 5 ms on the game thread, never calling `getattr` on properties (they can run arbitrary code), and cached per run. Results are merged with LSP items: runtime items rank first for names bound in the session.

### 2.6 Debugging Python that runs on the game thread

**The problem.** A paused frame is a paused game thread, so Slate cannot paint the debugger UI. External debuggers sidestep this because their UI lives in another process. With Maya and PyCharm, "the Software will likely freeze until you resume" [S58][S59].

**Prior art**
- Slate has an explicit "debugging mode": `FSlateApplication::EnterDebuggingMode()` "causes the Slate application to tick in place". There are also `LeaveDebuggingMode(bLeavingDebugForSingleStep)` and `FScopedPreventDebuggingMode` (a stack of reasons why debugging mode cannot be entered) [S42]. This looks purpose-built for script breakpoints such as the Blueprint debugger; which engine callers use it in 5.8 is UNVERIFIED.
- Modal loops tick Slate in place via `ModalLoopTickEvent` [S42].
- NVIDIA Omniverse Kit embeds debugpy as an extension (`omni.kit.debug.python`) [S60].
- Hazelight's UE Angelscript ships a DAP debug adapter for VS Code that talks to the running editor [S61].
- debugpy can run its adapter in process (`in_process_debug_adapter`, at the cost of subprocess debugging); `debugpy.breakpoint()` pauses all threads [S57].

**Design: two debuggers, one active at a time** (both use the trace hook)

1. **In-editor debugger (bdb-based)**
   - Breakpoints, step in/over/out, a call stack, locals and watches, and an evaluate-in-frame REPL.
   - The trace function is installed only when breakpoints exist, and it returns `None` for code objects with no breakpoints, which is bdb's `break_anywhere` optimization. That keeps overhead near zero in unrelated code.
   - On pause it enters a **restricted nested pump**: `EnterDebuggingMode`, or our equivalent Slate-only tick loop. The same input pre-processor as 2.2 lets through only the debugger panels, the script tabs (read-only while paused) and the Continue/Stop controls. The world does not tick and GC does not run.
   - `FScopedPreventDebuggingMode`-style guards refuse to pause inside nested pumps or during PIE transitions.
   - Evaluate-in-frame runs on the game thread inside the pause, and is explicitly labelled "may modify editor state".
   - Move to `sys.monitoring` when UE ships Python ≥ 3.12 [S40][S1].
2. **External attach (debugpy)**
   - "Start debug server" calls `debugpy.listen(127.0.0.1:<port>)` with the port shown and copyable, and the VS Code/PyCharm configuration generated.
   - Warn up front that the editor UI freezes while paused, because it is the external IDE's pause.
   - The adapter may need the bundled `python.exe` path, because `sys.executable` in an embedded host is not a Python interpreter (UNVERIFIED for UE 5.8).
3. **Arbitration.** The DebugService owns the trace slot. Starting one mode disables the other, and also warns if coverage or a profiler hook is present.

### 2.7 Security

**Threats**
- Running a script from an untrusted shared drive or a downloaded file.
- Snippet or shelf libraries on network shares being tampered with.
- Remote-execution exposure. Epic's protocol has no authentication beyond a magic string and UUIDs [S35]. The node is told via UDP to connect back to an arbitrary IP and port [S35]. The multicast bind address `0.0.0.0` binds all adapters, and TTL 1 reaches the subnet [S36].
- `init_unreal.py` auto-runs from any configured path [S2], which is a supply-chain risk outside our scope but worth surfacing.

**Design**
- **Trust model**, after VS Code Workspace Trust's Restricted Mode with trusted folders inherited by subfolders [S65]. Zones:
  - studio-trusted roots (from policy);
  - the project tree (prompt once per project);
  - user folders;
  - everything else, which is restricted.

  In a restricted file, you can edit freely. Run requires a confirmation that shows the full path and the SHA-256, and offers "Trust folder" unless policy forbids it.
- **Studio policy file**: read-only ini on a share or env-var-pointed, in a layer users cannot override. Keys:
  - `TrustedRoots`, `BlockedRoots`
  - `AllowUserTrust=false`
  - `RequireHashPinnedSnippets`
  - `DisableBridge`
  - `AllowDebugServer`
  - `TelemetryMode`
- **Snippet and shelf libraries.** Each has a manifest with per-item SHA-256 and a version. Hash mismatches show "Modified since publish", and running such an item requires confirmation.
- **No auto-run features** in our plugin: no run-on-open, no run-on-startup. Startup scripts stay with UE's own mechanism.
- **Our bridge** for external IDEs and tools:
  - off by default;
  - loopback only, random port;
  - per-session token in a user-only-readable connection file;
  - HMAC-SHA256 message signing, following the Jupyter wire protocol precedent [S66];
  - WebSocket endpoints reject browser `Origin`s, to block DNS rebinding from web pages;
  - audit log of executed commands (hash, size, client).
- **Epic Remote Execution**, if a user enables it: show a status-bar warning when the bind address is `0.0.0.0` or TTL > 0 [S36]. Do not build on it ourselves.
- **Local audit log** of runs: path or buffer ID, content hash, duration, outcome. Optionally forwarded to a studio sink.

### 2.8 Studio deployment

- **Settings layering**, using `UDeveloperSettings`:
  - `UScriptEditorProjectSettings` (`Config=Editor`, `defaultconfig` → `Config/DefaultEditor.ini`, checked in): tab size, formatter, LSP server choice, trusted project roots, snippet library URLs.
  - `UScriptEditorUserSettings` (`EditorPerProjectUserSettings`, under `Saved/`, not in P4): theme, fonts, keymap overrides, namespace default, layout.
  - Global per-user keymaps and themes, to follow users across projects.
  - The studio policy layer (2.7) wins over all of them.
- **Snippet and shelf libraries.**
  - Three tiers: studio (read-only, network), project (in repo) and user (local, writable).
  - Network libraries are mirrored to a local cache on startup and periodically, for offline use and to avoid SMB latency on the game thread.
  - One item per file (JSON metadata + `.py` body) keeps Perforce merges trivial.
  - Paths use tokens (`{Project}`, `{Studio}`, `{Engine}`), never absolute drive letters.
  - Manifests carry a schema version, with migrations.
- **Perforce friendliness.**
  - Detect read-only files on save and offer "Check out" through the editor's source-control provider, "Make writable" or "Save as".
  - Never write caches into `Content/` or `Config/`. Use `Saved/` and `Intermediate/` (already ignored by typical P4 typemaps), or per-user AppData.
  - Use stable key ordering and LF in all files we author.
- **Shipping Python deps** through `.uplugin` `PythonRequirements` with strict hashes: binary wheels into `Intermediate/PipInstall` [S33]. Our helper package is namespaced (`ue_script_editor`) and vendors nothing that commonly clashes (no Qt, no `requests` pin).
- **Telemetry.** Opt-in only and off by default. It is studio-routable, never contains code or paths, and is limited to aggregate timings and crash or recovery counts. The policy file can force it off.
- **Versioning.** The plugin version is shown in About and in logs. The session and journal format is versioned, and the plugin refuses to downgrade a newer journal format.

### 2.9 Performance budgets

**Reference points.** Pavel Fatin measured average typing latency at:

| Editor | Average latency |
|---|---|
| GVim | 0.9 ms |
| IntelliJ zero-latency mode | 2.9 ms |
| Sublime Text | 8.2 ms |
| IntelliJ default | 24.7 ms |
| Atom | 49.4 ms |

He argues that even 20 ms of visual delay is noticeable, and that editor-side work should aim for 1–5 ms [S67]. IntelliJ achieved its "zero-latency" mode by painting immediately and bypassing double-buffering [S68].

**UE-specific truth.** Every in-process option paints once per editor frame: Slate always, CEF when the texture refreshes, and Qt on Windows through the shared message pump [S25]. So **keystroke-to-photon ≥ one editor frame.** A Lumen viewport at 25 fps means ≥ 40 ms echo, whatever we do. Budget our own work tightly, measure end to end with Unreal Insights, and add a user option "Pause realtime viewports while the Script Editor has focus" (feasibility UNVERIFIED).

| Budget | Target |
|---|---|
| Added editor startup | ≤ 30 ms (module load, tab and menu registration only). No Python import, LSP spawn or journal scan until first open |
| First open of tab (restore 20 tabs) | ≤ 300 ms. Only the visible buffer is loaded and highlighted; others load lazily |
| Our keystroke processing (100k-line file) | ≤ 2 ms p95 (model, incremental tokenize, invalidate) |
| Our paint (4K, full viewport of code) | ≤ 2 ms p95. ≤ 0.5 ms when only the caret changed |
| End-to-end keystroke-to-present (editor idle at ≥ 60 fps) | ≤ 16 ms p95 (one frame) |
| Runtime (live-namespace) completion | ≤ 5 ms game-thread time box, popup ≤ 30 ms |
| LSP completion | p50 ≤ 150 ms, p95 ≤ 400 ms. Never blocks the UI |
| Diagnostics refresh | ≤ 1 s after typing pause |
| Output ingest | Sustain 50k lines/s with ≤ 2 ms/frame game-thread cost. Scroll 200k lines at 60 fps |
| Open 10 MB file | ≤ 1 s to first paint. Highlighting progressive |
| Memory (core, 20 tabs, full output ring) | ≤ 50 MB + 3× text size. LSP process ≤ 1.5 GB (policy-capped) |
| Journal snapshot cost on game thread | ≤ 0.2 ms (COW snapshot); I/O on background thread |

### 2.10 Testing strategy

- **Pure C++ core** (text model, undo, tokenizer, search, LSP framing), built as a UE-independent library:
  - fast unit tests on every commit, on all three platforms, without the engine;
  - **golden tests** for the tokenizer and highlighter over a corpus (CPython stdlib plus anonymized studio code), with token streams snapshotted;
  - **property and differential fuzzing** with libFuzzer: random edit sequences must produce the same incremental tokenization as a full retokenize; tokens must cover the text with no gaps; undo/redo round-trips must hold; no crash on invalid UTF-8.
- **Slate widget tests.** Automation Spec and CQTest:
  - `FCQTestSlateComponent` counts Slate ticks [S71];
  - Automation Driver simulates typing, clicks and drag at the platform layer [S69].
  - Known pitfalls: the Automation Driver's synchronous API can deadlock on the game thread [S73], and simulated input is unreliable on RDP or headless agents [S70]. Use the async driver and dedicated GPU agents, or `-RenderOffscreen`, for UI suites.
- **Python-side tests.**
  - The PythonAutomationTest plugin discovers `test_*.py` in `Content/Python` [S3], and `unreal.PythonTestRunner` (5.8, experimental) registers unittest suites with the automation runner [S72]. Use them for the execution-semantics matrix: namespaces, `__file__`, line offsets, reload, transactions.
  - Headless runs use `UnrealEditor-Cmd <proj> -run=pythonscript -script=…` (no levels auto-loaded) [S2], or `-ExecCmds="Automation RunTests ScriptEditor; Quit" -unattended`.
- **Robustness suites**
  - **Cancel:** 1,000 run-and-stop cycles across loop shapes (tight loop, bare-except loop, generator, UE-call loop).
  - **Crash recovery:** kill -9 the editor at random points during typing, autosave and run, then verify the journal. This is the most important suite.
  - **Output soak:** 10M lines over 8 hours, with memory growth ≤ 5%.
  - **LSP chaos:** kill, hang or slow the server at random; the editor stays responsive.
  - **Engine-upgrade canary:** build against the next engine preview and main when available, especially to catch the Python 3.13 switch early [S1].
- **Performance regression gates** in CI: frame-time and latency budgets from 2.9, recorded as automation telemetry measurements [S3], compared against a rolling baseline per build machine.
- **CI matrix.** Win64 (primary), macOS and Linux editor builds. IME smoke tests are manual per release; a checklist of IMEs is attached to the release template.

---

## Risks register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | A PyCharm-quality custom Slate code view overruns the schedule (text model, IME, a11y, features) | High | High | Phase 0 spike with numeric exit criteria. Engine-independent core. Ruthless v1 scope (LSP and external IDE carry advanced features). A1 plain-mode fallback per buffer. Renderer seam keeps a later Monaco option open |
| R2 | IME composition bugs in the custom widget (CJK users blocked); Linux IME known-bad [S74] | Medium | High | Implement `ITextInputMethodContext` in the first sprint, modelled on Epic's [S11]. Per-release IME checklist. Plain-mode fallback toggle |
| R3 | Typing latency bound by editor frame time (heavy viewports) | High | Medium | Tight own budgets (2.9). Insights instrumentation. Optional viewport throttle while focused. Document clearly |
| R4 | Re-entrancy crashes from nested Slate pumping (run pump, debugger pause) | Medium | High | Defer runs to a clean tick point. Input pre-processor whitelist. No world tick or GC in pump. Guards akin to `FScopedPreventDebuggingMode` [S42]. Soak and fuzz suites. Studio kill-switch "disable live pump" |
| R5 | Cancellation cannot interrupt native or system calls; users perceive Stop as broken | High | Medium | Honest overlay states. Post-run report of native time. Cooperative APIs and ScopedSlowTask interop [S41]. Docs |
| R6 | `unreal` stub size defeats the LSP (size or complexity caps, OOM) [S48][S49][S53] | High | High | Split `.pyi` generator. Exclude monolith. Server memory caps. Fall back to runtime completion for `unreal` |
| R7 | LSP crash, hang or leak | Medium | Medium | Supervisor with backoff, Job Objects or parent-death signal, version-stamped responses, timeouts |
| R8 | Engine upgrade breaks APIs, or the Python 3.13 bump changes ABI or tracing [S1] | Medium | High | Public APIs only. Thin compatibility module. Stable-ABI C calls (`Py_AddPendingCall` and `PyThreadState_SetAsyncExc` are stable ABI [S39]). Canary CI on previews. Tracing abstraction ready for `sys.monitoring` |
| R9 | Data loss despite autosave (torn writes, crash during save, multi-instance clobber) | Low | High | WAL plus atomic rename plus previous generation, pre-run snapshot, PID locks, kill -9 test suite |
| R10 | Users run untrusted scripts from shares; bridge exposure | Medium | High | Trust zones and policy, hash-pinned libraries, loopback+token+HMAC bridge, audit log, Remote Execution exposure warnings |
| R11 | Interpreter-sharing conflicts (other tools' tick callbacks run inside our pump; package clashes) | Medium | Medium | Optional suppression of foreign callbacks during pump (if feasible). Namespaced helper package. No Qt or common-lib pins |
| R12 | Perforce friction (read-only files, merge conflicts in shared libraries) | Medium | Low | Checkout-on-save prompt, one-item-per-file libraries, caches outside Content/Config |
| R13 | Trace-hook conflicts (our debugger vs debugpy, coverage or profilers) | Medium | Medium | Single owner of the trace slot. Cancellation does not use tracing. Clear UI when another tracer is detected |
| R14 | Output floods slow the UE Output Log even when our view is fine | Medium | Medium | Rate-limited echo with summary lines. Spill to file |
| R15 | Undo transactions around huge batch scripts consume memory, or partial rollback confuses users | Medium | Medium | Default-on with per-tab toggle and tip. Rollback opt-in only after validating `cancel()` semantics |

---

## Open questions (spikes to run on UE 5.8)

1. Does PythonScriptPlugin initialize CPython on the game thread, so that `Py_AddPendingCall` callbacks run there? Does it install a Python SIGINT handler (`initsigs`)? This decides whether `PyErr_SetInterruptEx` is viable at all. [S38][S39]
2. Does PythonScriptPlugin release the GIL during long reflected C++ calls? If so, the watchdog's `PyThreadState_SetAsyncExc` fallback and our I/O threads behave better.
3. Exact semantics and safe usage of `FSlateApplication::EnterDebuggingMode` outside the Blueprint debugger and PIE. Does a Slate-only tick in our pump repaint all windows safely while the game thread is mid-script? [S42]
4. Can foreign Python Slate tick callbacks (`register_slate_post_tick_callback`) be suppressed during our pump without patching the engine?
5. `ScopedEditorTransaction.cancel()`: does it revert changes or only stop recording? [S47]
6. Language server shortlist: measured RSS and latency of pyrefly vs basedpyright vs jedi on a split stub for a 100-plugin project. How does basedpyright obtain Node when pip-installed (UNVERIFIED)? Is ty mature enough by release? [S54][S55][S56]
7. Stub generation source: post-process `Intermediate/PythonStub/unreal.py`, which needs Developer Mode and its boot-time cost [S36], or walk reflection ourselves? Can we generate without forcing Developer Mode on artists?
8. IME on macOS and Linux for a custom `ITextInputMethodContext` widget. What is the minimum viable Linux story, given forum reports through 5.8 [S74]?
9. Default namespace choice (shared vs per-tab): needs a quick survey of Maya/Charcoal TDs. The recommendation is shared by default, but confirm.
10. Feasibility of "pause realtime viewports while Script Editor focused" without side effects on PIE and other tools.
11. Current status of UE-75056 (CEF IME) and of CEF high-DPI in 5.8. This is only relevant if Monaco is reconsidered later. [S18]
12. Telemetry: legal and privacy review for an opt-in, studio-routable channel.
13. Research limitation: the shared web-search budget was exhausted near the end of this pass. Items marked UNVERIFIED above that rely on engine internals should be confirmed against 5.8 source (the local UE_5.8 install contained only `.egstore` metadata at research time).

---

## Sources

1. [S1] Epic forums — "Unreal Engine 5.8 is on Python 3.11 when the VFX Reference Platform 2026 recommends Python 3.13" (Epic reply, 2026-07-23): https://forums.unrealengine.com/t/unreal-engine-5-8-is-on-python-3-11-when-the-vfx-reference-platform-2026-recommends-python-3-13-as-minimum-version/2745703
2. [S2] Epic docs — Scripting the Unreal Editor Using Python: https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python
3. [S3] Epic docs — Write Editor Tests with Python (5.8): https://dev.epicgames.com/documentation/unreal-engine/write-editor-tests-with-python-in-unreal-engine
4. [S4] Epic API — `ITextLayoutMarshaller` (`RequiresLiveUpdate`, `MakeDirty`): https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Slate/ITextLayoutMarshaller
5. [S5] Epic API — `FSyntaxHighlighterTextLayoutMarshaller`: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Slate/FSyntaxHighlighterTextLayoutMars-
6. [S6] Epic API — `FHLSLSyntaxHighlighterMarshaller`: https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FHLSLSyntaxHighlighterMarshaller
7. [S7] Epic API — `FNiagaraHLSLSyntaxHighlighter`: https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/NiagaraEditor/FNiagaraHLSLSyntaxHighlighter
8. [S8] Epic API — `FSyntaxTokenizer`: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Slate/FSyntaxTokenizer
9. [S9] Epic forums/Answers — Output Log performance degrades with log size (UE4-era reports): https://answers.unrealengine.com/questions/115724/view.html ; https://forums.unrealengine.com/t/clear-output-log/37666
10. [S10] Lars Tofus — "How to make Unreal's Message Log 100 times faster" (2026-05-28): https://larstofus.com/2026/05/28/how-to-make-unreals-message-log-100-times-faster/
11. [S11] Epic API — `FSlateEditableTextLayout::FTextInputMethodContext`: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Slate/FSlateEditableTextLayout/FTextInputMethodContext
12. [S12] Epic API — `ITextInputMethodSystem`: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/ApplicationCore/ITextInputMethodSystem
13. [S13] UE issue UE-234820 (Japanese IME in MultiLineEditBox): https://issues.unrealengine.com/issue/UE-234820
14. [S14] Epic docs — Supporting Screen Readers in Unreal Engine: https://dev.epicgames.com/documentation/en-us/unreal-engine/supporting-screen-readers-in-unreal-engine
15. [S15] Epic API — `SWebBrowser` (`BindUObject`, `ExecuteJavascript`): https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/WebBrowser/SWebBrowser
16. [S16] Epic API — `SWebBrowserView::SetBrowserKeyboardFocus`: https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/WebBrowser/SWebBrowserView/SetBrowserKeyboardFocus
17. [S17] Epic forums — web browser widget focus goes to container, not browser: https://forums.unrealengine.com/t/411397
18. [S18] UE issue UE-75056 (IME disabled in Web Browser widget; tracker returned 403 at research time): https://issues.unrealengine.com/issue/UE-75056
19. [S19] Coherent Labs — considerations when using CEF in games: https://coherent-labs.com/posts/?p=3908 ; CEF OSR shared-texture sample: https://github.com/twobrainsgmbh/cef-mixer ; UCefView listing (incompatible with built-in WebBrowser plugin): https://fab.com/s/ec232806c889
20. [S21] Marijn Haverbeke — CodeMirror 6 beta: https://marijnhaverbeke.nl/blog/codemirror-6-beta.html
21. [S22] Obsidian docs — editor viewport (only visible range rendered): https://docs.obsidian.md/Plugins/Editor/Viewport
22. [S23] TypeFox — monaco-languageclient README (Python/Pyright WebSocket example): https://github.com/TypeFox/monaco-languageclient/blob/main/README.md
23. [S24] Microsoft — monaco-editor: https://github.com/microsoft/monaco-editor
24. [S25] tk-unreal PR (Qt events must be pumped via Slate tick on non-Windows): https://github.com/GPLgithub/tk-unreal/pull/2/files
25. [S26] hannesdelbeke/unreal-qt README: https://github.com/hannesdelbeke/unreal-qt
26. [S27] leixingyu/unrealScriptEditor (Qt "Maya Script Editor" for UE, MIT): https://github.com/leixingyu/unrealScriptEditor
27. [S28] tech-artists.org — "PySide and Unreal5": https://www.tech-artists.org/t/pyside-and-unreal5/15475
28. [S29] Epic forums — PySide6 drag-and-drop broken in UE 5.7 (UE-364137): https://forums.unrealengine.com/t/pyside6-ue5-7-5-6/2704369
29. [S30] Riverbank — QScintilla (GPLv3/commercial, PyQt bindings): https://riverbankcomputing.com/software/qscintilla/intro
30. [S31] Python GUIs — PyQt6 vs PySide6 licensing: https://www.pythonguis.com/faq/licensing-differences-between-pyqt6-and-pyside6/
31. [S32] Qt for Python — Commercial distribution notes: https://doc-snapshots.qt.io/qtforpython-6.5/commercial/index.html
32. [S33] Epic docs — Python plugin dependency management (`PythonRequirements`, PipInstall): https://dev.epicgames.com/documentation/en-us/unreal-engine/python-plugin-dependency-management-for-unreal-engine
33. [S34] VS Code Marketplace — Unreal Engine Python (Nils Söderman): https://marketplace.visualstudio.com/items?itemName=NilsSoderman.ue-python ; Codeberg: https://codeberg.org/nils-soderman/vscode-unreal-python ; archived GitHub: https://github.com/nils-soderman/vscode-unreal-python
34. [S35] Epic `remote_execution.py` (© Epic Games; mirrored in a third-party repo): https://raw.githubusercontent.com/radial-hks/mcp-unreal-server/master/src/mcp_server_unreal/remote_execution.py
35. [S36] Epic docs — Python settings in Project Settings (remote execution bind address/TTL, isolation, developer mode): https://dev.epicgames.com/documentation/en-us/unreal-engine/python-settings-in-the-unreal-engine-project-settings
36. [S38] Python 3.11 C API — Exception handling (`PyErr_SetInterruptEx`, `PyErr_CheckSignals`): https://docs.python.org/3.11/c-api/exceptions.html
37. [S39] Python 3.11 C API — Initialization, threads (`PyThreadState_SetAsyncExc`, `Py_AddPendingCall`, `Py_InitializeEx`, `PyEval_SetTrace`): https://docs.python.org/3.11/c-api/init.html
38. [S40] PEP 669 — Low Impact Monitoring for CPython (3.12+): https://peps.python.org/pep-0669/
39. [S41] Epic Python API — `unreal.ScopedSlowTask`: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/ScopedSlowTask
40. [S42] Epic API — `FSlateApplication` (`EnterDebuggingMode`, `LeaveDebuggingMode`, `FScopedPreventDebuggingMode`, `ModalLoopTickEvent`): https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Slate/FSlateApplication
41. [S43] Epic Python API — `unreal.PythonFileExecutionScope`: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/PythonFileExecutionScope
42. [S44] Epic Python API — `unreal.PythonScriptLibrary`: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/PythonScriptLibrary
43. [S45] Python 3.11 — `importlib.reload` caveats: https://docs.python.org/3.11/library/importlib.html
44. [S46] IPython — autoreload extension caveats: https://ipython.readthedocs.io/en/stable/config/extensions/autoreload.html
45. [S47] Epic Python API — `unreal.ScopedEditorTransaction`: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/ScopedEditorTransaction
46. [S48] Epic forums — "unreal.py stubs file may become too big for Pylance / VSCode" (UE-119549): https://forums.unrealengine.com/t/unreal-py-stubs-file-may-become-too-big-for-pylance-vscode/2684691
47. [S49] microsoft/pylance-release #7274 — Unreal PythonStub regression, complexity limit, "by design": https://github.com/microsoft/pylance-release/issues/7274
48. [S50] TAColor — Split unreal.py for a better development experience: https://www.tacolor.xyz/TipsOfDay/Split_unreal_py_for_a_better_development_experience.html
49. [S51] Epic docs — Setting up autocomplete for Unreal Editor Python scripting (stub in `Intermediate/PythonStub`): https://dev.epicgames.com/documentation/unreal-engine/setting-up-autocomplete-for-unreal-editor-python-scripting
50. [S52] Cursor forum — origin of the 50 MB "maximum supported file size": https://forum.cursor.com/t/file-length-of-is-size-in-bytes-which-exceeds-the-maximum-supported-file-size-of-52428800/160900
51. [S53] microsoft/pyright #7159 — JS heap OOM on large file: https://github.com/microsoft/pyright/issues/7159
52. [S54] facebook/pyrefly (MIT, 1.0, pip): https://github.com/facebook/pyrefly
53. [S55] astral-sh/ty (MIT, beta): https://github.com/astral-sh/ty
54. [S56] basedpyright on PyPI (MIT): https://pypi.org/project/basedpyright/
55. [S57] microsoft/debugpy (API incl. `in_process_debug_adapter`, `breakpoint()`): https://github.com/microsoft/debugpy
56. [S58] ShotGrid community — Remote debugging (host freezes until resume): https://community.shotgridsoftware.com/t/remote-debugging/3869
57. [S59] tech-artists.org — PyCharm remote debugging Maya: https://www.tech-artists.org/t/pycharm-remote-debugging-maya-question/2969
58. [S60] NVIDIA Omniverse Kit — omni.kit.debug.python (embedded debugpy): https://docs.omniverse.nvidia.com/kit/docs/omni.kit.debug.python/1.0.1/debugpy/debugpy.breakpoint.html
59. [S61] vscode-unreal-angelscript (language server + debug adapter for UE Angelscript): https://github.com/joachimolergard/vscode-unreal-angelscript
60. [S62] VS Code — Hot Exit (blog and 1.8 release notes): https://code.visualstudio.com/blogs/2016/11/30/hot-exit-in-insiders ; https://code.visualstudio.com/updates/v1_8
61. [S63] JetBrains — IntelliJ IDEA Local History: https://www.jetbrains.com/help/idea/local-history.html
62. [S64] Autodesk — Maya Script Editor menus (Save All Script Tabs); forum on lost tabs: https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Scripting/files/GUID-B66020CC-1BBE-4EA1-B1DE-A288B044DD80.htm ; https://forums.autodesk.com/t5/maya-programming-forum/mel-command-save-contents-of-script-editor-tabs-and-mtk-shelf/td-p/9538713
63. [S65] VS Code — Workspace Trust: https://code.visualstudio.com/docs/editing/workspaces/workspace-trust
64. [S66] Jupyter client — Messaging (HMAC signatures): https://jupyter-client.readthedocs.io/en/stable/messaging.html
65. [S67] Pavel Fatin — Typing with pleasure: https://pavelfatin.com/typing-with-pleasure/
66. [S68] JetBrains — Experimental zero-latency typing in IntelliJ IDEA 15 EAP: https://blog.jetbrains.com/idea/2015/08/experimental-zero-latency-typing-in-intellij-idea-15-eap/
67. [S69] Epic docs — Automation Driver: https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-driver-in-unreal-engine
68. [S70] Epic forums — AutomationDriver failures on RDP/headless: https://forums.unrealengine.com/t/automationdriver-iscursordirectlyoverslatewindow-and-getwindowundercursor-bypass-fautomatedcursor-causing-test-failures-on-rdp-headless/2743941
69. [S71] Epic docs — CQTest; `FCQTestSlateComponent`: https://dev.epicgames.com/documentation/en-us/unreal-engine/cqtest-test-framework-for-unreal-engine ; https://dev.epicgames.com/documentation/unreal-engine/API/Developer/CQTest/FCQTestSlateComponent
70. [S72] Epic Python API — `unreal.PythonTestRunner` (5.8, experimental): https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/PythonTestRunner
71. [S73] Epic API — `USlateInspectorToolset` (notes AutomationDriver sync API deadlock on game thread): https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/SlateInspectorToolset/USlateInspectorToolset
72. [S74] Epic forums — Linux CJK IME text not committed (through 5.8): https://forums.unrealengine.com/t/linux-cjk-ime-text-not-committed-when-bare-en-us-locale-is-missing/2741978
73. [S75] tech-artists.org — Free plugin: Python Script Editor (Qt/PySide2): https://www.tech-artists.org/t/free-plugin-python-script-editor/15918
74. [S76] unreal-qt source (`register_slate_post_tick_callback`, orphan re-parent every 0.3 s): https://raw.githubusercontent.com/hannesdelbeke/unreal-qt/main/unreal_qt/__init__.py
75. [S77] unrealScriptEditor `main.py` (parenting, `execute_python_command_ex` with `PUBLIC` scope): https://raw.githubusercontent.com/leixingyu/unrealScriptEditor/master/unreal_script_editor/main.py
76. [S78] 20tab/UnrealEnginePython (docs: do not call `app.exec_()`, tick Qt from the engine loop): https://github.com/20tab/UnrealEnginePython
77. [S80] Epic UE 4.27 docs — Python autocomplete (PyCharm `idea.max.intellisense.filesize`): https://docs.unrealengine.com/4.27/en-US/ProductionPipelines/ScriptingAndAutomation/Python/Autocomplete
