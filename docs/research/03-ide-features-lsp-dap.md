> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 03 — IDE features, language servers (LSP) and debuggers (DAP) for an in-UE Python editor

Research lane: what makes PyCharm and VS Code good for Python, and which embeddable parts and protocols can bring that into Unreal Editor 5.8.
Date: 2026-10-28. Citations use `[Sn]` and map to the **Sources** list at the end. **UNVERIFIED** marks claims I could not confirm from a primary source in this session.

> Note on method: the web-search budget for this session ran out near the end. The last few facts were checked against official pages with direct fetches, or against the locally installed UE 5.8 at `C:\Program Files\Epic Games\UE_5.8` (cited as [S53]).

---

## Summary

1. **UE 5.8 ships Python 3.11.8** (checked in the local install's `NEWS.txt` [S53]; Epic confirmed it stayed on 3.11 for 5.8 [S52]). This one fact shapes the debugger design. `sys.monitoring` (PEP 669, 3.12+) [S37], write-through `frame.f_locals` (PEP 667, 3.13) [S39] and the safe external debugger interface (PEP 768, 3.14) [S38] are **not available**. A custom debugger has to use `sys.settrace`/`bdb` today, behind an abstraction that can move to `sys.monitoring` when Epic upgrades. Epic says the next upgrade will land in main, with no timeline [S52].
2. **The `unreal.py` stub is the biggest risk for static intelligence.** It is one generated file: >21 MB and >400k lines in UE 5.1 [S11], about 24 MB in 2025 [S9], and 53 MB on a plugin-heavy project [S10]. Several hard limits sit right at that size:
   - Pyright/Pylance/basedpyright refuse files over **50 MB** (`maxSourceFileSize`) [S8][S10].
   - Pyright's code-flow **complexity limit (768)** makes it give up on the stub. The issue was closed "by design" [S9].
   - Jedi **crops files at 10 MB** [S28].
   - PyCharm's default IntelliSense limit is 2.5 MB. Epic tells users to raise it to 25 000 KB [S12][S13].
   - Monaco turns off tokenization above **20 MB / 300k lines** [S71].

   **Recommendation: our plugin owns stub generation/post-processing and emits a split `unreal/` package of `.pyi` files.** Splitting is the community fix [S11]. Nils Söderman's extension already copies `unreal.py` to `unreal.pyi` to help pyright [S46].
3. **Pausing Python pauses the editor.** Python runs on the game thread, so a breakpoint freezes Slate. External IDEs (VS Code/PyCharm with debugpy) work around this by being a separate process. An in-process debugger UI needs a nested Slate loop. UE 5.8 has exactly such a hook: `FSlateApplication::EnterDebuggingMode()`, which "causes the Slate application to tick in place … in the middle of a stack frame" and is used for Kismet (Blueprint) debugging [S53]. This is the key enabler for an in-UE PyCharm-style debugger. The UE-internals lane must validate re-entrancy.
4. **Language-server landscape (late 2026):**
   - **basedpyright** 1.40.2: MIT, Pylance features re-implemented, PyPI wheel pulls a bundled Node through `nodejs-wheel-binaries` [S3][S4][S6].
   - **pyright** 1.1.414 [S7].
   - **Pyrefly 1.4.0**: stable since 1.0 in May 2026, MIT, Rust [S20][S21]. Posit chose it for Positron [S23].
   - **ty 0.0.94**: still beta, MIT, Rust, fastest startup [S17][S23].
   - **ruff server**: stable LSP for lint, fix and format [S31].
   - **jedi-language-server**: maintenance mode; it points users to **Zuban**, which is AGPL [S25][S30].
   - **Pylance**: VS Code-only; now pluggable through the new Type Server Protocol [S14].
   - Debugger side: **debugpy** 1.8.22 [S1] is now the *default* PyCharm debugger as of 2026.2 [S57].
5. **The best precedent for "live + static" completion is Positron.** Its runtime-aware language server (backed by the live kernel) runs alongside Pyrefly, and the results are merged [S24]. We should do the same: an LSP server for static completion, plus in-process live completion (a `jedi.Interpreter`-style or custom `getattr`-chain resolver on the game thread), merged and ranked in the host.
6. **Nils Söderman's VS Code extension is the reference for UE-specific mechanics:**
   - remote execution over UDP multicast and TCP;
   - exec in a persistent globals dict, with the last expression auto-printed;
   - `debugpy.configure(python=unreal.get_interpreter_executable_path())` before `debugpy.listen`.

   Its issue tracker shows that **remote-execution connectivity is the main pain point** [S44][S47][S48][S50], which is a strong argument for an in-process editor.

---

## Part A — PyCharm feature inventory, rated for an in-UE script editor

Ratings: **Must** = v1 blocker, **Should** = v1 if cheap, otherwise v1.x, **Could** = later, **Won't** = out of scope.

| # | PyCharm feature | Rating | One-line reasoning for an in-UE editor | How we'd get it |
|---|---|---|---|---|
| 1 | Code completion (type inference + stub awareness) | **Must** | Discovering the `unreal.*` API is the #1 reason TDs leave the Output Log console. It must understand the generated stub *and* live objects. | LSP `textDocument/completion` on split `.pyi` stubs, merged with live completion (C3) |
| 2 | Parameter info (signature help) | **Must** | UE functions have long parameter lists with UE types (`unreal.Vector`, `unreal.Name`), and TDs live in these signatures. | LSP `signatureHelp`; live fallback from `__doc__`/`inspect` |
| 3 | Quick documentation (hover / Ctrl+Q) | **Must** | UE docstrings are generated from reflection metadata [S12]; showing them inline replaces alt-tabbing to the API docs. | LSP `hover`; live `__doc__` for runtime-only types |
| 4 | Inspections + quick-fixes | **Should** (basic set **Must**) | Undefined names, syntax errors and unused imports are needed on day one. Full type checking is noisy because the stub's hints are approximate [S12][S55]. | LSP diagnostics (basic mode) + ruff diagnostics/fixes [S31] |
| 5 | Intentions (Alt+Enter actions) | **Could** | LSP code actions (add import, add ignore comment) cover most daily needs [S6][S18]. | LSP `codeAction` |
| 6 | Go to definition/declaration | **Must** | Jumping into the user's own modules and into the stub is how people learn the API. | LSP `definition`; stub opens read-only. Opening a 20 MB+ stub hits Monaco's limits [S71], another reason to split it. |
| 7 | Find usages | **Should** | Useful across `Content/Python` and tool repos, but less used in small scripts. | LSP `references` |
| 8 | Structure view / file outline | **Should** | Cheap and helps navigate 1–3k-line tool files. | LSP `documentSymbol` |
| 9 | Rename refactor | **Should** | Safe renames across a tool package; cheap given LSP. | LSP `rename` (basedpyright also renames modules [S6]) |
| 10 | Reformat code | **Should** | Studios standardize on ruff/black; one keystroke, no external tool. | `ruff format` via LSP or stdin [S31][S33] |
| 11 | Optimize imports | **Should** | Scripts pick up stale imports during iteration. | ruff isort rules / organize-imports code action [S31] |
| 12 | Live templates (snippets) | **Should** | UE boilerplate (`ScopedEditorTransaction`, `ScopedSlowTask`, tick-callback registration [S55]) is perfect snippet material. Simple, parameterized and surround types as in PyCharm [S64]. | Editor snippet engine + our UE snippet pack |
| 13 | Multi-caret | **Should** | Expected by modern users; free in Monaco and CodeMirror. | Editor component |
| 14 | Code folding | **Should** | Free in all candidate editors; LSP `foldingRange` is optional. | Editor component (+ ty/basedpyright folding ranges [S18]) |
| 15 | **Local History** | **Must** | DCC scripts are often unsaved or scratch, and the editor can crash. PyCharm keeps continuous revisions (5 working days by default), labels them, and allows partial revert [S60]. This is the safety net TDs actually use. | Snapshot on every Run/Save/autosave into `Saved/`; diff view (Monaco diff editor) |
| 16 | Scratch files | **Must** | Project-independent, runnable scratch files [S63] map directly to Maya/Charcoal-style persistent tabs. | Persistent tab store outside the project, plus "promote to file" |
| 17 | TODO view | **Could** | Nice, but low daily value in scripting. | Regex scan of open and project files |
| 18 | Python console + variable viewer | **Must** | The REPL is the heart of DCC scripting. PyCharm's console has completion, history, execute-selection (Alt+Shift+E) and a variables view [S61]. A variables view that expands UObjects is a differentiator. | Our console widget on the shared session namespace; live completion |
| 19a | Debugger: line breakpoints, step in/over/out, frames, variables | **Must** (for a "PyCharm-like" claim) | This is what separates an IDE from a script box. Technically the hardest part, because the game thread pauses. | Custom in-process debugger + `EnterDebuggingMode` (C4, Recommendation) |
| 19b | Conditional / log ("evaluate and log") / dependent breakpoints | **Should** | PyCharm and DAP both support them [S62][S41]. Logpoints are very useful in tick callbacks where pausing hurts. | Condition, hit count and log-message fields in our breakpoint model |
| 19c | Evaluate expression, watches | **Must** (eval) / **Should** (watches) | You can't debug UE state without poking at it. | `eval` in the paused frame's globals/locals |
| 19d | Inline values | **Could** | Nice polish; needs an editor decoration API. | Monaco inlay hints/decorations |
| 20 | Run configurations | **Could** (a light version is **Should**) | Full run configs are overkill. A "run target" model (selection / cell / buffer / file with args, `__name__=='__main__'`) covers TD needs. | Run menu with remembered per-file settings |
| 21 | Remote debugging via pydevd (`pydevd_pycharm.settrace`) | **Should** (debugpy) / **Could** (pydevd-pycharm) | Many TDs keep PyCharm or VS Code. A one-click "start debug server" for external IDEs is cheap. Since PyCharm 2026.2 defaults to debugpy [S57], debugpy alone serves both IDEs. `pydevd-pycharm` must match the IDE build exactly [S56]. | `debugpy.listen` with Epic's `configure(python=…)` recipe [S53] |
| 22 | VCS integration | **Won't** (git UI) / **Should** (UE source control checkout on save) | Studios use UE's Perforce/Git source control; duplicating a Git UI is waste. Checkout-on-save and gutter diffs are enough. | UE ISourceControl (UE-internals lane) |
| — | *(VS Code-derived)* Run cell `# %%`, run selection, print last expression | **Must** | See Part B; this is the modern run-a-chunk UX TDs expect from Jupyter/VS Code and from Maya's execute-selection. | AST rewrite like [S48] |

---

## Part B — VS Code's Python stack and existing Unreal integrations

### B1. Pylance / pyright
- **Pylance** is Microsoft's closed-source language server for VS Code. It is built on pyright and adds features. Its license limits it to Microsoft VS Code products, so it can't be embedded (UNVERIFIED this session; widely known). basedpyright re-implements most Pylance-only features in open source: semantic highlighting, inlay hints, docstrings for compiled builtins, auto-import code actions, module rename, go-to-implementation [S6].
- **New (Oct 21 2026): the Type Server Protocol (TSP).** It splits Pylance's editor features from the type-checking backend, so Pylance can run on top of Pyrefly. Pyright stays the default [S14][S15]. Pyright now ships a standalone TSP server (`pyright-typeserver` on npm) [S7]. This shows the ecosystem is decoupling "editor intelligence" from "type evaluation", but it doesn't change our choice: we talk plain LSP.
- **Pyright** is at 1.1.414 (Sep 2026), with ongoing tokenizer and memory work and fixes for OOM on large import graphs [S7].

### B2. debugpy + DAP in VS Code
- **debugpy** 1.8.22 (2026-09-15), MIT, Python ≥3.10 [S1]. It implements DAP, so VS Code, PyCharm 2026.2 (default backend [S57][S58]) and any DAP client can drive it.
- Features available through DAP: conditional breakpoints, hit counts, logpoints, exception breakpoints, `justMyCode`, attach by `connect` or `listen`, and remote attach [S43][S41].
- PyCharm 2026.1 blog: debugpy uses PEP 669 (`sys.monitoring`) on 3.12+ to cut overhead [S59]. That doesn't help us on UE's 3.11. On 3.11, pydevd's PEP 523 frame-eval acceleration is reported as unsupported (3.11 changed frame internals) [S81][S82], so debugpy falls back to `settrace` tracing (UNVERIFIED for the current release).

### B3. `# %%` cells and the Interactive window
- VS Code treats `# %%` comments in a `.py` file as cell markers. Each cell gets **Run Cell / Debug Cell** code lenses, plus **Run Above/Below**. Shift+Enter runs the cell and moves to the next one, creating a new cell at the end. Output goes to an Interactive window with a variables explorer and a data viewer [S42].
- It needs a Jupyter server/kernel [S42]. For us the "kernel" is the UE session itself. Copy the UX, not the transport.
- **Design notes for us:**
  - compile each cell with its real file line offset (pad or `ast.increment_lineno`) so tracebacks and breakpoints map back to the buffer;
  - register unsaved buffers in `linecache` so traceback source lines show;
  - auto-print the last expression with an AST rewrite, the way Nils' extension does: assign to `_`, print if not `None` [S48].

### B4. Notebooks (`.ipynb`)
- **Won't (v1).** Cells inside `.py` files give about 80% of the value without a notebook document model. ty and basedpyright already support notebooks if we ever need them [S6][S18].

### B5. Nils Söderman's "Unreal Engine Python" VS Code extension (`NilsSoderman.ue-python`)
- **Status:** v1.11.0 (2026-06-22). The GitHub repo is **archived** and development moved to **Codeberg** [S44][S45][S46].
- **Features:** Execute (selection or document, Ctrl+Enter), Setup Code Completion, Attach Debugger, a Documentation browser generated from the *running* instance (so it includes project C++ and BP-exposed types) [S44].
- **How execution works internally:**
  - **Transport.** Epic's Remote Execution protocol (`remote_execution.py` in the plugin).
    - UDP multicast discovery: `ping`/`pong`, `open_connection`/`close_connection` on `239.0.0.1:6766`, TTL 0 = localhost only.
    - A **TCP command channel hosted by the client** (UE connects back to `127.0.0.1:6776`).
    - Messages `command`/`command_result`.
    - Three modes: `ExecuteFile`, `ExecuteStatement`, `EvaluateStatement` [S53].
    - The extension uses its own TypeScript port, `unreal-remote-execution` [S51].
  - **Execution harness** (`vscode_unreal/execute.py`) [S48]:
    - reads the code from a temp file;
    - sets `__file__` to the original path, plus `__name__` and `__package__` (inferred from `sys.path`);
    - runs `ast.parse` and rewrites the last expression to print it;
    - runs `exec(compile(...), G)`, where `G` is a **persistent dict** (`__VsCodeVariables__`), not `__main__`;
    - formats tracebacks itself: skips harness frames, uses the original source lines, keeps 3.11 column info;
    - while debugging, temporarily redirects `unreal.log*` to `print` so the debug console sees it.
  - **Output** goes back over the socket since 1.0 [S46].
- **How debugging works** [S47][S49]:
  - checks whether debugpy is importable, and offers to pip-install it into UE's Python;
  - picks a free port (avoiding the remote-execution command port; `strictPort` option);
  - calls `debugpy.configure(python=unreal.get_interpreter_executable_path())`, then `debugpy.listen(port)`;
  - stores the port in an env var so later attaches reuse the running server;
  - VS Code then attaches with `{"request":"attach","connect":{"host":"localhost","port":N}, justMyCode…}`.
  - Epic's own `debugpy_unreal.py` in 5.8 does the same. Its comment explains why: without `configure(python=…)`, debugpy "tries to run another UE instance as an adapter" (because `sys.executable` is UnrealEditor.exe). It also warns that **`redirectOutput` must be true** or both debugger and editor hang [S53][S55].
- **How completion setup works:**
  - adds `<Project>/Intermediate/PythonStub` to `python.analysis.extraPaths`;
  - since 1.10, also supports basedpyright and ty;
  - **since 1.11, copies `unreal.py` → `unreal.pyi` on connect "to improve pyright type checking and completion"** [S46].
- **Known limitations, from its issue tracker** [S50]:
  - The largest cluster is **connection failures/timeouts**: multicast settings, breakage after UE upgrades (5.3, 5.4, 5.6), WSL path issues.
  - Others: output not appearing; GBK decode errors with non-ASCII code; modal dialogs blocking execution (hence the `-unattended` option); relative imports only within `sys.path`; module changes need explicit reload; "can't step after attach" (#50).
  - Stub quality issues: a property typed as a function in 5.7 (#66), missing per-method docs (#55).
  - Linux needs a manual multicast route [S51].

### B6. PyCharm + Unreal
- **Completion:**
  - Epic's 5.8 docs: enable Developer Mode; the stub goes to `PROJECT/Intermediate/PythonStub` and is regenerated on every editor start;
  - add it to the interpreter paths;
  - set `idea.max.intellisense.filesize = 25000` [S12]. The default is 2500 KB, and JetBrains warns that large files cost memory and speed [S13].
  - Users report PyCharm low-memory warnings with the 21 MB stub, and the community "DisUnrealStub" tool splits it per class [S11].
- **Type-hint mode:** the plugin setting `ETypeHintingMode` offers `AutoCompletion` (readable signatures, assumes UObjects are never None) or `TypeChecker` (closer to real coercions, UObjects marked possibly-None) [S53]. Epic notes `TypeChecker` makes the completion menu busy [S12].
- **Remote debug (classic):**
  - Python Debug Server run config; `pip install pydevd-pycharm~=<IDE build>` (must match the IDE build);
  - `pydevd_pycharm.settrace(host, port, stdout_to_server=True, stderr_to_server=True)`;
  - path mappings [S56].
  - Edition requirement (historically Professional only): UNVERIFIED for 2026.
- **Modern:** PyCharm 2026.1 added debugpy as an option, and 2026.2 made it the default for Python 3.9+ local/WSL interpreters. pydevd remains selectable [S57][S58]. So in 2026 the same `debugpy.listen` server can serve PyCharm and VS Code users (attach-to-debugpy flow in PyCharm: UNVERIFIED in detail).

### B7. MayaCharm (Maya analogue)
- PyCharm plugin that executes documents/selections in Maya over a configured port (Maya-side code is generated for `userSetup.py`). Maya shows up in PyCharm's **Attach to Process** list for debugging. Debugging through a run configuration was **removed as unreliable** [S66].
- Lesson: attach-to-process (pydevd code injection) is more robust than settrace-from-a-run-config in hosts. For UE we can be more robust still, because we *are* in-process.

### B8. Debugging Python on Unreal's game thread
- UE Python and all `unreal.*` API calls run on the game thread (UObjects are not thread-safe; tick work is spread with Slate post-tick callbacks [S55]).
- **When the debugger suspends, the game thread blocks, so the whole editor (viewport and Slate UI) freezes until Continue.** It works with VS Code/PyCharm only because the debugger UI lives in another process.
- Even `debugpy_unreal.start()` freezes the editor until a client attaches, because it calls `wait_for_client()` [S53][S54]. debugpy's `breakpoint()` pauses all threads [S2].
- The UE plugin takes the GIL per call (`PyGILState_Ensure` in `FPyScopedGIL`) [S53]. This implies the GIL is free while the editor is idle, which is why debugpy's background threads, and our own worker threads, can run between script executions. UNVERIFIED as a guarantee; consistent with debugpy attach working [S47].
- For an **in-process** debugger UI, UE 5.8 exposes **`FSlateApplication::EnterDebuggingMode()` / `LeaveDebuggingMode(bool bLeavingDebugForSingleStep)`**. The header describes it as making "the Slate application to tick in place … in the middle of a stack frame", and it sits beside `InKismetDebuggingMode()` (`GIntraFrameDebuggingGameThread`) [S53]. The Blueprint debugger uses this to keep the editor interactive while a BP is paused mid-frame. A Python debugger can call it from its stop handler. The UE-internals lane must confirm what keeps ticking in this mode (world tick is suspended; Slate and Python Slate-tick callbacks may still run).

---

## Part C — Embeddable building blocks and protocols

### C1. Editor components

| Component | License | Size / footprint | Large-file behaviour (10k+ lines) | LSP story | IME / accessibility | Embedding requirements in UE |
|---|---|---|---|---|---|---|
| **Monaco** (VS Code editor core) | MIT [S69] | Large: full ESM build is several MB minified (UNVERIFIED figure) | 10k–100k lines are fine (piece tree, virtualized). **Tokenization disabled above 20 MB or 300k lines**; 256M-character heap guard [S71] | No built-in LSP. **monaco-languageclient 11.0.3** (2026-10-15, MIT; aligned with monaco-editor 0.57 / VS Code 1.138; WebSocket or worker transports; has a pyright example) [S70]; or hand-map Monaco providers to LSP | Screen-reader mode and integrator accessibility guide [S69]; IME through a hidden textarea (fine in Chromium). Not supported on mobile [S69] | Needs a Chromium webview: UE 5.8 ships **CEF 128 / Chromium 128.0.6613.138** [S53]. Workers can't start from `file://` [S69], so serve assets through `IWebBrowserSingleton::RegisterSchemeHandlerFactory` [S53]. Bridge with `BindUObject` / `ExecuteJavascript` [S53]. IME: `SWebBrowserView::BindInputMethodSystem` exists [S53]; the UMG Web Browser widget had IME disabled (UE-75056) [S79]; CEF off-screen-rendering IME candidate placement is a known issue [S80] |
| **CodeMirror 6** | MIT [S72] | Modular, much smaller than Monaco (UNVERIFIED figure) | Viewport rendering. Official demo with **millions of lines**; the parser limits work and catches up over time [S73]. "Responsive on huge documents and long lines" [S72] | **`@codemirror/lsp-client` 6.2.2** (2026-03-02): completion, hover, signature help, diagnostics, formatting, rename, definition/declaration/type-definition/implementation, references. **A user-supplied `Transport`** (send JSON / receive JSON) maps neatly onto a host bridge [S74] | Best a11y story of the web editors ("works well with screen readers and keyboard-only users"; native selection, bidi) [S72]; contenteditable-based IME | Same CEF requirements as Monaco, but fewer worker needs. Note: the GitHub repo is archived and moved to Marijn's own forge [S74] |
| **Scintilla** (+ Lexilla) | HPND-style permissive [S77] | Small native C++ | Excellent: SciTE-class performance; >2 GB documents with `SC_DOCUMENTOPTION_TEXT_LARGE`; idle styling for very large files [S76] | None. Has autocompletion lists and call tips that we feed ourselves [S76] | Inline IME mode exists (`SCI_SETIMEINTERACTION`, from memory, UNVERIFIED); accessibility API limited by platform (UNVERIFIED) | No Slate platform layer. We'd **port Scintilla's platform abstraction** (Surface drawing, fonts, ListBox, Window, Menu) to Slate. Win32/GTK/Cocoa/Qt layers exist [S76]. Big but bounded job; keeps UE look and feel and avoids CEF |
| **QScintilla** | **GPL v3 or commercial** [S78] | Native + Qt | Same as Scintilla | None | Qt's | Needs Qt (UE doesn't ship it) and a commercial license for proprietary distribution. **Reject.** PyPI still shows 2.14.1 (2023) [S78] |
| **Ace** | BSD [S75] | Medium | Claims about 4M lines [S75] | `ace-linters` "language client" [S75] | Not documented on the site | CEF, like the others. Less momentum than Monaco/CM6 |
| *(Native Slate `SMultiLineEditableText` + syntax marshaller)* | Engine | n/a | UE-internals lane to measure | We'd build all of it | UE text input system handles IME | No CEF. We build multi-caret, folding, popups and virtualization ourselves |

**Takeaways:**
- **Monaco** gives the most PyCharm/VS Code-like UX for free: multi-cursor, folding, find/replace, snippet engine, inlay hints, semantic tokens, peek, sticky scroll, and a diff editor for Local History.
- **CodeMirror 6** is the lighter, more accessible choice with a cleaner LSP transport.
- **Scintilla-on-Slate** is the "no CEF" native option.
- Viewing the unsplit `unreal.py` breaks Monaco's highlighting (over 300k lines) [S71][S11]. That's one more reason to split stubs.

### C2. Language intelligence (LSP servers)

| Server | Version / status (Oct 2026) | License | Runtime & shipping | Startup / first completion (Positron's benchmark [S23]) | Notes for UE |
|---|---|---|---|---|---|
| **basedpyright** | 1.40.2 (2026-10-04) [S3] | MIT | PyPI wheel; the only runtime dependency is **`nodejs-wheel-binaries>=20.13.1`** (bundled Node) [S4]; command **`basedpyright-langserver`** [S5] | 3.1 s / 112 ms | Pyright-grade inference plus Pylance features [S6]. Noisy by default [S23]. **Inherits pyright's 50 MB file limit [S8] and complexity limit [S9]** |
| **pyright** | 1.1.414 [S7] | MIT | npm (Node) | n/a | Same limits; fewer editor features than basedpyright |
| **Pylance** | VS Code only | Proprietary | — | — | Not embeddable; TSP now lets it use Pyrefly [S14] |
| **Pyrefly** (Meta) | **1.0 stable 2026-05-12**; 1.4.0 (2026-10-15) [S20][S21][S22] | MIT [S21] | Rust binary in a 15.7 MB win_amd64 wheel [S21]; LSP command `pyrefly lsp` (UNVERIFIED) | 5.8 s / 190 ms | Best hover docs (Posit's pick) [S23]; incremental updates up to 125× faster [S20]. No UE stub data |
| **ty** (Astral) | **Beta**, 0.0.94 (2026-10-28) [S17]; "no stable API, breaking changes between versions" [S16] | MIT | Rust binary, 14.0 MB wheel [S17]; **`ty server`** [S19] | **2.2 s / 88 ms** (fastest) | Full LSP: completions + auto-import, hover, signature help, navigation, references, rename, inlay hints, semantic tokens, folding, call/type hierarchy; **no formatting (use ruff)** [S18]. Astral's acquisition by OpenAI was reported (UNVERIFIED) [S84] |
| **ruff server** | ruff 0.17.3 (2026-10-27) [S32]; server stable since 0.5.3 [S31] | MIT | Single Rust binary, 10.3 MB wheel [S32]; `ruff server` | n/a | Diagnostics, quick fixes, formatting; replaces ruff-lsp [S31]. Not a completion server |
| **jedi-language-server** | **Maintenance mode**; README points to Zuban [S25] | MIT | Pure Python (jedi) | n/a | Diagnostics only from `compile()` [S25]; Jedi crops files at 10 MB and "gets extremely slow" past a few thousand lines [S28]. **Useless for the unsplit stub** |
| **python-lsp-server** | 1.15.0 (2026-07-27) [S29] | MIT | Pure Python; Jedi plus plugins (rope, pyflakes, `python-lsp-ruff`, `pylsp-mypy`) [S29] | n/a | Same Jedi limits; good if you want the LSP *inside* UE's interpreter (the risk being blocking the GIL) |
| **Zuban** | 0.10.1 (2026-10-11), "1 – Planning" [S30] | **AGPL-3.0** [S30] | Rust | n/a / 97 ms | AGPL makes it a poor fit for a distributed plugin. **Avoid** |

**The `unreal.py` problem: hard numbers.**

| Limit | Value | Source |
|---|---|---|
| Stub size, UE 5.1 | >21 MB, >400k lines, grows with enabled plugins | [S11] |
| Stub size reported 2025 | ~24 MB | [S9] |
| Stub size, plugin-heavy project | 52,986,705 bytes | [S10] |
| Pyright family `maxSourceFileSize` | 50 × 1024 × 1024 bytes (matches VS Code's own limit); file silently skipped | [S8][S10] |
| Pyright code-flow complexity | `maxCodeComplexity = 768`; Pylance maintainers: stub "too complex for Pyright to analyze", won't be configurable, Epic should split it | [S9] |
| Jedi | crops at 10 MB | [S28] |
| PyCharm default code-assist limit | 2500 KB; Epic recommends 25000 | [S12][S13] |
| Monaco | no tokenization over 20 MB / 300k lines | [S71] |

- Epic acknowledges the size problem (UE-119549, open since 2021, "one person assigned to minimum maintenance") [S10].
- **Mitigation (recommended):** at editor start, after Epic writes `Intermediate/PythonStub/unreal.py`, our plugin post-processes it into a package `unreal/__init__.pyi` with **explicit re-exports** (`from ._x import Foo as Foo`) and submodules split by class, module or plugin, preserving base-class order. The community `DisUnrealStub` splits per class [S11].
- Store the output in a plugin cache dir and point the LSP at it (`extraPaths`/`stubPath`). Keep `.pyi` extensions [S46]. Exclude `Intermediate/` from workspace indexing.
- A better long-term option is to generate the stubs from reflection ourselves. Note that `PyGenUtilAccessor` is "not available for non-Engine use" [S53]; this is for the UE-internals lane.
- **Benchmark (open question):** basedpyright vs Pyrefly vs ty on the split and unsplit 5.8 stub. Measure cold start, RSS, and latency for `unreal.` and `unreal.EditorAssetLibrary.` completion. There is no published data for ty or Pyrefly on UE stubs.

**Shipping:**
- All recommended servers are MIT and run as **child processes over stdio JSON-RPC**, owned by the C++ plugin. Spawn them with `FPlatformProcess::CreateProc` and pipes, then restart on crash.
- Rust servers are single native binaries in about 10–16 MB wheels [S17][S21][S32]. basedpyright needs the bundled Node.
- Ship them as plugin `ThirdParty` binaries rather than pip-installing them into UE's Python. That keeps LSP CPU and memory out of the editor process and out of the GIL.

### C3. Live introspection completion (how DCC REPLs complete live objects)

- **`jedi.Interpreter(code, namespaces=[globals_dict, ...])`** combines static analysis with real runtime objects. It is Jedi's REPL API, with the same methods as `Script`: `complete`, `get_signatures`, `infer`, `goto`, `help` [S27].
  - `jedi.settings.allow_unsafe_interpreter_executions` (default **True**) lets Jedi evaluate descriptors such as `property` and `__getitem__` on live objects [S28]. **We should default it to False.**
  - Jedi 0.20.1 (2026-10-12) supports Python 3.10–3.14, so it runs in UE's 3.11 [S26].
  - Compiled modules like `unreal` are handled by runtime introspection, not by the 10 MB-cropped stub.
- **IPython `IPCompleter`** uses an evaluation policy, `Completer.evaluation`:
  - `forbidden`: no evaluation;
  - `minimal`: literals and built-ins only;
  - `limited` (**default**): namespaces plus allow-listed pure methods (`dict.keys()`, and so on);
  - `unsafe`: all calls except syntax with side effects;
  - `dangerous`: arbitrary evaluation.

  Its matcher API v2 merges sources by priority, and a matcher can `suppress` lower-priority ones. Jedi is optional (`use_jedi`, with `jedi_compute_type_timeout`) [S35]. This is a ready-made model for "safe live evaluation + multi-source merge".
- **`rlcompleter`** evaluates the dotted expression up to the last dot without calling functions, but attribute access can run `__getattr__`. It then lists `dir()` matches and silences exceptions [S36]. It's simple but too naive (no signatures, no types).
- **UE constraints:**
  - live resolution touches UObjects, so it **must run on the game thread** inside a small time budget (for example 5–10 ms during Slate idle);
  - static inference runs out of process (LSP);
  - never call functions during completion;
  - allow subscripting only for literal indices on `list`/`tuple`/`unreal.Array`/`dict` (IPython's "limited" idea);
  - catch everything.
- **Merge design (recommended):**
  1. Request both sources in parallel: the LSP completion (async, about 100 ms) and the live completion. Live completion is requested when the root name of the dotted expression exists in the session namespace, which is always true in the console.
  2. Key results by label. If both sources have an item, keep the static item's signature and docs and enrich it with the live *concrete* type. Example: static says `Actor`, live says `StaticMeshActor`, so you also show `StaticMeshActor` members, flagged "runtime".
  3. Live-only items appear with a "runtime" badge. These cover Blueprint-generated classes, objects created this session, and C++ classes added since the stub was last generated (the stub only regenerates on editor restart [S12]).
  4. Ranking: locals/params, then members of a live-resolved object, then static members, then keywords/snippets. In the console, live comes first; in files, static comes first.
- **Precedent:** Positron runs a runtime-aware language server (live session: DataFrame columns, dict keys) **concurrently** with Pyrefly and merges the results [S24].

### C4. Debugging building blocks

**DAP basics** [S41]:
- Wire format: `Content-Length` header + JSON; requests, responses and events.
- Startup: `initialize` → `initialized` event → `setBreakpoints` (per source) / `setExceptionBreakpoints` → `configurationDone` → `launch`/`attach`.
- On a stop: a `stopped` event, then `threads` → `stackTrace` → `scopes` → `variables` (lazy, by reference, valid only while suspended) and `evaluate`/`setVariable`.
- **Our internal debugger API should mirror this shape** so the UI can be backed by either our in-process engine or a debugpy session.

**debugpy embedding** [S2][S53][S67]:
- `debugpy.configure(...)` must be called *before* `listen`/`connect`. In embedded hosts, pass `python=<real python.exe>`, because `sys.executable` is the host. UE provides `unreal.get_interpreter_executable_path()` [S53]. This is a known pain point for embedded apps [S67].
- `debugpy.listen((host, port))` starts a debug server and by default **spawns a separate adapter process**. `in_process_debug_adapter=True` avoids the subprocess, at the cost of subprocess debugging. Only one client at a time.
- `wait_for_client()` **blocks** (freezes UE), so don't call it on the game thread in our flow. `debug_this_thread()` is for natively created threads; `trace_this_thread(False)` is for hot threads. With no client connected, tracing is off [S2].
- debugpy, pydevd and our custom debugger all install `sys.settrace` on 3.11, so **only one can be active**. We must detect a running debugpy and disable ours, or the other way round.

**pydevd / pydevd-pycharm:**
- `settrace()` connects out to the IDE's debug server, with version pinning to the IDE build [S56].
- Attach-to-process injects code into a running process. MayaCharm relies on it [S66]. Working against UnrealEditor.exe is UNVERIFIED.

**`sys.settrace` vs `sys.monitoring`:**
- **`sys.settrace` / `bdb` (all versions; the only option in UE 5.8):**
  - per-thread trace function; frame-local tracing.
  - `bdb` already avoids line events for frames whose file has no breakpoints (`dispatch_call` returns `None` unless `stop_here` or `break_anywhere`). Overhead is then mostly one call per function call.
  - Install the global trace only while a debug session has breakpoints or is stepping, so there is **zero overhead when not debugging**.
  - On 3.11, changing a local while paused needs the `PyFrame_LocalsToFast` ctypes trick (fixed by PEP 667 in 3.13 [S39]).
  - Large slowdowns from settrace-based debugging are inherent [S68].
- **`sys.monitoring` (3.12+):**
  - tool IDs (`DEBUGGER_ID`=0);
  - per-code-object local events (`LINE`, `PY_START`, …);
  - returning `DISABLE` turns off individual locations, giving near-zero overhead outside breakpoints;
  - `PY_START`/`PY_RESUME` help find code objects to instrument when attaching;
  - changing event sets is slow, so do it rarely.
  - It's incompatible with `settrace`-based tools running at the same time [S37].
  - **Use it when UE moves past 3.11.**
- **Python 3.13/3.14 changes worth tracking** (none available in UE 5.8):
  - PEP 667: write-through `f_locals`, reliable variable editing [S39].
  - 3.14 PEP 768: `sys.remote_exec(pid, script)`, which injects at eval-breaker safe points on the main thread, waits for the GIL, and can be disabled with `PYTHON_DISABLE_REMOTE_DEBUG` / `-X disable-remote-debug` [S38]. Plus `python -m pdb -p PID` and `python -m asyncio ps/pstree` [S40].
  - Free-threaded builds are officially supported in 3.14 [S40]. This matters only if UE ever adopts them; the `unreal` API would still be game-thread bound.

### C5. Formatting and linting

- **ruff** (lint + format, single Rust binary, MIT, 0.17.3) [S32]. On unsaved buffers through stdin [S33]:
  - Format: `ruff format --stdin-filename <path> -` reads stdin and prints the formatted code. `--range <l:c-l:c>` formats a selection. Pass `--config` for project settings.
  - Lint: `ruff check --stdin-filename <path> --output-format json -` returns machine-readable diagnostics. `ruff check --fix --stdin-filename <path> -` prints the fixed source to stdout (stdin mode writes content back to stdout [S33]).
  - Or simply run **`ruff server`** as a second LSP for diagnostics, quick fixes and formatting [S31]. **Preferred**, since we already have an LSP client.
- **black** 26.10.1, MIT, Python ≥3.10 [S34]. `black -q --stdin-filename <path> -` works the same way (exact syntax UNVERIFIED this session). Offer it only as an alternative formatter for studios standardized on black. ruff's formatter is black-compatible by design (UNVERIFIED wording).

---

## Recommendation

**1. Editor component: Monaco in UE's `SWebBrowser`/`SWebBrowserView` (CEF 128 in 5.8), with CodeMirror 6 as the pre-agreed fallback.**
- **Why Monaco:** it gives the most PyCharm/VS Code-like editing for free: multi-caret, folding, find/replace, snippet engine for live templates, inlay hints, semantic tokens, peek, and a **diff editor for Local History**. Our files are small (scripts of 100–5,000 lines), well under its 20 MB / 300k-line tokenization cut-off [S71].
- **How:**
  - serve assets through a registered scheme handler, because `file://` blocks workers [S69][S53];
  - bridge with `BindUObject`/`ExecuteJavascript` [S53];
  - wire IME with `BindInputMethodSystem` [S53];
  - keep LSP processes in C++ and connect them to Monaco through a **thin custom provider adapter**. That's smaller than pulling in `@codingame/monaco-vscode-api` through monaco-languageclient [S70]; spike both.
- **Gate:** a one-week spike must pass CJK IME, screen reader, keyboard focus/shortcut coexistence with the UE editor, input latency, and memory. If it fails, switch to **CodeMirror 6** (better a11y/IME, tiny `Transport`-based LSP client [S72][S74]). If CEF itself is rejected by the UE-internals lane, use **Scintilla ported to Slate** (permissive license, native performance) [S76][S77]. **Do not use QScintilla** (GPL/Qt) [S78].

**2. Language servers (out of process, stdio, plugin-shipped):**
- **Static types/completion:** **basedpyright** as v1 default. It is the most mature pyright-grade inference, open-source Pylance features [S6], MIT, and installs as a wheel with bundled Node [S4]. The most UE users already rely on pyright/Pylance behaviour.
- **Make the server pluggable.** Offer **Pyrefly** (stable 1.x, best hover, Positron's choice [S20][S23]) and **ty** (fastest; beta [S16][S23]) behind the same client, and choose the long-term default from the stub benchmark.
- **Lint/format:** **ruff server** [S31].
- **Stubs:** our plugin generates a **split `.pyi` package** of the UE API after each editor start, to stay under the 50 MB / complexity-768 / 10 MB / 300k-line ceilings [S8][S9][S28][S71].

**3. Completion: live + static merge (C3).**
- LSP static completion in all buffers.
- In-process live completion on the game thread with a 5–10 ms budget: our own safe resolver (name lookups and attribute access only, IPython "limited"-style policy), optionally `jedi.Interpreter` with `allow_unsafe_interpreter_executions=False` for signatures.
- Merge and rank in the host, with "runtime" badges. The console defaults to live-first.

**4. Debugger: two tiers, one DAP-shaped model.**
- **Tier 1, in-editor (v1):**
  - A custom `bdb`-based debugger in UE's interpreter: line, conditional, hit-count and log breakpoints; exception breakpoints; step in/over/out; frames; lazy UObject-aware variables; evaluate and watches.
  - On stop it calls a C++ hook that enters **`FSlateApplication::EnterDebuggingMode()`**, so our Slate debugger panel stays interactive. Continue/step calls `LeaveDebuggingMode()` [S53].
  - The backend abstraction swaps to `sys.monitoring` when UE ships Python ≥3.12 [S37].
  - Global tracing is installed only while a session has breakpoints, so there is zero cost otherwise.
- **Tier 2, external IDE:** a "Start debug server" command using Epic's recipe: `debugpy.configure(python=unreal.get_interpreter_executable_path())`, then `debugpy.listen(("127.0.0.1", port))`, never `wait_for_client` on the game thread, `redirectOutput` true [S53]. This serves VS Code and PyCharm 2026.2 (debugpy default) [S57]. It is mutually exclusive with Tier 1.
- **Why not "just embed debugpy" for the in-editor UI:** debugpy's suspend loop runs inside pydevd on the frozen game thread, so we can't tick Slate from it without fragile monkey-patching. A small `bdb` engine that we control is simpler to make editor-friendly.

**5. Execution UX (from B3/B5):**
- Run selection, cell (`# %%`), buffer and file, in one **persistent session namespace**.
- Auto-print the last expression [S48].
- Register buffers in `linecache` and preserve real line numbers.
- Our own traceback formatting.
- No remote-execution sockets for the in-editor path. That removes the main failure class of the VS Code extension [S50].

---

## Risks

1. **Stub limits:** pyright family 50 MB hard limit and 768 complexity limit; Jedi crops at 10 MB; Monaco stops tokenizing at 300k lines. If we don't split stubs, static completion silently degrades [S8][S9][S10][S28][S71]. The stub's own type quality is imperfect (wrong property types, None-ability tradeoffs) [S12][S50][S53].
2. **Python 3.11 in UE 5.8:**
   - no `sys.monitoring`, so settrace overhead while debugging;
   - variable editing needs the ctypes `PyFrame_LocalsToFast` hack;
   - no PEP 768.
   - An Epic Python upgrade (timing unknown [S52]) will change debugger internals, so build the backend abstraction now.
3. **Nested Slate loop while paused:** Python Slate-tick callbacks, timers or editor commands could re-enter Python while a frame is suspended. Breakpoints *inside* Slate tick callbacks would nest debugging mode inside a Slate tick. GC and transactions may misbehave. Mitigation: block all Python execution entry points while paused, and test Kismet-mode side effects (UE-internals lane).
4. **CEF webview:** IME has historically been broken in UE web widgets [S79][S80]; also focus and shortcut conflicts, extra processes and memory, and asset serving through scheme handlers. Mitigated by the spike gate and the CodeMirror/Scintilla fallbacks.
5. **Live completion side effects:** attribute access calls C++ getters or `__getattr__`, and user properties run arbitrary code [S35][S36]. It must run on the game thread with strict budgets.
6. **Toolchain churn:**
   - ty is 0.0.x with breaking changes [S16], and Astral's ownership change is reported [S84];
   - Pyrefly is moving fast (1.0 → 1.4 in five months) [S22];
   - Pylance/TSP is reshaping the space [S14];
   - jedi-language-server is in maintenance [S25].

   Pin versions per plugin release and keep the server pluggable.
7. **Shipping basedpyright's Node:** bundle size and corporate AV/allow-list friction for a `node.exe` inside a UE plugin (UNVERIFIED; check with pilot studios). The Rust servers avoid this.
8. **Tracer conflicts:** debugpy, pydevd, coverage tools and our debugger all want `sys.settrace`. We must detect them and refuse cleanly.
9. **Licensing:** avoid GPL/AGPL (QScintilla [S78], Zuban [S30]); Pylance can't be used outside VS Code (UNVERIFIED wording). Everything recommended is MIT/BSD/HPND.

## Open questions

1. What is the real 5.8 stub size for a typical production project (with Fab plugins), and how do basedpyright, Pyrefly and ty perform on split vs unsplit stubs (cold start, RSS, `unreal.` completion latency)?
2. Can our plugin generate stubs from reflection itself (better types, per-module output), given `PyGenUtilAccessor` is engine-only [S53]? Or should we post-process Epic's file? (UE-internals lane.)
3. Exactly what does `EnterDebuggingMode()` keep ticking (Slate only? Python post-tick callbacks? asset registry?), and is it safe to enter from a Python trace callback, including one called from inside a Slate tick? (UE-internals lane; prototype early.)
4. Does `SWebBrowserView::BindInputMethodSystem` give a correct CJK IME experience with Monaco/CodeMirror in CEF 128 off-screen rendering?
5. Run LSP servers outside UE (chosen), or optionally in UE's interpreter (python-lsp-server/Jedi) for access to the live namespace? This document assumes outside, with live data via our own completer.
6. Will Epic's next Python upgrade (3.13?) land in 5.9? It decides when to build the `sys.monitoring` backend [S52].
7. Is the PyCharm "Python Debug Server" still Professional-only in the 2026 unified PyCharm? This affects how we document the external-IDE path.

---

## Sources

- [S1] debugpy on PyPI (1.8.22, 2026-09-15; MIT; Python ≥3.10) — https://pypi.org/project/debugpy/
- [S2] debugpy API reference (listen/connect/configure/wait_for_client/breakpoint/in_process_debug_adapter/debug_this_thread) — https://github.com/microsoft/debugpy/wiki/API-Reference
- [S3] basedpyright on PyPI (1.40.2, 2026-10-04) — https://pypi.org/project/basedpyright/
- [S4] basedpyright `pyproject.toml` (runtime dependency `nodejs-wheel-binaries>=20.13.1`) — https://github.com/DetachHead/basedpyright/blob/main/pyproject.toml
- [S5] basedpyright CLI and language server install docs (`basedpyright-langserver`) — https://docs.basedpyright.com/latest/installation/command-line-and-language-server/
- [S6] basedpyright: Pylance features re-implemented — https://docs.basedpyright.com/latest/benefits-over-pyright/pylance-features/
- [S7] Pyright releases (1.1.414; TSP server, `pyright-typeserver`) — https://github.com/microsoft/pyright/releases
- [S8] Pyright `sourceFile.ts` (`maxSourceFileSize = 50 * 1024 * 1024`) — https://github.com/microsoft/pyright/blob/main/packages/pyright-internal/src/analyzer/sourceFile.ts
- [S9] pylance-release #7274 "Unreal PythonStub autocomplete and typing support regression" (24 MB stub, `maxCodeComplexity = 768`, by design) — https://github.com/microsoft/pylance-release/issues/7274
- [S10] Epic forum: "unreal.py stubs file may become too big for Pylance / VSCode" (52,986,705 bytes; UE-119549) — https://forums.unrealengine.com/t/unreal-py-stubs-file-may-become-too-big-for-pylance-vscode/2684691
- [S11] TAColor: "Split unreal.py for a better development experience" (UE 5.1: >21 MB, >400k lines) — https://www.tacolor.xyz/TipsOfDay/Split_unreal_py_for_a_better_development_experience.html
- [S12] Epic docs (UE 5.8): Setting up Autocomplete for Unreal Editor Python Scripting — https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-autocomplete-for-unreal-editor-python-scripting
- [S13] JetBrains platform properties (`idea.max.intellisense.filesize` default 2500 KB) — https://www.jetbrains.com/help/pycharm/2021.2/file-idea-properties.html
- [S14] Microsoft Python blog: Pylance + Pyrefly, Type Server Protocol — https://devblogs.microsoft.com/python/announcing-tsp/
- [S15] Visual Studio Magazine on TSP (2026-10-27) — https://visualstudiomagazine.com/articles/2026/10/27/microsoft-and-meta-bring-pyrefly-type-checking-to-pylance.aspx
- [S16] ty README (beta, MIT, LSP features) — https://github.com/astral-sh/ty
- [S17] ty on PyPI (0.0.94, 2026-10-28; 14.0 MB wheel; beta) — https://pypi.org/project/ty/
- [S18] ty language server features — https://docs.astral.sh/ty/features/language-server/
- [S19] ty editor integration (`ty server`; PyCharm 2025.3+ native) — https://docs.astral.sh/ty/editors/
- [S20] Pyrefly v1.0 announcement (2026-05-12) — https://pyrefly.org/blog/v1.0/
- [S21] Pyrefly on PyPI (1.4.0, MIT, 15.7 MB wheel) — https://pypi.org/project/pyrefly/
- [S22] Pyrefly releases — https://github.com/facebook/pyrefly/releases
- [S23] Posit: "How we chose Positron's Python type checker" (2026-03-31; startup / first-completion table) — https://opensource.posit.co/blog/2026-03-31_python-type-checkers/
- [S24] Positron Python guide (runtime-aware server + Pyrefly, merged) — https://positron.posit.co/guide-python.html
- [S25] jedi-language-server README (maintenance mode, recommends Zuban) — https://github.com/pappasam/jedi-language-server
- [S26] Jedi on PyPI (0.20.1, 2026-10-12) — https://pypi.org/project/jedi/
- [S27] Jedi API docs (`Interpreter`) — https://jedi.readthedocs.io/en/latest/docs/api.html
- [S28] Jedi `settings.py` (`allow_unsafe_interpreter_executions`, `_cropped_file_size = 10e6`) — https://github.com/davidhalter/jedi/blob/master/jedi/settings.py
- [S29] python-lsp-server on PyPI (1.15.0, 2026-07-27) — https://pypi.org/project/python-lsp-server/
- [S30] Zuban on PyPI (0.10.1; AGPL-3.0-only) — https://pypi.org/project/zuban/
- [S31] Ruff editor integration / `ruff server` — https://docs.astral.sh/ruff/editors/
- [S32] Ruff on PyPI (0.17.3, 2026-10-27; 10.3 MB win wheel) — https://pypi.org/project/ruff/
- [S33] Ruff configuration / CLI reference (`--stdin-filename`, `--output-format`, `--range`) — https://docs.astral.sh/ruff/configuration/
- [S34] Black on PyPI (26.10.1) — https://pypi.org/project/black/
- [S35] IPython completer API (evaluation policies, matchers v2, Jedi options) — https://ipython.readthedocs.io/en/stable/api/generated/IPython.core.completer.html
- [S36] Python docs: `rlcompleter` — https://docs.python.org/3/library/rlcompleter.html
- [S37] PEP 669 – Low Impact Monitoring for CPython — https://peps.python.org/pep-0669/
- [S38] PEP 768 – Safe external debugger interface — https://peps.python.org/pep-0768/
- [S39] PEP 667 – Consistent views of namespaces — https://peps.python.org/pep-0667/
- [S40] What's New in Python 3.14 — https://docs.python.org/3/whatsnew/3.14.html
- [S41] Debug Adapter Protocol overview — https://microsoft.github.io/debug-adapter-protocol/overview
- [S42] VS Code: Python Interactive window / `# %%` cells — https://code.visualstudio.com/docs/python/jupyter-support-py
- [S43] VS Code: Python debugging — https://code.visualstudio.com/docs/python/debugging
- [S44] vscode-unreal-python (GitHub, archived) — https://github.com/nils-soderman/vscode-unreal-python
- [S45] vscode-unreal-python (Codeberg, active) — https://codeberg.org/nils-soderman/vscode-unreal-python
- [S46] vscode-unreal-python CHANGELOG (1.11.0, 2026-06-22) — https://codeberg.org/nils-soderman/vscode-unreal-python/raw/branch/main/CHANGELOG.md
- [S47] vscode-unreal-python `debug.py` — https://github.com/nils-soderman/vscode-unreal-python/blob/main/site-packages/vscode_unreal/debug.py
- [S48] vscode-unreal-python `execute.py` — https://github.com/nils-soderman/vscode-unreal-python/blob/main/site-packages/vscode_unreal/execute.py
- [S49] vscode-unreal-python `attach.ts` — https://github.com/nils-soderman/vscode-unreal-python/blob/main/src/scripts/attach.ts
- [S50] vscode-unreal-python issues (via GitHub search API) — https://github.com/nils-soderman/vscode-unreal-python/issues?q=is%3Aissue
- [S51] unreal-remote-execution (npm/TS port of the protocol) — https://github.com/nils-soderman/unreal-remote-execution
- [S52] Epic forum: "Unreal Engine 5.8 is on Python 3.11…" (Epic reply 2026-07-23) — https://forums.unrealengine.com/t/unreal-engine-5-8-is-on-python-3-11-when-the-vfx-reference-platform-2026-recommends-python-3-13-as-minimum-version/2745703
- [S53] Local UE 5.8 install, inspected 2026-10-28, under `C:\Program Files\Epic Games\UE_5.8\Engine\`:
  - `Binaries/ThirdParty/Python3/Win64/NEWS.txt` (Python 3.11.8; pip 24.0 in site-packages)
  - `Plugins/Experimental/PythonScriptPlugin/Content/Python/debugpy_unreal.py`
  - `.../Content/Python/remote_execution.py` (protocol constants and modes)
  - `.../Source/PythonScriptPlugin/Private/PyGIL.h`
  - `.../Private/PythonScriptPluginSettings.h` (`ETypeHintingMode`, remote execution settings)
  - `.../Internal/PyGenUtilAccessor.h`
  - `Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h` (`EnterDebuggingMode`/`LeaveDebuggingMode`)
  - `Source/Runtime/WebBrowser/Public/SWebBrowserView.h` (`BindInputMethodSystem`), `IWebBrowserWindow.h` (`BindUObject`, `ExecuteJavascript`), `IWebBrowserSingleton.h` (`RegisterSchemeHandlerFactory`)
  - `Source/ThirdParty/CEF3/cef_binary_128.4.13+…+chromium-128.0.6613.138…`
- [S54] TAColor: How to debug Python in UE with VS Code (`debugpy_unreal`) — https://www.tacolor.xyz/tapython/how_to_debug_python_in_ue.html
- [S55] Ryan Dowling Soka: Python in Unreal tips — https://ryandowlingsoka.com/unreal/python-in-unreal/
- [S56] PyCharm: Remote debugging with the Python Debug Server — https://www.jetbrains.com/help/pycharm/remote-debugging-with-product.html
- [S57] JetBrains blog: What's new in PyCharm 2026.2 (debugpy default) — https://blog.jetbrains.com/pycharm/2026/07/what-s-new-in-pycharm-2026-2/
- [S58] PyCharm 2026.2 help: Python debugger (debugpy default, pydevd alternative) — https://www.jetbrains.com/pycharm/help/python-debugger.html
- [S59] JetBrains blog: PyCharm 2026.1 (debugpy option, PEP 669) — https://blog.jetbrains.com/en/pycharm/2026/04/pycharm-2026-1
- [S60] PyCharm: Local History — https://www.jetbrains.com/help/pycharm/local-history.html
- [S61] PyCharm: Python console — https://www.jetbrains.com/help/pycharm/using-consoles.html
- [S62] PyCharm: Breakpoints — https://www.jetbrains.com/help/pycharm/using-breakpoints.html
- [S63] PyCharm: Scratch files — https://www.jetbrains.com/help/pycharm/scratches.html
- [S64] PyCharm: Live templates — https://www.jetbrains.com/help/pycharm/using-live-templates.html
- [S65] PyCharm: Code inspections — https://www.jetbrains.com/help/pycharm/code-inspection.html
- [S66] MayaCharm README — https://github.com/cmcpasserby/MayaCharm
- [S67] debugpy embedded-interpreter discussions: #1233 (use `configure(python=…)`), #1097 (docs gap) — https://github.com/microsoft/debugpy/discussions/1233 , https://github.com/microsoft/debugpy/issues/1097
- [S68] debugpy #1496 (tracing overhead) — https://github.com/microsoft/debugpy/issues/1496
- [S69] monaco-editor README/FAQ (MIT; no `file://` workers; no mobile; accessibility guide) — https://github.com/microsoft/monaco-editor
- [S70] monaco-languageclient (11.0.3, 2026-10-15) — https://github.com/TypeFox/monaco-languageclient
- [S71] VS Code `textModel.ts` (20 MB / 300k-line / 256M-char thresholds) — https://github.com/microsoft/vscode/blob/main/src/vs/editor/common/model/textModel.ts
- [S72] CodeMirror home (MIT; accessibility; huge documents) — https://codemirror.net/
- [S73] CodeMirror example: huge document — https://codemirror.net/examples/million/
- [S74] `@codemirror/lsp-client` README / CHANGELOG (6.2.2, 2026-03-02; repo moved) — https://github.com/codemirror/lsp-client , https://raw.githubusercontent.com/codemirror/lsp-client/main/CHANGELOG.md
- [S75] Ace editor home (BSD; ~4M lines; ace-linters) — https://ace.c9.io/
- [S76] Scintilla documentation — https://www.scintilla.org/ScintillaDoc.html
- [S77] Scintilla license — https://www.scintilla.org/License.txt
- [S78] QScintilla on PyPI (GPL v3 / commercial; 2.14.1) — https://pypi.org/project/QScintilla/
- [S79] UE-75056: IME disabled in Web Browser widget — https://issues.unrealengine.com/issue/UE-75056
- [S80] CEF issue #3313: IME candidate window position in off-screen rendering — https://github.com/chromiumembedded/cef/issues/3313
- [S81] Debian pydevd packaging note (frame_eval not supported on 3.11) — https://metadata.ftp-master.debian.org/changelogs/main/p/pydevd/oldstable_python3-pydevd.README.Debian
- [S82] PyTorch dev-discuss: CPython 3.11 frame evaluation changes — https://dev-discuss.pytorch.org/t/supporting-dynamo-in-python-3-11-cpython-frame-evaluation/1425
- [S83] Wing IDE: Debugging embedded Python code — https://wingware.com/doc/debug/debugging-embedded-code
- [S84] pydevtools handbook: ty (reports OpenAI's planned acquisition of Astral) — https://pydevtools.com/handbook/topics/ty/
- [S85] ty 0.0.86 release notes (code-signed binaries) — https://github.com/astral-sh/ty/releases/tag/0.0.86
- [S86] Cursor forum: "exceeds the maximum supported file size of 52428800" — https://forum.cursor.com/t/file-length-of-is-size-in-bytes-which-exceeds-the-maximum-supported-file-size-of-52428800/160900
