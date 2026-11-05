# Dream Python Editor: status of slices A and B, the PO-04 restyle, PO-05 and the PO-06 rename

Slice A of M1 is the user-visible loop of TDD §15.2 (build brief items 1 to 18; ARCHITECTURE.md section 5 maps each
item to its TDD sections, files and owner). Slice B builds the slice-B rows of the slice-A status as B01 to B108: undo
on `dream::UndoTree`, the piece tree, smart editing, find and replace, Go to line and zoom; live completion; dream-check,
the Stage 1 snapshot and Problems with quick fixes; local history, the Recovery Review panel, the restore guard and
safe mode; the secondary-instance and compat namespaces and the storage-root setting; external-change detection and
the file bars; the audit, crash breadcrumbs and trace channels; and the G2, G5 and G7 suites, a G6 driver prototype and
the §2.5 static checks. ARCHITECTURE.md section 5 ("Slice-B items") maps each B-id to its TDD sections, files, owner
group and tests. Slice B was reviewed in rounds 1 to 5 on exact diffs; round 4 found no major, which ended the loop,
and round 5 checked the minor fixes made after it.

The PO-04 restyle (owner ruling, 2026-11-03) gives Dream the clean look of Maya's Python script editor: Maya-minimal
chrome (a slim menu bar, one row of icon buttons, plain input tabs, hidden extras), Maya-style history (`# Result: … #`,
`# Error: … #`) and ten themes with Darcula as the default. Features stay; only presentation changed. The theme, chrome
and output groups built it on the slice-B tree, with follow-ups from the run, tests and codeview groups. Review rounds
r6 to r8 ran on exact diffs until r8 found no major; r9 checked the post-loop patch, the integration followed, r10
checked the patch after it (no major), and the final verification passed (attended 286/286, the click cases green for
the first time). Round r11 checks the last minors. After the integration the Run window's Stop acts on the press
(ARCHITECTURE A216). ARCHITECTURE.md section 5 ("Restyle items (PO-04)") maps each item to its PO-04 point, files,
owner group and specs.

PO-05 (owner ruling, 2026-11-04, with amendment PO-05-A1) adds a left side bar, shown by default:
- **Project tree:** the project's `Content/Python`; roots you choose per project in Editor Preferences ▸ Plugins ▸ Dream
  (this project); the `Python` folder of every mounted project plugin and engine plugin (a setting you can turn on and
  off in the editor); and a Scratches section.
- **Structure:** the active tab's classes, functions, methods and `# %%` cells.

Every new unnamed tab is an auto-saved `scratch_N.py` in a per-user scratch folder, `%LOCALAPPDATA%\DreamPythonEditor\Scratches`
by default. The sample scripts used in tests and screenshots are seeded into `Scratches/Samples`. The open tabs come
back on start, now verified across real restarts.

