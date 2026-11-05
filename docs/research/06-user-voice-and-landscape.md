> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 06 — User voice and competitive landscape inside Unreal

Research lane: what Unreal Python users complain about and ask for, how studios use Python in Unreal, and which in-editor Python editors or consoles already exist. Compiled 2026-10-28 against UE 5.8 (5.8.3 hotfix shipped 2026-09-22 [S29]).

Citation convention: `[Sn]` points to the numbered list in **Sources**, where every entry has a URL. **UNVERIFIED** marks claims that come from memory, from a search-result snippet I could not open, or from a page that blocked fetching.

Method limits, stated up front:
- **Reddit could not be crawled.** The search tool refused reddit.com for this user agent, so no r/unrealengine, r/vfx, r/TechnicalArtist or r/gamedev threads are cited. User voice comes from the Epic Developer Community forums, tech-artists.org, GitHub issues, the ShotGrid community, Polycount (thin) and vendor and blog pages.
- **Fab and the legacy Marketplace returned HTTP 403** to fetches. Details for Fab-only products come from search-engine snippets and the sellers' own sites, and are marked as such.
- **The shared web-search budget ran out** late in the session. A few planned checks (Epic's experimental CodeEditor plugin internals, Nuke, Houdini and Blender console docs) were not run, and the related claims are marked UNVERIFIED.

---

## 1. Summary

- **Nobody owns this space.** The only actively sold in-editor option is a small $7.99–$9.99 Slate "scratchpad" from Minimal Effort Tech. Its author says it is deliberately not a full code editor [S2][S3]. The free Qt option (leixingyu / hannesdelbeke `unrealScriptEditor`) needs a PySide install and has had bugs open since 2023–2024 [S8][S9][S11]. Serious users leave the editor and use VS Code with Nils Söderman's extension, which has about 64k installs and works over Remote Execution [S21], or PyCharm with Epic's generated stub.
- **Epic's own REPL has barely moved.** The Output Log still has a one-line console input. Multi-line input needs Shift+Enter or a paste, history is up/down arrows, and Python output goes to the shared log [S1][S49]. The 5.8 release notes contain no REPL editing features, only a fix to the Recent Python Files menu and a small Output Log performance fix [S28]. Epic is putting its new tooling effort into AI: an experimental MCP server and Toolsets in 5.8 [S28][S30], and an AI Assistant panel since 5.7 [S31].
- **Top pain points, ranked:**
  1. The editor freezes during long scripts. Execution is game-thread only, which 5.6 began enforcing more strictly, and output is buffered until the script ends [S1][S43][S47].
  2. There is no real in-editor editor, and the Output Log makes a weak REPL [S1][S37][S48].
  3. Autocomplete setup is painful. The `unreal.py` stub is now too big or too complex for Pylance, and Epic's bug UE-119549 has been open since 2021 [S35][S36].
  4. Module reload is unreliable, so people restart the editor [S41][S42].
  5. There is no debugger without VS Code plus Remote Execution plus debugpy [S21].
  6. Python and Qt versions lag: UE 5.8 is on Python 3.11, PySide2 stops at 3.11, and PySide6 has regressions [S50][S51][S52].
  7. Packaging third-party libraries has friction: wheels only, and strict hash checking by default [S57].
- **Surprising findings:**
  - On a Nov 2025 forum thread, a respondent who linked the customer ticket to Epic's JIRA said only one person is assigned to minimum maintenance of UE's Python integration [S35].
  - UE 5.6 quietly broke background-thread Python that called the Unreal API [S43].
  - UEFN creators are queueing on the forums for a gated "Python Editor Scripting" beta, mostly so they can drive the editor from Claude Code or other MCP clients [S70].
  - Python is spreading into new engine surfaces: a PCG Python Data Processor node, Blueprint graph editing from Python, a batch-processor framework, and Python in naming tokens, all in 5.8 [S28][S29].

**Implication for our product:** be the Charcoal or PyCharm of Unreal, running in-process with completion drawn from live reflection instead of a 50 MB stub. Pair that with an execution model that does not freeze the editor (cancellable, with progress, streaming output and transactions), deep reload, a built-in debugger, and Maya-style ergonomics (Ctrl+Enter, persistent tabs, shelves). Share one execution session with Epic's MCP and AI surfaces rather than competing with them.

---

## 2. Existing Unreal options

### 2.1 Comparison table

| # | Product | Link | Engines | Approach | Key features | Price / license | Sentiment / traction | Last update | Visible bugs and limits |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **Python Script Editor** (Minimal Effort Tech, "MrMinimal") | Fab [S7]; docs [S4][S5][S6]; launch post [S2][S3] | Built for 4.27–5.5 at launch (Feb 2025); docs say 4.27+, with 5.3+ recommended [S2][S4]. Current 5.6–5.8 builds UNVERIFIED (Fab 403). | **Native Slate/C++.** No dependencies beyond PythonScriptPlugin. Custom `STabWidget`, `SPythonTextEditor` with a syntax marshaller, `IPythonRunner` interface [S2][S6]. | Multi-tab scratchpad modeled on Maya's; tabs persist per project or per user; syntax highlighting and line numbers; run selection or tab with Ctrl+Enter; completer on Ctrl+Tab drawn from variables in the current scope; help (Ctrl+H) and pretty-print (Ctrl+P) on the selection; searchable output with regex and history; snippets with categories and a `%SELECTION%` placeholder; light/dark theme; periodic autosave [S5][S7]. | $7.99–$9.99, unlimited seats [S2][S3]. | Launch thread has **zero replies** [S2]. No independent reviews found. Fab rating UNVERIFIED. | Feb 2025 launch. Later updates UNVERIFIED. | Author says it is "not a full code editor" [S3]. Docs warn that a crash can lose data [S5]. Before 5.1, snippets replace the selection and search does not highlight [S4]. Default user data lives under the project's Content folder, which docs advise changing for production [S6]. Fab review slows updates [S2]. No debugger, reload, linting, go-to-definition or file tree is documented. |
| 2 | **unrealScriptEditor** (Lei Xingyu), plus the **unreal-plugin-python-script-editor** wrapper (Hannes Delbeke) | [S8][S9][S10][S11][S12][S13] | UE4 and UE5 [S10]. | **External Qt widget** (PySide6 from v0.0.3; earlier Qt.py with PySide2/PyQt5), styled with `unreal_stylesheet` [S8]. | Maya-like editor: save and load files, temporary scripts, shortcuts, highlighter, auto-completion [S8][S10]. Installs through pip or plugget [S11]. | MIT, free [S8][S12]. | 64 stars on the original, 15 on the fork, 12 on the plugin [S8][S10][S11]. tech-artists announcement got no replies [S12]. | Open issues dated Sep 2023 to Oct 2024 [S9]. | README says "still in development" [S8]. Open bugs: random key-combo error, toolbar icon not showing, PySide6 deprecation warning [S9]. You must install PySide yourself [S12]. Qt windows can fall behind the editor and need help with parenting and GC [S53][S54]. |
| 3 | **Python Editor** (legacy Marketplace; seller UNVERIFIED) | [S14] | 4.25–4.27, 5.0–5.1 per snippet [S14]. | UNVERIFIED | "Complete suite" with smart auto-complete and shelves for icon shortcuts, per snippet [S14]. | UNVERIFIED | Listing about six years old [S14]. | No 5.2+ support, so effectively abandoned. | Page returned 403. |
| 4 | **TAPython** (Chen Hao / TAColor) | [S18][S19][S20] | Current release supports **UE 5.8**; older builds back to 4.21 [S18]. | Native plugin. Python plus JSON-defined Slate UIs ("Chameleon Tools") [S18]. | Maya-style **Shelf** (drag in assets, actors, snippets or tools; one-click run); Object Detail Viewer for UObject functions and properties, with diff; 300+ extra APIs; live UI preview; menu config; stub-splitting tool [S18][S19][S20]. | **AGPL-3.0**, free [S18]. | 441 stars [S18]. Often cited as the answer in forum workarounds [S48]. | v1.3.3; date UNVERIFIED [S18]. | **Not a code editor.** AGPL is a legal hurdle for many studios (our inference). |
| 5 | **Python Browser** (Lambda Works / "Gamji") | [S17] | UNVERIFIED | Unreal plugin; details UNVERIFIED. | Browse and search project `.py` files, run them, save argument presets [S17]. | Fab; price UNVERIFIED. | No replies [S17]. | May 2025 [S17]. | Script launcher only, no editing. |
| 6 | **Python Scripts Collection Menu** (legacy) | [S15] | 4.23–5.2 [S15]. | Code plugin. | Fixed menu dropdown that collects all project Python scripts [S15]. | Vault only; not migrated to Fab [S15]. | — | Stale | Launcher only. |
| 7 | **Python Times** (M. A. Moniem) | [S16] | 4.27, 5.0–5.3 [S16]. | Code plugin. | Runs Python scripts automatically on editor events, configured in Project Settings [S16]. | Fab; price UNVERIFIED. | — | Stale (≤5.3) | Event hooks only. |
| 8 | **Unreal Engine Python for VS Code** (Nils Söderman), *external* | [S21][S22][S23] | Any UE with Remote Execution enabled. | **External IDE** over Epic's Remote Execution [S21]: UDP multicast discovery [S25], then a TCP command channel (UNVERIFIED). | Ctrl+Enter runs selection or file in UE; sets up completion for the current project; attaches a debugger with breakpoints and stepping; browses docs generated from the running editor, including project C++ types [S21][S22]. | Free [S21]. | **63,920 installs**, 8 ratings (average UNVERIFIED) [S21]. Warmly received on tech-artists [S22]. | v1.11.0 [S21]. | Depends on Remote Execution, which fails when firewalls or multicast block it [S24][S25]. Completion comes from the giant stub, so it inherits the Pylance limits [S35][S36]. Code runs out of process from the editor's point of view. |
| 9 | **PyCharm + Epic stub** (DIY), *external* | [S37][S39][S20] | All | External IDE plus the `Intermediate/PythonStub/unreal.py` stub generated in Developer Mode [S39]. | Completion, navigation. | — | Common, but heavy: PyCharm warns about low memory once the stub passes 21 MB and 400k lines [S20]. | — | No execution or debug bridge without third-party pieces [S42]. |
| 10 | **Epic Output Log Python console** (built-in) | [S1][S49][S28] | All | Slate console input bar with Cmd / Python modes [S1]. A separate "Python (REPL)" executor that echoes expression results exists in the dropdown (**UNVERIFIED**, from memory of PythonScriptPlugin source; architecture lane should confirm in 5.8). | One-line input; Shift+Enter or paste for multi-line; type a file name to run it, with args; up/down history stored in `ConsoleHistory.ini`; `print` goes to the Output Log; File > Execute Python Script and Recent Python Scripts (re-reads from disk) [S1][S49]. | Built-in | — | 5.8 only fixed the Recent Python Files list and Output Log category performance [S28]. | No Python completion found in any source. No tabs or files. Can't clear the log from Python [S48]. Output buffered until a script exits [S47]. Editor UI blocked while a script runs [S1]. |
| 11 | **Epic Unreal MCP + Toolsets** (5.8, Experimental) | [S28][S30] | 5.8 | In-editor HTTP/SSE MCP server; toolsets written as Python modules under `Content/Python` [S30]. | AI agents call Python-authored tools (SceneTools, ActorTools, MaterialInstanceTools, ObjectTools, plus Sequencer and animation toolsets) [S28][S30]. | Built-in | New | 5.8 | No general execute-Python tool is documented. Calls run **serially on the game thread**. No authentication; loopback only [S30]. |
| 12 | **Epic AI Assistant panel** (5.7+, Experimental) | [S31][S28] | 5.7+ | Docked cloud-LLM panel. | Answers questions and generates code as text. A vendor review says it cannot run editor operations [S31]. | Free tier, per [S31]. | — | 5.8 fixed its tab label [S28]. | Python execution not confirmed [S31]. |
| 13 | **Third-party AI agents that run Python** (UnrealCopilot, VERA, Unreal-MCPython and many MCP servers) | [S32][S78][S33] | 5.6–5.8 | Chat panels or MCP servers that generate and `exec` Python in the editor [S32][S33][S78]. | "Virtual technical artist" that runs Python on request [S32]. | Mixed | Very active in 2025–26 | 2026 | They create a new need: reviewing, diffing and undoing machine-written Python (our inference). |
| 14 | **Epic "Code Editor" plugin** (Experimental, Beta 0.1) | [S27] | Listed for 5.5–5.8 [S27]. | Slate | "Allows editing of code from within the Unreal editor" [S27]. Believed to be a bare C++ source viewer/editor (**UNVERIFIED**). | Built-in, off by default [S27]. | — | Unchanged since added [S27]. | Not Python-aware as far as known (UNVERIFIED). |

### 2.2 Notes

- **Every in-editor option is either shallow or stalled.** The Minimal Effort editor gets the Slate-native foundation right but stops at a scratchpad on purpose [S3]. The Qt editors carry PySide install, parenting and window-behind problems [S12][S53][S54]. The plugins that sell "shelves" or "menus" are launchers, not editors [S15][S17]. Demand clearly exists, since at least six products attempt some slice of it, but none has visible reviews or an active community thread [S2][S12][S17].
- **The external-IDE path is the de facto professional workflow.** It is fragile in three places: stub size [S35][S36], Remote Execution connectivity [S24][S25], and the lack of live-object completion. The VS Code extension's completion comes from the stub, while the Minimal Effort completer reads live scope [S7][S21].
- **Epic's own new investment is AI-shaped.** The MCP server, Toolsets authored in Python, and the AI Assistant [S28][S30][S31] are not a script editor. Python is becoming the language Toolsets are written in, though, so a good editor for writing and testing Toolsets is a fresh need (our inference from [S30]).
- **Qt-based UI inside Unreal keeps regressing.** Examples: QApplication creation crashing the editor [S53][S55], and PySide6 drag-and-drop into the viewport breaking in 5.7 (UE-364137, workaround required an engine source edit) [S52]. This argues for **Slate-native UI** in our editor.

---

## 3. Pain points, ranked

Severity: **Critical** (blocks work or loses work), **High** (daily friction, makes people leave the editor), **Medium** (recurring friction), **Low**. Frequency is the number of independent sources found in this pass (Reddit excluded).

| Rank | Pain point | Evidence | Freq. | Severity | How our editor should address it |
|---|---|---|---|---|---|
| 1 | **The editor freezes while scripts run.** Long or blocking scripts lock the UI. `time.sleep` freezes the editor. Infinite loops cannot be escaped. The editor does not tick mid-script, which breaks Sequencer caches. **5.6 added game-thread checks** that broke background-thread Python touching the Unreal API, and Epic's advice was to time-slice the work. | Epic docs say the UI is blocked while a script runs [S1]; [S43] (Oct 2025, 5.6 regression, Epic reply); [S44]; [S45]; [S46]; [S81] | 6 | **Critical** | (a) Always-visible **Stop/Cancel** that raises `KeyboardInterrupt` through a trace hook or `PyErr_SetInterrupt`. (b) Opt-in **auto-progress**: wrap runs in `ScopedSlowTask` so a cancellable dialog appears after N ms. (c) A first-class **time-sliced runner** (generator or `yield`-based "cooperative script" mode ticked by a Slate/FTSTicker callback) that the 5.6+ threading rules permit. (d) A documented **worker-thread lane** for pure-Python work (I/O, REST) that marshals results back to the game thread. (e) A watchdog that flags runaway loops. |
| 2 | **No real in-editor editor; the Output Log is a poor REPL.** One-line input, Shift+Enter for new lines, no Python completion, no tabs or files, history only via arrows (and stuck history needs an ini deleted), no way to clear the log from Python, and Python output mixed with engine noise. | [S1]; [S49]; [S48] (2020–2023); [S37] (2019 user asking for an interactive interpreter with completion "like Blender's"); the existence of 6+ third-party attempts [S2][S8][S14][S15][S17] | 7 | **High** | A dockable Slate **Script Editor** tab: persistent multi-tab buffers, a split history/output and input layout, Ctrl+Enter for selection or all, a REPL pane with real multi-line editing and searchable history, **a dedicated Python output pane** filterable by run (separate from the engine log), and Clear plus `editor.clear()` APIs. |
| 3 | **Autocomplete and stub setup.** Developer Mode is needed to generate the stub. Stubs have been incomplete (only native types in 4.21, later fixed). The stub is **too big or complex for Pylance**: about 24 MB in 5.5 exceeds Pyright's complexity limit, and a 52.9 MB project stub went over the 50 MB cap. PyCharm runs low on memory and Wing shows the file as empty. Epic's **UE-119549** has been open since 2021. | [S39]; [S37][S38]; [S36] (closed by Microsoft as "by design"); [S35] (Nov 2025); [S20]; [S40]; Houdini analogue [S80] | 7 | **High** | **Live-reflection completion**: introspect `unreal` types, UFUNCTION and UPROPERTY metadata and docstrings in-process, including project and plugin C++ types, with no stub needed. Also offer **"Export split stubs"** (per-module `.pyi` package) for people who still use external IDEs, which fixes the Pylance problem as a side benefit. |
| 4 | **Module reload and iteration.** Edits to studio modules aren't picked up, so people restart the editor. `importlib.reload` misses `from x import y` and dependent modules. There is no out-of-the-box external reload. | [S41] (2019; Epic's Jamie Dale: reload your own modules, never `unreal`); [S42] (2020); Maya analogue on tech-artists (reverse-order reload) in the same search | 3 | **High** | **Deep reload** of a package or of everything under a project path, in dependency order, with stale-reference warnings. **Reload-on-save** for watched folders. A "Run entry point" binding per tab (e.g. reload `studio_tools`, then call `main()`). |
| 5 | **No debugger in the editor.** Breakpoints need the VS Code extension plus Remote Execution plus debugpy, or DIY `debugpy.listen` setups. | [S21][S22]; no in-editor alternative found [S5][S8] | 3 | **High** | An in-process **debugger**: gutter breakpoints, step over/into/out, locals and watch, and an evaluation console at the paused frame. Paused state must keep Slate pumping, which the architecture lane needs to design. Plus a **"Debug in VS Code"** button that starts debugpy for people who prefer it. |
| 6 | **Python and Qt version lag.** 5.8 ships Python **3.11.8**. The VFX Reference Platform CY2026 says 3.13, and Epic said 3.13 will come through main, not a 5.8 hotfix. **PySide2 has no Python above 3.11.** PySide6 had a QApplication crash and a 5.7 drag-and-drop regression (UE-364137). | [S1]; [S50] (Jul 2026, Epic reply); [S51]; [S52] (Feb 2026); [S53]; [S55] | 5 | **Medium–High** (pipeline studios) | Build the editor in **Slate, not Qt**, so it carries no PySide dependency. Keep the code **Python-version agnostic** (3.11 through 3.13+). Show the interpreter version and paths in the status bar. Ship an "environment doctor" panel. |
| 7 | **Third-party packages.** `PythonRequirements` in `.uplugin` accepts **wheels only**. Strict hash checking is on by default. Offline installs mean copying `site-packages`. ShotGrid's Qt import failed until PySide2 was installed inside Unreal. Community templates exist just to auto-install pip requirements. | [S57]; [S58]; [S59] | 3 | **Medium** | A **Packages panel**: show what is importable and from where, `pip install` into the project's `Intermediate/PipInstall` or a user site, and generate a `PythonRequirements` block with hashes for a plugin. Warn on wheel or ABI mismatch. |
| 8 | **Remote Execution flakiness** for IDE users. UDP multicast `239.0.0.1:6766` gets blocked by firewalls or VPNs, and sockets time out. | [S24]; [S25]; [S26] | 3 | **Medium** | Removed for in-editor users, since we run in-process. For external IDEs, add an optional **localhost TCP bridge** with a token, plus a connectivity self-test. |
| 9 | **API discoverability.** snake_case mapping, editor properties only reachable via `get/set_editor_property`, reliance on `dir()` and `getmembers()`, docs labeled "Experimental", "very little documentation". | [S1]; [S74]; [S75]; [S77]; [S84] | 5 | **Medium** | Inline **docs on hover** from reflection (C++ tooltip, category, editor-property list). An **object inspector** for the selected actor or asset (like TAPython's Object Detail Viewer [S18]). "Copy as Python" for a selected actor or asset path. A searchable API browser. |
| 10 | **Output ergonomics.** `unreal.log` output stays buffered until the script exits (Jun 2023). `print` from the command line appears only in the Saved/Logs files. Tracebacks are mixed into engine log spam. | [S47]; [S48]; [S1] | 3 | **Medium** | Stream stdout and stderr live, at least on cooperative yields and slow-task ticks. Render tracebacks with **click-to-line** links. Color-coded per-run output blocks. |
| 11 | **Safety and undo.** Not every operation is undoable. You must remember `ScopedEditorTransaction`. `os.rename` and `shutil.move` silently break references. Batch edits need a dry run first. | [S1]; [S76]; [S81] | 3 | **Medium** | A **"Run in transaction"** toggle (on by default for editor buffers). A dry-run mode. Lint warnings for known foot-guns such as `os.rename` on Content paths or direct attribute assignment where `set_editor_property` is needed. |
| 12 | **Sharing tools with artists.** TDs need a way to hand scripts to non-coders: menus, shelves, argument presets. Several plugins exist only for this. | [S15][S16][S17][S18]; Python plugin template [S58] | 5 | **Medium** | **Shelves** and a **snippet library** stored in project or plugin folders (source-control friendly) and in user folders. Add parameters to a snippet so artists get a small form. Drag selection to shelf, as in Maya. |
| 13 | **Epic's level of investment.** One person on minimum maintenance (per an Epic-linked respondent). The API is still labeled "Experimental" in the docs. | [S35]; [S84]; [S1] | 3 | Strategic | Avoid relying on engine changes. Keep everything in a plugin that works on the stock 5.8 PythonScriptPlugin. |
| 14 | **UEFN access gating.** The Python Editor Scripting beta is allow-listed; "Python disabled via CVar" reports. | [S70] | many threads | Low for us (third-party plugins can't load in UEFN) | Out of scope for v1. Track it as a future market. |

---

## 4. Feature requests

What users explicitly ask Epic or tool authors for. Each item is paraphrased.

1. **An interactive interpreter with autocomplete, like Blender's**, and a complete stub (2019) [S37]. Clear IDE setup steps and completion beyond native types [S38].
2. **A way to keep the editor responsive during long Python work:** tick the editor mid-script [S45], delays without freezing [S46], background threads allowed again in 5.6+, and guidance on what Epic expects for long-running Python [S43].
3. **A Python API to clear the Output Log** (2020, re-asked 2022) [S48]. **Live log output** while a script runs [S47].
4. **A fix for the stub generator:** split `unreal.py` by module or plugin so Pylance can handle it (UE-119549) [S35]. Microsoft explicitly asks for stub splitting on the generator side [S36].
5. **An out-of-the-box way to run or reload external code from an IDE**, compared with Maya's command port [S42].
6. **Python 3.13 parity with the VFX Reference Platform** [S50].
7. **PySide6 interop that keeps working:** drag-and-drop from Qt into the viewport [S52], and Qt windows that stay on top [S53].
8. **UEFN:** access to Python Editor Scripting, mainly for actor placement, batch import, scene setup and **MCP or Claude Code integration** [S70].
9. **From the third-party editors' marketing, read as unmet demand:** tabs that persist across projects, snippets, help and pretty-print on a selection [S2]; shelves and smart completion [S14]; one-click script launch with argument presets [S17].

---

## 5. How studios use Python in Unreal

| Use case | Evidence | Notes |
|---|---|---|
| **Sequencer and layout automation** (film/animation) | Sony Pictures Imageworks, Unreal Fest Orlando 2025: tools that split subsequences, bind many tracks to one camera, insert shots mid-sequence [S63] | Artists repeat manual Sequencer steps; TDs automate them. |
| **USD interchange** | Sony Imageworks, Orlando 2025: Python for USD export, parsing assets into USD layers, patch scripts [S64]. 5.8 makes Interchange USD import production-ready for assets [S29]. | Pipeline TD territory. |
| **Classic CG pipeline integration** | M2 Animation, Unreal Fest Bali 2025 [S65] | Title only; details UNVERIFIED. |
| **Production tracking (ShotGrid / Flow)** | tk-unreal is the Toolkit engine. It publishes static meshes as FBX, and Epic says it is not a finished pipeline [S61][S62]. Users hit Python tracebacks and Qt import errors [S59][S60]. | ftrack and AYON integrations were not researched (search budget). |
| **Virtual production** | Switchboard is a Python application for multi-device control [S66]. Remote Control plus Python remote execution for stage control [S26]. 5.8 naming tokens can run Python [S28]. | Operators need reliability and one-click actions. |
| **Data validation / CI** | Python validators subclass the editor validator base and must register themselves; they run on save, from menus and in CI [S67]. Python editor tests exist [S68]. | QA and tools programmers. |
| **Batch asset processing** | 5.8 batch processor framework with a Python example (`RunBatch`) [S28][S29]. Asset Registry queries, dry runs, transactions [S76]. MetaHuman assembly is scriptable in 5.7 (search snippet, UNVERIFIED wording) [S71]. | The most common TA job. |
| **Data import from external systems** | A studio framework that walks REST-like data on a background thread and builds assets; broken by 5.6 [S43] | Exactly the long-running case. |
| **Procedural / Blueprint authoring** | 5.8: PCG Python Data Processor node; `unreal.BlueprintGraphEditor` for graph editing [S29] | New 2026 surfaces where people need to explore APIs interactively. |
| **Editor UI tools** | PySide tools reacting to viewport selection [S56]; TAPython Chameleon UIs [S18]; Editor Utility Blueprints calling Python [S1] | Tool authors need fast iteration on UI code. |
| **AI-driven editing** | Epic MCP Toolsets written in Python [S30]; UEFN creators want MCP [S70]; third-party agents that run Python [S32][S78] | A new persona: the "AI-assisted operator". |

---

## 6. Personas

| Persona | Goals | Frequency | Skill | Must-haves | Frustrations today |
|---|---|---|---|---|---|
| **P1. Pipeline TD** (film, animation, VP studio) | Connect Unreal to ShotGrid, USD and render farms; maintain a studio Python package; batch jobs and commandlets. | Daily, for hours | Expert Python, moderate C++ | Deep reload; debugger; package and venv control; headless or commandlet runs; long jobs that don't freeze; version-agnostic (3.11 to 3.13); source-controlled settings | Restarting the editor to pick up changes [S41]; 5.6 threading break [S43]; Python 3.11 vs VFX Ref 3.13 [S50]; PySide breakage [S52]; pip friction [S57] |
| **P2. Technical Artist** (games) | Batch-fix assets, validate naming, build editor tools, prototype with the API | Daily, in short bursts | Intermediate to advanced Python | Fast REPL; completion on **live objects**; hover docs; snippets; Ctrl+Enter; transactions and undo; output that isn't buried in engine logs | Output Log REPL [S1]; stub/Pylance pain [S35]; API discoverability [S74] |
| **P3. Artist who runs scripts** (environment, lighting, layout) | Run the TD's tools on a selection without touching code | Weekly | Little or no coding | Shelf buttons; parameter forms; clear progress, cancel and undo; friendly error messages | Launch paths are hidden (hence Python Browser and Scripts Menu plugins [S15][S17]); freezes [S1] |
| **P4. Virtual production operator / VP TD** | Run stage scripts (Sequencer, naming, Remote Control) reliably under time pressure | Daily during shoots | Low to intermediate | One-click, rock-solid scripts; no freezes; logs saved per run; remote trigger | Freezes and fragile remote execution [S24][S43] |
| **P5. Tools / engine programmer** | Expose C++ to Python, write editor tests and validators, build MCP Toolsets | Weekly | Expert C++ and Python | Completion including project C++ types; jump from Python to the C++ declaration; run tests; inspect reflection metadata | Stub size [S35][S36]; manual validator registration [S67]; Toolset refresh loop [S30] |
| **P6. Learner / indie / AI-assisted creator** | Learn the API, automate repetitive level work, run AI-generated scripts | Occasional | Beginner | Discoverable docs and examples; safe runs (preview, dry run, undo); explanations of errors; easy paste of AI output | Thin docs [S74]; risky edits; UEFN gating [S70] |

---

## 7. User journeys our editor must nail

**J1. Explore an API interactively** (P2, P6)
1. Select actors in the viewport and press a key, or drag them into the editor, to get `sel = [...]` as live objects.
2. Type `sel[0].` to get completion from live reflection, with editor properties included.
3. Hover a symbol to see its C++ tooltip, type and category. Ctrl+H shows help; Ctrl+P pretty-prints.
4. Try `set_editor_property(...)` and watch the viewport update. Ctrl+Z undoes it, because REPL lines run in a transaction.
5. Send the working lines from history to a new tab.

**J2. Batch rename or process selected assets** (P2, then P3)
1. Select assets in the Content Browser and open the "Batch rename" snippet from the library.
2. Run it in **dry-run** mode to see a results table of old and new names.
3. Run it for real inside one transaction, with a progress bar and **Cancel**.
4. Check the summary (n changed, k skipped, errors linked to their lines), then undo or keep.
5. Save it to a shelf with parameters so artists can run it later.

**J3. Iterate on a studio module with reload** (P1, P5)
1. Open `studio_tools/importer.py` from the project's Python paths in the file tree.
2. Edit and save. Reload-on-save deep-reloads the `studio_tools` package in dependency order and warns about stale `from x import y` references.
3. Press F5, the tab's entry point, to run `importer.main()`.
4. Click a traceback line to land on the exact line in the file.

**J4. Debug a failing import script** (P1, P2)
1. Run; it fails. The red traceback block in the Python output has clickable frames.
2. Set a breakpoint in the gutter and press Debug.
3. Execution pauses while the editor stays responsive. Inspect locals and watch expressions, and evaluate `asset.get_editor_property(...)` in the paused frame.
4. Step, fix, continue. Optionally press "Debug in VS Code" to attach debugpy instead.

**J5. Run a long job without freezing** (P1, P4)
1. Start a 20-minute reimport.
2. The editor shows progress, streams log lines and offers Cancel. In cooperative mode it yields per item, so the editor stays usable.
3. On cancel, it stops cleanly at an item boundary and reports what finished.
4. A per-run log is saved to disk for the shoot report.

**J6. Share a snippet or tool with the team** (P1, P2, then P3)
1. Select code and choose "Save as snippet" with name, category and parameters.
2. Choose scope: user, project (`Content/Python/...`, checked into source control) or studio network path.
3. Teammates see it in the library and on shelves after sync. Artists run it from a shelf button with a small parameter form.

**J7. Run AI-generated or pasted code safely** (P6, P4)
1. Paste code from an AI assistant or MCP client.
2. Lint shows unknown `unreal.*` names, deprecated APIs and foot-guns.
3. Run in dry-run or transaction mode, review what changed, and undo with one keystroke.
4. Optionally expose the same session to Epic's MCP so the agent and the human see the same globals (to be designed).

**J8. Set up a fresh machine** (P1, P5)
1. Open the editor. Completion works immediately, with no stub generation or IDE setup.
2. The Environment panel shows the Python version, `sys.path` and importable packages.
3. Pip-install missing wheels into the project, and export a `PythonRequirements` block for the plugin.

---

## 8. Habits imported from Maya, Houdini and Nuke

| Habit | Source DCC | Evidence | Our default |
|---|---|---|---|
| History pane on top, input pane below | Maya | [S73] | Split output and input layout, with an optional single-pane mode |
| **Ctrl+Enter runs the selection**; numpad Enter runs everything | Maya | [S73] | Same bindings. Shift+Enter runs the current line or block. |
| Tabs per language that persist across sessions; middle-click closes a tab | Maya | [S73] | Persistent tabs with autosave; tab state restored on editor start |
| **Save script to shelf**; drag a script onto the shelf | Maya, Charcoal | [S73][S72] | Drag selection to shelf; shelves stored as data files |
| Executing clears the input pane unless text is selected (Charcoal advertises "never delete code on execute") | Maya, Charcoal | [S72] (the Maya default clearing behavior is inferred) | Never clear; history keeps executed blocks |
| Auto-save on execute; go to file, symbol or line; outline; pylint; split editors; output highlighting | Charcoal 2 ($79) | [S72] | All are v1 or v1.1 candidates; Charcoal is the bar to meet |
| Ctrl+scroll zooms font | Maya | [S73] | Same |
| **Echo All Commands** (UI actions logged as script) | Maya; Blender Info editor; 3ds Max Macro Recorder | UNVERIFIED (general knowledge; doc fetches failed) | Unreal has no equivalent (UNVERIFIED). A partial "echo" via transaction and property-change listeners that emit Python is a differentiator to explore. |
| Tab completion in the shell; HScript passthrough with `%` | Houdini hython | [S79] | Completion everywhere. Cmd-mode passthrough (`!` prefix runs an Unreal console command). |
| Drag a node or parameter into the Python shell to get its path | Houdini | UNVERIFIED | Drag actor or asset into the editor to insert `unreal.load_asset('/Game/..')` or an actor reference |
| Session module persisted in the scene (`hou.session`) | Houdini | UNVERIFIED | Per-project "session" module auto-imported into the REPL |
| Script Editor with input and output panes; Ctrl+Enter; "echo Python commands"; "clear output on run" | Nuke | UNVERIFIED (Foundry doc URLs 404'd) | Same toggles |
| Python console with Ctrl+Space completion; tooltips that show the Python path of UI elements | Blender | UNVERIFIED (docs 403); the 2019 Unreal user explicitly asked for "Blender-like" completion [S37] | Completion popup on Ctrl+Space and Ctrl+Tab (the Minimal Effort binding [S5]) |

---

## 9. Gap analysis: what no Unreal option does well

| Capability | Output Log | Minimal Effort | Qt unrealScriptEditor | TAPython | VS Code + ext | **Our target** |
|---|---|---|---|---|---|---|
| In-process, Slate-native, no deps | Yes | Yes | No (PySide) | Yes | No | **Yes** |
| Multi-tab persistent editor | No | Yes | Yes | No | Yes (external) | **Yes** |
| Completion from **live** objects and project C++ types | No | Partial (scope variables) | Basic | No | No (stub-based) | **Yes** |
| Hover docs / signature help | No | Ctrl+H help | ? | Object viewer | Stub-based | **Yes** |
| Go to definition / outline / file tree | No | No | No | No | Yes | **Yes** |
| Deep reload / reload-on-save | No | No | No | No | No | **Yes** |
| Debugger | No | No | No | No | Yes (remote) | **Yes (in-process) + VS Code handoff** |
| Non-freezing, cancellable, streaming execution | No | No | No | No | No | **Yes** |
| Transactions / dry run by default | No | No | No | No | No | **Yes** |
| Shelves / parameterized snippets for artists | No | Snippets | No | Shelf | No | **Yes** |
| Package / environment management | No | No | No | No | No | **Yes** |
| Lint for Unreal-specific foot-guns | No | No | No | No | Generic | **Yes** |
| AI / MCP-aware session | No | No | No | No | No | **Explore** |
| Studio-friendly license | — | Yes (cheap) | MIT | **AGPL** | Free | **Decide** |

**Our differentiation, in order of user value:**
1. **An execution model that never strands the user.** Cancel, progress, streaming output and a cooperative long-job mode built for 5.6+ game-thread rules. This is the #1 pain point, and no product addresses it [S1][S43].
2. **PyCharm-grade intelligence without the stub.** Live-reflection completion, hover docs and go-to-definition that work on day one and on 50 MB-stub projects [S35][S36]. Split-stub export is a bonus for IDE users.
3. **Iteration loop:** deep reload, reload-on-save, an entry point per tab, and click-through tracebacks [S41][S42].
4. **Debugging inside the editor.** Today this exists only through VS Code plus Remote Execution [S21].
5. **Maya/Charcoal muscle memory:** Ctrl+Enter, persistent tabs, history/input split, shelves and snippets [S72][S73].
6. **Team distribution:** source-controlled snippet and shelf libraries with parameter forms for artists [S15][S17][S18].
7. **Environment doctor and packages**, which smooth the 3.11-to-3.13 and PySide transitions [S50][S57].
8. **Safe AI-code runs**, a growing 2026 need [S30][S32][S70].

---

## 10. Open questions

1. **Threading design:** what can legally run off the game thread on stock 5.8 PythonScriptPlugin? Can a cooperative or time-sliced runner give live output and cancellation without engine changes [S43]? (Architecture lane.)
2. **Debugger feasibility:** can we pause in a `sys.settrace` breakpoint while still pumping Slate safely, given Epic's warnings about ticking the editor from inside Python [S43]? Or should we hand off to debugpy and VS Code?
3. **Output Log REPL in 5.8:** confirm the "Python (REPL)" executor and any 5.7/5.8 console changes in engine source, since the claim above is UNVERIFIED.
4. **Minimal Effort's current state:** latest version, 5.6–5.8 support and Fab rating (Fab returned 403). Worth buying a copy for teardown.
5. **Epic's CodeEditor plugin:** what it does in 5.8, and whether its Slate code-editing widgets can be reused (UNVERIFIED).
6. **License and pricing:** free/OSS for adoption, or paid on Fab (Charcoal is $79 [S72]; Minimal Effort is $8–10 [S2])? Studios avoid AGPL [S18].
7. **MCP coexistence:** should our session be exposed as an MCP toolset (Python-authored, game-thread serialized [S30]) so AI agents and humans share globals?
8. **Reddit gap:** a follow-up pass with Reddit access, or user interviews, should validate the ranking. Ask especially about freezes vs. completion vs. reload.
9. **Echo / macro recording:** is there an engine hook rich enough (transactions, property-change events) to emit equivalent Python for UI actions?
10. **ftrack / AYON / Deadline** usage of Unreal Python was not researched (search budget).

---

## 11. Sources

All accessed 2026-10-28. "(snippet)" means the page could not be fetched, and the claim relies on the search-engine snippet.

- **S1** Epic, *Scripting the Unreal Editor Using Python* (5.8 docs). https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python
- **S2** Epic forums, "Python Script Editor - Plugin Released" (MrMinimal, 2025-02-19). https://forums.unrealengine.com/t/python-script-editor-plugin-released/2345434
- **S3** Minimal Effort Tech product post. https://minimaleffort.tech/unreal-python-script-editor-plugin/
- **S4** Minimal Effort Python Script Editor docs (home). https://minimalefforttech.github.io/UnrealPythonScriptEditor/
- **S5** Minimal Effort user guide. https://minimalefforttech.github.io/UnrealPythonScriptEditor/user-guide/
- **S6** Minimal Effort developer guide. https://minimalefforttech.github.io/UnrealPythonScriptEditor/developer-guide/
- **S7** Fab listing, Python Script Editor (snippet; 403). https://www.fab.com/listings/85528b02-48bd-4ca1-83ea-d625953cb283
- **S8** leixingyu/unrealScriptEditor. https://github.com/leixingyu/unrealScriptEditor
- **S9** leixingyu/unrealScriptEditor issues. https://github.com/leixingyu/unrealScriptEditor/issues
- **S10** hannesdelbeke/unreal-script-editor. https://github.com/hannesdelbeke/unreal-script-editor
- **S11** hannesdelbeke/unreal-plugin-python-script-editor. https://github.com/hannesdelbeke/unreal-plugin-python-script-editor
- **S12** tech-artists.org, "[FREE PLUGIN] Python Script Editor" (2023-06-07). https://www.tech-artists.org/t/free-plugin-python-script-editor/15918
- **S13** PyPI unreal-script-editor. https://pypi.org/project/unreal-script-editor
- **S14** Legacy Marketplace "Python Editor" (snippet; 403). https://www.unrealengine.com/marketplace/en-US/product/python-editor/
- **S15** Legacy Marketplace "Python Scripts Collection Menu" (snippet). https://www.unrealengine.com/marketplace/en-US/product/python-scripts-collection-menu
- **S16** Python Times (tutorial and Fab listing; snippet). https://mamoniem.com/?p=1598 , https://fab.com/listings/48b8f872-9ca2-49dc-a3f3-f4574c50448b
- **S17** Epic forums, "Lambda Works - Python Browser" (2025-05-24). https://forums.unrealengine.com/t/2525303
- **S18** TAPython release repo. https://github.com/cgerchenhp/UE_TAPython_Plugin_Release
- **S19** TAColor, "Execute Python Command in Unreal Engine". https://www.tacolor.xyz/tapython/execute_python_command_in_unreal_engine.html
- **S20** TAColor, "Split unreal.py for a better development experience". https://www.tacolor.xyz/TipsOfDay/Split_unreal_py_for_a_better_development_experience.html
- **S21** VS Code Marketplace, "Unreal Engine Python" (Nils Söderman). https://marketplace.visualstudio.com/items?itemName=NilsSoderman.ue-python
- **S22** tech-artists.org, VS Code extension thread (2023). https://www.tech-artists.org/t/unreal-engine-python-visual-studio-code-extension/15733
- **S23** nils-soderman/unreal-remote-execution (snippet). https://github.com/nils-soderman/unreal-remote-execution
- **S24** poly-hammer/BlenderTools issue #19, remote-execution socket timeout (snippet). https://github.com/poly-hammer/BlenderTools/issues/19
- **S25** MCP Unreal Server README: multicast 239.0.0.1:6766, firewall notes (snippet). https://glama.ai/mcp/servers/@radial-hks/MCP-Unreal-Server
- **S26** upyrc on PyPI: Remote Control and remote Python wrapper (snippet). https://www.pypi.org/project/upyrc
- **S27** Unreal Directive, Code Editor engine plugin page. https://unrealdirective.com/resources/engine-plugins/codeeditor/
- **S28** Epic, *Unreal Engine 5.8 Release Notes*. https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes?lang=en-US
- **S29** Simstim, "Unreal Engine 5.8.3, what 5.8 changes for TDs, and the road to UE6". https://simstim.org/journal/unreal-news
- **S30** Epic, *Unreal MCP in Unreal Editor* (5.8). https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor
- **S31** StraySpark, UE 5.7 AI Assistant vs MCP (2026-03-24; vendor with an MCP product). https://www.strayspark.studio/blog/ue57-ai-assistant-vs-mcp-comparison
- **S32** atgoldberg/UnrealCopilot (snippet). https://github.com/atgoldberg/UnrealCopilot
- **S33** Fab listings surfaced for "Unreal-MCPython" and a "Python file explorer" plugin (snippet; 403; which name belongs to which listing is UNVERIFIED). https://fab.com/listings/d85485bc-46d8-4201-9aec-5b99965c3a29 , https://www.fab.com/listings/2ed4fd72-ed7d-447f-9305-1492cbd8e816
- **S35** Epic forums, "unreal.py stubs file may become too big for Pylance / VSCode" (2025-11-13; UE-119549). https://forums.unrealengine.com/t/unreal-py-stubs-file-may-become-too-big-for-pylance-vscode/2684691
- **S36** microsoft/pylance-release #7274, Unreal PythonStub regression (2025-05-28; 2026-02-27 follow-up). https://github.com/microsoft/pylance-release/issues/7274
- **S37** Epic forums, "How do you get auto completion and stuff?" (2018–2019). https://forums.unrealengine.com/t/how-do-you-get-auto-completion-and-stuff/118114
- **S38** Epic forums, "Python IDE, autocomplete" (2018–2019). https://forums.unrealengine.com/t/python-ide-autocomplete/119434
- **S39** Epic forums, "Setting up Autocomplete for Editor Python Scripting in UE5" (2022). https://forums.unrealengine.com/t/setting-up-autocomplete-for-editor-python-scripting-in-ue5/561758
- **S40** Wingware Q&A, unreal.py shown empty, 38 MB (snippet). https://ask.wingware.com/question/8687/auto-complete-quit-working-odd-behavior
- **S41** Epic forums, "Must restart the Editor to use updated Python code?" (2019). https://forums.unrealengine.com/t/must-restart-the-editor-to-use-updated-python-code/125847
- **S42** tech-artists.org, "Connecting Python IDEs to Unreal" (2020). https://www.tech-artists.org/t/connecting-python-ides-to-unreal/13010
- **S43** Epic forums, "Unreal 5.6 no longer allows Python to run on a background thread" (Oct 2025). https://forums.unrealengine.com/t/unreal-5-6-no-longer-allows-python-to-run-on-a-background-thread/2675284
- **S44** Epic forums, "Why do infinite loops in the command line block the editor's UI?" (snippet). https://forums.unrealengine.com/t/why-do-infinite-loops-in-the-command-line-block-the-editors-ui/447797
- **S45** Epic forums, "Is it possible to let the editor tick while in the middle of a script?" (2019). https://forums.unrealengine.com/t/is-it-possible-to-let-the-editor-tick-while-in-the-middle-of-a-script/125130
- **S46** Epic forums, "How to enable delay in python script instead of time.sleep(t)" (2020–21). https://forums.unrealengine.com/t/how-to-enable-delay-in-python-script-instead-of-time-sleep-t/149513
- **S47** Epic forums, "How can I get unreal.log in Python to output while the script is running?" (2023). https://forums.unrealengine.com/t/how-can-i-get-unreal-log-in-python-to-output-while-the-script-is-running/1188282
- **S48** Epic forums, "Python clear Output Log" (2020–2023). https://forums.unrealengine.com/t/python-clear-output-log/155056
- **S49** Epic forums, "How to clear Python Console Command History?" (snippet). https://forums.unrealengine.com/t/how-to-clear-python-console-command-history/1657459
- **S50** Epic forums, "UE 5.8 is on Python 3.11 when the VFX Reference Platform 2026 recommends 3.13" (Jul 2026). https://forums.unrealengine.com/t/unreal-engine-5-8-is-on-python-3-11-when-the-vfx-reference-platform-2026-recommends-python-3-13-as-minimum-version/2745703
- **S51** nixpkgs commit disabling PySide2 for Python ≥3.11 (snippet). https://code.ornl.gov/nix/nixpkgs/-/commit/b0f5cb08bf1f2f7d5a4c659bdd451b904cf7250e
- **S52** Epic forums, PySide6 drag-and-drop broken in UE 5.7, UE-364137 (Feb 2026). https://forums.unrealengine.com/t/pyside6-ue5-7-5-6/2704369
- **S53** tech-artists.org, "PySide and Unreal5" (2022–2023). https://www.tech-artists.org/t/pyside-and-unreal5/15475
- **S54** hannesdelbeke/unreal-qt. https://github.com/hannesdelbeke/unreal-qt
- **S55** Unreal AnswerHub, "Loading PyQt Crashes Editor" (snippet). https://answers.unrealengine.com/questions/877593/view.html
- **S56** tech-artists.org, UE5 viewport selection change event with PySide (2024–25). https://www.tech-artists.org/t/ue5-viewport-selection-change-event-setup-python-pyside/16743
- **S57** Epic, *Python Plugin Dependency Management* (snippet). https://dev.epicgames.com/documentation/unreal-engine/python-plugin-dependency-management-for-unreal-engine
- **S58** tech-artists.org, "[FREE] a Python Unreal plugin template" (2025). https://www.tech-artists.org/t/free-a-python-unreal-plugin-template/17995
- **S59** ShotGrid community, "Shotgrid Menu in Unreal is not responding" (2022–2024). https://community.shotgridsoftware.com/t/shotgrid-menu-in-unreal-is-not-responding/16313
- **S60** ShotGrid community, "State of the tk-unreal Engine?" (2019). https://community.shotgridsoftware.com/t/state-of-the-tk-unreal-engine/2972
- **S61** ue4plugins/tk-unreal (snippet). https://github.com/ue4plugins/tk-unreal
- **S62** Epic, *Using Unreal Engine with Autodesk ShotGrid* (snippet). https://dev.epicgames.com/documentation/unreal-engine/using-unreal-engine-with-autodesk-shotgrid
- **S63** Epic forums, Unreal Fest Orlando 2025, "Automate and Expand Sequencer's Functionality with Python" (Sony Imageworks). https://forums.unrealengine.com/t/talks-and-demos-automate-and-expand-sequencer-s-functionality-with-python-unreal-fest-orlando-2025/2672420
- **S64** Epic forums, Unreal Fest Orlando 2025, "Optimizing Unreal Engine's OpenUSD Workflow with Python" (snippet). https://forums.unrealengine.com/t/talks-and-demos-optimizing-unreal-engine-s-openusd-workflow-with-python-unreal-fest-orlando-2025/2674036
- **S65** Epic Dev Community, "Bringing Unreal Engine into a Classic CG Pipeline with Python" (M2 Animation, Bali 2025; snippet). https://dev.epicgames.com/community/learning/talks-and-demos/x1wb/bringing-unreal-engine-into-a-classic-cg-pipeline-with-python
- **S66** Epic, *Switchboard in Unreal Engine* (snippet). https://dev.epicgames.com/documentation/unreal-engine/switchboard-in-unreal-engine
- **S67** Epic, *Data Validation in Unreal Engine* (snippet). https://dev.epicgames.com/documentation/unreal-engine/data-validation-in-unreal-engine
- **S68** Epic, *Write Editor Tests with Python* (search result). https://dev.epicgames.com/documentation/en-us/unreal-engine/write-editor-tests-with-python-in-unreal-engine
- **S69** Epic, *Scripting in Unreal Engine for Maya Users*. https://dev.epicgames.com/documentation/unreal-engine/scripting-in-unreal-engine-for-maya-users?lang=en-US
- **S70** Epic forums, UEFN Python Editor Scripting beta threads (2026). https://forums.unrealengine.com/t/python-editor-scripting/2740459 , https://forums.unrealengine.com/t/python-editor-scripting-cannot-be-enabled-in-41-30-allow-list-or-regression/2744441 , https://forums.unrealengine.com/t/python-editor-scripting-beta-access-for-uefn-mcp-integration/2730953
- **S71** Epic news, UE 5.7 launch (MetaHuman Python automation; snippet). https://www.unrealengine.com/news/unreal-engine-5-7-is-now-available?lang=de
- **S72** Zurbrigg, *Charcoal Editor 2*. https://zurbrigg.com/charcoal-editor-2
- **S73** Autodesk Maya Help, Script Editor. https://help.autodesk.com/cloudhelp/2025/ENU/Maya-Scripting/files/GUID-7C861047-C7E0-4780-ACB5-752CD22AB02E.htm
- **S74** mab09/Unreal-Python-Scripting-Examples README (snippet). https://github.com/mab09/Unreal-Python-Scripting-Examples
- **S75** Epic forums, course thread "Utilizing Python for Editor Scripting" (snippet). https://forums.unrealengine.com/t/course-utilizing-python-for-editor-scripting-in-unreal-engine/575772
- **S76** The Technical Artist, "Python In Unreal". https://thetechnicalartist.com/unreal/python-in-unreal.html
- **S77** Thomas Mansencal, "Improving Unreal Engine API Documentation Discoverability" (snippet). https://thomasmansencal.substack.com/p/improving-unreal-engine-api-documentation
- **S78** Epic forums, VERA in-editor AI agent for UE 5.7 and 5.8 (snippet). https://forums.unrealengine.com/t/tool-plugin-vera-in-editor-ai-agent-for-unreal-engine-5-7-5-8-7-llm-providers-viewport-vision-open-source/2738556
- **S79** SideFX, Houdini hython / command-line docs. https://www.sidefx.com/docs/houdini/hom/commandline.html
- **S80** SideFX forum, PyCharm autocompletion for hou.py (stub size analogue; snippet). https://www.sidefx.com/forum/topic/74269/
- **S81** Epic forums, "Task queueing best practices" (snippet). https://forums.unrealengine.com/t/task-queueing-best-practices/295396
- **S84** Epic, Unreal Python API docs labeled "(Experimental)" (snippet). https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/?application_version=5.4

(S34 and S82–S83 were not used.)
