> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 08 — Decision log: architecture decision records D1–D30

| | |
|---|---|
| **Document** | Decision log for [SPEC.md](../SPEC.md) and [TDD.md](../TDD.md) |
| **Date** | 2026-10-28; amended 2026-10-29 (§10) |
| **Status** | Accepted. Where an ADR names a gate (§3), the gate can reverse that part of the decision, and the fallback is already agreed. **Amended after the red-team review: §7 overrides the ADR text where they differ. §8 holds the UX rulings U1–U41. Amended again after the v1.0 review: §9 (RV-01–RV-24) overrides §7, §8 and the ADRs where they differ. The owner rulings after v1.1, §10 (PO-01–PO-03), override §9 and everything earlier where they differ** |
| **Decided by** | Chief architect, after a three-judge panel (principal UE/Slate engineer, pipeline-TD product lead, delivery/QA lead) |
| **Inputs** | Research [01](01-maya-charcoal-and-autodesk.md)–[06](06-user-voice-and-landscape.md); engine-source verification [07a](07a-verify-python-runtime.md)–[07e](07e-verify-editor-apis.md); the judges' votes and spot checks; the chief architect's own checks of the UE 5.8 source (§1.3) |
| **Engine** | UE 5.8 local install (read-only), embedded CPython 3.11.8 |
| **Product name** | **Sumi** (ADR D13). Identifiers used below: plugin `SumiScriptEditor`, editor module `SumiEditor`, engine-independent core library `SumiCore`, Python helper package `sumi_editor`, native extension module `_sumi`, TCommands context `SumiEditor` |

---

## 1. How decisions were made

### 1.1 Process

1. **Panel.** Three judges, each with a different lens, voted on D1–D13. Each vote gave a choice, a rationale, risks, a confidence level and a "would change my mind" condition. Each judge also listed decisions the brief had missed. Those lists became D14–D30.
2. **Evidence order.** First, the 5.8 engine source and the binaries that ship with it: 07a–07e, the judges' spot checks and the checks in §1.3. Second, research 01–06. Third, judge preference. Where research contradicted the source, the source won. Votes broke ties only between options with equal evidence. Several splits went against the majority (D2 exception class, D3 pause mechanism, D5 Epic-stub ingestion, D6 getter reads).
3. **Gates instead of guesses.** Some questions cannot be settled by reading source: field stability of a mid-script pump, server latency on our stubs, IME behaviour. Those are decided provisionally behind a numbered gate (§3) with numeric pass/fail criteria and an agreed fallback, so the schedule never waits on a debate.
4. **Dissent is recorded**, with the condition that would reopen it.
5. **Confidence.**
   - **High:** the mechanism is verified in source and there is no credible alternative.
   - **Medium:** the mechanism is verified, but the outcome depends on a gate or on user data.
   - **Low:** it cannot be verified this session (for example, a trademark search).

### 1.2 Citation conventions

- Engine paths are relative to the install root, as `Engine/<path>:<line>`.
- Research is cited as `R0n §x`, and verification files as `07x Qn`.
- Gates are `G1`–`G7` (§3).

### 1.3 New engine evidence gathered for this log

These facts were not in 07a–07e. The ADRs that rely on them are noted.

| # | Fact | Evidence | Used by |
|---|---|---|---|
| E1 | Reading a wrapped unreal property through its attribute (getset descriptor) **calls the BlueprintGetter UFunction** when the property has one. Attribute reads are not side-effect free. | `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperObject.cpp:424-430`; getter bound at `.../Private/PyWrapperTypeRegistry.cpp:1444` | D6, D30 |
| E2 | `get_editor_property(name)` reads the reflected property directly, **without** a getter call. Deprecated properties raise or warn. | `.../Private/PyWrapperObject.cpp:801-835` → `.../Private/PyGenUtil.cpp:1418-1440` | D6, D30 |
| E3 | Every run entered through `ExecPythonCommandEx` (literal code) attaches a log-capture lambda. It appends **every** `unreal.log*` line to `FPythonCommandEx::LogOutput` until the run returns. Forwarding a long run's output through `unreal.log` therefore grows memory without bound. | `.../Private/PythonScriptPlugin.cpp:1865` (RunString), `:1960` (RunFile); broadcasts at `.../Private/PyCore.cpp:1187, 1207, 1227` | D2, D19 |
| E4 | A Private-scope file run copies the **live** `__main__.__dict__` at execution time. Public-scope runs (used for `init_unreal.py`) execute in the console dict and **delete** `__file__` afterwards. The console dict is a copy of `__main__` taken at init. | `.../Private/PythonScriptPlugin.cpp:1942-1946`, `:1639`, `:1965`, `:1256` | D8 |
| E5 | The engine's CPython 3.11.8 stdlib re-raises only `SystemExit` and `KeyboardInterrupt` out of asyncio callbacks and tasks. Any other `BaseException` goes to the loop's exception handler, and the loop keeps running. unittest re-raises only `KeyboardInterrupt`. | `Engine/Binaries/ThirdParty/Python3/Win64/Lib/asyncio/events.py:85-87`; `.../Lib/asyncio/tasks.py:291-294`; `.../Lib/unittest/case.py:58` | D2 |
| E6 | The blocking-modal loop sleeps to at most 60 Hz under `SCOPE_STALL_DETECTOR_PAUSE` and never ticks FTSTicker. Python scripts enter it mid-script every day through `unreal.EditorDialog`. | `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2216-2279`; `Engine/Plugins/Editor/EditorScriptingUtilities/Source/EditorScriptingUtilities/Private/EditorDialogLibrary.cpp:21-23, 184` | D3 |
| E7 | An OS drag-drop (for example from Explorer) arrives through `OnOLEDrop` → `FSlateApplication::OnDragDrop`, guarded only by `Window->IsEnabled()`. Neither an input pre-processor nor the modal input filter is shown to stop it. | `Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:4458-4470`; `.../SlateApplication.cpp:7559`; modal filter `.../SlateApplication.cpp:4886-4901` | G2 |
| E8 | **Ctrl+Break cannot be bound.** UE maps only `VK_PAUSE`, and the input-key tables contain no `VK_CANCEL`, which is what Windows reports for Ctrl+Pause/Break. Bare Pause is PIE Toggle Play/Pause, and Esc is PIE Stop. | `Engine/Source/Runtime/InputCore/Private/Windows/WindowsPlatformInput.cpp:23-24` (and no `VK_CANCEL` in InputCore or ApplicationCore/Windows); `Engine/Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp:358, 364` | D8 |
| E9 | `RunString` processes pending Python-class reinstancing only on its success path. A runner that catches every exception therefore keeps `@unreal.uclass` reinstancing working after a failed run. | `.../Private/PythonScriptPlugin.cpp:1870-1879` | D14, D18 |
| E10 | Tick order is: `GEngine->Tick`, then Slate PlatformAndInput, then Slate TimeAndWidgets, then the core FTSTicker. A one-shot FTSTicker callback therefore runs after both Slate ticks, with `IsTicking()` false. | `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5859, 5921, 5991, 6103` | D2, D14 |
| E11 | Nils Söderman's VS Code extension has not stalled. Its GitHub repository is archived, but development moved to Codeberg (v1.11.0, 2026-06-22). | R03 B5; R05 Option D | D10 |

---

## 2. Summary of decisions

| ID | Decision (one line) | Lands | Conf. | Panel |
|---|---|---|---|---|
| D1 | Native Slate **SCodeView** over the UE-independent C++ **SumiCore**; also used for the REPL and output; stock text only for small fields and a per-buffer Plain-mode escape hatch; Scintilla's document layer is the core fallback; **no CEF** | Phase 0 → M1 | High | 3–0 for (a); split on REPL widget and fallback |
| D2 | **RunSupervisor** (watchdog + `Py_AddPendingCall` pump + public mid-frame Slate tick), **on by default**. Presentation "Live (docked)" if **G2** passes, else the "Run window" modal. **`ScriptCancelled(KeyboardInterrupt)`** | M1 | High (mechanism), Medium (presentation) | 2–1 docked vs modal; 2–1 on the exception base |
| D3 | debugpy attach and post-mortem inspector in M2. **In-editor bdb debugger in 1.0**, pausing in a **blocking modal "Paused" window** (never EnterDebuggingMode). Ships as Preview if **G4** fails | M2 / 1.0 | Medium | 1 for (b), 2 for (c) |
| D4 | LSP adapter with two supported servers: **Pyrefly** (provisional default) and **basedpyright**; the default is fixed by **G3**. ruff server always. ty experimental; jedi/pylsp rejected. Plugin ThirdParty binaries, our own transport | M2 | Medium | 2–1 on the default |
| D5 | Our own **runtime reflection walker** produces a split `.pyi` package. **Developer Mode is never required.** Epic's stub is a CI oracle only | M2 | High | 3–0; split on ingesting Epic's stub |
| D6 | Our own **safe resolver** in Python. It never invokes functions, properties or getset descriptors. 5 ms budget. Merge and ranking rules defined | M1 | High | 3–0 |
| D7 | C++ owns threads, Slate, latency, CPython internals and lifecycle. The Python helper owns the exec pipeline, bdb, resolver, stubs and reload. Lints and the stub transform run **out of process** | M1 | High | 3–0 |
| D8 | Session namespace = **UE console dict**. Isolated for Run File and shelves; no per-tab namespaces in 1.0. Never clear input. One **transaction per run**. Auto-print. linecache rules. **Corrected Charcoal keymap** | M1 | Medium | Several 2–1 splits |
| D9 | Phase 0 (6–8 weeks; G1–G7). The M1 MVP ships no third-party binaries. 1.0 is **Win64 plus a Linux Preview**; Linux supported in 1.1; macOS in 1.2 | — | Medium | Three-way split on platforms |
| D10 | No bridge in 1.0. Our own **authenticated loopback bridge in 1.1**. Never build on Epic Remote Execution | 1.1 | Medium | 2–1 (1.1 vs 1.2) |
| D11 | Optional MCP toolset module: read-only and propose-edit tools in **1.1**, run tools in **1.2** behind opt-in. Nothing in 1.0 | 1.1 / 1.2 | Medium | Three different timings |
| D12 | **Apache-2.0** on GitHub plus a **free Fab** listing. Permissive-only dependency policy | — | Medium | 3–0 |
| D13 | Name **Sumi**; fallback **Bistre** | — | Low | 3–0 |
| D14 | **Single run-entry path** (an invariant with tests) | M1 | High | Raised by the engine engineer |
| D15 | Engine and Python version support policy, and release channels | — | Medium | All three raised it |
| D16 | Python lifecycle states (disabled, shutdown, re-enabled, headless) | M1 | High | All three |
| D17 | Persistence: storage locations, journal, history, safe mode, format versions | M1 | High | All three |
| D18 | Reload model: deep reload plus revert-module-state, both manual by default | M3 | Medium | TD lead |
| D19 | Output pipeline and Output Log integration | M1 | High | Engine engineer, TD lead |
| D20 | Trust and security model, and the policy file | M4 | Medium | All three |
| D21 | Settings layering | M1 / M4 | High | All three |
| D22 | Test tiers, CI, performance budgets as release gates, definition of done | Phase 0 | High | Engine engineer, QA lead |
| D23 | Text rendering, fonts, IME, accessibility, encodings, large files | M1 | High | All three |
| D24 | Worker threads and the cooperative execution API | M1 / M3 | Medium | Engine engineer |
| D25 | Coexistence with other plugins, input pre-processors, tracers and test discovery | M1 | Medium | Engine engineer, QA lead |
| D26 | Diagnostics bundle, crash attribution, telemetry | M4 | Medium | Engine engineer, QA lead |
| D27 | Headless (out-of-process) run mode | 1.2 | Medium | Engine engineer, TD lead |
| D28 | Snippet and shelf formats, tiers and placement in the editor | M4 | Medium | TD lead, QA lead |
| D29 | Stability of the public scripting API | M1+ | Medium | TD lead, QA lead |
| D30 | Object inspector, Copy as Python, code insertion | M2 / M3 | Medium | TD lead |

> **Amended (§7).** D2's default presentation is now the Run window (RT-03). D4 supports one server at 1.0 (RT-12). D8's transaction default is off for Run File (RT-08). D9's Phase 0 exits on G1 and G2 (RT-10). D14's runs start from `OnEndFrame` (RT-01).

---

## 3. Gates

Every gate is automated unless marked manual. "Owner" is the role that signs off; the chief architect owns the overall go/no-go at the end of Phase 0, and the QA lead co-signs.

