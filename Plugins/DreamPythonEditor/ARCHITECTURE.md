# Dream Python Editor — slice A, slice B, PO-04 restyle and PO-05 side bar architecture

Slice A of M1: the user-visible loop of TDD §15.2 with the TDD's architecture and names (build brief items 1–18).
This file is the contract between the seven implementer slices. It names, for every slice-A item, the TDD sections it
implements, the files that implement it and the slice that owns each file; the run sequence; the threading rules; and
every place where slice A chose a detail the TDD leaves open (**slice A choice**, also marked in the headers).

Design: `docs/TDD.md` v1.2 (the *how*), `docs/SPEC.md` (the *what*), `docs/research/08-decision-log.md`.
Engine: UE 5.8.4 at `C:/Program Files/Epic Games/UE_5.8` (read-only). Python 3.11.8.

Integration: after the seven slices, the contract requests between them (CR-01 to CR-36) were applied in four
ownership groups (core, Python and docs; run; output; shell). The choices that came out of that phase are A44 to A55
in section 7, rulings R1 to R3 among them; sections 1, 5, 6 and 8 are corrected to match what the slices built.
The review rounds after integration added A56 to A64. `STATUS.md` has the build and test steps, the latest results,
the review rounds, the known risks, the deferrals, the open items and the owner decisions of both slices.

Slice B of M1 builds the slice-B rows of the slice-A status (B01 to B108, section 5) and moves the rest out with a
reason (section 9). Ten implementer groups built it, each owning its files (section 1): core, run, lang, persist,
instances, codeview, buffers, completion, output and tests. The groups were stopped by an interruption on the first
evening; at 00:10 the next day replacement implementers with the same group names continued from the stopped ones'
handover (a brief, a digest of the work so far and the list of files changed since slice A), verified what was there
and finished it. The architect then had the exact diffs reviewed in rounds 1 to 5: round 1 on the whole slice-B diff
against the slice-A tree, each later round only on the previous round's fix patch. Round 4 found no major, which ended
the loop; round 5 checked the small patch of minor fixes made after it (`STATUS.md` has the rounds and the integration
results). Slice B's choices, from the groups' B-notes and
the architect's rulings, are A65 to A177 in section 7; the slice-A rows they change are marked "(slice B: …)" there.
Sections 1, 3, 4, 6 and 8 add what slice B made explicit; section 2 keeps slice A's run sequence and lists the slice-B
additions under it.