The sidebar, structure and scratch groups built it. Review rounds p1 to p3 ran on exact diffs until p3 found no major,
the architect checked the last small fix patch, and the integration followed. ARCHITECTURE.md section 5 ("PO-05
items") maps each item to its files, owner group and specs, and A227 to A245 hold its choices.

PO-06 phase 3 renamed the product from "Sumi" to **Dream Python Editor** ("Dream" where space is tight); the section
"What the rename contains" below says what changed, what a user's first start copies and what the owner must do. The
history sections of this file keep the names of their time, each with a note.

This file says what the slices and the restyle contain and how to open them, how to build and test them, the latest
results and review rounds, the known risks, what stays deferred, the open items and the decisions only the owner can
make. In the code
each deferral is marked `// Deferred (<target>): …` (`# Deferred (…)` in Python) where the feature would show; slice A's
`// Slice B:` markers are gone. ARCHITECTURE.md section 9 is the short form of the deferred list.

## Open it

1. Build the host project (next section), then open `DreamHost/DreamHost.uproject` in UE 5.8. The host finds the plugin
   through `AdditionalPluginDirectories: ["../Plugins"]`; its binaries land in `DreamHost/Binaries/Win64`.
2. Open the tab from **Tools ▸ Dream Python Editor**, the Window menu, or the console command `Dream.Open`.
3. The output area is on top (PO-01): Output · REPL · Problems, with the REPL prompt strip under the tab body, the
   splitter, then the code area. The tab's View ▸ Output Position, or Editor Preferences ▸ Plugins ▸ Dream, moves it to
   the bottom. Since the restyle the REPL and Problems tabs and the prompt strip are hidden by default (item 15).
4. Charcoal keys: Ctrl+Enter runs the selection or the buffer, Ctrl+Shift+Enter the buffer, Ctrl+Alt+Enter the current
   line or statement, **Alt+Enter runs the line and moves to the next statement** (RUN-26; a hint chip at the caret
   line says so until you use it once), Esc or Shift+F5 stops a run, Ctrl+Alt+Z undoes the last run. A run that lasts
   longer than about 300 ms opens the Run window over the output area, with the live output, "statement k of n" and
   Stop (PO-03). If the editor is busy, the status strip says "Starting… waiting for the editor to become idle" after
   1 s.

Slice B adds:

5. **Editing.** Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z undo and redo (one history per buffer, kept across saves; the context
   menu uses the same history). Enter indents after `:` and dedents after `return`, `pass`, `break`, `continue` and
   `raise`; Backspace goes back to the indent stop; brackets and quotes close themselves and the closer is typed over;
   Ctrl+D duplicates the line.
6. **Find and replace.** Ctrl+F finds and Ctrl+H replaces in the code view; F3 and Shift+F3 step; match case, whole
   word and regex (Alt+C, Alt+W, Alt+R) with a live count; Replace All is one undo step. In the output views Ctrl+F
   searches the output, and the Show menu has Soft wrap.
7. **Go to line and zoom.** Ctrl+G opens Go to line (`L`, `L:C` or `:L`). Ctrl+wheel, Ctrl+=, Ctrl+- and Ctrl+0 zoom
   from 50 to 300 % per tab; a change of the Font size setting applies at once. Ctrl+M toggles "Tab moves focus".
8. **Completion.** Ctrl+Space opens completion in the code view and the REPL prompt (Enter accepts in the REPL); it
   knows `unreal` classes, editor properties and enums and the names of the Session. The Python (Dream) console
   completes live too.
9. **Problems.** dream-check, syntax errors from runs and runtime errors show in the Problems tab, with the count on its
   label. F8 and Shift+F8 walk them; Ctrl+. shows the quick fixes at the caret (for example SUM102 rewrites
   `x.prop = v` to `set_editor_property`), applied as one undo step.
10. **Files.** A file changed on disk reloads by itself when its buffer is clean (one undo step, "Reloaded from disk");
    a dirty buffer shows [Keep mine] [Reload]; a deleted file shows [Save] [Close]; a save to a read-only file shows
    [Make writable] [Save As…]; a save the encoding cannot hold offers [Save as UTF-8]. File ▸ Recent Files lists the
    last 15 files. Tabs show language, dirty, lock and recovered badges. A closed tab's `<dream:…>` link in the output
    reopens it.
11. **Recovery.** After a crash the recovery banner's [Review] opens the Recovery Review panel: per buffer Restore,
    Restore as new tab, Compare (a unified diff) and Discard. Discarded texts go to local history. Scratch buffers that
    changed show their clean text, read-only, under "Not restored yet".
12. **Safe mode.** After two unclean starts in a row Dream opens in safe mode, as with `-DreamSafeMode` or Shift held
    when the tab spawns: no restore, no Python start, no language server, until Leave safe mode, which applies the
    skipped restore.
13. **Storage.** Editor Preferences ▸ Plugins ▸ Dream ▸ Storage sets the storage root override (it applies at the next
    start; a rejected or silent override leaves a notice). A not-executed footer has [Open storage settings] beside
    [Run without snapshot]; a finished run that ran without a transaction offers [Run in transaction]. A second editor
    on the same project journals in its own namespace and names the first one's process, with [View its tabs].

The restyle (PO-04) changes what you see:

14. **Look.** The tab opens in the Darcula theme, with a slim menu bar (File, Edit, History, Command, View, Help) and
    one row of small icon buttons (Open, Save, Run, Run line, Stop, Clear history, Undo last run, Echo commands, Line
    numbers). View ▸ Theme, or Editor Preferences ▸ Plugins ▸ Dream, switches among ten themes at once; user themes are
    JSON files in `%LOCALAPPDATA%\DreamPythonEditor\Themes`.
15. **Hidden parts.** The REPL tab, the Problems tab, the REPL prompt strip, run headers and footers, and the status
    line are off until you turn them on in View; Focus REPL shows the prompt while you use it. The input tabs show the
    name, `*` when dirty and a lock or recovered glyph only while it applies; `line:col`, status messages and health
    warnings (journal, Python, lint) show at the right end of the input tab row. A banner is one slim line with ×.
16. **History.** Echo is muted, results read `# Result: … #`, warnings `# Warning: … #`, and errors `# Error: … #`
    with the traceback lines prefixed `# `; links still open the file. Re-run, Run in transaction, Undo this run and
    Copy are in the history's right-click menu.
17. **Stop.** The Run window's Stop acts on the press, not the release, so a press made while a script blocks the editor
    is not lost when the cursor moves away before the editor catches up; Esc and Shift+F5 still stop a run (A216).

## What slice A contains

Every brief item is built. "Tested by" names the tier-4 specs (`Dream.*`) and the tier-1/tier-2 suites that exercise it.

| # | Item | Notes | Tested by |
|---|---|---|---|
| 1 | Tab `DreamEditor.Main` "Dream", Tools menu, Window menu, `Dream.Open` | | Dream.Shell.TabLayout, Dream.Lifecycle |
| 2 | Layout: toolbar, output on top, 40% splitter (min 120 px), code area, status strip, `layout.output_position` | Status strip has the slice-A WS-05 fields | Dream.Shell.TabLayout, Dream.Visual.Capture |
| 3 | Buffers: scratch `Untitled-N`, file tabs, Ctrl+S, dirty `*`, New/Close/Reopen closed, Ctrl+PageUp/PageDown | Files up to 1 MB (A40) | Dream.Persistence.Journal, Dream.Run.RunFile, Dream.Keys.Focus |
| 4 | Code view, plain mode (TDD §4.15 fallback) with the PyLexer marshaller, gutter, current line | The custom SCodeView moved to G1 (ARCHITECTURE section 9) | Dream.Keys.Focus; tier 1 lexer |
| 5 | Charcoal keymap through `FDreamCommands`, consume list, Numpad Enter | | Dream.Keys.Focus, Dream.Run.LineAdvance, Dream.Supervisor.Input; tier 1 keymap |
| 6 | Execution: run service, routes, `#N` numbering, Starting steps, OnEndFrame entry | | Dream.Run.* |
| 7 | Runner: compile rules, statement mode (RUN-27), echo, auto-print `_`, swaps, Session and Isolated namespaces | | tier 2; Dream.Run.SelectionEcho, Dream.Run.Session, Dream.Run.SyntaxError |
| 8 | `_dream` native module | | tier 2 native contract; every run spec |
| 9 | RunSupervisor: watchdog, pending call, pump, guards, Run window, protections, pre-processor | Paused window, Live (docked) and debugpy are later milestones | Dream.Supervisor.Input, Dream.Supervisor.Stop, Dream.Supervisor.SourceText, Dream.Visual.Capture |
| 10 | Cooperative API: `progress`, `check_cancel`, sliced sleep | | tier 2 stop tests, Dream.Supervisor.Stop |
| 11 | Transactions: one per run `Dream: <tab> #N`, size watch, Undo Last Run | Purged rule per A64 | Dream.Transactions.UndoLastRun |
| 12 | Output: store, forwarder, device, view, follow mode, type toggles, run logs, runtime Problems | | Dream.Output.Follow, Dream.Output.RunLog; tier 1 output |
| 13 | REPL: prompt strip, Enter when complete, Shift+Enter, Ctrl+Enter, history per project | Ctrl+R search is M2 | Dream.Repl.History, Dream.Repl.Prompt; tier 2 classify |
| 14 | Console executor `Python (Dream)` | Live completions since slice B (B63) | Dream.Output.ConsoleExecutor |
| 15 | Journal and recovery: storage root, WAL worker, pre-run snapshot, restore tabs, recovery banner | Restore after a real kill: the slice-B G6 driver prototype (B24) | Dream.Persistence.Journal; tier 1 journal |
| 16 | Settings: `UDreamUserSettings` (Editor Preferences ▸ Plugins ▸ Dream), per-project session | | Dream.Shell.TabLayout |
| 17 | WS-07 status-bar item | | Dream.Output.Follow (error click), Dream.Output.StatusItem |
| 18 | Public Python API (M1): `progress`, `check_cancel`, `sleep`, `clear_output`, `open`, `new_tab`, `run`, `suppress`, `ScriptCancelled`; `Dream.RunFile`, `Dream.ClearOutput` | | tier 2 API tests, Dream.Run.RunFile |

## What slice B contains

Every B-id of the slice-B plan is built, B06's libFuzzer target excepted (ARCHITECTURE A75). "t1" is tier 1, "t2"
tier 2; spec names are tier 4. A-numbers point at ARCHITECTURE.md section 7.

| B | Item | Notes | Tested by |
|---|---|---|---|
| B01 | `dream::UndoTree` | Undo ops store copied text; memory bounded (A69) | t1 `test_undotree.cpp` |
| B02 | Piece tree, `AnchorSet`, `ChangesBetween`, compaction | Same `TextSnapshot` API (A65 to A67); compaction after 2 s idle (A144); `AnchorSet` has no M1 caller | t1 `test_piecetree.cpp`, `DREAM_FULL_SCALE=1` runs; Dream.Buffers.Compaction |
| B03 | `dream::Diff`: line diff, minimal edits, unified diff | Used by Compare, the pending bar and reloads | t1 `test_diff_lines.cpp` |
| B04 | `dream::SearchEngine` | Own regex with Python `re` syntax, bounded per line (A70) | t1 `test_search.cpp` (golden data against Python `re`) |
| B05 | Edit builders: smart Enter, Backspace to the indent stop, auto-close, duplicate line | Smart Enter keeps the code's indentation (A71) | t1 `test_editbuilders.cpp` |
| B06 | Tier-1 gates: T-LEX-001, T-LEX-008, T-CORE-004, T-CORE-009, full-scale runs | T-LEX-001 against the engine's Lib; libFuzzer T-PER-005 not built (owner decision) | t1 |
| B10 | Journal checkpoints in 50 ms / 4 MiB slices | A5's one-go rotation ends; a session's first generation is still one write (A100) | t1 `test_journal.cpp` |
| B11 | Older-major migration | No older major exists yet; tested with a synthetic format 0 | t1 `test_journal.cpp` |
| B12 | g-1's header checked before it is deleted | Closes the slice-A open item | t1 `test_journal.cpp` |
| B13 | `BatchPlanner`; the retry queue replayed as one batch | Closes P2-per-02 | t1 `test_journal.cpp` |
| B14 | Local history store | Entries on save, run, reload, recovered, discarded, closed scratch and restore; 30 days / 500 MB, primary only; pauses while an index file is held (A104); the timeline UI is M2 | t1 `test_history.cpp`; Dream.Persistence.History, Dream.Persistence.Writer |
| B15 | Discarded texts move to history | A recovery resolves only once its entry is durable (A99) | Dream.Persistence.Recovery |
| B16 | Clean text of counted scratch buffers with the pending bar | Replaces slice A's direct restore of scratch text (A108) | t1; Dream.Persistence.Recovery |
| B17 | Recovery Review panel | Under the banners (A109) | Dream.Persistence.Recovery |
| B18 | Restore guard and automatic safe mode | Never by the guard in automation sessions (A107) | Dream.Persistence.SafeMode, Dream.Persistence.Recovery |
| B20 | Secondary-instance namespace, staging, merge, orphans, View its tabs | Replaces A39's Disabled journal (A110 to A115) | Dream.Persistence.Instances |
| B21 | Compat namespace | `main` stays byte-identical (A116) | Dream.Persistence.Instances |
| B22 | Storage-root override, probe, `owner.json`, `projects.json`, moved-project import, cross-root orphans | Applies at the next start; `-DreamStorageRoot` is strict (A117 to A119) | t1 `test_storage.cpp`; Dream.Persistence.Instances |
| B23 | [Open storage settings] in the not-executed footer | Opens the settings viewer | none |
| B24 | G6 driver B prototype | 5 kills, one during a run; the 10,000-kill run is moved (A121) | `run_g6.ps1`; Dream.Persistence.G6Driver (driver-only) |
| B30 | Audit (`FDreamAudit`) | Own writer thread; lines wait for the role (A77 to A79) | Dream.Audit.Files, .Runs, .Unwritable, .LostRun |
| B31 | Crash breadcrumbs (`FDreamCrashContext`) | Game thread only (A80) | Dream.Diagnostics.CrashContext |
| B32 | "Starting… waiting for the editor to become idle" | Only while waiting for an idle end of frame (A82) | Dream.Run.StartingText |
| B33 | `STREAM_AGENT` only for agent or bridge runs | | t2 `test_cancel_deferral`; Dream.Run.NativeSurface |
| B34 | check_cancel's deferral row | `_dream.check_cancel(deliver)` (A83) | t2 `test_cancel_deferral` |
| B35 | Selection aliases `sel`, `sel_assets`, `world`, `subsys` | Setting `bSelectionAliases` (A84) | t2 `test_aliases`; Dream.Run.Aliases |
| B36 | Map change releases the aliases and the last-run namespace | | Dream.Run.Aliases |
| B37 | API snapshot T-API-006 | CHANGELOG `API:` rule (A85) | t2 `test_api_snapshot` |
| B38 | [Run in transaction] | (A86) | Dream.Transactions.RunInTransaction |
| B39 | Insights trace channels | Named `DreamPump` and `DreamRun` (A81) | Dream.Diagnostics.Trace |
| B40 | Per-route seeding of "One undo step per run" | Mixed toolbar state when the settings differ (A87) | Dream.Transactions.ToggleSeed |
| B50 | dream-check | SUM101..SUM301 and SUM900..SUM902, SUM102 quick fix, `# noqa`, 300 ms debounce | t2 `test_check_rules`, `test_check_server` |
| B51 | Shared docstring parser | Reads UE 5.8's two spaces before `[Read-Write]` | t2 `test_docstrings` |
| B52 | Stage 1 snapshot | Idle slices, gated by runs, modals, the pump and keystrokes (A96) | t2 `test_stubs`; Dream.Language.Snapshot, .SnapshotGates |
| B53 | `dream::JsonRpc`, `dream::LspClient` | | t1 `test_jsonrpc.cpp` |
| B54 | LSP transport | Job object; the escape check needs the creation time (A91) | Dream.Language.Transport, .EscapeCheck |
| B55 | Language services | Off headless and in safe mode (A93) | Dream.Language.Services, .Headless, .InstanceLogs |
| B56 | Problems: lint, parse and runtime items, F8 / Shift+F8, quick fix | (A94, A95) | Dream.Language.Model, .Problems |
| B60 | Live resolver | D6 safety policy; editor-property and enum completers | t2 `test_resolver`; Dream.Completion |
| B61 | `dream::CompletionMerge` | Word-start weight 40 (A147) | t1 `test_completion.cpp` |
| B62 | Completion popup in the code view and the REPL prompt | (A149 to A151) | Dream.Completion |
| B63 | Console executor live completions | Ends A35 (A152) | Dream.Completion, Dream.Output.ConsoleExecutor |
| B70 | Undo on `dream::UndoTree`; grouped-edit API | The stock undo never runs (A123 to A126) | Dream.CodeView.Undo, .WholeText |
| — | Sticky column for code-placed carets (restored caret, `OpenLocation` at a column) | Refreshed before the next vertical-move key (A174) | Dream.CodeView.StickyColumn |
| B71 | Smart Enter, Backspace, auto-close, Ctrl+D | Type-over kept although EDT-05 lists it for M2 (A127) | Dream.CodeView.SmartEdit |
| B72 | Find and replace bar | Regex search on a cancellable worker (A130, A131) | Dream.CodeView.Find, .FindWorker |
| B73 | Go to line | (A132) | Dream.CodeView.GoToLine |
| B74 | Zoom and live Font size | (A133) | Dream.CodeView.Zoom |
| B75 | Ctrl+M: Tab moves focus | Not saved (A133) | Dream.CodeView.TabFocus |
| — | Read-only buffers during runs | Read live (A128) | Dream.CodeView.ReadOnlyKeys |
| B80 | External changes | (A135 to A137) | Dream.Buffers.ExternalChange, .DeletedFile |
| B81 | Read-only target bar | (A138) | Dream.Buffers.SaveFailure |
| B82 | "Save as UTF-8?" | (A139) | Dream.Buffers.SaveFailure |
| B83 | Recent Files | (A140) | Dream.Buffers.Tabs |
| B84 | Tab badges: language, dirty, lock, recovered | The slice-A status called the language badge M2; SPEC BUF-02 says M1 (A141); PO-04 no longer shows the language badge (A182) | Dream.Buffers.Tabs |
| B85 | RUN-26 hint chip | (A142) | Dream.Buffers.RunLineHint |
| B86 | Reopen tab from the output | (A143) | Dream.Buffers.Tabs |
| B90 | Thread table keyed by (native ID, `PyThreadState*`) | (A155) | Dream.Output.ThreadTable |
| B91 | Output Insights counters | `retention.run_logs_days` stays M4 (A156, A158) | Dream.Output.ThreadTable |
| B92 | Output Find and soft wrap | Built as M1 on the `SListView` view (A159, A160) | Dream.Output.Find |
| B93 | Output specs | T-OUT-020, the T-OUT-021 echo guard, T-OUT-023 run-log creation, P3-out-03, T-UEI-002 | Dream.Output.EchoCap, .EchoGuard, .RunLogCreation, .DownKey, .StatusItem |
| — | REPL keys in the same frame as a submit | (A161) | Dream.Repl.SameFrameKeys |
| B100 | §14.2 fixtures | The Paused-window driver is M3 | the specs below |
| B101 | G2 suite in the Run window | 200 stops per shape by default (A166, A170) | Dream.G2.Cancel, Dream.G2.Supervisor |
| B102 | G5 suite, consume-list leak checks, T-REPL-005, T-REPL-017 | Dangerous chords checked at the router only (A169) | Dream.Keys.G5 |
| B103 | G7 suite | | Dream.Keys.G7 |
| B104 | T-SUP-029 and the Esc cases (LRS-11) | | Dream.Supervisor.Bot |
| B105 | §2.5 static checks, T-GAP-001, T-GAP-008 | 13 scans | `run_tier1.bat --scans-only` |
| B106 | T-GAP-019: layout across restarts, the 40 % split | Steps 3 to 5 pass since the restyle (5/5 in its integration and final verification) | Dream.Restart.Layout through `run_restart.ps1` |
| B107 | RUN-10 in its own session | | Dream.Lifecycle.Dynamic |
| B108 | Slice-A open test items | P2-shl-06, A2-01 and P3-per-02 done; P2-shl-01 moved to the G6 driver | Dream.Keys.Focus, Dream.Repl.History, Dream.Persistence.Journal |
| — | Fast-chain re-delivery (T-CAN-003) | Ruling A76 | Dream.Supervisor.FastChain |
| — | A buffer run leaves the map clean | | Dream.Transactions.NoChange |
| — | Tier-4 hygiene ledger | Ruling A168 | Dream.Hygiene.Ledger |

## What the restyle contains

Every PO-04 point is built (owner ruling, ARCHITECTURE A178): 1 Maya-minimal chrome, 2 Maya-style history, 3 themes.
"Tested by" names the tier-4 specs and the tier-1 suite; A-numbers point at ARCHITECTURE.md section 7.

| PO-04 point | Item | Notes | Tested by |
|---|---|---|---|
| 1 | Menu bar: File, Edit, History, Command, View, Help | Every former toolbar action keeps its command and chord; File gains Review Recovered Buffers and Leave Safe Mode (A180) | Dream.Shell.Chrome |
| 1 | Icon row | Open, Save, Run (with a drop-down), Run line, Stop, Clear history, Undo last run, Echo and Line numbers toggles; tooltips, no labels, never focusable (A197) | Dream.Shell.Chrome, Dream.Visual.Capture |
| 1 | Plain input tabs, `line:col`, no breadcrumb row | No language badge; lock and recovered glyphs only while they apply; "+" (A181, A182) | Dream.Shell.Chrome, Dream.Buffers.Tabs, Dream.Visual.Capture |
| 1 | Hidden by default: the REPL tab, the Problems tab, the REPL prompt strip, run headers and footers, the status line | One View toggle each, saved per user; line numbers stay on; Focus REPL shows the prompt while it is used (A183, A199 to A201) | Dream.Shell.Chrome |
| 1, PO-04-R1 | Status messages and the health slot in the input tab row | Journal, Python and lint warnings in the warning colour while the status line is hidden (A181, A186) | Dream.Shell.Chrome (forced journal outage) |
| 1 | Slim banners and file bars with × | × hides only what was showing, recovery lines by id; View ▸ Show Hidden Notices (A198, A219) | Dream.Shell.Chrome, Dream.Persistence.Recovery |
| 1 | Run window restyled flat | Behaviour unchanged; teardown hands activation back only when the Run window is the foreground window (A210, A211) | Dream.Supervisor.RunWindowFocus, Dream.Theme.LiveApply |
| 1 | Centre splitter | A 2 px line with a 6 px hit area and invisible grips beside it (A220) | Dream.Shell.Chrome, Dream.Restart.Layout |
| — (ruling after integration) | Stop acts on the press | Mouse-down on Stop stops the run; keys unchanged; a Stop the first pump after a native call reads is recorded as Stop during it (A216, A222) | Dream.Supervisor.StopPress, Dream.G2.Supervisor (T-CAN-020), Dream.Supervisor.Bot (T-SUP-029) |
| 2 | Maya-style history | Muted echo without gutter or `›`; `# Result: … #`, `# Warning: … #`, `# Error: …` with `# ` traceback lines; no icons; links unchanged (A204 to A207) | Dream.Output.MayaStyle |
| 2 | Run headers and footers behind the toggle; the outcome line | One muted line each; with headers off a run that ends other than OK shows `# Error: Run #N <outcome> #` unless a row of its own Python exception block is shown (A208, A221) | Dream.Output.MayaStyle, Dream.Output.StatusItem |
| 2 | History context menu | Re-run, Run in transaction, Undo this run, Run without snapshot, Open storage settings, Copy (A209) | Dream.Output.MayaStyle |
| 3 | Ten built-in themes, Darcula by default, applied live | Darcula, Dracula, Maya, Monokai, One Dark, Nord, Gruvbox Dark, Solarized Light, GitHub Light, Unreal; the theme colours the whole tab and the Run window (A179, A184, A194, A195) | Dream.Theme.BuiltIns, .Default, .Unreal, .LiveApply |
| 3 | Theme pickers | View ▸ Theme and the ThemeId dropdown in Editor Preferences ▸ Plugins ▸ Dream (A193) | Dream.Theme.Dropdown, Dream.Shell.Chrome |
| 3 | Theme JSON | Each built-in exports to JSON; user themes from `%LOCALAPPDATA%\DreamPythonEditor\Themes`, read lazily and noticed when added; a lossless round trip (A189 to A191, A217) | t1 `test_theme`; Dream.Theme.Json |
| — | Test hygiene | The Dream window activated at each test start; a guard on the user settings per automation run; ASCII test names (A212 to A214, A224, A225) | Dream.Hygiene.SettingsGuard; `check_automation_report.py`; the `testnames` scan |
| — | Settings writes | `SaveKey` writes one key and checks the write; a refused write is retried with a backoff, a read-only ini gets one warning (A203, A218) | Dream.Shell.Chrome |

## What PO-05 contains

Every PO-05 point is built (owner ruling, ARCHITECTURE A227), along with the amendment PO-05-A1. "Tested by" names the
tier-4 specs and the tier-1 suite.

| PO-05 point | Item | Notes | Tested by |
|---|---|---|---|
| Side bar | Left, full height, shown by default | Project on top, Structure below; width and split saved per project; the centre column always keeps 240 px; View ▸ Project and View ▸ Structure; Focus Project and Focus Structure; Ctrl+Z and Ctrl+D in the side bar never edit the script (A228) | Dream.Shell.SideBar, Dream.Visual.Capture (14, 15) |
| Project tree | Roots and sections | The project's `Content/Python` first whenever it exists, then your roots, then Project Plugins, then Engine (collapsed), then Scratches; "No project roots. Add one…" when there is nothing to show (A230) | Dream.Shell.ProjectTree |
| Project tree | Roots in settings | Editor Preferences ▸ Plugins ▸ Dream (this project) ▸ Project Roots, per project, like PyCharm's content roots; also Add Root… and Remove Root in the tree (A229) | Dream.Shell.ProjectTree |
| Project tree, A1 | Plugin Python folders | `<content root>/Python` of every mounted engine and project plugin, as Unreal's Python plugin puts on `sys.path`; engine entries read-only in the tree; turned on or off live from the preferences page, View ▸ Side Bar ▸ Plugin Python Folders or the tree's menu (A233) | Dream.Shell.ProjectTree |
| Project tree | Open, New, Rename, Delete, Reveal, Copy Path, Refresh | Double-click opens a file (or focuses its tab); Rename keeps open tabs on the file; Delete goes to the Recycle Bin and is never permanent; roots, Scratches and engine entries are protected (A232) | Dream.Shell.ProjectTree |
| Project tree | Responsive with slow folders | Listing and file operations off the game thread; at most 4 local and 2 network listings at once; network roots refresh on expand and Refresh, not by watching (A231) | Dream.Shell.ProjectTree |
| Structure | The active tab's outline | Classes, functions, methods, async defs, `# %%` cells; a click, double-click or Enter jumps; the entry under the caret highlighted; typing filters; "No structure" for an empty or non-Python tab (A234, A235) | Dream.Shell.Structure; t1 `Outline_*` |
| Scratch files | Every unnamed tab is `scratch_N.py` | In the scratch folder (a setting, guarded against folders like a drive root or the project); a new tab reuses an empty file only if this editor made and closed it unused; Save As keeps the scratch file; old `Untitled-N` tabs become scratch files on restore (A236, A237, A239, A240) | Dream.Buffers.ScratchFiles |
| Scratch files | Auto-save | 1 s after typing stops, before runs, on focus loss, on close and at exit; never asks on close; `*` only while auto-save is blocked; closed text that can't be saved goes to local history (A238) | Dream.Buffers.ScratchFiles, Dream.Restart.Layout |
| Samples | Sample scripts in Scratches/Samples | demo, output styles, a long run to try Stop, Undo this run, cells, a Structure demo; seeded once; Help ▸ Restore Sample Scripts (A241) | Dream.Buffers.Samples |
| Tabs on start | Reopen tabs (PER-02) | Tabs, order, active tab, caret and scroll come back; two scroll-restore bugs fixed (A242) | Dream.Restart.Layout, `run_restart.ps1` |
| — | Test isolation | Tests use their own scratch folder, roots and recycle seam, and never touch your real scratch folder, roots or Recycle Bin (A244) | every PO-05 spec; the settings guard |

## What the rename contains (PO-06 phase 3)

Every name moved from Sumi to Dream by one scheme (ARCHITECTURE A246): the plugin `Plugins/DreamPythonEditor`
(`DreamPythonEditor.uplugin`, "Dream Python Editor"), the modules DreamCore, DreamEditor and DreamEditorTests and their
API macros, the `FDream`/`SDream`/`UDream`/`EDream`/`IDream`/`TDream` prefixes, `dream::` and `Public/dream/`, the
files, `LogDream`, the `Dream.*` console commands and automation tests, the tab ID `DreamEditor.Main`, the `-Dream*`
switches, the Python package `dream_editor`, the native module `_dream`, `dream_tools` and `dream-check`, the ini
sections `[/Script/DreamEditor.*]`, the style keys, the scans and the host project `DreamHost`. Visible text says
"Dream Python Editor"; the status bar, window titles, transactions, the console executor and the toolbar button say
"Dream".

| Item | What it does | Tested by |
|---|---|---|
| Compatibility package | `import sumi_editor` warns once (DeprecationWarning) and returns the `dream_editor` module itself, so old scripts and the old samples keep working | t2 `test_compat_shim` |
| CoreRedirects | `Config/DefaultDreamPythonEditor.ini` maps the old settings classes, `SumiTabState`, the three UENUMs and the test fixtures to the new names | Dream.Migration |
| Saved layouts | The old tab ID `SumiEditor.Main` is a legacy tab type of `DreamEditor.Main`, so a layout saved by a Sumi build reopens the tab where it was; the owner's `EditorLayout.json` is not edited | Dream.Migration (the mapping) |
| Settings | At the first start the sections `[/Script/SumiEditor.SumiUserSettings]` (EditorSettings.ini), `SumiPerProjectUserSettings` and `SumiProjectTreeSettings` (the project's EditorPerProjectUserSettings.ini) are copied key by key into the Dream sections when those are absent; saved scratch paths follow the scratch folder | Dream.Migration |
| Storage | `%LOCALAPPDATA%\Sumi\<EngineVer>\<Project>-<hex6>` (journal, history, REPL history, run logs, audit, breakpoints, state) is copied into `%LOCALAPPDATA%\DreamPythonEditor\...` before the journal opens; DreamHost takes SumiHost's old folder, other projects keep their hash; run logs follow in the background | Dream.Migration |
| Scratch files and themes | `%LOCALAPPDATA%\Sumi\Scratches` (Samples included) when the scratch folder setting is empty or the old default, and `%LOCALAPPDATA%\Sumi\Themes`, copied once | Dream.Migration |
| Old formats | Journals and history keep their `SUMIJRNL` / `SUMIHIDX` / `SUMIBLOB` magic; a theme file with `"format": "sumi-theme"` and a REPL history with the `sumi.repl-history` header still read | t1 `Theme_LegacyFormatNameReads`; Dream.Migration |

The migration only copies: the old `%LOCALAPPDATA%\Sumi` folder and the old ini sections stay as they were, as a
backup. It runs once (a second start copies nothing), logs one line when it copied something, and does no file I/O in
StartupModule. A storage-root override keeps its folder and is not copied.

**For the owner:**
- Open `DreamHost/DreamHost.uproject` from now on (the old `SumiHost` folder is gone; its Saved data moved with it).
- The tab comes back where the layout had it, through the legacy tab ID. If it does not show, open it once from
  **Tools ▸ Dream Python Editor** (or `Dream.Open`); the layout saves the new ID from then on.
- Your first start already happened in this group's first test pass: your settings, the DreamHost journal and history
  (from `SumiHost-9eabca`), REPL history, run logs and scratch files are in the new places. Delete `%LOCALAPPDATA%\Sumi`
  only when you no longer want the backup.
- Old scripts that `import sumi_editor` keep working and show a deprecation warning; change them to `dream_editor`.

### Rename results (2026-11-04)

- **Build:** the first build after the rename was a full one (fresh `Intermediate`, 220 actions): succeeded, 0 warnings;
  every incremental build after it, 0 warnings.
- **Tier 1:** 218/218 (the new `Theme_LegacyFormatNameReads` included). **Tier 2:** 343/343 (`test_compat_shim` included;
  the API snapshot was regenerated, digest `6fb9579626a0`, with its CHANGELOG `API:` line). **Scans:** 14/14.
- **Tier 4 headless, all Dream:** 103/103 (the 95 before plus the 8 `Dream.Migration` cases).
- **Not run (owner instruction during the rename: the tests took over the machine):** the attended all-Dream pass,
  `run_restart.ps1`, `run_smoke.ps1` and every SendInput or focus spec. An attended pass started before the instruction
  was stopped after about 80 specs; its results don't count (the desktop was in use, so the G2 cases failed on the
  foreground check, and `Dream.Buffers.Samples` failed on a header check that was fixed afterwards). The legacy tab ID
  is checked as registered; a live session that restores a layout naming `SumiEditor.Main` was not run either.
  The architect asks the owner before any attended pass.
- **The owner's first start:** the first headless pass did the real migration: 2 settings sections, the storage folder
  `SumiHost-9eabca` into `DreamHost-d5521b` (1,938 files, then 4,030 run-log files on the worker), 8 scratch files.
  The old `%LOCALAPPDATA%\Sumi` tree was checked file by file (SHA-256) before and after every pass: unchanged.

## Build and test

All commands are run from the repository root (`<repo>`). They are kept gentle on a
shared machine (owner instruction, PO-06 phase 3): `run_build.bat` passes `-MaxParallelActions` with half the logical
cores (`DREAM_BUILD_JOBS=<n>` overrides), test editors run at BelowNormal priority (`DREAM_EDITOR_PRIORITY=normal`
overrides), and `run_tier4.bat --attended` takes over the desktop, so it is opt-in only and the owner is asked first. The toolchain is MSVC
14.44.35229 with Windows SDK 10.0.22621; 14.38 fails on UE 5.8 headers and 14.43 is banned (ARCHITECTURE.md section 8).

```bat
:: UE build of the host project and the three plugin modules (no editor running)
Plugins\DreamPythonEditor\Tests\run_build.bat
:: Tier 1: DreamCore with cl /W4 /WX, standalone (--quick skips the golden data: T-CORE-039, T-CORE-017, T-LEX-001)
Plugins\DreamPythonEditor\Tests\DreamCore\build.bat
:: Tier 2: the dream_editor package under the engine's python.exe -I -B with a null _dream
Plugins\DreamPythonEditor\Tests\run_tier2.bat
:: Static scans (13): content, Python.h confinement, SPDX headers, DreamCore engine-free, section 2.5 rules 5 and 7-12,
:: T-GAP-001 (LOCTEXT), T-GAP-008 (traceability)
Plugins\DreamPythonEditor\Tests\run_tier1.bat --scans-only
:: Tier 4 headless (-unattended -nullrhi): the specs that need neither the tab nor a run
DreamHost\Scripts\run_tier4.bat
:: Tier 4 attended: every Dream spec; opens an editor window on the desktop
DreamHost\Scripts\run_tier4.bat --attended
:: One spec or several (+-joined prefixes)
DreamHost\Scripts\run_tier4.bat --attended --filter Dream.Keys+Dream.Output.Follow
:: Normal-exit smoke: Dream.RunFile of a script that calls quit_editor, then the full LogExit sequence
powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_smoke.ps1
```

Slice B adds:

```bat
:: Tier 1 in a private output folder (needed whenever two tier-1 builds can run at once)
set DREAM_TIER1_OUT=C:\Users\<you>\AppData\Local\Temp\dream-t1-<name>
Plugins\DreamPythonEditor\Tests\DreamCore\build.bat --quick
:: Tier 1 without the timing gates, or with the full-scale random runs (T-CORE-001 1M, T-LEX-007 100k, T-CORE-010 1M)
set DREAM_SKIP_TIMING=1
set DREAM_FULL_SCALE=1
:: Tier 4 with extra editor arguments for one pass, for example fewer G2 stops than the default 200
DreamHost\Scripts\run_tier4.bat --attended --filter Dream.G2 --args "-DreamG2Stops=20"
:: RUN-10 (Dream.Lifecycle.Dynamic) in its own session, with the dynamic-lifecycle CVar on the command line
DreamHost\Scripts\run_tier4.bat --attended --filter Dream.Lifecycle.Dynamic --args "-ini:Engine:[ConsoleVariables]:Engine.Python.IsPythonLifeCycleDynamic=1 -DreamLifeCycles=100"
:: Restart driver for T-GAP-019 (Dream.Restart.Layout, five editor sessions of about a minute)
powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_restart.ps1
:: G6 driver B prototype: kill -9 while typing and once during a run, on a private storage root
powershell -ExecutionPolicy Bypass -File DreamHost\Scripts\run_g6.ps1 -Kills 5 -RunKill 3 -Seed 7
```

The restyle adds:

```bat
:: Headless: Dream.Theme.BuiltIns, .Default, .Json, .Unreal and Dream.Hygiene.SettingsGuard
DreamHost\Scripts\run_tier4.bat --filter Dream.Theme+Dream.Hygiene
:: Attended: also Dream.Theme.LiveApply and .Dropdown, Dream.Shell.Chrome, Dream.Output.MayaStyle, Dream.Visual.Capture
DreamHost\Scripts\run_tier4.bat --attended --filter Dream.Theme+Dream.Shell+Dream.Output.MayaStyle+Dream.Visual.Capture
:: Run window focus and Stop on the press: need the editor in front and nobody using the desktop
DreamHost\Scripts\run_tier4.bat --attended --filter Dream.Supervisor.RunWindowFocus+Dream.Supervisor.StopPress
:: Static scans: now 14, with testnames (every automation test name is ASCII)
Plugins\DreamPythonEditor\Tests\run_tier1.bat --scans-only
```

In the editor console, `Dream.Theme.Export [folder]` writes the built-in themes as JSON (default
`%LOCALAPPDATA%\DreamPythonEditor\Themes\Built-in`) and `Dream.Theme.Reload` reads the user themes again; both are refused during
runs. The tier-1 build runs `test_theme` with the rest. A tier-4 pass now fails when a test leaves a user setting
changed: the settings guard logs `user setting <Key> changed by <test>` at Error and restores the key, and
`Tests/ci/check_automation_report.py` fails the pass on that line. A spec that must leave a setting changed for a later
step calls `DreamTest::AllowSettingChange` (ARCHITECTURE A224). The guard keeps its baseline in
`DreamHost/Saved/DreamTests/settings-snapshot.json` during a run; a file left there by a killed run is resolved at the
next run's first Dream test.

Tier 4 notes: the attended runner turns off the editor's background CPU throttle for that session only. Automation
`Quit` force-exits, so `ShutdownModule` never runs inside a tier-4 pass; `run_smoke.ps1` is the separate session that
checks a normal exit (exit code 0, the LogExit sequence, no Ensure, Fatal, LogDream Error or LogPython Error line).
Runs that are meant to fail declare their LogPython error blocks (`DreamTest::ExpectPythonErrorBlock`).

Slice-B tier-4 notes:
- The SendInput specs (`Dream.G2.*`, `Dream.Supervisor.Bot`, `Dream.Keys.G5` T-KEY-020, the restart driver's splitter
  drags) send real mouse and key input. They need an attended desktop that nobody uses during the pass, with nothing
  over the editor: no system prompt, crash reporter or other application window. The 200-stop `Dream.G2.Cancel` pass
  takes about 40 minutes.
- The driver-only specs (`Dream.Restart.Layout`, `Dream.Lifecycle.Dynamic`, `Dream.Persistence.G6Driver`) log a "NOT RUN"
  warning in a shared pass; run them through their driver or their own session.
- Tier 4 uses DreamHost's real per-user storage root. Every Dream test closes the tabs it opened, and the next pass
  discards only the pending recoveries of buffers that Dream tests opened, listed in
  `DreamHost/Saved/Automation/dream-tier4-buffers.txt`; a developer's own pending recoveries are left alone.
- `run_restart.ps1` and `run_g6.ps1` back up only what they change (two layout keys; the
  `[/Script/DreamEditor.DreamPerProjectUserSettings]` section of DreamHost's per-project ini), stop their own editor
  before they put it back, and restore a leftover backup at their next start. Interrupting them with Ctrl+C can skip
  the restore until that next run (open items). `run_g6.ps1` journals to `%TEMP%\DreamG6-<stamp>` with
  `-DreamStorageRoot`, which never falls back to the user's own root.
- When several people or agents share the machine, take the shared UE lock around a full build, a tier-4 pass, the
  smoke and the drivers (ARCHITECTURE.md section 1, rule 11).

PO-05 tier-4 notes:
- **Test scratch folder:** `run_tier4.bat` and the restart, smoke and G6 drivers pass
  `-DreamScratchFolder=<DreamHost>/Saved/DreamTests/Scratches` and empty that folder first. Runs from the editor's own
  automation window use the same folder through the hygiene hooks.
- **Recycle Bin:** the real Recycle Bin case runs only with `--args -DreamRealRecycleTest` and leaves one
  `real_recycle_<suffix>.py` in the Recycle Bin. Otherwise it logs a NOT RUN warning, which doesn't fail the pass.
- **Short Structure section:** `--args -DreamStructureSplit=0.9` runs `Dream.Shell.Structure` with a short section, in
  memory only.

## Latest results

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

### Slice A (history)

After review round 5, on a clean rebuild:

| Tier | Result |
|---|---|
| UE build (UBT -Clean, then build) | Succeeded, 0 errors, 0 warnings |
| Tier 1 (SumiCore, full, with golden data) | 81 / 81 cases, 216,107 checks, 0 failed |
| Tier 2 (Python) | 160 / 160 |
| Static scans | 4 / 4 |
| Tier 4 headless | 20 / 20 |
| Tier 4 attended (all Sumi specs) | 63 / 63 |
| Normal-exit smoke | EXIT 0, full LogExit sequence |

The attended, headless and smoke logs have no Ensure, Fatal, LogSumi Error or LogPython Error lines; the LogSumi
warnings are the expected ones from the history and read-only-save cases. Tier-4 cases per spec: Repl.History 14,
Persistence.Journal 6, Transactions.UndoLastRun 6, Output.Follow 5, Run.RunFile 4, Shell.TabLayout 4,
Supervisor.SourceText 4, Run.Session 3, Supervisor.Input 3, Lifecycle 2, Output.RunLog 2, Run.SyntaxError 2,
Supervisor.Stop 2, and one each for Keys.Focus, Output.ConsoleExecutor, Repl.Prompt, Run.LineAdvance, Run.SelectionEcho
and Visual.Capture.

Screenshots of the top-output layout, the Alt+Enter walk, an error run, the REPL and the Run window at top and bottom
are regenerated by `Sumi.Visual.Capture` into `SumiHost/Saved/Sumi/Screens/` (`01-tab-top-idle.png` to
`10-main-frame.png`).

### Slice B

The final integration run:

| Check | Result |
|---|---|
| `Tests/run_build.bat` | clean rebuild of the plugin, 149 compile actions, 0 warnings, 0 errors |
| `Tests/SumiCore/build.bat` (full, with goldens) | 193/193 cases, 662,630 checks, 0 failed; T-LEX-001 2,527 files / 968,186 lines, T-CORE-017 74,638 rows, T-CORE-039 2,470 files, all 0 mismatches |
| `Tests/run_tier2.bat` | 292 run, 0 failures |
| `Tests/run_tier1.bat --scans-only` | all scans pass (trace: 842 TDD test rows) |
| `run_tier4.bat` (headless) | 64/64 |
| `run_tier4.bat --attended` (all `Sumi.*`, `-SumiG2Stops=3`) | 220/237 |
| `run_smoke.ps1` | passed, editor exit 0 |
| `Sumi.Lifecycle.Dynamic` (own session) | 100/100 |
| `run_g6.ps1` | passed at 5 kills (max 127 ms lost) and at 3/2 kills after later fixes |

The 17 attended failures are all SendInput click cases, refused because a Windows Defender Firewall prompt
(PickerHost, open since 02:47) covers the desktop and every click lands on it: 13 in `Sumi.G2.Cancel`, 3 in
`Sumi.G2.Supervisor` (T-CAN-020, the T-SUP-002 subset, the T-SUP-003 subset) and `Sumi.Supervisor.Bot` T-SUP-029. Every
non-click case passes, all of `Sumi.Keys.G5` and `Sumi.Keys.G7` included. The click cases passed in the tests group's
02:00 pass, before the prompt appeared. They still need a run on a clean desktop: the third attended run of the click
specs, the 200-stop `Sumi.G2.Cancel` pass and restart-driver steps 3 to 5 (owner decisions). The restyle's integration
and final verification ran all of them (Restyle, below).

### Restyle

The groups' passes, from their reports (every full build at 0 warnings, the scans clean in every cycle):

| Group | Restyle passes before review | After the r6 fixes |
|---|---|---|
| theme | tier 1 `test_theme` 6/6 (3,150 checks); headless `Sumi.Theme` 13/13, attended 14/14; after the stock-style follow-up (19:41) attended `Sumi.Theme+Sumi.CodeView` 30/30, headless 13/13 | tier 1 `test_theme` 6/6; attended `Sumi.Theme+Sumi.CodeView` 34/34 with no warnings; headless `Sumi.Theme` 14/14 |
| chrome | `Sumi.Shell`, `Visual.Capture`, `Buffers.Tabs`, `Keys.Focus`, `Keys.G5`, `Transactions` 35/35; `Shell`, `Repl.SameFrameKeys`, `Completion.Repl` 15/15; a wider regression 47/52 (five focus cases that failed only after `Sumi.CodeView.ReadOnlyKeys`, later fixed by the activation at test start); after the settings-hygiene follow-up `Sumi.Shell+Sumi.Visual.Capture` 11/11 with the ini's Sumi section byte-identical | `Shell+Visual.Capture+Buffers+Persistence.Recovery+Keys.Focus` 40/41 (Edit ▸ Complete's probe text, fixed), then `Sumi.Shell` 15/15 with no warnings; the ini's Sumi section byte-identical; `Sumi.Keys.G5` not run (desktop in use, held for integration) |
| output | (build 19:21) attended `Sumi.Output+Repl+Run+Supervisor` 78/78; headless all `Sumi.*` 77/77; `Visual.Capture` 1/1 | (build 21:05) attended `Output+Repl+Run+Supervisor+Visual.Capture` 83/86: `Supervisor.Bot` T-SUP-027 with the Alt+Tab switcher in front and `RunWindowFocus` twice with Message Log active, the desktop in use; headless 79/79 |
| run | attended `Sumi.Supervisor+Repl+Completion+CodeView` 61/62 (only `Completion.Repl`: the Message Log activation, fixed by tests) | `RunWindowFocus` 2/2 on a free desktop (21:02); attended `Sumi.Supervisor+Sumi.Run` 31/36 (21:05): both `RunWindowFocus` cases and three `Supervisor.Bot` SendInput cases with RuneLite or the Alt+Tab switcher in front |
| tests | attended `Supervisor+Repl+Completion+CodeView+Shell` 72/72, twice | cycle 1 (21:15): headless 79/79, attended `Supervisor+Repl+Completion+CodeView+Shell+Hygiene` 75/80 (the Alt+Tab switcher in front: `Bot` three times and `RunWindowFocus` case 2; the `Repl.History` A2-01 gate at 2.13 ms against 2 ms under load); cycle 2 (21:24): headless 79/79, attended 80/80 |
| codeview (spec only) | attended `Sumi.CodeView` 16/16 on a clear desktop; the pump case reached the view with 38/38 chords | — |

The full attended pass (`-SumiG2Stops=3`), the SendInput click cases and `Sumi.Keys.G5` were held for the integration
on a free desktop. Screenshots of the Maya-minimal layout are written by `Sumi.Visual.Capture` to
`SumiHost/Saved/Sumi/Screens/`: 01 to 9 in Darcula, 11 to 13 in GitHub Light, and `10-main-frame.png` only while the
tab floats.

The integration, run by the tests group under the lock "integration" from 00:42 to 01:45 on the r9 tree (no rebuild
after step 1):

| # | Step | Result |
|---|---|---|
| 1 | Clean rebuild | 195 actions (182 compiles, 8 links), 0 warnings, 0 errors, 227 s |
| 2 | Tier 1, full | 199/199 cases, 665,789 checks; goldens 0 mismatches (extents 2,470 files, lex 2,527 files, search 74,638 rows) |
| 3 | Tier 2 | 292/292 |
| 4 | Static scans | 13/13 |
| 5 | Tier 4 headless | 81/81 |
| 6 | Tier 4 attended, all `Sumi.*`, `-SumiG2Stops=3` | 280/282 over two passes: the first stopped at test 68 when T-SUP-002 hung and the controller's 300 s timeout ended the pass, and the other 214 tests ran 214/214. `Supervisor.Bot`, `Keys.G5` T-KEY-020, `Visual.Capture` and `G2.Cancel` (13/13) passed; `G2.Supervisor` 8/10 |
| 7 | 200-stop `Sumi.G2.Cancel` | 13/13, p99 80–122 ms (limits 250/1250) |
| 8 | Drivers | `run_smoke.ps1` passed, `run_restart.ps1` 5/5, `run_g6.ps1` passed (5 kills plus one during a run) |
| 9 | `check_automation_report.py` | no settings-guard line anywhere |

The two `G2.Supervisor` failures reproduced alone with the Run window in front, so neither was interference. Both were
latent from slice B, whose final passes never ran these click cases green (the firewall prompt), and both are fixed:
- **T-CAN-020** (Stop during `SleepNative(5)`): the run was cancelled after 5.81 s, but its record had no native-call
  Stop data ("during native 0, 0.00 s, line 0"): the click reached Slate a pump late, after the supervisor had cleared
  its flag. Fixed in the supervisor (ARCHITECTURE A222), now under the one-pump rule of A216.
- **T-SUP-002** (`while True: tick_core_ticker(10)` with a bot Stop): the run never ended. The bot put the cursor back
  about 1 ms after its click, about 65 ms before the pump read it, and `SButton` clicks only while hovered (2 hangs in
  7 runs). Fixed in the bot scripts, which now wait for the Stop request; a miss fails after 15 s and the API Stop ends
  the run (A223).

Also in the integration: the ini's Sumi section stayed identical and nothing crashed. Its warnings were two
`Visual.Capture` "Could not find Glyph Index" warnings (non-ASCII test names, renamed afterwards, A225), the engine's
G5 SetUserFocus warning and a `Supervisor.Bot` "pump overload" warning; no ini-write warning.

The final verification, run by the tests group under the lock "integration-final" from 02:58 to 03:11 on the r10 tree:

| Step | Result |
|---|---|
| Full build (incremental) | Up to date, 0 warnings |
| Tier 4 headless | 82/82 |
| Tier 4 attended, all `Sumi.*`, `-SumiG2Stops=3` | **286/286**, with every click case (`G2.Cancel`, `G2.Supervisor` with T-CAN-020 and T-SUP-002, `Supervisor.Bot` with T-SUP-029, `Supervisor.StopPress`, `Keys.G5` T-KEY-020) and `Visual.Capture` |
| `run_restart.ps1` / `run_smoke.ps1` | 5/5 / passed |
| Static scans / tier 1 `--quick` | 14/14 / 199/199 (665,780 checks) |
| `check_automation_report.py` | Passed; no guard lines |

It was the first full attended pass with every click case green. No other window came to the front during a test, the
ini's Sumi section stayed identical and nothing crashed. The warnings no test asked for: `CodeView.ReadOnlyKeys`
"OpenClipboard failed" and the G5 SetUserFocus warning (both from the engine), the `Supervisor.Bot` "pump overload"
warning and T-SUP-004's flood `UserWarning` lines. Reports: `SumiHost/Saved/Automation/IntegPO04/` and `FinalPO04/`.

After the final verification the r10 minors were checked by targeted passes: chrome `Sumi.Shell+Sumi.Visual.Capture`
21/21 with no warnings; tests headless 82/82 and `Sumi.Hygiene+Sumi.Supervisor.Bot` 12/12; run
`Sumi.G2.Supervisor+Sumi.Supervisor` 32/32 (T-CAN-020 recorded at 4.98 s, `StopPress` at 2.97 s); scans 14/14 and
tier 1 199/199. Round r11 reviews that patch.

<!-- PO05-RESULTS -->

## Review rounds

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

### Slice A

Slice A was reviewed against the TDD and then re-checked patch by patch: each round after the first checks only the
previous round's fix patch, from an exact before/after diff, with verifiers, refuters that try to disprove each
finding, fixers and a full re-test. The loop stops when a round finds no major.

| Round | Checked | Findings after refutation | Fixed |
|---|---|---|---|
| 1 | All of slice A against TDD v1.2 | 80 (1 critical, 21 major, 58 minor) | 78; RUN-10 and SUP-04 open (below) |
| 2 | The round-1 patch | 31 (5 major, 26 minor) | 31, three of them partly (below) |
| 3 | The round-2 patch | 22 (2 major, 20 minor) | 17 entries covering 20; the rest doc or test-only (below) |
| 4 | The round-3 patch | 5 (all minor) | 4, and the doc item through a follow-up |
| 5 | The round-4 patch | 3 (all minor) | 3: the reset rule after an undo without redo (P5-txo-01), two comments, two history cases strengthened |

The slice-A choices that came out of the review are ARCHITECTURE.md A56 to A64.

### Slice B

The same loop on exact diffs: read-only reviewers per area, each candidate major checked by a refute step before it is
reported, fixes by the owning group with a test that fails without the fix where one could be written, and each round
after the first reviewing only the previous round's fix patch. It stops when a round finds no major.

| Round | Checked | Majors after refutation | Fixed |
|---|---|---|---|
| 1 | The whole slice-B diff against the slice-A tree, in eight areas: core-text, core-formats, persist, codeview, run, lang, shell, tests | 17 reports, 16 distinct defects: core-text 2, core-formats 2 (one the same as persist-M4), persist 4, codeview 1, run 1, lang 2, shell 2, tests 3; up to five minor lines per area | All majors and minors |
| 2 | The round-1 fix patch, in four areas: core, codeview + run + lang, persist + shell, tests | 5 new majors: codeview 1, persist + shell 2, tests 2 (core none); three round-1 majors were only partly fixed (persist-M1, persist-M2, tests-M1), and the new majors cover what was missing | All majors and minors |
| 3 | The round-2 fix patch, in three areas: core + lang + codeview, persist + shell, tests | 1 major: codeview-r3-M1; core, lang, persist, shell and tests none; every round-2 major confirmed fixed | The major and the minors, except two kept as known risks (persist minor 4, tests minor 3) |
| 4 | The round-3 fix patch, plus the whole change to `SumiHost/Scripts` against slice A | None: the loop's stop condition is met | 5 minors, fixed after the loop (round 5 found three of them only partly fixed) |
| 5 | The patch of those post-loop fixes | None | The remaining 5 minors are recorded as open items (below); no further fix round |

Round 3: **codeview-r3-M1**: the round-2 change that took the layout out of `Construct` let a caret placed before the
view's first paint (a restored caret, an `OpenLocation` at a column, a traceback link) lose its column on the first Up
or Down, because the sticky column was computed with no line views. Fixed: the sticky column is refreshed before the
next vertical-move key (ARCHITECTURE A174), with the new spec `Sumi.CodeView.StickyColumn`. The round-3 minors were
fixed, among them the stuck history pause on a newer format, the pause notice that depended on queued work (A172), the
reader's renames of generations it could not read (A173), the uncounted `EmitsNothing` walk (A171) and the
safe-mode restore that the hygiene counted as test-opened (A175).

Round 4 minors, fixed after the loop: G5 still sent Ctrl+C/X with a non-text clipboard; `run_g6.ps1`'s `finally`
restored the ini while its editor still ran, and restored the whole ini from a stale backup; `run_restart.ps1`'s
timeout kill stopped every SumiHost editor; `RefreshStickyX` ran on any key and scrolled a wheel-scrolled view back to
the caret (A174, A176, A177).

Round 5 found no major; its five remaining minors are open items below.

Integration findings (the final runs, outside the review rounds):
- **StickyColumn order dependence**: `Sumi.CodeView.StickyColumn`'s restored-caret case failed in the full attended
  pass, because `SSumiTab::HandleBufferOpened` builds a new buffer's view at once when no tab is active; the case now
  opens an active holder tab first.
- **Snapshot misses**: two of 100 `Sumi.Lifecycle.Dynamic` cycles ended "not executed" when the journal did not confirm
  the pre-run snapshot within 2 s. They were environmental (local compiles starved the below-normal writer thread);
  the writer now runs at `TPri_Normal`, and a history slice that waits out a held file serves commits (A101, A105).
- **T-PER-017 merge race**: a run's history entry could be indexed after a later entry of the same buffer, so runs were
  not merged. Fixed: merge by time (A99).
- **Read-only attribute hole**: G5 T-KEY-012 showed chords editing a read-only file buffer inside a pumping run: UE
  5.8's text widget caches its read-only attribute, a hole slice A's A27 already had. Fixed: read live (A128).

Review coverage: the review snapshots of rounds 1 to 3 missed `SumiHost/Scripts`; the reviewers read those scripts
live, and round 4 reviewed their whole change against slice A.

Writes outside the repository: the constraint was to write nothing under `C:/Program Files`. Early in the session one
codeview test run with a relative `--report` path wrote three report files under
`C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/SumiHost`; they were moved out at once and the folder is gone.
The engine itself writes `Engine/Intermediate/EditorRuns/<pid>` markers under its install folder at every editor
launch; those are the engine's, not Sumi's.

Round 1 majors:
- **core-text-M1**: Enter with the caret in an indented line's leading blanks stripped the line's indentation (a
  module-level `return`, or a statement silently moved into the outer block). Fixed (A71).
- **core-text-M2**: the PyLexer gap buffer self-moved records on an empty gap; harmless on MSVC, but libc++ and
  libstdc++ lose the tokens. Fixed (A72).
- **core-formats-M1**: a regex backreference's compares were not counted as steps (`(a+)\1b` over 100k units took
  7.3 s on the game thread). Fixed (A70).
