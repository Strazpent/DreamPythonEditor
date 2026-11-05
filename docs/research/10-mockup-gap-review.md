> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 10 — Mockup gap review (draft mockups, SPEC v0.9)

> Historical record. The mockup builders wrote these per-page reports against SPEC v0.9, when the product was still called "Python Script Editor". The gaps they found are closed by the UX rulings U1–U41 in [08 §8](08-decision-log.md#8-ux-rulings-from-the-mockup-review-u1u41) and traced in SPEC Appendix D.2. Names, keys and paths in this file are the draft ones.

### 02-run-and-cancel
I rendered the mockup three times and the last render is clean. Nothing is clipped, the callouts sit next to what they explain without covering text, and the legend fits inside the 1600x1810 canvas.

**Files**
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\02-run-and-cancel.html
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\02-run-and-cancel.png

**What the image shows**
- **Main window:** Run #22 of `batch_reimport.py` at 00:41. The script loops over 1,204 `AssetData` from `get_assets_by_path`, calling `ue_script_editor.progress` and `AssetToolsHelpers` / `AssetImportTask` / `import_asset_tasks`.
  - Line 17 has the `.running` highlight and a gutter marker. The buffer is marked "Read-only while Run #22 runs" and the REPL is disabled.
  - The toolbar has a red Stop with "Ctrl+Break · Esc". All other run commands are greyed out.
  - Output streams Interchange, FBX and Nanite log lines. A run overlay over the output shows 00:41, a 312 / 1,204 bar with `SM_Granite_Cliff_Hero_01`, the latest output line and a Stop button.
  - The viewport, Content Browser, menus and toolbar are dimmed and hatched. A note says "Editor input paused" and explains why.
  - The editor status bar reads "Running 41 s". The Unreal status bar has the WS-07 script item.
- **Three state cards:**
  - NativeWait at 00:47: `unreal.AssetTools.import_asset_tasks`, 6.2 s, "Stop takes effect when it returns.", Stop queued.
  - Stopping… at 00:52: the native call returned after 11.2 s and `ScriptCancelled` is raised at line 18.
  - Idle: the output block ends with "Run #22 cancelled after 00:52 · 312/1,204 processed · 41 s in native calls · transaction kept, Ctrl+Z to revert" and a link to `run-22.log`.
  - The timeline adds up: the 11.2 s call starts at 00:40.8 and returns at 00:52.
- **Cooperative long-job mode:** an inset with an `@ue_script_editor.task` generator (`StaticMeshEditorSubsystem.get_lod_count`, `yield` per item) beside a live viewport with an actor being dragged. The Unreal status bar shows "audit_lods 45% · 1,872 / 4,160 · Cancel".
- **10 numbered callouts and their legend.**

**SPEC.md inconsistencies and UX gaps**
1. **Task mode has no run state.** Section 9.2 has no state for an active `@ue_script_editor.task`. It doesn't say whether a blocking run can start while a task runs, whether task steps share the run transaction, or how a task is started, named or handled (API-01 lists only the decorator). The brief's "editor stays usable" contradicts the input-blocked main scene, so I drew task mode as a separate inset.
2. **Stop keys may not work from the viewport.** LRS-02 says Esc works "while the run overlay has focus", but the 9.3 keymap says "Esc (while running)". CFG-03 makes shortcuts focus-scoped, so it's unclear whether Ctrl+Break works when focus was in the viewport. The input pre-processor needs a global Stop chord on its allow-list.
3. **Two names for each state.** WS-05 uses "Cancelling" and "In native call". Section 9.2 uses "Stopping…" and "Waiting for native call…". LRS-03 says "Stop takes effect afterwards" where the brief says "when it returns". I used the status-bar names in status bars and the overlay names in overlays.
4. **What a cancelled run's transaction does is not specified.** Neither "kept" nor "rolled back" appears in SPEC. Asset reimports are mostly not undoable, so "Ctrl+Z to revert" over-promises for this script. The report should say what is actually undoable.
5. **"Processed" is undefined.** It isn't clear whether `progress(i, n)` counts the current item or completed items. That decides whether an item whose native call finishes after Stop is counted.
6. **J5 doesn't say which mode it uses.** It promises a 20-minute reimport "without freezing", but under LRS-01 a normal run blocks all editor input. Only LRS-05 (P1) keeps the editor usable.
7. **The reference mockup 01 disagrees with SPEC.**
   - It shows "Namespace: Shared", but RUN-10 calls the mode Session. I used Session.
   - It labels Ctrl+Shift+Enter as run cell, but section 9.3 binds that to Run whole buffer and Run cell to Ctrl+Alt+Shift+Enter.
8. **Unspecified while blocked:**
   - Whether the level viewport keeps rendering while input is blocked (the run pump does no world tick).
   - Where the overlay goes when the output panel or the tab is hidden.
9. **Two additions of mine that SPEC doesn't have:** the ETA ("about 2 min left") and the "Stop queued" button label. Both are suggestions to adopt or drop.

### 03-completion-and-docs
Files:
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\03-completion-and-docs.html
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\03-completion-and-docs.png (1600x1440, 4 renders)

**What the image shows**

One UE 5.8 editor window with the same title bar, dock tabs, toolbar and status bar as 01. Inside it is a 2x2 grid of zoomed panes. Each pane has a titled header strip with its requirement IDs. There are 10 numbered callouts and a 2-column legend.

- **A. REPL live completion (REPL-03, INT-02, INT-03).**
  - The history shows `len(sel)` and the `sel[0]` repr ending in `Class 'StaticMeshActor'`. The prompt reads `sel[0].`, and the popup opens upward.
  - The popup header reads "sel[0] is a StaticMeshActor [live] · static type: Actor".
  - StaticMeshActor's own members come first: `static_mesh_component`, `set_mobility(mobility)`, and the editor properties (E) `static_mesh_replicate_movement` and `static_mesh_physics_replication_mode`. Then comes an "Inherited from Actor · deduplicated" group with live+stub badges, then postfix snippets with snippet and project badges.
  - The footer reads "10 of 187 · live lookup 1.8 ms".
- **B. Signature help (INT-04).**
  - The cursor is at `unreal.EditorAssetLibrary.rename_asset(src, |)`, and `destination_asset_path: str` is the highlighted parameter.
  - The popup shows that parameter's own doc line, the real C++ header text ("Rename an asset from the Content Browser…"), "Returns True if the operation succeeds", and live/stub badges.
- **C. Hover docs (INT-05).**
  - The mouse rests on `unreal.ScopedEditorTransaction`. The card shows `class unreal.ScopedEditorTransaction(desc: Text | str)`, the docstring, and the `cancel`, `__enter__` and `__exit__` methods.
  - It also shows "Module unreal · native type from PythonScriptPlugin · docstring from C++", and the footer reads "F1 open API docs · Ctrl+Q pin as Quick Docs · F12 go to stub".
- **D. Diagnostics (INT-07..09).**
  - `import os` is faded with a squiggle and a lightbulb in the gutter. The Alt+Enter menu offers: Remove unused import 'os', Remove all unused imports in file, Ignore F401 on this line, and Explain rule F401.
  - A Problems list covers all open files:
    - F401 from ruff.
    - UE101: `os.rename` on a `/Game/` Content path, with a preview line and the suggestion to use `unreal.EditorAssetLibrary.rename_asset`.
    - UE103: actor edits in a loop without a transaction.
- **Status bar (INT-01, INT-11):** Language server, Ruff (38 ms), Stubs (5.8 · split · generated 14:01) and Live completion (1.8 ms), each with its state.

**SPEC and brief problems found while designing**

1. **The brief contradicts itself in pane D.** If `os.rename` is in the file, `import os` is not unused. I put the unused `os` in `audit_textures.py` and showed the `os.rename` warning from `move_legacy.py` in the Problems list, with a preview line.
2. **The "project" badge (INT-03) can't honestly appear in member completion on a live engine object.** It only applies to user-module symbols (INT-06). I put it on a project-library snippet instead. "project" also means two things in the SPEC: a completion source in INT-03 and a snippet scope in SNP-01. One of them needs a different name. The "keyword" badge isn't shown, because keywords never follow a dot.
3. **Keymap §9.3 is missing three rows:**
   - Quick fix. INT-09 says "Alt+Enter or Ctrl+., depending on keymap", but the Charcoal preset has no entry for it. I used Alt+Enter.
   - Signature help re-trigger.
   - Format document (INT-10). INT-10 isn't illustrated.
4. **INT-08 rules have no rule codes.** "Ignore rule on a line" and `pyproject` config need them. The mockup's codes UE101 and UE103 are invented.
5. **`sel` can be stale.** UEI-03 refreshes `sel` only on each run, so the live type in the completion header can be out of date if the selection changed since the last run. The SPEC should say whether completion refreshes the alias or labels it "as of last run".
6. **The INT-08 "missing transaction in loops" rule overlaps RUN-07.** Every run is already one transaction by default. The rule should fire only when that toggle is off, or for code run from shelves, menus or startup, or it will be noise.
7. **The SPEC doesn't say what accepting an editor-only property inserts.** EditAnywhere properties that aren't BlueprintVisible can only be reached through `get_editor_property`, so plain attribute access fails. Inserting `get_editor_property("…")` would make sense.
8. **INT-05's "docstring generated from C++ tooltips" doesn't cover native Python types.** `ScopedEditorTransaction`'s docstring is a `tp_doc` string in PyEditor.cpp, and it contains the typo "create and managed". I corrected it to "manage" in the card.
9. **The reference mockup 01 conflicts with §9.3.** It says Ctrl+Shift+Enter runs the current cell, but §9.3 makes it "Run whole buffer" and assigns cells to Ctrl+Alt+Shift+Enter.
10. **The SPEC links files that don't exist yet:** `TDD.md` and `research/08-decision-log.md` (ADR D4–D6).

### 04-traceback-to-source
I made the mockup and rendered it four times. The final render is clean: nothing is clipped, no callout covers important text and every popup fits.

**Files**
- `C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\04-traceback-to-source.html`
- `C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\04-traceback-to-source.png` (1600x1192)

**What it shows**

Three stages left to right, joined by blue elbow arrows. Each arrow is labelled with the user action ("Click frame, line 88" and "F4 next problem"). There are 10 numbered callouts and a 2-column legend.
- **A, the failed run.** scratch-1 shows the run range 12–20 with a red bar and line 18 tinted red. The Output view has:
  - Collapsed Run #29 and Run #30 headers, then an expanded red block for Run #31 · scratch-1 · lines 12–20. Its header shows time, duration, a KeyError badge and the undo name "Script: scratch-1 #31".
  - Per-type toggles (OUT-03), with Echo switched off.
  - Typed lines: a LogPython warning and info lines, then a Python 3.11 traceback with `~~~^^^` markers.
  - Two clickable frames: `"<scratch-1>", line 18` (RUN-06) and `"studio_tools/importer.py", line 88`, with a hover tooltip (OUT-05).
- **B, after the click.** importer.py opens at line 88:
  - The line is tinted red, `presets[name]` is underlined, the caret sits at Col 20, and an inline note says `✖ KeyError: 'LOD1'`.
  - Above the code are a bar reading "Opened from Run #31 traceback · frame 2 of 2 · Back to Output (Alt+←)" and breadcrumbs ending in `ƒ build_lod_chain`.
  - An inspection widget and error stripe in PyCharm style.
  - The Problems panel lists the runtime error (Run #31), an Unreal lint warning on `options.reduction_settings = reduction` (INT-08) and ruff F401. The status bar reads "✖ 1 error ⚠ 2 warnings".
- **C, after F4.** The caret is on line 94 with an Alt+Enter quick-fix popup (INT-09), and the Problems panel shows "Problem 2 of 3 · F4 next · Shift+F4 prev".
- **C′, a separate case.** scratch-2 has an unclosed `actor.set_actor_location(`. The output reads "Run #32 · not executed" and `SyntaxError: '(' was never closed — nothing was executed`, and the caret is placed on the `(` at line 6, Col 29 (RUN-16).

**SPEC.md inconsistencies and gaps**
1. **What F4 walks is unclear.** OUT-05 and the §9.3 table say "next/previous error in output". The brief asks for "next problem", and the PyCharm (F2) and VS Code (F8) presets behave like next-problem. The mockup walks the Problems list, lint warnings included. SPEC should say which.
2. **Runtime errors in Problems are not specified.** No requirement puts runtime errors in the Problems panel, or says when they clear (next run, editing the line, closing the tab).
3. **A parse failure has no outcome.** RUN-12 only has ok, error and cancelled. It is also undefined whether a RUN-16 failure takes a run number or appears in History. I showed it as "Run #32 · not executed".
4. **Undo after an error is undefined.** RUN-07 doesn't say whether a run that raises keeps or cancels its transaction. I showed it as kept, with "Ctrl+Z reverts".
5. **The quick-fix key is missing from §9.3.** INT-09 says "Alt+Enter or Ctrl+., depending on keymap", but the table has no row for it. I used Alt+Enter.
6. **Line vs caret.** OUT-05 says clicking a frame "selects the line", while RUN-16 says the caret goes to the error. I tinted the line and put the caret at the failing column. Path display is also unspecified: root-relative, as I showed it, or absolute.
7. **I added things SPEC doesn't have.** The "opened from traceback, frame N of M, Back to Output" bar and the inline runtime-error annotation are new. Worth adding to SPEC (OUT-05 or NAV-01). INT-08 also gives the Unreal lint rules no codes, so the Problems row just says "Unreal lint".
8. **RUN-16's test should say which line.** Python 3.11 reports "'(' was never closed" at the opening bracket, and only when the code ends inside it. If another statement follows, Python reports "invalid syntax. Perhaps you forgot a comma?" instead. "The correct line" should mean the opening-bracket line.
9. **01-main-window uses different names from SPEC.** I followed SPEC wording here. 01 says:
   - "Namespace: Shared" where RUN-10 says Session.
   - "Python Editor" where SPEC says Python Script Editor.
   - "Reload Modules" where RLD-01 says Reload package.
   - "Ctrl+Shift+Enter runs this cell", but §9.3 makes that chord Run whole buffer; Run cell is Ctrl+Alt+Shift+Enter.

**Render problem that affects the other builders:** headless Edge runs page scripts with a viewport about 24px narrower than the 1600px screenshot. So callout and popup positions computed by `callouts.js` drift when the layout depends on viewport width; my first render had a callout covering text because of this. I fixed it in my own page with `.canvas { width: 1600px }` and fixed-width columns. Other pages laid out with flex may have the same drift.

### 05-debugger-paused
I built and rendered the mockup. It went through 4 renders, and the final one has no overlaps or clipping.

**Files**
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\05-debugger-paused.html (canvas 1600x1320)
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\05-debugger-paused.png

**What the image shows**
- **Paused banner:** an amber banner across the whole Unreal window reads "Paused in debugger — the rest of the editor is input-blocked until you Continue (F5) or Stop". It has its own Continue and Stop buttons. The main menus, the Content Browser tab and a sliver of the Lvl_Downtown viewport are dimmed and hatched, with an "Input blocked" label.
- **Debug toolbar:** Continue F5, Step Over F10, Step Into F11, Step Out Shift+F11, Run to Cursor and Stop. Run and the Namespace combo are greyed out. A "Debug ▾" menu is open.
- **Code (batch_rename.py, cell [2]):**
  - Line 14 has the yellow arrow and tint, plus a conditional breakpoint.
  - The breakpoint editor sits inline under line 14, so it hides no code. It shows Condition `i == 8`, a Hit count field, and the counts "checked 9×, hit 1×".
  - The gutter also has a disabled (hollow) breakpoint, a logpoint diamond on line 18 and a plain breakpoint on line 22.
  - The breadcrumb shows "Read-only while paused".
- **Output:** a per-run header for "Debug run #15" with its open undo transaction, three logpoint lines, a DBG-08 line "Breakpoint … skipped (unsafe context: Slate post-tick callback)", and a "Paused at …" line. The REPL prompt reads "REPL suspended while paused. Use Evaluate in frame →".
- **Right side bar, stacked top to bottom:**
  - **Call Stack:** four frames: fix_name, then `_apply_one` and batch_apply in studio_tools/naming.py, then `<cell [2]>`. Editor runner frames are hidden.
  - **Variables:** Locals and Globals. The StaticMesh `asset` is expanded to its editor properties (lod_group, light_map_resolution, nanite_settings, static_materials), plus functions and class chain.
  - **Watch:** `len(assets)` = 12 with a "safe built-in" tag, `data.package_name`, and `asset.get_path_name()` showing "calls off · Allow calls".
  - **Evaluate in frame:** `asset.get_name()` returns `'Crate_Large'`.
- **Status bar:** an amber "Paused · debug run #15 · 00:42" chip and "Tracer on (debug session only)".
- **Debug menu:** includes exception-breakpoint toggles and "Start debug server for VS Code / PyCharm (debugpy on 127.0.0.1:5678)" with the freeze warning. The item is shown disabled, with the reason "an in-editor debug session is active", because DBG-07 says the two debuggers are mutually exclusive.
- There are 10 callouts, each explained in a two-column legend.

**SPEC inconsistencies and UX gaps I noticed**
1. **Watch rules conflict with the brief.** VAR-04 says watches run "no calls unless explicitly allowed", but the brief's `len(assets)` watch is a call. The mockup assumes a list of safe built-ins; SPEC needs to define that list.
2. **debugpy listener vs. security rules.** SEC-NF-01 says every listener is loopback-only and authenticated. `debugpy.listen` has no authentication, and port 5678 is fixed, while BRG-01 uses a random port. SPEC needs an exception or a mitigation.
3. **DBG-07 doesn't define the menu's behaviour mid-session.** It doesn't say what "Start debug server" does during an in-editor session, or what Shift+F5 does while the server is running. The mockup disables the item and gives the reason.
4. **The Paused row in §9.2 has no input whitelist inside the tab.** It should list what stays live:
   - the Debug ▾ menu
   - gutter and condition editing
   - the Evaluate input
   - opening caller-frame files such as naming.py
   - the banner buttons
5. **REPL vs. Evaluate in frame is undefined.** SPEC doesn't say whether the REPL prompt runs in the paused frame, runs in the session namespace, or is suspended. The mockup suspends it.
6. **Layout conflict.** §9.1 describes the right side bar as tabs (Variables / Watch / Debugger). DBG-03 needs Call Stack, Variables, Watch and Evaluate visible at the same time. The mockup stacks them in a 400 px bar and hides the left side bar. SPEC should define the debug layout.
7. **Keymap gaps (§9.3).** There is no chord for Run to Cursor, Evaluate in frame or Edit breakpoint condition. In the Charcoal preset, F5 means both Continue (while paused) and RUN-13's per-tab entry point (while idle). That context dependence isn't stated.
8. **Split priorities in DBG-01.** Conditions are P1, but hit counts and logpoints are P2. The M3 build would show a smaller breakpoint editor than this mockup.
9. **Banner placement is unspecified.** DBG-03 doesn't say whether the banner belongs to the main window or the tab. It also doesn't say how Paused shows up when the tab is hidden; WS-07's status item could cover that.
10. **DBG-08 timing is unclear.** It's not clear whether Slate post-tick callbacks can fire inside the run pump during a debug run, which the skip notice in Output assumes.
11. **Name mismatch with mockup 01.** Mockup 01 labels the dock tab "Python Editor", but SPEC's working title is "Python Script Editor". I used the SPEC name.

### 06-crash-recovery
C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\06-crash-recovery.html
C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\06-crash-recovery.png (1600x1825, rendered 3 times; the last render has no clipping or overlaps and every callout is anchored with data-for)

**What it shows**
- **Scene A, the tab reopens after a crash:**
  - The UE chrome matches 01. A non-modal banner reads "Recovered 7 buffers from the session that ended unexpectedly at 14:03", with Restore All, Review… and Discard.
  - The 8 tabs come back in their old order. The 7 recovered ones are marked ⟲.
  - scratch-1 is active and ends on a half-typed line, `actor.set_folder_pa`.
  - The Output panel has a "Session recovery" block: unclean exit, journal checksums, the conflict, the orphaned session, and the last run before the crash.
  - The Review window floats over the editor with no dim overlay. Its rows cover:
    - scratch-1: unsaved, journaled 2 s before the crash.
    - batch_rename.py: dirty, disk unchanged.
    - lod_audit.py: conflict, disk changed after a P4 sync. It has a Compare button and a detail line naming the Compare / Keep Mine / Reload bar.
    - Three more dirty or unsaved buffers.
    - A note that the clean init_unreal.py is not counted.
    - An "Orphaned session" group for PER-05, unticked.
    - Discard Selected… and Restore Selected (7).
- **Scene B, Local History for batch_rename.py:**
  - The timeline covers today and yesterday: current buffer, Before run #22 ("same as current"), Saved 13:47, Before run #18, Before run #14 12:41 (selected), Before run #13 ("same as 12:38:14", deduplicated), Saved 12:38, External reload after P4 sync 11:02, and two entries from yesterday.
  - A box shows where history is stored, per user under %LOCALAPPDATA%, with 18.4 MB of 200 MB used and 7 working days kept.
  - The side-by-side diff has highlights inside changed lines, hatched filler rows, and hunk checkboxes (2 ticked).
  - Buttons: Restore, Restore as new tab, Revert selected hunks (2).
- **Legend:** 10 callouts. It states that journals live outside the project in per-user storage and are written synchronously before every run.

**SPEC.md problems found while designing**
1. **Review cannot be a modal dialog.** §9.4 allows modal dialogs only for destructive confirmations and trust prompts, but the brief calls Review a "dialog". I drew it as a non-modal floating window. PER-03 should say Review is non-modal and that only Discard asks for confirmation.
2. **The "n" in "Recovered n buffers" is undefined.** PER-03 doesn't say whether it counts clean tabs or orphaned-session buffers. The banner also says "the session", singular, while PER-05 puts orphans in the same list. I counted only buffers whose text isn't on disk: 7. The clean tab and the orphan buffer are listed separately and not counted.
3. **The state while recovery is pending is missing.** Nothing says whether recovered tabs can be edited before Restore or Discard, or whether typing counts as restoring. I marked them "not restored yet".
4. **PER-03 doesn't say what happens when a file changed on disk after the crash.** I assumed: flag it as a conflict in Review, restore it dirty, then show the BUF-04 conflict bar. That rule should be written into PER-03.
5. **Discard is undefined.** If it deletes the journal for good, that breaks "Never lose work". I suggest keeping discarded journals in Local History until they expire.
6. **Local History "Restore" is undefined.** The SPEC doesn't say whether it writes to disk. I showed "one undoable edit, nothing written until you save". PER-04 also has no snapshot for recovery or restore events.
7. **No run outcome for a crash.** RUN-12 and RUN-18 have no outcome for a run cut off by an editor crash, and recovery doesn't name the run that was in progress. A scratch tab journaled 2 s before the crash can't happen during a run anyway, because §9.2 blocks editing while Running. So I made the crash happen after run #22 had finished.
8. **01-main-window doesn't match SPEC in two places:**
   - Its namespace combo says "Shared", but RUN-10 calls the default "Session". I used Session.
   - Its cell tag and legend give Ctrl+Shift+Enter as Run Cell. RUN-02 and §9.3 make Ctrl+Shift+Enter Run buffer and Ctrl+Alt+Shift+Enter Run cell.
9. **No storage path in SPEC.** It only says "per-user storage" (A4). I used the example path from research 05, `%LOCALAPPDATA%\UnrealEngine\ScriptEditor\<projectHash>\`.

### 07-snippets-and-shelves
Files:
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\07-snippets-and-shelves.html
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\07-snippets-and-shelves.png (1600x1640, 3 renders, no clipping or overlaps left)

**What the image shows**

The top half is the main window, with the same chrome as 01. Below the toolbar is a new shelf row:
- The shelf tabs are 🔒 Art Tools, CitySample and My Shelf.
- The buttons are Batch Rename, Fix LODs, Validate Naming (with a ⚠ marker) and Reimport.
- At the end is a dashed "Drop to add a button" slot with an insertion bar.

The rest of the window:
- **Snippets side bar.** It has Studio (lock, read-only, `\\studio\...` mirror, synced time), Project (Perforce), User (local), and a built-in Unreal pack. Items sit in categories and show prefix pills. Validate Naming carries a "⚠ Modified" badge.
- **Left editor group.** It holds `texture_fixes.py` (a `ScopedEditorTransaction` that sets `TC_NORMALMAP` and `srgb`, then `save_loaded_asset`). Lines 4–15 are selected, a drag ghost "＋ New shelf button · 12 lines" sits under the cursor on the drop slot, and a dashed trail runs from the selection to the ghost.
- **Right editor group.** It holds a snippet preview with highlighted placeholders: `${1:prefix}` twice (linked), `${2:SM_}`, `${3:...}`, `${TM_SELECTED_TEXT}`, `$0` and a prompt-once `${input:folder}`. Under it are a chip key and the small prompt-once form.
- **Output.** It shows shelf runs #41 (dry run) and #42 (one undo transaction "Script: Batch Rename #42"). The status bar reports "⚠ 1 modified since publish".

Below the window are three panels:
- **A: Save selection as snippet.** Fields are name, category, Tab prefix, description and a parameters table (string, enum, bool). The `"_N"` literal is promoted to a parameter. The scope radio offers User / Project / Studio; Studio is disabled and the dialog says why. Other options are "Add a button to CitySample shelf" and the two-file write note.
- **B: Batch Rename artist form.** It opens from the shelf button with Prefix SM_, Apply to Selected assets (12), Dry run, a one-undo-step note and Run. A toast follows (with "Run without dry run"), plus the Unreal status bar item from WS-07.
- **C: "Modified since publish" prompt.** It is modal over a dimmed shelf. It shows the published and on-disk SHA-256 values and a diff summary. The buttons are Cancel, "Run modified copy once" and "Restore published & run" (the default).

There are 10 callouts, all placed with data-for, and a 10-item legend.

**SPEC.md inconsistencies and UX gaps**
1. SNP-07 says "see SEC-03" for the confirmation, but SEC-03 is "No auto-run features". It should point to SEC-01 (trust prompt) and SEC-02 (hash-pinned libraries).
2. J6 needs the artist form (SNP-05) and hash checks (SNP-07), but both are P2. M4 and 1.0 only promise P1 in SNP, so J6's end-to-end test cannot pass at 1.0.
3. SNP-03 offers Studio scope, but SNP-01 makes the studio tier read-only for users. There is no publish workflow, no role that may publish, and nothing that says who writes the manifest hashes. I drew Studio as disabled with a reason.
4. SNP-02 never defines the syntax for prompt-once variables. `${input:name}` is my assumption; it comes from VS Code tasks and is not valid VS Code snippet syntax.
5. SNP-05 has a type for "actor selection" but none for "asset selection", which Batch Rename's "Apply to: Selected assets" needs. SNP-05 also doesn't say how parameter values reach the code (globals, substitution or function arguments). It doesn't say how they relate to SNP-02 placeholders and prompt-once variables either, which leaves two overlapping parameter mechanisms.
6. "Dry run" could mean a bool parameter declared by the snippet, or the RUN-09 dry-run mode (P2, and dependent on transaction cancel). I drew it as a snippet parameter.
7. RUN-07's name format `Script: <tab> #<run>` doesn't cover shelf runs, which have no tab. I used the snippet name.
8. Shelves only exist inside the code editor tab (SNP-04, WS-02), so a P3 artist has to open a code editor to use them. A standalone Shelf dock tab or a Level Editor toolbar is missing; WS-07 only covers status.
9. SNP-06 doesn't say which tier the shipped Unreal pack belongs to. I drew it as a fourth group, built in and read-only.
10. SNP-07 also covers project manifests. Project files are normally edited locally, so "Modified" would fire all the time for whoever is editing them. The spec needs to say when project hashes are recomputed.
11. Read literally, §9.4 forbids modal dialogs for anything but destructive confirmations and trust prompts. So I drew the Save dialog without a dim backdrop and the artist form as a popover; only the hash prompt is modal.
12. The 01 reference mockup itself disagrees with SPEC:
    - It says "Namespace: Shared", but RUN-10 calls the mode "Session". I used Session.
    - Its cell tag says "Ctrl+Shift+Enter runs this cell", but §9.3 binds that chord to Run whole buffer. Run cell is Ctrl+Alt+Shift+Enter.

### 08-reload-and-environment
Mockup 08 is finished and the render is clean (4 renders). Callouts 1–10 sit on their targets, nothing is clipped, and the doctor's last row clears the status bar.

**Files**
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\08-reload-and-environment.html
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\08-reload-and-environment.png (1600x1385)

**What the image shows**
- **Chrome:** same as 01. The toolbar has Reload Package and an "Entry point (F5): studio_tools.ui.main · reload first" combo (RUN-13, J3).
- **Left side bar:** a Dev root `D:\dev\pipeline\python` with a "Reload on save" badge (RLD-02), holding studio_tools with naming.py selected.
- **Centre, editor:** naming.py just saved (a `NiagaraSystem: "NS_"` line was added). A toast reads "reloaded studio_tools · 4 modules in 184 ms · 2 stale references".
- **Centre, Output:** a numbered "⟳ Reload #6" block (RLD-04):
  - A table in dependency order: naming → importer → ui → the `studio_tools` package, with why each one reloaded, unload hook state (importer's hook was called), and re-import times.
  - A timing line, plus "Never touched: unreal, builtins, C extensions".
  - **First stale warning:** 3 Slate post-tick callbacks still call old `studio_tools.ui` functions. It lists referrers at ui.py:48 and :61, advises adding `__ue_script_editor_unload__()` that calls `unreal.unregister_slate_post_tick_callback`, and has an "Insert unload hook in ui.py" button.
  - **Second stale warning:** the session names `asset_name` and `imp` still point at pre-reload objects. Buttons: "Re-import in REPL" and "Show in Variables".
- **Right, Environment tab:**
  - **Interpreter:** Python 3.11.8, the real UE_5.8 python.exe path, isolation on, Developer Mode on, and the pip venv.
  - **sys.path:** 13 entries with engine, project, plugin, UE_PYTHONPATH and user badges; the watched root is marked.
  - **Packages:** real PythonFoundationPackages versions, a pydantic_core cp312 build flagged as unable to load, and "Export PythonRequirements (1 selected)".
  - **Startup scripts:** last result for each; the StudioPipeline `init_unreal.py` failed with a link to line 7.
  - **Doctor:** 6 passed, 1 warning (Developer Mode, "Fix: turn off"), 1 error (ruff.exe missing, "Fix: restore" and "View log"). The passing checks are the five you asked for: language server binary found, stubs fresh (214 modules, 41.6 MB), journal folder writable, bridge off by policy, and policy loaded from a network path.
- **Status bar:** matches the panels (Ruff missing, reload result, Namespace: Session).

**SPEC.md gaps and UX problems I found**
1. **No home for the Environment panel.** §9.1 lists the right side bar as Variables / Watch / Debugger only, so I added Environment as a fourth tab. ENV-02 also calls Packages a separate "panel"; here it is a section of Environment.
2. **RLD-04 doesn't say where the reload report appears.** It also doesn't say whether reloads are numbered or recorded in History (RUN-12). I showed it as an Output block next to the run headers.
3. **The doctor can pass while the environment is broken.** ENV-04's check list leaves out startup-script failures and package ABI mismatches (ENV-02), so it reports "6 passed" while a startup script has failed. Both should be doctor checks.
4. **RLD-04 asks for stale-reference warnings but gives no fix actions.** "Insert unload hook" and "Re-import in REPL" are my inventions. Stale names in the session namespace (from `from x import y` in the REPL) aren't mentioned at all, though they are the most common stale reference in this workflow.
5. **RLD-02 doesn't define the per-root opt-in control.** I showed it as a badge on the Files root.
6. **01 and SPEC use different names.** 01 says "Namespace: Shared" and "Reload Modules"; SPEC says "Session" (RUN-10) and "Reload package" (RLD-01). I used the SPEC terms. RUN-10 also wants the namespace mode shown on the Run button, which 01 doesn't do.
7. **Possible auto-run conflict.** UEI-06 and SEC-03 say the editor never adds auto-run hooks. Research 04 says the plugin's own `Content/Python/init_unreal.py` runs automatically. If the plugin ships one, it should be listed under Startup scripts.
8. **Bug in the shared callouts.js.** Headless Edge lays the page out about 24 px narrower and then resizes, so markers pinned to right-hand elements land about 24 px too far left. You can see it on callouts 8 and 9 in 01. My page re-pins on resize and on timers; the fix belongs in callouts.js, which I did not edit.
9. **Invented values that the spec doesn't define:**
   - the policy file name `script_editor.json`
   - the ruff.exe path
   - the stub module count and size
   - the 47 MB unreal.py figure
   - `DisableBridge=true` (taken from the SEC-02 example)

### 09-unreal-integration
The mockup for Unreal integration is done. It took three renders and the final one has no clipping or overlaps: every callout is anchored with `data-for`, and no popup is cut off.

**Files**
- `C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\09-unreal-integration.html` (1600x2030; only page-specific CSS plus a small script that draws the dashed drag path; shared files untouched)
- `C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\09-unreal-integration.png`

**What the image shows**
- **Main window (Lvl_Downtown):** the Outliner (3 StaticMeshActors selected), the Content Browser (3 SM_Rock_* meshes selected) and the Python Script Editor docked as a panel tab.
  - A drag ghost with a "3" badge sits over the code. Its label reads "Insert 3 assets as a list of `unreal.load_asset()` calls" and says the format is a setting.
  - The inserted list is tinted, followed by `StaticMeshEditorSubsystem.set_lod_group` inside `ScopedEditorTransaction`.
  - The toolbar has an "Insert selection Ctrl+Shift+V" button.
  - The REPL shows `sel`, `assets`, `world` and `subsys(unreal.EditorActorSubsystem)` with realistic reprs. A Copy as Python actor lookup (`EditorActorSubsystem.get_actor_reference(...)`) is pasted in, then `cliff == sel[2]` returns `True`.
  - The Unreal status bar has the "PY Idle" item.
- **Outliner inset:** the actor context menu, Python ▸ (Copy as Python (3 actors), Run script on selection ▸, Open Python Script Editor, Edit menu scripts…). The Run script submenu lists Fix LODs (project), Snap to floor (studio), Randomize yaw… (user) and Run file on selection…. A tooltip gives the source path, says it receives `sel`, and names the undo transaction "Script: Snap to floor #18".
- **Content Browser inset:** the asset menu with Copy as Python (3 assets) and Scripts ▸ (Fix LODs, Set LOD group…, Validate naming, Reimport from source, Run file on selection…, Edit menu scripts…).
- **Tab-closed inset:** the status item "PY Idle · last run ok ✓".
  - A hover flyout shows the last runs with their undo names and the startup-script result, which is how UEI-06 appears here.
  - Chips show the other states: Running with Stop, Waiting for native call, last run failed.
- **Legend:** 10 numbered items.

**SPEC problems found while designing**
1. **Aliases overwrite user variables.** Refreshing `assets` and the other aliases in the Session namespace on every run would clobber a user variable with the same name. Mockup 01 does exactly that: `assets = unreal.EditorUtilityLibrary.get_selected_asset_data()`. UEI-03 needs a rule, for example keep aliases in a builtins-like scope or never overwrite a name the user assigned. It should also say whether a REPL Enter counts as a run for the refresh; I assumed it does.
2. **No Stop when the tab is closed.** §9.2 blocks editor input outside the tab while a run lasts 250 ms or more, and the run overlay with Stop lives in the tab. For runs from menus or shelves with the tab closed, nothing says where Stop is. I put Stop on the status item; the SPEC should exempt it from the input pre-processor, or define a floating overlay.
3. **WS-07's test cannot run at P1.** Its acceptance test runs a shelf with the tab closed, but shelves (SNP-04) are toolbar rows inside the tab. The other route, Unreal menus (SNP-08), is P2 while WS-07 is P1.
4. **Lookup formats are undefined.**
   - UEI-01 and UEI-02 don't define the actor lookup or Copy as Python expression. I used the full `get_actor_reference(path)`, so it works even when aliases are off. The SPEC should fix the format, including World Partition and level-instance paths.
   - "Insert selection" is ambiguous: J1 inserts the alias `sel`, but the name could also mean inserting explicit expressions.
5. **Menu names differ.**
   - The brief uses a "Python" submenu for actors and "Scripts" for assets, while SNP-08 says "Run script on selection" for both.
   - Unreal already has "Scripted Asset/Actor Actions" submenus, so a separate "Scripts" entry will confuse people. I suggest one "Python" submenu in both places.
6. **Native-wait label differs.** WS-05 says "In native call"; §9.2 and LRS-03 say "Waiting for native call". I used the latter.
7. **Smaller points:**
   - Mockup 01 shows "Namespace: Shared" but RUN-10 calls it "Session"; I used Session.
   - UEI-01 has no per-drop modifier to switch between `load_asset` calls and path strings, only a setting.
   - The brief quotes `load_asset('...')` with single quotes; I followed the SPEC's double quotes.
   - UEI-06's real home is the Environment panel (mockup 08). The status flyout is my proposal and is not specified in WS-07.
   - The §9.5 row for 09 should also mention selection aliases and the global status item.

### 10-settings-and-trust
Rendered `10-settings-and-trust.png` at 1600x1432 in 5 renders. The last one is clean: no clipping, and no callout covers important text. I did not touch `ue.css`, `callouts.js` or `render.ps1`.

**Files**
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\mockups\10-settings-and-trust.html
- C:\Users\anima\Desktop\UnrealPythonEditor\docs\images\10-settings-and-trust.png

**What the image shows**
- **Left: Unreal main window with Editor Preferences docked.**
  - The navigation goes Plugins › Python Script Editor › General, Execution, Keymap, Appearance, Language Services, Studio Policy. Categories that contain locked settings show a padlock.
  - Above the settings are a search box and a small layers strip: Studio policy › Project · DefaultEditor.ini › User · Saved/ + global. Every row has a tag saying which layer its value comes from (policy, project or user).
  - Execution shows a project-set row ("Set by project", with a link to Project Settings) and a policy-locked row (Debug server: padlock, "Set by studio policy", `AllowDebugServer=false`).
  - Keymap has a preset dropdown (Charcoal / Maya (default), also PyCharm and VS Code) and a binding table with 14 SPEC 9.3 chords.
  - The Completion row is marked as a conflict. Its note says Ctrl+Space and Ctrl+Tab are also System-wide Unreal bindings (Open Content Browser Drawer, Tab Navigation), and that the editor binding applies only while the editor has keyboard focus.
  - Below the table are Export keymap…, Import keymap… and Reset to preset. Studio Policy is read-only: trusted roots, AllowUserTrust, DisableBridge, TelemetryMode.
  - Unreal's bottom bar shows Content Drawer and a "Script: Idle" item.
- **Right: the Python Script Editor floating, with the trust prompt open.**
  - `fix_lod_groups.py` is open with a "restricted" tab badge and a banner: "You can edit this file; running it asks first."
  - The prompt shows the full path `D:\Downloads\ue_tools_pack\fix_lod_groups.py`, a shortened SHA-256 with "Copy full hash", the trust zone and file details. It explains Run once and Trust folder, and notes that the run is still one undo transaction.
  - The buttons are Cancel (keyboard focus starts here), Trust folder and Run once.
  - Below the window is a variant for `AllowUserTrust=false`: Trust folder is disabled with a padlock, and a note says to ask the pipeline team to add the folder to the studio trusted roots.
- 10 numbered callouts, each explained in the legend below.

**SPEC problems found while designing**
1. **Ctrl+Tab conflict is not in the SPEC.** In the Charcoal preset, Completion is Ctrl+Space and Ctrl+Tab. The engine source (`GlobalEditorCommonCommands.cpp`) binds both System-wide: Ctrl+Space opens the Content Browser Drawer and Ctrl+Tab is Tab Navigation. CFG-03 only mentions Ctrl+Space. Taking Ctrl+Tab means users lose Unreal's tab switcher exactly when they want to leave the editor. The other presets clash too: PyCharm's Ctrl+B (Go to definition) is Unreal's Browse to Asset, and VS Code's Ctrl+P is Unreal's Open Asset. 9.3 cites `research/07e` and `08-decision-log.md`, but neither file exists yet.
2. **Project vs user precedence is not defined.** CFG-01 lists the layers, and SEC-02 only says policy outranks user settings. "Studio policy > Project > User" can be read as a precedence order. I drew "higher layers win", with project values read-only on this page. If users are meant to override project defaults, the strip and the project row need to change.
3. **Two places to rebind.** The SPEC doesn't say whether the editor's commands also appear on Unreal's own Keyboard Shortcuts page, which has its own conflict handling. If they appear in both places, the two must stay in sync. I added a "Show in Keyboard Shortcuts" link.
4. **The trust prompt's behaviour is not defined.** The SPEC doesn't say:
   - what Run once remembers (this run only, the session, or this hash);
   - whether the hash is of the file on disk or the edited buffer that will actually run;
   - which button has keyboard focus by default;
   - what a file under BlockedRoots looks like (no state is defined);
   - whether the user's trusted folders are per-project or global, and where to manage or revoke them. I assumed a "Your trusted folders · Manage…" row.

   SEC-01's one-time "trust this project" prompt is also not covered by any mockup.
5. **No pre-run state.** The run state model in 9.2 has no step for checks before a run (trust prompt, parse check, save before run), and doesn't say whether a cancelled prompt uses up a run number.
6. **The reference mockup 01 disagrees with the SPEC.** It names the dock tab "Python Editor" instead of "Python Script Editor", and says "Namespace: Shared" where RUN-10 says "Session". Its legend and cell tag say Ctrl+Shift+Enter runs a cell, but in 9.3 that runs the whole buffer and Run cell is Ctrl+Alt+Shift+Enter. My mockup follows the SPEC.
7. **Policy file details come from research, not the SPEC.** The key names AllowUserTrust, AllowDebugServer and TelemetryMode, and the policy file being an `.ini`, are from `research/05`. SEC-02 only names `DisableBridge=true` and never gives the file's name or format.