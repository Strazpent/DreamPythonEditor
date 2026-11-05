> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 12 — Mockup v2 change report (SPEC v1.0)

> Record of the mockup update to the v1.0 rulings. Its open items are ruled in [08 §9](08-decision-log.md#9-amendments-after-the-v10-review-rv-01--rv-24) (RV-16 to RV-18).

# Mockup v2 change report (Sumi, final decisions)

All ten pages in `docs/mockups/` were updated, re-rendered to `docs/images/<name>.png` and inspected: the full image, plus zoomed crops of dense areas. A DOM checker run on every page found no clipped text, no overlapping text, no off-canvas content and no callouts covering live text. The only items it reports are deliberate: ellipsized tile names behind the Run window in 02, and callouts placed over the dimmed editor behind a modal in 05 and over a tab's × in 06. `render.ps1` and `callouts.js` are unchanged.

## Shared changes (every page)

- **Name.** The dock tab reads **Sumi** and carries a new Sumi mark (a paper disc with an ink "S"). Window and menu text read **Sumi Script Editor** (U1).
- **Run button.** The split button `▶ Run · Session ▾` replaces the Namespace combo (U6). The status bar also shows `Session`.
- **Undo.** The toggle reads **One undo step per run**, and undo names follow `Sumi: <tab> #N` (U3).
- **WS-07 status-bar item.** It now uses the Sumi mark and the U14 patterns. No `PY` badge remains.
- **Bottom and right areas.** The bottom tabs are Output · REPL · Problems. The right sidebar tabs are Variables · Watch · Environment (U25).
- **Buffers and runs.** Untitled buffers are named `Untitled-N`. Every REPL submission is a numbered run (U8). Results print as `# Result: …` (D8).
- **New classes appended to `ue.css`.** No existing rule was changed. The new classes are `.sumi-mark`, `.sumi-item` (with running, task, stopping and error variants), `.tb.splitrun`, `.keycap`, `.modal-backdrop`, `.blocked-cue`, `.modalwin`, `.modaltag`, `.meter`, `.srcb` (runtime, static, keyword, snippet), `.sbanner`.

## Per page

**01 main window (reference).**
- Global changes applied. The Explorer lists journaled Untitled buffers.
- The completion popup uses `static` badges. The docs card reads "Ctrl+Q pins this card · ↗ API reference", and F1 is no longer mentioned.
- Run #14 shows `error`, the undo name and **Undo this run**. The traceback shows the real file path. Run #15 is the REPL run.
- The Variables panel has a Selection aliases section (`sel`, `sel_assets`, `world`, `subsys`, "as of" a time), `_`, and the top of the undo stack with Undo Last Run (Ctrl+Alt+Z, inside Sumi only).
- New Unreal status bar with `Sumi · Idle · last run ok 0.01 s`. The legend now has 12 items.

**02 run and cancel (redesign).**
- The main scene is the Run window: modal and positioned over the Sumi output area. It shows `Running 41 s · Run #22`, the line at the last pump, progress `312 / 1,204 · 28/s · ETA 32 s`, an output tail, the meter `Undo record 96 MB of 256 MB`, Stop with "Esc or Shift+F5", and "Viewports resume when the run ends".
- The editor behind is hatched and carries an "Input blocked during the run · last frame shown" cue. Its status bar and WS-07 item are frozen at their first-pump text (`Running 0.3 s`).
- Panel (a): Stopping…, then the cancelled block "Run #22 · cancelled at batch_reimport.py:31 · 0.6 s after Stop" with Undo this run. I used the scene's own file, `batch_reimport.py`; the brief said `batch_rename.py`.
- Panel (b): the native-call note ("The last native call took 12.4 s; Stop took effect when it returned").
- Panel (c): Interrupt cleanup after 10 s (RT-06).
- Panel (d): task mode with `Sumi · Task running · 40 / 300 [Stop]`, a live viewport and `Sumi: task #31`.
- Panel (e): the Live (docked) — Preview banner inset.

**03 completion and docs.**
- The Ctrl+Space popup shows `runtime`, `static` and `snippet` badges. The "project" badge is gone.
- New pane B: editor-property name completion inside `set_editor_property("cast_…")`, with a signature strip. The names and signature were checked in the engine source.
- Pane C: the Ctrl+Q quick-docs card.
- Pane D: SUM102 quick fix on Ctrl+., and Problems entries SUM102, SUM301 and SUM202 next to F401. F8 navigation.

**04 traceback to source.**
- Frames use `"<sumi:Untitled-3#a41f>", line 18`. The 3.11 carets sit under `presets[name]`.
- F4 and Shift+F4 walk frames. F8 walks Problems, which was F4 before.
- A `runtime` Problems item points at importer.py:88 and clears per U13.
- Quick fix Ctrl+. on SUM102 (`mesh.lod_group = …`).
- The syntax-error run is `#32 · not executed`.

**05 debugger paused (redesign).**
- The modal Paused window is titled "Paused — batch_rename.py:27 (breakpoint)". Its toolbar has Continue F5, Step over F10, Step into F11, Step out Shift+F11 and Stop Shift+F5.
- The window holds the call stack, lazy UObject-aware variables ("read with get_editor_property on expand"), watches split into safe and manual (manual watches have a ⟳ refresh), an evaluate console with "May modify editor state — changes join Run #14's undo step", and a read-only source view.
- The footer notes that pauses inside Slate tick callbacks are refused. The editor behind is dimmed with an input-blocked cue.

**06 crash recovery.**
- Non-modal banner "Recovered 3 buffers…" with Restore all, Review and Discard.
- The Review panel is docked at the right, not floating. It has the conflict row, the note "Discard moves the journals into local history", a journal-location box (`%LOCALAPPDATA%\Sumi\5.8\CitySample-7c41e2\`) and a safe-mode box.
- The conflict opens `batch_rename.py (recovered)` beside the disk file, with a diff. Recovered-only text is blue and disk-only text is green.
- The run cut off by the crash shows the outcome `lost`.
- Local history retention is now 30 days and 500 MB (D17).

**07 snippets and shelves.**
- Body placeholders are `${param:folder}` and `${param:prefix}`, with the item's `params` block shown below.
- Save as snippet is non-modal, with Publish to library across the user, project and studio tiers. Studio publishing without write access produces the change for the library owner (U33).
- The non-modal parameter form shows the asset selection count with Pick again (U32).
- New standalone **Sumi Shelf** tab (U27).
- The studio item's "Modified since publish" prompt is non-modal; the dimmer was removed.
- The project tier moved to `<Project>/Sumi/Library/`. The built-in "Unreal pack" tier was removed.

**08 reload and environment.**
- The reload report is a numbered output block. Its stale Session names list (`asset_name`, `imp`) has **Reset session** and **Show holders**. Unload hooks are `__sumi_unload__()`. The never-touched list follows D18.
- The entry point runs on Ctrl+F5.
- The doctor runs the U36 checks: interpreter, sys.path shadowing, startup-script failure, ABI tag, Developer and restrictive mode, binary hashes, stubs, journal folder, and the effective policy with the three `Policy.json` layers.
- New note "Sumi ships no init_unreal.py". The bridge row was removed, because there is no bridge in 1.0.

**09 Unreal integration.**
- D30 formats: `load_asset("/Game/…/SM_Rock_Large.SM_Rock_Large")`; Alt inserts the soft path string; actors become `find_object(None, "…")  # label`; a Ctrl+Shift+V tooltip shows the U20 line; new Details inset with `obj.set_editor_property("cast_shadow", True)` and the getter.
- One **Sumi** submenu in the actor and asset menus: Copy as Python, Insert into Sumi, Run on selection ▸ (U28).
- Tools ▸ Sumi Script Editor and Sumi Shelf.
- The WS-07 chips follow U14: Running, Task running, Run #23 error, Idle. The "Waiting for native call" chip and the flyout were removed.

**10 settings and trust.**
- The precedence bar reads Machine › Studio › Project policy › User › Project default › Built-in.
- An effective-value inspector on `run.presentation` lists every layer with its file (U38, RT-18).
- The keymap follows D8 and U21–U22, with a "Shadows in Unreal" column.
- New pages: Shadowed Unreal shortcuts (U23, U24) and Settings › Trusted folders.
- The trust prompt is now the U39 inline banner with no default focus. The refused run is Run #15 `not executed`.

## SPEC wording the spec author should mirror

1. **WS-02 / §9.1 layout:** "Bottom area tabs: Output, REPL, Problems. The REPL prompt line also sits under Output (REPL-01); the REPL tab holds the REPL transcript and history. Right sidebar: Variables, Watch, Environment." The History tab is gone.
2. **WS-05:** "Status bar shows the run state (Idle, Starting, Running *n* s, Stopping…, Paused, Task running), the namespace mode (Session or Isolated), …". Remove "Cancelling" and "In native call".
3. **LRS / Run window:** "Proposed: on the first pump, before opening the Run window, Sumi shows a dim overlay with an 'input blocked' cue and runs one full Slate tick. After that only the modal repaints (`Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:1476-1497`), so the editor shows that frame until the run ends." This is a proposal. It is not in 08.
4. **WS-07:** add "During a Run-window run the item keeps its first-pump text; the Run window is the live view (U14)."
5. **RUN-16 / U7:** "A trust or parse failure ends the run in `Starting` with the outcome `not executed`; the WS-07 item reads `Sumi · Run #N not executed`."
6. **UEI (D30):** "Copy as Python on a Details property row is added to `PropertyEditor.RowContextMenu` through UToolMenus." I verified this myself: `Engine/Source/Editor/PropertyEditor/Public/PropertyEditorModule.h:30` defines the menu name, and `Engine/Source/Editor/PropertyEditor/Private/PropertyEditorModule.cpp:773-784` registers it with the Expansion and Edit sections.
7. **INT-08 / SUM102 rationale:** "Attribute assignment on a wrapped UObject sets the property with change-notify mode Never; `set_editor_property` uses Default, so the editor's pre- and post-change notifications fire." I verified this myself: `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyWrapperObject.h:52` sets the default `InNotifyMode = EPropertyAccessChangeNotifyMode::Never`, and the `set_editor_property` docstring is at `.../Private/PyWrapperObject.cpp:1133`.
8. **Editor-property completion (INT-02):** inside `set_editor_property('…')` and `get_editor_property('…')`, items carry the `runtime` badge (the class's Editor Properties docstring list), and signature help stays visible above the line.
9. **SNP:** the project tier lives at `<Project>/Sumi/Library/`, one item per file, with shelves in `Sumi/Library/shelves/*.json`. "Run on selection ▸" lists shelf items whose parameters accept the current selection type.
10. **Settings (CFG):** add pages "Shadowed Unreal shortcuts" and "Trusted folders". The inspector opens from a "Why this value?" link on each row.
11. **Policy key names:** the mockups use dotted names such as `run.presentation`, `debug.allowDebugpy`, `trust.roots`, `trust.allowUserTrust` and `languageServer.allowed`. These are proposed; 08 fixes only the file name and the paths.
12. **Keymap table (§9.3):** add Run to cursor Ctrl+F10, Evaluate in frame Alt+F8, Edit condition Shift+F9, Quick fix Ctrl+., Format Shift+Alt+F, Next/previous problem F8/Shift+F8, Entry point Ctrl+F5, and Undo Last Run Ctrl+Alt+Z (inside Sumi only). The shadowed Unreal commands to name are Ctrl+H Show All, Ctrl+Shift+F Open Content Browser and Ctrl+G Regroup actors (07e Q2).
