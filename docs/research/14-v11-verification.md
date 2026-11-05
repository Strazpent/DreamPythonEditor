> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 14 — Verification of SPEC v1.1, TDD v1.1 and the mockups

> Record of the five fresh-eyes verification rounds run after the v1.0 review fixes ([13](13-v1-review.md)) were applied. Rounds 1–4 had four lenses and round 5 two. In each lens a verifier checked the documents against the review fixes, the rulings in [08 §9](08-decision-log.md#9-amendments-after-the-v10-review-rv-01--rv-24) and the local UE 5.8 install, and an independent refuter tried to refute every issue it raised. One patcher per round then applied every surviving issue, re-ran the integrator self-check and re-rendered any mockup it changed. Rounds 2–5 checked only the previous round's patch, because each round of patching introduced new contradictions. Section numbers and quoted text refer to the documents as they stood at that round. Probe scripts cited as evidence are in [probes/v11/](probes/v11/) and ran on the engine's `python.exe -I -B`; other paths under "scratchpad" were the verifiers' working folder and are not in the repository. SPEC Appendix D.3 lists the requirement changes by issue ID. The two round-1 items the patcher could not edit, V-runtime-12 (in 13) and the 07a line of V-runtime-03, were applied by hand after round 1.

## Summary

| Round | What it checked | Confirmed | Critical | Major | Minor | Refuted | Added by refuter | Applied | Skipped in part |
|---|---|---|---|---|---|---|---|---|---|
| [1](#round-1) | v1.1 as integrated: did each of the 108 review fixes land, consistently, in every document it targets; regressions from the parallel fix pass | 57 | 0 | 13 | 44 | 0 | 6 | 56 | 1 |
| [2](#round-2) | the round-1 patch: completeness, correctness against the engine, contradictions it introduced | 54 | 0 | 5 | 49 | 0 | 9 | 54 | 0 |
| [3](#round-3) | the round-2 patch only, from an exact before/after diff | 22 | 0 | 3 | 19 | 1 | 3 | 22 | 0 |
| [4](#round-4) | the round-3 patch only, from an exact before/after diff | 11 | 0 | 1 | 10 | 1 | 1 | 11 | 0 |
| [5](#round-5) | the round-4 patch and its follow-on edits, in two lenses | 9 | 0 | 0 | 9 | 1 | 1 | 9 | 0 |

## Round 1

Scope: v1.1 as integrated: did each of the 108 review fixes land, consistently, in every document it targets; regressions from the parallel fix pass.

**Patch.** I applied 59 of the 60 confirmed issues to SPEC.md, TDD.md, the mockups (01, 02, 04, 06, 07, 10, all re-rendered and inspected) and 08 (only factual fixes to RV-01's latency figure and tier-3 timer condition). V-runtime-12 and the 07a line of V-runtime-03 were skipped because research/13 and 07a are outside the editable files. Where issues overlapped (V-persist-15 with V-ux-05; V-delivery-tests-13 with V-ux-06; V-runtime-09 with V-delivery-tests-M01; V-runtime-08 with V-delivery-tests-11), I merged them into one consistent text. Deliberate deviations: Sumi.Policy.Reload stays M4 (SPEC API-03) while the watcher, the poll and the swap move to M2. The mockup 07 'Selected assets and actors' row became 'Selected assets' so the built-in pack of 10 matches TDD 11.5.6. The mockup 06 cell header 'Rename with undo' became 'Rename'. I added one rule-8 line to check.py, as V-delivery-tests-07 asks. New tests: T-INSP-015 and T-PER-055. gen.py regenerated the generated parts. SPEC D.3 has nine new V- rows. check.py prints PROBLEMS 0.

**Checks after the patch.** Final check.py output: 'PROBLEMS 0 {}' (TDD headings 369, links 1381, SPEC ids 248, 814 unique test rows). I re-ran gen.py --write after the last edit: no further changes, 814 tests and no gaps. Before the check.py change it reported 'PROBLEMS 2 {lands-early: 2}' (T-GAP-016 against RUN-23, T-DBG-014 against DBG-09, both Later). V-delivery-tests-07's final_fix asks for check.py rule 8 to skip Later requirements, matching the new T-GAP-008 rule, so I made that one-line change in scratchpad/integ/check.py (backup at .../scratchpad/apply/check.py.bak). Other checks: no CR characters in SPEC, TDD, README or 08. No G1, G2 or G3 automated-suite test has a first Lands after M1. No split SPEC Lands lacks a test at its first milestone. A simulation of the new T-GAP-008 keyword lint (case-insensitive) finds 0 violations. I opened every re-rendered PNG and checked it: no clipping, overlaps or misplaced callouts remain. Engine facts re-checked locally: IAssetRegistry.h:943 OnAssetRenamed, ObjectTools.cpp:4659, UnixPlatformProcess.cpp:218, and codeop results for 'else:', 'if x:\n    pass\nelse:' and 'x = (1 2,' on the engine python -I -B.

### Round 1 — Runtime

I checked the runtime lens: 24 findings (RT-F01..F17, RT-M01, CORE-F04/F05/F08, STU-F01/F15/F16) and rulings RV-01..05, 10, 21 and 23. I checked them against the v1.1 TDD (§2, 3, 5, 8, 9, 12, 13), SPEC, 07a, 08, 13, the mockups and the local UE 5.8 install. I also ran probes on the engine's python.exe, including a compiled native watchdog DLL and the pinned debugpy 1.8.20 from the local VS Code extension.

The RV-01 GIL hand-off is described consistently across §3, §5, §12 and the B-rows. The watchdog owns a PyThreadState per generation and uses PyEval_RestoreThread then PyEval_SaveThread, bracketed by bInPython. Shutdown waits with the GIL released and then deletes the state. Nothing in SPEC, TDD, 08 or 07a still names PyGILState_Ensure as the Stop mechanism; only the review record does. A native-thread probe confirms that the mechanism makes a pure-Python loop pump.

Twelve issues remain, four of them major:
1. Both Slate-child re-parent recipes from RV-02 trip check() in SWindow::AddChildWindow, because RemoveDescendantWindow returns false for a direct child.
2. pydevd's threading.settrace stays installed after trace_this_thread(False), so the §8.1.4 Free state never returns and T-DBG-052 fails.
3. The hand-off costs about 16 ms, not 5 ms, at Windows' default timer resolution. The tier-3 harness T-SUP-032 fails unless it calls timeBeginPeriod(1) as UE does.
4. Supervised-evaluate posts are rejected by the run-ID guard and the IsPaused rule, and T-DBG-029 contradicts T-DBG-031.

The eight minor issues are:
- The watchdog thread state's creation is not placed inside the bInPython bracket, and bPythonAlive is never set.
- The SetAsyncExc return-value criteria cannot detect the ID collision they are meant to catch.
- A stale T-SUP-011 reference sits in the §13.2 section-budget row.
- B-12a is marked Gate but is listed as a proposed row in OI-98.
- T-DBG-052 lands at M3 while §8.12 and OI-97 say M2.
- §8.1.1 omits the SetAsyncExc GIL use.
- §8.8 still shows structs as 'Native repr'.
- The 13 review record still gives the PyGILState_Ensure fix.

Scratch probes are in scratchpad/verify2/ (wd.c, wd_probe.py, wd_probe2.py, dbg_free.py).

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [V-runtime-01](#v-runtime-01) | major | wrong engine fact | modified | Both RV-02 recipes for moving the modal's Slate child windows to the root window hit check(bRemovedSuccessfully) in SWindow::AddChildWindow. |
| [V-runtime-02](#v-runtime-02) | major | wrong engine fact | modified | RV-03 turns pydevd's tracer off with trace_this_thread(False), but in the pinned debugpy 1.8.20 that clears only sys.settrace on the game thread. |
| [V-runtime-03](#v-runtime-03) | major | wrong engine fact | modified | On Windows, the hand-off's wait in PyEval_RestoreThread is the switch interval rounded up to the process timer resolution. |
| [V-runtime-04](#v-runtime-04) | major | contradiction | modified | RT-M01 and RV-05 require supervised evaluates to pump and be cancellable, and T-DBG-031 now asserts 'at least one pump and a delivered Cancel within 1 s'. |
| [V-runtime-13](#v-runtime-13) | major | wrong engine fact | added by refuter | §8.8 expands every editor property of a UObject and every field of a struct with `get_editor_property`. |
| [V-runtime-05](#v-runtime-05) | minor | other | confirmed | The documents never say that PyThreadState_New runs inside the bInPython bracket after the bPythonAlive re-check. |
| [V-runtime-06](#v-runtime-06) | minor | untestable | modified | Two criteria rely on PyThreadState_SetAsyncExc's return value to prove that the watchdog's thread state does not carry the game thread's ID: T-SUP-011 ('matches exactly one state') and T-LIFE-006 ('returns 1 in every … |
| [V-runtime-07](#v-runtime-07) | minor | contradiction | confirmed | The section-budget row still names T-SUP-011 as its test. |
| [V-runtime-08](#v-runtime-08) | minor | contradiction | confirmed | B-12a is marked Kind 'Gate' (blocks the release), but its value is labelled 'proposed', and OI-98 lists 'B-12a mechanism ≤ 20 ms' among the rows that are Tracked until signed off. |
| [V-runtime-09](#v-runtime-09) | minor | contradiction | confirmed | The debug server ships in M2 (DBG-07, §8.6), and RV-03's choice between the trace_this_thread design and its fallback depends on T-DBG-052. |
| [V-runtime-10](#v-runtime-10) | minor | contradiction | confirmed | New text says the watchdog takes the GIL only for its hand-off. |
| [V-runtime-11](#v-runtime-11) | minor | not landed | modified | CORE-F05 and RV-21 bound the short-value text and forbid running user code, but the Variables table still shows an unreal struct's value text as 'Native `repr`'. |
| [V-runtime-12](#v-runtime-12) | minor | other | confirmed | The review record still gives PyGILState_Ensure/PyGILState_Release as RT-F01's fix and says v1.1 applies every surviving fix. |
| [V-runtime-14](#v-runtime-14) | minor | wrong engine fact | added by refuter | Step 2 re-owns HWNDs with `SetWindowLongPtr(GWL_HWNDPARENT)`. |

#### V-runtime-01

**Major, wrong engine fact.** Verdict: modified.

**Where:** TDD §5.5.3 Teardown step 1; TDD §8.3.1 step 11; TDD §5.15.1 T-SUP-034 (second case)

**Problem.** Both RV-02 recipes for moving the modal's Slate child windows to the root window hit check(bRemovedSuccessfully) in SWindow::AddChildWindow. That is a fatal assert in the Development editor. §5.5.3 calls the root's AddChildWindow while the child's parent (the Run window) is still alive. §8.3.1 calls RemoveDescendantWindow first and then AddChildWindow, but the child's ParentWindowPtr still points at the live Paused window. The two sections also describe the mechanism differently, although §5.5.3 says 'The Paused window follows the same teardown'. As written, T-SUP-034's second case ('adds a Slate child window to the Run window during the run') would crash the editor.

**Evidence.** §5.5.3 currently reads: '`FGlobalTabmanager::Get()->GetRootWindow()->AddChildWindow(Child)` for each entry of a copy of `GetChildWindows()`, which also removes it from the Run window (`SWindow.cpp:1863-1877`)'. §8.3.1 step 11 reads: 'moves to the root window (`RemoveDescendantWindow`, then the root's `AddChildWindow`; ...SWindow.h:986-1001)'. In the engine, Engine/Source/Runtime/SlateCore/Private/Widgets/SWindow.cpp:1865-1870 has AddChildWindow do `PreviousParent = ChildWindow->ParentWindowPtr.Pin(); if valid { bRemovedSuccessfully = PreviousParent->RemoveDescendantWindow(ChildWindow); check(bRemovedSuccessfully); }`. SWindow.cpp:1896-1911 shows that RemoveDescendantWindow computes bRemoved but ends with `return false;` unless a grandchild matched, so it returns false for a direct child. It also never resets the child's ParentWindowPtr. The re-parent therefore asserts whenever the old parent is alive. SlateApplication.cpp:2435-2456 confirms that destroying a window destroys its children first.

**Refuter.** The issue is real. In UE_5.8 SWindow.cpp:1863-1872, AddChildWindow pins ChildWindow->ParentWindowPtr. If that parent is valid, it calls PreviousParent->RemoveDescendantWindow(ChildWindow) and then check(bRemovedSuccessfully). RemoveDescendantWindow (SWindow.cpp:1896-1911) removes a direct child but returns true only when a grandchild matched, so for a direct child it returns false. It never resets ParentWindowPtr. Build.h turns DO_CHECK on for UE_BUILD_DEVELOPMENT, so the check is fatal in the Development editor. Both recipes therefore assert: §5.5.3 calls the root's AddChildWindow while the Run window is alive, and §8.3.1 calls RemoveDescendantWindow and then AddChildWindow, whose internal RemoveDescendantWindow on the still-alive Paused window again returns false. T-SUP-034's second case would crash. The proposed fix is right in substance but leaves three gaps. (a) RequestDestroyWindow is synchronous in 5.8 (SlateApplication.cpp:2423-2456 ends with DestroyWindowsImmediately), so re-parenting can normally happen right after the destroy. Deferring to 'the next idle tick' would leave a Paused-window child orphaned, neither ticked nor painted, until the run ends, because no idle tick happens while the resumed run executes. (b) §5.5.3 step 2 uses GWL_HWNDPARENT, which WinUser.h #undefs under _WIN64 (see V-runtime-14). (c) There is no Paused-window test case.

**Fix applied.** TDD.md §5.5.3, 'Teardown.' list. Replace step 1, currently '1. Re-parent the window's Slate child windows to the root window: `FGlobalTabmanager::Get()->GetRootWindow()->AddChildWindow(Child)` for each entry of a copy of `GetChildWindows()`, which also removes it from the Run window (`SWindow.cpp:1863-1877`). Destroying a window destroys its child windows first (`SlateApplication.cpp:2435-2456`).', with: '1. Detach the window's Slate child windows. Copy `GetChildWindows()` into `Kept` (a `TArray<TSharedRef<SWindow>>`), then call `Window->RemoveDescendantWindow(Child)` for each and ignore the return value, which is false for a direct child (`Engine/Source/Runtime/SlateCore/Private/Widgets/SWindow.cpp:1896-1911`). Destroying a window destroys the child windows still in its list first (`Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2435-2456`). Never call the root's `AddChildWindow` while a child's old parent is alive: it calls the old parent's `RemoveDescendantWindow` and `check`s the result (`SWindow.cpp:1863-1872`), which is fatal in the Development editor.' Keep step 2, with the V-runtime-14 correction. Replace step 3, currently '3. Clear the destroy override and call `RequestDestroyWindow`. The modal stack pops and Slate re-enables the other windows.', with: '3. Clear the destroy override and call `RequestDestroyWindow`, which destroys the window at once (`DestroyWindowsImmediately`, `SlateApplication.cpp:2456`). The modal stack pops and Slate re-enables the other windows. Then release Sumi's last `TSharedPtr` to the window. 4. Re-parent each entry of `Kept` whose `GetParentWindow()` is now null with `FGlobalTabmanager::Get()->GetRootWindow()->AddChildWindow(Child)`, guarded by `ensure(!Child->GetParentWindow().IsValid())`. An entry whose old parent is still referenced stays in `Kept` and is retried at the next pump or the next idle tick, whichever comes first.' Replace 'The Paused window follows the same teardown before each destroy (§8.3.1).' with 'The Paused window follows steps 1–4 at each destroy (§8.3.1). There, step 4 runs after `AddModalWindow` returns and before `PyEval_RestoreThread(Saved)`.' In TDD §8.3.1 step 11, replace 'each `SWindow` in its `GetChildWindows()` moves to the root window (`RemoveDescendantWindow`, then the root's `AddChildWindow`; `Engine/Source/Runtime/SlateCore/Public/Widgets/SWindow.h:986-1001`;' with 'each `SWindow` in its `GetChildWindows()` moves to the root window by §5.5.3 Teardown steps 1–4: it is detached with `RemoveDescendantWindow` before the destroy, and attached with the root's `AddChildWindow` only once the Paused window is gone (`Engine/Source/Runtime/SlateCore/Public/Widgets/SWindow.h:986-1001`);'. In TDD §5.15.1 T-SUP-034, append to the pass criterion: '; no `check` or `ensure` fires during teardown; the Slate child's `GetParentWindow()` is the root window before the first idle tick after the run ends'. In T-KEY-017, append to the scenario: '; in the M3 pass, a fixture also adds a Slate child window to the Paused window, then F10 is pressed'. Append to its pass criterion: '; after F10 the child is alive under the root window and no `check` fires'.

**Patch.** TDD §5.5.3 Teardown is now four steps: detach the child windows with RemoveDescendantWindow, re-own the HWNDs, destroy the window synchronously, then re-parent the kept children to the root with an ensure guard. The Paused-window note now says step 4 runs after AddModalWindow returns. Updated §8.3.1 step 11 and its mermaid lines to match. T-SUP-034 now requires that no check or ensure fires and that the child is parented to the root before the first idle tick. T-KEY-017 has an M3 Paused-window child-window case.

#### V-runtime-02

**Major, wrong engine fact.** Verdict: modified.

**Where:** TDD §8.1.4 owner table, row Free, and the bullet 'While the debug server listens with no client…'; TDD §8.11 T-DBG-052; TDD §8.1.3 'When the trace is installed'; SPEC DBG-06

**Problem.** RV-03 turns pydevd's tracer off with trace_this_thread(False), but in the pinned debugpy 1.8.20 that clears only sys.settrace on the game thread. The in-process listen also installs threading.settrace(trace_dispatch) and monkey-patches thread start, and neither is undone. §8.1.4 classifies the slot as Free only when 'sys.gettrace() and threading.gettrace() are None'. So after any 'Debug in VS Code' the slot reads Foreign for the rest of the session, and every F5 shows the foreign-tracer prompt. This contradicts the §8.1.4 bullet ('so the slot reads Free and Debug runs are allowed'). T-DBG-052 ('the §8.1.4 owner is Free, and F5 starts a Debug run with no prompt') cannot pass, and its failure would wrongly trigger the RV-03 fallback. Threads started in non-Debug runs are also traced by pydevd, while DBG-06 says 'no trace function unless a debug session has breakpoints or stepping'.

**Evidence.** In debugpy 1.8.20 (the pinned version, read from the local VS Code debugpy extension bundle), pydevd.py:3158-3167 runs py_db.patch_threads() on the in-process listen path. pydevd.py:2579-2591 shows patch_threads doing `threading.settrace(self.trace_dispatch)` plus patch_thread_modules(). pydevd.py:1172-1176 shows disable_tracing doing only pydevd_tracing.SetTrace(None). pydev_monkey.py:1098-1126 shows new threads calling _on_set_trace_for_new_thread. I ran verify2/dbg_free.py on the engine python.exe -I -B. After listen and trace_this_thread(False), sys.gettrace() was None, but threading.gettrace() was 'functools.partial(<cyfunction trace_dispatch …>, <pydevd.PyDB …>)', and a new worker thread's sys.gettrace() was pydevd's SafeCallWrapper.

**Refuter.** The issue is real. In the local debugpy 1.8.20 (VS Code ms-python.debugpy-2026.6.0 bundle), settrace's non-current-thread path runs py_db.patch_threads() (pydevd.py:3158-3166). patch_threads does threading.settrace(self.trace_dispatch) plus patch_thread_modules() (pydevd.py:2579-2591). disable_tracing only calls pydevd_tracing.SetTrace(None) (pydevd.py:1172-1176). The verifier's probe output (verify2/dbg_free_out.json) shows sys.gettrace() None but threading.gettrace() still pydevd's partial after trace_this_thread(False), and a new thread's sys.gettrace() is pydevd's SafeCallWrapper. The §8.1.4 Free row requires both to be None, so after listen the slot reads Foreign, F5 prompts, T-DBG-052 fails, and the RV-03 fallback would be triggered wrongly. The fix direction is right. Its detection rule should be an identity test against the debugger's own attribute: pydevd.py:746 sets self.trace_dispatch = partial(_trace_dispatch, self), and patch_threads passes that same object. Without that, the prompt's Foreign test would need to inspect a functools.partial.

**Fix applied.** TDD.md §8.1.4 table. Replace the Free row '| Free | `sys.gettrace()` and `threading.gettrace()` are `None` | Allowed | Allowed |' with '| Free | `sys.gettrace()` is `None`, and `threading.gettrace()` is `None` or, while `DebugpyBridge.is_listening()` and `is_client_connected()` is false, is pydevd's own dispatcher (`threading.gettrace() is pydevd.get_global_debugger().trace_dispatch`). The in-process `listen` installs it with `threading.settrace` in `PyDB.patch_threads` (`debugpy/_vendored/pydevd/pydevd.py:2579-2591`), and `trace_this_thread(False)` does not remove it | Allowed | Allowed |'. In the bullet 'While the debug server listens with no client, pydevd's game-thread tracer is off (§8.6.2, RV-03), so the slot reads Free and Debug runs are allowed.', append: ' A Debug run started in that state replaces pydevd's thread tracer with `SettraceBackend`'s for the run, with no prompt, and compare-and-restores it at run end.' In T-DBG-052's pass criterion, after 'the §8.1.4 owner is Free,' insert '`threading.gettrace()` is pydevd's dispatcher before the Debug run and again after it,'. In TDD §8.1.3 'When the trace is installed.', append: ' pydevd also sets `threading.settrace` and patches thread start, so Python threads started after the debug server starts, a run's worker threads among them, are traced by pydevd until the editor exits. Only the game thread is untraced while no IDE is attached.' In SPEC.md DBG-06, after 'while no IDE is attached, debugpy's tracer is off on the game thread (DBG-07, RV-03).', insert ' Python threads started after the debug server starts, such as a run's worker threads, are traced by pydevd until the editor closes, and the Debug in VS Code confirmation says so.'

**Patch.** §8.1.4 Free row: pydevd's threading dispatcher counts as Free while the server listens with no client, detected by an identity test. Added a no-prompt compare-and-restore bullet. §8.1.3 and SPEC DBG-06 now say that threads started after the server starts are traced. Added the confirmation text in §8.6.2. T-DBG-052 now checks threading.gettrace().

#### V-runtime-03

**Major, wrong engine fact.** Verdict: modified.

**Where:** TDD §3.2 GIL-4 ('costs at most one switch interval (5 ms)'); TDD §3.1 watchdog row; TDD §5.4.1 hand-off step 2 and 'The cost is at most one switch interval'; TDD §5.8 Measurement ('at most about one switch interval (5 ms by default)'; 'HandOffWait … capped at one switch interval'); TDD §5.13 row 'GIL hand-off after a post'; TDD §13.2 section-budget row 'Watchdog post to callback'; TDD §5.15.3 T-SUP-032; research/07a note under Q1 ('That costs at most one switch interval (5 ms) per post')

**Problem.** On Windows, the hand-off's wait in PyEval_RestoreThread is the switch interval rounded up to the process timer resolution. The editor runs at 1 ms resolution because UE calls timeBeginPeriod(1). The tier-3 SumiPyHarness is a plain GoogleTest executable and runs at the default 15.6 ms. There the hand-off costs about 16 ms, and the watchdog's own period waits round up too. T-SUP-032 ('each later one within one period plus 10 ms of the previous', 'first callback … within one period plus 10 ms after arming') then fails, and the '≤ 5 ms' figure is wrong even in the editor (measured 5.5 to 6.4 ms). Windows 11 can also drop a minimised process's 1 ms request.

**Evidence.** I built verify2/wd.c (MSVC, engine python311.lib) and ran it with wd_probe2.py on the engine python.exe -I -B. A native thread with no thread state calls Py_AddPendingCall, then PyThreadState_New(PyInterpreterState_Main()), PyEval_RestoreThread and PyEval_SaveThread, against `while time.perf_counter() < t_end: pass` with a 66 ms Sleep period. At default resolution: post to callback p50 15.75 ms, max 16.66 ms; callback gap p50 94.4 ms; first callback 85 ms after start. After winmm.timeBeginPeriod(1): p50 5.47 ms, max 6.44 ms; gap 72.2 ms; first callback 72.2 ms. UE sets 1 ms in FWindowsPlatformMisc::PlatformInit (Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1076, 'timeBeginPeriod( 1 )'). TDD §14.1 describes tier 3 as 'GoogleTest executable SumiPyHarness'. The hand-off mechanism itself works: 32 of 33 callbacks ran inside the pure-Python loop, all on the main thread, while the no-hand-off control ran none.

**Refuter.** The issue is real. I re-ran verify2/wd_probe2.py on the engine python.exe -I -B. At default resolution: post-to-callback p50 15.52 ms, max 16.24 ms; callback gap p50 94.1 ms; first callback 90 ms. After timeBeginPeriod(1): p50 5.51 ms, max 6.55 ms; gap 72.0 ms; first callback 72.4 ms. UE calls timeBeginPeriod(1) in WindowsPlatformMisc.cpp:1076, and the plain GoogleTest SumiPyHarness does not. So T-SUP-032's 'one period plus 10 ms' (76.7 ms at 15 Hz) fails in the Win64 harness, and '≤ 5 ms' is slightly wrong even in the editor. Two parts of the proposed fix are wrong. Its default-resolution margin of 'one period plus 25 ms' is 91.7 ms, but the measured gap is 94 ms (Sleep rounds up to 78 ms, plus about 16 ms of hand-off), so that variant would fail too. And tier 3 also runs on Linux, where timeBeginPeriod does not exist. The ruling text itself (08 RV-01) carries the '(5 ms)' figure and must change with the TDD.

**Fix applied.** Use one replacement phrase P everywhere below: 'one switch interval (5 ms) rounded up to the OS timer resolution: about 5–7 ms under the editor's 1 ms resolution (`timeBeginPeriod(1)`, `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1076`), and about 16 ms at Windows' default 15.6 ms resolution, which Windows 11 may apply to a minimised or occluded editor'. TDD.md §3.1 watchdog row: replace 'which costs at most one switch interval (5 ms) of GT latency per pump request (GIL-4, RV-01)' with 'which costs P of GT latency per pump request (GIL-4, RV-01)'. §3.2 GIL-4: replace 'That costs at most one switch interval (5 ms) of GT latency per pump request.' with 'That costs P of GT latency per pump request.' §5.4.1 step 2: replace 'this waits one switch interval (5 ms by default) and then asks it to drop the GIL at its next eval-breaker check. The time spent waiting, capped at one switch interval, is stored' with 'this waits one switch interval (5 ms by default, rounded up to the OS timer resolution) and then asks it to drop the GIL at its next eval-breaker check. The time spent waiting, capped at 20 ms, is stored'. Replace 'The cost is at most one switch interval of game-thread latency per pump request.' with 'The cost is P per pump request.' §5.8: replace 'In pure Python the gap is at most about one switch interval (5 ms by default): the GIL hand-off (§5.4.1).' with 'In pure Python the gap is the GIL hand-off (§5.4.1): P.' Replace 'capped at one switch interval.' with 'capped at 20 ms.' §5.13 row 'GIL hand-off after a post': replace '≤ one switch interval (5 ms) of added latency before the pending call runs, per post;' with 'P of added latency before the pending call runs, per post;'. §5.15.3 T-SUP-032: at the start of the scenario insert 'On Win64 the harness calls `timeBeginPeriod(1)` at start, as the editor does.' At the end of the pass criterion append '. Win64 only, second case without `timeBeginPeriod`: each callback within one period plus 40 ms of the previous, p99'. §14.1 tier-3 row, Harness cell: after 'built without Slate' append '; on Win64 it calls `timeBeginPeriod(1)` at start (WindowsPlatformMisc.cpp:1076), so timed waits match the editor'. §13.2 row 'Watchdog post to callback, pure-Python loop': replace 'Within one pump period + 10 ms; the GIL hand-off adds at most one switch interval (5 ms) per pump request (RV-01) | §5.4.1 | T-SUP-011 |' with 'Within one pump period + 10 ms at 1 ms timer resolution; the GIL hand-off adds P per pump request (RV-01) | §5.4.1 | T-SUP-032 |'. research/07a-verify-python-runtime.md line 97: replace 'That costs at most one switch interval (5 ms) per post' with 'That costs P per post'. research/08-decision-log.md §9 RV-01: replace 'That costs at most one switch interval (5 ms) of game-thread latency per pump request.' with 'That costs one switch interval (5 ms) rounded up to the OS timer resolution, about 5–7 ms in the editor, which runs at 1 ms, of game-thread latency per pump request.' Also replace 'The tier-3 suite posts from a native thread' with 'The tier-3 suite, on Win64 under timeBeginPeriod(1), posts from a native thread'.

**Patch.** Replaced every '(at most) one switch interval (5 ms)' latency claim (§3.1, GIL-4, §5.4.1, §5.8, §5.13, §13.2, the §5.4 mermaid) with the timer-resolution wording: about 5-7 ms at 1 ms resolution, about 16 ms at the 15.6 ms default. HandOffWait cap is now 20 ms. The §14.1 tier-3 harness calls timeBeginPeriod(1) on Win64. T-SUP-032 has a Win64 default-resolution case with a margin of one period plus 40 ms. In 08 RV-01, corrected the cost figure and the tier-3 timer condition as factual errors; what the ruling decides is unchanged. The 07a edit was not applied (see skipped).

#### V-runtime-04

**Major, contradiction.** Verdict: modified.

**Where:** TDD §5.4.2 step 1 ('If `Id != CurrentRunId` or the run has ended, return 0'); TDD §8.3.1 step 2 ('The pending-call callback (§5) returns 0 while `IsPaused()`: no pump, no cancellation, no `SetAsyncExc`'); TDD §8.3.4 first bullet ('`_sumi.eval_begin(run_id)` returns `EvalId`, and the pump runs for that id although the run is paused'); TDD §8.1.5 ('int64 BeginSupervisedEvaluate(FSumiRunId RunId); // returns the EvalId', 'void EndSupervisedEvaluate(int64 EvalId)'); TDD §12.1.2 ('None is `uint32` or `int64`'); TDD §8.11 T-DBG-029 and T-DBG-031

**Problem.** RT-M01 and RV-05 require supervised evaluates to pump and be cancellable, and T-DBG-031 now asserts 'at least one pump and a delivered Cancel within 1 s'. But the evaluate's pending calls carry an int64 EvalId that is never made CurrentRunId, so the step-1 run-ID guard rejects them. §8.3.1 step 2 also makes the callback return 0 whenever IsPaused(), with no exception for evaluates. T-DBG-029 ('an armed pending call fires during an evaluate | No Slate tick from the pump; no delivery') directly contradicts T-DBG-031. Taken literally, the RT-M01 fix cannot work, and one of the two tests will be built wrong.

**Evidence.** In §5.4.2, step 1 is the first check, before the deferral check. Nothing in §5.4, §8.1.5, §8.3.4 or §12.3 says BeginSupervisedEvaluate sets CurrentRunId or how the guard accepts an EvalId. §12.1.2 says every run ID is an FSumiRunId and none is int64, yet §8.1.5 returns and takes int64 EvalId. Only §8.3.2 says 'The pump stays inert while paused (step 2), except for supervised evaluates'.

**Refuter.** The issue is real. §5.4.2 step 1 returns 0 when Id != CurrentRunId, and no text makes the evaluate's ID current. §8.1.5 declares int64 EvalId while §12.1.2 says no run ID is int64. §8.3.1 step 2 says the callback returns 0 while IsPaused() with no exception. Only §8.3.2's bullet carves out evaluates. As written, T-DBG-029 ('an armed pending call fires during an evaluate' → no tick, no delivery) contradicts T-DBG-031 (at least one pump and a delivered Cancel). The proposed fix is sound. It needs one addition: the FSumiRunId comment in §5.2 says EntrySerial is 'bumped for every entry into _run', which must also cover evaluates if EvalIds come from that counter.

**Fix applied.** TDD.md §8.1.5. Replace '    int64 BeginSupervisedEvaluate(FSumiRunId RunId);                                // §8.3.4; returns the EvalId' and '    void  EndSupervisedEvaluate(int64 EvalId);' with '    FSumiRunId BeginSupervisedEvaluate(FSumiRunId RunId);                           // §8.3.4; returns the EvalId' and '    void  EndSupervisedEvaluate(FSumiRunId EvalId);'. In §8.3.4's first bullet, after '`_sumi.eval_begin(run_id)` returns `EvalId`,' insert ' an `FSumiRunId` with the current generation and a fresh `EntrySerial`. `BeginSupervisedEvaluate` saves `CurrentRunId`, sets it to the `EvalId` and arms the watchdog with the 300 ms delay, so the §5.4.2 step 1 guard accepts the evaluate's calls and rejects any call still carrying the paused run's ID. `EndSupervisedEvaluate` disarms and restores the saved ID.'. In §8.3.1 step 2, replace 'The pending-call callback (§5) returns 0 while `IsPaused()`: no pump, no cancellation, no `SetAsyncExc`.' with 'The pending-call callback (§5) returns 0 while `IsPaused()` and no supervised evaluate is active (§8.3.4): no pump, no cancellation, no `SetAsyncExc`.'. In §5.2 (the FSumiRunId code comment), replace '// EntrySerial: per-process counter, bumped for every entry into _run (allocated before prepare;' with '// EntrySerial: per-process counter, bumped for every entry into _run and every supervised evaluate (allocated before prepare;'. In §5.4.2 step 1, append ' During a supervised evaluate, `CurrentRunId` is its `EvalId` (§8.3.4).'. In §8.11, rewrite T-DBG-029 as: '| T-DBG-029 | 4 | Pump inert while paused: a pending call carrying the paused run's ID, queued before the pause, fires at the start of an evaluate that ends before the 300 ms arm delay | It returns at the run-ID guard (§5.4.2 step 1): no Slate tick from the pump, no delivery; the run is still paused afterwards | D3, D2 | M3 |'.

**Patch.** §8.1.5 BeginSupervisedEvaluate and EndSupervisedEvaluate now use FSumiRunId. §8.3.4 defines EvalId (same EntrySerial counter; sets and restores CurrentRunId; arms the watchdog). §8.3.1 step 2 exempts active evaluates. Updated the §5.2 EntrySerial comment and §5.4.2 step 1. Rewrote T-DBG-029 as a run-ID-guard test.

#### V-runtime-13

**Major, wrong engine fact.** Raised by the refuter.

**Where:** TDD §8.8 Variables table, rows 'unreal.Object' (Children cell) and 'unreal struct' (Children cell); TDD §8.9/§8.11 T-INSP-014 (containers only); SPEC VAR-01, VAR-03

**Problem.** §8.8 expands every editor property of a UObject and every field of a struct with `get_editor_property`. For a soft object or soft class property, that read loads the asset synchronously. Expanding a data asset, an actor or a struct that holds a TSoftObjectPtr in Variables or the inspector, also while paused, therefore loads assets. That contradicts §8.8's own intro ('without loading assets (VAR-01, RV-21)'), RV-21 ('Inspection never loads and never runs user code'), §7 N1 ('The D30 inspector needs the same rules (contract to §8)') and OI-67's closing note. No test catches it: T-INSP-014 covers only Array, Map and Set elements, and T-INSP-002 counts BlueprintGetters, not loads.

**Evidence.** Engine: PythonScriptPlugin/Private/PyConversion.cpp:1136-1145 pythonizes FSoftClassProperty and FObjectPropertyBase through `LoadObjectPropertyValue`. CoreUObject/Private/UObject/PropertySoftObjectPtr.cpp:254-257 has `FSoftObjectProperty::LoadObjectPropertyValue` return `GetPropertyValue(...).LoadSynchronous()`. TDD §7 N1 (line 3978) states the same. In TDD §8.8 the unreal.Object row reads 'Editor properties via `get_editor_property(name)` on expand only (E2)' and the struct row reads 'Fields via `get_editor_property`', with no exclusion of soft properties.

**Fix applied.** TDD.md §8.8 Variables table. In the 'unreal.Object' Children cell, replace 'Editor properties via `get_editor_property(name)` on expand only (E2)' with 'Editor properties via `get_editor_property(name)` on expand only (E2), except `FSoftObjectProperty`, `FSoftClassProperty` and `FLazyObjectProperty` properties (N1, RV-21). Those are leaves holding their path text from `_sumi.prop_export_text(v, name)` (C++ `FProperty::ExportTextItem_Direct` on the soft pointer, which writes the path without loading), never read through `get_editor_property`'. In the 'unreal struct' Children cell, replace 'Fields via `get_editor_property`' with 'Fields via `get_editor_property`, except soft, soft-class and lazy object fields, which are leaves holding `_sumi.prop_export_text(v, name)` as for `unreal.Object`'. Add rows to the §12.1.1 `_sumi` table: '| `soft_props(value: object) -> frozenset[str]` | GT | Script names of the soft, soft-class and lazy object properties of an unreal object's class or a struct wrapper's struct; read from reflection, never from a value | §8.8 |' and '| `prop_export_text(value: object, name: str) -> str` | GT | Export text of one property made in C++ with `FProperty::ExportTextItem_Direct`, cut at 200 characters; never loads | §8.8 |'. Add to §8.11: '| T-INSP-015 | 4 | Structs and soft properties (RV-21): a `@unreal.ustruct` subclass instance with a counting `__repr__`; a struct with a 100,000-element array field; a fixture UObject and a struct holding unloaded soft object, soft class and lazy object properties; expand each in Variables and in the object inspector | `__repr__` counter 0; the struct shows `<StructName> {…}` with the array field as `Array[100000]`; each request ≤ 5 ms; `OnEndLoadPackage` count 0; soft properties show their path text | VAR-01, VAR-03, D30, RV-21 | M3 |'.

**Patch.** §8.8 object and struct children: soft, soft-class and lazy properties are leaves holding prop_export_text, named by soft_props. Added soft_props and prop_export_text rows to §12.1.1. New test T-INSP-015 (tier 4, M3).

#### V-runtime-05

**Minor, other.** Verdict: confirmed.

**Where:** TDD §5.4.1 'The watchdog's thread state' ('creates it … before its first hand-off in that generation'); TDD §3.2 GIL-4 ('creates its thread state with `PyThreadState_New` on its own thread at its first post in a generation'); TDD §3.8 object table (watchdog PyThreadState row) and Python shutdown step 2 ('The next generation's first post creates a new one'); TDD §3.5 HandleInitialized

**Problem.** The documents never say that PyThreadState_New runs inside the bInPython bracket after the bPythonAlive re-check. They never set bPythonAlive to true or reset the cached WatchdogState pointer for a new generation. That leaves a shutdown race. The watchdog posts while alive under PostLock and releases the lock. The game thread's OnPythonShutdown then clears bPythonAlive, sees bInPython false, deletes a null state and returns, and the engine calls Py_Finalize, while the watchdog calls PyThreadState_New on the finalizing interpreter. Finalization would also free that leaked state, so the cached pointer dangles into the next generation. That is a crash in T-LIFE-006 and T-LIFE-008 territory.

**Evidence.** §5.4.1 lists the hand-off as steps 1-3 (set bInPython and re-check; RestoreThread; SaveThread) with creation as a separate bullet. Engine PythonScriptPlugin.cpp:1497 broadcasts OnPythonShutdown and :1553 calls Py_Finalize in the same ShutdownPython. Once Py_Finalize has started, CPython 3.11's take_gil exits any thread that is not the finalizing thread, so every watchdog Python call must sit inside the bracket. A grep shows bPythonAlive only being cleared (TDD lines 650, 886, 2396, 2405), never set.

**Refuter.** The issue is real but narrow. §5.4.1 places creation 'before its first hand-off', GIL-4 and the §3.8 table say 'at its first post', and none of them puts PyThreadState_New inside the bInPython bracket after the bPythonAlive re-check. If creation happens after the post but before step 1, OnPythonShutdown can clear bPythonAlive, see bInPython false, delete a null state and return to the engine's Py_Finalize (PythonScriptPlugin.cpp:1497 broadcast, :1553 Py_Finalize) while the watchdog is calling PyThreadState_New. Finalization then frees the leaked state, and a cached pointer would dangle into the next generation. The documents only clear bPythonAlive (lines 886 and 2405) and never set it. Store-then-load on both sides also needs sequentially consistent atomics. The verifier's fix covers this. The only adjustment is that bPythonAlive belongs to FRunSupervisor, so HandleInitialized should set it through the supervisor.

**Fix applied.** TDD.md §5.4.1, hand-off steps. After step 1, insert '1a. If `WatchdogState` is null or stamped with an older generation, create it here with `PyThreadState_New(PyInterpreterState_Main())` and stamp it with the current generation. Creation, like every Python call the watchdog makes, happens only inside the `bInPython` bracket, after the `bPythonAlive` re-check.' In the bullet 'The watchdog's thread state.', replace 'before its first hand-off in that generation.' with 'at its first hand-off in that generation (step 1a).' In the 'Shutdown safety' bullet, append ' `bInPython` and `bPythonAlive` are `std::atomic<bool>` with sequentially consistent stores and loads (the default), because each side stores one flag and then loads the other.' In §3.2 GIL-4, replace 'The watchdog creates its thread state with `PyThreadState_New` on its own thread at its first post in a generation' with 'The watchdog creates its thread state with `PyThreadState_New` on its own thread at its first hand-off in a generation, inside the `bInPython` bracket (§5.4.1 step 1a)'. In the §3.8 object table row 'Watchdog `PyThreadState`', replace 'with `PyThreadState_New` at its first post in a generation (GIL-4)' with 'with `PyThreadState_New` at its first hand-off in a generation, inside the `bInPython` bracket (GIL-4)'. In the §3.1 watchdog row, replace 'It creates the thread state on its own thread with `PyThreadState_New` at its first post in a generation' with 'It creates the thread state on its own thread with `PyThreadState_New` at its first hand-off in a generation, inside the `bInPython` bracket'. In §3.8 Python shutdown step 2, replace '(`PyThreadState_Clear`, `PyThreadState_Delete`). The next generation's first post creates a new one.' with '(`PyThreadState_Clear`, `PyThreadState_Delete`) and set `WatchdogState` to null. The next generation's first hand-off creates a new one.' In §3.5, replace '`HandleInitialized`: re-add the shutdown binding if `!OnPythonShutdown().IsBoundToObject(this)`; increment the generation; create `_sumi`.' with '`HandleInitialized`: re-add the shutdown binding if `!OnPythonShutdown().IsBoundToObject(this)`; increment the generation; create `_sumi`; then set `FRunSupervisor`'s `bPythonAlive` to true under `PostLock`, before any `BeginRun`.'

**Patch.** Added §5.4.1 step 1a: thread-state creation happens inside the bInPython bracket, stamped with the generation. bInPython and bPythonAlive are seq_cst atomics. Shutdown step 2 nulls WatchdogState. HandleInitialized sets bPythonAlive. Changed the GIL-4, §3.1 and §3.8 wording from 'first post' to 'first hand-off'.

#### V-runtime-06

**Minor, untestable.** Verdict: modified.

**Where:** TDD §5.15.3 T-SUP-011 pass criterion; TDD §3.9 T-LIFE-006 pass criterion; TDD §5.15 introduction (`PostSupervisorCall()`) with T-SUP-033

**Problem.** Two criteria rely on PyThreadState_SetAsyncExc's return value to prove that the watchdog's thread state does not carry the game thread's ID: T-SUP-011 ('matches exactly one state') and T-LIFE-006 ('returns 1 in every generation'). CPython 3.11 returns 1 at the first match, so neither can detect the collision. Separately, PostSupervisorCall 'posts one pending call of ours from the calling thread as the watchdog would'. T-SUP-033 calls it on the game thread from `_post_init` with the GIL held. If it also does the watchdog's hand-off there (PyEval_RestoreThread on the watchdog's state), it deadlocks. If it does not, the wording is wrong.

**Evidence.** verify2/wd_probe.py on the engine python.exe -I -B: with a second thread state created by PyThreadState_New on the main thread, so it has the main thread's ID, PyThreadState_SetAsyncExc(main_ident, NULL) returned 1, the same as with no duplicate. Posting KeyError to that ID did not raise in the main thread, because the newer state sits at the list head. In the same probe, the watchdog state's thread_id equalled the watchdog thread's ID (44892 vs main 43128).

**Refuter.** The issue is real. CPython 3.11's PyThreadState_SetAsyncExc returns 1 at the first thread state whose thread_id matches, and the verifier's probe (verify2/wd_probe.py) got 1 both with and without a duplicate-ID state. With the duplicate present, a posted exception did not reach the main thread, because the newer state sits at the list head. So 'matches exactly one state' (T-SUP-011) and 'returns 1 in every generation' (T-LIFE-006) cannot detect the collision they are meant to catch. PostSupervisorCall's wording 'as the watchdog would' could be read as including the hand-off. On the GT with the GIL held, PyEval_RestoreThread would wait for a GIL the same thread holds, a deadlock. A main-thread post does not need the hand-off, because COMPUTE_EVAL_BREAKER on the main thread sets the bit. The proposed fix is right but misses the second statement of the fixture in the §14.2 table.

**Fix applied.** TDD.md §5.15.3 T-SUP-011 pass criterion: replace 'the watchdog's thread state has the watchdog's `thread_id`, so `PyThreadState_SetAsyncExc(GameThreadId, nullptr)` matches exactly one state' with '`WatchdogState->thread_id` equals `PyThread_get_thread_ident()` read on the watchdog thread and differs from `GameThreadId`; `PyThreadState_SetAsyncExc(GameThreadId, ScriptCancelled)` raises `ScriptCancelled` in the game thread's loop'. In §3.9 T-LIFE-006, replace '`PyThreadState_SetAsyncExc` on the GT's ID returns 1 in every generation' with 'in every generation the watchdog state's `thread_id` differs from the GT's, and an async `ScriptCancelled` posted to the GT's ID reaches the GT run'. In the §5.15 introduction, replace '`PostSupervisorCall()`, which posts one pending call of ours from the calling thread as the watchdog would' with '`PostSupervisorCall()`, which calls `Py_AddPendingCall` with our callback from the calling thread and does no GIL hand-off (a post from the main thread sets the eval breaker itself)'. In the §14.2 fixtures row 'Editor test fixtures in `SumiEditorTests`', replace '`PostSupervisorCall()` (posts one Sumi pending call from the calling thread, as the watchdog would)' with '`PostSupervisorCall()` (calls `Py_AddPendingCall` with Sumi's callback from the calling thread, with no GIL hand-off)'.

**Patch.** T-SUP-011 and T-LIFE-006 now check thread_id and the delivery of an async ScriptCancelled. PostSupervisorCall in §5.15 and §14.2 is now described as Py_AddPendingCall only, with no hand-off.

#### V-runtime-07

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §13.2 Section budgets, row 'Watchdog post to callback, pure-Python loop (`while True: pass`, no other Python thread)'

**Problem.** The section-budget row still names T-SUP-011 as its test. In v1.1, T-SUP-011 checks thread-state identity and runs no latency check. The latency check is T-SUP-032, which §5.13 cites for the same budget.

**Evidence.** §13.2 row: '| Watchdog post to callback, pure-Python loop … | Within one pump period + 10 ms; the GIL hand-off adds at most one switch interval (5 ms) per pump request (RV-01) | §5.4.1 | T-SUP-011 |'. §5.13 row: '| GIL hand-off after a post | … each callback runs within one period plus 10 ms | RV-01 | T-SUP-032 |'. T-SUP-011's pass criterion: 'Runs on the initializing thread; the watchdog's thread state has the watchdog's `thread_id` …'.

**Refuter.** TDD line 8122, the §13.2 row 'Watchdog post to callback, pure-Python loop (`while True: pass`, no other Python thread)', names T-SUP-011 as its test. T-SUP-011 (line 3170) now checks only thread-state identity. The latency check is T-SUP-032 (line 3191), which §5.13 (line 3020) already cites for this budget.

**Fix applied.** TDD.md §13.2, row 'Watchdog post to callback, pure-Python loop (`while True: pass`, no other Python thread)': change the Test cell from 'T-SUP-011' to 'T-SUP-032'. Change the value cell as given in V-runtime-03: 'Within one pump period + 10 ms at 1 ms timer resolution; the GIL hand-off adds P per pump request (RV-01)'.

**Patch.** §13.2 watchdog budget row: Test cell is now T-SUP-032, and the value text follows V-runtime-03.

#### V-runtime-08

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §13.2 row B-12a (Kind 'Gate'); TDD §17.1 OI-98; TDD §13.3 rule 4; TDD §5.15.1 T-CAN-002

**Problem.** B-12a is marked Kind 'Gate' (blocks the release), but its value is labelled 'proposed', and OI-98 lists 'B-12a mechanism ≤ 20 ms' among the rows that are Tracked until signed off. T-CAN-002 also leaves open when the stop flag is set. If the harness sets it at a random time, the wait for the next post (up to one period, about 67 ms at 15 Hz) plus the hand-off dominates the p99, and the 20 ms target cannot be met.

**Evidence.** B-12a reads '| B-12a | Stop mechanism, stop flag → exception | p99 ≤ 20 ms (proposed value, T-CAN-002: mechanism only, no pump) | Gate |'. OI-98 reads 'B-12a mechanism ≤ 20 ms … Until signed off, Proposed rows are Tracked (§13.3 rule 4)'. §13.3 rule 4 reads 'Proposed rows are Tracked until they are signed off (OI-98)'.

**Refuter.** B-12a (TDD line 8105) has Kind 'Gate' while its value reads 'proposed value'. OI-98 (line 8809) lists 'B-12a mechanism ≤ 20 ms' with the Proposed rows B-03a, B-16 and B-17, and those all have Kind 'Proposed'. §13.3 rule 1 would block the release on a Gate row, while rule 4 and OI-98 say a Proposed row is only Tracked. T-CAN-002 also never says where the clock starts. In the real flow the stop flag is set inside a pump (§5.4.2 step 9), but a harness that sets it from another thread at a random time adds up to one period (about 67 ms at 15 Hz), so the 20 ms target could not be met.

**Fix applied.** TDD.md §13.2, row B-12a: change the Kind cell from 'Gate' to 'Proposed'. In §5.15.1 T-CAN-002, replace the scenario text 'the Stop mechanism sub-budget: stop flag set to `ScriptCancelled` constructed' with 'the Stop mechanism sub-budget: the fake pump sets the stop flag inside the pending call, as a Stop click processed during a pump does (§5.4.2 step 9), and the timer runs from that store to the `ScriptCancelled` constructor'.

**Patch.** B-12a Kind changed from Gate to Proposed. T-CAN-002 now starts the timer at the stop-flag store inside the pending call (§5.4.2 step 9).

#### V-runtime-09

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §8.11 T-DBG-052 Lands column ('M3'); TDD §8.12 open items ('T-DBG-052 at M2 and at every debugpy bump'); TDD §17.1 OI-97 (closes at 'M2; each debugpy bump')

**Problem.** The debug server ships in M2 (DBG-07, §8.6), and RV-03's choice between the trace_this_thread design and its fallback depends on T-DBG-052. But the integrator gave T-DBG-052 Lands M3, taken from DBG-06, which contradicts §8.12 and OI-97. Part of the test, F5 with no prompt, needs the M3 debugger.

**Evidence.** T-DBG-052 row ends '| DBG-06, U16, D25, RT-11 | M3 |'. integ/lands_report.json gives T-DBG-052 ['M3','covers'].

**Refuter.** T-DBG-052's Lands cell is 'M3' (lands_report.json gives M3, derived from DBG-06). §8.12 says 'T-DBG-052 at M2 and at every debugpy bump', and OI-97 closes at 'M2; each debugpy bump'. The debug server ships in M2 (SPEC DBG-07 is M2), and RV-03 chooses between the design and its fallback on this test. Only the 'F5 starts a Debug run with no prompt' part needs the M3 debugger. T-KEY-017 already uses the split form 'M1 (M3: Paused-window pass)'.

**Fix applied.** TDD.md §8.11, T-DBG-052 row: change the Lands cell from 'M3' to 'M2 (M3: the F5 part)'.

**Patch.** T-DBG-052 Lands is now 'M2 (M3: F5 Debug run)', in the M01 form, with '(from M3)' marking the F5 part.

#### V-runtime-10

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §8.1.1, paragraph after the components table

**Problem.** New text says the watchdog takes the GIL only for its hand-off. GIL-1, GIL-4 and §5.7.5 condition 4 also have it take the GIL on its own thread state for the PyThreadState_SetAsyncExc channel.

**Evidence.** §8.1.1 reads: 'The watchdog runs no Python; it takes the GIL only for its hand-off after a post (§5.4.1, RV-01).' GIL-1 reads: 'the watchdog (for the hand-off after each post and for `SetAsyncExc`, always on its own thread state, GIL-4)'.

**Refuter.** §8.1.1 (TDD line 5281) says 'it takes the GIL only for its hand-off after a post (§5.4.1, RV-01)'. GIL-1, GIL-4 and §5.7.5 condition 4 also have the watchdog take the GIL with PyEval_RestoreThread on its own state for the SetAsyncExc channel. The sentence contradicts them.

**Fix applied.** TDD.md §8.1.1, the paragraph after the components table: replace 'The watchdog runs no Python; it takes the GIL only for its hand-off after a post (§5.4.1, RV-01).' with 'The watchdog runs no Python; it takes the GIL only on its own thread state, for the hand-off after each post and for the §5.7.5 `SetAsyncExc` channel (GIL-1, GIL-4, RV-01).'

**Patch.** §8.1.1: the watchdog takes the GIL only on its own state, for the hand-off and for the §5.7.5 SetAsyncExc channel.

#### V-runtime-11

**Minor, not landed.** Verdict: modified.

**Where:** TDD §8.8 Variables table, row 'unreal struct'

**Problem.** CORE-F05 and RV-21 bound the short-value text and forbid running user code, but the Variables table still shows an unreal struct's value text as 'Native `repr`'. The struct wrapper's repr exports every field recursively, with arrays uncapped, which is unbounded game-thread work against the ≤ 5 ms budget. A Python `@unreal.ustruct` subclass can override `__repr__`, so `repr(v)` can run user code. Both contradict §8.8's own intro ('without running user code or getters') and the §8.3.6 saferepr rule, which shows structs as `<TypeName>`.

**Evidence.** §8.8 row: '| unreal struct | Fields via `get_editor_property` | Native `repr` |'. In the engine, PyWrapperStruct.cpp:1325-1326 sets tp_str and tp_repr to FFuncs::Str, which calls PyUtil::GetFriendlyStructValue (PyWrapperStruct.cpp:806). PyUtil.cpp:1311-1356 exports every init parameter through ExportTextItem_Direct with no cap.

**Refuter.** The issue is real. PyWrapperStruct.cpp:1325-1326 sets tp_repr to FFuncs::Str, which formats GetFriendlyStructValue (PyWrapperStruct.cpp:806). PyUtil.cpp:1311-1356 exports every init parameter, recursing into nested structs, with no cap. A @unreal.ustruct subclass can override __repr__. 'Native repr' therefore breaks §8.8's own intro, RV-21 and the §8.3.6 saferepr rule, which shows structs as `<TypeName>`. The proposed fix only says the export text is 'cut at 200 characters'. A full export followed by a cut still exports a 100,000-element array first, so the work stays unbounded. The bound has to be per field, with container fields shown by count. T-INSP-004 is tier 2 with the fake unreal module and cannot hold real ustruct fixtures, so the test belongs at tier 4.

**Fix applied.** TDD.md §8.8 Variables table, row '| unreal struct | Fields via `get_editor_property` | Native `repr` |': replace the Value text cell with '`<StructName> {field: value, …}` from `_sumi.struct_export_text(v)` (C++, §12.1.1). It walks the struct's properties with `TFieldIterator<FProperty>` and exports each scalar field with `FProperty::ExportTextItem_Direct`. An array, set or map field shows only its count (`Array[N]`, `Set[N]`, `Map[N]`), and nested struct fields recurse under the same budget. It stops at 200 characters. Never `repr(v)`: the wrapper's repr exports every field and element (`PyWrapperStruct.cpp:1325-1326`, `PyUtil.cpp:1311-1356`) and can run a `@unreal.ustruct` subclass's `__repr__`.' Add a row to the §12.1.1 `_sumi` table: '| `struct_export_text(value: object) -> str | None` | GT | Bounded value text of an unreal struct wrapper, its subclasses included, as §8.8 defines; `None` for any other value | §8.8 |'. In §7's `_sumi` comment block (the line '#   container_element_kind(value) -> str | None; container_export_text(value, start, count) -> list[str];'), append 'struct_export_text(value) -> str | None;'. The new test is T-INSP-015, defined under V-runtime-13.

**Patch.** §8.8 struct value text now comes from a bounded _sumi.struct_export_text (containers show counts, 200-character cap, never repr). Added a row to the §12.1.1 table and added the functions to the §7 _sumi comment block.

#### V-runtime-12

**Minor, other.** Verdict: confirmed.

**Where:** research/13-v1-review.md, header paragraph ('SPEC v1.1 and TDD v1.1 apply every surviving fix') and RT-F01 'Fix.' paragraph

**Problem.** The review record still gives PyGILState_Ensure/PyGILState_Release as RT-F01's fix and says v1.1 applies every surviving fix. RV-01 replaced that mechanism, and fixer F1 recorded the deviation. A builder reading 13 is told the opposite of the TDD. The same applies to STU-F16's ModuleManager.cpp:1086-1087 citation, which the fixers corrected to :1035-1039.

**Evidence.** In 13-v1-review.md, the RT-F01 'Fix.' reads: 'The watchdog sets an atomic bInPython, re-checks bPythonAlive, calls PyGILState_Ensure() and at once PyGILState_Release(), then clears bInPython.' F1-arch.json records: 'SKIP RT-F01: The final_fix's PyGILState_Ensure/PyGILState_Release hand-off was replaced by the RV-01 mechanism (own thread state, PyEval_RestoreThread/PyEval_SaveThread)'.

**Refuter.** The header of research/13-v1-review.md says 'SPEC v1.1 and TDD v1.1 apply every surviving fix'. Its RT-F01 'Fix.' paragraph still prescribes 'calls PyGILState_Ensure() and at once PyGILState_Release()', which RV-01 replaced with an own PyThreadState plus PyEval_RestoreThread and PyEval_SaveThread. The F1 fixer recorded that deviation only in its scratch report, not in the documents. The STU-F16 entry still cites ModuleManager.cpp:1086-1087, while TDD §13 now cites :1035-1039, the FScopedBootTiming('LoadModule') scope I checked in the install. The precedence rule makes RV-01 win, but the header's claim is false and sends a reader to the wrong mechanism, so a cheap correction is worth making.

**Fix applied.** research/13-v1-review.md header paragraph: replace 'SPEC v1.1 and TDD v1.1 apply every surviving fix.' with 'SPEC v1.1 and TDD v1.1 apply every surviving fix, as amended by the 08 §9 rulings; where a fix below differs from a ruling, the ruling wins.' At the end of the RT-F01 'Fix.' paragraph, append: ' Superseded by RV-01: the watchdog takes the GIL on its own PyThreadState, created on the watchdog thread with PyThreadState_New, with PyEval_RestoreThread and then PyEval_SaveThread, not PyGILState_Ensure and PyGILState_Release.' At the end of the STU-F16 'Fix.' paragraph, append: ' In v1.1 the citation is `ModuleManager.cpp:1035-1039`, the `FScopedBootTiming("LoadModule")` scope.'

**Not applied in part.** Its whole fix edits research/13-v1-review.md (header, RT-F01 note, STU-F16 note). That file is outside the files this stage may edit (SPEC, TDD, README, mockups, ue.css, 08), so I left it unchanged. The TDD already follows RV-01, which wins by precedence.

#### V-runtime-14

**Minor, wrong engine fact.** Raised by the refuter.

**Where:** TDD §5.5.3 Teardown step 2

**Problem.** Step 2 re-owns HWNDs with `SetWindowLongPtr(GWL_HWNDPARENT)`. The Windows SDK #undefs GWL_HWNDPARENT under _WIN64, so the code as written does not compile on Win64. §8.3.1 step 11 correctly uses `GWLP_HWNDPARENT` and names `EnumThreadWindows`, and the two descriptions of the same teardown should match.

**Evidence.** Windows Kits/10/Include/10.0.22621.0/um/WinUser.h:1906 defines GWL_HWNDPARENT (-8); :1912-1916 `#ifdef _WIN64` … `#undef GWL_HWNDPARENT`; :1923 defines GWLP_HWNDPARENT (-8). The engine sidesteps this with a literal: PySlate.cpp:189 `::SetWindowLongPtr((HWND)ExternalWindowHandle, -8/*GWL_HWNDPARENT*/, ...)`. TDD §8.3.1 step 11 reads '(`EnumThreadWindows`, `SetWindowLongPtr(GWLP_HWNDPARENT)`, the call `PySlate.cpp:189` makes)'.

**Fix applied.** TDD.md §5.5.3 Teardown step 2: replace 'On Win64, re-own every top-level HWND whose `GW_OWNER` is the Run window's HWND to the main frame's HWND (`SetWindowLongPtr(GWL_HWNDPARENT)`, as `PySlate.cpp:189` sets it).' with 'On Win64, re-own every top-level HWND of the game thread whose `GW_OWNER` is the Run window's HWND to the main frame's HWND (`EnumThreadWindows`, `SetWindowLongPtr(GWLP_HWNDPARENT)`; `PySlate.cpp:189` makes the same call with the literal -8). This also covers the native windows of the Slate children detached in step 1, which Slate created owned by this window's HWND.'

**Patch.** §5.5.3 step 2 now uses EnumThreadWindows and SetWindowLongPtr(GWLP_HWNDPARENT), notes PySlate.cpp's literal -8, and covers the detached children's native windows.

### Round 1 — persist-policy

Persist-policy lens, v1.1. I checked every listed finding against SPEC, TDD §10, §11 and §12.4, mockups 06, 07, 08 and 10, and rulings RV-04 and RV-06 to RV-10. Every listed finding's fix is present in the documents it targets.
- **Policy key spellings:** every key in SPEC, TDD and the mockups matches the RV-07 canonical list. The only non-canonical strings are the deliberate misspelling fixtures, audit event names and the A7 history note.
- **Merge rules, tamper prompt, journal and lock:** the union, min, max and locked-roots rules and the built-in-defaults rule agree across SPEC, TDD and 12.4. The tamper prompt has no default action, its buttons have equal weight in RV-09 order, and `require_pinned` defaults to false everywhere. The journal record codes match between §10.2.2 and 12.4. The lock write path is consistent across §10.2.3, §10.5, §10.11 and 12.4.
- **Engine citations re-checked in the local install:** PythonScriptPlugin.cpp:2063-2080, PyUtil.cpp:1187-1195, WindowsPlatformFile.cpp:1216-1218, CoreDelegates.h:259, IDirectoryWatcher.h:55/:72 and WindowsPlatformProcess.cpp:1459-1471 are correct. ModuleManager.cpp:1372-1381 abandons module DLLs at shutdown, which supports CORE-F22's "code kept alive".

Remaining problems:
- **One major:** §11.2 says the M2 minimal resolver has no reload. T-DBG-043, T-ARCH-015 and SPEC DBG-07 still require `debug.allow_debug_server` turning off mid-session to close the listener in M2.
- **Wrong engine fact:** `{User}` is defined as `FPlatformProcess::UserName()`. By default that strips every non-alphanumeric character and carries no domain, so it cannot guarantee "two users never share a root".
- **Contradictions between parallel fixes:**
  - `telemetry.opt_in` in a policy file counts as an unknown `telemetry` key and would invalidate the layer, but T-TEL-005 says it has no effect.
  - The Invalid-column legend "D = use the built-in default" contradicts RV-08 (built-in defaults apply only when no layer sets the key) and the fall-through rule.
  - The new Terms line says a secondary writes no shared file, but the audit log, telemetry spool and LibraryState.json are shared and have no owner rule.
  - Mockup 06 shows a pending read-only `(recovered)` tab, while TDD §10.7.2 and §10.7.3 create that tab only on Restore, dirty and journaled.
  - In an invalid layer, the closed value of `language_services.binaries.<name>` is undefined, which can conflict with "language services stay on".
- **Lands values that cannot be met:** T-PER-053 at M1 needs policy layers. T-POL-009 at M2 needs policy-supplied binaries. The Linux cases of T-PER-046 and T-PER-048 at M1 need Linux tier-4 jobs, which start in M4.
- **Gaps in new design text:**
  - The M2 resolver is not required to apply the invalid-layer and unknown-key rules, so a misspelt `debug.allow_debug_server` fails open in M2-M3.
  - The checkpoint's final flush is not sliced.
  - The pending SNAP is bound only by position, which salvage can break.
  - The abandoned writer keeps doing I/O after the locks are released.
  - The scope of `storage.root_override` is undefined.
  - 12.4's REPL history schema lacks the new `id` field.
  - The mirror retries forever while a stale `manifest.lock` exists.

Scratch scripts are in scratchpad/verify2/persist/ (keys.py, secdiff.py, removed.txt, findings.txt, reports.txt).

No issues confirmed.

### Round 1 — delivery-tests

Delivery-tests lens, v1.1. All ten listed findings and the 17 minor QA findings landed in the prose of SPEC, TDD, 08 and the mockups. The rulings RV-11 to RV-15 and RV-24 read the same in TDD 15.1, 15.2, 13.2, 13.4, 14.1, 14.4, 15.4, 7.4.1, 7.10.1 and 10.10, and in SPEC §6, §10, INT-14, INT-15, PERF-07, COMPAT-05 and REL-01. I checked the new engine citations against the local 5.8 install and they are correct: StepExecutor.cpp:79/171, ModuleManager.cpp:1035-1039/1086-1087/1189/1217/1248-1250, SlateRHIRenderer.cpp:1512-1515.

The regressions are in the new Lands column. lands.py takes the latest Lands among the covered SPEC IDs before it looks at the component's milestone or at feature keywords, and its milestone detector only matches 'M1'-'M4' and 'Phase 0'. As a result:
- The G3 suite's server, selection and type-check tests (T-LSP-038, T-LSP-039, T-STUB-012) land in M2, which contradicts RV-11 and 15.2.
- G2's RT-03 EUW/AsyncTask test T-SUP-008 lands in M4, although Phase 0 and M1 need it.
- About a dozen M1 tests need M2-M4 or 1.2 features: Save All, the PyCharm Alt+Enter, trust, the library, async checkout, docked, Linux Preview, macOS.
- About twenty M1 tests contain M3/M4 sub-cases with no split value.
- Two overrides (T-UEI-015, T-UEI-009) push whole tests to M4. Together with T-VIEW-012 and T-NAV-006 being M2, the M1 parts of API-03, BUF-05, WS-05 (cursor) and NAV-03 have no M1 test.
- Two 1.0 tests carry Lands 'Later'.
- T-TEL-008, which 15.4 runs on every milestone build, is M4.
- No row uses 'Phase 0', even though 15.1 schedules the G1 and G2 suites there.
- The 1.5 rule 'the exit rule reads the first milestone' leaves every later split step out of every milestone exit.

T-GAP-008 as specified passes the current table (my simulation finds 0 violations), because it compares only against the covered requirement IDs. So the integrator's clean self-check does not catch these.

Of the 21 overrides, 19 are correct; T-UEI-015 and T-UEI-009 lose their M1 parts. Of the 4 hand rows, 3 are correct; T-GAP-008 should be Phase 0. I checked about 55 split-rule rows across 20 areas; the wrong ones are listed below.

Smaller contradictions:
- B-12a's Kind ('Gate') conflicts with OI-98, which treats it as Proposed.
- B-13's absolute check is assigned both to GPU-1 and to the Win64 harness agent.
- RV-15's 'while paused' kills cannot run in the driver-B run required before M1 exits.
- T-E2E-006 asserts an exact '40 / 300' without a latch, against the 14.2 synchronisation rule.

No issues confirmed.

### Round 1 — ux-mockups

I checked the ux-mockups lens findings and rulings RV-07 (mockup keys), RV-09, RV-16 to RV-20 and RV-22 against SPEC 9 and WS/REPL, TDD 4, 6, 10.7 and 11.6, all ten PNGs with their HTML sources, and the backup copies. Engine citations were checked against the local UE 5.8 install, and codeop behaviour was probed on the engine's 3.11.8 interpreter.

Most of the fixes landed. No image draws anything outside a Sumi modal while it is open, there is no dimmed frame, WS-07 has no Paused text, the policy keys are canonical, the integrity prompt has no default action, snippet insert shows no form, and the listed shadowed chords are complete.

Nine issues remain, two of them major:
- **V-ux-01 (major):** RUN-07 v1.1 says asset renames, moves and reimports are never undoable and that the run block says so. Images 01, 02 and 07 still show such runs as plainly undoable, T-SNP-014 has artists undo an asset Batch rename, and TDD 5.9.2 has no mechanism for the rename note.
- **V-ux-02 (major):** Ctrl+Shift+K (DeleteLine) shadows the Level Editor's EnableActorSnap at LevelEditorActions.cpp:4689. TDD 4.9.4 also says the extension rows appear on the page. The golden list omits both, and so does the PyCharm Ctrl+Shift+A shadow, so the G5 test T-CFG-011 fails against a correct page.

The seven minor issues:
- **V-ux-03:** an invented engine label for the Ctrl+PageUp/PageDown command.
- **V-ux-04:** the REPL prompt strip is missing under Output in four image panels.
- **V-ux-05:** image 06's pending conflict view and Review placement differ from TDD 10.7.
- **V-ux-06:** T-E2E-006 gives the input bot clicks that RV-16 says are consumed.
- **V-ux-07:** T-UEI-002 lands at M1 but checks M3 features.
- **V-ux-08:** the SPEC 9.2 Paused row omits Esc.
- **V-ux-09:** T-REPL-002 fails on lone `else:` and similar lines.

Probe scripts are in scratchpad/verify2/probe/.

No issues confirmed.

## Round 2

Scope: the round-1 patch: completeness, correctness against the engine, contradictions it introduced.

**Patch.** I applied all 56 confirmed issues to the documents: SPEC.md, TDD.md, 08-decision-log.md (RV-02 and RV-07, factual and clarifying corrections only) and mockups 01, 05, 07 and 10. Two parts were left out on purpose: the mockup 06 change, which the refuter marked optional, and a wording change to T-TXN-009 that the fix did not ask for.

Where two fixes edited the same text, I merged them:
- T-CFG-011's Lands combines W-delivery-S01 and W-ux-04 as 'M1 (M2: Ctrl+Shift+K row; M4: PyCharm and VS Code preset rows)'.
- RUN-07 and mockup 01 callout 2 combine W-ux-01 and W-ux-05.
- T-AUD-007 combines W-persist-03, -04 and -15.
- T-PER-055 combines W-persist-05, -06, -07 and -14.
- T-GAP-008 combines the term additions from W-delivery-03, -04, -06 and -S03 with the W-delivery-09 and -10 clauses.

Four small consistency edits beyond the listed fixes:
- The TDD §10.2.1 thread row now points at the new worker probe.
- The SPEC §9.5 caption for image 01 matches the new Run #14.
- Mockup 05 callout 2 now says 'with no rollback' instead of 'with its undo step kept', because that run renames assets.
- The SPEC D.3 intro now says what W- rows are.

Six D.3 rows were added for the SPEC requirement changes, and one new test, T-TXN-016, was added. Generated parts were regenerated with gen.py, and check.py prints PROBLEMS 0.

**Checks after the patch.** Final check.py output: "PROBLEMS 0 {}" (815 test rows, all unique). gen.py --write was re-run after the last test-row change; it updated the 14.1 TXN count, the 14.7 summary, the PER-03, PER-08, SEC-03 and AI-01 rows, and A-062. SPEC.md, TDD.md, README.md and 08 contain no CR characters (LF kept). I also simulated three T-GAP-008 rules on the final TDD: the lands_terms lint with the new term list, the new first-milestone clause, and the G1–G3 rule that no test's first Lands is after M1. Each flags 0 rows. The same simulation on the pre-patch documents flags exactly SEC-03, SEC-08 and AI-01, as the W-delivery-10 verifier reported. Engine references I added were checked against the local UE 5.8 source: ResetTransaction at EditorEngine.h:1067, OnAssetsPreDelete at Editor.h:378. Every re-rendered PNG was opened and inspected. Images 01, 07 and 10 needed taller canvases so the callout legends do not clip; image 05 kept its size. Scratch scripts are in scratchpad/verify3/patch/, and pre-patch copies are there as TDD.before.md, SPEC.before.md and 08.before.md.

### Round 2 — Runtime

Runtime lens, round 2. I checked all 14 round-1 runtime fixes against the current SPEC, TDD, 08, 07a and 13. SPEC, TDD and 08 match the backup-r2 snapshot byte for byte. 07a and 13 had their skipped edits (V-runtime-03's 07a line and V-runtime-12) applied after the snapshot, and both are now in place. I checked the mechanisms against the engine source and with probes:
- Slate child-window recipe (SWindow.cpp:1863-1912, SlateApplication.cpp:2124-2280, 2423-2457, 3248-3289 and 7352-7388): RemoveDescendantWindow returns false for a direct child, and AddChildWindow checks that return value. The detach, destroy, re-attach order works, including after the blocking AddModalWindow returns in the Paused case.
- pydevd thread tracer (debugpy 1.8.20 probe on the engine python): `threading.gettrace() is pydevd.get_global_debugger().trace_dispatch` holds after listen, and it holds again after a swap and compare-and-restore.
- Hand-off timing (round-1 wd.dll probe re-run): post to callback was 5.45/6.48 ms (p50/max) at 1 ms resolution and 15.55/16.57 ms at the default resolution. Gaps between callbacks were 72-73 ms and 93.5-95 ms for a 66 ms period, so the +10 ms and +40 ms margins hold.
- Engine facts: timeBeginPeriod(1) is at WindowsPlatformMisc.cpp:1076; soft-property pythonize loads through LoadObjectPropertyValue; FSoftObjectProperty export text does not load; GWLP/-8 is at PySlate.cpp:189.

The new problems mostly come from the V-runtime-04 run-ID swap and the V-runtime-02 debugpy text:
- Major: swapping CurrentRunId to the EvalId breaks the §5.7.6 run-ID guards of `_sumi.write` and the run's writers. Evaluate output then lands as background, so T-DBG-047 cannot pass as designed.
- T-DBG-029 names the wrong place where its stale pending call is rejected. A probe confirms the call fires before `_sumi.eval_begin`.
- 08 RV-02 still says to re-parent before the destroy, which is the recipe that crashes.
- pydevd's `sys.settrace` wrapper writes 'PYDEV DEBUGGER WARNING' into the output of the new no-prompt Debug runs (probe-confirmed).
- The DBG-06 wording leaves out threads already running when the server starts (probe-confirmed).
- The Free row depends on `is_listening()`, so after a policy close every F5 shows the foreign-tracer prompt.
- T-SUP-032's case without timeBeginPeriod contradicts the harness, which always calls timeBeginPeriod(1).
- The EntrySerial and run-ID definitions were not updated for supervised evaluates.
- One older problem nobody caught: T-LIFE-008's pass criterion contradicts its own scenario.

Scratch probes are in scratchpad/verify3/rt/ (probe/dbg_probe.py, probe/dbg_probe2.py, wd/early.py, wd/wd_probe2.py).

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [W-runtime-01](#w-runtime-01) | major | contradiction | modified | The V-runtime-04 fix makes BeginSupervisedEvaluate set CurrentRunId to the EvalId for the whole evaluate. |
| [W-runtime-02](#w-runtime-02) | minor | untestable | modified | The rewritten T-DBG-029 says a pending call queued before the pause 'fires at the start of an evaluate' and returns at the run-ID guard. |
| [W-runtime-03](#w-runtime-03) | minor | patch incomplete | modified | V-runtime-01 changed the recipe: children are detached before the destroy and attached to the root only after the window is gone, because the root's AddChildWindow `check`s while the old parent is alive. |
| [W-runtime-04](#w-runtime-04) | minor | wrong engine fact | modified | V-runtime-02 makes a Debug run started while the server listens with no client a no-prompt path. |
| [W-runtime-05](#w-runtime-05) | minor | wrong engine fact | confirmed | The new text says only threads started after the debug server starts are traced by pydevd. |
| [W-runtime-06](#w-runtime-06) | minor | patch incomplete | modified | The new Free condition accepts pydevd's thread dispatcher only 'while DebugpyBridge.is_listening()'. |
| [W-runtime-07](#w-runtime-07) | minor | untestable | confirmed | V-runtime-03 makes the tier-3 harness call timeBeginPeriod(1) at start, unconditionally. |
| [W-runtime-08](#w-runtime-08) | minor | patch incomplete | confirmed | V-runtime-04 makes each supervised evaluate's EvalId an FSumiRunId drawn from the same EntrySerial counter, and updated only the §5.2.5 comment. |
| [W-runtime-09](#w-runtime-09) | minor | untestable | confirmed | This is an older problem next to the V-runtime-05 text. |
| [W-runtime-10](#w-runtime-10) | minor | wrong engine fact | added by refuter | The engine's CPython uses frozen stdlib modules, so `os.path.realpath.__code__.co_filename` is '<frozen ntpath>'. |
| [W-runtime-11](#w-runtime-11) | minor | wrong engine fact | added by refuter | The V-runtime-13 patch makes soft, soft-class and lazy object properties leaves that hold 'their path text from `_sumi.prop_export_text`', claiming `ExportTextItem_Direct` 'writes the path without loading'. |

#### W-runtime-01

**Major, contradiction.** Verdict: modified.

**Where:** TDD §8.3.4 Supervised evaluates, first bullet; TDD §5.4.2 step 1; TDD §5.7.6 'Run-ID guards' bullet; TDD §12.1.1 rows write and current_run_id; TDD §6.2 bind rule (line 'Before each entry into `_run` ... it calls `BindRunId(Id, RunNumber)`'); TDD §8.11 T-DBG-047

**Problem.** The V-runtime-04 fix makes BeginSupervisedEvaluate set CurrentRunId to the EvalId for the whole evaluate. The rest of the design still keys output attribution and the stale-ID checks on CurrentRunId: `_sumi.write` and the run's writers check it (§5.7.6), and current_run_id() returns it. During an evaluate, every write that carries the paused run's ID (the run's stdout and stderr Writer, STREAM_DEBUGGER and STREAM_EVALUATE notices, the paused run's worker threads) is therefore treated as no longer active and tagged `background`. A write that carries the EvalId goes to an ID that BindRunId never bound, which ingest tags run 0 and Background. Either way, print() inside an evaluate cannot become `evaluate` lines in the run block as T-DBG-047 requires. The stale-ID RuntimeError rule also applies to any `_sumi` call made with the run's ID inside an evaluate.

**Evidence.** §8.3.4: '`BeginSupervisedEvaluate` saves `CurrentRunId`, sets it to the `EvalId` ... `EndSupervisedEvaluate` disarms and restores the saved ID.' §5.7.6: '**Run-ID guards.** Pending calls carry the run ID. `check_cancel()`, `_sumi.write` and the alternate channel check `CurrentRunId`.' §12.1.1 write: 'a run ID that is no longer active tags the line `background` (D24)'. §6.2: 'An ID that was never bound ... is tagged run 0 and `Background`.' T-DBG-047: '`print` in an evaluate | ... output in the console and as `evaluate` lines'.

**Refuter.** The problem is real. TDD §8.3.4 sets CurrentRunId to the EvalId for the whole evaluate. §5.7.6 says `_sumi.write` checks CurrentRunId, and §12.1.1 says a run ID that is no longer active tags the line `background`. So the evaluate's output, the run's Writer, debugger notices and the paused run's workers are all written with the run's ID and come out as background lines. A write carrying the EvalId is never bound (§6.2) and lands in run 0. Nothing in the documents says which ID the evaluate's STREAM_EVALUATE writes use, so T-DBG-047's 'evaluate lines in the run block' cannot be built as written. The verifier's ActiveEvalId direction is right but leaves gaps. (a) §5.4.1 posts `(void*)RunId`, so nothing makes the watchdog post the EvalId; under the verifier's step-1 rule, if it kept posting CurrentRunId, every evaluate call would be refused and T-DBG-031 would fail. (b) The evaluate's Cancel path (RequestStop and the stop state) worked only because CurrentRunId was the EvalId. (c) The ID that the evaluate's print output is written with is never stated.

**Fix applied.** TDD.md. (1) §8.3.4 first bullet: replace '`BeginSupervisedEvaluate` saves `CurrentRunId`, sets it to the `EvalId` and arms the watchdog with the 300 ms delay, so the §5.4.2 step 1 guard accepts the evaluate's calls and rejects any call still carrying the paused run's ID. `EndSupervisedEvaluate` disarms and restores the saved ID.' with '`BeginSupervisedEvaluate` sets `ActiveEvalId` to the `EvalId` and arms the watchdog with the 300 ms delay. While `ActiveEvalId` is nonzero the watchdog posts it instead of `CurrentRunId` (§5.4.1), so the §5.4.2 step 1 guard accepts the evaluate's calls and rejects any call still carrying the paused run's ID. `CurrentRunId` stays the paused run's ID, so `_sumi.write`, the run's writers, `current_run_id()`, thread attribution and the other run-ID checks (§5.7.6, §6.1, §12.1.1) keep attributing output to the paused run. The evaluate writes its `print` output with `_sumi.write(STREAM_EVALUATE, run_id, text)`, where `run_id` is the paused run's ID, so the run's block receives `evaluate` lines (T-DBG-047). `EndSupervisedEvaluate` disarms and sets `ActiveEvalId` to 0.' (2) §5.4.1, third bullet: replace 'calls `Py_AddPendingCall(&PendingCallTrampoline, (void*)RunId)` under `PostLock`.' with 'calls `Py_AddPendingCall(&PendingCallTrampoline, (void*)PostId)` under `PostLock`, where `PostId` is `ActiveEvalId` while a supervised evaluate is active and `CurrentRunId` otherwise (§8.3.4).' (3) §5.4.2 step 1: replace 'If `Id != CurrentRunId` or the run has ended, return 0. During a supervised evaluate, `CurrentRunId` is its `EvalId` (§8.3.4).' with 'While a supervised evaluate is active, return 0 unless `Id == ActiveEvalId` (§8.3.4). Otherwise, if `Id != CurrentRunId` or the run has ended, return 0.' (4) §5.7.5 condition 4: replace '`CurrentRunId` unchanged' with '`CurrentRunId` and `ActiveEvalId` unchanged'. (5) §5.7.6: replace '**Run-ID guards.** Pending calls carry the run ID. `check_cancel()`, `_sumi.write` and the alternate channel check `CurrentRunId`.' with '**Run-ID guards.** Pending calls carry the run ID, or `ActiveEvalId` during a supervised evaluate (§8.3.4). `check_cancel()`, `_sumi.write` and the alternate channel check `CurrentRunId`, which stays the paused run's ID during an evaluate; `check_cancel()` on the game thread during an evaluate reports the evaluate's stop state.' (6) §8.3.4 Cancel bullet: replace 'Cancel delivers `ScriptCancelled` to `EvalId` under the D2 and RT-06 rules.' with 'Cancel calls `FRunSupervisor::RequestStop(EvalId)`. While `ActiveEvalId == EvalId`, that sets the evaluate's stop state and leaves the paused run's untouched, and the next pending call delivers `ScriptCancelled` to the evaluate under the D2 and RT-06 rules.'

**Patch.** TDD §8.3.4 first bullet (ActiveEvalId; CurrentRunId stays the paused run's; evaluate print goes to STREAM_EVALUATE with the run's ID), §5.4.1 posts PostId, §5.4.2 step 1 guard, §5.7.5 condition 4, §5.7.6 Run-ID guards, §8.3.4 Cancel bullet now calls RequestStop(EvalId)

#### W-runtime-02

**Minor, untestable.** Verdict: modified.

**Where:** TDD §8.11 T-DBG-029; TDD §5.4.2 steps 1-10; TDD §8.3.1 step 2

**Problem.** The rewritten T-DBG-029 says a pending call queued before the pause 'fires at the start of an evaluate' and returns at the run-ID guard. In fact the call fires at the first eval-breaker check after the request handler takes the GIL, which is inside handle_request before Python reaches `_sumi.eval_begin`. At that point CurrentRunId is still the paused run's ID, so the call passes step 1. It is rejected only by the 'IsPaused() and no supervised evaluate is active' rule in §8.3.1 step 2, and that rule is not a step in §5.4.2, the callback's authoritative step list. The test's observed return point therefore cannot match its criterion. Probe (verify3/rt/wd/early.py): a native-thread post made while the main thread slept had already run by the first line of the next Python function.

**Evidence.** T-DBG-029: 'a pending call carrying the paused run's ID, queued before the pause, fires at the start of an evaluate ... | It returns at the run-ID guard (§5.4.2 step 1)'. §8.3.1 step 2: 'The pending-call callback (§5) returns 0 while `IsPaused()` and no supervised evaluate is active (§8.3.4)'. The §5.4.2 steps go from '1. **Run-ID guard.**' to '2. **Bookkeeping.**' with no paused step. §8.1.5: 'def eval_begin(run_id: int) -> int' (called from Python). Probe output: {"inside_first_function": [1, 1]}.

**Refuter.** The problem is real. The Paused-window request handler takes the GIL, and on the game thread take_gil recomputes the eval breaker. The pending bit is then set, and the queued call runs at the RESUME check of the first Python function the handler calls, before `_sumi.eval_begin` (CallDebugger enters Python first). At that point CurrentRunId is still the paused run's ID, so step 1 lets the call through. Only the §8.3.1 step 2 rule, which is missing from the §5.4.2 step list, turns it away. Without that rule it would be deferred at step 5, after the bookkeeping and the drain. T-DBG-029's 'returns at the run-ID guard' is therefore wrong. The verifier's fix is right, but its scenario still cannot be set up as written. A call queued from Python just before a breakpoint fires inside the trace callback (at bdb's RESUME), before the pause, so 'queued before the pause' cannot be arranged. A no-hand-off post from a non-main thread while the Paused window is idle produces the stale call (the verifier's early.py probe shows this). Also, the first Python code after the post may belong to a request other than the evaluate.

**Fix applied.** TDD.md. (1) §5.4.2: after step 1 insert '1a. **Paused gate.** If `IsPaused()` and no supervised evaluate is active, return 0: no bookkeeping, no drain, no pump, no cancellation, no `SetAsyncExc` (§8.3.1 step 2). A call left queued while paused runs in the first Python code a Paused-window request handler runs, before any `_sumi.eval_begin`, and returns here.' (2) §8.11: replace the T-DBG-029 row with '| T-DBG-029 | 4 | Pump inert while paused: while the Paused window is idle, a fixture thread calls `PostSupervisorCall()` with the paused run's ID (no GIL hand-off, so the call waits for the game thread's next GIL acquisition); then an evaluate is requested that ends before the 300 ms arm delay, and the evaluated code calls `PostSupervisorCall(RunId)` with the paused run's ID on the game thread | The first call runs in the first Python code the game thread runs after the post, before `_sumi.eval_begin`, and returns at the paused gate (§5.4.2 step 1a); the second returns at the run-ID guard (step 1); no Slate tick from the pump, no delivery; the run is still paused afterwards | D3, D2 | M3 |'. (3) §5.15 introduction: replace '`PostSupervisorCall()`, which calls `Py_AddPendingCall` with our callback from the calling thread' with '`PostSupervisorCall(RunId)`, which calls `Py_AddPendingCall` with our callback and `RunId` (default: the current run's ID) from the calling thread'. (4) §14.2 fixtures row 'Editor test fixtures in `SumiEditorTests`': replace '`PostSupervisorCall()` (calls `Py_AddPendingCall` with Sumi's callback from the calling thread, with no GIL hand-off)' with '`PostSupervisorCall(RunId)` (calls `Py_AddPendingCall` with Sumi's callback and `RunId`, default the current run's ID, from the calling thread, with no GIL hand-off)'.

**Patch.** TDD §5.4.2 new step 1a Paused gate; T-DBG-029 rewritten (fixture-thread post while idle, then an in-evaluate PostSupervisorCall(RunId)); §5.15 intro and §14.2 fixtures row give PostSupervisorCall(RunId)

#### W-runtime-03

**Minor, patch incomplete.** Verdict: modified.

**Where:** research/08-decision-log.md §9 RV-02; TDD §8.3.1 step 11, first sentence

**Problem.** V-runtime-01 changed the recipe: children are detached before the destroy and attached to the root only after the window is gone, because the root's AddChildWindow `check`s while the old parent is alive. The ruling that wins by precedence still prescribes re-parenting before the destroy. Read literally, RV-02 tells a builder to use the recipe that crashes, and it overrides the TDD. §8.3.1 step 11 also opens with 'Before RequestDestroyWindow, Sumi re-parents', although its step 4 runs after the destroy.

**Evidence.** 08 RV-02: 'Before either window is destroyed, Sumi re-parents its Slate child windows to the root window and, on Win64, moves every top-level HWND it owns to the main frame.' TDD §5.5.3 step 1: 'Never call the root's `AddChildWindow` while a child's old parent is alive: it calls the old parent's `RemoveDescendantWindow` and `check`s the result'. The engine source confirms this: SWindow.cpp:1869-1870 `check(bRemovedSuccessfully)`, and RemoveDescendantWindow at :1896-1911 returns false for a direct child. TDD §8.3.1 step 11: 'Before `RequestDestroyWindow`, Sumi re-parents what the window owns (RV-02): ... by §5.5.3 Teardown steps 1–4'.

**Refuter.** The problem is real. RV-02, which wins by precedence, still says the children are re-parented to the root before the destroy. The local SWindow.cpp:1863-1870 calls `check(bRemovedSuccessfully)` after `PreviousParent->RemoveDescendantWindow`, and RemoveDescendantWindow (:1896-1911) returns false for a direct child. So the RV-02 order is fatal while the old parent is alive. The §8.3.1 step 11 opener is wrong in the same way. The verifier missed one more place: the paragraph after the §8.3.1 steps still says 'each destroy re-parents first (step 11)'.

**Fix applied.** (1) research/08-decision-log.md §9 RV-02: replace 'Before either window is destroyed, Sumi re-parents its Slate child windows to the root window and, on Win64, moves every top-level HWND it owns to the main frame.' with 'Before either window is destroyed, Sumi detaches its Slate child windows (`RemoveDescendantWindow`) and, on Win64, moves every top-level HWND it owns to the main frame; once the window is gone, it attaches the kept children to the root window (TDD §5.5.3 Teardown). The root's `AddChildWindow` is never called while a child's old parent is alive, because it `check`s (`SWindow.cpp:1863-1872`).' (2) TDD §8.3.1 step 11: replace 'Resume. Before `RequestDestroyWindow`, Sumi re-parents what the window owns (RV-02):' with 'Resume. Around `RequestDestroyWindow`, Sumi re-parents what the window owns (RV-02):'. (3) TDD §8.3.1, the paragraph after step 11: replace 'and each destroy re-parents first (step 11).' with 'and each destroy detaches the window's child windows first and re-parents them to the root once the window is gone (step 11).'

**Patch.** 08 RV-02 recipe corrected (detach first, attach to root once the window is gone; SWindow.cpp:1863-1872 check); TDD §8.3.1 step 11 'Around RequestDestroyWindow' and the paragraph after it

#### W-runtime-04

**Minor, wrong engine fact.** Verdict: modified.

**Where:** TDD §8.1.4 bullet 'While the debug server listens with no client…'; TDD §8.6.2 'Game-thread tracer off while no IDE is attached (RV-03)'; TDD §8.11 T-DBG-052

**Problem.** V-runtime-02 makes a Debug run started while the server listens with no client a no-prompt path. When the in-process listen builds PyDB, pydevd also replaces `sys.settrace` with `pydevd_tracing._internal_set_trace`. That wrapper writes a multi-line 'PYDEV DEBUGGER WARNING: sys.settrace() should not be used when the debugger is being used. This may cause the debugger to stop working correctly…' to sys.stderr, once per distinct call stack, for any caller whose file is not threading or pydevd_tracing. SettraceBackend (sys.settrace(self._on_call)) and bdb (set_trace, set_continue) are such callers. During a Debug run sys.stderr is the run's Writer, so these warnings appear in the user's run output. No section mentions the wrapper. Probe (verify3/rt/probe/dbg_probe.py, debugpy 1.8.20, engine python): after listen, sys.settrace.__name__ == '_internal_set_trace', and a sys.settrace call from sumi_backend.py printed the warning to the captured stderr.

**Evidence.** pydevd.py:577-579 `def __init__(self, set_as_global=True): if set_as_global: pydevd_tracing.replace_sys_set_trace_func()`. pydevd_tracing.py:30 `_warn = True`, :58-90 (`_internal_set_trace` writes the message to sys.stderr), :125-130 (`sys.settrace = _internal_set_trace`). TDD §8.1.4: 'A Debug run started in that state replaces pydevd's thread tracer with `SettraceBackend`'s for the run, with no prompt'. TDD §8.1.3: 'Call filter. `sys.settrace(self._on_call)`'. Probe output: "sys_settrace_replaced": "_internal_set_trace", "stderr_during_backend": "\nPYDEV DEBUGGER WARNING:\nsys.settrace() should not be used when the debugger is being used...".

**Refuter.** Confirmed against the pinned debugpy 1.8.20 (VS Code extension copy) and by re-running the probe on the engine python. pydevd.py:577-579 calls `replace_sys_set_trace_func()`. pydevd_tracing.py:58-90 writes the PYDEV DEBUGGER WARNING to sys.stderr once per distinct call location, for any caller whose file is not threading or pydevd_tracing. After listen, `sys.settrace.__name__` is '_internal_set_trace', and a backend `sys.settrace` call printed the warning to the captured stderr. Nothing in the documents mentions this. The verifier's fix says 'T-DBG-040 pins that private name' but does not edit T-DBG-040 or OI-76, so the pin would not exist.

**Fix applied.** TDD.md. (1) §8.6.2, at the end of the 'Game-thread tracer off while no IDE is attached (RV-03)' bullet (after 'if it fails, the §8.1.4 fallback applies.'), append: 'The PyDB constructor also replaces `sys.settrace` with `pydevd_tracing._internal_set_trace` (`debugpy/_vendored/pydevd/pydevd.py:577-579`, `pydevd_tracing.py:58-90, 125-130`). That wrapper writes a "PYDEV DEBUGGER WARNING" to `sys.stderr` for callers such as `SettraceBackend` and `bdb`, and during a Debug run `sys.stderr` is the run's writer. Right after `listen` returns, `DebugpyBridge` sets `pydevd_tracing.TracingFunctionHolder._warn = False`; T-DBG-040 pins that private name.' (2) §8.11 T-DBG-040 scenario: replace '`py_db.initialize_network` and `do_kill_pydev_thread()` exist and behave as §8.6.2 uses them' with '`py_db.initialize_network` and `do_kill_pydev_thread()` exist and behave as §8.6.2 uses them; `pydevd_tracing.TracingFunctionHolder._warn` exists'. In its pass criterion, after 'after `do_kill_pydev_thread()` a connect to the port is refused' append '; with `_warn = False`, a `sys.settrace` call from a file outside pydevd and `threading` writes nothing to `sys.stderr`'. (3) §17.1 OI-76: after 'the RV-04 listener wrapper also relies on the private `_waiting_for_connection_thread._server_socket` and `initialize_network`' insert ', and §8.6.2 sets the private `pydevd_tracing.TracingFunctionHolder._warn`'. (4) T-DBG-052 pass criterion: after '(from M3) F5 starts a Debug run with no prompt' insert ', whose output has no `PYDEV DEBUGGER WARNING` line,'.

**Patch.** TDD §8.6.2 RV-03 bullet adds the pydevd_tracing._internal_set_trace warning and TracingFunctionHolder._warn = False; T-DBG-040 pins _warn (scenario and pass); OI-76 names it; T-DBG-052 asserts no PYDEV DEBUGGER WARNING line

#### W-runtime-05

**Minor, wrong engine fact.** Verdict: confirmed.

**Where:** SPEC DBG-06; SPEC Appendix D.3 row V-runtime-02; TDD §8.6.2 'Once per editor session' (confirmation text); TDD §8.1.3 'When the trace is installed', last two sentences

**Problem.** The new text says only threads started after the debug server starts are traced by pydevd. pydevd's in-process settrace also installs its tracer on every Python thread already running at listen time (set_trace_to_threads), and §8.1.3's own earlier sentence says so. The SPEC requirement, its D.3 row and the user-facing 'Debug in VS Code' confirmation therefore understate the cost: a studio tool's background thread that is already running becomes traced. Probe (verify3/rt/probe/dbg_probe2.py): a thread started before listen had sys.gettrace() None before and a pydevd SafeCallWrapper after listen and trace_this_thread(False).

**Evidence.** SPEC DBG-06: 'Python threads started after the debug server starts, such as a run's worker threads, are traced by pydevd until the editor closes'. TDD §8.6.2: 'the Debug in VS Code confirmation adds "Python threads started from now on run under the debugger's tracer until the editor closes"'. TDD §8.1.3: 'The in-process debug server installs pydevd's tracer on every thread when it starts listening'. Probe: {"pre_before": "None", "pre_after": "<_pydevd_bundle.pydevd_cython.SafeCallWrapper object ..."}.

**Refuter.** I re-ran dbg_probe2.py on the engine python with debugpy 1.8.20. A thread started before listen had `sys.gettrace()` None before, and pydevd's SafeCallWrapper after listen and trace_this_thread(False). The TDD's own §8.1.3 says pydevd installs its tracer 'on every thread when it starts listening', and `attach_amd64.dll` is kept for `set_trace_to_threads` (§8.6.1). So SPEC DBG-06, the D.3 row, the §8.1.3 sentence added in round 1 and the confirmation text all understate the cost. The verifier's fix is correct; the D.3 text is spelled out below.

**Fix applied.** (1) SPEC.md DBG-06: replace 'Python threads started after the debug server starts, such as a run's worker threads, are traced by pydevd until the editor closes' with 'Python threads other than the game thread, both those running when the debug server starts and those started later (a run's worker threads among them), are traced by pydevd until they end or the editor closes'. (2) SPEC.md Appendix D.3 row V-runtime-02: replace 'Python threads started after the debug server starts are traced by pydevd until the editor closes, and the Debug in VS Code confirmation says so' with 'Python threads other than the game thread, both those running when the debug server starts and those started later, are traced by pydevd until they end or the editor closes, and the Debug in VS Code confirmation says so'. (3) TDD §8.6.2 'Once per editor session': replace '"Python threads started from now on run under the debugger's tracer until the editor closes"' with '"Python threads other than the editor's main thread, running now or started later, run under the debugger's tracer until the editor closes"'. (4) TDD §8.1.3 'When the trace is installed': replace 'so Python threads started after the debug server starts, a run's worker threads among them, are traced by pydevd until the editor exits' with 'so Python threads running when the debug server starts, and those started later (a run's worker threads among them), are traced by pydevd until they end or the editor exits'.

**Patch.** SPEC DBG-06 and the D.3 V-runtime-02 row, TDD §8.6.2 confirmation text and §8.1.3 sentence now include threads already running when the server starts

#### W-runtime-06

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §8.1.4 owner table, row Free; TDD §8.1.4 Fallback bullet; TDD §8.6.2 'Closing (RV-04)'

**Problem.** The new Free condition accepts pydevd's thread dispatcher only 'while DebugpyBridge.is_listening()'. The listener can close mid-session when debug.allow_debug_server turns false (RV-04). Closing only kills the waiting thread's socket: threading.settrace(trace_dispatch) and the thread-start patch stay for the session, and the server cannot restart. From then on the slot reads Foreign, and every F5 shows the 'Another tracer is active' prompt for the rest of the session. `is_listening()` is not defined anywhere else in the TDD.

**Evidence.** §8.1.4 Free: '`threading.gettrace()` is `None` or, while `DebugpyBridge.is_listening()` and `is_client_connected()` is false, is pydevd's own dispatcher'. §8.6.2: '`DebugpyBridge` calls pydevd's `do_kill_pydev_thread()` on `py_db._waiting_for_connection_thread`, which closes the listening socket' and 'a closed server cannot start again in that session'. Only pydevd.stoptrace() (pydevd.py:3216-3231) undoes threading.settrace, and Sumi never calls it.

**Refuter.** The problem is real. pydevd's `_WaitForConnectionThread.do_kill_pydev_thread` (pydevd.py:1588-1596) only closes the server socket. `threading.settrace(trace_dispatch)` and the thread-start patch stay in place, and only `stoptrace` undoes them, which Sumi never calls. After an RV-04 close, a check keyed on 'listening' makes the slot read Foreign, and every later F5 shows the prompt. `is_listening()` is not defined anywhere, and the DebugpyBridge interface row in §12 lists only `start()`, `poll_port()` and `is_client_connected()`. So the verifier's new `started()` also has to be added to that row.

**Fix applied.** TDD.md. (1) §8.1.4 Free row: replace 'while `DebugpyBridge.is_listening()` and `is_client_connected()` is false' with 'once `DebugpyBridge.started()` is true (the debug server was started in this session; it stays true after the listener closes, §8.6.2) and while `is_client_connected()` is false'. (2) §8.1.4 Fallback bullet: replace 'detected by `DebugpyBridge.is_listening()`' with 'detected by `DebugpyBridge.started()`'. (3) §12 interface row '`_impl.debugger.DebugpyBridge`: `start()`, `poll_port()`, `is_client_connected()`, and the close through pydevd's `do_kill_pydev_thread()`': replace '`start()`, `poll_port()`,' with '`start()`, `started()` (true once the server has started in this session, also after the listener closes), `poll_port()`,'. (4) §8.11 T-DBG-052: append to the scenario '; then `debug.allow_debug_server` is turned off', and append to the pass criterion '; after the listener closes, F5 still starts a Debug run with no prompt (from M3)'.

**Patch.** TDD §8.1.4 Free row and Fallback bullet use DebugpyBridge.started(); §12 DebugpyBridge interface row adds started(); T-DBG-052 scenario turns debug.allow_debug_server off and checks F5 after the listener closes (from M3)

#### W-runtime-07

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD §5.15.3 T-SUP-032; TDD §14.1 tier-3 row (Harness cell); TDD §5.13 row 'GIL hand-off after a post'

**Problem.** V-runtime-03 makes the tier-3 harness call timeBeginPeriod(1) at start, unconditionally. It also adds a T-SUP-032 case 'without timeBeginPeriod'. Timer resolution is per process, so that case cannot run in a process that already called timeBeginPeriod(1) unless it calls timeEndPeriod(1) first, and neither row says to. The §5.13 row also still states 'each callback runs within one period plus 10 ms' with no condition, while the same row says Windows 11 may apply the 15.6 ms default to a minimised editor. At that resolution the measured gap is about 94 ms for a 66 ms period, beyond period + 10 ms (re-run of wd_probe2.py: default p50 93.5 ms, max 95.08 ms; 1 ms p50 71.99 ms, max 73.17 ms).

**Evidence.** T-SUP-032: 'On Win64 the harness calls `timeBeginPeriod(1)` at start, as the editor does. ... Win64 only, second case without `timeBeginPeriod`: each callback within one period plus 40 ms of the previous, p99'. §14.1 tier 3: 'on Win64 it calls `timeBeginPeriod(1)` at start'. §5.13: 'against `while True: pass` each callback runs within one period plus 10 ms'.

**Refuter.** §14.1 makes the tier-3 harness call timeBeginPeriod(1) unconditionally at start, and since Windows 10 2004 the timer resolution is per process. T-SUP-032's second case 'without timeBeginPeriod' therefore cannot run in that process unless it first calls timeEndPeriod(1), and neither row says so. The §5.13 row still states 'one period plus 10 ms' unconditionally, while the §13.2 row is already qualified 'at 1 ms timer resolution'. At the 15.6 ms default, both the period wait and the hand-off round up (about 78 + 16 ms against a 66.7 ms period), which is beyond +10 ms. The verifier's fix is correct.

**Fix applied.** As proposed. TDD §5.15.3 T-SUP-032: replace 'Win64 only, second case without `timeBeginPeriod`:' with 'Win64 only, second case after `timeEndPeriod(1)` (the harness calls `timeBeginPeriod(1)` again when it ends):'. §14.1 tier-3 Harness cell: after 'so timed waits match the editor' append '; T-SUP-032's default-resolution case calls `timeEndPeriod(1)` first'. §5.13 row 'GIL hand-off after a post': replace 'against `while True: pass` each callback runs within one period plus 10 ms' with 'against `while True: pass` each callback runs within one period plus 10 ms at 1 ms timer resolution, and within one period plus 40 ms at the 15.6 ms default (T-SUP-032)'.

**Patch.** T-SUP-032 second case after timeEndPeriod(1); §14.1 tier-3 Harness cell note; §5.13 hand-off row qualified for 1 ms and 15.6 ms resolution

#### W-runtime-08

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD §5 Terms table, row 'Run ID'; TDD §8.1.5, sentence after the C++ block; TDD §12.1.2 first paragraph

**Problem.** V-runtime-04 makes each supervised evaluate's EvalId an FSumiRunId drawn from the same EntrySerial counter, and updated only the §5.2.5 comment. The three other definitions of the run ID and of EntrySerial still say one per entry into `_run`. A builder who follows §12.1.2 or §5 Terms would give EvalIds a separate counter, or think they collide with run IDs.

**Evidence.** §5 Terms: 'Run ID | `uint64`, unique per entry into `_run` within the editor process ... `EntrySerial`, a per-process counter bumped for every entry into `_run`'. §8.1.5: '`FSumiRunId` is the 64-bit ID of §12.1.2, one per entry into `_run`.' §12.1.2: '`EntrySerial` is a per-process counter bumped at every entry into `_run`'. §5.2.5 (patched): '// EntrySerial: per-process counter, bumped for every entry into _run and every supervised evaluate'.

**Refuter.** Only the §5.2.5 comment was updated. §5 Terms (Run ID row), §8.1.5 ('one per entry into `_run`') and §12.1.2 ('bumped at every entry into `_run`') still define EntrySerial and the run ID per `_run` entry only. That contradicts §8.3.4, where the EvalId is an FSumiRunId with a fresh EntrySerial. The verifier's three replacements are exact and sufficient. §2's 'Run' glossary entry describes runs only and can stay.

**Fix applied.** As proposed. TDD §5 Terms, row 'Run ID': replace 'unique per entry into `_run` within the editor process' with 'unique per entry into `_run`, and per supervised evaluate (§8.3.4), within the editor process', and replace 'a per-process counter bumped for every entry into `_run`' with 'a per-process counter bumped for every entry into `_run` and every supervised evaluate'. §8.1.5, sentence after the C++ block: replace 'one per entry into `_run`.' with 'one per entry into `_run`; each supervised evaluate's `EvalId` is one too (§8.3.4).' §12.1.2: replace 'bumped at every entry into `_run`' with 'bumped at every entry into `_run` and every supervised evaluate'.

**Patch.** TDD §5 Terms Run ID row, §8.1.5 and §12.1.2 count supervised evaluates in EntrySerial / FSumiRunId

#### W-runtime-09

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD §3.9 T-LIFE-008

**Problem.** This is an older problem next to the V-runtime-05 text. T-LIFE-008's first scenario is shutdown while the watchdog is already waiting in PyEval_RestoreThread. By design (§3.8 step 2, §5.4.1 Shutdown safety), that hand-off completes after bPythonAlive is cleared: the watchdog gets the GIL, gives it back and clears bInPython. The pass criterion 'no ... hand-off after bPythonAlive is false' therefore fails in exactly the case it tests, if a hand-off counts as happening when RestoreThread returns.

**Evidence.** T-LIFE-008: 'Shutdown while the watchdog waits for the GIL in its hand-off (`PyEval_RestoreThread`) ... | No deadlock; the wait for `bInPython` ends in < 100 ms; no post or hand-off after `bPythonAlive` is false'. §3.8 step 2: 'one waiting in `PyEval_RestoreThread` gets the GIL, sees `bPythonAlive == false`, releases it and clears `bInPython`'.

**Refuter.** T-LIFE-008's first scenario puts the watchdog already inside PyEval_RestoreThread when shutdown begins. By §3.8 step 2 and §5.4.1 'Shutdown safety', that hand-off gets the GIL only after bPythonAlive has been cleared and the game thread has released the GIL. So the literal criterion 'no ... hand-off after `bPythonAlive` is false' contradicts the designed behaviour in the very case it tests. The property the design actually guarantees, with seq_cst flags, is that no hand-off passes its step-1 re-check after the flag is cleared. The verifier's rewording states exactly that.

**Fix applied.** As proposed. TDD §3.9 T-LIFE-008 pass criterion: replace 'no post or hand-off after `bPythonAlive` is false' with 'no post, and no hand-off whose step-1 re-check (§5.4.1) ran after `bPythonAlive` became false; a hand-off already past that re-check completes before the wait for `bInPython` ends'.

**Patch.** T-LIFE-008 pass criterion reworded around the step-1 re-check

#### W-runtime-10

**Minor, wrong engine fact.** Raised by the refuter.

**Where:** TDD §8.6.2 'Call' bullet; TDD §8.11 T-DBG-052

**Problem.** The engine's CPython uses frozen stdlib modules, so `os.path.realpath.__code__.co_filename` is '<frozen ntpath>'. When the in-process `listen` first imports pydevd, pydevd sees this and writes four 'Debugger warning: It seems that frozen modules are being used, which may make the debugger miss breakpoints…' lines to `sys.stderr` through `pydev_log.critical`. Start runs from an idle FTSTicker callback, outside any run, where `sys.stderr` is the stock `_Logger(log_error)`. Every 'Debug in VS Code' therefore logs four LogPython errors, and pydev_log keeps that stock stream for every later pydevd message. No section mentions this or sets the documented switch.

**Evidence.** debugpy 1.8.20 `_vendored/pydevd/pydevd_file_utils.py:588-603` (report = pydev_log.critical unless DISABLE_FILE_VALIDATION; fires when realpath's co_filename starts with '<frozen'); `_pydevd_bundle/pydevd_constants.py:261` `DISABLE_FILE_VALIDATION = is_true_in_env("PYDEVD_DISABLE_FILE_VALIDATION")`; `_pydev_bundle/pydev_log.py:48-49` (stream is sys.stderr). Probe (verify3/skeptic_rt/imp_probe.py, engine python -I -B): `import debugpy` wrote nothing, and `listen` wrote the four lines to the captured stderr. With `os.environ['PYDEVD_DISABLE_FILE_VALIDATION']='1'` set first (imp_probe2.py), nothing was written. `os.path.realpath.__code__.co_filename` printed '<frozen ntpath>'. UE does not set `use_frozen_modules` (PythonScriptPlugin.cpp:1161-1181). TDD §6.1 table: stock `sys.stderr` is `_Logger(log_error)` (`unreal_core.py:59`); §8.6.2: start 'from an idle FTSTicker callback ... never during a run'.

**Fix applied.** TDD.md §8.6.2, 'Call' bullet: after '`wait_for_client()` is never called (07a Q8).' append 'Just before the call, `DebugpyBridge` sets `os.environ["PYDEVD_DISABLE_FILE_VALIDATION"] = "1"`, which pydevd reads once, when `listen` first imports it (`debugpy/_vendored/pydevd/_pydevd_bundle/pydevd_constants.py:261`). The engine's interpreter uses frozen stdlib modules, so without it pydevd writes four "Debugger warning: It seems that frozen modules are being used" lines to `sys.stderr` (`pydevd_file_utils.py:588-603`), which outside a run is the stock stream and logs them as `LogPython` errors.' §8.11 T-DBG-052 pass criterion: prepend 'Starting the server logs no `LogPython` error; '.

**Patch.** TDD §8.6.2 Call bullet sets PYDEVD_DISABLE_FILE_VALIDATION before listen; T-DBG-052 pass criterion starts with 'Starting the server logs no LogPython error'

#### W-runtime-11

**Minor, wrong engine fact.** Raised by the refuter.

**Where:** TDD §8.8 Variables table, row 'unreal.Object' (Children cell); TDD §12.1.1 row `prop_export_text`; TDD §8.11 T-INSP-015

**Problem.** The V-runtime-13 patch makes soft, soft-class and lazy object properties leaves that hold 'their path text from `_sumi.prop_export_text`', claiming `ExportTextItem_Direct` 'writes the path without loading'. That holds for soft object and soft class properties. An FLazyObjectPtr holds a GUID, not a path. FLazyObjectProperty uses FObjectPropertyBase's export, which resolves the pointer with `Get()` (no load) and writes `None` when the target is not loaded. T-INSP-015's unloaded lazy object property would therefore show `None`, which reads as a null reference. The test's 'soft properties show their path text' cannot pass for that case.

**Evidence.** UE_5.8 `Engine/Source/Runtime/CoreUObject/Private/UObject/PropertyLazyObjectPtr.cpp:109-116` (`GetObjectPropertyValue` returns `GetPropertyValue(...).Get()`); `PropertyBaseObject.cpp:267-283` (`ExportText_Internal`: `if (!Temp) ValueStr += TEXT("None")`); by contrast `PropertySoftObjectPtr.cpp:115-135` exports `SoftObjectPtr.GetUniqueID()` (the path) when unloaded. `Public/UObject/LazyObjectPtr.h:57, 291` (`FUniqueObjectGuid::ToString`, `GetUniqueID`). T-INSP-015: 'a fixture UObject and a struct holding unloaded soft object, soft class and lazy object properties ... soft properties show their path text'.

**Fix applied.** TDD.md. (1) §8.8 'unreal.Object' Children cell: replace '(C++ `FProperty::ExportTextItem_Direct` on the soft pointer, which writes the path without loading)' with '(C++ `FProperty::ExportTextItem_Direct` on the soft pointer, which writes the path without loading; an unresolved `FLazyObjectPtr` has no path and would export as `None` (`PropertyBaseObject.cpp:267-283`), so for a lazy object property whose target is not loaded `prop_export_text` writes `Lazy <guid>` from `GetUniqueID().ToString()`)'. (2) §12.1.1 row `prop_export_text(value: object, name: str) -> str`: after 'cut at 200 characters; never loads' append '; an unresolved lazy object property gives `Lazy <guid>`'. (3) §8.11 T-INSP-015 pass criterion: replace 'soft properties show their path text' with 'the soft object and soft class properties show their path text, and the unloaded lazy object property shows `Lazy <guid>`'.

**Patch.** TDD §8.8 unreal.Object Children cell, §12.1.1 prop_export_text row and T-INSP-015: unresolved lazy object properties show 'Lazy <guid>'

### Round 2 — Persistence, policy and studio

I checked all 18 persist round-1 entries. To do it I read TDD §10, §11.1-§11.5, §11.11-§11.12, §12.3-§12.4, §14.7 and §15.2, SPEC PER, SEC and CFG and Appendix D.3, 08 §9, and mockups 06/07/08/10 with their PNGs. I verified the engine citations (UserName(false) in GenericPlatformProcess.h:454, WindowsPlatformProcess.cpp:1622-1648 and UnixPlatformProcess.cpp:218; OnPreExit; MoveFileW; UserSettingsDir) and probed the DirectoryWatcher source. The integrator check.py reports 0 problems and 814 unique test IDs.

V-persist-01 (major) landed at every listed location. The §11 feature table, §11.2 Lands, §12.3, §15.2 M2 sections and §8.6.4 now agree that the M2 resolver ships the machine-file watcher, the studio poll and the deferred swap, with Sumi.Policy.Reload left in M4 (API-03, T-UEI-015). T-DBG-043, T-ARCH-015 and T-AUD-007 (all M2), SPEC DBG-07, SEC-02 (M2 part), SEC-09 (M2 part) and 08 RV-04 agree with it. Two gaps remain in that M2 contract:
- **W-02:** IDirectoryWatcher opens the folder with OPEN_EXISTING, so a machine folder created after startup is never watched, and M2 has no manual reload.
- **W-03:** §11.2.3 Startup (timeout, PolicyCache) is not scoped to M2, although SEC-09's M2 acceptance needs it, and the IsReady gate omits the debug start.

W-04: T-AUD-007's new invalid machine files are not marked admin-owned. Other findings:
- **W-01:** V-persist-08 was not carried into 08 §9 RV-07, which still says "fall back to their defaults" and wins under the stated precedence.
- **V-persist-09:** the new T-PER-045 criterion cannot pass, because each run writes run.start and run.end (W-08). The new Terms sentence forbids a secondary's legitimate writes outside `<root>` (W-09).
- **V-persist-18 (W-05, W-06, W-07):** the new cross-root scan ignores owner.json and ns.lock, so it can list and delete another account's or a live instance's journal (W-05). It runs on the writer before the reconcile, so an unreachable override stalls journaling (W-06). ResolveStorageRoot does not say that the machine-layer owner check applies, and the studio-override tests need a second start (W-07).
- **§12.4 index (W-11):** not updated for V-persist-09/11/13/18.
- **W-12:** SPEC PER-08 Lands is still M1, against V-persist-03's M4 split.

Two new defects neither round caught: the StudioLibrary mirror is shared by all editor processes with no lock (W-10), and mockup 07 shows param fields and file paths that differ from §11.5 (W-13). Patches found complete and correct: V-persist-04, 05, 06, 07, 10, 12, 15 (PNG re-rendered and matches the TDD), 16 and 17. V-persist-01 and V-persist-02 landed fully but have the gaps above, so they are not in patch_ok.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [W-persist-05](#w-persist-05) | major | contradiction | modified | The V-persist-18 cross-root scan ignores two guards that other patched text relies on. |
| [W-persist-01](#w-persist-01) | minor | contradiction | confirmed | The V-persist-08 patch settled the invalid-layer meaning of D in the TDD and the SPEC: the invalid layer's value is dropped and the other layers, else the defaults, apply. |
| [W-persist-02](#w-persist-02) | minor | patch wrong | modified | V-persist-01 makes the M2 reload path the only way the machine key can turn off mid-session, because Sumi.Policy.Reload stays in M4. |
| [W-persist-03](#w-persist-03) | minor | patch incomplete | confirmed | The M2 resolver's startup behaviour is undefined. |
| [W-persist-04](#w-persist-04) | minor | untestable | confirmed | The two new invalid machine files are not stated to be admin-owned. |
| [W-persist-06](#w-persist-06) | minor | patch wrong | modified | The cross-root scan is placed in step G, on the writer thread, before the reconcile. |
| [W-persist-07](#w-persist-07) | minor | patch incomplete | confirmed | (1) ResolveStorageRoot reads the machine layer synchronously at tab spawn, outside the async FSumiPolicy load, and the text does not say that the §11.2.1 owner check and the §11.2.3 validation apply. |
| [W-persist-08](#w-persist-08) | minor | patch wrong | confirmed | The criterion added by the V-persist-09 patch cannot pass. |
| [W-persist-09](#w-persist-09) | minor | contradiction | modified | (1) The patched Terms sentence makes the secondary's write set absolute ('outside its namespace writes only ...'). |
| [W-persist-10](#w-persist-10) | minor | other | modified | Neither round caught this. |
| [W-persist-11](#w-persist-11) | minor | patch incomplete | modified | Round 1 accepted that a missing field in the §12.4 index matters (V-persist-16: 'a writer built from the index produces entries the merge cannot deduplicate'). |
| [W-persist-12](#w-persist-12) | minor | lands | confirmed | V-persist-03 made the TDD say that the machine- and studio-layer `storage.root_override` lands in M4, and split T-PER-053 and T-PER-055 to match. |
| [W-persist-13](#w-persist-13) | minor | mockup | modified | Mockup 07 shows the item schema and library layout with names the TDD does not use. |
| [W-persist-14](#w-persist-14) | minor | patch incomplete | added by refuter | V-persist-18 made 'the network override is down at start' a designed case, but the root is still resolved on the game thread. |
| [W-persist-15](#w-persist-15) | minor | patch incomplete | added by refuter | The M2 resolver resolves `debug.allow_debug_server` only. |
| [W-persist-16](#w-persist-16) | minor | other | added by refuter | `Trust.json` is per user, across projects and engine versions, so every editor process of the user writes it: a second instance, a second project, another engine version. |

#### W-persist-05

**Major, contradiction.** Verdict: modified.

**Where:** TDD.md §10.1 Rules, the ResolveStorageRoot / cross-root scan bullet; TDD.md §10.1 Rules, storage.root_override bullet (owner.json); TDD.md §10.5 Orphans; TDD.md §10.12 T-PER-055

**Problem.** The V-persist-18 cross-root scan ignores two guards that other patched text relies on. (1) owner.json (V-persist-13): a root whose owner.json names another account is rejected for this user, yet the scan still reads journal/* under 'the override' candidate. User B, whose {User} name collides with A's on a shared NAS root, then sees A's unsaved code in Review, and after B resolves it A's namespace is deleted. That breaks SEC-NF-04 and loses A's crash recovery. (2) Liveness: the scan's only criterion is a namespace that lacks CLEAN, and a running instance's main lacks CLEAN. A second editor whose root differs (override down at one start, up at the next) lists a live instance's journal as an orphan and deletes it after resolution. The same-root rule in §10.5 scans only namespaces whose ns.lock is free.

**Evidence.** §10.1: "a root whose `owner.json` names another account is rejected with a warning, and the next candidate of the fallback chain is used". The same section: "At primary start the recovery scan (§10.3 step G) also reads `journal/*` of this `<ProjectHash>` under every other candidate root (the override, the platform default, the temp fallback and the recorded `root`), and Review lists each namespace there that lacks `CLEAN` as an orphan (§10.5); after it is resolved, that namespace is deleted." §10.5: "At primary start, every namespace with a free `ns.lock` is scanned."

**Refuter.** The cross-root sentence (TDD.md:6314) uses only 'lacks CLEAN' as its criterion. It names the override as a candidate without applying the owner.json rule of the bullet above (TDD.md:6312). The same-root rule in §10.5 (TDD.md:6551) scans only namespaces whose ns.lock is free. So (1) a user whose override root name collides with another account's root, for example the same {User} in two domains with the same project path and hash, can read, restore and then delete that account's unclean journal. That breaks SEC-NF-04 and costs the other account its crash recovery. (2) Two concurrent instances with different roots, for example an override that was down when the first started and up when the second started, or a changed user setting, are both primaries. The second lists the first's live `main` (no CLEAN yet) as an orphan and deletes it after resolution. The proposed fix is incomplete on (2). Checking ns.lock still lets two primaries in different roots both resolve and delete the same dead orphan in the root the other one owns. A candidate root whose instance.lock is live belongs to its own primary and should be skipped. The text below is anchored so that it composes with the W-persist-06 and W-persist-07 edits to the same bullet.

**Fix applied.** TDD §10.1 Rules, the `ResolveStorageRoot` bullet (TDD.md:6314): replace "under every other candidate root (the override, the platform default, the temp fallback and the recorded `root`), and Review lists each namespace there that lacks `CLEAN` as an orphan (§10.5)" with "under every other candidate root (the override, the platform default, the temp fallback and the recorded `root`) that passes the §10.1 checks for this account (outside the engine and project trees, with no `owner.json` or one that names this account) and whose `instance.lock` is not held by a live instance (§10.5 liveness rules; that root's own primary owns its namespaces). A root whose `owner.json` names another account is never read, listed or deleted from. Review lists each namespace there whose `ns.lock` is free and that lacks `CLEAN` as an orphan (§10.5)". Leave the following "; after it is resolved, that namespace is deleted." in place. TDD §10.12 T-PER-055 (TDD.md:6770), scenario cell: append "; a candidate root whose `owner.json` names another SID and holds an unclean namespace; a candidate root whose `instance.lock` and `main` are held by a running instance of the same project". Result cell: append "; nothing from those two roots is listed in Review, read into history or deleted, and the running instance's journal is intact".

**Patch.** TDD §10.1 cross-root scan skips roots failing the account checks or with a live instance.lock, never touches another account's root, lists only namespaces with a free ns.lock; T-PER-055 two new roots

#### W-persist-01

**Minor, contradiction.** Verdict: confirmed.

**Where:** research/08-decision-log.md §9, RV-07 row (line 1732); TDD.md §11.2.2 Invalid-column legend; SPEC.md §7.18 SEC-09; SPEC.md Appendix D.3 row V-persist-08

**Problem.** The V-persist-08 patch settled the invalid-layer meaning of D in the TDD and the SPEC: the invalid layer's value is dropped and the other layers, else the defaults, apply. The authoritative ruling in 08 §9 was not touched and still gives the other reading. Under the stated precedence (08 §9 wins), a builder must follow RV-07's literal text. That is the ambiguity V-persist-08 was raised to remove, and T-POL-001's new cases (studio 90 with project 0 gives 90; a studio lock on live_completion under an invalid project file) contradict it.

**Evidence.** 08 §9 RV-07: "Editing aids (completion, language services, ruff, sumi-check, the console executor) fall back to their defaults". TDD §11.2.2: "**D** = that layer's value is dropped; the key is resolved from the other layers, and the built-in default applies only when no valid layer sets it (RV-08)". SPEC SEC-09: "are not closed: the invalid layer's values for them are ignored and the other layers, else the defaults, apply". README: "Where documents disagree, 08 §9 wins".

**Refuter.** 08 §9 RV-07 (research/08-decision-log.md:1732) still ends 'Editing aids (completion, language services, ruff, sumi-check, the console executor) fall back to their defaults', the wording V-persist-08 removed from SPEC SEC-09 (SPEC.md:492) and TDD §11.2.2 (TDD.md:7008). README and 08 §9 put §9 first, so its literal text governs. That literal text contradicts the two cases added to T-POL-001 (TDD.md:7718: studio 90 with project 0 gives 90; an invalid project file under a studio live_completion false leaves completion off). In-place edits to §9 rulings already happened in round 1 (RV-01, and RV-04's RV-05 to RV-06 fix), and 08 D12 marks an in-place change with 'added after the v1.0 review', so an in-place clarification follows precedent.

**Fix applied.** research/08-decision-log.md §9, RV-07 row (line 1732), last sentence: replace "Editing aids (completion, language services, ruff, sumi-check, the console executor) fall back to their defaults" with "Editing aids (completion, language services, ruff, sumi-check, the console executor) are not closed: the invalid layer's values for them are ignored, and the other layers, else the built-in defaults, apply (RV-08; clarified after the v1.1 check, V-persist-08)". Make no other change.

**Patch.** 08 §9 RV-07 last sentence clarified in place: editing aids are not closed; the invalid layer's values are ignored and other layers, else defaults, apply (RV-08; clarified after the v1.1 check, V-persist-08)

#### W-persist-02

**Minor, patch wrong.** Verdict: modified.

**Where:** TDD.md §11.2 Lands paragraph; TDD.md §11.2.3 Watching bullet; TDD.md §8.11 T-DBG-043

**Problem.** V-persist-01 makes the M2 reload path the only way the machine key can turn off mid-session, because Sumi.Policy.Reload stays in M4. That path is an IDirectoryWatcher on the machine folder. The engine's watcher opens the directory with OPEN_EXISTING, so registration fails when %ProgramData%\Sumi (or /etc/sumi) does not exist at startup. The case is common: it is the first deployment of a machine policy. A Policy.json that IT then pushes with debug.allow_debug_server: false is never seen. An open listener stays open until restart, and M2 has no manual reload. T-DBG-043 does not say whether the folder exists before 'turning it false', so it can pass or fail depending on how it is written.

**Evidence.** TDD §11.2.3: "Machine and project files are watched through `IDirectoryWatcher`, which calls back on the GT (07e Q4)." TDD §11.2 Lands: "the machine-file `IDirectoryWatcher`, the 5-minute studio poll and the deferred snapshot swap; `Sumi.Policy.Reload` follows in M4". Engine: Engine/Source/Developer/DirectoryWatcher/Private/Windows/DirectoryWatchRequestWindows.cpp:78-93 (CreateFile with OPEN_EXISTING; on failure it logs "CreateFile failed" and returns false), DirectoryWatcherWindows.cpp:63-72 (registration returns false when Init fails).

**Refuter.** The engine claim holds in the local install. DirectoryWatchRequestWindows.cpp:78-93 opens the directory with CreateFile(..., OPEN_EXISTING, ...) and returns false after logging 'CreateFile failed'. DirectoryWatcherWindows.cpp:63-72 then deletes the request, and RegisterDirectoryChangedCallback_Handle returns false. Linux inotify_add_watch fails the same way (DirectoryWatchRequestLinux.cpp:266-282). TDD §11.2.3 Watching (TDD.md:7080) gives the machine file only the watcher, and only the studio file is polled. In M2 Sumi.Policy.Reload does not exist (TDD.md:6974), so a machine policy deployed after editor start, into a folder that did not exist, is never read until restart, and an open listener stays open. The proposed fix is incomplete. Polling only for a missing folder misses a folder that IT deletes and recreates, where the watch handle points at the deleted directory. A poll that compares file identity covers both cases. Also, the T-DBG-043 phrase 'turning it false while the server listens' is in the result column, not the scenario.

**Fix applied.** TDD §11.2.3, Watching bullet (TDD.md:7080): append " From M2, the 5-minute poll also stats the machine file. When the file was absent at the last load and now exists, or when its size, mtime or file ID has changed, the machine layer is reloaded through the normal path (owner check, validation, swap). If watcher registration had failed, it is attempted again. Registration fails while the machine folder does not exist, which is the usual state before IT first deploys a machine policy, because `IDirectoryWatcher` opens the folder with `OPEN_EXISTING` (`Engine/Source/Developer/DirectoryWatcher/Private/Windows/DirectoryWatchRequestWindows.cpp:78-93`, `DirectoryWatcherWindows.cpp:63-72`)." TDD §11.2 Lands (TDD.md:6974): replace "(the machine-file `IDirectoryWatcher`, the 5-minute studio poll and the deferred snapshot swap;" with "(the machine-file `IDirectoryWatcher`, the 5-minute poll of the studio file and the machine file, and the deferred snapshot swap;". TDD §8.11 T-DBG-043 (TDD.md:5857), scenario cell: append "; the key is turned false while the server listens (a) by editing an existing admin-owned machine file and (b) by creating an admin-owned `%ProgramData%\Sumi\Policy.json` in a folder that did not exist at editor start". Result cell: replace "turning it false while the server listens closes the listener within one idle tick (a new connect is refused)" with "turning it false while the server listens closes the listener within one idle tick of the policy swap (a new connect is refused); the swap follows the watcher event in case (a) and the next 5-minute poll in case (b)".

**Patch.** TDD §11.2.3 Watching: from M2 the 5-minute poll also stats the machine file and retries watcher registration (OPEN_EXISTING, DirectoryWatchRequestWindows.cpp:78-93); §11.2 Lands names the poll of both files; T-DBG-043 cases (a) and (b)

#### W-persist-03

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §11.2 Lands paragraph; TDD.md §11.2.3 Startup and Gate bullets; SPEC.md §7.18 SEC-09 (Lands M2 part, acceptance); TDD.md §11.12 T-AUD-007

**Problem.** The M2 resolver's startup behaviour is undefined. §11.2 Lands lists the owner check, the invalid-layer rules and the reload path for M2, but not the §11.2.3 Startup rules for the studio layer (the 5 s timeout, PolicyCache/studio.json, a missing configured file counted as unreachable). SPEC SEC-09 lands in M2 for the machine and studio layers, and its acceptance includes the cache. The Gate bullet also omits the one M2 consumer: the Debug in VS Code start does not wait on IsReady(), so while the studio layer is still loading, the start can take the default `true`. That is the fail-open the M2 resolver exists to prevent. The Gate also lists runs, which would make every M2 run wait up to 5 s on a studio share for a resolver that resolves no run key.

**Evidence.** TDD §11.2.3: "**Gate.** Runs, LSP spawns, telemetry sends and the bridge wait on `IsReady()`. Editing never waits." and "A network studio path gets a 5 s timeout ... the last good copy in `%LOCALAPPDATA%\Sumi\PolicyCache\studio.json` is used". SPEC SEC-09 Lands: "M2 (machine and studio layers, closing `debug.allow_debug_server`; RV-04); M4 (rest)". Its acceptance: "with the studio share offline, the cached copy applies". Only T-POL-006 (M4) tests the cache.

**Refuter.** TDD §11.2 Lands (TDD.md:6974) lists the owner check, the invalid-layer rules and the reload path for M2. It does not list the §11.2.3 Startup rules (TDD.md:7075-7078), although the M2 resolver reads the studio layer, SPEC SEC-09's M2 part covers that layer, and its acceptance says 'with the studio share offline, the cached copy applies' (SPEC.md:492). Only T-POL-006 (M4) tests the cache. The Gate (TDD.md:7079) names runs, LSP spawns, telemetry sends and 'the bridge', which §11.2.3 elsewhere calls the 1.1 bridge listener (TDD.md:7084). It does not name the debug server start. Nothing defines what Snapshot() returns before IsReady() (no other text defines pre-ready behaviour), so the command can show and start with the default true while a network studio path is still loading. That is the fail-open RV-04 (3) exists to prevent. Gating M2 runs on a resolver that resolves no run key is pointless. The proposed fix is right. Below it is tightened so the bridge is marked 1.1 and the T-AUD-007 result is explicit.

**Fix applied.** TDD §11.2 Lands (TDD.md:6974): after "so the key can turn off mid-session (RV-04 (2))." insert " It also ships the §11.2.3 Startup rules for the studio layer (the 5 s timeout, `PolicyCache/studio.json`, and a missing configured file treated as unreachable) and the M2 part of the Gate." TDD §11.2.3 (TDD.md:7079): replace "- **Gate.** Runs, LSP spawns, telemetry sends and the bridge wait on `IsReady()`. Editing never waits." with "- **Gate.** From M2, the Debug in VS Code command and `DebugpyBridge.start()` wait on `IsReady()`: until then the command is shown disabled with the reason \"Waiting for policy\" (**proposed**). From M4, runs, LSP spawns and telemetry sends wait on it too, and from 1.1 the bridge. Editing never waits." TDD §11.12 T-AUD-007 (TDD.md:7745), scenario cell: append ", an unreachable studio path with a cached studio `debug.allow_debug_server: false`, and the same path with no cache". Result cell: append "; with the unreachable studio path the command is disabled with \"Waiting for policy\" until the 5 s timeout, then hidden in both cases (the cached `false`, or the closed value when there is no cache)".

**Patch.** TDD §11.2 Lands ships the studio-layer Startup rules and the M2 Gate; §11.2.3 Gate rewritten (M2: Debug in VS Code and DebugpyBridge.start() wait, 'Waiting for policy'; M4 runs, LSP, telemetry; 1.1 bridge); T-AUD-007 unreachable-studio cases

#### W-persist-04

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD.md §11.12 T-AUD-007

**Problem.** The two new invalid machine files are not stated to be admin-owned. A machine file that fails the §11.2.1 owner check is ignored, and the command stays. A test written with files created by the test user therefore sees the command stay, which contradicts the stated result ('either invalid machine file hides the command'). Only the 'false' case says admin-owned.

**Evidence.** T-AUD-007 scenario: "with an admin-owned machine `debug.allow_debug_server: false`, with a studio-layer `false`, and with a squatted machine file, a machine file with a JSON syntax error, a machine file with `debug.allow_debugserver: false`". §11.2.3: "**Untrusted machine layer:** one that fails the §11.2.1 owner check is ignored as if absent, never failed closed".

**Refuter.** T-AUD-007 (TDD.md:7745) calls only the `false` machine file admin-owned. §11.2.1 (TDD.md:6984-6989) and §11.2.3 (TDD.md:7086) say a machine file that fails the owner check is ignored as if absent, before its content matters. A test harness that writes the two invalid files as the test user, which the inherited Users add-file right on %ProgramData%\Sumi allows, gets an ignored layer and a visible command. The test then fails against its stated result ('either invalid machine file hides the command'). The fix is a wording change and removes the ambiguity.

**Fix applied.** TDD §11.12 T-AUD-007 (TDD.md:7745), scenario cell: replace "a machine file with a JSON syntax error, a machine file with `debug.allow_debugserver: false`" with "an admin-owned machine file with a JSON syntax error, an admin-owned machine file with `debug.allow_debugserver: false`".

**Patch.** T-AUD-007: the two invalid machine files are admin-owned

#### W-persist-06

**Minor, patch wrong.** Verdict: modified.

**Where:** TDD.md §10.1 Rules, cross-root scan bullet ('§10.3 step G'); TDD.md §10.3 Restore order flowchart, steps G and H; TDD.md §10.2.3 Rotation ('the writer holds any commit requested before the reconcile until that checkpoint is written')

**Problem.** The cross-root scan is placed in step G, on the writer thread, before the reconcile. The new session's first checkpoint, and every commit, wait for the reconcile. The scan's motivating case is an override that is down at this start, and in that case one candidate it must read is that unreachable UNC root. The writer then blocks for the SMB timeout. Meanwhile nothing is durable, and pre-run snapshots time out after 2 s, so runs end 'not executed'. The scan meant to protect crash recovery switches journaling off in exactly the outage it handles. (The GT-side probe of a remote override in ResolveStorageRoot at tab spawn has the same exposure, §10.2.1 'root resolution, locks | Game thread'.)

**Evidence.** §10.1: "At primary start the recovery scan (§10.3 step G) also reads `journal/*` ... under every other candidate root (the override, ...)". §10.3 flowchart: "G[Writer thread reads main and orphan namespaces] --> H[Game thread reconcile at most 2 ms, then the first commit opens a new generation ...]". §10.2.3: "the writer holds any commit requested before the reconcile until that checkpoint is written". §10.2.5 item 4: not durable within 2 s means the run ends `not executed`.

**Refuter.** TDD.md:6314 puts the cross-root scan in '§10.3 step G', on the writer thread (flowchart TDD.md:6484, reader §10.2.4 'runs on the writer thread'). Step H's first commit is the new generation's checkpoint, and 'the writer holds any commit requested before the reconcile until that checkpoint is written' (§10.2.3, TDD.md:6396). A pre-run snapshot not durable within 2 s ends the run `not executed` (§10.2.5 item 4, TDD.md:6432). Any candidate on a host that does not answer, which is the override, or the recorded `root` after a session that used the override, therefore stops all journaling for the SMB timeout. Orphans are not part of the reconcile, because n excludes them (§10.7.1, TDD.md:6609), so the scan can run after the first checkpoint. The proposed fix has two gaps. It leaves the post-resolution delete unassigned, so it could still land on the writer and block it on the same host. And its '5 s timeout' cannot cancel a blocked SMB call and conflicts with 'namespaces join Review when the task returns'. Nothing needs to wait on the task, so no timeout is needed. The GT-side probe at tab spawn is a separate defect and is reported as missed item W-persist-14.

**Fix applied.** TDD §10.1 Rules, the `ResolveStorageRoot` bullet (TDD.md:6314): (1) replace "At primary start the recovery scan (§10.3 step G) also reads" with "After the reconcile and the first checkpoint (§10.3 step H), a worker task, never the journal writer, also reads". (2) Replace "; after it is resolved, that namespace is deleted." with "; after it is resolved, a worker deletes that namespace. Nothing waits on this task, and it may take as long as the file system needs: what it finds joins Review when it returns, and the n = 0 banner (§10.7.1) then appears if needed." TDD §10.12 T-PER-055 (TDD.md:6770), scenario cell: append "; a start whose override host does not answer, after a session that used the override". Result cell: append "; at that start the first commit is durable within 1.75 s of the first edit, a pre-run snapshot succeeds, and the journal writer performs no file operation under the override root".

**Patch.** TDD §10.1 cross-root scan moved to a worker task after the reconcile and first checkpoint, with the delete on a worker and nothing waiting on it; T-PER-055 unreachable-override start

#### W-persist-07

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet; TDD.md §10.12 T-PER-053; TDD.md §10.12 T-PER-055

**Problem.** (1) ResolveStorageRoot reads the machine layer synchronously at tab spawn, outside the async FSumiPolicy load, and the text does not say that the §11.2.1 owner check and the §11.2.3 validation apply. A squatted %ProgramData%\Sumi\Policy.json (RV-06's attacker) could then send every account's journals and history to a share the squatter reads (`\\host\x\{User}` satisfies the {User} rule, and the victim writes owner.json itself). That breaks RV-06 and SEC-NF-04. The cached studio copy is also used without checking that its recorded source path is the currently configured studio path. (2) A studio override now applies only from the start after it is cached, but T-PER-053 and the M4 part of T-PER-055 expect a studio value to be used without saying that a second start is needed. Written as a single start, both fail.

**Evidence.** §10.1: "`ResolveStorageRoot` reads `storage.root_override` from the user settings, the machine layer and the cached studio copy (`PolicyCache/studio.json`) only, never waiting on the studio share; a value that first arrives from the live share applies from the next start." T-PER-053: "from M4, ... a studio value with `{User}` is used and passes the §10.1 checks". T-PER-055: "(from M4) the same with a studio-layer override". §12.4 Studio policy cache: "with its source path and SHA-256".

**Refuter.** TDD.md:6314 has ResolveStorageRoot read 'the machine layer' at tab spawn, outside the async FSumiPolicy load (§11.2.3 Startup launches a background task, TDD.md:7075). The sentence does not apply the §11.2.1 owner check, although §11.2.1 states it for the M2 resolver explicitly ('The check gates the M2 minimal resolver too', TDD.md:6989). A builder who reads the file directly lets a squatted %ProgramData%\Sumi\Policy.json (the RV-06 attacker) redirect every account's journals to a share the squatter can read: '\\host\x\{User}' satisfies the {User} rule, and the victim writes owner.json itself. The cache records its source path (§12.4, TDD.md:8023), but nothing says the path is compared with the configured one. The second point also holds. 'a value that first arrives from the live share applies from the next start' (TDD.md:6314) means T-PER-053's studio case (TDD.md:6768) and the M4 part of T-PER-055 (TDD.md:6770) fail if a tester writes them as a single start.

**Fix applied.** TDD §10.1 Rules, the `ResolveStorageRoot` bullet (TDD.md:6314): replace "from the user settings, the machine layer and the cached studio copy (`PolicyCache/studio.json`) only" with "from the user settings; from the machine layer only when it passes the §11.2.1 owner check and is a valid layer (§11.2.3), read synchronously because it is a local file; and from the cached studio copy (`PolicyCache/studio.json`) only when its recorded source path equals the configured studio path". TDD §10.12 T-PER-053 (TDD.md:6768), scenario cell: append "; each studio case is checked at the second start after the studio file is written, because the first start fills `PolicyCache/studio.json`; from M4, a squatted machine file (owned by the test user) that sets `storage.root_override`". Result cell: append "; the squatted machine value is ignored and the default root is used". T-PER-055 (TDD.md:6770), scenario cell: replace "(from M4) the same with a studio-layer override" with "(from M4) the same with a studio-layer override already in `PolicyCache/studio.json`".

**Patch.** TDD §10.1 ResolveStorageRoot applies the §11.2.1 owner check and validity to the machine layer and the source-path match to the cached studio copy; T-PER-053 second-start and squatted-file cases; T-PER-055 studio override already cached

#### W-persist-08

**Minor, patch wrong.** Verdict: confirmed.

**Where:** TDD.md §10.12 T-PER-045 (pass criterion)

**Problem.** The criterion added by the V-persist-09 patch cannot pass. Every run writes two audit records, `run.start` at Starting and `run.end`, so no run has exactly one audit record.

**Evidence.** T-PER-045: "every run of both instances has exactly one audit record in `<root>/audit/`". §11.4 Other events: "`run.start` (at `Starting`, after the trust and parse checks)"; the schema example and T-AUD-001 count `run.end` records.

**Refuter.** §11.4 (TDD.md:7206, 7229, 7243) writes `run.start` at Starting and a `run.end` for every run, including `lost` ones. T-AUD-001 (TDD.md:7739) counts `run.end` records. So every run has at least two audit lines, and 'exactly one audit record' in T-PER-045 (TDD.md:6760) cannot pass as written. The criterion is meant to catch duplicates and losses, which the replacement states.

**Fix applied.** TDD §10.12 T-PER-045 (TDD.md:6760), result cell: replace "every run of both instances has exactly one audit record in `<root>/audit/`" with "every run of both instances has exactly one `run.start` and one `run.end` record in `<root>/audit/`, the secondary's in its own `audit-YYYY-MM-DD.<sec-id>.jsonl`, and no record appears twice".

**Patch.** T-PER-045: one run.start and one run.end record per run, secondary file named, no duplicates

#### W-persist-09

**Minor, contradiction.** Verdict: modified.

**Where:** TDD.md §10 Terms, 'Primary' bullet; TDD.md §10.1 Rules, storage.root_override bullet ('projects.json and PolicyCache stay under the platform default base')

**Problem.** (1) The patched Terms sentence makes the secondary's write set absolute ('outside its namespace writes only ...'). It therefore forbids writes that every instance must make outside `<root>`: Trust this folder writes `%LOCALAPPDATA%\Sumi\Trust.json` (§11.3.3), FSumiPolicy writes `PolicyCache/studio.json` (§11.2.3), and the library mirrors into `StudioLibrary\` (§11.5.1). A builder following §10 would block them in a secondary. (2) The V-persist-14 text lists only projects.json and PolicyCache as staying under the default base, which leaves open whether Trust.json and the StudioLibrary mirror (also directly under `%LOCALAPPDATA%\Sumi`) move to the override base.

**Evidence.** §10 Terms: "A **secondary** journals in its own namespace and outside its namespace writes only `projects.json`, its own `RunLogs/<session start>/` folder and its own `audit/audit-YYYY-MM-DD.<sec-id>.jsonl` files (§10.5)." §10.1: "`projects.json` and `PolicyCache` stay under the platform default base." §12.4: Trusted folders "`%LOCALAPPDATA%\Sumi\Trust.json`"; Snippet item "`%LOCALAPPDATA%\Sumi\StudioLibrary\<sha8(path)>\`".

**Refuter.** The Terms bullet (TDD.md:6292) is not limited to `<root>`, because it already lists `projects.json`, which lives outside `<root>`. Read literally, it forbids a secondary's writes to Trust.json (§11.3.3, TDD.md:7180), PolicyCache/studio.json (§11.2.3, TDD.md:7076), the StudioLibrary mirror (§11.5.1, TDD.md:7269), the user library and project files. TDD.md:6312 names only projects.json and PolicyCache as staying under the default base when the override replaces `%LOCALAPPDATA%\Sumi`, so a reader can infer that Trust.json and StudioLibrary/ move with it. The proposed Terms text is incomplete in the other direction. Its parenthetical reads as an exhaustive list of what a secondary may write outside `<root>` and leaves out the user library tier, project files and the stub cache. Scoping the rule to `<root>` and pointing elsewhere to each file's own rules is correct.

**Fix applied.** TDD §10 Terms, Primary bullet (TDD.md:6292): replace "A **secondary** journals in its own namespace and outside its namespace writes only `projects.json`, its own `RunLogs/<session start>/` folder and its own `audit/audit-YYYY-MM-DD.<sec-id>.jsonl` files (§10.5)." with "A **secondary** journals in its own namespace. Under `<root>` but outside that namespace it writes only its own `RunLogs/<session start>/` folder and its own `audit/audit-YYYY-MM-DD.<sec-id>.jsonl` files (§10.5). Outside `<root>` it updates `projects.json` under its lock (§10.1); per-user files that every editor process shares, such as `Trust.json`, `PolicyCache/` and the `StudioLibrary` mirror, follow their own rules (§11.2.3, §11.3.3, §11.5.1)." TDD §10.1 Rules, `storage.root_override` bullet (TDD.md:6312): replace "`projects.json` and `PolicyCache` stay under the platform default base." with "Only `<root>` moves: `projects.json`, `PolicyCache/`, `Trust.json` and `StudioLibrary/` stay under the platform default base."

**Patch.** TDD §10 Terms Primary bullet scoped to <root> with shared per-user files pointed at their own rules; §10.1 'Only <root> moves' lists projects.json, PolicyCache/, Trust.json, StudioLibrary/

#### W-persist-10

**Minor, other.** Verdict: modified.

**Where:** TDD.md §11.5.1 Tiers table (Studio row) and Consistent mirrors bullet; TDD.md §11.12 T-SNP-017

**Problem.** Neither round caught this. The studio mirror folder is per user and per studio path. It is shared by every editor process of that user: two instances of one project, two projects, or two engine versions. Each process mirrors on its own 15-minute timer, creates generation folders, swaps, and deletes the new or old generation, with no lock and no owner rule. One process can delete the generation another has just swapped in or is scanning, so items vanish or fail verification, and the false 'Modified since publish' state that Consistent mirrors exists to prevent can appear. The §10.5 ownership table covers only files under `<root>`.

**Evidence.** §11.5.1: "Studio | `library.studio.path`, mirrored to `%LOCALAPPDATA%\Sumi\StudioLibrary\<sha8(path)>\`" and "A mirror run copies the share into a new generation folder beside the current one. ... on any failure it keeps the previous generation and deletes the new one." No lock or owner is named for this folder anywhere in §10.5 or §11.5.

**Refuter.** The folder `%LOCALAPPDATA%\Sumi\StudioLibrary\<sha8(path)>\` (TDD.md:7269) is keyed only by the studio path. Every editor process of the user therefore shares it: instances, projects and engine versions. §11.5.1 (TDD.md:7274-7275) has each process mirror on its own timer into generation folders and swap them, and deletes the new generation on failure. It names no lock and no deletion owner, and the §10.5 ownership table covers only `<root>`. One process can therefore delete a generation another is scanning or reading. §11.5.4 reads item bytes from the mirror at run time ('A worker reads the item bytes once from the mirror', TDD.md:7354), so a run can then fail, or show false verification failures. The proposed fix is incomplete. It refers to a `current.json` that no section defines. Keeping the previous generation does not protect a long-lived reader once two swaps have happened. And the Restore published & run re-sync of one item (TDD.md:7357) is another unlocked writer into the mirror.

**Fix applied.** TDD §11.5.1, Consistent mirrors bullet (TDD.md:7275): append " The mirror folder `StudioLibrary\<sha8(path)>\` is shared by every editor process of the user (instances, projects and engine versions). `current.json` in it names the current generation folder and is replaced atomically at each swap (the §10.2.3 small-file rule). A mirror run holds `mirror.lock` in that folder for its whole duration, opened and held like `instance.lock` (§10.5); a process that finds it held skips that run. The one-item re-sync of Restore published & run (§11.5.4) also takes `mirror.lock`, waiting for it up to 2 s on its worker (**proposed**). Only the lock holder deletes generation folders, and only those older than the previous generation. A reader resolves `current.json` when it scans and before it reads an item; if the generation named there is gone, it re-reads `current.json` and retries once." TDD §11.12 T-SNP-017 (TDD.md:7762), scenario cell: append "; two editor processes start a mirror of the same studio path at the same moment while a third runs an item from the current generation". Result cell: append "; one process copies, the other skips, and no process sees a missing or unverified item".

**Patch.** TDD §11.5.1 Consistent mirrors: current.json, mirror.lock, deletion owner, reader retry, re-sync takes the lock; T-SNP-017 concurrent-mirror case

#### W-persist-11

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §12.4 rows: projects.json, Audit log, Snippet parameter memory, Journal; TDD.md §10.5 Merge bullet (LibraryState); TDD.md §10.1 Rules (owner.json)

**Problem.** Round 1 accepted that a missing field in the §12.4 index matters (V-persist-16: 'a writer built from the index produces entries the merge cannot deduplicate'). Several patched formats were not carried into it. projects.json has no `root` (V-persist-18). The Audit log row does not give the secondary's `.<sec-id>.jsonl` file (V-persist-09). The LibraryState row does not give the secondary copy, and its schema has no per-entry time, although the merge keeps 'the newest per key'. The Journal row does not give the SNAP `pending` head that the reader now binds by (V-persist-11), and lists only part of the secondary namespace's contents. owner.json (V-persist-13) has no row, and its location is ambiguous now that the override is a base folder: the override base, or each `<EngineVer>/<ProjectHash>/` root?

**Evidence.** §12.4 projects.json: "ProjectHash → path, `.uproject` file identity (volume serial and file ID, or `st_dev` and `st_ino`), last seen". Audit log: "`<root>/audit/audit-YYYY-MM-DD.jsonl`, rolled at 16 MB". Snippet parameter memory: "`<root>/LibraryState.json` | `schema` | Last values per user per item, keyed `<tier>:<id>@<schema>`". Journal: "A secondary instance's namespace ... also holds its `pending-history/` and `repl-history.jsonl`"; "followed by a `SNAP` of the recovered text". §10.5: "LibraryState entries per key with the newest kept". §10.1: "an override root holds `owner.json`".

**Refuter.** Each gap exists in the current §12.4 rows. projects.json has no `root` (TDD.md:8017), although the §10.1 example has it (TDD.md:6320). The Audit log row has no secondary file (TDD.md:8013). The Snippet parameter memory row has no secondary copy and no per-entry time (TDD.md:8024), although §10.5 merges 'per key with the newest kept' (TDD.md:6564, 6568). The Journal row still binds the recovered SNAP by position ('followed by a SNAP of the recovered text') and lists only part of the secondary namespace. owner.json has no row, and 'an override root holds owner.json' (TDD.md:6312) does not say whether that means the override base or each `<EngineVer>/<ProjectHash>/`. The proposed fix leaves out the owner section: §11.5.3 (TDD.md:7340) defines LibraryState's content and also needs `t`, and the §10.5 table row repeats 'newest kept'.

**Fix applied.** TDD §12.4: (a) `projects.json` row, schema cell: replace "ProjectHash → path, `.uproject` file identity (volume serial and file ID, or `st_dev` and `st_ino`), last seen." with "ProjectHash → path, `.uproject` file identity (volume serial and file ID, or `st_dev` and `st_ino`), `root` (the last used storage root, scanned for orphans, §10.1), last seen." (b) Audit log row, location cell: replace "`<root>/audit/audit-YYYY-MM-DD.jsonl`, rolled at 16 MB" with "`<root>/audit/audit-YYYY-MM-DD.jsonl`, rolled at 16 MB; a secondary instance writes `audit-YYYY-MM-DD.<sec-id>.jsonl` instead, and only the primary sweeps (§10.5)". (c) Snippet parameter memory row: location cell "`<root>/LibraryState.json`" becomes "`<root>/LibraryState.json`; a secondary keeps its own in `<sec>/state/LibraryState.json` until the merge (§10.5)"; schema cell "Last values per user per item, keyed `<tier>:<id>@<schema>` (U31, §11.5.3)" becomes "Last values per user per item, keyed `<tier>:<id>@<schema>`, each entry with `t` (UTC ms of its last use) for the per-key merge (U31, §11.5.3, §10.5)". (d) Journal row: replace "also holds its `pending-history/` and `repl-history.jsonl` (§10.5)" with "also holds its `pending-history/`, `repl-history.jsonl`, `state/`, `telemetry.jsonl` and `Logs/LSP/` (§10.5)", and replace "followed by a `SNAP` of the recovered text)" with "followed by a `SNAP` of the recovered text whose `pending` head names it; a SNAP with a non-null `pending` is never applied to a live buffer)". (e) Add a row after the `projects.json` row: "| Storage-root owner | §10 | `owner.json` at the top of a `storage.root_override` base folder | `schema` | Account SID (`ConvertSidToStringSidW`) or POSIX uid, written at first use; a root naming another account is rejected (§10.1) |". TDD §11.5.3 (TDD.md:7340): replace "keyed `<tier>:<id>@<schema>`. Selection parameters are never remembered." with "keyed `<tier>:<id>@<schema>`; each entry records `t`, the UTC ms of its last use. Selection parameters are never remembered." TDD §10.5: in the ownership table row `<root>/LibraryState.json` replace "merged per key, newest kept" with "merged per key, keeping the larger `t`"; in the Merge bullet replace "LibraryState entries per key with the newest kept" with "LibraryState entries per key, keeping the larger `t`". TDD §10.1 (TDD.md:6312): replace "so an override root holds `owner.json`" with "so the override base folder holds `owner.json`".

**Patch.** TDD §12.4 rows projects.json (root), Audit log (secondary file), Snippet parameter memory (secondary copy, t), Journal (secondary contents, SNAP pending head), new Storage-root owner row; §11.5.3 t field; §10.5 table and Merge bullet 'keeping the larger t'; §10.1 'override base folder holds owner.json'

#### W-persist-12

**Minor, lands.** Verdict: confirmed.

**Where:** SPEC.md §7.14 PER-08 (Lands column); TDD.md §10.1 Rules, storage.root_override bullet; TDD.md §10 feature table; TDD.md §14.7 PER-08 row

**Problem.** V-persist-03 made the TDD say that the machine- and studio-layer `storage.root_override` lands in M4, and split T-PER-053 and T-PER-055 to match. SPEC PER-08 still lands wholly in M1, while its text requires the policy-layer rule ('a policy value must contain the {User} token'), which no M1 build can have. The §10 feature table lists no M4 part for storage either. This is the SPEC-TDD Lands mismatch class that QA-F06 fixed.

**Evidence.** SPEC PER-08: "... the project layer cannot set it, and a policy value must contain the `{User}` token (RV-08). ... | M1 |". TDD §10.1: "`storage.root_override` (proposed, §11.2.2) (the user setting from M1; the machine and studio layers from M4)". T-PER-053 and T-PER-055 Lands: "M1 (M4: policy layers)". §10 feature table M4 row: "Studio retention controls | M4 (D17)".

**Refuter.** SPEC PER-08 (SPEC.md:431) requires 'a policy value must contain the {User} token' and lands wholly in M1. The TDD (TDD.md:6312) lands the machine and studio layers of `storage.root_override` in M4, and T-PER-053 and T-PER-055 are 'M1 (M4: policy layers)'. No M1 build has a policy layer, because the M2 resolver is debug-only and the full one is M4. The §10 feature table (TDD.md:6282) has no M4 storage part, and §14.7 (TDD.md:8509) shows PER-08 | M1. This is the SPEC-TDD Lands mismatch class that QA-F06 addressed, and other SPEC rows already use the multi-milestone form (SEC-02, PER-04).

**Fix applied.** SPEC.md §7.14 PER-08, Lands cell: replace "M1" with "M1; M4 (`storage.root_override` from the machine and studio policy layers)". TDD §10 feature table (TDD.md:6282): replace "| Studio retention controls | M4 (D17) |" with "| Studio retention controls; `storage.root_override` from the machine and studio layers (§10.1) | M4 (D17) |". Regenerate §14.7 with the integrator gen.py so the PER-08 row reads "M1; M4 (`storage.root_override` from the machine and studio policy layers)". SPEC Appendix D.3: add after the V-persist-14 row "| — | V-persist-03 | The machine- and studio-layer `storage.root_override` lands in M4 | PER-08 |".

**Patch.** SPEC PER-08 Lands 'M1; M4 (storage.root_override from the machine and studio policy layers)'; TDD §10 feature table M4 row; §14.7 regenerated; D.3 row added

#### W-persist-13

**Minor, mockup.** Verdict: modified.

**Where:** docs/mockups/07-snippets-and-shelves.html lines 339-340 (#pBlock), 312, 412, 464, 214 and 488 (callout 1); docs/images/07-snippets-and-shelves.png (top-right item preview, panel A footer, panel C, callout 1)

**Problem.** Mockup 07 shows the item schema and library layout with names the TDD does not use. The params block uses type "asset path" and the field "label", where §11.5.2 and §11.5.3 use `asset_path` and `title`. The file paths omit `items/<category>/` and the `.sumi.json` and `.shelf.json` suffixes, and use snake_case ids where the TDD uses kebab-case ids. Someone copying the JSON from the image writes items that the §11.5.2 parser rejects or migrates wrongly.

**Evidence.** Mockup 07:339-340: `"folder": {"type": "asset path", "default": "/Game/Props"}`, `"prefix": {"type": "string", "default": "SM_", "label": "Prefix"}`. Line 312: "Sumi/Library/assets/prefix_folder_assets.json"; 412: "fix_normal_maps.json"; 464: "validation/validate_naming.json"; 214/488: "Sumi/Library/shelves/citysample.json". TDD §11.5.2: `"prefix":  { "type": "string", "title": "Prefix", ...}`. §11.5.3 type table: `asset_path`. §11.5.1: "`items/<category>/<id>.sumi.json` ... and `shelves/<id>.shelf.json`".

**Refuter.** The mockup and the rendered image both show the mismatches (07-snippets-and-shelves.html:214, 312, 339-340, 396, 412, 464, 488, 494). The params block uses type "asset path" and the field "label", where the TDD uses `asset_path` (TDD.md:7329) and `title` (TDD.md:7285-7286). The paths omit `items/<category>/` and the `.sumi.json` and `.shelf.json` suffixes (TDD.md:7271). The fix is wrong for `folder`. `asset_path` is an asset picker whose value is an object path such as '/Game/Path/Asset.Asset' (TDD.md:7329), while the item passes a folder ('/Game/Props', fed to `list_assets(folder)` on mockup line 323). Relabelled as asset_path it would still be an invalid default. With no folder type in §11.5.3, the correct type is `string`. The verifier also missed line 406 (`Sumi/Library/assets/` in the Publish-to Project row).

**Fix applied.** docs/mockups/07-snippets-and-shelves.html: line 339 becomes `  <span class="k2">"folder"</span>: {"type": <span class="s2">"string"</span>, "title": <span class="s2">"Folder"</span>, "default": <span class="s2">"/Game/Props"</span>},`. Line 340 becomes `  <span class="k2">"prefix"</span>: {"type": <span class="s2">"string"</span>, "title": <span class="s2">"Prefix"</span>, "default": <span class="s2">"SM_"</span>}`. Line 312: `Sumi/Library/assets/prefix_folder_assets.json` becomes `Sumi/Library/items/assets/prefix-folder-assets.sumi.json`. Line 406: `Sumi/Library/assets/` becomes `Sumi/Library/items/assets/`. Line 412: `fix_normal_maps.json` becomes `items/assets/fix-normal-maps.sumi.json`. Line 464: `validation/validate_naming.json` becomes `items/validation/validate-naming.sumi.json`. Lines 214 and 488: `Sumi/Library/shelves/citysample.json` becomes `Sumi/Library/shelves/citysample.shelf.json`. Line 396: `Label in form` becomes `Title in form`. Line 494 (callout 7): "enum, asset path, and the selection types" becomes "enum, <code>asset_path</code>, and the selection types". Re-render with `powershell -ExecutionPolicy Bypass -File docs/mockups/render.ps1 -Only 07-snippets-and-shelves`, then check that the longer path on line 312 still fits the preview header in images/07-snippets-and-shelves.png.

**Patch.** Mockup 07: params block uses type string with title fields, item paths under items/<category>/ with .sumi.json, shelf .shelf.json, 'Title in form', callout 7 asset_path; line 406 path too; canvas 1600x1900 to 1600x1920

#### W-persist-14

**Minor, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §10.1 Rules, fallback-order bullet (line 6313); TDD.md §10.2.1 table ('root resolution, locks | Game thread', line 6339); TDD.md §10.3 Budgets (line 6496); TDD.md §10.12 T-PER-055

**Problem.** V-persist-18 made 'the network override is down at start' a designed case, but the root is still resolved on the game thread. The tab spawn opens `instance.lock` in the override root on the GT (§10.3: 'the spawn path does two small file operations on the game thread, the instance lock first'). When the override host does not answer, that CreateFileW on a UNC or disconnected mapped path blocks for the SMB connect timeout, often 20 s or more, before the fallback chain is reached. Every editor start then freezes while a NAS is down, and PERF-01 (≤ 30 ms added startup) fails. The verifier noted this in passing under W-persist-06 but gave no fix.

**Evidence.** §10.2.1: "Snapshots, request queue, UE delegates, root resolution, locks | `FSumiJournal`, SumiEditor | Game thread". §10.3 Budgets: "the spawn path does two small file operations on the game thread, the instance lock first and then the restore guard". §10.1: "If the root is not writable, the fallback order is: override, platform default, ..." with no bound on how long the override check may take.

**Fix applied.** TDD §10.1 Rules, fallback-order bullet (line 6313): append " The override is never probed by a blocking call on the game thread: a worker opens `instance.lock` there (creating the folders if needed), and the tab spawn waits for it at most 2 s (**proposed**). An override that has not answered by then counts as not writable for this session, with the persistent warning \"Storage folder <path> did not answer; this session uses <fallback>\". A probe that completes later closes what it opened. The platform default and temp roots are local and are opened on the game thread as before." TDD §10.3 Budgets (line 6496): after "the instance lock first and then the restore guard" insert " (with an override, the lock comes from the bounded §10.1 probe, so an override that does not answer costs at most 2 s, once)". TDD §10.12 T-PER-055, result cell: append "; at the start whose override host does not answer, the tab spawn returns within 2 s plus the PERF-01 budget, and the session journals to the platform default with the warning". If the W-persist-06 fix has not added "a start whose override host does not answer, after a session that used the override" to the scenario cell, append it as well.

**Patch.** TDD §10.1 fallback bullet: override instance.lock opened by a worker probe bounded at 2 s with a warning; §10.3 Budgets note; T-PER-055 result; also qualified the §10.2.1 'root resolution, locks' row to point at the worker probe

#### W-persist-15

**Minor, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §11.2 Lands paragraph (line 6974); TDD.md §8.6.4 'Lands with the listener in M2' bullet (line 5697); TDD.md §11.12 T-AUD-007

**Problem.** The M2 resolver resolves `debug.allow_debug_server` only. 'A valid key it does not yet resolve ... is accepted and ignored until M4', and 'Every other key ... land[s] in M4'. Read that way, the machine-layer `studio_policy_path` is ignored in M2, and the studio layer is found only through the `SUMI_POLICY_PATH` environment variable. A studio that points workstations at its share through the machine key, which §11.2.1 offers because 'an environment variable cannot be enforced', then has its studio `debug.allow_debug_server: false` silently unread in M2-M3 builds, and the listener stays allowed. That is the fail-open case RV-04 (3) brought the resolver forward to close. T-POL-004, which covers discovery through `studio_policy_path`, lands in M4.

**Evidence.** §11.2 Lands: "resolving `debug.allow_debug_server` only (RV-04) ... while a valid key it does not yet resolve (for example `trust.roots`) is accepted and ignored until M4 ... Every other key, the project layer and the inspector land in M4." §11.2.1 Studio row: "A machine-layer `studio_policy_path` overrides the variable, with a log line (**proposed**: an environment variable cannot be enforced)". §11.2.2: "| `studio_policy_path` | path | none | machine layer only | Variable used | Startup |". T-POL-004 Lands M4.

**Fix applied.** TDD §11.2 Lands (line 6974): replace "resolving `debug.allow_debug_server` only (RV-04)." with "resolving `debug.allow_debug_server` only, plus the machine layer's `studio_policy_path`, which it needs to find the studio layer (RV-04)." TDD §8.6.4 (line 5697): replace "for `debug.allow_debug_server` only, including reload" with "for `debug.allow_debug_server` only (the machine `studio_policy_path` is honoured to find the studio layer), including reload". TDD §11.12 T-AUD-007, scenario cell: append ", and a studio file reached only through an admin-owned machine `studio_policy_path` (with `SUMI_POLICY_PATH` pointing at a file that allows the server) that sets `debug.allow_debug_server: false`". Result cell: append "; the studio file reached through `studio_policy_path` wins over the variable, which is logged, and hides the command".

**Patch.** TDD §11.2 Lands and §8.6.4 honour the machine studio_policy_path in M2; T-AUD-007 studio_policy_path case

#### W-persist-16

**Minor, other.** Raised by the refuter.

**Where:** TDD.md §11.3.3 'The store' bullet (line 7180); TDD.md §11.12 T-TRU-006

**Problem.** `Trust.json` is per user, across projects and engine versions, so every editor process of the user writes it: a second instance, a second project, another engine version. The only rule is 'written atomically on the I/O thread', with no lock and no re-read before writing. A process that writes back its cached folder list loses another process's change. Worse, it can undo a removal. The user untrusts folder Y in editor A, then trusts folder Z in editor B, whose cached list still holds Y. B's write makes Y trusted again without any prompt, which defeats Remove ('A removal re-evaluates open buffers'). `projects.json`, the other shared per-user file, already has a lock and a read-modify-replace rule.

**Evidence.** §11.3.3: "The store is `%LOCALAPPDATA%\Sumi\Trust.json` ... It is per user, across projects and engine versions ... It is data only (SEC-06), written atomically on the I/O thread." §10.1: "`projects.json` ... is updated at each start: read, modify, atomic replace, under `projects.json.lock` created exclusively with retries for 1 s (proposed)". T-TRU-006: "Survives restart; applies in a second project; subfolders inherit; Remove re-restricts open buffers".

**Fix applied.** TDD §11.3.3, 'The store' bullet (line 7180): add a sub-bullet after "It is data only (SEC-06), written atomically on the I/O thread.": "  - Every editor process of the user shares it. `TrustFolder` and `UntrustFolder` apply their one change to a fresh read of the file under `Trust.json.lock` (created exclusively, retries for 1 s, as for `projects.json`, §10.1), then replace it atomically; a process never writes back its cached list. Each process re-reads `Trust.json` after its own write and when the editor regains focus, and re-evaluates open buffers when it changed, so a removal in one process is never undone by another." TDD §11.12 T-TRU-006, result cell: append "; with two editor processes open, a folder removed in one and another folder added in the other leaves only the added folder trusted in both once each has regained focus".

**Patch.** TDD §11.3.3 store sub-bullet: Trust.json.lock, read-modify-replace, re-read on write and focus; T-TRU-006 two-process case

### Round 2 — Delivery and tests

The generated parts are current. integ/check.py reports 0 problems, and running gen.fill without --write and diffing its output against TDD.md gives 0 changed lines. The suites in 14.3 match the generated lists. Every G1, G2 and G3 automated-suite test has first Lands M1. The 14.3 suites cover every criterion in the 13.4 gate rows; the remaining 13.4 disagreements are the measuring tiers and agents (W-13) and G5's 'both presentations' (W-03). The check.py rule-8 change (skip Later requirements) is correct. A simulation of the new lands_terms lint finds 0 violations with the stated exempt list, and each exemption is needed.

Patches confirmed in place and correct: V-01, -02, -04, -05, -06, -07, -09, -11, -13 and -M02. The split-step exit rule in 1.5 and 15.2 can be computed from the current Lands, and every split's later milestone matches the feature it names.

Problems found:
- **Major, Phase 0 (W-01):** the Phase 0 exit runs the whole G2 automated suite in the Run window, and the V-02 text now says every G2 test's Run window pass runs from Phase 0. P0-2 builds no output store or output view, and Phase 0 has none of the journal-writer, LSP or search threads that T-LIFE-012 checks.
- **Incomplete patches:**
  - V-10: T-PER-036, the driver-B test itself, still needs kills while paused at M1 (W-02).
  - V-M01: T-DBG-042 and T-E2E-004 still need F5 Debug runs at M2 (W-04).
  - V-12: the 13.2 header and the Appendix C glossary still say all tier-4 absolute numbers come from GPU-1 (W-13).
- **Contradictions the patches introduced:**
  - V-03 left SEC-03 (M1) with no M1 test (W-10).
  - V-03 moved T-LSP-042 to 1.2, but the 1.2 exit does not require it (W-11).
  - V-03 left the 1.5 Lands derivation rule unchanged, and many patched rows now contradict it (W-07).
  - V-08 put T-TEL-008 at M1, but its design section 11.10 is scheduled only in M4 (W-08).
- **Rows the lint misses:**
  - T-KEY-012 and T-SUP-025 still test both run presentations at M1 (W-03).
  - T-LSP-040 is a tier-4 G3 test at M1 that needs an in-editor type server (W-05).
  - Eight more rows have a step whose feature lands later, without a split, such as task, Run cell, format/snippet, History or docked steps (W-06).
- **Other gaps:**
  - The 15.3 cut-list test lists are incomplete for items 2, 3, 4 and 6 (W-09).
  - SEC-08 (M1) has no M1 test, and AI-01 is covered only by a 1.2 test (W-10).
  - T-E2E-001's 'M1 variant' contradicts its plain M2 Lands (W-12).

No engine facts changed in this lens. I checked the AutomationDriver StepExecutor citation in 14.5 against the local source, and it is correct. Scratch files are in scratchpad/verify3/delivery/.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [W-delivery-01](#w-delivery-01) | major | contradiction | modified | Phase 0 can only exit with G2 green in the Run window (RV-13, no fallback), and P0-2's Tests column runs the whole G2 automated suite. |
| [W-delivery-02](#w-delivery-02) | minor | patch incomplete | confirmed | V-10 ruled that driver B's kills while paused join from M3, and it changed §10.10, §13.4 G6, §14.1 tier 6, SPEC REL-01 and T-PER-032. |
| [W-delivery-03](#w-delivery-03) | minor | lands | modified | Live (docked) Preview lands in M4 (LRS-10: 'M1; M4 (`live_docked`)'). |
| [W-delivery-04](#w-delivery-04) | minor | lands | confirmed | V-M01 split T-DBG-052 because F5 Debug runs land in M3 (DBG-02). |
| [W-delivery-05](#w-delivery-05) | minor | untestable | confirmed | V-01 put the G3 suite at M1 against CI-pinned servers in the G3 harness, and T-GAP-008 now fails any G1–G3 automated test whose first Lands is after M1. |
| [W-delivery-06](#w-delivery-06) | minor | lands | modified | These rows are the same class V-03 and V-04 fixed: each has a step whose feature lands later than the row's first Lands, without a split. |
| [W-delivery-07](#w-delivery-07) | minor | contradiction | modified | §1.5 still gives the rule that derives each Lands value. |
| [W-delivery-08](#w-delivery-08) | minor | contradiction | modified | V-08 moved T-TEL-008, the distribution job, to M1, and §15.4 runs it on every milestone build. |
| [W-delivery-09](#w-delivery-09) | minor | patch incomplete | modified | The cut list says 'Each cut moves one item and nothing else moves', and M4's exit requires every M4 test and every split step that names M4. |
| [W-delivery-10](#w-delivery-10) | minor | other | confirmed | V-03 moved T-RLD-021 from M1 to M4. |
| [W-delivery-11](#w-delivery-11) | minor | contradiction | confirmed | V-03 moved T-LSP-042 to Lands 1.2, and §1.5 now defines Lands as 'the first milestone whose exit requires the test'. |
| [W-delivery-12](#w-delivery-12) | minor | contradiction | confirmed | T-E2E-001 describes an M1 variant, and T-E2E-010 (Lands M1) runs during that variant. |
| [W-delivery-13](#w-delivery-13) | minor | contradiction | confirmed | V-12 settled that B-13's tier-4 absolute check runs in the SumiG3Fixture session on the Win64 harness agent, and it changed §13.3 rule 1 and §14.1. |
| [W-delivery-S01](#w-delivery-s01) | minor | lands | added by refuter | T-CFG-011 is an M1 test in the G5 suite, which every M1 build must pass. |
| [W-delivery-S02](#w-delivery-s02) | minor | lands | added by refuter | The M1 exit requires 'G5', and §4.16.1 runs G5 'every build from M1'. |
| [W-delivery-S03](#w-delivery-s03) | minor | lands | added by refuter | Two M1 rows test the Paused source view: T-KEY-010 is in G5, and T-VIEW-017 asserts the current-line arrow. |

#### W-delivery-01

**Major, contradiction.** Verdict: modified.

**Where:** TDD §15.1 P0-2 row (line 8622); TDD §5.15.1 intro (line 3103); TDD §5.15.1 T-SUP-004, T-SUP-009; TDD §6.8 T-OUT-016, T-OUT-017; TDD §4.14 T-VIEW-016; TDD §3.9 T-LIFE-012

**Problem.** Phase 0 can only exit with G2 green in the Run window (RV-13, no fallback), and P0-2's Tests column runs the whole G2 automated suite. The V-02 patch now says outright that 'The Run window pass runs from Phase 0' for every G2 test. But neither P0-2 nor P0-1 builds what several G2 tests need: the output store and output view (G2(3), G2(7)), SCodeView's output line source, and the journal-writer, LSP and search threads that T-LIFE-012 checks. As written, the Phase 0 exit cannot be met, and the go/no-go has no fallback.

**Evidence.** P0-2 Builds: "`FSumiRunService` with the `OnEndFrame` start, the runner core, `FRunSupervisor` (...), `SSumiRunWindow` as a popup window (RV-02), `FSumiInputPreProcessor` from the first pump, `ScriptCancelled` delivery with the RT-06 check, the `unreal.log*` shims"; TDD column "§3.6, §5.1–§5.7, §6.1"; Tests "G2 automated suite; T-ARCH-010, T-ARCH-011, T-SUP-011". P0-1 TDD column: "§4.1–§4.3, §4.8, §4.10, §4.15" (no §4.14). §5.15.1: "The Run window pass runs from Phase 0." T-SUP-004 pass: "Output view repaints ≥ 10 Hz (paint counter); ... all lines present in the OutputStore". T-OUT-016: "View repaint ≥ 10 Hz". T-VIEW-016: "Output source: follow, hold, ring drop; 50k lines/s from a fake store". T-SUP-009 and T-OUT-017: 8-hour streaming soaks of the output ring. T-LIFE-012: "`PyGILState_Check` false on journal, LSP, search threads". Phase 0 builds only the journal codec (P0-7) and a host prototype (P0-9); the LSP transport and search are M1.

**Refuter.** Real. P0-2 Tests runs the whole G2 automated suite, and the §5.15.1 intro now says the Run window pass runs from Phase 0. Several pieces those tests need are missing from P0-2. The Run window's Output tail row (§5.5.3) reads from FSumiOutputStore through the §4.14 output line source (§4.14 table: 'The Run window's output tail uses the same source'), but neither §6.2 nor §4.14 is in P0-1 or P0-2. T-ENTRY-004 and T-SUP-002 check that Sumi's ticker clients are deferred, which needs FSumiDeferral (§3.7), and §3.7 is not in P0-2's TDD column either. T-LIFE-012 checks journal, LSP and search threads. None of them exists in Phase 0. The search thread does not exist at M1 either: §3.1 gives the Find-in-files worker '1; first use (M2)', and OUT-06 is M2. So the verifier's 'search-thread checks join at M1' is wrong, and T-LIFE-012 needs an M2 split. Two smaller corrections. In the Run window presentation only the modal is drawn (RV-16), so G2(3)'s repainting view is the Run window's output tail, not the docked §6.4 output view. And `PyGILState_Check` false applies only to never-GIL threads; the watchdog takes the GIL on every post, so it cannot be one of the threads that check covers.

**Fix applied.** TDD §15.1, row P0-2. (a) Builds: append "; `FSumiDeferral` (§3.7) for Sumi's own ticker clients; `FSumiOutputStore` with the `SumiOutput` ingest thread (§6.2), and the Run window's output tail on SCodeView's output line source (§4.14)". (b) TDD column: "§3.6, §5.1–§5.7, §6.1" → "§3.6, §3.7, §4.14 (output line source), §5.1–§5.7, §6.1, §6.2". (c) Tests column: "G2 automated suite; T-ARCH-010, T-ARCH-011, T-SUP-011" → "G2 automated suite in the Run window (T-LIFE-012 without its journal-writer, LSP and search-thread checks); T-ARCH-010, T-ARCH-011, T-SUP-011". TDD §5.15.1 intro: "The Run window pass runs from Phase 0." → "The Run window pass runs from Phase 0 (P0-2), except T-LIFE-012's journal-writer and LSP-thread checks, which join at M1, and its search-thread check, which joins at M2." TDD §3.9, row T-LIFE-012. What it checks: "Thread-affinity and lock-order checkers during the G2 soak" → "Thread-affinity and lock-order checkers during the G2 soak, over the game, watchdog and output-ingest threads, (from M1) the journal-writer and LSP threads and (from M2) the find-in-files worker". Pass criterion: "Zero asserts; `PyGILState_Check` false on journal, LSP, search threads; zero inversions" → "Zero asserts; `PyGILState_Check` false on the output-ingest thread, (from M1) the journal and LSP threads and (from M2) the search thread; zero inversions". Lands: "M1" → "M1 (M2: search thread)".

**Patch.** TDD §15.1 P0-2 Builds (FSumiDeferral, FSumiOutputStore and ingest thread, Run window output tail), TDD column and Tests column; §5.15.1 intro exceptions; T-LIFE-012 split (M1 journal/LSP, M2 search) with Lands 'M1 (M2: search thread)'

#### W-delivery-02

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD §10.12 T-PER-036 (line 6751)

**Problem.** V-10 ruled that driver B's kills while paused join from M3, and it changed §10.10, §13.4 G6, §14.1 tier 6, SPEC REL-01 and T-PER-032. It did not change T-PER-036, the driver-B test itself. That row still requires pause kills at M1, including in the full 10,000-kill run before M1 exits. The lands_terms lint does not catch the row, because 'pause' is not a term.

**Evidence.** "| T-PER-036 | 6 | G6 driver B, editor kills in typing, autosave, runs, pause | I1–I7; at least 200 per agent nightly; 10,000 green per release candidate and once before M1 exit (RV-15) | G6, PER-01, PER-02, PER-03 | M1 |" against §10.10: "Before M3, driver B's kills cover typing, autosave and runs; kills while paused join from M3, when the debugger lands."

**Refuter.** T-PER-036 (line 6751) still reads 'editor kills in typing, autosave, runs, pause' with plain M1 Lands and '10,000 green ... once before M1 exit'. §10.10 (line 6677) and §13.4 G6 say kills while paused join from M3. The lint misses it because 'pause' is not a term.

**Fix applied.** TDD §10.12, row T-PER-036. What it checks: "G6 driver B, editor kills in typing, autosave, runs, pause" → "G6 driver B, editor kills in typing, autosave, runs and (from M3) while paused in the debugger". Lands: "M1" → "M1 (M3: kills while paused)".

**Patch.** T-PER-036 kills while paused from M3; Lands 'M1 (M3: kills while paused)'

#### W-delivery-03

**Minor, lands.** Verdict: modified.

**Where:** TDD §4.16.2 table T-KEY-012 (line 1978); TDD §5.15.3 T-SUP-025 (line 3187); TDD §5.15.1 T-SUP-008 Pass criterion (line 3118); TDD §4.16.1 'During runs' bullet (line 1958); TDD §13.4 G5 row (line 8169); SPEC §8.2 REL-07 (line 587)

**Problem.** Live (docked) Preview lands in M4 (LRS-10: 'M1; M4 (`live_docked`)'). V-02 and V-04 moved the docked passes of G2 tests and of T-SUP-027 to M4. Two M1 rows still test both presentations with plain M1, and one of them, T-KEY-012, is in G5, which the M1 exit requires. The G5 texts in §4.16.1, §13.4 and SPEC REL-07 still require both presentations on every build from M1. T-SUP-008's patched pass criterion still says 'Both:' for its M1 step. The lint terms (`Live (docked)`, `(docked)`, `docked pump`) do not match 'both presentations'.

**Evidence.** T-KEY-012: "G5 during runs in both presentations | Every probe holds; buffers unchanged | G5, RT-03, RT-04, REL-07 | M1". T-SUP-025: "Pre-processor registration in both presentations | ... | M1". §4.16.1: "the whole set is re-sent while a run pumps ..., in the Run window and in Live (docked) Preview". §13.4 G5: "... AltGr+Z/X fire no Undo Last Run (RT-16); both presentations". REL-07: "key routing is checked on every build in both presentations". T-SUP-008: "Run window: 0 EUW Python ticks. Both: the AsyncTask case follows §5.4.8 ... (from M4) Docked: ...".

**Refuter.** Real. LRS-09 and LRS-10's `live_docked` are M4. T-KEY-012 ('in both presentations', M1, G5), T-SUP-025 ('in both presentations', M1), T-SUP-008's 'Both:' step, §4.16.1's 'During runs' bullet, §13.4 G5 ('both presentations') and SPEC REL-07 all require the docked pass with no M4 marker, and none of the lint terms matches them. The verifier also missed a G5 row in the same class. T-KEY-013 (M1, G5) runs 'RT-05 cases 1–4', and case 3 in §4.16.1 (line 1963) is 'Esc with a menu open in docked mode closes only the menu', so it is docked-only. The verifier's §13.4 citation is also wrong: the G5 procedure is in §4.16.1, not §5.15.1.

**Fix applied.** (1) TDD §4.16.2 table, T-KEY-012. What it checks: "G5 during runs in both presentations" → "G5 during runs in the Run window and (from M4) in Live (docked) Preview". Lands: "M1" → "M1 (M4: Live (docked) pass)". (2) T-KEY-013. What it checks: "RT-05 cases 1–4" → "RT-05 cases 1, 2 and 4, and (from M4) case 3 (a menu open in docked mode)". Pass criterion: "CANCEL with outcome `ok`; cancels; menu only; cancels after the dialog" → "CANCEL with outcome `ok`; cancels; (from M4) menu only; cancels after the dialog". Lands: "M1" → "M1 (M4: case 3, docked)". (3) TDD §4.16.1 RT-05 list, item 3: "Esc with a menu open in docked mode closes only the menu." → "(From M4) Esc with a menu open in docked mode closes only the menu." (4) §4.16.1 'During runs' bullet: "in the Run window and in Live (docked) Preview" → "in the Run window and, from M4, in Live (docked) Preview". (5) TDD §5.15.3, T-SUP-025. What it checks: "Pre-processor registration in both presentations" → "Pre-processor registration in the Run window and (from M4) Live (docked) Preview presentations". Lands: "M1" → "M1 (M4: Live (docked) pass)". (6) TDD §5.15.1, T-SUP-008 Pass criterion: "Both: the AsyncTask case follows" → "In the Run window, and from M4 also docked: the AsyncTask case follows". (7) TDD §13.4 G5 row, Thresholds: "; both presentations" → "; both presentations (Live (docked) from M4, §4.16.1)". (8) SPEC §8.2 REL-07: "key routing is checked on every build in both presentations; a failure blocks the release." → "key routing is checked on every build in both presentations (the Live (docked) pass from M4, when that presentation lands); a failure blocks the release." (9) TDD §14.6 T-GAP-008 Terms: after "`docked pump`," insert " `both presentations`, `docked Preview`, `docked mode`," in the M4 list.

**Patch.** T-KEY-012, T-KEY-013 (case 3 from M4), §4.16.1 RT-05 item 3 and During-runs bullet, T-SUP-025, T-SUP-008, §13.4 G5 row, SPEC REL-07, T-GAP-008 M4 terms

#### W-delivery-04

**Minor, lands.** Verdict: confirmed.

**Where:** TDD §8.11 T-DBG-042 (line 5856); TDD §14.5 T-E2E-004 (line 8310)

**Problem.** V-M01 split T-DBG-052 because F5 Debug runs land in M3 (DBG-02). Two M2 rows assert the same F5 behaviour with plain M2 Lands, and both are in the M2 exit: T-E2E-004 by name, T-DBG-042 as an M2 test. Neither can pass in M2, because there is no in-editor Debug run to disable or re-enable.

**Evidence.** T-DBG-042: "Exclusivity with a client attached: banner, F5 disabled, `breakpoint()` routed to debugpy; re-enable after disconnect | Banner shown; F5 disabled with reason; IDE gets `stopped`; F5 enabled after disconnect | U16, D25, DBG-07 | M2". T-E2E-004 Pass criterion: "while attached, F5 in Sumi is disabled with the U16 reason; after disconnect Debug runs return", Lands M2. SPEC DBG-02: "**Debug run** (**F5** when idle, U21) ... | M3 (gate G4)". T-DBG-052: "(from M3) F5 starts a Debug run", Lands "M2 (M3: F5 Debug run)".

**Refuter.** DBG-02 Debug runs (F5 when idle) are M3. T-DBG-042 (M2) asserts 'F5 disabled; F5 enabled after disconnect', and T-E2E-004 (M2, named in the M2 exit) asserts 'F5 in Sumi is disabled ... after disconnect Debug runs return'. At M2 there is no Debug run to disable. §8.6.3's exclusivity bullet has the same scope, but the tests are what block the exit. A scan shows `Debug run` and `F5 disabled` flag only T-E2E-004 and T-DBG-042. Every other hit is already M3 or split to M3.

**Fix applied.** As proposed. TDD §8.11 T-DBG-042. What it checks: "Exclusivity with a client attached: banner, F5 disabled, `breakpoint()` routed to debugpy; re-enable after disconnect" → "Exclusivity with a client attached: banner, `breakpoint()` routed to debugpy; (from M3) F5 disabled, then re-enabled after disconnect". Pass criterion: "Banner shown; F5 disabled with reason; IDE gets `stopped`; F5 enabled after disconnect" → "Banner shown; IDE gets `stopped`; (from M3) F5 disabled with reason and enabled after disconnect". Lands: "M2" → "M2 (M3: F5 state)". TDD §14.5 T-E2E-004 Pass criterion: "while attached, F5 in Sumi is disabled with the U16 reason; after disconnect Debug runs return" → "(from M3) while attached, F5 in Sumi is disabled with the U16 reason, and after disconnect Debug runs return". Lands: "M2" → "M2 (M3: F5 checks)". T-GAP-008 Terms: add `Debug run` and `F5 disabled` to the M3 list.

**Patch.** T-DBG-042 and T-E2E-004 F5 steps from M3 with split Lands; T-GAP-008 M3 terms 'Debug run', 'F5 disabled'

#### W-delivery-05

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD §7.12 T-LSP-040 (line 5113); TDD §7.10.1 Procedure (line 4994)

**Problem.** V-01 put the G3 suite at M1 against CI-pinned servers in the G3 harness, and T-GAP-008 now fails any G1–G3 automated test whose first Lands is after M1. T-LSP-040 is a tier-4 (in-editor) G3 test at M1 that needs a type server running in the editor with the minimal stub as StubPath. In M1 no type server ships, and the G3 harness runs servers only out of process through the probe client. So the test cannot run as written, but T-GAP-008 forbids moving it to M2.

**Evidence.** "| T-LSP-040 | 4 | Fallback minimal stub | `unreal.*` from live; no unknown-attribute errors on `unreal` | G3 | M1 |". §7.10.1: "Nothing third-party ships in M1"; "One editor session (tier 4) waits for the oracle, then runs stubsnap ... the server probes (tier 7: the probe client against the CI-pinned servers) then run out of process."

**Refuter.** T-LSP-040 (tier 4, G3, M1) needs a type server with the minimal stub as StubPath. §7.10.1 says nothing third-party ships in M1, and the server probes run out of process (tier 7) through the probe client. In M1, '`unreal.*` from live' is trivially true, because no server competes. Since T-GAP-008 holds G3 tests at first Lands M1, the right fix is a split, as proposed.

**Fix applied.** As proposed. TDD §7.12 T-LSP-040. Tier: "4" → "7, 4". What it checks: "Fallback minimal stub" → "Fallback minimal stub: in M1 the G3 probe client runs each CI-pinned server with `StubPath` set to the minimal `unreal/__init__.pyi` (§7.10.1, RV-11); (from M2) the shipped server in the editor". Pass criterion: "`unreal.*` from live; no unknown-attribute errors on `unreal`" → "No unknown-attribute diagnostics on `unreal.*` names; (from M2) `unreal.*` completion comes from live results". Lands: "M1" → "M1 (M2: shipped server in the editor)".

**Patch.** T-LSP-040 tiers 7, 4; M1 probe-client case with the minimal stub; Lands 'M1 (M2: shipped server in the editor)'

#### W-delivery-06

**Minor, lands.** Verdict: modified.

**Where:** TDD §5.15.2 T-RUN-019 (line 3145), T-RUN-023 (line 3149); TDD §6.8 T-REPL-012 (line 3944), T-OUT-013 (line 3905); TDD §14.6 T-GAP-014 (line 8339), T-GAP-007 (line 8332); TDD §4.10 T-IME-002 (line 1773); TDD §8.11 T-DBG-050 (line 5864); TDD §14.6 T-GAP-008 term list (line 8333)

**Problem.** These rows are the same class V-03 and V-04 fixed: each has a step whose feature lands later than the row's first Lands, without a split. The lands_terms lint added by V-03 does not catch them, which is why the patcher's simulation shows 0 violations. Each row blocks its milestone exit, and T-IME-002 is in G1, which also gates Phase 0.

**Evidence.** T-RUN-019 (M1): "Numbering across REPL, console, `not executed`, task and a Python restart" (task mode is M3). T-REPL-012 (M1): "... refused during a task". T-OUT-013 (M1, tier 3): "two steps of one task, with two run IDs, land in one run block" (the task run-ID encoding test T-API-017 is M3). T-GAP-014 (M1): "Every run command (selection, line, cell, buffer, Run File, REPL)" (RUN-03 Run cell is M2). T-IME-002 (M1, G1): "Notifications for typing, paste, undo, format, snippet, reload" (INT-10 format is M2; EDT-12 snippets are M4). T-RUN-023 (M1, G6): "History shows the run as `lost`" (RUN-12: "M1 (records); M2 (browse, re-run)"). T-GAP-007 (M2): "in the Run window and the task banner". T-DBG-050 (M3): "docked Preview banner refreshed before the modal".

**Refuter.** Most rows are real: T-RUN-019 (task), T-REPL-012 (refused during a task), T-GAP-014 (Run cell is RUN-03, M2), T-IME-002 (format is INT-10, M2; snippets are EDT-12, M4; the row is in G1, which runs in Phase 0), T-RUN-023 (the History view is RUN-12's M2 part; G6 at M1), T-GAP-007 (task banner, M3) and T-DBG-050 (docked banner, M4). T-OUT-013 is not real. It is a tier-3 store test, and §6.2 gives the store a `BindRunId(Id, RunNumber)` call that the harness can make twice with one run number. Two run IDs in one block needs no @task, so it stays M1 and only the wording should change. T-RUN-023 also needs an M1 observable. At M1 the lost run is visible as the recovery output block (§10, `RecordLostRun`), not only as 'the run record'. With T-OUT-013 reworded, the term `one task` is unnecessary.

**Fix applied.** TDD §5.15.2 T-RUN-019. What it checks: "Numbering across REPL, console, `not executed`, task and a Python restart" → "Numbering across REPL, console, `not executed`, (from M3) task steps, and a Python restart". Lands: "M1 (M3: task)". TDD §6.8 T-REPL-012 Pass criterion: "; refused during a task" → "; (from M3) refused during a task". Lands: "M1 (M3: task refusal)". TDD §6.8 T-OUT-013 Pass criterion: "two steps of one task, with two run IDs, land in one run block" → "two run IDs bound to one run number with `BindRunId` (as a task's steps are, §6.2) land in one run block". Lands stays M1. TDD §14.6 T-GAP-014. What it checks: "Every run command (selection, line, cell, buffer, Run File, REPL) on a dirty buffer" → "Every run command (selection, line, (from M2) cell, buffer, Run File, REPL) on a dirty buffer". Lands: "M1 (M2: Run cell)". TDD §4.10 T-IME-002. What it checks: "Notifications for typing, paste, undo, format, snippet, reload" → "Notifications for typing, paste, undo, reload, and a multi-edit programmatic ChangeSet of the kind format and snippet expansion apply". Lands stays M1. TDD §5.15.2 T-RUN-023 Pass criterion: "History shows the run as `lost`" → "The restart shows the lost-run block for it in the output, and its run record's outcome is `lost`; (from M2) History shows it as `lost`". Lands: "M1 (M2: History view)". TDD §14.6 T-GAP-007. What it checks: "in the Run window and the task banner (M2)" → "in the Run window and (from M3) the task banner (M2)". Lands: "M2 (M3: task banner)". TDD §8.11 T-DBG-050. What it checks: "docked Preview banner refreshed before the modal" → "(from M4) docked Preview banner refreshed before the modal". Pass criterion: "docked banner reads" → "(from M4) docked banner reads". Lands: "M3 (M4: docked banner)". T-GAP-008 Terms: add `task banner` and `during a task` to the M3 list, and `History shows` to the M2 list.

**Patch.** T-RUN-019, T-REPL-012, T-GAP-014, T-RUN-023, T-GAP-007, T-DBG-050 split; T-OUT-013 and T-IME-002 reworded (stay M1); T-GAP-008 terms 'task banner', 'during a task' (M3), 'History shows' (M2)

#### W-delivery-07

**Minor, contradiction.** Verdict: modified.

**Where:** TDD §1.5 Lands bullet (line 265)

**Problem.** §1.5 still gives the rule that derives each Lands value. V-03 identified that rule as the cause of the early Lands ('a covered requirement's Lands beats the test's own component milestone') and changed only lands.py's bookkeeping. Many patched rows now contradict the documented rule, so anyone who adds a test by following §1.5 recreates the V-03 errors.

**Evidence.** §1.5: "The value comes from the first of these that applies: a milestone the row itself names, such as \"(M2)\", \"From M2:\" or \"in an M2 build\"; otherwise the latest Lands among the SPEC requirements in Covers; otherwise the milestone of the test's component in §15.2." Under that rule: T-RLD-021 (Covers SEC-03, M1) would be M1, but it is M4. T-SNP-016 (RUN-06, M1) would be M1, but it is M4. T-UEI-012 (RUN-24, M1) would be M1, but it is M4. T-CAN-017 and T-SUP-015 (no SPEC ID, components M1) would be M1, but both are M4. T-GAP-016 (RUN-23, Later) would be Later, but it is M1. T-DBG-014 (DBG-09, Later) would be Later, but it is M3. T-STUB-011 (INT-12, M2, no milestone in its text) would be M2, but it is "M1 (M2: shipped package)".

**Refuter.** Real. The §1.5 rule (row marker, then the latest covered-requirement Lands, then the component) produces the values V-03 and V-07 had to correct (T-RLD-021, T-SNP-016, T-UEI-012, T-CAN-017, T-SUP-015, T-GAP-016, T-DBG-014). The verifier's replacement is wrong, though. It adds 'the milestone of the test's component' as a term of the maximum, but areas span milestones. TEL is M4 in §15.2 and in lands.py COMPONENT, so the rule would push T-TEL-008, the M1 distribution job that V-08 moved, back to M4. DBG is M3, so it would push T-DBG-044 (M2) and other M2 debugpy rows to M3. The component must stay a fallback that applies only when nothing else gives a value.

**Fix applied.** TDD §1.5, Lands bullet. Replace the two sentences "The value comes from the first of these that applies: a milestone the row itself names, such as "(M2)", "From M2:" or "in an M2 build"; otherwise the latest Lands among the SPEC requirements in Covers; otherwise the milestone of the test's component in §15.2. A requirement with a split Lands ("M1 (x); M2 (y)") contributes the part that the test exercises." with: "The value is the latest of: the milestone of each feature the test's steps need (the §15.2 Scope and TDD sections columns); any milestone the row itself names, such as "(M2)", "From M2:" or "in an M2 build"; and, for each SPEC requirement in Covers, the part of its Lands that the test exercises (a split Lands such as "M1 (x); M2 (y)" contributes only that part; Later and Withdrawn requirements contribute nothing). Only when none of these gives a value does the milestone of the test's component in §15.2 apply. When only some steps need a later milestone, the value is split, as in `M1 (M4: trust step)`, and those steps are marked "(from Mx)" in the row."

**Patch.** TDD §1.5 Lands derivation rule rewritten (latest of features, row markers, exercised requirement parts; component only as fallback)

#### W-delivery-08

**Minor, contradiction.** Verdict: modified.

**Where:** TDD §15.2 M1 row 'TDD sections' (line 8637); TDD §15.2 M2, M3 and M4 rows 'TDD sections' (lines 8638–8640); TDD §11.12 T-TEL-008 (line 7788); TDD §15.4 first bullet (line 8665)

**Problem.** V-08 moved T-TEL-008, the distribution job, to M1, and §15.4 runs it on every milestone build. Its design, §11.10, is scheduled only in the M4 row. V-06 fixed the same mismatch for §7.9.3, but the M1 sections still miss other designs that M1 tests need: §11.7, the API wiring and console commands (T-UEI-013, -014, -015, -016; API-01 and API-03 at M1), and the keymap, shadowed-shortcut and UE-Python-settings parts of §11.1.4 (T-CFG-008, T-CFG-009, T-CFG-011; CFG-06 at M1). In the other direction, '§5 (except §5.10.4 tasks)' places §5.5.4 Live (docked) Preview (M4) and §5.9.6 task steps (M3) in M1, and neither the M3 nor the M4 row lists them.

**Evidence.** M1 TDD sections: "... §5 (except §5.10.4 tasks), ... §11.1 (project and user classes), §11.4 (the writer; run and snapshot-skip events), §11.6.1, §11.6.2, §11.8.1, §12". M3: "§5.10.4, §8.1–§8.5, §8.8, §8.9, §9.1–§9.4". M4: "§9.5, §11.1–§11.8, §11.10". T-TEL-008 Lands "M1 (M2: debugpy prune)". §15.4: "Every milestone build is produced by `BuildPlugin` ...; the distribution job (T-TEL-008) checks the SBOM, notices, the SHA-256 manifest, licences and Epic headers before a package is published".

**Refuter.** Real. M1 needs §11.7: API-01 and API-03 are M1, and T-UEI-013..016 are M1 with Sumi.Open, Sumi.RunFile and Sumi.ClearOutput. It needs §11.10, because T-TEL-008 is M1 and §15.4 runs it on every milestone build. It needs the UE-Python-settings, keymap and shadowed-shortcuts parts of §11.1.4 (CFG-06 and T-CFG-008/009/011 at M1). '§5 (except §5.10.4 tasks)' wrongly places §5.5.4 (docked, M4) and §5.9.6 (task steps, M3) in M1. One refinement: once M1 takes §11.10 without the debugpy prune and M2 takes the prune, nothing of §11.10 is left for M4. The M4 row's '§11.10 (rest)' would be empty, so M4 should drop §11.10. The Authenticode signing of ThirdParty binaries belongs with M2, when the first third-party binaries ship.

**Fix applied.** TDD §15.2, M1 row, TDD sections: "§5 (except §5.10.4 tasks)" → "§5 (except §5.5.4 Live (docked) Preview, §5.9.6 task steps and §5.10.4 tasks)"; "§11.1 (project and user classes)" → "§11.1 (project and user classes; from §11.1.4 the UE Python settings, keymap views and shadowed-shortcuts page)"; after "§11.6.2," insert " §11.7 (the M1 API and the Sumi.Open, Sumi.RunFile and Sumi.ClearOutput commands),"; after "§11.8.1," insert " §11.10 (without the pruned-debugpy and signing rows),". M2 row, TDD sections: append ", §11.10 (pruned debugpy; signing of the shipped ThirdParty binaries)". M3 row, TDD sections: "§5.10.4," → "§5.9.6, §5.10.4,". M4 row, TDD sections: "§9.5, §11.1–§11.8, §11.10" → "§5.5.4, §9.5, §11.1–§11.8 (the parts not in M1 or M2)".

**Patch.** TDD §15.2 TDD-sections cells: M1 excludes §5.5.4 and §5.9.6, adds §11.1.4 parts, §11.7 and §11.10 (without pruned debugpy and signing); M2 adds §11.10 pruned debugpy and signing; M3 adds §5.9.6; M4 adds §5.5.4 and drops §11.10

#### W-delivery-09

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §15.3 cut list 'Tests that move with it', rows 2, 3, 4 and 6 (lines 8654–8658)

**Problem.** The cut list says 'Each cut moves one item and nothing else moves', and M4's exit requires every M4 test and every split step that names M4. If a cut is invoked, these tests and steps must leave the M4 exit with their item. Round 1 created several such steps and extended only row 3, and only for G2. Without the moved tests listed, invoking a cut leaves the M4 exit unmeetable.

**Evidence.** Row 2 (presets → 1.0.x): "T-CFG-010, T-CFG-012", but T-DIAG-021 is "M1 (M4: Alt+Enter in the PyCharm preset)". Row 3 (docked → 1.1): "T-SUP-022, T-SUP-010, and the Live (docked) pass of every G2 test", but T-SUP-015 ("reached during a docked pump", M4) and T-SUP-023 ("Docked chosen with the tab closed", M4) are whole docked tests, and T-SUP-027 ("M4: docked case", G5), T-UEI-002 ("M4: Live (docked) Preview Stop") and T-DBG-050's docked banner are docked steps outside G2. Row 4 (studio tier → 1.1): "T-SNP-007, T-SNP-017 (studio parts)", but T-SNP-008 ("studio tier without write access writes a change folder"), T-GAP-020 ("the studio tier comes from the policy path") and T-E2E-007 ("a tampered studio item shows Modified since publish") also have studio parts. Row 6 (bundle UI → 1.0.x): "T-TEL-006, T-TEL-007", but T-LSP-043 is "M1 (M4: bundle exclusion)".

**Refuter.** Real. §15.3 says each cut moves one item, but rows 2, 3, 4 and 6 do not list all the tests and steps that move. The verifier's list is correct but incomplete. Row 5 (Revert module state) misses T-RLD-015, 'Revert on execute and Update module state' (M3), which tests §9.2's Revert on execute and Update module state. Row 6 (diagnostics bundle UI; only crash breadcrumbs remain) misses T-UEI-015's Sumi.Diagnostics.Export step. Row 2 (presets) misses T-GAP-009's per-preset keymap sheets, and T-CFG-011's PyCharm and VS Code rows (see the missed issue on T-CFG-011). Row 3 misses T-KEY-013's docked case 3 (see W-delivery-03). Row 1 is adequate, because 'Linux tier 4 and 6 jobs' covers the Linux cases of T-PER-046 and T-PER-048.

**Fix applied.** TDD §15.3, 'Tests that move with it' column. Row 2: "T-CFG-010, T-CFG-012" → "T-CFG-010, T-CFG-012, T-DIAG-021's Alt+Enter step, T-CFG-011's PyCharm and VS Code rows, and T-GAP-009's PyCharm and VS Code keymap sheets". Row 3: "T-SUP-022, T-SUP-010, and the Live (docked) pass of every G2 test (§5.15.1), including T-SUP-008's docked step" → "T-SUP-022, T-SUP-010, T-SUP-015, T-SUP-023's docked case, the Live (docked) pass of every G2 test (§5.15.1), including T-SUP-008's docked step, and the docked steps of T-SUP-025, T-SUP-027, T-KEY-012, T-KEY-013 (case 3), T-UEI-002 and T-DBG-050". Row 4: "T-SNP-007, T-SNP-017 (studio parts)" → "T-SNP-007 and T-SNP-017 (studio parts), and the studio parts of T-SNP-008, T-GAP-020 and T-E2E-007". Row 5: "T-RLD-014" → "T-RLD-014, T-RLD-015". Row 6: "T-TEL-006, T-TEL-007" → "T-TEL-006, T-TEL-007, T-LSP-043's bundle step, and T-UEI-015's Sumi.Diagnostics.Export step". TDD §14.6 T-GAP-008 What it checks: after "prints, per milestone, the tests and the split steps its exit requires" insert ", and per cut-list item (§15.3) the tests and steps that move with it".

**Patch.** TDD §15.3 rows 2 to 6 list every moved test and step (including T-RLD-015 and T-UEI-015); T-GAP-008 prints per cut-list item

#### W-delivery-10

**Minor, other.** Verdict: confirmed.

**Where:** SPEC §7.18 SEC-03 (line 486), SEC-08 (line 491); TDD §9.7 T-RLD-021 (line 6248); TDD §2.8 T-ARCH-004 (line 597), T-ARCH-015 (line 608); SPEC §7.21 AI-01 (line 525); TDD §11.12 T-MCP-004 (line 7799); TDD §14.6 T-GAP-008 Pass criterion (line 8333)

**Problem.** V-03 moved T-RLD-021 from M1 to M4. That was correct, but it left SEC-03 (Lands M1) with no test at M1; its other test, T-RLD-016, is M3. SEC-08 (Lands M1) has only M2 tests. So the M1 exit verifies neither requirement. AI-01 (MCP tiers 1–2, Lands 1.1) is covered only by T-MCP-004, the 1.2 tier-3 test. The clause V-06 added to T-GAP-008 checks only the first part of split Lands, so it misses unsplit requirements like these.

**Evidence.** SEC-03: "**No auto-run** ... no `init_unreal.py` shipped. | M1"; Covers: T-RLD-016 (M3), T-RLD-021 (M4). SEC-08: "No listening sockets in 1.0 ... | M1 | With every 1.0 feature on except Debug in VS Code, a port scan finds no Sumi listener."; Covers: T-ARCH-015, T-AUD-007, T-E2E-004, all M2. T-ARCH-004 (M1) already checks "no `init_unreal.py`" but its Covers are "D7, D22, U36, MAINT-03, MAINT-06". T-MCP-004: "Tier 3 (1.2) ... | D11, AI-01, AI-03 | 1.2", while T-MCP-001..003 (1.1) cover only D11. T-GAP-008: "when the first part of a split SPEC Lands has no covering test whose first Lands is that part's milestone".

**Refuter.** A simulation over the current rows, SPEC Lands and Covers flags exactly SEC-03 (tests M3 and M4 only), SEC-08 (tests all M2) and AI-01 (Lands 1.1, covered only by the 1.2 test T-MCP-004) under the proposed clause. The V-06 clause checks only split Lands, so it misses them. T-ARCH-004 already checks for no init_unreal.py but does not cover SEC-03. T-ARCH-015's 'every 1.0 feature' also cannot be met at M2. T-MCP-001..003 check AI-01's acceptance: toggle and registration, busy or refused during a run, and propose_edit.

**Fix applied.** As proposed, with the audit wording made precise. TDD §2.8 T-ARCH-004. Covers: append ", SEC-03". What it checks: "Tree and staged-package scan" → "Tree and staged-package scan; a source scan that every call of `FSumiRunService::Submit` is reached from a user command, the public API (§11.7), a console command or the console executor, or reload-on-save, and none from a startup, tab-open or file-change handler (SEC-03 code-path audit)". Pass criterion: append "; the Submit scan finds no other caller". TDD §2.8 T-ARCH-015. What it checks: "Listener audit over a session using every 1.0 feature, with and without a machine-layer `debug.allow_debug_server: false`, and with the key turned off mid-session while the listener is open" → "Listener audit over a session using every feature of the build under test; (from M2) with and without a machine-layer `debug.allow_debug_server: false`, and with the key turned off mid-session while the listener is open". Pass criterion: "No new listener; after \"Debug in VS Code\" one on 127.0.0.1" → "No new listener; (from M2) after \"Debug in VS Code\" one on 127.0.0.1". Lands: "M2" → "M1 (M2: Debug in VS Code and the policy key)". Keep its lands_terms exempt entry. TDD §11.12: remove AI-01 from T-MCP-004's Covers ("D11, AI-01, AI-03" → "D11, AI-03"), and add ", AI-01" to the Covers of T-MCP-001, T-MCP-002 and T-MCP-003. Update the §14.7 rows: SEC-03 → "T-ARCH-004, T-RLD-016, T-RLD-021"; AI-01 → "T-MCP-001, T-MCP-002, T-MCP-003". TDD §14.6 T-GAP-008 Pass criterion: replace "when the first part of a split SPEC Lands has no covering test whose first Lands is that part's milestone," with "when the first milestone of a requirement's Lands (split or not; Later and Withdrawn requirements excepted) has no covering test whose first Lands is that milestone or one of whose split steps names it,".

**Patch.** T-ARCH-004 covers SEC-03 with the Submit caller scan; T-ARCH-015 split 'M1 (M2: Debug in VS Code and the policy key)'; AI-01 moved from T-MCP-004 to T-MCP-001..003; T-GAP-008 first-milestone clause; §14.7 regenerated

#### W-delivery-11

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §15.2 1.2 row 'Suites that must be green to exit' (line 8644); TDD §7.12 T-LSP-042 (line 5115)

**Problem.** V-03 moved T-LSP-042 to Lands 1.2, and §1.5 now defines Lands as 'the first milestone whose exit requires the test'. The 1.2 exit lists specific tests only and does not include T-LSP-042, so no exit requires it. Unlike the M1–M4 rows, the post-1.0 rows also lack the 'every test whose Lands is …' clause.

**Evidence.** 1.2 exit: "T-MCP-004, T-HDL-001..004; the macOS IME checklist". "| T-LSP-042 | 7 | macOS `sumi-spawn --watch`: host killed | Server group killed within 1 s (1.2) | D4, D9 | 1.2 |".

**Refuter.** Of the post-1.0 tests, every 1.1 test (T-BRG-*, T-MCP-001..003, T-MCP-005) is in the 1.1 exit, and every 1.2 test except T-LSP-042 is in the 1.2 exit. No exit requires T-LSP-042, so by §1.5's definition its Lands value is meaningless. The 1.0.x and 1.1 Scope cells name the cut-list items, but their exits do not name those items' tests.

**Fix applied.** As proposed. TDD §15.2. 1.2 row, Suites: "T-MCP-004, T-HDL-001..004; the macOS IME checklist" → "T-MCP-004, T-HDL-001..004, T-LSP-042; the macOS IME checklist; every test whose Lands is 1.2". 1.1 row, Suites: append "; every test whose Lands is 1.1, and the tests of each cut-list item moved to 1.1 (§15.3)". 1.0.x row, Suites: append "; the tests of cut-list items 2 and 6 if they were cut (§15.3)".

**Patch.** TDD §15.2 1.2 exit adds T-LSP-042 and every 1.2 test; 1.1 exit adds every 1.1 test and moved cut-list tests; 1.0.x exit adds cut items 2 and 6 tests

#### W-delivery-12

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §14.5 T-E2E-001 (line 8307); TDD §14.5 T-E2E-010 (line 8316); TDD §15.2 M1 row 'Suites' (line 8637)

**Problem.** T-E2E-001 describes an M1 variant, and T-E2E-010 (Lands M1) runs during that variant. But T-E2E-001's Lands is plain M2, and §14.5 runs journey tests 'nightly from the milestone in the Lands column'. So the M1 variant is never scheduled at M1, and the M1 exit does not list it. Apart from Ctrl+Shift+V (UEI-09, M2), every step uses an M1 feature: REPL completion, hover, Ctrl+Q, the sel alias, Undo Last Run.

**Evidence.** T-E2E-001: "**J1 Explore an API** (from M2; the M1 variant skips the Ctrl+Shift+V step)", Lands "M2". T-E2E-010: "During the M1 variant of T-E2E-001 and during a supervised 60 s Run-window run, kill -9 ...", Lands "M1 (M3: kill during T-E2E-006)". §14.5: "nightly from the milestone in the **Lands** column".

**Refuter.** T-E2E-001 defines an M1 variant but has plain M2 Lands, and §14.5 schedules journeys 'nightly from the milestone in the Lands column'. Every requirement J1 exercises except UEI-09 (Ctrl+Shift+V, M2) is M1: UEI-03, INT-01..05, REPL-01, REPL-03, RUN-07 and RUN-22's M1 part. T-E2E-010 only runs the variant's steps as a backdrop for kills and checks recovery, so the M1 public preview exits without its J1 acceptance criteria being checked.

**Fix applied.** As proposed. TDD §14.5 T-E2E-001. What it checks: "(from M2; the M1 variant skips the Ctrl+Shift+V step)" → "(M1; the Ctrl+Shift+V step from M2)", and "Then, in a scratch tab, Ctrl+Shift+V and Ctrl+Enter, and change the selection" → "Then, (from M2) in a scratch tab, Ctrl+Shift+V and Ctrl+Enter, and change the selection". Pass criterion: "the inserted line is exactly the U20 text" → "(from M2) the inserted line is exactly the U20 text". Lands: "M2" → "M1 (M2: Ctrl+Shift+V step)". TDD §15.2 M1 row, Suites: "T-E2E-008 (without trust), T-E2E-010" → "T-E2E-001 (without Ctrl+Shift+V), T-E2E-008 (without trust), T-E2E-010".

**Patch.** T-E2E-001 M1 with the Ctrl+Shift+V step from M2, Lands 'M1 (M2: Ctrl+Shift+V step)'; M1 exit names T-E2E-001 (without Ctrl+Shift+V)

#### W-delivery-13

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §13.2 Kind paragraph (line 8104); TDD Appendix C glossary row 'Perf-reference GPU agent' (line 9225); TDD §13.4 G2 row 'Measured where' and 'Agent and cadence' (line 8166)

**Problem.** V-12 settled that B-13's tier-4 absolute check runs in the SumiG3Fixture session on the Win64 harness agent, and it changed §13.3 rule 1 and §14.1. Two untouched statements still say that every tier-4 absolute number comes from GPU-1. Under the same rule 1 ('the agent class named in §13.4 for its gate'), the §13.4 G2 row names only GPU agents and only B-12a as tier 3. But G2's automated suite also runs the tier-3 tests T-ARCH-010, T-SUP-032, T-CAN-011 and T-CAN-012, which §14.1 places on the Win64 harness agent.

**Evidence.** §13.2: "The reference machine for absolute tier-4 numbers is the perf-reference GPU agent at 3840×2160 (§14.1)." Appendix C: "`GPU-1`, the one agent whose tier-4 results are the absolute numbers and the baselines". §13.3 rule 1: "the perf-reference GPU agent for tier 4, except G3's tier-4 `SumiG3Fixture` session (B-13), which runs on the Win64 harness agent". §13.4 G2: "Tier 4 editor suite `Sumi.G2.*` for (1)–(7), ...; tier 3 harness for the (1) mechanism sub-budget (B-12a) | GPU agents; every build from Phase 0; soak nightly".

**Refuter.** §13.2's Kind paragraph and the Appendix C glossary row ('the one agent whose tier-4 results are the absolute numbers') contradict the B-13 exception that V-12 added to §13.3 rule 1 and the §14.1 GPU-1 row. The §13.4 G2 row names only GPU agents, but G2's automated suite includes the tier-3 tests T-ARCH-010, T-SUP-032, T-CAN-011, T-CAN-012 and T-CAN-002, which run on the Win64 harness agent. That agent runs tiers 1–3 (§14.1 agents table), and §13.3 rule 1 sends tier 3 to it.

**Fix applied.** As proposed. TDD §13.2 Kind paragraph: "The reference machine for absolute tier-4 numbers is the perf-reference GPU agent at 3840×2160 (§14.1)." → "The reference machine for absolute tier-4 numbers is the perf-reference GPU agent at 3840×2160 (§14.1), except B-13, whose tier-4 absolute check runs in the `SumiG3Fixture` session on the Win64 harness agent (§13.3 rule 1)." Appendix C, row 'Perf-reference GPU agent': "`GPU-1`, the one agent whose tier-4 results are the absolute numbers and the baselines" → "`GPU-1`, whose tier-4 results are the absolute numbers and baselines for every tier-4 Gate row except B-13 (§13.3 rule 1)". §13.4 G2 row. Measured where: "tier 3 harness for the (1) mechanism sub-budget (B-12a)" → "tier 3 `SumiPyHarness` for T-ARCH-010, T-SUP-032, T-CAN-011, T-CAN-012 and the (1) mechanism sub-budget (B-12a, T-CAN-002)". Agent and cadence: "GPU agents; every build from Phase 0; soak nightly" → "GPU agents, and the Win64 harness agent for the tier-3 tests; every build from Phase 0; soak nightly".

**Patch.** TDD §13.2 Kind paragraph and Appendix C GPU agent row name the B-13 exception; §13.4 G2 row names the tier-3 tests and the Win64 harness agent

#### W-delivery-S01

**Minor, lands.** Raised by the refuter.

**Where:** TDD §11.12 T-CFG-011 (line 7716); TDD §11.1.4 'Shadowed Unreal shortcuts page' golden lists; SPEC §7 CFG-02

**Problem.** T-CFG-011 is an M1 test in the G5 suite, which every M1 build must pass. It checks the shadowed-shortcuts page 'per preset', and §11.1.4's golden list has separate PyCharm and VS Code rows. The PyCharm and VS Code presets land in M4 (CFG-02: 'M1 (Charcoal, rebinding); M4 (presets)'), and they are cut-list item 2. The row has no split, and the lint misses it because its text says 'per preset', not 'PyCharm'.

**Evidence.** T-CFG-011: "Rows equal the golden list, the fixed rows and the extension rows of §11.1.4, per preset and milestone (Ctrl+Shift+K from M2); ..." | U23, D25, G5, CFG-06, RV-19 | M1. CFG-02 Lands: "M1 (Charcoal, rebinding); M4 (presets), cut list #2 → 1.0.x". §11.1.4: "VS Code adds Ctrl+P. PyCharm, for §9.3 as written: the Charcoal list without F10, F11 and Shift+F11 ...".

**Fix applied.** TDD §11.12 T-CFG-011 Pass criterion: "Rows equal the golden list, the fixed rows and the extension rows of §11.1.4, per preset and milestone (Ctrl+Shift+K from M2)" → "Rows equal the golden list, the fixed rows and the extension rows of §11.1.4 for Charcoal and, from M4, for the PyCharm and VS Code presets, per milestone (Ctrl+Shift+K from M2)". Lands: "M1" → "M1 (M4: PyCharm and VS Code presets)". The §15.3 row 2 entry is in the W-delivery-09 fix.

**Patch.** T-CFG-011 pass criterion: Charcoal, and from M4 the PyCharm and VS Code presets (Lands merged with W-ux-04)

#### W-delivery-S02

**Minor, lands.** Raised by the refuter.

**Where:** TDD §14.3 intro paragraph (line 8276); TDD §14.3 G5 row (T-UEI-004 M2, T-RLD-020 M3); TDD §15.2 M1 row 'Suites that must be green to exit'; TDD §4.16.1 heading 'every build from M1'

**Problem.** The M1 exit requires 'G5', and §4.16.1 runs G5 'every build from M1'. But the generated G5 suite includes T-UEI-004 (Ctrl+Shift+V, Lands M2) and T-RLD-020 (entry point Ctrl+F5, Lands M3). T-GAP-008's V-01 rule holds only G1–G3 to first Lands M1, and no text says what a gate named in an exit means for tests that land later. Read literally, the M1 exit cannot be met.

**Evidence.** §14.3 G5 automated suite: "..., T-REPL-003, T-RLD-020, T-CFG-011, T-UEI-004". T-UEI-004 Lands "M2"; T-RLD-020 Lands "M3". §15.2 M1 Suites: "G2, G5, G6 (...), G7; ...". T-GAP-008: "when a test in the automated suite of G1, G2 or G3 (§14.3) has a first Lands later than M1".

**Fix applied.** TDD §14.3 intro: after "The automated status that CI, T-SUP-010 and T-DBG-028 read excludes the manual checklist, and T-GAP-008 enforces the split." append "Where a milestone exit (§15.2) or the Phase 0 go/no-go (§15.1) names a gate, it means the tests of the gate's automated suite whose first Lands is at or before that milestone, run with only the split steps that have joined by then (§1.5). G1, G2 and G3 hold no test whose first Lands is after M1 (T-GAP-008); G5 grows after M1 (T-UEI-004 joins at M2 and T-RLD-020 at M3)."

**Patch.** TDD §14.3 intro defines what a gate named in an exit or the Phase 0 go/no-go means

#### W-delivery-S03

**Minor, lands.** Raised by the refuter.

**Where:** TDD §4.14 T-VIEW-017 (line 1904); TDD §4.16.2 table T-KEY-010 (line 1976); TDD §4.16.1 Fixture bullet (line 1938); TDD §4.14 line-source table, row 'Paused-window source | §8'

**Problem.** Two M1 rows test the Paused source view: T-KEY-010 is in G5, and T-VIEW-017 asserts the current-line arrow. §4.14 assigns that source to §8. §8 is the Paused window, which lands in M3 (DBG-03), and the breakpoints it draws are DBG-01, also M3. The lint term `Paused window` does not match 'Paused source', so these rows were not caught.

**Evidence.** T-KEY-010: "G5 variants: floating, output, Paused source, prompt, PIE running" | M1. T-VIEW-017: "Paused source view; prompt mode with a fake handler | Read-only with the arrow; ..." | M1. §4.14: "| Paused-window source | §8 | Snapshot of the paused file plus a full lex; current-line arrow and breakpoints (U17) |". §4.16.1: "Repeated floating, as the output view, the Paused source view and the prompt, and with PIE running."

**Fix applied.** TDD §4.16.2 T-KEY-010. What it checks: "G5 variants: floating, output, Paused source, prompt, PIE running" → "G5 variants: floating, output, prompt, PIE running and (from M3) the Paused source view". Lands: "M1" → "M1 (M3: Paused source view)". TDD §4.14 T-VIEW-017. What it checks: "Paused source view; prompt mode with a fake handler" → "Prompt mode with a fake handler; (from M3) the Paused source view". Pass criterion: "Read-only with the arrow; Enter, ..." → "(from M3) the Paused source view is read-only with the arrow; Enter, ...". Lands: "M1" → "M1 (M3: Paused source view)". TDD §4.16.1 Fixture bullet: "Repeated floating, as the output view, the Paused source view and the prompt, and with PIE running." → "Repeated floating, as the output view and the prompt, (from M3) as the Paused source view, and with PIE running." T-GAP-008 Terms: add `Paused source` to the M3 list.

**Patch.** T-KEY-010 and T-VIEW-017 Paused source view from M3 with split Lands; §4.16.1 Fixture bullet; T-GAP-008 M3 term 'Paused source'

### Round 2 — UX and mockups

Lens ux, round 2. I re-rendered all ten mockups from their HTML sources in scratch. Every PNG matches its source pixel for pixel. I also checked layout in a DOM pass: no callout or legend number is missing, misnumbered, overlapping or clipped. Every round-1 change is visible in images 01, 02, 04, 06, 07 and 10.

Eight round-1 patches check out: V-ux-03 to V-ux-10. The V-ux-09 goldens were re-run on the engine's python -I -B and hold.

Two patches have real problems:
- V-ux-01 (major): the new rename design rests on a wrong engine fact. Inside Sumi's open transaction, UObject::Rename calls Modify(), and the transacting load restores the old name and outer. So Undo this run, Undo Last Run, or the editor's own Ctrl+Z would move the in-memory asset back while its packages, redirector and registry stay moved. T-TXN-015 cannot pass as written, and images 01, 05 and 07 offer that undo.
- V-ux-02 (major): the patch is incomplete. The page as designed also matches the consume list, which shadows about 30 more mapped Level Editor commands (H, K, M, T, /, the Shift+letter surface-selection keys, Shift+1 to Shift+4, Shift+End, Ctrl+End, Ctrl+Shift+PageUp/PageDown, Ctrl+X). The fixed rows omit them, so T-CFG-011 still fails. The PyCharm list also misses F2 (Rename), and mockup 10's Preset column contradicts the new PyCharm list.

Smaller findings:
- The T-CFG-011 Lands value is not split, although the row names an M2 step and M4 presets.
- RUN-07, OUT-02 and the new callout 2 say the run block reports file writes, but there is no design for detecting them.
- Image 01's Run #14 shows rename_asset raising an exception. The engine only logs an error and returns False.
- The lexer pre-check's "ends with :" counts a colon inside a trailing comment. On the engine interpreter, `if x: pass  # note:` is complete, which breaks T-REPL-002's zero-tolerance property.
- TDD 6.4 has no field for the never-undoable notes.
- SPEC 9.3's definition of "proposed" is contradicted by the Ctrl+Shift+K and Ctrl+M rows.
- No test covers the new pending (recovered)-tab bars.

Scratch evidence is in scratchpad/verify3/: allchords.txt, mapped.txt, probe/repl_probe.py, meas/*.dom.txt and render/images.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [W-ux-01](#w-ux-01) | major | wrong engine fact | modified | The round-1 text says an asset rename is not transacted, and that 'Undo this run reverts only the recorded edits' while the asset keeps its new name. |
| [W-ux-02](#w-ux-02) | major | patch incomplete | confirmed | RV-19, CFG-06 and TDD 4.9.4 say the page is computed by matching Sumi's chords and the D8 consume list against the main-frame and Level Editor command lists. |
| [W-ux-03](#w-ux-03) | minor | contradiction | confirmed | The new PyCharm golden list has two problems. |
| [W-ux-04](#w-ux-04) | minor | lands | confirmed | The V-ux-02 patch made T-CFG-011's pass criterion name a later step ('Ctrl+Shift+K from M2'). |
| [W-ux-05](#w-ux-05) | minor | contradiction | confirmed | RUN-07 requires the run block to say that file writes are never undoable. |
| [W-ux-06](#w-ux-06) | minor | mockup | modified | The flagship error flow, which V-ux-01 edited, shows `unreal.EditorAssetLibrary.rename_asset` raising 'Exception: rename_asset: Failed to find source asset ...' at line 14, preceded by 'LogAssetTools: Warning: … |
| [W-ux-07](#w-ux-07) | minor | untestable | confirmed | The pre-check calls a last line 'definitely incomplete' when it starts with an opening keyword and 'ends with `:`'. |
| [W-ux-08](#w-ux-08) | minor | contradiction | modified | §5.9.2 says the never-undoable notes, now including V-ux-01's rename note, are added to 'the header'. |
| [W-ux-09](#w-ux-09) | minor | contradiction | confirmed | The §9.3 legend says a 'Proposed' chord was 'checked free outside tool-local bindings'. |
| [W-ux-10](#w-ux-10) | minor | untestable | confirmed | V-ux-05 gave the conflicting `(recovered)` tab a new pending behaviour: it is read-only under 'Not restored yet' with [Restore] [Compare with disk] [Discard]. |
| [W-ux-11](#w-ux-11) | minor | wrong engine fact | added by refuter | §5.9.3 treats any `TransactionCanceled` for Sumi's transaction as Purged only when a map load happened, and as CancelledByScript ('a user cancel() on a nested scope') otherwise. |

#### W-ux-01

**Major, wrong engine fact.** Verdict: modified.

**Where:** TDD §5.9.2 'Renames and moves' bullet; TDD §5.9.5 Undo Last Run check table; TDD §5.15.5 T-TXN-015; TDD §14.5 T-E2E-002 parenthetical; SPEC §7.4 RUN-07; SPEC §5.2 J2; SPEC §7.13 SNP-10; mockups/01-main-window.html Run #14 header, info line 139, Undo card line 176, callouts 2 and 7 (images/01); mockups/07-snippets-and-shelves.html Run #42 header line 354, note line 356, callouts 5 and 10 (images/07); mockups/05-debugger-paused.html Evaluate note line 212 and callout 8 (images/05)

**Problem.** The round-1 text says an asset rename is not transacted, and that 'Undo this run reverts only the recorded edits' while the asset keeps its new name. In fact, Sumi's run transaction does record the rename of the in-memory object.
- Shelf, snippet and buffer runs have the transaction on (RUN-07), and loaded assets are RF_Transactional.
- So rename_asset inside a run saves the asset's old name and outer into Sumi's transaction, and saves each referencer whose soft path is fixed up.
- Undoing that record (Undo this run, Undo Last Run, the task walk-back, or the editor's own Ctrl+Z/Edit > Undo) LowLevelRenames the object back into its old package and renames the colliding redirector aside. The new package on disk, the redirector, the asset registry and the saved referencers all stay moved. That split state becomes permanent on the next save.
- T-TXN-015's Expected ('Undo this run reverts the lod_group edit while the asset keeps its new name') cannot hold.
- T-E2E-002's reason ('undo restores none of that') is wrong.
- Images 01 and 07 offer this undo on rename runs, image 01's Undo card still shows the rename run as the top undo entry, image 05 promises 'Undo Last Run reverts them with the rest of Run #14' for a debug run that calls rename_asset, and SNP-10's failure notification offers Undo this run for such runs.

**Evidence.** TDD §5.9.2: "the package move, the redirector and the referencer fix-ups are not transacted (`Engine/Source/Developer/AssetTools/Private/AssetRenameManager.cpp` opens no transaction) ... If any fired, the header adds \"asset renames and moves in this run are not undoable\"; **Undo this run** reverts only the recorded edits."

T-TXN-015 Expected: "Undo this run reverts the `lod_group` edit while the asset keeps its new name".

Engine source:
- Obj.cpp:399 `if (!(Flags & REN_NonTransactional))` then Modify(), so the rename is recorded.
- ObjectTools.cpp:4635 `GEditor->RenameObject( Object, NewPackage, *ObjName, bLeaveRedirector ? REN_None : REN_DontCreateRedirectors )`, so no REN_NonTransactional.
- EditorObject.cpp:114-116 calls Object->Rename.
- UObjectGlobals.cpp:3207-3230 SaveToTransactionBuffer records when GUndo && RF_Transactional.
- TransBuffer.h:131 `GUndo = &UndoBuffer.Last().Get();` in BeginInternal.
- ObjectMacros.h:643 RF_Load includes RF_Transactional.
- Obj.cpp:1787 "Special support for supporting undo/redo of renaming"; :1819 Collision->LowLevelRename; :1831 LowLevelRename(LoadName,LoadOuter).
- AssetRenameManager.cpp:672 `CurrentObject->Modify(true)` on soft-path referencers.
- The path is EditorAssetSubsystem.cpp:1018-1057 → IAssetTools::RenameAssets → RenameSingleObject; nothing on it resets or suppresses the transaction.
- The engine itself resets the undo buffer after non-undoable asset deletes (ObjectTools.cpp:2548, 3505).

**Refuter.** The engine fact is real; I checked it in the local 5.8 source.
- `UObject::Rename` calls `Modify()` unless `REN_NonTransactional` (Obj.cpp:399).
- `RenameSingleObject` calls `GEditor->RenameObject(..., REN_None or REN_DontCreateRedirectors)` (ObjectTools.cpp:4635), which calls `Object->Rename` (EditorObject.cpp:115).
- `SaveToTransactionBuffer` records when `GUndo` is set and the object is RF_Transactional (UObjectGlobals.cpp:3207-3230).
- The soft-path fix-up calls `Modify(true)` (AssetRenameManager.cpp:672).
- Transactional serialization LowLevelRenames the object back to LoadName/LoadOuter and renames a colliding object aside (Obj.cpp:1787-1831).
- `AssetRenameManager` also saves the renamed packages and redirectors itself (:1992-2006).
- Nothing on the `rename_asset` path resets or suppresses the transaction (EditorAssetSubsystem.cpp:1018-1083).

So T-TXN-015 ('asset keeps its new name') cannot pass, and Undo this run or Edit > Undo splits the object from its package. No 08 ruling covers renames (RV-20 is about reimports).

The verifier's direction (ResetTransaction, as the engine does after deletes) is sound, but the fix is incomplete:
- It would reset even when the run had no transaction (Run File), which needlessly wipes history.
- Its walk-back sentence is wrong: the reset clears earlier steps; it does not just 'end' the walk-back.
- It adds no status value for the record.
- T-TXN-015 must run the rename case last, because the reset clears the other cases' records.
- The mockup 05 Evaluate note needs only a word change.

**Fix applied.** (1) TDD §5.9.2, the **Renames and moves.** bullet.
Replace "the package move, the redirector and the referencer fix-ups are not transacted (`Engine/Source/Developer/AssetTools/Private/AssetRenameManager.cpp` opens no transaction)." with:
"the packages on disk, the redirector, the asset registry and the saved referencers are not transacted: `Engine/Source/Developer/AssetTools/Private/AssetRenameManager.cpp` opens no transaction and saves the renamed packages itself (`:1992-2006`). Inside an open transaction, however, the engine records the renamed object. `UObject::Rename` calls `Modify()` unless `REN_NonTransactional` (`Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp:399`), `ObjectTools::RenameSingleObject` passes `REN_None` or `REN_DontCreateRedirectors` (`ObjectTools.cpp:4635`), and the soft-path fix-up calls `Modify(true)` on each referencer (`AssetRenameManager.cpp:672`). Undoing that record renames the object back to its old name and outer (`Obj.cpp:1787-1831`) while its packages, the redirector and the registry stay moved."
In the same bullet, replace "If any fired, the header adds \"asset renames and moves in this run are not undoable\"; **Undo this run** reverts only the recorded edits." with:
"If any fired, the header adds \"asset renames and moves in this run are not undoable\". If the run or step had a transaction (status not Off), Sumi sets its status to Cleared and, after End, calls `GEditor->ResetTransaction(LOCTEXT(\"SumiAssetRename\", \"Sumi: assets renamed\"))` (`.../Classes/Editor/EditorEngine.h:1067`), as the engine does after asset deletes (`ObjectTools.cpp:2548, 3505`). Neither Undo this run nor Edit ▸ Undo can then split an asset from its package. The note becomes \"asset renames and moves in this run are not undoable · undo history cleared\", and no Undo this run is offered. The reset also clears a task's earlier steps, so the walk-back reaches only steps after the last renaming step."

(2) TDD §5.9.5 table: insert a first row "| 0 | Status is not Cleared | \"Run #22 renamed or moved assets, so the undo history was cleared.\" |".

(3) TDD §5.9.7: replace "enum class ESumiTxnStatus : uint8 { Off, NotStarted, Open, Recorded, Empty, Partial, Purged, CancelledByScript, Undone };" with "enum class ESumiTxnStatus : uint8 { Off, NotStarted, Open, Recorded, Empty, Partial, Purged, CancelledByScript, Cleared, Undone }; // Cleared: reset after an asset rename (§5.9.2)".

(4) TDD §5.15.5 T-TXN-015.
- Scenario: replace "; and a run that renames a fixture static mesh" with "; and, run last, a run that renames a fixture static mesh".
- Expected: replace "a run without an import shows no note; that run's header says \"asset renames and moves in this run are not undoable\", and Undo this run reverts the `lod_group` edit while the asset keeps its new name" with "a run without an import shows no import note; the rename run says \"asset renames and moves in this run are not undoable · undo history cleared\", offers no Undo this run, and Undo Last Run refuses it with check 0's text; `GetQueueLength()` is 0 after it; after a save and a reload the mesh keeps its new path and its `lod_group`".

(5) TDD §14.5 T-E2E-002: replace "because an asset rename moves the asset to a new package, leaves a redirector and fixes its referencers, and undo restores none of that, RUN-07" with "because a run that renames assets cannot be undone and clears the undo history, §5.9.2, RUN-07".

(6) SPEC.
- RUN-07: replace "(the package move, redirector and referencer fix-ups are not transacted)" with "(packages, redirector and registry are not transacted; a run in a transaction that renamed assets clears the editor's undo history, TDD §5.9.2)".
- J2: replace "(property edits undo; asset renames and moves never do, RUN-07)" with "(property edits undo; a run that renamed or moved assets cannot be undone and clears the undo history, RUN-07)".
- SNP-10: replace "and [Undo this run]." with "and [Undo this run] (absent when the run renamed or moved assets, RUN-07)."

(7) mockups/01-main-window.html.
- Line 138: replace "· undo “Sumi: batch_rename.py #14”</span><span class=\"act\">Undo this run</span>" with "· not undoable</span>".
- Line 139: replace "· Undo this run reverts only the recorded edits" with "· undo history cleared".
- Line 176: replace "<code>Sumi: batch_rename.py #14</code><br><span class=\"lo\">Undo Last Run · Ctrl+Alt+Z in Sumi · 1.2 MB record</span>" with "<code>empty</code><br><span class=\"lo\">cleared after Run #14 renamed assets</span>".
- Callout 2 (line 215): after "(RUN-07);", insert " a run that renames assets also clears the editor's undo history, as Run #14 did;".
- Callout 7 (line 220): replace "the header offers Undo this run." with "the header offers Undo this run unless the run renamed assets."

(8) mockups/07-snippets-and-shelves.html.
- Line 354: replace "· ok · undo “Sumi: Batch Rename #42”" with "· ok · not undoable".
- Line 356: replace the info text with "asset renames and moves in this run are not undoable · undo history cleared".
- Callout 5 (line 492): replace "its asset renames are never undoable, and the block says so (RUN-07)." with "because it renamed assets, Sumi clears the undo history when it ends, and the block says so (RUN-07)."
- Callout 10 (line 497): replace "plus Details and Undo this run." with "plus Details and Undo this run (not for a run that renamed assets)."

(9) mockups/05-debugger-paused.html.
- Line 212: replace "changes join Run #14's undo step" with "changes join Run #14's transaction".
- Callout 8 (line 294): replace "and those edits land in the run's undo step, so Undo Last Run reverts them with the rest of Run #14." with "and those edits land in the run's transaction and share its fate: Run #14 renames assets, so its undo history is cleared when it ends (RUN-07)."

(10) Re-render images 01, 05 and 07 with mockups/render.ps1.

**Patch.** TDD §5.9.2 Renames bullet (engine records the rename; Cleared status and ResetTransaction), §5.9.5 check 0, §5.9.7 Cleared enum value, T-TXN-015 (rename run last, new expected), T-E2E-002 reason; SPEC RUN-07, J2, SNP-10 and the §9.5 image-01 description; mockup 01 Run #14 header, info line, Undo card, callouts 2 and 7; mockup 07 Run #42 header, note, callouts 5 and 10; mockup 05 Evaluate note and callout 8, plus callout 2 'with no rollback' (it said 'undo step kept' for the same renaming run); D.3 row

#### W-ux-02

**Major, patch incomplete.** Verdict: confirmed.

**Where:** TDD §11.1.4 Shadowed Unreal shortcuts page, 'Fixed rows' bullet; TDD §4.9.4 bullet 'The "Shadowed Unreal shortcuts" page'; TDD §11.12 T-CFG-011; SPEC §9.3 'Text-editing keys (consume list, CFG-07)' row; mockups/10-settings-and-trust.html line 388 Consume list row (images/10)

**Problem.** RV-19, CFG-06 and TDD 4.9.4 say the page is computed by matching Sumi's chords and the D8 consume list against the main-frame and Level Editor command lists. V-ux-02 added Ctrl+Shift+K, the extension rows and Ctrl+Shift+A. But the consume list (every printable key; Home/End and PgUp/PgDn with Shift and Ctrl; Ctrl+X) also shadows about 30 more Level Editor commands that are mapped in the global Level Editor list. The fixed rows name only Ctrl+Z/Y, Ctrl+C/V/A, Ctrl+D, Ctrl+S, Ctrl+W, Delete, Backspace and End. A page built as specified therefore still has more rows than T-CFG-011's 'golden list, fixed rows and extension rows', so the G5 test fails. SPEC 9.3 and image 10 under-report shadows that CFG-06 says are all listed.

**Evidence.** TDD §11.1.4: "Fixed rows: Esc (run-time Stop), and the consume list Ctrl+Z/Y, Ctrl+C/V/A, Ctrl+D, Ctrl+S, Ctrl+W, Delete, Backspace, End."

LevelEditorActions.cpp chords (mapping line in LevelEditor.cpp):
- H Toggle Selected Hierarchy Visibility :4589 (mapped :1397)
- Shift+H Show Selected Hierarchy :4587 (:1385)
- K Keep Simulation Changes :4647 (:1755)
- M Make Selected Actor's Level Current :4649 (:1762)
- T Allow Translucent Selection :4675 (:1942)
- / and Shift+/ particle resets :4707-4708 (:2097)
- Shift+V Toggle VR :4667 (:977)
- Shift+G Ungroup :4571 (:1331)
- Shift+E :4608 (:1490)
- Shift+S/B/T/J/C/W/U/Y/Q/M/R/O/A/X surface selection :4618-4632 (:1547 ...)
- Shift+1..4 modes :4695-4698 (:2048)
- Shift+End :4503 (:1226)
- Ctrl+End Snap Origin to Grid :4486 (:1131)
- Ctrl+Shift+PageUp/PageDown :4493-4494 (:1177, :1182)
- Ctrl+X FGenericCommands::Cut (GenericCommands.cpp:11; LevelEditor.cpp:1107)

TDD §4.9.4 consume list: "Every printable and AltGr key; Tab, arrows, Home/End, PgUp/PgDn ...; ... Ctrl+X ...". Primitives: "arrows, Home/End and PgUp/PgDn (with Shift and Ctrl)".

Scan output: scratchpad/verify3/mapped.txt.

**Refuter.** §4.9.4 and §11.1.4 have ComputeShadowed match the D8 consume list (every printable key; Home/End and PgUp/PgDn with Shift and Ctrl; Ctrl+X) against the main-frame and Level Editor lists, and RV-19 and CFG-06 require this.

The local 5.8 source maps these in the global Level Editor list:
- H, Shift+H, K, M, T, / and Shift+/;
- Shift+V, Shift+G, Shift+E, Shift+1 to Shift+4 and the Shift+letter surface selections;
- Shift+End, Ctrl+End and Ctrl+Shift+PageUp/PageDown;
- Ctrl+X (FGenericCommands::Cut at LevelEditor.cpp:1107).

Ctrl+Shift+Z is Redo's second chord, mapped in MainFrameActions.cpp:186.

The fixed rows list only Ctrl+Z/Y, Ctrl+C/V/A, Ctrl+D, Ctrl+S, Ctrl+W, Delete, Backspace and End. So a page built to the spec has rows that T-CFG-011's equality check does not expect, and that G5 test fails. Alt+End and Alt+PageUp/Down are correctly left out, because Alt chords are not consumed. The verifier's fix enumerates the missing chords and adds the grouping rule.

**Fix applied.** Apply the verifier's fix as written: TDD §11.1.4 Fixed rows, TDD §4.9.4 bullet, SPEC §9.3 text-editing row, mockups/10 line 388 plus the new Typed characters row, then re-render images/10.

**Patch.** TDD §11.1.4 Fixed rows enumerate the extra consume-list chords and a Typed characters row; §4.9.4 bullet; SPEC §9.3 text-editing row; mockup 10 Consume list row extended (shortened description) and new Typed characters row; canvas 1600x1880 to 1600x2032

#### W-ux-03

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §11.1.4 golden-list bullet (PyCharm list); SPEC §9.3 'Next / previous problem' row; mockups/10-settings-and-trust.html lines 377, 380, 381, 383, 389 (images/10 Shadowed page)

**Problem.** The new PyCharm golden list has two problems.
- It omits F2. PyCharm's Next problem is F2 †, which shadows FGenericCommands::Rename, mapped in the global Level Editor list.
- Image 10's Preset column contradicts it. The F10/F11, Shift+F11 and Ctrl+H rows say 'all', but the list says PyCharm lacks F10, F11 and Shift+F11, and moves Ctrl+H to the extension. The F8 row says 'Next problem · all', but in PyCharm F8 is Step over. The Extension row ('all') lists Ctrl+Shift+A as consumed with no action, while two rows lower the same chord is PyCharm's bound Palette.

**Evidence.** TDD §11.1.4: "PyCharm, for §9.3 as written: the Charcoal list without F10, F11 and Shift+F11 ..., plus Ctrl+B and the bound Ctrl+Shift+A (Palette); Ctrl+H moves to the extension rows".

SPEC §9.3: "Next / previous problem | **F8 / Shift+F8** (U13) | F2 / Shift+F2 † | | F8: PIE Possess or Eject Player".

Engine: GenericCommands.cpp:24 `UI_COMMAND(Rename, ... FInputChord(EKeys::F2))`, mapped at LevelEditor.cpp:1101 inside BindGlobalLevelEditorCommands (:943).

mockup 10 line 380: `<span class="kc">F10</span><span class="kc">F11</span>...<span class="pr2">all</span>`; line 389: "... Ctrl+Shift+A Select All Actors of Same Class ...</span><span>Consumed, no action</span><span class="pr2">all</span>"; line 391: "Ctrl+Shift+A ... Palette ... PyCharm".

**Refuter.** F2 is FGenericCommands::Rename (GenericCommands.cpp:24). It is mapped at LevelEditor.cpp:1101 inside BindGlobalLevelEditorCommands (:943), and PyCharm's Next problem is F2 † in SPEC §9.3, so the PyCharm golden list misses it.

Image 10 marks three rows 'all' although §11.1.4 says PyCharm lacks them: F10/F11 (line 380), Shift+F11 (line 381) and Ctrl+H as Replace (line 377). F8 is labelled Next problem for all presets, but in PyCharm it is Step over. The Extension row ('all') lists Ctrl+Shift+A as consumed with no action, while line 391 makes it PyCharm's Palette. VS Code keeps the Charcoal chords for these rows, so 'Charcoal · VS Code' is right.

**Fix applied.** Apply the verifier's fix as written: TDD §11.1.4 F2 insertion, SPEC §9.3 Next/previous problem last cell, mockups/10 lines 377/380/381/383/389 and the new F2 row, then re-render images/10.

**Patch.** TDD §11.1.4 PyCharm list adds F2 (Rename); SPEC §9.3 Next/previous problem row; mockup 10 Preset cells 'Charcoal · VS Code' for Ctrl+H, F10/F11 and Shift+F11, F8 'Next problem (PyCharm: Step over)', Extension row Ctrl+H for PyCharm, new F2 PyCharm row

#### W-ux-04

**Minor, lands.** Verdict: confirmed.

**Where:** TDD §11.12 T-CFG-011 Lands cell

**Problem.** The V-ux-02 patch made T-CFG-011's pass criterion name a later step ('Ctrl+Shift+K from M2'). It also checks the PyCharm and VS Code preset lists, which land in M4. But Lands stayed a plain 'M1'. Under TDD 1.5 a named later step needs a split value, or the M2 and M4 exits never require those parts. V-ux-07 applied exactly this pattern to T-UEI-002.

**Evidence.** T-CFG-011: "Rows equal the golden list, the fixed rows and the extension rows of §11.1.4, per preset and milestone (Ctrl+Shift+K from M2) ... | U23, D25, G5, CFG-06, RV-19 | M1 |".

TDD 1.5: "a milestone the row itself names, such as \"(M2)\", \"From M2:\" ... A split value such as `M1 (M4: trust step)` means the test runs from M1 and the named step joins at M4; the Mx exit requires ... the named step of every split value that names Mx".

TDD 4.9.4: "**Presets** (PyCharm, VS Code) are in §11 (M4)."

**Refuter.** T-CFG-011's pass criterion names 'Ctrl+Shift+K from M2' and checks per-preset rows, and §4.9.4 says the presets land in M4. TDD §1.5 requires a split Lands value when a row names a later milestone, and says the Mx exit requires the named step of every split that names Mx. With Lands a plain 'M1', the M2 and M4 parts are never required at those exits. V-ux-07 applied the same pattern to T-UEI-002.

**Fix applied.** TDD §11.12 T-CFG-011, Lands cell: replace "M1" with "M1 (M2: Ctrl+Shift+K row; M4: PyCharm and VS Code preset rows)".

**Patch.** T-CFG-011 Lands 'M1 (M2: Ctrl+Shift+K row; M4: PyCharm and VS Code preset rows)'

#### W-ux-05

**Minor, contradiction.** Verdict: confirmed.

**Where:** SPEC §7.4 RUN-07; SPEC §7.6 OUT-02 example footer; mockups/01-main-window.html callout 2 legend (line 215, images/01); TDD §5.9.2 End checks

**Problem.** RUN-07 requires the run block to say that file writes are never undoable. OUT-02's example footer says it too, and V-ux-01's new callout 2 repeats it ('file writes and PIE edits are never undoable, and the run block says so'). TDD 5.9.2 has detectors only for PIE objects, imports and reimports, and renames. Nothing in the TDD detects file writes or package saves, so a builder cannot produce that note.

**Evidence.** SPEC RUN-07: "Imports and reimports, asset renames and moves (...), file writes and PIE edits are never undoable, and the block says so."

OUT-02: "transaction kept (imports and file writes are not undoable)".

mockup 01 line 215: "imports, asset renames and moves, file writes and PIE edits are never undoable, and the run block says so (RUN-07)".

TDD 'not undoable' notes exist only at §5.9.2 lines 2882-2884 (PIE, Imports, Renames). There is no audit-hook or save-event detector anywhere in the TDD.

**Refuter.** SPEC RUN-07 says 'file writes and PIE edits are never undoable, and the block says so' (unchanged since v1.0), and OUT-02's example footer says 'imports and file writes are not undoable'. The round-1 callout 2 in mockup 01 repeats the claim. But TDD §5.9.2 has detectors only for PIE objects, imports and reimports, and renames. No audit hook or package-save watcher exists anywhere in the TDD, so the note cannot be built. 08 D8 says only that file operations are never undoable; it does not require a note.

**Fix applied.** Apply the verifier's primary fix: reword RUN-07 and OUT-02 and mockups/01 callout 2 as given, then re-render images/01. If W-ux-01's callout-2 insertion is also applied, insert it after the "(RUN-07);" that ends this new wording.

**Patch.** SPEC RUN-07 (file writes not detected), OUT-02 example footer (imports and reimports); mockup 01 callout 2; D.3 row

#### W-ux-06

**Minor, mockup.** Verdict: modified.

**Where:** mockups/01-main-window.html Run #14 block lines 140-143 and cell [2] line 14 (images/01); mockups/06-crash-recovery.html panel B 'Before run #14' snapshot line 14 (images/06)

**Problem.** The flagship error flow, which V-ux-01 edited, shows `unreal.EditorAssetLibrary.rename_asset` raising 'Exception: rename_asset: Failed to find source asset ...' at line 14, preceded by 'LogAssetTools: Warning: SM_Rock_04 already exists, skipped'. In 5.8, rename_asset only logs LogEditorAssetSubsystem errors and returns False. The plugin raises Python exceptions only for FFrame::KismetExecutionMessage script errors. As drawn, this run would end ok with two error log lines and no traceback, so the image's traceback, runtime Problems item and red gutter mark cannot happen.

**Evidence.** EditorAssetLibrary.cpp:387-391 forwards to UEditorAssetSubsystem::RenameAsset.

EditorAssetSubsystem.cpp:1080: `UE_LOGF(LogEditorAssetSubsystem, Error, "RenameAsset failed: Could not find the source asset. %ls", ...); return false;`, and :1043 "RenameAsset failed: The destination path '%ls' already exists."

PyUtil.cpp:619 FScopedScriptExceptionHandler converts only script exceptions (KismetExecutionMessage).

mockup 01 line 143: `<div class="err">Exception: rename_asset: Failed to find source asset '/Game/Props/Rock_09'</div>`.

**Refuter.** The engine fact is right.
- `rename_asset` forwards to `UEditorAssetSubsystem::RenameAsset` (EditorAssetLibrary.cpp:387-391), which logs LogEditorAssetSubsystem errors and returns false (EditorAssetSubsystem.cpp:1043, 1080).
- The plugin turns only script exceptions into Python errors (PyUtil.cpp:619).
- Nothing on the path calls KismetExecutionMessage.

So the drawn traceback cannot happen. The proposed fix has two problems:
- Its one-line `if not ...: raise RuntimeError(f"...")` is about 150 characters. Image 01's editor fits about 114 before the minimap, so the line would clip.
- It keeps two failures before the traceback (SM_Rock_04 'already exists', then Rock_09 'not found'). With raise-on-False the first failure ends the run, so the second can never print.

The Variables panel shows the run stopped at data=Rock_09 with new_path='/Game/Props/SM_Rock_09'. The single failure should therefore be Rock_09's destination already existing.

**Fix applied.** mockups/01-main-window.html.
- Line 110: replace `<div class="ran">            unreal.<span class="t">EditorAssetLibrary</span>.<span class="f">rename_asset</span>(<span class="t">str</span>(data.package_name), new_path)</div>` with `<div class="ran">            <span class="c">assert</span> unreal.<span class="t">EditorAssetLibrary</span>.<span class="f">rename_asset</span>(<span class="t">str</span>(data.package_name), new_path), new_path</div>`. This is 103 characters, keeps line 14, and fits.
- Line 140: replace `<div class="warn">LogAssetTools: Warning: SM_Rock_04 already exists, skipped</div>` with `<div class="err">LogEditorAssetSubsystem: Error: RenameAsset failed: The destination path '/Game/Props/SM_Rock_09' already exists.</div>`.
- Line 143: replace `<div class="err">Exception: rename_asset: Failed to find source asset '/Game/Props/Rock_09'</div>` with `<div class="err">AssertionError: /Game/Props/SM_Rock_09</div>`.
- Keep the traceback File line (line 14), the gutter mark and callout 7.
- Re-render images/01.

mockups/06 is optional. Its 'Before run #14' snapshot is not shown failing. If it is changed, make the left (del) line 14 the same assert text with the `xl` highlight on `assert unreal.EditorAssetLibrary.rename_asset(`, re-render images/06, and check that the side-by-side line does not clip.

**Patch.** Mockup 01 line 14 is now an assert on rename_asset's result; output shows the LogEditorAssetSubsystem destination-exists error and 'AssertionError: /Game/Props/SM_Rock_09'; canvas 1600x1358 to 1600x1376

#### W-ux-07

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD §6.5 'Lexer pre-check' bullet; TDD §6.8 T-REPL-002

**Problem.** The pre-check calls a last line 'definitely incomplete' when it starts with an opening keyword and 'ends with `:`'. Read literally, that includes a colon inside a trailing comment. On the engine interpreter, `if x: pass  # note:` and `while x: y = 1  # :` are complete in classify_input. So a generator that emits trailing comments breaks T-REPL-002's patched zero-tolerance property ('no input the pre-check calls "definitely incomplete" is `complete`'). The user would also get a newline instead of a run.

**Evidence.** TDD §6.5: "a last line that starts with an opening compound-statement keyword (`if`, ...) and ends with `:`".

Probe scratchpad/verify3/probe/repl_probe.py on the engine python -I -B: 'if x: pass  # note:' complete; 'while x: y = 1  # :' complete. The goldens 'else:' invalid, 'if x:\n    pass\nelse:' incomplete and 'x = (1 2,' invalid were confirmed.

**Refuter.** I re-ran scratchpad/verify3/probe/repl_probe.py on the engine python with -I -B. `if x: pass  # note:` and `while x: y = 1  # :` are `complete` in classify_input. Both start with an opening keyword and end textually with ':', so a literal reading of §6.5 calls them 'definitely incomplete' and breaks T-REPL-002's zero-tolerance property. The three goldens added in round 1 still hold (else: invalid; if/else: incomplete; x = (1 2, invalid). A token-level rule that ignores trailing comments removes the counterexamples: brackets and strings are handled by the other clauses.

**Fix applied.** Apply the verifier's fix as written: in TDD §6.5, "and ends with `:`" becomes "and whose last token, ignoring a trailing comment, is `:`"; T-REPL-002's generator includes trailing comments that contain `:`; T-REPL-001 adds the golden "`if x: pass  # note:` complete".

**Patch.** TDD §6.5 pre-check rule uses the last token ignoring a trailing comment; T-REPL-002 generator includes colon comments; T-REPL-001 golden 'if x: pass  # note:' complete

#### W-ux-08

**Minor, contradiction.** Verdict: modified.

**Where:** TDD §6.4 'Design: header and footer' (header fields list and footer table, Transaction row); TDD §5.9.2 Transient / Imports / Renames bullets

**Problem.** §5.9.2 says the never-undoable notes, now including V-ux-01's rename note, are added to 'the header'. Every mockup draws them as a line directly under the header. But §6.4, which defines what the block renders from FSumiRunRecord, has no notes field in the header list, and its Transaction footer field lists other values only. A builder of the output view has no specified place or record field for these notes.

**Evidence.** TDD §6.4: "**Header fields:** chevron; run number; route badge ...; source and range; namespace; start time. On the right: the live state (U2), then outcome and duration."

Footer: "Transaction (U3, RT-08) | `undo: \"Sumi: <tab> #N\" (k objects, size)`; `partly undoable ...`; `no transaction (Run File)`; `undo unavailable: <reason>` (D8)".

§5.9.2: "the header adds \"asset renames and moves in this run are not undoable\"".

**Refuter.** The gap is real.
- §6.4 renders the header and footer from FSumiRunRecord, and its header-field list and footer table have no place for the §5.9.2 notes.
- The footer 'Notes' row lists other notes.
- §5.9.7's FSumiTxnHandle (the record's 'transaction info') has no fields to carry them.

The proposed bullet says they come from 'FSumiRunRecord flags' without defining any. It also includes a cross-reference ('with W-ux-01') that a writer cannot apply as text. The fix must add the fields and give literal text.

**Fix applied.** (1) TDD §6.4, after the "- **Header fields:** ..." bullet, add:
"- **Transaction notes** (§5.9.2): one muted line directly under the header, with one clause per flag joined by \" · \" in this order: \"changes to PIE objects are not undoable\", \"imports and reimports in this run are not undoable\", \"asset renames and moves in this run are not undoable\". When §5.9.2 reset the undo buffer, the line ends \" · undo history cleared\". The flags are `FSumiTxnHandle::bPieObjects`, `bImported` and `bRenamed` in the run record's transaction info (§5.9.7), set by `EndRunTransaction`. For a task, a flag set by any step is set on the record."

(2) TDD §5.9.7: replace "SIZE_T LastSize = 0; SIZE_T Budget = 0; bool bWarned50 = false; };" with "SIZE_T LastSize = 0; SIZE_T Budget = 0; bool bWarned50 = false;
                        bool bPieObjects = false; bool bImported = false; bool bRenamed = false; }; // §5.9.2 notes".

**Patch.** TDD §6.4 Transaction notes bullet; §5.9.7 FSumiTxnHandle gains bPieObjects, bImported, bRenamed

#### W-ux-09

**Minor, contradiction.** Verdict: confirmed.

**Where:** SPEC §9.3 legend paragraph; SPEC §9.3 'Move line up / down; delete line' row

**Problem.** The §9.3 legend says a 'Proposed' chord was 'checked free outside tool-local bindings'. V-ux-02 kept Ctrl+Shift+K '(proposed)' while recording that it shadows the global Level Editor command EnableActorSnap, and Ctrl+M '(proposed)' shadows Move Selection to Current Level. The legend now misdescribes those rows, and a reader would expect them free.

**Evidence.** SPEC §9.3: "\"Proposed\" = new in this SPEC, checked free outside tool-local bindings."

Row: "| Move line up / down; delete line | Alt+Up / Alt+Down; Ctrl+Shift+K (proposed) | | | Ctrl+Shift+K: Level Editor snap toggle \"Actor\" |".

LevelEditorActions.cpp:4689 EnableActorSnap Ctrl+Shift+K, mapped at LevelEditor.cpp:2027.

**Refuter.** The SPEC §9.3 legend says 'Proposed = new in this SPEC, checked free outside tool-local bindings'. Yet the Ctrl+Shift+K row is marked (proposed) and names the Level Editor EnableActorSnap it shadows (LevelEditorActions.cpp:4689, mapped), and the Ctrl+M row is marked (proposed) and shadows Move Selection to Current Level (4651/4653, mapped). Alt+J is consistent with the legend, since its conflict is viewport-local. The legend now misdescribes two rows.

**Fix applied.** Apply the verifier's fix as written: replace the SPEC §9.3 legend sentence "\"Proposed\" = new in this SPEC, checked free outside tool-local bindings." with "\"Proposed\" = new in this SPEC; the last column names any main-frame or Level Editor command it shadows (tool-local bindings never act while Sumi has focus)."

**Patch.** SPEC §9.3 legend sentence for 'Proposed'

#### W-ux-10

**Minor, untestable.** Verdict: confirmed.

**Where:** TDD §10.12 T-PER-028; TDD §10.7.2 Conflict rule

**Problem.** V-ux-05 gave the conflicting `(recovered)` tab a new pending behaviour: it is read-only under 'Not restored yet' with [Restore] [Compare with disk] [Discard]. Compare toggles to [Close diff]. Restore makes it dirty and editable, and its bar becomes [Compare with disk] [Save over <name>…]. No test checks any of this. T-PER-028 still checks only that the tab exists and the disk is unchanged, and T-PER-029 covers non-conflict pending buffers.

**Evidence.** T-PER-028: "Conflict: disk changed after the base version | `(recovered)` tab beside the disk version; disk unchanged | U30 | M1".

§10.7.2: "While pending (§10.7.1), the `(recovered)` tab is read-only under \"Not restored yet\", with the bar \"Changed on disk after the journal's base version [Restore] [Compare with disk] [Discard]\" ... Restore ... makes it dirty and editable, and its bar becomes [Compare with disk] [Save over <name>…]".

**Refuter.** §10.7.2, as patched by V-ux-05, gives the conflicting `(recovered)` tab a new pending state: read-only under 'Not restored yet', with Restore, Compare with disk and Discard, Compare toggling to Close diff, and after Restore a dirty, editable tab with Save over. T-PER-028 still checks only that the tab exists and that the disk is unchanged. T-PER-029 covers non-conflict counted buffers: its 'one Ctrl+Z gives disk text' cannot apply to a separate recovered tab. So nothing tests the new behaviour.

**Fix applied.** Apply the verifier's replacement of T-PER-028's pass criterion. Also change T-PER-028's Covers cell from "U30" to "U30, PER-03".

**Patch.** T-PER-028 pass criterion covers the pending, Compare and Restore states; Covers adds PER-03 (§14.7 regenerated)

#### W-ux-11

**Minor, wrong engine fact.** Raised by the refuter.

**Where:** TDD §5.9.3 Purged bullet; TDD §5.14 failure-mode row 'User `cancel()` on a nested transaction, or a map load'; TDD §5.15.5 tests (after T-TXN-006)

**Problem.** §5.9.3 treats any `TransactionCanceled` for Sumi's transaction as Purged only when a map load happened, and as CancelledByScript ('a user cancel() on a nested scope') otherwise. But the engine also resets the whole undo buffer during a run when the script deletes an asset that only the undo buffer still references, for example one the run edited first. `ObjectTools::CleanupAfterSuccessfulDelete` and the delete path call `GEditor->ResetTransaction("Delete Selected Item")`. `UTransBuffer::Reset` with an active transaction cancels it and empties the buffer. Such a run is labelled CancelledByScript, and the banner gives a false reason. The user is not told that the run's later edits went unrecorded, or that their earlier undo history is gone. This matters more now that W-ux-01 cites this same engine reset as precedent.

**Evidence.** - ObjectTools.cpp:2545-2549 ('only ref to this object is the transaction buffer, clear the transaction buffer' → `GEditor->ResetTransaction(NSLOCTEXT("UnrealEd","DeleteSelectedItem","Delete Selected Item"))`) and :3501-3506.
- The `delete_asset` path is EditorAssetSubsystem.cpp:769/792/814/864 → `ObjectTools::ForceDeleteObjects`.
- EditorTransaction.cpp:1379-1402: `if (ActiveCount != 0) { ... Cancel(0); } UndoBuffer.Empty(); ... ResetReason = Reason;`.
- :1427-1430: Cancel broadcasts `TransactionCanceled`.
- :1306-1308: End tolerates ActiveCount 0 'as we now purge the buffer when resetting'.
- :1490-1495: CanUndo then reports "(Can't undo after: {ResetReason})".
- TDD §5.9.3: "**Purged** if a map load happened during the run (`EditorTransaction.cpp:1382-1394`); **CancelledByScript** otherwise, for a user `cancel()` on a nested scope".

**Fix applied.** (1) TDD §5.9.3: replace "- **Purged** if a map load happened during the run (`EditorTransaction.cpp:1382-1394`);" with "- **Purged** if the undo buffer was reset while our transaction was open. At End, `GetQueueLength()` is below its value before Begin, or a map load (`FEditorDelegates::OnMapOpened`) or an asset delete (`FEditorDelegates::OnAssetsPreDelete`, `Engine/Source/Editor/UnrealEd/Public/Editor.h:378`) happened during the run. The engine resets on a map load and when a delete finds an object that only the undo buffer references (`ObjectTools.cpp:2548, 3505`). The reset cancels our transaction and empties the buffer (`EditorTransaction.cpp:1379-1402`), and edits after it are not recorded. The banner names the reason from `Trans->CanUndo(&Text)` (\"Can't undo after: <reason>\", `EditorTransaction.cpp:1490-1495`);"

(2) TDD §5.14 failure-mode row: replace "User `cancel()` on a nested transaction, or a map load" with "User `cancel()` on a nested transaction, a map load, or an asset delete that resets the undo buffer". Make the same change to Appendix A row A-062.

(3) Add a test after T-TXN-006: "| T-TXN-0xx | 4 | A run sets a property on a fixture asset, then deletes it with `unreal.EditorAssetLibrary.delete_asset` | Status Purged; the banner names \"Delete Selected Item\"; `GetQueueLength()` is 0 | D8 | M1 |".

**Patch.** TDD §5.9.3 Purged detects any undo-buffer reset (map load, asset delete, queue length) and names the reason; §5.14 failure row (Appendix A A-062 regenerated); new T-TXN-016 (delete_asset after an edit) after T-TXN-006

## Round 3

Scope: the round-2 patch only, from an exact before/after diff.

**Patch.** I applied all 22 confirmed issues and skipped none; check.py ends with PROBLEMS 0 {}. Where two issues edited the same text (X-persist-01/02/09/10 in the §10.1 bullets, X-persist-06 and X-delivery-03 in the M2 policy rows, X-ux-02/03 in the §5.9.2 Renames bullet), I applied them one after another without overlap. For the M2 TDD sections cell I used X-delivery-03's wording, and for the §11 feature-table row X-persist-06's; the content is the same. I made three small consistency additions beyond the approved text: §3.8 Module shutdown says the SumiStorageProbe thread is not joined; T-PER-055 says 'at the root where the second editor became primary' so it does not read as the unanswering-override root; and the Trust.json lock-failure message is shown by 'the trust banner or the Trusted folders page that asked for it', to match T-TRU-006. I added SPEC Appendix D.3 rows for X-persist-03 (PERF-01), X-ux-01 (§9.3) and X-ux-04 (§9.4). I re-rendered only images/10-settings-and-trust.png, and it is clean. Files changed: C:/Users/anima/Desktop/UnrealPythonEditor/docs/TDD.md, C:/Users/anima/Desktop/UnrealPythonEditor/docs/SPEC.md, C:/Users/anima/Desktop/UnrealPythonEditor/docs/mockups/10-settings-and-trust.html and C:/Users/anima/Desktop/UnrealPythonEditor/docs/images/10-settings-and-trust.png.

**Checks after the patch.** Before the first edit, I saved the pre-patch copies to scratchpad/verify4/patch/ (SPEC.before.md, TDD.before.md, 08.before.md, README.before.md, 07a.before.md, mockups.before/). Engine claims were re-checked read-only in UE_5.8: FLazyObjectPtr::IsPending, LaunchEngineLoop and ThreadingBase pool destroy, the editor-mode chords in LevelEditorModesActions, SLevelEditor and the commented-out LevelEditorActions lines, and the AssetRenameManager redirector and cleanup lines with the ObjectTools ResetTransaction. After the edits I searched SPEC, TDD, 08, 07a, README and the mockups for each changed contract. Searched: 'transaction kept', 'Shift+1', 'journal I/O only on the writer', 'instance lock and the restore guard', export-text helpers, 'bStopRequested' and the evaluate stop state, 'Sumi.G2', 'input bot', 'FSumiReloadOnSave', 'Trust.json.lock', 'mirror.lock', 'root_override'. No other contradicting text was found. No test rows were added and no Covers or Lands values changed. gen.py --write produced no change to TDD.md. LF endings hold in SPEC.md, TDD.md and README.md (0 CR), and the backslash count in TDD.md is unchanged. I re-rendered mockup 10 and viewed the PNG: the Typed characters row reads 'Shift+1–9' and nothing overlaps or clips. 08 and 07a were not edited. Final check.py output line: PROBLEMS 0 {}

### Round 3 — Runtime

Runtime lens, round 3. I read all 11 runtime entries in round2.json and the word-diffs of TDD, SPEC, 08, 07a and the mockups (the current docs are byte-identical to backup-r3). I checked the engine and debugpy claims against UE_5.8 source (SWindow.cpp:1863-1877 and 1896-1911, PropertyBaseObject.cpp:267-283, PropertyLazyObjectPtr.cpp:109-117, WindowsPlatformMisc.cpp:1076) and against the debugpy 1.8.20 bundle in the local VS Code extension (pydevd.py:577-579, pydevd_tracing.py:26-31/58-90/125-130, pydevd_constants.py:261, pydevd_file_utils.py:588-603). A new probe on the engine python.exe -I -B (verify4/rt/probe/p_listen.py, results in p_listen_env.json and p_listen_noenv.json) confirms three things. Without PYDEVD_DISABLE_FILE_VALIDATION, the in-process listen writes the four frozen-modules lines to stderr; with it set, listen writes nothing. sys.settrace becomes _internal_set_trace and warns for an outside caller. TracingFunctionHolder._warn = False silences both a module caller and bdb. The integrator check.py reports 0 problems. Two issues remain, both in the round-2 text. (1) W-runtime-01 (major): the patch now fixes how an evaluate is identified and attributed, but it brings in an "evaluate's stop state" that §5.7.2/§5.7.3 never define. Read literally, the run-level Stop (Shift+F5, Esc, Stop buttons) no longer reaches a hung evaluate. That contradicts SPEC DBG-03 "Stop works" and §5.6.3 "in every context", and no test covers the case. (2) W-runtime-11 (minor): the 'Lazy <guid>' rule was applied to prop_export_text only. container_export_text (T-INSP-014's lazy-object Array) and struct_export_text's scalar fields still export an unloaded lazy pointer as 'None'. All other round-2 runtime patches are correct and complete. I found no stale text in SPEC, 08, 07a or the mockups.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [X-runtime-01](#x-runtime-01) | major | patch incomplete | modified | The W-runtime-01 patch (item 6) makes the evaluate's cancellation separate from the paused run's: 'Cancel calls `FRunSupervisor::RequestStop(EvalId)`. |
| [X-runtime-02](#x-runtime-02) | minor | patch incomplete | modified | W-runtime-11 found that an unresolved FLazyObjectPtr exports as `None` through `FObjectPropertyBase::ExportText_Internal` (`Get()` with no load, then 'None'), which reads as a null reference. |

#### X-runtime-01

**Major, patch incomplete.** Verdict: modified.

**Where:** TDD §8.3.4 Supervised evaluates, Cancel bullet; TDD §5.7.6 'Run-ID guards' bullet; TDD §5.7.2 Per-run cancellation state; TDD §5.7.3 Delivery in the pending call (code block); TDD §5.6.3 / §5.6.2 Shift+F5 rows; TDD §12.1.1 row check_cancel; TDD §8.11 T-DBG-031; SPEC DBG-03

**Problem.** The W-runtime-01 patch (item 6) makes the evaluate's cancellation separate from the paused run's: 'Cancel calls `FRunSupervisor::RequestStop(EvalId)`. While `ActiveEvalId == EvalId`, that sets the evaluate's stop state and leaves the paused run's untouched'. §5.7.6 adds '`check_cancel()` on the game thread during an evaluate reports the evaluate's stop state.' But no section defines an evaluate stop state. §5.7.2 lists one set of atomics ('Atomics: `bStopRequested`, `StopSource`, `bUserCodeActive`, `bInterruptCleanup`, `Deliveries`. Times: ...'), and §5.7.3 delivers on `if (bStopRequested && bUserCodeActive)`. A builder who keeps the single state has the evaluate's Cancel set the run's `bStopRequested` and `Deliveries`. `eval_end` ('restores the run's entry mark and pump gate, clears the async exception and drains') does not reset them, so after Continue the run is re-delivered ScriptCancelled. That contradicts 'it never reaches the paused run'. A builder who follows the new text and keeps two states finds no rule for the run-level Stop during an evaluate. Shift+F5, Esc and the Stop buttons call `RequestStop` with the paused run's ID, because CurrentRunId now stays the run's ID (§5.6.5: 'RequestStop called from the pre-processor or a button only sets atomics'). That sets only the run's state, while every pending call during the evaluate carries ActiveEvalId and delivers from the evaluate's state. So a `while True: pass` evaluate cannot be stopped with Shift+F5. This contradicts §5.6.3 ('**Shift+F5** and the Stop buttons are the run-level Stop in every context') and SPEC DBG-03 ('each evaluate is supervised, so it pumps and Stop works, RV-05'). T-DBG-031 tests only the Cancel button, so neither reading is caught.

**Evidence.** TDD.md §8.3.4: 'Cancel calls `FRunSupervisor::RequestStop(EvalId)`. While `ActiveEvalId == EvalId`, that sets the evaluate's stop state and leaves the paused run's untouched, and the next pending call delivers `ScriptCancelled` to the evaluate'. §5.7.2 defines only one state. §5.7.3: '`if (bStopRequested && bUserCodeActive) {`'. §5.6.2: Shift+F5 key-down with 'Our modal active (Run or Paused window)' → 'Stop, swallow'. §5.6.3: 'Shift+F5 and the Stop buttons are the run-level Stop in every context'. §12.1.1 check_cancel row: 'True when Stop is pending for the calling run'. SPEC.md DBG-03: 'each evaluate is supervised, so it pumps and Stop works, RV-05'. T-DBG-031: 'at least one pump and a delivered Cancel within 1 s'. No test presses Shift+F5 during an evaluate.

**Refuter.** The issue is real, and the round-2 patch caused it. Before the patch, CurrentRunId was swapped to EvalId and there was one stop state, so a run-level Stop during an evaluate reached the evaluate's pending calls. The W-runtime-01 patch (current TDD line 5531) now says Cancel 'sets the evaluate's stop state and leaves the paused run's untouched', and §5.7.6 (line 2742) refers to 'the evaluate's stop state'. Neither is defined anywhere: §5.7.2 (line 2684) still lists one set of atomics, §5.7.3 delivers on the single `bStopRequested`, and the eval_begin/eval_end row (line 7859) restores only the mark and the gate. A builder who reads 'per-run state' plus 'EvalId is an FSumiRunId' (§8.1.5) as giving the evaluate its own state still has no rule for a run-level Stop. Since the patch, CurrentRunId stays the run's ID, so Shift+F5 calls RequestStop(run ID) (§5.6.2, §5.6.5) and sets only the run's state. Meanwhile every accepted pending call carries ActiveEvalId (§5.4.2 step 1) and delivers from the evaluate's state. A `while True: pass` evaluate then ignores Shift+F5, against line 5496 ('Shift+F5 always stops'), §5.6.3 and SPEC DBG-03. §8.3.3 (line 5515: 'A click or key queues a request in PendingRequests'; queued requests 'wait until the evaluate returns') also leaves it open whether the Cancel click can be processed at all during the evaluate. Parts (1) to (3) of the verifier's fix are sound. Part (4) does not work as written: it appends 'Shift+F5 during a third evaluate at the breakpoint stop … the run then ends as cancelled' to a row whose pass criterion already says 'then F5 continues the run at the breakpoint stop'. Both cannot happen at the same stop. It also never checks the single-state leak (a run cancelled after Continue). The corrected fix below uses a separate run for the Shift+F5 case and requires the continued run to end `ok`.

**Fix applied.** TDD.md, four edits.
(1) §5.7.2 Per-run cancellation state. After the paragraph ending '`bUserCodeActive` also gates new posts (§5.4.1) and the pump (§5.4.2 step 5).', add a new paragraph: '**Evaluate stop state.** A supervised evaluate (§8.3.4) has its own copy of `bStopRequested`, `StopSource`, `bInterruptCleanup`, `Deliveries`, the four times and the two flags. `bUserCodeActive` is not copied: `eval_begin` and `eval_end` save and restore it. `BeginSupervisedEvaluate` zeroes the copy and `EndSupervisedEvaluate` discards it. While `ActiveEvalId` is nonzero, the §5.4.2 step 9 and §5.7.3 deliveries, the §5.7.4 checks, the §5.7.5 conditions 1 to 3 and `check_cancel()` on the game thread use the evaluate's copy. `RequestStop(EvalId)`, the evaluate's Cancel, sets only the evaluate's copy. `RequestStop` with the paused run's ID (Shift+F5, Esc or a Stop button, §5.6) sets the run's state and, while an evaluate is active, the evaluate's copy too. The evaluate is cancelled first ("Evaluate cancelled"); once it returns, the run stops as a Paused-window Stop does (§8.5).'
(2) §8.3.4, Cancel bullet. After '…and it never reaches the paused run.' append: ' The Cancel button calls `RequestStop` from its click handler, as the Stop buttons do (§5.6.5); it is not a `PendingRequests` entry (§8.3.3), which would wait for the evaluate to return. A run-level Stop during an evaluate cancels the evaluate first and then stops the run (§5.7.2).'
(3) §12.1.1, row `check_cancel() -> bool`. Replace 'True when Stop is pending for the calling run;' with 'True when Stop is pending for the calling run or, on the game thread during a supervised evaluate, for the evaluate (§5.7.2);'.
(4) §8.11, T-DBG-031. Scenario: replace 'Supervised evaluate of `while True: pass`, at a breakpoint stop and at an uncaught-exception stop' with 'Supervised evaluate of `while True: pass`, cancelled with the Cancel button at a breakpoint stop and at an uncaught-exception stop; then a second run paused at the same breakpoint, where Shift+F5 is pressed during a `while True: pass` evaluate'. Pass criterion: replace 'In both: at least one pump and a delivered Cancel within 1 s ("Evaluate cancelled"); repaint ≤ 1 s; then F5 continues the run at the breakpoint stop and ends it as `error` at the exception stop' with 'In both Cancel cases: at least one pump and a delivered Cancel within 1 s ("Evaluate cancelled"); repaint ≤ 1 s; then F5 continues the run at the breakpoint stop, which ends as `ok` with no `ScriptCancelled` delivered to it, and ends the run as `error` at the exception stop. In the second run: "Evaluate cancelled" within 1 s of Shift+F5, then the run ends as `cancelled`'. Lands stays M3.

**Patch.** TDD §5.7.2: new 'Evaluate stop state' paragraph. An evaluate has its own copy of the stop atomics, times and flags. bUserCodeActive is saved and restored, not copied. Deliveries, the §5.7.4 checks, §5.7.5 conditions 1 to 3 and check_cancel use the evaluate's copy while ActiveEvalId is nonzero. RequestStop(EvalId) sets only the evaluate's copy. A run-level Stop (Shift+F5, Esc, Stop buttons) sets both, so the evaluate is cancelled first and then the run stops. §8.3.4 Cancel bullet: Cancel calls RequestStop from its click handler and is not a PendingRequests entry; a run-level Stop cancels the evaluate first, then the run. §12.1.1 check_cancel row now covers the evaluate. §8.11 T-DBG-031: the scenario adds a second run where Shift+F5 is pressed during the evaluate, and the pass criterion requires the continued run to end ok with no ScriptCancelled and the second run to end cancelled. Lands stays M3.

#### X-runtime-02

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §8.8 Variables table, row 'unreal.Array, unreal.Map, unreal.Set' (Children cell); TDD §8.8 Variables table, row 'unreal struct' (Value text cell); TDD §12.1.1 rows container_export_text and struct_export_text; TDD §8.11 T-INSP-014; TDD §8.11 T-INSP-015

**Problem.** W-runtime-11 found that an unresolved FLazyObjectPtr exports as `None` through `FObjectPropertyBase::ExportText_Internal` (`Get()` with no load, then 'None'), which reads as a null reference. The patch fixed only `prop_export_text`. The same export path is used, unchanged, by `container_export_text` for a lazy-object container and by `struct_export_text` for a lazy scalar field. T-INSP-014's unloaded 'lazy-object `Array`' therefore shows 150 `None` leaves. In T-INSP-015, the struct holding an unloaded lazy object property shows `lazy_field: None` in its `<StructName> {…}` value text, while the same field's child leaf shows `Lazy <guid>`. The §12.1.1 container row also still implies every reference shows its path: 'Never converts an element to Python, so a soft reference shows its path and is not loaded'.

**Evidence.** Engine source: `PropertyLazyObjectPtr.cpp:114-117`, `GetObjectPtrPropertyValue` returns `GetPropertyValue(...).Get()` (no load). `PropertyBaseObject.cpp:267-283`: `if (!Temp) { ValueStr += TEXT("None"); }`. TDD §8.8 container row: 'Otherwise the first 100 elements are leaves holding their export text from `_sumi.container_export_text` (C++ `FProperty::ExportTextItem_Direct` ...)'. Struct row: 'exports each scalar field with `FProperty::ExportTextItem_Direct`'. T-INSP-014: 'a soft-class `Array` and a lazy-object `Array`, 150 elements each ... each container shows at most 100 export-text leaves'.

**Refuter.** The engine claim holds in the local install. FLazyObjectProperty does not override export (UnrealType.h declares no ExportText for it). GetObjectPtrPropertyValue returns `GetPropertyValue(...).Get()` with no load (PropertyLazyObjectPtr.cpp:114-117). FObjectPropertyBase::ExportText_Internal writes 'None' when `!Temp` (PropertyBaseObject.cpp:267-283). The W-runtime-11 patch now states this rationale in the TDD and special-cases only `prop_export_text`. `container_export_text` (§12.1.1 line 7863) and `struct_export_text`/§8.8 struct value text (line 5744) use the same ExportTextItem_Direct path with no such rule. So the lazy-object Array in T-INSP-014 shows `None` leaves. In T-INSP-015, which the patch touched, the same unloaded lazy field shows `Lazy <guid>` as a leaf but `None` in the struct's `<StructName> {…}` text. The defect is minor and no listed criterion fails because of it, but the fix is a direct completion of the round-2 patch's own rationale, so it is in scope. The verifier's fix misses T-API-016 (line 8071). That test runs `container_export_text` on lazy-object containers and asserts 'shows each soft path', which an unresolved lazy element can never meet. 'Unresolved' should also mean `IsPending()` (a valid GUID, target not loaded: PersistentObjectPtr.h:218-221), so that a null lazy pointer keeps exporting `None`.

**Fix applied.** TDD.md.
(1) §12.1.1, row `prop_export_text(value: object, name: str) -> str`. Replace '; an unresolved lazy object property gives `Lazy <guid>`' with '; an unresolved lazy object property (`FLazyObjectPtr::IsPending()`: a valid GUID whose target is not loaded) gives `Lazy <guid>` from `GetUniqueID().ToString()`, and a null one gives `None`'.
(2) §12.1.1, row `container_export_text(...)`. Replace 'Never converts an element to Python, so a soft reference shows its path and is not loaded.' with 'Never converts an element to Python, so a soft reference shows its path and is not loaded; an unresolved lazy object element gives `Lazy <guid>`, as `prop_export_text` does.'
(3) §12.1.1, row `struct_export_text(value: object) -> str | None`. Replace 'as §8.8 defines; `None` for any other value' with 'as §8.8 defines, writing an unresolved lazy object field as `Lazy <guid>`, as `prop_export_text` does; `None` for any other value'.
(4) §8.8 Variables table. In the `unreal.Array, unreal.Map, unreal.Set` row, replace 'but capped at 100 elements, §12.1.1); they are never indexed or iterated' with 'but capped at 100 elements, §12.1.1; an unresolved lazy element shows `Lazy <guid>`); they are never indexed or iterated'. In the `unreal struct` row, replace 'exports each scalar field with `FProperty::ExportTextItem_Direct`.' with 'exports each scalar field with `FProperty::ExportTextItem_Direct`, writing an unresolved lazy object field as `Lazy <guid>` (§12.1.1).'
(5) §8.11 T-INSP-014, pass criterion. Replace 'each container shows at most 100 export-text leaves;' with 'each container shows at most 100 export-text leaves, the lazy-object `Array`'s as `Lazy <guid>`;'.
(6) §8.11 T-INSP-015, pass criterion. Replace 'and the unloaded lazy object property shows `Lazy <guid>`' with 'and the unloaded lazy object property shows `Lazy <guid>`, as a leaf and in the struct's value text'.
(7) §15 T-API-016, pass criterion. Replace 'and shows each soft path;' with 'and shows each soft path, and `Lazy <guid>` for each unresolved lazy element;'.

**Patch.** TDD §12.1.1 prop_export_text: 'unresolved' is defined as FLazyObjectPtr::IsPending(), the text comes from GetUniqueID().ToString(), and a null pointer gives None. container_export_text and struct_export_text: an unresolved lazy element or field gives `Lazy <guid>`. §8.8: the container and struct rows say the same. T-INSP-014, T-INSP-015 and T-API-016 pass criteria now expect `Lazy <guid>`. IsPending was checked in PersistentObjectPtr.h:218-221 and LazyObject Get()-without-load in PropertyLazyObjectPtr.cpp.

### Round 3 — Persistence, policy and studio

Persist lens, round 3. Method: word-diffed the round-2 before and after files, which match the current docs; read TDD §10, §11.2-§11.5 and §12.3-§12.4, SPEC PER-08, SEC-09, PERF-01 and D.3, and the 08 RV-07 row; checked the DirectoryWatcher citations against the UE 5.8 source; looked at images/07-snippets-and-shelves.png; ran the integrator check.py (0 problems).

Correct and complete: W-persist-01, -04, -07, -08, -09, -11, -12, -13 and -16. For -13 the params block now uses string type with title fields, the items/<category>/*.sumi.json and *.shelf.json paths are in place, the PNG is re-rendered at 1600x1920, and the long path wraps but fits.

W-persist-05 (major) is only half closed. The owner.json and live-root checks are right, but they run only when the task scans. The delete after the user resolves never re-checks the locks, and the scan's ReadNamespace deletes and renames files without holding the root's locks. If another editor becomes primary of that root before resolution, the delete removes its live journal/main. Reported as X-persist-01, major.

W-persist-06 dropped 'At primary start', so a secondary would also scan and delete (X-persist-02). It also left §10.3 Budgets, §10.2.4 and SPEC PERF-01 saying all journal I/O is on the writer (X-persist-03) and left the §10.7 state diagram treating orphans as final at Scanning (X-persist-08).

W-persist-14's bounded probe covers instance.lock only. owner.json, ns.lock and the restore guard on the override root are still game-thread operations (X-persist-04).

W-persist-02's engine citations are correct (OPEN_EXISTING at DirectoryWatchRequestWindows.cpp:78-93, failure path at DirectoryWatcherWindows.cpp:63-72). But the poll is not stated to run when no studio path is configured, and retrying the registration every 5 minutes logs a LogDirectoryWatcher warning each time (X-persist-05).

W-persist-03 and -15 are right in §11.2, §8.6.4 and T-AUD-007, but the §12.3 interface row and the §15.2 M2 row still give the old M2 scope, which TDD 1.5 uses to derive Lands (X-persist-06).

W-persist-10's mirror.lock design is sound. The outcome of the 2 s re-sync timeout is missing, and current.json and mirror.lock are not in the §12.4 index (X-persist-07).

Scratch diffs: scratchpad/verify4/persist/ (tdd.diff, spec.diff, 08.diff, mock.diff).

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [X-persist-01](#x-persist-01) | major | patch incomplete | modified | The W-persist-05 liveness guard only applies when the task scans. |
| [X-persist-09](#x-persist-09) | major | patch incomplete | added by refuter | The cross-root scan lists only namespaces that lack CLEAN. |
| [X-persist-02](#x-persist-02) | minor | patch wrong | confirmed | The W-persist-06 replacement text dropped the old qualifier "At primary start". |
| [X-persist-03](#x-persist-03) | minor | contradiction | modified | W-persist-06 moved the cross-root orphan scan and its deletes to a worker that is never the journal writer. |
| [X-persist-04](#x-persist-04) | minor | patch incomplete | modified | Only instance.lock moved to the bounded worker probe. |
| [X-persist-05](#x-persist-05) | minor | patch incomplete | confirmed | (1) "The 5-minute poll" is defined only as the studio-file poll. |
| [X-persist-06](#x-persist-06) | minor | patch incomplete | modified | W-persist-03 and W-persist-15 moved several parts into M2: the machine studio_policy_path, the studio-layer Startup rules (5 s timeout, PolicyCache/studio.json, a missing configured file treated as unreachable), the … |
| [X-persist-07](#x-persist-07) | minor | patch incomplete | confirmed | (1) A full mirror run holds mirror.lock for its whole duration, which for a studio share over SMB is often longer than 2 s, so this timeout will be common. |
| [X-persist-08](#x-persist-08) | minor | contradiction | confirmed | Since W-persist-06, cross-root orphans arrive from a worker task after the reconcile: 'what it finds joins Review when it returns, and the n = 0 banner (§10.7.1) then appears if needed'. |
| [X-persist-10](#x-persist-10) | minor | patch incomplete | added by refuter | Round 2 added two pieces of background work that can block on an unanswering network root: the override probe (W-persist-14), which may complete later than its 2 s deadline, and the cross-root scan and its deletes … |
| [X-persist-11](#x-persist-11) | minor | patch wrong | added by refuter | The W-persist-16 lock rule conflicts with the thread it runs on. |

#### X-persist-01

**Major, patch incomplete.** Verdict: modified.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet: "Review lists each namespace there whose `ns.lock` is free and that lacks `CLEAN` as an orphan (§10.5); after it is resolved, a worker deletes that namespace."; TDD.md §10.2.4 step 1 "Delete stale `*.tmp`." and "A damaged generation is renamed `wal.<g>.sjl.corrupt`"; TDD.md §10.12 T-PER-055

**Problem.** The W-persist-05 liveness guard only applies when the task scans. The delete happens when the user resolves the orphan, which can be hours later because Review is non-modal. In between, another editor of the same project can start at the scanned root and become its primary. That happens when the override is down again and the editor falls back to the platform default. A primary always journals in `journal/main/` (§10.5 table, 'Lock free | Primary | journal/main/'), so it reopens the namespace that was listed as an orphan, and the later delete removes a live instance's journal. That is the failure W-persist-05 was raised for. The scan is not read-only either: ReadNamespace deletes stale `.tmp` files, which can be the new primary's checkpoint in progress, and renames damaged generations, all without holding that root's locks. T-PER-055 tests only a root that is already live when the scan runs.

**Evidence.** §10.1 states the instance.lock and ns.lock conditions only when it selects roots and lists namespaces. The delete sentence has no re-check. §10.5 checks liveness by acquiring the lock (on Windows a sharing violation is authoritative), and nothing tells the task to hold or retake it. T-PER-055's new case is 'a candidate root whose `instance.lock` and `main` are held by a running instance', which is live before the scan, not after.

**Refuter.** The issue is real. The W-persist-05 guards in TDD §10.1 (the instance.lock not held by a live instance, and a free ns.lock) are applied only when the task picks roots and lists namespaces. The delete after resolution, added by W-persist-06 as 'a worker deletes that namespace', checks nothing again, and Review is non-modal, so resolution can come hours later. In that gap a second editor of the same project can become primary at the scanned root and journal in journal/main/ (§10.5 table). The natural case: A starts while the override is down, its worker scans the override root once it answers, and B then starts with the override up. On POSIX the delete then unlinks B's live journal. On Windows the files B holds open refuse deletion, but the kept previous generation is closed and can be deleted. ReadNamespace's stale-.tmp removal and .corrupt rename (§10.2.4) also run without that root's locks. The verifier's fix is right in substance. I split it so it does not overlap another edit to the same sentence (missed X-persist-09), and I widened the test case, because B need not start at the platform default.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: (1) after "A root whose `owner.json` names another account is never read, listed or deleted from." insert " The task reads the other roots through a `FileOps` that never deletes or renames, so the stale-`.tmp` removal and the `.corrupt` rename of §10.2.4 do not run there." (2) Replace "; after it is resolved, a worker deletes that namespace." with "; after it is resolved, a worker acquires that root's `instance.lock` and then the namespace's `ns.lock` (§10.5), deletes the namespace only while it holds both, and then releases them. If either is held, the namespace is not deleted: that root now has a live primary, which owns it." TDD §10.12 T-PER-055, scenario cell: append "; a second editor of the same project that becomes primary at a scanned root after the scan and before the listed orphan is resolved". Result cell: append "; for that root the delete is skipped and the second editor's journal is intact".

**Patch.** TDD §10.1 ResolveStorageRoot bullet: the cross-root task reads other roots through a FileOps that never deletes or renames, so there is no stale-.tmp removal and no .corrupt rename there. The delete after resolution now takes that root's instance.lock and then ns.lock, deletes only while it holds both, and skips the delete if either is held. T-PER-055 adds the case of a second editor that becomes primary at a scanned root before the orphan is resolved; the delete is skipped and that editor's journal is intact.

#### X-persist-09

**Major, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet: "Review lists each namespace there whose `ns.lock` is free and that lacks `CLEAN` as an orphan (§10.5)"; TDD.md §10.3 Reconcile: "After a clean exit, the ini's tab list wins and the journal supplies dirty and scratch text."; TDD.md §10.12 T-PER-055

**Problem.** The cross-root scan lists only namespaces that lack CLEAN. When the storage root changes between two clean sessions, unsaved work from the earlier session is silently lost. This is the case W-persist-14 now makes routine: the override does not answer within 2 s, so the session journals at the platform default, and at the next start the override answers again. Sumi keeps dirty file buffers and scratch buffers across a clean exit. The next session restores the ini's tab list but takes the text from its own root's journal/main, which does not hold the previous session's text. The previous session's namespace ends with CLEAN, so the scan ignores it. Dirty edits reopen as the disk text and scratch tabs come back empty, with nothing offered in Review. The data stays on disk under the other root, but no UI ever offers it.

**Evidence.** §10.3: after a clean exit 'the journal supplies dirty and scratch text', and that journal is this root's `journal/main` (§10.5 table, Primary → journal/main/). §10.7.1: n counts only namespace `main` of this root. §10.5 handles the same situation for a second instance in one root ('With `CLEAN` but buffers not on disk: "Tabs from a second editor instance (closed 14:10)"'), but the cross-root rule in §10.1 has no equivalent. T-PER-055 covers only the kill -9 path. The criterion predates round 2, but W-persist-05 rewrote this very sentence and kept it.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: replace "whose `ns.lock` is free and that lacks `CLEAN` as an orphan (§10.5)" with "whose `ns.lock` is free and that lacks `CLEAN` as an orphan (§10.5), and also one that ends with `CLEAN` but holds a non-empty scratch buffer or a file buffer whose journaled text differs from the file on disk, in a section \"Tabs from another storage folder (closed 14:10)\", because this session restores its tabs from its own root's journal (§10.3)". TDD §10.12 T-PER-055, scenario cell: append "; a session at the platform default (override unwritable) that exits cleanly with a dirty file buffer and a non-empty scratch buffer, then a start with the override writable". Result cell: append "; after that clean exit both buffers are listed in Review under \"Tabs from another storage folder\", and Restore recovers their text".

**Patch.** TDD §10.1: Review also lists a CLEAN namespace at another root that holds a non-empty scratch buffer or a dirty file buffer, in a section "Tabs from another storage folder (closed 14:10)". T-PER-055 adds the case of a clean exit at the platform default followed by a start with the override writable; both buffers are listed and Restore recovers their text.

#### X-persist-02

**Minor, patch wrong.** Verdict: confirmed.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet: "After the reconcile and the first checkpoint (§10.3 step H), a worker task, never the journal writer, also reads `journal/*` of this `<ProjectHash>` under every other candidate root"; TDD.md §10.5: "A secondary never writes, rotates or deletes another namespace."

**Problem.** The W-persist-06 replacement text dropped the old qualifier "At primary start". As written, a secondary instance also runs the cross-root scan, lists the same orphans as its primary, and has a worker delete another root's namespace after resolution. §10.5 forbids that, and it gives two editors competing deletes of the same orphan.

**Evidence.** Before the patch: "At primary start the recovery scan (§10.3 step G) also reads ...". After: "After the reconcile and the first checkpoint (§10.3 step H), a worker task, never the journal writer, also reads ...". The approved final_fix itself omitted 'primary', and the patcher applied it verbatim.

**Refuter.** Checked against TDD.before.md:6314. It read "At primary start the recovery scan (§10.3 step G) also reads ...". The current text (TDD.md:6319) reads "After the reconcile and the first checkpoint (§10.3 step H), a worker task, never the journal writer, also reads ...". The approved final_fix left out 'primary', and the patcher applied it word for word. A secondary also reconciles and writes a first checkpoint in its own namespace, so as written it runs the cross-root scan too: it lists the same orphans as its primary, and its worker deletes another root's namespace, which §10.5 forbids ("A secondary never writes, rotates or deletes another namespace"). No other text limits this bullet to the primary.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: replace "After the reconcile and the first checkpoint (§10.3 step H), a worker task, never the journal writer, also reads" with "After the primary's reconcile and first checkpoint (§10.3 step H), a worker task of the primary (never a secondary, §10.5, and never the journal writer) also reads".

**Patch.** TDD §10.1: the scan is now 'After the primary's reconcile and first checkpoint (§10.3 step H), a worker task of the primary (never a secondary, §10.5, and never the journal writer)'.

#### X-persist-03

**Minor, contradiction.** Verdict: modified.

**Where:** TDD.md §10.3 Budgets: "the scan and all journal I/O are on the writer."; TDD.md §10.2.4: "`sumi::JournalFormat::ReadNamespace(FileOps&, Dir)` runs on the writer thread:"; SPEC.md §8.1 PERF-01: "journal I/O only on the writer thread, and the game thread's only file operations are the instance lock and the restore guard (TDD §10.3)"; TDD.md §10.1 ResolveStorageRoot bullet: "from the machine layer only when it passes the §11.2.1 owner check and is a valid layer (§11.2.3), read synchronously because it is a local file"

**Problem.** W-persist-06 moved the cross-root orphan scan and its deletes to a worker that is never the journal writer. Three statements still say all journal I/O, ReadNamespace included, is on the writer. A builder who follows SPEC PERF-01 or §10.2.4 puts the scan back on the writer and reintroduces the stall W-persist-06 removed. Root resolution runs on the game thread (§10.2.1), and from M4 it also reads the machine Policy.json with its owner check, plus PolicyCache/studio.json; W-persist-07 made that read explicitly synchronous. For an override, the spawn can also wait up to 2 s (W-persist-14). PERF-01 still says the game thread's only file operations are the instance lock and the restore guard.

**Evidence.** The §10.1 text after the patch: "a worker task, never the journal writer, also reads `journal/*` ... a worker deletes that namespace". §10.2.1 places 'root resolution, locks' on the Game thread. The PERF-01 text is unchanged.

**Refuter.** The issue is real. TDD §10.1 now runs the cross-root read and delete of journal namespaces on a worker that is 'never the journal writer'. But §10.3 Budgets still says "the scan and all journal I/O are on the writer", §10.2.4 says ReadNamespace "runs on the writer thread", and SPEC PERF-01, a Gate, says "journal I/O only on the writer thread". From M4, ResolveStorageRoot also reads the machine Policy.json (with its owner check) and PolicyCache/studio.json on the game thread (§10.2.1: root resolution is on the game thread). W-persist-07 made that read explicitly synchronous, yet PERF-01 still names only the instance lock and the restore guard. W-persist-14 adds a GT wait of up to 2 s for an override that does not answer, and the SPEC does not mention it. The verifier's §10.3 wording added the M4 reads after the writer clause and left the 'two small file operations' sentence unqualified. I reworded it, and anchored it on a substring that does not overlap the X-persist-04 edit in the same sentence.

**Fix applied.** TDD §10.3 Budgets: replace "so a secondary uses its own guard (§10.5); the scan and all journal I/O are on the writer." with "so a secondary uses its own guard (§10.5); from M4, root resolution also reads the local machine `Policy.json` (with its §11.2.1 owner check) and `PolicyCache/studio.json` on the game thread (§10.1). The scan of this root and all its journal I/O are on the writer; the cross-root orphan scan and its deletes run on their own worker (§10.1)." TDD §10.2.4: replace "runs on the writer thread:" with "runs on the writer thread (the cross-root orphan scan of §10.1 runs it on its own worker):". SPEC §8.1 PERF-01: replace "journal I/O only on the writer thread, and the game thread's only file operations are the instance lock and the restore guard (TDD §10.3)" with "journal I/O never on the game thread, whose only file operations are the instance lock and the restore guard, plus from M4 the local machine policy file and the cached studio policy read to resolve the storage root; a storage override folder that does not answer adds at most 2 s, once (TDD §10.1, §10.3)". SPEC Appendix D.3: after the row "| — | V-persist-03, W-persist-12 | The machine- and studio-layer `storage.root_override` lands in M4 | PER-08 |" add "| — | W-persist-06, W-persist-07, W-persist-14 | Journal I/O stays off the game thread (the cross-root orphan scan runs on its own worker); from M4, resolving the storage root reads the local machine policy and the cached studio policy; an override folder that does not answer costs at most 2 s, once | PERF-01 |".

**Patch.** TDD §10.3 Budgets: from M4, root resolution also reads the machine Policy.json (with owner check) and PolicyCache/studio.json on the GT; the scan of this root is on the writer; the cross-root scan and its deletes run on their own worker. TDD §10.2.4: ReadNamespace runs on the writer, except for the cross-root scan's own worker. SPEC PERF-01 reworded as approved. SPEC D.3 row added: X-persist-03 (W-persist-06, W-persist-07, W-persist-14), PERF-01.

#### X-persist-04

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §10.1 Rules, fallback-order bullet: "The override is never probed by a blocking call on the game thread: a worker opens `instance.lock` there (creating the folders if needed), and the tab spawn waits for it at most 2 s (**proposed**)."; TDD.md §10.1 storage.root_override bullet: "a root whose `owner.json` names another account is rejected with a warning, and the next candidate of the fallback chain is used"; TDD.md §10.3 Budgets: "the instance lock first and then the restore guard (with an override, the lock comes from the bounded §10.1 probe ...)"; TDD.md §10.2.1 row "root resolution, locks (an override root's `instance.lock` is opened by a bounded worker probe, §10.1) | ... | Game thread"

**Problem.** Only instance.lock moved to the bounded worker probe. The other spawn-time file operations on the override root are still game-thread work in ResolveStorageRoot and Open: reading owner.json and writing it at first use, reading, incrementing and flushing the restore guard, and opening journal/main/ns.lock. A builder following §10.1's order (resolve, check owner.json, then open the lock) reads owner.json from an unanswering NAS on the game thread. That is the SMB-timeout freeze W-persist-14 removes. Even after a successful probe, the restore-guard flush is a blocking SMB round trip on the game thread.

**Evidence.** §10.3 still says the restore guard is a game-thread file operation on the spawn path. §10.2.1 still puts 'locks' (plural) on the Game thread. The probe text names only instance.lock and the folders.

**Refuter.** The owner.json part is real. §10.1 says the override is 'never probed by a blocking call on the game thread', but the mechanism it gives, and the §10.2.1 row, name only instance.lock. The owner.json check (and its first-use write) belongs to ResolveStorageRoot, which runs on the game thread, and it must happen before the root is accepted. A builder can therefore read owner.json from a NAS that does not answer, on the GT. The proposed fix also moves the restore-guard read and increment, and ns.lock, into the probe, and that creates a new failure. A probe that has already flushed pending += 1 at the override root (§10.8 step 2) but has not answered by the 2 s deadline leaves an increment that no session clears, because only the session using that root resets it. Two such starts then put a later start at the override into safe mode. Once the probe has answered, the host is reachable, and the guard and ns.lock are ordinary small operations, as at any root. So only owner.json moves into the probe.

**Fix applied.** TDD §10.1 Rules, fallback-order bullet: replace "a worker opens `instance.lock` there (creating the folders if needed)" with "a worker creates the folders there if needed, checks `owner.json` (writing it at first use; a root that names another account is rejected as above) and opens `instance.lock`". TDD §10.3 Budgets: replace "(with an override, the lock comes from the bounded §10.1 probe," with "(with an override, the `owner.json` check and the lock come from the bounded §10.1 probe,". TDD §10.2.1 table, row 'Snapshots, request queue, ...': replace "(an override root's `instance.lock` is opened by a bounded worker probe, §10.1)" with "(an override root's `owner.json` check and `instance.lock` are done by a bounded worker probe, §10.1)".

**Patch.** TDD §10.1 fallback bullet: the bounded worker creates the folders, checks owner.json (writing it at first use; another account is rejected) and opens instance.lock. §10.3 Budgets and the §10.2.1 row now say the owner.json check and the lock come from the bounded probe. Per the refuter, the restore guard and ns.lock were not moved into the probe.

#### X-persist-05

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §11.2.3 Watching bullet: "From M2, the 5-minute poll also stats the machine file. ... If watcher registration had failed, it is attempted again."

**Problem.** (1) "The 5-minute poll" is defined only as the studio-file poll. Before IT deploys a machine policy, usually no studio path is configured either, because the studio path normally comes from the machine file's studio_policy_path. A builder may then never start the poll, and T-DBG-043 case (b) fails. (2) Trying the registration again at every poll while the folder is missing logs an engine warning every 5 minutes on every workstation without a machine policy. Checked in the 5.8 source: FDirectoryWatcherWindows::RegisterDirectoryChangedCallback_Handle logs `UE_LOGF(LogDirectoryWatcher, Warning, "Failed to begin reading directory changes for %ls...")` (DirectoryWatcherWindows.cpp:68), and the request also logs a Display line (DirectoryWatchRequestWindows.cpp:92). The patch's citations (DirectoryWatchRequestWindows.cpp:78-93 for OPEN_EXISTING, DirectoryWatcherWindows.cpp:63-72) are correct.

**Evidence.** Engine source at C:/Program Files/Epic Games/UE_5.8/Engine/Source/Developer/DirectoryWatcher/Private/Windows: CreateFile with OPEN_EXISTING at :78-86, failure return at :88-93; Init failure leads to a Warning log at DirectoryWatcherWindows.cpp:68 and return false at :72.

**Refuter.** Both parts hold. (1) TDD §11.2.3 Watching defines the 5-minute poll as the studio-file poll ('The studio file is polled every 5 min'), and 'the 5-minute poll also stats the machine file' only extends it. With no studio path configured, which is the usual state before IT deploys, a builder can skip the poll entirely. T-DBG-043 case (b) configures no studio path, so it would fail. (2) In the local 5.8 source, a failed registration logs a Warning every time: FDirectoryWatcherWindows::RegisterDirectoryChangedCallback_Handle calls UE_LOGF(LogDirectoryWatcher, Warning, "Failed to begin reading directory changes ...") at DirectoryWatcherWindows.cpp:68 and returns false at :72, after FDirectoryWatchRequestWindows::Init logs a Display line at DirectoryWatchRequestWindows.cpp:92 on the OPEN_EXISTING failure at :78-93. Retrying at every poll therefore puts a warning in the log every 5 minutes on each workstation without a machine policy. Both replaced strings are unique in TDD.md.

**Fix applied.** TDD §11.2.3 Watching bullet: replace "From M2, the 5-minute poll also stats the machine file." with "From M2, a 5-minute poll on a worker runs whether or not a studio path is configured: it polls the studio file when there is one, and it always stats the machine file and its folder." Replace "If watcher registration had failed, it is attempted again." with "The watcher is registered only while the machine folder exists, and the poll registers it when the folder appears, because registering a missing folder fails and logs a `LogDirectoryWatcher` warning each time (`DirectoryWatcherWindows.cpp:68`)."

**Patch.** TDD §11.2.3 Watching: from M2 a 5-minute worker poll always runs, polling the studio file when one is configured and always statting the machine file and its folder. The watcher is registered only while the machine folder exists, and the poll registers it when the folder appears, citing the LogDirectoryWatcher warning at DirectoryWatcherWindows.cpp:68.

#### X-persist-06

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §12.3 row `FSumiPolicy`, `sumi::PolicyResolver`: "M2 ships a minimal resolver over the machine and studio layers, with the §11.2.1 owner check, for `debug.allow_debug_server` only (RV-04), with its §11.2.3 reload and swap; the full resolver lands in M4"; TDD.md §15.2 M2 row, Scope: "a minimal `FSumiPolicy` (`sumi::PolicyResolver` over the machine and studio layers, with the §11.2.1 owner check) for `debug.allow_debug_server` only"; TDD sections: "§11.2 (minimal resolver for `debug.allow_debug_server`, with its §11.2.3 reload and swap)"

**Problem.** W-persist-03 and W-persist-15 moved several parts into M2: the machine studio_policy_path, the studio-layer Startup rules (5 s timeout, PolicyCache/studio.json, a missing configured file treated as unreachable), the machine-file poll and the M2 Gate ('Waiting for policy'). The interface row and the milestone row still describe the old M2 scope. TDD 1.5 derives Lands from the §15.2 Scope and TDD sections columns, and M4's row takes 'the parts not in M1 or M2'. Read from §15.2, these parts are M4, yet T-AUD-007 (M2) depends on them.

**Evidence.** TDD §11.2 Lands now says "resolving `debug.allow_debug_server` only, plus the machine layer's `studio_policy_path` ... It also ships the §11.2.3 Startup rules for the studio layer ... and the M2 part of the Gate". §12.3 and §15.2 were not updated.

**Refuter.** The issue is real. TDD §11.2 Lands now ships, in M2, the machine studio_policy_path (W-persist-15), the §11.2.3 studio Startup rules and the M2 Gate (W-persist-03). The summary rows still list only 'with its §11.2.3 reload and swap', which by contrast leaves out Startup and the Gate. TDD §1.5 takes a test's Lands partly from the §15.2 Scope and TDD sections columns, and the M4 row takes 'the parts not in M1 or M2'. The verifier missed one of the three summary rows: the §11 feature table M2 row (TDD.md:6844) has the same wording. All four anchors below are unique in TDD.md.

**Fix applied.** TDD §11 feature table, M2 row: replace "for `debug.allow_debug_server` only, with its §11.2.3 reload and swap, and FSumiAudit's" with "for `debug.allow_debug_server` only, with the machine `studio_policy_path`, the §11.2.3 studio Startup rules, the M2 Gate, reload and swap (§11.2 Lands), and FSumiAudit's". TDD §12.3, row `FSumiPolicy`, `sumi::PolicyResolver`: replace "for `debug.allow_debug_server` only (RV-04), with its §11.2.3 reload and swap; the full resolver lands in M4" with "for `debug.allow_debug_server` only (RV-04), with the machine `studio_policy_path`, the §11.2.3 studio Startup rules, the M2 Gate, reload and swap (§11.2 Lands); the full resolver lands in M4". TDD §15.2 M2 row, Scope cell: replace "for `debug.allow_debug_server` only, and FSumiAudit's" with "for `debug.allow_debug_server` only, with the machine `studio_policy_path`, the studio policy cache and the \"Waiting for policy\" gate, and FSumiAudit's". Same row, TDD sections cell: replace "§11.2 (minimal resolver for `debug.allow_debug_server`, with its §11.2.3 reload and swap)" with "§11.2 (minimal resolver for `debug.allow_debug_server`, with the machine `studio_policy_path`, the §11.2.3 studio Startup rules, the M2 Gate, reload and swap)".

**Patch.** TDD §11 feature table M2 row, §12.3 FSumiPolicy row and §15.2 M2 Scope cell now name the machine studio_policy_path, the §11.2.3 studio Startup rules (policy cache) and the M2 Gate ('Waiting for policy'). The §15.2 M2 TDD sections cell was rewritten with X-delivery-03's wording (same content).

#### X-persist-07

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §11.5.1 Consistent mirrors: "The one-item re-sync of Restore published & run (§11.5.4) also takes `mirror.lock`, waiting for it up to 2 s on its worker (**proposed**)."; TDD.md §12.4 row 'Snippet item; shelf item' (location cell ending "verified against the copied manifest before the swap (§11.5.1)")

**Problem.** (1) A full mirror run holds mirror.lock for its whole duration, which for a studio share over SMB is often longer than 2 s, so this timeout will be common. What Restore published & run does after the 2 s wait is not stated: run anyway without the lock, fail, or wait for the mirror. Running without the lock is the race W-persist-10 closes. (2) The patch adds a new per-user data file, `current.json`, which names the current generation and is what readers resolve, plus `mirror.lock`. Neither is in the §12.4 index. W-persist-11 accepted that §12.4 has to be complete.

**Evidence.** §11.5.1 after the patch has no outcome for the timeout. The §12.4 Snippet item row still describes the mirror only as 'a new generation folder ... verified ... before the swap'.

**Refuter.** (1) TDD §11.5.1 makes a full mirror run hold mirror.lock for its whole duration, and over SMB that is often longer than 2 s. The re-sync waits up to 2 s, and nothing says what Restore published & run does when the wait times out. Running without the lock would reopen the race W-persist-10 closed. (2) TDD §12.4 says "`Resources/Schemas/formats.json` registers every row", and W-persist-11 accepted that the index must be complete. The new per-user file current.json (a JSON format) and mirror.lock are missing from the 'Snippet item; shelf item' row, which still describes the mirror only as a generation folder verified before the swap. Both anchors are unique.

**Fix applied.** TDD §11.5.1 Consistent mirrors bullet: after "waiting for it up to 2 s on its worker (**proposed**)." insert " If it is still held, nothing runs, and the prompt keeps its buttons and adds the line \"The studio library is being synced; try again in a moment\"." TDD §12.4, row 'Snippet item; shelf item', Location cell: after "verified against the copied manifest before the swap (§11.5.1)" insert "; `current.json` there names the current generation and is replaced atomically at each swap, and `mirror.lock` is held by the process that is mirroring (§11.5.1)".

**Patch.** TDD §11.5.1: if mirror.lock is still held after the 2 s wait, nothing runs, and the prompt keeps its buttons and adds 'The studio library is being synced; try again in a moment'. §12.4 Snippet/shelf item row now lists current.json (replaced atomically at each swap) and mirror.lock.

#### X-persist-08

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD.md §10.7 stateDiagram-v2: "Scanning --> Clean: CLEAN last, no orphans, nothing pending" and "Clean --> [*]"

**Problem.** Since W-persist-06, cross-root orphans arrive from a worker task after the reconcile: 'what it finds joins Review when it returns, and the n = 0 banner (§10.7.1) then appears if needed'. The recovery state machine still decides 'no orphans' when Scanning ends and makes Clean and Resolved final. An implementation built from the diagram drops orphans that arrive late.

**Evidence.** The §10.1 text after the patch: "Nothing waits on this task ... what it finds joins Review when it returns". The §10.7 diagram has no transition out of Clean or Resolved.

**Refuter.** Since W-persist-06, cross-root orphans come from a worker after the reconcile and the first checkpoint ('what it finds joins Review when it returns, and the n = 0 banner (§10.7.1) then appears if needed'). The TDD §10.7 stateDiagram still decides 'no orphans' when Scanning ends and makes Clean and Resolved final (Clean --> [*], Resolved --> [*]), so a state machine built from the diagram has no path for late orphans. The fix is small and the anchor string is unique.

**Fix applied.** TDD §10.7 stateDiagram-v2: change "    Scanning --> Clean: CLEAN last, no orphans, nothing pending" to "    Scanning --> Clean: CLEAN last, no orphans in this root, nothing pending", and add the lines "    Clean --> Pending: cross-root orphans found later (§10.1)" and "    Resolved --> Pending: cross-root orphans found later (§10.1)" before "    Clean --> [*]".

**Patch.** TDD §10.7 stateDiagram: the Scanning-->Clean label is now 'no orphans in this root', and the transitions 'Clean --> Pending' and 'Resolved --> Pending: cross-root orphans found later (§10.1)' were added.

#### X-persist-10

**Minor, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §10.1 Rules, fallback-order bullet: "A probe that completes later closes what it opened."; TDD.md §10.1 Rules, ResolveStorageRoot bullet: "Nothing waits on this task, and it may take as long as the file system needs"; TDD.md §3.1 Thread inventory; TDD.md §10.12 T-PER-055

**Problem.** Round 2 added two pieces of background work that can block on an unanswering network root: the override probe (W-persist-14), which may complete later than its 2 s deadline, and the cross-root scan and its deletes (W-persist-06), which 'may take as long as the file system needs'. Neither has a shutdown rule. If they run as UE::Tasks or on GThreadPool, which is what 'a worker task' suggests, engine exit waits for them. A NAS that is down at start, the very case these patches handle, then delays editor exit by the SMB timeout. On a hard-mounted Linux NFS share (Linux Preview from M4) the delay has no limit. The journal writer was designed to avoid exactly this ('Exit therefore never waits on a blocked FlushFileBuffers'), and T-PER-041 bounds exit at baseline + 2 s.

**Evidence.** Local 5.8 source: FEngineLoop::Exit (LaunchEngineLoop.cpp:4987) calls GThreadPool->Destroy(), then GLargeThreadPool->Destroy() and FTaskGraphInterface::Shutdown() (Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5195-5215). FQueuedThreadPoolBase::Destroy waits until every pool thread is idle and then calls KillThread, which calls WaitForCompletion (Engine/Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:1317-1352, 1181-1191). ~FTaskGraphCompatibilityImplementation calls LowLevelTasks::FScheduler::StopWorkers (Async/TaskGraph.cpp:1313-1327), which joins every worker (Async/Fundamental/Scheduler.cpp:439-457). The §3.1 thread inventory lists neither task.

**Fix applied.** TDD §10.1 Rules, fallback-order bullet: after "A probe that completes later closes what it opened." insert " The probe, and the cross-root task of the next bullet, run on one Sumi-owned `FRunnable` thread, `SumiStorageProbe`, never on `UE::Tasks` or `GThreadPool`, whose workers engine exit joins (`Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5195-5215`, `Engine/Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:1317-1352`). At `OnPreExit` that thread is abandoned like the journal writer (§10.2.5): its `FileOps` fence is set, nothing joins it, and its code and state stay alive until the process exits." TDD §3.1 Thread inventory: after the row starting "| **Find-in-files worker**" add "| **Storage probe** (`SumiStorageProbe`, `FRunnable`) | 1; created at the first override probe or cross-root scan; abandoned at `OnPreExit`, never joined (§10.1) | The override root until its probe returns; other candidate storage roots for the cross-root orphan scan and deletes (§10.1) | Python, Slate, UObjects; the session's own journal namespace | Never | Result queue polled on the GT |". TDD §10.12 T-PER-055, result cell: append "; exiting the editor while the override host still does not answer takes at most 2 s more than the baseline".

**Patch.** TDD §10.1 fallback bullet: the probe and the cross-root task run on one Sumi-owned FRunnable, SumiStorageProbe, never on UE::Tasks or GThreadPool (LaunchEngineLoop.cpp:5195-5215 and ThreadingBase.cpp:1317-1352, checked). It is abandoned at OnPreExit like the journal writer. A §3.1 thread-inventory row was added after Find-in-files worker. For consistency, §3.8 Module shutdown now says the probe thread is not joined. T-PER-055 adds: exiting with an unanswering override host takes at most 2 s more than the baseline.

#### X-persist-11

**Minor, patch wrong.** Raised by the refuter.

**Where:** TDD.md §11.3.3 'The store' sub-bullet: "`TrustFolder` and `UntrustFolder` apply their one change to a fresh read of the file under `Trust.json.lock` (created exclusively, retries for 1 s, as for `projects.json`, §10.1), then replace it atomically"

**Problem.** The W-persist-16 lock rule conflicts with the thread it runs on. Trust.json is 'written atomically on the I/O thread', and the I/O thread is the journal writer, where each PostIo item must fit one 50 ms slice. A 1 s retry loop there holds commits back by up to 1 s, which pushes the §10.2.5 worst case of 1.75 s past the 2 s G6 bound. A lock that is 'created exclusively' is a file that a crash during the hold leaves behind, unlike the handle locks of §10.5, so after such a crash every later Trust or Untrust waits the full second and fails. What the user sees when the lock cannot be taken is not stated.

**Evidence.** TDD §11 Terms: "I/O thread: FSumiJournal's background writer (§10), reached through FSumiJournal::PostIo()". §10.2.6: "PostIo(...) // low-priority lane of the writer thread; each item fits one 50 ms slice". §10.2.5 worst case: 1.0 s + 0.25 s + 0.5 s = 1.75 s ≤ 2 s. W-persist-10 chose a crash-safe 'opened and held like `instance.lock`' lock for the comparable mirror.lock.

**Fix applied.** TDD §11.3.3, 'The store' sub-bullet: replace "(created exclusively, retries for 1 s, as for `projects.json`, §10.1)" with "(opened and held like `instance.lock`, §10.5, so a crash never leaves it held; the I/O-thread item never waits on it: while another process holds it, the item re-posts itself through `PostIo` every 50 ms for up to 1 s, and if it still cannot take the lock the change is not saved and the Trusted folders page says \"Could not save trusted folders; try again\" (**proposed**))". TDD §11.12 T-TRU-006, result cell: append "; with `Trust.json.lock` held by another process for 5 s, Trust this folder reports that it could not save, and no journal commit waits behind it for more than one 50 ms slice".

**Patch.** TDD §11.3.3 store: Trust.json.lock is opened and held like instance.lock, so a crash never leaves it held. The I/O item re-posts itself through PostIo every 50 ms for up to 1 s. On failure the change is not saved and 'Could not save trusted folders; try again' (proposed) is shown. I made one small clarification: the message appears on 'the trust banner or the Trusted folders page that asked for it', so that it matches the test. T-TRU-006 adds the case of the lock held for 5 s.

### Round 3 — Delivery and tests

I checked the round-2 delivery patch and found six minor issues, no critical or major ones. The patch is in place at every listed location and the generated parts are current. Tool results: integ/check.py reports PROBLEMS 0 (815 unique test rows, 248 SPEC IDs, 1381 links). gen.py's fill() output is byte-identical to the current TDD.md (815 tests, 0 gaps, 229 covered, 21 Later or Withdrawn), so §14.1, §14.3, §14.7 and Appendix A are current. I re-ran the round-2 lands-terms and first-milestone lint (verify3/patch/lint.py) against the current TDD with the new term set: 0 term flags and 0 first-milestone flags. A case-insensitive variant also gives 0. The same lint on TDD.before.md shows the 10 term flags and 3 first-milestone flags (SEC-03, SEC-08, AI-01) that the patch fixed, so the T-GAP-008 term list and its new clause match the current rows. Every G1, G2 and G3 test has first Lands at or before M1. Only T-UEI-004 (M2) and T-RLD-020 (M3) make G5 grow, as the new §14.3 sentence says. Every split Lands value has a matching "(from Mx)" marker in its row. Every Lands value the patch changed is consistent with §1.5, §15.2 and SPEC (T-ARCH-015, T-LIFE-012, T-VIEW-017, T-KEY-010/012/013, T-SUP-025, T-RUN-019, T-RUN-023, T-REPL-012, T-LSP-040, T-DBG-042, T-DBG-050, T-PER-036, T-CFG-011, T-E2E-001, T-E2E-004, T-GAP-007, T-GAP-014). The six issues: (1) the new §14.3 sentence, read literally, gives an empty test set for the Phase 0 go/no-go; (2) P0-2 still omits the run-scoped sleep shim and the tier-4 test infrastructure that the Phase 0 G2 run needs; (3) the new M4 cell "the parts not in M1 or M2" moves §11.7's M3 API wiring, and the M2 policy parts the persist patch added, to M4; (4) T-ARCH-004's new SEC-03 scan forbids the file-change path that reload-on-save itself uses; (5) the cut list names the moving tests, but no exit text releases them, and the M4 exit names three of them outright; (6) the G2 agent cell leaves out the IME agent that G2(5) needs. The 08 log, 07a, README and mockups have no delivery changes. Scratch files: scratchpad/verify4/delivery/ (gencmp.py, suites.py, cutscan.py, lint_ci.py) and scratchpad/verify4/delivery_r2.txt.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [X-delivery-01](#x-delivery-01) | minor | contradiction | confirmed | The new sentence defines what a gate means when the Phase 0 go/no-go names it: "the tests of the gate's automated suite whose first Lands is at or before that milestone". |
| [X-delivery-02](#x-delivery-02) | minor | patch incomplete | modified | W-delivery-01 added the output store, the ingest thread, the output line source and FSumiDeferral to P0-2, but two things the Phase 0 G2 run in the Run window needs are still in no Phase 0 row. |
| [X-delivery-03](#x-delivery-03) | minor | patch wrong | modified | W-delivery-08 replaced M4's "§11.1–§11.8, §11.10" with "§11.1–§11.8 (the parts not in M1 or M2)". |
| [X-delivery-04](#x-delivery-04) | minor | contradiction | confirmed | W-delivery-10's SEC-03 scan allows Submit callers reached from "reload-on-save" and forbids any reached from a "file-change handler". |
| [X-delivery-06](#x-delivery-06) | minor | patch incomplete | modified | W-delivery-13 rewrote the G2 agent cell to name every agent class that G2 runs on, but left out the IME agent. |

#### X-delivery-01

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §14.3 Gate suites, intro paragraph (sentence added by W-delivery-S02); TDD §15.1 Go/no-go paragraph and P0-1/P0-2 Tests column; TDD §5.15.1 intro

**Problem.** The new sentence defines what a gate means when the Phase 0 go/no-go names it: "the tests of the gate's automated suite whose first Lands is at or before that milestone". For the go/no-go, that milestone is Phase 0, and §1.5 orders Phase 0 < M1. Every G1 and G2 test has first Lands M1, so read literally the G1 and G2 suites at the go/no-go are empty. This is the no-fallback gate (RV-13). The same sentence contradicts §15.1's Tests column (P0-1 runs the G1 suite; P0-2 runs "G2 automated suite in the Run window (T-LIFE-012 without ...)") and §5.15.1 ("The Run window pass runs from Phase 0 (P0-2)"). If CI computes Phase 0 status from §14.3, it gets nothing.

**Evidence.** §14.3: "Where a milestone exit (§15.2) or the Phase 0 go/no-go (§15.1) names a gate, it means the tests of the gate's automated suite whose first Lands is at or before that milestone, run with only the split steps that have joined by then (§1.5)." §1.5: "The order is the SPEC's: Phase 0 < M1 < M2 ...". Suite listing: all 19 G1 tests and all 27 G2 automated tests have first Lands M1 (T-LIFE-012 is 'M1 (M2: search thread)', T-SUP-008 is 'M1 (M4: Live (docked) pass)'). §1.5 itself says the Phase 0 spikes run "the tests named in the Tests column of §15.1".

**Refuter.** TDD.md:8283 (§14.3 intro, added by W-delivery-S02) says that when the Phase 0 go/no-go names a gate, it means the gate's tests "whose first Lands is at or before that milestone". Lands values are M1 to Later only, and §1.5 orders Phase 0 < M1. I checked every G2 automated-suite test (all 27) and all of them have first Lands M1; every G1 test is M1 as well. Read literally, the G1 and G2 suites at the go/no-go are empty. That contradicts §15.1 (the P0-1 Tests column runs the "G1 suite"; P0-2 runs the "G2 automated suite in the Run window ..."), the §5.15.1 intro ("The Run window pass runs from Phase 0 (P0-2)") and §1.5 ("the Phase 0 spikes run the tests named in the Tests column of §15.1"). This is the RV-13 no-fallback gate, so CI must not derive an empty suite for it. The proposed fix is consistent with the §15.1 Tests column and with the T-LIFE-012 split.

**Fix applied.** TDD §14.3 intro. Replace "Where a milestone exit (§15.2) or the Phase 0 go/no-go (§15.1) names a gate, it means the tests of the gate's automated suite whose first Lands is at or before that milestone, run with only the split steps that have joined by then (§1.5)." with "Where a milestone exit (§15.2) names a gate, it means the tests of the gate's automated suite whose first Lands is at or before that milestone, run with only the split steps that have joined by then (§1.5). Where the Phase 0 go/no-go (§15.1) names G1 or G2, it means the tests its Tests column names: the whole G1 automated suite, and the G2 automated suite in the Run window without T-LIFE-012's journal-writer, LSP and search-thread checks (§5.15.1)."

**Patch.** TDD §14.3 intro, a hand-written sentence outside the generated table: the milestone-exit rule is split from the Phase 0 go/no-go rule. At the go/no-go, G1 and G2 mean the tests named in the §15.1 Tests column (all of G1; G2 in the Run window without T-LIFE-012's journal-writer, LSP and search-thread checks).

#### X-delivery-02

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §15.1 table, row P0-2, Builds and TDD columns; TDD §5.15.1 T-CAN-001 (G2(1) shape `time.sleep(60)`); TDD §5.3.5 Swaps table, row `time.sleep`; TDD §5.10.3 Run-scoped sleep shim; TDD §14.2 Editor test fixtures, SumiTestLatch, SumiTestProject

**Problem.** W-delivery-01 added the output store, the ingest thread, the output line source and FSumiDeferral to P0-2, but two things the Phase 0 G2 run in the Run window needs are still in no Phase 0 row. (a) The run-scoped sleep shim. T-CAN-001's `time.sleep(60)` shape can only meet p99 ≤ 250 ms if `time.sleep` is swapped for the sliced sleep (§5.3.5 → §5.10.3). `SumiTestLatch`, which T-SUP-034 waits on, is built on `sumi_editor.sleep(0.01)`. §5.10 is outside P0-2's TDD column (§5.1–§5.7), and Builds lists the `unreal.log*` shims but not the sleep shim. (b) The tier-4 test infrastructure: the `Sumi.G2.*` specs in `SumiEditorTests`, the §14.2 fixtures the G2 rows call (`TickCoreTicker` in T-SUP-002, `CallPythonOverride` in T-SUP-003, the core-ticker UFUNCTION in T-ENTRY-004, latches and run-state probes), and `SumiTestProject` (the EUW whose Blueprint tick calls Python, for T-SUP-008). P0-1 lists its own harnesses (`Sumi.Perf.CodeView`, `Sumi.IME.Composition`). P0-6 builds only `SumiPyHarness` (tiers 1–3), and P0-4 only the G5 prototype. As written, the Phase 0 plan omits work the no-fallback exit depends on.

**Evidence.** T-CAN-001: "... `time.sleep(60)`. Stop by the input bot after the first pump ... p99 ≤ 250 ms over 200 stops per shape". §5.3.5: "| `time.sleep` in the `time` module | `_impl.task.sliced_sleep` (§5.10.3) | D2 Stop during sleep |". §5.14: "`time.sleep(600)` on the game thread | — | Sliced sleep | Live UI; Stop works". §14.2 SumiTestLatch: "`_sumi_test.wait(name)`: a pumped wait loop built on `sumi_editor.sleep(0.01)`". P0-2 Builds ends "... the `unreal.log*` shims; `FSumiDeferral` (§3.7) ...; `FSumiOutputStore` ...". TDD column: "§3.6, §3.7, §4.14 (output line source), §5.1–§5.7, §6.1, §6.2".

**Refuter.** Both parts are real. (a) T-CAN-001's G2(1) shape `time.sleep(60)` can meet p99 ≤ 250 ms only through the §5.3.5 swap to `_impl.task.sliced_sleep` (§5.10.3). Without the swap, Stop waits out the whole sleep. `SumiTestLatch` (§14.2), which T-SUP-034 and the probe/latch synchronisation rule of §5.15.1 need, is built on `sumi_editor.sleep(0.01)`, and §11.7 routes that to the same run-scoped sleep. §5.10.3 is outside P0-2's TDD column (§5.1–§5.7), and Builds does not list it. (b) The P0-2 Tests column runs tier-4 G2 tests that need the `SumiEditorTests` fixtures: `TickCoreTicker` (T-SUP-002; T-ENTRY-004's "fixture UFUNCTION ticking the core ticker" is the same fixture), `CallPythonOverride` (T-SUP-003), the delegate/override fixtures of T-ARCH-011, and the latches and run-state probes. They also need `SumiTestProject` (the EUW of T-SUP-008, PIE and the import corpus). No Phase 0 row builds any of this, while P0-1, P0-4 and P0-6 do list their own harnesses. W-delivery-01 was meant to make P0-2 build what the Phase 0 G2 run needs, so this gap is in scope. Corrections to the proposed fix: "the core-ticker UFUNCTION" duplicates `TickCoreTicker`, so I dropped it, and I refer to the §14.2 fixture set as a whole instead of a partial list.

**Fix applied.** TDD §15.1, row P0-2. Builds: after "and the Run window's output tail on SCodeView's output line source (§4.14)" append "; the run-scoped sleep shim behind the `time.sleep` swap and `sumi_editor.sleep` (§5.10.3); the tier-4 `Sumi.G2.*` specs in `SumiEditorTests` with the §14.2 editor test fixtures, `SumiTestLatch` and the run-state probes, on the `SumiTestProject` fixture, with Stop injected by the P0-4 input bot". TDD column: replace "§3.6, §3.7, §4.14 (output line source), §5.1–§5.7, §6.1, §6.2" with "§3.6, §3.7, §4.14 (output line source), §5.1–§5.7, §5.10.3, §6.1, §6.2, §14.2".

**Patch.** TDD §15.1 P0-2: Builds now adds the run-scoped sleep shim behind the time.sleep swap and sumi_editor.sleep (§5.10.3), the tier-4 Sumi.G2.* specs in SumiEditorTests with the §14.2 fixtures, SumiTestLatch and the run-state probes on SumiTestProject, with Stop injected by the P0-4 input bot. The TDD column adds §5.10.3 and §14.2.

#### X-delivery-03

**Minor, patch wrong.** Verdict: modified.

**Where:** TDD §15.2 M4 row, TDD sections: "§5.5.4, §9.5, §11.1–§11.8 (the parts not in M1 or M2)"; TDD §15.2 M3 row, TDD sections: "§5.9.6, §5.10.4, §8.1–§8.5, §8.8, §8.9, §9.1–§9.4"; TDD §15.2 M2 row, Scope and TDD sections: "§11.2 (minimal resolver for `debug.allow_debug_server`, with its §11.2.3 reload and swap)"; TDD §11.7 table rows `revert_module_state()` and `task`; SPEC API-01 Lands "M1; M3 (task, revert_module_state)"

**Problem.** W-delivery-08 replaced M4's "§11.1–§11.8, §11.10" with "§11.1–§11.8 (the parts not in M1 or M2)". That cell now also takes §11 parts that land in M3, because no M3 row lists any §11 part: §11.7's `task` and `revert_module_state()` wiring (API-01 M3; the M1 cell now names only "the M1 API"). It also takes the M2 policy parts that W-persist-03 and W-persist-15 added to §11.2 (the §11.2.3 studio-layer Startup rules with `PolicyCache/studio.json`, the M2 Gate "Waiting for policy", and the machine `studio_policy_path`). The M2 cell names only "reload and swap" of §11.2.3, and its Scope says "for `debug.allow_debug_server` only". The rewritten §1.5 derives Lands from "the §15.2 Scope and TDD sections columns", so these cells now give M4 for T-RLD-014 (M3, uses `sumi_editor.revert_module_state()`), the T-TASK tests and T-AUD-007 (M2, checks "Waiting for policy" and the studio cache). That contradicts their Lands.

**Evidence.** M1 cell: "§11.7 (the M1 API and the Sumi.Open, Sumi.RunFile and Sumi.ClearOutput commands)". §11.7 table: "| `revert_module_state()` | `_impl.reload` | GT | Inline | Enqueued as a `reload` run |", "| `task` | `_impl.task` (M3) |". §11.2 Lands: "It also ships the §11.2.3 Startup rules for the studio layer (the 5 s timeout, `PolicyCache/studio.json`, and a missing configured file treated as unreachable) and the M2 part of the Gate." plus "plus the machine layer's `studio_policy_path`". T-AUD-007 (M2): "the command is disabled with \"Waiting for policy\" until the 5 s timeout". §1.5: "the milestone of each feature the test's steps need (the §15.2 Scope and TDD sections columns)".

**Refuter.** Real. W-delivery-08 changed the M4 cell to "§11.1–§11.8 (the parts not in M1 or M2)". No M3 row lists any §11 part, so the §11.7 rows for `task` (marked "(M3)" in the table itself) and `revert_module_state()` (API-01 "M1; M3 (task, revert_module_state)"; §12.2 "those two land in M3") now fall to M4. W-persist-03 and W-persist-15 (round 2) added the following to §11.2 Lands for M2: the studio-layer Startup rules (5 s timeout, `PolicyCache/studio.json`), "the M2 part of the Gate" (§11.2.3 Gate: "From M2 ... 'Waiting for policy'") and the machine `studio_policy_path`. They did not update the §15.2 M2 TDD cell, which still names only "its §11.2.3 reload and swap". Under the rewritten §1.5 rule (the latest of the feature milestones from the §15.2 Scope and TDD sections columns), T-AUD-007 ("in an M2 build", checks "Waiting for policy" and the studio cache) derives M4. W-persist-12 (round 2) also put an M4 part (`storage.root_override` from the machine and studio layers) into the §10 feature table. The M1 cell "§10 (except the §10.4 UI and the M2 parts ...)" still schedules that part at M1, and the M4 cell names no §10. Corrections to the proposed fix: the M2 Scope edit is unnecessary, because the TDD sections cell is the operative one and "for debug.allow_debug_server only" correctly describes the resolved key. I added the §11 feature table's M2 row, which repeats the same incomplete phrase.

**Fix applied.** TDD §15.2. (1) M2 row, TDD sections: replace "§11.2 (minimal resolver for `debug.allow_debug_server`, with its §11.2.3 reload and swap)" with "§11.2 (the minimal resolver of its Lands paragraph: `debug.allow_debug_server` and the machine `studio_policy_path`, with the §11.2.3 studio-layer Startup rules, the M2 Gate, reload and swap)". (2) M3 row, TDD sections: replace "§5.9.6, §5.10.4, §8.1–§8.5, §8.8, §8.9, §9.1–§9.4" with "§5.9.6, §5.10.4, §8.1–§8.5, §8.8, §8.9, §9.1–§9.4, §11.7 (`task` and `revert_module_state()`)". (3) M4 row, TDD sections: replace "§5.5.4, §9.5, §11.1–§11.8 (the parts not in M1 or M2)" with "§5.5.4, §9.5, §10 (studio retention controls; `storage.root_override` from the machine and studio layers), §11.1–§11.8 (the parts not in M1, M2 or M3)". (4) M1 row, TDD sections: replace "§10 (except the §10.4 UI and the M2 parts of §10.14–§10.15)" with "§10 (except the §10.4 UI, the M2 parts of §10.14–§10.15 and the M4 rows of the §10 feature table)". TDD §11 feature table, M2 row: replace "for `debug.allow_debug_server` only, with its §11.2.3 reload and swap" with "for `debug.allow_debug_server` only (plus the machine `studio_policy_path` that locates the studio layer), with its §11.2.3 studio-layer Startup rules, M2 Gate, reload and swap".

**Patch.** TDD §15.2: the M2 TDD sections cell names the minimal resolver of its Lands paragraph (debug.allow_debug_server, machine studio_policy_path, studio-layer Startup rules, M2 Gate, reload and swap). The M3 cell adds §11.7 (task and revert_module_state()). The M4 cell adds §10 (studio retention controls; storage.root_override from the machine and studio layers) and now reads 'the parts not in M1, M2 or M3'. The M1 cell excludes the M4 rows of the §10 feature table. The §11 feature-table M2 row is covered by the X-persist-06 edit to the same text.

#### X-delivery-04

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD §2.8 T-ARCH-004, What it checks; TDD §9.3 Reload-on-save, Watching bullet; SPEC §7.18 SEC-03

**Problem.** W-delivery-10's SEC-03 scan allows Submit callers reached from "reload-on-save" and forbids any reached from a "file-change handler". Reload-on-save is itself started by a file-change handler: §9.3 registers an `IDirectoryWatcher` directory-changed callback for saves made outside Sumi. SEC-03 states it as the exception ("no run-on-... -file-change except opt-in reload-on-save"). From M3, the scan as written either flags the legitimate reload-on-save path or has to special-case it without saying so.

**Evidence.** T-ARCH-004: "a source scan that every call of `FSumiRunService::Submit` is reached from a user command, the public API (§11.7), a console command or the console executor, or reload-on-save, and none from a startup, tab-open or file-change handler (SEC-03 code-path audit)". §9.3: "**Watching:** `IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle(Root, Delegate, Handle, 0)` ... The callbacks run on the game thread". SEC-03: "no run-on-open, -startup or -file-change except opt-in reload-on-save (RLD-02)".

**Refuter.** T-ARCH-004 (TDD.md:597, added by W-delivery-10) allows Submit callers reached from "reload-on-save" and forbids callers reached "from a startup, tab-open or file-change handler". §9.3 starts reload-on-save from an `IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle` callback: a file-change handler that feeds the debounce ticker, which then submits the `ReloadOnSave` run. SEC-03 states it as the exception ("no run-on-open, -startup or -file-change except opt-in reload-on-save"). A reachability scan that follows the callback therefore meets both clauses for the same call site. The scan must also tell FSumiReloadOnSave's callback apart from other IDirectoryWatcher users (the §10.15 external-change handler). The proposed one-clause fix removes the contradiction. Restricting the scan to shipped code is also a reasonable clarification, since tier-4 tests may submit runs directly.

**Fix applied.** TDD §2.8 T-ARCH-004, What it checks: replace "and none from a startup, tab-open or file-change handler (SEC-03 code-path audit)" with "and none from a startup or tab-open handler, or from a file-change handler other than FSumiReloadOnSave's (§9.3) (SEC-03 code-path audit; `SumiEditorTests` is not scanned)".

**Patch.** TDD §2.8 T-ARCH-004: Submit may not be reached from a startup or tab-open handler, or from a file-change handler other than FSumiReloadOnSave's (§9.3). SumiEditorTests is not scanned.

#### X-delivery-06

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §13.4 G2 row, Agent and cadence: "GPU agents, and the Win64 harness agent for the tier-3 tests; every build from Phase 0; soak nightly"; TDD §14.1 Agents table, Windows IME agent row (Jobs: "G5 on every build; G1(3)") and GPU-2/GPU-3 row; TDD §5.15.1 T-SUP-006; TDD §4.10 T-IME-007

**Problem.** W-delivery-13 rewrote the G2 agent cell to name every agent class that G2 runs on, but left out the IME agent. G2(5) includes MS-IME Japanese composition (T-SUP-006, and T-IME-007, both G2 and both M1, so also in Phase 0). In §14.1 only the Windows IME agent has MS-IME Japanese. The GPU agents are "same hardware and image as GPU-1", with no IME listed, and the IME agent's jobs do not include G2. Agents provisioned as written cannot run the IME cases of the G2 suite, which the no-fallback Phase 0 exit and the M1 exit require.

**Evidence.** T-SUP-006: "G2(5) input-leak suite: typing, MS-IME JP composition, clicks ...". T-IME-007: "During a run, inside the G2(5) leak suite | Context inactive; nothing inserted". §14.1: "**Windows IME agent** (MS-IME Japanese, Microsoft Pinyin, Korean; ...) | 1 | G5 on every build; G1(3)"; "**Functional and soak GPU agents** `GPU-2`, `GPU-3` (same hardware and image as `GPU-1`) | 2 | ... G2 without the soak per build ...".

**Refuter.** Real. G2(5) includes MS-IME Japanese composition (T-SUP-006: "typing, MS-IME JP composition, ..."; T-IME-007 runs "inside the G2(5) leak suite", checking that the context is inactive and nothing is inserted). Both are M1 and run on every build from Phase 0. §14.1 lists MS-IME Japanese only for the Windows IME agent, whose jobs are "G5 on every build; G1(3)". GPU-2 and GPU-3, which run "G2 without the soak per build", are "same hardware and image as GPU-1", and no IME is listed for that image. W-delivery-13 rewrote the G2 agent cell to name every agent class G2 needs, so this gap is in that patch's scope. The verifier gave two alternatives, and a writer needs one. T-SUP-006 is a single leak-suite test with many inputs injected at set times, so moving only its IME cases to the IME agent would split the test. The smallest fix is to state the GPU image's IME.

**Fix applied.** TDD §14.1 Agents table, row "Functional and soak GPU agents": replace "(same hardware and image as `GPU-1`)" with "(same hardware and image as `GPU-1`; the image includes MS-IME Japanese for the G2(5) composition cases of T-SUP-006 and T-IME-007)".

**Patch.** TDD §14.1 GPU-2/GPU-3 row: the image includes MS-IME Japanese for the G2(5) composition cases of T-SUP-006 and T-IME-007.

### Round 3 — UX and mockups

Round 3, ux lens. I checked all 11 round-2 ux entries against the word diffs, the four re-rendered PNGs (01, 05, 07 and 10, each compared with its backup-r2 version), the 5.8 engine source, and an engine-Python probe of the REPL goldens (scratch in verify4/ux/).

The four images match their HTML, and the before/after changes are what the patch describes.

W-ux-01: every engine fact the patch cites checks out:
- UObject::Rename calls Modify() unless REN_NonTransactional (Obj.cpp:399).
- RenameSingleObject reaches UObject::Rename through UEditorEngine::RenameObject (EditorObject.cpp:114-118), passing REN_None or REN_DontCreateRedirectors (ObjectTools.cpp:4635).
- The soft-path fix-up calls Modify(true) on each referencer (AssetRenameManager.cpp:672), and the renamed packages are saved at :1992-2006.
- Undo LowLevelRenames the object back and renames a colliding object aside (Obj.cpp:1787-1831).
- ResetTransaction is at EditorEngine.h:1067, and the delete resets are at ObjectTools.cpp:2548 and 3505.
- Reset cancels an active transaction and empties the buffer (EditorTransaction.cpp:1379-1402), and the "Can't undo after" text is at 1490-1495.

Check 0, the Cleared enum value, SPEC RUN-07, J2, SNP-10 and §9.5, T-E2E-002, the D.3 row and mockups 01, 05 and 07 all follow the new contract. The patch is still incomplete in three ways:
- When a rename leaves no redirector (the usual case without source control, and likely T-TXN-015's fixture), the engine resets the undo buffer itself in the middle of the run. That path broadcasts no OnAssetsPreDelete, so §5.9.3 labels the run before End, and the documents never say that Cleared overrides that label. T-TXN-015's check-0 expectation depends on it.
- The task case is contradictory: no Undo this run versus a walk-back over later steps.
- SPEC §9.4, TDD §5.9.3, the §6.4 footer and the TDD SNP-10 bullet still promise "transaction kept" with Undo this run, or give no text for a Cleared run.

W-ux-02: the new consume-list rows check out against LevelEditorActions.cpp and LevelEditor.cpp: Ctrl+X Cut, Shift+End and Ctrl+End, Ctrl+Shift+PgUp/PgDn, and the typed keys H, Shift+H, K, M, T, /, Shift+/, Shift+V, Shift+G, Shift+E and the 14 Shift+letter surface selections. V is correctly left out, because it is not mapped. PyCharm's F2 maps Rename at LevelEditor.cpp:1101. The exception is "Shift+1 to Shift+4": it comes from commented-out code. The live editor-mode chords run from Shift+1 up to Shift+9, one per visible mode, so T-CFG-011, a G5 test, can fail.

The other patches are correct and complete:
- W-ux-03: the PyCharm list, the Preset cells and the new F2 row.
- W-ux-04: T-CFG-011's split Lands follows TDD 1.5.
- W-ux-05: the RUN-07 and OUT-02 wording and callout 2.
- W-ux-06: the 5.8 message "RenameAsset failed: The destination path '%ls' already exists." is at EditorAssetSubsystem.cpp:1043, and the assert line fits on line 14.
- W-ux-07: on the engine Python, `if x: pass  # note:` and `while x: y = 1  # :` classify as complete, and `if x:  # c` as incomplete.
- W-ux-08: the transaction-notes bullet and the three flags.
- W-ux-09: the "Proposed" legend.
- W-ux-10: the T-PER-028 states match §10.7.2, and PER-03 was added to Covers.
- W-ux-11: ForceDeleteObjects broadcasts OnAssetsPreDelete (ObjectTools.cpp:3978). After the replace step, the transaction's LoadOuter reference to the package survives, so CleanupAfterSuccessfulDelete resets with "Delete Selected Item", and T-TXN-016 can pass.

No critical or major issues found. All four issues below are minor.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [X-ux-01](#x-ux-01) | minor | wrong engine fact | modified | The W-ux-02 patch says the editor-mode chords are Shift+1 to Shift+4. |
| [X-ux-02](#x-ux-02) | minor | patch incomplete | modified | The patch assumes Sumi's transaction keeps the rename record until End. |
| [X-ux-03](#x-ux-03) | minor | contradiction | modified | The text added for W-ux-01 contradicts itself for tasks. |
| [X-ux-04](#x-ux-04) | minor | patch incomplete | modified | W-ux-01 made an error or cancelled run that renamed assets 'not undoable · undo history cleared', with no Undo this run. |

#### X-ux-01

**Minor, wrong engine fact.** Verdict: modified.

**Where:** TDD §11.1.4 'Shadowed Unreal shortcuts page', Fixed rows bullet: "... Shift+E, Shift+1 to Shift+4 and the surface-selection Shift+S/B/T/J/C/W/U/Y/Q/M/R/O/A/X (`LevelEditorActions.cpp:4486-4708`, mapped in `LevelEditor.cpp`)"; TDD §11.12 T-CFG-011 pass criterion: "Rows equal the golden list, the fixed rows and the extension rows of §11.1.4"; SPEC §9.3 'Text-editing keys (consume list, CFG-07)' row: "Shift+1–4 (editor modes)"; mockups/10-settings-and-trust.html line 389 Typed characters row: "H, Shift+H, K, M, T, /, Shift+1–4, Shift+letters" (images/10)

**Problem.** The W-ux-02 patch says the editor-mode chords are Shift+1 to Shift+4. In 5.8 those four chords exist only in commented-out code. The live editor-mode chords run from Shift+1 up to Shift+9, one per visible editor mode. The page lists whatever ComputeShadowed finds, so on an editor with more than four visible modes the Typed characters row holds more chords than the §11.1.4 fixed row. T-CFG-011 is in the G5 suite and compares the page with that row, so it can fail. SPEC §9.3 and image 10 under-report the shadowed mode chords.

**Evidence.** Engine/Source/Editor/LevelEditor/Private/LevelEditorActions.cpp:4692-4699: the BspMode, MeshPaintMode, LandscapeMode and FoliageMode UI_COMMANDs with FInputChord(EModifierKey::Shift, EKeys::One..Four) are commented out, inside `//if (FParse::Param( FCommandLine::Get(), TEXT( "editortoolbox" ) ))`. The live chords come from FLevelEditorModesCommands::RegisterCommands (Engine/Source/Editor/LevelEditor/Private/LevelEditorModesActions.cpp:17-41): `EdModeKeys = { EKeys::One, ..., EKeys::Nine }`, giving `DefaultKeyBinding = FInputChord(EModifierKey::Shift, EdModeKeys[editorMode])` for each visible mode in priority order. SLevelEditor::RefreshEditorModeCommands maps them on LevelEditorCommands (SLevelEditor.cpp:1924-1957), the same list that GetLevelEditorActions() returns and that ComputeShadowed walks. ControlRig (Animation mode) and MeshPainting are EnabledByDefault, so a default editor typically shows more than four modes.

**Refuter.** The issue is real, and the round-2 patch introduced it (W-ux-02). In UE_5.8 the only Shift+One..Four UI_COMMANDs in LevelEditorActions.cpp sit in comment lines 4695-4698, under the commented-out `//if (FParse::Param(... "editortoolbox"))`. The live editor-mode chords come from FLevelEditorModesCommands::RegisterCommands. That function uses EdModeKeys One..Nine, and each visible mode, in priority order, gets FInputChord(Shift, EdModeKeys[editorMode]) (LevelEditorModesActions.cpp:17-44). SLevelEditor::RefreshEditorModeCommands (SLevelEditor.cpp:1924-1960) maps those commands on SLevelEditor's LevelEditorCommands. ILevelEditor::GetLevelEditorActions() returns that same list, and §11.1.4 says ComputeShadowed walks it. So a default editor shadows Shift+1 to Shift+n, and n is usually above 4. The §11.1.4 claim and its 'mapped in LevelEditor.cpp' attribution are both wrong for these chords. SPEC §9.3 and mockup 10 inherit the error. The verifier's fix is right in substance, but its line ranges stop before the binding assignment (lines 42-43) and before the function's closing brace (line 1960). It also does not say that at most 9 modes get a chord.

**Fix applied.** TDD §11.1.4, Fixed rows bullet: replace "Shift+E, Shift+1 to Shift+4 and the surface-selection" with "Shift+E, Shift+1 to Shift+9 (one per visible editor mode in priority order, at most nine: `FLevelEditorModesCommands::RegisterCommands`, `Engine/Source/Editor/LevelEditor/Private/LevelEditorModesActions.cpp:17-44`, mapped by `SLevelEditor::RefreshEditorModeCommands`, `SLevelEditor.cpp:1924-1960`; the fixed Shift+1–4 mode commands at `LevelEditorActions.cpp:4695-4698` are commented out) and the surface-selection". TDD §11.12 T-CFG-011, pass criterion: after "per milestone (Ctrl+Shift+K from M2)" insert "; in the Typed characters row the editor-mode chords are Shift+1 to Shift+n, where n is the number of commands in `FLevelEditorModesCommands::Get().EditorModeCommands` that have a chord in the test editor". SPEC §9.3, 'Text-editing keys (consume list, CFG-07)' row: replace "Shift+1–4 (editor modes)" with "Shift+1–9 (editor modes, one per visible mode)". mockups/10-settings-and-trust.html line 389: replace "Shift+1–4" with "Shift+1–9" (same length), then re-render images/10 with mockups/render.ps1.

**Patch.** TDD §11.1.4 Fixed rows: the mode chords are now Shift+1 to Shift+9, one per visible mode in priority order, at most nine, citing LevelEditorModesActions.cpp:17-44 and SLevelEditor.cpp:1924-1960. It also notes that the Shift+1-4 lines at LevelEditorActions.cpp:4695-4698 are commented out (all three checked in the 5.8 source). T-CFG-011: in the Typed characters row, n is the number of EditorModeCommands that have a chord. SPEC §9.3: 'Shift+1–9 (editor modes, one per visible mode)'. mockups/10 line 389: Shift+1–4 changed to Shift+1–9. SPEC D.3 row added for X-ux-01 (§9.3).

#### X-ux-02

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §5.9.2 'Renames and moves' bullet: "Undoing that record renames the object back to its old name and outer (`Obj.cpp:1787-1831`) while its packages, the redirector and the registry stay moved." and "If the run or step had a transaction (status not Off), Sumi sets its status to Cleared"; TDD §5.9.3: "- **CancelledByScript** otherwise, for a user `cancel()` on a nested scope (07e Q1)."; TDD §5.15.5 T-TXN-015 pass criterion ("Undo Last Run refuses it with check 0's text")

**Problem.** The patch assumes Sumi's transaction keeps the rename record until End. That holds only when the rename leaves a redirector. Without one (an asset not under source control with no unloaded or map referencer, which is likely T-TXN-015's fixture), the engine deletes the emptied old package inside rename_asset. The rename record is that package's only referencer, so the engine resets the whole undo buffer at once, in the middle of the run. That cancels Sumi's transaction, and the run's later edits, such as T-TXN-015's lod_group set, are not recorded. §5.9.3 then labels the run on the TransactionCanceled event. No OnAssetsPreDelete fires, and the queue may have been empty before Begin, so the run gets CancelledByScript ("a user cancel() on a nested scope") or Purged, with a banner. §5.9.2 then sets Cleared 'if status not Off'. Nothing says Cleared overrides Purged or CancelledByScript, or what happens to that banner. T-TXN-015's 'Undo Last Run refuses it with check 0's text' passes only if Cleared wins.

**Evidence.** AssetRenameManager.cpp:105-110: bCreateRedirector defaults to false. It is set true only for non-local (source-controlled) assets or for unloaded or map referencers (:994, :1032, :1068). PerformAssetRename roots the old package when no redirector is left (:1855-1860), adds it to PotentialPackagesToDelete (:1880), then calls ObjectTools::CleanupAfterSuccessfulDelete(PotentialPackagesToDelete) (:2029-2036; bPerformReferenceCheck defaults to true, ObjectTools.h:313). The rename's Modify() serializes LoadOuter, the old package, into ReferencedObjects (Obj.cpp:1787-1797, save branch), and FObjectRecord::AddReferencedObjects reports it (EditorTransaction.cpp:594-611). GatherObjectReferencersForDeletion therefore finds the transactor as the only referencer, and ObjectTools.cpp:2541-2549 calls GEditor->ResetTransaction("Delete Selected Item"). UTransBuffer::Reset with ActiveCount != 0 calls Cancel(0) (EditorTransaction.cpp:1379-1394), which broadcasts TransactionCanceled for GUndo (:1428-1431). OnAssetsPreDelete is broadcast only at ObjectTools.cpp:1825, 3163 and 3978, never on this path.

**Refuter.** The mechanism checks out in the 5.8 source. FAssetRenameDataWithReferencers::bCreateRedirector defaults to false (AssetRenameManager.cpp:110). With no redirector, PerformAssetRename roots the old package (:1856), queues it in PotentialPackagesToDelete (:1880) and calls ObjectTools::CleanupAfterSuccessfulDelete before returning (:2028-2036). No path through it broadcasts OnAssetsPreDelete (the only broadcasts are at ObjectTools.cpp:1825, 3163 and 3978). The rename's Modify() serializes LoadOuter (the old package) in the transacting save branch (Obj.cpp:1838-1839). FPersistentObjectRef::AddReferencedObjects reports that package as RootObject (TransactionCommon.cpp:148-150), so GatherObjectReferencersForDeletion sees the transactor as the only referencer. That makes ObjectTools.cpp:2540-2549 call ResetTransaction("Delete Selected Item"), and UTransBuffer::Reset with ActiveCount != 0 calls Cancel(0), which broadcasts TransactionCanceled (EditorTransaction.cpp:1379-1394, 1428-1431). §5.9.3 then gives the run Purged or CancelledByScript: Purged only when the queue held entries before Begin. §5.9.2 then sets Cleared 'if status not Off'. Nothing says which status wins or whether the §5.9.3 banner still shows, and T-TXN-015's 'check 0's text' depends on the answer. Both texts come from round 2 (W-ux-01, W-ux-11). The verifier's fix is right, with two corrections. Its list of redirector causes is incomplete: collections (:1291), read-only referencers (:1322) and flagged soft references (:796) also force one, so the list should read 'for example'. And the reset does not happen 'at once' but inside the rename call, after the packages are saved.

**Fix applied.** TDD §5.9.2, 'Renames and moves' bullet: after "while its packages, the redirector and the registry stay moved." insert "That record outlasts the call only when the rename leaves a redirector, for example for a source-controlled asset, for a map, unloaded or read-only referencer, or for an asset in a collection (`AssetRenameManager.cpp:994, 1032, 1068, 1291, 1322`). Without a redirector, `rename_asset` deletes the emptied old package before it returns (`ObjectTools::CleanupAfterSuccessfulDelete`, `AssetRenameManager.cpp:2028-2036`). The undo buffer is that package's only referencer, so the engine resets the buffer (`ObjectTools.cpp:2540-2549`, reason \"Delete Selected Item\"). This cancels our transaction (§5.9.3), and the run's later edits are not recorded." In the same bullet, replace "Sumi sets its status to Cleared and, after End," with "Sumi sets its status to Cleared, which replaces a Purged or CancelledByScript status from §5.9.3 and suppresses that section's banner, and, after End,". TDD §5.9.3: replace "- **CancelledByScript** otherwise, for a user `cancel()` on a nested scope (07e Q1)." with "- **CancelledByScript** otherwise, for a user `cancel()` on a nested scope (07e Q1). A run or step in which an asset rename fired ends Cleared instead, with no banner (§5.9.2)." TDD §5.15.5 T-TXN-015, Expected: replace "offers no Undo this run, and Undo Last Run refuses it with check 0's text" with "offers no Undo this run, shows no \"Undo is unavailable\" banner, ends with status Cleared, and Undo Last Run refuses it with check 0's text".

**Patch.** TDD §5.9.2 Renames bullet: the rename record outlasts the call only when the rename leaves a redirector (AssetRenameManager.cpp:994, 1032, 1068, 1291, 1322). Without one, rename_asset deletes the emptied old package (CleanupAfterSuccessfulDelete, :2028-2036), and the engine resets the undo buffer (ObjectTools.cpp:2540-2549, 'Delete Selected Item'), which cancels our transaction. All of these were checked in the 5.8 source. Cleared replaces a Purged or CancelledByScript status and suppresses the §5.9.3 banner. §5.9.3: a run or step in which a rename fired ends Cleared with no banner. T-TXN-015 Expected: no 'Undo is unavailable' banner, and status Cleared.

#### X-ux-03

**Minor, contradiction.** Verdict: modified.

**Where:** TDD §5.9.2 'Renames and moves' bullet: "...and no Undo this run is offered. The reset also clears a task's earlier steps, so the walk-back reaches only steps after the last renaming step."; TDD §5.9.5 check 0: "| 0 | Status is not Cleared | ..."; TDD §6.4 Transaction notes bullet: "For a task, a flag set by any step is set on the record."; TDD §5.9.6 Walk-back: "A manual edit after step 12 blocks the rest."

**Problem.** The text added for W-ux-01 contradicts itself for tasks. Suppose step 5 of 40 renames assets. §5.9.2 says no Undo this run is offered, check 0 refuses a Cleared status, and §6.4 puts the rename flag (and so the 'undo history cleared' note) on the whole task record. Yet §5.9.2 also says the walk-back still reaches the steps after the last renaming step. A builder cannot tell whether that task offers Undo this run for steps 6–40. If it does, the §5.9.6 walk-back message would blame 'a manual edit' for the cleared steps.

**Evidence.** TDD §5.9.2: "If the run or step had a transaction (status not Off), Sumi sets its status to Cleared ... and no Undo this run is offered. The reset also clears a task's earlier steps, so the walk-back reaches only steps after the last renaming step." §5.9.5 check 0 refuses any record whose status is Cleared. §6.4: "For a task, a flag set by any step is set on the record." No test covers a renaming task step: T-TXN-015's task only reimports.

**Refuter.** This is real but narrower than claimed. The W-ux-01 sentence says the walk-back still reaches the steps after the last renaming step, so the intended behaviour is clear: a task keeps a partial walk-back. Two things are left undefined. First, 'If the run or step had a transaction ... sets its status to Cleared ... and no Undo this run is offered' does not say whose status check 0 reads for a task. §6.4's flag rule covers only the note flags, and §6.4 says a task's footer undoes 'k of N steps'. Second, §5.9.6 gives only the 'A manual edit after step 12 blocks the rest' wording. When a rename reset the buffer, the walk ends at the bottom of the buffer, not at a foreign entry, so that wording blames the wrong cause. The verifier's direction is right. Its message should apply only when the walk stops at the cleared point, because a foreign entry after the renaming step still blocks with the existing wording.

**Fix applied.** TDD §5.9.2, 'Renames and moves' bullet: replace "and no Undo this run is offered. The reset also clears a task's earlier steps, so the walk-back reaches only steps after the last renaming step." with "and a run offers no Undo this run. In a task, Cleared is the status of the renaming step, not of the task. The reset also clears the task's earlier steps, so the task's walk-back (§5.9.6) counts only the steps after the last renaming step. When the walk stops there rather than at a foreign entry, its message reads \"Can undo k of N recorded steps (steps a–b). Step s renamed assets, so the undo history before it was cleared.\"" TDD §5.9.5 table, row 0: replace the Check cell "Status is not Cleared" with "Status is not Cleared (for a task: the status of its last step that recorded changes)".

**Patch.** TDD §5.9.2: a run offers no Undo this run. In a task, Cleared is the renaming step's status, the walk-back counts only the steps after the last renaming step, and when the walk stops at the cleared point the message is 'Can undo k of N recorded steps (steps a–b). Step s renamed assets, so the undo history before it was cleared.' §5.9.5 check 0: for a task, the status of its last step that recorded changes.

#### X-ux-04

**Minor, patch incomplete.** Verdict: modified.

**Where:** SPEC §9.4 'Honest-state wording' bullet: "never \"rolled back\" for an error or cancel, but \"transaction kept\" with **Undo this run**"; TDD §5.9.3 last paragraph: "Errors and cancels keep the transaction, and the run header offers **Undo this run**."; TDD §6.4 footer table, Transaction (U3, RT-08) row: "`undo: \"Sumi: <tab> #N\" (k objects, size)`; `partly undoable (...)`; `no transaction (Run File)`; `undo unavailable: <reason>` (D8)"; TDD §11.5 SNP-10 bullet (line 7433): "and [Undo this run] when the run's transaction holds changes (RUN-22)"

**Problem.** W-ux-01 made an error or cancelled run that renamed assets 'not undoable · undo history cleared', with no Undo this run. Image 01's Run #14 is exactly that case: an error, with a 'not undoable' header. Several untouched places still state the old rule. SPEC §9.4 tells copywriters every error or cancel says 'transaction kept' with Undo this run, and TDD §5.9.3 says error and cancel headers offer Undo this run. The §6.4 footer Transaction field has no value for the new Cleared status, so a builder has no text for it; the mockups show 'not undoable'. The TDD SNP-10 design keys the notification's Undo on 'holds changes' rather than on the status SPEC SNP-10 now names.

**Evidence.** SPEC SNP-10 now says "[Undo this run] (absent when the run renamed or moved assets, RUN-07)". mockups/01 line 138 and mockups/07 line 354 render the header as "... · not undoable". TDD §5.9.7 adds ESumiTxnStatus::Cleared, but the §6.4 Transaction footer list has no Cleared entry. The SPEC §9.4 and TDD §5.9.3 sentences quoted in 'where' are unchanged from before round 2.

**Refuter.** The issue is real. Round 2 (W-ux-01) made a Cleared run show no Undo this run, and SPEC SNP-10 and mockups 01 and 07 were updated. Several sentences the patch did not touch still state the old rule. SPEC §9.4 line 726 says every error or cancel says 'transaction kept' with Undo this run. TDD §5.9.3 says errors and cancels offer Undo this run. TDD §11.5 SNP-10 (line 7433) offers [Undo this run] 'when the run's transaction holds changes', which no longer matches SPEC SNP-10. The verifier's fix misses two points. The §6.4 footer field should read 'not undoable', as mockups 01 and 07 render it ("· not undoable"). The verifier's 'not undoable · undo history cleared' would repeat the clause that the Transaction notes line already ends with. Also, §6.4 line 3646 says that when CanUndoLastRun fails 'the chip is disabled and its tooltip gives the reason', but §5.9.2, SPEC SNP-10 and the mockups have no chip at all for a Cleared run, so that sentence needs an exception too.

**Fix applied.** SPEC §9.4, 'Honest-state wording' bullet: replace "but \"transaction kept\" with **Undo this run**;" with "but \"transaction kept\" with **Undo this run**, or, for a run that renamed or moved assets, \"not undoable\" and \"undo history cleared\" (RUN-07);". TDD §5.9.3, last paragraph: replace "Errors and cancels keep the transaction, and the run header offers **Undo this run**." with "Errors and cancels keep the transaction, and the run header offers **Undo this run**, except for a Cleared run (§5.9.2)." TDD §6.4 footer table, Transaction (U3, RT-08) row, Text cell: replace "; `no transaction (Run File)`; `undo unavailable: <reason>` (D8)" with "; `no transaction (Run File)`; `not undoable` (status Cleared; the Transaction notes line gives the reason, §5.9.2); `undo unavailable: <reason>` (D8)". TDD §6.4, '**Undo this run** (U9) is enabled when' bullet: after "Otherwise the chip is disabled and its tooltip gives the reason." insert " A Cleared run (§5.9.2) shows no chip." TDD §11.5 SNP-10 bullet: replace "[Undo this run] when the run's transaction holds changes (RUN-22)." with "[Undo this run] when the run's transaction holds changes and its status is not Cleared (RUN-22, RUN-07)."

**Patch.** SPEC §9.4 honest-state wording adds the case of a run that renamed or moved assets: 'not undoable' and 'undo history cleared' (RUN-07). TDD §5.9.3: Undo this run is offered except for a Cleared run. §6.4 footer Transaction row adds `not undoable` (status Cleared). The §6.4 Undo this run bullet adds 'A Cleared run (§5.9.2) shows no chip.' §11.5 SNP-10 offers Undo only when the status is not Cleared. SPEC D.3 row added for X-ux-04 (§9.4).

### Round 3 — refuted

| ID | Issue | Why it was refuted |
|---|---|---|
| X-delivery-05 | W-delivery-09 lists the tests and steps that move with each cut item, and W-delivery-11 adds them to the 1.0.x and 1.1 exits. | The §15.3 column is titled "Tests that move with it", and each item has a "Moves to" release. Moving a test means it leaves the exit that held it and joins the exit of the release it moves to. W-delivery-11 made the joining side explicit (1.1: "the tests of each cut-list item moved to 1.1 (§15.3)"; 1.0.x: "the tests of cut-list items 2 and 6 if they were cut"). §1.5 defines Lands as "the first milestone whose exit requires the test", so once an item is cut, the M3 and M4 exits' "every test whose Lands is Mx" no longer picks up the moved tests. The documents already handle cuts this way elsewhere: T-GAP-003's own pass criterion reads "the Linux Preview job is green for the 1.0 sign-off, or cut-list item 1 is invoked", and the 1.0 GA exit requires "every cut-list item shipped or formally moved". T-GAP-008 already prints the per-item lists (W-delivery-09). Nobody building or running the plan would read a cut as leaving T-GAP-003 (Linux Preview) or the studio parts of T-E2E-007 in the M4 exit, so the added sentence changes nothing. |

## Round 4

Scope: the round-3 patch only, from an exact before/after diff.

**Patch.** All 11 confirmed issues are applied and none was skipped. The edits are in C:/Users/anima/Desktop/UnrealPythonEditor/docs/TDD.md and C:/Users/anima/Desktop/UnrealPythonEditor/docs/SPEC.md (§9.4, plus a new Y-ux-01 row in Appendix D.3). Beyond the approved fixes, I made a few small follow-on edits so the same contract reads the same everywhere: §8.5 Stop now says RequestStop has already run, so it is not called twice; §5.6.5 has an exception note for OnUserCancel; the §3.7 swap-client row lists the watcher registration; the studio-policy AccessCheck runs on SumiPolicyIO; the UntrustFolder comment matches TrustFolder; the D.3 intro names the X- and Y- rows; and T-DBG-031 no longer says 'the same breakpoint' now that it names two. 08, 07a, README and the mockups are unchanged, so no PNG was re-rendered. The generated TDD parts are current, and check.py prints 'PROBLEMS 0 {}'.

**Checks after the patch.** Final run of check.py: 'PROBLEMS 0 {}' (TDD headings 369 numbered 363, links checked 1381, SPEC ids 248, test rows 815 unique 815). No test row was added or removed and no Covers or Lands value changed. A dry run of gen.py's fill() gives output identical to the current TDD.md, so the generated parts are current and gen.py --write was not needed. SPEC.md, TDD.md and README.md contain no CR bytes (LF only). Every engine line citation added this round was re-checked in C:/Program Files/Epic Games/UE_5.8, read only: FeedbackContextEditor.cpp:441/471/702-710, SlowTask.cpp:235-237/248-258, DirectoryWatcherWindows.cpp:43-76/68/128/147-149, Linux/DirectoryWatchRequestLinux.cpp:79. No mockup HTML was edited, so no PNG was re-rendered. 08-decision-log.md, 07a and README.md are unchanged. Backups of the state before this patch: scratchpad/verify5/patch/ (SPEC.before.md, TDD.before.md, README.before.md, 08.before.md, 07a.before.md, mockups.before/). The edits were applied with exact-once replacement scripts in that folder (rep.py and p1.json to p15.json). One gap outside these findings, left as it is: §11.5.5 still does not say which thread writes Publish's studio-share output (manifest.lock, item and manifest).

### Round 4 — Runtime

The runtime part of the round-3 patch is applied as approved, with two residual gaps, both minor. X-runtime-01 is in place at all four locations its final_fix lists: §5.7.2 has the new 'Evaluate stop state' paragraph, and the §8.3.4 Cancel bullet, the §12.1.1 check_cancel row and T-DBG-031 (tier 4, Lands M3) were updated. These now agree with §5.4.1, §5.4.2, §5.4.5, §5.7.3–§5.7.6, §5.10.2, §8.10, SPEC DBG-03, RV-05 and mockup 05. Y-runtime-01: no text says how a run-level Stop gets to a hung evaluate or how it ends the pause. The Paused window's Stop is a queued `terminate` request, which waits for the evaluate (§8.1.2, §8.3.3). RequestStop only sets atomics, and the paused gate blocks delivery. So 'once it returns, the run stops' has no trigger, and the Stop button never reaches the evaluate. T-DBG-031 also does not say the input bot must send the key and click. Y-runtime-02: tying Cancel to the Stop buttons' RequestStop pulls in OnUserCancel. When the run is paused inside its own ScopedSlowTask, that call sets the editor's cancel flag, which is reset only when a root slow task starts. The run's should_cancel() then returns True after Continue (checked in FeedbackContextEditor.cpp and SlowTask.cpp). X-runtime-02 is correct and complete. All seven edits are in place (prop/container/struct_export_text rows, the §8.8 container and struct rows, T-INSP-014, T-INSP-015, T-API-016). The engine facts hold in source: IsPending() is Get()==nullptr && ObjectID.IsValid() (PersistentObjectPtr.h:218-221), Get() resolves without loading, FLazyObjectProperty::GetObjectPropertyValue uses Get(), FObjectPropertyBase::ExportText_Internal writes 'None' for null (PropertyBaseObject.cpp:267-283), and FUniqueObjectGuid::ToString exists. No other occurrence in SPEC, 08, 07a or the mockups contradicts this. The §3.8 Module shutdown sentence ('the storage probe thread was abandoned at OnPreExit and is not joined (§10.1)') matches the new §3.1 row and §10.1, and the engine facts behind it hold: GThreadPool->Destroy waits for its workers (ThreadingBase.cpp:1317-1352), UnloadModulesAtShutdown does not free module DLLs (LaunchEngineLoop.cpp:5176-5182), and nothing at exit joins FRunnableThreads. Diffs are in scratchpad/verify5/runtime/.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Y-runtime-01](#y-runtime-01) | minor | patch incomplete | modified | The round-3 rule says a run-level Stop reaches a hung evaluate through `RequestStop(RunId)` and that the run then stops. |
| [Y-runtime-02](#y-runtime-02) | minor | contradiction | modified | The round-3 Cancel text says that the evaluate's Cancel calls `RequestStop` 'as the Stop buttons do (§5.6.5)'. |

#### Y-runtime-01

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §5.7.2 'Evaluate stop state' paragraph; TDD §8.3.3 Requests from a clean point; TDD §8.1.2 The DAP-shaped interface, rows `evaluate…` and `continue … terminate`; TDD §5.1.3 Stop sources by state, row `Paused`; TDD §5.6.5 closing paragraph; TDD §5.4.2 step 1a; TDD §8.11 T-DBG-031; TDD §8.9.1 Driver bullet

**Problem.** The round-3 rule says a run-level Stop reaches a hung evaluate through `RequestStop(RunId)` and that the run then stops. The documents give no path for this in either direction. (a) The Paused window's own Stop (the button `pwStop` in mockup 05 and the `Paused` row of §5.1.3) is a `terminate` request. Like every Paused-window request it goes through `PendingRequests` (§8.1.2, §8.3.3), and a request queued during an evaluate waits until the evaluate returns. Nothing says that this Stop also calls `RequestStop(RunId)` straight away. A builder following §8.3.3 therefore gets a Stop button that never reaches `while True: pass`, which contradicts §5.7.2's 'or a Stop button' and §5.6.3's 'Shift+F5 and the Stop buttons are the run-level Stop in every context'. (b) The other way round, a Shift+F5 handled as §5.6.5 says ('only sets atomics') does cancel the evaluate. But the paused gate (§5.4.2 step 1a) means atomics alone never end a pause, and no text queues `terminate` after the evaluate. The run stays paused with its stop state set, and 'once it returns, the run stops as a Paused-window Stop does (§8.5)' has no trigger. (c) T-DBG-031 does not test the Stop button and does not say how the click and key reach the game thread during the evaluate. The documented Paused-window driver (§8.9.1) acts only at modal-loop iterations, and none occur while an evaluate runs.

**Evidence.** §5.7.2: '`RequestStop` with the paused run's ID (Shift+F5, Esc or a Stop button, §5.6) sets the run's state and, while an evaluate is active, the evaluate's copy too. The evaluate is cancelled first ("Evaluate cancelled"); once it returns, the run stops as a Paused-window Stop does (§8.5).' §8.3.3: 'UI handlers never call Python. A click or key queues a request in `SSumiPausedWindow::PendingRequests`.' and 'Requests queued during an evaluate, for example F10 pressed while "Evaluating…" shows, wait until the evaluate returns'. §8.1.2: 'like every request it goes through the queue (§8.3.3)' and '`continue`, `next`, `stepIn`, `stepOut`, `runToCursor`, `terminate` | Returned to the trace callback as the resume command (§8.5)'. §5.1.3: '`Paused` | Paused window Stop (§8), Shift+F5, Esc under RT-05'. §5.6.5: '`RequestStop` called from the pre-processor or a button only sets atomics, calls `OnUserCancel` (§5.12) and updates the UI. Delivery happens in the pending call (§5.4.2 step 9).' §5.4.2 step 1a: 'If `IsPaused()` and no supervised evaluate is active, return 0'. §8.9.1: '`FSumiPausedWindowTestDriver` … subscribes to `GetOnModalLoopTickEvent()`. Each modal-loop iteration it runs scripted actions'. §8.3.4: 'While it runs, the modal loop is not iterating'.

**Refuter.** The issue is real. The round-3 text in §5.7.2 and §8.3.4 says a run-level Stop during an evaluate calls RequestStop(RunId) at once, which cancels the evaluate, and that the run then stops 'as a Paused-window Stop does (§8.5)'. The documents give no route for this. §8.3.3 says 'A click or key queues a request', and §8.1.2 lists `terminate` as a queued request, so under the queue reading a Stop pressed during an evaluate waits for the evaluate to return. §5.6.5 says a pre-processor Stop 'only sets atomics', and §5.4.2 step 1a means atomics never end a pause, so nothing triggers the run's stop after the evaluate returns. The pump (§5.4.4, App.Tick with TickPlatform under a modal) does dispatch clicks and the pre-processor during the evaluate, so the immediate RequestStop is feasible; only the rule is missing. Part (c) also holds. The driver in §8.9.1 runs only on GetOnModalLoopTickEvent, and the pump does not broadcast that event. The §14.2 input bot is the tool that 'also acts while the game thread is inside a supervised run or a modal loop'. The proposed fix is right in substance with two wording problems. In (1), 'the first call' is unclear. In (2), 'it also queues' makes RequestStop itself queue `terminate`, which would make the evaluate's Cancel (also RequestStop, from a button) end the pause. The corrected fix names the caller and excludes Cancel.

**Fix applied.** TDD.md, four edits.
(1) §8.3.3 Requests from a clean point, last paragraph. After 'wait until the evaluate returns; a dialog the evaluate opens never drains them.' add: ' Stop is the exception. Shift+F5, Esc under §5.6.3 and the Paused window's Stop button call `RequestStop(RunId)` at once, from the pre-processor or the click handler (§5.6.5), and then queue `terminate`. During an evaluate, that immediate `RequestStop` cancels the evaluate (§5.7.2); the queued `terminate` waits until the evaluate returns and then stops the run (§8.5).'
(2) §5.6.5 Interface, closing paragraph. After '`RequestStop` called from the pre-processor or a button only sets atomics, calls `OnUserCancel` (§5.12) and updates the UI. Delivery happens in the pending call (§5.4.2 step 9).' add: ' While the run is paused, a run-level Stop (Shift+F5, Esc, the Paused window's Stop button) also queues `terminate` in the Paused window, which ends the pause (§8.3.3); the evaluate's Cancel does not (§8.3.4).'
(3) §5.7.2, 'Evaluate stop state' paragraph. Replace 'once it returns, the run stops as a Paused-window Stop does (§8.5).' with 'once it returns, the `terminate` that the Stop queued (§8.3.3) stops the run as a Paused-window Stop does (§8.5).'
(4) §8.11 T-DBG-031. In the Scenario cell, after 'where Shift+F5 is pressed during a `while True: pass` evaluate' add '; then a third run, where the Paused window's Stop button is clicked during a `while True: pass` evaluate. Every key and click is sent by the input bot (§14.2), because the Paused-window test driver acts only at modal-loop iterations (§8.9.1)'. In the Pass cell, replace 'In the second run: "Evaluate cancelled" within 1 s of Shift+F5, then the run ends as `cancelled`' with 'In the second and third runs: "Evaluate cancelled" within 1 s of the Stop, then, with no further input, the run ends as `cancelled`'. Lands stays M3.

**Patch.** TDD §8.3.3: Stop is the exception to the queue. Shift+F5, Esc under §5.6.3 and the Paused window's Stop button call RequestStop(RunId) at once and then queue `terminate`; during an evaluate the immediate call cancels the evaluate, and the queued `terminate` stops the run after the evaluate returns. §5.6.5: while paused, a run-level Stop also queues `terminate`; the evaluate's Cancel does not. §5.7.2: the run stops through the queued `terminate`. T-DBG-031: added a third run where the Stop button is clicked during the evaluate, with every key and click sent by the input bot (§14.2); pass is now 'In the second and third runs ... within 1 s of the Stop, then, with no further input, ... cancelled'. Two follow-on fixes: §8.5 Stop now says RequestStop(RunId) has already run and the trace callback raises when the queued `terminate` is applied, so the call is not doubled; in T-DBG-031, 'the same breakpoint' became 'the first of those breakpoints', because the row now names a second breakpoint. Lands stays M3.

#### Y-runtime-02

**Minor, contradiction.** Verdict: modified.

**Where:** TDD §8.3.4 Supervised evaluates, Cancel bullet; TDD §5.6.5 closing paragraph; TDD §5.12 row 'Stop while a user slow task is open'; TDD §8.9.1 G4(3) fixture

**Problem.** The round-3 Cancel text says that the evaluate's Cancel calls `RequestStop` 'as the Stop buttons do (§5.6.5)'. §5.6.5's `RequestStop` calls `OnUserCancel` (§5.12), and §5.12 makes that call whenever a user slow task is open. In the G4(3) situation, a run paused inside its own `ScopedSlowTask` dialog, cancelling a hung evaluate therefore sets the editor's cancel flag. That flag is reset only when a new root slow task starts. Nested slow tasks do not reset it, because `MakeDialog` calls `StartSlowTask` only while `GIsSlowTask` is false. After Continue, the run's `t.should_cancel()` returns True. The evaluate's Cancel then does reach the paused run, which contradicts 'it never reaches the paused run' in the same bullet and §5.7.2's 'sets only the evaluate's copy'.

**Evidence.** §8.3.4: 'The Cancel button calls `RequestStop` from its click handler, as the Stop buttons do (§5.6.5)'. §5.6.5: '`RequestStop` called from the pre-processor or a button only sets atomics, calls `OnUserCancel` (§5.12)'. §5.12: 'Stop while a user slow task is open | Also calls `FFeedbackContextEditor::OnUserCancel()` … Their `should_cancel()` then returns true'. Engine source: FeedbackContextEditor.cpp:707-710 `OnUserCancel() { HasTaskBeenCancelled = true; }`; :702-705 `ReceivedUserCancel` returns that flag; it is reset only at :471 inside `StartSlowTask`; SlowTask.cpp:235-237 calls `StartSlowTask` only `if (bIsDialogAllowed && !GIsSlowTask)`; SlowTask.cpp:248-258 `ShouldCancel` returns `Context.ReceivedUserCancel()`; PyCore.cpp:762-770 binds `should_cancel` to `SlowTask->ShouldCancel()`.

**Refuter.** The issue is real, and I checked the engine claims in the local install. FeedbackContextEditor.cpp:707-710 sets HasTaskBeenCancelled=true, and :702-705 returns that flag. The flag is reset only in the constructor (:442) and at :471 inside StartSlowTask. SlowTask.cpp:235-237 calls Context.StartSlowTask only `if (bIsDialogAllowed && !GIsSlowTask)`. SlowTask.cpp:248-257 ShouldCancel returns Context.ReceivedUserCancel() while GIsSlowTask is true. GIsSlowTask is set in FeedbackContext.h:83 and cleared at :96. PyCore.cpp:762-770 binds should_cancel to SlowTask->ShouldCancel(). The round-3 sentence 'as the Stop buttons do (§5.6.5)' points to RequestStop, which per §5.6.5 'calls OnUserCancel (§5.12)'. So in the G4(3) case (paused inside a ScopedSlowTask with its dialog), cancelling an evaluate sets the shared flag, and after Continue the run's t.should_cancel() returns True. That contradicts 'it never reaches the paused run'. The proposed fix has a grammar defect. Replacing only the clause before the semicolon leaves the orphan fragment 'it is not a `PendingRequests` entry…' after a full stop. It also says 'It calls OnUserCancel', where §5.6.5 makes RequestStop the caller. The test addition was left optional. The corrected fix replaces the whole sentence and commits to a test edit whose anchors do not overlap Y-runtime-01's T-DBG-031 edits.

**Fix applied.** TDD.md, two edits.
(1) §8.3.4 Supervised evaluates, Cancel bullet. Replace 'The Cancel button calls `RequestStop` from its click handler, as the Stop buttons do (§5.6.5); it is not a `PendingRequests` entry (§8.3.3), which would wait for the evaluate to return.' with 'The Cancel button calls `RequestStop(EvalId)` from its click handler, as the Stop buttons do (§5.6.5); it is not a `PendingRequests` entry (§8.3.3), which would wait for the evaluate to return. `RequestStop(EvalId)` calls `OnUserCancel` (§5.12) only when `GIsSlowTask` was false at `eval_begin`. Every open slow task shares the editor's cancel flag, which is reset only when a root slow task starts (`Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp:471`; `Engine/Source/Runtime/Core/Private/Misc/SlowTask.cpp:235-237`). With the paused run's slow-task dialog open, calling it would make that task's `should_cancel()` return true after Continue (`FeedbackContextEditor.cpp:702-710`, `SlowTask.cpp:248-258`).'
(2) §8.11 T-DBG-031. In the Scenario cell, replace 'cancelled with the Cancel button at a breakpoint stop and at an uncaught-exception stop;' with 'cancelled with the Cancel button at a breakpoint stop, at an uncaught-exception stop and at the G4(3) breakpoint inside a `ScopedSlowTask` with its dialog (§8.9.1);'. In the Pass cell, replace 'In both Cancel cases:' with 'In every Cancel case:', and replace 'then F5 continues the run at the breakpoint stop, which ends as `ok` with no `ScriptCancelled` delivered to it,' with 'then F5 continues the run at each breakpoint stop, which ends as `ok` with no `ScriptCancelled` delivered to it and, in the slow task, `t.should_cancel()` False after Continue,'. Lands stays M3.

**Patch.** TDD §8.3.4 Cancel bullet: the button calls RequestStop(EvalId), which calls OnUserCancel only when GIsSlowTask was false at eval_begin, with the engine citations (FeedbackContextEditor.cpp:471, 702-710; SlowTask.cpp:235-237, 248-258), all re-checked in the local 5.8 source. T-DBG-031 now has a Cancel case at the G4(3) ScopedSlowTask breakpoint; pass says 'In every Cancel case', 'continues the run at each breakpoint stop', and t.should_cancel() is False after Continue. Follow-on fix: §5.6.5's 'calls OnUserCancel (§5.12)' now adds 'for the evaluate's Cancel, only when §8.3.4 allows it', so the general rule does not contradict the exception.

### Round 4 — Persistence, policy and studio

I checked the round-3 persist patch with git diff --no-index --word-diff, comparing the verify4/patch/*.before files with fix/backup-r4. The backup matches the live docs. Every final_fix of X-persist-01 to -11 is in place at its listed locations. The integrator check.py reports 0 problems. One major and six minor issues remain.

Major: Y-persist-01. The X-persist-01 delete guard only protects a primary that is still running. Suppose a second editor becomes primary at a scanned root, then exits (cleanly or by a crash) before the user resolves the stale listing. Both locks are then free, so the delete removes that editor's newer journal, including dirty and scratch text kept across a clean exit and any pending recoveries. Fix: re-check the namespace's generation fingerprint while both locks are held, and add a T-PER-055 case.

Minor issues:
- Y-persist-02: X-persist-05 has the worker poll register IDirectoryWatcher itself. Registration is game-thread only in 5.8: Linux asserts IsInGameThread at DirectoryWatchRequestLinux.cpp:79, and on Windows the request map is unlocked and the completion APC goes to the registering thread.
- Y-persist-03: X-persist-09's new 'Tabs from another storage folder' section is missing from the §10.7.1 n = 0 banner condition and text, the §10.7.3 Review section list and the §10.7 diagram labels.
- Y-persist-04: the exit-join hazard that X-persist-10 removed for storage roots remains for the studio policy load (UE::Tasks), the 5-minute studio poll (the text X-persist-05 touched) and the studio mirror.
- Y-persist-05: on POSIX the new lock guard fails against a path alias of the session's own root, because fcntl locks never conflict within one process. The same applies to mirror.lock within one process.
- Y-persist-06: the §12.4 index lacks Trust.json.lock and projects.json.lock.
- Y-persist-07: the new 'not saved' trust outcome leaves the audit event timing unstated, so a failed change can still be audited as done.

Engine facts verified against the 5.8 source:
- LaunchEngineLoop.cpp:5195-5215 and ThreadingBase.cpp:1317-1352, plus the scheduler's StopWorkers Join (Scheduler.cpp:457): engine exit joins pool and task-graph workers.
- DirectoryWatcherWindows.cpp:63-72 and :68, and DirectoryWatchRequestWindows.cpp:78-93 and :92: correct as cited.

Patches found correct and complete: X-persist-02, -03 (§10.3, §10.2.4, SPEC PERF-01 and the D.3 row agree), -04, -06 (§11 table, §12.3 and §15.2 agree with the §11.2 Lands paragraph), -07, -08 and -10 (thread row, §3.8 and T-PER-055 consistent; engine citations correct).

Not passed: X-persist-01 (Y-01, Y-05), X-persist-05 (Y-02), X-persist-09 (Y-03) and X-persist-11 (Y-06, Y-07).

I wrote no scratch files and edited no docs.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Y-persist-01](#y-persist-01) | major | patch incomplete | confirmed | The new guard only protects a primary that is still running when the user resolves the listing. |
| [Y-persist-02](#y-persist-02) | minor | patch wrong | modified | The X-persist-05 text has the poll, which runs on a worker, register the `IDirectoryWatcher` watch itself. |
| [Y-persist-03](#y-persist-03) | minor | patch incomplete | modified | X-persist-09 adds a Review section, 'Tabs from another storage folder', for CLEAN namespaces. |
| [Y-persist-04](#y-persist-04) | minor | patch incomplete | modified | X-persist-10 moved storage-root work off the engine pools because engine exit joins their workers, and a NAS that does not answer would then hold exit for the SMB timeout (with no limit on a hard-mounted NFS share). |
| [Y-persist-05](#y-persist-05) | minor | patch incomplete | confirmed | The X-persist-01 guard relies on lock conflicts. |
| [Y-persist-07](#y-persist-07) | minor | patch incomplete | confirmed | X-persist-11 adds an outcome in which Trust this folder or Remove is not saved. |
| [Y-persist-08](#y-persist-08) | minor | patch incomplete | added by refuter | The X-persist-09 criterion for listing a CLEAN namespace at another root names only live scratch and file buffers. |

#### Y-persist-01

**Major, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet: "after it is resolved, a worker acquires that root's `instance.lock` and then the namespace's `ns.lock` (§10.5), deletes the namespace only while it holds both, and then releases them. If either is held, the namespace is not deleted: that root now has a live primary, which owns it."; TDD.md §10.12 T-PER-055 (scenario "a second editor of the same project that becomes primary at a scanned root after the scan and before the listed orphan is resolved"; result "at the root where the second editor became primary, the delete is skipped and that editor's journal is intact")

**Problem.** The new guard only protects a primary that is still running when the user resolves the listing. Here is the failure. E1 lists P/journal/main (a killed session's namespace, or since X-persist-09 a CLEAN one with dirty text). E2 then starts with the override not answering, becomes primary at P and reopens journal/main. That is the 'Lock free | Primary | journal/main/' row of §10.5. E2 writes its own generation there and exits, either cleanly (keeping dirty and scratch text across the clean exit, and any RECOVERY_PENDING records) or by a crash. Hours later the user resolves the stale listing in E1's non-modal Review. Both locks are now free, so E1 deletes a namespace that holds E2's newer work, which nothing has offered anywhere. This is the loss W-persist-05 and X-persist-01 were raised for, reached through an exit instead of a live holder. T-PER-055 tests only the case where the second editor is still running.

**Evidence.** §10.5 table: "Lock free | Primary | `journal/main/` | Recovery if `main` lacks `CLEAN`". §10.3 Reconcile: "After a clean exit, the ini's tab list wins and the journal supplies dirty and scratch text." §10.7.1: "A recovery left pending therefore survives a clean exit". §10.2.3: "Each session also starts its own generation: the header names the writing instance (`instance_pid`, `instance_start`)", so any later session changes the namespace's generation set. Nothing in §10.1 compares the namespace at delete time with what was scanned. Review is non-modal, so the gap can be hours long (the X-persist-01 problem statement).

**Refuter.** Real. TDD §10.1 now skips the delete only when `instance.lock` or `ns.lock` is held at the moment of deletion. Nothing compares the namespace with what was scanned. §10.5's table puts a later primary at the scanned root into `journal/main/`. §10.2.3 says every session starts its own generation, and §10.3 and §10.7.1 keep dirty text, scratch text and pending recoveries across a clean exit. So a second editor can become primary at that root, write there and exit (cleanly or killed) between the scan and the user's late resolution in the non-modal Review, and the guarded delete then removes newer work that was never offered. A realistic path: the override NAS is down when E1 starts, so E1's scan returns late; the NAS comes back; E2 becomes primary on the override and later exits. This is the same loss X-persist-01 was major for, so major stands. Every session writes a new `wal.<g+1>.sjl` and deletes `g-1`, so comparing file names and sizes is enough to detect it. I moved the insertion point so that "If either is held" still refers to the locks.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: after "If either is held, the namespace is not deleted: that root now has a live primary, which owns it." insert " When the task lists a namespace it also records the names and sizes of its `wal.*.sjl` files. While it holds both locks it reads them again. If anything differs, a session has written the namespace since the scan (every session starts its own generation, §10.2.3), so it is not deleted either, and that root is scanned again so that its new content is listed afresh." TDD §10.12 T-PER-055: at the end of the scenario cell append "; the same second editor exits before the listed orphan is resolved, once cleanly with a dirty file buffer and a non-empty scratch buffer and once by kill -9". At the end of the result cell append "; after the second editor has exited, resolving the listed orphan deletes nothing at that root, its namespace is intact, and its buffers are listed again".

**Patch.** TDD §10.1 ResolveStorageRoot bullet: the task records the names and sizes of the namespace's wal.*.sjl files when it lists it, and reads them again while holding both locks; if anything differs, the namespace is not deleted and that root is scanned again. T-PER-055: scenario adds the second editor exiting before the orphan is resolved (once cleanly with dirty and scratch buffers, once by kill -9); result adds that resolving then deletes nothing, the namespace is intact and its buffers are listed again.

#### Y-persist-02

**Minor, patch wrong.** Verdict: modified.

**Where:** TDD.md §11.2.3 Load, reload and invalid files, Watching bullet: "From M2, a 5-minute poll on a worker runs whether or not a studio path is configured ... The watcher is registered only while the machine folder exists, and the poll registers it when the folder appears"

**Problem.** The X-persist-05 text has the poll, which runs on a worker, register the `IDirectoryWatcher` watch itself. Registration only works on the game thread. On Linux it asserts. On Windows it races the game thread's Tick over an unsynchronised map, and the change notifications are queued as APCs to the worker, which never drains them. Taken literally, the text makes every editor that is open when IT first deploys the machine folder assert (Linux Preview) or race (Win64).

**Evidence.** Engine 5.8: `Engine/Source/Developer/DirectoryWatcher/Private/Windows/DirectoryWatcherWindows.cpp:43-76`: RegisterDirectoryChangedCallback_Handle does RequestMap.Find/Add with no lock, while Tick iterates RequestMap (:128) and drains completion routines with SleepEx(0, TRUE) on the calling thread (:147-149). `Windows/DirectoryWatchRequestWindows.cpp:96-104`: ReadDirectoryChangesW is called with a completion routine, which runs as an APC on the issuing thread. `Linux/DirectoryWatchRequestLinux.cpp:79`: Init begins with checkf(IsInGameThread(), "INotify operations only support on main thread"). TDD §3.1 File watcher row: "`IDirectoryWatcher` completes on the GT in `UEditorEngine::Tick`".

**Refuter.** Real, and checked in the 5.8 source. `FDirectoryWatcherWindows::RegisterDirectoryChangedCallback_Handle` (DirectoryWatcherWindows.cpp:43-76) does `RequestMap.Find`/`Add` with no lock while `Tick` iterates `RequestMap` (:128). `Tick` drains completion routines with `SleepEx(0, TRUE)` on the ticking thread (:147-149). `ReadDirectoryChangesW` is issued with a completion routine (DirectoryWatchRequestWindows.cpp:96-104), which runs as an APC on the issuing thread. The Linux `Init` begins with `checkf(IsInGameThread(), ...)` (DirectoryWatchRequestLinux.cpp:79). `FDirectoryWatcherProxy` passes calls straight through and does not move them to the game thread. Yet §11.2.3 has the worker poll register the watcher. The proposed fix routes the registration "through `FSumiDeferral`", but `FSumiDeferral` is GT-only (§3.7: "GT only"), so a worker cannot call it. The worker must post to the GT, and the GT registers the watcher.

**Fix applied.** TDD §11.2.3 Watching bullet: replace "The watcher is registered only while the machine folder exists, and the poll registers it when the folder appears, because registering a missing folder fails and logs a `LogDirectoryWatcher` warning each time (`DirectoryWatcherWindows.cpp:68`)." with "The watcher is registered only while the machine folder exists, because registering a missing folder fails and logs a `LogDirectoryWatcher` warning each time (`DirectoryWatcherWindows.cpp:68`). When the poll sees the folder appear, it posts that to the game thread, and the policy snapshot swap's ticker client (§3.7) registers the watcher there. No worker ever calls `IDirectoryWatcher`: its request map is unsynchronised, and the Windows completion routine is queued to the registering thread, which only the game thread's tick drains (`DirectoryWatcherWindows.cpp:43-76`, `147-149`); the Linux request asserts `IsInGameThread()` (`Linux/DirectoryWatchRequestLinux.cpp:79`)."

**Patch.** TDD §11.2.3 Watching: the poll no longer registers the watcher. It posts the folder's appearance to the game thread, and the policy snapshot swap's ticker client (§3.7) registers the watcher there. No worker calls IDirectoryWatcher (citations DirectoryWatcherWindows.cpp:43-76, 147-149 and DirectoryWatchRequestLinux.cpp:79, all re-checked). Follow-on fix: the §3.7 'Policy snapshot swap' client row now lists this registration duty.

#### Y-persist-03

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §10.7.1 Banner: "When *n* is 0 and Review lists only orphaned, imported or compat buffers, it reads "Buffers from another editor instance or an imported journal can be recovered. [Review]"."; TDD.md §10.7.3 Actions, Review row: "one unticked section per orphaned, imported or compat namespace (another instance with PID and end time; imported from a path; an older build)"; TDD.md §10.7 stateDiagram-v2: "Clean --> Pending: cross-root orphans found later (§10.1)" and "Resolved --> Pending: cross-root orphans found later (§10.1)"; TDD.md §10.12 T-PER-055 result: "after that clean exit both buffers are listed in Review under "Tabs from another storage folder", and Restore recovers their text"

**Problem.** X-persist-09 adds a Review section, 'Tabs from another storage folder', for CLEAN namespaces. These are neither orphaned, imported nor compat. §10.1 says that when the scan returns, 'the n = 0 banner (§10.7.1) then appears if needed'. But §10.7.1's banner condition names only orphaned, imported and compat buffers, so a builder may show no banner. Even when it is shown, its text says the buffers come from another editor instance or an imported journal, when they are the user's own unsaved tabs from the last session. Without the banner, the only place these tabs are offered is a panel the user has no reason to open. That is close to the 'no UI ever offers it' failure X-persist-09 was meant to remove. The Review section list and the state-diagram labels also name only orphans.

**Evidence.** §10.1: "in a section "Tabs from another storage folder (closed 14:10)" ... what it finds joins Review when it returns, and the n = 0 banner (§10.7.1) then appears if needed." §10.7.1 and §10.7.3 as quoted. Neither the banner condition nor its text was touched by the round-3 patch.

**Refuter.** Partly real. §10.5 already files its same-root equivalent ("With `CLEAN` but buffers not on disk: Tabs from a second editor instance") under "Orphans (PER-05)", and that case uses the existing n = 0 banner text, which is adequate here too: the earlier session was another editor process, and the Review section title explains the rest. The defect is that the X-persist-09 sentence in §10.1 says "as an orphan ..., and also one that ends with `CLEAN` ...". That wording lets a builder treat the new section as not an orphan, and then the §10.7.1 banner condition, the n exclusion, Restore all's exclusion, the §10.7.3 Review row and the §10.7 diagram labels (which all key on "orphaned") do not apply to it. Saying explicitly that it is listed as an orphan fixes every one of those places at once. A new banner string is not needed.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: replace "and also one that ends with `CLEAN` but holds" with "and also, as an orphan (§10.7), one that ends with `CLEAN` but holds". TDD §10.12 T-PER-055, result cell: replace "after that clean exit both buffers are listed in Review under "Tabs from another storage folder", and Restore recovers their text" with "after that clean exit both buffers are listed in Review under "Tabs from another storage folder", the n = 0 banner is shown, and Restore recovers their text".

**Patch.** TDD §10.1: the CLEAN-namespace case is now listed explicitly 'as an orphan (§10.7)'. T-PER-055 result adds 'the n = 0 banner is shown'.

#### Y-persist-04

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §11.2.3 Startup: "`StartupModule` only launches a `UE::Tasks` task, which stays inside the D22 limit"; TDD.md §11.2.3 Watching (X-persist-05 text): "From M2, a 5-minute poll on a worker runs whether or not a studio path is configured: it polls the studio file when there is one"; TDD.md §11.5.1: "The studio tier is mirrored on a worker every `mirror_minutes`, and on Refresh."; TDD.md §10.1 fallback bullet (X-persist-10): "never on `UE::Tasks` or `GThreadPool`, whose workers engine exit joins"

**Problem.** X-persist-10 moved storage-root work off the engine pools because engine exit joins their workers, and a NAS that does not answer would then hold exit for the SMB timeout (with no limit on a hard-mounted NFS share). The studio policy load is specified as a UE::Tasks task. The 5-minute studio-file poll rewritten by X-persist-05, and the studio library mirror, are 'on a worker'. All three read the studio share, so they have the same exit stall. In a studio the policy share and a {User} storage override are often on the same NAS. So in the very outage X-persist-10 handles, exit can still wait on a blocked policy read. T-PER-055 passes only because its fixture has no studio policy.

**Evidence.** Verified in 5.8: `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5195-5215` (GThreadPool->Destroy, then FTaskGraphInterface::Shutdown). `Engine/Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:1317-1352`: Destroy spins until every pool thread is idle. `Engine/Source/Runtime/Core/Private/Async/TaskGraph.cpp:1295/1327/1358` calls `LowLevelTasks::FScheduler::Get().StopWorkers()`, which calls `Thread->Join()` at `Async/Fundamental/Scheduler.cpp:457`. The 5 s studio timeout bounds only the waiter. The blocked read stays on the worker.

**Refuter.** Real, and checked in 5.8. LaunchEngineLoop.cpp:5195-5215 calls `GThreadPool->Destroy()` and then `FTaskGraphInterface::Shutdown()`. ThreadingBase.cpp:1317-1352 spins until every pool thread is idle, and Scheduler.cpp:457 calls `Thread->Join()`. `UnloadModulesAtShutdown` abandons module DLLs rather than freeing them, so the result is an exit stall rather than unloaded code, but the task can still touch `FSumiPolicy` state after module shutdown. §11.2.3 puts the studio policy load on `UE::Tasks`, and the studio poll, which round 3 rewrote, and the library mirror on "a worker". In a studio NAS outage, X-persist-10's new T-PER-055 exit bound (baseline + 2 s) then fails whenever the studio policy share is on the same host, which is typical. The proposed single thread is wrong in one detail: putting the one-item re-sync on the same thread as the mirror queues Restore published & run behind a whole mirror run, with no bound. Running a policy poll behind a long mirror would also delay a policy turning `debug.allow_debug_server` off. So policy reads and library reads get separate threads, and a re-sync is refused at once while this process's own mirror runs.

**Fix applied.** TDD §11.2.3 Startup: replace "`StartupModule` only launches a `UE::Tasks` task, which stays inside the D22 limit of ≤ 30 ms added startup." with "`StartupModule` only hands the load to `SumiPolicyIO`, a Sumi-owned `FRunnable` that does every policy-file read, the 5-minute poll included, never `UE::Tasks` or `GThreadPool`, whose workers engine exit joins (§10.1); this stays inside the D22 limit of ≤ 30 ms added startup. Like `SumiStorageProbe` (§10.1), it is abandoned at `OnPreExit` and never joined, so a studio share that does not answer never delays exit." Watching bullet: replace "The studio file is polled every 5 min on a worker" with "The studio file is polled every 5 min on `SumiPolicyIO`", and "a 5-minute poll on a worker runs" with "a 5-minute poll on `SumiPolicyIO` runs". TDD §11.5.1: replace "The studio tier is mirrored on a worker every `mirror_minutes`, and on Refresh." with "The studio tier is mirrored every `mirror_minutes`, and on Refresh, on `SumiLibraryIO`, a Sumi-owned `FRunnable` that does every read of the studio library share, the one-item re-sync of §11.5.4 included; like `SumiStorageProbe` (§10.1) it is abandoned at `OnPreExit` and never joined." In the Consistent mirrors bullet, replace "waiting for it up to 2 s on its worker (**proposed**)." with "waiting for it up to 2 s on `SumiLibraryIO` (**proposed**); a re-sync requested while this process's own mirror run is in progress is not queued behind it and gets the line below at once." TDD §3.1: after the Storage probe row add "| **Studio share I/O** (`SumiPolicyIO` from M2, `SumiLibraryIO` from M4; `FRunnable`) | 1 each; `SumiPolicyIO` from `StartupModule`, `SumiLibraryIO` at the first mirror; abandoned at `OnPreExit`, never joined (§11.2.3, §11.5.1) | `SumiPolicyIO`: the policy files and their 5-minute poll; `SumiLibraryIO`: the studio library share, its mirror folder and `mirror.lock` | Python, Slate, UObjects; the storage roots | Never | Result queue polled on the GT |". TDD §3.8 Module shutdown: replace "the storage probe thread was abandoned at `OnPreExit` and is not joined (§10.1);" with "the storage probe, policy I/O and library I/O threads were abandoned at `OnPreExit` and are not joined (§10.1, §11.2.3, §11.5.1);".

**Patch.** TDD §11.2.3 Startup: the load goes to SumiPolicyIO, a Sumi-owned FRunnable, never UE::Tasks or GThreadPool, abandoned at OnPreExit and never joined. I worded it as 'every policy-file read ... except the local reads of ResolveStorageRoot (§10.1)', so it does not contradict the synchronous machine-layer read in §10.1. Both Watching poll mentions now say SumiPolicyIO. §11.5.1: the mirror and the one-item re-sync run on SumiLibraryIO, also abandoned at OnPreExit; a re-sync requested while this process's own mirror runs is refused at once. §3.1: new 'Studio share I/O' row (6 columns). §3.8 Module shutdown names all three abandoned threads. Follow-on fix: the §11.12 failure row 'Studio policy writable by the user' did its AccessCheck on the UNC path 'on a worker'; it now says 'on SumiPolicyIO'.

#### Y-persist-05

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §10.1 ResolveStorageRoot bullet: "reads `journal/*` of this `<ProjectHash>` under every other candidate root (the override, the platform default, the temp fallback and the recorded `root`)" and the new delete guard "acquires that root's `instance.lock` and then the namespace's `ns.lock`"; TDD.md §11.5.1: "The one-item re-sync of Restore published & run (§11.5.4) also takes `mirror.lock`, waiting for it up to 2 s ... If it is still held, nothing runs"

**Problem.** The X-persist-01 guard relies on lock conflicts. On POSIX an fcntl lock never conflicts with one held by the same process, and closing any descriptor of the file drops all of that process's locks on it. Suppose a candidate root is this session's own root reached through another path: a symlinked $XDG_STATE_HOME or home folder, or an override typed as an alias. Its instance.lock and journal/main/ns.lock then look free to this process. The scan lists the live session's own buffers as orphans, and the guarded delete then succeeds and removes the live journal. The scan's own open and close of the lock file also drops the session's primary lock, so a second instance can become primary on journal/main. Windows is safe, because share modes are per handle. The same per-process behaviour means that on Linux the mirror run and the one-item re-sync of one process never see each other's mirror.lock, so the new 'If it is still held' outcome never fires there.

**Evidence.** §10.5: "POSIX: one descriptor with `fcntl(F_SETLK, F_WRLCK)`. fcntl locks drop when any descriptor on the file closes, so the process never opens the file twice." §10.1 compares candidate roots by path only and never excludes an alias of the session's own root. The Linux Preview lands in M4 (§10 feature table).

**Refuter.** Real for POSIX (Linux Preview in M4, macOS in 1.2). §10.5 specifies `fcntl(F_SETLK)` and admits that these locks drop when any descriptor on the file closes ("the process never opens the file twice"). fcntl locks are owned by the process, so a second descriptor in the same process never conflicts with them. §10.1 only says "every other candidate root" and gives no identity rule. A candidate that is this session's own root under another path therefore looks unlocked: its live `main` is listed as an orphan, and the round-3 guarded delete then succeeds against the session's own journal. The probe's close also drops the session's primary lock. The X-persist-07 path "If it is still held, nothing runs" can likewise never happen between the mirror run and the re-sync of one process, and the re-sync's close drops the mirror run's lock. Windows is unaffected, because share modes are per handle. I reworded the §10.1 part as a separate sentence so the bullet still reads cleanly; the mirror part is the verifier's text and works with the Y-persist-04 fix.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: after "A root whose `owner.json` names another account is never read, listed or deleted from." insert " Whether a candidate is another root is decided by folder identity (volume serial and file ID from `GetFileInformationByHandle`, or `st_dev` and `st_ino`), never by path: a candidate that is this session's own root under another path (a symlink, a junction or an alias typed as the override) is skipped, because on POSIX a lock this process holds never conflicts with its own test, and closing the test's descriptor would drop it (§10.5)." TDD §11.5.1 Consistent mirrors bullet: after "opened and held like `instance.lock` (§10.5)" insert "; within one process every `mirror.lock` user goes through one owner object that holds the handle and a mutex, so the mirror run and the one-item re-sync exclude each other on POSIX too".

**Patch.** TDD §10.1: a candidate root is compared with this session's root by folder identity (volume serial and file ID, or st_dev and st_ino), never by path, and skipped when it is the same folder. §11.5.1: within one process every mirror.lock user goes through one owner object with the handle and a mutex.

#### Y-persist-07

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §11.3.3 The store: "if it still cannot take the lock the change is not saved and the trust banner or the Trusted folders page that asked for it says "Could not save trusted folders; try again""; TDD.md §11.3.3 interface: "void TrustFolder(const FString& Folder); // GT; Trust.json and audit"; TDD.md §11.4 Other events: "`trust.run_once`, `trust.folder_added`, `trust.folder_removed`"; Writing: "The GT serialises each line"

**Problem.** X-persist-11 adds an outcome in which Trust this folder or Remove is not saved. TrustFolder is a GT call that writes the audit event, and audit lines are serialised on the GT. Nothing says the event waits for the I/O-thread item. Built as written, a change that fails after the 1 s of retries still leaves `trust.folder_added` or `trust.folder_removed` in the audit log (SEC-05), recording a trust change that never happened. T-TRU-006's new case checks only the message.

**Evidence.** §11.3.3 and §11.4 as quoted. The audit timing is not stated anywhere, and the new failure path is asynchronous (it re-posts through `PostIo` for up to 1 s).

**Refuter.** Real, and created by X-persist-11's new outcome where the change "is not saved". `TrustFolder` is a GT call annotated "Trust.json and audit", and audit lines are serialised on the GT (§11.4). Nothing ties the `trust.folder_added` and `trust.folder_removed` events to the asynchronous I/O-thread item, which can now give up after re-posting itself for 1 s. An implementation that writes the event at the GT call leaves a SEC-05 record of a trust change that never happened. The fix is small and I kept it, adding the GT hand-off so the line is still serialised on the GT.

**Fix applied.** TDD §11.3.3, 'The store' sub-bullet: replace "(**proposed**)), then replace it atomically; a process never writes back its cached list." with "(**proposed**)), then replace it atomically; `trust.folder_added` or `trust.folder_removed` is written only after that replace succeeds (the I/O item reports the result to the GT, which writes the line, §11.4), and a change that was not saved writes no event; a process never writes back its cached list." TDD §11.3.3 interface: replace "// GT; Trust.json and audit" with "// GT; queues the Trust.json change; audit after it is saved". TDD §11.12 T-TRU-006, result cell: append "; that attempt writes no `trust.folder_added` record".

**Patch.** TDD §11.3.3 store: trust.folder_added and trust.folder_removed are written only after the replace succeeds (the I/O item reports to the GT, which writes the line), and an unsaved change writes no event. The TrustFolder comment now reads 'GT; queues the Trust.json change; audit after it is saved'; I also changed the UntrustFolder comment to 'GT; as TrustFolder' to match. T-TRU-006 result adds 'that attempt writes no trust.folder_added record'.

#### Y-persist-08

**Minor, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §10.1 Rules, ResolveStorageRoot bullet (X-persist-09 text): "and also one that ends with `CLEAN` but holds a non-empty scratch buffer or a file buffer whose journaled text differs from the file on disk, in a section "Tabs from another storage folder (closed 14:10)""; TDD.md §10.7.1 Persistence: "A recovery left pending therefore survives a clean exit ... and the next start offers it again (I7, §10.10). Ignoring the banner loses nothing."

**Problem.** The X-persist-09 criterion for listing a CLEAN namespace at another root names only live scratch and file buffers. It leaves out unresolved `RECOVERY_PENDING` records, which by design survive a clean exit. A pending recovered text is never live-buffer state (§10.2.4 step 3: a SNAP with a non-null `pending` is never applied to a live buffer), so the buffer shows its disk or clean text and is not dirty. Scenario: a crash at the platform default; the next start there, with the override still down, leaves the recovery pending and exits cleanly; the start after that uses the override. The default root's `main` is then either not listed at all, so the pending recovery is not offered (the 'no UI offers it' gap X-persist-09 was meant to close, and I7's 'the next start offers it again' fails across a root change), or it is listed for some other dirty buffer and deleted once those are resolved, dropping the pending recovery without offering it. The text then survives only in the other root's local history, which this session does not show.

**Evidence.** §10.2.4 step 6 returns pending recoveries separately from buffers. §10.7.1 Persistence writes `RECOVERY_PENDING` plus a `SNAP` for counted buffers and keeps them across clean exits. The §10.1 criterion as quoted. T-PER-055 has no pending-recovery case.

**Fix applied.** TDD §10.1 Rules, `ResolveStorageRoot` bullet: replace "holds a non-empty scratch buffer or a file buffer whose journaled text differs from the file on disk," with "holds a non-empty scratch buffer, a file buffer whose journaled text differs from the file on disk, or an unresolved `RECOVERY_PENDING` (§10.7.1, listed with its recovered text),". TDD §10.12 T-PER-055: at the end of the scenario cell append "; the same clean exit at the platform default with a recovery left pending there". At the end of the result cell append "; that pending recovery is listed in the same section, and Restore recovers it".

**Patch.** TDD §10.1: the CLEAN-namespace listing criterion now includes an unresolved RECOVERY_PENDING, listed with its recovered text. T-PER-055 scenario adds the clean exit with a recovery left pending; result adds that it is listed in the same section and Restore recovers it. I placed this result clause right after the 'Tabs from another storage folder' clause, so 'the same section' points at it, and the Y-persist-01 clause at the end of the cell.

### Round 4 — Delivery and tests

Delivery lens, round 4. Checks run: word diffs of every before/after pair; check.py (0 problems; 815 tests, 248 SPEC IDs, links checked); gen.py dry run (the in-memory regeneration is byte-identical to the current TDD.md, so the generated parts are current); and a script that checks, for every §15.1 and §15.2 exit, that the named tests exist and that their Lands fit the milestone. All named tests exist. The only early ones are split tests whose split step names the milestone (T-E2E-001 at M2; T-E2E-008 and T-GAP-003 at M4). Gate suites: G1, G2, G3, G6 and G7 are all M1; G4 is all M3; G5 has T-UEI-004 at M2 and T-RLD-020 at M3, as §14.3 states. Results per issue: X-delivery-01: the final_fix text is in §14.3 word for word. It matches the §15.1 P0-1 and P0-2 Tests columns and the §5.15.1 intro (journal-writer and LSP checks join at M1, search at M2). One residual minor issue is reported as Y-delivery-01: \"the whole G1 automated suite\" includes the conditional fallback tests T-VIEW-019 and T-CORE-036, so G1 green without a fallback cannot be computed literally. X-delivery-02: the P0-2 Builds and TDD columns match the final_fix. `Sumi.G2.*` matches the §13.4 G2 row. The input bot is part of P0-4's §4.16.1. The time.sleep swap (§5.3.5), sliced_sleep (§5.10.3) and SumiTestLatch (§14.2) chain is consistent, and no text elsewhere says these land later. X-delivery-03: all four §15.2 cells match the final_fix. The §11 feature-table M2 row, the §12 FSumiPolicy row and the §15.2 M2 Scope are consistent with the §11.2 Lands paragraph and the §11.2.3 Gate (\"Waiting for policy\"). The M3 and M4 §11.7 split agrees with SPEC API-01 and API-03, and the M4 §10 parts agree with SPEC PER-04 and PER-08. No stale \"parts not in M1 or M2\" or \"its §11.2.3 reload and swap\" text remains in SPEC, TDD, 08 or 07a. T-AUD-007 (M2) and T-RLD-014 (M3) now follow §1.5. X-delivery-04: the T-ARCH-004 text matches. FSumiReloadOnSave and its IDirectoryWatcher registration in §9.3 were checked against the engine (IDirectoryWatcher.h:72). SPEC SEC-03 agrees, the test can pass at M1 (before FSumiReloadOnSave exists), and its Lands of M1 is correct. X-delivery-06: the §14.1 GPU-2/GPU-3 image now includes MS-IME Japanese, which matches the §13.4 G2 agent cell and the G2 per-build job. No delivery-lens changes in SPEC, 08, README, 07a or the mockups. The only mockup and PNG change, 10-settings-and-trust, belongs to the ux lens.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Y-delivery-01](#y-delivery-01) | minor | patch incomplete | modified | The round-3 sentence now says that at the Phase 0 go/no-go, G1 means "the whole G1 automated suite". |

#### Y-delivery-01

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §14.3 Gate suites, intro paragraph (the go/no-go sentence added by X-delivery-01); TDD §4.15 test table, rows T-VIEW-019 and T-CORE-036; TDD §15.1 Go/no-go paragraph and the P0-1 Tests column "G1 suite (§14.3)"

**Problem.** The round-3 sentence now says that at the Phase 0 go/no-go, G1 means "the whole G1 automated suite". That suite is generated from Covers cells, so it includes two §4.15 fallback tests. T-VIEW-019 tests the caret-only split, and its own row says that split is "built only if G1(2) fails". T-CORE-036 tests seam conformance with the Scintilla adapter, which the §4.15 fallback table builds only when G1(1) or G1(4) fails. Read literally, "G1 green" without a fallback cannot be reached: when G1(2) passes, T-VIEW-019 does not exist and cannot be green. X-delivery-01 rewrote this sentence precisely so that CI could compute Phase 0 status from §14.3, so a CI built from it would report G1 as never green on the no-fallback path. The same problem applies at the M1 exit, because both tests have Lands M1.

**Evidence.** §14.3: "Where the Phase 0 go/no-go (§15.1) names G1 or G2, it means the tests its Tests column names: the whole G1 automated suite, and the G2 automated suite in the Run window ...". The §14.3 G1 row lists "... T-IME-009, T-CORE-036, T-VIEW-019 | 19". §4.15: "| T-VIEW-019 | 4 | Caret-only split (built only if G1(2) fails) | Caret frames ≤ 0.5 ms p95 | G1(2) | M1 |" and "| T-CORE-036 | 1 | Seam conformance between TextBuffer and the Scintilla adapter | ... | D1, G1 | M1 |". The fallback table says "(1) or (4) fails (model or latency) | **Scintilla's document layer and Lexilla** behind the seam: `sumi::IDocument` (TextBuffer or `ScintillaDocumentAdapter`)". §15.1: "It exits on G1 green or its fallback invoked". A dry run of gen.py confirms both tests are in the generated G1 automated suite (19 tests).

**Refuter.** The issue is real, and the round-3 patch caused it. Before round 3, §14.3 tied the go/no-go meaning to "first Lands at or before that milestone" (TDD.before.md:8283). The X-delivery-01 text now at TDD.md:8288 says that at the go/no-go, G1 means "the whole G1 automated suite". The generated G1 row (TDD.md:8292, 19 tests; gen.py builds it from Covers cells and leaves the intro hand-written) includes T-CORE-036 and T-VIEW-019. Both test fallbacks that exist only when G1 fails:
- T-VIEW-019's own row says "built only if G1(2) fails" (TDD.md:1932).
- The Scintilla adapter behind T-CORE-036 is also built only on failure. TDD §4.15's fallback table gives it for "(1) or (4) fails". 08 D1 "Fallbacks if G1 fails" (a) says to replace the model and lexer with Scintilla on a model or latency failure. 08's licence row says "HPND (Scintilla, if used)".

So on the no-fallback path, "G1 green or its fallback invoked" (TDD.md:8629, 8643) cannot be reached through "G1 green" when CI computes it from §14.3. CI computing status from §14.3 is exactly what X-delivery-01 set out to make possible.

The proposed fix needs two changes:
- Drop the hedge. The adapter is not built unconditionally, so the alternative should not be offered.
- Its "at every milestone exit ... count toward G1" clause does nothing, because no §15.2 exit names G1 (the M1 exit names G2, G5, G6 and G7). At M1 the two tests are pulled in by "every test whose Lands is M1" (TDD.md:8649). That clause predates round 3, but the same sentence can cover it at no cost.

**Fix applied.** Edit 1, required. TDD.md §14.3 Gate suites, intro paragraph. The current text reads: "... and the G2 automated suite in the Run window without T-LIFE-012's journal-writer, LSP and search-thread checks (§5.15.1). G1, G2 and G3 hold no test whose first Lands is after M1 (T-GAP-008); ...". Between "(§5.15.1)." and "G1, G2 and G3 hold no test", insert: "T-CORE-036 and T-VIEW-019 test the §4.15 fallbacks and are built only with them: T-CORE-036 once the Scintilla document layer is invoked (G1(1) or (4) failed), and T-VIEW-019 once the caret-only split is (G1(2) failed). Until then, neither counts toward G1 at the go/no-go, nor toward the M1 exit's "every test whose Lands is M1" (§15.2)."

Edit 2, for symmetry with T-VIEW-019. TDD.md §4.15 test table, row T-CORE-036, What-it-checks cell. Current text: "Seam conformance between TextBuffer and the Scintilla adapter". New text: "Seam conformance between TextBuffer and the Scintilla adapter (built only if G1(1) or (4) fails)".

Do not change the generated §14.3 table or the count of 19.

**Patch.** TDD §14.3 intro (hand-written part): T-CORE-036 and T-VIEW-019 are built only with their §4.15 fallbacks and count toward neither G1 at the go/no-go nor the M1 exit's 'every test whose Lands is M1' until then. §4.15 T-CORE-036 'What it checks' adds '(built only if G1(1) or (4) fails)'. The generated table and its count of 19 are unchanged.

### Round 4 — UX and mockups

I checked the round-3 ux patch against the before and after word diffs, the local UE 5.8 source and the images. One minor issue remains (Y-ux-01).

X-ux-01 is correct and complete:
- Engine facts: `LevelEditorModesActions.cpp:17-44` gives Shift+One to Shift+Nine to the first nine visible modes, in priority order. The context is "LevelEditorModes", whose parent is MainFrame. `SLevelEditor::RefreshEditorModeCommands` (`SLevelEditor.cpp:1924-1960`) maps the commands into `LevelEditorCommands`, which is what `GetLevelEditorActions()` returns, so ComputeShadowed finds them. The Shift+1-4 commands at `LevelEditorActions.cpp:4695-4698` are commented out.
- Every document now matches. "Shift+1–9" or "Shift+1 to Shift+9" appears in TDD §11.1.4, T-CFG-011, SPEC §9.3, mockup 10 line 389 and the SPEC D.3 row. No "Shift+1–4" is left in SPEC, TDD, 08, 07a, the README or the mockups.
- T-CFG-011 can pass as written: its n clause matches the at-most-nine rule, and its Lands value is unchanged.
- Image 10: a pixel diff against backup-r3 changes only a 7×9 px area (x 1286-1292, y 1396-1404), the 4 becoming 9. The PNG reads "Shift+1–9".

X-ux-02 is correct; every engine claim checks out in the source:
- `bCreateRedirector` is set at `AssetRenameManager.cpp:994` (asset not local), 1032 (map), 1068 (unloaded referencer), 1291 (collection) and 1322 (referencer not renamed).
- The old package is queued for deletion only when no redirector is left (`:1880`). `CleanupAfterSuccessfulDelete` runs at `:2028-2036`.
- The transaction record keeps the old outer as LoadOuter (`Obj.cpp`, transacting branch). The undo buffer therefore references the old package. That is how `ObjectTools.cpp:2540-2549` calls `ResetTransaction("Delete Selected Item")`.
- `UTransBuffer::Reset` calls `Cancel(0)`, which broadcasts TransactionCanceled, and `End()` tolerates an active count of 0. The cleanup path fires no OnAssetsPreDelete.
- T-TXN-015 can pass whether or not the rename leaves a redirector: the status ends Cleared, no banner shows and the queue length is 0.

X-ux-03 is in place and works for these cases: last step renamed, later steps recorded, and later steps empty.

X-ux-04 is applied at every location it listed. Images 01, 05 and 07 agree with it. Image 05 stays valid because its project uses source control, so the renames leave redirectors.

Y-ux-01: the footer value `not undoable`, the "Cleared run shows no chip" rule and SPEC §9.4 all depend on a run having status Cleared. X-ux-03 says a task never has that status, and these rules lack the task qualifier that check 0 has. So:
- A task whose last recording step renamed assets has no defined footer value, and its chip still shows.
- SPEC §9.4 would label as "not undoable" a task whose later steps can still be walked back.

The issue entry gives the text changes. Scratch file: scratchpad/verify5/ux_pngdiff.py (the PNG pixel-diff probe).

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Y-ux-01](#y-ux-01) | minor | patch incomplete | modified | X-ux-04 added three rules that depend on the run having status Cleared: the footer value `not undoable`, the 'no chip' rule and the SPEC §9.4 wording. |

#### Y-ux-01

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD §6.4 footer table, Transaction (U3, RT-08) row: "`not undoable` (status Cleared; the Transaction notes line gives the reason, §5.9.2)"; TDD §6.4, Undo this run bullet: "A Cleared run (§5.9.2) shows no chip."; SPEC §9.4 Messaging rules, Honest-state wording bullet: "or, for a run that renamed or moved assets, \"not undoable\" and \"undo history cleared\" (RUN-07);"; TDD §5.9.2 Renames and moves bullet: "In a task, Cleared is the status of the renaming step, not of the task."; TDD §5.9.5 check 0: "Status is not Cleared (for a task: the status of its last step that recorded changes)"

**Problem.** X-ux-04 added three rules that depend on the run having status Cleared: the footer value `not undoable`, the 'no chip' rule and the SPEC §9.4 wording. X-ux-03 says a task never has that status, because only its renaming step does. The X-ux-04 rules do not use the task qualifier that check 0 got. Take a task whose last step that recorded changes renamed assets. Nothing is left to walk back, and check 0 refuses. But the footer has no defined value, so a builder falls back to `undo: "Sumi: task #N" (k objects, size)`, and the header shows the chip even though nothing can be undone. Now take a task whose step 5 renamed and whose steps 6 to 40 recorded edits. SPEC §9.4 tells copywriters to say "not undoable" instead of offering Undo this run, but TDD §5.9.2 keeps the walk-back for steps 6 to 40.

**Evidence.** TDD §6.4 footer: "`not undoable` (status Cleared; the Transaction notes line gives the reason, §5.9.2)". §6.4 chip: "A Cleared run (§5.9.2) shows no chip." TDD §5.9.2: "In a task, Cleared is the status of the renaming step, not of the task. ... the task's walk-back (§5.9.6) counts only the steps after the last renaming step." §5.9.5 check 0 has a task-specific qualifier; the §6.4 rules and SPEC §9.4 do not.

**Refuter.** The issue is real, and both parts of it come from round 3. X-ux-03 says that inside a task only the renaming step has status Cleared, never the task. X-ux-04 added three rules that apply only when the status is Cleared: the footer value `not undoable`, the rule "A Cleared run (§5.9.2) shows no chip.", and the exception in §5.9.3, "except for a Cleared run (§5.9.2)". None of the three got the task qualifier that check 0 has. So take a task whose last step that recorded changes renamed assets. The reset cleared the whole history, and check 0 refuses. Yet the record is not Cleared, so the footer falls back to `undo: "Sumi: task #N" (k objects, size)`. That sits next to the notes line "... not undoable · undo history cleared". The chip appears, disabled, where a plain Cleared run shows none. The chip part is not undefined: the general rule "Otherwise the chip is disabled and its tooltip gives the reason" covers it. But the task and plain-run cases disagree, and the footer text is false. No ruling in 08 §9 touches renames. The SPEC §9.4 part also holds. A task is its own numbered run (LRS-05). §9.4 says a run that renamed assets gets "not undoable" in place of "transaction kept" with Undo this run, while TDD §5.9.2 keeps the walk-back for the steps after the last renaming step. The proposed fix is incomplete: it edits §6.4 but not §5.9.3. After it, §5.9.3 would still say an erroring or cancelled task in this case offers Undo this run, while §6.4 says it shows no chip, which is a new contradiction. One definition in §5.9.2 covers §5.9.3 and both §6.4 rules in a single edit.

**Fix applied.** (1) TDD §5.9.2, 'Renames and moves' bullet, at its end. Current text: "Step s renamed assets, so the undo history before it was cleared."" New text: "Step s renamed assets, so the undo history before it was cleared." A task whose last step that recorded changes is Cleared counts as a Cleared run wherever §5.9.3 and §6.4 name one: its footer reads `not undoable`, and its header offers no Undo this run chip. A task with recorded steps after its last renaming step keeps the chip for the walk-back (§5.9.6)." Do not edit §6.4 or §5.9.3; they then apply as written. (2) SPEC §9.4 Messaging rules, 'Honest-state wording' bullet. Current text: ""not undoable" and "undo history cleared" (RUN-07);" New text: ""not undoable" and "undo history cleared" (RUN-07; a task still offers the walk-back for its steps after the last renaming step, TDD §5.9.2);"

**Patch.** TDD §5.9.2 Renames and moves: a task whose last step that recorded changes is Cleared counts as a Cleared run wherever §5.9.3 and §6.4 name one (footer `not undoable`, no chip); a task with recorded steps after its last renaming step keeps the chip for the walk-back. SPEC §9.4 Honest-state wording adds that a task still offers the walk-back for its steps after the last renaming step (TDD §5.9.2). SPEC Appendix D.3: added a Y-ux-01 row (§9.4), and the D.3 intro now also says where X- and Y- rows come from; it named only V- and W- rows although X- rows already exist.

### Round 4 — refuted

| ID | Issue | Why it was refuted |
|---|---|---|
| Y-persist-06 | X-persist-07 applied the accepted rule that the §12.4 index lists every per-user data file, lock files included: it added `current.json` and `mirror.lock`, and the index already lists `ns.lock`, `instance.lock` and … | No rule says the §12.4 index must list every lock file. W-persist-11 accepted that the index must carry the fields a writer needs (merge keys, the secondary's file names, owner.json), not every auxiliary file. Round 3 did not touch the Trusted folders or `projects.json` rows; X-persist-11 changed only §11.3.3. `projects.json.lock` is older still. Both locks are fully specified in their owning sections (§11.3.3 and §10.1), so a builder working from the documents builds them correctly. An index entry would add nothing they need. |

## Round 5

Scope: the round-4 patch and its follow-on edits, in two lenses.

**Patch.** All 9 confirmed round-5 issues are applied (Z-persist-01, -02, -03, -04, -05, -07 and Z-other-01, -02, -03), each with its final_fix text exactly, and none was skipped. Every edit is in C:/Users/anima/Desktop/UnrealPythonEditor/docs/TDD.md. No SPEC requirement changed, so no Appendix D.3 row was added, and 08, 07a and README are untouched. The generated parts are current and check.py prints 'PROBLEMS 0 {}'.

**Checks after the patch.** Before snapshots are in scratchpad/verify6/patch/ (SPEC.before.md, TDD.before.md, README.before.md, 08.before.md, 07a.before.md). The live docs were byte-identical to fix/backup-r5 before editing. All 22 replacements were applied by scratchpad/verify6/patch/apply.py, which checked first that every anchor occurs exactly once. git diff --stat: TDD.md 18 insertions, 16 deletions (16 lines changed plus the new §11.5.5 Threads paragraph and its blank line). TDD.md still has LF endings (0 CR). No added test text names a milestone or a later lands_terms term, so the Lands values stay valid. A gen.py dry run gave output identical to the patched file, and gen.py --write left it unchanged. check.py: 'TDD headings 369 numbered 363 / links checked 1381 / SPEC ids 248 / test rows 815 unique 815 / PROBLEMS 0 {}'.

### Round 5 — Persistence, policy and studio

I diffed the persist lens with git diff --no-index --word-diff, comparing scratchpad/verify5/patch/TDD.before.md with scratchpad/fix/backup-r5/TDD.md. The backup is byte-identical to the live docs; SPEC changed only outside this lens, and 08, 07a and README are unchanged. integ/check.py still prints PROBLEMS 0.

Correct as patched:
- Y-persist-02: §11.2.3 watcher registration now happens on the GT through the §3.7 swap client. I re-checked every engine citation in UE_5.8 and all hold: DirectoryWatcherWindows.cpp:43-76 (unsynchronised RequestMap), :68 (the warning), :147-149 (SleepEx drain in Tick), Linux/DirectoryWatchRequestLinux.cpp:79 (checkf(IsInGameThread())), and EditorEngine.cpp:1948 (the GT tick).
- Y-persist-03: CLEAN-namespace tabs are now orphans, and T-PER-055 asserts the n = 0 banner.
- Y-persist-05: candidate roots are compared by folder identity, and mirror.lock goes through one owner object.
- Y-persist-07: the trust audit line is written on the GT only after the save succeeds. This matches the §11.4 writing rule, and the UntrustFolder comment matches TrustFolder.
- Y-persist-08: RECOVERY_PENDING is in the cross-root criterion, consistent with the §10.2.4 step 6 output.

Y-persist-01 (major): the fingerprint is consistent with the wal.<generation>.sjl naming in §10.2.2 and §12.4. It depends on every session starting its own generation (§10.2.3 and §10.3 step H). The .tmp, .corrupt and .v<major> names fall outside the pattern. T-PER-055 now covers the second editor exiting cleanly and by kill -9. One gap remains (Z-persist-01): the fingerprint is recorded when the namespace is listed, which is after the scan has read it. A second editor that starts during a slow read can still lose its work. Recording the names and sizes before the read fixes it.

Y-persist-04 is in place in §3.1, §3.7, §3.8, §11.2.3, §11.5.1 and §11.11, but:
- It puts every policy read on one thread, so a studio read that does not answer holds up the local machine and project reads. That can keep IsReady() false and delays the RV-04 mid-session turn-off (Z-persist-02).
- Two statements still say "on a worker": the AccessCheck in §11.2.1 and the mirror in the §12.4 snippet row (Z-persist-04).
- The new exit guarantee and the immediate re-sync refusal have no test (Z-persist-05).

The known Publish gap is reported as Z-persist-03. It gives SumiLibraryIO every studio-share write, keeps those writes off the GT, the journal writer and the engine pools, and writes library.published on the GT only after the writes succeed. The Y-persist-03 residual is Z-persist-06: the specified n = 0 banner text misstates the tabs that T-PER-055 now checks it for.

Seen but not reported, as the round-4 patch did not touch them and they are not critical: trust resolution (§11.3) and saves (§10.14) do UNC I/O on worker tasks, so they have the same exit-join exposure.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Z-persist-01](#z-persist-01) | minor | patch incomplete | modified | The Y-persist-01 fingerprint is fine as a comparison. |
| [Z-persist-02](#z-persist-02) | minor | patch wrong | modified | Y-persist-04 put every policy-file read on one thread: the local machine and project layers, their watcher reloads and Sumi.Policy.Reload, as well as the studio share. |
| [Z-persist-03](#z-persist-03) | minor | patch incomplete | modified | This is the gap the round-4 patcher noted and did not close. |
| [Z-persist-04](#z-persist-04) | minor | patch incomplete | confirmed | Y-persist-04 and its follow-on edit moved these two pieces of studio-share I/O onto named Sumi threads. |
| [Z-persist-05](#z-persist-05) | minor | patch incomplete | modified | The new exit guarantee and the immediate re-sync refusal have no test. |
| [Z-persist-07](#z-persist-07) | minor | patch incomplete | added by refuter | The Y-persist-05 patch added the folder-identity skip but no test for it. |

#### Z-persist-01

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §10.1 Rules, `ResolveStorageRoot` bullet: "When the task lists a namespace it also records the names and sizes of its `wal.*.sjl` files. While it holds both locks it reads them again."

**Problem.** The Y-persist-01 fingerprint is fine as a comparison. It is defined against the wal.<generation>.sjl naming of §10.2.2 and §12.4, it relies on every session starting its own generation (§10.2.3, §10.3 step H), and the `.tmp`, `.corrupt` and `.sjl.v<major>` names fall outside the `wal.*.sjl` pattern. The problem is when it is captured. In §10.1, "lists" means adding the namespace to Review, which happens after the scan has read it. The scan checks the root's `instance.lock` and the namespace's `ns.lock` once, at the start, and holds neither while it reads, and §10.1 says the read "may take as long as the file system needs". A second editor E2 can start at that root during the read, become primary, write its first checkpoint `wal.<g+1>.sjl`, keep working, and then exit. The scan then records a fingerprint that already includes E2's generation, although the content it listed came from the older generations. When the user resolves the listing, the re-read under both locks matches, and E2's newer work is deleted. This is the Y-persist-01 loss, reached through a start during the scan's read instead of after it.

**Evidence.** §10.1: the task reads `journal/*` of roots "whose `instance.lock` is not held by a live instance", and "Nothing waits on this task, and it may take as long as the file system needs: what it finds joins Review when it returns". §10.3 flowchart H: "the first commit opens a new generation" at every primary start. §10.2.4 step 1 lists the generations before step 2 reads them, so the names the read used are known before the read, but the patch records them "when the task lists a namespace".

**Refuter.** The race is real, though the window is narrow. TDD §10.1 says the scan checks `instance.lock` and `ns.lock` without holding them and that the task "may take as long as the file system needs". If the fingerprint is taken when the namespace is listed, which comes after the read, it can include a generation that E2 wrote and finished during the read. The resolve-time re-read then matches, and E2's newer work is deleted. Every other part of the fingerprint holds up: names and sizes, `wal.*.sjl` only, appends always grow a file, and rotation, the `.corrupt` rename and the `.sjl.v<major>` rename all change the name set. The only defect is when it is taken. The verifier's extra post-read compare-and-re-read loop is not needed for safety. If E2 has written anything, the resolve-time compare against a pre-read record already fails, and that path skips the delete and rescans the root. The loop also re-checks only `ns.lock`, not the root's `instance.lock` rule. The smallest fix is to move the capture to before the read.

**Fix applied.** TDD.md §10.1 Rules, `ResolveStorageRoot` bullet. Replace "When the task lists a namespace it also records the names and sizes of its `wal.*.sjl` files. While it holds both locks it reads them again. If anything differs, a session has written the namespace since the scan (every session starts its own generation, §10.2.3)," with "Before the task reads a namespace, it records the names and sizes of its `wal.*.sjl` files. While it holds both locks it reads them again. If anything differs, a session has written the namespace since that record (every session starts its own generation, §10.2.3),". The rest of the sentence ("so it is not deleted either, and that root is scanned again so that its new content is listed afresh.") stays as it is.

**Patch.** TDD §10.1 ResolveStorageRoot bullet: the wal.*.sjl names and sizes are now recorded 'Before the task reads a namespace' rather than when it lists it, and 'since the scan' became 'since that record'. The rest of the sentence is unchanged.

#### Z-persist-02

**Minor, patch wrong.** Verdict: modified.

**Where:** TDD.md §11.2.3 Startup: "`StartupModule` only hands the load to `SumiPolicyIO`, a Sumi-owned `FRunnable` that does every policy-file read, the 5-minute poll included, except the local reads of `ResolveStorageRoot` (§10.1)."; TDD.md §11.2.3 Watching: "From M2, a 5-minute poll on `SumiPolicyIO` runs whether or not a studio path is configured: it polls the studio file when there is one, and it always stats the machine file and its folder."; TDD.md §3.1 Studio share I/O row: "`SumiPolicyIO`: the policy files and their 5-minute poll"

**Problem.** Y-persist-04 put every policy-file read on one thread: the local machine and project layers, their watcher reloads and Sumi.Policy.Reload, as well as the studio share. A read of a studio share that does not answer blocks that thread for the full SMB timeout, and with no limit on a hard-mounted Linux share. A blocked thread cannot enforce the 5 s studio timeout itself. Every machine or project read queued behind it waits too. (1) At load, the project layer can be queued after the studio read: the machine layer has to come first because it holds `studio_policy_path`, and no further order is given. Then `IsReady()` stays false, so the "Waiting for policy" gate holds Debug in VS Code and, from M4, every run and LSP spawn until the SMB call returns. (2) A watcher-driven machine reload that turns `debug.allow_debug_server` off mid-session, which RV-04 (2) requires, waits behind a studio poll that has not returned. Before the patch only the load task and the poll were named, and the local reloads were not tied to the studio read.

**Evidence.** §11.2.3: "A network studio path gets a 5 s timeout"; state diagram "Loading --> Ready: every layer read, failed or timed out"; Gate: "From M4, runs, LSP spawns and telemetry sends wait on it"; §11.11 "Studio policy offline ... 5 s timeout". 08 §9 RV-04 (2): "Turning `debug.allow_debug_server` off mid-session ... closes it". §10.1 already reads the machine layer on the GT "because it is a local file".

**Refuter.** Confirmed in the current text. §11.2.3 Startup puts "every policy-file read, the 5-minute poll included" on the single thread `SumiPolicyIO`. That includes the project layer and every watcher, poll or `Sumi.Policy.Reload` reload of the local layers. A blocked SMB call cannot time itself out. So a project-layer read queued behind it holds `IsReady()` past the 5 s studio timeout, and from M4 runs and LSP spawns wait on that gate. A machine reload that turns `debug.allow_debug_server` off (RV-04 (2)), or a project edit (T-POL-005's 2 s), waits behind a studio poll that has not returned. Before the patch, only the load task and the poll were named, so the patch's wording created this coupling. The verifier's fix is directionally right but leaves "`StartupModule` only hands the load to `SumiPolicyIO`" in place, while the new text loads the machine and project layers on `UE::Tasks`. That is a contradiction. It also does not say that the studio load has to wait for the machine layer, whose `studio_policy_path` names the studio file. The corrected fix rewrites the whole sentence and adds a test clause.

**Fix applied.** (1) TDD.md §11.2.3, Startup bullet. Replace "`StartupModule` only hands the load to `SumiPolicyIO`, a Sumi-owned `FRunnable` that does every policy-file read, the 5-minute poll included, except the local reads of `ResolveStorageRoot` (§10.1). The load never runs on `UE::Tasks` or `GThreadPool`, whose workers engine exit joins (§10.1); handing it over stays inside the D22 limit of ≤ 30 ms added startup. Like `SumiStorageProbe` (§10.1), `SumiPolicyIO` is abandoned at `OnPreExit` and never joined, so a studio share that does not answer never delays exit." with "`StartupModule` only starts the load, which stays inside the D22 limit of ≤ 30 ms added startup. The machine and project layers are local files: their load and every reload of them (a watcher event, the 5-minute stat of the machine file and its folder, `Sumi.Policy.Reload`) run as `UE::Tasks` tasks, and `ResolveStorageRoot` reads the machine layer on the GT (§10.1). Every read of the studio policy share runs on `SumiPolicyIO`, a Sumi-owned `FRunnable`, never on `UE::Tasks` or `GThreadPool`, whose workers engine exit joins (§10.1). These reads are the studio layer's load, which starts once the machine layer has been read because its `studio_policy_path` names the file; its 5-minute poll; and the doctor's `AccessCheck`. Like `SumiStorageProbe` (§10.1), `SumiPolicyIO` is abandoned at `OnPreExit` and never joined, so a studio share that does not answer never delays exit. A studio read that has not returned never delays the local layers either: the load is Ready once they are read and the studio layer has loaded, failed or reached its 5 s timeout." (2) Same section, Watching bullet. Replace "From M2, a 5-minute poll on `SumiPolicyIO` runs whether or not a studio path is configured: it polls the studio file when there is one, and it always stats the machine file and its folder." with "From M2, a 5-minute poll runs whether or not a studio path is configured: `SumiPolicyIO` polls the studio file when there is one, and a `UE::Tasks` task always stats the machine file and its folder." (3) TDD.md §3.1, Studio share I/O row, May-touch cell. Replace "`SumiPolicyIO`: the policy files and their 5-minute poll" with "`SumiPolicyIO`: the studio policy share (the studio layer's load, its 5-minute poll and the doctor's `AccessCheck`, §11.2.3)". (4) TDD.md §11.12, T-POL-005. Replace the scenario cell "Reload, including an edit during a run" with "Reload, including an edit during a run, and a project-policy edit made while a test hook holds a studio read blocked". At the end of the result cell ("...I keys apply within one idle tick after the run ends") append "; the edit made while the studio read is blocked also reloads within 2 s". Covers and Lands stay the same.

**Patch.** TDD §11.2.3 Startup bullet rewritten as approved. Machine and project layers load and reload as UE::Tasks tasks, and ResolveStorageRoot reads the machine layer on the GT. Every studio-share read (the studio load, which starts once the machine layer is read, its 5-minute poll and the doctor's AccessCheck) runs on SumiPolicyIO, which is abandoned at OnPreExit. Ready no longer waits on a studio read that has not returned. The Watching bullet now has SumiPolicyIO poll the studio file and a UE::Tasks task stat the machine file and its folder. In the §3.1 Studio share I/O row, the SumiPolicyIO May-touch cell is now the studio policy share (load, poll, AccessCheck, §11.2.3). T-POL-005 gains the project-policy edit made while a test hook holds a studio read blocked, and its result says that edit also reloads within 2 s. Covers and Lands (M4) are unchanged.

#### Z-persist-03

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §11.5.5 Publish to library (U33), step 4 "Studio tier with write access: 1. take `manifest.lock` ...; 2. write the item; 3. re-read the manifest, update the entry's hash and version, and write it through a temporary file and rename; 4. release the lock", the pinned-manifest paragraph, step 5 and step 6; TDD.md §11.5.1: "on `SumiLibraryIO`, a Sumi-owned `FRunnable` that does every read of the studio library share, the one-item re-sync of §11.5.4 included"; TDD.md §3.1 Studio share I/O row: "`SumiLibraryIO` at the first mirror"; "`SumiLibraryIO`: the studio library share, its mirror folder and `mirror.lock`"

**Problem.** This is the gap the round-4 patcher noted and did not close. Publish writes to the studio share: `manifest.lock`, the item, the manifest through a temporary file, or in the pinned case `manifest.next.json` and a change bundle. No text says which thread does these writes. §11.5.1 gives `SumiLibraryIO` only the share's reads, so the manifest re-read in step 4.3 belongs to it but the writes around it do not. A builder could put the writes on the journal writer's `PostIo` lane, where a blocked SMB write breaks the commit-first and 2 s rules of §10.2.1 and §10.2.5, or on `UE::Tasks`, which engine exit joins (§10.1). They could also split one publish across two threads. Nothing says what a publish does while this process's own mirror run is using `SumiLibraryIO`, or when `library.published` may be written.

**Evidence.** §11.5.6 interface: "TFuture<FSumiPublishResult> Publish(const FSumiPublishRequest& Req);            // GT call". §11.11: "Concurrent studio publish | `manifest.lock` held | Wait 5 s, then fail". §10.14 sets the same rule for saves: "never on the game thread, because a network path can block, and never on the journal writer (§10.2.1)". Y-persist-07 set the pattern of writing the audit line only after the I/O succeeds.

**Refuter.** The gap is real, and the round-4 patcher flagged it themselves. §11.5.5 never names a thread for Publish's writes to the share. §11.5.1 now gives `SumiLibraryIO` only the reads, while the §3.1 row says it touches "the studio library share" in general, so the patch left these two out of step. A builder who puts the writes on `UE::Tasks` brings back the exit-join stall that Y-persist-04 removed. The verifier's fix, however, makes up a new refusal: a publish during this process's own mirror run fails at once with a new message. Nothing calls for that. The re-sync rule exists because a run prompt is waiting on it. A publish can simply queue on the same thread. The audit-after-success clause matches the Y-persist-07 pattern and is kept. I removed the invented refusal and the abandonment sentence, which §11.5.1's existing "abandoned at `OnPreExit`" already covers.

**Fix applied.** (1) TDD.md §11.5.5. After the line "6. Audit `library.published`, then rescan." add a new paragraph: "**Threads.** `Publish` is a GT call that only queues the work and returns its future. For the studio tier, every step that touches the share runs on `SumiLibraryIO` (§11.5.1), after any mirror run already queued there, and never on the GT, the journal writer (§10.2.1), `UE::Tasks` or `GThreadPool` (§10.1). Those steps are step 1's temporary file, step 4 with the 5 s `manifest.lock` wait of §11.11, the pinned case's `manifest.next.json` and change bundle, and step 5's change folder when it is on the share. The GT completes the future, and step 6 writes `library.published` only when those writes succeeded." (2) TDD.md §11.5.1, 'The GT never touches a UNC path' bullet. Replace "a Sumi-owned `FRunnable` that does every read of the studio library share, the one-item re-sync of §11.5.4 included;" with "a Sumi-owned `FRunnable` that does every read and write of the studio library share, the one-item re-sync of §11.5.4 and Publish's studio-tier steps (§11.5.5) included;". (3) TDD.md §3.1, Studio share I/O row, lifetime cell. Replace "`SumiLibraryIO` at the first mirror;" with "`SumiLibraryIO` at the first mirror or studio publish;".

**Patch.** TDD §11.5.5: new **Threads.** paragraph after step 6, worded as in the final fix. Publish is a GT call that queues the work; the studio-tier share steps run on SumiLibraryIO after any queued mirror run, never on the GT, the journal writer, UE::Tasks or GThreadPool; library.published is written only after those writes succeed. §11.5.1 now says SumiLibraryIO does 'every read and write of the studio library share, the one-item re-sync of §11.5.4 and Publish's studio-tier steps (§11.5.5) included'. The §3.1 lifetime cell now reads '`SumiLibraryIO` at the first mirror or studio publish;'.

#### Z-persist-04

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §11.2.1 Layers, 'The studio layer' paragraph: "The doctor (U36) therefore runs `AccessCheck` for write access on the studio file and its folder (`GetFileSecurity` on the resolved UNC path, on a worker)"; TDD.md §12.4 Data-format index, 'Snippet item; shelf item' row: "studio from policy, mirrored on a worker into a new generation folder under `%LOCALAPPDATA%\Sumi\StudioLibrary\<sha8(path)>\`"

**Problem.** Y-persist-04 and its follow-on edit moved these two pieces of studio-share I/O onto named Sumi threads. The follow-on changed the §11.11 row to "on `SumiPolicyIO`" and §11.5.1 to `SumiLibraryIO`. Two more statements of the same contract still say "on a worker", which a builder can read as `UE::Tasks`, the pool whose exit join the patch exists to avoid.

**Evidence.** §11.11 row now reads "`AccessCheck` on the resolved UNC path, in the doctor, on `SumiPolicyIO`", while §11.2.1 says "on a worker" for the same check. §11.5.1 now says the mirror runs "on `SumiLibraryIO`", while §12.4 says "mirrored on a worker".

**Refuter.** Both strings are in the current text as the verifier quotes them. §11.2.1 says the studio `AccessCheck` runs "on a worker", while §11.11 (patched in round 4) says the same check runs "on `SumiPolicyIO`". The doctor's other file probes run on the §9.5 `UE::Tasks` scan task, so a doctor builder could read "a worker" as that task. §12.4 says "mirrored on a worker" while §11.5.1 now names `SumiLibraryIO`. Neither statement is false, but the patcher's stated goal was that "the same contract reads the same everywhere", and these two cheap edits finish that.

**Fix applied.** (1) TDD.md §11.2.1, 'The studio layer' paragraph. Replace "(`GetFileSecurity` on the resolved UNC path, on a worker)" with "(`GetFileSecurity` on the resolved UNC path, on `SumiPolicyIO`, §11.2.3)". (2) TDD.md §12.4, 'Snippet item; shelf item' row. Replace "mirrored on a worker into a new generation folder" with "mirrored on `SumiLibraryIO` into a new generation folder".

**Patch.** TDD §11.2.1 studio-layer paragraph: 'on a worker' became 'on `SumiPolicyIO`, §11.2.3'. TDD §12.4 Snippet item; shelf item row: 'mirrored on a worker' became 'mirrored on `SumiLibraryIO`'.

#### Z-persist-05

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §11.2.3 Startup: "Like `SumiStorageProbe` (§10.1), `SumiPolicyIO` is abandoned at `OnPreExit` and never joined, so a studio share that does not answer never delays exit."; TDD.md §11.5.1 Consistent mirrors: "a re-sync requested while this process's own mirror run is in progress is not queued behind it and gets the line below at once"; TDD.md §11.12 T-POL-006 and T-SNP-017

**Problem.** The new exit guarantee and the immediate re-sync refusal have no test. The Y-persist-04 report itself noted that T-PER-055 passes only because its fixture has no studio policy. T-POL-006 ("Unreachable studio share at startup") checks startup time and the cache, not exit. T-SNP-017 ("Studio share offline") checks GT file accesses and lock behaviour, not exit and not a re-sync requested during this process's own mirror run. A build that puts the policy load or the mirror back on `UE::Tasks` would pass every listed test.

**Evidence.** T-POL-006 result: "Added startup ≤ 30 ms; the cache is used; with no cache, closed values apply; the missing file is treated as unreachable". T-SNP-017 result: "The verified mirror runs; a probe counts 0 GT file accesses under the studio root; the locked mirror retries later; ...". No test row contains "being synced" or an exit check with a studio share.

**Refuter.** The gap is real. No row tests the new exit guarantee of `SumiPolicyIO` or `SumiLibraryIO`, or the immediate re-sync refusal during this process's own mirror run. T-POL-006 and T-SNP-017 check neither. But the proposed scenario, "a studio share that does not answer ... still not answering when the editor exits", does not catch the regression it targets. With a host that drops packets, the SMB open fails after the TCP connect timeout (about 20 to 40 s) and the thread goes idle until the next poll. If the editor exits after that, a `UE::Tasks` build exits just as fast and passes. The exit has to happen while the read is still blocked. A test hook makes that deterministic, as T-TRU-009 already does.

**Fix applied.** (1) TDD.md §11.12, T-POL-006. At the end of the scenario cell ("...a configured studio path whose share answers but has no `Policy.json`") append "; the editor exits while a test hook holds the studio load's read blocked". At the end of the result cell ("...never as an absent layer") append "; with that read blocked, exiting the editor takes at most 2 s more than the baseline". (2) TDD.md §11.12, T-SNP-017. At the end of the scenario cell ("...while a third runs an item from the current generation") append "; Restore published & run requested during this process's own mirror run; the editor exits while a test hook holds a mirror run's read of the share blocked". At the end of the result cell ("...and no process sees a missing or unverified item") append "; that re-sync shows \"The studio library is being synced; try again in a moment\" at once and runs nothing; with the mirror read blocked, exiting the editor takes at most 2 s more than the baseline". Covers and Lands stay the same for both rows (M4).

**Patch.** TDD §11.12 T-POL-006: scenario adds the editor exiting while a test hook holds the studio load's read blocked, and the result adds that exit takes at most 2 s more than the baseline. T-SNP-017: scenario adds Restore published & run requested during this process's own mirror run, and the exit while a test hook holds a mirror read blocked. Its result adds the immediate "The studio library is being synced; try again in a moment" with nothing run, and the 2 s exit bound. Covers and Lands (M4) are unchanged for both rows.

#### Z-persist-07

**Minor, patch incomplete.** Raised by the refuter.

**Where:** TDD.md §10.1 Rules, `ResolveStorageRoot` bullet: "Whether a candidate is another root is decided by folder identity (volume serial and file ID from `GetFileInformationByHandle`, or `st_dev` and `st_ino`), never by path: a candidate that is this session's own root under another path (a symlink, a junction or an alias typed as the override) is skipped"; TDD.md §10.12 T-PER-046 and T-PER-055

**Problem.** The Y-persist-05 patch added the folder-identity skip but no test for it. Its own problem statement describes a POSIX data loss. A path-based comparison lists the live session's own buffers as orphans, and the guarded delete then removes `journal/main`. The scan's open and close of its own `instance.lock` also drops the primary's fcntl lock, so a second instance can become primary on `journal/main`. No row in §10.12 has a candidate root that is the session's own root under another path. T-PER-012 covers aliases only for the project hash, and T-PER-055 has none. A build that compares candidates by path passes every listed test.

**Evidence.** T-PER-046 ("Lock content written through the handle, on Windows and (from M4) Linux") checks only second and third instances started normally. T-PER-055's scenarios name the override, the platform default, an other-SID root and a running instance, but no symlinked or junctioned alias of the session's own root. The Y-persist-05 final_fix changed only §10.1 and §11.5.1 and added no test.

**Fix applied.** TDD.md §10.12, T-PER-046. At the end of the scenario cell ("...a lock held with empty content") append "; a user-setting `storage.root_override` that is a symlink (on Windows a junction) to the platform default base folder, so the §10.1 cross-root scan meets this session's own root under another path". At the end of the result cell ("...and then treats the lock as live") append "; with that override the scan skips the platform default root, nothing of this session is listed in Review or deleted, and an instance started after the scan is Secondary". Covers (PER-05, D17) and Lands ("M1 (M4: Linux cases)") stay the same.

**Patch.** TDD §10.12 T-PER-046: the scenario adds a user-setting storage.root_override that is a symlink (a junction on Windows) to the platform default base folder. The result adds that the scan skips the platform default root, nothing of this session is listed in Review or deleted, and an instance started after the scan is Secondary. Covers (PER-05, D17) and Lands ('M1 (M4: Linux cases)') are unchanged.

### Round 5 — Runtime, delivery and UX

I checked all four round-4 fixes in my lens against the word diffs from verify5/patch/*.before.md to fix/backup-r5/*. The live docs match the backup byte for byte. check.py prints "PROBLEMS 0 {}" (369 headings, 1381 links, 815 unique test rows). A dry run of gen.py's fill() gives output identical to TDD.md, so the generated parts are current.

Two fixes are correct and complete:
- **Y-runtime-01:** The text now agrees across §8.3.3, §5.6.5, §5.7.2, §8.5, §8.1.2, §5.1.3 and §5.4.2 step 1a. A run-level Stop calls RequestStop(RunId) at once and queues terminate. The evaluate's Cancel does not queue terminate. §8.5 no longer calls RequestStop twice. T-DBG-031 now uses the input bot, which can act during an evaluate because the pump handles OS messages.
- **Y-ux-01:** The task rule in §5.9.2 agrees with §5.9.3, §5.9.5 check 0, the §6.4 footer and chip rules, SPEC §9.4 and the new D.3 row. The D.3 intro now names the X- and Y- rows.

I re-checked the engine citations added for Y-runtime-02 in the local 5.8 source and they hold. FeedbackContextEditor.cpp:471 resets the cancel flag only in StartSlowTask, and lines 702-710 read and set it. SlowTask.cpp:235-237 calls StartSlowTask only while GIsSlowTask is false, and lines 248-258 gate ShouldCancel on GIsSlowTask. FeedbackContext.h:83 sets GIsSlowTask, and FeedbackContextEditor.h:49 confirms the reset happens on a new root slow task.

Three minor gaps remain:
- **Z-other-02 (Y-runtime-02):** The §5.12 row still says Stop calls OnUserCancel whenever a user slow task is open, with no exception for the evaluate's Cancel.
- **Z-other-03 (Y-runtime-02, Y-runtime-01):** In T-DBG-031's new slow-task case, the G4(3) breakpoint is inside a loop, so one F5 does not end the run. The fixture also never records should_cancel(). The third run does not say where it is paused.
- **Z-other-01 (Y-delivery-01):** The fallback-test exclusion is limited to the go/no-go and the M1 exit. §1.5, T-GAP-008 and §14.4 item 4 still require T-CORE-036 and T-VIEW-019 when they are never built.

No file under docs or C:/Program Files was modified. The only scratch file written is scratchpad/verify6/gencmp.py, plus the diff scratchpad/verify6/tdd_r4.diff.

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [Z-other-01](#z-other-01) | minor | patch incomplete | modified | Y-delivery-01 excludes T-CORE-036 and T-VIEW-019 (tests that are built only when their §4.15 fallback is used) from only two places: G1 at the go/no-go and the M1 exit. |
| [Z-other-02](#z-other-02) | minor | patch incomplete | confirmed | Y-runtime-02 listed this §5.12 row as a location, and its problem statement says it is the row that 'makes that call whenever a user slow task is open'. |
| [Z-other-03](#z-other-03) | minor | untestable | modified | The Cancel case that Y-runtime-02 added reuses the G4(3) fixture, and that fixture's breakpoint sits inside the slow task's loop. |

#### Z-other-01

**Minor, patch incomplete.** Verdict: modified.

**Where:** TDD.md §14.3 Gate suites, intro paragraph: "Until then, neither counts toward G1 at the go/no-go, nor toward the M1 exit's "every test whose Lands is M1" (§15.2)."; TDD.md §1.5 How to read the test tables, Lands bullet: "A milestone exit requires every test whose Lands is that milestone (§15.2)."; TDD.md §14.7 test table, T-GAP-008 What-it-checks: "prints, per milestone, the tests and the split steps its exit requires"; TDD.md §14.4 Definition of done, item 4: "for a gate mechanism, the gate's automated suite (§14.3) is green."

**Problem.** Y-delivery-01 excludes T-CORE-036 and T-VIEW-019 (tests that are built only when their §4.15 fallback is used) from only two places: G1 at the go/no-go and the M1 exit. Other text still requires them. §1.5 states the general rule that an exit needs every test whose Lands is that milestone, with no exception, and both rows have Lands M1. T-GAP-008 builds the per-milestone list from Lands cells, so it will list both tests as needed at M1. §14.4 item 4 requires 'the gate's automated suite (§14.3)' to be green, and the generated G1 row (count 19) still holds both tests. Because §14.3 names its two exceptions explicitly, a reader takes the tests to count everywhere else. On the path where no fallback is used, a CI or traceability check built from §1.5 or T-GAP-008 would still report M1 as blocked by tests that are never built. Any G1-mechanism change after Phase 0 also fails DoD item 4. This is the same CI-computability defect that Y-delivery-01 set out to remove.

**Evidence.** §14.3 new text names only 'G1 at the go/no-go' and 'the M1 exit's "every test whose Lands is M1"'. §1.5: 'A milestone exit requires every test whose Lands is that milestone (§15.2).' T-GAP-008: 'reads each test's Lands and each gate's suite, and prints, per milestone, the tests and the split steps its exit requires'. §14.4 item 4: 'for a gate mechanism, the gate's automated suite (§14.3) is green'. Generated §14.3 G1 row: '..., T-CORE-036, T-VIEW-019 | 19'. gen.py dry run is identical to TDD.md, so the table is meant to keep them.

**Refuter.** Real. The round-4 sentence in §14.3 exempts T-CORE-036 and T-VIEW-019 in only two places: G1 at the go/no-go and the M1 exit. Three other places still require both tests with no exception. (a) TDD §1.5, line 265, says "A milestone exit requires every test whose Lands is that milestone (§15.2)" and, further on, "the Mx exit requires every test whose first Lands is Mx ...; T-GAP-008 lists both". (b) The T-GAP-008 row (line 8346) prints, per milestone, the tests its exit requires, built from Lands cells. Both rows have Lands M1. (c) §14.4 item 4 (line 8306) says "for a gate mechanism, the gate's automated suite (§14.3) is green", and the generated G1 row (line 8293, count 19) still contains both tests. On the no-fallback path, neither test is ever built (§4.15 rows 1932-1933 and the fallback table). §4.15 line 1920 confirms that a CI-read "automated G1 status" exists. The verifier's fix is not quite right in two ways. Edit (1) says the tests do not "belong to G1's automated suite", which contradicts the generated table in the same section, and that table is meant to keep them. Edit (2) is inserted mid-sentence, so it misses the second statement of the rule in the same §1.5 bullet ("the Mx exit requires every test whose first Lands is Mx"). All anchor strings below occur exactly once in TDD.md.

**Fix applied.** Three edits to TDD.md. Leave the generated §14.3 table and its count of 19 unchanged.
(1) §14.3 Gate suites, intro paragraph. Current text: "Until then, neither counts toward G1 at the go/no-go, nor toward the M1 exit's "every test whose Lands is M1" (§15.2)." New text: "Until then, neither is required wherever G1's automated suite or the M1 tests are read: G1 at the go/no-go (§15.1), the M1 exit's "every test whose Lands is M1" (§15.2), §14.4 item 4 and the automated G1 status that CI reads. The table below still lists both, and T-GAP-008 marks them as conditional on their fallback."
(2) §1.5 How to read the test tables, Lands bullet. Current text: "...and the named step of every split value that names Mx; T-GAP-008 lists both." New text: "...and the named step of every split value that names Mx; T-GAP-008 lists both. The §4.15 fallback tests T-CORE-036 and T-VIEW-019 are required at M1 only once their fallback is invoked at the go/no-go (§14.3)."
(3) §14.7, row T-GAP-008, What-it-checks cell. Current text: "prints, per milestone, the tests and the split steps its exit requires," New text: "prints, per milestone, the tests and the split steps its exit requires (T-CORE-036 and T-VIEW-019 marked as required only once their §4.15 fallback is invoked, §14.3),"

**Patch.** TDD §14.3 intro (the hand-written part): the exception now covers G1 at the go/no-go (§15.1), the M1 exit (§15.2), §14.4 item 4 and the automated G1 status CI reads, and notes that the table still lists both tests while T-GAP-008 marks them conditional. The §1.5 Lands bullet gains a sentence after 'T-GAP-008 lists both.' saying T-CORE-036 and T-VIEW-019 are required at M1 only once their fallback is invoked (§14.3). The T-GAP-008 What-it-checks cell gains the parenthetical marking both as conditional. The generated §14.3 table and its count of 19 are untouched.

#### Z-other-02

**Minor, patch incomplete.** Verdict: confirmed.

**Where:** TDD.md §5.12 User modals and slow tasks during runs, row "Stop while a user slow task is open": "Also calls `FFeedbackContextEditor::OnUserCancel()` ... but only if `GWarn` is still the editor context captured at `OnPostEngineInit`. Their `should_cancel()` then returns true"

**Problem.** Y-runtime-02 listed this §5.12 row as a location, and its problem statement says it is the row that 'makes that call whenever a user slow task is open'. The patch narrowed the rule in §8.3.4 and in the general §5.6.5 sentence ('for the evaluate's Cancel, only when §8.3.4 allows it'). This row, the behaviour table for slow tasks, still states the unconditional rule. §8.3.4 says the evaluate's Cancel 'calls RequestStop(EvalId) ... as the Stop buttons do'. A builder who implements the slow-task behaviour from §5.12 therefore calls OnUserCancel for the evaluate's Cancel while the paused run's ScopedSlowTask dialog is open. That sets the shared editor cancel flag, and the run's t.should_cancel() returns True after Continue, which is the defect Y-runtime-02 fixed. The new T-DBG-031 slow-task case would then fail.

**Evidence.** §5.12 row text above (unchanged by round 4). §8.3.4 (new): "`RequestStop(EvalId)` calls `OnUserCancel` (§5.12) only when `GIsSlowTask` was false at `eval_begin`." §5.6.5 (new): "calls `OnUserCancel` (§5.12; for the evaluate's Cancel, only when §8.3.4 allows it)". Engine: FeedbackContextEditor.h:49 "The user cancel flag is reset when starting a new root slow task"; FeedbackContextEditor.cpp:471, 707-710; SlowTask.cpp:235-237, 248-258 (checked in the local 5.8 source).

**Refuter.** Real, minor. The §5.12 row "Stop while a user slow task is open" (TDD line 3017) still says, with no condition, that the call is made and that "Their `should_cancel()` then returns true". Round 4 narrowed the rule in two places. §8.3.4 (line 5536) now says `RequestStop(EvalId)` calls `OnUserCancel` only when `GIsSlowTask` was false at `eval_begin`. §5.6.5 (line 2672) adds "for the evaluate's Cancel, only when §8.3.4 allows it". §8.3.4 describes the evaluate's Cancel as a `RequestStop`, made "as the Stop buttons do", so a builder working from the §5.12 behaviour table puts the call into the same `RequestStop` with no EvalId exception. The engine facts hold in the local 5.8 source. FeedbackContextEditor.cpp:471 resets the flag only in StartSlowTask, and :707-710 has OnUserCancel set it. SlowTask.cpp:235-237 calls MakeDialog→StartSlowTask only while !GIsSlowTask, and :248-258 has ShouldCancel return ReceivedUserCancel. The suggested append is accurate and fixes the gap. Its anchor "Their `should_cancel()` then returns true |" is unique.

**Fix applied.** TDD.md §5.12 User modals and slow tasks during runs, row "Stop while a user slow task is open", Behaviour cell. Current ending: "Their `should_cancel()` then returns true |". New ending: "Their `should_cancel()` then returns true. The Paused window's evaluate Cancel (`RequestStop(EvalId)`) calls it only when `GIsSlowTask` was false at `eval_begin` (§8.3.4), so it never cancels a slow task that the paused run opened |"

**Patch.** TDD §5.12 row 'Stop while a user slow task is open': after 'Their `should_cancel()` then returns true' appended '. The Paused window's evaluate Cancel (`RequestStop(EvalId)`) calls it only when `GIsSlowTask` was false at `eval_begin` (§8.3.4), so it never cancels a slow task that the paused run opened'.

#### Z-other-03

**Minor, untestable.** Verdict: modified.

**Where:** TDD.md §8.11 T-DBG-031, Scenario cell: "at the G4(3) breakpoint inside a `ScopedSlowTask` with its dialog (§8.9.1)" and "then a third run, where the Paused window's Stop button is clicked"; TDD.md §8.11 T-DBG-031, Pass cell: "then F5 continues the run at each breakpoint stop, which ends as `ok` with no `ScriptCancelled` delivered to it and, in the slow task, `t.should_cancel()` False after Continue,"; TDD.md §8.9.1 G4 table row (3) fixture: "`with unreal.ScopedSlowTask(10) as t: t.make_dialog(True)`, breakpoint in the loop"

**Problem.** The Cancel case that Y-runtime-02 added reuses the G4(3) fixture, and that fixture's breakpoint sits inside the slow task's loop. After the Cancel, F5 stops again at the next iteration, so the run does not 'end as ok' after the single Continue the row describes. An input-bot script written from the row presses F5 once and then waits for an outcome that never comes. The fixture also never calls t.should_cancel(), so nothing records the new assertion 't.should_cancel() False after Continue'. The third run added by Y-runtime-01 does not say where it is paused, unlike the second run ('paused at the first of those breakpoints').

**Evidence.** §8.9.1 (3) fixture: 'breakpoint in the loop' of `ScopedSlowTask(10)`. T-DBG-022's pass criterion is 'steps work; the task completes'. T-DBG-031 Pass: 'then F5 continues the run at each breakpoint stop, which ends as `ok`'. The patch added the slow-task case and the third run, and kept Lands M3.

**Refuter.** Part of this is wrong. The row never says F5 is pressed only once: the Pass cell reads "F5 continues the run at each breakpoint stop". The G4(3) loop stops at every iteration, so F5 is pressed at each stop, and a bot synchronised on stopped events (§14.2) does exactly that. No breakpoint condition is needed. The third-run point does not matter: Stop at any pause, breakpoint or exception stop, ends the run `cancelled` (§8.5). The proposed "paused at the same breakpoint" would also bring back the ambiguity the patcher removed when the row came to name several stops. One part is real. The G4(3) fixture (line 5771, `with unreal.ScopedSlowTask(10) as t: t.make_dialog(True)`, breakpoint in the loop) never calls `t.should_cancel()`. The new assertion "in the slow task, `t.should_cancel()` False after Continue" therefore has no defined observation point. If the call is placed after the `with` block, it passes vacuously: SlowTask.cpp:248-258 returns false whenever GIsSlowTask is false, which is checked in the local source. The ending `ok` does not catch the defect either, because the usual idiom `if t.should_cancel(): break` also ends `ok`. The smallest fix states where the value is read. Conditional breakpoints are M3 (T-DBG-056), so the verifier's `i == 3` would be allowed, but it is not needed.

**Fix applied.** TDD.md §8.11, row T-DBG-031, Scenario cell only. Current text: "at the G4(3) breakpoint inside a `ScopedSlowTask` with its dialog (§8.9.1);" New text: "at the G4(3) breakpoint inside a `ScopedSlowTask` with its dialog (§8.9.1), whose loop body calls `t.should_cancel()` on the line after the breakpoint and records each result;". Leave the Pass cell, the third-run wording, Covers and Lands (M3) unchanged.

**Patch.** TDD §8.11 T-DBG-031, Scenario cell only: the G4(3) slow-task case now reads '... with its dialog (§8.9.1), whose loop body calls `t.should_cancel()` on the line after the breakpoint and records each result;'. The Pass cell, the third-run wording, Covers and Lands (M3) are unchanged.

### Round 5 — refuted

| ID | Issue | Why it was refuted |
|---|---|---|
| Z-persist-06 | The Y-persist-03 patch is correct as approved: the CLEAN-namespace tabs are now orphans, so the n = 0 banner appears, and T-PER-055 now asserts that it does. | This is not a defect of the round-4 patch, and the verifier calls the Y-persist-03 patch "correct as approved". The round-4 refuter weighed this exact wording in Y-persist-03 and chose to leave it, changing only the listing rule and the test. The string also predates round 4: kill -9 orphans from another storage root, listed since the X- round, already showed this n = 0 banner. The text is not false, because the earlier session was another editor process, and the Review section the banner opens is titled "Tabs from another storage folder (closed 14:10)". T-PER-055 asserts only "the n = 0 banner is shown", not its wording, so the test does not pin a wrong message. Ignoring the banner loses nothing, because the namespace is deleted only after the user resolves it (§10.1, §10.7.1). This is optional copy-editing and does not affect anyone building from the documents. |