- **core-formats-M2 = persist-M4**: a rotation whose rename succeeded but whose open failed kept committing to g,
  which the next start deletes. Fixed: the rename is the switch (A103).
- **persist-M1**: orphan finalize deleted a namespace whose staged history or REPL entries were never kept. Fixed
  (A113).
- **persist-M2**: an unreadable generation was treated as absent and later deleted; history retention swept its blobs.
  Fixed (A102).
- **persist-M3**: the G6 driver could type into and kill the user's real SumiHost profile when the probe fell back.
  Fixed: `-SumiStorageRoot` is strict (A119).
- **codeview-M1**: a regex find ran unbounded on the game thread (about 22 s for one ordinary pattern over 1,000
  lines). Fixed: regex search and Replace All on a worker (A130).
- **run-M1**: the fast chain re-delivered Stop into a generator's cleanup while the first delivery unwound. Fixed
  (A76).
- **lang-M1**: the LSP escape check could kill an unrelated process after a PID was reused. Fixed (A91).
- **lang-M2**: the T-STUB-028 gate could not fail. Fixed (A96).
- **shell-M1**: a self-write stamp stayed valid 60 s after its save landed, so an external write of the earlier bytes
  was ignored. Fixed (A135).
- **shell-M2**: undoing Save as UTF-8's coding-line edit left the encoding at UTF-8, so the next save wrote UTF-8 under
  a Latin-1 cookie. Fixed (A139).