| Gate | When | Pass criteria | If it fails |
|---|---|---|---|
| **G1 Code view** | Phase 0, time box 6 weeks | (1) On a 100k-line file: model + incremental tokenize + invalidate **≤ 2 ms p95** per keystroke. (2) Paint **≤ 2 ms p95** for a full 4K viewport, **≤ 0.5 ms** when only the caret changes. (3) Inline composition with MS-IME Japanese, Microsoft Pinyin and Korean (TSF): a scripted 30-minute composition session with zero desyncs, plus the manual IME checklist. (4) Differential fuzzing: incremental tokenization equals a full retokenize over 10M random edits, and undo/redo round-trips | Model or latency failure: Scintilla's document layer plus Lexilla behind the SumiCore seam (D1). IME failure: Plain mode for affected users while the context is fixed. Never CEF |
| **G2 Run supervisor** | Phase 0; must pass before M1 exit to keep Live (docked) as the default | (1) Stop latency **p99 ≤ 250 ms** from click to exception for `while True: pass`, a tight list comprehension, a generator and deep recursion. **≤ 1.25 s** for loops with a bare `except:` or `except KeyboardInterrupt: pass`. (2) **1,000 run/stop cycles** across those loop shapes, including loops that call `unreal.*` UFunctions, with Content Browser and Sequencer open and a PIE session running: zero `check`, `ensure` or crash, and no stale `ScriptCancelled` in the next run. (3) A 60 s run printing 1,000 lines/s: our view repaints at ≥ 10 Hz and `LogOutput` stays bounded (E3). (4) Pump overhead ≤ 10% of wall time at the target rate; **fail above 15% at 10 Hz**. (5) Input-leak suite: typing, IME composition, clicks on toolbars, menus, Details, viewport and Content Browser, an **OS drag-drop from Explorer** onto the Content Browser and viewport (E7), Alt+F4, File > Exit, window close, PIE chords. Zero leaked effects. (6) A registered `unreal.register_slate_post_tick_callback` and `register_ticker_callback` fire **0 times** during a pumped run, and FTSTicker is never ticked by the pump. (7) An 8-hour streaming soak with memory growth ≤ 5% | "Run window" becomes the default presentation and Live (docked) becomes opt-in (D2) |
| **G3 Stubs and language server** | Phase 0; fixes the D4 default before M2 | **Walker:** on a fixture project with ≥ 100 enabled plugins, each slice ≤ 4 ms and the total ≤ 60 s of idle time; every `.pyi` parses with `ast` and type-checks clean under both servers; coverage against Epic's stub (Developer Mode fixture) **≥ 99.5%** of classes, functions and properties; type agreement **≥ 95%** for parameters, returns and properties. **Server:** RSS ≤ 1.5 GB; warm completion p95 ≤ 400 ms for `unreal.`, `unreal.EditorAssetLibrary.` and `unreal.EditorActorSubsystem().`; diagnostics ≤ 1 s after a typing pause; first completion ≤ 15 s after spawn, in the background. **Quality:** a golden corpus of 200 hover, completion and signature cases | No server passes: live completion serves `unreal.*`, and the LSP serves user code only (R05 Phase 0 #3). Walker fidelity still below the bar after 2 weeks of parser work: Epic's stub, produced by a background `-run=PythonOnlineDocs` child process, becomes the high-fidelity path (D5) |
| **G4 Debugger pause** | Spike in Phase 0; the fixture suite gates 1.0 at M3 exit | (1) 100 stepped breakpoints with the Paused window interactive and zero asserts or ensures. (2) While paused, a watch on `unreal.SystemLibrary.get_engine_version()` returns the real version, which proves we are not in debugging mode. (3) A breakpoint inside a user `unreal.ScopedSlowTask` with its dialog open: the Paused window is parented to that dialog and works. (4) A breakpoint inside a Python Slate tick callback is refused with a notice, and the callback continues. (5) An evaluate that changes an actor is recorded in the run transaction, and Undo Last Run reverts it. (6) Editor close and PIE start are refused while paused. (7) The suite is green for 2 consecutive weeks | The in-editor debugger ships in 1.0 as an opt-in **Preview**, debugpy is the supported path, and the debugger reaches GA in 1.1 (D3) |
| **G5 Key routing** | M1, then every build | With SCodeView docked in the Level Editor and actors selected, send every consume-list chord and every Charcoal chord. Assert: no actor deleted or duplicated, no level saved, no TRANSACTION UNDO, no tab closed, no PIE action. AltGr on German, French and Polish layouts types `@ { [ \`. Repeat in both run presentations | Blocks the release; fix the consume list |
| **G6 Crash recovery** | M1, then nightly | kill -9 at 10,000 random points during typing, autosave and runs: no buffer loses more than 2 s of typing; the journal is always readable (with fallback to the previous generation); recovery lists every buffer | Blocks the release |
| **G7 Numpad Enter** | M1 | Extended-key detection tells keypad Enter from main Enter in 100% of automated cases on Win64 (synthetic `WM_KEYDOWN` with and without bit 24), including over Remote Desktop | The Numpad Enter binding is removed. It **never** falls back to main Enter |

---

## 4. Corrections to the SPEC v0.9 draft

| SPEC location | Draft says | Decision |
|---|---|---|
| §6 Platforms | "macOS and Linux editors at 1.0" | 1.0 is Win64 (supported) plus a **Linux Preview**. Linux is supported in 1.1 and macOS in 1.2 (D9) |
| §9.2 state model | Stop via "Ctrl+Break" | Ctrl+Break cannot be bound (E8). Stop is **Esc** while a run is active, plus **Shift+F5** (D8) |
| §9.2 NativeWait | "Waiting for native call…" overlay | During a native call that does not pump Slate itself, the watchdog cannot paint anything. The state is shown at the next pump, and the post-run report states the native time. If the native call opens its own slow task, the engine's dialog is what shows (D2) |
| §9.2 Paused row | Banner plus docked panels interactive | The debugger UI lives in a modal **Paused** window in 1.0. Docked in-place panels come in 1.x (D3) |
| §9.3 Completion | "Ctrl+Space, Ctrl+Tab" | **Ctrl+Space** only. Ctrl+Tab is the global tab switcher (`Engine/Source/Editor/UnrealEd/Private/Toolkits/GlobalEditorCommonCommands.cpp:48`) |
| §9.3 Debug run | Shift+F5 | **F5** starts debugging and continues. Shift+F5 is Stop, matching VS Code and Visual Studio |
| §9.3 Replace / help | Ctrl+H help, Ctrl+R replace | **Ctrl+H** replace, **Ctrl+Q** quick docs. Help on the selection is Ctrl+Q plus `dir()`/`help()` in the context menu |
| §9.3 Numpad Enter | Run alias | Win64 only, through extended-key detection, and only if G7 passes |
| §7.4 save on run | Implied save before run | **Journal snapshot** before every run. Only Run File saves to disk first. "Save on execute" is an opt-in setting (D8) |
| §7.17 / §7.21 | Bridge and AI timing open | Bridge in 1.1 (D10); MCP tools in 1.1/1.2 (D11) |
| §12 risks | "Stable-ABI calls" | Only `python311.lib` ships, so we rebuild at every Python bump. Stable-ABI-only calls reduce source churn; they do not give binary compatibility (D15) |
| A3 / A4 | Language server unnamed; storage "outside the project" | Pyrefly is the provisional default (D4). Storage locations are in D17 |
| §4.3 metric | "Stop … within 250 ms" | Applies to pure-Python loops that do not swallow the exception (G2). Telemetry is opt-in only (D26) |
| §2.1 pain 10 | "Buffered output" | The cause is Slate not ticking, not buffering (07a Q4) |

---

## 5. Architecture decision records D1–D13

### D1. Code editing surface

**Status:** Accepted. **Confidence:** High. **Lands:** Phase 0 spike, then M1.

**Decision.**

**SCodeView and SumiCore.** SCodeView is a custom `SLeafWidget`, line-virtualized and custom-painted. It sits on top of **SumiCore**, a C++ library with no UE dependency. SumiCore contains:
- a piece table and an operation-based undo tree;
- an incremental line-state Python 3.11 lexer;
- search;
- the multi-caret and folding data model (the UI for both comes in M2);
- Myers diff;
- LSP/JSON-RPC framing and client;
- the snippet engine.

**Rendering.**
- Shape per line or token run with `FSlateFontCache::ShapeUnidirectionalText` (KerningOnly).
- Keep our own LRU of `FShapedGlyphSequenceRef`, keyed by line hash, style and DPI scale.
- Paint with `FSlateDrawElement::MakeShapedText`, culled to the visible lines.

**Input.**
- Our own `ITextInputMethodContext`. It answers queries from the piece table in O(line), uses `\n`-only ACP indices and reports minimal `NotifyTextChanged` ranges.
- An OnKeyChar path that always accepts multi-character commits.
- A synchronous Handled reply for every chord in the consume list (D8).

**One widget everywhere.** A read-only line-source interface lets the same widget draw the **output view** and the **debugger source view**. A prompt mode makes it the **REPL input**. That gives one keymap, one highlighter and one completion host.

**Stock text widgets** (`SMultiLineEditableText`, `SEditableText`) are used only in two places:
- small fields: the find box, rename, form fields;
- a per-buffer **Plain mode** escape hatch, with live syntax update off so Epic's IME and the per-line `InsertAt` path apply. It is for small and medium files.

**Phase 0 harness.** An internal dogfood harness on the stock widget lets pilots exercise the execution stack before SCodeView is ready. It never ships.

**Fallbacks if G1 fails.**
- (a) Model or latency failure: replace SumiCore's text model and lexer with Scintilla's document layer and Lexilla (HPND), behind the core seam. Our renderer and IME context stay.
- (b) IME failure: Plain mode for affected buffers while the context is fixed.
- CEF is not a fallback.

**Alternatives rejected.**

**(b) Monaco in SWebBrowser and (c) CodeMirror 6 in CEF.** Three source facts rule them out:
1. **Frozen when it matters.** `FWebBrowserSingleton` is an `FTSTickerObjectBase` (`Engine/Source/Runtime/WebBrowser/Private/WebBrowserSingleton.h:53-55`). It pumps CEF only from that ticker (`Engine/Source/Runtime/WebBrowser/Private/WebBrowserSingleton.cpp:839, 934-938`; external pump `:340-342`). Our run pump and pause loop must never tick FTSTicker, because a nested Tick trips `checkf` (`Engine/Source/Runtime/Core/Private/Containers/Ticker.cpp:101`). A web buffer would therefore freeze in every run pump, every debugger pause and every user slow task.
2. **Unsafe keys.** CEF's OnKeyDown always returns handled, and re-injects unhandled keys later (`Engine/Source/Runtime/WebBrowser/Private/CEF/CEFWebBrowserWindow.cpp:1212-1228, 1302-1327`). A leaked chord does real damage:
   - Ctrl+Z becomes TRANSACTION UNDO (`Engine/Source/Editor/MainFrame/Private/Frame/MainFrameActions.cpp:184`);
   - Ctrl+D, Delete and Ctrl+S duplicate or delete actors, or save the level (`Engine/Source/Editor/LevelEditor/Private/LevelEditor.cpp:1089-1095`);
   - Ctrl+W closes the major tab (`Engine/Source/Runtime/Slate/Private/Framework/Docking/TabCommands.cpp:9`).
3. **Rendering and accessibility gaps.**
   - Off-screen rendering defaults to 24 fps (`Engine/Source/Runtime/WebBrowser/Public/IWebBrowserSingleton.h:62`).
   - 60 fps needs D3D accelerated paint, which is disabled under automation (`Engine/Source/Runtime/WebBrowser/Private/CEF/CEFWebBrowserWindowRHIHelper.cpp:95-103`), so CI would test a different path from users.
   - There is no accessibility handler, and IME is compiled out on Linux (`Engine/Source/Runtime/WebBrowser/Private/CEF/CEFImeHandler.h:7`).
   - A CEF 147 jump is pending (`Engine/Source/ThirdParty/CEF3/CEF3.build.cs:16-45`).

**Resolving R03 against R05.** R03 chose Monaco because it assumed two things:
- EnterDebuggingMode would keep the panel live. In fact it makes `ProcessEvent` a no-op (`Engine/Source/Runtime/CoreUObject/Private/UObject/ScriptCore.cpp:2073-2083`) and refuses under any modal (`.../SlateApplication.cpp:3864-3891`).
- A one-week spike could settle keys and IME. The key-routing and frame-rate facts above already fail R03's own spike gate without running it.

So R05's recommendation stands (E1 weighted 85.4 against B at 63.0, R05 §Scoring).

**(d) Stock text or Epic's CodeEditor plugin for a public milestone.** Every keystroke costs O(document): a serialise and compare, a full-text undo snapshot (up to 99 levels), an ICU re-tokenize, a token deep copy and a re-shape of every line (`Engine/Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp:3257-3314`; `Engine/Source/Runtime/Slate/Private/Framework/Text/TextLayout.cpp:2801-2809`). Everything built on it would be rewritten, and it would look like the "scratchpad" products in R06 §2. The CodeEditor plugin exports nothing (07c Q3).

**(e) A full Scintilla port with a Slate platform layer, as the primary path.** We would still write the IME glue and a whole platform layer. Only its document layer is worth keeping, as the fallback.

**Rationale.**
- **Precedent.** Epic's own 5.8 Terminal is a custom `SLeafWidget` that paints with MakeText (`Engine/Plugins/Experimental/Terminal/Source/Terminal/Public/STerminal.h:24`; `.../Terminal/Private/STerminal.cpp:358`). It is tested with Automation Specs (`Engine/Plugins/Experimental/Terminal/Source/TerminalTests/Private/Tests/STerminalTests.cpp:95`).
- **Muscle memory is mostly keyboard ownership**, which needs synchronous Handled/Unhandled per chord (R01 Steal 1–4; R05 rationale 2).
- **Testability.** A UE-independent core gets millisecond unit tests, golden corpora and differential fuzzing (R05 §2.10).
- **Upgrade survival.** It depends only on long-stable SlateCore APIs (R05 rationale 1).

**Dissent.**
- *Engine engineer:* wanted the stock widget for the REPL input (R05's A1 REPL line). **Overruled.** One code path halves the IME and keymap work, and the REPL is just a prompt-mode configuration. Plain mode remains if G1 fails.
- *TD lead:* reject Scintilla entirely. *QA lead:* keep it only as fallback #2, for feature velocity. **Resolved:** Scintilla appears only as the core-layer fallback for G1 model failures; an IME failure has a different remedy.
- Nobody argued for CEF.

**Consequences.**
- SCodeView is the largest build item (R05 R1). The M1 widget is thin: single caret, highlighting, gutter, find/replace, indent, comment, brace pairing, zoom, Windows IME and undo. The multi-caret UI, folding UI and diff view come in M2.
- We lose Monaco's free diff editor, snippet UI and sticky scroll. We build them or defer them.
- Linux IME is commit-only, and the macOS context is deactivated during runs (D23). The accessibility ceiling is in D23.
- Keystroke-to-photon is at least one editor frame (R05 §2.9).

**Verification still needed.** G1 and G5. Column math must use measured advances, because CJK fallback glyphs do not sit on the monospace grid.

---

### D2. Run, Stop and UI liveness during long runs

**Status:** The mechanism is accepted. The default presentation is provisional on G2. **Confidence:** High for the mechanism, Medium for the presentation. **Lands:** Phase 0, then M1.

**Decision.**

**1. RunSupervisor (C++).** It is always on, unless a studio uses the kill switch.
- Every run starts from a one-shot FTSTicker callback and goes through the single entry path (D14).
- The **watchdog thread** arms about 300 ms after a run starts. It keeps **at most one** `Py_AddPendingCall` outstanding. It requests pumps adaptively at 10–20 Hz (target 15 Hz), holding pump cost to ≤ 10% of wall time. Most short runs end before it arms.
- The **pending-call callback** runs in C++ on the game thread with the GIL held. It drains output and then, unless it is re-entered, runs the pump:
  1. `Py_BEGIN_ALLOW_THREADS`
  2. `PumpMessages()`, only if no modal is active
  3. `FSlateApplication::Tick(All)`
  4. enqueue an RHI EndFrame, only if `!bWasTicking`
  5. `Renderer->Sync()`
  6. `Py_END_ALLOW_THREADS`

  The whole pump runs under `FScopedPreventDebuggingMode` (`Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:954-957`), `SCOPE_STALL_DETECTOR_PAUSE` and a re-entrancy counter.
- The pump **never ticks FTSTicker or GEngine**. It does nothing while `IsTicking()` is true, while the debugger is paused (D3) or when re-entered.
- This is the engine's own public mid-frame recipe (`Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp:419-439`; `Engine/Source/Developer/SourceControl/Private/ScopedSourceControlProgress.cpp:234-254`).

**2. Presentation.** This is a setting, and studio policy can force it. Both presentations ship in 1.0, and G2 picks the default before M1 exits.

**Live (docked)**, the provisional default:
- Our tab keeps painting: a run banner (elapsed time, the line at the last pump, state), streaming output and Stop.
- An `IInputProcessor`, registered at index 0 in an early bucket only for the run, swallows key and mouse events. The exceptions are hits on Stop, the run banner and the output view, found with `LocateWindowUnderMouse` and a widget-path test (the Translation Picker pattern).
- Keyboard focus moves to the run banner, and all buffers become read-only.
- An `IMainFrameModule::RegisterCanCloseEditor` veto is in place (`Engine/Source/Editor/MainFrame/Public/Interfaces/IMainFrameModule.h:231`).
- Our widgets that call into Python (completion, hover, inspector) are disabled.

**Run window:**
- A non-blocking modal, `AddModalWindow(..., bSlowTaskWindow=true)` (`.../SlateApplication.h:428`), opened after about 1 s. It uses the same mechanism as source-control progress (`.../ScopedSourceControlProgress.cpp:42-189`) and shows an output tail, elapsed time and Stop.
- The platform filters input, WM_CHAR included (`.../SlateApplication.cpp:4886-4901`; `Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:3195-3205`). Only the modal is drawn (`.../SlateApplication.cpp:1476-1497`), and the editor refuses to close.
- It is deliberately **not** an `FScopedSlowTask`: `GIsSlowTask` would suppress users' own `make_dialog`, and repaint is throttled to 5 Hz.

**Off:** stock blocking behaviour plus the cooperative API. This is the kill switch.

**With a user's modal or slow task already open:**
- We keep pumping, which keeps their dialog painting.
- Only keyboard pre-processing reaches us, so Esc stops the run.
- Stop also calls `FFeedbackContextEditor::OnUserCancel()` on `GWarn` (`Engine/Source/Editor/UnrealEd/Public/FeedbackContextEditor.h:53`), so their `should_cancel()` returns true.

**3. Stop and cancellation.**
- **The exception class** is `class ScriptCancelled(KeyboardInterrupt)` in `sumi_editor`. C++ holds a strong reference to it.
- **First delivery.** Stop sets a flag. The next pending call checks the run ID and the flag, calls `PyErr_SetObject(ScriptCancelled, …)` and returns -1. The callback must set an exception before returning -1, or CPython raises SystemError (07a Q1).
- **Re-delivery.** If the run is still alive 250 ms after a delivery, re-deliver every 250 ms. This defeats bare `except:` and `except KeyboardInterrupt: pass`. From 1 s on, also deliver through `PyThreadState_SetAsyncExc(game_thread_id, …)` each period. That is an alternate channel, independent of CPython's 32-slot pending-call queue.
- **Guards.** Every delivery checks the run ID. At run end: invalidate the run ID, call `SetAsyncExc(tid, NULL)` and drain the queues.
- **Never** `PyErr_SetInterruptEx`, signals or killing anything.
- **Honest states.**
  - The UI says "Stopping…" until control actually returns.
  - A native call that does not pump Slate freezes the UI, and the watchdog cannot paint from its own thread. Afterwards we show, for example: "the last native call took 12.4 s; Stop took effect when it returned".
  - The run ends in a distinct **Cancelled** state with its location. `finally` blocks and context managers still run, and "Undo this run" is offered (D8).

**4. Cooperative tier (M1).**
- `sumi_editor.progress(i, n, msg)` and `check_cancel()`.
- A run-scoped shim for game-thread `time.sleep` that sleeps in slices, so Stop works during a sleep. It is active only on the game thread during our runs.
- The `@task` generator mode comes in M3 (D24).

**5. Output during runs.** See D19: a C-API writer feeds an MPSC queue, and lines are forwarded in batches through C++ `UE_LOG`, never through `unreal.log` (E3).

**Alternatives rejected.**
- **(b) An auto-wrapping slow-task dialog as the default.** Only the modal draws, so our docked output is dead. An engine slow task repaints only when progress is reported, throttled to 0.2 s (`Engine/Source/Runtime/Core/Private/Misc/FeedbackContext.cpp:184-197`). The idea is kept as the Run window profile, built on our own modal rather than an engine slow task.
- **(c) settrace line checks.** They cost every line, defeat the 3.11 specializing interpreter, and fight the debugger, debugpy and coverage for the single trace slot (R05 §2.2; R03 C4).
- **(d) Cooperative only.** It fails pain #1. It is kept as Off.
- **SIGINT or `PyErr_SetInterruptEx`.** Python is initialized with `install_signal_handlers = 0` (`Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:1178`). Before `import signal`, the call logs a spurious "Signal 2 ignored" error (07a Q1).
- **`ScriptCancelled(BaseException)` that does not subclass KeyboardInterrupt (QA lead).** Rejected on E5. asyncio sends any non-KeyboardInterrupt BaseException raised in a callback to the loop's exception handler and keeps running, and tasks store it rather than propagating (`.../Lib/asyncio/events.py:85-87`; `.../Lib/asyncio/tasks.py:291-294`). unittest re-raises only KeyboardInterrupt (`.../Lib/unittest/case.py:58`). Common libraries would swallow a plain BaseException. User code that swallows KeyboardInterrupt is handled by re-delivery.

**Rationale.**
- **Value.** Pain #1 is Critical, with 6 sources (R06 §3), and principle 2 is "never strand the user".
- **Feasibility is verified.**
  - The 07a probe on the shipped `python311.dll` showed pending calls run on the initializing thread, and Python is initialized on the game thread.
  - The GIL is released around every reflected call (`.../Private/PyUtil.cpp:644-647`), so the watchdog and `SetAsyncExc` work during native calls; delivery happens on return.
  - Foreign Python Slate and ticker callbacks are gated while EvalString sets `GIsRunningUserScript` (`.../Private/PythonScriptPlugin.cpp:1831`; `.../Private/PySlate.cpp:31`).
- **Not ticking FTSTicker** also defers CEF, the HTTP server behind MCP (`Engine/Source/Runtime/Online/HTTPServer/Public/HttpServerModule.h:24-26`) and our own bridge. Nothing can re-enter a script.
- **Docked versus modal, on evidence.**
  - Both use the same engine-proven pump.
  - The modal gets four protections from the platform layer: KeyChar and IME filtering, mouse filtering for other windows, close refusal and draw isolation. Docked mode must build each of these: the pre-processor, the focus move, read-only buffers and `RegisterCanCloseEditor`.
  - Each of those is small, public API and testable on its own. None depends on speculative engine behaviour.
  - What the source cannot settle is soak stability, so G2 decides. Until then the product's preferred presentation is the default: output and tracebacks stay in the user's own layout, and users' own slow-task dialogs are not suppressed. The modal fallback is already agreed.
  - Neither profile is shown to block an OS drag-drop (E7), so G2 tests it.

**Dissent.**
- *QA lead:* the modal should be the default, because it has fewer failure classes and is the proven profile. **Recorded.** It becomes the default automatically if G2 fails, and the QA lead's input-leak list is adopted whole into G2.
- *QA lead:* `ScriptCancelled` must not subclass KeyboardInterrupt. **Overruled** on E5.
- *TD lead:* a visible "Force stop" button at about 3 s. **Rejected.** `SetAsyncExc` acts at the same bytecode boundary as the pending call, so a separate button would promise a capability we do not have. The alternate channel fires automatically instead.
- *TD lead:* 15–30 Hz. **Adopted instead:** adaptive 10–20 Hz with a cost cap, because in docked mode every pump paints the whole editor.

**Consequences.**
- `Renderer->Sync()` runs queued `AsyncTask(GameThread)` work mid-script (`Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp:2562-2566`). Our own async completions, such as LSP results, are applied only in idle state.
- Python callables bound to unreal delegates are not gated (`.../Private/PyWrapperDelegate.cpp:21-37`). In docked mode, the whitelist must block every UI path that could fire them.
- Viewports do not render during a run, because GEngine is not ticked. The banner says so.
- The SPEC's NativeWait state changes (§4).
- On macOS, the IME context is deactivated during runs (`Engine/Source/Runtime/ApplicationCore/Private/Mac/CocoaTextView.cpp:253-275`).

**Verification still needed.** G2.

---

### D3. Debugger

**Status:** Accepted, with a pre-agreed descope rule. **Confidence:** Medium. **Lands:** debugpy and post-mortem in M2; the in-editor debugger in 1.0 (M3).

**Decision.**

**M2: "Debug in VS Code / PyCharm" (debugpy attach).**
- debugpy is vendored under the plugin's `Content/Python/Lib/site-packages`. It is never installed into the engine interpreter; Epic's `debugpy_unreal.install_debugpy()` pip-installs into Program Files (07d Q7).
- We call `debugpy.configure(python=unreal.get_interpreter_executable_path())` and listen on 127.0.0.1 on a random port.
- We never call `wait_for_client` on the game thread; a C++ ticker polls `is_client_connected` instead.
- `redirectOutput` is on, and we generate the `launch.json` and PyCharm configuration.
- A banner says the editor is frozen while paused in the external debugger.

**M2: post-mortem inspector.**
- "Inspect" on any traceback opens the frames and locals of the failed run.
- It is read-only and UObject-aware, with lazy views and no getters (D30).
- Frames pin UObjects, so they are released at the next run, on a map change, or when dismissed.

**1.0: in-editor debugger.**
- **Engine.** A bdb engine in `sumi_editor`, behind a DAP-shaped interface and a tracing abstraction that is ready for `sys.monitoring`.
- **Features at 1.0:**
  - line, conditional and hit-count breakpoints, and logpoints;
  - `breakpoint()` through `sys.breakpointhook` during our runs;
  - break on uncaught exceptions in user code;
  - step in, over and out;
  - call stack, lazy variables, evaluate in frame, and watches.
- **The pause is a blocking modal "Paused" window**, using the non-slow-task `AddModalWindow` path.
  - The window holds the whole debugger UI: stack, variables, watches, an evaluate console, a read-only SCodeView of the paused file, and Continue, Step and Stop.
  - It is parented to `GetActiveModalWindow()` when one exists, so pausing inside a user `ScopedSlowTask` works.
  - The GIL is released around `AddModalWindow`; nested Python such as an evaluate re-takes it.
  - The pause runs under our `FScopedPreventDebuggingMode`. The RunSupervisor pump is inert while paused.
  - It **never** uses `EnterDebuggingMode`.
- **Pause rules.**
  - Pauses happen only during our Debug runs, and only on the game thread.
  - A pause is refused, with a visible notice, inside Python Slate or ticker callbacks (detected by our wrapper flag) and while `FSlateApplication::IsTicking()` is true.
  - Breakpoints on worker threads are skipped in 1.0, with a notice.
- **One trace owner.** Starting debugpy disables our tracer, and the reverse. A foreign `sys.gettrace()` is detected before a debug run starts.

**Descope rule.** If G4 is not green at M3 exit, 1.0 ships the in-editor debugger as an opt-in **Preview**, debugpy is the supported path, and the debugger reaches GA in 1.1.

**1.x:**
- in-place docked panels on a non-modal pause loop, once G2 field data exists;
- set-variable (needs `PyFrame_LocalsToFast` on 3.11);
- worker-thread pauses through an event wait;
- breakpoints in code outside our runs (EUW buttons, menus);
- a `sys.monitoring` backend once UE ships Python 3.12 or later.

**Alternatives rejected.**
- **EnterDebuggingMode.** Its only caller is the Kismet debugger (`Engine/Source/Editor/UnrealEd/Private/Kismet2/KismetDebugUtilities.cpp:817`). It has five problems:
  - it sets `GIntraFrameDebuggingGameThread`, so `ProcessEvent` returns early (`.../ScriptCore.cpp:2073-2083`) and every `unreal.*` evaluate or watch silently returns default values;
  - it refuses under any modal, including slow tasks (`.../SlateApplication.cpp:3864-3891`);
  - it has no throttle;
  - its EndFrame is unguarded;
  - PIE close and Blueprint Resume can resume it externally (07b Q1).
- **A custom docked pause loop at 1.0** (TD lead; also the engine engineer's 1.1 design). It is feasible on public API, but only medium-confidence outside slow-task contexts (07b Q6), and it needs four context handlers. The blocking modal is the engine's most exercised nested loop (E6). Scripts already enter it mid-run through EditorDialog, and it handles the "modal already open" case natively.
- **(b) debugpy only at 1.0.** A debugger is a Must for the PyCharm claim (R03 Part A 19a). It is pain #5 (R06 §3), and CE2 has none (R01). The value case wins as long as the risk is gated.

**Dissent.**
- *Engine engineer:* (b) for 1.0 and the in-editor debugger in 1.1. **Partly accommodated.** The modal pause removes the extra re-entrancy surface they objected to: no custom loop and no four handlers. Their "pull into 1.0" conditions are now G4, and if G4 fails, their plan is what happens.
- *TD lead:* docked in-place panels at 1.0. **Deferred to 1.x.** Journey J4 is met with the panels inside the Paused window.

**Consequences.**
- The Paused window floats, which feels less like PyCharm. It remembers its size and position.
- Evaluate runs on the game thread and can modify editor state. Edits land in the run's transaction, and the console says "may modify editor state".
- The debugger is disabled in unattended sessions, because unattended mode cancels blocking modals (`.../SlateApplication.cpp:2134-2140`).
- The modal loop pumps messages, so Windows does not ghost the editor while paused.
- settrace overhead applies while debugging.
- debugpy needs cp311 compiled speedups, which must be refreshed at the 3.13 bump. Its adapter subprocess runs in our Job Object.
- The pydevd licence must be checked (D12).

**Verification still needed.** G4. Also measure trace overhead on a 1M-iteration loop with one breakpoint set, against an acceptance threshold of ≤ 3× slowdown.

---

### D4. Static language server, lint and format

**Status:** Accepted; the default server is provisional on G3. **Confidence:** Medium. **Lands:** M2. The transport and our own checker land in M1.

**Decision.**

**The LSP adapter in SumiCore:**
- per-server configuration mapping (`stubPath`, `extraPaths`, init options);
- version-stamped documents; stale results are dropped;
- request cancellation at 1.5 s;
- restart with backoff.

**Supported servers at 1.0: exactly two.**
- **Pyrefly** is the provisional default: MIT, a single Rust binary, no Node.
- **basedpyright** is the supported alternate. It bundles Node.
- G3 picks the shipped default: among the servers that pass the absolute thresholds, the one with the higher golden-corpus score. Within 3 points, Pyrefly wins.
- **ty** is experimental: user-supplied path, unsupported until it reaches a stable 1.0.
- **jedi-language-server and pylsp** are not offered.

**ruff server is always on**, as a second LSP:
- a quiet default rule set: syntax errors, pyflakes `F` rules and a few bug-risk rules;
- formatting on demand;
- project `ruff.toml` and `pyproject.toml` are honoured;
- it still provides syntax and undefined-name diagnostics when the type server is down.

**Unreal-specific lints come from our own checker, `sumi-check`.** It is our own Python, running out of process on the engine's `python.exe -I` and speaking LSP diagnostics over the same transport. It flags:
- `os.rename` or `shutil.move` on Content paths;
- `unreal.*` use inside threading targets;
- `time.sleep` or `asyncio.run` loops on the game thread;
- attribute assignment where `set_editor_property` is needed;
- unknown `unreal.*` names.

It ships in **M1**, which also proves the transport without any third-party binary.

**Shipping.**
- Version-pinned, per-platform plugin ThirdParty binaries (RuntimeDependencies), with SHA-256 hashes in a manifest. **Never `PythonRequirements`.**
- Our own `FLspTransport` on an `FRunnable`:
  - `CreateProc(FProcessStartInfo)` with **separate** stdin, stdout and stderr pipes;
  - a kill-on-close Job Object assigned immediately, with a native `CREATE_SUSPENDED` path if grandchildren escape;
  - binary `ReadPipeToArray` and `WritePipe(uint8*)` with Content-Length framing;
  - the game thread only posts to and reads from queues;
  - stderr goes to a log file.
- On Mac and Linux: `PR_SET_PDEATHSIG` and a parent-PID watch.
- Servers spawn on first buffer open, never at editor start.

**Policy controls.** Studio policy can disable language services, pin a server, or point to approved binaries ("bring your own binary"). Editing and running never depend on language services.

**Alternatives rejected.**
- **basedpyright as the default now (TD lead).** No benchmark on UE stubs exists (R03 C2 open question). Positron's numbers (basedpyright 3.1 s / 112 ms, Pyrefly 5.8 s / 190 ms) were not measured on UE stubs. Node adds AV, allow-list and notarisation friction, and size. It stays as the supported alternate.
- **ty as the default.** It is a 0.0.x beta with breaking changes between versions (R03 C2).
- **jedi-language-server / pylsp.** In-process GIL contention, maintenance mode, and a 10 MB crop (R03 C2).
- **PythonRequirements.** When packages are missing, the engine runs a synchronous pip install on the game thread at startup (`.../Private/PythonScriptPlugin.cpp:1353-1371`). It installs into Intermediate, is wiped when the Python version changes, and needs every transitive wheel hash-pinned per platform (07d Q5).
- **The engine's process helpers.**
  - `CreateProc` merges stderr into stdout (`Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp:511-518`) and assigns no Job Object (`:1076-1097`).
  - `WritePipe(FString)` appends `\n` (`:2052`).
  - `FInteractiveProcess` splits output into lines, drops empty lines and logs every line (`Engine/Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp:108, 133-146`).
  - Any of these corrupts JSON-RPC framing.

**Dissent.** *TD lead:* basedpyright by default, for consistency with pyright and Pylance users. **Accommodated through G3.** The gate encodes their switch criterion (quality on the golden corpus), and studios can pick basedpyright by policy.

**Consequences.**
- Toolchain churn: versions are pinned per release, with scheduled re-qualification on the G3 suite.
- Two supported servers double the integration tests; the matrix is capped at two.
- Fab's policy on bundled executables is unverified (D12), and the binaries need code signing.

**Verification still needed.**
- G3.
- Fab's executable policy, before M2.
- Pyrefly's LSP command and feature list (R03 C2 marks `pyrefly lsp` as UNVERIFIED).

---

### D5. Stub strategy for the `unreal` module

**Status:** Accepted. **Confidence:** High. **Lands:** M2.

**Decision.** Our runtime walker is the only production path at 1.0. **Developer Mode is never required and never toggled.**

**Stage 1: snapshot (in the editor).** `sumi_editor` runs on the game thread, on idle ticks, in slices of at most 4 ms.
- It takes a read-only snapshot of every `sys.modules['_unreal_<Module>']`: names, type dict entries, descriptor kinds, `__doc__` and bases.
- It **never calls** getters, properties or ScriptConstant functions. Epic's own generator calls ScriptConstant UFunctions on CDOs (`.../Private/PyWrapperTypeRegistry.cpp:3590-3597`).
- It filters out the `PyTest*` fixture types.
- It groups types by `_unreal_*` module, because wrapped types report `__module__ == 'builtins'`.
- It writes JSON.

**Stage 2: transform (out of process).** Our transformer runs on the engine's `python.exe -I`.
- It parses Epic's docstring grammar:
  - the method first line `x.name(p, q=default) -> Ret`;
  - Google-style `Args:` and `Returns:` blocks;
  - property docstrings of the form `(Type): [Read-Write]`;
  - the class "Editor Properties" list;
  - the "C++ Source" block.
- It emits a split package: `unreal/__init__.pyi` with explicit `from ._x import Foo as Foo` re-exports, plus one `.pyi` per C++ module or plugin, with bases ordered before subclasses.
- If parsing fails, it degrades to untyped stubs.

**Cache and refresh.**
- The cache lives at `<Project>/Intermediate/Sumi/Stubs/<key>/`. The key combines the engine build, a hash of the enabled-plugin set and the project module hashes.
- Regeneration happens per changed module, after a debounced `FModuleManager::OnModulesChanged`.

**One package, two consumers.** The same package feeds our LSP and "**Export stubs for VS Code / PyCharm**". Export writes the package and a `pyrightconfig.json`, `pyrefly.toml` or `.vscode` settings snippet into a folder the user chooses.

**Epic's stub.** Epic's `Intermediate/PythonStub/unreal.py` is never analysed by our LSP; it is excluded from the workspace. It serves as the CI oracle on fixture projects with Developer Mode on.

**One docstring parser.** The parser is a single module shared with live hover and signature help (D6).

**Alternatives rejected.**
- **Post-processing Epic's `unreal.py` as the default.** It requires Developer Mode (`.../Private/PythonScriptPlugin.cpp:2008-2021`), which then regenerates about 2 s after every module load or unload, behind a modal slow task (`.../Private/PyWrapperTypeRegistry.cpp:2979-2981`). There is no on-demand API (07d Q1), and `-run=PythonOnlineDocs` writes into the read-only engine SphinxDocs folder.
- **PyGenUtilAccessor.** It sits behind UBT's Internal include scope (`Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Internal/PyGenUtilAccessor.h:18`) and covers too little (07d Q3).
- **Ingesting Epic's stub whenever it exists** (TD lead primary; engine engineer as optional enrichment). That creates two production paths, so completion would differ between machines depending on a Developer Mode flag: a support-ticket generator. The walker has to pass the fidelity bar anyway.

**Rationale.**
- **Epic-exact grouping.** Each C++ module maps to a real `_unreal_<module>` Python module (`.../Private/PyWrapperTypeRegistry.cpp:2044-2058`).
- **Safe to walk.** Wrappers are generated eagerly at registration (`:981-1045`), so walking them is a pure read.
- **Enough information.** Docstrings carry types, defaults, editor-property lists and plugin ownership (07d Q2).
- **Size limits go away.** Splitting removes the 50 MB, complexity-768 and 10 MB ceilings (R03 C2).
- **Low game-thread cost.** The out-of-process transform keeps game-thread and GIL cost to the snapshot alone.

**Dissent.** *TD lead:* use Epic's stub when present. **Kept as a fallback trigger:** if G3's fidelity bar still fails after about 2 weeks of parser work, post-processing Epic's stub becomes the high-fidelity path. The stub would be obtained from a background `UnrealEditor-Cmd -run=PythonOnlineDocs` child process, which writes it regardless of Developer Mode (07d correction), never by toggling Developer Mode.

**Consequences.**
- The docstring format is an undocumented contract. It needs golden tests per engine version and a canary on previews.
- Blueprint and Python-generated classes appear only once loaded; live completion (D6) covers them.

**Verification still needed.** The stub half of G3.

---

### D6. Live completion engine and merge

**Status:** Accepted. **Confidence:** High. **Lands:** M1; the asset-path completer in M2.

**Decision.**

**Where and how fast.** Our own resolver in `sumi_editor`, on the game thread. Each request has a **5 ms hard deadline**; a partial result is flagged as incomplete. The popup appears within **≤ 30 ms**.

**Policy** (stricter than IPython's "limited" level):
- **Root.** Resolve the root name in the buffer's execution namespace: the Session, the last Isolated namespace, or frame locals while paused.
- **Walk.** Use only `inspect.getattr_static`.
- **Subscripts.** Allow only literal subscripts, and only on `list`, `tuple`, `dict`, `unreal.Array` and `unreal.Map`. These are C-level reads.
- **Never** call functions, properties, a descriptor's `__get__`, or `__getattr__`. That **includes unreal getset descriptors**, because they can run a BlueprintGetter UFunction (E1).
- **Unreal properties.** When a step is an unreal property:
  - If it is an **object-reference** property, the resolver may read the value through `get_editor_property`. That reads the reflected property directly without calling a getter (E2), and gives the concrete class.
  - Deprecated properties, containers and structs are never read; they resolve at type level from the docstring's `(Type)`.
- **No `inspect.signature`** on unreal callables; it raises ValueError on them (`.../Private/PyMethodWithClosure.cpp:470-483`).

**Unreal-aware completers.**
- Editor-property names inside `get_editor_property('…')` and `set_editor_property('…')`, taken from the class's "Editor Properties" docstring list (M1).
- Enum members (M1).
- `/Game` asset paths inside string arguments to `load_asset`, `load_object` and `find_asset`, from an async, cached Asset Registry query (M2).
- Fuzzy matching that understands snake_case.

**Signature help and hover** come from the shared docstring parser (D5).

**When it runs.**
- `unreal.*` chains: in every buffer.
- Session names: only in buffers that execute in the Session.
- Never during runs.
- Results are cached per run generation.

**Merge.** The merge is a pure C++ function in the host.
- LSP and live results are requested in parallel, and the popup opens on whichever answers first.
- Late results merge in without moving the selected item or anything above it.
- Items are keyed by label. When both sources have an item, the static source supplies the signature and docs, and the live concrete type is added as an annotation. Live-only items get a "runtime" badge.
- **Ranking:**
  - REPL and scratch buffers: locals and parameters, then live members, then static members, then keywords and snippets.
  - File buffers: static first, unless the root name is bound in the Session.
- Results for stale document versions are dropped, and the LSP request is cancelled after 1.5 s.

**Alternatives rejected.**
- **`jedi.Interpreter`.** `allow_unsafe_interpreter_executions` defaults to True (R03 C3). It relies on `inspect.signature`, its latency is unpredictable against a 5 ms budget, and it adds vendored dependencies.
- **The IPython completer.** A large dependency. We copy its policy idea instead.
- **Reading live values through unreal attribute access** (TD lead). Rejected on E1. The concrete-type benefit they wanted is delivered through the `get_editor_property` path (E2).
- **A C++ resolver.** Harder to keep version-agnostic across Python versions.

**Dissent.** None on the choice. The TD lead's attribute reads were overruled on E1, and their goal was met through E2.

**Consequences.**
- The resolver misses attributes that only `__getattr__` would reveal.
- Type-level resolution for containers and structs is less concrete than evaluation would be.
- Namespaces with more than 10k names need per-type member caches.

**Verification still needed.**
- p95 ≤ 5 ms over a corpus of 500 chains, including `unreal.`, `sel[0].` and deep chains.
- An instrumented `ProcessEvent` counter shows **0 calls** during 10,000 completions.
- Golden merge tests, including the no-reorder rule.

---

### D7. Implementation language split

**Status:** Accepted. **Confidence:** High. **Lands:** M1.

**Decision.**

| Layer | Owns |
|---|---|
| **SumiCore** (C++, no UE dependency) | Piece table, undo, lexer, folding, search, multi-caret, diff, keymap resolution, output ring and line index, journal format, JSON-RPC/LSP client, snippet expansion |
| **SumiEditor** (C++ editor module) | Slate shell, SCodeView, output, REPL, popups, tabs, menus, TCommands, settings, IME and accessibility; RunSupervisor (watchdog, pending calls, async exceptions, pump, pre-processor, close veto, both presentations); the debugger's Paused window; per-run transactions; the journal writer thread; supervision of the LSP and checker processes; the file watcher and async source control; the `Python (Script Editor)` console executor; later, bridge socket I/O |
| **`_sumi`** (C-API native module) | Created with `PyModule_Create` after `OnPythonInitialized` and inserted into `sys.modules` (inittab is impossible after init). Provides thread-safe `write(stream, run_id, text)`, `check_cancel()`, pause hooks and ScriptCancelled registration |
| **`sumi_editor`** (Python, in plugin `Content/Python`) | Runner (compile with line offsets, linecache, namespaces, displayhook, dunders, `sys.path` entry, stream swap, traceback formatting); bdb engine; resolver and docstring parser; stub snapshot; reload manager; environment queries; public scripting and cooperative API (D29); optional MCP toolset (D11) |
| **Out-of-process Python** (engine `python.exe -I`) | Stub transformer (D5) and `sumi-check` (D4) |

**Rules.**
- `Build.cs` links the Python3 ThirdParty module (`python311.lib`).
- `sumi_editor` has a unique top-level name and is imported lazily on first tab open. There is **no `init_unreal.py`**, and no `test_*.py` anywhere under `Content/Python`. The only third-party dependency is the vendored debugpy.
- Our frames are hidden from user tracebacks, stepping and reload.
- Critical state lives in C++: run IDs, cancel flags and the journal. User code can monkey-patch the Python helper, so C++ validates run IDs. The diagnostics bundle hashes the package to detect modified installs (D26).
- Nothing that must work off the game thread or under a pause is exposed as a UFUNCTION. Reflected calls raise RuntimeError off the game thread (`.../Private/PyUtil.cpp:606-616`).

**Alternatives rejected.**
- **An all-C++ debugger or resolver.** It is not needed for the budgets, and it would block the 3.13 and `sys.monitoring` port.
- **Snippet expansion and lints in the in-process Python** (TD lead).
  - Snippet tab-stop navigation is on the per-keystroke path, so it belongs in the core.
  - Lints run out of process: no game-thread or GIL cost, they work while a script runs, and they are unit-testable.

**Rationale.**
- The latency and pipe-I/O budgets (R05 §2.9; 07e Q3) force C++ for the hot paths.
- TDs read and patch Python (R06 §6).
- Python logic survives the 3.11-to-3.13 move. Only the thin C binding is tied to the ABI (07a Q6).

**Dissent.** *TD lead:* put snippets and Unreal lints in the in-process helper. **Overruled**, for the reasons above.

**Consequences.**
- There are two C++/Python boundaries, the native module and the UFunction library, so both need contract tests.
- The native module is rebuilt at every Python bump (D15).

**Verification still needed.** A lifecycle stress test: 100 cycles of `ForceDisablePythonAtRuntime` and `ForceEnablePythonAtRuntime`, with zero leaked references (refcount audit) and no crash (D16).

---

### D8. Execution defaults

**Status:** Accepted. **Confidence:** Medium. **Lands:** M1; the keymap presets in M4.

**Decision.**

**Namespaces.**
- **Session** is the default for buffer, selection, line, cell, REPL and console-executor runs. It is **UE's console globals dict**, captured once by an entry command that passes `globals()`, and captured again after a Python restart (D16).
  - It shares variables with the Output Log Python console, Epic remote-execution literal commands, and the names that `init_unreal.py` defines in Public scope (E4).
- **Isolated** means a fresh dict with `__name__ = '__main__'` and the real `__file__`.
  - It is the default for **Run File** and for **shelf and snippet items**, and an opt-in per tab.
  - After an Isolated run, its namespace is kept as the "last run" namespace for the Variables view and the post-mortem inspector, until the next run.
- **Per-tab persistent namespaces** are not in 1.0.
- **"Reset session"** clears the console dict back to the keys present at first capture, keeping UE's dict object.
- The mode is always shown on the Run button and in the status bar.

**Run identity.**
- `__name__ == '__main__'` for every buffer run.
- **Saved files:** `__file__` is set to the real path for the run and the previous value is restored afterwards. Restoring must also handle the key having been deleted mid-run, because Public-scope file runs delete it (E4).
- **Untitled buffers:** no `__file__`. Tracebacks use the pseudo-filename `<sumi:Tab Name#id>`, registered in linecache with `mtime=None`.
- **Dirty saved files** are registered in linecache under their real path, with the buffer's text.
- **Selections and lines** are dedented and compiled with `ast.increment_lineno`, so tracebacks show buffer line numbers.
- The runner catches every `BaseException` at top level (D14).