The PO-04 restyle (owner ruling of 2026-11-03; it outranks SPEC and TDD UI rows, PO > RV > RT > U > ADRs) gives Dream
the clean look of Maya's Python script editor instead of a C++/IDE code editor: Maya-minimal chrome, Maya-style history
and ten themes with Darcula as the default. Features stay; only presentation changed, and every hidden part is one View
toggle away. Three groups built it on the final slice-B tree under slice B's rules (section 1: own files, touch points
marked `(theme)` or `(chrome)`, the UE lock, 0 warnings): theme, chrome and output. The run group changed the Run
window's teardown activation, the tests group added the activation at test start and a pass-level settings guard, and
codeview changed one spec. The architect had the exact diffs reviewed in rounds r6 to r8 (r6 on the restyle patch, each
later round on the previous round's fix patch); r8 found no major, which ended the loop. Round r9 checked the post-loop
patch of minor fixes, the integration ran on that tree, round r10 checked the patch after it (no major), a final
verification passed (attended 286/286, every click case included), and round r11 checks the last minors (`STATUS.md`
has the rounds and the results). The restyle's choices, from the groups' R-notes and the rulings PO-04 and PO-04-R1,
are A178 to A215 in section 7, the rows PO-04 overrides among them; A216 is the architect's ruling after integration
(Stop acts on the press), and A217 to A225 are the choices of rounds r7 to r10. The slice-A and slice-B rows the restyle
changes are marked "(PO-04: …)" there. Section 5 lists the restyle items, section 6 the restyle contracts and section 8
the new specs.

PO-05 (owner ruling of 2026-11-04, with amendment PO-05-A1 the same day) adds a left side bar, shown by default, that
holds a Project tree above a Structure section:
- **Project tree:** configurable roots per project; the project's `Content/Python`; the `Python` folder of every
  mounted plugin and of the engine; a Scratches section.
- **Structure:** the active tab's classes, functions, methods and cells.

Every new unnamed tab is an auto-saved `scratch_N.py` in a per-user scratch folder. Sample scripts are seeded into
`Scratches/Samples`, and the open tabs come back on start (PER-02, verified across real restarts).

Three groups built it under the same rules (section 1: own files, touch points marked `(sidebar)`, `(structure)` or
`(scratch)`, the UE lock, 0 warnings): sidebar, structure and scratch. The architect had the exact diffs reviewed in
rounds p1 to p3:
- **p1** on the PO-05 patch against the last reviewed restyle tree;
- each later round on the previous round's fix patch, with PO-05-A1 reviewed as new code in p3;
- **p3** found no major, and the architect checked the last small fix patch.

PO-05's choices are A227 to A245 in section 7. Section 5 lists the PO-05 items, section 6 the PO-05 contracts and
section 8 the new specs and switches. A226 (the owner dropped the `>>>` / `...` echo prompts) came just before PO-05.

PO-06 phase 3 renamed the product from "Sumi" to **Dream Python Editor** ("Dream" where space is tight), and this file
uses the new names throughout: the plugin `Plugins/DreamPythonEditor`, the modules DreamCore, DreamEditor and
DreamEditorTests, the `Dream` class prefixes, `dream::`, `dream_editor`, `_dream`, `LogDream`, `Dream.*` tests and
commands, `-Dream*` switches, `%LOCALAPPDATA%\DreamPythonEditor` and the host project DreamHost. The older choices
below are the same decisions under the new names. A246 is the rename and the one-time copy of a user's Sumi data.

## 1. Slices and the rules between them

Slice A ownership (the slice-A history; the slice-B table below replaces it for every file it names):

| Slice | Owns |
|---|---|
| **core** | `Source/DreamCore` (all of `Private/`, the `Public/dream/` headers after hand-off), `Tests/DreamCore` tier 1, `Tools/Python/dream_tools/gen_unicode_tables.py` |
| **python** | `Content/Python/dream_editor` (every file, `_impl/native.py` included: its `_dream` surface changes only in one commit with `Python/DreamNativeModule.h`, which bridge owns), `Tests/python` tier 2 and its runner `Tests/run_tier2.bat` (CR-36: the python slice wrote it) |
| **bridge** | `Private/Python` (FDreamPython lifecycle, `_dream`, GIL helpers), `Private/Compat`, `Private/Run`: FDreamRunService, FDreamTransactions, FDreamDeferral, DreamRunTypes |
| **supervisor** | `Private/Run`: FRunSupervisor (+ watchdog), FDreamRunProtections, SDreamRunWindow, cancellation; `Private/Commands/DreamInputPreProcessor` |
| **output** | `Private/Output` (store, ingest, run logs, forwarder, device, console executor, REPL history, runtime problems); `Private/Shell`: SDreamOutputArea, SDreamOutputView, SDreamReplStrip |
| **shell** | `Private/Module`; `Private/Shell` tab layout, toolbar, status strip, WS-07 item, menus, banners, style; `Private/CodeView` (plain mode, Python marshaller); `Private/Commands` FDreamCommands, FDreamKeyRouter and FDreamNumpadEnter; `Private/Buffers`; `Private/Persistence`; `Private/Settings`; `DreamPythonEditor.uplugin`, `Source/DreamEditor/DreamEditor.Build.cs` |
| **tests** | `Source/DreamEditorTests` (tier-4 specs, `_dream_test` fixture), host-project scripts under `DreamHost/`, `Tests/run_tier1.bat`, `run_tier4.bat`, `run_build.bat`, `Tests/ci/` scans |
| **architect** | this file and `STATUS.md` (the slice-B deferrals the brief asks for, assembled from each slice's report; slices send their `// Slice B:` lists, they do not edit it) |

Slice B ownership (paths under `Source/DreamEditor/Private/` unless they say otherwise; a group edits another group's
file only at the touch points in its row, rule 10):

| Group | Owns | Touch points in other groups' files |
|---|---|---|
| **core** | `Source/DreamCore/**` and `Tests/DreamCore/**`, except the persist, instances, lang and completion files below | — |
| **persist** | `Persistence/**` (new `DreamLocalHistory.*`, `DreamHistoryTypes.h`, `DreamRestoreGuard.*`), `Shell/SDreamRecoveryBanner.*`, new `Shell/SDreamRecoveryReview.*` and `Shell/SDreamTabRecovery.cpp`; DreamCore `dream/JournalFormat.h`, `Private/JournalFormat.cpp`, new `dream/HistoryFormat.h` + cpp, `Tests/DreamCore/test_journal.cpp`, new `test_history.cpp`; specs `Dream.Persistence.*` | `Module/DreamEditorModule.*` (services, accessors, pre-exit), `Shell/SDreamTab.*` and `SDreamTabImpl.h` (layout hosts, RestoreSession, safe mode, banners), `Settings/*` |
| **instances** (after persist) | persist's files above, plus new `Persistence/DreamInstances.*`, `DreamStorageProbe.*`, `DreamStorageRoot.h`, `Persistence/DreamStorageRoot.cpp`, `Output/DreamReplHistory.*`; DreamCore new `dream/StorageIndex.h` + cpp + `test_storage.cpp`; `DreamHost/Scripts/run_g6.ps1`; specs `DreamInstancesSpec.cpp`, `DreamG6DriverSpec.cpp` | module, `SDreamTab.*`, `Settings/DreamUserSettings.h`, `Shell/DreamOutputRows.cpp` (B23), `DreamEditor.Build.cs` (the `Settings` include path), `JournalFormat.h/.cpp` (one marked line, review round 2) |
| **run** | `Content/Python/dream_editor/**` except `_impl/docstrings.py`, `_impl/stubs.py`, `_impl/resolver.py`; `Tests/python/**` except the lang and completion tests; `Python/**`, `Compat/**`, `Run/**`, `Commands/DreamInputPreProcessor.*`, new `Diagnostics/**` (audit, crash context, trace); `CHANGELOG.md`, `Tests/python/api/` | `Shell/SDreamStatusStrip.cpp` and `SDreamStatusItem.cpp` (B32), `Shell/DreamOutputRows.cpp` (B38), `Shell/SDreamTab.cpp` and `SDreamTabToolbar.cpp` (B40), `Buffers/DreamBuffer.h` (B40 flag), module, `Settings/*`, `DreamEditor.Build.cs` (TraceLog) |
| **lang** | `Tools/Python/**` (new `dream_launch.py`, `dream_tools/check/**`), `_impl/docstrings.py`, `_impl/stubs.py`, their tier-2 tests and `Tests/python/check_fixtures/`; DreamCore new `dream/JsonRpc.h`, `dream/LspClient.h` + cpps + `test_jsonrpc.cpp`; new `Language/**`; `Shell/SDreamProblemsView.*`, `Output/DreamRuntimeProblems.*`; new `Python/DreamPythonSnapshot.*` (in run's folder); specs `Dream.Language.*` | `Python/DreamPythonEntry.cpp` (OnDeferredStart), `Python/DreamPython.cpp` (HandleShutdown step 3), `Run/DreamRunProtections.cpp` (SetRunActive), `Commands/DreamCommands.*` (F8, Shift+F8, Ctrl+.), `Shell/SDreamTab.cpp` (dispatch), `Shell/SDreamOutputArea.cpp` (Problems count), module |
| **codeview** | `CodeView/**` except completion's popup, `Commands/DreamCommands.*`, `Commands/DreamKeyRouter.*`, `Commands/DreamNumpadEnter.*`, `Shell/DreamEditorStyle.*`; specs `Dream.CodeView.*` | `Shell/SDreamTab.cpp` (`// codeview:` blocks in `CanExecuteDreamCommand`/`ExecuteDreamCommand`, find-bar and Go to line host), `Settings/DreamUserSettings.*` |
| **buffers** | `Buffers/**` (new `DreamFileWatch.*`), `Shell/SDreamTab.*`, `SDreamTabImpl.h`, new `SDreamTabFiles.cpp`, `SDreamTabToolbar.cpp`, `SDreamEditorTabs.*`, `SDreamStatusStrip.*`, `SDreamStatusItem.*`, `Module/**`, `Settings/**`, `DreamEditor.Build.cs`, the uplugin; specs `Dream.Buffers.*` | `Shell/DreamOutputRows.cpp` (B86), `Tests/ci/loctext_baseline.json` (its one entry) |
| **completion** | `_impl/resolver.py` + `Tests/python/test_resolver.py`; DreamCore new `dream/CompletionMerge.h` + cpp + `test_completion.cpp`; `Output/DreamConsoleExecutor.*`; new `CodeView/SDreamCompletionPopup.*`, `Completion/**`, `Python/DreamPythonCompletion.cpp`; spec `Dream.Completion` | `Python/DreamPython.h/.cpp` (resolver declarations, step-3 comment), module, `Commands/DreamCommands.*` (Complete), `CodeView/SDreamCodeView.h/.cpp` (`(completion)` hooks, `OnKeyUp` included) |
| **output** | `Output/**` except the files named above, `Shell/SDreamOutputArea.*`, `SDreamOutputView.*`, `DreamOutputRows.*`, `SDreamReplStrip.*`; specs `Dream.Output.*` (new ones), `Dream.Repl.SameFrameKeys` | — (run's `DreamNativeModule.cpp` passes the thread state, B90) |
| **tests** | `Source/DreamEditorTests/**` except other groups' spec files, `DreamHost/Scripts/**` except `run_g6.ps1`, `Tests/ci/**`, `Tests/run_*.bat` | `Run/RunSupervisor.h/.cpp` (`PostPendingCallForTest`, marked `(tests)`) |
| **architect** | this file, `STATUS.md`, the slice-B plan (outside the repo); integration and the review loop | — |

Rules:
1. **Write only your own files.** A shared header (section 4) belongs to the slice in its "Owner" column. To change a
   signature another slice calls, change the header and every caller in one commit, or ask the owner. Adding members
   that no one else calls needs no coordination.
2. Every `.cpp` includes its own header first, then what it uses. Paths are relative to `Private/` (`"Run/DreamRunService.h"`)
   and to `Public/` for DreamCore (`"dream/TextBuffer.h"`).
3. **Python.h** only through `Python/DreamPythonInclude.h`, and only in `Private/Python`, `Private/Compat` and
   `Private/Run/RunSupervisor*.cpp` (§2.5 rule 6). Other headers that must name CPython types use `Python/DreamPythonFwd.h`.
4. **DreamCore** includes only the standard library and `dream/…` (§2.5 rule 1); no exceptions, no RTTI, no I/O, no threads,
   no mutable globals; `DREAMCORE_API` on exported classes and functions. It compiles standalone: `Tests/DreamCore/build.bat`.
5. Unreal coding standard; `LOCTEXT_NAMESPACE "Dream"`; `LogDream` for Dream's own lines (`Module/DreamLog.h`); U2 state
   names from `DreamRunText::StateName` and outcomes from `DreamRunText::OutcomeName`, never from enum names.
6. **`DREAMEDITOR_API`** on every class with out-of-line members and every free function in a shared header (slice A
   choice): the tests module includes DreamEditor's `Private/` headers and links those exports. Not on header-only types
   (`IDreamCommandTarget`, `FDreamScopedGil`, plain structs), on `thread_local` data (MSVC C2492) or on USTRUCTs/UENUMs.
7. Every source file starts with `// SPDX-License-Identifier: Apache-2.0` (`#` in `.py`, `rem` in `.bat`; §2.5 rule 2,
   T-ARCH-003). No Epic code is copied.
8. No `TODO` that hides missing behaviour. Slice A wrote `// Slice B: …` where a TDD feature was deferred; slice B
   removed each marker it built and reworded the rest to `// Deferred (<M2|M3|M4|1.1|G1|G3>): …` (`# Deferred (…)` in
   Python), where G1 means "needs the SCodeView/G1 spike" and G3 "needs the G3 harness" (section 9). No `Slice B:`
   marker is left. The one marker outside that list is `Deferred (M1, needs the OFL font files)` in
   `Shell/DreamEditorStyle.*` (A38).
9. Line endings: LF, except `.bat` files (CRLF).
10. **Touch points** (slice B). A group edits a file another group owns only at the touch points of its section-1 row,
    as small edits marked with its group name (`// codeview:`, `(lang)`, `(persist)`, `(instances)`, `(completion)`,
    `(tests)`, `(run group)`), re-reading the file right before each edit. An API another group waits for is posted
    with its header and signature when it compiles; a change to a signature others call changes every caller at once.
11. **The UE lock** (slice B). A full UE build, any tier-4 pass, `run_smoke.ps1`, the restart and G6 drivers and any
    DreamHost editor launch run under one shared lock, because a running editor locks the plugin DLLs and every editor
    shares `DreamHost/Saved` and the Dream storage root. The lock is held for one build-and-test cycle only; a
    `run_build.bat -SingleFile=<cpp>` compile needs none. Only `UnrealEditor*` processes whose command line names `DreamHost.uproject` may
    be killed, and only by the lock holder. The lock is a working tool outside the repository.
12. **Tier 1 builds in a private output folder** (`DREAM_TIER1_OUT`, section 8) whenever more than one build can run, so
    builds do not overwrite each other's objects.
13. **No sleeping file I/O on the game thread** (slice B ruling). The `FDreamWin32FileOps` retries that wait out a held
    file (`WithRetries`, about 0.9 s at most per call) run on worker threads; game-thread callers (the restore guard)
    use `FDreamWin32FileOps::FScopedNoRetryWait`, and a history slice on the writer thread waits through
    `FScopedRetryWait`, which serves due commits meanwhile (A101, A106).
14. **Every asynchronous result applies on the game thread through `FDreamDeferral`** (section 3), with a generation or
    version check where the result can go stale.

## 2. The run sequence

```text
 Slate input handler (Ctrl+Enter in SDreamCodeView)                                   GT, no Python (D14(1))
   SDreamCodeView::HandleTextKeyDown -> FDreamKeyRouter::RoutePre -> SDreamTab::ExecuteDreamCommand
   SDreamTab::SubmitBufferRun: dream::StatementExtent / selection -> FDreamRunRequest{Text, FirstLine, SourceSnapshot,
       Filename = FDreamBufferManager::GetCompileFilename}
   FDreamRunService::Submit ........ number #N, record State=Starting, FDreamOutputStore::BeginRunBlock(N),
                                    FDreamRuntimeProblems::OnRunStarted, ArmEndFrame; Python Cold -> RequestStart
   <- FDreamSubmitResult{accepted, N}; Slate paints "Starting" (the core ticker runs normally)

 FCoreDelegates::OnEndFrame (one-shot, FDreamRunService::HandleEndFrame)               GT, IsInEndFrameStart()
   FDreamPython::ProcessEndFrame ... boot (_boot via ExecPythonCommandEx), Enable Python, Reset Session, decrefs
   RT-02 trio (saving, GC, PostLoad) -> re-arm next frame
   Starting steps (§5.1.2), one step per frame at most:
     Prepare ........ Id = (generation << 32) | ++EntrySerial; FDreamPython::Prepare -> runner.prepare(Id, request)
                      SyntaxError -> not executed (C++ formats the error block, caret moves via OnRunFinished)
     AwaitSnapshot .. FDreamJournal::SnapshotForRun -> ticket; poll IsDurable; 2 s -> not executed
     AwaitSave ...... Run File of a dirty buffer: FDreamBufferManager::SaveAsync
     Stop during these steps -> not executed "stopped before user code started"; runner.discard(Id)
     Enter .......... FDreamTransactions::BeginRunTransaction (route default, per-tab toggle, U6 override)
                      FRunSupervisor::BeginRun(Id, FDreamRunPolicy{output-area rect, filename, OnPump})
                      FDreamOutputStore::BindRunId(Id, N); FDreamOutputDevice::SetRunActive(true); State=Running
                      FDreamPython::EnterRun(Id) == ExecPythonCommandEx(EvaluateStatement, Flags None,
                                                   "__import__('dream_editor')._run(<Id>)")
 inside _run (Python, GT):
   runner.run: prep = _PREPARED.pop(Id); swaps install (Writers, displayhook, unreal.log shims, showwarning,
     time.sleep, dunders, sys.path); _dream.capture_begin(Id, alive); _dream.user_code_begin(Id)
   for each statement: _dream.run_statement(Id, k, code, ns)   C: Stop check -> started k -> echo lines -> exec -> returned k+1
   _dream.user_code_done(Id)  (first statement of the handler, RT-06); classify; swaps restore
   notes as STREAM_NOTICE lines; _dream.report_outcome(Id, outcome, detail, location); _dream.capture_end(Id)
 meanwhile (watchdog thread):  armed ~300 ms -> Py_AddPendingCall(Id) -> GIL hand-off on its own PyThreadState
   FRunSupervisor::OnPendingCall (GT, GIL held): run-ID guard, stats, drain output 2 ms, guards (§5.4.3),
   cancellation (§5.7.3), Policy.OnPump(Live) (size watch, undo meter), pump (GIL released: first pump installs FDreamRunProtections and
   opens SDreamRunWindow before PumpMessages; PumpMessages, Tick(All), guarded RHI EndFrame, Renderer->Sync), post-pump delivery
 ExecPythonCommandEx returns -> FinishRun (§5.2.4):
   1 FRunSupervisor::EndRun(Id, Stats)    2 FDreamTransactions::EndRunTransaction
   3 FRunSupervisor::EndPresentation (protections off, live-view hand-off, Run window teardown)
   4 outcome (internal error when bOk false or no report), footer, FDreamOutputStore::EndRunBlock(N),
     FDreamJournal::MarkRunEnded, FDreamRuntimeProblems::AddRuntimeError, history
   5 State=Idle, OnRunFinished (SDreamTab moves the caret on a SyntaxError, WS-07 repaints)
```

Run IDs: `FDreamRunId = (Python generation << 32) | EntrySerial`, allocated at the Prepare step (one per entry into
`_run`). Run numbers: per editor session from 1, assigned at Submit, so `not executed` runs are numbered (U7, U8).

Slice-B additions to the sequence above:
- **Submit / Starting.** The completion popup closes when a run enters Starting (A151). While the run waits for an
  idle end of frame (Queued, Prepare, Enter) for more than 1 s, the status strip and WS-07 say "Starting… waiting for
  the editor to become idle" (B32, A82).
- **Prepare.** The runner writes the selection aliases under U19 and the session epoch moves when it writes one (B35,
  A84).
- **AwaitSnapshot.** `SnapshotForRun` also carries every other buffer's pending edits (A120); local history adds the
  run's `run` entry once that commit is durable (A99).
- **Enter / Running.** `FDreamAudit::RunStarted` and the crash-context run keys are set at Running (A78, A80); the run
  protections call `FDreamLanguageServices::SetRunActive(true)` (B55).
- **Pending call.** The fast chain re-delivers without the 250 ms gap only while no exception at all is being handled
  (ruling, A76).
- **Finish.** `FDreamAudit::RunEnded` (not executed runs included), the crash-context run keys cleared, a not executed
  run with a location becomes a Parse item in Problems (A94), and the file watch re-stats every open file (A136). Every
  result that waited for the run (find, file checks, LSP diagnostics, history results, alias refreshes) applies at the
  next idle tick through `FDreamDeferral`.

## 3. Threading rules (TDD §3.1, §3.2)

| Thread | May call | Never |
|---|---|---|
| **Game thread** | everything not listed below; all UI; `FDreamRunService`, `FDreamTransactions`, buffers, journal snapshots, `FDreamOutputStore::Drain`/queries, every helper call (only from the OnEndFrame handler or `FDreamDeferral` idle ticks, INV-6) | Python from a Slate handler (D14(5)); waiting on the watchdog while holding the GIL (GIL-6); `FTSTicker::Tick`, `GEngine->Tick`, `EnterDebuggingMode`, `PyErr_SetInterrupt`, `EPythonCommandFlags::Unattended` (§2.5 rule 8) |
| **Watchdog** (`DreamRunWatchdog`) | supervisor atomics, `FDreamPyCompat::AddPendingCall`, the GIL hand-off and `SetAsyncExc` on its own thread state inside the `bInPython` bracket | Slate, UObjects, the output store, any other Python |
| **DreamOutput** (ingest) | `dream::LineSplitter`, `ScanLinks`, `FDreamLogForwarder::Forward`, run-log appends, the view queue | Python, Slate, UObjects |
| **DreamJournalWriter** (slice B: `TPri_Normal`, A105) | `dream::journal::Writer`/`ReadNamespace`/`BatchPlanner` through the Win64 `FileOps`; commits first, then on its logical low-priority lane the sliced rotation steps and local history (blobs, index commits and rotations, retention, Clear, merges), one unit per loop after every due commit (A100, A101) | Python, Slate, UObjects |
| **DreamAudit** (slice B, `TPri_BelowNormal`) | the audit queue (10,000 lines), `<root>/audit/*.jsonl`, the sweep; the dirty-buffer hash of a run (A79) | Python, Slate, UObjects |
| **DreamStorageProbe** (slice B, `TPri_Normal`) | the override probe at engine init (folders, `owner.json`, writability, 2 s bound), `projects.json` updates, the cross-root orphan scan and deletes, wal sizes (A117) | Python, Slate, UObjects; the session's own namespace |
| **Dream.Lsp.`<id>`.Read / .Write** (slice B, `TPri_BelowNormal`, one pair per server) | its pipes and the job object, `dream::JsonRpc` framing, the stderr log; commands MPSC in, events SPSC out (A91) | Python, Slate, UObjects |
| **`UE::Tasks` workers** (slice B) | snapshots and shared state captured by value, never `this`: the find worker (code-view regex search and Replace All, output-view Find; `dream::CancelToken`), the compaction worker (`BuildCompacted`), the file-watch checks (stat, hash, decode), the save worker, the snapshot-key task | Python, Slate, UObjects |
| **`DreamStubSnapshot` pipe** (slice B, `UE::Tasks::FPipe`) | the Stage 1 snapshot files: `.jsonl.tmp`, rename, `progress.json`, `manifest.json`, `.complete` (A96) | Python, Slate, UObjects |
| **Any thread** | `FDreamOutputStore::Push*`/`SetThreadLabel`, `FDreamOutputDevice::Serialize`, `FRunSupervisor::RequestStop` (its atomics; the `OnUserCancel` call and the live-stats update run only on the GT and are deferred to the next pending call otherwise)/`RequestInterruptCleanup`/`IsStopRequested`/`IsStopPendingForCurrent`/`SetProgress`, `FDreamRunService::GetActiveRunId`, the any-thread `_dream` functions, `FDreamLogForwarder::EchoNow`, `FDreamPython::GetState`/`GetGeneration`/`QueueDecref` | blocking on the UI; Slate, `GWarn` or UObjects |

- **GIL is the outermost lock** (GIL-5): never take the GIL while holding a Dream lock; no Dream lock is held across a
  Python call. `_dream` functions never release the GIL, call Python or wait (GIL-2).
- **Ticker clients defer while a run is active** (§3.7): register through `FDreamDeferral`; raw `AddTicker`,
  `RegisterActiveTimer`, `OnEndFrame.Add` only in `FDreamDeferral`, `FDreamRunService`, or `DREAM_TIMER_EXEMPT(paint-only)`.
- **One entry call** (§2.5 rule 5): `ExecPythonCommandEx` only in `Private/Python/DreamPythonEntry.cpp` (EnterRun, Boot).
- **Read-only during runs**: views bind `IsReadOnly` to `FDreamRunService::IsRunActive()` (slice B: the code view reads
  its read-only state live, because UE 5.8 caches the attribute, A128).
- **Every C++-held PyObject is an `FDreamPyRef`** (GIL-7); `HandleShutdown` releases all and asserts the registry is empty.
- **Every asynchronous result applies through `FDreamDeferral`** (slice B): find results ("DreamCodeViewFind", every
  20 ms), file checks and save results, LSP events, history results (4 Hz), compaction installs, probe results
  (`FDreamInstances::Tick`), snapshot slices and completion requests all reach the game thread through an
  `FDreamDeferral` ticker. Most use `DeferWhileExecuting`; live completion uses `RefuseWhileExecuting` (§3.7: a request
  is refused while a run is active or a pump is on the stack, A145). Tickers that call Python pass `bNeedsPython`.
  The one slice-B `DREAM_TIMER_EXEMPT(paint-only)` timer is completion's one-shot typing decision, which reads only text
  and C++ caches (A150).
- **The journal writer's lane is logical, not an OS priority** (slice B ruling): the thread runs at `TPri_Normal`, and
  the lower-priority work runs in slices between commits (A105).
- **Threads are abandoned, not joined, when they could hang shutdown**: the audit writer after 1 s, the storage probe
  at `OnPreExit`, the journal writer after 2 s without CLEAN, each behind a fence (A79, A117).

## 4. Shared headers and their owners

| Header (Source/…) | Declares | Owner | Main callers |
|---|---|---|---|
| `DreamCore/Public/dream/Types.h` | `Offset`, `LineNo`, `LineRange` | core | all |
| `dream/Utf.h` | UTF-8/16 conversions, column conversions, identifier classes | core | core, output, shell |
| `dream/Checksum.h` | `Crc32c`, `Sha256` | core | shell (journal, base versions, project hash), bridge (source hash) |
| `dream/Json.h` | the JSON codec | core | core (journal), output (REPL history), shell (state files) |
| `dream/TextBuffer.h` | `TextBuffer`, `TextSnapshot`, `EditBatch`, `ChangeSet`, `MapLine` | core | shell, bridge, output |
| `dream/Diff.h` | `DiffSingleRange` | core | shell (plain mode, journal DELTA) |
| `dream/TextCodec.h` | file decode/encode, BOM, cookies, line endings | core | shell (buffers, saves) |
| `dream/PyLexer.h` | tokens, kinds, line facts, line states, incremental relex | core | shell (marshaller), output (REPL) |
| `dream/StatementExtent.h` | `StatementExtent`, `NextStatementLine`, `SelectionLines`, REPL pre-check | core | shell, output |
| `dream/EditBuilders.h` | `ToggleComment` | core | shell |
| `dream/Keymap.h` | `Stroke`, `Binding`, `Keymap::Resolve` | core | shell |
| `dream/OutputRing.h` | `LineType`, flags, `LinkSpan`, `LineInfo`, `OutputRing`, `OutputSnapshot` | core | output |
| `dream/OutputLines.h` | `LineSplitter`, `ScanLinks`, `ParsePseudoFilename` | core | output |
| `dream/VisibleIndex.h` | row ↔ sequence map | core | output |
| `dream/JournalFormat.h` | records, frames, `FileOps`, `Writer`, `ReadNamespace`, `CommitPolicy` | core | shell |
| `DreamEditor/Private/Python/DreamPythonInclude.h` | the one Python.h include | bridge | bridge, supervisor |
| `Python/DreamPythonFwd.h` | `FDreamPyObject`, `FDreamPyThreadState` forward types | bridge | any header |
| `Python/DreamGil.h` | `FDreamScopedGil`, `FDreamScopedGilRelease`, `FDreamPyRef` | bridge | bridge, supervisor |
| `Python/DreamPython.h` | `EDreamPythonState`, `EDreamStartReason`, `FDreamPython` (lifecycle, helpers, EnterRun) | bridge | shell, output, bridge |
| `Python/DreamNativeModule.h` | the `_dream` table (comments), helper contracts, `DreamNative::*` | bridge | supervisor (cancel class), python (contract) |
| `Compat/DreamPyCompat.h` | `FDreamEntryMark`, `FDreamPyCompat` | bridge | supervisor |
| `Run/DreamRunTypes.h` | run IDs, routes, U2 states and outcomes, request, record, stats, live stats, txn handle, `DreamRunText` | bridge | all DreamEditor slices |
| `Run/DreamRunService.h` | `FDreamRunService` (incl. `CanUndoRun`/`UndoRun`, the only mutable path to a history record) | bridge | shell, output, supervisor, tests |
| `Run/DreamDeferral.h` | `FDreamDeferral`, `DREAM_TIMER_EXEMPT` | bridge | shell, output |
| `Run/DreamTransactions.h` | `FDreamTransactions`, `FDreamUndoPreview` | bridge | shell (Undo Last Run, footer chip), output (footer) |
| `Run/RunSupervisor.h` | `FDreamRunPolicy`, `FRunSupervisor` | supervisor | bridge, shell (status), output (drain) |
| `Run/DreamRunProtections.h` | `FDreamRunProtections` | supervisor | supervisor |
| `Run/SDreamRunWindow.h` | `SDreamRunWindow` | supervisor | supervisor |
| `Commands/DreamInputPreProcessor.h` | `FDreamInputPreProcessor` | supervisor | supervisor |
| `Commands/DreamCommands.h` | `EDreamCommand`, `FDreamCommands` | shell | shell, output (REPL strip), supervisor (chord text) |
| `Commands/DreamKeyRouter.h` | `IDreamCommandTarget`, `FDreamKeyRouter` | shell | shell, output |
| `Commands/DreamNumpadEnter.h` | `FDreamNumpadEnter` (Win64 Numpad Enter, G7) | shell | shell (key router), output (REPL strip) |
| `Output/DreamOutputTypes.h` | `EDreamStream`, raw record, forward line, visibility, run span | output | bridge, output, shell |
| `Output/DreamOutputStore.h` | `FDreamOutputStore` | output | bridge (`_dream.write`, echo, blocks), supervisor (drain), shell |
| `Output/DreamLogForwarder.h` | `FDreamLogForwarder`, `GDreamForwarding` | output | output |
| `Output/DreamOutputDevice.h` | `FDreamOutputDevice` | output | shell (registration), bridge (SetRunActive) |
| `Output/DreamConsoleExecutor.h` | `FDreamConsoleExecutor` | output | shell (module owns it), bridge (OnInitialized) |
| `Output/DreamReplHistory.h` | `FDreamReplHistory` | output | output, shell (module opens it) |
| `Output/DreamRuntimeProblems.h` | `FDreamRuntimeProblems` | output | bridge |
| `Shell/SDreamOutputArea.h` | `EDreamOutputTab`, `SDreamOutputArea` | output | shell, supervisor (rect, hand-off) |
| `Shell/SDreamOutputView.h` | `SDreamOutputView`, `FDreamOutputViewHandoff` | output | supervisor (live run view), shell |
| `Shell/SDreamReplStrip.h` | `SDreamReplStrip` | output | output |
| `Shell/SDreamTab.h` | `SDreamTab` | shell | output (OpenLocation, status), bridge (rect via module) |
| `Shell/DreamEditorStyle.h` | `FDreamEditorStyle`, `DreamStyleKeys` | shell | output, supervisor |
| `CodeView/SDreamCodeView.h` | `SDreamCodeView` (plain-mode facade), `FDreamTextSelection` | shell | output (REPL prompt) |
| `Buffers/DreamBuffer.h` | `FDreamBuffer`, `FDreamBaseVersion` | shell | output, bridge (Run File) |
| `Buffers/DreamBufferManager.h` | `FDreamBufferManager` | shell | bridge (save before Run File, API targets), output (links) |
| `Persistence/DreamJournal.h` | `FDreamStorageRoot`, `FDreamJournal` | shell | bridge (snapshot, run end), output (storage root) |
| `Settings/DreamUserSettings.h` | `UDreamUserSettings` (config=EditorSettings) | shell | all |
| `Settings/DreamPerProjectUserSettings.h` | `UDreamPerProjectUserSettings`, `FDreamTabState` | shell | shell |
| `Settings/DreamProjectSettings.h` | `UDreamProjectSettings` | shell | output (executor, echo cap) |
| `Module/DreamEditorModule.h` | `FDreamEditorModule` service accessors | shell | all |
| `Module/DreamLog.h`, `Module/DreamText.h` | `LogDream`; FString ↔ UTF-16/UTF-8 | shell | all |

Slice-internal headers (thread table, run-log writer, output rows, marshaller, gutter, status strip, WS-07 item,
banners, Win64 FileOps, file saver) are written by the owning slice under the same folders.

In slice B the owner of a slice-A header is the slice-B group whose section-1 row holds its file (for example
`dream/JournalFormat.h` → persist, `Buffers/*` → buffers, `CodeView/*` → codeview, `Python/*` and `Run/*` → run).
Slice-B headers, new or extended (owner = slice-B group):

| Header (Source/…) | Declares | Owner | Main callers |
|---|---|---|---|
| `DreamCore/Public/dream/TextBuffer.h` (extended) | the piece tree behind the same `TextSnapshot` API; `ChangesBetween`; `NeedsCompaction`, `BuildCompacted`, `InstallCompacted`; `ForEachSpan`, `PieceCount`, `TreeDepth`; `TextBuffer` is move-only | core | all; buffers (compaction) |
| `dream/AnchorSet.h` | `AnchorSet` (Point and LineStart anchors, `MapThrough`, `Apply`, `TakeRemoved`) | core | none in M1 |
| `dream/UndoTree.h` | `UndoTree` (groups, coalescing, branches, save point, run boundaries, process cap) | core | codeview (`FDreamUndoHistory`) |
| `dream/Diff.h` (extended) | `DiffSingleRange`; `Diff::Lines` (Myers, D = 4,000 / 50 ms cap, intra-line ranges), `ToMinimalEdits`, the unified formatter | core | persist (journal DELTA, Review Compare, pending bar), codeview (`ReplaceAllText`), buffers (reload) |
| `dream/SearchEngine.h` | `SearchQuery`, `SearchEngine` (compile, `FindAll`, `Run`, `FindNext`, `UpdateAfterEdit`, replace templates), `SearchLimits` | core | codeview (find bar), output (output Find) |
| `dream/CancelToken.h` | `CancelToken` (sticky relaxed flag) | core | the find workers, `Diff` |
| `dream/EditBuilders.h` (extended) | `ToggleComment`; `IndentStyle`, `BuiltEdit`, `SmartNewline`, Backspace to the indent stop, auto-close pairing and type-over, duplicate line | core | codeview |
| `dream/JournalFormat.h` (extended) | sliced rotation (`BeginRotation`/`StepRotation`/`FinishRotation`/`AbandonRotation`), `BatchPlanner`, `OlderMajorReader`, CLEAN_TEXT, `SetAsideUnusedNewer`, `RecoveredNamespace::{SourceMajor, UsedGeneration, bSkippedUnreadable}` | persist (slice A: core) | persist, instances |
| `dream/HistoryFormat.h` | the history index family (ENTRY, TOMBSTONE, CLEAR), blob header and chunks, `ReadIndex`, `PlanRunEntry`, the retention plan | persist | persist, instances |
| `dream/StorageIndex.h` | namespace names, lock content, `owner.json`, `projects.json`, moved-project and same-identity rules, imported-namespace names | instances | instances |
| `dream/JsonRpc.h` | JSON-RPC framing and messages (on `dream/Json.h`) | lang | lang |
| `dream/LspClient.h` | `LspClient`, `LspClientOptions`, `LspEvent` (incl. `Dropped`), documents, staleness, backoff | lang | lang (`FLspTransport`, `FDreamLanguageServices`) |
| `dream/CompletionMerge.h` | `CompletionMerge` (§7.6.1), fuzzy score, chain extraction, keyword table | completion | completion |
| `DreamEditor/Private/CodeView/DreamEditGroup.h` | `FDreamTextSelection`, `FDreamTextEdit`, `FDreamEditGroup` | codeview | buffers, persist, instances, lang, completion |
| `CodeView/DreamUndoHistory.h` | `FDreamUndoHistory` (one per buffer: `For`, `Find`, `ApplyEditGroup`, `ReplaceAllText`, `ApplyEditBatch`, `ClearNextEdit`; 256 MB process cap) | codeview | the same, `SDreamCodeView` |
| `CodeView/SDreamCodeView.h` (extended) | `ApplyEditGroup`, the find bar and Go to line hosts, zoom, `GetShownText`, the `(completion)` hooks | codeview | output (REPL prompt), lang (quick fix), completion |
| `CodeView/SDreamFindBar.h` | `SDreamFindBar` | codeview | output (output-view Find) |
| `CodeView/SDreamCompletionPopup.h` | `FDreamCompletionController`, the popup | completion | codeview (hooks) |
| `Completion/DreamCompletionService.h` | `FDreamCompletionService` (`QueryCached`, `RequestLive`, `CancelLive`, `Prefetch`, `NamespaceFor`, the `FUnrealNameIndex`) | completion | code view, REPL strip, console executor |
| `Persistence/DreamLocalHistory.h` | `FDreamLocalHistory` (`AddEntry`, `Query`, `LoadText`, `Clear`, `MergeFrom`, `IsPaused`, `OnPausedChanged`) | persist | buffers (reload), persist (discard, restore), instances (merge), `SDreamTab` |
| `Persistence/DreamHistoryTypes.h` | `EDreamHistoryKind`, `FDreamLocalHistoryEntry`, `DreamHistoryNoteUtcMs` | persist | persist, instances |
| `Persistence/DreamRestoreGuard.h` | `FDreamRestoreGuard` (`BeginRestore`, `OnRestoreApplied`, `Clear`, `EnterSafeMode`, `LeaveSafeMode`, `IsSafeMode`, `GetSafeModeReason`, `OnSafeModeChanged`) | persist | `SDreamTab`, module, lang |
| `Persistence/DreamInstances.h` | `FDreamInstances` (`ResolveStorageRoot(bStrict)`, `GetResolvedRoot`, orphans, finalize, `projects.json`, import, the secondary's poll) | instances | module, `SDreamTab`, lang (log root) |
| `Persistence/DreamStorageProbe.h`, `DreamStorageRoot.h` | the `DreamStorageProbe` thread; the stateless §10.1 helpers in namespace `DreamStorageRoot` (bases, project hash, root refusals, writability probe, `projects.json` path) | instances | instances, journal, module |
| `Persistence/DreamJournal.h` (extended) | `OnBufferMetaChanged`, `SetCleanText`, `GetRole`, `GetNamespace`, `GetNamespaceDir`, `GetStateDir`, `GetHistoryDir` | persist, then instances | buffers, lang, module, instances |
| `Diagnostics/DreamAudit.h` | `FDreamAudit` (`RunStarted`, `RunEnded`, `SnapshotSkipped`, `RecordLostRun(s)`, `Event`, `SetRole`, `MakeSecondaryId`) | run | run service, module, persist (`history.clear`), instances (secondary id) |
| `Diagnostics/DreamCrashContext.h` | `FDreamCrashContext` (the §11.8.1 keys, game thread only) | run | run service, supervisor |
| `Diagnostics/DreamTrace.h` | the `DreamPump` and `DreamRun` trace channels and events | run | supervisor, run service, tests |
| `Language/DreamLanguageServices.h` | `FDreamLanguageServices` (`OnDeferredStart`, `SetRunActive`, `Shutdown`, `GetProblems`, `GetSnapshot`, `MakeLogPath`, code actions, `WalkProblems`, `ShowQuickFixes`) | lang | Python entry, protections, module, `SDreamTab`, Problems view |
| `Language/DreamProblemsModel.h`, `DreamLspTransport.h`, `DreamStubSnapshot.h` | `FDreamProblemsModel`; `FLspTransport`, `FDreamChildJob`; `FDreamStubSnapshot` | lang | lang, Problems view |
| `Python/DreamPythonSnapshot.h` | the wrapper over `stubs.start`/`step`/`stop` (Python.h stays in the .cpp) | lang | lang |
| `Python/DreamNativeModule.h` (extended) | `check_cancel(deliver)`, `hard_object_props`, `container_element_kind` | run | python, completion (resolver) |
| `Run/DreamRunService.h` (extended) | `RerunWithTransaction`, `CanRerunInTransaction`, `ResolveTabTransaction`, `TabToggleShown`, `TabToggleTooltip` | run | output (footer), buffers (toolbar), lang (configuration) |
| `Run/RunSupervisor.h` (extended) | `PostPendingCallForTest` (marked `(tests)`) | run | tests |
| `Buffers/DreamFileWatch.h` | `FDreamFileWatch` (expected stamps, checks, rescans) | buffers | buffers (saver, manager) |
| `Buffers/DreamBufferManager.h` (extended) | `ReopenClosed`, `FindClosedByFilename`, `ConvertToUtf8`, recent files, file state | buffers | output (Reopen tab link), `SDreamTab` |

## 5. Slice-A, slice-B and restyle items: TDD sections, files, owners

### Slice-A items

Files are under `Source/DreamEditor/Private/` unless the path says otherwise. `.h` files marked * were written by the
architect; the owner writes the `.cpp` bodies and any internal headers.

| # | Item (brief) | TDD sections | Files → owner |
|---|---|---|---|
| 1 | Tab and menus: nomad tab `DreamEditor.Main` "Dream", Tools ▸ Dream Python Editor, Window menu, `Dream.Open` | §11.6.1, §11.7, WS-01 | `Module/DreamEditorModule.h`*/`.cpp`, `Module/DreamMenus.cpp`, `Module/DreamConsoleCommands.cpp` → shell |
| 2 | Layout: toolbar, output area on top (Output · REPL · Problems + prompt strip), splitter 40% / 120 px, code area, status strip, `layout.output_position` and Move Output commands | §6.4 output area, §6.5 placement, WS-02, WS-03, WS-05, PO-01, RV-18 | `Shell/SDreamTab.h`*/`.cpp`, `SDreamTabToolbar.cpp`, `SDreamStatusStrip.h/.cpp`, `SDreamEditorTabs.h/.cpp` → shell; `Shell/SDreamOutputArea.h`*/`.cpp` → output |
| 3 | Buffers: scratch `Untitled-N` (journaled), file tabs, save Ctrl+S, `*` dirty, path tooltip, New/Close/Reopen closed, Ctrl+PageUp/PageDown | §4.13, §10.2.5 untitled, §10.14, BUF-01..BUF-03 | `Buffers/DreamBuffer.h`*, `DreamBufferManager.h`*, `.cpp`s, `DreamFileSaver.cpp` → shell; `dream/TextCodec.h`* + `Private/TextCodec.cpp` → core |
| 4 | Code view = plain mode: `SMultiLineEditableText`, PyLexer marshaller, gutter, current line, mono font, `OnKeyDownHandler` keymap, edits reflected to TextBuffer | §4.15 row (3), §4.3, §4.12, EDT-15, EDT-01 | `CodeView/SDreamCodeView.h`*/`.cpp`, `CodeView/DreamPythonMarshaller.h/.cpp`, `CodeView/SDreamGutter.h/.cpp`, `Shell/DreamEditorStyle.h`*/`.cpp` → shell; `dream/PyLexer.h`*, `dream/Diff.h`*, `dream/TextBuffer.h`* + cpps → core |
| 5 | Keymap (Charcoal) through `FDreamCommands` in context `DreamEditor`; consume list; Numpad Enter on Win64 (G7 hooks) | §4.9.1, §4.9.2, §4.9.3, §4.9.4, SPEC §9.3, D8, RT-16 | `Commands/DreamCommands.h`*/`.cpp`, `Commands/DreamKeyRouter.h`*/`.cpp`, `Commands/DreamNumpadEnter.h`*/`.cpp` → shell; `dream/Keymap.h`*, `dream/StatementExtent.h`*, `dream/EditBuilders.h`* + cpps → core |
| 6 | Execution: `FDreamRunService`, routes, numbering, records, Starting steps, OnEndFrame entry | §5.1, §5.2, §3.6, D14, RT-01, RT-14, RT-15 | `Run/DreamRunTypes.h`*/`.cpp`, `Run/DreamRunService.h`*/`.cpp`, `Run/DreamDeferral.h`*/`.cpp`, `Python/DreamPython.h`*/`.cpp`, `Python/DreamPythonEntry.cpp` → bridge |
| 7 | Runner: compile rules, statement mode, echo call, auto-print `_`, dunders, swaps, namespaces, Reset Session, SystemExit, tracebacks, outcomes; plain CPython with null `_dream` | §5.3, §6.1 swaps, RUN-06..RUN-08, RUN-11, RUN-16, RUN-18, RUN-27 | `Content/Python/dream_editor/__init__.py`, `_version.py`, `_impl/__init__.py`, `runner.py`, `namespaces.py`, `autoprint.py`, `streams.py`, `task.py` (sliced sleep), `compat.py`, `deprecation.py` → python; `_impl/native.py`* → python (surface fixed by bridge's header); `Tests/python/`, `Tests/run_tier2.bat` → python |
| 8 | `_dream` native module | §12.1, §3.4, GIL-2 | `Python/DreamNativeModule.h`*/`.cpp`, `Python/DreamNativeWriter.cpp`, `Python/DreamGil.h`*/`.cpp`, `Python/DreamPythonInclude.h`*, `Python/DreamPythonFwd.h`*, `Compat/DreamPyCompat.h`*/`.cpp` → bridge |
| 9 | RunSupervisor: watchdog, one pending call, GIL hand-off, pump, guards, Run window, protections, pre-processor, cancellation, native-call reporting, `run.pump` kill switch | §5.4–§5.8, §2.6.3, D2, RV-01, RV-05 | `Run/RunSupervisor.h`*/`.cpp`, `Run/RunSupervisorWatchdog.cpp`, `Run/DreamRunProtections.h`*/`.cpp`, `Run/SDreamRunWindow.h`*/`.cpp`, `Commands/DreamInputPreProcessor.h`*/`.cpp` → supervisor |
| 10 | Cooperative API: `progress`, `check_cancel`, run-scoped sliced sleep | §5.10.1–§5.10.3 | `_impl/task.py`, `__init__.py` → python; `_dream.progress`/`check_cancel` → bridge (calls `FRunSupervisor::SetProgress`/`IsStopPendingForCurrent`) |
| 11 | Transactions: one per run `Dream: <tab> #N`, Begin/End on every exit, size watch, Undo Last Run | §5.9.1–§5.9.5, RUN-07, RUN-22, U3, U9, RT-08 | `Run/DreamTransactions.h`*/`.cpp`, `FDreamRunService::CanUndoRun`/`UndoRun` → bridge; toolbar toggle and Undo Last Run command (calls `UndoRun(0, …)`) → shell; footer chip (calls `UndoRun(N, …)`) → output |
| 12 | Output: store, forwarder, device, output view, run logs, runtime Problems | §6.1–§6.4, OUT-01..05, 07..09, 11 (runtime), 12 | `Output/*.h`* and `.cpp`s, `Output/DreamOutputIngest.cpp`, `DreamThreadTable.h/.cpp`, `DreamRunLog.h/.cpp`, `Shell/SDreamOutputView.h`*/`.cpp`, `Shell/SDreamProblemsView.h/.cpp` → output; `dream/OutputRing.h`*, `OutputLines.h`*, `VisibleIndex.h`* + cpps → core |
| 13 | REPL: prompt strip, Enter when complete, Shift+Enter, Ctrl+Enter, Up/Ctrl+Up history per project | §6.5, §4.14 prompt mode, REPL-01, REPL-02 basic, REPL-06 | `Shell/SDreamReplStrip.h`*/`.cpp`, `Output/DreamReplHistory.h`*/`.cpp` → output; `FDreamPython::ClassifyInput` → bridge; `dream::ReplPreCheck` → core |
| 14 | Console executor `Python (Dream)` | §6.6, UEI-08 | `Output/DreamConsoleExecutor.h`*/`.cpp` → output |
| 15 | Journal and recovery: storage root, WAL on a worker, pre-run snapshot, restore tabs, recovery banner | §10.1–§10.3, §10.7 basic, D17, U30 | `Persistence/DreamJournal.h`*/`.cpp`, `DreamJournalWriter.cpp`, `DreamStorageRoot.cpp`, `DreamWin32FileOps.h/.cpp`, `Shell/SDreamRecoveryBanner.h/.cpp` → shell; `dream/JournalFormat.h`*, `dream/Json.h`*, `dream/Checksum.h`* + cpps → core |
| 16 | Settings: `UDreamUserSettings` and the per-project session | §11.1.1 (subset), §10.3 | `Settings/DreamUserSettings.h`*/`.cpp`, `DreamPerProjectUserSettings.h`*, `DreamProjectSettings.h`* → shell |
| 17 | WS-07 status-bar item on `LevelEditor.StatusBar.ToolBar` | §11.6.2, U14 | `Shell/SDreamStatusItem.h/.cpp` → shell |
| 18 | Public Python API (M1): `progress`, `check_cancel`, `sleep`, `clear_output`, `open`, `new_tab`, `run`, `suppress`, `ScriptCancelled`; console commands `Dream.Open`, `Dream.RunFile <path>`, `Dream.ClearOutput` | §12.2, §11.7 | `dream_editor/__init__.py` → python; `_dream.open_tab/new_tab/enqueue_run/suppress/clear_output` → bridge; console commands → shell |
| — | Build: `DreamEditor.Build.cs`, `DreamEditorTests` module, uplugin | §2.3 | `DreamPythonEditor.uplugin`*, `Source/DreamEditor/DreamEditor.Build.cs`* → shell; `Source/DreamEditorTests/*`* → tests; `Source/DreamCore/DreamCore.Build.cs` → core |
| — | `STATUS.md` (slice-B deferrals, brief) | — | `STATUS.md` → architect, from each slice's `// Slice B:` report |
| — | Tier 1 build and framework | §2.3.4, §14.1 | `Tests/DreamCore/build.bat`*, `dream_test.h`*, `test_main.cpp`* → core |
| — | Tier 4 specs, host scripts, runners | §14.1, §14.2 | `Source/DreamEditorTests/Private/**`, `DreamHost/Scripts/**`, `Tests/run_tier1.bat`, `run_tier4.bat`, `run_build.bat`, `Tests/ci/*` → tests (`Tests/run_tier2.bat` → python, item 7) |

### Slice-B items

B-ids from the slice-B plan; files under `Source/DreamEditor/Private/` unless the path says otherwise, DreamCore files
under `Source/DreamCore/`. Tests: spec names are tier 4; "t1" is tier 1 (`Tests/DreamCore`), "t2" tier 2
(`Tests/python`).

| B | Item | TDD sections | Main files | Owner | Tests |
|---|---|---|---|---|---|
| B01 | `dream::UndoTree`: groups, coalescing, branches, bounded memory, survives save | §4.2, EDT-08 | `dream/UndoTree.h`, `UndoTree.cpp` | core | t1 `test_undotree.cpp` (T-CORE-010, deterministic undo round-trip fuzz) |
| B02 | Piece tree, `AnchorSet`, `ChangesBetween`, compaction API | §4.1.3 | `dream/TextBuffer.h`, `TextBuffer.cpp`, `dream/AnchorSet.h`, `AnchorSet.cpp` | core | t1 `test_piecetree.cpp` (T-CORE-001, 002, 003, 005, 006); `Dream.Buffers.Compaction` |
| B03 | `dream::Diff`: `Diff::Lines`, `ToMinimalEdits`, unified formatter | §4.6, §10.7.2 | `dream/Diff.h`, `Diff.cpp` | core | t1 `test_diff_lines.cpp` |
| B04 | `dream::SearchEngine`: find, replace, case, regex, whole word | §4.4, EDT-09 | `dream/SearchEngine.h`, `dream/CancelToken.h`, `SearchEngine.cpp`, `SearchRegex.h/.cpp`, `SearchTables.inc` | core | t1 `test_search.cpp` (T-CORE-017 golden against Python `re`, T-CORE-018) |
| B05 | Edit builders: smart indent on Enter, Backspace to the tab stop, auto-close pairing, duplicate line | §4.5 (edit builders), EDT-03, EDT-04, EDT-05 | `dream/EditBuilders.h`, `EditBuilders.cpp` | core | t1 `test_editbuilders.cpp` |
| B06 | Tier-1 gates: T-LEX-001, T-LEX-008, T-CORE-004, T-CORE-009, full-scale runs; libFuzzer T-PER-005 not built (A75) | §14.1, §4.1.4, §4.3 | `Tests/DreamCore/lex_golden.py`, `test_lex_golden.cpp`, `test_perf.cpp`, `build.bat` | core | t1 goldens and timing gates; `DREAM_FULL_SCALE=1` |
| B10 | Checkpoints in 50 ms / 4 MiB slices with the catch-up step | §10.2.1, §10.2.3 | `dream/JournalFormat.h`, `JournalFormat.cpp`, `Persistence/DreamJournalWriter.cpp` | persist | t1 `Journal_RotationInSlicesWithCatchUp` |
| B11 | Older-major migration to `wal.<g>.sjl.v<major>` | §10.6 | `dream/JournalFormat.h`, `JournalFormat.cpp` | persist | t1 `Journal_OlderMajorIsReadAndMigrated` |
| B12 | g-1's header checked before it is deleted | §10.2.3, §10.6 | `JournalFormat.cpp` | persist | t1 `Journal_RotateLooksAtPreviousHeaderBeforeDeleting` |
| B13 | `dream::journal::BatchPlanner`; the retry queue replayed as one batch | §10.2.1, §10.11 | `dream/JournalFormat.h`, `Persistence/DreamJournalWriter.cpp` | persist | t1 `Journal_BatchPlannerReplaysFailedBatchWhole` |
| B14 | `FDreamLocalHistory`: blobs, index, entries, retention, Clear | §10.4, PER-04 | `Persistence/DreamLocalHistory.h/.cpp`, `DreamHistoryTypes.h`, `dream/HistoryFormat.h`, `HistoryFormat.cpp` | persist | t1 `test_history.cpp` (T-PER-010, mutation fuzz, merge by time); `Dream.Persistence.History` (T-PER-017, T-PER-018, T-PER-010), `Dream.Persistence.Writer` |
| B15 | Discarded texts move to history | §10.4, §10.7.3 | `Shell/SDreamTabRecovery.cpp`, `Shell/SDreamTab.cpp` | persist | `Dream.Persistence.Recovery` (T-PER-031) |
| B16 | Counted scratch buffers show their clean text under "Not restored yet" with the pending bar | §10.7.1 | CLEAN_TEXT in `JournalFormat`, `Persistence/DreamJournal.cpp`, `Shell/SDreamTab.cpp` | persist | t1 `Journal_CleanTextOfScratchBuffers`; `Dream.Persistence.Recovery` (T-PER-029) |
| B17 | Recovery Review panel: Restore, Restore as new tab, Compare, Discard | §10.7.2, §10.7.3 | `Shell/SDreamRecoveryReview.h/.cpp`, `SDreamTabRecovery.cpp`, `SDreamRecoveryBanner.*` | persist | `Dream.Persistence.Recovery` |
| B18 | Restore guard and automatic safe mode; Leave safe mode applies the skipped restore | §10.8 | `Persistence/DreamRestoreGuard.h/.cpp`, `Shell/SDreamTab.cpp` | persist | `Dream.Persistence.SafeMode`, `Dream.Persistence.Recovery` |
| B20 | Secondary namespace with staging, merge, orphan scan, View its tabs, 30 s poll (replaces A39's Disabled journal) | §10.5 | `Persistence/DreamInstances.h/.cpp`, `DreamJournal.cpp`, `DreamJournalWriter.cpp`, `Output/DreamReplHistory.*`, `Shell/SDreamTabRecovery.cpp` | instances | `Dream.Persistence.Instances` (T-PER-023 and T-PER-045 shapes) |
| B21 | Compat namespace `journal/compat-<major>.<minor>/` | §10.6, PER-11 | `Persistence/DreamJournalWriter.cpp` | instances | `Dream.Persistence.Instances` (T-PER-025) |
| B22 | `storage.root_override` user setting, probe, `owner.json`, fallback, `projects.json`, moved-project import, orphans, cross-root scan | §10.1 | `Persistence/DreamInstances.*`, `DreamStorageProbe.*`, `DreamStorageRoot.*`, `dream/StorageIndex.h`, `Settings/DreamUserSettings.h` | instances | t1 `test_storage.cpp`; `Dream.Persistence.Instances` (T-PER-053 user layer, T-PER-013 and T-PER-055 shapes) |
| B23 | [Open storage settings] beside [Run without snapshot] | §10.2.5 | `Shell/DreamOutputRows.cpp` | instances | none |
| B24 | G6 driver B prototype; restore across a real restart | §10.10, T-PER-036, T-E2E-010 | `DreamHost/Scripts/run_g6.ps1`, `DreamEditorTests/.../DreamG6DriverSpec.cpp` (`Dream.Test.G6Step`) | instances | `run_g6.ps1` (5 kills); `Dream.Persistence.G6Driver` (driver-only) |
| B30 | `FDreamAudit`: writer, run and snapshot-skip events, `audit.gap`, lost runs, sweep, banner | §11.4, SEC-05 | `Diagnostics/DreamAudit.h/.cpp` | run | `Dream.Audit.Files`, `.Runs`, `.Unwritable`, `.LostRun` (T-AUD-001, 002, 004, 005, 006, T-RUN-024) |
| B31 | `FDreamCrashContext`: every §11.8.1 key, game thread only | §11.8.1, TEL-04 | `Diagnostics/DreamCrashContext.h/.cpp` | run | `Dream.Diagnostics.CrashContext` (T-TEL-001, T-API-015) |
| B32 | "Starting… waiting for the editor to become idle" after 1 s | §3.9 | `Run/DreamRunTypes.h`, `Shell/SDreamStatusStrip.cpp`, `SDreamStatusItem.cpp` | run | `Dream.Run.StartingText` |
| B33 | `STREAM_AGENT` only for agent or bridge runs | §1.1, §6.1 | `Python/DreamNativeModule.cpp`, `native.py` | run | t2 `test_cancel_deferral`; `Dream.Run.NativeSurface` |
| B34 | check_cancel's deferral row (ends A19) | §5.10.2 | `Python/DreamNativeModule.*`, `native.py`, `__init__.py`, `_impl/task.py` | run | t2 `test_cancel_deferral`, `test_api` |
| B35 | Selection aliases at prepare, `GetSessionEpoch`, Reset Session clears the ownership table | §5.3.8, U19, UEI-03 | `Python/DreamPythonAliases.cpp`, `_impl/aliases.py`, `_impl/namespaces.py` | run | t2 `test_aliases` (T-RUN-017); `Dream.Run.Aliases` |
| B36 | Map change releases the aliases and the last-run namespace | §5.3.3, D3 | `Python/DreamPythonAliases.cpp`, `_impl/aliases.py` | run | `Dream.Run.Aliases` (T-LIFE-014 in part) |
| B37 | T-API-006: API snapshot and the CHANGELOG "API:" rule | §12.2 | `Tests/python/api/dream_editor_api.json`, `CHANGELOG.md` | run | t2 `test_api_snapshot` |
| B38 | Rerun with a transaction override; footer [Run in transaction] | RT-08, §5.9 | `Run/DreamRunService.h/.cpp`, `Shell/DreamOutputRows.cpp` | run | `Dream.Transactions.RunInTransaction` |
| B39 | Insights trace channels `DreamPump`, `DreamRun` | TEL-03, §13.1 | `Diagnostics/DreamTrace.h/.cpp`, `Run/RunSupervisor.cpp` | run | `Dream.Diagnostics.Trace` |
| B40 | Per-route seeding of the "One undo step per run" toggle; `bTransactionSelectionAndLine` read (replaces A56) | §5.9.1 | `Run/DreamRunService.cpp`, `Shell/SDreamTab.cpp`, `SDreamTabToolbar.cpp`, `Settings/*` | run | `Dream.Transactions.ToggleSeed`, `Dream.Transactions.NoChange` |
| B50 | `dream-check`: SUM101..SUM301, SUM900..SUM902, SUM102 quick fix, `# noqa`, parent watch, debounce | §7.3.3, INT-08 | `Tools/Python/dream_launch.py`, `Tools/Python/dream_tools/check/**` | lang | t2 `test_check_rules`, `test_check_server`, `check_fixtures/` |
| B51 | Shared docstring parser | §7.4.4 | `_impl/docstrings.py` | lang | t2 `test_docstrings` |
| B52 | Stage 1 snapshot at idle after the deferred start, stopped at shutdown | §7.4.2, §7.4.3, RT-13 | `Language/DreamStubSnapshot.*`, `Python/DreamPythonSnapshot.*`, `_impl/stubs.py` | lang | t2 `test_stubs`; `Dream.Language.Snapshot`, `Dream.Language.SnapshotGates` (T-STUB-024, T-STUB-028) |
| B53 | `dream::JsonRpc`, `dream::LspClient` | §7.2 | `dream/JsonRpc.h`, `dream/LspClient.h`, cpps | lang | t1 `test_jsonrpc.cpp` |
| B54 | `FLspTransport`: spawn, job object, I/O threads, framing, shutdown, stderr log | §7.1, TEL-01 | `Language/DreamLspTransport.*` | lang | `Dream.Language.Transport`, `Dream.Language.EscapeCheck` |
| B55 | `FDreamLanguageServices`: deferred start, `SetRunActive`, supervision, idle application, safe mode | §7.3 | `Language/DreamLanguageServices.*` and its touch points | lang | `Dream.Language.Services`, `.Headless`, `.InstanceLogs` (T-LSP-020, T-LSP-028, T-DIAG-015) |
| B56 | Problems: dream-check, parse and runtime items, dedup, F8 / Shift+F8, quick fix | §7.8 | `Language/DreamProblemsModel.*`, `Shell/SDreamProblemsView.*` | lang | `Dream.Language.Model`, `Dream.Language.Problems` (T-DIAG-002, 003, 016, 017, 021) |
| B60 | Live resolver: D6 policy, deadline, caches, Unreal-aware completers, fuzzy matching | §7.5 | `_impl/resolver.py`, `Python/DreamPythonCompletion.cpp` | completion | t2 `test_resolver`; `Dream.Completion` Resolver (T-RES-008, T-RES-024) |
| B61 | `dream::CompletionMerge` | §7.6.1 | `dream/CompletionMerge.h`, cpp | completion | t1 `test_completion.cpp` |
| B62 | Completion popup in the code view and the REPL prompt; Enter accepts in the REPL | §7.6.2, §6.5 | `CodeView/SDreamCompletionPopup.*`, `Completion/DreamCompletionService.*` | completion | `Dream.Completion` CodeView, Repl, Runs (T-MRG-011, 013, 015, 024, T-RES-011) |
| B63 | Console executor live completions, `FUnrealNameIndex` (ends A35) | §6.6, §7.5 | `Output/DreamConsoleExecutor.*`, `Completion/*` | completion | `Dream.Completion` ConsoleExecutor (T-REPL-013, T-RES-021) |
| B70 | Plain-mode undo on `dream::UndoTree`; grouped-edit API | §4.15 row 3, §4.2, EDT-08 | `CodeView/DreamUndoHistory.*`, `DreamEditGroup.h`, `SDreamCodeView.*`, `SDreamPlainText.*` | codeview | `Dream.CodeView.Undo`, `Dream.CodeView.WholeText`, `Dream.CodeView.StickyColumn`, `Dream.CodeView.ReadOnlyKeys` |
| B71 | Smart Enter, Backspace to the indent stop, auto-close, Ctrl+D duplicate line in the view | §4.5, EDT-03, EDT-04, EDT-05 | `CodeView/SDreamCodeView.cpp` | codeview | `Dream.CodeView.SmartEdit` |
| B72 | Find and replace bar: Ctrl+F, Ctrl+H, F3, case, whole word, regex, live count | §4.4, EDT-09 | `CodeView/SDreamFindBar.*`, `DreamFindSearch.*`, `SDreamCodeView.cpp` | codeview | `Dream.CodeView.Find`, `Dream.CodeView.FindWorker` |
| B73 | Go to line, Ctrl+G | §7.9.3, NAV-03 | `CodeView/SDreamGoToLine.*` | codeview | `Dream.CodeView.GoToLine` |
| B74 | Ctrl+wheel and Ctrl+= / Ctrl+- / Ctrl+0 zoom, live Font size | §4.8.3, EDT-10 | `CodeView/SDreamCodeView.cpp`, `Shell/DreamEditorStyle.*` | codeview | `Dream.CodeView.Zoom` |
| B75 | Ctrl+M: Tab moves focus | CFG-08 | `Commands/DreamCommands.*`, `CodeView/SDreamCodeView.cpp` | codeview | `Dream.CodeView.TabFocus` |
| B80 | External changes: watch, clean reload, deferral, deleted file, dirty bar, expected stamps, rescans | §10.15, §10.14, BUF-04 | `Buffers/DreamFileWatch.*`, `DreamFileSaver.*`, `DreamBufferManager.*`, `Shell/SDreamTabFiles.cpp` | buffers | `Dream.Buffers.ExternalChange` (T-PER-050, T-GAP-025), `Dream.Buffers.DeletedFile` (T-PER-052) |
| B81 | Read-only target bar: Make writable, Save As | BUF-05 | `Buffers/DreamBufferManager.*`, `Shell/SDreamTabFiles.cpp` | buffers | `Dream.Buffers.SaveFailure` (T-UEI-009 M1 part) |
| B82 | "Save as UTF-8?" | BUF-06, §4.13 | `Buffers/DreamBufferManager.cpp` | buffers | `Dream.Buffers.SaveFailure` (T-ENC-006) |
| B83 | Recent Files | BUF-03 | `Shell/SDreamTabToolbar.cpp`, `SDreamTabFiles.cpp`, `Settings/DreamPerProjectUserSettings.h` | buffers | `Dream.Buffers.Tabs` (T-GAP-002 recent part) |
| B84 | Tab badges: language, dirty, lock, recovered | BUF-02 | `Shell/SDreamEditorTabs.*` | buffers | `Dream.Buffers.Tabs` |
| B85 | RUN-26 hint chip | RUN-26 | `Shell/SDreamTabFiles.cpp`, `SDreamTab.cpp` | buffers | `Dream.Buffers.RunLineHint` (T-KEY-018) |
| B86 | Per-buffer reopen and the output's "Reopen tab" link | §6.4 | `Buffers/DreamBufferManager.*`, `Shell/DreamOutputRows.cpp` | buffers | `Dream.Buffers.Tabs` |
| B90 | `FDreamThreadTable` keyed by (native ID, `PyThreadState*`) | §6.1 | `Output/DreamThreadTable.*`, `DreamOutputIngest.cpp`, `Python/DreamNativeModule.cpp` | output | `Dream.Output.ThreadTable` (T-OUT-012) |
| B91 | DreamOutput Insights counters | TEL-03 | `Output/DreamOutputStore.*`, `DreamOutputIngest.cpp` | output | `Dream.Output.ThreadTable` |
| B92 | Output-view Find (Ctrl+F) and the soft-wrap toggle | §6.4 | `Shell/SDreamOutputView.*`, `DreamOutputRows.*` | output | `Dream.Output.Find` |
| B93 | Output specs: T-OUT-020, the T-OUT-021 echo guard, T-OUT-023 run-log creation, P3-out-03, T-UEI-002 | §6.8 | `DreamEditorTests/.../DreamOutput*Spec.cpp` | output | `Dream.Output.EchoCap`, `.EchoGuard`, `.RunLogCreation`, `.DownKey`, `.StatusItem` |
| B100 | §14.2 fixtures: `_dream_test`, the input bot, TickCoreTicker, SleepNative, CallPythonOverride, FillPendingCallQueue, PostSupervisorCall, QueueOsKeys, the getter-counting fixture | §14.2 | `DreamEditorTests/Private/Fixtures/**`, `Python/DreamTestPython.*`, `Support/**` | tests | used by the specs below |
| B101 | G2 suite in the Run window; T-CAN-001 with 200 stops per shape | §5.15.1 | `Specs/DreamG2CancelSpec.cpp`, `DreamG2SupervisorSpec.cpp` | tests | `Dream.G2.Cancel`, `Dream.G2.Supervisor` |
| B102 | G5 docked suite, consume-list leak checks, T-REPL-005, T-REPL-017 | §4.16.1 | `Specs/DreamKeysG5Spec.cpp` | tests | `Dream.Keys.G5` (T-KEY-009..012, T-TXN-010, 011, T-KEY-020) |
| B103 | G7: synthetic WM_KEYDOWN with and without bit 24 | §4.16.2 | `Specs/DreamKeysG7Spec.cpp` | tests | `Dream.Keys.G7` (T-KEY-014, 015, T-REPL-015) |
| B104 | T-SUP-029 mouse pairing; Esc with a foreign modal and an open menu | §5.6, LRS-11 | `Specs/DreamSupervisorBotSpec.cpp`, `Fixtures/DreamTestModalDialog.*` | tests | `Dream.Supervisor.Bot` |
| B105 | Static checks: §2.5 rules 5, 7, 8, 9, 10, 11, 12, T-GAP-001, T-GAP-008 | §2.5, §14.6 | `Tests/ci/dream_ci_scans.py`, `dream_trace.py`, `lands_terms.json`, `loctext_baseline.json` | tests | `run_tier1.bat --scans-only` (13 scans) |
| B106 | T-GAP-019: output position and height across restarts, the exact 40 % split | §14.6 | `Specs/DreamRestartLayoutSpec.cpp`, `DreamHost/Scripts/run_restart.ps1` | tests | `Dream.Restart.Layout` (driver) |
| B107 | RUN-10 in its own session | §3.5, T-LIFE-006, T-LIFE-007 | `Specs/DreamLifecycleDynamicSpec.cpp` | tests | `Dream.Lifecycle.Dynamic` (own session) |
| B108 | Slice-A open test items P2-shl-06, A2-01, P3-per-02 | — | `DreamKeyFocusSpec.cpp`, `DreamReplHistorySpec.cpp`, `DreamJournalSpec.cpp` | tests | `Dream.Keys.Focus`, `Dream.Repl.History`, `Dream.Persistence.Journal`; tier-4 hygiene: `Dream.Hygiene.Ledger` |

### Restyle items (PO-04)

The PO-04 points of the restyle plan: 1 Maya-minimal chrome, 2 Maya-style history, 3 themes; PO-04-R1 is the
architect's ruling on the health slot (A186). Files under `Source/DreamEditor/Private/` unless the path says otherwise;
`(theme)` and `(chrome)` mark touch points in another group's file (rule 10). Spec names are tier 4; "t1" is tier 1.

| Item | PO-04 point | Files | Owner | Specs |
|---|---|---|---|---|
| Settings: `ThemeId`, the five View toggles and `bShowLineNumbers`; `OnViewSettingsChanged`, `SetViewSetting`, `SetThemeId`, `SaveKey` with its checked write and backoff, `OnFileWritten` (A203, A218) | 1, 3 | `Settings/DreamUserSettings.h/.cpp`, `Module/DreamEditorModule.cpp` (shutdown flush, `(chrome)`) | chrome | `Dream.Shell.Chrome` (defaults on a fresh object; the SaveKey backoff, success, read-only and shutdown cases), `Dream.Theme.Default`, `Dream.Hygiene.SettingsGuard` |
| Menu bar File, Edit, History, Command, View, Help; the six View toggle commands (A180) | 1 | `Shell/SDreamTabToolbar.cpp` (rewritten), `Commands/DreamCommands.h/.cpp` | chrome | `Dream.Shell.Chrome` |
| Icon row (A197) | 1 | `Shell/SDreamTabToolbar.cpp`, `Shell/DreamChrome.h/.cpp` | chrome | `Dream.Shell.Chrome`, `Dream.Visual.Capture` |
| Plain input tabs, `line:col` and the message slot; no breadcrumb row (A181, A182) | 1 | `Shell/SDreamEditorTabs.h/.cpp`, `Shell/SDreamTab.h/.cpp`, `SDreamTabImpl.h` | chrome | `Dream.Shell.Chrome`, `Dream.Buffers.Tabs`, `Dream.Visual.Capture` |
| Health slot (A186) | PO-04-R1 | `Shell/SDreamTab.cpp`, `Shell/SDreamEditorTabs.cpp` | chrome | `Dream.Shell.Chrome` (forced journal outage) |
| Parts hidden by default: the status line, the output tab row (REPL, Problems), the REPL prompt strip; the line-number gutter toggle (A183, A199 to A201) | 1 | `Shell/SDreamTab.cpp`, `SDreamStatusStrip.h/.cpp`, `SDreamOutputArea.h/.cpp`; `Shell/SDreamReplStrip.cpp` (placeholder, `(chrome)`), `CodeView/SDreamCodeView.cpp` (gutter, `(chrome)`) | chrome | `Dream.Shell.Chrome`, `Dream.Keys.G5` (REPL-017 toggles View parts) |
| Slim banners and file bars with ×; recovery line ids (A198, A219) | 1 | `Shell/SDreamRecoveryBanner.h/.cpp`, `Shell/SDreamTab.cpp`, `Shell/SDreamTabFiles.cpp`, `Shell/SDreamTabRecovery.cpp` (`(chrome)`) | chrome | `Dream.Shell.Chrome`, `Dream.Persistence.Recovery` |
| Run window flat layout (A211) | 1 | `Run/SDreamRunWindow.cpp` (layout; colours `(theme)`) | chrome, theme | `Dream.Visual.Capture`, `Dream.Theme.LiveApply` |
| Run window teardown activation (A210) | 1 (the Run window keeps its behaviour), RT-05 | `Run/SDreamRunWindow.h/.cpp`, `Run/RunSupervisor.cpp` | run | `Dream.Supervisor.RunWindowFocus` |
| Stop acts on the press; the Stop-during-native record and the one-pump rule (A216, A222) | — (ruling after integration) | `Run/SDreamRunWindow.cpp`, `Run/RunSupervisor.cpp`, `Run/RunSupervisorImpl.h` | run | `Dream.Supervisor.StopPress`, `Dream.G2.Supervisor` (T-CAN-020), `Dream.Supervisor.Bot` (T-SUP-029) |
| Centre splitter grips (A220) | 1 | `Shell/SDreamTab.cpp`, `Shell/DreamChrome.cpp` | chrome | `Dream.Shell.Chrome` (a press on either grip), `Dream.Restart.Layout` |
| Maya-style history text: echo, results, warnings, errors, stderr; no icons (A185, A204 to A207) | 2 | `Shell/DreamOutputRows.h/.cpp`, `Shell/SDreamOutputView.h/.cpp` | output | `Dream.Output.MayaStyle` |
| Run headers and footers behind `bShowRunHeaders`; the outcome line (A183, A208, A221) | 1, 2 | `Shell/SDreamOutputView.h/.cpp`, `Shell/DreamOutputRows.h/.cpp` | output | `Dream.Output.MayaStyle`, `Dream.Output.StatusItem` |
| History context menu: the "Run #N" section and Copy (A209) | 2 | `Shell/SDreamOutputView.cpp`, `Shell/DreamOutputRows.h` | output | `Dream.Output.MayaStyle` |
| REPL prompt strip rules and history popup (A195) | 3 | `Shell/SDreamReplStrip.cpp` | output | `Dream.Output.MayaStyle` (GitHub Light), `Dream.Visual.Capture` |
| Theme API, ten built-in themes, live apply (A179, A184, A187, A188, A192, A194, A195) | 3 | `Shell/DreamTheme.h/.cpp`, `Shell/DreamThemeTable.h/.cpp`, `Shell/DreamEditorStyle.h/.cpp` (rewritten); `(theme)` in `CodeView/SDreamCodeView.h/.cpp`, `SDreamGutter.cpp`, `DreamPythonMarshaller.h/.cpp`, `SDreamFindBar.cpp`, `SDreamGoToLine.cpp`, `SDreamCompletionPopup.cpp`, `Run/SDreamRunWindow.cpp`, `Shell/SDreamRecoveryReview.cpp`, `Shell/SDreamProblemsView.cpp`, and one-line style lines in `SDreamOutputView.cpp`, `SDreamReplStrip.cpp`, `SDreamOutputArea.cpp`, `SDreamEditorTabs.cpp` | theme | `Dream.Theme.BuiltIns`, `Dream.Theme.Default`, `Dream.Theme.Unreal`, `Dream.Theme.LiveApply` |
| Chrome colours through `FDreamChrome` (A196) | 3 | `Shell/DreamChrome.h/.cpp` | chrome | `Dream.Shell.Chrome` (live theme), `Dream.Visual.Capture` (pixels) |
| Theme JSON codec, user themes, export (A189 to A191, A217) | 3 | DreamCore `dream/ThemeFormat.h`, `Private/ThemeFormat.cpp`, `Tests/DreamCore/test_theme.cpp`; `Shell/DreamTheme.cpp` (console commands); `Shell/SDreamTabToolbar.cpp` (end of `FillThemeMenu`, `(theme)`) | theme | t1 `test_theme`; `Dream.Theme.Json` |
| Theme pickers: View ▸ Theme and the Editor Preferences ThemeId dropdown (A193) | 3 | `Shell/DreamThemeDetails.h/.cpp`, `Shell/SDreamTabToolbar.cpp` (`FillThemeMenu`); `DreamEditor.Build.cs` and `DreamEditorTests.Build.cs` (PropertyEditor, `(theme)`) | theme, chrome | `Dream.Theme.Dropdown`, `Dream.Shell.Chrome` |
| Test hygiene: activation at test start, the pass-level settings guard, ASCII test names (A212 to A214, A224, A225) | — | `DreamEditorTests/Private/Support/DreamTestSteps.cpp/.h`, `Specs/DreamHygieneSpec.cpp`, `Tests/ci/check_automation_report.py`, `Tests/ci/dream_ci_scans.py` (`testnames`) | tests | `Dream.Hygiene.SettingsGuard`; `run_tier1.bat --scans-only` |
| Specs the restyle moved: `DreamVisualCaptureSpec` (Darcula and GitHub Light), `DreamBuffersSpec`, `DreamKeyFocusSpec`, `DreamKeysG5Spec`, `DreamTabLayoutSpec`; `DreamOutputStatusItemSpec`, `DreamOutputFindSpec`, `DreamReplSameFrameSpec`; the `Dream.CodeView.ReadOnlyKeys` pump case (A215); the input bot and the T-SUP-002, T-SUP-003 and T-SUP-029 scripts (A223) | — | `DreamEditorTests/Private/Specs/*`, `Fixtures/DreamInputBot.cpp` | chrome, output, codeview, run, tests | the same specs |

### PO-05 items

The points of `po05-plan.md`, by section:
- **Sections of the plan:** 1 the side bar shell, 2 the Project tree, 3 Structure, 4 scratch files, 5 reopening tabs on
  start.
- **A1:** the amendment for plugin Python folders.

Files are under `Source/DreamEditor/Private/` unless the path says otherwise. `(sidebar)`, `(structure)` and `(scratch)`
mark touch points in another group's file. Spec names are tier 4; "t1" is tier 1.

| Item | PO-05 point | Files | Owner | Specs |
|---|---|---|---|---|
| Side bar: left, full height, width and split saved, View ▸ Project and View ▸ Structure on by default, Focus Project / Focus Structure, key routing (A228) | 1 | `Shell/SDreamSideBar.h/.cpp`; `(sidebar)` in `Shell/SDreamTab.h/.cpp`, `SDreamTabImpl.h`, `Shell/SDreamTabToolbar.cpp`, `Settings/DreamUserSettings.h/.cpp`, `Commands/DreamCommands.h/.cpp` | sidebar | `Dream.Shell.SideBar`, `Dream.Visual.Capture` (screens 14, 15) |
| Project Roots and Add plugin Python folders on Editor Preferences ▸ Plugins ▸ Dream (this project) (A229) | 2, A1 | `Settings/DreamProjectTreeSettings.h`, `Settings/DreamPerProjectUserSettings.h` (no longer registered) | sidebar | `Dream.Shell.ProjectTree` (roots, reset, preferences notifications) |
| Tree top level: the project's `Content/Python`, user roots, Project Plugins, Engine, Scratches (A230, A233) | 2, A1 | `Shell/SDreamProjectTree.h/.cpp`, `Shell/DreamProjectFiles.h/.cpp` | sidebar | `Dream.Shell.ProjectTree` |
| Listing, watching, retries and budgets (A231) | 2 | `Shell/DreamProjectFiles.h/.cpp`, `Shell/SDreamProjectTree.cpp` | sidebar | `Dream.Shell.ProjectTree` (missing items, game-thread listing count, budgets, hidden names, remote scratch folder) |
| File operations: inline editor, New, Rename, Recycle Bin delete, protected rows (A232) | 2 | `Shell/SDreamProjectTree.cpp`, `Shell/DreamProjectFiles.cpp` | sidebar | `Dream.Shell.ProjectTree` (the recycle seam; the real case only with `-DreamRealRecycleTest`) |
| Plugin Python folders and their live toggle (A233) | A1 | `Shell/DreamProjectFiles.cpp`, `Shell/SDreamProjectTree.cpp`, `Settings/DreamProjectTreeSettings.h`, `Commands/DreamCommands.*` (`TogglePluginPythonFolders`), `Shell/SDreamTabToolbar.cpp` | sidebar | `Dream.Shell.ProjectTree` (groups, toggle) |
| `dream::Outline` (A234) | 3 | DreamCore `dream/Outline.h`, `Private/Outline.cpp`; `Tests/DreamCore/test_outline.cpp` | structure | t1 `Outline_*` |
| Structure section (A235) | 3 | `Shell/SDreamStructureView.h/.cpp`; hosted by `Shell/SDreamSideBar.cpp` | structure | `Dream.Shell.Structure` |
| Scratch folder, its guard and API (A236) | 4 | `Buffers/DreamScratch.h/.cpp`; `(scratch)` in `Settings/DreamUserSettings.h` | scratch | `Dream.Buffers.ScratchFiles` |
| Scratch files, reuse, auto-save, close and exit saves, the `*` rule (A237, A238, A239) | 4 | `Buffers/DreamBufferManager.h/.cpp`, `Buffers/DreamBuffer.h/.cpp`, `Buffers/DreamFileSaver.h/.cpp`, `Buffers/DreamFileWatch.cpp`, `Shell/SDreamTabFiles.cpp`, `Module/DreamEditorModule.h/.cpp`; `(scratch)` in `Shell/SDreamTab.h/.cpp`, `Shell/SDreamEditorTabs.cpp` | scratch | `Dream.Buffers.ScratchFiles`, `Dream.Keys.Focus`, `Dream.Shell.Chrome` (shutdown flush) |
| Migration of `Untitled-N` and restored conflicts (A240) | 4 | `Shell/SDreamTabFiles.cpp`, `Shell/SDreamTabRecovery.cpp`, `Buffers/DreamBufferManager.cpp` | scratch | `Dream.Buffers.ScratchFiles` |
| Sample scripts and Help ▸ Restore Sample Scripts (A241) | 4 | `Resources/Samples/*.py`, `Buffers/DreamScratch.cpp`; `(scratch)` in `Shell/SDreamTabToolbar.cpp` | scratch | `Dream.Buffers.Samples` |
| Tabs reopen on start; scroll restore (A242) | 5 | `Shell/SDreamTab.cpp` (`(scratch)`), `Shell/SDreamTabFiles.cpp` | scratch | `Dream.Restart.Layout`, `run_restart.ps1` |
| Test isolation: scratch folder, roots, default root, recycle seam, the guard's keys (A244) | — | `DreamEditorTests/Private/Support/DreamTestSteps.h/.cpp`, `DreamHost/Scripts/run_tier4.bat`, `run_restart.ps1`, `run_smoke.ps1`, `run_g6.ps1` | scratch, sidebar | every PO-05 spec; `Dream.Hygiene.SettingsGuard` |

## 6. Contracts between slices (the details that are easy to get wrong)

**Python ↔ C++ (python, bridge).** The `_dream` table and the helper contracts are in `Python/DreamNativeModule.h`;
`_impl/native.py` mirrors it, including argument errors. Key points:
- `_boot(globals(), generation)` returns exactly `True` on success; an ABI mismatch raises
  `ImportError("Dream Python package and native module do not match; reinstall Dream")`; any other failure (a shadowing
  package, an import or capture error) is written to stderr, which reaches LogPython, and returns `False` (A53, ruling
  R3). `FDreamPython::Boot` treats `CommandResult == "True"` as success, a failed `ExecPythonCommandEx` as BootFailed
  with the last line of `CommandResult` in the banner, and anything else as BootFailed: the banner shows the reason of
  the `Dream did not start: <reason>` line `_boot` wrote to stderr (the engine's stderr logs through
  `unreal.log_error`, which the command's `LogOutput` captures), else "returned <repr>; see LogPython". `_run(run_id)`
  never raises; `_shutdown(generation)`.
- `prepare(run_id, request) -> dict`, request keys as documented in DreamNativeModule.h; buffer coordinates, 1-based.
  The optional key `"run_file": bool` (default `route == "run_file"`) gives the Run File semantics of §5.3.4: C++ sends
  True for route `run_file` and for an API run of a file (route `api`, no buffer, a file path), CR-05.
  The error dict is `{msg, lineno, offset, end_offset, text, type}`: offsets are 1-based UTF-8 columns with
  `end_offset > offset`, and `type` is the class name (`SyntaxError`, `IndentationError`, `TabError`, `ValueError`, ...)
  that the not-executed block prints as `<type>: msg` (CR-06, CR-12).
- Outcome codes 1 ok, 2 error, 3 cancelled, 4 not executed (`native.OUTCOME_*` = `EDreamRunOutcome`).
- **Error detail** (A51, ruling R1): for any `BaseException` other than SystemExit and a cancel of ours, the detail is
  `"<Type>: <first line of str(exc)>"`, just `"<Type>"` when that line is empty or blank, cut to 200 characters with `…`
  (`<Type>` is module-qualified outside `builtins` and `__main__`, as the traceback's last line). C++ uses it as given
  for the footer and as `FDreamRuntimeProblem::Message`, and takes the text before the first `": "` where it needs the
  bare type. The line stays in `location`; SystemExit and cancelled details are unchanged.
- **Buffer saved** (A52, ruling R2): `FDreamPython::NotifyBufferSaved(const FString& Path)` (game thread) queues an
  `FDreamDeferral` ticker with bNeedsPython that calls `_impl.runner.on_buffer_saved(path)`, which drops the path's
  linecache entry if it is still the tuple the runner wrote (§5.3.2); a no-op when Python is not alive. The shell calls
  it from `FDreamBufferManager`'s save completion for a buffer that was dirty and has a real path (CR-04).
- The null `_dream` (`native.NullDream`) mirrors the C module's argument rules exactly: `user_code_begin`,
  `user_code_done`, `capture_begin`, `capture_end`, `run_statement`, `statement_counts` and `report_outcome` raise
  `RuntimeError("<name>(): run <id> is not the active run")` for a stale or foreign ID and `TypeError` for a non-int
  (a bool is not an int; T-API-003, CR-13); `report_outcome` treats a bool outcome or line as malformed; `write`
  replaces each lone surrogate with one U+FFFD.
- **Result lines**: `__dream_display__` writes `repr(value)` with `STREAM_RESULT` and **no prefix**, one write per line
  of a multi-line repr; the view draws `# Result: ` as decoration and the forwarder prepends it for `LogPython` (§6.3, §6.4).
- **Tracebacks** are written with `STREAM_ERROR` after `user_code_done`, runner frames removed; real paths and
  `<dream:…>` filenames in `File "…", line N` form so `dream::ScanLinks` finds them.
- **Footer notes** (stream slots left in place, threads still alive) are `STREAM_NOTICE` lines written before
  `report_outcome` (slice A choice).
- `user_code_begin` returning False means Stop came first: the runner raises `ScriptCancelled` itself.
- A run may open a second `user_code_begin` / `user_code_done` window after its first `user_code_done` (the runner's
  `sys.excepthook` call, §5.3.7). It takes a fresh entry mark and reopens delivery and the pump, keeping the run's stats
  and Stop state; a refused second window counts no delivery (CR-07).
- A True `_dream.check_cancel()` on the game thread inside the user-code window counts as a delivery
  (`FRunSupervisor::CheckCancel`: the counters and the first and last delivery times, §5.10.2, CR-08).
- `statement_counts(run_id) -> (started, returned)` (§12.1.1) feeds `_classify` for a Stop with no user frame
  (§5.3.7, §5.3.9); C++ reads it from the record through `FDreamRunService::GetStatementCounts`.
- `suppress(flags)` uses the §12.2 polarity: True suppresses (hides), and the returned dict has the same polarity
  (T-OUT-043: `suppress(echo=True)` turns Echo input off). `FDreamOutputVisibility` stores visibilities; the bridge inverts.
- `ScriptCancelled(KeyboardInterrupt)` has class attributes `run_number = 0`, `source = "async"`, `delivery = 0`;
  `_boot` registers it with `_dream.register_cancel_class`. Source strings are `DreamRunText::StopSourceText`.

**Run service ↔ output (bridge, output).** `BeginRunBlock(N)` at acceptance; `BindRunId(Id, N)` before entry;
`PushEcho(Id, line, text, first, replPrompt)` from `BeginStatement` (each snapshot line once per run; the selection's
end column on the last line); `EndRunBlock(N)` after finish. Headers and footers are rendered by the view from
`FDreamRunService::FindRecord(N)`. Compile errors (`not executed`) are formatted by the run service as a Python-style
block (`File "<name>", line N` / source / caret / `<type>: msg`) and pushed with `PushDreamLine(N, Error, …)`; Dream-origin
Error lines go to LogPython at Error, Info, Warning and Notice lines to LogDream (A46). The header and footer records
carry an `FDreamRunBlockInfo` from which the ingest thread writes the run-log header and end lines and the one LogDream
summary per header and footer (A44), so the run service logs no summary of its own (CR-26), and `EndRunBlock(N)` runs
only once Outcome, Detail, EndSeconds, ExitCode and Txn are final. Lost runs from recovery are pushed as `Warning` lines
in run 0 (slice A choice). A `FlushPartials` record commits the
run's open partial lines flagged `OLF_Partial | OLF_EchoSplit` (§6.1); `FDreamForwardLine` carries `Type` and `Flags`, so
the forwarder adds the `# Result: ` prefix and joins an `EchoSplit` line with the next line of its type and run (§6.3 rule 1).
A not executed run whose Detail starts with `DreamRunText::SnapshotFailedPrefix` ("Journal snapshot failed": the failed
commit and the 2 s timeout of §10.2.5 step 4) shows [Run without snapshot] in its footer, which calls
`FDreamRunService::RerunWithoutSnapshot(N)` (CR-17); the run service builds both details from that constant. A failed REPL
classify (`FDreamPython::ClassifyInput` returns an error) submits the input, and the run reports the problem (CR-16).

**Run service ↔ supervisor (bridge, supervisor).** `BeginRun(Id, FDreamRunPolicy&&)` with the output area's desktop
rect from `SDreamOutputArea::GetDesktopRect`, `EndRun(Id, Stats)`, `EndPresentation(Id)`; `RequestStop` from any Stop
control goes through `FDreamRunService::RequestStop*` (Starting) and then `FRunSupervisor::RequestStop` (Running).
The Run window reads `FRunSupervisor::GetLiveStats()` and the active `FDreamRunRecord` (statement k of n, source text
from `SourceSnapshot`). `FDreamRunPolicy::OnPump(FDreamRunLiveStats&)` is the run service's hook into each pump: it runs
`FDreamTransactions::OnPump` on the record's handle and writes `TxnBytes`/`TxnBudget` for the undo meter.

**Run service ↔ journal and buffers (bridge, shell).** `SnapshotForRun(Id, N, BufferId, SourceSnapshot, Route, bDebug)`
then poll `IsDurable`/`HasFailed` from the Starting handler; `MarkRunEnded`. Before the first tab spawn the journal is not
open, and `SnapshotForRun` returns a zero ticket that is durable at once (Dream.RunFile with the tab closed, WS-07). Run File of a dirty buffer saves with
`FDreamBufferManager::SaveAsync` at step 4. API tab targets resolve with `FindByIdHex` / `FindByName` only; a path
target is always a file target with Run File semantics (`bFileTarget`, `DreamRunFile::MakeFileRequest`, which takes an
open buffer's text and saves it at step 4 when dirty; RUN-03).

**Shell ↔ everyone.** `FDreamEditorModule::Get()` owns every service (created in `StartupModule`, no threads or Python
there): `GetPython`, `GetRunService`, `GetSupervisor`, `GetTransactions`, `GetOutputStore`, `GetOutputDevice`,
`GetConsoleExecutor`, `GetReplHistory`, `GetRuntimeProblems`, `GetBufferManager`, `GetJournal`, `GetTab`, `InvokeTab`,
`OpenLocation`, `NewScratchTab`. Start-up order: `StartupModule` (style, commands; the services, `FDreamOutputStore`
before `FDreamOutputDevice`, and the services-alive flag; key router, spawner, menus, the global chord, console commands;
the device into GLog; the module's bindings, the `FDreamPython` lifecycle handlers first; then `FDreamPython::Initialize`,
which on a late load calls `HandleInitialized` at once, and `FDreamRunService::Initialize`, CR-15) →
`OnFEngineLoopInitComplete` (`ResolveStorageRoot`, `FDreamOutputStore::Start(<root>/RunLogs/<session start>)`,
`FDreamReplHistory::SetMemoryOnly(true)` before `FDreamReplHistory::Open`: the history reads its file and keeps every
entry in memory; the instance lock is not taken here (CR-24, OUT-05, P2-per-03); `FDreamTransactions::Initialize`; arm
the 10 s deferred-start timer when a tab is open) → the first tab spawn of the session (`FDreamJournal::Open`, attempted
once per session, `bJournalOpenAttempted`: it takes instance.lock and decides the role (§10.3 step B, §10.5), then
`FDreamReplHistory::SetMemoryOnly(Role == Secondary)`, so a Primary result switches history to writing its file (it
appends the entries added meanwhile; when history.jsonl changed since the engine-init read, or could not be read then,
it is read again first; it is written whole only when new, torn, corrupt, missing the import flag or at the 2x-cap
compaction, P3-out-01, A2-01) and a secondary stays Disabled and memory-only (slice B: a secondary journals in its
own namespace and stages its REPL entries; see the slice-B contracts below); a late open never starts unreconciled, SHL-12; recovery scan,
`SDreamTab::RestoreSession`). An editor that never opens Dream, or a headless one, never takes the lock, so its Python
(Dream) executor history stays in memory for that session. `FDreamPython::OnInitialized` registers the console executor; `OnShutdown` unregisters it.
Shutdown order (`ShutdownModule`, CR-23): the module's engine delegates and deferred-start ticker; `HandlePreExit`
(session ini, CLEAN; a no-op after OnPreExit); the tab widgets, closed while every service lives; the entry points; the
module's bindings; each service's `Shutdown` (console executor, run service, Python, supervisor, transactions; the device
out of GLog and destroyed before `FDreamOutputStore::Shutdown`; `FDreamReplHistory::Close`); `FDreamDeferral::RemoveAllTickers`;
then `IsAvailable()` turns false; then the services are destroyed in reverse creation order. `IsAvailable()` is an
atomic flag, true only while the services exist and run (A55): every late path (`FDreamDeferral` bodies,
`FDreamRunService::HandleEndFrame`, widget destructors that unbind) checks it first.

**Key routing (shell, output).** Two passes in plain mode: `RoutePre` from the text widget's `OnKeyDownHandler`
(bound chords), `RouteBubble` from `SDreamCodeView::OnKeyDown` (consume list). The REPL strip's prompt keys run first
through `SDreamCodeView`'s `OnKeyHook`. `FDreamKeyRouter::ToStroke` sets `bNumpadEnter` from
`FDreamNumpadEnter::IsCurrentEnterNumpad()`; the Numpad Enter binding exists only while `FDreamNumpadEnter::IsAvailable()`.
In slice B the order in a code view is: the completion controller, then the REPL strip's hook, then `RoutePre`, the
stock widget and `RouteBubble`; child boxes (find bar, Go to line) route through `RoutePre` too (A150, A134).

### Slice-B contracts

**Grouped edits (codeview → buffers, persist, instances, lang, completion).** `CodeView/DreamEditGroup.h`:
`FDreamTextEdit {StartLine, StartColumn, EndLine, EndColumn, Text}` (0-based lines and UTF-16 columns of the text
before the group, `'\n'` breaks, a `'\r'` dropped) and `FDreamEditGroup {Label, Edits, SelectionAfter, Origin,
bEvenIfReadOnly}` (edits must not overlap, any order). `FDreamUndoHistory::ApplyEditGroup(FDreamBuffer&, const
FDreamEditGroup&)`, `ReplaceAllText(FDreamBuffer&, NewText, Label, Origin = Command, bEvenIfReadOnly)` (one group from
`Diff::ToMinimalEdits`) and `ApplyEditBatch` work without a code view; `SDreamCodeView::ApplyEditGroup` is used when the
buffer has one. Each is one undo step; one Ctrl+Z brings back the selection from before it. Edit groups are refused
while a run is active (RT-01), and apply to a read-only buffer only with `bEvenIfReadOnly`. Labels in use: "Reloaded
from disk" (Origin Reload), "Save as UTF-8", "Restored from history", "Quick fix: <title>", "Completion", "Replace All".

**Local history (persist → buffers, instances, `SDreamTab`).** `FDreamEditorModule::Get().GetLocalHistory()`.
`AddEntry(Buffer, Kind, Text, Path, DisplayName, RunNumber = {}, OnDone)`: `OnDone(true)` on the game thread once the
index record is durable, false when nothing could be written; a secondary's entries are staged. `Query(Buffer or an
invalid FGuid for all, OnGameThread)` newest first; `LoadText(Entry, OnGameThread)` unset when the blob is missing or
damaged; `Clear(OnGameThread)` writes CLEAR, deletes every blob, rotates the index and records `history.clear` in the
audit (the UI is M2); `MergeFrom(SourceHistoryDir, OnGameThread(ok, merged))` for the primary only. **Pause**: while an
index generation cannot be read, `IsPaused()` is true and `OnPausedChanged()` fires on the game thread at both ends;
`AddEntry` waits at the front of the history lane instead of failing (the whole lane waits, queries and Discard
included), and the tab shows a banner line (A104). The pause ends on its own once the file is released; if the
released index proves to be a newer format, history turns read-only and the waiting entry answers false (A172). The
pause covers only the index read: a failed generation rename, blob write or index commit, or a full disk, still
answers false. Run, save, closed and recovered entries come from the journal's own notes, not from callers (A99).

**Buffer metadata (buffers → persist).** `FDreamJournal::OnBufferMetaChanged(const TSharedRef<FDreamBuffer>&)` is a
forced META with the current base, path, display name and dirty flag and no history entry. Buffers call it on a file's
delete and return, on Keep mine and on a reload. The deleted state is part of `IsDirty()`, not `IsTextDirty()` (A137).

**Python ↔ C++ additions (run, completion).** `_dream.check_cancel(deliver: bool = False)`: with `deliver` True it
returns True only when no deferral condition holds, and that True counts as a delivery; `dream_editor.check_cancel()`
returns True instead of raising under a deferral, and treats frozen importlib frames on the stack as one (A83).
`_dream.hard_object_props(py_class_name: str) -> frozenset[str]` and `_dream.container_element_kind(value)`, which
returns None in M1; None and an empty set both mean "do not read", so those chains resolve at type level (A88).
`native.py`'s `NullDream` mirrors all three, argument errors included. `_dream.write(STREAM_AGENT, …)` raises ValueError
unless the active run's route is bridge or mcp (B33).

**Completion service (completion → code view, REPL strip, console executor).** `FDreamCompletionService::QueryCached
(Query, Answer&)` works from any game-thread context and never calls Python. `RequestLive` is served on
`FDreamDeferral` idle ticks that need Python; it is refused while a run is active or a pump is on the stack, and from
state Cold it still boots Python. `CancelLive(Ticket)` and `Prefetch(Query)`; `NamespaceFor(Buffer)` gives `session`,
`isolated:<id>` or `none` (A153). A request queued before a run end or Reset Session gets an empty incomplete answer.
The caches clear when Python enters ShuttingDown; the resolver keeps no Python object across calls. No language
services are needed in M1 (A145).

**Language services (lang → run's files, module).** `FDreamLanguageServices::OnDeferredStart()` is called after every
good boot in `DreamPythonEntry.cpp` (idempotent); `SetRunActive(bool)` in `FDreamRunProtections` Install and Remove;
`Shutdown()` in `HandlePreExit` and `ShutdownModule` step 6 (servers get shutdown, exit, then the kill, about 1 s in
all); HandleShutdown step 3 in `DreamPython.cpp` stops the snapshot walk, never while executing. It listens to
`FDreamRestoreGuard::OnSafeModeChanged` (off in safe mode, started on Leave) and is off in a headless editor.
`GetProblems()` is the model the Problems view, F8 and the tab read; `ApplyCodeAction` refuses while a run is active.
`MakeLogPath(Journal, ServerId)` picks the LSP log folder by the journal's role at each spawn (A92).

**Restore guard and safe mode (persist → `SDreamTab`, module, lang).** `FDreamRestoreGuard`: `BeginRestore(StateDir,
bAutomationSession)` at a spawn with the journal open and no switch or Shift, `OnRestoreApplied()`, `Clear()` (in
`HandlePreExit`), `EnterSafeMode(Reason)`, `LeaveSafeMode()`, `IsSafeMode()`, `GetSafeModeReason()` (None, Switch,
Shift, Guard) and `OnSafeModeChanged()` (on enter and on Leave). The deferred start skips Python in safe mode; Leave
applies the skipped restore and starts Python (A107). A secondary passes `<sec>/state` as its state folder.

**Instances and namespaces (instances → persist, run, lang, output, `SDreamTab`).** "Journal open" no longer means
"primary": code checks `FDreamJournal::GetRole()`. `GetNamespace()` is `main`, `i-<pid>-<process start UTC ms>` for a
secondary (`FDreamAudit::MakeSecondaryId()`, so audit and journal names match) or `compat-<major>.<minor>`;
`GetNamespaceDir()` is `<sec>` for a secondary, `GetStateDir()`/`GetHistoryDir()` give its `state/` and
`pending-history/`. A secondary stages REPL entries in `<sec>/repl-history.jsonl` (`FDreamReplHistory::SetStagingFile`),
writes LSP logs in `<sec>/Logs/LSP` and its audit file as `audit-YYYY-MM-DD.<sec-id>.jsonl`, never sweeps, never runs
retention, orphan or cross-root scans, and never writes the ini's `OpenTabs`/`ActiveTab` (A112). Orphans: only the
primary lists them; a namespace with any generation the reader skipped as unreadable is kept, not listed, never merged
or deleted (A115); a namespace is deleted only after its texts are in history and its staged history and REPL entries
merged, and any failed step keeps it (A113). `RECOVERY_PENDING`/`RECOVERY_RESOLVED` name the namespace the recovery came
from.

**Storage root (instances → module, output, lang).** `FDreamInstances::ResolveStorageRoot(UProject, Override,
WaitSeconds, bStrict)` runs at engine init (A117): override probed on `DreamStorageProbe` for at most 2 s, then the
platform default, then the temp fallback, each fallback with a persistent notice. `bStrict` is set by
`-DreamStorageRoot=<dir>`: an unusable root gives an error notice and no journal, never the platform default (A119).
`GetResolvedRoot()` answers which root this session uses (the G6 step checks it before it types; lang uses it before
the journal opens).

**Tier-4 hygiene (tests → persist, every spec).** Every `Dream.` test closes the tabs it opened, failure paths included,
and saves the tab list at once. A ledger, `DreamHost/Saved/Automation/dream-tier4-buffers.txt`, records every buffer a
`Dream.` test opens. A buffer opened while `SDreamTab::IsApplyingRestoreForTest()` is true (inside `ApplyRestore`: the
startup restore, Leave safe mode, `ApplyRecoveryForTest`) is the developer's: it is never recorded, and the per-test
close skips it, unless it has the tests' own scratch name `DreamT4-<purpose>-<4 hex>` (A175). At the first `Dream.` test
of the next pass only the listed buffers that are pending are discarded, through `SDreamTab::DiscardRecovered`, which
moves their texts to local history, then closes their tabs and prunes the ledger (A168).
A test open without a test name while the restore is not yet settled is neither listed nor closed, which fails safe.
A spec that changes `UDreamUserSettings` captures the values before its steps and restores only what it changed.

**Module start-up and shutdown (slice-B additions to "Shell ↔ everyone").** `StartupModule` creates
`FDreamLocalHistory` and `FDreamRestoreGuard` after the journal, `FDreamInstances` after them, then `FDreamCrashContext`,
`FDreamAudit`, `FDreamCompletionService` and `FDreamLanguageServices` (bindings only: no thread, no Python).
`OnFEngineLoopInitComplete` resolves the storage root through `FDreamInstances` and updates `projects.json` (not under
`-DreamStorageRoot`), starts the audit writer (its lines wait for the role) and arms the deferred-start timer, which
skips Python in safe mode. The first tab spawn opens the journal (`main`, a secondary namespace or compat), then
`Audit->SetRole` with the namespace id, `ReplHistory->SetMemoryOnly(Secondary)`, `Instances->OnJournalOpened()` and,
for any open journal, `LocalHistory->OnJournalOpened(bRunRetention)`: every role gets the history ticker (results, the
pause notice and its probe), and only the primary runs retention, 60 s after and then hourly. `HandlePreExit`:
the session ini, `LanguageServices->Shutdown`, `RestoreGuard->Clear`, `Instances->Shutdown` (the probe abandoned),
`Journal->Shutdown` (CLEAN; history drains until OnPreExit + 2 s), `Audit->Shutdown` (at most 1 s). `ShutdownModule`
step 6 stops language services and completion before the run service, and the buffer manager (file watch), instances,
local history, audit and crash context after the output store; services are destroyed in reverse creation order.

### Restyle contracts

**Theme API (theme → chrome, output, codeview, the Run window).** `Shell/DreamTheme.h`, game thread only.
`EDreamThemeSlot` has 41 slots plus `Count`, in the slot order of the JSON format: chrome `ChromeBackground`,
`ChromeBorder`, `ChromeText`, `ChromeMutedText`, `ChromeHover`, `ChromeSelected` (the active tab), `Accent` (Run,
focus), `Danger` (Stop); panes `OutputBackground` (the history), `EditorBackground` (the input), `GutterBackground`,
`GutterText`, `CurrentLine`, `Selection`, `FindMatch`, `FindCurrent`, `Caret`; output lines `OutEcho`, `OutStdout`,
`OutStderr`, `OutResult`, `OutWarning`, `OutError`, `OutNotice`, `OutInfo`, `OutLink`, `OutHeader`; syntax `SynNormal`,
`SynKeyword`, `SynBuiltin`, `SynSelf`, `SynDefinition`, `SynDecorator`, `SynNumber`, `SynString`, `SynDocstring`,
`SynEscape`, `SynOperator`, `SynComment`, `SynCell`, `SynError`. `FDreamTheme {Id, Name, bLight, bBuiltIn, BaseId,
SourcePath, Colors[Count]}` with `Get`/`Set`. `FDreamThemes`:
- `Initialize`/`Shutdown` (no file I/O at startup, A192); `Current`, `GetCurrentId`, `GetDefaultId` ("Darcula");
- colours: `Color(Slot)` (an `FSlateColor`, read at paint), `LinearColor`, `ColorAttr` (a bound getter), `Brush(Slot)`
  (one solid brush per slot whose pointer never changes, retinted in place);
- themes: `List()` and `Find(Id)` (both load user themes lazily and list the folder at most once a second, A217),
  `FindLoaded(Id)` (never touches the folder), `SetCurrent(Id, bPersist = true)` (persists through
  `UDreamUserSettings::SetThemeId`), `OnChanged()`;
- JSON: `ExportJson`, `ImportJson(Json, Out, OutError)`, `ExportToFile`, `ExportBuiltIns(Dir)`,
  `ExportBuiltInsToUserFolder`, `GetUserThemesDir`, `OpenUserThemesFolder`, `ReloadUserThemes`, and the test hook
  `SetUserThemesDirForTest`;
- shared styles, recoloured in place on every switch (A195): `FlatButtonStyle`, `TextBoxStyle`, `ScrollBarStyle`,
  `TableRowStyle`, `ListViewStyle`, `ProgressBarStyle`.

`FDreamEditorStyle`'s `GetTokenColor`, `GetLineTypeColor` and the `DreamStyleKeys` colours return the current theme
(`GetTokenSlot` and `GetLineTypeSlot` give the slot; run headers and footers map to `OutHeader`), and the code text
style and the Run window's Stop style (Danger) are rewritten in place on every switch, so code that already used them
follows the theme without edits. **The FSlateColor copy rule** (theme R1, A187): nothing stores an `FSlateColor`. A
widget reads the colour at paint, binds `ColorAttr` or uses `Brush()`; shared styles are mutated in place, outside
paint; anything built once with a colour subscribes to `OnChanged()`, which fires after the styles and brushes are
updated, and unsubscribes on destruction. Theme ids, their resolution and the Unreal theme: A184, A192.

**`FDreamChrome` (chrome → its own widgets, the find bar).** `Shell/DreamChrome.h` forwards to `FDreamThemes` (A196):
`Linear`, `Color`, `ColorAttr` and `Brush` over `EDreamChromeSlot`; the flat styles `FlatButton`, `FlatToggle` (the find
bar's options use it), `TabToggle`, `FlatCombo` and `Splitter`; `Icon(EDreamChromeIcon)` (engine SVGs, A197), `Font`,
`IsLightTheme`, `ListThemes`, `GetThemeId`, `SetTheme`, `OnChanged`. Chrome code takes its colours only through it.

**Settings (chrome → theme, output, every spec).** `UDreamUserSettings` (config=EditorSettings) adds
`FString ThemeId = TEXT("Darcula")`, `bShowStatusLine`, `bShowReplPrompt`, `bShowReplTab`, `bShowProblemsTab` and
`bShowRunHeaders` (all false) and `bShowLineNumbers` (true, new). `EDreamViewSetting {ReplTab, ProblemsTab, ReplPrompt,
RunHeaders, StatusLine, LineNumbers}` names the toggles for `GetViewSetting`. `static FSimpleMulticastDelegate&
OnViewSettingsChanged()` fires on the game thread after a View setting or the theme id changed: from
`SetViewSetting(Setting, bValue)` and `SetThemeId(Id)` (each sets the value, saves that one key and broadcasts, and does
nothing when the value is unchanged; `FDreamThemes::SetCurrent` is the `SetThemeId` caller that also applies the
colours), and from Editor Preferences (`PostEditChangeProperty`; `OnSettingChanged` still fires as before).
`SaveKey(FName Property)` writes one key to the user's EditorSettings.ini (A203): the key is set as `Obj.cpp`'s
`SaveConfig` sets it, then one `GConfig->Flush` is checked; a read-only ini gets one warning and no retry, any other
failure is retried 0.5 s to 30 s after the end of the last attempt with a warning after 5 s, and the shutdown flushes a
pending write once (A218). `static FSimpleMulticastDelegate& OnFileWritten()` fires after each successful write by
Dream. The View toggles, ThemeId, Echo input and Output position save through `SaveKey`. Output reads
`bShowRunHeaders` and binds `OnViewSettingsChanged`; theme binds it and reacts only to a `ThemeId` change (A192).

**Theme JSON (theme; `dream/ThemeFormat.h`).** The format of A189 and the import rules of A190, theme R3 and R4 as
amended in review r6: `"format": "dream-theme"` is required for folder files; an id with a control character is
rejected and the file name is used; a file is read through a bounded 1 MB read; `base` names only a built-in. Files
live directly in `%LOCALAPPDATA%\DreamPythonEditor\Themes`; `List()` and `Find()` list the folder at most once a second after the
first lazy load, and a changed listing reloads on a deferred tick (A217); nothing is read in StartupModule; exports go
to `Built-in`, which is never loaded (A191).

**Output decoration rules (output → the history and the Run window's live view).** A204 to A209: decorations are
decided at row build and never stored; `# Result: <repr> #`, `# Warning: <text> #`, `# `-prefixed traceback lines with
the exception line as `# Error: <Type>: <message> #`, and `# Error: <text> #` for `unreal.log_error`; muted echo with
no gutter, `›` or `>>> ` / `... ` prompt (A226); stderr in its colour with no badge; no per-line icons;
`File "…", line N` links, Copy, Find and link offsets work on the raw text. Headers and footers are rows only with
`bShowRunHeaders`; without them a run that ends other than OK gets the outcome line unless a row of its own Python
exception block is shown (A208, A221). Run actions live in the "Run #N" section of the history's context menu.

**Health slot (ruling PO-04-R1, A186).** While the status line is hidden, `SDreamTab` puts the highest-priority health
warning (Journal retrying, Journal off, Python not available, Lint paused) in the input tab row's message slot beside
`line:col`, in the warning colour, with the strip's action on a click; otherwise the slot shows status messages,
muted. A banner × hides only what was showing (A198).

**Run window teardown and Stop (run).** A210: activation and keyboard focus go back to the recorded focus home at
finish step 3 only when the Run window itself is the OS foreground window; there is no delayed re-focus. A216: Stop
acts on the press (`EButtonClickMethod::MouseDown`, `EButtonTouchMethod::Down`), keys unchanged, and an input Stop that
the first pump after a native call of 100 ms or more processes is recorded as Stop during that call (A222).

**Test hygiene (tests → every spec).** Besides the ledger hygiene above: every `Dream.` test starts by activating the
Dream tab's window again (A212), and the settings guard, with a snapshot per automation run, compares every
`UDreamUserSettings` key at each test end, restores a changed key in memory, in the cache and on disk, and logs the
change at Error, which fails the pass in `check_automation_report.py` (A213, A224). A spec that writes a user setting
writes it back before it ends, only the keys it changed, through `SaveKey` or `SetThemeId` (A214); a spec that must
leave a setting changed for a later step calls `DreamTest::AllowSettingChange(Test, Key)` (A224). Test names are ASCII,
checked by the `testnames` scan (A225), and an input-bot check that asks an editor window something runs before a
native call, never during it (A216, A223).

### PO-05 contracts

**Scratch folder API (scratch → sidebar, buffers, module).** `Buffers/DreamScratch.h`, namespace `DreamScratch`:
- `GetFolder()`: normalized, absolute, forward slashes, a UNC path keeping its `//`. No I/O, safe on any thread once the
  game thread has computed it.
  - Precedence: `SetFolderForTest` > `-DreamScratchFolder=<dir>` > `UDreamUserSettings::ScratchFolder` >
    `%LOCALAPPDATA%\DreamPythonEditor\Scratches`.
  - A refused setting falls back to the default (A236).
- `IsScratchPath`, `IsScratchFileName`, `EnsureFolder(OutError, bReprobe)`, `CreateNextFile(OutError)`,
  `NoteClosedUnused(Path)`, `IsCommandLineFolder()`, and the test hooks `SetFolderForTest` / `GetFolderForTest`.
- `OnFolderChanged()` fires when the folder's path changes and when `EnsureFolder` creates it; the tree rebuilds its top
  level on it.
- `FDreamBufferManager::IsPathBusy(Path)` tells whether a buffer still needs a path: open, closed with unsaved text, or a
  save in flight or due. `IsSaveInFlight(Id)` is public, and nothing waits on it.

**Side bar hosting (sidebar ↔ structure).**
- `SDreamSideBar` builds `SNew(SDreamStructureView, TWeakPtr<SDreamTab>)` in its lower slot.
- The view polls the active buffer, its version and the caret in its own Tick, so `SDreamTab` raises no event for it.
- `FocusList()` focuses the tree, and the filter row takes Esc only while it is open, so with no filter Esc reaches the
  side bar, which returns focus to the code view.
- The side bar lists are not focus homes (`SDreamTab::OnFocusChanging`).
- The side bar routes unhandled keys with `Ctx_Focused|Ctx_Output`, so code-view commands (undo, duplicate line, find)
  do nothing there, while tab commands (Run, Save, New Tab) work.

**Project files (sidebar).** `Shell/DreamProjectFiles.h`, `FDreamProjectFiles`:
- `ResolveRoots()`, `RequestListing(Folder)`, `CheckFolders`;
- the file operations, each run on a worker with its result applied on the game thread;
- `IsRemotePath` (cached per drive letter for 5 s), `IsEnginePath`, `PathKey`, `IsUnder`, `ResolveRootPath`;
- `MaxListingsInFlight = 4` local and `MaxRemoteListingsInFlight = 2` network, in separate queues;
- the test hooks `SetRootsForTest`, `SetDefaultRootForTest`, `SetRemoteForTest`, the recycle seam, and
  `SetSettingsSaveOffForTest`.

Settings: `UDreamProjectTreeSettings` (`ProjectRoots`, `bAddPluginPythonFolders`, `OnRootsChanged` from
`PostReloadConfig`). The old `ProjectRoots` key of `UDreamPerProjectUserSettings` migrates once.

**Outline (structure, DreamCore).** `dream/Outline.h`:
- `BuildOutline(const TextSnapshot&)` or `BuildOutline(text)` gives entries
  `{Kind, Name, Line, Column, Depth, Parent, FirstLine, EndLine}`. Line strictly increases, and each FirstLine comes
  after the previous entry's Line.
- `FindOutlineEntryAt(entries, line)` returns the deepest entry holding the line; between equal depths, the later one.
- `FilterOutline(entries, text, untitledCellLabel)` keeps the parents of matches.
- `MaxStringRecoveries = 4`.
- Std-only, any thread.

**Auto-save (scratch → buffers, journal, tabs).**
- A buffer is a scratch file when its path is under the folder (`FDreamBuffer::IsScratchFile`).
- Its saves go through `FDreamFileSaver` with a reason:
  - **Timer:** 1 s after the last edit, and never more than 2 s after the oldest unsaved one;
  - **FocusLost:** including the whole editor losing the foreground;
  - **RunStart:** every dirty scratch file, after the journal snapshot;
  - **Close**, **Exit** (synchronous at OnPreExit, before the journal's CLEAN), **Other** (Keep mine, explicit saves).
- History is throttled for Timer, FocusLost and RunStart only (A238).
- Paused states, in which the tab shows `*`: a save failure, a pending disk change, deleted on disk, read-only, a pending
  recovery.

## 7. Slice-A, slice-B and restyle choices (where the TDD leaves a detail open or PO-04 overrides it)

A1 to A64 are slice A's; a row that slice B changed or replaced ends with "(slice B: …)" and the row that now holds.
A65 to A170 are slice B's, grouped by area (core, run, lang, persist, instances, codeview, buffers, completion, output,
tests); A171 to A177 are the choices of review rounds 3 to 5 that the earlier rows do not cover, in the same area
order. A178 to A215 are the restyle's: the owner ruling PO-04 and the rows it overrides (A178 to A185) and ruling
PO-04-R1 (A186), then the groups' R-notes and the review-r6 choices by group (theme, chrome, output, run, tests,
codeview). A216 is the architect's ruling after integration, and A217 to A225 are the choices of review rounds r7 to
r10 and the integration that the earlier rows do not cover, in the same area order (theme, chrome, output, run,
tests); an earlier restyle row they change names the row that now holds. A slice-A or slice-B row the restyle changes
ends with "(PO-04: …)" and the row that now holds. Rows marked **Ruling** are the architect's.

| # | Choice | Where |
|---|---|---|
| A1 | Plain mode (stock `SMultiLineEditableText`) is the only code view; text undo is the stock widget's, so `dream::UndoTree` is not built in slice A; Ctrl+Z stays consumed (slice B: text undo is `dream::UndoTree` through `FDreamUndoHistory`, B70, A123, A124; plain mode stays the only view until G1, section 9) | `CodeView/SDreamCodeView.h` |
| A2 | Plain-mode keys route in two passes (`RoutePre`, `RouteBubble`) so the stock widget keeps the editing primitives and IME while the consume list holds (slice B: the completion controller and the REPL hook run before `RoutePre`, A150) | `Commands/DreamKeyRouter.h` |
| A3 | `TextSnapshot`'s representation is private (`detail::TextRep`); a copy-on-write flat text with a line index is allowed in slice A; the piece tree can replace it (slice B: replaced by the §4.1.3 piece tree with the same API, B02, A65 to A67) | `dream/TextBuffer.h` |
| A4 | Only the single-range diff (`DiffSingleRange`): plain-mode reflection and journal DELTA ops (offsets in UTF-8 bytes) computed on the writer from the last committed and the new snapshot; no `ChangesBetween` (slice B: `Diff::Lines`, `ToMinimalEdits` and the unified formatter (B03) and `ChangesBetween` (B02, A67) exist; journal DELTA ops still use `DiffSingleRange`) | `dream/Diff.h`, `dream/JournalFormat.h` |
| A5 | `dream::journal::FileOps` is an interface in DreamCore; the Win64 implementation is in `Persistence/` (DreamCore does no I/O, §2.1 wins over §10.2.1's placement); rotation writes the checkpoint in one go (slice B: rotation is sliced, B10, A100; only the session's first generation is written in one go) | `dream/JournalFormat.h` |
| A6 | JSON codec in `dream/Json.h`, ahead of `dream::JsonRpc` (slice B: `dream::JsonRpc` is built on it, B53) | `dream/Json.h` |
| A7 | `dream::VisibleIndex` lives in DreamCore so T-OUT-006 runs at tier 1 | `dream/VisibleIndex.h` |
| A8 | Extra `LineFacts` fields and flags (`FirstTokenCol`, `FirstTokenKind`, `LF_EndsInString`, `LF_EndsWithBackslash`, `LF_Decorator`); line hash omitted (shaping cache is slice B) (slice B: the line hash moved to G1; the gap-buffer line array is built, A72) | `dream/PyLexer.h` |
| A9 | `StatementExtent`/`NextStatementLine` take `PyLexer&` and relex to the end of the buffer first | `dream/StatementExtent.h` |
| A10 | `ToggleComment` as a free function over a line range (no `CaretSet` in slice A) (slice B: the B05 builders join it as free functions, A71; `CaretSet` moved to G1) | `dream/EditBuilders.h` |
| A11 | `dream::Keymap`: KeyCodes assigned by DreamEditor, `Stroke` carries a `KeyClass`; no sequences or Shift Shift; extra `Ctx_Buffer` bit | `dream/Keymap.h` |
| A12 | `OutputRing` stores run numbers (eviction reports run numbers), `TimeMs` = ms since session start, `OLF_ReplPrompt` flag for `>>>`/`...` echo (`OLF_EchoSplit` is the §6.2 flag, not a choice) | `dream/OutputRing.h` |
| A13 | Tier-1 framework `dream_test.h` replaces GoogleTest until the CMake build lands; `build.bat` mirrors UBT's DreamCore flags (`/D_HAS_EXCEPTIONS=0`, `/GR-`, `/W4 /WX`, `/Zc:preprocessor`, the UBT error warnings `/we4456 /we4458 /we4459 /we4668 /we4702`, `/external:anglebrackets /external:W0`) with UBT's toolset, MSVC 14.44 (section 8, CR-01) (slice B: adds the private `DREAM_TIER1_OUT`, the T-CORE-017 and T-LEX-001 goldens, `DREAM_SKIP_TIMING` and `DREAM_FULL_SCALE`, section 8, A73, A74) | `Tests/DreamCore/` |
| A14 | `FDreamRunService` owns the one OnEndFrame handler; boot, Enable Python and Reset Session ride it via `ArmEndFrame` / `FDreamPython::ProcessEndFrame` (§2.5 rule 9) | `Run/DreamRunService.h`, `Python/DreamPython.h` |
| A15 | Typed helper wrappers (`Prepare`, `Discard`, `ClassifyInput`, `EmergencyRestore`) instead of a generic `CallHelper`; Reset Session is helper `_impl.namespaces.reset_session()` (slice B: the alias, completion and snapshot helpers follow the same pattern in `Python/DreamPythonAliases.cpp`, `DreamPythonCompletion.cpp`, `DreamPythonSnapshot.h/.cpp`) | `Python/DreamPython.h` |
| A16 | Starting steps as an explicit state machine (`EDreamStartingStep`), at most one step per frame; run IDs allocated at Prepare | `Run/DreamRunTypes.h` |
| A17 | `EDreamTxnStatus`/`FDreamTxnHandle` declared in `DreamRunTypes.h` to avoid a header cycle | `Run/DreamRunTypes.h` |
| A18 | `FDreamRunService::Rerun` takes the run number (the footer knows it), not a run ID | `Run/DreamRunService.h` |
| A19 | `_dream.check_cancel()` never raises; `dream_editor.check_cancel()` raises on the game thread and returns the bool elsewhere; §5.10.2's deferral row is slice B (slice B: built, `check_cancel(deliver)`, B34, A83) | `Python/DreamNativeModule.h`, `native.py` |
| A20 | `_dream.open_tab` returns the tab ID (an ID is pre-allocated), because §12.2 `open()` returns it | `Python/DreamNativeModule.h` |
| A21 | (withdrawn by review: TDD v1.2 §12.2 fixes the polarity) `_dream.suppress` takes and returns the §12.2 polarity, True = suppressed, so `suppress(echo=True)` turns Echo input off (T-OUT-043); only `FDreamOutputVisibility` stores visibilities | `Python/DreamNativeModule.h`, `native.py`, `Output/DreamOutputTypes.h` |
| A22 | Outcome codes live in `native.py` (`OUTCOME_*`), not on `_dream` (T-API-001: exactly the §12.1.1 names) | `native.py` |
| A23 | Footer notes are `STREAM_NOTICE` lines; lost runs are run-0 warning lines | section 6 |
| A24 | `FDreamPyRef` is one non-template type (§3.8's `TDreamPyRef<T>`) | `Python/DreamGil.h` |
| A25 | `DreamPythonInclude.h` undefines no macros, as the engine's own wrapper; it refuses `Py_LIMITED_API`; `structmember.h` is not included | `Python/DreamPythonInclude.h` |
| A26 | `DreamPythonFwd.h` forward-declares `PyObject`/`PyThreadState` so non-Python folders stay Python.h-free | `Python/DreamPythonFwd.h` |
| A27 | Read-only during runs is an attribute bound to `IsRunActive()`, not `SetReadOnly` pushes (slice B: amended; UE 5.8's text widget caches that attribute, so the code view reads read-only live, A128) | `Run/DreamRunProtections.h` |
| A28 | `LiveDocked` presentation runs as the Run window (it is M4) | `Run/DreamRunTypes.h`, `Settings/DreamUserSettings.h` |
| A29 | `DreamOutput` starts at `OnFEngineLoopInitComplete` (B1 forbids threads in `StartupModule`); earlier records wait in the queue | `Output/DreamOutputStore.h` |
| A30 | Own static `LogPython` category object (the engine's is private) | `Output/DreamLogForwarder.h` |
| A31 | The startup ring is filled by the ingest thread, so `Serialize` stays lock-free | `Output/DreamOutputDevice.h` |
| A32 | Raw control records (`EDreamRawKind`) carry bind, header, footer, capture, flush-partials and clear through the one MPSC queue | `Output/DreamOutputTypes.h` |
| A33 | Output view on `SListView` over `dream::VisibleIndex` (SCodeView line-source mode is slice B) (slice B: stays; line-source mode moved to G1, and Find and soft wrap are built on the `SListView` view, A159) | `Shell/SDreamOutputView.h` |
| A34 | Runtime Problems in `FDreamRuntimeProblems` (Output) instead of `FProblemsModel` (slice B) (slice B: `FDreamProblemsModel` merges dream-check, parse and runtime items and reads the runtime items from `FDreamRuntimeProblems`, B56, A94) | `Output/DreamRuntimeProblems.h` |
| A35 | Console executor completions return nothing in slice A; a miss in state Cold still triggers `RequestStart(ConsoleCompletion)` (slice B: replaced by live completions, B63, A152) | `Output/DreamConsoleExecutor.h` |
| A36 | REPL prompt keys are hard-wired in `SDreamReplStrip` (like the §4.9.4 primitives); a secondary editor instance keeps REPL history in memory only (slice B: a secondary stages its entries in `<sec>/repl-history.jsonl`, which the primary merges, B20, A112) (PO-04: the prompt strip is hidden by default; its View toggle or Focus REPL shows it, A199) | `Shell/SDreamReplStrip.h`, `Output/DreamReplHistory.h` |
| A37 | Settings: user layer only (no key registry, `bOverride_` pattern or policy); transaction defaults per route in `UDreamUserSettings`; `UDreamProjectSettings` holds only `bRegisterConsoleExecutor` and the echo cap (slice B: still the user layer; new user settings `StorageRootOverride` and `bSelectionAliases`, `RecentFiles` per project, and `bTransactionSelectionAndLine` is read, A87) (PO-04: adds `ThemeId` and the View settings, each saved as one key, A203) | `Settings/*.h` |
| A38 | Code font: the engine's monospace face; JetBrains Mono (§4.12) waits for its OFL files; style keys are `TCHAR` literals (slice B: unchanged; vendoring the four OFL files is an owner decision, section 9) (PO-04: every colour of `FDreamEditorStyle` comes from the current theme, A179, A187; the font is unchanged) | `Shell/DreamEditorStyle.h` |
| A39 | Journal: a secondary instance runs with the journal Disabled; no root override or probe thread; Restore all and Discard only (no Review panel); buffer and journal uuids are 32 lower-case hex digits. The instance lock is not taken at `OnFEngineLoopInitComplete`: REPL history opens memory-only there, the first tab spawn's `FDreamJournal::Open` takes instance.lock and decides the role (§10.3 step B, §10.5), and a Primary result switches history to writing its file. It appends the entries added meanwhile. When another editor wrote `history.jsonl` since the engine-init read (size or mtime changed), or it could not be read then, the file is read again (still unreadable: this session keeps its history in memory and never replaces the file, OUT-03) and only this editor's own entries join it, so nothing of the old read is written back and a stale corrupt state no longer counts. It is written whole only when new, torn, corrupt or missing the import flag (P3-out-01, A2-01); an editor that never opens Dream, or is headless, never takes the lock, so its Python (Dream) executor history stays in memory for that session (P2-per-03) (slice B: replaced in part: a secondary journals in its own namespace instead of running Disabled (B20, A110), the root override and probe exist (B22, A117) and the Review panel is built (B17, A109); the instance lock is still taken at the first tab spawn, and the history rules above still hold for the primary) | `Persistence/DreamJournal.h`, `dream/JournalFormat.h`, `Module/DreamEditorModule.cpp`, `Output/DreamReplHistory.cpp` |
| A40 | Buffers: plain mode holds files up to 1 MB; larger files are refused (the stock widget shapes every line on the game thread; the §4.13 progressive and mapped tiers are slice B); dirty tracked by save version plus a compare when the length matches (slice B: the tiers moved to G1; a deleted file also counts as dirty, A137) | `Buffers/*.h`, `dream/TextCodec.h` |
| A41 | `DREAMEDITOR_API` on shared classes; `DreamEditorTests` adds `DreamEditor/Private` to its include path; CQTest not used yet; the uplugin's tests module carries a `Comment` key (ignored by the descriptor readers) (slice B: CQTest moved, no functional gap, section 9; the tests module adds LevelEditor, EditorFramework and Projects) | `Source/DreamEditorTests/DreamEditorTests.Build.cs`, uplugin |
| A42 | `FDreamStorageRoot` helpers and the run-log folder `<root>/RunLogs/<yyyymmdd-hhmmss>` fixed when `FDreamJournal` is constructed (slice B: the root itself is resolved at engine init by `FDreamInstances::ResolveStorageRoot`, A117) | `Persistence/DreamJournal.h` |
| A43 | `run_file(path)`: §12.2 defines no `run_file`; `run(path)` (route Api, Isolated) and `Dream.RunFile <path>` (route RunFile) cover it | section 5 item 18 |
| A44 | Output control records (CR-29 (1)): `EDreamRawKind::StartupDone` marks the first `OnPythonInitialized`, and the header and footer records carry an `FDreamRunBlockInfo`; from them the ingest thread writes the run-log header and end lines and one LogDream summary per header and footer, after the run's LogPython lines (§6.3 rule 4) | `Output/DreamOutputTypes.h`, `Output/DreamOutputIngest.cpp` |
| A45 | Only the writer streams (stdout, stderr) keep a partial line across writes (§6.1); a write on any other stream (the shims, `_dream.write` of streams 3 to 11, Dream's own lines) is whole lines (CR-29 (2)) | `Output/DreamOutputIngest.cpp` |
| A46 | Dream-origin Error lines (`PushDreamLine(N, Error, …)`: compile-error blocks, internal errors) are forwarded to LogPython at Error; Dream Info, Warning and Notice lines go to LogDream (CR-29 (3)) | `Output/DreamOutputIngest.cpp`, `Output/DreamLogForwarder.h` |
| A47 | The startup ring holds output-device lines until the first `FDreamPython` generation; its Warning and Error lines show in the collapsed "Editor startup" block, not as loose lines (CR-29 (4)) | `Output/DreamOutputDevice.h`, `Shell/DreamOutputRows.cpp` |
| A48 | Run-log retention is fixed at 30 days and 1 GB per project (`FDreamRunLogWriter::RetentionDays`, `RetentionMaxBytes`); the `retention.run_logs_days` key is slice B (CR-29 (5)) (slice B: unchanged; the key is a §11.2.2 policy key, M4, A158) | `Output/DreamRunLog.h` |
| A49 | LOCTEXT keys carry the owning area's prefix: `Out.` for output (CR-29 (6)), `Supervisor.`, `Protections.` and `RunWindow.` for the supervisor | `Output/**`, `Run/**` |
| A50 | `dream/Export.h` defines `DLLEXPORT` and `DLLIMPORT` when no engine header has, token for token as `Windows/WindowsPlatform.h:209-210` and `Unix/UnixPlatform.h:124-125`, because UBT defines `DREAMCORE_API` as `DLLEXPORT` / `DLLIMPORT` (`Definitions.h`) and DreamCore includes no engine header (CR-02, CR-30) | `dream/Export.h` |
| A51 | Ruling R1 (CR-09): the error detail of any other `BaseException` is `"<Type>: <first line of str(exc)>"`, `"<Type>"` when that line is empty or blank, at most 200 characters with `…`; a failing `__str__` gives CPython's `<exception str() failed>`. It doubles as the runtime Problems message; C++ takes the bare type before the first `": "` (section 6) | `_impl/runner.py`, `Output/DreamRuntimeProblems.h` |
| A52 | Ruling R2 (CR-04): `FDreamPython::NotifyBufferSaved(const FString& Path)` and the helper `_impl.runner.on_buffer_saved(path)` end a dirty real path's linecache entry at save (§5.3.2), called from `FDreamBufferManager`'s save completion (section 6) | `Python/DreamPython.h`, `_impl/runner.py`, `Buffers/DreamBufferManager.cpp` |
| A53 | Ruling R3 (CR-10, CR-14): `_boot` returns exactly `True`, raises the reinstall `ImportError` on an ABI mismatch and otherwise writes the reason to stderr and returns `False` (section 6). "Under this plugin" (§2.7, ENV-07) is checked as: `DreamPythonEditor.uplugin` exists three folders above the package (`<plugin>/Content/Python/dream_editor`); a copy anywhere else refuses boot and names its folder, which `FDreamPython::Boot` finds in the command's `LogOutput` and shows in the BootFailed banner (T-ARCH-017). A shadowing package that is not Dream has no `_boot`, so its `AttributeError` reaches the banner as the failed command's last line | `dream_editor/__init__.py`, `Python/DreamPythonEntry.cpp` |
| A54 | Expand Echo and Fold Echo are `FDreamCommands` (View menu, bindable, no default chord) acting on the active output view (`SDreamOutputArea::GetActiveOutputView`); they stand in for the palette entries of §6.4 until the palette lands (T-OUT-042) (PO-04: they are in the menu bar's History menu, A180) | `Commands/DreamCommands.h` |
| A55 | `FDreamEditorModule::IsAvailable()` is the services-alive flag (an atomic set after the services are constructed and cleared once they have stopped, before they are destroyed), not `FModuleManager::IsModuleLoaded`; safe on any thread and during static destruction | `Module/DreamEditorModule.h` |
| A56 | A tab's "One undo step per run" toggle decides for its Selection, Line, Cell and Buffer runs; scratch and new file buffers seed it from the Buffer-route user default (`GetTransactionDefault(Buffer)`); Run File keeps its route default ANDed with the toggle (SHL-05). `bTransactionSelectionAndLine` (`run.transaction_defaults.selection/line`) is not read in slice A and is hidden from Editor Preferences (config only). The tab toggle, seeded from `bTransactionBuffer` and now labelled "One undo step per run: tab runs", decides every tab run. Seeding the toggle per route is slice B (P2-run-02). `GetTransactionDefault(Selection/Line/Cell)` returns `bTransactionBuffer` in slice A, so a re-run whose tab is closed follows the tab-runs setting and `bTransactionSelectionAndLine` is never read (P3-run-04) (slice B: replaced by per-route seeding, B40, A87) | `Run/DreamRunService.cpp`, `Shell/SDreamTab.cpp`, `Settings/DreamUserSettings.h`, `Settings/DreamUserSettings.cpp` |
| A57 | The pump that builds and first paints the Run window is counted in the run stats but excluded from the §5.4.6 cost EWMA and the overload check (VIS-04) | `Run/RunSupervisor.cpp` |
| A58 | In safe mode the skipped session's buffers are carried in the new journal generation as parts outside the tab list, with their counted recoveries written as RECOVERY_PENDING, so a later Restore tabs needs no new record (PER-01, ADD3-02) | `Persistence/DreamJournal.cpp`, `Persistence/DreamJournalWriter.cpp` |
| A59 | In the output views only Slate input (wheel, scrollbar, keys) leaves follow mode; the list's own clamp of ScrollToBottom never does (VIS-01, VIS-02). The list navigation keys Up, PageUp and Home (without Alt) leave follow mode in `HandleListKey`, because `SListView` scrolls them at its next tick, outside input processing; `TickFollow` resumes following when the view is still at the bottom two ticks later (P2-out-01). Down and PageDown leave follow mode too when the row they reach from the list's selector item (row 0 without one) lies above the first fully shown row (P3-out-03): the offset the list last applied (`ShownOffset`, its `CurrentScrollOffset`), not `GetScrollOffset`, which a drain's `ScrollBottom` leaves at the row count until the list's next tick, and a partly shown top row counts as above, since the list's navigation scrolls up to show it whole (P4-shl-01). `SDreamTab` gives keyboard focus it receives back to the last focus home inside it that had focus (tracked in `OnFocusChanging`: a code view, the script's or the REPL prompt's, or the list of the Output, REPL, history or Problems view; never a tab-row segment, a button or a link, which take focus on their click) while `FindPathToWidget` still finds it shown, else to the active code view (Slate's mouse-down fallback on the tab chrome, the strip's x, a dock-tab label). So a click on chrome neither swallows typing nor moves the REPL prompt's caret into the script, also after a click on a tab-row segment; the tab keeps focus only in the empty state (A2-03, P2-shl-02, P3-shl-01, P4-shl-02) (PO-04: the output tabs are focusable toggle buttons, and their row is hidden while the REPL and Problems tabs are off, A201) | `Shell/SDreamOutputView.cpp`, `Shell/SDreamTab.cpp`, `Shell/SDreamOutputArea.cpp` |
| A60 | `FDreamOnRecoveryScan` returns bool (consumed); the journal caches an unconsumed scan result and delivers it to the next request (PER-02) | `Persistence/DreamJournal.h` |
| A61 | `dream_editor.open()` outside a run is applied at the next idle tick (`FDreamEditorModule::OpenLocationFromApi`), inside a run at run end; the pre-allocated ID is returned (SHL-09). `dream_editor.open()` refuses, up front, a real path over 1 MB (`FDreamBufferManager::PlainModeMaxBytes`, A40) and a headless session (no tab spawner), returning an empty ID; a repeated open of the same path before the queued open runs returns the same pre-allocated ID (P2-shl-03) | `Module/DreamEditorModule.h` |
| A62 | Journal retention and fault handling (PER-04, PER-05, PER-11, PER-12): `ReadNamespace` with bMayModify deletes the generations older than the complete one it read (this format only; another major is kept, a bad header becomes .corrupt), and modifies nothing when the newest header that decodes is a newer major or minor, and nothing more once the generation being read turns out to be one (`bNewerMinor`, §10.6; a damaged this-format generation above it may already be set aside, and stale `*.tmp` deleted); `Writer::StartGeneration` deletes `wal.<g>.sjl.corrupt` after 30 days (30 days from when the reader set it aside: ReadNamespace touches the file just before the rename, as a rename keeps the write time; a file with no known write time is kept). A failed commit spends its seqs and is truncated back off the file; when it cannot be, `Writer::NeedsCheckpoint` forces the next rotation and the writer writes SNAPs until then. `FileOps` gained optional `Truncate`, `ReadPrefix`, `ModifiedUtcMs` and `Touch` (the Win64 journal handle is GENERIC_WRITE, positioned at the end). Mixed line-ending exceptions are mapped by line through each ChangeSet (`codec::MapEolExceptions`, called by `FDreamBuffer`) until the AnchorSet lands (slice B: an unreadable generation now counts as damaged and nothing is written above it, A102; older majors become `.v<major>`, A100; a successful rename is the switch, A103; `AnchorSet` exists but has no M1 caller, so `MapEolExceptions` stays) | `dream/JournalFormat.h`, `dream/TextCodec.h`, `Persistence/DreamWin32FileOps.h`, `Buffers/DreamBuffer.cpp` |
| A63 | Output line flags `OLF_PromptFirst` (a REPL or console echo line that is its statement's own first line: '>>>'; `OLF_EchoFirst` stays "first line echoed for the statement") and `OLF_ResultCont` (a result line after the first line of its repr: no "# Result: " decoration in the view or the Unreal log); auto-print sends a whole repr in one write (OUT-06, OUT-07) (PO-04: the view decorates the first line as `# Result: ` and closes the last line with ` #`, A204) | `dream/OutputRing.h`, `_impl/autoprint.py`, `Output/DreamOutputIngest.cpp` |
| A64 | §5.9.3 Purged: only an observed reset reads Purged, meaning `FEditorDelegates::OnMapOpened`, `OnAssetsPreDelete`, or `UTransBuffer::Reset` seen as `OnUndoBufferChanged` with `GetQueueLength() == 0` and `!IsActive()`. The Reset must come right after our `TransactionCanceled` (our transaction open, no state event between them), or at any time after our `PostTransactionFinalized` left our record in the queue (a 100% end, §5.9.4; a reset of an inactive buffer raises no cancel), and in both cases outside `GIsTransacting`: `UTransBuffer::Undo(bCanRedo=false)` also empties the queue, for example a Sequencer export's spawnable restore after its Begin evicted our record, but it broadcasts under `GIsTransacting`, where Reset never runs (`ensure(!GIsTransacting)`), so that run stays Partial (P4-run-01), and an undo without redo that removes our record ends the watch, so a transient End on the emptied queue is not read as Reset (P5-txo-01). A queue shorter at End than before Begin is not evidence, because it also counts a redo stack a script's Begin drops and the budget evictions of an over-budget record (A1-02, A1-01, P3-run-03, P4-run-01) | `Run/DreamTransactions.cpp` |
| A65 | Piece layout: a piece is `{Chunk, Start, Len, Lf, LfIndex}` (20 bytes; the TDD lists four fields), so finding a line inside a piece is O(1); each chunk has an append-only LF index in 1,024-entry blocks; fan-out 32, minimum fill 8 (depth 3 at 50k pieces). A paste over 32,768 units gets a chunk of its own size, smaller ones go to 64K-unit add chunks, a surrogate pair is never split across chunks, and a chunk no piece uses any more leaves the next chunk table (older snapshots keep theirs) | `DreamCore/Private/TextBuffer.cpp` |
| A66 | A batch of more than 16 ranges with ranges × 64 > pieces (Replace All) is rebuilt in one linear pass. Compaction: `NeedsCompaction` is the TDD's piece rule (more than 2 × lines + 4,096 pieces) or a memory rule (chunks over 2 × the text + 1 Mi units); `BuildCompacted` (any thread) makes one chunk and one piece and keeps the version and change log; `InstallCompacted` (game thread) refuses a stale build, another buffer's or one `BuildCompacted` did not make, and changes neither text nor version (scheduling: A144) | `dream/TextBuffer.h` |
| A67 | `ChangesBetween(From, To, EditBatch&)` runs on any thread over weak back-links from each snapshot to the ChangeSet that made it, kept alive by the buffer's history (limit shared with MapLine, 1,000); it returns one batch in From's coordinates, trimmed and never splitting a pair, and false means "write a SNAP". Snapshots of one buffer share an identity object; `TextBuffer` is move-only; `TextSnapshot` adds `ForEachSpan`, `PieceCount` and `TreeDepth` | `dream/TextBuffer.h` |
| A68 | `AnchorSet`: 64-bit ids (slot and generation, slots reused); gravity for Point anchors only; a LineStart anchor follows MapLine's deletion rule, adjacent ranges included (judged in Before, the new start read from After), and text inserted at a line's first column goes above it; Collapse moves an anchor to the edit, Remove reports it through `TakeRemoved()`. No M1 caller yet | `dream/AnchorSet.h` |
| A69 | `UndoTree` ops store copied text, not PieceSpans; a trim at the memory cap walks the undo path once (the 1M-edit round trip went from over 25 minutes to 31 s); an internal reset never makes a save node, and an empty ChangeSet is ignored before the Undo-origin check | `dream/UndoTree.h` |
| A70 | Regex: DreamCore's own bounded backtracking matcher with Python `re` syntax and tables generated from CPython 3.11.8 (`gen_search_tables.py` → `SearchTables.inc`); no `std::regex` (DreamCore has no exceptions), no PCRE2 or ICU seam. Per line at most 10^6 steps, each code point a backreference compares counting one; cancel is polled every 4,096 steps, inside backreferences too; a compile counts one unit per node visited, capped at 4 × MaxProgram (`TooComplex`). `(?a)` with IGNORECASE folds ASCII only, IGNORECASE backreferences compare `_sre.unicode_tolower`, `\B` finds nothing in empty text; `(.+)\1` on a 100k-unit line reports Limit where Python matches | `dream/SearchEngine.h`, `DreamCore/Private/SearchRegex.cpp` |
| A71 | Smart Enter (`SmartNewline`, never empty): `:` indents, return/pass/break/continue/raise dedent; with the caret in the leading blanks the blanks after it are kept (only a blank-only rest of the line is dropped), and with a selection only when its End lies in its line's leading blanks (core-text-M1) | `dream/EditBuilders.h` |
| A72 | The §4.3.3 gap-buffer line array is built, pulled back from G1 (a splice at 100k lines costs 0.3 µs, the plain vector 0.25–0.5 ms), in the private `DreamCore/Private/GapArray.h`, which never self-moves on an empty gap (core-text-M2). A budgeted relex that stops early marks the next line dirty, and the visible-lines pass counts against the budget. The XXH3 line hash stays G1 | `DreamCore/Private/PyLexer.cpp`, `GapArray.h` |
| A73 | T-LEX-001 golden data: the generator joins NAME tokens that the pure-Python `tokenize` splits at combining marks and variation selectors when the join passes `isidentifier()`, skips lines over 20,000 units and files with a lone CR, and writes a stamp line (script hash, interpreter, Lib root) so the output is reused while it matches | `Tests/DreamCore/lex_golden.py` |
| A74 | Timing gates keep the best of three attempts (noise only adds time) and `DREAM_SKIP_TIMING=1` skips them; the corpus is generated (100k lines). T-LEX-008 times the core stages only: Apply plus Record, MapThrough over 1,000 anchors, and splice plus relex with 1 ms minus the splice; the view stages stay G1 (`Dream.Keystroke`) | `Tests/DreamCore/test_perf.cpp` |
| A75 | T-PER-005 is not a libFuzzer target: the MSVC 14.44.35207 install has only i386 ASan/libFuzzer libraries, so x64 `/fsanitize=fuzzer` fails with LNK1104, and 14.38 (which links) is not UBT's toolset. Deterministic mutation fuzzes stand in (journal reader, history index and blobs); T-CORE-003 runs without ASan. Installing the x64 component is an owner decision | `Tests/DreamCore/` |
| A76 | **Ruling** (refines the TDD §5.7.3 pseudo-code): a pending call the fast chain re-queued re-delivers without the 250 ms gap only when no exception at all is being handled: `bRedeliver = !bHandling && ((bChained && !bAnyHandled) \|\| Now - LastDeliveryAt >= 250 ms)`. With ours handled the chain ends (RT-06); with a foreign one handled, such as the GeneratorExit of a generator closed during the unwind, the 250 ms rule applies; only the check before the pump uses the chained flag (T-CAN-003, run-M1) | `Run/RunSupervisor.cpp`, `Run/RunSupervisorImpl.h` |
| A77 | Audit API: fields are a `dream::json::Value` (no Json module), `Event(const TCHAR*, const dream::json::Value&)`, and `RecordLostRun(s)` take `FDreamLostRun` (which gained `RunId`), because a lost record needs route, number and start time. Lines wait until the first tab spawn decides the role; a session that never opens the tab writes them at shutdown, or when the queue is half full, under the secondary-style name | `Diagnostics/DreamAudit.h` |
| A78 | Audit records: `run.start` at Running (§5.2.3), so a not executed run gets only `run.end`. `source.sha256` of a clean buffer is its base version's hash; Run File hashes the bytes it read (in the read pass) or the base after the step-4 save; a dirty partial run gets a placeholder that the writer thread replaces with the whole buffer's hash; `code.sha256` is the compiled UTF-8 (Run File: the file's bytes); `trust` is "n/a" until M4; lost runs carry nulls, `previous_session: true` and `paused`. Routes: Mcp → `agent`, Shelf and Snippet → `shelf`, Api → `api` (kind `file` for a file target), console executor → `console`/`repl`, REPL → `user`/`repl`, other tab routes → `user` | `Diagnostics/DreamAudit.cpp` |
| A79 | The audit has its own writer thread (`DreamAudit`, `TPri_BelowNormal`), not the journal writer's lane: a 250 ms flush, files rolled at 16 MB and at the UTC day; the sweep goes by the day in the file name, 30 days until the M4 policy keys, primary only, never today's file. An unwritable folder gives one Slate notification and one run-0 warning line per session (the doctor item is M4); shutdown waits at most 1 s, then abandons the writer behind a fence | `Diagnostics/DreamAudit.cpp` |
| A80 | Crash keys, set only on the game thread (every call checks `IsInGameThread()`): `Dream.State` in English U2 names; `Dream.Run` is "N/" plus the first 8 hex digits of the audit session GUID; `Dream.TabHash` hashes the buffer's 32-hex ID (empty for non-tab runs); `Dream.LastPumpMs` counts from entry into `_run`; `Dream.Pause` is always "none" in M1. Run keys clear at run end, every key at shutdown | `Diagnostics/DreamCrashContext.cpp` |
| A81 | Trace channels are `DreamPump` and `DreamRun` (a channel name is a C identifier, so `Dream.Pump` is impossible), with events Pump, RunBegin, RunEnd and Deferral; the existing CPU scopes sit on them, and the counters (`Dream.Pump/*`, output's `Dream.Output/*`) stay on the counters channel | `Diagnostics/DreamTrace.h` |
| A82 | The "Starting… waiting for the editor to become idle" text shows only while the run waits for an idle end of frame (steps Queued, Prepare, Enter), after `DreamRunText::StartingWaitNoticeSeconds` (1 s); the journal snapshot and the Run File save have their own bounds and do not show it (PO-04: with the status line hidden the text shows in the input tab row, A181) | `Run/DreamRunTypes.h`, `Shell/SDreamStatusStrip.cpp`, `Shell/SDreamStatusItem.cpp` |
| A83 | §5.10.2's deferral row: `_dream.check_cancel` gained an optional `deliver` bool instead of a new name (T-API-001 fixes the `_dream` names); `dream_editor.check_cancel()` and `sleep()` return True instead of raising under a deferral, and Python adds the import check C++ cannot see (frozen importlib frames within 64 frames). The plain call no longer counts a delivery | `Python/DreamNativeModule.h`, `native.py`, `_impl/task.py` |
| A84 | Selection aliases: Python reads the selection through `unreal` (`EditorActorSubsystem.get_selected_level_actors`, `EditorUtilityLibrary.get_selected_asset_data`, `UnrealEditorSubsystem.get_editor_world`), and `subsys(cls)` is a function returning the editor subsystem, else the engine subsystem. C++ only decides when: at prepare, on an idle tick 250 ms after `USelection` events, and on a map change, which also releases the last-run namespace; the handlers run no Python. Alias routes: selection, line, cell, buffer, repl, console, api with a tab target, bridge (Run File semantics get none); writes compare element by element, so an unchanged selection neither rewrites nor moves the epoch; setting `bSelectionAliases` | `Python/DreamPythonAliases.cpp`, `_impl/aliases.py` |
| A85 | T-API-006: the digest is the first 12 hex digits of SHA-256 over the snapshot's canonical `names` section; the CHANGELOG line is `API: <version> <digest> <what changed>`, enforced at tier 2, and `--update` regenerates the snapshot | `Tests/python/api/dream_editor_api.json`, `CHANGELOG.md` |
| A86 | [Run in transaction] shows only for a finished run in the history that executed without a transaction (not for not executed or lost runs); the override wins over the toggle, and a refused re-run shows its reason in the tab's status line (PO-04: an entry of the history's context menu instead of a footer chip, A209) | `Run/DreamRunService.h`, `Shell/DreamOutputRows.cpp` |
| A87 | Toggle seeding (replaces A56): an untouched "One undo step per run" toggle follows `bTransactionSelectionAndLine` for selection, line and cell runs and `bTransactionBuffer` for buffer and API runs; Run File keeps its default ANDed with the toggle; `bOneUndoStepPerRunSet` marks a touched toggle in the view and ini tab state. The toolbar shows a mixed state while the toggle is untouched and the two settings differ, and a click from mixed turns it on and sets it. The ini restore reads only the set flag; the journal (SESSION) path counts a value that differs from its buffer-run seed as set (PO-04: the toggle is its command's own entry in the Command menu, mixed state and chord included, A180) | `Run/DreamRunService.cpp`, `Shell/SDreamTab.cpp`, `Shell/SDreamTabToolbar.cpp`, `Settings/DreamUserSettings.h` |
| A88 | **Ruling**: `_dream.hard_object_props` and `_dream.container_element_kind` are M1 (§12.1.1 points them at §7.5.2; T-RES-026 is M1). `hard_object_props(py_class_name)` returns the normalised names of object, class and weak object properties (soft, soft-class and lazy excluded; the first `ScriptName` item, else the C++ name minus the "b"/"In" prefixes, lower case without `_`; colliding pairs dropped), with a length guard at 1,024 characters, a find-only FName lookup and a cache of native classes by `FObjectKey` (4,096 entries; Blueprints read fresh). `container_element_kind` returns None in M1, because the container wrappers are private to PythonScriptPlugin; `Deferred (M2)` | `Python/DreamNativeModule.cpp`, `native.py` |
| A89 | A dynamic Python lifecycle: `DecideUnstartedState()` starts Dream Disabled ("This project starts Python on demand.", Enable Python offered) when Python is configured but unavailable, or when `Engine.Python.IsPythonLifeCycleDynamic` is set and `Py_IsInitialized()` is false; Dream never enables Python by itself. The three Engine.Python CVars it reads are looked up once and cached (PO-04-R1: with the status line hidden, "Python not available" shows in the input tab row's health slot, A186) | `Python/DreamPython.cpp` |
| A90 | M1 describes dream-check with `LspClientOptions`; the §7.2.5 profile files and `NeutralConfig` come in M2 with the type server and ruff | `dream/LspClient.h` |
| A91 | Transport: spawn mode Engine only (Suspended spawning, the SHA-256 check and the protocol trace file are M2). The 2 s escape check counts a process as the server's child only if it was created at or after the server, using one handle for the time and the kill, and scans nothing without a job (lang-M1); stderr goes to the log, or to `<serverId>.<pid>.log` when another editor holds it | `Language/DreamLspTransport.h/.cpp` |
| A92 | The LSP log folder follows the journal's role at each spawn: `<sec>/Logs/LSP` for a secondary, `<root>/Logs/LSP` for the primary and a compat session; before the journal opens, the root from `FDreamInstances::GetResolvedRoot()`, and no log file (stderr still drained) when there is no usable root | `Language/DreamLanguageServices.cpp` |
| A93 | Start and documents: one health value for the one M1 server, shown in the Problems header with [Restart] and [View log] when Paused (the status-bar item is M2); `OnDeferredStart` after every good boot; dream-check spawns once the snapshot key exists, so it gets `--data` and `--key`; every open buffer but the REPL input is opened (scratch URI `file:///<Project>/Intermediate/Dream/Scratch/<Name>-<id8>.py`), each ChangeSet posted at once as one `didChange`; a 1 s check re-sends the `"dream"` configuration because the tab toggles raise no event. Backoff 1, 2, 4 s up to 60 s, Paused after 5 crashes in 3 minutes. `SetRunActive` only holds result application and snapshot slices (M1 has no live language feature) (PO-04-R1: with the status line hidden, Paused also shows as "Lint paused" in the input tab row's health slot, A186) | `Language/DreamLanguageServices.cpp` |
| A94 | Parse items come from `FDreamRunService::OnRunFinished`: a not executed run with a location; dedup class `syntax`; they clear at the buffer's next Starting and when their line is edited. Runtime items stay in `FDreamRuntimeProblems` (A34) with the class `runtime:<run>:<type>`, which never merges | `Language/DreamProblemsModel.cpp` |
| A95 | Quick fix: Ctrl+. (command QuickFix) asks for actions on the whole caret line and shows a menu at the caret, as the Problems context menu does for an item; undo label "Quick fix: <title>"; a fix computed for an older version is refused with "try again". F8 follows the visible order (open buffers in open order by line and column, then runtime items of files that are not open), starts after the caret, wraps, and breaks ties by the last walk index | `Language/DreamLanguageServices.cpp`, `Language/DreamProblemsModel.cpp` |
| A96 | Snapshot: the key text adds `format=dream.stubsnap/1`, `walker=2` and `stubgen=none` to the §7.4.3 inputs; a `.used` marker orders keys and the two newest are kept; `stubs.step` returns its output, so `_dream.stub_emit`/`stub_module_done` are not needed; lines are buffered on the game thread and appended by the `DreamStubSnapshot` pipe in 1 MB batches, and a file is renamed only after a clean close; a failed slice stops the walk for the session. `IsGated()` before each step counts a violation if a run is active, the pump depth is above 0, a modal is open or a Dream keystroke (REPL included) came within 300 ms (lang-M2) | `Language/DreamStubSnapshot.cpp`, `Python/DreamPythonSnapshot.cpp` |
| A97 | CLEAN_TEXT (0x16, skippable) carries a scratch buffer's clean text, its text when this session restored it: head `{uuid, text_crc32c, same}`, no body when `same` (the text equals the SNAP before it). It is written in every checkpoint and when the clean text is set later (Leave safe mode), because §10.7.1 needs it after a crash and the journal is the only crash-safe place for it | `dream/JournalFormat.h`, `Persistence/DreamJournal.cpp`, `DreamJournalWriter.cpp` |
| A98 | History index: magic `SUMIHIDX`, records ENTRY 0x50, TOMBSTONE 0x51, CLEAR 0x52 in `index.<g>.shi`, reusing the journal's header, framing and Writer; each session starts a new index generation in one go and the reader deletes older ones; entry ids are random GUID hex, merged by id. Blobs: SHA-256 addressed, a 32-byte SUMIBLOB header with a chunk size and its own CRC, zlib in 1 MiB chunks (`u32 compressed, u32 raw`), `.tmp` then rename, deduplicated | `dream/HistoryFormat.h`, `Persistence/DreamLocalHistory.cpp` |
| A99 | History entries: the save entry rides on the journal's META commit (`OnBufferSaved`), not on a buffer-manager touch point; the run entry is added once the pre-run commit is durable, with the compiled text, and merges with the buffer's entry just before it in time (`utc <= New.utc`), every note getting a strictly increasing time (`DreamHistoryNoteUtcMs`; the T-PER-017 race); closed entries only for a non-empty scratch buffer closed with reason "closed"; a recovered entry is not repeated. Discard resolves a recovery only once its `discarded` entry is durable; Restore adds a `restore` entry with the text it replaced | `Persistence/DreamJournal.cpp`, `DreamHistoryTypes.h`, `dream/HistoryFormat.h` |
| A100 | Rotation in units (B10): each step is one write of at most 4 MiB to the `.tmp` or the flush after it, records encoded as written; commits keep going to g, and every durable batch committed meanwhile is appended to the `.tmp` in one flush before the rename. The session's first generation is written in one call, since every commit waits for it. Before deleting g-1 its header is read: this format deleted, an older major kept as `.v<major>`, a newer format untouched, an undecodable header set aside as `.corrupt`; `.v<major>` and `.corrupt` files expire 30 days after they are set aside, checked at `StartGeneration`; `KnownOlderMajors()` is empty (format 1 is the first) | `dream/JournalFormat.h`, `DreamCore/Private/JournalFormat.cpp` |
| A101 | Background slices: hash 1M UTF-16 units, compress one chunk, one index commit, one rotation step, list 16 of the 256 `objects/` folders, commit 1,000 tombstones or 200 deletes; results return through a 4 Hz `FDreamDeferral` ticker that waits while a run is active. A slice that waits out a held file serves due commits meanwhile (`FScopedRetryWait`, `ServeCommitsFor`). At exit the writer drains queued history work until OnPreExit + 2 s (`RequestStop(DrainUntilSeconds)`), and `FDreamIoContext::bExiting` stops retention, Clear, index rotation and merges | `Persistence/DreamJournalWriter.cpp`, `DreamLocalHistory.cpp`, `DreamWin32FileOps.h` |
| A102 | **Ruling**: a generation the reader cannot read is damaged: it is kept and never deleted, and nothing is written above it while it is in place. `ReadAll`, `ReadPrefix` and `OpenAppend` retry with the §10.11 backoff (about 0.9 s at most per call); a listed journal generation still unreadable makes the reader fall back to an older one, and only the start-up set-aside of A173 may move it; while a newer generation is in place the writer starts nothing (health Degraded, commits held, run snapshots fail with the reason, retried every 1 s); a newer major or minor above is never renamed and makes the journal read-only (PO-04-R1: with the status line hidden, the journal's health shows in the input tab row's health slot, A186) | `DreamCore/Private/JournalFormat.cpp`, `Persistence/DreamJournalWriter.cpp`, `DreamWin32FileOps.cpp` |
| A103 | A successful rename is the switch (persist-M4): after the rename the writer is on g+1 even if the open fails (`bHandleLost`), `Commit` re-opens the current generation first, and the next rotation goes past to g+2; the journal and the history writers share this | `DreamCore/Private/JournalFormat.cpp` |
| A104 | **Ruling**: local history pauses and never latches. An unreadable index generation is left in place and blocks the store (`bBlocked`), which re-reads it every 1 s; `AddEntry` waits at the front of the lane and gives up only in the exit drain; retention and Clear skip their run; `IsPaused()`/`OnPausedChanged()` drive a banner line. The pause ends on its own after the release (A172). Only a newer index format still makes history read-only for the session | `Persistence/DreamLocalHistory.cpp`, `Shell/SDreamTab.cpp` |
| A105 | **Ruling** (amends slice A's below-normal writer): the journal writer and the storage probe threads run at `TPri_Normal`; the writer's "lower-priority lane" is logical (slices between commits), not an OS priority. A below-normal writer was starved by local compiles, and two pre-run snapshots missed their 2 s bound | `Persistence/DreamJournal.cpp`, `Persistence/DreamStorageProbe.cpp` |
| A106 | **Ruling**: no sleeping file I/O on the game thread; the restore guard reads and writes under `FDreamWin32FileOps::FScopedNoRetryWait` | `Persistence/DreamRestoreGuard.cpp`, `DreamWin32FileOps.h` |
| A107 | Restore guard: `<root>/state/restore-guard.json` under the small-file rules (`.tmp`, flush, replace with `.bak`; fall back to `.bak`, then defaults; unknown keys kept; a newer schema never rewritten). Pending ≥ 2 enters safe mode, else pending + 1 is flushed before any restore work; it returns to 0 ten seconds after the import settles, at clean shutdown and on Leave safe mode. It counts only at a spawn with the journal open and no switch or Shift; an automation session never enters safe mode by the guard (a count of 2 or more restarts at 1); when Python never imports, the 10 s start at the restore. Safe mode needs no presentation override in M1 (runs use the Run window, A28) | `Persistence/DreamRestoreGuard.cpp`, `Shell/SDreamTab.cpp` |
| A108 | Scratch counting: after an unclean exit a scratch buffer counts when its recovered text differs from its clean text (empty for one created in the crashed session); an earlier start's pending scratch waits over the text its live SNAP holds. Counted scratch buffers show the clean text read-only under "Not restored yet" with the pending bar (+a −r lines from `dream::Diff`); a conflict tab gets [Restore] [Compare with disk] [Discard], then [Compare with disk] [Save over…] with an inline confirm | `Shell/SDreamTab.cpp`, `Shell/SDreamTabRecovery.cpp` |
| A109 | The Review panel sits under the banners, above the output and code areas, because the right side bar does not exist yet; rows show name, path, +/−, conflict flag and last edit time; Restore goes through `FDreamUndoHistory::ReplaceAllText` (one undoable edit); Restore as new tab opens "<name> (recovered)" before resolving and closes a conflict tab; Save over confirms inline (PO-04: File ▸ Review Recovered Buffers opens it too, A198) | `Shell/SDreamRecoveryReview.cpp`, `Shell/SDreamTabRecovery.cpp` |
| A110 | A held `instance.lock` makes a secondary that journals in `journal/i-<pid>-<process start UTC ms>/` under its own `ns.lock`; the id is `FDreamAudit::MakeSecondaryId()`, so journal and audit names match, and `SetSecondaryIdForTest` lets one process run several journals on one root | `Persistence/DreamJournal.cpp` |
| A111 | Lock files are opened with `FILE_SHARE_READ` so readers can see the holder, with JSON content `{schema, pid, process_start, host, plugin_version, namespace, utc}` written through the handle; `IsLockHeld` never creates a missing file; the orphan scan and the cross-root delete open `ns.lock` only if it exists (a folder without one is a starting namespace and is never taken) | `Persistence/DreamWin32FileOps.cpp`, `DreamJournalWriter.cpp` |
| A112 | A secondary ignores the ini's tab list at restore (it is the primary's session; the secondary starts with its own scratch tab) and never writes `OpenTabs`/`ActiveTab`; its history Query and LoadText see only its staged entries; its REPL entries, those made before the role was known included, go to `<sec>/repl-history.jsonl` with the `history.jsonl` header, and the merge dedupes by id, sorts by time, applies the text dedupe and rewrites the file once (imported console entries carry time 0 and stay first) | `Shell/SDreamTab.cpp`, `Output/DreamReplHistory.cpp` |
| A113 | An orphan offers a non-empty scratch buffer, a file buffer that differs from disk or whose file is gone, and every unresolved pending recovery, with or without CLEAN (only the section title differs). A same-root namespace that offers nothing is merged and deleted without being listed; a cross-root one is left alone. Resolution lasts for the session; deletion follows the history copy and the merges, under the held `ns.lock`, and any failed step keeps the namespace: a target-side blob failure, an unreadable source blob or index, an unreadable staged REPL file, a `history.jsonl` that cannot be written (`MergeEntries` reports only after the rewrite lands) | `Persistence/DreamInstances.cpp`, `DreamLocalHistory.cpp`, `Output/DreamReplHistory.cpp` |
| A114 | Orphan actions: Restore of a scratch buffer opens a new scratch tab; Restore of a file opens it and replaces its text as one undoable edit with a history `restore` entry, or gives a dirty "(recovered)" tab when the file changed or is gone; Restore as new tab gives "<name> (recovered)", Open a copy "<name> (copy)" | `Shell/SDreamTabRecovery.cpp` |
| A115 | **Ruling**: an orphan namespace with any generation the reader skipped as unreadable is kept: `RecoveredNamespace::bSkippedUnreadable` makes `ReadOrphan` return empty (not listed), and `FMergeTask` fails on the source's flag, so it is never merged or deleted | `dream/JournalFormat.h`, `Persistence/DreamJournalWriter.cpp`, `DreamLocalHistory.cpp` |
| A116 | Compat: when `main` was written in a newer format, the writer takes `journal/compat-<FormatMajor>.<FormatMinor>/` under its own lock and journals there; `main` stays byte-identical and the banner names the newer format. The build restores from that namespace's previous session (only the first compat session restores a newer minor's `main`, read-only); a compat session lists no orphans, and a newer build lists `compat-*` as an orphan | `Persistence/DreamJournalWriter.cpp` |
| A117 | The override probe runs at engine init, because run logs, the audit and REPL history need the root there (a deviation from §10.1, where the tab spawn waits for it), and does not open `instance.lock`, which stays at the first tab spawn (A39). It creates the folders, checks `owner.json` against the account's SID (written at first use; an empty SID counts as another account) and proves the root writable within 2 s; the fallback order is override, platform default, temp, each fallback with a persistent notice; probe completions are drained only by `FDreamInstances::Tick` | `Persistence/DreamInstances.cpp`, `DreamStorageProbe.cpp` |
| A118 | `projects.json`: its lock retries for 1 s and a lock older than 30 s is removed once as stale; `pending_imports` and `decided_imports` live on the project's entry (an offer stays until answered, the answer is remembered); the cross-root scan reads the root recorded before this start's update; `file_id` is `%08x:%016llx`. Import copies only the generation files of namespaces nobody holds, to `imported-<oldhash>` or `imported-<oldhash>.<ns>`, creates `ns.lock` after the copy, refuses unsafe hashes and leaves the old root as it was | `dream/StorageIndex.h`, `Persistence/DreamInstances.cpp` |
| A119 | **Ruling**: `-DreamStorageRoot=<dir>` sets the root for one session (drivers and tests), never writes `projects.json`, and is strict: an unusable root gives an error notice and no journal, never the platform default; `GetResolvedRoot()` lets the G6 step refuse to type anywhere else | `Persistence/DreamInstances.cpp`, `Module/DreamEditorModule.cpp` |
| A120 | `SnapshotForRun` carries every buffer's pending edits, not only the run's buffer, because the 4 Hz scheduler is deferred while a run executes and a kill during a long run would otherwise lose more than 2 s of typing in other tabs (PER-01; found with the G6 driver) | `Persistence/DreamJournal.cpp` |
| A121 | The G6 driver runs as the console command `Dream.Test.G6Step`, not as an automation test, because the tier-4 hygiene discards exactly the recoveries it checks; it types through `FSlateApplication::ProcessKeyCharEvent`, flushing the sidecar before every key, and waits for the restore guard to clear before typing (two kills inside the restore window put the third start in safe mode, as §10.8 says) | `DreamEditorTests/Private/Specs/DreamG6DriverSpec.cpp`, `DreamHost/Scripts/run_g6.ps1` |
| A122 | `SDreamTab::RebuildBanners` does nothing once Slate is shut down: a tab still held past Slate shutdown crashed when Python's state changed at exit (the driver's own strong reference, now weak) | `Shell/SDreamTab.cpp` |
| A123 | One undo history per buffer: `FDreamUndoHistory::For` creates it on first use in a static registry keyed by the buffer (weak pointers; closed buffers are purged, so they leave the 256 MB process cap); it records every change through `FDreamBuffer::OnChanged`, whoever makes it, and a Load starts a new root; the code view syncs its widget from `OnModelChanged`; the save point is marked whenever the buffer is clean at the version the history last recorded | `CodeView/DreamUndoHistory.cpp` |
| A124 | The stock undo never runs: its states are cleared after every stock edit, the context menu's Undo and Redo are UndoTree's, and unbound stock undo chords (Ctrl+Z/Y, Alt+Backspace) are consumed as no-ops | `CodeView/SDreamPlainText.cpp`, `CodeView/SDreamCodeView.cpp` |
| A125 | Model changes reach the widget as range edits inside one stock transaction, last range first, with the selection mapped through; above 64 ranges, during IME, or when the widget missed a version, the whole text is set, and `SDreamPlainText::SetWholeText` lays it out at once (`UpdateIfNeeded`), so a key or click in the same frame finds line views; `Construct` does not lay out at scale 1, and a key before the first paint lays out then | `CodeView/SDreamCodeView.cpp`, `CodeView/SDreamPlainText.cpp` |
| A126 | The undo kind comes from the key-down (Backspace and Delete mark the next stock edit Delete; paste, cut, Shift+Insert and Shift+Delete mark it Command), is used once and reset by a printable character; `ClearNextEdit` drops the hint when an edit changed nothing. Delete, Backspace, typing, cut, paste, Ctrl+Backspace/Ctrl+Delete and `InsertAtCaret` over a selection apply as model edits, because UE 5.8's `DeleteSelectedText` drops one line break too many for a range that ends at a line end | `CodeView/SDreamCodeView.cpp`, `CodeView/SDreamPlainText.cpp` |
| A127 | Type-over is kept although EDT-05 lists it for M2: only closers that auto-close inserted are typed over, and only while the caret stays on that line before the closer | `CodeView/SDreamCodeView.cpp` |
| A128 | **Ruling** (amends A27): read-only is read live in the code view, never through UE 5.8's cached attribute. `SMultiLineEditableText` keeps IsReadOnly in a `TSlateNestedAttribute` cached at the last attribute update, which does not happen inside the Run window's pump, so the stock edit paths edited read-only buffers during runs; now `SDreamPlainText::IsTextReadOnly` reads the view's state (`SetLiveReadOnly`), `ApplyBuiltEdit` refuses while read-only, and `HandleTextChanged` takes back any stock edit made while read-only with a LogDream warning (`GetReadOnlyTakeBacks`) | `CodeView/SDreamPlainText.cpp`, `CodeView/SDreamCodeView.cpp` |
| A129 | Undo and Redo are refused while the view is read-only or a run is active; edit groups are refused while a run is active (RT-01) and apply to a read-only buffer only with `bEvenIfReadOnly`; the undo tree marks a run boundary (#N) when a run of the buffer starts at the model's current version; an IME undo group ends on focus loss and before any edit group | `CodeView/DreamUndoHistory.cpp`, `CodeView/SDreamCodeView.cpp` |
| A130 | **Ruling**: regex find and regex Replace All run on a cancellable worker (§4.4); literal search stays on the game thread (plain-mode buffers are under 1 MB). The worker searches a snapshot copy with a `dream::CancelToken` set by a new query, closing the bar or the destructor; results return through the "DreamCodeViewFind" `FDreamDeferral` ticker (every 20 ms) with generation and version checks. Replace All is built on the worker and applied as one undo step only at the version it read (else rebuilt, at most 3 times; an edit does not cancel it), nothing is replaced over 100,000 matches, and every Replace All exit that applies nothing searches again when the shown matches are stale; after a regex Replace the worker searches the edited text and selects the next match from the end of the replacement (codeview-r2-M1). F3 and Replace never wait on the game thread | `CodeView/SDreamCodeView.cpp`, `CodeView/DreamFindSearch.cpp` |
| A131 | Find bar: inside the code view above the text; literals search at once, regexes 150 ms after typing stops; while typing the first match from where the find started is selected; a one-line selection seeds the query; Alt+C, Alt+W and Alt+R (left Alt) toggle the options; Enter and Shift+Enter step and give focus back to the text (an Enter replayed after waiting for the worker does so only if focus is still in the find bar); F3 with no query opens the bar; matches are capped at 100,000 ("m+") | `CodeView/SDreamFindBar.cpp`, `CodeView/SDreamCodeView.cpp` |
| A132 | Go to line is an inline box over the text accepting `L`, `L:C` and `:L`; a line past the end goes to the last line, no column goes to the first non-blank column, and a click elsewhere cancels | `CodeView/SDreamGoToLine.cpp` |
| A133 | Zoom is the tab's view state: 50–300 % in 10 % steps (Ctrl+wheel, Ctrl+=, Ctrl+-, Ctrl+0), font points clamped to 4–160, fractional wheel deltas added into whole notches; the Font size setting applies to open views at once. Tab moves focus (Ctrl+M) is one state for the editor session, off at start and not saved | `CodeView/SDreamCodeView.cpp`, `Shell/DreamEditorStyle.cpp` |
| A134 | Text commands (Undo, Redo, DuplicateLine, Find, Replace, FindNext, FindPrevious, ToggleTabMovesFocus) act on the focused code view and are not bound in output views (`Ctx_Output`); Ctrl+Z in the REPL prompt undoes the prompt, and Find from the prompt goes to the active buffer's view; from the find bar's boxes these commands are refused with a reason | `Commands/DreamCommands.cpp`, `Commands/DreamKeyRouter.cpp` |
| A135 | The self-write stamp is size plus SHA-256, registered before the replace under the normalised, case-folded path and the resolved target, and forgotten when the save fails, when it lands (`SaveAsAsync` success, shell-M1) or after 60 s; a check that started before the landing re-queues through its stale base; a watcher event always hashes the file | `Buffers/DreamFileSaver.cpp`, `Buffers/DreamFileWatch.cpp`, `Buffers/DreamBufferManager.cpp` |
| A136 | Watch timing: 0.15 s quiet time after an event; a missing file is checked again after 0.5 s before it counts as deleted; a failed read is retried 4 times 0.25 s apart; a rescan trusts equal size and mtime only when the mtime is older than 3 s. Every open file is re-checked after each run and when focus returns; read-only attribute changes are noticed only then or at the next write event, because UE's Windows DirectoryWatcher does not ask for `FILE_NOTIFY_CHANGE_ATTRIBUTES` | `Buffers/DreamFileWatch.h/.cpp` |
| A137 | Disk versions: an unloadable one (over 1 MB, binary, UTF-32) gives "<name> changed on disk, but <why>." with [Keep mine] only; a text-equal one (only BOM, line endings or encoding changed) is taken with no undo step and no history entry; the reload's history entry is written after a successful replace, from the snapshot before it. A deleted file stays dirty (`IsDirty()`, not `IsTextDirty()`) with [Save] [Close], and clears when the file returns with the base bytes; META is committed through `OnBufferMetaChanged` on delete, return, Keep mine and reload | `Buffers/DreamBufferManager.cpp` |
| A138 | Bars: Io → [Retry] [Save As…], read-only target → [Make writable] [Save As…], encoding → [Save as UTF-8] [Save As…], each with a × dismiss, aimed at the failed save's path and kept until a save succeeds, it is dismissed or the tab closes. Order: deleted, disk change, save failure, stacked under persist's pending bar; bars never take focus and are rebuilt on `OnFileStateChanged` and on activation. Make writable clears the attribute on the save worker right before the write (PO-04: each bar is one slim line with a mark, text actions and ×, A198) | `Shell/SDreamTabFiles.cpp` |
| A139 | A save writes the format under which the bytes read back as this text, in §4.13 step 4 order: a UTF-16 BOM keeps its encoding; then the PEP 263 coding line (a cookie naming another supported encoding decides the save, and a UTF-8 BOM is dropped then); then UTF-8. With no cookie and a Latin-1/cp1252/ascii buffer, ASCII text saves as UTF-8 and other text is blocked with the Save as UTF-8 offer. Save as UTF-8 writes UTF-8 without BOM, keeps line endings and rewrites a coding line on line 1 or 2 to `utf-8` as one undoable edit. A future "Reopen with encoding" choice wins over this rule | `Buffers/DreamBufferManager.cpp` |
| A140 | Recent Files: at most 15 per project; a file moves to the front when opened, saved under a new path or closed; missing files are greyed, stay clickable with a tooltip and are removed when chosen; the list is saved with the session state, and Remove and Clear save at once | `Shell/SDreamTabToolbar.cpp`, `Settings/DreamPerProjectUserSettings.h` |
| A141 | Badges: language "PY" for scratch, recovered, `.py`, `.pyw` and `.pyi`, otherwise the extension in upper case (up to 4 letters); lock when the buffer is read-only or its file is read-only on disk (the text stays editable); recovered for a pending recovery or a "(recovered)" tab; each badge widget carries a tag (`DreamTab.Language`, `.Dirty`, `.Lock`, `.Recovered`) (PO-04: BUF-02 overridden: no language badge is shown, and lock and recovered are small glyphs shown only while they apply, A182) | `Shell/SDreamEditorTabs.cpp` |
| A142 | The RUN-26 chip is drawn in an overlay at the end of the caret line plus 24 px, only at a statement start (`StatementExtent(caret).First == caret line`, cached per version and line), and is hidden without editor focus, with a selection, when the line is scrolled out, while any Slate menu is open and within 1 s of an edit; the first RunLineAdvance or a click hides it for good and saves the flag at once | `Shell/SDreamTabFiles.cpp` |
| A143 | "Reopen tab" is offered while the closed buffer is in the closed list (the 32 most recent; a non-empty scratch or a file buffer); the click reopens it with its ID and text (`ReopenClosed`, `FindClosedByFilename`) | `Buffers/DreamBufferManager.cpp`, `Shell/DreamOutputRows.cpp` |
| A144 | Compaction is scheduled per buffer in `FDreamBuffer`: a model change arms one `FDreamDeferral` ticker (deferred during runs, polling every 0.5 s); after 2 s idle with `NeedsCompaction()` true it runs `BuildCompacted` on a `UE::Tasks` worker and `InstallCompacted` on the game thread, retrying at the next quiet period; idle buffers cost nothing, and the REPL input and closed-but-kept buffers are covered | `Buffers/DreamBuffer.h/.cpp` |
| A145 | `FDreamCompletionService` is the live half of §7.3.7's `BeginCompletion`/`QueryLiveCompletionCached` and needs no language services in M1 (lang can route to it when the static source lands in M2). Ticker policy per §3.7: live completion Refuse (`RefuseWhileExecuting`; `CanRunNow` without the Python-state check, so Cold still boots Python), prewarm and the name index Defer | `Completion/DreamCompletionService.h/.cpp` |
| A146 | `FUnrealNameIndex` is filled from the resolver's `unreal_names` helper on 2 ms idle slices after boot and after each run; class member lists load on demand into a 512-entry member LRU, with a 256-entry LRU of class tokens; loading from the Stage 1 snapshot is `Deferred (M2)` | `Completion/DreamCompletionService.cpp` |
| A147 | Fuzzy matching: the word-start weight is 40, not §7.5.5's proposed 16 (with 16, "salt" beats `set_actor_location` for "sal", T-RES-016); a whole-label prefix is scored along its contiguous alignment only | `dream/CompletionMerge.h`, `_impl/resolver.py` |
| A148 | The circuit breaker of a chain rooted in a module is keyed by the member it reaches (`module <name>.<first attribute>`), so one slow class does not turn off live results for all of `unreal`; the C++ timer starts after the GIL is acquired (a mid-call GIL hand-off to another Python thread still counts) | `_impl/resolver.py`, `Python/DreamPythonCompletion.cpp` |
| A149 | The popup is a Slate menu pushed without focus, below the caret line and anchored where it opened; the list is cut at 1,000 items; kinds show as letter glyphs; after an explicit Ctrl+Space with nothing yet, a "Loading…" footer shows at once (the 150 ms delay is M2) | `CodeView/SDreamCompletionPopup.cpp` |
| A150 | Keys: the controller catches Ctrl+Space before the REPL hook and the key router (so it works in the REPL prompt), `EDreamCommand::Complete` stays bound (rebindable, consumed elsewhere), the space Windows sends with Ctrl+Space is swallowed, and the key-up of every key-down it consumed is consumed too (`HandleKeyUp`, first in `SDreamCodeView::OnKeyUp`). A typed character is decided one tick later on a one-shot `DREAM_TIMER_EXEMPT(paint-only)` timer (the view places an auto-closed pair's caret after the model change); keys pressed down act at once. A Ctrl+Space list holds for its word: `(` or a space ends it, `.` or a quote starts a new list | `CodeView/SDreamCompletionPopup.cpp`, `CodeView/SDreamCodeView.cpp` |
| A151 | Accept replaces the text from the prefix start to the caret as one undo group "Completion"; a refused accept (read-only, a run active) still closes the popup and consumes the key; Numpad Enter never accepts. The popup closes when a run enters Starting; a Ctrl+Space during a run opens from the caches with the paused footer and never queues the resolver | `CodeView/SDreamCompletionPopup.cpp` |
| A152 | Console executor: a narrowing cache (one more character of the same chain over the same shared list rescores only the previous matches), ranking of the top 400 matches (`nth_element`) and conversion of only the labels it shows; a bare name with an empty prefix returns nothing but still boots Python from Cold; the string-argument context returns nothing | `Output/DreamConsoleExecutor.cpp` |
| A153 | Namespace per buffer (`NamespaceFor`): the REPL input and Session buffers use `session`; an Isolated buffer uses `isolated:<id>` only while the newest executed run was its own Isolated run, otherwise `none` | `Completion/DreamCompletionService.cpp` |
| A154 | Resolver protocol: a string-argument result has no token; the walk stops min(300 µs, budget/10) before the deadline; a chain over 4,096 steps answers at once as incomplete; an instance with a non-empty `__dict__` gets a per-value token suffix `@<id>`; edit-only editor properties appear as rows that insert `get_editor_property("name")`; negative results are cached per (generation, epoch); the instance-dict walk checks the deadline every 256 names, discovery resumes under each call's deadline, `_GEP` is captured once per `unreal` module, and `_gep_cache` holds at most 256 entries for 10 s. A missing `_dream` helper means "do not read" | `_impl/resolver.py` |
| A155 | Thread identity: each Python write carries its `PyThreadState*` as an opaque `uint64` taken under the GIL; device records and Dream's own lines carry 0 and follow whichever thread holds the native ID now; a new (native ID, thread state) pair forgets its origin run and gets a new generation and badge tag, and an old name passes to it only if the label was set after the old thread's last write (`LabelStamp`) | `Output/DreamThreadTable.*`, `Output/DreamOutputIngest.cpp` |
| A156 | Output counters (`Dream.Output/QueueBytes`, `LinesPerSecond`, `ViewBacklogLines`, `EchoSuppressedLines`, `DroppedLines`, `DrainMs`) and the CPU scopes `Dream.Output.Ingest` and `Dream.Output.Drain` use the engine's counters channel, not a channel of their own; `LinesPerSecond` is the last full one-second window and `DroppedLines` adds queue-cap and view-queue drops; `FDreamOutputStore::GetCounters()` is the hook for the later `Dream.Perf.Dump` | `Output/DreamOutputStore.h`, `Output/DreamOutputIngest.cpp` |
| A157 | Echo cap: `SetEchoCap` wins over the project setting until `ClearEchoCap`; a new cap starts a new one-second window, the open window's summary names the cap that held its lines back, and lines echoed under the old cap no longer count against the new one | `Output/DreamLogForwarder.cpp`, `Output/DreamOutputStore.h` |
| A158 | With "Always write run logs" off, the 10,000-line threshold counts output lines only, not echo lines (`FRun::HeldOutputLines`), so a REPL run printing 9,999 lines makes no file (T-OUT-023). `retention.run_logs_days` is a §11.2.2 policy key, M4, with no M1 user setting, so A48 stays | `Output/DreamRunLog.h/.cpp` |
| A159 | Output Find and soft wrap are M1 (§15.2 lists all of §6 under M1 and §6.4 gives them no milestone; OUT-06's M2 search is the filter box), built on the slice-A `SListView` view (A33). Find searches each stored line's own text on a worker over a ring snapshot (`dream::SearchEngine`), skips empty matches, caps at 100,000 matches, searches new lines at most every 100 ms and takes no snapshot when nothing is new, counts by the view's filters, reveals a match inside a collapsed block or folded echo, and is seeded from a selected single line of up to 200 characters | `Shell/SDreamOutputView.cpp` |
| A160 | Find keys (Ctrl+F, F3/Shift+F3, Enter/Shift+Enter, Esc) are hard-wired in the output view, like the REPL prompt keys; the find bar is in the widget tree only while open (a hidden bar broke specs that look for the first focusable widget); rows are rebuilt only when the shown matches or the current match change. Soft wrap is one toggle for the session in the shared Show menu, off at start and not saved; only a line's tail wraps, and truncated results don't wrap (PO-04: with the output tab row hidden, the Show menu is History ▸ Show in Output, A201) | `Shell/SDreamOutputView.cpp`, `Shell/DreamOutputRows.cpp` |
| A161 | The REPL strip lays its prompt out (`RefreshPromptLayout`) after Submit's clear, before a queued caret key and on mouse-down, so keys in the same frame as a whole-text replace find line views (the same-frame Slate crash; the root fix is A125) | `Shell/SDreamReplStrip.cpp` |
| A162 | `_dream_test` (DreamTestLatch) lives in the tests module, registered from `DreamEditorTests/Private/Python`, and includes Python only through `DreamPythonInclude.h`; the scans apply only rules 2 and 6 to the tests module. The product's test hooks (`PostPendingCallForTest`, the `*ForTest` methods, `Dream.Test.G6Step`) cannot be reached from user code | `DreamEditorTests/Private/Python/DreamTestPython.*` |
| A163 | **Ruling**: the rule-5 scan (one entry call) exempts two reviewed call sites: `PyEval_EvalCode` in `run_statement` (§5.3.7 prescribes it, against §2.5 rule 5's literal text; `ENTRY_EXEMPT` allows exactly that one site) and `PyRun_StringFlags("None")` in the ClearAsyncExc placeholder | `Tests/ci/dream_ci_scans.py`, `Python/DreamNativeModule.cpp`, `Compat/DreamPyCompat.cpp` |
| A164 | Input bot and probes: probes are published on Slate pre-tick and modal-loop ticks in desktop (tick-space) coordinates; the bot raises the editor's own covered window without activating it, searches a grid for an uncovered point, checks `WindowFromPoint`, and releases a held left button and modifiers; `EBotStop::Api` stops through the API with no OS input | `DreamEditorTests/Private/Fixtures/DreamInputBot.*`, `DreamTestProbes.*` |
| A165 | The foreign-modal fixture is a custom SWindow, because `EditorDialog.show_message` returns its default without opening anything while automation runs; it closes itself after 20 s (`GetOnModalLoopTickEvent`) and returns "timeout", so a missed Esc cannot hang a pass that holds the UE lock | `DreamEditorTests/Private/Fixtures/DreamTestModalDialog.*` |
| A166 | T-CAN-001/003 time from the SendInput QPC timestamp to a handler mark around the shape, or to `StopEffectiveAt` when the mark is missing; click and Shift+F5 alternate; a run still alive 10 s after Stop is drained by keeping the Interrupt-cleanup flag set, and the case fails with the count | `DreamEditorTests/Private/Specs/DreamG2CancelSpec.cpp` |
| A167 | Driver-only specs (`Dream.Restart.Layout`, `Dream.Lifecycle.Dynamic`, `Dream.Persistence.G6Driver`) log a "NOT RUN" warning in a shared pass. `Dream.Lifecycle.Dynamic` needs its own session with the dynamic CVar set by an `-ini` override (100 cycles by default); the restart driver removes the two layout keys for a fresh profile, keeps their backups in `Saved/Automation/RestartBackup` (restore rules: A176), and each step exits normally one second after the test | `Specs/DreamLifecycleDynamicSpec.cpp`, `Specs/DreamRestartLayoutSpec.cpp`, `DreamHost/Scripts/run_restart.ps1` |
| A168 | **Ruling**: ledger-based tier-4 hygiene (section 6): tests close the tabs they open, and a pass discards only the pending recoveries of buffers that Dream tests opened, never the developer's (tests-M1, tests-r2-M1). Buffers the restore opens are classified by the flag of A175; the pass start discards the listed pending recoveries, then closes their tabs and prunes the ledger | `DreamEditorTests/Private/Support/DreamTestSteps.cpp`, spec `Dream.Hygiene.Ledger` |
| A169 | G5 checks Undo Last Run, Save As and Open always, and Save during a run, at the key router only, never executing them (they undid other specs' runs or opened a native dialog); AltGr layouts are synthesized as Slate events; G5 settles 5 frames before its baselines (dirty-event counting and the clipboard rule: A177) | `DreamEditorTests/Private/Specs/DreamKeysG5Spec.cpp` |
| A170 | Test switches and gates: `-DreamG2Stops=N` (200 by default), `-DreamG2FloodSeconds=60` (T-SUP-004 runs 20 s per phase by default), `-DreamLifeCycles=N`; window ghosting is disabled for T-CAN-020 only, process-wide in test sessions; the A2-01 gate is at most 2 ms (median of 5) for the history switch at the 10,000-entry cap; T-GAP-008's merge-base check reports itself inactive without version control; `run_tier4.bat --args` passes its arguments unquoted (UE ignored the quoted form, so earlier passes ran 200 stops instead of 3) | `DreamHost/Scripts/run_tier4.bat`, `DreamEditorTests/Private/Specs/*` |
| A171 | The "body emits no code" check (`EmitsNothing`) of the regex compile bound is cached per node, so each node is walked once per compile inside the 4 × MaxProgram bound of A70 (the reviewer's nested patterns went from 774 ms to 1.6–13 ms) | `DreamCore/Private/SearchRegex.cpp` |
| A172 | The history pause ends on its own: while paused, `Tick` posts one cheap probe per second (`bProbeQueued`), so the pause and its banner end after the release without other queued work, and the ticker runs in every role (`OnJournalOpened(bRunRetention)` for any open journal; retention for the primary only). A held index that proves a newer format once released clears `bBlocked`/`bPausedNow` and takes the read-only latch, and the waiting entry answers false. The banner says closed tabs and discarded texts are "kept while the editor stays open and written once it is released" (the exit drain ends a paused entry false) (PO-04: × hides the pause line until history stops being paused, A198) | `Persistence/DreamLocalHistory.cpp`, `Module/DreamEditorModule.cpp`, `Shell/SDreamTab.cpp` |
| A173 | The journal reader never peeks at, touches or renames a generation it cannot read, and sets `bSkippedUnreadable` for every unreadable generation, older majors included. Only `SetAsideUnusedNewer`, called once per start by `ProcessStartGeneration`, sets such a file aside, after reading every header, and it renames nothing once a newer format is found (the journal then goes read-only); a scan of a locked generation pays no Touch or rename retries | `DreamCore/Private/JournalFormat.cpp`, `Persistence/DreamJournalWriter.cpp` |
| A174 | Sticky X follows code-placed carets (codeview-r3-M1): `SetCaret` marks the sticky column stale when the view has no line views yet, and `SetSelection` always does; before the next vertical-move key only (Up, Down, PageUp, PageDown, without Alt) `HandleTextKeyDown` calls `RefreshStickyX`, which re-runs the stock `GoTo` at the caret, puts the selection back and runs under `bSettingText` so the typing group stays open. Other keys, a bare Ctrl included, never refresh it, so a wheel-scrolled view does not jump back (round-4 minor); Ctrl+PageUp/PageDown (the tab switch) still trigger it while the column is stale (a round-5 open item). With A125's layout timing (no layout at scale 1 in `Construct`; a key before the first paint lays out then), a restored caret or an `OpenLocation` at a column keeps its column on the first Up/Down | `CodeView/SDreamCodeView.cpp` |
| A175 | `SDreamTab::IsApplyingRestoreForTest()` (a marked `(tests)` touch point) is true while `ApplyRestore` runs: the startup restore, Leave safe mode and `ApplyRecoveryForTest`. The tier-4 hygiene classifies restore-opened buffers from this flag only; `Dream.Hygiene.Ledger` drives it through `ApplyRecoveryForTest`, checks that it is false before and after, and runs the shared pass-start halves (discard, then close and prune) on a test-named leftover | `Shell/SDreamTab.h`, `DreamEditorTests/Private/Support/DreamTestSteps.cpp` |
| A176 | The drivers restore only what they change: `run_restart.ps1` its two layout keys, `run_g6.ps1` the `[/Script/DreamEditor.DreamPerProjectUserSettings]` section of DreamHost's per-project ini (a stale backup is applied to that section only; a section or ini the driver created is removed). In `finally` each first stops its own step editor, after the `UnrealEditor*` and `DreamHost.uproject` command-line check, and waits for it (`run_g6.ps1` up to 15 s), so the editor cannot write over the restored ini; the restart driver's timeout kill stops only its own process | `DreamHost/Scripts/run_restart.ps1`, `DreamHost/Scripts/run_g6.ps1` |
| A177 | G5 counts every `MarkPackageDirty` on the map, already-dirty marks included; `Dream.Transactions.NoChange` counts every mark during its run and fails on any whose stack does not go through the engine's `FUnsavedAssetsTracker`. G5 saves and restores the clipboard only when it is empty or holds text only; otherwise it skips Ctrl+C and Ctrl+X, in its consume-extension loop too, and logs that | `DreamEditorTests/Private/Specs/DreamKeysG5Spec.cpp`, `DreamRunNoChangeSpec.cpp` |
| A178 | **Ruling PO-04** (owner, 2026-11-03): Dream keeps the clean look of Maya's Python script editor and does not look like a C++/IDE code editor. It outranks SPEC and TDD UI rows (precedence PO > RV > RT > U > ADRs). The owner's three answers: the default theme is Darcula (PyCharm's built-in dark scheme), the chrome is Maya-minimal, and errors read Maya style. Features stay: only presentation changed, every hidden part is one View toggle away, and every command and chord keeps working. A179 to A185 are the rows it overrides | section 5 (restyle items) |
| A179 | **PO-04 overrides CFG-04's default**: the default colours are the Darcula theme (`ThemeId = "Darcula"`, the plan's exact values in all 41 slots), not "follows SyntaxHighlight"; following `SyntaxHighlight.SourceCode.*` is now the Unreal theme (A184). The code font is unchanged (A38) | `Settings/DreamUserSettings.h`, `Shell/DreamThemeTable.cpp` |
| A180 | **PO-04 overrides WS-02 (toolbar and shelf)**: a slim menu bar inside the tab (File, Edit, History, Command, View, Help) and one row of small flat icon buttons with tooltips and no labels (A197) replace the toolbar; there is no shelf, checkbox or text button. Menus are built from the command list, so chords show, and hover switches between open menus. Every former toolbar action keeps its command and chord: Recent Files is in File; Run, Run in transaction, One undo step per run (its command's own entry, mixed state included, chrome-run-r6-m5), Undo Last Run, the namespace choice and Reset Session in Command (and the Run button's drop-down); Clear History, Echo Commands, Show in Output, Expand Echo, Fold Echo and the error and problem walks in History. File gains Review Recovered Buffers and Leave Safe Mode (enabled only in safe mode); Edit ▸ Complete focuses the code view and opens completion; View holds the six View toggles, Show Hidden Notices, Theme, Output Position and Zoom; Help holds Dream Settings, Keyboard Shortcuts and About. The new commands `ToggleReplTab` … `ToggleLineNumbers` are unbound and rebindable | `Shell/SDreamTabToolbar.cpp`, `Commands/DreamCommands.h/.cpp` |
| A181 | **PO-04 overrides the WS-05 status bar default**: there is no status line by default (`bShowStatusLine = false`). The caret's `line:col` shows as small muted text at the right end of the input tab row; while the status line is hidden, status messages show muted beside it (chrome R6), and the health slot of A186 outranks them | `Shell/SDreamTab.cpp`, `Shell/SDreamEditorTabs.cpp`, `Shell/SDreamStatusStrip.*` |
| A182 | **PO-04 overrides BUF-02's badges**: input tabs are plain: the file or scratch name, `*` when dirty, a small lock or recovered glyph only while that state applies, and a small "+"; there is no language ("PY") badge. The × shows on the active or hovered tab (bound after the widget exists, chrome-run-r6-m6). The badge data stays in `SDreamEditorTabs::GetBadges()`, and the dirty mark and the glyphs keep the tags `DreamTab.Dirty`, `DreamTab.Lock` and `DreamTab.Recovered` (A141) | `Shell/SDreamEditorTabs.h/.cpp` |
| A183 | **PO-04 overrides the default run-header presentation**: run headers and footers are hidden by default (`bShowRunHeaders = false`); with the toggle on, each is one muted line (A208). Also off by default, each switchable in View and saved per user: the REPL tab and the Problems tab (with both off the output area has no tab row), the REPL prompt strip and the status line; line numbers stay on (`bShowLineNumbers = true`) | `Settings/DreamUserSettings.h`, `Shell/SDreamOutputView.cpp`, `Shell/SDreamOutputArea.cpp` |
| A184 | **PO-04 overrides T-VIEW-014**: its equality check against `SyntaxHighlight.SourceCode.*` applies only to the Unreal theme (`Dream.Theme.Unreal`). The Unreal theme's syntax colours equal those keys exactly, and its chrome, panes and output map to the `FStyleColors` slice B used (theme R9). It follows Editor Preferences ▸ Appearance through `USlateThemeManager::OnThemeChanged` (theme-r6-m5); the copy built at startup may predate the editor's own theme load, and choosing Unreal again re-reads it | `Shell/DreamThemeTable.cpp`, `Shell/DreamTheme.cpp` |
| A185 | **PO-04 overrides A11Y-05** (output R9): in the history, echo and stderr differ from stdout by colour alone: echo has no gutter or `›`, stderr no `[stderr]` badge, and no line has an icon | `Shell/DreamOutputRows.cpp` |
| A186 | **Ruling PO-04-R1** (architect, on chrome-run-r6-M2, chrome-run-r6-m8 and output-r6-M1): PO-04 hides bars, not warnings. With the status line hidden, any state the status strip or a hidden tab shows as a warning or error appears in the input tab row's message slot (right side, beside `line:col`) as short warning-coloured text; a click does the strip's action, and a health warning outranks transient messages. The ruling covers journal Degraded or Disabled, language features paused and Python not available; chrome built it (R14) as, in priority order: Journal retrying (opens the journal folder), Journal off (opens Dream settings), Python not available (retries or enables Python), Lint paused (restarts the language servers); the architect accepted the two journal actions in round r7, and a click on Python not available without Retry or Enable clears only the Python hide key (A219). Hidden banners show again when a new line or state appears, and × hides only what was showing (A198). For a run with no line of its own the same principle gives the outcome line of A208 | `Shell/SDreamTab.cpp`, `Shell/SDreamEditorTabs.cpp` |
| A187 | **FSlateColor copy rule** (theme R1): in UE 5.8 an `FSlateColor` copies its colour, so nothing stores one: widgets read the colour at paint (`FDreamThemes::Color`), bind `ColorAttr`, or use `Brush()`, one solid brush per slot whose pointer never changes and whose tint is changed in place. Shared styles are mutated in place, never while Slate paints. Anything built once with a colour (output rows, cached text styles) subscribes to `FDreamThemes::OnChanged()`, which fires after the styles and brushes are updated, rebuilds, and unsubscribes on destruction | `Shell/DreamTheme.h/.cpp` |
| A188 | Code view colours (theme R2, R12): the code text style's colour is the Caret slot and every token run sets its own colour; the selection uses the style's selected background; a switch re-copies the style and re-marshals, keeping the selection and the caret. The gutter band is painted only in line-number mode; the REPL prompt uses EditorBackground and the history pane OutputBackground | `CodeView/SDreamCodeView.cpp`, `CodeView/DreamPythonMarshaller.cpp`, `CodeView/SDreamGutter.cpp` |
| A189 | **Theme JSON format** (theme R3): `"format": "dream-theme"`, `"version": 1`; slot keys like `chrome.background`; colours `#RRGGBB` or `#RRGGBBAA`, stored as 8-bit sRGB, so all 256 values of a channel survive; every slot is written, in the fixed order of `EDreamThemeSlot` (checked at compile time), and `base` only when it is set. Export, import and export again give identical text (T-VIEW-015). The codec checks hex lengths, caps a document at 1 MiB and its nesting depth, and returns typed errors | `DreamCore/Public/dream/ThemeFormat.h`, `DreamCore/Private/ThemeFormat.cpp` |
| A190 | **Theme import rules** (theme R4 as amended in review r6): a file in the themes folder needs `"format": "dream-theme"` (`ParseReport::bHasFormat`), so another editor's theme file is not loaded as mostly Darcula; `base` may only name a built-in, and without one the base is Darcula, or GitHub Light when `light` is true; a missing id, or one with a control character, becomes the file name. Skipped: a built-in's id, a duplicate id, a file over 1 MB (read through a bounded read), a file not directly in the folder | `Shell/DreamTheme.cpp`, `dream/ThemeFormat.h` |
| A191 | Themes folder and commands (theme R5, theme-r6-m2, m3): user themes load from `%LOCALAPPDATA%\DreamPythonEditor\Themes\*.json`, shared by all engine versions and projects, never in StartupModule (B1); `List()` and `Find()` notice a changed folder listing, so a theme added mid-session can be picked (since r7 at most one listing a second, with the reload deferred, A217). Exports go to the `Built-in` subfolder, which the loader does not read: the console command `Dream.Theme.Export [folder]` (every built-in as `<folder>/<Id>.json`) and View ▸ Theme ▸ Export Built-in Themes; View ▸ Theme ▸ Open Themes Folder opens the folder, and `Dream.Theme.Reload` reads it again. Reload, Export and the export menu entry are refused during runs (RT-01), with a log line | `Shell/DreamTheme.cpp`, `Shell/SDreamTabToolbar.cpp` (`FillThemeMenu`, `(theme)`) |
| A192 | Theme choice (theme R6, R7, theme-r6-m1): a built-in id is chosen at startup with no file I/O; a user id resolves at the first idle tick; a user theme whose file is missing falls back to Darcula and keeps the setting; an unknown id keeps the current theme and logs a warning. Choosing the current theme again does nothing, except Unreal, which is re-read from the editor; ids match case-insensitively. The theme reacts to a settings broadcast only when `ThemeId` itself changed (`LastSeenSettingId`, tracked from first use, headless too), so a View toggle never moves the theme, and `ReloadUserThemes` resolves the setting again. `SetCurrent` is not deferred during runs: only menus and Editor Preferences reach it, and both are blocked during runs | `Shell/DreamTheme.cpp` |
| A193 | The Editor Preferences ThemeId dropdown is a details customization of `UDreamUserSettings` (theme R8), a deviation from the plan's `GetOptions` meta, because scan rule 11 forbids UFUNCTIONs in DreamEditor (D29). One source of entries feeds the menu, user themes included (theme-r6-M1), and the combo's per-frame text uses `FindLoaded`, which never lists the folder (theme-r7-m1, A217); the row holds its property handle strongly: a weak handle expired after `CustomizeDetails`, so the row showed "Multiple values" and a choice did nothing (found by `Dream.Theme.Dropdown`). Register and unregister call `NotifyCustomizationModuleChanged` | `Shell/DreamThemeDetails.h/.cpp` |
| A194 | Palettes (theme R10, R11): Darcula has the plan's exact values; the eight table themes derive their other slots: docstring from string and comment, decorator from function, self as a desaturated keyword, current line as the background lifted 4 %, gutter a third of the way from background to chrome, find as the warning colour over the background, stderr an amber between warning and error, and an accent per scheme (Maya `#5285A6`, GitHub Light `#0969DA`). Line kinds without a slot: Agent and Debugger use notice, Logpoint warning, Evaluate result; run headers and footers use OutHeader | `Shell/DreamThemeTable.cpp`, `Shell/DreamEditorStyle.cpp` |
| A195 | Shared styles (theme addendum, review r6): `FDreamThemes::FlatButtonStyle`, `TextBoxStyle` (EditorBackground fill, ChromeBorder outline, Accent when focused), `ScrollBarStyle` (no track; thumb ChromeHover, ChromeMutedText while hovered or dragged), `TableRowStyle` (transparent; hover ChromeHover; selection Selection with or without focus), `ListViewStyle` (no background: the stock one kept the history dark in GitHub Light) and `ProgressBarStyle` (a border-coloured track) are recoloured in place on every switch and applied to the Dream lists and scroll bars, the find bar, the completion popup, Problems, the recovery review and the Run window (throbber and step bar Accent). The prompt strip's rules are 1-px ChromeBorder (arch-r6-m1), and the REPL history popup is ChromeBackground inside a ChromeBorder outline with ChromeText (output-r6-M2). Still stock: scroll bars inside stock text boxes, dropdown menus and tooltips; the WS-07 item and the Editor Preferences combo sit outside the tab and stay editor-styled | `Shell/DreamTheme.h/.cpp` and the `(theme)` lines |
| A196 | **`FDreamChrome`** (chrome R1, R9): all chrome takes its colours through `Shell/DreamChrome.h`, an adapter over `FDreamThemes` with its own slots (`EDreamChromeSlot`: Background, Border, Text, MutedText, Hover, Selected, Accent, Danger, OutputBackground, EditorBackground, Warning) and flat button, toggle, tab toggle, combo and splitter styles recoloured in place on `FDreamThemes::OnChanged`, so a theme switch needs no rebuild | `Shell/DreamChrome.h/.cpp` |
| A197 | Icon row (chrome R2): engine Slate SVGs loaded by `Engine/Content` path, not by AppStyle key (Run line `DebugStepOver`, Echo `Console`, Line numbers `FileLined`); tooltips, no labels; the buttons are never focusable, and each carries the tag `DreamChrome.<command>` | `Shell/SDreamTabToolbar.cpp`, `Shell/DreamChrome.cpp` |
| A198 | Banner × (chrome R3, R13; chrome-run-r6-M1, m1): every banner line has ×, and × hides only what was showing: recovery lines by their line id (A219; by their text until r7), safe mode by its reason, Python by its state and reason, the history banner until history stops being paused; a new line or state shows again. View ▸ Show Hidden Notices brings hidden lines back and is enabled only while something is hidden. With buffers still pending, × hides only the line: the per-buffer bars stay and File ▸ Review Recovered Buffers opens the panel, so PER-06 holds. The file bars are one slim line each too, with a mark, text actions and × | `Shell/SDreamTab.cpp`, `Shell/SDreamRecoveryBanner.h/.cpp`, `Shell/SDreamTabFiles.cpp` |
| A199 | Focus REPL reveals a hidden prompt strip while it is used (chrome R4): it hides again when focus leaves and the prompt is empty, and a same-call layout pass (`SDreamOutputArea::RefreshPromptVisibility`) lets focus land in the same call. The placeholder reads "Python" | `Shell/SDreamOutputArea.cpp`, `Shell/SDreamReplStrip.cpp` (`(chrome)`) |
| A200 | View toggles (chrome R5, chrome-run-r6-m7) set plain visibility values, so they apply in the same frame; the line-number gutter's visibility is a marked `(chrome)` touch point in `SDreamCodeView.cpp`. When a toggle hides the widget that holds keyboard focus, focus goes to the code view; the output tab row is rebuilt only for the REPL and Problems toggles | `Shell/SDreamOutputArea.cpp`, `Shell/SDreamTabToolbar.cpp`, `CodeView/SDreamCodeView.cpp` |
| A201 | Output tab row (chrome R7, R8): the output tabs are focusable toggle buttons, not `SSegmentedControl` (P4-shl-02); the row is hidden while the REPL and Problems tabs are both off, and Filter then lives in History ▸ Show in Output. A hidden active output tab switches the area to Output, and a saved hidden tab starts as Output | `Shell/SDreamOutputArea.cpp` |
| A202 | `SDreamTab::GetOwnerTab()` (chrome R10): the DreamHost layout docks the Dream tab in the Level Editor beside Details, where an `FGlobalTabmanager` lookup does not find it; `Dream.Shell.TabLayout` falls back to this accessor | `Shell/SDreamTab.h` |
| A203 | **`UDreamUserSettings::SaveKey(FName)`** (chrome R11) writes one key of the section. The View toggles, ThemeId, Echo input and Output position save through it, from `SDreamTab.cpp` too, so a save never writes another setting's in-memory value (a value a spec pinned for its steps, for example). Chrome R12 (review r6) sent it through the engine's `SaveConfig` with `FSaveConfigContext.PropertyNames`; since review r8 it is one checked write that mirrors `Obj.cpp`, with a backoff retry (A218) | `Settings/DreamUserSettings.h/.cpp`, `Shell/SDreamTab.cpp` |
| A204 | History decorations are decided by the view while it builds rows (output R1, `FDreamOutputRow::Deco`/`bDecoEnd`); a drawn row is redrawn when its block grows across drains. The store, ingest and LogPython are unchanged; the forwarder still adds `# Result: ` for LogPython. Results read `# Result: <repr> #`, and a multi-line repr decorates its first line and puts ` #` on its last (A63); a line over 64 KB is decorated once (output-r6-m2) | `Shell/DreamOutputRows.h/.cpp`, `Shell/SDreamOutputView.cpp` |
| A205 | Error and warning blocks (output R2, R3, output-r6-m1): an error block is the consecutive same-origin error lines of a run; a log-origin line starts a new block unless it is indented or a traceback is still being written. Every traceback line is prefixed `# `; the exception line (the last unindented `Type: message` line after an indented frame, source or caret line, taken only while a frame section is open) reads `# Error: <Type>: <message> #`, and without one the first line gets `Error:`. In a chained traceback only the final exception gets `Error:`; `unreal.log_error` lines read `# Error: <text> #`; all in the error colour. A warning and its indented lines get one decoration, `# Warning: <text> #`, with ` #` on the last line | `Shell/DreamOutputRows.cpp` |
| A206 | REPL and console echo kept their `>>> ` / `... ` prompts inline (output R4), because the ruling names only the gutter and `›`. Superseded by A226: the owner dropped them | `Shell/DreamOutputRows.cpp` |
| A207 | Badges and colours (output R5, R8): type badges are gone; thread-name badges and the "back to run" link stay, and the eviction marker lost its ▲. Rows are transparent over OutputBackground (Starship's table-row style had painted `FStyleColors::Recessed`), hover is CurrentLine, selection Selection and the focused-row outline Accent; muted chips use the header colour and decorations the line's type colour; rows are rebuilt on theme and view-setting changes, and the Run window's live view is the same | `Shell/SDreamOutputView.cpp`, `Shell/DreamOutputRows.cpp` |
| A208 | Headers and the outcome line (output R6, R11 to R13, output-r6-M1, m5): run headers and footers, synthetic headers and "Outside runs" are rows only with `bShowRunHeaders`, one muted line each, with transaction notes joined to the header; a collapsed run keeps its header so it can be expanded; toggling keeps the selection. With headers off, a run that ends other than OK shows `# Error: Run #N <outcome> #`, or `# Warning: … #` when stopped (cancelled, or not executed because of Stop), unless one of its own Python exception rows is shown (R12 as refined in round r7, A221; until then any error line of the run, a filtered-out one included, suppressed it). `ScrollToRun` prefers the outcome line, else the run's first row; the outcome line is also the target of REPL links and the context menu | `Shell/SDreamOutputView.cpp`, `Shell/DreamOutputRows.h/.cpp` |
| A209 | History context menu (output R7, R10, output-r6-m4, m6): on any line of a run a "Run #N" section offers Re-run (disabled while the run is running), Run in transaction, Undo this run, Run without snapshot and Open storage settings; a partial undo needs a second choice, "Confirm: undo the recorded part"; the section is left out for a selection across runs. Copy copies the raw text without decorations, a long result whole. Show all / Hide stays inline, and folding stays. `IDreamOutputRowHost` lost `UndoRun`, `IsUndoConfirmPending`, `Rerun` and `RerunWithoutSnapshot` | `Shell/SDreamOutputView.cpp`, `Shell/DreamOutputRows.h` |
| A210 | **Run window teardown activation** (run, chrome-run-r6-m3): `SDreamRunWindow::OpenModal` records the regular window that holds Dream's focus home (the Dream tab's window, else the output area's, else the active window, else the main frame). At finish step 3, after the run's code has ended (RT-05), `Teardown` enables and activates that window before it destroys the Run window and once more after, but only when the Run window itself is the OS foreground window (`GetNativeWindow()->IsForegroundWindow()`); it returns whether it was, and `FRunSupervisor::EndPresentation` returns keyboard focus only then, because Slate focus on an inactive window activated it natively and pushed an unparented tool window behind it. There is no delayed re-focus, which could steal a click made right after a run. The Run window still takes focus when it opens while the editor holds OS focus | `Run/SDreamRunWindow.h/.cpp`, `Run/RunSupervisor.cpp` |
| A211 | Run window restyle (chrome layout, theme colours): one header row (the title is its tooltip; tab and item muted in it), the live view edge to edge between two 1-px ChromeBorder rules, thin meters and a muted footer line; ChromeBackground with ChromeText, OutputBackground under the live view, progress Accent and OutWarning, Interrupt cleanup and the Stop button Danger. The outer 1–2 px frame stays the engine SWindow border (#0F0F0F/#333333) in every theme (arch-r6-m2, accepted) | `Run/SDreamRunWindow.cpp` |
| A212 | **Activation at test start** (tests): the engine's automation controller opens `FMessageLog("AutomationTestingLog")` after every test (and at pass start), which activates a floating Message Log window, so later focus requests into the main window were overridden and focus specs failed by order. At the start of every `Dream.` test the Dream tab's window is activated again unless a modal window is up: `BringToFront(true)` with OS focus, else the native `SetWindowFocus`, which never takes the foreground from another application; there is no early return on Slate's active window, which lags behind `WM_ACTIVATE` (tests-r6-m6). The engine cvar `MessageLog.DisablePopup` is not used | `DreamEditorTests/Private/Support/DreamTestSteps.cpp` |
| A213 | **Pass-level settings guard** (tests-r6-m2): at the first `Dream.` test of an automation run every `UDreamUserSettings` config key is snapshotted from `GConfig` and from EditorSettings.ini on disk; at each test end they are compared as values, and each changed key logs `Error: user setting <Key> changed by <test>: <old> -> <new>` and is restored in memory, in the config cache and on disk. `Tests/ci/check_automation_report.py` fails the pass on those lines, from the report and from `editor.log`. It found the OutputPosition writer: `Dream.Shell.TabLayout`'s Top and Bottom case once restored the config cache but not the file. Rounds r7 to r10 made the snapshot per run, the missing-key rule, `AllowSettingChange`, the persisted snapshot and the disk settle: A224 | `DreamEditorTests/Private/Support/DreamTestSteps.cpp/.h`, `Specs/DreamHygieneSpec.cpp`, `Tests/ci/check_automation_report.py` |
| A214 | Spec settings rules (review r6): a spec that changes a user setting saves it first and restores only the keys it changed, through `SaveKey` or `SetThemeId` (ThemeId taken from the setting, not the current theme, tests-r6-m3, m4), in `AfterEach` and in each case's `Finally`; a spec shows themes or echo for its captures without saving them; defaults are checked on an object built by the constructor, because a stored key would hide a wrong default (tests-r6-M1, theme R13). Test names are ASCII: × or ▸ in a name turned into LogSlate glyph warnings; the `testnames` scan enforces it (A225) | `Specs/DreamShellChromeSpec.cpp`, `DreamVisualCaptureSpec.cpp`, `DreamThemeSpec.cpp`, `DreamTabLayoutSpec.cpp` |
| A215 | `Dream.CodeView.ReadOnlyKeys`'s "while a run pumps" case (codeview, spec only): with the Run window open as an active modal window, `FSlateApplication::SetUserFocus` refuses every widget outside it, so the case cannot focus its target from the pump. It sets `FApp::SetHasFocusFunction` to return false just before it submits, so `OpenModal` opens the Run window as it does with another application in front (no activation, no focus); the engine default returns at the first pump with the window open, and again in `Finally`. All 38 chords go through `ProcessKeyDownEvent`; the product's focus rule is unchanged (RT-05) | `DreamEditorTests/Private/Specs/DreamCodeViewSpec.cpp` |
| A216 | **Stop acts on the press** (architect, after PO-04; the TDD leaves the click method open): the Run window's Stop button uses `EButtonClickMethod::MouseDown` and `EButtonTouchMethod::Down`. A click made while user code blocks the game thread is read after the call returns, when Slate's hover already follows the live cursor; a release-click needs hover, so a cursor moved off Stop in between would lose the emergency control. A stray press only stops the run; no drag-off cancel is needed. Keys are unchanged: the button is not focusable, and Esc and Shift+F5 stop through the pre-processor. `Dream.Supervisor.StopPress` presses Stop 0.5 s into a 3 s native call and moves the cursor away at once; the press is read by the first pump after the call and recorded as Stop during it (TDD 5.8). Only a Stop that first pump processes counts (`StopAfterNativePumps = 1`, chrome-run-r10-m1). Bot rule: a check that asks an editor window something (the foreground window's title is a WM_GETTEXT) waits for the blocked game thread, so StopPress and T-CAN-020 run their checks before the call and only call `SetCursorPos` and `SendInput` during it | `Run/SDreamRunWindow.cpp`, `Run/RunSupervisor.cpp`, `DreamEditorTests/Private/Specs/DreamRunStopPressSpec.cpp`, `DreamG2SupervisorSpec.cpp` (T-CAN-020) |
| A217 | User-theme folder (theme-r7-m1): `FDreamThemes::FindLoaded` looks only at the built-ins and the user themes already loaded, never at the folder, and the Editor Preferences combo's per-frame text uses it. After the first lazy load, `List()` and `Find()` list the folder at most once a second and only note a change; the reload then runs on a deferred `DeferWhileExecuting` ticker, removed in `Shutdown`, so nothing applies or broadcasts inside Slate's layout pass. An explicit `ReloadUserThemes` call, `Dream.Theme.Reload` and the test hook still reload at once. Together with the required `"format"` of A190 and View ▸ Theme ▸ Open Themes Folder and Export Built-in Themes of A191, this is how user themes come and go during a session | `Shell/DreamTheme.h/.cpp`, `Shell/DreamThemeDetails.cpp` |
| A218 | **`SaveKey` is one checked write** (rulings after review r8 and in round r9; chrome-run-r8-m1, chrome-run-r9-M1, m1, m2): it sets the key as `Obj.cpp`'s `SaveConfig` sets it, then calls `GConfig->Flush` once and checks the result; `SaveConfig(Context)` followed by a checked flush wrote the file twice, and the second move over the ini was refused. Under automation (unattended) the engine writes the ini in place, and a held file fails silently; an attended editor writes a temporary file and moves it, with retries. A read-only EditorSettings.ini gets one Warning, "EditorSettings.ini is read-only; Dream settings are not saved", logged once until a write succeeds, and no retry; the next `SaveKey` tries again. Any other failure sets `bWritePending`, and the next attempt is due 0.5 s after the end of the previous one, backing off 0.5, 1, 2, 4 s up to 30 s; a per-frame ticker only compares the time (an uncapped 0.5 s retry timed from the tick's start had stalled a user editor at about 2 FPS). The first failure logs at Log level, and one Warning follows after 5 s pending. A successful write, by `SaveKey` or the retry, clears pending, removes the ticker, resets the timers and logs at Display after a warning. At module shutdown `CancelSaveRetry` checks the pending flag, not the ticker handle (`FEngineLoop::Exit` resets the core ticker first), does one final checked flush and warns if it is refused. `static FSimpleMulticastDelegate& OnFileWritten()` fires on the game thread after every successful write by `SaveKey`, its retry or the shutdown flush, never for the engine's own saves (Editor Preferences). An `ensure` guards the property kinds the mirror supports (the r9 list: one element, no set, no global-config, deprecated or console-variable property, declared on the class itself) and falls back to `SaveConfig(Context)` plus the checked write. UE 5.8 never clears the branch's dirty flag, so every flush rewrites the whole file. Test hooks: `SetSaveFailuresForTest(Count, ESaveFailureForTest::ReadOnly)`, `GetSaveRetryStateForTest()` | `Settings/DreamUserSettings.h/.cpp`, `Module/DreamEditorModule.cpp` (`(chrome)`) |
| A219 | Recovery line ids (chrome-run-r7-m1, m2): each recovery banner line has an id, and × hides by id. The count line keeps its id when its count is updated (a marked `(chrome)` line in `SDreamTabRecovery.cpp`), so restoring one buffer no longer brings the hidden banner back; a new failed Discard is a new id, so an identical repeat shows again. A click on Python not available without Retry or Enable clears only the Python hide key instead of showing every hidden banner | `Shell/SDreamTab.cpp`, `Shell/SDreamTabRecovery.cpp` |
| A220 | Centre splitter grips (chrome R15, review r8 and r9 nits): the line between the history and the input is a 2 px gap with a 6 px hit area (it was 3 px with 5). Each area has an invisible grip at its edge next to the handle (tag `DreamChrome.SplitterGrip`), 2 px on the upper area and 3 px on the lower one, so the history rows and the input tab faces no longer take a press next to the gap. SSplitter's hit detection is the handle plus twice the grip plus 1 px, because each grip's closed edge row fell outside its open hit range, and the lower grip's extra pixel keeps that row off the lower area's content. Review r8 found no stolen click and no layout shift. SSplitter starts a resize only after a mouse move has set its hovered handle, so `Dream.Restart.Layout`'s drag waits two Slate ticks between the move and the press | `Shell/SDreamTab.cpp`, `Shell/DreamChrome.cpp`, `DreamEditorTests/Private/Specs/DreamRestartLayoutSpec.cpp` |
| A221 | Outcome-line suppression (ruling on output-r7-m1, refining R12): with headers off, a not-OK run's outcome line is suppressed only by a shown row of the run's own Python exception block, a traceback or exception line written by that run's Python (an error line without `OLF_FromUnrealLog` or `OLF_FromLogPython` that no filter hides). `unreal.log_error` lines, captured UE log errors and filtered-out rows don't suppress it, so a stopped run or a non-zero exit always shows its line unless a traceback for that run is shown. The rest of A208 holds: stopped means cancelled, or not executed because of Stop, and gets the warning form (R11), and `ScrollToRun` prefers the outcome line (R13). `Dream.Output.MayaStyle` case 9: `log_error` then `SystemExit(3)` shows the line, Stop after a `log_error` shows the warning line, a traceback run shows none, and with the Errors filter on the outcome line appears | `Shell/SDreamOutputView.cpp` |
| A222 | **Stop during a native call** (run, integration; T-CAN-020, latent from slice B): `PumpsSinceNative` and `NativeEndedAt` replace the one-shot `bPumpAfterNative`. An input Stop (not an API Stop) first requested in the first pump after a native interval of 100 ms or more records that interval as Stop during the native call: the one-pump rule of A216 (`StopAfterNativePumps = 1`). The integration fix allowed two pumps, because the click reached Slate one pump late after the flag had been cleared; run traced that pump to the bot's blocking title read (A223), and after chrome-run-r10-m1 (a Stop pressed during pump 1 was attributed to the earlier call) the ruling went back to one pump | `Run/RunSupervisor.cpp`, `Run/RunSupervisorImpl.h` |
| A223 | Input-bot timing (integration, review r10): the T-SUP-002 and T-SUP-003 bot scripts wait for the Stop request after the click, as T-CAN-020 and `DreamBotSteps` already did; a miss fails after 15 s and the API Stop ends the run. The bot had put the cursor back about 1 ms after the click, about 65 ms before the pump read it, and `SButton` clicks only while hovered, so 2 runs in 7 hung (a latent slice-B race). The bot's foreground check reads the window title with `InternalGetWindowText`, which does not block; `GetWindowTextW` on an editor window sends WM_GETTEXT and waits for the blocked game thread. T-SUP-029 does the wheel step first, and its "down on Stop, release outside" pair now stops the run (A216). Run's edits in tests' files are marked touch points: `(run group, PO-04)`, `(run group, A216)`, `(run group, chrome-run-r10-m1)` | `DreamEditorTests/Private/Specs/DreamG2SupervisorSpec.cpp`, `DreamSupervisorBotSpec.cpp`, `Fixtures/DreamInputBot.cpp` |
| A224 | **Settings guard after rounds r7 to r10** (tests-r7-M1, M2, m1 to m4, the r7b disk lag, tests-r8-m1 to m3, tests-r9-M1, m1, tests-r10-m1): the snapshot is per automation run, reset on `OnBeforeAllTestsEvent` and `OnAfterAllTestsEvent`, so a later run never blames the user's own changes on a test. `DreamTest::AllowSettingChange(Test, Key)`: for that test the guard neither flags nor restores the key, and the test's value becomes the run's baseline (`Dream.Restart.Layout` opens `OutputPosition` in its driver steps). A missing key counts as the value of an object built by the constructor, and a change of presence alone is restored silently. A restore writes once and re-applies: it broadcasts `OnSettingChanged` and `OnViewSettingsChanged`, and calls `SetCurrent` for ThemeId and `SetOutputPosition` for OutputPosition. The snapshot is persisted to `DreamHost/Saved/DreamTests/settings-snapshot.json` with the `AllowSettingChange` marks and a stamp, a checksum of the ini's Dream section, updated at each persist and test end and, while a Dream test runs, on `UDreamUserSettings::OnFileWritten()`; the file is deleted at the end of the run. A leftover file from a killed run is restored with an Error per differing key ("changed by an automation run that did not finish") only while the Dream section still matches its stamp; otherwise one warning lists the differences and nothing is written, and a file without a stamp counts as changed. An outside write or an Editor Preferences save leaves the stamp stale, which only warns (the safe direction, ruling in round r9). The disk settle: the cache comparison and restores come first; then, while disk and cache disagree, the guard flushes and re-reads, checking `Flush`'s result with a backoff from 10 ms doubling to 250 ms over about 1 s. Its own failed write logs "this editor's write of EditorSettings.ini failed" and skips the disk comparison; "another editor" is named only after a successful flush and a different read; the baseline takes only a disk value that agrees with the cache, and a lasting disagreement warns once per value. `check_automation_report.py` fails a pass on the guard's Error lines, not on its warnings. Test hooks: `PlantLeftoverSnapshotForTest`, `ResolveLeftoverSnapshotForTest` | `DreamEditorTests/Private/Support/DreamTestSteps.cpp/.h`, `Specs/DreamHygieneSpec.cpp`, `Tests/ci/check_automation_report.py`, `DreamEditorTests.Build.cs` (Json) |
| A225 | **ASCII test names** (integration, chrome r9, tests r9 and r10): automation test names are narrow strings read through the ANSI code page, so a non-ASCII character (×, ▸, §, …) turned into U+FFxx glyphs and LogSlate "Could not find Glyph" warnings when the Message Log drew the names. Twenty names in the buffers, journal, language and undo-run specs were renamed ("§" → "TDD ", "…" → "...") and marked `(tests) ASCII test name`, with the T-ids unchanged. The `testnames` scan in `Tests/ci/dream_ci_scans.py` (the 14th scan) checks plain names, `TEXT`, `Printf(TEXT)`, both sides of `?:` and nested `Describe` names; names held in variables are not followed (10 computed sites, all ASCII today). `Dream.Visual.Capture` fails on mis-encoded captured text and menu labels: a U+FFC2–U+FFF4 lead followed by a U+FF80–U+FFBF continuation, C1 controls and U+FFFD, so fullwidth characters in file or tab names pass (tests-r10-m2) | `Tests/ci/dream_ci_scans.py`, `DreamEditorTests/Private/Specs/*`, `DreamVisualCaptureSpec.cpp` |
| A226 | **No prompts on echo** (owner, 11-04, answering output R4): REPL and console echo draw no `>>> ` / `... ` prompt in the history or the Run window's live view, as in Maya; every echo row is its source line as it is. `DecorationPrefix` lost its `LineInfo` parameter. `OLF_ReplPrompt` and `OLF_PromptFirst` stay in the store, and the run log still writes `>>>` / `...` (§6.2). The REPL prompt strip's own `>>>` gutter, shown only with View ▸ REPL prompt, is unchanged. `Dream.Output.MayaStyle` case 3 checks a REPL run's echo rows | `Shell/DreamOutputRows.cpp` |
| A227 | **PO-05** (owner ruling, 11-04): a directory-structure section and a code-structure section; all unnamed tabs in a scratch folder; root paths chosen in settings, as in PyCharm; the structure section shows only the active tab; tabs save and load; the test scripts in the scratch section. The owner's answers: a left side bar **shown by default** (Project on top, Structure below); the scratch folder **per user, shared by all projects**; "save and load" means **scratch tabs auto-save** and **the open tabs come back on start**; the **sample scripts** used in tests and screenshots go into the scratch folder. View ▸ Project and View ▸ Structure are the one exception to PO-04's "every View part off by default". The side bar follows PO-04's look: theme colours, small muted section titles, 1 px rules, no file icons | `po05-plan.md` |
| A228 | Side bar layout and keys: It sits left, outside the output/code splitter, so moving the output Top or Bottom never moves it. It is 240 px wide by default, at least 160 px. The shown width is capped so the centre column keeps 240 px, while the saved width is kept (review p1 m1). The Project/Structure split defaults to 0.6, clamped to 0.1–0.9. Both splitters are 1 px rules with 2 px grips beside the lists, under their own tag, so they never cover a scroll bar. With both sections off the side bar collapses to nothing. Focus Project and Focus Structure turn their section on first (saved) and have no chord. Keys are routed as for the output view (`Ctx_Focused|Ctx_Output`, p1 m4). A secondary instance saves no width, split or expansions | `Shell/SDreamSideBar.cpp`, `Shell/SDreamTab.cpp` |
| A229 | Roots live in their own settings class, `UDreamProjectTreeSettings` (per-project user config, own section). Its Editor Preferences page "Dream (this project)" shows only Project Roots and Add plugin Python folders, so Reset to Defaults and Set as Default can no longer touch the session state of `UDreamPerProjectUserSettings` (review p1 m5), which stops registering. The old key migrates once. Reset to Defaults refreshes the tree (`PostReloadConfig` → `OnRootsChanged`), and Add Root and Remove Root broadcast `OnObjectPropertyChanged` for an open page | `Settings/DreamProjectTreeSettings.h` |
| A230 | Tree top level, in order: the project's `Content/Python`, whenever it exists: first, shown once even when a user root equals it, and not removable, because Unreal's Python plugin imports from it. This replaces the p1 rule that it showed only while the roots were empty (the architect's ruling in p3); the user's roots: relative paths resolve against the project folder; a missing one shows muted as "(not found)" and is never created; with nothing to show, "No project roots. Add one…" opens a folder picker; Project Plugins, then Engine (A233); Scratches, last, not removable. Within a folder: folders first, then files, case-insensitively. Dotfiles, `__pycache__`, `*.pyc` and `*.pyo` are hidden. Python files use normal text, other files muted, and there are no file icons. Roots and Scratches have no Rename or Delete | `Shell/SDreamProjectTree.cpp`, `Shell/DreamProjectFiles.cpp` |
| A231 | Listing and watching. Nothing is listed in StartupModule; a folder is listed lazily on expand. **Workers:** one task per folder at `BackgroundLow`. At most 4 local and, apart from them, 2 network listings run at once, in separate queues with one key set (p1 m2, p2 m3, p3 m1). A listing on an offline share holds only its own rows until the OS times out. **Retries:** missing top-level items are retried every 2 s while the side bar shows (network ones back off from 2 s to 60 s, p1 M1). A root is listed once more when its watch starts, which closes the race of a file created between the first listing and the watch. **Watching:** DirectoryWatcher with a 250 ms debounce, reacting to added and removed entries only. Hidden names, such as the auto-save's temp file, are ignored (p1 m10). Network roots are never watched (R-p1-11); they refresh on expand, on Refresh (F5 unless the keymap binds a Dream command to it) and on the retry. **Other:** a hidden Project section lists and watches nothing. A folder over 5,000 entries shows the first 5,000 and "… n more". Results wait while a run is active. Expanded folders are saved per project (capped at 1000), and the groups' states as `DreamGroup:*` | `Shell/DreamProjectFiles.cpp`, `Shell/SDreamProjectTree.cpp` |
| A232 | File operations: **The inline name editor** keeps its text and focus on a refused name. It cancels on Esc or a click elsewhere, and closes when code moves focus away while the editor app is active and no menu is open; an Alt-Tab keeps it (p1 M2, p2 m1). **New Python File** adds `.py` only when the name has no extension and never overwrites a file created meanwhile. **Rename** retargets open tabs, including files inside a renamed folder; a save that started under the old path is not applied (A238). **Delete** goes to the Recycle Bin through `IFileOperation` with recycle-on-delete. A progress sink aborts any delete the shell could not recycle, and a drive without a bin is refused, so a delete is never permanent (p1 m3). The confirm is an inline prompt in the message line. **Protected rows:** Rename and Delete are refused on a root, the scratch folder, a folder holding one, and anything under the engine (p1 m9, A233) | `Shell/SDreamProjectTree.cpp`, `Shell/DreamProjectFiles.cpp` |
| A233 | **PO-05-A1** (owner, verbatim: "Also by default scan the engine and project plugins for python content directories to be added automatically to the project" and "This should be a setting that can be enabled and disabled in the editor"). The roots mirror Unreal's Python plugin, so they are what `sys.path` gets: `<content root>/Python` for every mounted content root (`FPackageName::QueryRootContentPaths`), following `OnContentPathMounted` and `OnContentPathDismounted`. `/Game/` is skipped because it is A230's first item. **Groups:** Project Plugins (expanded at first) and Engine (collapsed at first; `Engine/Content/Python` labelled "Engine" when it exists, plus each mounted engine plugin), labelled through `IPluginManager`. A group is hidden while empty. **Existence:** a missing `Python` folder isn't shown; existence is checked on a worker (`CheckFolders`). A folder created later shows after Refresh or a remount. **Engine entries are read-only in the tree:** no New, Rename or Delete ("is part of the engine"), and a user root in the engine's folder is read-only too. **Removing:** automatic entries have no Remove Root. A user root equal to one shows once, as the user's. **Cost:** an entry under a collapsed group is never listed or watched (p3 m2). **The setting `bAddPluginPythonFolders`** (default true) is changed live from three places kept in sync: the preferences page, View ▸ Side Bar ▸ Plugin Python Folders (`TogglePluginPythonFolders`, unbound), and the tree's background menu | `Shell/DreamProjectFiles.cpp`, `Shell/SDreamProjectTree.cpp` |
| A234 | `dream::Outline` uses its own small scanner (strings, comments, brackets, joins), not PyLexer state, so it runs on any thread over a snapshot. It matched Python's `ast` on all 2,533 parseable files of the engine's Lib (84,747 classes and defs) and takes 4.6 ms per MB against a 20 ms gate. **Kinds:** Class, Function, Method, AsyncFunction, AsyncMethod, Cell. **Cells** sit at depth 0 and are the parent of the top-level entries after them. A cell header ends a decorator run. Indented headers count; headers inside brackets or strings don't. **Decorators** fold in at the def's own indentation. **Broken code:** statement-only keywords start a new logical line even inside unclosed brackets. A triple-quoted string still open at the end of the file is closed at the end of its line and rescanned, at most 4 times (`MaxStringRecoveries`). **Ends:** an entry ends at its body's last line, plus trailing deeper comments | DreamCore `Private/Outline.cpp` |
| A235 | Structure section: **Builds:** a tab switch builds at once (on the game thread up to 256 Ki UTF-16 units, otherwise on a worker). Edits rebuild on a `UE::Tasks` worker after 300 ms, and a result older than the buffer is dropped. **Highlight:** the deepest entry holding the caret gets the current-line colour through the row's `GetBorder()`, so selection and hover win (review p1 A1: the custom paint had drawn white). A collapsed or filtered-out caret entry passes the highlight to its nearest visible parent. **Jump:** a click, double-click or Enter puts the caret on the entry's name (mapped with `TextBuffer::MapLine` if the text changed) and focuses the code view. A double-click never collapses a row. **Filter:** typing opens the filter row, which keeps the parents of matches and also matches untitled cells by "(cell)". Esc clears it. **Memory:** collapsed parents and the selection are remembered per buffer for the session, keyed `<kind>:<ordinal>:<name length>:<name>` per level. **No structure:** shown for an empty tab and for a tab whose path doesn't end in `.py`, `.pyw` or `.pyi` | `Shell/SDreamStructureView.cpp` |
| A236 | Scratch folder: **The setting** `ScratchFolder` is per user; empty means `%LOCALAPPDATA%\DreamPythonEditor\Scratches`. The folder is created on first use, not in StartupModule. **The guard** (R-p1-2) refuses a relative path; a drive or share root; the profile folder or a folder holding it; the project folder or a folder holding it; anything inside or holding the engine folder; and a project root or a folder holding one. A refused value warns once per value and the default is used. **UNC paths** keep their leading `//` in every normalizer (scratch, buffers, the file watch, the tree, and the module's API open key; p2 m2) | `Buffers/DreamScratch.cpp` |
| A237 | Scratch files: Every unnamed tab from the UI, `dream_editor.new_tab()` and the recovery new-tab variants is `scratch_N.py`: the lowest free N, made at once with `CREATE_NEW`, so a second editor can't take the name. **Reuse:** a new tab reuses an empty file only if this editor made it and closed it unused this session and no buffer needs it (R-p1-8, p3 m1). **The flag is path-based:** a scratch renamed in the tree stays a scratch, and Save As out of the folder clears it. **Stay in memory:** named API tabs (`new_tab(name=…)`) and the low-level `CreateScratch`. **Recent Files** leaves scratch paths out. **Fallback:** when the folder can't be written, one warning per folder and journaled `Untitled-N`, as before | `Buffers/DreamScratch.cpp`, `Buffers/DreamBufferManager.cpp` |
| A238 | Auto-save. The reasons and timing are in section 6. Rules that keep text safe (review p1 M1, p2): Never a second write while one is in flight. A close during a save keeps the save tracked and saves the newer text when it lands. A refused or failed close save becomes a Closed local-history entry. The exit save also covers closed buffers still tracked. A save result for a path the buffer no longer has (renamed meanwhile) isn't applied, and a scratch file re-saves at the new path. History is throttled to one entry per buffer per 5 minutes for Timer, FocusLost and RunStart (R-p1-3). Close, Exit, Save As, Keep mine, explicit saves and the first save after a failure always add one. No `*` while auto-save is armed, `*` only while it is paused (R-p1-4) | `Buffers/DreamBufferManager.cpp`, `Shell/SDreamEditorTabs.cpp` |
| A239 | Save As on a scratch tab writes the new file and retargets the tab. It writes nothing back into the scratch file, which keeps its last auto-save, at most 2 s old (R-p1-1). The p1 copy could overwrite a pending outside version (review p1 M2) | `Buffers/DreamBufferManager.cpp` |
| A240 | Migration and restored conflicts: Restored journaled `Untitled-<digits>` buffers become scratch files, but only the buffers that restore brought back, and not when the folder comes from `-DreamScratchFolder` (p1 m1). Safe mode's per-buffer restore migrates too. A failed write leaves `Untitled-N` and removes the empty file. A restored dirty scratch tab whose file changed since keeps the journal's base, so every watch check reports Changed and the Keep mine / Reload bar stays until the user answers; auto-save stays paused (review p2 M1) | `Shell/SDreamTabFiles.cpp`, `Shell/SDreamTabRecovery.cpp`, `Buffers/DreamBufferManager.cpp` |
| A241 | Samples: `Resources/Samples/` holds `demo.py`, `output_styles.py`, `long_run_stop.py`, `undo_run.py`, `cells.py` and `structure_demo.py`, built from the scripts used in tests and screenshots. Each starts with a comment saying what it shows and runs cleanly in an empty level. **Seeding:** they are seeded once per user into `<Scratches>/Samples/` by a worker. `bSamplesSeeded` is written only when every sample is in place. **Other folders:** a test or command-line folder gets a `.dream-samples-seeded` marker instead. **Restore:** Help ▸ Restore Sample Scripts copies missing ones. Nothing is ever overwritten | `Buffers/DreamScratch.cpp`, `Resources/Samples/` |
| A242 | PER-02 across real restarts: scratch and file tabs come back in order, with the active tab, caret and first visible line. A scratch edit typed just before exit is in its file. The restart driver found two bugs, both fixed: a restored view's scroll was reset when the view was built; a scroll made only with the wheel was never saved. The saved line is the first line at least half visible | `Shell/SDreamTab.cpp`, `Shell/SDreamTabFiles.cpp` |
| A243 | Network scratch folders: `FDreamFileWatch` registers no directory watch for a network folder, because the engine opens it with a synchronous `CreateFile` on the game thread. Focus return and run ends recheck those files. `CreateNextFile` reads the folder in one `IterateDirectoryStat` pass (review p3 m2) | `Buffers/DreamFileWatch.cpp`, `Buffers/DreamScratch.cpp` |
| A244 | Test isolation: `run_tier4.bat` and the restart, smoke and G6 drivers pass `-DreamScratchFolder=<DreamHost>/Saved/DreamTests/Scratches` and empty that folder first (R-p1-6). Without such a flag, the hygiene hooks point the folder at the same place at the first Dream test and release it after the run (p1 m8). Specs use `SetRootsForTest`, `SetDefaultRootForTest` and the recycle seam, so routine passes never touch the user's roots, the host's `Content` or the user's Recycle Bin. The real recycle case runs only with `-DreamRealRecycleTest`, logging a NOT RUN warning otherwise. The settings guard adds `ScratchFolder`, `bSamplesSeeded`, `bShowProjectPanel` and `bShowStructurePanel`, plus the per-project `ProjectRoots` and `bAddPluginPythonFolders`. `-DreamStructureSplit=<f>` runs `Dream.Shell.Structure` with a short section, in memory only | `DreamEditorTests/Private/Support/DreamTestSteps.cpp`, `DreamHost/Scripts/*` |
| A245 | The module's queued API open key (`dream_editor.open`) keeps a UNC path's leading `//`, as `FDreamBufferManager`'s `NormalizePath` does (architect, review p2 m2) | `Module/DreamEditorModule.cpp` |
| A246 | **Rename and one-time migration (PO-06 phase 3).** Every name moved from Sumi to Dream by one scheme: plugin folder and descriptor `DreamPythonEditor` (FriendlyName "Dream Python Editor"), modules and API macros, the `F/S/U/E/I/T` class prefixes, `dream::` and `Public/dream/`, files, `LogDream`, `Dream.*` console commands and automation tests, tab ID `DreamEditor.Main`, `-Dream*` switches, `dream_editor`, `_dream`, `dream_tools`, `dream-check`, the `<dream:…>` pseudo filenames, ini sections `[/Script/DreamEditor.*]`, style keys, scan names, the host project DreamHost. Visible text says "Dream Python Editor" (tab label, Tools menu, Editor Preferences pages, plugin) and "Dream" where it is tight (status bar, window titles `Dream · Run #N`, transactions `Dream: <tab> #N`, Python (Dream), the toolbar button). Folders: `%LOCALAPPDATA%\DreamPythonEditor\` for storage, scratch files and themes; `<Project>/Saved/Dream` and `Intermediate/Dream` for project-local files. **Kept on purpose:** the on-disk magic `SUMIJRNL`, `SUMIHIDX` and `SUMIBLOB`, so copied journals and history read; the readers also accept the old theme format name `sumi-theme` and REPL-history header `sumi.repl-history`. **Compatibility:** `Content/Python/sumi_editor` is a shim that warns once (DeprecationWarning) and puts the `dream_editor` module itself in `sys.modules["sumi_editor"]`; `Config/DefaultDreamPythonEditor.ini` holds CoreRedirects for the reflected types (settings classes, `FDreamTabState`, the three UENUMs, the test fixtures); the old tab ID `SumiEditor.Main` is registered with `FGlobalTabmanager::AddLegacyTabType`, so a saved layout reopens the tab in its place. **Migration** (`DreamLegacyMigration`, copies only, old paths stay as a backup, idempotent, one Display line when it copied): in StartupModule, without file I/O, the three settings sections are copied key by key in the config cache into absent Dream sections, the settings objects reload, a ScratchFolder equal to the old default becomes empty, and saved paths under the old default scratch folder are remapped (the journal's recovered paths too, `RemapLegacyPath`); at the first `ResolveStorageRoot`, before the root is prepared and the journal opens, the old root `%LOCALAPPDATA%\Sumi\<EngineVer>\<Name>-<hex6>` is copied into the new root when that does not exist (DreamHost maps to SumiHost's hash folder, other projects keep their hash; no `*.lock`, no `project.json`; marker `.migrating-root` resumes a copy cut short), RunLogs follow on a worker (marker `.migrating-runlogs`, stopped at OnPreExit and resumed at the next start), the default scratch folder with Samples and the themes are copied once (markers `.migrated-scratches`, `.migrated-themes`), and the ini files are flushed. Not migrated: a storage-root override (its folder is unchanged; only a renamed project would need its hash folder copied), the temp fallback, `projects.json` (rebuilt). | `Persistence/DreamLegacyMigration.*`, `Module/DreamEditorModule.cpp`, `Persistence/DreamJournalWriter.cpp`, `Output/DreamReplHistory.cpp`, `dream/ThemeFormat.h`, `Config/DefaultDreamPythonEditor.ini`, `Content/Python/sumi_editor/__init__.py`; spec `Dream.Migration`, t1 `Theme_LegacyFormatNameReads`, t2 `test_compat_shim` |

## 8. Build and verification

- **Tier 1**: `Tests/DreamCore/build.bat [--build-only] [--quick] [filter]` runs `vcvarsall x64 -vcvars_ver=14.44`
  (the toolset UBT uses), falls back to 14.38 when 14.44 is missing, and `DREAM_VCVARS_VER` overrides the choice
  (CR-01). It compiles `Source/DreamCore/Private/**/*.cpp` except `Private/UE/` plus `Tests/DreamCore/**/*.cpp` with
  `cl /std:c++20 /permissive- /Zc:preprocessor /W4 /WX /we4456 /we4458 /we4459 /we4668 /we4702 /external:anglebrackets
  /external:W0 /EHs-c- /GR- /D_HAS_EXCEPTIONS=0` (the extra `/we` warnings are the ones UBT makes errors for DreamCore,
  `DreamCore.Shared.rsp`, so tier 1 rejects what the UE build would), links `Tests/DreamCore/out/dreamcore_tests.exe` and
  runs it. Before the run it writes the T-CORE-039 golden data with the engine's `python.exe -I -B` (`DREAM_ENGINE_DIR`
  overrides the engine folder; about 30 s and 15 MB in `Tests/DreamCore/out`, which stays out of version control);
  `--quick` skips that step. `Tests/run_tier1.bat [--scans-only] [build.bat arguments]` runs the `Tests/ci` scans and
  then build.bat with up to three of its arguments, `--quick` included (CR-03).
  Every `Public/dream/*.h` compiles alone and all together with this toolchain at `/W4 /WX` in three modes:
  `DREAMCORE_API` empty (tier 1), `DLLEXPORT` (as UBT builds DreamCore) and `DLLIMPORT` (as DreamEditor sees it). UBT
  defines `DREAMCORE_API` as `DLLEXPORT` / `DLLIMPORT` in the module's `Definitions.h`, not as `__declspec(...)`, so
  `dream/Export.h` supplies those two macros when no engine header has (A50, CR-02); a header check with
  `__declspec(dllexport)` written out cannot see a missing definition (every DreamCore cpp failed under UBT with C2079
  "undefined class dream::DLLEXPORT" until Export.h defined them). The two DLL modes also need `dream/Export.h`'s MSVC
  `#pragma warning(disable : 4251)`: without it every exported class with a `std::` member raises C4251 (review
  finding; UBT's DreamCore build includes no engine header that would silence it). Keep it so: an exported class must
  not need an incomplete type in an implicitly defined member (declare those members and define them in the `.cpp`, as
  `json::Value` does), and a type an exported function returns by value must be complete in its header
  (`dream/Checksum.h` instantiates `Sha256Digest` with a `static_assert`; otherwise a dllimport caller gets C4686,
  CR-11, CR-31).
- **UE build**: `Build.bat DreamHostEditor Win64 Development -Project=…/DreamHost/DreamHost.uproject -WaitMutex`
  (`Tests/run_build.bat`; `-SingleFile=<path>` or `-FileList=<file>` compile single files, headers included). UBT picks
  MSVC 14.44.35229 from the folder `VC/Tools/MSVC/14.44.35207` (14.38 fails on UE 5.8 headers; 14.43 is banned) with
  Windows SDK 10.0.22621. The plugin sits outside the project folder (`AdditionalPluginDirectories: ["../Plugins"]`), so
  its binaries land in the host project's `DreamHost/Binaries/Win64` (`UnrealEditor-DreamCore.dll`,
  `UnrealEditor-DreamEditor.dll`, `UnrealEditor-DreamEditorTests.dll`, their `.pdb` files and `DreamHostEditor.target`),
  not under `Plugins/DreamPythonEditor/Binaries`; the plugin's intermediates are in `Plugins/DreamPythonEditor/Intermediate`.
  The full build of the seven slices succeeds with 0 errors and 0 warnings. With this toolset, review compiled every
  `Source/DreamEditor/Private/**/*.h` alone and all together through UBT's own response files and shared PCH, as
  DreamEditor (dllexport) and as DreamEditorTests (dllimport): no errors, no warnings.
- **Tier 2**: `Tests/run_tier2.bat [-v] [-k substring]` runs the engine's `python.exe -I -B` (no user site, no bytecode
  files under `Content/Python`) on `Tests/python/run_tier2.py`, `unittest` discovery over `Tests/python/test_*.py`; every
  test gets a fresh `NullDream` through `_impl.native.set_for_test` and a Session captured by `_boot` (which must return
  exactly `True`, A53). `UE_ROOT` overrides the engine folder.
- **Tier 4**: `Tests/run_tier4.bat` runs the automation specs. With its default flags (`-unattended -nullrhi`) Dream is
  Headless (§3.4, §3.5: `FApp::IsUnattended`, `!FApp::CanEverRender`), so only `Dream.Lifecycle` runs; the specs that open
  the tab or run code carry `EAutomationTestFlags::NonNullRHI` and need `run_tier4.bat --attended` on an interactive
  desktop (§14.1).

Slice B adds:
- **Tier 1.** `DREAM_TIER1_OUT=<dir>` gives `build.bat` a private output folder outside `Tests/DreamCore` (rule 12).
  Without `--quick`, the build writes three golden sets with the engine's `python.exe -I -B` before the run: T-CORE-039
  extents, T-CORE-017 Python `re` results (`search_golden.py`, `DREAM_SEARCH_GOLDEN`) and T-LEX-001 tokenize boundaries
  over the engine's Lib (`lex_golden.py`, `DREAM_LEX_GOLDEN`; reused from the output folder while its stamp line
  matches, so only the first run there pays its minute, A73). The search tables are generated from CPython 3.11.8 by
  `Tests/DreamCore/gen_search_tables.py` into the checked-in `SearchTables.inc`. `DREAM_SKIP_TIMING=1` skips the timing
  gates (T-CORE-004, T-CORE-009, T-LEX-008 and the two search gates, each best of three, A74). `DREAM_FULL_SCALE=1`
  runs the full-scale random runs: T-CORE-001 at 1M edits (about 35 s), T-LEX-007 at 100k edits per grammar (about
  2 min 46 s), T-CORE-010 at 1M (about 31 s). Every new `Public/dream/*.h` compiles alone and together in the three
  export modes. T-PER-005 as libFuzzer is not built (A75).
- **Static scans (B105).** `Tests/run_tier1.bat --scans-only` runs 13 scans (`Tests/ci/dream_ci_scans.py`): slice A's
  four, plus §2.5 rules 5 (one entry call, with the two exemptions of A163), 7 (compat confinement), 8 (banned APIs),
  9 (ticker gating: raw `AddTicker`/`RegisterActiveTimer`/`OnEndFrame.Add` only through `FDreamDeferral` or after
  `DREAM_TIMER_EXEMPT(paint-only)`), 10 (Build.cs direction), 11 (UFUNCTION) and 12 (manifests), T-GAP-001 (no
  `FText::FromString` or `INVTEXT` on a literal with letters; `loctext_baseline.json` is empty, and a stale entry fails)
  and T-GAP-008 (traceability, `dream_trace.py` with `lands_terms.json`; its merge-base check is inactive without
  version control). The tests module gets rules 2 and 6 only. Review mutation-tested rules 5, 7, 8, 9, 10, 11 and
  T-GAP-001 on a scratch plugin; each reported its violation.
- **UE build.** `DreamEditor.Build.cs` adds DirectoryWatcher (B80), TraceLog (B39) and `Settings` as an include path only
  (B23); `DreamEditorTests.Build.cs` adds LevelEditor, EditorFramework and Projects.
- **Tier 2.** New suites: `test_aliases`, `test_cancel_deferral`, `test_api_snapshot` (the CHANGELOG `API:` rule),
  `test_resolver`, `test_check_rules`, `test_check_server`, `test_docstrings`, `test_stubs`, with fixtures under
  `Tests/python/check_fixtures/`.
- **Tier 4.** `run_tier4.bat --args "<editor args>"` adds editor arguments for one pass (passed unquoted, A170), for
  example `-DreamG2Stops=20` (fewer G2 stops than the default 200) or `-DreamRestartStep=2`. The exported `index.json` is
  checked by `Tests/ci/check_automation_report.py`, since the automation Quit exits 0. The driver-only specs
  (`Dream.Restart.Layout`, `Dream.Lifecycle.Dynamic`, `Dream.Persistence.G6Driver`) log a "NOT RUN" warning in a shared
  pass (A167). `Dream.Lifecycle.Dynamic` (RUN-10, T-LIFE-006, T-LIFE-007) runs in its own session with the CVar on the
  command line: `run_tier4.bat --attended --filter Dream.Lifecycle.Dynamic --args
  "-ini:Engine:[ConsoleVariables]:Engine.Python.IsPythonLifeCycleDynamic=1 -DreamLifeCycles=100"` (`-ini:` overrides are
  not written back to disk). Every pass applies the ledger hygiene of section 6.
- **Drivers.** `DreamHost/Scripts/run_restart.ps1 [-Steps 5] [-TimeoutMinutes 6]` runs `Dream.Restart.Layout` once per
  step (T-GAP-019): it removes the two layout keys for a fresh profile and puts exactly those two keys back afterwards
  (A176).
  `DreamHost/Scripts/run_g6.ps1 [-Kills 5] [-RunKill 3] [-Seed 1]` is the G6 driver B prototype: each session runs
  `Dream.Test.G6Step` with `-DreamG6Step=<k>` on a private `-DreamStorageRoot=%TEMP%\DreamG6-<stamp>`, types, is killed
  with `Stop-Process -Force` (once during a long REPL run, T-E2E-010), and the next start checks I1 to I5 against the
  sidecar (T-PER-036); the `[/Script/DreamEditor.DreamPerProjectUserSettings]` section of DreamHost's per-project ini is
  backed up and restored (A176). Both need the UE lock and an interactive desktop; both kill only editors whose command
  line names `DreamHost.uproject`. Interrupting either with Ctrl+C can still skip its restore until its next run, which
  restores a leftover backup first (STATUS.md open items).
- **Attended desktop.** The SendInput specs (`Dream.G2.*`, `Dream.Supervisor.Bot`, `Dream.Keys.G5` T-KEY-020 and the
  restart driver's splitter drags) send real OS input: the desktop must stay free (no clicks or typing) during the
  pass, and nothing may cover the editor: no system prompt, crash reporter or other application window. The 200-stop
  `Dream.G2.Cancel` pass holds the UE lock for about 40 minutes.

The restyle (PO-04) adds:
- **Tier 1.** `Tests/DreamCore/test_theme.cpp` (the `Theme_*` cases, 6, with 3,150 checks) tests the `dream/ThemeFormat.h`
  codec, `ParseReport::bHasFormat` included; build.bat compiles it with every tier-1 run.
- **Tier 4, headless and attended.** `Dream.Theme.BuiltIns` (every built-in loads, the shared styles in all ten themes,
  an unrelated toggle does not move the theme), `Dream.Theme.Default` (Darcula on a settings object built by the
  constructor), `Dream.Theme.Json` (the round trip and the import rules: the 1 MB cap, a subfolder, no format, a
  control-character id, a theme added mid-session (latent, up to 5 s), a saved theme returning), `Dream.Theme.Unreal`
  (T-VIEW-014 for the Unreal theme) and `Dream.Hygiene.SettingsGuard` (A224: the snapshot file, a silent presence
  restore, `AllowSettingChange`, a change, write, restore and write that must log nothing, both branches of a planted
  leftover snapshot with the re-apply, and the leak case: a value leaked through `SaveKey` makes the guard restamp the
  run's real `settings-snapshot.json` on `OnFileWritten`, and resolving a copy of it logs the Error and restores the
  value in memory, cache and disk), for example `run_tier4.bat --filter Dream.Theme+Dream.Hygiene`.
- **Tier 4, attended only** (`--attended`): `Dream.Theme.LiveApply` (painted pixels after `SetCurrent` and after an
  Editor Preferences edit: background, gutter band, current line, selection, keyword glyphs, the history pane, a Run
  window colour case in Solarized Light; it turns line numbers on, needs the editor window shown, and its skips are
  warnings), `Dream.Theme.Dropdown` (the details layout registered for `UDreamUserSettings` and an off-screen
  `IDetailsView` showing the combo with a user theme; it opens that window briefly), `Dream.Shell.Chrome` (defaults,
  hidden parts, every toggle, every former action reachable with each Show in Output type by name, banner × and Show
  Hidden Notices with the count-line update and an identical repeat, the health slot with a forced journal outage,
  persistence and live theme, no chrome focus at run start and focus back in the code view after it, a press on either
  splitter grip, and the `SaveKey` cases of A218: the backoff with four forced failures, attempts 0.5, 1, 2 and 4 s
  after each end, one 5 s warning and one `OnFileWritten`, about 8 s; a success ending a pending write; a read-only
  ini; the shutdown flush), `Dream.Output.MayaStyle` (9 cases: echo, decorations, a chained traceback and `log_error`, a
  link that navigates, headers off and on, the outcome line, the cross-drain redraw, the context menu with a partial
  undo, a GitHub Light check of the prompt strip rules and the popup, and the outcome-line suppression of A221),
  `Dream.Supervisor.StopPress` (A216: the bot presses Stop 0.5 s into a 3 s native call and moves the cursor away
  10 ms later; the Stop must be requested and recorded as Stop during the call, which fails with the old release-click
  method), `Dream.Supervisor.RunWindowFocus` (2 cases: a floating
  window owned by the main frame, then on Win64 an unowned top-level window that takes the foreground during the run
  and must keep it; it calls `RequireEditorInFront` and fails when it cannot) and `Dream.Visual.Capture` (screens 01–10
  in Darcula and 11–13 in GitHub Light under `DreamHost/Saved/Dream/Screens/`, with structure checks, Darcula and
  GitHub Light pixel checks and the mis-encoding check of A225; `10-main-frame.png` is written only while the tab
  floats), for example `run_tier4.bat --attended --filter
  Dream.Theme+Dream.Shell+Dream.Output.MayaStyle+Dream.Visual.Capture` and `run_tier4.bat --attended --filter
  Dream.Supervisor.StopPress+Dream.G2.Supervisor`.
- **Free desktop.** `Dream.Supervisor.RunWindowFocus` checks the OS foreground window and `Dream.Supervisor.StopPress`
  sends real input, so like the SendInput specs they need the editor in front and nobody using the desktop; in the r6
  passes another application in front (RuneLite, the Alt+Tab switcher) failed them. On a free desktop the final
  verification ran every click case green (STATUS.md).
- **Static scans.** `Tests/run_tier1.bat --scans-only` runs 14 scans: slice B's 13 and `testnames` (A225), which fails
  on a non-ASCII character in an automation test name.
- **Reports.** `Tests/ci/check_automation_report.py` also fails a pass on any settings-guard Error line (`user setting
  <Key> changed by <test>: <old> -> <new>`, or a leftover's "changed by an automation run that did not finish"), in a
  report entry or in `editor.log`; the guard's warnings do not fail it (A213, A224).
- **Console commands and folder.** `Dream.Theme.Export [folder]` writes every built-in theme as `<folder>/<Id>.json`
  (default `%LOCALAPPDATA%\DreamPythonEditor\Themes\Built-in`); `Dream.Theme.Reload` reads the user themes again; both are refused
  during runs. User themes are `%LOCALAPPDATA%\DreamPythonEditor\Themes\*.json` (View ▸ Theme ▸ Open Themes Folder; A191).
- **UE build.** `DreamEditor.Build.cs` adds PropertyEditor for the ThemeId details customization, and
  `DreamEditorTests.Build.cs` adds it for `Dream.Theme.Dropdown` and Json for the guard's snapshot file.

PO-05 adds:
- **Tier 1.** `Tests/DreamCore/test_outline.cpp`, the `Outline_*` cases (18), compiled with every tier-1 run. They cover
  nesting, decorators, async, cells (a cell inside a class included), strings and comments, continuations, tabs, CRLF,
  empty input, broken code, the recovery cap, Unicode, snapshot and text agreeing, 3,000 random edits, and the 1 MB gate.
- **Tier 4, attended only:**
  - `Dream.Shell.SideBar` and `Dream.Shell.ProjectTree` (24 cases together): defaults, toggles, collapse, width capping,
    persistence, focus and keys, roots and the default root, missing items appearing, budgets, hidden names, a remote
    scratch folder, the inline editor with real key events, rename following tabs, the recycle seam, protected rows,
    the plugin groups with a temporary mount point, and the live toggle;
  - `Dream.Shell.Structure` (11 cases);
  - `Dream.Buffers.ScratchFiles` and `Dream.Buffers.Samples`: numbering, reuse, auto-save timing and reasons, close and
    exit saves including refused and in-flight ones, Save As, the folder guard with UNC cases, migration, the restored
    conflict, and seeding;
  - the PER-02 block of `Dream.Restart.Layout`.

  For example: `run_tier4.bat --attended --filter Dream.Shell+Dream.Buffers+Dream.Restart+Dream.Visual.Capture`.
  `Dream.Visual.Capture` adds screens 14 (Darcula) and 15 (GitHub Light) with the side bar. The plugin folders are off
  in its other captures, so those look as before.
- **Switches:**
  - `-DreamScratchFolder=<dir>` (the runner passes it);
  - `-DreamRealRecycleTest` (runs the real Recycle Bin case, which leaves one `real_recycle_<suffix>.py` in the bin);
  - `-DreamStructureSplit=<f>`.
- **Folders:** scratch files are in `%LOCALAPPDATA%\DreamPythonEditor\Scratches` (a setting; A236). The tests use
  `DreamHost/Saved/DreamTests/Scratches`, emptied before each runner or driver pass.

The rename (PO-06 phase 3, A246) adds:
- **Tier 4, any editor:** `Dream.Migration` (8 cases) works on its own folders under
  `DreamHost/Saved/DreamTests/Migration-<id>` and an ini file of its own: the section copy, the path remap, the
  DreamHost to SumiHost hash mapping, the storage-root, scratch and theme copies with the old tree checked byte for
  byte, idempotency, a resumed copy, the run-log worker, the old REPL-history header, the CoreRedirects and the legacy
  tab ID. It never touches the user's real Sumi or Dream folders or settings.
- **Tier 1:** `Theme_LegacyFormatNameReads`. **Tier 2:** `test_compat_shim` (3 cases).
- **Gentle runners** (owner instruction during the rename): `Tests/run_build.bat` passes `-MaxParallelActions=<half the
  logical cores>` (`DREAM_BUILD_JOBS` overrides); `Tests/ci/editor_layout.py run` starts every test editor (tier 4 and the
  drivers) at BelowNormal priority (`DREAM_EDITOR_PRIORITY=normal` overrides); `run_tier4.bat --attended` says that it
  takes over the desktop and stays opt-in, with the owner asked first.
- **The real migration** runs once, at the first start of a Dream build on a machine with Sumi data, whichever session
  that is (a tier-4 pass included); it only copies.

## 9. Not in slice B

Slice A's section 9 listed what it left to slice B; slice B built those items except the ones below. Each stays marked
`// Deferred (<target>): …` where it would show (rule 8).

| Item | Target | Reason |
|---|---|---|
| Custom `SCodeView` (§4.8) and the G1 harness (`DreamCoreBench`, `fuzz_*` at scale, `Dream.Perf.CodeView`, `Dream.IME.Composition`) | G1 | The Phase 0 P0-1 spike decides SCodeView against its §4.15 fallbacks with measurements this machine cannot take (no perf-reference GPU agent, no IME agent); M1 keeps fallback row (3), plain mode, for every buffer under 1 MB (A1) |
| IME and accessibility beyond the stock widget (EDT-14); `dream::CaretSet`; the XXH3 line hash; the view stages of T-LEX-008 (`Dream.Keystroke`) | G1 | They depend on SCodeView or key its shaping cache; the gap-buffer line array was pulled back and built (A72) |
| The §4.13 progressive (>10 MB) and mapped (>50 MB) tiers and every view over 1 MB; Show all over 1 MB; the output view on the line-source mode (A33); one joined row for lines over 64 KB | G1 | Plain mode is the only view and holds 1 MB (A40) |
| Italic Comment/Docstring and bold Definition faces, the JetBrains Mono composite (CFG-04, M1 font) | owner decision | The four OFL font files and their licence are in neither the repo nor the engine; fetching third-party files is the owner's call (marker `Deferred (M1, needs the OFL font files)`) |
| G3 harness: Stage 2 transformer, probe client against CI-pinned Pyrefly and basedpyright | G3 | Needs third-party language servers pinned in CI; none ships or exists here (RV-11) |
| G6 driver B at 10,000 editor kills (tier 6 nightly, the M1 gate run) | M1 gate, outside slice B | Days of wall-clock time; slice B built the driver and ran it at 5 kills (B24) |
| T-OUT-005 under TSan; ASan for T-CORE-003; T-PER-005 as libFuzzer (journal and history index) | none here | MSVC has no ThreadSanitizer; the x64 ASan/libFuzzer component is not installed for 14.44 (A75, owner decision) |
| T-CAN-009's direct read of `pending.async_exc` | none | Needs CPython's internal headers (`Py_BUILD_CORE`), which are banned; the placeholder delivery is asserted instead |
| CQTest specs (§2.3.3) | none | No functional gap; the module stays on the Automation Spec (A41) |
| The bridge FIFO of 8 waiting requests and the MCP refusal (§5.2.2) | 1.1 | The bridge and agent routes are 1.1 (D10) |
| Hover, signature help and saferepr (§7.7, REPL-03's hover part), T-RES-029, T-MRG-020..022; the 150 ms "Loading…" row (T-MRG-023) | M2 | §15.2's M1 row lists §7.5–§7.6, not §7.7 |
| T-RES-023 | M3 | Its Lands line is M3 |
| `container_element_kind` reading unreal containers (E2/N1 element reads), the element-kind part of T-API-016, T-RES-028; `container_export_text`, `struct_export_text`, `soft_props`, `prop_export_text` | M2 (the last four M2/M3) | The container wrappers are private to PythonScriptPlugin; M1 returns None, "do not read" (A88) |
| Loading `FUnrealNameIndex` from the Stage 1 snapshot | M2 | M1 fills it from the live resolver (A146) |
| LSP profile files and `NeutralConfig` (§7.2.5); Suspended spawning, the SHA-256 check, the protocol trace file; the language-health status-bar item; the orphan sweep `lsp-children.json` (T-LSP-041) | M2 | They come with the type server and ruff; on Win64 the kill-on-close job already ends every child |
| T-DIAG-025 (status-bar counts), T-DIAG-018 (session-name filter) | M2 | The status-bar counts are WS-05 M2; dream-check reports no undefined names |
| Problems: type-server items | M2 | One supported type server lands in M2 (RT-12) |
| Clearing runtime Problems when a module reloads (RLD-01) | M3 | Reload is M3 |
| The local-history timeline, diff and Restore UI; Clear's inline-confirm UI | M2 | §10.4 UI is M2; the `Clear` API exists |
| Remote-root heartbeat liveness (rewriting the lock's `utc` every 30 s, taking over a stale lock from another host) | open (no target set) | No network share here to test it, and Windows SMB keeps a dead client's handle until the server times it out, so the takeover needs its own design |
| Staging of `file-state.json` (BUF-07) | M2 | BUF-07 is M2 |
| Staging of telemetry and LibraryState | M4 | Their features are M4 |
| `PostMortem.retain`, `sys.last_*`, the footer's [Inspect] | M2 | The post-mortem inspector is M2 (§8.7) |
| `@dream_editor.task`, `Task` handles, `task_step`; the `reload` and `entry_point` run kinds; `revert_module_state`; Debug runs (F5); supervised evaluates; task-step supervision and the walk-back; the Paused window, its gate and pause refusal; REPL suspension while paused (REPL-07); `Dream.Pause` paused/evaluating; Stop in task mode (T-UEI-002) | M3 | Tasks, the debugger and reload are M3 |
| debugpy (the server, closing it at HandleShutdown, the frame rule exercised once `_dream_vendor/debugpy` is vendored) | M2 | debugpy attach is M2 (RV-04) |
| Live (docked) Preview and its Stop targets | M4 | §5.5.4 is M4; LiveDocked runs as the Run window (A28) |
| Trust (`FDreamTrust::Evaluate`, `trust` verdicts in the audit), the policy keys (`retention.audit_days`, `retention.run_logs_days`, `python.allow_enable_at_runtime`), the audit doctor item, `Dream.Doctor`, `Dream.Policy.Reload`, `Dream.Diagnostics.Export`, the async source-control checkout, the restricted badge, the Remote Execution warning (SEC-04), §11.1.3 project-layer save | M4 | Policy, trust and diagnostics are M4 |
| Presets (Minimal, Echo All), SGR spans (`ParseSgr`, OUT-03), run, source and regex filters (OUT-06), Copy as script (OUT-10), Ctrl+R and the history prefix filter (REPL-02) | M2 | SPEC milestones |
| Tab pin and reorder (BUF-02), Save All, Revert, Rename, Close Others (BUF-03), the palette and Search Everywhere (WS-06), the WS-05 M2 fields, outline breadcrumbs (NAV-02), ran-line marks (EDT-18), gutter change bars (EDT-02 M2 part), the external-change conflict bar, multi-caret (EDT-06) and folding (EDT-07, `FoldModel`) | M2 | SPEC milestones and TDD §15.2's M2 row |
| Keymap sequences and Shift Shift | M2/M4 | A11 |
| Snippets (`SnippetEngine`, EDT-12) | M4 | SPEC milestone |
| `DreamEditor.Build.cs` modules: ContentBrowser, ContentBrowserData (M2; PropertyEditor is in since PO-04, A193); SourceControl, Json, JsonUtilities (M4); Sockets (1.1) | M2, M4, 1.1 | Added when their features land |
| T-AUD-003 and T-TEL-002 (tier 6) | M1 gate, outside slice B | Need the G6 kill driver at scale and a crash fixture in a test build; `Dream.Audit.LostRun` covers the lost-run record path |
| The G2 table rows T-SUP-006 (needs an OLE drag helper and IME), T-SUP-008 (an EUW asset fixture), T-SUP-009 (8-hour nightly), T-SUP-034 (PySide6 not installed), T-SUP-038, T-SUP-001 with PIE, the hook and static-mesh steps of T-SUP-002/003, the floating-tab and PIE variants of T-KEY-010, T-KEY-016 (RDP), the watchdog internals of T-LIFE-006 | open (no target set) | Fixtures or environments this machine lacks, or no observable outside the supervisor |
| The tier-7 LSP chaos and gate tests (T-LSP-004..008, 021–024, 030); T-LSP-045 at tier 4; T-DIAG-023/024 at tier 4 | open (no target set) | No tier-7 harness here; the 5 MB rule and applyEdit are covered at tiers 1 and 2 (dream-check sends no `applyEdit`) |
| T-GAP-021 golden images; T-KEY-018's keymap-page step; T-PER-047's `FaultyFileOps` byte-offset matrix and the sharing-violation retries in `ReplaceAtomic` | open (no target set) | No approved images; no keymap-edit test API; `FDreamFileSaver` uses the Win64 FileOps directly (a real share-deny handle stands in) |
| T-RES-017; T-MRG-007 (keystroke to popup paint ≤ 30 ms p95); the Cold-boot, tab-closed part of T-REPL-013 | open (no target set) | The §7.10.3 counter fixtures do not exist; not built for lack of time; needs a fresh editor session (T-LIFE-003 covers the boot request) |
| T-STUB-016 (snapshot key inputs and retention) | open (no target set) | The key is computed in C++; tier 4 checks only its form and location |
| T-LIFE-014's collection of the old world at the next GC | open (no target set) | The map change is simulated through the same handler; the GC part is not asserted |
| T-OUT-023 rows: the eviction marker naming the run-log folder, the synthetic header after an evicted header, the gap line from a throttled disk | open (no target set) | They need a full 200,000-line ring or a file-operations fixture in the run-log pipe |
| T-PER-014 (100 concurrent `projects.json` updates), T-PER-046 (junction override, 10 heartbeat rewrites), T-PER-034 and T-PER-044 as automated checks (real crashes) | open (no target set) | The lock logic exists; folder identity is compared but not tested with a real junction; the real-crash cases need the restart driver beyond its prototype |
| P2-shl-01 (a counted recovery over 1 MB after a real crash) | the G6 driver | It needs a killed DreamHost session, which counts toward the restore guard every attended run shares |