- **tests-M1**: the pass-start cleanup discarded every pending recovery of the real SumiHost root, from any automation
  run. Fixed with the ledger (A168).
- **tests-M2**: G7 forced `bRunPump` on and `Sumi.Visual.Capture` saved it to the user-global EditorSettings.ini.
  Fixed.
- **tests-M3**: the modal fixture had no timeout, so a missed Esc hung an attended pass that held the UE lock. Fixed
  (A165).

Round 2 majors:
- **codeview-r2-M1**: after a regex Replace (one) the selection landed on stale offsets and the next match was skipped.
  Fixed (A130).
- **persist-r2-M1**: an orphan whose newest generation could not be read was merged from the older generation and then
  deleted. Fixed (A115).
- **persist-r2-M2**: one transient unreadable index made local history read-only for the whole session, so closed
  scratch tabs lost their only copy. Fixed: history pauses (A104).
- **tests-r2-M1**: the ledger recorded the buffers the session restore opened during the first test, so the
  developer's pending recoveries were discarded one pass later. Fixed (`Sumi.Hygiene.Ledger`; A168, A175).
- **tests-r2-M2**: `Sumi.CodeView.ReadOnlyKeys` could write default run settings over the user's on failure paths.
  Fixed.

Other real defects found by testing rather than review (besides the integration findings above):
- **T-CAN-003 fast chain**: `for i in itertools.count(): try: sum(range(1000)) except BaseException: pass` was not
  stopped within 1.25 s; the run ended 140 s after a Shift+F5 Stop and 538 s after an API Stop. Ruling and fix: A76.
