> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 11 — Mockup QA pass (draft mockups, SPEC v0.9)

> Historical record of the cross-mockup QA pass against SPEC v0.9. Superseded by the v1.0 mockups; see [12](12-mockup-v2-report.md).

# QA report: Python Script Editor mockups 01–10

I checked all ten mockups against SPEC.md and against each other, fixed the problems I found in the HTML, and re-rendered every image. I inspected each changed region again after rendering. I did not touch `ue.css`, `callouts.js` or `render.ps1`.

## Cross-set fixes

- **Product name:** the dock tab now reads "Python Script Editor" everywhere. It said "Python Editor" in 01, 03, 06 (both scenes) and 08.
- **Namespace mode:** "Namespace: Shared" is now "Session" (RUN-10) in 01, 03 and 05. The wording "Shared namespace" / "shared namespace" is fixed in 01 (Variables) and 03 (the REPL header).
- **Reload command:** the toolbar's "Reload Modules" is now "Reload Package" (RLD-01) in 01, 02, 03, 06 (×2), 07 and 09.
- **Unreal status-bar item (WS-07):** it had four different looks. 02 showed "🐍 Python · Run #22", 07 "🐍 Batch Rename · …", 10 "🐍 Script: Idle", 09 "PY Idle · last run ok". All now follow 09's form: a PY badge, then the state, then the detail.
  - 02's running item now reads "Running 41 s · Run #22 … 312 / 1,204" and has a Stop button, as in 09's chips.
  - 02's task item now has the badge too.
- **Journal path:** 08's doctor now shows the same path as 06 and research 05: `%LOCALAPPDATA%\UnrealEngine\ScriptEditor\CitySample-7c41e2\`.
- **Policy file:** 08 said `script_editor.json` and 10 said `ScriptEditorPolicy.ini`. Research 05 calls the policy file an ini, so 08 now uses the `.ini` name.

## Per image

- **01 (reference):**
  - The cell tag and legend 1 now say Ctrl+Alt+Shift+Enter runs a cell and Ctrl+Shift+Enter runs the whole buffer (§9.3).
  - The undo name is "Script: batch_rename.py #14" in both the run header and Variables (RUN-07).
  - The Run #14 header now shows the outcome "error" (OUT-02).
  - The traceback line is now in real Python form: `File "batch_rename.py", line 14, in <module>`.
  - Line 17 was invalid Python (`print("done")  rename_count`). It is now `print(f"renamed {rename_count}")`, with the undefined name squiggled.
  - The docs card was clipped 8 px by the code area. I narrowed it so it fits.
  - Callout 6 covered the end of line 17, so I moved it.
  - The status bar now shows "Namespace: Session" (RUN-10).
  - Legends 1, 2, 7 and 10 are updated to match.
- **02:** I changed the reload label and the status items. A first version of the task item wrapped the inset status bar; I fixed that and re-checked.
- **03:** chrome and naming only. Callout 3 sits in the signature popup's indentation and covers no text.
- **04:** the scratch code used `sel` as its own variable, which clashes with the `sel` alias (UEI-03). It is now `picked`.
- **05:** namespace combo only. The 9.2 Paused row is shown correctly.
- **06, 08, 09:** naming and paths as listed above. 09's API names and selections match one another.
- **07:** reload label and status item. My first, longer status text was clipped by the panel, so I shortened it and re-checked.
- **10:** status item only. Its keymap matches §9.3 exactly.

All `unreal.*` names used across the set are real 5.x API.

## SPEC.md gaps and inconsistencies for the spec author

1. **Names are not settled.**
   - RUN-10 says the namespace mode is shown "on the run button", but no mockup does this. All of them use a toolbar combo plus the status bar. Either drop that clause or design it.
   - The toggle is labelled "Undo as one transaction" in the toolbar but "One undo transaction per run" in Settings.
   - The state names differ between sections: WS-05 uses "Cancelling" / "In native call", while §9.2 uses "Stopping…" / "Waiting for native call…".
2. **The WS-07 status item has no spec.** Nothing defines its icon, its text pattern, or whether it carries Stop. Stop matters most when the tab is closed, because the input pre-processor blocks everything outside the tab, so the item has to be allowed through. The Esc / Ctrl+Break scope also conflicts with CFG-03.
3. **Task mode is missing from the state model.** §9.2 has no state for `@ue_script_editor.task` (LRS-05). J5's "without freezing" only holds in that mode.
4. **Several outcomes are undefined.**
   - Whether a run's transaction is kept or rolled back after an error or a cancel.
   - What outcome and run number a parse failure (RUN-16) gets, and what a run cut off by a crash gets.
   - Whether runtime errors appear in Problems, and when they clear.
   - What F4 walks: "errors in output" (§9.3) or Problems.
5. **The keymap has gaps.**
   - §9.3 has no rows for quick fix (Alt+Enter vs Ctrl+.), Format document, Run to Cursor, Evaluate in frame, or edit breakpoint condition.
   - In the Charcoal preset, F5 means both Continue and RUN-13's entry point, and the context that decides it isn't stated.
   - Ctrl+Tab (and Ctrl+B / Ctrl+P in the other presets) collide with System-wide Unreal bindings. CFG-03 only mentions Ctrl+Space.
6. **Layout (§9.1):** the right side bar lists only Variables / Watch / Debugger. The mockups needed an Environment tab (08) and a stacked debug layout (05, DBG-03).
7. **Settings and paths:**
   - Precedence between project and user settings is undefined.
   - SPEC gives no policy file name or format, and no storage path for journals and history. The mockups now follow research 05.
8. **Conflicts with other requirements.**
   - **Aliases vs user variables:** `sel` / `assets` refreshed on every run will overwrite user variables with the same name (01's code assigns `assets`).
   - **debugpy port:** debugpy on a fixed port 5678 conflicts with SEC-NF-01 ("authenticated").
   - **Watch rule:** VAR-04's "no calls" rule conflicts with the `len(assets)` watch.
   - **Modal dialogs:** §9.4's modal rule versus PER-03 Review and SNP-03.
   - **Wrong cross-reference:** SNP-07 cites SEC-03 where it means SEC-01 / SEC-02.
   - **Priority mismatch:** J6 depends on P2 items, and WS-07 (P1) is tested through shelves or menus that are P1 / P2.
9. **INT-08 lint rules have no codes.** The UE101 / UE103 codes in 03 are invented.
10. **Missing files:** SPEC links TDD.md, `research/08-decision-log.md` and `research/07e`, which don't exist yet.

## Files

All ten mockups were edited and re-rendered. Each image is written to `docs\images\<same name>.png`.
- <repo>\docs\mockups\01-main-window.html
- <repo>\docs\mockups\02-run-and-cancel.html
- <repo>\docs\mockups\03-completion-and-docs.html
- <repo>\docs\mockups\04-traceback-to-source.html
- <repo>\docs\mockups\05-debugger-paused.html
- <repo>\docs\mockups\06-crash-recovery.html
- <repo>\docs\mockups\07-snippets-and-shelves.html
- <repo>\docs\mockups\08-reload-and-environment.html
- <repo>\docs\mockups\09-unreal-integration.html
- <repo>\docs\mockups\10-settings-and-trust.html