**`sys.path`.**
- No run changes it permanently.
- Run File prepends the file's directory for the run, then removes only that entry, and only if it is still present.
- We never snapshot and restore the whole of `sys.path`, and never call `os.chdir`.
- Permanent paths go in UE's AdditionalPaths setting, which we deep-link to.

**Input.**
- Code is never cleared on execute. The only exception is the REPL line, which moves into history.
- A **journal snapshot** is taken before every run (D17).
- Run File saves to disk first, with an async checkout if the file is read-only.
- Selection and buffer runs do not save by default. "Save on execute" (CE2 parity) is an opt-in setting.

**Auto-print.**
- For Session runs (selection, line, cell, buffer, REPL), an AST rewrite captures the last top-level expression.
- The value is shown as `# Result: <repr>`, truncated at 10,000 characters with an expander.
- It is bound to `_` in the Session namespace, not in builtins, and `None` is suppressed.
- It is off for Run File and Isolated runs.

**Undo.**
- **One editor transaction per run, on by default for every user-initiated route:** buffer, selection, line, cell, REPL submission, console executor, shelf, snippet and Run File. Later, bridge and agent runs get the same treatment.
- The title is `Sumi: <tab> #N`.
- C++ pairs `BeginTransaction` and `EndTransaction` on every exit path. After End, it checks `IsTransactionActive()` and closes any dangling transaction.
- If user code called `cancel()`, or a map load purged the transaction (`Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp:1382-1394, 1411-1462`), a banner says undo is unavailable for this run.
- **Undo Last Run** (toolbar and Ctrl+Alt+Z) first checks two things: that our transaction is on top (title and counter), and that `GEditor->Trans` is still the standard buffer, since the AI Assistant can swap it (07e Q6). Only then does it call `UndoTransaction` (`Engine/Source/Editor/UnrealEd/Classes/Editor/EditorEngine.h:1068`).
- **Ctrl+Z inside our widget is always text undo**, and the chord is always consumed.
- We never present `cancel()` as rollback, and there is no rollback on exception.
- There is a per-tab toggle and a studio default.
- The engine discards empty transactions (`.../EditorTransaction.cpp:515-527, 1331-1336`), so REPL lines that change nothing do not clutter the undo history.