- **Same-frame Slate crash** (crash report UECC-Windows-D711643D): a whole-text replace followed by a caret key or a
  click in the same frame hit an engine ensure and read `LineViews[-1]`, in the REPL prompt (Submit's clear, then the
  type-ahead replay or a second key of a slow frame) and in the code view. User-reachable; fixed in
  `SSumiPlainText::SetWholeText` and the REPL strip (A125, A161).
- **Dynamic lifecycle**: with `Engine.Python.IsPythonLifeCycleDynamic` set, Sumi sat in WaitingForInit and never
  offered Enable Python. Fixed (A89).
- **Docstring parser**: UE 5.8 writes two spaces before `[Read-Write]`, so no editor properties reached the snapshot
  (0 of 11,114 types) and SUM102 could never fire; the resolver's regex had the same fault. Fixed.
- **UndoTree trim**: a trim at the memory cap walked the undo path once per removed group; the 1M-edit round trip went
  from over 25 minutes to 31 s.
- **PyLexer stale tokens**: a budgeted relex that stopped early left later lines marked clean (found by the T-LEX-008
  trace). Fixed (A72).
- **Code view**: `SyncWidget` reset the auto-close record, so typing a closer inserted a second one; UE 5.8's
  `DeleteSelectedText` drops one line break too many for a range ending at a line end (A126).
- **Shutdown crashes**: a tab held past Slate shutdown crashed in `RebuildBanners` at exit (A122); the restart spec kept
  the tab alive the same way (test code).
- **G6 first attempt**: a storage root inside the project folder, which §10.1 refuses, made the driver's sessions
  journal into SumiHost's real profile; cleaned up by hand, and `-SumiStorageRoot` became strict (A119).
- **Test-fixture defects** that broke real runs: probe rects in window space instead of desktop space (every Run-window
  click missed), T-REPL-005 pressing Up before layout, G5 executing Undo Last Run and Save during a run (a native
  dialog hung a pass for 20 minutes), G7 reading the Enter classification too early, and `--args` passed with quotes,
  which UE ignored.

### Restyle

The same loop on exact diffs: read-only reviewers per area, a refute step before a major is reported, fixes by the
owning group, and each round after the first reviewing only the previous round's fix patch. It stops when a round finds
no major.

| Round | Checked | Majors after refutation | Fixed |
|---|---|---|---|
| r6 | The PO-04 patch against final slice B, in four areas: theme, chrome-run, output, tests; plus the architect's pixel check of the screenshots | 6: theme 1 (theme-r6-M1), chrome-run 2 (chrome-run-r6-M1, M2), output 2 (output-r6-M1, M2), tests 1 (tests-r6-M1, in chrome's spec); minors: theme 5 and nits, chrome-run 9, output 8 and nits, tests 12, architect 2 (arch-r6-m1, arch-r6-m2) | All majors and minors, with ruling PO-04-R1 for chrome-run-r6-M2, m8 and output-r6-M1; arch-r6-m2 (the Run window's engine border) accepted as an open item |
| r7 | The r6 fix patch, by two reviewers: product (theme, chrome-run, output) and tests (with `check_automation_report.py`) | 2, both in the settings guard: tests-r7-M1, tests-r7-M2; product none, and every r6 product finding fixed. Minors: theme-r7-m1, chrome-run-r7-m1, m2, output-r7-m1, tests-r7-m1 to m8 and nits | All; a ruling refined output R12 (output-r7-m1), and the health slot's journal actions were accepted as built |
| r8 | The r7 fix patch, in all four areas, with checks of `SaveKey`'s write, the guard's settle loop, the splitter grips, the recovery line ids, the deferred theme reload and the refined R12 | None: the loop's stop condition is met (r6 6 majors, r7 2, r8 0). Every r7 finding fixed, tests-r7-m3 partly by design. Minors: chrome-run-r8-m1, a grip nit, tests-r8-m1 to m3 and two nits | All, in a post-loop patch |
| r9 | The post-loop patch of the r8 minors (like slice B's round 5) | 2, both made by that patch: chrome-run-r9-M1, tests-r9-M1. Minors: chrome-run-r9-m1, m2, tests-r9-m1 and two nits | All, under the round-r9 rulings (A218, A224); r10 confirmed each fixed |
| r10 | The patch after the integration: the r9 fixes, the two integration failures, Stop on the press (A216) and the `testnames` scan | None. All seven r9 findings fixed, and so are both integration failures, the glyph warnings and the stale capture. Minors: chrome-run-r10-m1, tests-r10-m1, m2 and five nits | All, in a minors patch; chrome-run-r10-m1 by the one-pump rule (A216, A222) |
| r11 | That minors patch | Pending (below) | — |

Round r7 majors (both fixed):
- **tests-r7-M1**: the settings guard broke `Sumi.Restart.Layout` and `run_restart.ps1`, whose step 1 writes
  `OutputPosition=Bottom` for step 2 to check: the guard logged an Error and removed the key. Fixed:
  `SumiTest::AllowSettingChange(Test, Key)` (A224).
- **tests-r7-M2**: the guard's snapshot was taken once per editor process, so a second automation run in the same
  editor blamed the user's own changes on a test and wrote the old values back to the user-global ini. Fixed: the
  snapshot is per run (A224).

Round r7 minors worth naming: theme-r7-m1 (the Editor Preferences combo listed the themes folder every frame and could
reload inside Slate's layout pass; A217), chrome-run-r7-m1 (× hid recovery lines by their exact text, so a count update
brought the banner back; A219) and output-r7-m1 (any error line of a run, `unreal.log_error` included, suppressed its
outcome line; A221).

Round r8 found no major. Its checks: `SaveKey` reaches disk synchronously, but under automation the ini is written in
place and a held file fails silently, and `SaveKey` dropped the result (chrome-run-r8-m1); the guard's settle loop
writes nothing beyond this editor's own next flush; the grips, the recovery line ids, the deferred theme reload and the
refined R12 are correct.

Round r9 majors (both fixed and confirmed in r10):
- **chrome-run-r9-M1**: the post-loop `SaveKey` retry had no cap: in a user editor with EditorSettings.ini read-only
  or held, each 0.5 s retry stalled about 0.5 s and was due again at once, so the editor ran at about 2 FPS with log
  spam. Fixed: the next attempt is timed from the end of the last one with a backoff to 30 s, and a read-only ini gets
  one warning and no retry (A218).
- **tests-r9-M1**: a leftover baseline could not restore a setting a killed test had changed, because the stamp was
  written only at test ends, so a leak written by `SaveKey` made the stamp stale and the guard only warned. Fixed: the
  guard re-persists its stamp on `USumiUserSettings::OnFileWritten()` while a Sumi test runs (A224).

Round r10 found no major. Its minors: chrome-run-r10-m1 (the two-pump rule attributed a Stop pressed during pump 1 to
the earlier native call, and T-CAN-020 passed only through that tolerance; fixed by the one-pump rule and T-CAN-020's
StopPress shape, A216, A222, A223), tests-r10-m1 (the leak case restamped through a test hook, so the product's restamp
was untested) and tests-r10-m2 (`Visual.Capture` flagged legitimate fullwidth characters; A225).

Round r11 reviewed only the r10 minors patch and found no major; every r10 minor and nit is fixed. Its one new minor
and its nits were fixed in a last patch that changed only tests:
- tests-r11-m1: the Hygiene leak case assumed `bAlwaysWriteRunLogs` was already in the user's ini, so it would fail
  falsely on a fresh profile. It now records the key's presence and text before the leak, and checks the restore
  against them.
- Stale comments about the bot's window-title read.
- A self-check for the `Visual.Capture` mis-encoding detector.
- A scanner fallback in the shutdown-flush case.
- The shared `FSumiInputBotContext::FindUncoveredPointRaising`: it raises the Run window without activating it before
  T-CAN-020 and StopPress look for an uncovered point.

Last runs after that patch:
- chrome: full build 0 warnings; attended `Sumi.Shell+Sumi.Visual.Capture` 22/22.
- tests: headless 82/82; attended `Sumi.Hygiene+Sumi.G2.Supervisor+Sumi.Supervisor.StopPress` 16/16; scans 14/14.
- The ini's Sumi section was unchanged.

Round r6 majors:
- **theme-r6-M1**: the Editor Preferences dropdown spec compared `GetThemeIdOptions()` with `List()`, a tautology, so a
  broken dropdown passed. Fixed: one source of entries and the attended `Sumi.Theme.Dropdown` (an off-screen
  `IDetailsView` showing the combo with a user theme), which found a product bug: the ThemeId row held its property
  handle weakly, showed "Multiple values" and ignored a choice (A193).
- **chrome-run-r6-M1** (PER-06): after × on the recovery line, a later recovery notice (a Discard whose history entry
  was not made durable) stayed hidden, so the Discard silently did nothing. Fixed: × hides only the lines that were
  showing (A198).
- **chrome-run-r6-M2**: the journal's Degraded warning (work not crash-safe) had no UI while the status line was
  hidden; chrome-run-r6-m8 the same for "Language features paused". Fixed by ruling PO-04-R1, the health slot (A186).
- **output-r6-M1**: with run headers off, a run that wrote no line (a failed or timed-out snapshot, a failed save,
  stopped before user code, Python not available) vanished with its outcome and its actions: Run without snapshot,
  Open storage settings, `ScrollToRun`, REPL links. Fixed: the outcome line (A208).
- **output-r6-M2**: the REPL history popup was unreadable in light themes (about 1.2:1, theme text over the stock
  `Menu.Background`). Fixed (A195).
- **tests-r6-M1**: the PO-04 defaults case in `Sumi.Shell.Chrome` could not fail once any spec had stored the keys.
  Fixed: defaults are checked on a fresh object, `OutputPosition` in `Sumi.Shell.TabLayout` too (A214).

Round r6 minors worth naming: arch-r6-m1 (2-px #242424 rules around the prompt strip, near black in GitHub Light; now
1-px ChromeBorder with a pixel check), chrome-run-r6-m3 (the Run window teardown pushed an unparented window a run had
made active behind the main frame; A210), tests-r6-m2 (no pass-level settings guard although an OutputPosition leak had
been seen; A213), theme-r6-m1 (a View toggle could move the theme; A192), theme-r6-m3 (Reload and Export not refused
during runs; A191), theme-r6-m5 (the Unreal theme did not follow Editor Preferences ▸ Appearance; A184) and the theme
nits that amended R4 (A190).

Findings of the restyle passes, outside the review:
- **Near-black history**: Starship's table-row style painted `FStyleColors::Recessed` on every row, and the stock list
  background kept the history dark in GitHub Light. Fixed: transparent rows over OutputBackground and
  `ListViewStyle` (A195, A207).
- **No echo in the captures**: the profile had `bEchoInput=False`, most likely left by `Sumi.Shell.Chrome`, which
  clicked the Echo toggle twice without restoring it. It was reset to True with no editor running; the spec now
  restores it, and `Sumi.Visual.Capture` shows echo for its captures without saving it (A214).
- **`ReadOnlyKeys` pump case**: on a clear desktop the editor holds OS focus, so the Run window takes keyboard focus
  (RT-05) and the chords never reached the code view. Fixed in the spec (A215).
- **Message Log after every test**: the engine's automation controller activates a floating Message Log window after
  every test, so `Completion.Repl` and `Repl.SameFrameKeys` failed after other specs. Fixed in the tests' hygiene
  (A212); the run group's teardown activation (A210) came out of the same search.
- **User-setting leaks**: besides Echo, `ThemeId=SumiT4 Mine` after one intermediate headless theme pass (reset to
  Darcula; cause not pinned down) and `OutputPosition=Bottom`, written by `Sumi.Shell.TabLayout` when its restore
  reached the config cache but not the file. The settings guard now fails such a pass with the test's name (A213).
- **The ini trailing the cache** (tests, after r7): after a burst of `SaveKey` writes, EditorSettings.ini on disk
  trailed the config cache by several writes, with no log line, until the next flush, and the guard's disk read had
  adopted a test's temporary value. The guard now settles the disk before it compares (A224), and `SaveKey` checks its
  write (A218).
- **The double write** (chrome, after r8): `SaveConfig(Context)` flushed, then `SaveKey`'s checked flush wrote the file
  again and its move over the ini was refused; `SaveKey` became one checked write (A218). Afterwards a refused move,
  probably by a scanner, still happened for writes 30 to 91 ms apart and always recovered within 0.5 to 1.3 s.
- **Restart step 3**: the bot's splitter drag left the output area at 326.5 px instead of 120 px, because SSplitter
  starts a resize only after a mouse move over the handle and the spec pressed right after moving; the drag now waits
  two Slate ticks, and `run_restart.ps1` passes 5/5 (A220).
- **Glyph warnings**: twenty non-ASCII test names were garbled through the ANSI code page when the Message Log drew
  them; renamed, and the `testnames` scan keeps them ASCII (A225).

Process slips: two full builds ran outside the UE lock at 18:10 to 18:12 when a script dropped `-SingleFile` (the lock
was free and no editor ran), and one at about 20:49 after a lock request timed out (a no-op; no editor was started).
Spec edits broke the full build at 20:20 to 20:21 (output), 20:33 to 20:34 and 21:57 (chrome) until they were fixed.
Chrome's lock stayed held with no work from 22:49 until the architect released it at 23:44; the tests group ran two
copies of one cycle back to back (each in its own lock hold); and at about 00:34 a chrome build ran outside the lock
after a failed acquire (it relinked only for `SumiUserSettings.cpp`).

### PO-05

The same loop on exact diffs, with the three areas sidebar, structure and scratch:
- p1 reviewed the PO-05 patch against the last reviewed restyle tree, and each later round reviewed the previous round's
  fix patch;
- the p1 reviewers re-checked their own areas in p2 and p3, and confirmed each earlier finding fixed;
- PO-05-A1 came in with the p2 fixes and was reviewed as new code in p3.

| Round | Checked | Majors after refutation | Fixed |
|---|---|---|---|
| p1 | The PO-05 patch, in three areas; plus the architect's look at the screenshots (A1 the white Structure highlight, A2 the `*` flicker, A3 the test folder growing, A4 a file left in the Recycle Bin) | 4: sidebar-p1-M1 (a top-level item once found missing was never listed again, so new scratch files never showed), sidebar-p1-M2 (Enter on a bad name closed the inline editor), scratch-p1-M1 (a scratch tab closed while its save could not finish lost its text at exit), scratch-p1-M2 (Save As overwrote a pending outside version). Minors: sidebar 10, structure 3, scratch 9 | All, under the rulings R-p1-1 to R-p1-12 (Owner decisions) |
| p2 | The p1 fix patch | 1: scratch-p2-M1 (a restored tab's "changed on disk" hold was cleared by the file watch, so auto-save could still overwrite the outside version). Minors: sidebar 3, structure 1, scratch 3 | All; PO-05-A1 and its live toggle were built in the same patch |
| p3 | The p2 fix patch and PO-05-A1 | None. Minors: sidebar 3 (the listing cap let dead shares block local folders, a collapsed Engine group still listed, a guard key), scratch 2 (reuse across editors, a game-thread watch on network scratch folders); the architect reviewed structure (one nit) | All |
| p4 | The p3 fix patch, checked by the architect | None | — |

## Known risks

From slice A, updated:
- The editor writes bytecode into `Content/Python/dream_editor/**/__pycache__` when it imports the package. It is
  harmless (CPython checks the source time) but must be left out of a packaged plugin; tier 2 runs with `-B` and writes
  none.
- Restore after a real kill is checked only by the G6 driver prototype (`run_g6.ps1`, 5 kills), outside the regular
  tier-4 pass; the 10,000-kill run is moved.
- Shutdown order is checked only by the normal-exit smoke (`run_smoke.ps1`), because automation `Quit` skips
  `ShutdownModule`, and that smoke has no run in flight at exit; its script quits before dream-check spawns.
- Tier 4 has no test profile: it runs on DreamHost's real per-user storage root and ini. The ledger hygiene protects the
  developer's pending recoveries, and the user-global `[/Script/DreamEditor.DreamUserSettings]` section is checked to
  still hold its defaults after every pass.

PO-05:
- **Network folders:**
  - An offline share holds only its own folder's rows until Windows times out the listing.
  - Network roots refresh on expand, on Refresh and on the retry, not by watching.
  - A scratch folder on a share gets no live change notices; an outside edit there shows when the editor regains focus
    or a run ends.
- **New plugin folders:** a plugin `Python` folder created while the editor runs shows after Refresh or a remount.
- **Empty scratch files** from earlier sessions or other editors are never reused, so they can pile up until you delete
  them in the tree.
- **Rename race:** a save that lands after a rename can recreate the old name next to the renamed file. Nothing is lost.
- **Failed close save:** a closed scratch tab whose save failed keeps its text only in local history, which the journal
  writer writes near exit.
- **Specs:**
  - the watcher, delete and missing-item cases wait up to 10 s;
  - the inline-editor close and the scratch timing cases need the editor app active;
  - the plugin-folders case depends on the engine's layout: `PythonScriptPlugin/Content/Python/remote_execution.py`
    exists, and `Engine/Content/Python` doesn't;
  - the real Recycle Bin path runs only in the opt-in case;
  - the side bar's rule pixels are checked only at 100% display scale.
- **Test host state:** DreamHost's per-project ini collects the test sessions' state (Recent Files and expanded folders
  under `DreamHost/Saved/DreamTests`). It affects only the test host project, never your own projects or the user-global
  Dream settings, which the guard checks after every pass.
- **Timing gate:** the `Dream.Repl.History` A2-01 gate (2 ms, from slice A) went over under load three times during
  PO-05 (2.016, 2.13 and 2.155 ms). It passes on a rerun (open items).

core:
- T-LEX-008's relex stage sits at about 0.995 ms against its 1.00 ms limit by design; a heavily loaded machine can push
  p95 over even with three attempts.
- `SearchEngine::FindNext` and `UpdateAfterEdit` copy a snapshot that spans pieces (at most 1 MB in plain mode, tens
  of µs per call).
- `DiffSingleRange` on two snapshots copies both texts once a buffer is edited; it runs on the journal writer at 4 Hz.
- T-CORE-003 runs without ASan, and no tier-1 test runs under a thread sanitizer.

run:
- The journal (SESSION) restore path still infers "set" for a toggle that differs from its buffer-run seed, which
  misreads a tab after the user changed the buffer-run setting (the ini path reads the set flag).
- A session that never opens the tab, or that crashes, loses the audit lines still waiting for a role.
- check_cancel's import detection relies on 3.11's frozen importlib file names.
- The alias reads run on the game thread at prepare; their cost with a very large selection is not measured.

lang:
- `StopServers` closes the child job, which kills any child still in it, including a transport a spec started itself.
- The Problems model merges and sorts again on every change; fine at M1 sizes.
- Attended specs spawn dream-check and, on a new snapshot key, walk the snapshot once (slices of at most 4 ms), which a
  latency-sensitive spec can see.

persist:
- Each session rewrites the whole history index in one background unit; a very large index could exceed one 50 ms
  slice. `LoadText` reads a whole blob in one slice.
- The Review panel recomputes every row's line diff on each refresh, on the game thread (buffers under 1 MB, 50 ms cap
  per diff).
- If a session never imports Python, the restore guard stays at 1 until clean shutdown.
- The Recovery specs work on the live attended tab and add banner lines and history entries to the DreamHost profile.
- A held file costs about 0.9 s per open attempt on the writer thread, and the exit drain can overrun its 2 s by about
  as much; rotation step 5 and `SetAsideUnusedNewer` still retry on the writer thread.
- While history is paused, the whole history lane waits, queries and Discard included.
- The history pause covers only the index read: a failed generation rename at `StartGeneration`, a failed blob write,
  `CommitIndex` after a failed `OpenAppend`, and a full disk still finish an entry false (review round 3, persist
  minor 4).

instances:
- A failed G6 run keeps `%TEMP%\DreamG6-<stamp>`; the driver sessions' cross-root scan reads DreamHost's real journal,
  read-only.
- `RecordWalFiles` reads whole wal files to measure them, on the probe thread.
- In the same root, `ns.lock` is removed just after its handle is released, which leaves a tiny window.
- Probe completions capture a raw `this`; this is safe only because `FDreamInstances::Tick` alone drains them and the
  module destroys the instances service before history and the journal.
- If the journal closes mid-finalize, the namespace is kept and listed again at the next start.

codeview:
- The range sync depends on the stock text widget's internals (`SelectText`, `InsertTextAtCursor`, the
  `DeleteSelectedText` compensation); a mismatch falls back to a whole-text set with a LogDream warning.
- The selection before an edit relies on OnTextChanged firing before OnCursorMoved (UE 5.8).
- A forward Delete that does not move the caret leaves the next navigation unable to close the typing group.
- Cut and paste over a selection have no tier-4 test; the clipboard case runs only when the clipboard holds plain text.
- IME composition is untested (G1).
- The REPL prompt can keep text from earlier specs across specs.

buffers:
- A directory watch arms at DirectoryWatcher's next tick; a change in the same frame as the open is caught only by a
  rescan.
- The hint chip's position mixes desktop and text-local rects and assumes no render transform between them.
- A "Reopen tab" link's enabled state is computed when its row is built.
- The 2 s latency checks assume an attended editor that is not CPU-throttled.
- `Dream.Run.RunFile`'s read-only case logs two expected "Not saved" warnings it does not declare.
- After Save as UTF-8 the journal has no record of the format change, so a crash before the save restores the old
  encoding.
- `ReplaceAtomic` has no sharing-violation retries.

completion:
- A click on a popup row is recognised by `IsHovered()` or by focus moving into the menu's window.
- Very large types (1,000+ members) may never list fully within 5 ms on a busy machine; each keystroke then gets
  partial results.
- The timing gates (T-RES-007, T-RES-020, the console's 1 ms budget) flake under heavy load.
- The deferred typing decision needs the view to be ticking.
- The resolver finds the engine's descriptor types by name; tier 2 uses fakes, so only tier 4 exercises it, indirectly.
- The breaker still counts a mid-call GIL hand-off to another Python thread.

output:
- EchoCap's per-window check assumes every one-second window of the run holds more than 5,000 lines.
- The output view compiles against codeview's `SDreamFindBar` API.
- StatusItem's tooltip check relies on a text tooltip's widget being an `SToolTip`.
- The tests group's shared `AddRun` helper allows 10 s for the output; runs of about 100,000 lines exceed it on a cold
  editor.

tests:
- Every SendInput spec needs a clear desktop; a covering window makes the click cases fail.
- The 200-stop G2 pass holds the UE lock for about 40 minutes.
- G5 sends chords as Slate events, so a change of the code view's key handling can move what is consumed.
- A buffer a test opens without a test scratch name while the restore is not yet settled is neither closed nor listed
  in the ledger; this fails safe (review round 3, tests minor 3).

restyle, theme:
- `Dream.Theme.LiveApply` checks painted pixels, so it needs the editor window shown; its skips are warnings.
- `SetCurrent` is not deferred during runs; only menus and Editor Preferences reach it, and both are blocked during runs
  (`Dream.Theme.Reload` and `Dream.Theme.Export` are refused during runs).
- The Unreal theme built at startup may predate the editor's own theme load; choosing it again rebuilds it.
- A theme file added or changed while the editor runs is noticed by the next `List()` or `Find()` after at most a second
  and applied on a deferred tick (A217); `Dream.Theme.Json`'s case for it waits up to 5 s.
- One intermediate headless pass left `ThemeId=DreamT4 Mine` in the user's EditorSettings.ini; it was reset to Darcula
  and three later passes left it unchanged. The cause was not pinned down; the settings guard should catch a repeat.

restyle, chrome:
- Focus-dependent specs vary with OS focus and stray top-level windows.
- The Focus REPL reveal's same-call layout pass depends on Slate internals.
- A test's first synthetic click can be lost; `Dream.Shell.Chrome` makes a harmless click first.
- `Dream.Keys.G5` T-KEY-012 logs one "Ignoring SetUserFocus … SAssetSearchBox" warning.
- Test names must stay ASCII: × or ▸ in a spec name turns into LogSlate glyph warnings in attended passes. The
  `testnames` scan checks literal names only; names held in variables (10 sites, all ASCII today) are not followed.
- Settings writes are checked (A218): a refused write is retried 0.5 s to 30 s after the end of the last attempt, with
  a Warning after 5 s pending, and the module's shutdown flushes a pending write once. A read-only EditorSettings.ini
  gets one Warning ("EditorSettings.ini is read-only; Dream settings are not saved") and no retry, so changes made
  meanwhile live in memory until a later `SaveKey` succeeds; a crash while a write is pending loses it. Under
  automation the engine writes the ini in place, and a scanner briefly refused moves over the ini in chrome's passes
  (each write recovered within 0.5 to 1.3 s).
- The splitter grips depend on SSplitter's hit range and hover rules (A220).

restyle, output:
- The exception line is found by reading the traceback text: an ExceptionGroup falls back to its first line, and a
  log-origin traceback without an exception line absorbs later `unreal.log_error` lines (open items).
- Echo and stderr differ from stdout by colour alone (PO-04 over A11Y-05, ARCHITECTURE A185).
- `Dream.Repl.SameFrameKeys` turns the REPL prompt on and brings the Dream window to the front itself; it failed after
  other specs until the activation at test start.

restyle, run, tests and codeview:
- The Run window takes keyboard focus when it opens while the editor holds OS focus (RT-05);
  `Dream.CodeView.ReadOnlyKeys`'s pump case depends on `OpenModal` reading `FApp::HasFocus()` when it opens the window.
- `Dream.Supervisor.RunWindowFocus`, `Dream.Supervisor.StopPress` and the SendInput specs need the editor in front and a
  free desktop; another application in front (RuneLite, the Alt+Tab switcher) failed them in the r6 passes. On a free
  desktop the final verification ran every click case green.
- Stop acts on the press (A216): a stray press on Stop ends the run, with no drag-off cancel. An input Stop counts as
  Stop during a native call only when the first pump after the call processes it (the one-pump rule, A222), so a bot
  check that asks an editor window something (a WM_GETTEXT) must run before the call, never during it; the bot reads
  window titles with `InternalGetWindowText` (A223).
- The engine's automation controller activates a floating Message Log window after every test (the engine cvar
  `MessageLog.DisablePopup` is not used); the activation at test start counters it, and the specs' own bring-to-front
  workarounds stay.
- The settings guard restores a changed key and fails the pass, so a spec that writes a `UDreamUserSettings` key must
  write it back before it ends, or call `DreamTest::AllowSettingChange` when a later step needs the value (A224). An
  outside write or an Editor Preferences save leaves the guard's stamp stale, so a leftover snapshot is then only
  warned about, never restored. The guard's disk-only warning has no test: the file is rewritten before the
  comparison.
- The `Repl.History` A2-01 gate (2 ms) is flaky under load: 2.13 ms in a tests cycle and 2.016 ms in an output cycle,
  while it passed 15/15 alone and in the integration and final passes (open items).
- Warnings no test asks for remain in a full attended pass: `CodeView.ReadOnlyKeys` "OpenClipboard failed" and the G5
  SetUserFocus warning (both from the engine), the `Supervisor.Bot` "pump overload" warning and T-SUP-004's flood
  `UserWarning` lines.

## Deferred by the build brief

The slice-A list, with what slice B did:
- The custom `SCodeView` (§4.8) and the G1 harness: Deferred (G1); M1 keeps plain mode (A1).
- IME and accessibility beyond the stock `SMultiLineEditableText`: Deferred (G1).
- Runtime completion: built (B60 to B63). Hover and signature help: Deferred (M2).
- `dream-check` and the Problems lint items: built (B50, B56).
- The Stage 1 stub snapshot: built (B52).
- Local history (§10.4): the store is built (B14); the timeline and diff UI are M2.
- The audit writer (§11.4): built (B30).
- Crash breadcrumbs (§11.8.1): built (B31).
- Multi-caret and folding: Deferred (M2).
- The G2, G5, G6 and G7 gate suites: G2, G5 and G7 built (B101 to B103); a G6 driver prototype (B24); the 10,000-kill
  G6 run is outside slice B.
- Ran-line marks (EDT-18): Deferred (M2).
- Nothing from M2 to M4.

## core (SumiCore, tier 1)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- TextBuffer: the §4.1.3 piece tree, `sumi::AnchorSet` and `ChangesBetween`: built (B02).
- Diff: `Diff::Lines` and `ToMinimalEdits`: built (B03), with the unified formatter.
- PyLexer: the gap-buffer line array: built (A72). The 128-bit XXH3 line hash: Deferred (G1).
- Keymap: two-stroke sequences and Shift Shift: Deferred (M2/M4) (A11).
- TextCodec: progressive decoding above 10 MB and the mapped source above 50 MB: Deferred (G1).
- OutputRing: `ParseSgr` and stored style spans (OUT-03): Deferred (M2).
- JournalFormat: sliced checkpoints with the catch-up step: built (B10); the older-major migration: built (B11).
- `sumi::UndoTree` (B01), `SearchEngine` (B04), `CompletionMerge` (B61) and `JsonRpc` (B53): built. `CaretSet`:
  Deferred (G1). `FoldModel`: Deferred (M2). `SnippetEngine`: Deferred (M4).
- Tier-1 gates: T-LEX-001, the T-LEX-008, T-CORE-004 and T-CORE-009 timing gates and the full-scale random runs: built
  (B06). T-OUT-005 under TSan: not possible with MSVC. T-PER-005 as a libFuzzer target: not built until the x64 ASan
  component is installed (owner decision); deterministic mutation fuzzes stand in.

## python (sumi_editor, tier 2)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- The selection aliases and Reset Session clearing their ownership table (§5.3.8, T-RUN-017): built (B35).
- `PostMortem.retain(run_id, exc)` (§8.7): Deferred (M2); `sys.last_*` are never set.
- `@sumi_editor.task`, `Task` handles, the `task_step`, `reload` and `entry_point` run kinds, `revert_module_state`:
  Deferred (M3). `__all__` is the M1 subset.
- The check_cancel deferral row of §5.10.2: built (B34).
- The map-change release of the last-run namespace (§5.3.3, D3): built (B36).
- Debug runs (F5) in statement mode (§8.5): Deferred (M3).
- The API snapshot test T-API-006: built (B37).

## bridge (Python lifecycle, `_sumi`, run service, transactions)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- Starting step 1, the trust check (`FSumiTrust::Evaluate`, U39): Deferred (M4).
- `FSumiAudit::RunStarted` / `RunEnded` and `run.snapshot_skipped`: built (B30).
- `FSumiCrashContext` run breadcrumbs: built (B31).
- HandleShutdown step 3: the snapshot stop and the resolver's cleanup are built (B52, B60); closing the debugpy server:
  Deferred (M2).
- `FSumiLanguageServices::OnDeferredStart` after a successful boot: built (B55).
- Supervised evaluates (`eval_begin` / `eval_end`): Deferred (M3).
- Task steps and the walk-back (`BeginTaskStep`, `UndoTask`, `TaskRunning`): Deferred (M3).
- `STREAM_AGENT` only for agent or bridge runs: built (B33).
- The bridge FIFO of 8 waiting requests and the MCP refusal: Deferred (1.1).
- `FSumiPython::GetSessionEpoch`: built (B35). The generic `CallHelper` stays replaced by typed wrappers (A15).
- The async source-control checkout during the Run File save step: Deferred (M4).
- The "Starting… waiting for the editor to become idle" text: built (B32).

## supervisor (RunSupervisor, Run window, protections, pre-processor)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- §5.4.2 step 1a (the Paused gate) and the supervised-evaluate ID guard: Deferred (M3).
- The Paused window (`SSumiPausedWindow`): Deferred (M3).
- Task-step supervision: Deferred (M3).
- Live (docked) Preview: Deferred (M4); LiveDocked runs as the Run window (A28).
- `FSumiLanguageServices::SetRunActive` in the common protections: built (B55).
- The debugpy frame rule: implemented in slice A; nothing exercises it until debugpy is vendored, Deferred (M2).
- Insights trace channels: built as `SumiPump` and `SumiRun` (B39, A81).
- The pause refusal inside a pump (`ESumiPauseRefusal::InsidePump`): Deferred (M3).

## output (store, forwarder, device, views, REPL, console executor)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- Console executor live completions and `FUnrealNameIndex`: built (B63); loading the index from the Stage 1 snapshot:
  Deferred (M2).
- Show all for results over 1 MB: Deferred (G1); slice A's Copy stays.
- Reopen tab for a closed buffer behind a `<sumi:…>` link: built (B86).
- [Run in transaction]: built (B38). [Inspect]: Deferred (M2).
- [Open storage settings]: built (B23).
- Copy as script, presets, SGR spans, run/source/regex filters: Deferred (M2). Find (Ctrl+F) and the soft-wrap toggle:
  built (B92).
- REPL: Ctrl+R and the prefix filter: Deferred (M2); Enter accepting a completion: built (B62); suspension while
  paused: Deferred (M3); merging a secondary instance's history: built (B20).
- Problems: lint and parse items, dedup and severity, F8 / Shift+F8: built (B56); type-server items: Deferred (M2);
  clearing runtime items when a module reloads (RLD-01): Deferred (M3).
- SumiOutput Insights counters: built (B91). `retention.run_logs_days`: Deferred (M4) as a policy key (A158).
- `FSumiThreadTable` keyed by (native ID, `PyThreadState*`): built (B90).
- One joined row for a line over 64 KB, and the output view on SCodeView's line-source mode: Deferred (G1).

## shell (module, tab, code view, buffers, journal, settings, commands)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- The custom `SCodeView`, IME and accessibility beyond the stock widget: Deferred (G1).
- Italic and bold faces and the JetBrains Mono composite (CFG-04): waiting for the owner's decision on the OFL files.
  Theme JSON: built by the restyle (PO-04, ARCHITECTURE A189 to A191).
- The gutter run lane, change bars and ran-line marks (EDT-18): Deferred (M2).
- The RUN-26 hint chip: built (B85).
- Auto-indent, auto-close and duplicate line: built (B05, B71); other line commands: Deferred (M2).
- Symbol breadcrumbs from the outline (NAV-02): Deferred (M2).
- Tab badges: language, dirty, lock and recovered built (B84), and since PO-04 the language badge is not shown (A182);
  pin and reorder Deferred (M2); the restricted badge Deferred (M4).
- Recent Files: built (B83). Save All, Revert, Rename and Close Others: Deferred (M2).
- External-change detection and the expected-stamp registration: built (B80); the conflict bar: Deferred (M2).
- The read-only target bar: built (B81); the async checkout: Deferred (M4).
- "Save as UTF-8?": built (B82).
- The Recovery Review panel, Compare and the per-buffer actions: built (B17).
- Local history: built (B14, B15).
- The clean text of counted scratch buffers after a crash: built (B16), replacing slice A's direct restore.
- `storage.root_override`, the probe thread, `projects.json`, the moved-project import, orphans and the secondary
  namespaces: built (B20, B22).
- The restore guard and automatic safe mode: built (B18).
- `state/file-state.json` for closed files (BUF-07): Deferred (M2).
- Checkpoint writing in slices: built (B10).
- Live Font size changes and Ctrl+wheel zoom: built (B74).
- The console commands `Sumi.Doctor`, `Sumi.Policy.Reload` and `Sumi.Diagnostics.Export`: Deferred (M4).
- The WS-05 M2 fields: Deferred (M2).
- The command palette and Search Everywhere (WS-06): Deferred (M2); Expand Echo and Fold Echo stay View-menu commands
  (A54).
- The Enable Python condition with the policy key `python.allow_enable_at_runtime`: Deferred (M4).
- Crash keys: built (B31). The `FEditorDelegates::MapChange` alias release: built (B36).
- The Remote Execution exposure warning (SEC-04): Deferred (M4).
- `SumiEditor.Build.cs`: DirectoryWatcher, TraceLog and the Settings include path added, and PropertyEditor with the
  restyle (the ThemeId dropdown); ContentBrowser and ContentBrowserData Deferred (M2); SourceControl, Json and
  JsonUtilities Deferred (M4); Sockets Deferred (1.1).

## tests (tier 4 and the static checks)

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

- The §14.2 editor fixtures: built (B100); `FSumiPausedWindowTestDriver`: Deferred (M3).
- The G2, G5 and G7 gate suites, T-CAN-001 with 200 stops per shape timed from the SendInput click: built (B101 to
  B103); the 200-stop pass ran in the restyle integration (13/13, p99 80–122 ms).
- Journal restore across a real restart or kill -9 (T-PER-036, T-E2E-010): the G6 driver prototype (B24).
- T-GAP-019: built (B106); all five steps pass since the restyle (5/5 in its integration and final verification).
- The tier-1 static checks of TDD §2.5 rules 5, 7, 8, 9, 10, 11 and 12, T-GAP-001 and T-GAP-008: built (B105).
- CQTest-based specs (§2.3.3): moved, no functional gap; the module stays on Automation Spec.

## Open after the review rounds

From slice A (A56 to A64), with their state after slice B:
- Seeding the tab's "One undo step per run" toggle per route (A56, P2-run-02): **closed** by B40 (A87).
- Batched retry-queue replay in `FDreamJournalWriter` (P2-per-02): **closed** by B13.
- A tier-4 case for a counted recovery whose disk file is over 1 MB (P2-shl-01): open; it needs a killed DreamHost
  session and belongs with the G6 driver.
- An unconditional empty-state check in `Dream.Keys.Focus` (P2-shl-06): **closed** (B108).
- A lost-run probe after a disk outage (P3-per-02): **closed**, a new case in `Dream.Persistence.Journal` (B108).
- A no-selection step for Down leaving follow mode (P3-out-03): **closed** by `Dream.Output.DownKey` (B93). The timing
  gate for the history switch at the first tab spawn (A2-01): **closed**, a 2 ms gate in `Dream.Repl.History` (B108).
- RUN-10, Python disabled and re-enabled at runtime around a run: **closed** by `Dream.Lifecycle.Dynamic` in its own
  session (B107).
- T-SUP-029 mouse pairing and the Esc cases with a foreign modal and with an open menu (LRS-11): built in
  `Dream.Supervisor.Bot` (B104); the click cases ran green in the restyle's final verification, and T-SUP-029's
  "down on Stop, release outside" now stops the run (A216).
- T-CAN-009's direct read of the interpreter-wide `pending.async_exc`: stays open (CPython internal headers); the
  placeholder delivery is asserted.
- T-REPL-005 and T-REPL-017: **closed** in `Dream.Keys.G5` (B102).
- T-OUT-020: **closed** by `Dream.Output.EchoCap`. "T-OUT-023 (the output device's no-double-echo guard)" was a wrong
  name: the guard is a row of T-OUT-021, covered by `Dream.Output.EchoGuard`; T-OUT-023 is run-log creation, covered by
  `Dream.Output.RunLogCreation` except three rows (below).
- T-UEI-002's other WS-07 rows: **closed** by `Dream.Output.StatusItem`; Stop in task mode (M3) and Live (docked) Stop
  (M4) are not tested.
- SHL-14's consume-list leak checks T-KEY-009..013: **closed** in `Dream.Keys.G5` (B102).
- §10.6 compat namespace (PER-11): **closed** by B21.
- Tier-1 coverage of the writer's commit plan and retry queue: **closed** by `BatchPlanner` and its case (B13).
- `Writer::Rotate` deleting g-1 without looking at its header: **closed** by B12.

New in slice B:
- **Round-5 minors**, recorded as known open items with no further fix round:
  - G5 still sends Ctrl+Shift+C when the clipboard holds non-text, and the Output view copies on any Ctrl+C without
    Alt, so the clipboard can be emptied.
  - `run_g6.ps1` and `run_restart.ps1` on Ctrl+C: plain output inside `finally` after the kill aborts the rest of the
    PowerShell 5.1 `finally`, so the restore (and `run_g6.ps1`'s root cleanup) is skipped until the next run; the
    restart driver's header claim about Ctrl+C is therefore false.
  - `run_smoke.ps1` (slice A) kills every DreamHost editor on a timeout, not only its own.
  - Ctrl+PageUp/PageDown (the tab switch) still trigger the sticky-column refresh while the column is stale.
  - `run_g6.ps1`'s path for an ini that was absent before the run deletes the whole ini.
- **The click specs** (17 cases: 13 in `Dream.G2.Cancel`, 3 in `Dream.G2.Supervisor`, `Dream.Supervisor.Bot` T-SUP-029)
  did not pass three attended runs in a row on slice B's final fixes: from 02:47 on the night of the build a Windows
  Defender Firewall prompt (PickerHost.exe) covered the desktop, and every SendInput click landed on it. The restyle's
  integration ran them on a free desktop: two `G2.Supervisor` cases failed (T-CAN-020, T-SUP-002, latent slice-B
  defects, fixed, A222, A223), the others passed, and the final verification ran all of them green (Latest results,
  Restyle).
- **The 200-stop `Dream.G2.Cancel` pass**: **closed**, 13/13 in the restyle integration (p99 80–122 ms).
- **Restart-driver steps 3 to 5** (the splitter drags of T-GAP-019): **closed**, 5/5 in the restyle integration and
  final verification, after the drag learned to wait for a Slate tick (A220).
- **libFuzzer T-PER-005** needs the x64 ASan component for MSVC 14.44 (owner decisions).
- **Remote-root heartbeat liveness**: nothing rewrites the lock's `utc`, and taking over a stale lock from another host
  needs its own design and a network share to test.
- Tests not written: T-PER-014, T-PER-046's junction and heartbeat cases, T-PER-034 and T-PER-044 as automated checks,
  T-AUD-003 and T-TEL-002 (tier 6), T-LIFE-014's GC check, T-RES-017 (needs the §7.10.3 counter fixtures), T-MRG-007,
  the Cold-boot part of T-REPL-013, T-STUB-016, the element-kind part of T-API-016, three T-OUT-023 rows (the eviction
  marker, the synthetic header after an evicted header, the throttled-disk gap line), T-GAP-021's golden images,
  T-KEY-018's keymap-page step and T-PER-047's byte-offset matrix.
- The G2 table rows that need fixtures or environments this machine lacks (T-SUP-006, 008, 009, 034, 038, T-SUP-001
  with PIE, parts of T-SUP-002/003, the floating-tab and PIE variants of T-KEY-010, T-KEY-016) and the tier-7 LSP chaos
  tests (ARCHITECTURE.md section 9).

New in the restyle:
- **Stock scroll bars inside stock text boxes** (the find bar, Go to line, the recovery diff box) keep the editor's
  style, and so do dropdown menus and tooltips; the WS-07 status-bar item and the Editor Preferences combo sit outside
  the tab and stay editor-styled (ARCHITECTURE A195).
- **The Run window's outer frame**: its 1 to 2 px border is the engine SWindow's (#0F0F0F/#333333) in every theme
  (arch-r6-m2). Accepted; no action unless it is cheap.
- **ExceptionGroup and the exception-line heuristic**: the exception line is found by reading the traceback text (since
  r6 only while a frame section is open), so an ExceptionGroup gets `Error:` on its first line, and a log-origin
  traceback without an exception line absorbs later `unreal.log_error` lines (A205).
- **The `Repl.History` A2-01 gate (2 ms) is flaky under load**: it measured 2.13 ms and 2.016 ms in two loaded group
  cycles, and it passed alone (15/15) and in the integration and the final verification. It stays on watch.
- **Focus-dependent specs need a free desktop** (`Dream.Supervisor.RunWindowFocus`, `Dream.Supervisor.StopPress`, the
  SendInput specs): no longer open, since the final verification ran them all green on a free desktop, but every
  attended rerun needs the same conditions (known risks).
- **Review round r11** checks the r10 minors patch (Review rounds, Restyle).

## TDD patch list

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

Wording the TDD needs at its next revision (`docs/tools`; the code already follows it):

- §5.9.3 Purged: replace "At End, `GetQueueLength()` is below its value before Begin, or a map load ... or an asset
  delete" with "`UTransBuffer::Reset` was observed (`OnUndoBufferChanged` with an empty, inactive queue outside
  `GIsTransacting`, right after our cancel or after our 100% end, until an undo without redo removes our record), or a
  map load or an asset delete; a shorter queue alone is not evidence (redo stacks, budget evictions, an undo that drops
  entries)" (ARCHITECTURE.md A64, P3-run-02, P4-run-01, P5-txo-01).

From slice B (A-numbers are ARCHITECTURE.md section 7):

| TDD section | What the text should say | Source |
|---|---|---|
| §5.7.3 | The pseudo-code's `bDue` re-delivers only after 250 ms, while the fast-chain prose and the §5.7.8 sequence re-deliver at the next check. Write: a re-queued (chained) call is due without the gap only when no exception at all is being handled; otherwise the 250 ms rule applies | A76 |
| §11.4 vs §5.2.3 | §11.4 puts `run.start` "at Starting, after the trust and parse checks"; §5.2.3 and the code put it at Running, so a not executed run has only `run.end` | A78 |
| §11.4, §12.3 | `Event(const TCHAR*, const sumi::json::Value&)` and `RecordLostRun(s)` taking `FSumiLostRun`; lines written before the role is known wait for it | A77 |
| §3.1, §10.2.1, §11.4 | The audit has its own writer thread (`SumiAudit`), not a lane of the journal writer; the journal writer and the storage probe run at `TPri_Normal`, and the writer's lower-priority lane is logical (slices between commits) | A79, A105 |
| §5.4.6, §13.1 (TEL-03) | Trace channel names cannot contain dots: the channels are `SumiPump` and `SumiRun`; counters stay on the counters channel | A81 |
| §3.9 | The "waiting for the editor to become idle" text shows only while the run waits for an idle end of frame, not during the snapshot or the Run File save | A82 |
| §5.10.2, §12.1.1 | `_sumi.check_cancel(deliver: bool = False)`; the plain call counts no delivery | A83 |
| §7.5.2, §12.1.1 | `container_element_kind` returns None in M1 (the wrappers are private to PythonScriptPlugin); the `hard_object_props` normalisation and collision rule | A88 |
| §5.3.8 | `subsys` is a function `subsys(cls)`; aliases are refreshed on an idle tick 250 ms after selection events | A84 |
| §2.5 rule 5 vs §5.3.7 | `PyEval_EvalCode` in `run_statement` and the ClearAsyncExc placeholder's `PyRun_StringFlags` are allowed exceptions to "one entry call" | A163 |
| §4.1.3 | A piece has a fifth field, `LfIndex`; compaction also triggers on chunk memory over 2 × the text + 1 Mi units | A65, A66 |
| §4.2 | Undo ops store copied text, not PieceSpans | A69 |
| §4.3.3 | The gap-buffer line array lands in M1; the line hash stays with SCodeView (G1) | A72 |
| §4.4 | The limits are counted per step, each code point a backreference compares counting one, and a compile is bounded at 4 × MaxProgram; literal search may run on the game thread for plain-mode buffers, regex search and Replace All on the worker | A70, A130 |
| §4.13 step 4, §10.14 | A save writes the format under which the bytes read back as the text: a UTF-16 BOM, then the coding line, then UTF-8 | A139 |
| §10.14 step 6, §10.15 | The self-write stamp is size plus SHA-256 (no mtime) and is forgotten when the save lands; read-only attribute changes are noticed at rescans, since DirectoryWatcher does not watch attributes | A135, A136 |
| §10.1 | The override probe runs at engine init and does not open `instance.lock`; a non-main imported namespace is `imported-<oldhash>.<ns>`; the strict `-SumiStorageRoot` session switch | A117 to A119 |
| §10.2.2, §10.7.1 | Record CLEAN_TEXT (0x16, skippable) carries a scratch buffer's clean text | A97 |
| §10.4 | The history index and blob formats; run entries merge with the buffer's entry before them in time; history pauses (never latches) while an index generation is unreadable, and the pause ends on its own after the release | A98, A99, A104, A172 |
| §10.2.4, §10.11 | A generation still unreadable after the FileOps retries counts as damaged: kept, never deleted, nothing written above it; the reader never touches or renames it, and only the start-up set-aside, after reading every header, may move it | A102, A173 |
| §10.2.3 | A successful rename is the switch to g+1, even if the open that follows fails | A103 |
| §10.2.5 | `SnapshotForRun` carries every buffer's pending edits, not only the run's buffer | A120 |
| §10.5 | An orphan with any unreadable generation is kept and not listed; any failed merge step keeps the namespace | A113, A115 |
| §10.7.3 | The Review panel sits under the banners in M1 (no side bar) | A109 |
| §10.8 | The guard counts only at a spawn with the journal open, and an automation session never enters safe mode by the guard | A107 |
| §10.10 | The G6 step is a console command, not an automation test, and waits for the restore guard to clear before typing | A121 |
| §6.4 | Find and soft wrap in the output view are M1 | A159 |
| §6.3, §6.8 T-OUT-023 | The run-log threshold counts output lines only, not echo lines; a new echo cap starts a new window | A157, A158 |
| §6.8 T-OUT-021, T-OUT-023 | No change: T-OUT-023 is run-log creation and the no-double-echo guard is a row of T-OUT-021 (the slice-A status named it wrongly) | output report |
| §7.1.3 | The escape check counts a process as the server's child only if it was created at or after the server, and scans nothing without a job | A91 |
| §7.2.5 | Profile files and `NeutralConfig` are M2; M1 configures sumi-check through `LspClientOptions` | A90 |
| §7.4.2, §12.1.1 | `stub_emit` and `stub_module_done` are not needed: `stubs.step` returns its output, and a pipe appends it in 1 MB batches | A96 |
| §7.4.3 | The key adds `format=sumi.stubsnap/1`, `walker=2` and `stubgen=none` | A96 |
| §7.5.5 | The word-start weight is 40, not 16 | A147 |
| §7.6.2 | In M1 "Loading…" shows at once after an explicit Ctrl+Space; the 150 ms delay is M2 | A149 |
| §14.2 | The foreign-modal fixture is a custom SWindow, because `EditorDialog.show_message` opens nothing under automation | A165 |
| SPEC EDT-05 | Type-over ships in M1 (only for auto-inserted closers) | A127 |

From the restyle (PO-04):

| TDD section | What the text should say | Source |
|---|---|---|
| §5.15.3 T-SUP-029 | The expected result says "Stop fires only for down and up on Stop". Write: Stop fires on the press (mouse-down, touch down), so "down on Stop, up outside" stops the run and "down outside, up on Stop" does not; there is no drag-off cancel, and the keys are unchanged | A216 |

From PO-05 (SPEC and TDD rows; A-numbers are ARCHITECTURE.md section 7):

| Section | What the text should say | Source |
|---|---|---|
| SPEC WS-02 | The left side bar lands with PO-05, shown by default, holding Project (BUF-08's Files side bar) and Structure (NAV-02's outline); Snippets stays Later | A227, A228 |
| SPEC BUF-01 | An unnamed tab is a scratch file `scratch_N.py` in the per-user scratch folder, auto-saved, and journaled between saves; a journaled `Untitled-N` is only the fallback when the folder can't be written | A237, A238 |
| SPEC BUF-08 | The Files side bar is the Project tree: the project's `Content/Python`, roots configured per project (Editor Preferences ▸ Sumi (this project)), the mounted plugins' and the engine's `Python` folders (a setting, on by default), and Scratches | A229 to A233 |
| SPEC NAV-02, TDD T-NAV-005 | NAV-02 lands in M1 without a language server (PO-05), with jump and filter. Change T-NAV-005's Lands in the same edit (TDD rows at about lines 5401 and 8653) to "M1 (without a server, PO-05); M2 (with a server)", or check 6 of the trace scan fails | A234, A235 |
| TDD §10.2.5 | A closed scratch file whose close save is refused or fails gets a Closed local-history entry, as an `Untitled-N` did | A238 |
| TDD §10.3 | Scratch file tabs are saved as File tabs under the scratch folder; `ESumiTabKind::Scratch` stays for the fallback | A237 |
| TDD §10.14 | The scratch auto-save reasons (Timer, FocusLost, RunStart, Close, Exit, Other) and the 5-minute history throttle for the first three | A238 |

## Owner decisions

_Historical record: kept with the names of its time. The product is now "Dream Python Editor" (PO-06 phase 3)._

Things only the owner can decide or do:
1. **Install the x64 "C++ AddressSanitizer" component for MSVC 14.44** (Visual Studio Installer). It enables
   T-PER-005 as a libFuzzer target and ASan for T-CORE-003; today only the i386 libraries are installed (A75).
2. **Keep the desktop free during attended SendInput runs**: no clicks or typing, and no other window over the editor (a
   Windows Defender Firewall prompt, PickerHost.exe, covered it from the night of slice B's build, a Discord window
   during earlier runs, and RuneLite and the Alt+Tab switcher during restyle passes). The reruns this item asked for
   are done: the restyle integration ran the 200-stop `Sumi.G2.Cancel` pass (13/13) and restart-driver steps 3 to 5
   (5/5), and its final verification ran every click case green.
3. **The JetBrains Mono fonts**: vendor the four OFL font files with their licence for the CFG-04 code font and the
   italic and bold faces, or keep the engine's monospace face (A38).
4. **A network share** if remote-root liveness (the lock heartbeat and stale-lock takeover across hosts) should be
   designed and tested.

### Restyle (PO-04)

Decided:
- **PO-04** (owner ruling, 2026-11-03): Sumi should not look like a C++/IDE code editor; it keeps the clean look of
  Maya's Python script editor, with a set of themes. The owner's three answers: the default theme is **Darcula**
  (PyCharm's built-in dark scheme), the chrome is **Maya-minimal**, and errors read **Maya style**. It outranks SPEC and
  TDD UI rows; features stay, and every hidden part is one View toggle away (ARCHITECTURE A178 to A185).
- **PO-04-R1** (the architect's ruling under PO-04): PO-04 hides bars, not warnings. With the status line hidden,
  journal, Python and lint warnings show in the input tab row beside `line:col` in the warning colour, a click does the
  status strip's action, and a hidden banner shows again when a new line or state appears (A186). In round r7 the
  architect accepted the journal actions as built: Degraded opens the journal folder, off opens Sumi settings.
- **Stop acts on the press** (the architect's ruling after the integration, A216): the Run window's Stop fires on
  mouse-down, because Stop is the emergency control and a press must not be lost when the cursor moves before a blocked
  editor processes it; a stray press only stops the run, so there is no drag-off cancel. Esc and Shift+F5 are
  unchanged. The TDD's T-SUP-029 row needs the new wording (TDD patch list).
- **Settings writes are checked** (the architect's rulings in rounds r8 and r9, A218): `SaveKey` writes once and checks
  the write; a refused write is retried with a backoff from 0.5 s to 30 s, with one warning after 5 s, and a read-only
  EditorSettings.ini gets one warning ("Sumi settings are not saved") and no retry.

Open questions for the owner (still open after round r10):

5. **`>>> ` / `... ` prompts in the history** (output R4): answered 11-04, **dropped** (A226). Echo rows are their
   source lines as they are, as in Maya; the run log keeps its `>>>` / `...` (§6.2). Checked 11-04: build 0 warnings;
   attended `Sumi.Output+Sumi.Repl+Sumi.Run.Session+Sumi.Visual.Capture` 63/63 (MayaStyle 9/9 after its new echo check
   waited for the rows to be drawn); headless 82/82; tier 1 199/199; scans 14/14; the ini's Sumi section unchanged.
6. **`OutputPosition`**: PO-01's default is Top, but the user profile (`[/Script/SumiEditor.SumiUserSettings]` in the
   user-global EditorSettings.ini) showed Bottom several times. Bottom was already in the backup the restart driver
   took before slice B's restart tests; during the restyle passes the key went between Top and Bottom, and the writer
   found was `Sumi.Shell.TabLayout`'s Top and Bottom case, whose restore reached the config cache but not the file. That
   leak is fixed, and the settings guard now fails any pass that leaves a user setting changed (A213, A224); the tests
   group set the key back to Top before its last r6 cycles, and it stayed Top: every later pass that checked the ini's
   Sumi section found it identical, the integration and the final verification included. Confirm which you want, Top
   or Bottom. The same section also holds `StorageRootOverride=(Path="")`, which a settings save added (harmless: empty
   means no override), and `bRunPump=True`, the default.

### PO-05

Decided:
- **PO-05** (owner ruling, 2026-11-04, verbatim): "There should also be a directory structure AND a structure section
  and all unnamed tabs go into a scratch folder and allow you to choose your root paths for the directory structure
  similar to PyCharm in settings. The code structure section should just show the structure of the active tab. Tabs
  should also save on save and load, and all your test scripts should be in the scratch section".
  - Your answers: the side bar on the left, shown by default; the scratch folder per user, shared by all projects;
    "save and load" means scratch tabs auto-save and the open tabs come back on start; the sample scripts from the
    tests and screenshots go into the scratch folder (ARCHITECTURE A227).
- **PO-05-A1** (owner, verbatim): "Also by default scan the engine and project plugins for python content directories
  to be added automatically to the project", and "This should be a setting that can be enabled and disabled in the
  editor" (A233).
- **The architect's rulings in the review rounds:**
  - **Save As:** keeps the scratch file as last auto-saved, with no copy back (R-p1-1, A239).
  - **Scratch folder:** the scratch flag is path-based, and the folder setting is guarded (R-p1-2, A236).
  - **Throttled history:** auto-save history is throttled to one entry per 5 minutes (R-p1-3).
  - **The `*` mark:** shows only while auto-save is paused (R-p1-4).
  - **Recycle Bin:** routine passes never touch it (R-p1-5).
  - **Test folder:** emptied before every pass (R-p1-6).
  - **Empty-file reuse:** only empty files this editor made and closed unused (R-p1-8, p3).
  - **Structure highlight:** drawn through the row's border, so selection and hover win (R-p1-9).
  - **Roots settings:** the roots get their own settings class (R-p1-10).
  - **Network roots:** never watched (R-p1-11), with their own listing budget (p3).
  - **Restored conflicts:** keep the journal's base, so the Keep mine / Reload bar holds (p2).
  - **`Content/Python`:** the project's `Content/Python` always shows when it exists (p3).

Open items for the owner:

7. **The Recycle Bin file:** the integration ran the real Recycle Bin case once. It left one `real_recycle_<suffix>.py`
   in your Recycle Bin (results below); empty it when convenient.
8. **The A2-01 timing gate** (`Sumi.Repl.History`, 2 ms, slice A) fails under load now and then. Either keep it and
   accept a rerun, or have it measure more samples (the median of several runs). This is a follow-up and doesn't block
   PO-05.