**Flags.** Runs use attended flags (None) (D14).

**Keymap: the "Charcoal" preset (the default).** It applies only while our widget has focus, unless noted.

| Action | Chord | Notes |
|---|---|---|
| Run selection, or the buffer if nothing is selected | **Ctrl+Enter**; **Numpad Enter** | Numpad Enter is Win64 only, detected through the extended-key bit via `IWindowsMessageHandler` (`Engine/Source/Runtime/ApplicationCore/Public/Windows/WindowsApplication.h:262, 346`), subject to G7. Never main Enter (`EKeys::Enter` is a single key, `Engine/Source/Runtime/InputCore/Private/Windows/WindowsPlatformInput.cpp:23`) |
| Run the whole buffer | **Ctrl+Shift+Enter** | Free editor-wide (07e Q2) |
| Run the current line, dedented | **Ctrl+Alt+Enter** | Free editor-wide |
| Run cell / run cell and advance | Ctrl+Alt+Shift+Enter / Alt+Shift+Enter | Checked by G5 |
| **Stop** | **Esc** while a run is active (global, through the run pre-processor, in both presentations); **Shift+F5** while running or paused | Not Ctrl+Break (E8). Swallowing Esc during a run is harmless: PIE's Esc Stop cannot act anyway, because GEngine is not ticking |
| Debug run / Continue | **F5** | Free editor-wide |
| Step over / into / out | F10 / F11 / Shift+F11 | Shadows the PIE Blueprint-step chords and global fullscreen, only while focused |
| Toggle breakpoint | F9 | Free editor-wide |
| Completion / parameter info | **Ctrl+Space** / Ctrl+Shift+Space | Ctrl+Space shadows the Content Browser drawer while focused (`.../GlobalEditorCommonCommands.cpp:71`). **Never Ctrl+Tab** (`:48`) |
| Quick docs | **Ctrl+Q** | F1 is taken by the AI Assistant's pre-processor (`Engine/Plugins/Experimental/AIAssistant/Source/AIAssistant/Private/AIAssistant.cpp:100`). `dir()` and `help()` are in the context menu |
| Toggle comment | Ctrl+/ | Free |
| Find / replace / find in files | Ctrl+F / **Ctrl+H** / Ctrl+Shift+F | Shadows Level Editor Ctrl+H and Ctrl+Shift+F while focused |
| Go to line / definition | Ctrl+G / F12 or Ctrl+Click | Ctrl+G shadows the Level Editor's regroup |
| Palette / search everything | Ctrl+Shift+P / Shift Shift | Shift Shift uses custom timing; it cannot be an `FInputChord` (07e Q2) |
| Tabs | Ctrl+T new, **Ctrl+W close ours (consumed)**, Ctrl+Shift+T reopen, Ctrl+PageUp/PageDown switch, middle-click close | Ctrl+Tab stays with UE |
| Save buffer / duplicate line | **Ctrl+S** / **Ctrl+D**, both consumed | Never a level save or actor duplicate |
| Undo Last Run | **Ctrl+Alt+Z** | Unbound in the engine (TD lead's check) |
| Next / previous error; insert `sel`; zoom | F4 / Shift+F4; Ctrl+Shift+V; Ctrl+wheel, Ctrl+= / Ctrl+- | Checked by G5 |
| Left to UE | Ctrl+Tab, Ctrl+P, Ctrl+B, F1, Alt+F1 | |

**The consume list** (07e Q2). Our widget always returns Handled for these, even when it has no action for them:
- every printable and AltGr key (Ctrl+Alt with `IsRightAltDown` is typed as text, mirroring `.../SlateEditableTextLayout.cpp:1280-1285`);
- Tab, the arrows, Home/End, PgUp/PgDn, Delete, Backspace and Esc;
- Ctrl+Z/Y/Shift+Z, Ctrl+C/V/X/A, Ctrl+D, Ctrl+S and Ctrl+W.

**REPL keys.**
- Enter runs when `codeop` says the input is complete; Shift+Enter inserts a newline; Ctrl+Enter always runs.
- Up at an empty prompt, or Ctrl+Up, opens history.

**Rebinding and presets.** All chords live in the `SumiEditor` TCommands context, persisted in `EditorKeyBindings.ini` and rebindable. PyCharm and VS Code presets come in M4. The UE chords we shadow are listed on a settings page (D25).

**Alternatives rejected.**
- **Our own Session dict** (QA lead). It loses the names `init_unreal.py` defines in Public scope (`.../PythonScriptPlugin.cpp:1639`) and the sharing with the console and remote execution. The QA lead's design also sets `__name__ == '__main__'`, so buffer-defined classes resolve to the real `__main__` module either way; there is no fidelity gain. Testability comes from Reset session instead.
- **The real `__main__.__dict__` as the Session.** Every Private-scope file run copies the live `__main__` dict (E4), so our variables would leak into other tools' script runs.
- **Run File in the Session** (TD lead). Python and UE Private-scope semantics are isolated. Inspection after the run is kept through the last-run namespace.
- **Per-tab namespaces as an opt-in at 1.0** (engine engineer, TD lead). There is no evidence of demand (R05 OQ9; the survey has not been run), and each mode multiplies the semantics test matrix.
- **Save on execute on by default** (TD lead, CE2). The journal already gives crash safety. Saving on every Ctrl+Enter would cause Perforce checkout prompts on read-only files and trigger reload-on-save.
- **Ctrl+Break** (SPEC draft, engine engineer): E8.
- **Ctrl+Tab for completion** (SPEC draft): it is global.
- **Ctrl+H for help** (TD lead). CE2's default bindings are undocumented (R01 B, Hotkeys row). Ctrl+H as Replace matches VS Code, Visual Studio and MotionBuilder (R01, MotionBuilder table).
- **Shift+F5 for Debug run** (SPEC draft). Shift+F5 is Stop in both VS Code and Visual Studio.

**Dissent.**
- *QA lead:* own Session dict.
- *TD lead:* Run File in the Session, save on execute, Ctrl+H for help.
- *Engine engineer and TD lead:* per-tab namespaces as an opt-in.

Each is recorded with its reopening trigger: a TD survey (namespaces), pilot feedback (save on execute), or a preset request (Ctrl+H).

**Consequences.**
- Other tools can pollute the shared Session.
- Pickle, `dataclasses` string annotations and `typing.get_type_hints` on buffer-defined classes resolve against the real `__main__` module. This is a known limitation, shared with UE's own console today.
- Transaction memory on huge batches: show a tip after long runs, and offer "Run without transaction" per run.
- Edits to PIE objects make the transaction transient.
- Imports and file operations are never undoable.

**Verification still needed.**
- G5 and G7.
- A runner semantics matrix, run in plain CPython and under `-run=PythonScript`.
- A pickle, dataclass and `get_type_hints` test on buffer-defined classes. If common workflows break, the candidate mitigation is a PEP 562 module-level `__getattr__` fallback on the real `__main__` module that looks names up in the Session (pickle resolves through `getattr`). The TDD records the result.

---

### D9. Release phasing, scope and platforms

**Status:** Accepted. **Confidence:** Medium. Estimates assume 3–4 engineers and a part-time QA engineer, and are re-baselined at the Phase 0 go/no-go.

**Decision.**

| Phase | Approx. | Scope | Exit |
|---|---|---|---|
| **Phase 0** | 6–8 weeks | Spikes for G1–G4; the SumiCore skeleton; CI for every test tier (D22); the internal dogfood harness from about week 6 | G1–G4 pass, or their fallbacks are invoked; go/no-go |
| **M1 MVP** (public 0.x preview, Win64, UE 5.8) | Month 4–5 | Docked tab; scratch and file tabs; journal, crash recovery and pre-run snapshot; SCodeView basics (D1); the REPL on SCodeView with history; the Ctrl+Enter family through the runner; Session namespace, auto-print and linecache tracebacks; a transaction per run and Undo Last Run; RunSupervisor with Stop and streaming output; `progress`/`check_cancel`/`sleep`; output view (typed lines, ring buffer, per-run blocks, clickable tracebacks, Clear plus API); live completion with docstring signature help and hover, plus editor-property and enum completers; `sumi-check` syntax and foot-gun diagnostics (our code, out of process); the consume list and Charcoal keymap; the `Python (Script Editor)` console executor; basic settings. **No third-party executables** | G2, G5, G6 and G7 green; the cancel and output soaks green; 5 TDs use it for 2 weeks (survey) |
| **M2 Intelligence** | | Type server (G3 default) and ruff; walker stubs and Export stubs; go-to-definition, outline, the Go To palette, find in files; multi-caret, indentation folding, `# %%` cells; local history with diff; **debugpy attach**; post-mortem inspector; asset-path completion; drag actors and assets into code, Copy as Python, open `.py` from the Content Browser (D30) | LSP chaos suite green; G3 thresholds hold in CI |
| **M3 Debug and iterate** | | **In-editor debugger** (D3, G4); deep reload, revert module state and opt-in reload-on-save (D18); variables view and object inspector; the `@task` cooperative mode (D24) | G4 fixture suite (or the descope rule applies) |
| **M4 Studio** | | Snippets and shelves (D28); trust model and policy file (D20); full settings layering (D21); async checkout-on-save; Environment panel; diagnostics bundle (D26); PyCharm and VS Code keymap presets; **Linux Preview build**; docs | Pilots at two studios; NFR gates (D22) green |
| **1.0 GA** | Month 11–12 | Everything above. **Win64 supported; Linux Preview** (CI-built, automated suites green, CJK input commit-only, no SLA) | NFR gates green for 4 weeks |
| **1.1** | | **Linux supported**; IDE bridge (D10); MCP tiers 1–2 (D11); the in-editor debugger reaches GA if it shipped as Preview | |
| **1.2** | | **macOS** (Cocoa IME validated with the context deactivated during runs; notarised binaries); MCP tier 3; headless run mode (D27) | |
| **Later / explore** | | Docked in-place debugger panels; worker-thread breakpoints; editor-hosted LSP/DAP endpoint; `sys.monitoring` backend (UE with Python 3.12+); action recorder; viewport throttle while focused; per-tab namespaces if the survey asks; a Packages panel. **CEF is not planned. Notebooks are a non-goal** | |

Every platform is compiled in CI from day one, even before it is supported.

**Alternatives rejected.**
- **macOS and Linux supported at 1.0** (SPEC draft; TD lead for Linux).
  - Linux has no Slate IME system (`Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericApplication.h:550`).
  - Each OS needs its own process containment, signed toolchain, CI agents and IME checklist.
  - For a small team, that roughly triples the release test matrix (QA lead).
- **Win64 only, with Linux at 1.2** (QA lead). P1 pipeline TDs in film, VFX and virtual production run Linux (R06 §6), and CE2 shipped Linux one month after launch (R01, CE2 release table). The core and Python test tiers are cross-platform anyway (R05 §2.10), so a Linux Preview at 1.0 costs little.
- **macOS before Linux.** Cocoa IME callbacks block on `GameThreadCall` (`.../Mac/CocoaTextView.cpp:253-275`), and bundled binaries need notarisation. Linux has no IME integration at all (OnKeyChar only), so it is the simpler port.
- **Third-party binaries in M1** (engine engineer, for ruff). M1 already proves the transport with `sumi-check`, and keeping binaries out removes AV, signing and Fab risk from the first release (QA lead).

**Dissent.**
- *TD lead:* Linux supported at 1.0. **Reopen** if a launch-partner studio needs Linux at 1.0.
- *QA lead:* macOS 1.1 and Linux 1.2. **Reopen** if launch partners are Mac-heavy.
- *Engine engineer:* ruff in M1.

**Consequences.**
- Early users have no static type diagnostics until M2, though `sumi-check` covers syntax and foot-guns from M1.
- The SPEC's platform line changes (§4).

**Verification still needed.** Phase 0 throughput data, to re-baseline the dates.

---

### D10. External IDE bridge

**Status:** Accepted. **Confidence:** Medium. **Lands:** 1.1.

**Decision.**

**1.0 ships only the external-IDE complements that need no protocol:**
- debugpy attach (D3);
- Export stubs (D5);
- "Open in external editor", with `{file}`, `{line}` and `{col}` placeholders;
- reload on external change, through the directory watcher.

**Remote Execution warning.** We show a status-bar warning if a studio widens Epic's Remote Execution beyond its 5.8 defaults: bind address 127.0.0.1 and TTL 0 (`Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPluginSettings.cpp:15-19`).

**1.1 adds our own bridge.**
- **Off by default**, and studio policy can disable it.
- TCP on 127.0.0.1, on a random port.
- A per-session token in a connection file that only the user can read.
- HMAC-SHA256 signing of every message.
- Browser `Origin` headers are rejected, and every request is written to the audit log (D20).
- An Isaac-Sim-style JSON envelope with these fields: `code`, `file`, `line_offset`, `context`, `timeout`, `fire_and_forget`, `introspect`.
- Structured results and **streamed output events**.
- Socket I/O runs on an `FRunnable`. Execution enters the RunSupervisor queue from FTSTicker (D14), so bridge requests are serialised with user runs, cannot re-enter them, and get the same Stop, transaction, history and output echo.
- We publish the protocol spec and a small Python reference client. We commit to **no first-party VS Code extension**; the protocol is offered to Söderman's extension, which is active on Codeberg (E11).

**Never** built on Epic Remote Execution.

**An editor-hosted LSP/DAP endpoint (the Godot model)** is 2.x exploration.

**Alternatives rejected.**
- **Epic Remote Execution.** It has no authentication, depends on UDP multicast discovery, and returns output only once, after completion (07a Q8). Connectivity is the top failure class in the VS Code extension's tracker (R03 B5).
- **A bridge in 1.0.** It is a public protocol to support forever, and its main advantage, streaming plus Stop, depends on a mature RunSupervisor.
- **A hosted LSP/DAP endpoint.** It duplicates our out-of-process LSP, an external DAP pause freezes the editor just as debugpy does, and it adds socket surface.

**Dissent.** *QA lead:* 1.2, because a bridge means owning a second product. **Mitigated:** we ship no first-party extension, and the reference client is small.

**Consequences.**
- Until 1.1, VS Code users keep using Epic's non-streaming remote execution, and we document its risks.
- Any loopback bridge gives code execution to processes running as the same user. This is documented.

**Verification still needed.**
- A security review: token-file permissions on each OS, DNS-rebinding and Origin tests, and a fuzzed envelope parser.
- A pilot survey. If at least half of a pilot studio's TDs mainly use an external IDE and ask for run-with-live-output, the bridge moves into 1.0.

---

### D11. AI and MCP integration

**Status:** Accepted. **Confidence:** Medium. **Lands:** 1.1 for tiers 1–2, 1.2 for tier 3. **Never in 1.0.**

**Decision.**

**1.0 has no dependency on any Experimental or NoRedist plugin:** not ModelContextProtocol, ToolsetRegistry or AIAssistant. It delivers the AI-safe *human* workflow instead (journey J7):
- paste and review;
- `sumi-check` flags unknown `unreal.*` names and foot-guns;
- a transaction per run, and Undo Last Run.

**From 1.1, an optional module.** `SumiMCP` is a separate module with Optional plugin dependencies. Its Python toolset registers only when `toolset_registry` is importable and the user has turned on "Expose Sumi to Unreal MCP".

| Tier | Release | Tools |
|---|---|---|
| **1. Read-only** | 1.1 | List buffers; get buffer text and selection; diagnostics; last run output; environment |
| **2. Propose edit** | 1.1 | `propose_edit` returns a diff, which the human accepts in our UI |
| **3. Run tools** | 1.2 | `run_buffer` and `run_code`, behind a per-session opt-in. Studio policy can forbid them |

**Rules for tier-3 agent runs:**
- they run in a separate agent namespace by default; sharing the human's Session is a second opt-in;
- each call is its own transaction;
- each call is echoed in our output with an "agent" badge and its full code, and Stop is available;
- calls are refused while a user run or pause is active;
- a persistent banner shows while agent execution is enabled;
- nothing is ever auto-approved.

**Alternatives rejected.**
- **MCP in 1.0.** Unreal MCP and ToolsetRegistry are Experimental, NoRedist and off by default (`Engine/Plugins/Experimental/ToolsetRegistry/ToolsetRegistry.uplugin:16-40`; `Engine/Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocolEngine/Public/ModelContextProtocolSettings.h:17-46`). MCP checks Origin but has no authentication (`Engine/Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocol/Private/ModelContextProtocolServer.cpp:69-118`).
- **Never.** Demand is real (R06 §1, §6), and Gaffer's precedent of echoing agent actions shows a safe UX (R02 Steal 27).

**Rationale.**
- MCP requests are dispatched from the core ticker (`.../HttpServerModule.h:24-26`). Our pump never ticks FTSTicker, so agent calls cannot re-enter a script; this property is tested (G2).
- Staging read-only tools first keeps the security review small. Read access to buffers is roughly what any same-user process already has through the journal files.

**Dissent.**
- *Engine engineer:* everything at 1.2.
- *TD lead:* the first point release.
- **Resolved by staging** across 1.1 and 1.2.

**Consequences.**
- Churn in Epic's API is contained in the optional module, which feature-detects and version-gates.
- If the AI Assistant has swapped `GEditor->Trans`, Undo Last Run detects it (D8).

**Verification still needed.**
- Re-check the MCP dispatch path and the ToolsetRegistry API on each engine release.
- Move tier 3 earlier if Epic adds authentication and leaves Experimental.

---

### D12. Distribution, licensing and dependency policy

**Status:** Accepted. **Confidence:** Medium.

**Decision.**

**Channels.**
- **Apache-2.0**, open source.
- **GitHub** is primary: source of truth, hotfix channel, issues and CI.
- A **free Fab listing** mirrors each engine version.
- Optional paid studio support or sponsorship. Contributions come in under a DCO.

**Dependency policy.**

| Status | Licences |
|---|---|
| **Allowed** | MIT, BSD-2/3, Apache-2.0, ISC, Zlib, PSF, HPND (Scintilla, if used), BSL-1.0, OFL-1.1 (fonts only), Unicode-3.0 and Unicode-DFS-2016 (character data only; added after the v1.0 review, 13 STU-F25) |
| **Allowed with conditions** | EPL-1.0/2.0 and MPL-2.0, only for **unmodified, optional, separately packaged** components, with notices. debugpy's vendored pydevd is the expected case, and must be confirmed from its licence files before M2 |
| **Denied** | GPL, LGPL, AGPL, SSPL, BUSL and source-available licences. So no QScintilla, PySide/Qt, Zuban or TAPython code |

**Release hygiene.**
- No Epic engine or plugin source is ever copied (Unreal EULA). CI fails on Epic copyright headers. The Optimus, PCG and CodeEditor behaviours are re-implemented (07c Q4).
- Each release ships an SBOM and third-party notices. ThirdParty binaries are pinned with SHA-256 hashes and signed where we can sign them.
- Telemetry is never on by default (D26).

**If a sponsor needs revenue:** open-core, meaning an Apache core plus a paid studio pack. Stop, crash safety and undo are never paywalled.

**Alternatives rejected.**
- **A commercial Fab product** (the Charcoal model, $79). Licensing infrastructure and seat friction cost a small team more than they bring in early, and a closed binary cannot follow the custom source-built engines that studios run (R06 §2; R01).
- **An internal-only studio tool.** Narrower reach for the same cost.

**Rationale.**
- A paid $8–10 scratchpad got zero forum replies, while a free VS Code extension has about 64k installs (R06 §2).
- AGPL blocks TAPython adoption (R06 §2).
- Epic has one person on minimum maintenance of the Python integration (R06 §3, rank 13), so studios must be able to patch the tool themselves.
- Apache-2.0's patent grant eases legal review.

**Dissent.** None on the licence.

**Consequences.**
- No direct revenue to fund the engine-upgrade treadmill.
- Forks may fragment the user base.
- Fab's terms on OSS-licensed listings and bundled executables are unverified. The Fab build may carry Fab's standard licence while GitHub stays Apache; that is workable because we hold the copyright.

**Verification still needed.**
- Fab terms for open-source listings and for bundled `.exe` files.
- The pydevd licence.
- Legal review of the NOTICE file.

---

### D13. Product name

**Status:** Accepted, subject to a clearance search. **Confidence:** Low.

**Decision.** **Sumi**, as in "Sumi, a Python script editor for Unreal Engine". Sumi is the carbon-black ink of East Asian brush painting, a sibling medium to charcoal. It nods to the Charcoal lineage without borrowing its name.

**Candidates.**
1. **Sumi** (the pick). Four letters, easy to say, reads well in UI labels such as "Sumi Output".
2. **Bistre** (the fallback). A soot-based brown ink. The least likely to collide, but harder to pronounce.
3. **Inkstone.** Descriptive, but a Chinese-handwriting app already uses the name.

**Identifiers.** All are distinct from Epic's CodeEditor names (07c Q3: the tab ID `CodeEditor`, style set `CodeEditorStyle`, keys `TextEditor.*` and `SyntaxHighlight.CPP.*`) and from generic "ScriptEditor" names other plugins may use.

| Thing | Name |
|---|---|
| Plugin | `SumiScriptEditor` |
| Modules | `SumiEditor`, `SumiCore`, `SumiMCP` |
| Python package | `sumi_editor` |
| Native extension module | `_sumi` |
| TCommands context | `SumiEditor` |
| Style set | `SumiEditorStyle` |
| Tab ID | `SumiEditor.Main` |
| Config section | `[/Script/SumiEditor.*]` |

**Alternatives rejected.**
- Anything containing **Unreal, UE, Epic, Fab, Slate, Blueprint, Kismet, Verse, MetaHuman, Lumen, Nanite, Niagara or Chaos**.
- Anything containing **Charcoal, Char or Coal** (confusable with CE2), or **PyCharm or Pylance**.
- Names already used by developer or DCC tools: Graphite, Pyre, Cinder, Carbon, Chalk, Quill, Hatch, Vellum, Umbra, Willow, Anvil, Ember, Crucible, Kaa, Mamba.

**Dissent.** None. All three judges independently picked Sumi.

**Consequences.** "Sumi" is a common Japanese word and surname, so search visibility may be weak. "for Unreal Engine" is used only as a descriptor.

**Verification still needed.**
- USPTO and EUIPO searches in classes 9 and 42.
- Checks on PyPI, GitHub, the VS Code Marketplace and Fab.
- A cultural check with Japanese-speaking users.
- Any conflict means falling back to **Bistre**.

---

## 6. Additional ADRs from the judges' missing decisions (D14–D30)

### D14. Single run-entry path (invariant)

**Status:** Accepted. **Confidence:** High. **Lands:** M1. **Raised by:** engine engineer (with the QA lead's attended-flags point).

**Decision.**

**Every route that executes user code** goes through one supervisor queue and one entry call. The routes are: buffer, selection, line, cell, REPL, our console executor, Run File, shelf and snippet items, reload, debug runs, the bridge (1.1) and MCP tools (1.1/1.2).
1. The run starts from a **one-shot FTSTicker callback**, which runs after both Slate ticks (E10). It never starts inside a Slate input handler or a tick delegate. This also avoids `SButton` holding pointer capture during `OnClicked` (07b Q2).
2. The entry call is `IPythonScriptPlugin::ExecPythonCommandEx` in EvaluateStatement mode, with the command `sumi_editor._run(<id>)` and **Flags None (attended)**.
   - That sets `GIsRunningUserScript` (`.../PythonScriptPlugin.cpp:1831`), which keeps foreign Python Slate and ticker callbacks quiet (`.../PySlate.cpp:31`).
   - Unattended mode would cancel non-slow-task modals such as `unreal.EditorDialog` (`.../SlateApplication.cpp:2134-2140`).
3. Our runner compiles the code under a real or pseudo filename and **catches every `BaseException`**. That way `EvalString` always succeeds, `RunString` always processes pending `@unreal.uclass` reinstancing (E9), and tracebacks are always ours.
4. **One run at a time.**
   - A user run disables run commands for other user requests.
   - Bridge and agent requests wait up to their timeout, or are refused while a user run or pause is active.
5. Our own helper calls never execute user code (the resolver, the stub snapshot, environment queries). They may use the C API directly, but only from our idle FTSTicker callbacks, never inside the pump and never during a run.

**Alternatives rejected.**
- **Calling `PyRun_*` or `PyEval_EvalCode` directly from C++.** That loses the `GIsRunningUserScript` gating, which is private and not exported (07a Q7).
- **Literal-code `ExecuteFile`.** It means the `<string>` filename, `.py` sniffing and console-dict-only execution (`.../PythonScriptPlugin.cpp:814-838`).

**Consequences.** The gating depends on a private engine flag set by `EvalString`. If a future engine changes that guard, foreign callbacks would re-enter, so the invariant is tested on every engine version.

**Verification still needed.** These tests run in CI on every engine version:
- G2(6): foreign callbacks fire 0 times;
- Unattended is never set;
- a failed run still processes pending reinstancing;
- no route reaches Python without going through `_run`, enforced by a C++ static check on our call sites.

---

### D15. Engine and Python version support, and release channels

**Status:** Accepted. **Confidence:** Medium. **Raised by:** all three judges.

**Decision.**

**Engine versions.**
- **1.0 supports UE 5.8** (Python 3.11.8).
- Each new engine release is targeted for support within **6 weeks** of its release. The plugin is built per engine version; there is no binary compatibility across engine versions.
- **Forward-only.** A **5.7 backport** happens only on funded studio demand after 1.0, behind a compatibility layer. 5.8 behaviour we rely on (the TSF ACP rules, `.../SlateEditableTextLayout.cpp:21-60`; the Python lifecycle; SWebBrowser defaults) makes backports costly. Nothing older than 5.7 is supported.
- **Canary CI:** core suites are built and run against each engine preview. Studios with source access can run the canary against main.

**Python 3.13 bump.**
- Rebuild `_sumi` and the editor module against the new `python3xx.lib`. Only `python311.lib` ships, and there is no `python3.lib` (07a Q6). Calls are kept to the stable ABI where possible to minimise *source* churn.
- Use the tracer abstraction for `sys.monitoring` (3.12+).
- Refresh debugpy's cp313 wheels.
- Run the docstring golden tests for the new engine version.

**Custom source-built engines.** Supported through a source build for the same engine version, best effort.

**Release channels.**
- GitHub releases are primary, versioned with SemVer, with same-day hotfixes. Fab follows after its review.
- Critical fixes are provided for the current and previous supported engine versions.
- Toolchain versions (language servers, ruff) are pinned per plugin release, and any bump is re-qualified with the G3 suite.

**Alternatives rejected.**
- **5.6–5.8 support at 1.0** (TD lead). It multiplies the matrix before product-market fit.
- **5.8 only, forever.** Studios lock their engine mid-production.

**Dissent.** *TD lead:* 5.7, and 5.6 if feasible, at M4. **Reopen** with a pilot contract that requires it.

**Consequences.** Fab's per-version review latency slows hotfixes there; GitHub covers them.

**Verification still needed.** A canary pipeline must exist before 1.0.

---

### D16. Python lifecycle and environment states

**Status:** Accepted. **Confidence:** High. **Lands:** M1. **Raised by:** all three judges.

**Decision.**

**Start-up.**
- The editor module loads at Default or PostEngineInit, so the tab restores in the saved layout (07e Q5).
- All Python work waits for `RegisterOnPythonInitialized` *and* the first tab open. Added startup stays ≤ 30 ms.

**Python disabled at boot.** This covers the `Engine.Python.IsEnabledByDefault` CVar, `EnablePythonOverride` and `-DisablePython`.
- The tab still opens, and editing and the journal work, because they are in C++.
- A banner says Python is disabled in this editor session.
- It offers **Enable Python** (`ForceEnablePythonAtRuntime`) when `IsPythonLifeCycleDynamic` allows it, and instructions otherwise.

**`OnPythonShutdown`** (the GIL is held here):
- stop or abandon runs;
- cancel pending calls;
- release every `PyObject` we hold;
- mark the Session invalid and stop the resolver;
- keep the buffers.

**After `ForceEnablePythonAtRuntime`:**
- re-register `OnPythonInitialized` and `OnPythonShutdown`, because both delegates are `Clear()`ed after broadcast (07a Q6);
- recreate `_sumi`;
- re-capture the Session.

**Headless modes** (commandlets, `-unattended`, `-nullrhi`):
- no UI registration, no pump, no LSP;
- `sumi_editor` is still importable for `-run=PythonScript` tests, and degrades when `_sumi` is absent.

**Game and standalone builds.** The module type is Editor only, so it is never loaded.

**Alternatives rejected.** **One-time delegate binding.** It breaks after a disable/enable cycle.

**Consequences.** Every C++ path that holds `PyObject`s must have an owner that releases it at shutdown.

**Verification still needed.**
- The D7 stress test (100 disable/enable cycles).
- Behaviour under restrictive mode (`Engine.Python.IsPythonInRestrictiveMode`) when our package is imported lazily.

---

### D17. Persistence: storage locations, journal, local history, safe mode

**Status:** Accepted. **Confidence:** High. **Lands:** M1; retention policy controls in M4. **Raised by:** all three judges (the TD lead called it Maya's and CE2's #1 complaint, R01 Summary).

**Decision.**

**Locations.**

| Data | Location |
|---|---|
| Journal, local history, run logs, session recovery, audit log | **Per user, per project, outside both the project and engine trees.** Windows: `%LOCALAPPDATA%\Sumi\<EngineVer>\<ProjectHash>\`. Linux: `$XDG_STATE_HOME/sumi/…`. macOS: `~/Library/Application Support/Sumi/…` |
| Regenerable stub cache | `<Project>/Intermediate/Sumi/` |

**Nothing is ever written under the engine directory** (Program Files is read-only), and nothing is ever pip-installed into the engine interpreter (07a Q8; 07d Q7).

**Journal.**
- A write-ahead log, atomic rename and a kept previous generation (R05 §2.1).
- A copy-on-write snapshot on the game thread, ≤ 0.2 ms, written by a background thread.
- At most 2 s of typing lost in a crash.
- A snapshot before every run. Untitled buffers are journaled too.

**Multi-instance.** A per-project lock records PID and start time. A second editor instance on the same project gets a read-only recovery view and its own journal namespace.

**Local history.**
- An entry on every save and every run snapshot.
- Retention defaults to **30 days and 500 MB per project**, and studios can configure it. There is a "Clear local history" command.
- It stays local and is never uploaded. Journals can contain secrets, so they are excluded from the diagnostics bundle unless the user opts in.

**Format versioning.** Journal and history files carry a format version header. A newer plugin migrates them forward; an older plugin opens newer-format data read-only.

**Safe mode** skips session restore (the recovery list is still shown), forces the Run window presentation and turns language servers off. It is entered by any of:
- `-SumiSafeMode`;
- holding Shift while opening the tab;
- automatically, after two consecutive crashes during session restore.

**Recovery UX.** A banner lists recovered buffers, with a diff against disk and a one-click "restore all".

**Alternatives rejected.**
- **The project's `Saved/` folder.** It is shared or zipped for bug reports, may sit on network drives, and is sometimes wiped by clean scripts.
- **CE2's save-on-execute as the only safety net** (R01 Avoid).

**Consequences.** Journals hold proprietary code in per-user storage, so studios that need it get a retention policy (D20).

**Verification still needed.** G6.

---

### D18. Reload model

**Status:** Accepted. **Confidence:** Medium. **Lands:** M3. **Raised by:** TD lead (pain #4, R06 §3).

**Decision.** Both reload mechanisms ship, and both are **manual by default**.
1. **Deep reload.** Modules whose files live under user or project roots are reloaded in dependency order, using import edges.
   - Never touched: `unreal`, the stdlib, site-packages, the engine's `Content/Python`, `sumi_editor`.
   - A stale-reference report lists instances of old classes still held in the Session, and registered callbacks.
   - **Unload hook:** a module-level `__sumi_unload__()` is called before the module is reloaded or purged, so tools can unregister ticker, Slate and menu callbacks.
2. **Revert module state** (CE2, R01 B):
   - `sys.modules` is snapshotted at session start, or by "Update module state".
   - Revert purges everything imported since then that lives under user or project roots.
   - "Revert on execute" is a per-tab option, off by default.
3. **Reload-on-save** is opt-in per watched root and off by default. Re-running top-level code without unload hooks duplicates menu and callback registrations.
4. **Run entry point.** A per-tab command reloads a chosen package, then calls a chosen function (journey J3).

Reloads go through the D14 entry path, so `@unreal.uclass` reinstancing is processed afterwards (E9).

**Alternatives rejected.**
- **Plain `importlib.reload`.** It misses `from x import y` bindings and dependents (R06 §3).
- **Reload-on-save by default.** Side effects.

**Dissent.** None recorded.

**Consequences.** Reload can never be perfect, so the stale-reference report is how we stay honest about what it missed.

**Verification still needed.** A reload test corpus: a package with diamond imports, `from`-imports, Python UClasses and registered menus, with no duplicate registrations when unload hooks are present.

---

### D19. Output pipeline and Output Log integration

**Status:** Accepted. **Confidence:** High. **Lands:** M1. **Raised by:** engine engineer and TD lead.

**Decision.**

**Capture.**
- During a run, `sys.stdout` and `sys.stderr` are replaced by `_sumi` writers (C API, thread-safe). Lines are tagged with run ID, stream and thread.
- Lines go to an MPSC queue and then to our **OutputStore**, a typed ring buffer (default 200k lines) that spills to a per-run log file.
- Line types: stdout, stderr, result, info/warning/error (LogPython), run header and footer, agent.
- Blank lines are kept (UE's ShouldLog drops them, 07a risks). stderr is labelled **stderr**, not Error.
- Streams are restored on every exit path with compare-and-restore, so a tool that replaced the streams during the run is not clobbered.

**Forwarding to the Output Log and log file.**
- Lines are forwarded to `LogPython` **in batches through C++ `UE_LOG`**, never through `unreal.log`. This avoids two costs:
  - `GLog->Flush()` on every line (`.../Private/PyCore.cpp:1234-1244`);
  - the RunString capture that appends every `unreal.log` line to `FPythonCommandEx::LogOutput` for the life of the run (E3).
- Everything is forwarded by default, so pipelines that parse `Saved/Logs` keep full fidelity. An echo cap setting (default 5,000 lines/s) replaces the excess with summary lines and points to the run log.

**Catching other paths.** An `FOutputDevice` with `CanBeUsedOnMultipleThreads()` set, filtered to `LogPython`, catches direct `unreal.log` calls, UE tracebacks and stale stream references. A thread-local flag prevents echo loops.

**Console executor.** The `Python (Script Editor)` executor ships in **M1**. It routes input through our runner and gives real completions.
- It is not made the preferred executor; the user's `[OutputLog] PreferredCommandExecutor` is respected.
- Studios can hide it with the executor permission list (07a Q5).
- On first use, the Python sections of `ConsoleHistory.ini` are imported into REPL history, read-only.

**Alternatives rejected.**
- **Chaining to UE's stock stream** (R05 §2.3). It brings back the per-line flush (07a correction).
- **Capping log-file output by default.** It breaks pipeline log parsing.

**Consequences.** Our view is the authoritative record of a run. The Output Log is the echo.

**Verification still needed.** Ingest of 50k lines/s at ≤ 2 ms per frame of game-thread time; G2(3); 10M lines over 8 hours with memory growth ≤ 5% (R05 §2.10).

---

### D20. Trust and security model

**Status:** Accepted. **Confidence:** Medium. **Lands:** "No listening sockets" applies from M1; trust prompts and the policy file come in M4. **Raised by:** all three judges.

**Decision.**

**Trust is about files from outside trusted roots.**
- **Trusted by default:** the project directory; the user's Sumi library folders; paths in UE AdditionalPaths; and roots named in studio policy.
- **Everything else is untrusted:** Downloads, temp folders, removable drives, and network shares not named in policy. Files there open in **restricted mode**: viewable and editable, but Run is disabled until the user chooses "Trust this folder". That prompt appears once per folder and is remembered per user.
- Code the user types or pastes is never prompted for.
- The target is **at most one prompt per week** for a TD in steady state. Studio policy can turn prompts off, or make trust policy-only.

**Listening sockets.** None in 1.0. The bridge (D10) and MCP tools (D11) are opt-in and controlled by policy.

**Libraries.** Studio snippet and shelf libraries are hash-pinned (D28).

**Audit log.** A local JSONL file in the D17 location records each run: run ID, route (user, shelf, bridge, agent), source path with SHA-256, start and end, and outcome.

**Policy file.** Precedence is **machine policy, then project policy, then user settings**.
- The project policy is `<Project>/Config/Sumi/Policy.json`, under source control.
- The machine policy path comes from the `SUMI_POLICY_PATH` environment variable, for studio deployment.
- Keys:
  - allowed language servers and binary paths;
  - trusted roots;
  - bridge and agent-run toggles;
  - the run presentation, and the kill switch for the live pump;
  - the transaction default;
  - retention;
  - telemetry;
  - the echo cap.

**This is not a sandbox.** User code runs with full editor privileges, and the documentation says so.

**Alternatives rejected.**
- **VS Code-style restricted mode for everything by default.** It produces too many prompts for in-editor scratch work.
- **No trust model.** Running scripts from network shares is a real risk (R05 R10).

**Consequences.** One more settings surface. Its effective values are shown in the settings inspector (D21).

**Verification still needed.** Usability with pilot TDs (count prompts per week); a policy precedence test matrix.

---

### D21. Settings layering

**Status:** Accepted. **Confidence:** High. **Lands:** M1 (basic), M4 (full). **Raised by:** all three judges.

**Decision.** Three `UDeveloperSettings` classes, following 07e Q7.

| Layer | Class configuration | Holds |
|---|---|---|
| **Project** | `config=Editor, defaultconfig`, saved to `DefaultEditor.ini` | Shared studio defaults |
| **Per-project user** | `EditorPerProjectUserSettings` | Session state: open tabs, carets, splits, per-tab modes |
| **Global user** | `config=EditorSettings`, engine-version-wide | Font, theme, keymap preset |

- **Project layer saves.** We check out `DefaultEditor.ini` before saving. `TryUpdateDefaultConfigFile` fails with only a log warning when the file is read-only, so we surface that failure in the UI.
- **Session state** lives in our per-project settings because `EditorLayout.ini` is shared by every project on the same engine version (`Engine/Source/Runtime/Core/Private/Misc/ConfigCacheIni.cpp:6649-6654`).
- **Chord rebinds** use TCommands in `EditorKeyBindings.ini`.
- **The policy file** (D20) overrides all three layers.
- **An effective-value inspector** shows which layer set each value.
- **UE's own Python settings** are shown read-only, with deep links. They are not BlueprintType (07d Q6).

**Alternatives rejected.** **`EditorPerProjectUserSettings` for everything** (R05 L495). It does not follow users across projects (07e correction).

**Verification still needed.** A precedence test; the read-only `DefaultEditor.ini` path under Perforce.

---

### D22. Quality engineering: test tiers, CI, performance gates, definition of done

**Status:** Accepted. **Confidence:** High. **Lands:** Phase 0. **Raised by:** engine engineer and QA lead.

**Decision.**

**Test tiers.**

| Tier | What | When |
|---|---|---|
| 1 | SumiCore standalone target (CMake): unit, golden tokenizer corpus, libFuzzer differential and round-trip fuzzing | Every commit, all three OSes |
| 2 | `sumi_editor` in plain CPython 3.11, against a fake `unreal` module: runner semantics, resolver, docstring parser, stub transformer, reload, bdb against a fake pause port | Every commit |
| 3 | C-API embedding harness against the shipped `python311.dll`: pending-call pump, ScriptCancelled, re-delivery, run-ID guard, cancel suite across loop shapes | Every commit |
| 4 | Editor Automation Specs: G5 key routing, transaction rules, Slate widget tests, G2 pump soak, G4 debugger fixtures. Run on GPU agents, using the async driver | Nightly |
| 5 | `UnrealEditor-Cmd -run=PythonScript` execution-semantics tests | Nightly |
| 6 | G6 kill -9 recovery suite | Nightly |
| 7 | LSP chaos: a scripted fake server on every commit; the pinned real servers nightly | Commit / nightly |
| 8 | Manual IME checklist | Each release |

**Performance budgets are release gates** (R05 §2.9). CI compares each build against a rolling baseline per machine.

| Budget | Target |
|---|---|
| Added editor startup | ≤ 30 ms |
| Keystroke processing | ≤ 2 ms p95 on 100k lines |
| Paint | ≤ 2 ms p95 |
| Live completion | ≤ 5 ms of game-thread time |
| LSP completion | p95 ≤ 400 ms |
| Output ingest | 50k lines/s at ≤ 2 ms per frame |
| Pump overhead | ≤ 10% of wall time (hard limit 15%) |
| LSP memory | RSS ≤ 1.5 GB |
| Journal snapshot | ≤ 0.2 ms |

**Definition of done.**
- Each requirement ID has an automated test that failed before the implementation existed.
- Each journey J1–J8 has an end-to-end acceptance test.
- No shipped file under `Content/Python` is named `test_*.py`, because Editor.Python would discover it (`Engine/Plugins/Tests/PythonAutomationTest/Source/PythonAutomationTest/Private/PythonAutomationTest.cpp:117-134`).

**The viewport throttle while focused** (R05 OQ10) is exploration, not a commitment.

**Alternatives rejected.** **Relying on in-editor tests alone.** Too slow for test-driven development, and fragile on RDP or headless agents (R05 §2.10).

**Verification still needed.** GPU agents provisioned before Phase 0 ends; baselines recorded per machine.

---

### D23. Text rendering, fonts, IME, accessibility, encodings and large files

**Status:** Accepted. **Confidence:** High. **Lands:** M1. **Raised by:** all three judges.

**Decision.**

**Font.**
- Bundle **JetBrains Mono** (OFL), with regular, bold, italic and bold-italic faces, as an `FCompositeFont`. The engine's `DroidSansFallback` provides the CJK fallback.
- DroidSansMono, the only monospace font the engine ships, has a single face (07c Q6).
- Column math uses measured advances. Zoom is per tab, and DPI follows Slate scaling.

**Theme.** Defaults to the editor's `SyntaxHighlight.SourceCode.*` palette (07c Q4).

**IME by platform.**

| Platform | Commitment | When |
|---|---|---|
| Windows | Inline composition through TSF/IMM | M1, gated by G1 |
| macOS | Inline composition; the context is deactivated during runs (blocking `GameThreadCall`) | 1.2 |
| Linux | **Commit-only**, through OnKeyChar. This is an engine limit (`.../GenericApplication.h:550`) | 1.0 Preview |

**Accessibility.** SCodeView overrides `CreateAccessibleWidget` and returns `IAccessibleText` plus `IAccessibleProperty`, whose value is the current line or selection.
- It is focusable and named, and announces run states.
- **Full screen-reader editing is out of scope**, because 5.8's Windows UIA text-range provider is mostly `E_NOTIMPL` (07c Q7).

**Encodings.**
- UTF-8 by default. BOM and line endings (CRLF or LF) are preserved per file, and a PEP 263 coding cookie is honoured on load.
- Invalid UTF-8 opens read-only, with replacement markers and a "reopen with encoding" option.
- A file with a NUL byte in its first 8 KB is treated as binary and refused.

**Large files.** Above 10 MB, files open progressively and highlighting is progressive. Above 50 MB, files are read-only. Plain mode is offered only below 1 MB.

**Localization.** UI strings use `LOCTEXT` from day one. 1.0 ships in English only.

**Alternatives rejected.**
- **Promising inline IME on Linux.** Impossible in 5.8.
- **DroidSansMono only.** It has no bold or italic face.

**Consequences.** The SPEC states the IME and accessibility limits honestly, per platform.

**Verification still needed.** G1 IME; a CJK column-alignment golden test; a UIA smoke test with Narrator.

---

### D24. Worker threads and the cooperative execution API

**Status:** Accepted. **Confidence:** Medium. **Lands:** M1 (`progress`, `check_cancel`, `sleep`); M3 (`@task`). **Raised by:** engine engineer (with R06 §3 pain 1(c) and 1(d)).

**Decision.**

**No "run on a worker thread" mode in 1.0.** User code may start threads, as Python allows.

**Cooperative API.**
- M1: `progress`, `check_cancel` and the run-scoped `sleep` (D2).
- M3: `@sumi_editor.task`, a generator stepped once per engine frame from FTSTicker. Between items, the viewport, Asset Registry and Sequencer tick fully (journey J5). Stop works at item boundaries, and the task is its own run, with its own transaction.

**The documented worker pattern.** Pure-Python I/O runs in threads, and a `@task` consumes the results from a `queue.Queue`.
- There is **no `call_on_game_thread` in 1.0**. A worker that marshals to the game thread while that thread's run is waiting on the worker deadlocks. It is a 1.x candidate, with a timeout, if users ask for it.

**Thread rules.**
- `sumi-check` flags `unreal.*` use inside threading targets, because property access is not thread-gated (`.../Private/PyUtil.cpp:606-616` gates only UFunction calls).
- Output from worker threads is captured by the `_sumi` writer and labelled with the thread name. After the run ends, it is tagged "background".
- Stop cancels the game-thread run only. At run end, any threads started during the run that are still alive are listed; they are never killed.
- Debugger breakpoints are not hit on worker threads in 1.0 (D3).

**Alternatives rejected.**
- **A worker-thread execution lane for `unreal.*` code.** Illegal (07a Q2).
- **Shipping `call_on_game_thread` now.** The deadlock hazard.

**Consequences.** Long jobs that need the editor to keep ticking use `@task`. That is the only non-freezing in-process path for them; D27 is the out-of-process one.

**Verification still needed.** A `@task` soak: a 20-minute task with 10,000 items, with the viewport ticking, Stop within one item, and a per-run log.

---

### D25. Coexistence with other plugins, input processors and tracers

**Status:** Accepted. **Confidence:** Medium. **Lands:** M1. **Raised by:** engine engineer and QA lead.

**Decision.**

**Names.** All names are distinct from Epic's CodeEditor and from generic "ScriptEditor" names (D13).

**Input pre-processors.**
- Ours is registered only for the duration of a run (Stop/Esc and the docked whitelist). While idle, all our keys are handled in the widget's `OnKeyDown`.
- We do not fight other pre-processors (AI Assistant F1/Alt+F1, CommonUI, VCam). Known shadowing is documented.

**Shortcuts.** The `SumiEditor` TCommands context starts **parentless**, so the engine never unbinds our chords. A "Shadowed Unreal shortcuts" settings page lists every chord we shadow while focused, because the engine's conflict UI cannot show them (07e risk).

**Tracers.** There is one trace-slot owner. Before a debug run we detect a foreign `sys.gettrace()` and prompt. Cancellation never uses tracing.

**Shared interpreter.**
- No Qt imports and no pins on common libraries.
- `sys.displayhook` and the streams are swapped only for the duration of a run, with compare-and-restore.

**Test discovery.** No `test_*.py` files under `Content/Python` (D22).

**Other editors** (Minimal Effort, unrealScriptEditor, TAPython, tk-unreal/PySide): there is no exclusive resource to fight over. The console dict is shared by design (D8).

**Alternatives rejected.** **A global pre-processor at all times.** It would intercept keys meant for other tools.

**Verification still needed.**
- Parent the context to MainFrame in a test build, to check whether the engine only warns about conflicts or also unbinds. If it only warns, parent it, so users see conflicts in the Keyboard Shortcuts UI.
- A coexistence matrix test with the AI Assistant, CodeEditor and tk-unreal enabled.

---

### D26. Diagnostics bundle, crash attribution and telemetry

**Status:** Accepted. **Confidence:** Medium. **Lands:** M4 (breadcrumbs in M1). **Raised by:** engine engineer and QA lead.

**Decision.**

**Telemetry.**
- None is on by default. It is opt-in only, routable to a studio endpoint, and content-free: no code, paths or symbol names.
- Pilot metrics (SPEC §4.3) come from opt-in telemetry plus surveys.

**Crash breadcrumbs (M1).** Recorded with `FGenericCrashContext::SetGameData` (`Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformCrashContext.h:647`):
- run ID and route;
- a hash of the tab ID;
- the time of the last pump;
- pause state;
- plugin version.

This lets us tell "a user script crashed the editor" apart from a plugin bug.

**Diagnostics bundle.** One-click export of:
- versions and effective settings;
- language-server stderr logs;
- the last N run logs, after the user reviews them;
- a hash of the `sumi_editor` package, to detect modified installs.

Journals are excluded unless the user ticks them in (D17).

**Alternatives rejected.** **Default-on telemetry.** It would sink studio security sign-off.

**Verification still needed.** A privacy review of the opt-in channel (R05 OQ12); a check that breadcrumbs appear in CrashReportClient output.

---

### D27. Headless (out-of-process) run mode

**Status:** Accepted for 1.2. **Confidence:** Medium. **Raised by:** engine engineer and TD lead.

**Decision.**
- In **1.2**, "Run headless" executes a saved file as a supervised child process: `UnrealEditor-Cmd <project> -run=PythonScript -Script=<file>`.
- It uses the D4 transport and Job Object. Output streams into our output view.
- Stop terminates the process tree.
- It is for batch jobs, CI, and long jobs that do not need the live editor; this is the Godot model (R02 §4).
- A headless process boots its own editor (no live session state) and runs unattended (07d Q4).

**Alternatives rejected.** **Shipping it in 1.0.** It costs a second editor boot per run, and the in-process product comes first.

**Consequences.** Users must understand that headless runs cannot see the live session. The UI labels them clearly.

**Verification still needed.** Boot time and exit-code handling on large projects; source-control behaviour (`-run=PythonScript` opens source control, 07d Q4).

---

### D28. Snippets, shelves and tool distribution

**Status:** Accepted. **Confidence:** Medium. **Lands:** M4. **Raised by:** TD lead and QA lead.

**Decision.**

**Snippet body syntax** is compatible with VS Code snippet JSON: `$1`, `${1:default}`, `${1|a,b|}`, `$TM_SELECTED_TEXT`, `$CLIPBOARD`. Existing collections import as they are.

**Parameter forms** are a Sumi extension: a `params` JSON-schema block.
- Types: string, int, float, bool, enum, asset path, actor.
- Each one is rendered as a form field for artists.
- Values are substituted as **Python literals via `repr`**, never concatenated raw.

**One item per file**, which merges cleanly under Perforce (R05 R12). Files carry a versioned schema with migrations.

**Tiers.**

| Tier | Location |
|---|---|
| User | The user settings folder |
| Project | `<Project>/Sumi/Library/`, under source control, outside `Content` so it is never cooked |
| Studio | A path from policy, with a **hash-pinned** manifest (SHA-256). Changed content prompts the user (D20) |

**Where tools appear.**
- Our shelf bar in the tab.
- Optional Level Editor toolbar and Tools menu entries through UToolMenus (`LevelEditor.MainMenu.Tools`).
- "Run on selection" entries in the actor and asset context menus (07e Q5).
- Shelf items run **Isolated** by default, with `sel` bound (D8).

**Alternatives rejected.**
- **KnobScripter `$$` placeholders.** Not an interchange format.
- **Storing libraries under `Content/`.** They would be cooked and treated as assets.

**Verification still needed.** Usability with an artist (P3) running a parameterized shelf tool; a round-trip test of the schema migration.

---

### D29. Public scripting API stability

**Status:** Accepted. **Confidence:** Medium. **Lands:** M1, then grows. **Raised by:** TD lead and QA lead.

**Decision.**

**The public API** is the top level of `sumi_editor`:
- `open(path, line)`, `new_tab(text)`, `clear_output()`, `run(tab|file)`;
- `revert_module_state()`, `suppress(...)`;
- `progress()`, `check_cancel()`, `sleep()`, `task`;
- `__version__`.

It aims for parity with CE2's `charcoalEditor2` flags (R01 B, Scripting API row).

**Rules.**
- Internals live in `sumi_editor._impl`.
- The API follows **SemVer** from 1.0. Deprecations warn for at least one minor release *and* one engine version.
- Reference docs are generated from the code.
- There is no public C++ API in 1.0.

**Alternatives rejected.** **Exposing UFUNCTIONs.** They fail off the game thread and under a pause (D7).

**Verification still needed.** An API surface snapshot test in CI, which fails on any unannounced change.

---

### D30. Object inspector, Copy as Python and code insertion

**Status:** Accepted. **Confidence:** Medium. **Lands:** M2 (insertion and Copy as Python), M3 (inspector). **Raised by:** TD lead.

**Decision.**

**Inspector and Variables view rules.**
- **Never invoke getset descriptors or Python properties** (E1).
- unreal property values are read with `get_editor_property` (E2), on expand only, on the game thread, while idle. They are never read during runs.
- Containers are paged, 100 items at a time.

**Insertion formats.**

| Source | Inserted code |
|---|---|
| Asset dragged or copied | `unreal.load_asset("/Game/Path/Asset.Asset")` |
| Asset with Alt held | The soft object path as a string |
| Actor | `unreal.find_object(None, "<full object path>")  # <label>`; several actors become a list |
| Ctrl+Shift+V | `sel = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()` |
| Details panel property | `obj.set_editor_property("name", <repr>)` and the matching getter form |

**Content Browser.** "Open in Sumi" on `ContentBrowser.ItemContextMenu.PythonData` (07e Q5).

**Action recorder (Echo All):** exploration only (D9).

**Alternatives rejected.**
- **Label-based actor lookups.** Labels are ambiguous.
- **Evaluating properties for display.** E1.

**Verification still needed.**
- `find_object` round-trip tests for level actors in World Partition maps.
- An instrumented `ProcessEvent` count of 0 while the inspector expands 1,000 properties.

---

## 7. Amendments after the red-team review (RT-01 – RT-20)

**Status:** Accepted, 2026-10-28, by the chief architect, after reading [09-red-team.md](09-red-team.md). All 20 objections are accepted, and four are accepted with changes (marked *modified*). Where an amendment conflicts with the ADR text above, **the amendment wins**. SPEC v1.0 and TDD v1.0 implement the amended decisions.

| RT | Ruling | Amends | What changes |
|---|---|---|---|
| RT-01 | Accepted | D14(1), D2 §1, D10, D24 | **Runs start from a one-shot `FCoreDelegates::OnEndFrame` handler**, not from an FTSTicker callback. The handler runs after the core ticker and deferred commands (`LaunchEngineLoop.cpp:6103, 6108, 6127`), so `IsTicking()` is false and no pointer capture is held, which keeps E10's properties. `@task` steps, bridge dispatch and MCP tool dispatch use the same context. User code can tick the core ticker (material baking, HLOD builds), and that dispatches other ticker clients in a nested way. So **every Sumi ticker client (bridge, MCP toolset, LSP result application, file-watcher actions) checks "run active" and defers or refuses**. D2's sentence "Nothing can re-enter a script" is withdrawn. G2(2) gains the RT-01 cases (§4 of 09) |
| RT-02 | Accepted | D2 §1 | **Pump guard = the engine's `CanTickPython` conditions plus native re-entry.** Skip the pump while `UE::IsSavingPackage()`, `IsGarbageCollecting()` or `FUObjectThreadContext::Get().IsRoutingPostLoad` is true. Also skip it when Python has been re-entered from native code since `_run` began: compare the current frame depth or entry frame with the value captured at `_run` entry (3.11: `cframe`, behind the D15 compatibility layer). **Cancellation delivery is deferred under the same conditions**: the pending call re-arms without raising. A skipped pump re-arms. G2(2) gains the import-hook, delete-hook and UFUNCTION-override cases |
| RT-03 | Accepted, *modified* | D2 §2 | **The Run window becomes the provisional default presentation.** Live (docked) ships in 1.0 as an opt-in **Preview** profile with four extra protections: an `OnWindowAction` veto for the run; no `SetAsyncExc` while the pump depth is above 0; the RT-03 hazard cases added to G2(6); and an evaluated `ForceRedrawWindow`-only variant for a floating Sumi window. Docked becomes the default only after the extended G2 is green for 4 consecutive weeks *and* a documented policy for nested foreign Python exists. **The Run window is positioned over the Sumi output area by default**, so it reads as part of the tab |
| RT-04 | Accepted | D2 §1–2 | **The Run window opens on the first pump** (when the watchdog arms, about 300 ms into the run), not at 1 s. **The swallow-all input pre-processor is installed from the first pump in both presentations**, with the Stop targets whitelisted. The G2(5) input-leak suite also runs inside the first-pump window |
| RT-05 | Accepted | D2, D8 keymap | **Esc means Stop only when the active modal window is one of ours (the Run or Paused window), or when no modal is active, and only when no menu is open.** Otherwise Esc passes through, so a script's own dialog gets Cancel. **Shift+F5** and the Stop buttons remain the run-level Stop in every context. G5 gains "Esc in `show_message` during a run returns Cancel" |
| RT-06 | Accepted | D2 §3 | **Re-delivery respects cleanup.** Each pending call checks `PyErr_GetHandledException()`. If the handled exception is our `ScriptCancelled` instance, the stop is being handled, so nothing is re-delivered. Re-delivery (every 250 ms) and the `SetAsyncExc` channel are used only after the exception has been cleared, which means it was swallowed. If cleanup runs longer than **10 s** (a setting), the Run window and banner offer **Interrupt cleanup**, which re-delivers once. `_run` calls `_sumi.user_code_done(run_id)` as the **first statement** of its handler, and all delivery stops before our own cleanup. G2(1) gains "Stop during a 3 s `finally`" |
| RT-07 | Accepted | D19, G2(3)/(7) | **Run-scoped `unreal.log*` shims.** For the run, `unreal.log`, `unreal.log_warning` and `unreal.log_error` in the `unreal` module dict are rebound to `_sumi` shims. The shims keep `ShouldLog` semantics, emit through our C++ batch path at the matching verbosity, and are compare-and-restored at run end (D25). A `from unreal import log` binding made before the run bypasses the shim; this is documented, and the FOutputDevice catcher still records those lines. G2(3) and G2(7) gain an `unreal.log`-heavy loop |
| RT-08 | Accepted, *modified* | D8 Undo | **Transaction defaults by route.** On: selection, line, cell, buffer, REPL, console executor, shelf and snippet runs. **Off: Run File and headless runs**, with a one-click **Run in transaction**. **Size watch:** on every pump, read the open transaction's size. At 50% of `UndoBufferSize`, the banner warns. **At 100%, recording stops automatically**, which is safer than an offer nobody may be watching: the transaction is ended, the run is labelled **partly undoable**, and Undo Last Run warns before reverting a partial record. Undo Last Run shows the record size and warns when the next edit will evict it. GC pinning is documented. The D8 tests gain the 10,000-object case |
| RT-09 | Accepted | D24 | **Per-step transactions for `@task`.** Each step opens and closes its own transaction titled `Sumi: task #N`. **The editor stays interactive between steps.** Undo Last Run on a task walks back consecutive `Sumi: task #N` transactions and stops at the first foreign one, and says how many steps it can undo ("12 of 40 steps; a manual edit after step 12 blocks the rest"). A step that runs longer than 1 s is supervised like a run (pump, Stop, banner). The D24 soak gains PIE start and a manual edit mid-task |
| RT-10 | Accepted | D9, §3 | **Phase 0 exits on G1 and G2**, plus harness prototypes for G5 and G7 and CI tiers 1–3. **G3 runs during M1** alongside the walker and must pass before M2 starts. **The G4 spike moves to M2**; the G4 suite still gates 1.0 at M3 exit. **The 1.0 cut list is published** (§7.1) |
| RT-11 | Accepted | D3, D12 | `debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)`: no adapter process, no `configure(python=…)`, no blocking `accept`, no Job Object needed. "Debug in VS Code / PyCharm" is **once per editor session**, and the UI says so. The vendored tree is pruned to what tracing needs (`attach_amd64.dll` and the cp311 `.pyd` files). `inject_dll_*`, `run_code_on_dllmain_*`, the x86 files, the `.pdb` files and `winappdbg` are removed. What remains goes through the D4 SHA-256 manifest. **pydevd is EPL-1.0**, which is allowed with conditions (D12). The debugpy socket is a documented, user-initiated exception to SEC-NF-01 (§8, U15) |
| RT-12 | Accepted | D4 | **One supported language server at 1.0**: the G3 winner (Pyrefly if within 3 points). The other server is **experimental** like ty: a user- or policy-supplied binary, no SLA, not bundled. It can be promoted in 1.1 if pilots ask |
| RT-13 | Accepted | D4, D5, D9 | **D5 Stage 1 (the snapshot JSON) moves into M1** as `sumi-check`'s data file. It holds names, descriptor kinds, docstrings and editor-property flags. The unknown-name and `set_editor_property` lints therefore work from M1. The transform and the `.pyi` package stay in M2 |
| RT-14 | Accepted | D8 run identity | **Selections are compiled inside an `if True:` wrapper**, keeping the original indentation, so 3.11 traceback carets stay exact and triple-quoted strings compile. A selection whose first line is at column 0 compiles directly with `increment_lineno(start - 1)`. Otherwise the runner parses `"if True:\n" + text`, takes `body[0].body` and calls `increment_lineno(start - 2)`. A selection that starts mid-line is extended to the start of its first line |
| RT-15 | Accepted | D14(2) | The entry expression is **`__import__('sumi_editor')._run(<id>)`**, so it does not depend on any Session name. Reset session never restores or removes `sumi_editor` |
| RT-16 | Accepted | D8, G5 | G5 gains AltGr+Z and AltGr+X on a Polish layout, and asserts that no Undo Last Run fires. **Undo Last Run is never a global chord**; it is bound only inside our widgets |
| RT-17 | Accepted | D4, D7, D16 | **Deferred start.** The tab and its buffers restore from C++ at layout restore, and paint at once. The `sumi_editor` import and every child process (the type server, ruff, `sumi-check`) wait until the **first focus or keystroke in a Sumi widget**, or **10 s after `OnFEngineLoopInitComplete`**, whichever comes first |
| RT-18 | Accepted | D20 | **Machine policy** is read from a fixed location that only administrators can write: `%ProgramData%\Sumi\Policy.json`, `/etc/sumi/policy.json`, `/Library/Application Support/Sumi/Policy.json`. `SUMI_POLICY_PATH` points to a **studio policy** layer below machine policy. Precedence: machine policy, then studio policy, then project policy (`<Project>/Config/Sumi/Policy.json`), then user settings. The settings inspector shows the source of each value |
| RT-19 | Accepted | D12 | **No CLA.** Contributions stay under the DCO. The Fab package ships the Apache-2.0 LICENSE and NOTICE and relies on Apache-2.0 §2 and §4. The sentence "we hold the copyright" is withdrawn. Added to the legal review list |
| RT-20 | Accepted, *modified* | D15, D9 | **1.0 is scoped as UE 5.8 only.** The next engine version ships in **1.0.x**, under the 6-week rule. The port is a named, budgeted item. The **Python 3.13 canary runs from M2** |

### 7.1 The 1.0 cut list

If the schedule slips, cut in this order. Each cut moves an item to the release shown; nothing else moves.

| Order | Item | Moves to |
|---|---|---|
| 1 | Linux Preview build | 1.1 |
| 2 | PyCharm and VS Code keymap presets (the Charcoal preset stays) | 1.0.x |
| 3 | Live (docked) Preview presentation (the Run window stays) | 1.1 |
| 4 | Studio-tier libraries with hash-pinned manifests (the user and project tiers stay) | 1.1 |
| 5 | Revert module state (deep reload stays) | 1.1 |
| 6 | Diagnostics bundle UI (crash breadcrumbs stay) | 1.0.x |
| 7 | In-editor debugger GA (the G4 rule makes it a Preview; debugpy stays supported) | 1.1 |

**Never cut:** Stop and the RunSupervisor, the journal and crash recovery, one undo step per run with Undo Last Run, clickable tracebacks, live completion, the consume list and G5, the trust model's restricted mode.

### 7.2 Gate changes in one place

| Gate | Change |
|---|---|
| G2 | Adds every case in 09 §4, plus the RT-03 docked-mode hazards. Docked mode needs the extended G2 green for 4 consecutive weeks before it can become the default |
| G3 | Runs during M1; must pass before M2 starts (was Phase 0) |
| G4 | Spike in M2 (was Phase 0); the suite still gates 1.0 at M3 exit |
| G5 | Adds Esc-in-dialog (RT-05) and AltGr+Z/X (RT-16) |
| Phase 0 exit | G1 + G2 + G5 and G7 harness prototypes + CI tiers 1–3 |

---

## 8. UX rulings from the mockup review (U1–U41)

The mockup builders and the QA pass found gaps in the SPEC v0.9 draft. These rulings close them. They are product decisions with the same standing as the ADRs, and SPEC v1.0 implements them.

**Naming and labels**

| # | Ruling |
|---|---|
| U1 | **Product name: Sumi** (D13). The dock tab reads **Sumi**. The menu entry is **Tools ▸ Sumi Script Editor**, mirrored under Window. The descriptor is "Sumi — Python script editor for Unreal Engine" |
| U2 | **Canonical run-state names**, used everywhere (status bar, banner, Run window, WS-07 item, docs): `Idle`, `Starting`, `Running`, `Stopping…`, `Paused`, `Task running`. "In native call" is not a live state, because nothing can paint during a native call; it appears after the fact in the run report ("the last native call took 12.4 s"). **Run outcomes**: `ok`, `error`, `cancelled`, `not executed` (trust, parse or compile failure), `lost` (the editor exited during the run; recorded on recovery) |
| U3 | The undo toggle is labelled **One undo step per run** everywhere (toolbar, settings, docs). The undo title is `Sumi: <tab> #N` |
| U4 | The completion source badges are `runtime`, `static`, `keyword` and `snippet`. **No "project" badge**, because "project" is a library tier (SNP) |
| U5 | `sumi-check` lint codes: **SUM1xx** Unreal API (SUM101 unknown `unreal.*` name, SUM102 attribute assignment where `set_editor_property` is needed, SUM103 deprecated API, SUM104 missing transaction around actor edits in a loop, which fires only when One undo step per run is off), **SUM2xx** threads and the game thread (SUM201 `unreal.*` inside a threading target, SUM202 `time.sleep` in a loop on the game thread, SUM203 `asyncio.run` on the game thread), **SUM3xx** files (SUM301 `os.rename`/`shutil.move` on a Content path), **SUM9xx** internal. SUM102 has a quick fix that rewrites `obj.prop = v` as `obj.set_editor_property("prop", v)` |

**Run model and states**

| # | Ruling |
|---|---|
| U6 | **The Run button is a split button** that shows the namespace mode: `▶ Run · Session ▾`. Its menu holds Run selection, Run buffer, Run file, Run in transaction / without transaction, and the namespace choice. The status bar also shows the mode (RUN-10) |
| U7 | **Pre-run steps** belong to `Starting`: trust check, then parse check, then journal snapshot, then (Run File only) save. Any failure ends the run as `not executed`. A `not executed` run still gets a run number, so its output block can be referenced |
| U8 | **Every run gets a number**, `#N`, counted per editor session from 1. REPL submissions are runs. Agent and bridge runs get numbers too, with their badge |
| U9 | **Errors and cancels keep the transaction.** There is no rollback. The run header offers **Undo this run** |
| U10 | **Task mode** is part of the state model: `Task running` keeps the editor interactive. Its Stop is on the WS-07 item, on the tab banner, and Shift+F5 when a Sumi widget has focus. Journey J5 uses task mode |
| U11 | **Viewports do not render during a supervised run** (GEngine is not ticked). The Run window says "Viewports resume when the run ends". During a task, viewports render normally |
| U12 | `progress(i, n, msg)`: `i` is the count of items completed (0…n). The UI shows `i / n`, a rate and an ETA after 3 samples. `n=None` gives an indeterminate bar |
| U13 | **Runtime errors in Problems.** A run's uncaught exception appears in Problems as a `runtime` item for the buffer and line where it was raised. It clears at the next run of that buffer, or when that line is edited. **F4 / Shift+F4** walk traceback frames and error links in the output. **F8 / Shift+F8** walk Problems |
| U14 | **WS-07 Unreal status-bar item.** Pattern: Sumi mark · state · detail · [Stop]. Idle: `Sumi · Idle · last run ok 0.4 s`. Running: `Sumi · Running 41 s · Run #22 · 312 / 1,204 [Stop]`. Task: `Sumi · Task running · 40 / 300 [Stop]`. Error: `Sumi · Run #23 error` (click opens the tab at the traceback). The item is painted only when the editor paints, so during a Run-window run the Run window is what shows |

**Debugger and inspection**

| # | Ruling |
|---|---|
| U15 | **debugpy and SEC-NF-01.** The debugpy socket is a documented, user-initiated exception to "every listener is authenticated": loopback only, a random port, opened only by an explicit command, a persistent "Debug server listening on 127.0.0.1:<port>" indicator while it is open, and a policy key to forbid it. If the pinned debugpy supports an access token, we use it (TDD open item) |
| U16 | **Debugger exclusivity.** While a debugpy client is connected, Debug runs in Sumi are disabled with a reason. After the client disconnects, they are re-enabled if `sys.gettrace()` is clear; otherwise Sumi prompts (D25) |
| U17 | **The Paused window** (D3) holds the whole 1.0 debugger: call stack, variables, watches, an evaluate console, a read-only source view and Continue / Step over / Step into / Step out / Stop. It is modal, so editor input is blocked by the platform. The REPL is suspended while paused; the evaluate console replaces it. Docked debug panels are Later |
| U18 | **Watches.** *Safe watches* refresh automatically while paused and after each run. They may use names, attribute reads through the safe resolver (no getters, E1), literal subscripts, and the built-ins `len`, `type`, `isinstance`, `repr` (on built-in types only), `str`/`int`/`float`/`bool` (on built-in types), `abs`, `min`, `max`. Any other expression is a *manual watch*: marked "may run code", evaluated only when the user clicks refresh. `len(sel_assets)` is safe |

**Namespaces and aliases**

| # | Ruling |
|---|---|
| U19 | **Selection aliases** are `sel` (selected level actors), `sel_assets` (selected Content Browser assets, as asset data), `world` (the editor world) and `subsys(cls)`. They are written into the Session only **while idle, when the key is absent or still holds the object we last wrote** (identity check). Once user code rebinds a name, Sumi never touches it again in that session. Shelf and snippet runs are Isolated and get the aliases bound in their fresh namespace. Hover on an alias says "selection as of <time>" |
| U20 | **Ctrl+Shift+V** inserts the D30 line `sel = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()` at the caret. It is an ordinary user assignment, so the alias rule (U19) treats `sel` as user-owned from then on |

**Keymap (Charcoal preset additions, all only while a Sumi widget has focus; checked by G5)**

| # | Ruling |
|---|---|
| U21 | **F5**: Debug run when idle; Continue when paused. **Ctrl+F5**: run the tab's entry point (RUN-13: reload its package, then call it). **Shift+F5**: Stop |
| U22 | Quick fix **Ctrl+.** (PyCharm preset: Alt+Enter). Format document **Shift+Alt+F** (PyCharm preset: Ctrl+Alt+L). Run to cursor **Ctrl+F10**. Evaluate selection in the paused frame **Alt+F8**. Edit breakpoint condition **Shift+F9**, or right-click the breakpoint glyph |
| U23 | **Shadowing, stated plainly.** While focused, Sumi shadows Ctrl+Space (Content Browser drawer), Ctrl+H, Ctrl+Shift+F, Ctrl+G, F10, F11 and Shift+F11. It never takes Ctrl+Tab, F1 or Alt+F1. The PyCharm preset's Ctrl+B (go to definition) shadows Browse to Asset, and the VS Code preset's Ctrl+P (go to file) shadows Open Asset, while focused. The "Shadowed Unreal shortcuts" page lists all of them |
| U24 | **One storage, two views for rebinding.** Sumi's keymap page and Unreal's Keyboard Shortcuts both edit the same `SumiEditor` TCommands bindings in `EditorKeyBindings.ini` |

**Layout and windows**

| # | Ruling |
|---|---|
| U25 | **Right sidebar tabs:** Variables, Watch, Environment. The debugger lives in the Paused window (U17). The bottom area holds Output, REPL and Problems |
| U26 | **Only two Sumi windows are modal:** the Run window and the Paused window. Crash-recovery review, Save as snippet, the trust prompt, the snippet parameter form and settings are non-modal (docked panels, inline banners or non-modal windows) |
| U27 | **A standalone "Sumi Shelf" dock tab** shows the same shelf bar without the editor tab, alongside the Level Editor toolbar and Tools menu entries (D28) |
| U28 | **One "Sumi" submenu** in the actor and asset context menus. It holds Copy as Python, Insert into Sumi and Run on selection ▸ (shelf items) |

**Persistence and recovery**

| # | Ruling |
|---|---|
| U29 | **Storage paths** (D17): `%LOCALAPPDATA%\Sumi\<EngineVer>\<ProjectHash>\` on Windows. `<ProjectHash>` is the project name plus the first 6 hex digits of SHA-256 over the normalised absolute `.uproject` path (lower-cased on Windows), for example `CitySample-7c41e2`. It is recomputed at each editor start. A `projects.json` index maps hashes to paths, so a moved project's old journal is found and offered for import |
| U30 | **Crash-recovery review (PER-03).** The banner count *n* is the number of buffers whose journal holds changes not on disk. **Conflict rule:** if the disk file changed after the journal's base version, the recovered buffer opens as `name (recovered)` next to the disk version with a diff, and nothing is overwritten. **Discard** moves the journals into local history; it never deletes them. **Restore from local history** replaces the buffer text as one undoable edit and never writes to disk until the user saves |

**Snippets and shelves**

| # | Ruling |
|---|---|
| U31 | **Parameter placeholders** in snippet bodies are `${param:<name>}`, declared in the item's `params` block (D28). Values are substituted as `repr` literals. The form appears once per run of the item ("prompt once"), and the last-used values are remembered per user per item |
| U32 | **Selection parameter types**: `actors` and `assets`, whose default `"$selection"` takes the current selection. The form shows the count and lets the user pick again |
| U33 | **Publish to library** writes the item file into the chosen tier folder, checking it out through source control when needed. For the studio tier, it also writes the updated manifest entry (SHA-256), or produces the change for the library owner when the user has no write access |
| U34 | **Priority alignment.** J6 is a 1.0 journey, so the SNP items it needs (forms, publish, manifests for the project tier) land in **M4** (1.0). The studio tier follows the cut list (§7.1, item 4) |
| U35 | **SNP-07 cites SEC-01 and SEC-02** (trust), not SEC-03 |

**Environment, reload and settings**

| # | Ruling |
|---|---|
| U36 | **Environment doctor checks** (ENV-04): interpreter path and version; `sys.path`, with duplicates and shadowed modules; startup-script failures (`init_unreal.py` errors from this session's log); compiled extensions whose ABI tag is not cp311 for this platform; `PYTHONPATH` and `UE_PYTHONPATH` influence; Developer Mode and restrictive mode; language-server binaries and their hashes; stub freshness; journal-folder write access; and the effective policy. **Sumi ships no `init_unreal.py`** (D7); the doctor lists third-party ones found on `sys.path` |
| U37 | **Reload report (RLD-04)** appears as an output block with actions (Reset session, Show holders). Stale Session names, meaning names bound to instances of classes from reloaded modules, are listed by name |
| U38 | **Settings precedence for overlapping keys:** a policy lock wins; otherwise the user's value (global or per-project user layer); otherwise the project default; otherwise the built-in default. The effective-value inspector shows which layer won |
| U39 | **The trust prompt** is an inline banner on the buffer: "Run is disabled: `<folder>` is not a trusted location. [Trust this folder] [Run once] [Learn more]". Default focus is on neither action. **Run once** remembers nothing and is written to the audit log. **Trust this folder** is remembered per user, and Settings ▸ Trusted folders lists and removes entries. Trust is per folder for local files; studio libraries use hash pins instead (D28) |
| U40 | **Policy file:** JSON, `Policy.json`, at the paths in RT-18. The `.ini` name in the draft mockups is withdrawn |
| U41 | **Charcoal framing.** CE2 (Charcoal Editor 2) is a third-party commercial plug-in for Maya, not an Autodesk product. The SPEC says "Maya's Script Editor and the Charcoal Editor 2 plug-in" |

---

## 9. Amendments after the v1.0 review (RV-01 – RV-24)

**Status:** Accepted, 2026-10-28, by the chief architect. Five review lenses each had an independent refuter: runtime, editor core, studio and delivery, UX and mockups, QA and consistency. They checked SPEC v1.0, TDD v1.0 and the mockups against this log and the local 5.8 install. Of 112 findings, 106 survived the refuters, and the refuters added 2 more; all 108 are accepted with the refuters' fixes. The rulings below are the ones that change or clarify a decision above; the rest are text fixes and need no ruling. **Where a ruling here conflicts with §7, §8 or an ADR, this section wins.** The finding IDs (RT-F, CORE-F, STU-F, UX-F, QA-F, and -M for findings the refuters added) refer to the review records.

**Runtime**

| RV | Ruling | Amends | What changes |
|---|---|---|---|
| RV-01 | Accepted (RT-F01, critical) | D2 §1, 07a Q1 | **The watchdog forces a GIL hand-off after every `Py_AddPendingCall`.** In CPython 3.11 a pending call posted from a thread other than the main thread does not set the eval breaker (`_Py_ThreadCanHandlePendingCalls()` is `_Py_IsMainThread()`, `Engine/Source/ThirdParty/Python3/Win64/include/internal/pycore_pystate.h:57-61`). A pure-Python loop never re-takes the GIL, so it would never pump and could never be stopped. The 07a probe missed this because its posting thread held the GIL. The watchdog therefore owns one `PyThreadState` per Python generation (`PyThreadState_New`). After a successful post it calls `PyEval_RestoreThread` and then `PyEval_SaveThread` at once, outside `PostLock`. That costs one switch interval (5 ms) rounded up to the OS timer resolution, about 5–7 ms in the editor, which runs at 1 ms, of game-thread latency per pump request. An atomic `bInPython` is set before `bPythonAlive` is re-checked. On Python shutdown the game thread waits, with the GIL released, until `bInPython` is false, then clears and deletes the watchdog's thread state under the GIL. Native-interval reporting (§5.8 of the TDD) subtracts the hand-off wait. The tier-3 suite, on Win64 under timeBeginPeriod(1), posts from a native thread with no thread state and no other Python threads, against `while True: pass`, and passes when the callback runs within one period plus 10 ms. `SetAsyncExc` is not a liveness mechanism; the hand-off is |
| RV-02 | Accepted (RT-F02) | D2 §2, U17 | **The Run window and the Paused window are popup windows** (`.IsPopupWindow(true)`, as the engine's slow-task modal does at `FeedbackContextEditor.cpp:530-536`), so neither ever becomes the active top-level window that `parent_external_window_to_slate` parents tool windows to. Before either window is destroyed, Sumi detaches its Slate child windows (`RemoveDescendantWindow`) and, on Win64, moves every top-level HWND it owns to the main frame; once the window is gone, it attaches the kept children to the root window (TDD §5.5.3 Teardown). The root's `AddChildWindow` is never called while a child's old parent is alive, because it `check`s (`SWindow.cpp:1863-1872`). G5 confirms that Esc and Shift+F5 still reach both windows. G2(5) adds a PySide window parented during a 2 s run, which must still be visible and owned by the main frame after the run |
| RV-03 | Accepted (RT-F04) | U16, DBG-06 | **debugpy's tracer is turned off on the game thread whenever no IDE is attached.** The in-process `listen` installs pydevd's tracer on every thread before any client connects (measured at about 6.5× on a call-heavy loop). Right after `listen` returns, and whenever the 2 Hz poll sees the client disconnect, Sumi calls `debugpy.trace_this_thread(False)` on the game thread; on connect it calls `trace_this_thread(True)`. If the pinned debugpy fails that test, the fallback is an owner row "debugpy listening, no client" with swap-and-restore, and DBG-06 is limited to "until the debug server is started" |
| RV-04 | Accepted (STU-F01 as modified, STU-F03, OI-92) | U15, RT-11 | **The debugpy listener is hardened and ships with its policy key.** Loopback is reachable by every local user's processes, and pydevd treats an HTTP POST as a DAP frame and runs `evaluate` without a frame. So: (1) the listener refuses any connection whose first bytes are not `Content-Length:` and any second client while one is attached; it does not close on first attach and has no idle timeout, so re-attach keeps working. (2) Turning `debug.allow_debug_server` off mid-session, or editor shutdown, closes it through pydevd's `do_kill_pydev_thread()`. (3) A minimal policy resolver (machine and studio layers, with the RV-06 owner check) for `debug.allow_debug_server`, and the audit event `debug.server_started`, land in **M2** with the listener. The token question (U15's open item) stays settled: the pinned debugpy has no usable token |
| RV-05 | Accepted (RT-M01, RT-F03) | RT-02 | **The native re-entry check uses `gilstate_counter` on 3.11** (frame depth fails for generators). It adds a `bUserCodeActive` pump gate, so Python run after `_run` returns never pumps, and it re-captures the mark at each supervised evaluate in the Paused window, so those evaluates pump and can be stopped |

**Security and policy**

| RV | Ruling | Amends | What changes |
|---|---|---|---|
| RV-06 | Accepted (STU-F02, critical) | RT-18, D20 | **The machine layer is honoured only if it is admin-owned.** On default Windows any standard user can create `%ProgramData%\Sumi` before IT does. The layer is used only if `Policy.json` is owned by Administrators, SYSTEM or TrustedInstaller and no other SID has write-data, append, WRITE_DAC, WRITE_OWNER or DELETE on it. The folder must be owned by one of those SIDs, with no other SID holding FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER or DELETE; the inherited Users add-file right is allowed. On POSIX the file and every ancestor must be owned by uid 0 with no group or other write. A layer that fails the check is **ignored**, not failed closed, so a squatter cannot force closed values on other accounts. A persistent banner and a doctor error name the file and its owner |
| RV-07 | Accepted (STU-F06, QA-F01, UX-F03, STU-F05) | U40, D21 | **Canonical policy and settings keys are the TDD §11.2.2 dotted snake_case IDs**, identical to `FSumiSettingKey.Id`: for example `run.presentation` (`run_window` or `live_docked`), `run.pump`, `debug.allow_debug_server`, `trust.mode`, `trust.roots`, `trust.allow_user_trust`, `trust.allow_run_once`, `language_services.*`. The SPEC labels `AllowDebugServer` and `AllowUserTrust`, and the camelCase names in the draft mockups, are withdrawn. **Unknown keys:** in a layer whose `schema` is not newer than the build's, an unknown key at the top level or under `trust`, `debug`, `bridge`, `mcp`, `telemetry`, `library`, `language_services`, `python` or `storage` makes the layer invalid. Its fail-closed keys then close, and the banner names the line and the nearest known key. Unknown keys elsewhere, and in a newer-schema file, give one warning. **An invalid layer closes only permissions:** Python at runtime, user binaries, trust, debug server, bridge, MCP, telemetry and the studio library. Editing aids (completion, language services, ruff, sumi-check, the console executor) are not closed: the invalid layer's values for them are ignored, and the other layers, else the built-in defaults, apply (RV-08; clarified after the v1.1 check, V-persist-08) |
| RV-08 | Accepted (STU-F04, STU-F07, STU-F21, STU-F22) | D21 | **Merges cannot loosen a lock or destroy data.** `trust.roots` is the union of the layers at or above the strongest layer that sets `trust.mode = policy_only` or the new `trust.roots_locked = true`; roots from lower layers are ignored and the inspector says so. Retention keys have ranges (history 7–3650 days and 50–100,000 MB, run logs 7–3650 days, audit 30–3650 days). An out-of-range value invalidates only that key, and `audit_days` merges with max. Built-in defaults never take part in merges. The newest history entry of each buffer, and every entry younger than 24 h, are never swept. The telemetry opt-in is a per-user key that no policy layer can set (OI-91), and `telemetry.endpoint` is honoured only from the machine and studio layers. `storage.root_override` cannot be set from the project layer, and a policy value must contain the `{User}` token |
| RV-09 | Accepted (STU-F08, QA-F02, UX-F08) | D28, SNP-07 | **A changed studio item prompts; it does not block by default** (D28: "changed content prompts the user"). The prompt is non-modal with no default action: [Restore published & run] [Run modified copy once] [Cancel]. Running the modified copy is audited. `library.studio.require_pinned` defaults to false; when any layer sets it to true, the item cannot run and offers only Restore. Project-tier items with a manifest show "Unpublished changes" and run without a prompt |
| RV-10 | Accepted (OI-35) | D3, RT-11 | **debugpy is vendored in a private folder** (`Content/Python/_sumi_vendor/debugpy`, not `Lib/site-packages`) and is prepended to `sys.path` only on the first "Debug in VS Code", so it never shadows a studio's own debugpy |

**Delivery and gates**

| RV | Ruling | Amends | What changes |
|---|---|---|---|
| RV-11 | Accepted (STU-F11, CORE-F10) | RT-10, RT-13 | **The Stage 2 stub transformer is built in M1 for G3 and shipped in M2.** The docstring parser and transformer (TDD §7.4.4–§7.4.5) and a test-only probe client run against CI-pinned Pyrefly and basedpyright inside the G3 harness during M1; nothing third-party ships in M1. The `.pyi` package and its use by the shipped server land in M2. The Phase 0 re-baseline budgets this work |
| RV-12 | Accepted (STU-F12) | D4, §3 G3 | **G3's "no server passes" fallback has thresholds.** The shipped server is the highest scorer that meets the RSS and diagnostics thresholds. PERF-07 then measures three user-code probes (`self.`, a project module, `os.path.`) at warm p95 ≤ 400 ms, and `unreal.*` completion is gated by PERF-06 (runtime completion). The M2 exit and INT-14 read "G3 thresholds, or the recorded fallback thresholds, hold in CI" |
| RV-13 | Accepted (STU-F13) | §3 G2 | **G2 has no fallback in the Run window.** A G2 failure in Live (docked) keeps the Run window as default and docked as Preview, and cut item 3 can remove docked. A G2 failure in the Run window means Phase 0 does not exit. The chief architect either extends P0-2 by up to 4 weeks or re-plans at the go/no-go, and records the decision in §7.2. Phase 0 exits on "G1 green or its fallback invoked, and G2 green in the Run window" |
| RV-14 | Accepted (STU-F14 as modified) | RT-20, COMPAT-05 | **For an engine released before 1.0 GA, the 6-week clock starts at GA**, and that engine is the 1.0.x port. RT-20 is otherwise unchanged |
| RV-15 | Accepted (QA OI-84) | G6 | **G6 passes for a release only when driver B's 10,000 editor kills,** including kills during runs and while paused, are green on that release candidate. Nightly status uses driver A's 10,000 host kills plus at least 200 driver-B kills per agent. One full 10,000-kill driver-B run is also required before M1 exits, because the M1 build is a public preview |

**UX**

| RV | Ruling | Amends | What changes |
|---|---|---|---|
| RV-16 | Accepted (UX-F01, mockup report) | U14, U2, REPL-07 | **Nothing outside a Sumi modal is drawn while it is open.** While the Run window or the Paused window is up, only the modal hierarchy, tooltips and notifications are drawn (`SlateApplication.cpp:1476-1497`), and input outside it is consumed. WS-07, the Sumi tab and the REPL keep their last painted frame and cannot be clicked. WS-07 therefore has **no visible Paused text**; Paused stays in the status snapshot for screen readers and the API. U14 gains `Sumi · Run #N not executed` and `Sumi · Run #N cancelled` (a click opens the run block). The Running pattern is live only in task mode and in Live (docked) Preview. The REPL's "suspended" message moves to the Paused window's footer |
| RV-17 | Accepted (UX-F02, mockup report item 3) | — | **No dimmed "input blocked" frame.** Painting a dimmed editor would mean drawing the whole main frame, editor utility widgets included, inside the user's script, which is the RT-03 hazard. The editor behind the Run window and the Paused window shows its last painted frame, undimmed. The Run window's text ("Clicks and keys outside this window are ignored until the run ends") is the cue. A transparent-scrim modal is noted as a post-1.0 idea (Later) |
| RV-18 | Accepted (mockup report, U25) | U25, REPL-01 | **One REPL prompt strip** sits below the bottom area's tab body. It shows for the Output and REPL tabs and hides for Problems; the palette command "Focus REPL" reveals it. Output shows every run as numbered blocks, REPL runs included. The REPL tab is a filtered view of the output store (route = REPL) plus the history list |
| RV-19 | Accepted (UX-F04) | U23 | **The shadowed-shortcut enumeration is completed:** F4 (Show Actor Details), F8 (Possess or Eject Player), Ctrl+PageUp and Ctrl+PageDown, and Ctrl+M (Move Selection to Current Level, used by the CFG-08 focus toggle) join the U23 list. The page computes the list from the main-frame bindings, the Level Editor command list and the consume list |
| RV-20 | Accepted (UX-F05) | J5, RT-09 | **Reimports are never undoable** (RUN-07). In J5 each task step runs in its own transaction `Sumi: task #N`, so property edits in a step can be undone; the reimports themselves cannot, and the run block says so |
| RV-21 | Accepted (CORE-F04, CORE-F05) | D6, U18 | **Inspection never loads and never runs user code.** Container elements of `unreal.Array`, `Map` and `Set` are subscripted, iterated or paged only when the element property is not a soft, soft-class or lazy object reference; otherwise they resolve at type level and show export text through C++. "repr on built-in types only" means a bounded `saferepr`: it recurses only into exact list, tuple, dict, set and frozenset (depth 2, 20 items per level), calls repr only on exact int, float, bool, None, str and bytes (cut at 200 characters), and shows anything else as `<TypeName>` |
| RV-22 | Accepted (OI-43, QA-F13) | U31 | **Inserting a snippet never shows a form.** Parameters become mirrored tab stops holding their default literals (SNP-02). U31's once-per-run form applies to runs only |
| RV-23 | Accepted (OI-61) | — | **No exception to D14(5).** Completion never calls Python from a Slate handler. To remove the first-keystroke lag, `unreal.*` members come from `FUnrealNameIndex`, and the Session's top-level names, plus the member lists of their types, are copied into a C++ cache in the idle slice after each run |
| RV-24 | Accepted (OI-05) | D12 | **The Fab fallback is decided now.** If Fab refuses bundled third-party executables, the Fab package ships without them, plus a GitHub binaries pack that the environment doctor links to |

**Refuted, no change:** QA-F23 (split TDD.md into per-section files; the file stays whole, because the split is a structural preference and its anchors are used by SPEC, README and this log), STU-F18 (cut list too small; the Phase 0 re-baseline is where measured throughput enters), UX-F15, UX-F16, UX-F18 and UX-F19.

---

## 10. Owner rulings after v1.1 (PO-01 – PO-03)

**Status:** Accepted, 2026-10-29, by the product owner. The owner's request after v1.1 was: "output should be on the top and there should be line by line execution and realtime execution similar to maya's script editor". The three rulings below read it as follows. **"Output on top"** is Maya's Script Editor layout: the history (output) pane above the input (code) pane. Sumi already had it as an M2 option (WS-03); it becomes the default. **"Line by line execution"** is three things Maya and Charcoal Editor 2 users expect: (a) run the line under the caret with one key and keep going down the buffer one statement at a time; (b) the output reads as a history of what ran, each executed statement echoed with its output directly under it, as Maya's history pane echoes the code it executes; (c) the code view shows which lines ran. **"Realtime execution"** is code that runs in the live editor session the moment the key is pressed, which is already true (the Session namespace, in process, D8), **and** a run the user can watch: echo and output appear in the output area as the code produces them, with the executing line shown, not all at once when the run ends. **Out of scope:** Maya's *Echo All Commands* (editor UI actions echoed as Python) is an action recorder. It stays Later (D9, D30; SPEC §13 "Closed since v0.9"), because the request names execution, not recording; SPEC §13 #11 asks the owner whether one is wanted. **RV-16 and RV-17 are unchanged:** nothing outside a Sumi modal is drawn while it is open, and there is no dimmed frame. **Rename:** the "bottom area" is now the **output area** in SPEC, TDD and the mockups; quoted text in §1–§9 of this log and in research 13 and 14 keeps the old name. **Where a ruling here conflicts with §9 or earlier, this section wins.** SPEC v1.2 and TDD v1.2 implement these rulings.

| PO | Ruling | Amends | What changes |
|---|---|---|---|
| PO-01 | Accepted ("output on top") | U25, RV-18, WS-02, WS-03 | **The output area is on top by default.** The bottom area is renamed the **output area**: the Output, REPL and Problems tabs and the one REPL prompt strip (RV-18 unchanged). The Sumi tab, top to bottom: toolbar; shelf bar (M4); the output area (tab row Output · REPL · Problems, tab body, then the prompt strip directly below the tab body); a horizontal splitter; the code area (editor tabs, breadcrumbs, cell headers, task banner). The left side bar (Files, Snippets, Outline) and the right side bar (Variables, Watch, Environment) span the full height beside both, so the REPL prompt sits between the output and the code, like Maya's input under its history. Default split (proposed): the output area takes 40% of the centre column's height, each area keeps at least 120 px, and sizes persist (WS-02). The user setting `layout.output_position` is `top` (default, Maya layout) or `bottom` (the v1.1 arrangement, PyCharm style); no policy layer sets it. **View ▸ Output Position ▸ Top / Bottom** and the palette commands **Move Output to Top** / **Move Output to Bottom** move the area at once, keep its size and persist. WS-03 lands in **M1** (was M2). The Run window still opens over the output area (LRS-01; PO-03) |
| PO-02 | Accepted ("line by line execution") | D8, RUN-02, RUN-08 wording, OUT-03, OUT-05, OUT-10, the REPL header rule | **Line-by-line execution.** (a) **Run line and advance** (new RUN-26, M1): command `RunLineAdvance`, Charcoal **Alt+Enter** (proposed). No main-frame, Level Editor or consume-list command binds Alt+Enter in 5.8; the only bindings are tool-local, the PCG HLSL editor's Apply Changes (`Engine/Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:69`) and the game viewport's fullscreen toggle (`Engine/Source/Runtime/Engine/Private/GameViewportClient.cpp:674`, `bAltEnterTogglesFullscreen`), and neither acts while a Sumi widget has focus. The PyCharm preset leaves it unbound, because Alt+Enter is Quick fix there; it is in the palette and the Run button menu. With no selection it runs the current line or statement, then moves the caret to the first non-blank column of the next statement (not blank, not comment-only, not a continuation line, and not a clause of a compound statement whose body the run was inside, so stepping through a body never re-runs its header); with a selection it runs the selection, clears it and moves the same way from its last line; at the end of the buffer the caret stays and the status bar says "End of buffer". Each press is one numbered run (U8) with its own undo step (RUN-07). **RUN-02's "statement"** is defined: a line that opens a compound statement (its first token is `if`, `while`, `for`, `try`, `with`, `def`, `class` or `async`, or its last token, ignoring a comment, is `:`) runs the whole compound statement with its body and continuation clauses (`elif`, `else`, `except`, `finally`, `case`); a line inside an open bracket, an open triple-quoted string or after a trailing backslash extends to the end of that logical line; a decorator line extends through the decorated definition, and a `def` or `class` line takes its decorators; otherwise the single line runs (an indented line through the RT-14 wrapper). Run current line (Ctrl+Alt+Enter) and Run line and advance use the same extent. (b) **Statement mode** (new RUN-27, M1) for every route whose auto-print is On (selection, line, cell, buffer, REPL, console executor; bridge from 1.1) and API runs that target such a tab. The module is compiled once exactly as today, so every compile-time error, including a misplaced `from __future__` import, ends the run `not executed` before any code runs (RUN-16 unchanged). Then each top-level statement is compiled as its own code object, `compile(ast.Module(body=[stmt], type_ignores=[]), filename, "exec", flags=future_flags, dont_inherit=True)`, where `future_flags` ORs the `compiler_flag` of every feature that the module's leading `from __future__` imports name, and the statements run in order in the same dict, each through one C call, `_sumi.run_statement(run_id, index, code, ns)`, which checks Stop, echoes the statement and runs it, with the same filenames and line numbers. An exception or Stop ends the run at that statement, even when the script swallowed the Stop. A bare string statement after the first is echoed but not executed, because compiled alone it would rebind `__doc__` in the Session; the first statement keeps one-`exec` semantics, and a trailing string that the auto-print rewrite turned into a display call runs. Evidence: [probes/v12/stmt_mode.py](probes/v12/stmt_mode.py) on the engine's `python.exe -I -B` (3.11.8): a mid-buffer string compiled alone sets `__doc__`, skipping it keeps the leading docstring, and `from __future__ import annotations` passed in `flags` gives the same annotations as one `exec`. Debug runs (F5) on these routes use statement mode; the runner's frames are never user frames, so a step at a statement boundary continues to the next statement's first line. Run File, Isolated runs, shelf and snippet items, entry points, reload routes and task steps do not use statement mode (one `exec`, or for entry points, reloads and task steps their own calls). Auto-print (RUN-08) still prints only the last top-level expression. Cost: one C call per top-level statement (proposed budgets ≤ 5 µs per statement to run and ≤ 10 µs per statement for the extra compile in `Starting`). (c) **Input echo** (new OUT-12, M1): an `echo` line type that C++ appends from the run's journal snapshot, each line with its buffer line number and each snapshot line once per run (REPL and console: `>>> ` and `... `), so each statement's echo is followed directly by its output. Normal folds a statement's echo after 6 lines (proposed); **Echo All** now means "shows echo unfolded" (it no longer expands REPL and buffer headers); Minimal hides echo. The toolbar toggle **Echo input**, on by default, is the per-type toggle. Echo goes to the per-run log (OUT-08) and not to LogPython (D19). REPL and console headers no longer repeat the submitted code unless none of the run's echo is shown; `<sumi:REPL#42>, line 1` scrolls to run #42's echo; Copy as script (OUT-10) copies echo as runnable Python. (d) **Ran-line markers** (new EDT-18, M2): after a statement-mode run of a buffer, the gutter marks the statements that completed (ok), the one that raised (error), the one Stop ended (cancel) and the one that ended the run through `SystemExit` (exited), each with a tooltip such as "Ran in #22 · ok"; the marks keep the line-by-line trail: a run replaces only the marks on the lines of the statements in its range, so marks from earlier runs stay, each naming its own run (five Alt+Enter presses leave five ok marks), a whole-buffer run replaces all of them, a Run File of the buffer and View ▸ Clear Ran-Line Marks clear them, an edited line loses its mark, and lines map through edits with `sumi::TextBuffer::MapLine` |
| PO-03 | Accepted ("realtime execution") | LRS-01, LRS-09, TDD §5.4.7, §5.5.3, §5.6 | **The Run window is the live output area.** Short runs, which end before the first pump (about 300 ms, RT-04), show their echo and output together on the next painted frame, because the editor cannot paint while Python holds the game thread; per-line painting is never promised for them. Longer runs: the Run window opens on the first pump **exactly over the output area**, with the same screen position and size, from the output widget's cached geometry captured at entry (at the top by default). Each dimension has its own minimum: 480 wide, and tall enough for the window's other rows plus 5 lines of the live run view (proposed); a shorter output area gets a window of the minimum height, growing toward the code area, and an area narrower than 480 gets a 480-wide window from its left edge, both kept inside the monitor's work area; a note that appears while the window is at the minimum height grows it, so the live run view keeps 5 lines; with the Sumi tab not visible, the window is 640 × 360, centred on the active window (proposed). Its Output tail row becomes the **live run view**: a read-only `SCodeView` over `FSumiOutputLineSource` filtered to the run, showing echo and output as each pump drains them (a written line is painted by the first pump that starts at least 5 ms after it is written, about 110 ms at the G2(3) floor of 10 Hz for a script that prints between sleeps), with follow mode and the "↓ n new lines" chip (OUT-09); folding follows the Output preset. The Location row shows the executing line with its source text from the snapshot, "Line 43 · `for a in actors:` · fix_materials.py" (and the innermost frame if different), and in statement mode "statement 7 of 12" (proposed). `FSumiInputPreProcessor` passes mouse-wheel events, scrollbar drags, clicks on the "↓ n new lines" chip and clicks on echo fold chips aimed at the live run view, as docked mode passes the same input aimed at the output view; every other key and click outside the Stop targets stays swallowed. G2(5) gains "wheel and scrollbar in the Run window scroll its view; no input reaches any other window". When the run ends, the window is destroyed (teardown unchanged) and the output area under it shows the same block, scrolled the same way: a following live view hands over follow mode, with the run's last line at the bottom, and a scrolled-back one keeps the line at its top where it was on screen, or at the output view's nearest row where that place is outside the view; a run that the active tab cannot show (Problems, or REPL for a run that is not a REPL run) switches the area to Output when it starts, short runs included. The code area is not drawn during a run (RV-16, RV-17), so the executing line is shown in the Run window; Live (docked) Preview (LRS-09) also draws an execution arrow in the code view's gutter at the line of the last pump. Task mode (LRS-05) is unaffected |

**Refined when v1.2 was verified** (the rulings stand; these details replace the first wording): the Run window's minimum height covers 5 lines of the live run view, because the old 480 × 200 px minimum, which fitted the v1.1 output tail, left it no line; "scrolled the same way" is met by follow state, because the live view and the Output view differ in height, so a literal "nothing jumps" could not hold; each statement runs through one C call, `_sumi.run_statement`, instead of `_sumi.echo` followed by `exec`, so the started and returned counts are exact wherever a Stop lands and a swallowed Stop cannot let the next statement start; the ran-line marks keep the trail of an Alt+Enter walk instead of belonging only to the buffer's last run; and the Alt+Enter check found one tool-local binding (PCG), which changes nothing for Sumi. In the second verification round: a written line is painted by the first pump that starts at least 5 ms after it is written, about 110 ms at 10 Hz for a script that prints between sleeps, not "within about 100 ms", because the output thread collects for up to 5 ms and a sliced sleep is pumped only at its slice ends; a shorter output area grows the window toward the code area, up from the area's bottom-left in the bottom layout, instead of anchoring it at the area's top-left, a note that appears at the minimum height grows it again, and a hidden Sumi tab gives a 640 × 360 window, centred, because the minimum height now covers the live view; a scrolled-back hand-off falls back to the output view's nearest row where the old place is outside the view; clicks on echo fold chips in the live run view pass too, so folding works during a run; a run that the active tab cannot show switches the area to Output when it starts, short runs included, so an Alt+Enter walk keeps its history in view; and entry points, reloads and task steps run through their own calls rather than one `exec`.

**Not adopted:** Live (docked) as the default presentation (RT-03, RV-13): the engine cannot paint the Sumi tab while Python holds the game thread except through the RunSupervisor pump, and drawing the whole main frame inside a user's script is the RT-03 hazard, so PO-03 makes the Run window be the output area during a run instead. An each-expression auto-print: only the last top-level expression prints `# Result:` (RUN-08; proposed: no "each expression" option in 1.0). An action recorder (Maya's Echo All Commands): Later (D9, D30), with the owner question in SPEC §13 #11.
