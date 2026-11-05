> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 13 — Review of SPEC v1.0, TDD v1.0 and the mockups

> Record of the five-lens adversarial review. Each lens had a reviewer and an independent refuter, who tried to refute every finding against the documents and the local UE 5.8 install. 112 findings were raised and 6 refuted; the refuters added 2. The rulings that change decisions are in [08 §9](08-decision-log.md#9-amendments-after-the-v10-review-rv-01--rv-24); SPEC v1.1 and TDD v1.1 apply every surviving fix, as amended by the 08 §9 rulings; where a fix below differs from a ruling, the ruling wins. Probe scripts cited as evidence are in [probes/](probes/); they ran on the engine's `python.exe -I -B`. Line numbers refer to SPEC v1.0 and TDD v1.0. Paths under "scratchpad" were the reviewers' working folder and are not in the repository, except the probes copied here.

## Summary

| Lens | Raised | Survived | Refuted | Added by refuter | Critical | Major | Minor |
|---|---|---|---|---|---|---|---|
| Runtime and engine feasibility | 17 | 17 | 0 | 1 | 1 | 3 | 14 |
| Editor core, language and persistence | 26 | 26 | 0 | 1 | 2 | 10 | 15 |
| Studio, security and delivery | 27 | 26 | 1 | 0 | 1 | 13 | 12 |
| Product, UX and mockups | 19 | 15 | 4 | 0 | 0 | 3 | 12 |
| QA, traceability and consistency | 23 | 22 | 1 | 0 | 0 | 6 | 16 |

## Runtime and engine feasibility

| ID | Severity | Verdict | Finding |
|---|---|---|---|
| [RT-F01](#rt-f01) | critical | confirmed | Watchdog posts pending calls without the GIL, so pure-Python loops never pump and cannot be stopped |
| [RT-F02](#rt-f02) | major | confirmed | The Run window becomes the active top-level window, so script-parented Qt tool windows are destroyed when the run ends |
| [RT-F04](#rt-f04) | major | confirmed | debugpy's in-process listen installs pydevd's tracer on the game thread for the rest of the editor session |
| [RT-M01](#rt-m01) | major | missed | Supervised evaluates in the Paused window always count as native re-entry, so they never pump and cannot be cancelled |
| [RT-F03](#rt-f03) | minor | modified | The native re-entry check misses Python run during reinstancing, so the pump can tick Slate inside FReload |
| [RT-F05](#rt-f05) | minor | confirmed | SPEC gives a task the creating run's number; TDD and D24 make it a separate numbered run |
| [RT-F06](#rt-f06) | minor | modified | SPEC LRS-13 specifies the frame-depth re-entry signal that the TDD shows fails the generator case |
| [RT-F07](#rt-f07) | minor | confirmed | §12.1.1 lists 'native re-entered' as a pause refusal; §8 says native-entered frames pause |
| [RT-F08](#rt-f08) | minor | confirmed | GetHandledException is documented as borrowed in §2.6.3 but returns a new reference |
| [RT-F09](#rt-f09) | minor | confirmed | FSumiRunId encoding cannot give one run ID per task step |
| [RT-F10](#rt-f10) | minor | confirmed | The Paused window's request handler runs in every blocking modal loop and can re-enter during an evaluate |
| [RT-F11](#rt-f11) | minor | confirmed | The PIE-transition latch for pause rule 9 sticks after an aborted PIE start |
| [RT-F12](#rt-f12) | minor | confirmed | In the Run-window presentation, keys pressed before the window is focused never reach the pre-processor |
| [RT-F13](#rt-f13) | minor | confirmed | A breakpoint can open the Paused window nested inside the RunSupervisor's own pump |
| [RT-F14](#rt-f14) | minor | modified | While VS Code holds the game thread at a breakpoint, the pump and Stop run inside pydevd and disable IDE debugging |
| [RT-F15](#rt-f15) | minor | confirmed | Esc in a text field of the Paused window stops the debug run |
| [RT-F16](#rt-f16) | minor | confirmed | C++ and _sumi names disagree across §3, §8 and §12 |
| [RT-F17](#rt-f17) | minor | modified | SPEC and TDD use different native-interval thresholds (1 s against 100 ms) |

### RT-F01

**Watchdog posts pending calls without the GIL, so pure-Python loops never pump and cannot be stopped** — critical, confirmed

**Where:** TDD §5.4.1 line 2360; TDD §3.2 GIL-4 line 635; TDD §3.1 watchdog row line 617; TDD §2.6.3 line 546; TDD §5.15.3 T-SUP-011 line 3102; TDD §8.3.4 lines 5351-5355; SPEC LRS-01, LRS-02 lines 273-274

**Problem.** The RunSupervisor's only way to get control mid-script is a Py_AddPendingCall posted by the native watchdog thread, which the TDD says needs no GIL. In CPython 3.11 a pending call posted from a thread that is not the main thread does not set the eval breaker: the breaker is recomputed on the posting thread, and the pending-call term is gated by _Py_ThreadCanHandlePendingCalls(), which is false off the main thread. The game thread only notices the call the next time it re-takes the GIL (a reflected unreal call, time.sleep, or another Python thread forcing a GIL hand-off). A pure-Python loop (while True: pass, a comprehension, JSON or string work, print through the _sumi C writer, check_cancel(), progress()) never re-takes the GIL. So no pump runs: the Run window never opens, Esc, Shift+F5 and Stop are never read, and the SetAsyncExc channel never starts because it needs a prior delivery. The run cannot be stopped and the user must kill the editor. The same failure hits supervised evaluates in the Paused window (T-DBG-031), and §5.8 reports pure-Python stretches as native intervals. Behaviour turns erratic rather than absent whenever some other Python thread (debugpy daemons, a studio tool's thread) wakes and contends for the GIL, which will make the bug look flaky.

**Evidence.** TDD §5.4.1 line 2360: 'it sets the flag and calls Py_AddPendingCall(&PendingCallTrampoline, (void*)RunId) (no GIL needed)'. TDD §3.2 GIL-4 line 635: 'Py_AddPendingCall needs no GIL and works from any thread; the callback runs on the GT with the GIL at the next bytecode boundary of whatever Python runs there (07a Q1)'. Engine: Engine/Source/ThirdParty/Python3/Win64/include/internal/pycore_pystate.h:57-61 '_Py_ThreadCanHandlePendingCalls(void) { return _Py_IsMainThread(); }' (state fields pycore_interp.h:32 calls_to_do, :50 eval_breaker). Probe on the shipped interpreter (Engine/Binaries/ThirdParty/Python3/Win64/python.exe -I -B, 3.11.8): a Win32 timer-queue thread with no thread state calls Py_AddPendingCall at 300 ms while the main thread runs a 2 s pure-Python loop: fired_during_loop_at_s null, fired_after_loop true. Same native poster with a loop body of time.sleep(0): fired at 0.31 s. A poster that takes the GIL (ctypes callback, i.e. PyGILState_Ensure): fired at 0.32 s. Scripts: probes/pending_native.py and pending_native_fix.py. The 07a probe that D2 relies on (scratchpad/pyprobe/probe.py:66-79) posted from a Python threading.Thread through ctypes.pythonapi, which keeps the GIL and forces a hand-off, so it masked the defect.

**Challenges ruling.** D2 Rationale (08 §5 D2), 'Feasibility is verified ... pending calls run on the initializing thread', rests on the 07a probe, whose posting thread held the GIL. The D2 mechanism stays; it needs the GIL hand-off to work.

**Fix.** TDD §5.4.1 line 2360: replace '(no GIL needed)' with: 'then, outside PostLock, forces one GIL hand-off. CPython 3.11 computes the pending-call bit of the eval breaker on the posting thread, where _Py_ThreadCanHandlePendingCalls() is false (pycore_pystate.h:57-61), so the game thread notices a post only when it next takes the GIL. The watchdog sets an atomic bInPython, re-checks bPythonAlive, calls PyGILState_Ensure() and at once PyGILState_Release(), then clears bInPython. Cost: up to one switch interval (5 ms) of extra latency per post.' In the shutdown-safety bullet (line 2364), replace 'for at most 1 s' with 'until bInPython is false'. Update GIL-1 (the watchdog also takes the GIL for the hand-off), GIL-4 (drop 'needs no GIL and works from any thread ... next bytecode boundary' and state the hand-off rule), the §3.1 watchdog row, the §2.6.3 AddPendingCall comment, and §5.8 line 2783 ('In pure Python the gap is microseconds' becomes 'at most about one switch interval'). Add a note to 07a Q1 that its poster held the GIL. T-SUP-011: post from a native thread with no thread state and no other Python thread alive, against 'while True: pass'; pass when the callback runs within one period plus 10 ms; keep a no-hand-off variant as a negative control. Superseded by RV-01: the watchdog takes the GIL on its own PyThreadState, created on the watchdog thread with PyThreadState_New, with PyEval_RestoreThread and then PyEval_SaveThread, not PyGILState_Ensure and PyGILState_Release.

**Refuter.** Reproduced on the shipped 3.11.8 python.exe (-I -B). Native timer-queue poster with no thread state against a 2 s pure-Python loop: fired_during_loop_at_s null, fired_after_loop true. With time.sleep(0) in the body it fired at 0.308 s; a GIL-taking poster fired at 0.325 s. Engine header pycore_pystate.h:57-61 gates the pending-call bit on _Py_IsMainThread(), and that check runs on the posting thread. The 07a probe (scratchpad/pyprobe/probe.py:66-79) posted from a Python thread through ctypes.pythonapi and recorded only add_rc and ran_on_main, never latency. GIL-3 (TDD line 634) says reflected calls release the GIL, so unreal-calling loops work by accident. T-CAN-001's 'while True: pass', comprehension and generator shapes and T-DBG-031 cannot pass. check_cancel() cannot help either: Stop is never read, because no pump runs.

### RT-F02

**The Run window becomes the active top-level window, so script-parented Qt tool windows are destroyed when the run ends** — major, confirmed

**Where:** TDD §5.5.3 Opening step 3 line 2502; TDD §5.5.3 Teardown line 2526; TDD §8.3.1 step 9 and per-step re-creation line 5337

**Problem.** SSumiRunWindow is a regular SWindow (no IsPopupWindow) shown with FocusWhenFirstShown(true) as the active modal. While a modal is active, Slate keeps the activated regular modal as ActiveTopLevelWindow. Unreal's own way to host PySide or Qt tools, unreal.parent_external_window_to_slate(hwnd), sets the tool's Win32 owner to the active top-level regular window, which is then the Run window. At run end Sumi calls RequestDestroyWindow on it. Slate destroys child windows first and then the window; FWindowsWindow::Destroy calls ::DestroyWindow, and Windows destroys every window the Run window owns. Failure scenario: a TA runs launch_tool.py from Sumi. It imports PySide6 (well over 300 ms on first import, so the Run window opens), builds a QWidget, calls w.show() and unreal.parent_external_window_to_slate(int(w.winId())). When the run returns, the tool window vanishes with the Run window, and Qt is left holding a destroyed HWND. The Paused window has the same property and is destroyed on every step.

**Evidence.** Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PySlate.cpp:158-196: default ESlateParentWindowSearchMethod::ActiveWindow, then '::SetWindowLongPtr((HWND)ExternalWindowHandle, -8/*GWL_HWNDPARENT*/, (LONG_PTR)SlateParentWindowHandle)'. Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2889-2893 (uses GetActiveTopLevelWindow() when it IsRegularWindow()); :7156-7169 (activating the modal sets ActiveTopLevelWindow when it is regular); :2423-2456 (RequestDestroyWindow queues children first, then DestroyWindowsImmediately). Engine/Source/Runtime/SlateCore/Private/Widgets/SWindow.cpp:1603-1606 (IsRegularWindow returns !bIsPopupWindow && ...). Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsWindow.cpp:672 '::DestroyWindow( HWnd )'. The engine's own slow-task modal avoids this with .IsPopupWindow(true) (Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp:530-536). TDD line 2502 lists CreateTitleBar(false), HasCloseButton(false), SupportsMinimize(false), SupportsMaximize(false), FocusWhenFirstShown(true) and no IsPopupWindow; line 2526: 'Finish step 3 clears the destroy override and calls RequestDestroyWindow'.

**Fix.** As stated. TDD §5.5.3 step 3: add '.IsPopupWindow(true)' and '.ActivationPolicy(FApp::HasFocus() ? EWindowActivationPolicy::Always : EWindowActivationPolicy::Never)', matching FeedbackContextEditor.cpp:530-536; do the same for SSumiPausedWindow (§8.3.1). Add a G5 check that Esc and Shift+F5 still reach the window. Teardown (line 2526): before RequestDestroyWindow, re-own every top-level HWND whose GW_OWNER is the Run window to the main frame's HWND. Add a G2(5) case: a PySide window parented during a 2 s run is still visible after the run.

**Refuter.** Each step checks out in source. PySlate.cpp:176-189 (default ActiveWindow; SetWindowLongPtr GWL_HWNDPARENT). SlateApplication.cpp:2889-2892 (an active top-level regular window is the parent). SlateApplication.cpp:7156-7168 (activating the modal itself sets ActiveTopLevelWindow when it is regular). SWindow.cpp:1603-1606 (IsRegularWindow is !bIsPopupWindow ...). SlateApplication.cpp:2435-2456 (children are destroyed first). WindowsWindow.cpp:672 (::DestroyWindow destroys the windows it owns). FeedbackContextEditor.cpp:530-536 uses IsPopupWindow(true) for the slow-task modal. TDD §5.5.3 step 3 (line 2502) sets no IsPopupWindow, and the TDD never mentions PySide or parent_external_window_to_slate.

### RT-F04

**debugpy's in-process listen installs pydevd's tracer on the game thread for the rest of the editor session** — major, confirmed

**Where:** TDD §8.1.4 table line 5175 and line 5180; TDD §8.1.3 line 5167; TDD §8.6.2 lines 5488-5491; SPEC DBG-06 line 368; SPEC DBG-07 line 369

**Problem.** The trace-owner model treats debugpy as the owner only while a client is connected (is_client_connected), and DBG-06 promises sys.gettrace() is None in every non-debug run. But debugpy.listen(..., in_process_debug_adapter=True) calls pydevd.settrace, which runs enable_tracing(trace_dispatch, apply_to_all_threads=True) and set_tracing_for_untraced_contexts() at listen time, before any client connects, on the calling thread (the game thread, because §8.6.2 calls start() from an idle ticker helper) and on all other threads. The server cannot be stopped or restarted (once per session, RT-11). So after one 'Debug in VS Code', every later Sumi run in that session runs under pydevd's trace function whether or not an IDE is attached: DBG-06's acceptance fails, every run pays pydevd's per-call trace cost (T-DBG-030 does not measure it), the §8.1.4 'Free' state never returns, and the 're-enabled if sys.gettrace() is clear' branch of U16 is unreachable, so every F5 shows the foreign-tracer prompt and 'Debug anyway' swaps pydevd's tracer out and back on each run.

**Evidence.** debugpy 1.8.20, the version 09 examined (local copy: C:/Users/anima/.vscode/extensions/ms-python.debugpy-2026.6.0-win32-x64/bundled/libs/debugpy): server/api.py:139-158 (the in-process branch calls _settrace(host=..., port=..., wait_for_ready_to_run=False, block_until_connected=False)); _vendored/pydevd/pydevd.py:3158-3168 ('py_db.enable_tracing(py_db.trace_dispatch, apply_to_all_threads=True)', then 'py_db.set_tracing_for_untraced_contexts()'); pydevd.py:1128-1168 (enable_tracing ends in 'pydevd_tracing.SetTrace(thread_trace_func)'). TDD line 5175: owner 'debugpy | debugpy.is_client_connected() (polled, §8.6.3)'; line 5167: 'Outside Debug runs Sumi never sets sys.gettrace() (DBG-06)'; line 5491: 'there is no Stop debug server'. SPEC line 368, DBG-06 acceptance: 'sys.gettrace() is None in every non-debug run'.

**Fix.** TDD §8.6.2: right after listen() returns, and again whenever the 2 Hz poll sees is_client_connected() go false, call the public debugpy.trace_this_thread(False) on the game thread (public_api.py:200; server/api.py:358-366 calls pydb.disable_tracing()). Call debugpy.trace_this_thread(True) when the poll sees a client connect. Add T-DBG cases against the pinned pydevd: with the server listening and no client, sys.gettrace() is None in a non-debug run; after a connect, a VS Code breakpoint on the game thread still hits; after a disconnect, the 'Free' state returns. Only if that test fails, use the reviewer's fallback: a §8.1.4 owner row 'debugpy listening, no client' (DebugpyBridge.is_listening()) with swap-and-restore and no F5 prompt; limit SPEC DBG-06 and TDD §8.1.3 to 'until the debug server is started'; say so in the 'Debug in VS Code' confirmation; record the overhead in T-DBG-030.

**Refuter.** Source confirmed in the local debugpy 1.8.20: server/api.py:148-158 (the in-process branch calls _settrace with block_until_connected=False), and pydevd.py:3158-3166 runs enable_tracing(trace_dispatch, apply_to_all_threads=True) and set_tracing_for_untraced_contexts() on both connect paths. Probe on the engine's python.exe (scratchpad/review/verify/dbg_listen.py): before listen sys.gettrace() is None; after debugpy.listen(('127.0.0.1',0), in_process_debug_adapter=True), with no client and is_client_connected() False, sys.gettrace() is pydevd's SafeCallWrapper. A 200k-call loop took 0.239 s against a 0.037 s baseline (about 6.5x). DBG-06's acceptance and the 'Free' state of §8.1.4 are therefore unreachable after the first 'Debug in VS Code'.

### RT-M01

**Supervised evaluates in the Paused window always count as native re-entry, so they never pump and cannot be cancelled** — major, missed

**Where:** TDD §8.3.4 lines 5351-5355; TDD §8.3.2 line 5341; TDD §8.3.3 line 5347; TDD §5.4.5 line 2431; TDD §5.4.2 step 5 line 2374; TDD T-DBG-031 line 5646

**Problem.** The entry mark is captured only in user_code_begin (lines 2301, 2431, 8735). A pause calls PyEval_SaveThread (step 8, line 5332) with the counter at the mark. The Paused window's handler then takes the GIL with PyGILState_Ensure (FPyScopedGIL or FSumiScopedGil, lines 5129, 5341, 5347), which raises the game thread's gilstate_counter to mark + 1 for the whole evaluate. Every pending call during the evaluate hits the 'Native re-entry since _run began' deferral at §5.4.2 step 5 (line 2388) before both the pump (step 8) and delivery (step 6). So 'Evaluating… [Cancel]' never paints and Cancel is never delivered. An evaluate of 'while True: pass' hangs the editor, the user must kill it, and unsaved work is lost. This holds even after RT-F01 is fixed.

**Evidence.** TDD line 2431: 'IsNativeReentered(Mark) is PyThreadState_Get()->gilstate_counter > Mark.GilCounter. Mark is captured in user_code_begin.' Line 5341: 'Nested Python re-takes it with FPyScopedGIL; PyGILState_Ensure on the game thread restores the saved thread state'. Line 5352: 'eval_begin(run_id) returns EvalId, and the pump runs for that id although the run is paused' (no new mark). The TDD's own T-SUP-013 (line 3104) expects the counter to be 'greater in the native callback via PyGILState_Ensure'. Line 5646: T-DBG-031 'Supervised evaluate of while True: pass | Repaint <= 1 s; Cancel works'.

**Fix.** TDD §8.3.4: '_sumi.eval_begin(run_id) captures a fresh entry mark (FSumiPyCompat::CaptureEntryMark) for the evaluate; while an evaluate is active, the §5.4.3 re-entry check and the §5.7.5 condition 4 compare against the evaluate's mark; eval_end restores the run's mark.' Add the same sentence to §5.4.5. Extend T-DBG-031 to assert at least one pump and a delivered Cancel within 1 s.

**Refuter.** added by verifier

### RT-F03

**The native re-entry check misses Python run during reinstancing, so the pump can tick Slate inside FReload** — minor, modified

**Where:** TDD §5.3.10 line 2329; TDD §5.4.2 steps 1, 5 and 8 (lines 2370-2377); TDD §5.4.5 line 2431; TDD §5.7.7 line 2690

**Problem.** §5.3.10 says a pending call that fires in a PostInitInstance override during reinstancing 'is a deferral'. It is not. The entry mark is captured in user_code_begin, inside RunString's FPyScopedGIL, so it includes RunString's own PyGILState_Ensure. ProcessPending runs after that scope is released (counter down by one); PostInitInstance's FPyScopedGIL raises the counter back to exactly the mark, so IsNativeReentered (counter > mark) is false. CurrentRunId stays valid until EndRun, PumpDepth is 0, none of the RT-02 trio is set, and step 8 pumps without checking bUserCodeActive (only delivery checks it). Because of RT-F01, an outstanding call is noticed exactly at that Ensure's GIL re-take, so the first bytecode of _post_init runs a full pump (PumpMessages, Tick(All), AsyncTask(GameThread) work in Sync) while FReload is replacing instances of the old class, which is what RT-02 forbids. Failure scenario: re-running a buffer that redefines @unreal.uclass() class Marker(unreal.Actor) with a _post_init while Marker actors are in the level; the Outliner and Details panels tick against half-replaced actors mid-FReload.

**Evidence.** Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:1843-1876 ('{ FPyScopedGIL GIL; ... EvalString ... }') and :1879 'FPyWrapperTypeReinstancer::Get().ProcessPending();' after the scope closes. PyGIL.h:14-24 (FPyScopedGIL is PyGILState_Ensure/Release). PyWrapperObject.cpp:2024-2041 (UPythonGeneratedClass::PostInitInstance takes FPyScopedGIL and calls PyPostInitFunction). PyWrapperTypeRegistry.cpp:798-821 (ProcessPending builds an FReload of type Reinstancing). TDD line 2329: 'Python PostInitInstance overrides called during it enter through FPyScopedGIL, so a pending call that fires there is a deferral (§5.4.5)'. Line 2431: 'IsNativeReentered(Mark) is PyThreadState_Get()->gilstate_counter > Mark.GilCounter. Mark is captured in user_code_begin'. Line 2690: EndRun sets CurrentRunId = 0 and disarms the watchdog only after ExecPythonCommandEx returns.

**Fix (as modified by the refuter).** TDD §5.4.2 step 5: add 'or !bUserCodeActive: record deferral reason OutsideUserCode and return 0' (step 4's C++ drain may still run). §5.7.6/§5.7.7: user_code_done sets an atomic that stops new posts and does not wait for the watchdog (GIL-6); EndRun still waits for the watchdog to leave Python and drains as now. Rewrite §5.3.10's last sentence: 'A pending call that fires in a PostInitInstance override during reinstancing is refused by the user-code-window gate (§5.4.2 step 5); the re-entry mark does not catch it, because the counter equals the mark there.' Add a T-SUP case: a re-run that redefines a @uclass whose _post_init loops for 200 ms, with a call outstanding at user-code end, gives zero pumps during ProcessPending. The same gate covers task steps.

**Refuter.** The mechanism is real. RunString's FPyScopedGIL closes (PythonScriptPlugin.cpp:1844-1877) before ProcessPending (:1879). PostInitInstance's FPyScopedGIL (PyWrapperObject.cpp:2031) brings gilstate_counter back to exactly the mark, so the §5.3.10 claim that this 'is a deferral' is false. Step 1 and step 8 pump until EndRun clears CurrentRunId (line 2690). The harm is overstated, though. In 1.0 only the Run-window presentation exists (docked is cut to 1.1, line 8239). Under a modal, Slate draws and ticks only the modal, tooltips and notifications (SlateApplication.cpp:1476-1497), so the Outliner and Details panels do not tick. The engine itself pumps Slate mid-reinstance through a slow task (KismetReinstanceUtilities.cpp:286-291, MakeDialogDelayed(1.0)). What remains is a false safety claim and a contained gap.

### RT-F05

**SPEC gives a task the creating run's number; TDD and D24 make it a separate numbered run** — minor, confirmed

**Where:** SPEC LRS-05 line 277; TDD §5.1.1 line 2042; TDD §5.10.4 lines 2918-2919

**Problem.** SPEC says the creating run itself continues as the task with the same number. TDD says the task is a new run with its own number and record, started after the creating run ends. The two models give different run records, outcomes for the creating run, undo titles and WS-07 strings.

**Evidence.** SPEC line 277: 'when the run's top-level code returns, the run (same #N) enters Task running'. TDD line 2042: 'A task is its own run with its own number (D24); the run that created it ends first.' TDD line 2919: 'It is its own run, with its own number and record (D24).' 08 D24 (line 1363): 'the task is its own run, with its own transaction'; U8: 'Every run gets a number'.

**Fix.** SPEC LRS-05: replace 'when the run's top-level code returns, the run (same #N) enters Task running' with 'the task is its own numbered run (D24, U8); it starts at the first OnEndFrame after the creating run ends and enters Task running'.

**Refuter.** SPEC line 277 says 'the run (same #N) enters Task running'. TDD line 2042 and lines 2918-2919 say the task is its own run with its own number, and task_register (line 7498) returns the task's run number. D24 (08 line 1363, 'the task is its own run') and U8 back the TDD.

### RT-F06

**SPEC LRS-13 specifies the frame-depth re-entry signal that the TDD shows fails the generator case** — minor, modified

**Where:** SPEC LRS-13 line 285; TDD §5.4.5 lines 2428-2433; TDD T-SUP-013 line 3104

**Problem.** A builder implementing LRS-13 as written compares frame depth, which in 3.11 changes on every C-level evaluator entry (generator resumption, __init__, sorted(key=...)), so Stop would be deferred forever inside generator loops and G2(1) would fail.

**Evidence.** SPEC line 285: 'or when Python was re-entered from native code since _run began (frame depth compared, D15 layer)'. TDD line 2433: 'A cframe comparison would defer Stop inside generator bodies forever and fail G2(1)'s generator shape.' TDD line 2431 uses gilstate_counter instead.

**Fix (as modified by the refuter).** SPEC LRS-13: replace '(frame depth compared, D15 layer)' with '(detected in the D15 compatibility layer; the 3.11 mechanism is in TDD §5.4.5 and OI-33)'. When OI-33 closes, the RT-02 owner amends RT-02's 'frame depth or entry frame (3.11: cframe)' wording.

**Refuter.** The contradiction is real: frame depth rises on every Python call, and TDD line 2433 shows cframe fails for generators. The SPEC text, though, copies RT-02 (08 line 1573), and the TDD already tracks the deviation as OI-33, §2.9 item 1 and §5.16 item 2, pending T-ARCH-010 and CA confirmation. Writing gilstate_counter into the SPEC now would settle an open item outside the TDD.

### RT-F07

**§12.1.1 lists 'native re-entered' as a pause refusal; §8 says native-entered frames pause** — minor, confirmed

**Where:** TDD §12.1.1 line 7500; TDD §8.1.5 lines 5187-5188; TDD §8.4 line 5429; TDD T-DBG-036 line 5651

**Problem.** Whether a breakpoint in a Python UFUNCTION override or an import hook pauses depends on which section the builder reads.

**Evidence.** Line 7500: 'pause_context ... (not a debug run, inside a Python Slate or ticker callback, ticking, engine busy, native re-entered, unattended)'. The ESumiPauseRefusal enum (lines 5187-5188) has no native re-entry value. Line 5429: 'A user frame entered from C++ during the run ... may pause unless rules 6-9 refuse.' T-DBG-036: 'Both pause and resume'.

**Fix.** TDD §12.1.1 line 7500: remove 'native re-entered' from the reason list and add 'reasons per ESumiPauseRefusal (§8.1.5) and §8.4'.

**Refuter.** Line 7500 lists 'native re-entered' as a pause_context reason. ESumiPauseRefusal (lines 5187-5188) has no such value, and §8.4 line 5429 says native-entered user frames may pause.

### RT-F08

**GetHandledException is documented as borrowed in §2.6.3 but returns a new reference** — minor, confirmed

**Where:** TDD §2.6.3 line 545; TDD §5.7.4 line 2659

**Problem.** A builder who trusts the '// borrowed' comment never decrefs. IsHandlingOurs runs on every pending call during cleanup (10-20 Hz), and each call would leak a reference to the handled ScriptCancelled, its traceback and frames, which pin UObjects past the §8.7 release triggers.

**Evidence.** Line 545: 'static PyObject* GetHandledException();  // borrowed (RT-06)'. Line 2659: 'H = PyErr_GetHandledException() (.../include/pyerrors.h:22, a new reference)'. PyErr_GetHandledException is declared at Engine/Source/ThirdParty/Python3/Win64/include/pyerrors.h:22; CPython 3.11 documents it as returning a new reference.

**Fix.** TDD §2.6.3 line 545: change the comment to '// new reference (RT-06); the caller holds it in a TSumiPyRef'.

**Refuter.** PyErr_GetHandledException is declared at pyerrors.h:22. A probe on the engine's python.exe showed the handled exception's refcount rising from 3 to 13 after 10 calls with no decref, so each call returns a new reference. §2.6.3 line 545 says 'borrowed'; §5.7.4 line 2659 says 'a new reference'.

### RT-F09

**FSumiRunId encoding cannot give one run ID per task step** — minor, confirmed

**Where:** TDD §5 Terms line 1977; TDD §5.2.5 line 2147; TDD §12.1.2 line 7511; TDD §5.3.7 line 2281

**Problem.** §5 defines the run ID as unique per entry into _run, with one ID per task step, but §12.1.2 encodes it from the run number, which a task keeps for all its steps. The runner pops _PREPARED[run_id] per entry, the record stores 'run IDs (one per step)', and stale-call guards compare IDs; with the §12 encoding all steps share one ID.

**Evidence.** Line 1977: 'unique per entry into _run within the editor process, never reused ... A task has one run number and one run ID per step'. Line 7511: 'FSumiRunId is a uint64: (Generation << 32) | Number'. Line 2147: 'using FSumiRunId = uint64; // (Generation << 32) | Number'.

**Fix.** As stated: TDD §12.1.2 and §5.2.5 define FSumiRunId as (Generation << 32) | EntrySerial, a per-process counter bumped at every entry into _run. The run number stays a separate field of FSumiRunRecord and of the _sumi calls that report it (task_register, ScriptCancelled.run_number).

**Refuter.** Line 1977 says the run ID is unique per entry into _run, with one per task step. Line 2108 stores 'run IDs (one per step for tasks)'. Lines 2147 and 7511 encode (Generation << 32) | Number, and a task keeps its number across steps, so the run-ID guard cannot tell step k's stale call from step k+1.

### RT-F10

**The Paused window's request handler runs in every blocking modal loop and can re-enter during an evaluate** — minor, confirmed

**Where:** TDD §8.3.3 line 5347; TDD §8.10 line 5602

**Problem.** ModalLoopTickEvent is broadcast by every blocking AddModalWindow loop, not only the Paused window's. An evaluate runs inside this handler and is allowed to open an EditorDialog, whose own loop broadcasts the event again. If Enter in the evaluate console and F10 land in the same iteration, the nested broadcast drains 'step over' and calls CallDebugger inside the running evaluate; the resume path destroys the Paused window while the evaluate and the dialog are still on the stack.

**Evidence.** Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2238-2253: the blocking loop calls 'ModalLoopTickEvent.Broadcast(DeltaTime)' for whichever modal is blocking. TDD line 5347: 'A handler on FSlateApplication::Get().GetOnModalLoopTickEvent() (SAH:1851) drains the queue at the top of each modal-loop iteration'. Line 5602: 'An evaluate raises, or opens an EditorDialog ... a nested modal stacks'.

**Fix.** As stated: TDD §8.3.3: the handler acts only when GetActiveModalWindow() is the Paused window and a bInRequest flag is clear; it processes one request per iteration; requests queued during an evaluate wait until it returns.

**Refuter.** SlateApplication.cpp:2238-2253: every blocking modal loop broadcasts ModalLoopTickEvent. TDD §8.3.3 (line 5347) has no guard on which modal is iterating, and §8.10 (line 5602) allows an evaluate to open an EditorDialog. A request queued during a supervised evaluate's pump, such as F10 clicked while 'Evaluating…' shows, is drained by the dialog's loop inside the evaluate.

### RT-F11

**The PIE-transition latch for pause rule 9 sticks after an aborted PIE start** — minor, confirmed

**Where:** TDD §8.4 rule 9 line 5419

**Problem.** Rule 9's flag is set by PreBeginPIE and EndPIE and cleared only by PostPIEStarted and ShutdownPIE. PIE aborts after PreBeginPIE broadcast CancelPIE, and some also broadcast EndPIE, but never PostPIEStarted or ShutdownPIE. Failure scenario: the user presses Play, gets the Blueprint compile-error dialog and chooses not to continue; from then on every breakpoint in every Debug run is skipped with 'PIE is starting or stopping' until a successful PIE session ends.

**Evidence.** Engine/Source/Editor/UnrealEd/Private/PlayLevel.cpp:2584 (PreBeginPIE is broadcast 'before checks that might block PIE'); :2616-2630 (authorizer denial: 'CancelRequestPlaySession(); return;'); :1014-1018 (CancelRequestPlaySession broadcasts only CancelPIE); :2671-2674 and :2694-2697 (EndPIE, CancelRequestPlaySession, return, with no PostPIEStarted or ShutdownPIE). TDD line 5419: 'a flag set by FEditorDelegates::PreBeginPIE and EndPIE, cleared by PostPIEStarted and ShutdownPIE'.

**Fix.** As stated: TDD §8.4 rule 9: also clear the flag on FEditorDelegates::CancelPIE (Editor.h:298). Add a T-DBG-032 case: an aborted PIE start (Blueprint error dialog, Cancel), then a breakpoint pauses.

**Refuter.** PlayLevel.cpp:2583 broadcasts PreBeginPIE before the checks. The authorizer denial (:2616-2630) and the Blueprint-error and asset-compiler aborts (:2671-2674, :2694-2697) end in CancelRequestPlaySession (:1014-1018), which broadcasts only CancelPIE, sometimes after EndPIE. None broadcasts PostPIEStarted or ShutdownPIE, so the rule-9 latch (TDD line 5419) sticks. CancelPIE is declared at Editor.h:298.

### RT-F12

**In the Run-window presentation, keys pressed before the window is focused never reach the pre-processor** — minor, confirmed

**Where:** SPEC LRS-11 line 283; TDD §5.5.3 line 2510; TDD T-SUP-027 line 3118

**Problem.** LRS-11 says the pre-processor sees every key during a supervised run, and its acceptance requires 'Esc with the viewport focused stops a run'. With a modal open, the Windows platform layer consumes input messages aimed at any other window before FSlateApplication::OnKeyDown, which is where pre-processors run. In the default presentation the viewport can hold focus only during the first ~300 ms; the TDD itself says the queued input is dropped after the modal opens. So an Esc or Shift+F5 pressed in the viewport is lost, and the LRS-11 acceptance can pass only in the docked Preview.

**Evidence.** Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:3195-3205 ('if ( !MessageHandler->ShouldProcessUserInputMessages( CurrentNativeEventWindowPtr ) && IsInputMessage( msg ) ) ... return 0; // consume input messages'); SlateApplication.cpp:4884-4898 (ShouldProcessUserInputMessages is false for non-modal windows); pre-processors run inside ProcessKeyDownEvent (SlateApplication.cpp:4982). TDD line 2510: 'Input queued by the OS during the first 300 ms is dispatched by the first pump's TickPlatform after the modal exists, so the platform filter drops it'. SPEC line 283: 'During a supervised run the pre-processor sees every key' and 'Esc with the viewport focused stops a run'.

**Fix.** SPEC LRS-11: replace 'During a supervised run the pre-processor sees every key' with 'From the first pump, the pre-processor sees every key delivered to the active modal (Run window) or to any window (docked Preview); keys pressed before the Run window appears are discarded (RT-04)'. Replace the acceptance 'Esc with the viewport focused stops a run' with 'Esc or Shift+F5 pressed after the Run window appears stops the run'. Drop the optional PeekMessage idea, or keep it as a TDD note.

**Refuter.** WindowsApplication.cpp:3195-3205 consumes deferred input for windows that fail ShouldProcessUserInputMessages, and SlateApplication.cpp:4886-4898 makes that false for non-modal windows under a modal. Pre-processors run only after that, in ProcessKeyDownEvent (:4982). TDD line 2510 says the first 300 ms of input is dropped. Once the modal is up, the viewport cannot hold focus: activating another window refocuses the modal (:7189-7193). In M1, which has only the Run window, LRS-11's 'Esc with the viewport focused stops a run' can be met only by Slate-level key injection.

### RT-F13

**A breakpoint can open the Paused window nested inside the RunSupervisor's own pump** — minor, confirmed

**Where:** TDD §8.1.5 lines 5187-5188; TDD §8.4 rules (lines 5409-5420) and line 5429; TDD §5.4.4 line 2415; TDD §5.4.8 line 2464

**Problem.** No pause rule refuses when the pump is on the stack. The pump's Renderer->Sync() processes queued AsyncTask(GameThread) work, IsTicking() is already false there, and §8.4 lets native-entered Python pause. In a Debug run, a Python delegate fired by such a task with a breakpoint opens the blocking Paused loop inside the game-thread task that FFrameEndSync::Sync is processing, inside our pending call. While paused, the modal loop's own Sync skips game-thread tasks because the game thread is already processing tasks, so all other queued game-thread work stalls for the whole pause, and after Continue the pump resumes mid-Sync. This also contradicts §5.4.8's 'No pump and no delivery inside nested Python'.

**Evidence.** Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp:2560-2565 ('if (FlushMode == EFlushMode::EndFrame && !FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread)) { FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread); }'); Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp:1845-1848 (Sync is FFrameEndSync::Sync(EndFrame)). TDD line 2415: 'App.GetRenderer()->Sync(); // runs queued AsyncTask(GameThread) work'. The ESumiPauseRefusal enum (lines 5187-5188) has no pump value; line 5429: 'may pause unless rules 6-9 refuse'.

**Fix.** As stated: TDD §8.1.5 and §8.4: add ESumiPauseRefusal::InsidePump, used when FRunSupervisor::GetPumpDepth() > 0, notice 'inside an editor update during the run'; add the case to T-DBG-032.

**Refuter.** RenderingThread.cpp:2560-2565 runs ProcessThreadUntilIdle(GameThread) inside Sync(EndFrame) unless the game thread is already processing tasks, and SlateRHIRenderer.cpp:1845-1848 shows Sync is that call. The pump calls it after Tick returns, so IsTicking is false. Rule 3 covers only wrapped Slate and ticker callbacks. The GT thread state restored by PyGILState_Ensure keeps its trace function, so a delegate fired by a queued task is traced and can pass rules 1-10. No pump-depth refusal exists (lines 5187-5188, 5409-5420).

### RT-F14

**While VS Code holds the game thread at a breakpoint, the pump and Stop run inside pydevd and disable IDE debugging** — minor, modified

**Where:** TDD §5.4.3 deferral table lines 2383-2393; TDD §8.6.3 line 5499

**Problem.** Normal Sumi runs stay supervised while a debugpy client is attached (only Debug runs are disabled). When the IDE suspends the game thread, pydevd's wait loop keeps executing Python there and keeps re-taking the GIL, which is neither a native re-entry nor any other deferral condition. So the pump runs and the Run window stays live, contradicting the 'whole editor is frozen' banner. A Stop then raises ScriptCancelled inside pydevd's frames; pydevd re-raises it out of its trace function, and Python removes the trace function on that thread. VS Code debugging of the game thread then stays disabled for the rest of the session, because the debug server cannot be restarted (RT-11).

**Evidence.** Local debugpy 1.8.20 (C:/Users/anima/.vscode/extensions/ms-python.debugpy-2026.6.0-win32-x64/bundled/libs/debugpy): _vendored/pydevd/pydevd.py:2238-2268 (the suspend loop runs Python on the suspended thread: 'queue.get(False)', 'internal_cmd.do_it(self)', 'notify_event.wait(wait_timeout)'); _pydevd_bundle/pydevd_cython.pyx:1355-1367 ('Unfortunately Python itself stops the tracing when it originates from the tracing function ... Debugging will be disabled for this thread', then 'raise'). TDD §5.4.3 has no external-debugger condition. Line 5499: 'While it has the editor paused, the whole editor is frozen'.

**Fix (as modified by the refuter).** TDD §5.4.3: add a row 'innermost Python frame is in the vendored debugpy/pydevd directory (the §5.4.7 frame walk)': skip delivery and the §5.7.5 channel, keep pumping. The Run window shows 'Paused in VS Code: Stop takes effect when you continue in the IDE'. Reword the §8.6.3 banner (line 5499) to 'code outside Sumi runs freezes the editor while the IDE has it paused; a Sumi run's Run window stays live'. Add a T-DBG case: Stop while VS Code holds the game thread; after Continue the run is cancelled and a later IDE breakpoint still hits.

**Refuter.** The defect is real. pydevd's suspend loop (pydevd.py:2237-2268) runs Python on the suspended game thread and calls notify_event.wait(timeout), which releases and re-takes the GIL. A pending call therefore fires there with the counter equal to the mark, and a delivered ScriptCancelled leaves through the trace function, which pydevd_cython.pyx:1355-1367 says disables debugging for that thread. The proposed fix contradicts itself, though: 'no pump' and 'the Run window shows Paused in VS Code' cannot both hold, because only a pump paints the window, and without pumps Windows ghosts the editor. Pumping inside the wait loop is no riskier than any other pump.

### RT-F15

**Esc in a text field of the Paused window stops the debug run** — minor, confirmed

**Where:** TDD §5.6.3 lines 2571-2576; TDD §8.3.5 line 5379; MOCKUP 05-debugger-paused (Watch: + Add watch)

**Problem.** Pre-processors see KeyDown before widgets, and the only text field §5.6.3 exempts is the evaluate console. The Paused window also has editable watch rows ('+ Add watch') and a breakpoint-condition editor. A user who presses Esc to cancel an inline edit ends the run as cancelled. RT-05 allows narrowing the Esc rule further ('only when'), as the TDD already does for the evaluate console.

**Evidence.** Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:4982 ('if (InputPreProcessors.HandleKeyDownEvent(*this, InKeyEvent))' runs before focus routing). TDD lines 2571-2574 list the Esc conditions, the third being 'the Paused window's evaluate console does not have keyboard focus'. Line 5379: 'Stop | Shift+F5; Esc when no menu or popup is open'. Mockup 05 shows an editable '+ Add watch' row.

**Fix.** As stated: TDD §5.6.3: add a condition 'the keyboard-focused widget in our window is not editable text (SEditableText, SMultiLineEditableText or an editable SCodeView)', where Esc goes to the widget; add the watch editor to T-DBG-035.

**Refuter.** Pre-processors see KeyDown before focus routing (SlateApplication.cpp:4982). §5.6.3 (lines 2571-2574) exempts only the evaluate console, and mockup 05 (mockups/05-debugger-paused.html:268) shows an '+ Add watch' row in the Paused window. Esc while typing a watch therefore stops the debug run. RT-05's 'only when' allows the narrowing.

### RT-F16

**C++ and _sumi names disagree across §3, §8 and §12** — minor, confirmed

**Where:** TDD §3.2 GIL-2 line 633; TDD §8.1.1 line 5129; TDD §8.1.5 lines 5190-5198; TDD §12.3 line 7597

**Problem.** Several interface names and types differ between sections, so builders of the two sides would not link up.

**Evidence.** GIL-2 (line 633) exempts '_sumi' function 'pause_enter'; §8.1.5 and §12.1.1 (line 7501) define 'pause'. Line 5129: 'CallDebugger(const FString&) under FPyScopedGIL', while §3.2 line 628 says that header is private and Sumi uses FSumiScopedGil. §8.1.5 uses 'int64 RunId' in CanPause, BeginSupervisedEvaluate and FSumiStopInfo, while §5.2.5 and §12.1.2 define FSumiRunId as uint64. §12.3 line 7597 lists FSumiDeferral entry points 'Defer, CanRunNow'; §3.7 lines 825-829 declare AddTicker, RemoveTicker, CanRunNow and GetDeferredCount, with no Defer.

**Fix.** As stated: TDD line 633 'pause_enter' becomes 'pause'; line 5129 'FPyScopedGIL' becomes 'FSumiScopedGil'; §8.1.5 'int64 RunId' becomes 'FSumiRunId' (FSumiStopInfo, CanPause, BeginSupervisedEvaluate); line 7597 becomes 'AddTicker, RemoveTicker, CanRunNow, GetDeferredCount'.

**Refuter.** All four mismatches are present. 'pause_enter' at line 633 against 'pause' at lines 5124 and 7501. 'FPyScopedGIL' at line 5129 against §3.2 line 628 (the header is private; use FSumiScopedGil). int64 RunId at lines 5190-5198 against uint64 FSumiRunId (lines 2147, 7511). §12.3 line 7597 'Defer, CanRunNow' against the §3.7 declarations (lines 825-829), which have no Defer.

### RT-F17

**SPEC and TDD use different native-interval thresholds (1 s against 100 ms)** — minor, modified

**Where:** SPEC LRS-03 line 275; TDD §5.8 line 2785; TDD T-SUP-030 line 3121

**Problem.** The user-visible 'last native call took ...' note and the run footer totals differ by 10x depending on which document is followed, and the two tests would disagree.

**Evidence.** SPEC line 275: 'The watchdog counts a pending call that waited over 1 s (proposed: four re-delivery periods ...) as native time.' TDD line 2785: 'A gap of at least 100 ms (proposed: one period at 10 Hz) is a native interval.'

**Fix (as modified by the refuter).** SPEC LRS-03: replace 'counts a pending call that waited over 1 s (proposed: four re-delivery periods, so jitter is not reported) as native time' with 'counts a pending-call gap of at least 100 ms (one period at 10 Hz) as native time, and shows the live note only for a gap over 1 s, so jitter is not reported'. TDD §5.8 table, row 'A native interval ended': add '(intervals over 1 s; shorter ones count only toward the totals)'. T-SUP-030 (3 s) is unchanged.

**Refuter.** The contradiction is real: SPEC line 275 says over 1 s 'so jitter is not reported', and TDD line 2785 says at least 100 ms. But the proposed fix shows the live note at 100 ms, which drops the SPEC's stated anti-jitter intent: every 0.1 s UFunction call would produce a note. Measurement and display need separate thresholds.

## Editor core, language and persistence

| ID | Severity | Verdict | Finding |
|---|---|---|---|
| [CORE-F01](#core-f01) | critical | confirmed | Unrestored recovered texts are lost after two journal rotations and a second crash |
| [CORE-M01](#core-m01) | critical | missed | instance.lock content is written by atomic replace, which defeats the lock it lives in |
| [CORE-F02](#core-f02) | major | confirmed | A second editor instance shares local history, REPL history and state files with no ownership rule |
| [CORE-F03](#core-f03) | major | confirmed | Saving files and handling external changes (BUF-03 Save, BUF-04) have no design; the references point to a section that does not contain it |
| [CORE-F04](#core-f04) | major | confirmed | Literal subscripts and paging on unreal.Array / unreal.Map load soft-referenced assets during completion, Variables and safe watches |
| [CORE-F05](#core-f05) | major | confirmed | 'repr only on built-in types' runs user __repr__ and unbounded work on the game thread |
| [CORE-F06](#core-f06) | major | confirmed | REPL completeness uses codeop in 'exec' mode, which submits after the first body line |
| [CORE-F08](#core-f08) | major | confirmed | Deep reload runs every unload hook up front, then 'restores' old modules whose hooks already tore down their registrations |
| [CORE-F09](#core-f09) | major | confirmed | One journal writer thread serialises commits behind history compression, retention sweeps and full checkpoints, so the 2 s bound does not hold |
| [CORE-F10](#core-f10) | major | confirmed | G3 must pass at M1 exit, but the Stage 2 transformer it measures is scheduled for M2 |
| [CORE-F11](#core-f11) | major | confirmed | Section 12 contracts disagree with sections 6, 7 and 9 on native signatures, units, helper allow-list, version key and run-ID width |
| [CORE-F19](#core-f19) | major | modified | Pending recovery behaves differently in SPEC, TDD and mockup 06 (read-only state and the orphan count) |
| [CORE-F07](#core-f07) | minor | modified | Line splitter discards output whose CR and LF arrive in separate writes, which is how print() writes CRLF text |
| [CORE-F12](#core-f12) | minor | confirmed | 'Plain mode' means two things, and the 5 MB language-server limit contradicts itself |
| [CORE-F13](#core-f13) | minor | confirmed | Language-server memory cap is measured as private commit, not RSS, at a different rate than the gate |
| [CORE-F14](#core-f14) | minor | confirmed | Memory targets cannot be met by the described layout: T-ENC-007 '<= 3x text' and PERF-11 core '<= 50 MB + 3x' |
| [CORE-F15](#core-f15) | minor | confirmed | PERF-18 (pre-run ack <= 16 ms p95) is below the TDD's own flush estimate and its per-frame polling |
| [CORE-F16](#core-f16) | minor | confirmed | FSumiOutputDevice is specified lock-free but attributes through an RW-locked table, and it re-captures the console executor's own echo |
| [CORE-F17](#core-f17) | minor | confirmed | Deep reload can re-import stale bytecode after a same-second, same-size save |
| [CORE-F18](#core-f18) | minor | confirmed | Doctor DOC-05 would flag loadable abi3 and pure wheels as 'cannot load' |
| [CORE-F20](#core-f20) | minor | confirmed | Mockup 08 offers 'Turn off' Developer Mode and shows a reload order Python cannot produce |
| [CORE-F21](#core-f21) | minor | confirmed | Project hash does not resolve subst drives, junctions or symlinks, so a crash journal is not offered when the project is reopened by another path |
| [CORE-F22](#core-f22) | minor | confirmed | Editor exit can hang forever joining a journal writer blocked on a dead network root |
| [CORE-F23](#core-f23) | minor | confirmed | Very long lines: segment x-offsets and scroll extent are unspecified, and each paint copies the whole line |
| [CORE-F24](#core-f24) | minor | confirmed | The output ingest gate is satisfied by construction and never measured during a run |
| [CORE-F25](#core-f25) | minor | confirmed | G1(2) paint gate times only OnPaint; Slate element batching of the same frame on the game thread is excluded |
| [CORE-F26](#core-f26) | minor | confirmed | Section 6 moves regex filters, Ctrl+R history search and Copy as script to 'by M4' while SPEC lands them in M2 |

### CORE-F01

**Unrestored recovered texts are lost after two journal rotations and a second crash** — critical, confirmed

**Where:** TDD 10.7.1 line 6367; TDD 10.2.3 lines 6172-6177; TDD 10.2.4 lines 6185-6188; TDD 10.3 line 6270; TDD 10.10 lines 6430-6437; SPEC PER-03 line 426

**Problem.** While recovery is pending, the recovered text exists only in memory and in the crashed generation; the live buffer shows the disk (or clean) text. Rotation checkpoints write only live buffers, rotation step 5 deletes g-1 unconditionally, and the reader only reads G and falls back to G-1. Nothing persists the pending state. The flowchart rotates once right after reconcile and again after 10 minutes, so ~10 minutes after a crash the crashed generation is deleted (or, if kept, is never read). A second crash then silently drops every recovered text the user had not yet restored, although the banner promise is that ignoring it loses nothing. G6 invariants I1-I6 only check buffers, so the suite cannot catch it.

**Evidence.** TDD 10.7.1 l.6367: "Recovered texts stay in memory and in the old generation, which is not deleted while anything is pending. Ignoring the banner loses nothing." TDD 10.2.3 l.6172: "Rotation: when the file exceeds 16 MiB, 10 minutes after the last checkpoint if anything was committed, and once after recovery is reconciled"; l.6173: "BUF_OPEN and SNAP for every live buffer"; l.6177: "5. Delete wal.<g-1>.sjl; g is the kept previous generation." TDD 10.2.4 l.6186-6188: read generation G, "If G's header is bad or its checkpoint is incomplete, use G-1" (no older generation is ever read). TDD 10.3 l.6270: "O --> P[Rotate to a new generation]". Scenario: crash 1 at 14:03; restart; user keeps working without clicking Restore; rotation at reconcile (g=8, keeps 7) and at 14:15 (g=9, deletes 7); crash 2 at 14:20; restart reads 9 and 8, neither contains crash-1 texts.

**Fix.** TDD 10.2.2: add record 0x15 RECOVERY_PENDING {source_namespace, source_generation, uuid, kind, path, base, eol} followed by a SNAP of the recovered text. TDD 10.7.1: at reconcile, the first commit of the new session writes RECOVERY_PENDING for every counted buffer, and the writer adds each pending text to local history as kind recovered. TDD 10.2.3 step 1: the checkpoint also carries every unresolved RECOVERY_PENDING and its SNAP; RECOVERY_RESOLVED ends it. TDD 10.7 state diagram: add 'Scanning --> Pending: unresolved RECOVERY_PENDING', also after CLEAN. TDD 10.10: invariant I7 'a recovery left pending survives a clean exit, three rotations and a second kill'; test T-PER-044 covering both. SPEC PER-03 acceptance: add 'a recovery left pending is offered again after a clean exit or a second crash'.

**Refuter.** TDD 10.7.1 l.6367 promises the old generation is kept while anything is pending, but nothing persists the pending state: 10.2.2 has no pending record, rotation (10.2.3 l.6173) checkpoints only live buffers (which show disk or clean text while pending), and ReadNamespace (10.2.4 l.6185-6188) reads only G and falls back to G-1. A kept crashed generation is therefore never read again. Worse than reported: a CLEAN exit with recoveries still pending also loses them, because the next scan goes Scanning -> Clean (10.7 state diagram l.6345) and the texts were never put into local history. Also unstated: where the new session writes before the first rotation (flowchart step P comes after the deferred import plus 10 s), so a second crash in that window rebuilds the same uuid from the new session's disk-text SNAP.

### CORE-M01

**instance.lock content is written by atomic replace, which defeats the lock it lives in** — critical, missed

**Where:** TDD 10.2.3 line 6180; TDD 10.5 lines 6312-6316; TDD 10.5 table lines 6318-6322; TDD 10.12 T-PER-023/T-PER-024 lines 6485-6486

**Problem.** 10.2.3 lists instance.lock content among the small JSON files written by temp file plus ReplaceFileW (Windows) or link-to-.bak plus rename (POSIX), and 10.5 rewrites it every 30 s as a heartbeat. 10.5 also makes the same file the lock: a CreateFileW handle with FILE_SHARE_READ held for the process lifetime (Windows), or one fcntl F_WRLCK descriptor (POSIX). POSIX: rename swaps in a new inode, the holder's fcntl lock stays on the old inode (now .bak), and the next instance locks the new file and becomes a second primary on journal/main, so two writers append to, rotate and delete the same generations. Windows: ReplaceFileW must open the replaced file with DELETE access, which the holder's handle (no FILE_SHARE_DELETE) refuses, so the content and heartbeat are never written. A second instance then gets a sharing violation but finds no valid pid/process_start, the 'live only if the PID exists and its creation time equals process_start' rule calls the owner dead, and the table makes it Primary on journal/main as well. Its recovery scan also reports the live first instance's session as a crash.

**Evidence.** TDD 10.2.3 l.6180: "Small JSON files (`instance.lock` content, `projects.json`, `state/*.json`): write `x.json.tmp`, flush, then `ReplaceFileW(x.json, x.json.tmp, x.json.bak)` on Windows, or link the old file to `.bak` and `rename` on POSIX." TDD 10.5 l.6313: "Windows: `CreateFileW(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, OPEN_ALWAYS)`, held for the process lifetime." l.6314: "POSIX: one descriptor with `fcntl(F_SETLK, F_WRLCK)`." l.6316: "Same host: the lock is live only if the PID exists and its creation time ... equals `process_start`. Another host ...: live if the `utc` heartbeat, rewritten every 30 s, is under 2 minutes old." Table l.6322: "Held, owner dead | Primary | `journal/main/`".

**Fix.** TDD 10.2.3: remove instance.lock from the small-JSON list. TDD 10.5: 'The holder writes instance.lock content only through its lock handle (seek to 0, write, SetEndOfFile or ftruncate, flush); it is never renamed or replaced. A reader that finds the lock held but the content empty or unparseable retries for 200 ms and then treats the lock as live; on Windows a sharing violation from the same host is authoritative.' ns.lock follows the same rule. Add T-PER-046 (Windows and Linux): a second instance is Secondary immediately and after 10 heartbeat rewrites; a third instance started after the rewrites is also Secondary.

**Refuter.** added by verifier

### CORE-F02

**A second editor instance shares local history, REPL history and state files with no ownership rule** — major, confirmed

**Where:** TDD 10.5 lines 6312-6326; TDD 10.4 lines 6281-6290; TDD 6.5 lines 3673-3676; TDD 10.1 lines 6081-6082; TDD 10.12 T-PER-023 line 6485; SPEC PER-05 line 428

**Problem.** Only the journal is namespaced per instance. Both instances use the same <root>/history/index.<gen>.shi with the journal's generation protocol, both run retention (mark and sweep, rotate the index), both append to and compact <root>/Repl/history.jsonl, and both rewrite <root>/state/*.json (file-state, restore guard) and the stub cache. Failure scenarios: instance B's hourly sweep deletes blobs that instance A wrote after B loaded the index (A's 'discarded' recovery texts and 'Before run' snapshots point at missing blobs); A rotates the history index and deletes the generation B is still appending to; B's REPL compaction renames history.jsonl while A appends, losing A's entries; B's restore-guard write resets A's crash counter. PER-05 says 'Two instances run without corruption', but T-PER-023 checks only the primary's journal namespace.

**Evidence.** TDD 10.5 l.6321: secondary gets "journal/i-<pid>-<process_start>/"; l.6324: "A secondary never writes, rotates or deletes another namespace." TDD 10.4 l.6282: "Index: <root>/history/index.<gen>.shi, with the journal's framing and generation protocol"; l.6289: "Mark and sweep unreferenced blobs."; l.6306: "void EnforceRetention(); // writer thread". TDD 6.5 l.3673: "File: <root>/Repl/history.jsonl"; l.3675: "it is rewritten to a temporary file and renamed into place". TDD 10.1 l.6082: "File state, restore guard | <root>/state/". T-PER-023 l.6485: "Primary's files byte-identical before and after" (journal only).

**Fix.** TDD 10.5: add an ownership table: only the primary writes <root>/history/, <root>/state/ and Repl/history.jsonl and runs retention. A secondary writes history entries (index records plus blobs) under journal/i-<pid>-<start>/pending-history/ and REPL entries to journal/i-<pid>-<start>/repl-history.jsonl, and keeps its restore guard and file state in its namespace; the next primary start merges them (blobs deduplicated by SHA-256, entries by id) before deleting the namespace. The sweep never deletes a blob younger than 24 h. TDD 6.5: name the secondary's file. Add T-PER-045: two instances save, run and force retention for 10 minutes; afterwards every history entry's blob exists, both REPL histories are complete after the merge, and the primary's restore guard is unchanged by the secondary.

**Refuter.** Verified: only the journal namespace is per-instance (10.5 l.6321-6324). History index and blobs (10.4 l.6281-6290), retention on each instance's writer thread (l.6306), REPL history with rename compaction (6.5 l.3673-3675) and <root>/state (restore guard, file state) have no owner rule, and T-PER-023 checks only the primary's journal files. Instance B's in-memory index lacks A's later ENTRY records, so B's sweep deletes A's blobs and B's index rotation drops A's entries, including 'discarded' recovery texts that U30 promises are never deleted. The proposed fix contradicts itself on REPL history (its own file vs shared FILE_APPEND_DATA); a shared append file still loses entries when the primary renames it during compaction.

### CORE-F03

**Saving files and handling external changes (BUF-03 Save, BUF-04) have no design; the references point to a section that does not contain it** — major, confirmed

**Where:** TDD 4.13 line 1821; TDD 5 line 2053; TDD 9.3 line 5837; TDD 3.7 line 842; TDD 10 (10.1-10.13); TDD 11.6.5 line 7180; TDD 14.7 BUF-03/BUF-04 rows lines 7954-7955; SPEC BUF-03 line 205, BUF-04 line 206

**Problem.** Section 4 says section 10 owns the atomic write, sections 5 and 9 call 'the save path (section 10)', and section 3 assigns file-watcher actions to sections 9 and 10. Section 10 has no save design and no BUF-04 design; the only save text is the checkout callback in 11.6.5. A builder has no rule for: temp-file location (same directory, or rename is not atomic), preserving ACLs, attributes, hard links and symlinked files (a temp-then-rename save replaces a symlink with a plain file, common for studio tool folders), what happens on disk full or a sharing violation mid-save (original bytes must survive, buffer stays dirty and journaled), META and history ordering, self-change suppression in the watcher, Save All partial failure, and for BUF-04 the clean-reload-as-one-edit, the dirty Compare / Keep Mine / Reload bar, the deleted-file Save offer and the stamp comparison. Tests cover only 'each command works' (T-GAP-002) and anchor mapping (T-CORE-030); no test covers the M2 conflict bar or a failed save.

**Evidence.** TDD 4.13 l.1821: "section 10 owns the atomic write and section 11 the checkout." TDD 5 l.2053: "buffer save path (section 4, section 10)". TDD 9.3 l.5837: "Sumi's own saves call OnBufferSaved(Path) directly from the save path (section 10)." TDD 3.7 l.842: "File-watcher actions; re-stat after a run ... | Defer | No | section 9, section 10". Section 10 headings 10.1-10.13 cover storage, journal, session, history, lock, versioning, recovery, safe mode, breadcrumbs, G6, failures, tests. TDD 11.6.5 l.7180 is the only description: "It writes atomically (temporary file, then rename)". Traceability l.7955: "BUF-04 | ... | T-CORE-030, T-PER-017, T-E2E-003".

**Fix.** As proposed: add TDD 10.14 'Saving files' (temp file in the same directory, ReplaceFileW or rename plus directory fsync, hard-link and symlink rules, failure keeps the original bytes and the buffer dirty and journaled with an OS-error bar, then META, the history entry and the watcher stamp; Save All per-file results) and TDD 10.15 'External changes' (watch the directories of open files, stamp = size, mtime, SHA-256; clean = ToMinimalEdits as one undo group plus a reload history entry; dirty = Compare / Keep Mine / Reload bar; deleted = Save bar; deferred while a run is active). Point 4.13, 5, 9.3 and 11.6.5 at them, and add tests: disk full mid-save keeps the original bytes; a symlinked file stays a symlink; dirty-buffer conflict bar; deleted file; Sumi's own save raises no external-change event.

**Refuter.** Verified: 4.13 l.1821, 5 l.2053, 9.3 l.5837 and 3.7 l.842 all point to section 10 for the save path, and sections 10.1-10.13 contain no save or external-change design; the only description is 11.6.5 step 3 (l.7180), which is the M4 checkout callback. BUF-04's clean reload, conflict bar, deleted-file offer and self-save suppression have no design, and no test covers a failed save or the M2 conflict bar.

### CORE-F04

**Literal subscripts and paging on unreal.Array / unreal.Map load soft-referenced assets during completion, Variables and safe watches** — major, confirmed

**Where:** TDD 7.5.2 line 4523; TDD 8.8 line 5544; TDD 8.3.6 lines 5387-5389; SPEC INT-02 line 327; SPEC 7.12 VAR rules line 389; TDD 7.12 T-RES-005 line 5000

**Problem.** The resolver calls __getitem__ on exact unreal.Array and unreal.Map values, Variables pages containers by indexing them, and safe watches allow subscripts plus min/max over unreal containers. In the engine, Array and Map element access pythonizes the inner property, and for FSoftObjectProperty / FSoftClassProperty that calls LoadObjectPropertyValue, which is LoadSynchronous. So 'meshes = data_asset.get_editor_property("soft_meshes")' in a run, then typing 'meshes[0].' or expanding meshes in Variables, loads packages synchronously on the game thread inside a 5 ms call. This breaks INT-02 ('soft references are never read'; '0 package loads in 10,000 completions') and the VAR rule. The hard_object_props guard covers only direct properties, and T-RES-005 has no soft-element container.

**Evidence.** TDD 7.5.2 l.4523: "Allowed when type(v) is exactly list, tuple, dict, unreal.Array or unreal.Map, and the key is an int, negative int or str literal." TDD 8.8 l.5544: "unreal.Array, unreal.Map, unreal.Set | Pages of 100". TDD 8.3.6 l.5387-5388: Subscript on unreal.Array, unreal.Map; Call to min, max. Engine: PyWrapperArray.cpp:426 GetItem -> PyConversion::PythonizeProperty(InSelf->ArrayProp->Inner, ...); PyWrapperMap.cpp:848 PythonizeProperty(GetValueProperty(), ...); PyConversion.cpp:1136-1139 FSoftClassProperty -> LoadObjectPropertyValue; :1142-1145 FObjectPropertyBase -> LoadObjectPropertyValue; Engine/Source/Runtime/CoreUObject/Private/UObject/PropertySoftObjectPtr.cpp:254-257 'return GetPropertyValue(PropertyValueAddress).LoadSynchronous();'.

**Challenges ruling.** D6 as written in SPEC INT-02 and U18 allow literal subscripts on unreal.Array and unreal.Map unconditionally; engine source shows that this loads soft references, so the allowance needs the element-kind condition.

**Fix.** TDD 7.5.2 rule 2: 'For unreal.Array, unreal.Map and unreal.Set, first call _sumi.container_element_kind(v) (C++: ArrayProp->Inner, the map value property, the set element property). Subscript, iterate or page only when that property is not FSoftObjectProperty, FSoftClassProperty or FLazyObjectProperty; otherwise resolve at type level and show the first 100 elements as export text through C++ (PyUtil::GetFriendlyPropertyValue, as FPyWrapperArray::Str does), never by indexing.' Apply the same rule to TDD 8.8 paging and to 8.3.6 subscripts and min/max. Add container_element_kind to TDD 12.1.1. Add T-RES-028: Array, Map and Set of soft references subscripted, paged and watched, with OnEndLoadPackage count 0. SPEC INT-02: add 'element reads of soft-reference containers' to what is never read.

**Refuter.** Engine checked: FPyWrapperArray::GetItem calls PyConversion::PythonizeProperty(ArrayProp->Inner, ...) (PyWrapperArray.cpp:426), and the map getter does the same for the value property (PyWrapperMap.cpp:848). PythonizeProperty goes to PythonizeProperty_Direct (PyConversion.cpp:692-700), where FSoftClassProperty and FObjectPropertyBase (which includes FSoftObjectProperty) call LoadObjectPropertyValue (PyConversion.cpp:1136-1145). FSoftObjectProperty::LoadObjectPropertyValue is GetPropertyValue(...).LoadSynchronous() (PropertySoftObjectPtr.cpp:254-257). So the unconditional subscript and paging allowances in TDD 7.5.2 l.4523, 8.8 l.5544 and 8.3.6 l.5387 load assets, against INT-02 and the VAR rule. The fix needs two more pieces: the new native helper must be added to the 12.1.1 single list (T-API-001 checks it exactly), and the fallback text must be capped, because FPyWrapperArray::Str exports every element (PyWrapperArray.cpp:900-919).

### CORE-F05

**'repr only on built-in types' runs user __repr__ and unbounded work on the game thread** — major, confirmed

**Where:** TDD 7.7 line 4714; TDD 8.3.6 lines 5388-5389; SPEC 7.8 intro line 322; SPEC VAR-01 line 393

**Problem.** Hover and safe watches call repr/str when the value's type is built-in. A built-in container's repr calls repr on every element, so a list or dict holding user objects runs their __repr__ (user code, possibly side-effecting or slow) inside the resolver or SafeEvaluator, and a large list is rendered in full before any cap. That violates the rule that runtime introspection never invokes user code and blows the 5 ms deadline; the 20 ms circuit breaker only trips after the damage.

**Evidence.** TDD 7.7 l.4714: "repr is used only on built-in types (U18)." TDD 8.3.6 l.5389: "The evaluator applies them only to built-in and unreal wrapper types, as U18 already does for repr and str." SPEC l.322: "runtime introspection runs in process within a hard deadline and never invokes user or engine code." Probe on the engine's python.exe 3.11.8 (scratchpad review/repr_probe.py): type(v) is list -> True; repr([Job(), {'k': Job()}]) called the user __repr__ 2 times; repr of a 2,000,000-int list took 111.7 ms and produced 16,888,890 characters before any cap.

**Challenges ruling.** U18 ('repr (on built-in types only)'): the probe shows built-in containers still execute user __repr__, so the ruling needs the bounded saferepr reading.

**Fix.** As proposed: TDD 7.7 and 8.3.6 use a bounded _impl.saferepr that recurses only into exact list, tuple, dict, set and frozenset (depth 2, 20 items per level), applies repr only to exact int, float, bool, None, str and bytes (cut at 200 characters), renders any other element as <TypeName> from type.__dict__['__name__'] and unreal wrappers as class plus path through C++, and stops at 200 characters or the deadline. Add T-RES-029: a list of counting-__repr__ objects and a 2M-element list give counter 0 in under 5 ms.

**Refuter.** Re-ran the probe on the engine's python.exe 3.11.8: type(v) is list is True, and repr([Job(), {'k': Job()}]) called the user __repr__ twice; repr of a 2,000,000-int list took 113.7 ms and produced 16.9M characters. TDD 7.7 l.4714 and 8.3.6 l.5389 gate repr only on the outer type, so hover and auto-refreshed safe watches run user code and blow the 5 ms deadline, against SPEC 7.8 ('never invokes user or engine code'). This refines how U18's 'built-in types only' is implemented rather than overturning it.

### CORE-F06

**REPL completeness uses codeop in 'exec' mode, which submits after the first body line** — major, confirmed

**Where:** TDD 6.5 lines 3646-3652; TDD 6.8 T-REPL-001 line 3827; SPEC REPL-01 line 312

**Problem.** classify_input calls codeop.compile_command(source, '<sumi:REPL>', 'exec') and claims an indented block stays incomplete until a blank line. On the engine's CPython 3.11.8 that is false in 'exec' mode: 'for i in range(3):\n    print(i)' and 'def f():\n pass' are complete. Typing 'for i in range(3):' Enter, 'print(i)' Enter submits immediately, so multi-statement blocks cannot be typed with Enter and REPL-01's 'a blank Enter runs it' fails. 'single' mode keeps blocks incomplete but makes 'x=1\ny=2' invalid, which T-REPL-001 expects to be complete, so neither mode matches the goldens.

**Evidence.** TDD 6.5 l.3648: "it calls codeop.compile_command(source, \"<sumi:REPL>\", \"exec\")"; l.3652: "codeop compiles with PyCF_DONT_IMPLY_DEDENT, so an indented block stays incomplete until a blank line, which REPL-01 needs." T-REPL-001 l.3827: "def f():\n pass incomplete". Probe on Engine/Binaries/ThirdParty/Python3/Win64/python.exe 3.11.8 (scratchpad review/codeop_probe.py): exec: 'def f():\n pass' -> complete; 'for i in range(3):\n    print(i)' -> complete. single: 'def f():\n pass' -> incomplete; 'x=1\ny=2' -> invalid (multiple statements).

**Fix.** TDD 6.5: 'classify_input returns incomplete when the source does not end with a blank line and its last logical line starts with indentation (the first physical line of that logical line is indented, from sumi::PyLexer line facts); otherwise it returns compile_command(source, "<sumi:REPL>", "exec") (None = incomplete; SyntaxError, OverflowError, ValueError = invalid).' Delete the PyCF_DONT_IMPLY_DEDENT sentence. T-REPL-001: add the probe cases 'for i in range(3):\n    print(i)' incomplete, the same plus '\n' complete, and 'x = [\n    1,\n    2]' complete, pinned on the engine interpreter; T-REPL-002 compares against classify_input.

**Refuter.** Re-ran the probe on the engine interpreter: in 'exec' mode 'def f():\n pass' and 'for i in range(3):\n    print(i)' are complete, and 'x=1\ny=2' is complete; in 'single' mode the block cases are incomplete but 'x=1\ny=2' is invalid. With the 6.5 flow (the lexer pre-check fails on the body line, then classify_input), Enter after the first body line submits, so REPL-01's acceptance ('a blank Enter runs it') and T-REPL-001's golden 'def f():\n pass incomplete' both fail. The proposed rule is slightly too broad: 'last physical line is indented' also holds for the closing line of a bracket continuation such as 'x = [\n    1,\n    2]', which would then need a blank Enter for no reason.

### CORE-F08

**Deep reload runs every unload hook up front, then 'restores' old modules whose hooks already tore down their registrations** — major, confirmed

**Where:** TDD 9.1 lines 5758-5761; TDD 9.6 line 5982; TDD 9.7 T-RLD-007 line 6019, T-RLD-012 line 6024

**Problem.** Step 1 calls __sumi_unload__ for every module of the reload set before the purge. Step 4, on an import failure, puts the old module and its unreloaded dependents back into sys.modules marked 'restored previous version'. Those old modules have already unregistered their menus, tickers and Slate callbacks, and nothing re-runs their top-level code, so the tool silently stops working while the report says the previous version was restored. The most common failure is a syntax error in the file just saved under reload-on-save, which the planner already parses with ast but does not use to abort. T-RLD-007 and T-RLD-012 never combine a failure with unload hooks.

**Evidence.** TDD 9.1 l.5758: "Unload hooks. For each module of S in reverse dependency order, mod.__dict__.get(\"__sumi_unload__\") is called"; l.5761: "If module M raises ... The old M, and every module of S that depends on M and has not been re-imported, are put back into sys.modules. They are marked \"restored previous version\"." l.5729: "Edges are found with ast". 9.6 l.5982: "A re-import raises (for example a SyntaxError in the saved file) | ... | Restore the old module".

**Fix.** As proposed: TDD 9.1 adds step 0 'compile() every changed source in the plan; on SyntaxError run nothing (no hooks, no purge) and end the reload with the error and a link (RUN-16)'. Change step 4 and the 9.6 row to 'restored previous version: its __sumi_unload__ already ran, so registrations made at import are gone; fix the error and reload', with [Re-run module top level]. Add T-RLD-022: menus corpus with hooks plus a syntax error in mid leaves the menu present and calls no hook; a runtime import error shows the 'registrations gone' note.

**Refuter.** Verified in TDD 9.1 l.5758-5761: unload hooks run for all of S before the purge, and on an import failure the old M and its unreloaded dependents are put back as 'restored previous version'. Their __sumi_unload__ already removed their menus, tickers and Slate callbacks (D18 says that is what hooks are for), and nothing re-runs their top level, so after the most common failure (a SyntaxError on reload-on-save, J3) the tool silently stops working while the report claims a restore. T-RLD-007 and T-RLD-012 never combine a failure with hooks.

### CORE-F09

**One journal writer thread serialises commits behind history compression, retention sweeps and full checkpoints, so the 2 s bound does not hold** — major, confirmed

**Where:** TDD 10.2.1 line 6119; TDD 10.2.5 line 6200; TDD 10.4 lines 6281, 6284, 6286-6290, 6306; TDD 10.2.3 line 6173; TDD 10.10 line 6424; SPEC PER-01 line 424

**Problem.** The worst-case arithmetic (1.0 s + 0.25 s + 0.5 s commit) assumes the writer is free. The same single thread also zlib-compresses a history blob for every save and every run snapshot, runs retention mark-and-sweep (deleting up to 500 MB of blob files, hourly), and writes rotation checkpoints that SNAP every live buffer. A 40 MB buffer's blob takes about a second to compress; a sweep of thousands of blobs with antivirus takes seconds; a checkpoint of several 10-40 MB buffers takes seconds. Commits queue behind that work, so a kill in that window loses more than 2 s of typing. 'History I/O runs after the commit that caused it' protects only that one commit. Driver A uses 1 MiB rotation and at most 1 MB pastes, so G6 never exercises it.

**Evidence.** TDD 10.2.1 l.6119: "Frame building, append, flush, rotation, history updates | FSumiJournal::FWriter | SumiJournalWriter (one background thread)". 10.4 l.6281: blobs "compressed with FCompression::CompressMemory(NAME_Zlib, ...)"; l.6284: "All history I/O runs on the writer thread after the commit that caused it, so it never delays durability."; l.6306: "void EnforceRetention(); // writer thread". 10.2.5 l.6200: "Worst case: 1.0 s latency + 0.25 s ticker granularity + a commit of <= 0.5 s at p99 (proposed) = 1.75 s <= 2 s." 10.10 l.6424: "pastes up to 1 MB; ... rotation lowered to 1 MiB".

**Fix.** As proposed, or the equivalent single-thread rule: TDD 10.2.1 'commit requests always run first; history blobs, retention, PostIo and checkpoint writing run in slices of at most 50 ms of work, and the writer drains due commits between slices (or on a second thread)'. TDD 10.2.3: the checkpoint is written in slices while commits keep appending to g; before the rename, the delta committed since the checkpoint began is appended to g+1. TDD 10.10: add driver A traces with 3 x 40 MB buffers, the real 16 MiB rotation and a forced retention sweep of 5,000 blobs, with I2 unchanged.

**Refuter.** Verified: TDD 10.2.1 l.6119 puts frame building, flush, rotation and history updates on one SumiJournalWriter thread, and PostIo is only a 'low-priority lane of the writer thread' (l.6231). 10.4 l.6284 protects only the commit that caused the history work. Rotation step 1 (l.6173) SNAPs every live buffer before step 4 resumes appending, so commits wait for the whole checkpoint. The 10.2.5 worst case (l.6200) assumes a free writer. With the design's own fixture (T-PER-020: 20 dirty tabs of 100k lines) a checkpoint is tens of MB, and driver A (l.6424: 1 MiB rotation, pastes up to 1 MB) cannot see the problem. The exact timings in the finding are estimates, but the serialization and the blind gate come straight from the text.

### CORE-F10

**G3 must pass at M1 exit, but the Stage 2 transformer it measures is scheduled for M2** — major, confirmed

**Where:** TDD 15 M1 row line 8221; TDD 7.4.1 line 4352; TDD 7.10.1 lines 4845-4860; SPEC INT-15 line 340; 08 RT-10 line 1581

**Problem.** G3 checks that every generated .pyi parses and type-checks under Pyrefly and basedpyright, coverage >= 99.5%, type agreement >= 95%, and server latency on the stub package. Those need stubgen (7.4.5) and the pinned servers wired to the package. The M1 scope lists 7.4.2 (Stage 1) only and 'no third-party executables', while also saying G3 runs during M1 and must pass before M2 starts. Following the plan, M1 ends with nothing for G3 to measure, and M2 cannot start.

**Evidence.** TDD 15 l.8221 (M1): "... section 7.1-7.3 (sumi-check only), section 7.4.2, section 7.5 ... **G3 runs during M1** and must pass before M2 starts (RT-10)". TDD 7.4.1 l.4352: "Stage 2 (M2). sumi_tools.stubgen turns the snapshot into a split .pyi package". TDD 7.10.1 l.4857: "Every .pyi parses with ast, and type-checks with zero errors under both Pyrefly and basedpyright". SPEC INT-15 Lands: "M1 (Stage 1); M2 (gate G3)".

**Fix.** As proposed: TDD 15 M1 row adds '§7.4.5 stubgen and the §7.10.1 G3 harness, with CI-only pinned Pyrefly and basedpyright (internal, not shipped)'; TDD 7.4.1 Stage 2 becomes 'built in M1 for G3 (internal); shipped in M2'; SPEC INT-15 Lands becomes 'M1 (Stage 1 shipped; Stage 2 built for G3, internal); M2 (Stage 2 shipped)'.

**Refuter.** Verified: RT-10 (08 l.1581) says 'G3 runs during M1 alongside the walker and must pass before M2 starts', and RT-13 keeps only the shipped .pyi package in M2. Yet TDD 15 M1 (l.8221) lists only 7.4.2 and 'no third-party executables', 7.4.1 (l.4352) puts Stage 2 in M2, and 7.10.1's G3 checks (l.4857: every .pyi type-checks under Pyrefly and basedpyright) need stubgen (7.4.5) and the pinned servers. SPEC INT-15 says 'M2 (gate G3)'. A plan that follows the M1 row has nothing for G3 to measure.

### CORE-F11

**Section 12 contracts disagree with sections 6, 7 and 9 on native signatures, units, helper allow-list, version key and run-ID width** — major, confirmed

**Where:** TDD 6.1 lines 3297-3299; TDD 12.1.1 lines 7482-7483; TDD 6.8 T-OUT-013 line 3799; TDD 7.4.2 line 4386; TDD 7.5.6 line 4626; TDD 12.1.3 line 7515; TDD 9.5 line 5971; TDD 12.4 line 7617; TDD 10.6 line 6333; TDD 12.1.2 line 7511; TDD 6.2 lines 3346, 3367, 3434

**Problem.** (1) _sumi.write and Writer take a str stream in 6.1 ('others raise ValueError') but an int STREAM_* constant in 12.1.1, which also adds debugger, logpoint, evaluate and runner-notice streams that have no LineType, rendering or echo rule in 6.2-6.4; T-OUT-013 (write('header') raises ValueError) and T-API-001 (exactly the 12.1.1 surface) cannot both pass. (2) stubsnap.step takes budget_ns returning (done, stats) in 7.4/7.5 but budget_ms returning bool in 12.1.3, a 10^6 unit error between caller and callee. (3) 9.5 defines _impl.env.collect(); 12.1.3 allows only _impl.env.query(kind). (4) 12.1.3 says C++ may call 'only these' helpers, which omits runner.classify_input (6.5 REPL Enter), reload.loaded_modules_for and describe_holders (9.1, 9.3), resolver.session_names, hover, signature, definition (7.5, 7.8). (5) 12.4 says JSON files carry "schema"; 10.6, projects.json, restore-guard and REPL history use "version". (6) Run IDs are uint64 (generation << 32 | number) in 12.1.2 but uint32 in FSumiRawRecord, the FRunSpan map and BeginRunBlock, so stale IDs after a Python restart cannot be rejected in the output store.

**Evidence.** 6.1 l.3298: "write(stream: str, run_id: int, text: str) stream in {\"stdout\",...}; others raise ValueError". 12.1.1 l.7482: "write(stream: int, run_id: int, text: str) ... 1 stdout, 2 stderr, 3 result, 4 runner notice, 5 info, 6 warning, 7 error, 8 agent, 9 debugger, 10 logpoint, 11 evaluate". 7.4.2 l.4386: "stubsnap.step(budget_ns=4_000_000)"; 12.1.3 l.7515: "_impl.stubsnap.step(budget_ms) -> bool" and "Only these ... _impl.env.query(kind) -> dict". 9.5 l.5971: "def collect() -> dict". 12.4 l.7617: "JSON files carry an integer \"schema\""; 10.6 l.6333: "JSON state files | \"version\": <int>". 12.1.2 l.7511: "FSumiRunId is a uint64"; 6.2 l.3346: "uint32 RunId".

**Fix.** As proposed, with T-OUT-013 reworded: 'write(12, ...) (no such constant) raises ValueError; write("stdout", ...) raises TypeError'. Align 6.1 to int STREAM_* constants; add LineType values and 6.3/6.4 render and echo rows for runner notice, debugger, logpoint and evaluate; 12.1.3 to step(budget_ns) -> tuple[bool, dict] and env.collect(); add classify_input, loaded_modules_for, describe_holders, resolver.hover, signature, definition, session_names and property_literal (l.7158) to the 12.1.3 list; pick one JSON version key in 12.4 and 10.6; make every RunId in 6.2 an FSumiRunId (uint64).

**Refuter.** Each item checked: 6.1 l.3297-3299 take a str stream ('others raise ValueError') while 12.1.1 l.7482, which calls itself 'the single list of _sumi functions', takes an int STREAM_* constant with four streams (runner notice, debugger, logpoint, evaluate) that 6.2's LineType (l.3414) lacks; stubsnap.step(budget_ns) -> tuple (l.4386, l.4626) vs step(budget_ms) -> bool (12.1.3 l.7515); env.collect() (l.5971) vs env.query(kind); 12.1.3's 'Only these' omits classify_input (l.3647), loaded_modules_for and describe_holders (l.5800-5801), and resolver hover, signature, definition and session_names (l.4620-4623); 12.4 l.7617 'schema' vs 10.6 l.6333 'version' (both keys are in use); FSumiRawRecord, BeginRunBlock and GetRunLogPath use uint32 RunId (l.3346, l.3434-3437) vs uint64 FSumiRunId (12.1.2 l.7511). T-OUT-013 and T-API-001 cannot both pass. One problem in the proposed fix: 'write(STREAM_HEADER)' names a constant that does not exist, and with int streams a str argument is a TypeError under 12.1's error rules.

### CORE-F19

**Pending recovery behaves differently in SPEC, TDD and mockup 06 (read-only state and the orphan count)** — major, modified

**Where:** SPEC PER-03 line 426; TDD 10.7.1 lines 6360-6367; TDD T-PER-027 line 6489; Mockup images/06-crash-recovery.png panel A

**Problem.** SPEC: orphaned sessions are 'listed apart' and recovered tabs 'stay read-only under Not restored yet'. TDD: n counts every non-empty orphan buffer (T-PER-027 expects n = 5 including the orphan) and pending buffers are editable with a 'Recovered changes available' bar. Mockup 06 shows 'Recovered 3 buffers' while listing a fourth buffer from another instance unchecked. A builder cannot satisfy all three; editable pending buffers also let a user type over text whose recovery is still pending.

**Evidence.** SPEC PER-03: "n counts buffers whose journal holds changes not on disk; orphaned sessions are listed apart. Recovered tabs stay read-only under \"Not restored yet\" until the user chooses." TDD 10.7.1 l.6363: "every non-empty buffer of an orphaned or imported namespace"; l.6367: "a counted file buffer shows its disk text ... with the bar". T-PER-027: "... + 1 orphan gives n = 5". Mockup 06 banner: 'Recovered 3 buffers'; Review lists 'ANOTHER EDITOR INSTANCE ... Untitled-4' unchecked.

**Fix (as modified by the refuter).** TDD 10.7.1: 'n excludes orphaned, imported and compat namespaces, which Review lists in their own section; a pending buffer is read-only under "Not restored yet" with the bar until Restore, Restore as new tab or Discard.' T-PER-027: '3 dirty files + 1 changed scratch + 2 unchanged scratch + 1 orphan gives n = 4 and one orphan row in Review'. Add to T-PER-029: typing into a pending buffer is refused.

**Refuter.** Verified: SPEC PER-03 (l.426, the 06-3 rule) lists orphaned sessions apart and keeps recovered tabs read-only under 'Not restored yet'. Mockup 06 matches the SPEC: the banner says 'Recovered 3 buffers', and callout 5 lists 'another editor instance's buffers ... separately and unticked'. TDD 10.7.1 counts every orphan buffer in n (l.6363), shows editable disk text with a bar (l.6367), and T-PER-027 encodes n = 5 with the orphan. The TDD contradicts both the SPEC and the mockup, and its gating test asserts the wrong value. That is a contradiction that matters for a builder, so it is major, not minor.

### CORE-F07

**Line splitter discards output whose CR and LF arrive in separate writes, which is how print() writes CRLF text** — minor, modified

**Where:** TDD 6.1 lines 3252-3256; TDD 6.8 T-OUT-002 line 3788

**Problem.** Splitting runs per raw record. '\r\n' ends a line but 'a lone \r discards the open partial text'. print(x) issues write(x) then write('\n') as two calls, so a string ending in CR (any line from Windows tools split on '\n': p4, git, subprocess output, CSV read with newline='') arrives as 'text\r' followed by '\n'. Processed record by record, the CR looks lone and the text is discarded, leaving an empty line in the view, the echo and the run log. Stock UE prints these lines. The authoritative run record loses real output in a common pipeline pattern, and the 500-case corpus is not required to split CR and LF across records.

**Evidence.** TDD 6.1 l.3253: "\n and \r\n end a line."; l.3255: "A lone \r discards the open partial text". Probe on the engine interpreter (scratchpad review/print_probe.py): print(line, file=w) for a p4 line split from CRLF output recorded writes ['//depot/main/a.py#3 - edit default change\r', '\n'].

**Fix (as modified by the refuter).** TDD 6.1 line splitting: add 'A CR at the end of a record is held in that key's partial state; if the next record for the key starts with LF the pair ends the line; anything else makes it a carriage return (replace); at run end or at 16 KB a held CR ends the line.' T-OUT-002: add print('x\r'), CRLF split across two writes, and a CR at the end of a run's last write.

**Refuter.** The cited text does not say splitting runs per raw record: TDD 6.1 l.3251 keeps 'one partial-line buffer per (native thread ID, stream, run ID)' across records, and the natural streaming reading already treats a CR followed by an LF in the next record as CRLF. The probe confirms print() writes 'text\r' and '\n' as two writes, so the ambiguity matters, and T-OUT-002's corpus does not require the split case. This is a contained spec gap, not a major design defect.

### CORE-F12

**'Plain mode' means two things, and the 5 MB language-server limit contradicts itself** — minor, confirmed

**Where:** TDD 7.1.5 line 4012; TDD 4.13 line 1828; TDD 4.15 line 1898; TDD 7.3.3 line 4213; TDD 7.12 T-LSP-045 line 4967; TDD 7.12 T-DIAG-027 line 5073; TDD 7.1.4 lines 3996-3998

**Problem.** Section 4 defines Plain mode as the stock widget for files under 1 MB (the IME fallback); 7.1.5 says 'plain-mode buffers (files over 5 MB, BUF-09) are never opened in any server', so a literal build cuts every IME-fallback user off from diagnostics and completion. BUF-09 has no 5 MB rule. T-LSP-045 says a 6 MB file is never sent to any server, yet sumi-check (a server) must emit SUM902 for it (T-DIAG-027). A buffer that grows past 5 MB by paste is never closed in the servers; a paste past the 32 MB outbound cap becomes a protocol error, and the restart re-sends didOpen with the same text, looping into Paused.

**Evidence.** 7.1.5 l.4012: "for plain-mode buffers (files over 5 MB, BUF-09). Those buffers are never opened in any server." 4.13 l.1828: "< 1 MB | Normal; Plain mode is offered". 7.3.3 l.4213: "Files over 5 MB get SUM902 and nothing else." T-LSP-045: "Open a 6 MB file | Never sent to any server". 7.1.4 l.3998: "pending outbound bytes <= 32 MB"; 7.3.5 l.4296: "every open document is re-sent with didOpen".

**Fix.** As proposed: TDD 7.1.5 'for buffers over 5 MB (proposed): the type server and ruff never get them; sumi-check gets only a sumi/skipped notification and reports SUM902; a buffer that crosses 5 MB is didClose'd, and one that drops below is reopened.' Plain-mode buffers stay in every server. T-LSP-045: 'never sent to the type server or ruff'. SPEC BUF-09: add the 5 MB language-service rule.

**Refuter.** Verified: 7.1.5 l.4012 defines 'plain-mode buffers (files over 5 MB, BUF-09)', but Plain mode is the stock-widget IME fallback for files under 1 MB (4.13 table, 4.15 l.1898, SPEC EDT-15 and BUF-09), and BUF-09 has no 5 MB rule. T-LSP-045 (never sent to any server) contradicts T-DIAG-027 and 7.3.3 l.4213 (sumi-check reports SUM902, so it must receive the file). The behaviour of a buffer that grows past 5 MB is unspecified.

### CORE-F13

**Language-server memory cap is measured as private commit, not RSS, at a different rate than the gate** — minor, confirmed

**Where:** TDD 7.3.5 line 4295; TDD 13.2 B-10 line 7726; SPEC PERF-11 line 561

**Problem.** The supervisor restarts a server whose 'RSS' stays over 1.5 GB for 60 s, sampled every 5 s with GetApplicationMemoryUsage; B-10 samples RSS at 1 Hz. On Windows that API returns PrivateUsage (commit charge), not the working set, so the restart rule and the G3 gate measure different quantities; a server under 1.5 GB RSS but over in commit (common for Node and large Rust heaps) is restarted repeatedly and reaches Paused.

**Evidence.** TDD 7.3.5 l.4295: "RSS is sampled every 5 s with FPlatformProcess::GetApplicationMemoryUsage (GenericPlatformProcess.h:708)." B-10: "Child RSS sampled at 1 Hz". Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp:862-881: GetProcessMemoryInfo(... PROCESS_MEMORY_COUNTERS_EX ...); '*OutMemoryUsage = MemoryInfo.PrivateUsage;'.

**Fix.** As proposed: TDD 7.3.5 and B-10 define RSS as WorkingSetSize from GetProcessMemoryInfo (Windows) or VmRSS (Linux), read by FSumiChildJob and sampled at 1 Hz in both the supervisor and the gate; private commit is logged, not enforced.

**Refuter.** Engine checked: FWindowsPlatformProcess::GetApplicationMemoryUsage returns PROCESS_MEMORY_COUNTERS_EX.PrivateUsage (WindowsPlatformProcess.cpp:862-881), and the header calls it 'virtual memory usage' (GenericPlatformProcess.h:707-708), not RSS. So TDD 7.3.5 l.4295 enforces private commit every 5 s while B-10 (l.7726) gates RSS at 1 Hz.

### CORE-F14

**Memory targets cannot be met by the described layout: T-ENC-007 '<= 3x text' and PERF-11 core '<= 50 MB + 3x'** — minor, confirmed

**Where:** TDD 4.13 T-ENC-007 line 1842; TDD 4.1.2 line 976; TDD 4.3.3 lines 1166-1171; TDD 6.2 lines 3392-3397; SPEC PERF-11 line 561; TDD 14.7 PERF-11 row line 8155

**Problem.** A 10 MB ASCII file is 20 MB as UTF-16 (2x). Measured on the engine's stdlib, Python averages about 0.1 tokens per byte and 36-38 bytes per line, so 10 MB is about 1M tokens (4 MB at 4 bytes each) and about 280k lines; each line record carries a 128-bit hash, end state, facts and token reference (at least ~28 bytes), about 8 MB. That is about 3.2-3.9x before folds, so T-ENC-007, listed under the PERF-11 Gate row, fails by construction. PERF-11's tracked 'Core <= 50 MB + 3x text with 20 tabs and a full ring' is exceeded by the ring defaults alone (64 MB text plus 8 MB index).

**Evidence.** T-ENC-007 l.1842: "Memory for a 10 MB ASCII file: model, lexer, folds | <= 3 x text size | PERF-11". 4.1.2 l.976: "a 10 MB file takes 20 MB". 4.3.3 l.1169: "a 128-bit XXH3 line hash". 6.2 l.3392-3393: "Ring text | 64 MB or 200k lines"; "Line index | 200k x <= 40 B = 8 MB". Token density probe on Engine/Binaries/ThirdParty/Python3/Win64/Lib/asyncio: 0.111 tokens/byte, 35.9 bytes/line; json: 0.095, 37.7.

**Fix.** As proposed: TDD T-ENC-007 '<= 4.5 x text size' (or drop the per-line 128-bit hash and key the shape cache by line text); SPEC PERF-11 core target '<= 50 MB + 4.5x text + the output ring (72 MB at defaults)'.

**Refuter.** UTF-16 alone is 2x (4.1.2 l.976). The per-line record (4.3.3 l.1166-1171) holds a 16-byte XXH3 hash, an end-state id, facts and a token container, plus 4-byte packed tokens, so about 280k lines and about 1M tokens for a 10 MB Python-like file add roughly 1.3-1.9x on their own. T-ENC-007's 3x is out of reach by construction. PERF-11's 'Core <= 50 MB + 3x text ... with a full ring' is exceeded by the ring defaults alone (6.2: 64 MB text plus about 8 MB index).

### CORE-F15

**PERF-18 (pre-run ack <= 16 ms p95) is below the TDD's own flush estimate and its per-frame polling** — minor, confirmed

**Where:** SPEC PERF-18 line 568; TDD 10.2.5 line 6206; TDD 13.2 B-16 line 7732; TDD T-GAP-011 line 7926

**Problem.** The ack needs one FlushFileBuffers, which 10.2.5 itself puts at 5-30 ms typical on SSD, and Starting observes durability only from a per-frame poll, adding up to a frame. A p95 of 16 ms is therefore unlikely on a typical SATA or BitLocker SSD, and the stated reason (Starting within one frame at 60 fps) cannot hold with per-frame polling.

**Evidence.** SPEC PERF-18: "<= 16 ms p95 on a local SSD | Proposed | Keeps Starting within one frame at 60 fps". TDD 10.2.5 l.6206: "FSumiRunService polls IsDurable(Ticket), an atomic load, from its own per-frame Starting handler ... Typical latency is one flush, 5-30 ms on SSD."

**Fix.** As proposed: SPEC PERF-18 and TDD B-16/T-GAP-011 '<= 50 ms p95 on a local SSD, SnapshotForRun to the run's OnEndFrame start (proposed)', and drop the one-frame reason.

**Refuter.** TDD 10.2.5 l.6206 itself estimates one flush at '5-30 ms on SSD', and Starting sees durability only from a per-frame poll, so the PERF-18/B-16 p95 of <= 16 ms and its 'within one frame at 60 fps' reason cannot hold together. The target is only Proposed, so the impact is contained.

### CORE-F16

**FSumiOutputDevice is specified lock-free but attributes through an RW-locked table, and it re-captures the console executor's own echo** — minor, confirmed

**Where:** TDD 6.1 line 3259; TDD 6.3 lines 3476-3479; TDD 6.6 Exec line 3723

**Problem.** The device runs synchronously on whatever thread logs, including threads inside Sumi code; it is said to take no lock, yet step 3 attributes through FSumiThreadTable, which is guarded by a reader-writer lock. A UE_LOG raised on a thread that holds the write lock (for example while registering a new thread's name, which runs Python) re-enters the device and self-deadlocks on the non-recursive lock. Separately, Exec echoes console input to LogPython on the game thread without GSumiForwarding, so the device captures it as an 'Outside runs' line and the input shows twice.

**Evidence.** 6.1 l.3259: "FSumiThreadTable ... guarded by a reader-writer lock." 6.3 l.3476: "attribution from FSumiThreadTable (section 6.1)"; l.3478: "It never logs, takes no lock other than the lock-free push". 6.6 l.3723: "(3) Echo the input to LogPython at Log, as the stock executors do." Unbuffered devices are called synchronously on the logging thread: Engine/Source/Runtime/Core/Private/Misc/OutputDeviceRedirector.cpp:440-442, 905.

**Fix.** As proposed: TDD 6.3 'the device pushes only the native thread ID; attribution happens on the SumiOutput thread'; TDD 6.6 step 3 echoes through FSumiLogForwarder with GSumiForwarding set; T-OUT-021 adds 'console input appears once' and 'a log emitted while the thread table is written does not deadlock'.

**Refuter.** Verified the contradiction: 6.3 l.3476-3478 attributes 'from FSumiThreadTable' and 'takes no lock other than the lock-free push', while 6.1 l.3259 guards that table with a reader-writer lock. The device runs synchronously on the logging thread (OutputDeviceRedirector.cpp:440-442), so a log emitted while the write lock is held can deadlock. 6.6 Exec step 3 (l.3723) echoes to LogPython while idle without GSumiForwarding, and step 1 of the device filter only skips forwarder lines, so the echo is captured as an 'Outside runs' line.

### CORE-F17

**Deep reload can re-import stale bytecode after a same-second, same-size save** — minor, confirmed

**Where:** TDD 9.1 line 5760; TDD 9.3 lines 5837-5839

**Problem.** Re-import uses importlib.import_module after a purge. CPython validates a timestamp .pyc by int(source mtime) and source size, so a second save within the same wall-clock second that keeps the byte size (changing a constant) loads the previous version's bytecode, and reload-on-save silently runs old code. The TDD never mentions bytecode caches or importlib.invalidate_caches().

**Evidence.** TDD 9.1 l.5760: "Re-import each name with importlib.import_module, dependencies first." Engine Lib/importlib/_bootstrap_external.py:1030: 'source_mtime = int(st['mtime'])', validated by _validate_timestamp_pyc (:675) against mtime and size. PythonScriptPlugin sets no dont_write_bytecode (grep of Source and Content finds none).

**Fix.** As proposed: TDD 9.1 step 3 'before re-import, call importlib.invalidate_caches() and remove importlib.util.cache_from_source(path) for every changed file in S'; add T-RLD-023: two equal-size saves 0.3 s apart both reload the new value.

**Refuter.** Engine stdlib checked: SourceFileLoader uses source_mtime = int(st['mtime']) (_bootstrap_external.py:1030) and _validate_timestamp_pyc (:675) checks only mtime and size; PythonScriptPlugin sets no dont_write_bytecode (grep of Source and Content finds none). A same-second, same-size save re-imports the previous bytecode after the purge. Needs two saves within a second, so it is minor.

### CORE-F18

**Doctor DOC-05 would flag loadable abi3 and pure wheels as 'cannot load'** — minor, confirmed

**Where:** TDD 9.5 DOC-05 line 5914; SPEC ENV-01 line 457

**Problem.** The rule 'ABI tag is not cp311 for this platform -> Error (cannot load)' marks cp3x-abi3 wheels (for example cryptography cp39-abi3-win_amd64) and py3-none-any wheels as errors, although they load on 3.11; untagged .pyd files (Windows abi3 naming) cannot be judged from the filename at all.

**Evidence.** TDD DOC-05 l.5914: "Compiled extensions or wheels whose ABI tag is not cp311 for this platform | Error (\"cannot load\")". Engine importlib.machinery.EXTENSION_SUFFIXES on Win64 includes '.pyd' without a tag.

**Fix.** As proposed: TDD DOC-05 'compatible: cp311-cp311-<plat>, cp3x-abi3-<plat> with x <= 11, py3-none-any, py3-none-<plat>, py2.py3-none-any; others are Error; an untagged .pyd is judged by its PE import (python311.dll = cp311, python3.dll = abi3)'. Extend T-ENV-005 with an abi3 wheel and a pure wheel that pass.

**Refuter.** Engine interpreter checked: EXTENSION_SUFFIXES is ['.cp311-win_amd64.pyd', '.pyd'], so stable-ABI and other untagged .pyd files cannot be judged by name, and cpXY-abi3 and py3-none wheels load on 3.11 while DOC-05 (l.5914) calls them errors. U36 has the same wording, but its intent ('cannot load') is met by a compatibility test, so this refines the ruling rather than overturning it.

### CORE-F20

**Mockup 08 offers 'Turn off' Developer Mode and shows a reload order Python cannot produce** — minor, confirmed

**Where:** Mockup images/08-reload-and-environment.png doctor row and reload table; SPEC INT-01 line 326; SPEC CFG-01 line 500; TDD 9.5 DOC-07 line 5916; TDD 9.1 line 5732

**Problem.** The doctor row 'Developer Mode is on' has a [Turn off] button, but INT-01 says Developer Mode is never toggled, CFG-01 shows UE Python settings read-only with deep links, and DOC-07 offers 'Open the setting'. The reload table lists the package studio_tools last ('package __init__'), but 9.1 makes a submodule depend on its parent and orders dependencies first, and after a purge importing studio_tools.naming re-executes the parent first, so the parent always reloads before its children.

**Evidence.** Mockup 08 doctor: 'Developer Mode is on ... Sumi does not need it [Turn off]'; reload table rows 1 studio_tools.naming ... 4 studio_tools 'package __init__'; callout 3 'the saved leaf first, then its importers'. SPEC INT-01: "Developer Mode is never required or toggled (D5)." TDD DOC-07 fix: "Open the setting". TDD 9.1 l.5732: "A is a submodule of B (parent package)."

**Fix.** As proposed: mockup 08 replaces [Turn off] with [Open setting], reorders the reload table to studio_tools (parent package), studio_tools.naming (saved), importer (imports naming), ui, and updates callout 3 to 'parent packages first, then the saved module, then its importers'.

**Refuter.** Mockup 08 shows '[Turn off]' on the Developer Mode doctor row, against SPEC INT-01 ('never required or toggled', D5) and DOC-07 ('Open the setting'). Its reload table puts the package studio_tools last, but 9.1 l.5732 makes a submodule depend on its parent (dependencies first), and importing studio_tools.naming after the purge executes the parent first.

### CORE-F21

**Project hash does not resolve subst drives, junctions or symlinks, so a crash journal is not offered when the project is reopened by another path** — minor, confirmed

**Where:** TDD 10.1 lines 6098, 6101; SPEC PER-08 line 431

**Problem.** The hash is SHA-256 over the textually normalised path. Studios often open the same workspace as S:/Proj (subst or mapped drive) and D:/work/Proj. A crash under one path and a restart under the other gives a different hash; the moved-project offer requires the old path to no longer exist, which it still does, so recovery shows nothing and the user believes the work is lost.

**Evidence.** TDD 10.1 l.6098: "ConvertRelativePathToFull, NormalizeFilename, CollapseRelativeDirectories, no trailing slash, lower-cased on Windows only"; l.6101: "projects.json lists a project with the same name whose path no longer exists".

**Fix.** As proposed: TDD 10.1 hashes the final path (GetFinalPathNameByHandleW on the .uproject handle; realpath on POSIX). At start, Review also lists any projects.json entry with the same .uproject file identity (volume serial and file ID, or st_dev and st_ino) whose main namespace lacks CLEAN.

**Refuter.** TDD 10.1 l.6098 normalises the path textually only, and the moved-project offer (l.6101) needs the old path to be gone. A subst or junction path therefore gets a separate root, and its crash journal is not offered until the project is reopened by the original path. The same split also lets two instances that open one project by two paths both become primary in different roots.

### CORE-F22

**Editor exit can hang forever joining a journal writer blocked on a dead network root** — minor, confirmed

**Where:** TDD 3.8 line 872; TDD 10.2.5 line 6202; TDD 10.11 lines 6447, 6453

**Problem.** Network roots are supported with a banner. If the share hangs, the writer blocks inside FlushFileBuffers; OnPreExit waits 2 s for CLEAN, but ShutdownModule then 'flushes and joins the journal writer' with no bound and no rule for a thread that never returns, so the editor hangs on exit (or, if detached, the thread runs after the module unloads).

**Evidence.** TDD 3.8 l.872: "flush and join the journal writer; join the watchdog. Total <= 2 s (proposed)". 10.2.5 l.6202: "waits at most 2 s for it ... ShutdownModule joins the writer." 10.11 l.6453: "Hung disk | Flush > 2 s | ... shutdown waits <= 2 s".

**Fix.** As proposed: TDD 10.2.5 'if the writer has not acknowledged CLEAN within 2 s, mark it abandoned, release the locks, skip the join and keep the writer's code and state alive until process exit'; TDD 3.8 says the same; T-PER-041 adds a FileOps that blocks forever.

**Refuter.** TDD 3.8 l.872 ('flush and join the journal writer ... Total <= 2 s') and 10.2.5 l.6202 ('ShutdownModule joins the writer') bound the CLEAN wait but not the join, so a writer stuck in FlushFileBuffers on a dead share holds exit. T-PER-041 ('a hung disk delays exit <= 2 s') cannot pass as designed.

### CORE-F23

**Very long lines: segment x-offsets and scroll extent are unspecified, and each paint copies the whole line** — minor, confirmed

**Where:** TDD 4.8.1 line 1441; TDD 4.8.4 T-VIEW-009 line 1512; TDD 4.1.3 line 1015

**Problem.** Lines over 10,000 units are shaped in 1,024-unit segments near the viewport, but measured advances (CJK fallback, tabs) mean the x of a far segment, the caret x for End and the horizontal scroll extent need the widths of every earlier segment; the TDD does not say how they are obtained, and T-VIEW-009 asserts only nearby segments are shaped. LineView is zero-copy only for a single piece, so after one edit a 1 MB line is copied (2 MB) on every frame's paint.

**Evidence.** 4.8.1 l.1441: "Long lines over 10,000 units (proposed) are shaped in 1,024-unit segments near the viewport." T-VIEW-009: "A 1 MB line scrolled horizontally | Within G1(2); only nearby segments shaped". 4.1.3 l.1015: "LineView(LineNo, std::u16string& Scratch) const; // zero-copy if one piece".

**Fix.** As proposed: TDD 4.8.1 keeps a per-long-line segment-width table (unmeasured segments estimated as units x the space advance, refined by a worker, scroll extent corrected) and reads only viewport segments through ForEachSpan; T-VIEW-009 adds 'End on a 1 MB CJK line puts the caret at the measured x within 2 frames'.

**Refuter.** TDD 4.8.1 l.1441 shapes long lines in segments near the viewport, but caret x uses measured advances (l.1436-1438), so the x of a far segment and the horizontal extent need the widths of earlier segments, which are never specified. LineView is zero-copy only for one piece (4.1.3 l.1015).

### CORE-F24

**The output ingest gate is satisfied by construction and never measured during a run** — minor, confirmed

**Where:** TDD 6.2 line 3321; TDD 6.8 T-OUT-015 line 3801; TDD 13.2 B-08 line 7724; SPEC OUT-04 line 299

**Problem.** Drain stops at 2 ms and leaves the rest queued, so 'Drain <= 2 ms per frame at p99' passes even when the view falls behind until the 200k-line view-queue cap stops the view. Floods happen during runs, where drain runs per pump (10-20 Hz), not per frame, and T-OUT-015 measures only the idle case. The gate never checks that 50k lines/s is actually absorbed.

**Evidence.** 6.2 l.3321: "<= 2 ms per call; unfinished work waits for the next call". T-OUT-015: "Game-thread drain cost at 50k lines/s | Drain <= 2 ms per frame at p99 in an Insights capture".

**Fix.** T-OUT-015 (idle) and T-SUP-026 (during a run at 10 Hz pumps): add 'over 60 s at 50k lines/s the view-queue backlog stays under 1 s of input and no "lines not shown" marker appears'. SPEC OUT-04 and PERF-09 say whether 50k lines/s applies during runs.

**Refuter.** Drain is budget-capped (6.2 l.3321), so T-OUT-015 and T-SUP-026 (l.3117, during a run, per pump) both pass by construction. No test checks that 50k lines/s is absorbed without the view-queue backlog growing to the cap. The finding overlooked T-SUP-026, but T-SUP-026 has the same gap, so the defect stands.

### CORE-F25

**G1(2) paint gate times only OnPaint; Slate element batching of the same frame on the game thread is excluded** — minor, confirmed

**Where:** TDD 4.15 line 1888; TDD 13.2 B-03 line 7719; SPEC PERF-04 line 554

**Problem.** Sumi.Perf.CodeView times SCodeView::OnPaint. The draw elements it emits are batched into glyph quads later in the same game-thread frame by FSlateRHIRenderer::DrawWindows_Private, a cost proportional to visible glyphs and runs that the gate never sees. The Phase 0 architecture decision can pass G1(2) while the widget's real frame cost exceeds the budget.

**Evidence.** TDD 4.15 l.1888: "Sumi.Perf.CodeView ... OnPaint time from FPlatformTime::Cycles64". Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp:1512-1515 DrawWindows_Private (CSV_SCOPED_TIMING_STAT(Slate, DrawWindows_Private)) and :1634 ElementBatcher->AddElements(*WindowElementList); SlateCore/Private/Rendering/ElementBatcher.cpp:303.

**Fix.** As proposed: TDD 4.15 and B-03 add a second measure, the Slate/DrawWindows_Private delta between the 4K fixture with the Sumi tab visible and hidden, with a proposed budget of <= 1 ms p95 reported with G1(2).

**Refuter.** Engine checked: element batching runs after OnPaint, in FSlateRHIRenderer::DrawWindows_Private (SlateRHIRenderer.cpp:1511-1513, CSV stat DrawWindows_Private) via ElementBatcher->AddElements (:1634; ElementBatcher.cpp:303), on the Slate rendering (game) thread, and scales with emitted text elements. B-03 and Sumi.Perf.CodeView time only SCodeView::OnPaint.

### CORE-F26

**Section 6 moves regex filters, Ctrl+R history search and Copy as script to 'by M4' while SPEC lands them in M2** — minor, confirmed

**Where:** TDD 6 milestones line 3210; SPEC OUT-06 line 301; SPEC REPL-02 line 313; SPEC OUT-10 line 305

**Problem.** A builder following section 6 schedules three M2 requirements two milestones late.

**Evidence.** TDD l.3210: "Filters by regex, Ctrl+R history search and Copy as script are P1 and land by M4." SPEC OUT-06 Lands M2; REPL-02 'M1; M2 (Ctrl+R, prefix filter)'; OUT-10 Lands M2.

**Fix.** TDD 6 milestones: 'Regex filters (OUT-06), Ctrl+R and the prefix filter (REPL-02) and Copy as script (OUT-10) land in M2.'

**Refuter.** TDD 6 l.3210 says regex filters, Ctrl+R and Copy as script 'land by M4', while SPEC OUT-06 and OUT-10 land in M2, REPL-02 lands 'M1; M2 (Ctrl+R, prefix filter)', and the 14.7 rows (l.8022-8029) say M2. 'By M4' misleads a planner.

## Studio, security and delivery

| ID | Severity | Verdict | Finding |
|---|---|---|---|
| [STU-F02](#stu-f02) | critical | modified | On default Windows any standard user can create the machine policy, and a planted file is still applied to every account |
| [STU-F01](#stu-f01) | major | modified | The debugpy listener runs code sent by any local user or by a web page, and keeps doing so for the rest of the session |
| [STU-F03](#stu-f03) | major | confirmed | The debugpy listener ships in M2, but the policy key and audit entry that U15 requires land in M4 |
| [STU-F04](#stu-f04) | major | confirmed | trust.roots merges as a union, so a project Policy.json can widen trust past a machine or studio policy_only lock |
| [STU-F05](#stu-f05) | major | modified | SPEC and TDD disagree on which keys fail closed, and TDD's set lets a project merge conflict switch off completion for a whole team |
| [STU-F06](#stu-f06) | major | modified | Three spellings of the debug-server key, and an unknown key only warns, so a studio's lock can be silently ignored |
| [STU-F07](#stu-f07) | major | modified | Retention keys merge as min with no lower bound, so one project Policy.json value can wipe every teammate's local history and audit log |
| [STU-F08](#stu-f08) | major | modified | What happens when a studio item is tampered with differs between SPEC, TDD and the mockup |
| [STU-F10](#stu-f10) | major | confirmed | 'I keys apply even during a run' cannot happen: the snapshot swap is a ticker client, and ticker clients defer during runs |
| [STU-F11](#stu-f11) | major | confirmed | G3 must pass during M1, but it needs the stub transformer and server probes that RT-13 and the milestone table place in M2 |
| [STU-F12](#stu-f12) | major | confirmed | If G3's 'no server passes' fallback is invoked, the PERF-07/08/11 release gates and the M2 exit can never pass |
| [STU-F13](#stu-f13) | major | modified | G2's fallback does nothing, because the Run window is already the default, so a failing G2 has no defined outcome |
| [STU-F15](#stu-f15) | major | confirmed | §13 measures the Stop-latency gate from the pre-processor, in tier 3, which leaves out the wait for the next pump and has no pre-processor at all |
| [STU-F16](#stu-f16) | major | confirmed | The 30 ms startup gate has no defined baseline, and enabling Sumi force-enables Python |
| [STU-F09](#stu-f09) | minor | modified | Actor labels may contain newlines, so dragging an actor or using Copy as Python can insert live code into the buffer |
| [STU-F14](#stu-f14) | minor | modified | The 5.8-only 1.0 plan cannot meet its own 6-week rule: the next engine will almost certainly ship before GA |
| [STU-F17](#stu-f17) | minor | modified | The CI failing rule is incomplete: absolute checks run only on the GPU agent, though G3 and Stop budgets are measured elsewhere |
| [STU-F19](#stu-f19) | minor | confirmed | Anyone who can write the studio policy share can run code on every workstation, and nothing checks or documents this |
| [STU-F20](#stu-f20) | minor | confirmed | A manifest pin breaks the studio tier on every publish, and a mirror taken during a publish shows false tamper warnings |
| [STU-F21](#stu-f21) | minor | confirmed | A project file can redirect telemetry, and the opt-in itself is not excluded from policy locks |
| [STU-F22](#stu-f22) | minor | confirmed | storage.root_override can be set from the project layer with no per-user part, which mixes users' journals |
| [STU-F23](#stu-f23) | minor | confirmed | A configured studio policy file that is missing is not defined as unreachable, so it may fail open |
| [STU-F24](#stu-f24) | minor | confirmed | SetGroup(GetToolsCategory()) puts the tab in the Tools menu, not the Window menu |
| [STU-F25](#stu-f25) | minor | confirmed | The licence allow-list omits the Unicode licence that SumiCore's own character tables need, so the distribution check would fail |
| [STU-F26](#stu-f26) | minor | confirmed | No fallback if Fab refuses bundled executables, and no packaging plan if basedpyright, which needs a Node runtime, wins G3 |
| [STU-F27](#stu-f27) | minor | confirmed | Three versions of the Details-row Copy as Python text; the mockup's version raises NameError when run |
| [STU-F18](#stu-f18) | minor | refuted | The cut list is too small to absorb a realistic slip in M2–M4 |

### STU-F02

**On default Windows any standard user can create the machine policy, and a planted file is still applied to every account** — critical, modified

**Where:** TDD §11.2.1 lines 6666-6672; TDD §11.11 line 7342; TDD §2.7 line 561; TDD §11.2.2 line 6710; SPEC §7.18 SEC-02 line 485; 08 §7 RT-18 line 1589

**Problem.** RT-18 and SEC-02 assume %ProgramData%\Sumi\Policy.json can be written only by administrators. On a default Windows install, BUILTIN\Users may create folders and files under C:\ProgramData, and the creator owns what it creates. Until IT pre-creates the Sumi folder, any standard user can create it and plant Policy.json, which becomes the strongest layer for every account on that machine. The planted file can set 'language_services.binaries.pyrefly' to its own exe and sha256 (an override key, where machine wins). Every other user's editor then spawns that exe at deferred start, without any action from them. The doctor only checks whether the current user can write the file. That check is defeated when the planting owner removes the Users write entry, and in any case the failure table says the layer is 'Still applied'. Shared machines (school labs, RDS hosts, shared workstations) therefore get cross-user code execution.

**Evidence.** Running 'icacls C:\ProgramData' on this machine prints 'BUILTIN\Users:(CI)(WD,AD,WEA,WA)' and 'CREATOR OWNER:(OI)(CI)(IO)(F)', and 'Test-Path C:\ProgramData\Sumi' is False. TDD 6668: Machine | %ProgramData%\Sumi\Policy.json | Administrators. TDD 6672: the doctor warns only 'when the machine file or its folder is writable by the current non-elevated user'. TDD 7342: '| Machine policy writable by the user | AccessCheck in the doctor | Still applied | "Not enforced" warning |'. TDD 6710: 'language_services.binaries.<name> | {path, sha256} | bundled | override'. TDD 3.4 line 691: language-server children spawn at deferred start, 10 s after init if a tab is open.

**Challenges ruling.** RT-18 rests on the premise 'a fixed location that only administrators can write'. On default Windows that premise is false for %ProgramData%\Sumi until an administrator creates the folder (icacls evidence above).

**Fix (as modified by the refuter).** TDD §11.2.1, replace the doctor paragraph at line 6672 with: 'The machine layer is honoured only if Policy.json is owned by BUILTIN\Administrators, SYSTEM or TrustedInstaller and grants no other SID FILE_WRITE_DATA, FILE_APPEND_DATA, WRITE_DAC, WRITE_OWNER or DELETE, and %ProgramData%\Sumi is owned by one of those SIDs and grants no other SID FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER or DELETE (the inherited Users add-file right is allowed). On POSIX the file and every ancestor are owned by uid 0 with no group or other write. Otherwise the layer is ignored as untrusted, and a persistent banner and a doctor error name the file and its owner.' In §11.11 line 7342, change the row to '| Machine policy not admin-owned | Owner and DACL check at load | Layer ignored | Banner and doctor error naming the owner |'. Rewrite T-POL-007 as: a folder created by a standard user, with and without a Users write entry, is ignored with the banner; an admin-created folder with the default inherited ACL is honoured. TDD §2.7 line 561 and SPEC SEC-02: 'admin-only paths, verified by owner and ACL at load'. Record the RT-18 clarification in 08 §7.

**Refuter.** Confirmed on this machine: icacls C:\ProgramData shows 'BUILTIN\Users:(CI)(WD,AD,WEA,WA)' and 'CREATOR OWNER:(OI)(CI)(IO)(F)', and C:\ProgramData\Sumi does not exist. A standard user can create the folder, owns it, can strip other users' write access and plant Policy.json. TDD 6668 treats that file as admin-written, 6710 lets it name a language-server exe and sha256 (override, machine wins), and 7342 says 'Still applied'. The doctor check at 6672 tests only the current user's write access, which the squatter's ACL defeats. This is cross-user code execution on shared machines, so critical stands. The proposed fix is wrong in two ways. (1) 'No other SID holds write' on the folder rejects every ordinary deployment: a folder an admin creates under ProgramData inherits Users AD/WD (CI), yet Users still cannot replace an admin-owned Policy.json, because that needs DELETE or FILE_DELETE_CHILD, which they lack. The current FILE_ADD_FILE doctor check has the same false positive. (2) Making a squatted layer 'invalid' would let any standard user force the closed values on every account, a cross-user DoS, including on machines where IT never deploys a machine policy.

### STU-F01

**The debugpy listener runs code sent by any local user or by a web page, and keeps doing so for the rest of the session** — major, modified

**Where:** TDD §8.6.2 lines 5488-5491; TDD §8.6.4 line 5507; TDD §2.7 line 562; TDD §2.8 T-ARCH-015 line 595; SPEC §8.5 SEC-NF-01 line 600; SPEC §7.10 DBG-07 line 369; 08 §8 U15 line 1653; 08 §7 RT-11 line 1582

**Problem.** The design treats the unauthenticated in-process pydevd server as a short-lived socket that only same-user processes can reach. The pinned pydevd says otherwise. (1) It accepts new connections in a loop until the process exits, and each new connection disconnects the attached IDE. (2) Its reader accepts any 'Content-Length:' header followed by a blank line as a DAP frame and silently skips other lines. So a browser HTTP POST is a valid DAP message, and so is a probe from another Windows session, because loopback TCP is not scoped to a user. (3) With no access token every request counts as authorized. (4) An 'evaluate' request without a frameId is executed with exec on pydevd's command thread, with no paused thread needed. The result: while the listener is open, which TDD says is until the editor closes, any local process of any user, or a web page that loops a no-cors POST over the ephemeral port range, can run arbitrary Python in the editor. Any stray connection also kicks the user's VS Code session. No test probes for this.

**Evidence.** debugpy 1.8.20 (bundled/libs/debugpy/_version.py:15), the same bundle 09-red-team.md:8 used, under C:/Users/anima/.vscode/extensions/ms-python.debugpy-2026.6.0-win32-x64/bundled/libs/debugpy/_vendored/pydevd. pydevd.py:1561-1581: 'while not self._kill_received: ... s.listen(1); new_socket, _addr = s.accept()', then 'api.request_disconnect(self.py_db, resume_threads=False)' for the previous client and 'self.py_db.initialize_network(new_socket, terminate_on_socket_close=False)'. _pydevd_bundle/pydevd_comm.py:255-275: 'if line.startswith(b"Content-Length:"): content_len = ...'; other lines are skipped until 'line == b"\r\n"', then 'self.process_net_command_json(self.py_db, json_contents)'. Lines that fail to parse are only logged (303-311). _pydevd_bundle/pydevd_process_net_command_json.py:212-216 refuses a request only 'if not py_db.authentication.is_authenticated()', which is true when no token is set (TDD 8.6.4: '1.0 uses none'). Same file, 1064-1065: 'if arguments.frameId is None: self.api.request_exec_or_evaluate_json(py_db, request, thread_id="*")'. pydevd.py:1605-1617 sends '*' commands to the PyDB command thread. pydevd_comm.py:1199-1209: with no frame, 'try_exec = True  # Always exec in this case'. TDD line 562 says 'Same-user processes can connect while it is open'. TDD line 5491 says 'The debug server stays open until the editor closes; there is no Stop debug server'. T-ARCH-015 checks only that the socket is bound to 127.0.0.1.

**Challenges ruling.** U15 and RT-11 accept 'any local process can connect while the server listens' as the residual risk. The pinned pydevd source shows a larger risk: web pages can reach the socket with a cross-protocol POST, other users' sessions can reach it, a single evaluate request runs code without a pause, and the exposure lasts the whole session because accept() loops and evicts the IDE.

**Fix (as modified by the refuter).** TDD §8.6.2, add to DebugpyBridge, applied right after poll_port() finds the port. (1) Replace py_db._waiting_for_connection_thread._server_socket with a wrapper that delegates listen/close/getsockname. Its accept() loops on the real socket, peeks 16 bytes with MSG_PEEK and a 2 s timeout, and closes any connection whose bytes do not start with b'Content-Length:' or that arrives while is_client_connected() is true. It returns the first one that passes. (2) The accept already blocked on the raw socket when the wrapper goes in is covered by wrapping py_db.initialize_network on the instance with the same prologue check. Pin both private names in T-DBG-040. Do not close after the first attach and add no idle timeout. TDD §2.7 line 562: replace 'Same-user processes can connect while it is open' with 'Any local process of any user can connect while it is open, and a connected client can run code; HTTP-shaped clients are refused, and no second client is accepted while an IDE is attached'. Add T-DBG-051 (tier 4): an HTTP POST with an Origin header and the body {command: evaluate, arguments: {expression: <create canary file>}} creates no canary; a second TCP client while the scripted DAP client is attached is closed, and the first stays attached with its paused state unchanged; after the first disconnects, a new client attaches. SPEC SEC-NF-01: append 'it refuses non-DAP (HTTP-shaped) clients and accepts no second client while one is attached'. Record as an amendment to U15 in 08 §8.

**Refuter.** The pydevd facts check out in the 1.8.20 bundle. pydevd.py:1561-1581 runs an accept loop until kill; on a new connection it calls request_disconnect(resume_threads=False) for the old client (pydevd_api.py:214-221 does not resume threads, so an IDE-paused editor stays frozen until someone reattaches). pydevd_comm.py:245-275: non-Content-Length lines go to the line protocol and their int() parse error is caught and logged (295-301), so an HTTP POST gets through. _Authentication.is_authenticated() returns access_token is None (pydevd.py:539-541). An evaluate with no frameId goes to '*' (pydevd_process_net_command_json.py:1064-1065), and the comm code execs it (pydevd_comm.py:1199-1209). TDD 2.7 line 562 ('Same-user processes') is wrong: loopback is reachable by every user's processes. I lowered it from critical. U15 and TDD 8.6.4 already accept that any local process connecting while the server listens gets a debugger, and a debugger is code execution. Exposure 'until the editor closes' is already stated at TDD 5491. The listener only opens on an explicit command with a persistent indicator. The web vector depends on browser local-network-access rules the reviewer could not verify. What is new is cross-user reach on multi-session hosts, HTTP cross-protocol, and eviction of the attached IDE. Two parts of the fix are wrong. (b) closing the listener on first attach breaks disconnect-and-reattach, which pydevd's loop exists for and TDD 8.6.3 assumes (the port is stable per session). (c) a 10-minute idle close forces an editor restart, because debugpy refuses a second listen (api.py:143-145). Also, wrapping initialize_network alone runs after request_disconnect, so it cannot stop the eviction.

### STU-F03

**The debugpy listener ships in M2, but the policy key and audit entry that U15 requires land in M4** — major, confirmed

**Where:** SPEC §7.10 DBG-07 line 369; SPEC §7.18 SEC-02 line 485; TDD §11 feature table line 6533; TDD §8.6.4 lines 5505-5506; TDD §8.11 T-DBG-043 line 5658; TDD §15.2 M2 row line 8222

**Problem.** U15 allows the unauthenticated socket only with 'a policy key to forbid it'. TDD 8.6.4 adds an audit entry when the server starts. Both FSumiPolicy and FSumiAudit land in M4, while 'Debug in VS Code' lands in M2. In the M2 and M3 builds, studios therefore cannot forbid the listener. DBG-07's acceptance ('The policy key hides the command') and T-DBG-043 cannot pass at M2 exit, and the M2 exit requires 'every M2 test'.

**Evidence.** SPEC DBG-07: '`AllowDebugServer=false` forbids it. ... | M2 | ... The policy key hides the command.' SPEC SEC-02: '... `AllowDebugServer` ... | M4'. TDD 6533: 'Full layering and the inspector; FSumiPolicy, FSumiTrust, FSumiAudit ... | M4'. TDD 5506: 'Starting the server writes an audit entry (§11)'. TDD 8222, M2 exit: '... every M2 test'.

**Fix.** As proposed. In TDD §15.2 M2 scope and sections, the §11 feature table (line 6533) and the SPEC §10 M2 row, add 'a minimal FSumiPolicy (sumi::PolicyResolver over the machine and studio layers, with the §11.2.1 owner check) for debug.allow_debug_server only, and FSumiAudit's debug.server_started event'. Fallback if that cannot be done: in M2-M3, Debug in VS Code is behind an off-by-default user setting, and DBG-07's acceptance says so until M4.

**Refuter.** SPEC DBG-07 is M2 with the acceptance 'The policy key hides the command'. TDD 6533 and the §15.2 M4 row put FSumiPolicy and FSumiAudit in M4, and the M2 TDD sections (line 8222) do not include §11.2. The M2 exit requires every M2 test. Studios get the unauthenticated listener in M2-M3 builds with no way to forbid it.

### STU-F04

**trust.roots merges as a union, so a project Policy.json can widen trust past a machine or studio policy_only lock** — major, confirmed

**Where:** TDD §11.2.2 line 6695; TDD §11.2.2 line 6737; TDD §11.3.1 lines 6799-6804; SPEC §7.18 SEC-02 line 485; SPEC §7.18 SEC-01 line 484

**Problem.** SEC-02 says 'a key set above is locked below', and TDD itself claims 'none can loosen'. Yet trust.roots is a union across machine, studio and project layers, and policy roots are trusted even in policy_only mode. Suppose IT sets machine trust.mode=policy_only with roots=[\\studio\pipeline]. Any project committer, or any sample project that is opened, can add roots: ['C:\\'] and every local file becomes Trusted. The inspector shows no lock. The SPEC precedence matrix and the TDD golden table (T-POL-001) would also disagree on this key.

**Evidence.** TDD 6695: '`trust.roots`, `trust.blocked_roots` | path list | [] | union'. TDD 6737: 'So for security keys, any layer can tighten and none can loosen'. TDD 6803-6804: trusted by default are 'policy `trust.roots`' and 'the user's trusted folders, unless the mode is `policy_only`'. SPEC 485: 'Precedence: machine, studio, project, then user settings; a key set above is locked below.'

**Fix.** As proposed: in TDD §11.2.2, trust.roots is a union of the layers at or above the strongest layer that sets trust.mode=policy_only or trust.roots_locked=true (a new bool key, default false, merge any). Roots from lower layers are ignored and the inspector lists them as 'ignored: locked by <layer>'. trust.blocked_roots stays a full union. Reword line 6737 and SEC-02 as proposed, and add the locked-roots case to T-POL-001.

**Refuter.** TDD 6695 merges trust.roots as a union, and 6803 trusts policy trust.roots even in policy_only mode, so a project layer can add roots below a machine lock. That contradicts TDD 6737 ('none can loosen') and SPEC SEC-02 ('a key set above is locked below'). TDD 2.7 accepts that repository committers are not protected, but that covers the project layer's own keys, not overriding machine or studio locks. A builder cannot satisfy both texts in T-POL-001.

### STU-F05

**SPEC and TDD disagree on which keys fail closed, and TDD's set lets a project merge conflict switch off completion for a whole team** — major, modified

**Where:** SPEC §7.18 SEC-09 line 492; TDD §11.2.2 lines 6704-6713; TDD §11.2.3 lines 6759-6761; TDD §11.12 T-POL-003 line 7375; 08 §7.1 never-cut list line 1607

**Problem.** SEC-09 lists the closed values: bridge, MCP and debug server not allowed, telemetry off, trust policy_only, studio library off. It says 'other keys fall through to the next layer' and that the file 'leaves editing working'. TDD instead marks python.allow_enable_at_runtime, console_executor.enabled, live_completion.enabled, language_services.enabled, ruff, sumi_check and allow_experimental as 'C false'. One Policy.json with a merge-conflict marker in a source-controlled project then turns off live completion, which is on the never-cut list, and also language services, the Python (Sumi) executor and Enable Python, for every teammate until someone fixes the file. A builder gets two different behaviours, and T-POL-003 contradicts the SPEC acceptance test.

**Evidence.** SPEC 492: 'an unreadable or invalid policy layer makes its security keys take their closed values (bridge, MCP and debug server not allowed, telemetry off, trust `policy_only`, studio library off), while other keys fall through to the next layer'. TDD 6706-6708: '`console_executor.enabled` ... C false', '`live_completion.enabled` ... C false', '`language_services.enabled` ... C false'. TDD 6760: 'C keys take their closed values, as if that layer had set them.'

**Fix (as modified by the refuter).** TDD §11.2.2: change Invalid to D for live_completion.enabled, language_services.enabled, language_services.ruff, language_services.sumi_check, language_services.allow_experimental and console_executor.enabled. Keep C for python.allow_enable_at_runtime, python.allow_run_in_restrictive_mode, language_services.binaries.<name>, language_services.allow_user_binaries, trust.*, debug.allow_debug_server, bridge.*, mcp.*, telemetry.* and library.studio.*. SPEC SEC-09: extend the parenthetical to '(bridge, MCP and debug server not allowed, Enable Python at runtime and user-supplied server binaries not allowed, telemetry off, trust policy_only, studio library off)'. T-POL-003 then matches SEC-09.

**Refuter.** Confirmed. SPEC SEC-09 (line 492) lists only bridge, MCP, debug server, telemetry, trust and studio library as closed. TDD §11.2.2 also closes live_completion.enabled, language_services.*, ruff, sumi_check and console_executor.enabled, so one broken project Policy.json turns completion off for the whole team, and T-POL-003 contradicts SEC-09's 'leaves editing working'. The fix goes too far for python.allow_enable_at_runtime. That key is a permission over starting Python at all (SEC-07, D16). With D, a broken machine file that set it false would fall through to true and loosen a security lock.

### STU-F06

**Three spellings of the debug-server key, and an unknown key only warns, so a studio's lock can be silently ignored** — major, modified

**Where:** SPEC §7.18 SEC-01, SEC-02 and SEC-09 lines 484-492; SPEC §7.10 DBG-07 line 369; mockups/10-settings-and-trust.html lines 249 and 257 (images/10 callout 4); TDD §11.2.2 lines 6697 and 6702; TDD §11.2.3 line 6763; TDD §11.12 T-POL-002 line 7374

**Problem.** Admins will copy key names from the SPEC (AllowDebugServer, AllowUserTrust) or from the settings mockup (debug.allowDebugpy, 'run-window'). The keys TDD actually parses are debug.allow_debug_server and run_window. TDD treats an unknown key as 'one warning', so a policy that sets debug.allowDebugpy=false leaves the debug server allowed. The admin sees only a warning in the doctor. For a security lock this is fail-open.

**Evidence.** mockups/10-settings-and-trust.html:257: '<span class="hint">debug.allowDebugpy: false</span>'. Line 249: '"run-window" · locked'. TDD 6702: '`debug.allow_debug_server` (SPEC `AllowDebugServer`)'. TDD 6697: enum values 'run_window', 'live_docked'. TDD 6763: 'Unknown key: one warning.' TDD 17.2 A7 (line 8417) already found and fixed four other spellings inside the TDD.

**Fix (as modified by the refuter).** (1) and (2) as proposed: SPEC SEC-01, SEC-02, SEC-09, DBG-07 and SEC-NF-01 use debug.allow_debug_server and trust.allow_user_trust. Mockup 10 line 257 becomes 'debug.allow_debug_server: false' and line 249 '"run_window"'; re-render images/10. (3) TDD §11.2.3: 'In a file whose schema is not newer than the reader's, an unknown key at the top level or under trust, debug, bridge, mcp, telemetry, library, language_services, python or storage makes the layer invalid (C keys closed; the banner names the line and the nearest known key, so AllowDebugServer suggests debug.allow_debug_server). Other unknown keys, and unknown keys in a newer-schema file, give one warning.' (4) Add these cases to T-POL-002.

**Refuter.** Confirmed: mockups/10 line 257 reads 'debug.allowDebugpy: false' and line 249 reads '"run-window"'. SPEC uses AllowDebugServer and AllowUserTrust. TDD 6763 says an unknown key gets only a warning, so a lock written in either spelling fails open. The proposed rule (3) misses the most likely mistake: a top-level 'AllowDebugServer' copied from the SPEC is not 'under debug'. It also breaks forward compatibility, because TDD 6763 deliberately applies known keys from a newer schema.

### STU-F07

**Retention keys merge as min with no lower bound, so one project Policy.json value can wipe every teammate's local history and audit log** — major, modified

**Where:** TDD §11.2.2 lines 6720-6721; TDD §10.4 retention lines 6287-6292; TDD §11.4 line 6910; SPEC §7.14 PER-04 line 427; SPEC §7.14 PER-03 line 426

**Problem.** retention.history_days, history_mb, run_logs_days and audit_days are plain ints merged with min across the machine, studio and project layers, and none has a range. A project Policy.json with history_days: 0 or 1 is valid. At the next hourly sweep every entry older than the limit is tombstoned, and the 'keep each buffer's newest entry' rule applies only to the size limit. That removes every teammate's local history, including texts that recovery Discard moved there. audit_days: 0 deletes every audit file except today's, so a project committer can erase the audit trail. And if the built-in default takes part in min, studios can never raise retention above 30 days and 500 MB, although PER-04 calls retention 'studio-configurable'.

**Evidence.** TDD 6720: '`retention.history_days`, `.history_mb` | int | 30, 500 (D17) | min | D | W'. TDD 6721: '`retention.run_logs_days`, `.audit_days` | int | 30 ... | min'. TDD 6290: '1. Tombstone entries older than the day limit.' TDD 6291: '2. While blobs exceed the size limit, tombstone the oldest entries, keeping each buffer's newest entry'. TDD 6910: 'A sweep ... deletes files older than `retention.audit_days`, never today's file.' SPEC PER-03: 'discarded text is in local history'.

**Fix (as modified by the refuter).** TDD §11.2.2: ranges history_days 7-3650, history_mb 50-100000, run_logs_days 7-3650, audit_days 30-3650; an out-of-range value makes only that key invalid (D). Merge audit_days with max (a longer trail is stricter). Add under Merge kinds: 'Built-in defaults never take part in min, max, any, all or strictest merges; they apply only when no layer sets the key.' TDD §10.4 step 1: 'Tombstone entries older than the day limit, keeping each buffer's newest entry and every entry younger than 24 h.' Add a T-PER or T-POL case: a project file with history_days 0 is rejected for that key and deletes nothing.

**Refuter.** Confirmed. TDD 6720-6721 give the retention keys no range and merge them with min from any layer. §10.4 step 1 (line 6290) has no keep-newest rule, so history_days 0 in a committed project file tombstones all history, including the discarded recovery text that PER-03 promises is kept. audit_days 0 removes every audit file except today's (6910). Whether the built-in default takes part in min is unspecified; if it does, studios can never raise retention, contrary to PER-04's 'studio-configurable'. Parts of the fix are more than the defect needs. The 7-day delayed-apply notice adds a new mechanism, and once ranges exist, blocking the project layer is optional.

### STU-F08

**What happens when a studio item is tampered with differs between SPEC, TDD and the mockup** — major, modified

**Where:** SPEC §7.13 SNP-07 line 410; TDD §11.5.4 lines 7022-7025; TDD §11.2.2 line 6724; TDD §11.12 T-SNP-007 line 7405; mockups/07-snippets-and-shelves.html lines 449 and 478 (images/07 panel C, callout 11)

**Problem.** SNP-07 says a mismatch 'opens a non-modal prompt with no default action' offering Run modified copy once, and its acceptance test is 'A tampered studio item prompts; Run once is audited'. TDD says that with require_pinned, which is true by default, the item cannot run at all, and the prompt exists only when require_pinned is false. require_pinned also merges with 'any' (OR) and has a default of true. If the default takes part in the OR, false can never win, and the popover branch of SNP-07 and T-SNP-007 is unreachable. The mockup draws 'Restore published & run' as the primary button and calls it 'The safe default', which contradicts 'no default action' and 'Focus is on neither action'. Builders will ship different security defaults depending on which document they read.

**Evidence.** SPEC 410: 'running opens a non-modal prompt with no default action: Restore published & run, Run modified copy once (audited), Cancel'. TDD 7023: 'With `require_pinned` (the default), the item cannot run.' TDD 6724: '`library.studio.require_pinned` | bool | true | any | C true'. mockups/07 line 449: '<span class="btn primary">Restore published &amp; run</span>'. Line 478: 'The safe default restores the published version'.

**Fix (as modified by the refuter).** Conform the TDD to D28 and SNP-07. TDD §11.2.2: library.studio.require_pinned default false, merge any (true wins when any layer sets it; the built-in default does not take part). TDD §11.5.4: 'By default a mismatch opens the non-modal popover (no default action) ...; when policy sets require_pinned, the item cannot run and offers only Re-sync from library.' SPEC SNP-07: add 'Policy key library.studio.require_pinned=true blocks the run instead.' Use one label in both documents: SPEC's 'Restore published & run', or TDD's 'Re-sync from library and run', not both. T-SNP-007 stays as written. Mockup 07 panel C: give the two actions equal (non-primary) weight and reword callout 11 to 'Neither action is the default; running the changed copy is an explicit, audited, one-time choice'. Re-render images/07.

**Refuter.** The three-way contradiction is real. SPEC SNP-07 (line 410) says a mismatch prompts with no default. TDD 7023 says that with require_pinned, default true, the item cannot run. Mockup 07 line 449 makes 'Restore published & run' the primary button, and callout 11 calls it 'The safe default'. With merge any and a default of true, false can never win if the default takes part. The direction of the fix is wrong, though. ADR D28 (08 line 1489) says 'Changed content prompts the user (D20)', and ADRs outrank the TDD, so rewriting SPEC to match the TDD's blocking default would override D28 without challenging it.

### STU-F10

**'I keys apply even during a run' cannot happen: the snapshot swap is a ticker client, and ticker clients defer during runs** — major, confirmed

**Where:** TDD §11.2.3 lines 6755-6757; TDD §11 Terms line 6524; TDD §5.4.4 line 2423; TDD §11.12 T-POL-005 line 7377

**Problem.** The policy snapshot is swapped in by 'A Sumi ticker client'. Per RT-01 every Sumi ticker client checks IsRunActive() and defers. The pump never ticks FTSTicker. So during a supervised run no swap happens, and a tightened I key (debug.allow_debug_server, telemetry.mode, console_executor.enabled, trust.*, and from 1.1 bridge.allowed with 'listener closes') applies only after the run ends. T-POL-005's pass criterion 'I keys apply during the run' cannot be met.

**Evidence.** TDD 6756: '2. A Sumi ticker client swaps it in as `TSharedRef<const FSumiPolicySnapshot>`.' TDD 6757: '3. R keys are read once per run, at `Starting`. I keys apply even during a run.' TDD 6524: 'each one first checks `IsRunActive()`, then defers (re-arms) or refuses.' TDD 2423: 'The pump never calls `FTSTicker::GetCoreTicker().Tick`'. TDD 7377 (T-POL-005): 'I keys apply during the run'.

**Fix.** As proposed. TDD §11.2.2 Applies legend: 'I = at the next idle tick; never inside a run'. TDD 6757: 'I keys apply at the first idle tick after the swap; during a run they apply when it ends.' Any value that must act mid-run (the 1.1 bridge listener) is read from an atomic flag the policy worker sets. T-POL-005: 'I keys apply within one idle tick after the run ends; R keys at the next Starting.'

**Refuter.** TDD 6756 swaps the snapshot from 'A Sumi ticker client'. TDD 6524 says every Sumi ticker client checks IsRunActive() and defers. TDD 2423-2424 say the pump never ticks FTSTicker and that our deferred work is never applied inside a pump. 6757 ('I keys apply even during a run') and T-POL-005 (7377) therefore cannot hold, so the test cannot pass as written.

### STU-F11

**G3 must pass during M1, but it needs the stub transformer and server probes that RT-13 and the milestone table place in M2** — major, confirmed

**Where:** TDD §7.10.1 lines 4845-4860; TDD §15.2 M1 row line 8221; 08 §7 RT-13 line 1584; 08 §7 RT-10 line 1581; SPEC §10 M1 row line 746

**Problem.** G3 checks that every .pyi parses and type-checks under both servers, coverage of at least 99.5%, type agreement of at least 95%, and the pinned servers' RSS and latency. All of that is output of Stage 2 (the transformer and the .pyi package, §7.4.5) running against real servers. M1's scope covers only §7.4.2 (the Stage 1 snapshot) and sumi-check, and RT-13 says 'The transform and the .pyi package stay in M2'. G3 therefore cannot run when it is scheduled to, unless unbudgeted M2 work moves into M1. If it doesn't, M2 cannot start.

**Evidence.** TDD 4845: 'G3 runs during M1 and must pass before M2'. TDD 4852: 'stubgen, the comparisons and the server probes (tier 7, pinned real servers) then run out of process'. TDD 4857: 'Every `.pyi` parses ... and type-checks with zero errors under both Pyrefly and basedpyright'. TDD 8221 (M1 sections): '§7.1–§7.3 (`sumi-check` only), §7.4.2'. 08 1584: 'The transform and the `.pyi` package stay in M2'.

**Fix.** Prefer the first option, which keeps every ruling. TDD §15.2 M1 sections and SPEC §10 M1 row: add '§7.4.4-§7.4.5 docstring parser and transformer, built and run by the G3 harness only (the .pyi package and its use by the server ship in M2), plus a test-only probe client against the pinned servers'. Budget them in the Phase 0 re-baseline. Add a one-line note to 08 §7.2: 'RT-13: the transform ships in M2; a test-only build runs G3 during M1 (RT-10).'

**Refuter.** G3's checks need stubgen output: '.pyi parses and type-checks', coverage and type agreement (TDD 4852-4860). TDD 3892 places stubgen in M2, T-STUB-024 tests an 'M1 build without stubgen', and the M1 TDD sections (8221) list only §7.4.2. Yet RT-10, TDD 4845 and the M1 row require G3 to pass during M1. RT-10 and RT-13 pull against each other inside 08 §7 itself.

### STU-F12

**If G3's 'no server passes' fallback is invoked, the PERF-07/08/11 release gates and the M2 exit can never pass** — major, confirmed

**Where:** SPEC §8.1 PERF-07, PERF-08 and PERF-11 lines 557-561; SPEC §7.8 INT-14 line 339; SPEC §10 M2 row line 747 and G3 row line 764; TDD §7.10.1 fallbacks lines 4865-4867; TDD §15.2 M2 exit line 8222

**Problem.** The fallback keeps a language server for user code and serves unreal.* from runtime completion against a stub of '__getattr__ -> Any'. Yet PERF-07 is still a release Gate on 'the G3 probes' (unreal., unreal.EditorAssetLibrary., unreal.EditorActorSubsystem().). INT-14's acceptance requires the G3 thresholds. The M2 exit requires 'G3 thresholds hold in CI', which by definition no server met. No rule says which thresholds apply after the fallback, so M2 cannot exit and 1.0 cannot ship on the path the plan names.

**Evidence.** SPEC 557: 'PERF-07 | Static (LSP) completion | Warm p95 ≤ 400 ms on the G3 probes ... | Gate'. SPEC 747: 'LSP chaos suite green; G3 thresholds hold in CI'. TDD 4866: '*No server passes:* live completion serves `unreal.*`, and the server serves user code only ... minimal `unreal/__init__.pyi` that holds only `def __getattr__...`'.

**Fix.** As proposed: in TDD §7.10.1 Fallbacks and the SPEC §10 G3 row, define the fallback thresholds: the shipped server is the highest-scoring one that meets RSS and diagnostics; PERF-07 uses three user-code probes (self., a project module, os.path.) at warm p95 ≤ 400 ms; unreal.* completion is gated by PERF-06; the M2 exit and INT-14 read 'G3 thresholds, or the recorded fallback thresholds, hold in CI'.

**Refuter.** TDD 4866 and 08 §3 define a 'no server passes' fallback, yet SPEC PERF-07, PERF-08 and PERF-11 stay release Gates tied to G3, INT-14's acceptance repeats the G3 thresholds, and the M2 exit (TDD 8222, SPEC 747) needs 'G3 thresholds hold in CI'. On the fallback path, either the thresholds that failed still block M2, or the unreal.* probes run against a stub of `__getattr__ -> Any` and pass meaninglessly. Neither outcome is defined.

### STU-F13

**G2's fallback does nothing, because the Run window is already the default, so a failing G2 has no defined outcome** — major, modified

**Where:** SPEC §10 gates table, G2 row line 763; 08 §3 G2 row line 100; 08 §7 RT-03 line 1574; TDD §15.1 P0-2 line 8206 and go/no-go line 8215

**Problem.** 08 §3 says that if G2 fails, the Run window becomes the default. RT-03 already made the Run window the default, and SPEC now reads 'The Run window stays the default'. The Phase 0 go/no-go passes on 'G1 and G2 green or fallbacks invoked'. So a G2 failure in the Run window itself, for example Stop p99 above 250 ms (G2(1)) or crashes over 1,000 run/stop cycles (G2(2)), formally satisfies Phase 0 exit through a fallback that changes nothing, while M1 exit stays blocked with no plan B. The RunSupervisor is on the never-cut list, so this is the project's central risk without a decision rule.

**Evidence.** SPEC 763: '| G2 Run supervisor ... | Phase 0 and M1 exit; ... | The Run window stays the default |'. TDD 8215: 'G1 and G2 green or fallbacks invoked'. 08 1574: 'The Run window becomes the provisional default presentation.'

**Fix (as modified by the refuter).** SPEC §10 G2 row 'If it fails' and 08 §3 note: 'Failure in Live (docked): the Run window stays the default and docked stays Preview. Failure in the Run window: no fallback; Phase 0 does not exit. The CA either extends P0-2 by up to 4 weeks or re-plans at the go/no-go, and the decision is recorded in 08 §7.2.' TDD §15.1 line 8215: 'G1 green or its fallback invoked; G2 green in the Run window presentation'.

**Refuter.** Real. 08 §3's G2 fallback ('Run window becomes the default') has been a no-op since RT-03 made the Run window the default, and SPEC 763 now says only 'The Run window stays the default'. TDD 8215 lets Phase 0 exit on 'G2 green or fallbacks invoked', while P0-2's own exit (8206) requires G2(1)-(7) green, so the two contradict on whether Phase 0 can exit with G2 red in the Run window. The proposed 'reduced profile' is wrong. In pump Off (TDD §5.5.5, 2537-2541) 'check_cancel() returns False' and 'Stop is unavailable', so the profile contradicts itself, and making it the default would cut Stop and the RunSupervisor, which are on the 08 §7.1 never-cut list.

### STU-F15

**§13 measures the Stop-latency gate from the pre-processor, in tier 3, which leaves out the wait for the next pump and has no pre-processor at all** — major, confirmed

**Where:** TDD §13.2 B-12 line 7728; TDD §13.4 G2 row line 7772; TDD §5.15.1 T-CAN-001 and T-CAN-002 lines 3037-3038; TDD §5.4.6 lines 2445-2447; TDD §14.1 tier 3 row line 7793; SPEC §8.1 PERF-14 line 564

**Problem.** §13 claims to be the single list of where each budget is measured. It puts B-12 (PERF-14, 'click to exception', p99 ≤ 250 ms) from the 'Pre-processor timestamp of the Stop input' in the 'tier 3 harness'. Tier 3 is built 'without Slate', so it has no pre-processor. During a run, a click reaches the pre-processor only at the next pump, which runs at 15 Hz (up to 67 ms) and down to 2 Hz under overload (up to 500 ms), or later still when RT-02 defers it. The §13 point therefore leaves out the largest part of the latency users see. §5.15.1's T-CAN-001 measures correctly, 'from injection', in tier 4. A CI built from §13 would gate the wrong interval.

**Evidence.** TDD 7728: 'B-12 | Stop latency, click to exception | ... | Pre-processor timestamp of the Stop input → `ScriptCancelled` raised in the pending call | Counter pair; tier 3 harness'. TDD 7772: 'Tier 3 harness for (1)'. TDD 3037: 'Stop by an injected click; time from injection to the `ScriptCancelled` constructor'. TDD 3038: 'p99 ≤ 20 ms (proposed: mechanism only, no pump)'. TDD 2445: 'Target rate 15 Hz'. TDD 2447: 'floor of 2 Hz'. TDD 7793: '`FRunSupervisor`'s pending-call callback built without Slate'.

**Fix.** As proposed. TDD §13.2 B-12: Measuring point 'Injected OS click or Shift+F5 (SendInput timestamp) → ScriptCancelled constructor'; Instrument 'T-CAN-001/T-CAN-003, tier 4, GPU agent'. Add a sub-row 'Stop mechanism, flag → exception, p99 ≤ 20 ms, tier 3 (T-CAN-002)'. TDD §13.4 G2 'Measured where': 'tier 4 Sumi.G2.* for (1)-(7); tier 3 for the mechanism sub-budget'.

**Refuter.** TDD 7728 measures B-12 from the 'Pre-processor timestamp' in the 'tier 3 harness'. Tier 3 is built without Slate (7793), so it has no pre-processor. During a run the pre-processor sees input only at the next pump (15 Hz target, 2 Hz floor, 2445-2447). T-CAN-001 (3037) measures correctly, from injection, in tier 4. A CI built from §13.2 and §13.4 ('Tier 3 harness for (1)', 7772) would gate the wrong interval.

### STU-F16

**The 30 ms startup gate has no defined baseline, and enabling Sumi force-enables Python** — major, confirmed

**Where:** TDD §13.2 B-01 line 7717; TDD §3.9 T-LIFE-001 line 889; TDD §13.3 rule 3 line 7760; TDD §2.3.1 lines 349-360; SPEC §8.1 PERF-01 line 551; SPEC §11 A5 line 778

**Problem.** B-01 is measured 'compared with the same editor without the plugin', but the descriptor lists PythonScriptPlugin as Enabled:true. That plugin is EnabledByDefault:false, and once enabled Python initializes by default. On a project that did not use Python, enabling Sumi therefore adds the interpreter start and the unreal module setup, far above 30 ms, and B-01 could never pass. If the gate means only the Sumi.Startup.* scopes, it misses DLL load and CDO construction, which happen before StartupModule. The statistic is also defined two ways: B-01 and §13.3 say 'median of 5 cold starts', T-LIFE-001 says 'p95 over 20 boots'.

**Evidence.** TDD 359: '"Plugins": [ { "Name": "PythonScriptPlugin", "Enabled": true } ]'. Engine/Plugins/Experimental/PythonScriptPlugin/PythonScriptPlugin.uplugin:13: '"EnabledByDefault": false'. Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:105-106: 'static bool bIsEnabledByDefault = true;'. TDD 7717: '... compared with the same editor without the plugin | `Sumi.Startup.*` scopes; median of 5 cold starts per variant'. TDD 889: 'p95 over 20 boots'.

**Fix.** As proposed, with the instrument named. B-01 Measuring point: 'Both variants have PythonScriptPlugin enabled and Python initialised. Added cost = the LoadModule boot-timing scopes for SumiCore and SumiEditor (ModuleManager.cpp:1086-1087, which include DLL load and CDO construction) through the end of StartupModule, plus SSumiTab spawn to first paint.' Use one statistic, the median of 5 cold starts, in B-01, §13.3 and T-LIFE-001. SPEC PERF-01 and A5: 'Enabling Sumi enables PythonScriptPlugin; on a project that had it off, the interpreter start is added, is not counted in PERF-01, and the docs say so.' In v1.1 the citation is `ModuleManager.cpp:1035-1039`, the `FScopedBootTiming("LoadModule")` scope.

**Refuter.** The descriptor (TDD 359) enables PythonScriptPlugin. The plugin is 'EnabledByDefault': false (uplugin:13), and once loaded Python initialises at boot by default (PythonScriptPlugin.cpp:99 IsPythonLifeCycleDynamic false; :105 bIsEnabledByDefault true). Measured against 'the same editor without the plugin' on a project without Python, B-01 would count the interpreter start. Sumi.Startup.Module sits inside StartupModule, so it misses DLL load. B-01 and §13.3 say 'median of 5 cold starts', while T-LIFE-001 (889) says 'p95 over 20 boots'. The engine does time module loads: ModuleManager.cpp:1086-1087 has FScopedBootTiming and TRACE_LOADTIME_REQUEST_GROUP_SCOPE 'LoadModule - <name>'.

### STU-F09

**Actor labels may contain newlines, so dragging an actor or using Copy as Python can insert live code into the buffer** — minor, modified

**Where:** SPEC §7.15 UEI-01 line 440; TDD §11.6.3 lines 7150, 7152 and 7155; TDD §11.12 T-UEI-003 line 7418

**Problem.** The inserted text is 'unreal.find_object(None, "<path>")  # <label>'. Only the path goes through py_literal; the label is pasted raw into a comment. The engine accepts labels with embedded newlines. Someone can rename an actor in a shared or downloaded level, or a script can set its label, to 'Chair' followed by a newline and an os.system(...) call. The second line is then live code in the inserted text, and inside a 50-actor list it is easy to miss. The run that follows is typed code, which by design never prompts (SEC-01). The SCodeView buffer is LF-only, so CR, LF, U+2028 and U+2029 all need handling. The Details-row insert has the same '# <label>' pattern.

**Evidence.** Engine/Source/Runtime/Engine/Private/ActorEditorUtils.cpp:130-153: ValidateActorLabel only trims both ends ('InName.TrimStartAndEndInline()'), and rejects an empty name, '>= NAME_SIZE' and 'None'. It has no check for control characters. Engine/Source/Runtime/Engine/Private/ActorEditor.cpp:1294-1309 stores the label after that check. TDD 7150: '`unreal.find_object(None, "<GetPathName()>")  # <label>`'. TDD 7155: 'Literals use `py_literal(…, Quote::Double)`', which applies to the path, not the comment.

**Fix (as modified by the refuter).** As proposed. TDD §11.6.3: 'Label comments are sanitised: characters in Unicode categories Cc, Cf, Zl and Zp become a space, and the label is cut to 80 characters with an ellipsis; this applies to drops, the Details-row comment and Copy as Python.' Add T-UEI-017: labels containing \n, \r and U+2028 insert exactly one line per actor, and ast.parse finds no node beyond the expected find_object calls. SPEC UEI-01: 'label comments are sanitised to one line'.

**Refuter.** Real. ActorEditorUtils.cpp:130-153 only trims and rejects empty, too-long and 'None', and ActorEditor.cpp:1294-1309 stores the result, so a label set by a script or stored in a level or external-actor asset can contain LF. TDD 7150/7152 paste '# <label>' raw, and py_literal (7155) applies only to the path. Inside a multi-actor list, a second line such as __import__('os').system(...) runs as a list element. I lowered it to minor. Slate's single-line rename box strips control characters on input (SlateEditableTextLayout.cpp:2122-2128), so the label must come from a crafted asset or script. The injected text is also visible in the user's buffer, and the user must run it explicitly. U+2028 does not end a line for CPython's tokenizer, so only CR and LF can inject code, though sanitising the rest costs nothing.

### STU-F14

**The 5.8-only 1.0 plan cannot meet its own 6-week rule: the next engine will almost certainly ship before GA** — minor, modified

**Where:** SPEC §8.3 COMPAT-05 line 587; SPEC §8.3 COMPAT-01 line 583; SPEC §12 risk row line 798; TDD §15.2 1.0.x row line 8226; 08 §7 RT-20 line 1591

**Problem.** The local install is already on 5.8.4, its fourth hotfix, and 1.0 GA is planned for month 11–12. The SPEC's own risk table expects the next engine to 'land inside the 1.0 window'. COMPAT-05 says each new engine release is supported within 6 weeks, but the plan ports only after GA, as 1.0.x. An engine released at month 4–8 would breach the rule before 1.0 exists, and 1.0 would launch supporting only an engine one or more versions old. The plan has no rule and no budget for a port before GA.

**Evidence.** C:/Program Files/Epic Games/UE_5.8/Engine/Build/Build.version: 'MajorVersion': 5, 'MinorVersion': 8, 'PatchVersion': 4, 'BranchName': '++UE5+Release-5.8'. SPEC 587: 'each new engine release is supported within **6 weeks**; the first after 1.0 ships as **1.0.x**'. SPEC 798: 'Python 3.13 or the next engine lands inside the 1.0 window | 1.0 is 5.8 only; budgeted 1.0.x port'.

**Fix (as modified by the refuter).** SPEC COMPAT-05: add 'For an engine released before 1.0 GA, the 6-week clock starts at GA, and that engine is the 1.0.x port.' TDD §15.2 1.0.x row: the same sentence. No change to RT-20.

**Refuter.** Build.version shows 5.8.4, and SPEC 798 already expects the next engine 'inside the 1.0 window'. RT-20 deliberately scopes 1.0 to 5.8 only, with the next engine as a budgeted 1.0.x port, and the risk table accepts that 1.0 may launch behind. Making a pre-M4 engine an M4 item and having 1.0 support two engines re-opens RT-20, and the finding does not name it in challenges_ruling or bring evidence that the ruling is wrong. The only real defect is that COMPAT-05's 6-week clock is undefined for an engine released before GA.

### STU-F17

**The CI failing rule is incomplete: absolute checks run only on the GPU agent, though G3 and Stop budgets are measured elsewhere** — minor, modified

**Where:** TDD §13.3 lines 7758-7761; TDD §13.2 line 7713 and rows B-09, B-14, B-16, B-17 (lines 7725, 7730, 7732-7733); TDD §13.4 G3 row line 7773; TDD §14.1 tier 3 and tier 7 rows lines 7793 and 7797; SPEC §7.22 TEL-03 line 537

**Problem.** Rule 1 applies the absolute check only 'On the reference agent', which is the GPU agent. B-06, B-07 and B-10 (G3, tier 7, 'Win64 agent') and B-12 (tier 3) run on other agents, so they are only ever regression-checked. A G3 latency that is steadily over 400 ms never fails, and 'Agents with fewer than 5 results run the absolute check only' then checks nothing. Rows of kind Proposed (B-16, B-17) and 'Gate (M3)' (B-14) are covered by no rule. B-09 has two thresholds (10% target, fail above 15%) for a rule that fails 'above its target'. The 4-week rule has no definition of 'green' (does a one-night warning reset the clock?). SPEC TEL-03 says 'the CI PERF gates read' the Insights channels, while §13.1 says CI reads the Sumi.Perf.Dump JSON.

**Evidence.** TDD 7758: 'On the reference agent, a measured value above its target fails the build'. TDD 7713: 'The reference machine for absolute numbers is the GPU agent at 3840×2160'. TDD 7773: 'Win64 agent; during M1, then nightly'. TDD 7725: '≤ 10% of wall time at the target rate; **fail above 15% at 10 Hz**'. TDD 7761: 'Tracked rows produce the same report but never fail'; nothing covers Proposed rows. TDD 7693: CI reads 'JSON by the console command `Sumi.Perf.Dump`'.

**Fix (as modified by the refuter).** TDD §13.3 rule 1: 'Each Gate row's absolute check runs on the agent class named in §13.4 for its gate (GPU agent for tier 4; the Win64 harness agent for tiers 3 and 7).' Add: 'Proposed rows are Tracked until signed off; Gate (M3) rows block from M3 exit.' SPEC TEL-03 acceptance: 'CI gates read the Sumi.Perf.Dump counters; Insights captures are attached for diagnosis.'

**Refuter.** The central claim is refuted: 'a G3 latency steadily over 400 ms never fails'. T-LSP-038 (TDD 4960) has its own pass criterion 'warm p95 ≤ 400 ms on 3 probes', and T-DIAG-019, T-GAP-011 and T-IME-003 carry their own thresholds too, so those tests fail whatever §13.3's agent scope says. What remains is real but contained. §13.3 rule 1 names only the GPU agent although G3 runs on 'Win64 agent' (7773). Proposed and 'Gate (M3)' kinds are not covered by any rule. SPEC TEL-03 (537) says 'the CI PERF gates read' Insights channels, while TDD 7693 says CI reads the Sumi.Perf.Dump JSON.

### STU-F19

**Anyone who can write the studio policy share can run code on every workstation, and nothing checks or documents this** — minor, confirmed

**Where:** TDD §11.2.1 lines 6669-6672; TDD §2.7 Policy row line 561; TDD §11.12 T-POL-007 line 7379

**Problem.** The studio layer outranks the project layer for override keys such as language_services.binaries.<name> and library.studio.path. A Policy.json on a share that many TDs can write is therefore equivalent to automatic code execution on every workstation at deferred start. The doctor checks writability only for the machine file, and §2.7's 'Not protected' column lists only 'Local admins; repository committers'.

**Evidence.** TDD 6672: 'The doctor (U36) warns "machine policy not enforced" when the machine file or its folder is writable by the current non-elevated user'. The studio file has no equivalent check. TDD 561: 'Not protected: Local admins; repository committers'.

**Fix.** TDD §11.2.1: the doctor applies the AccessCheck to the studio policy file and its folder (GetFileSecurity on the resolved UNC path) and warns 'studio policy writable by you'. TDD §2.7 line 561, Not protected: add 'anyone who can write the studio policy file or its folder (equivalent to code execution on every workstation)'. Extend T-POL-007 to the studio layer.

**Refuter.** The studio layer outranks the project layer on override keys such as language_services.binaries.<name> and library.studio.path (TDD 6710, 6722). The doctor's writability check (6672) covers only the machine file, and TDD 2.7's 'Not protected' column does not mention the studio share. The binaries are SHA-pinned by the policy itself, so the binaries' own location adds nothing beyond write access to the policy file.

### STU-F20

**A manifest pin breaks the studio tier on every publish, and a mirror taken during a publish shows false tamper warnings** — minor, confirmed

**Where:** TDD §11.5.4 lines 7020-7022; TDD §11.5.5 lines 7034-7038; TDD §11.5.1 line 6942

**Problem.** If policy pins manifest_sha256, every Publish rewrites manifest.json, and the studio tier is disabled for everyone until IT edits the policy. The publish steps never mention the pin. The mirror also copies the share every 15 minutes without honouring manifest.lock or taking a consistent snapshot. A mirror run between steps 2 and 3 of a publish can copy the new item with the old manifest, which shows 'Modified since publish' (blocked by default) for up to one mirror interval and trains users to distrust the warning.

**Evidence.** TDD 7020: 'If policy sets `manifest_sha256`, the manifest must match it, or the studio tier is disabled with a banner.' TDD 7035-7037: 'take `manifest.lock` ... write the item ... re-read the manifest, update the entry's hash ... write it'. TDD 6942: 'The studio tier is mirrored on a worker every `mirror_minutes`, and on Refresh.'

**Fix.** As proposed. TDD §11.5.5 step 4: when policy pins manifest_sha256, Publish writes manifest.next.json plus a change bundle carrying the new pin, and the tier keeps the pinned manifest until policy changes. TDD §11.5.1: mirroring copies into a new generation folder, retries later while manifest.lock exists, verifies every item against the copied manifest before swapping, and keeps the previous generation on failure.

**Refuter.** TDD 7020: a policy manifest_sha256 pin disables the tier on mismatch, yet the publish steps (7034-7038) rewrite manifest.json without mentioning the pin. The mirror (6942) runs every mirror_minutes and does not honour manifest.lock. A copy taken between publish step 2 (item written) and step 3 (manifest updated) shows false 'Modified since publish' warnings until the next mirror.

### STU-F21

**A project file can redirect telemetry, and the opt-in itself is not excluded from policy locks** — minor, confirmed

**Where:** TDD §11.2.2 lines 6719 and 6727; TDD §11.1.1 line 6555; TDD §11.8.3 lines 7266-7268; SPEC §7.22 TEL-02 line 536; TDD §17.1 OI-91 line 8399

**Problem.** telemetry.endpoint is an override key, so a project Policy.json, including one in a downloaded sample, can send an opted-in user's events to a host of its choosing. The user consented globally (bTelemetryOptIn is engine-version-wide). The row 'Any FSumiSettingKeys id | override' also lets a policy lock any setting, and nothing excludes the telemetry opt-in. Policy could therefore force telemetry on, contradicting TEL-02 and OI-91 ('Today policy can only force it off').

**Evidence.** TDD 6719: '`telemetry.endpoint` | https URL | none | override'. TDD 6727: '| Any `FSumiSettingKeys` id | per key | per key | override |'. TDD 6555: USumiUserSettings holds the 'telemetry opt-in'.

**Fix.** TDD §11.2.2: telemetry.endpoint LayerMask is machine and studio only, and a project value is ignored with a warning. State that the telemetry opt-in has LayerMask GlobalUser only and is never a policy key. TDD §11.8.3: the opt-in names the endpoint host and asks again when it changes. Extend T-TEL-005 with a project-layer endpoint case.

**Refuter.** telemetry.endpoint is an override key with no layer restriction (TDD 6719), so a project file can redirect an opted-in user's events. The policy row for 'Any FSumiSettingKeys id' (6727) relies on each key's LayerMask (6569-6571), and the LayerMask for the opt-in held in USumiUserSettings (6555) is never stated. Nothing therefore enforces OI-91's 'policy can only force it off'. The events are content-free (7266), which keeps this minor.

### STU-F22

**storage.root_override can be set from the project layer with no per-user part, which mixes users' journals** — minor, confirmed

**Where:** TDD §11.2.2 line 6726; TDD §10.1 lines 6092-6094; SPEC §7.14 PER-08 line 431; SPEC §8.5 SEC-NF-04 line 603

**Problem.** A project Policy.json can point <root> at a shared UNC folder. Every teammate with the same project path gets the same <ProjectHash>, so their journals, history, run logs and audit logs, which hold proprietary and unsaved code, land in one folder that all of them can read. Instance locks then treat other people's editors as 'another instance'. This contradicts 'per user' in PER-08 and 'stay in per-user storage' in SEC-NF-04.

**Evidence.** TDD 6726: '`storage.root_override` | path | none (§10.1) | override | D | Startup'. TDD 6093: 'replaces the default and passes the same checks', and the checks reject only the engine and project trees.

**Fix.** As proposed. TDD §11.2.2: storage.root_override may be set only in user settings and the machine or studio layers. A policy value must contain a new {User} token (added to the path-token list), or it is rejected with a warning. Add a T-PER case: a project-layer override is ignored.

**Refuter.** storage.root_override is an override key from any layer (TDD 6726). §10.1 (6093) checks only that the root is outside the engine and project trees, and the path tokens offer no per-user part. A project file can therefore point every teammate's journals, history and audit at one shared folder, against PER-08 'per user' and SEC-NF-04.

### STU-F23

**A configured studio policy file that is missing is not defined as unreachable, so it may fail open** — minor, confirmed

**Where:** TDD §11.2.1 line 6669; TDD §11.2.3 lines 6750-6751 and 6759; SPEC §7.18 SEC-09 line 492

**Problem.** The text defines only an unreachable share (cache, then closed values) and an invalid file (unreadable, syntax error, over 1 MB, or no schema). It does not say what happens when SUMI_POLICY_PATH or the machine studio_policy_path is set, the share answers, and Policy.json is not there, for example after a rename. If an implementation treats that as an absent layer, every studio lock disappears.

**Evidence.** TDD 6750: 'A network studio path gets a 5 s timeout ... the last good copy ... is used'. TDD 6759: 'Invalid layer: unreadable, a JSON syntax error, over 1 MB, or no `schema`.' Neither covers 'not found'.

**Fix.** As proposed. TDD §11.2.3: 'When a studio path is configured (SUMI_POLICY_PATH or the machine studio_policy_path), a file that is not found is treated as unreachable (last good cache, else closed values), never as an absent layer.' Add the case to T-POL-006.

**Refuter.** TDD 6750-6751 define an unreachable share, and 6759 defines an invalid file ('unreadable, a JSON syntax error, over 1 MB, or no schema'). Neither covers a configured studio path whose share answers but whose Policy.json is missing, and reading that case as an absent layer drops every studio lock.

### STU-F24

**SetGroup(GetToolsCategory()) puts the tab in the Tools menu, not the Window menu** — minor, confirmed

**Where:** TDD §11.6.1 lines 7116-7123; TDD §11.5.6 menu table line 7057; SPEC §7.15 UEI-10 line 449; TDD §11.12 T-UEI-001 line 7416

**Problem.** TDD says the Tools-category group 'gives the Window-menu mirror'. In 5.8 the Tools category hangs under the Tools menu root, which the Tools main menu populates automatically. The Window menu populates from the other root. As written, Window gets no Sumi entry (UEI-10 and T-UEI-001 fail), and Tools shows the auto-generated 'Sumi' and 'Sumi Shelf' spawner entries in addition to the explicit 'Sumi Script Editor' section. Separately, UEI-10 extends MainFrame.MainMenu.Tools while §11.5.6 extends LevelEditor.MainMenu.Tools, which is its child, so following both duplicates the entry in the Level Editor.

**Evidence.** Engine/Source/Editor/WorkspaceMenuStructure/Private/WorkspaceMenuStructureModule.cpp:27-30 and 191-192: 'ToolsCategory(ToolsMenuRoot->AddGroup("Tools", ...))'. Engine/Source/Editor/MainFrame/Private/Menus/MainMenu.cpp:593-599: the Tools menu calls 'PopulateTabSpawnerToolMenu(TabManager, InMenu, MenuStructure.GetToolsStructureRoot(), true)'. MainMenu.cpp:268-294: the Window menu uses 'MenuStructure.GetStructureRoot()'. Engine/Source/Editor/LevelEditor/Private/LevelEditorMenu.cpp:372: 'RegisterMenu("LevelEditor.MainMenu.Tools", "MainFrame.MainMenu.Tools", ...)'.

**Fix.** As proposed. TDD §11.6.1: SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory()), or a Sumi group under GetStructureRoot(), for the Window entry, and keep the explicit Tools section. Use MainFrame.MainMenu.Tools in both TDD §11.5.6 and SPEC UEI-10. T-UEI-001 asserts exactly one Sumi Script Editor entry in Tools and one in Window.

**Refuter.** Verified in the engine. WorkspaceMenuStructureModule.cpp:191-192 adds ToolsCategory under ToolsMenuRoot. MainMenu.cpp:548 registers MainFrame.MainMenu.Tools, whose dynamic section (593-599) populates from GetToolsStructureRoot(). The Window menu (270-294) populates from GetStructureRoot() and the local workspace root. So SetGroup(GetToolsCategory()) (TDD 7123) lists the spawners in Tools, not Window, next to the explicit Tools section. LevelEditorMenu.cpp:372 registers LevelEditor.MainMenu.Tools as a child of MainFrame.MainMenu.Tools, while TDD 7057 and UEI-10 name different parents.

### STU-F25

**The licence allow-list omits the Unicode licence that SumiCore's own character tables need, so the distribution check would fail** — minor, confirmed

**Where:** SPEC §8.6 MAINT-05 line 611; TDD §11.10 line 7328; TDD §2.8 T-ARCH-008 line 588; TDD §11.12 T-TEL-008 line 7439; TDD §17.1 OI-48 line 8356

**Problem.** OI-48 records that SumiCore ships XID and grapheme tables derived from the Unicode Character Database. The allow-list is closed: allowed, allowed with conditions, denied. It has no Unicode licence. T-ARCH-008 ('allowed licence') and T-TEL-008 ('Each check fails on a seeded violation and passes on the release tree') therefore cannot both pass on the release tree.

**Evidence.** SPEC 611: '**allowed** MIT, BSD-2/3, Apache-2.0, ISC, Zlib, PSF, HPND (Scintilla fallback), BSL-1.0, OFL-1.1 (fonts)'. TDD 8356: 'Add the Unicode Character Database licence notice (XID and grapheme tables) to the D12 legal review'.

**Fix.** As proposed. Add 'Unicode-3.0 and Unicode-DFS-2016 (character data)' to the allowed list in SPEC MAINT-05 and TDD §11.10, and to D12. Close OI-48 when the notice is in ThirdPartyNotices.

**Refuter.** The MAINT-05 and D12 allow-list is closed and has no Unicode licence, yet SumiCore's XID and grapheme tables derive from the UCD (TDD 1961, OI-48). OI-48 tracks only the notice, not the allow-list entry. The 'cannot both pass' claim is overstated: T-ARCH-008 checks manifest files under Binaries/ThirdParty, Content/Python, Tools/Python and Resources (7326), not Source. The SBOM and licence check in T-TEL-008 would still hit it.

### STU-F26

**No fallback if Fab refuses bundled executables, and no packaging plan if basedpyright, which needs a Node runtime, wins G3** — minor, confirmed

**Where:** SPEC §13 open question 2 line 812; TDD §17.1 OI-05 line 8308; TDD §15.4 line 8250; TDD §7.2.5 line 4143; TDD §7.3.2 line 4191

**Problem.** Fab's executable policy gates the M2 binaries and is still open, and no outcome is planned if the answer is no. The options would be a Fab package without the language server, a runtime download (which conflicts with A3's 'no internet at runtime'), or GitHub-only. The G3 winner is bundled, and basedpyright is a Node program: TDD configures it with Node's --max-old-space-size. If it wins, the bundle grows by a Node runtime, which affects size, the SBOM, signing and Fab status, and none of that is planned.

**Evidence.** TDD 8308: 'OI-05 | **Fab's executable policy** ... it gates the M2 ThirdParty binaries in the Fab package | ... | Open'. TDD 4143: 'basedpyright (experimental) | `basedpyright-langserver --stdio` | ... Node `--max-old-space-size`'. TDD 4191: 'Type server | The **G3 winner** ... | Yes, from M2'.

**Fix.** As proposed. TDD §15.4: 'If Fab refuses bundled executables, the Fab package ships without Binaries/ThirdParty executables; the doctor and the status bar link the GitHub binaries pack, and language_services.binaries accepts the downloaded path; GitHub stays complete.' TDD §7.3.2 and §7.10.1: 'Packaging cost (runtime size, SBOM, signing, Fab status) is a G3 selection input; a Node-based winner ships a pinned Node.js runtime listed in Resources/Manifest.json.'

**Refuter.** basedpyright is configured with Node's --max-old-space-size (TDD 4143), and the G3 winner is bundled from M2 (4191), yet the TDD never mentions packaging a Node runtime (no other 'Node' hit), with its size, SBOM, signing and Fab effects. OI-05 is open and due before M2 (8308) with no fallback decided.

### STU-F27

**Three versions of the Details-row Copy as Python text; the mockup's version raises NameError when run** — minor, confirmed

**Where:** SPEC §7.15 UEI-01 line 440; TDD §11.6.3 line 7152; mockups/09-unreal-integration.html lines 461-462 (images/09 callout 9)

**Problem.** SPEC says the text is the set_editor_property line with the getter as a comment. TDD adds an 'obj = unreal.find_object(...)  # <label>' line first. The mockup's clipboard shows no obj line and an uncommented getter. Pasted into an empty buffer and run, the mockup's text raises NameError on obj, and the T-UEI-005 acceptance ('Running the line restores each value') depends on which version is built.

**Evidence.** mockups/09 461-462: 'obj.set_editor_property("cast_shadow", True)' / 'obj.get_editor_property("cast_shadow")'. TDD 7152: '`obj = unreal.find_object(None, "<path>")  # <label>`, then `obj.set_editor_property("<py name>", <literal>)`, plus the getter form as a comment'.

**Fix.** As proposed. SPEC UEI-01: 'they insert obj = unreal.find_object(None, "<path>")  # <label> and obj.set_editor_property("name", <repr>), with the getter as a comment'. Mockup 09 lines 461-462: add the obj line and comment out the getter; re-render images/09. Label sanitising per STU-F09 applies.

**Refuter.** SPEC UEI-01 and D30 (08 line 1550) give only 'obj.set_editor_property(...)' plus the getter. TDD 7152 adds an 'obj = unreal.find_object(None, "<path>")  # <label>' line, which T-UEI-005's 'Running the line restores each value' needs. Mockup 09 lines 461-462 show no obj line and an uncommented getter. TDD's text is a superset of D30, so no ruling changes.

### STU-F18

**The cut list is too small to absorb a realistic slip in M2–M4** — minor, refuted

**Where:** SPEC §10 lines 750 and 756; TDD §15.3 lines 8231-8245; TDD §15.2 M4 row line 8224; TDD §14.7 summary line 7942; 08 §7.1 lines 1595-1607

**Problem.** Three to four engineers must deliver 229 required IDs with 769 tests. Each test must fail before its implementation exists (MAINT-03). M2, M3 and M4 have to fit into roughly 6–7 months after M1 (month 4–5), with 4 green weeks before GA (month 11–12). Of the seven cuts, items 2, 5 and 6 are small, and item 7 keeps the in-editor debugger fully built as a Preview, with its G4 suite running nightly. The heaviest M4 work is on neither the cut list nor the never-cut list: policy, trust and audit, libraries with forms and publish, the inspector, the doctor, telemetry, async checkout and the docs. The rule 'Each cut moves one item and nothing else moves' forbids moving any of it. Telemetry in 1.0 ships 'no default public endpoint', yet still cannot be cut.

**Evidence.** TDD 7942: '247 requirement IDs; 229 have at least one test ... 769 test IDs'. 08 1595: 'Each cut moves an item to the release shown; nothing else moves.' TDD 8243: '7 | In-editor debugger GA | 1.1 | The debugger as an opt-in Preview'. TDD 7268: '1.0 ships **no default public endpoint**'. SPEC 741: 'Estimates assume 3–4 engineers and a part-time QA engineer'.

**Challenges ruling.** 08 §7.1: the list is not wrong, but it is too small for the scope. Evidence: the counts above and item 7 not removing the debugger's build cost.

**Fix.** None. If the Phase 0 re-baseline shows a shortfall, the CA amends 08 §7.1 then.

**Refuter.** This re-argues 08 §7.1 without the evidence the rules require. Counts of IDs and tests, set against a 3-4 engineer team, do not show that the plan cannot be met. The plan already re-baselines the dates at the Phase 0 go/no-go (SPEC 741; TDD 8215 'the date plan re-baselined'), and that is where measured throughput enters. 'Each cut moves an item ...; nothing else moves' describes what a single cut does. It does not forbid amending the list. The finding offers no concrete failure scenario, so choosing which further items to cut is a planning preference.

## Product, UX and mockups

| ID | Severity | Verdict | Finding |
|---|---|---|---|
| [UX-F01](#ux-f01) | major | modified | Paused, and tab-closed Running, status text is specified on surfaces the engine cannot paint |
| [UX-F04](#ux-f04) | major | modified | The shadowed-shortcut list is incomplete in U23, the TDD golden list and mockup 10, and SPEC 9.3 misses Ctrl+M |
| [UX-F05](#ux-f05) | major | modified | J5's flagship workload (a per-asset reimport) breaks both task-mode interactivity and the undo rules |
| [UX-F02](#ux-f02) | minor | modified | Mockup 02's 'first-pump frame' behind the Run window needs a full Slate tick inside the user's script, which is the RT-03 hazard |
| [UX-F03](#ux-f03) | minor | modified | Policy keys are spelled three different ways, and unknown keys only warn, so a policy written from the SPEC fails open |
| [UX-F06](#ux-f06) | minor | modified | SUM102's default scope and message contradict each other across the mockups, the report and the TDD |
| [UX-F07](#ux-f07) | minor | modified | Persona must-haves with no requirement: P3 'friendly errors', P6 'plain errors', P5 'running tests' |
| [UX-F08](#ux-f08) | minor | confirmed | The integrity prompt is drawn and described with a default action, and its labels differ across documents |
| [UX-F09](#ux-f09) | minor | modified | Scratch-buffer naming and the SPEC 9.5 image descriptions do not match the images |
| [UX-F10](#ux-f10) | minor | confirmed | Copy as Python on a Details row produces code that fails when pasted on its own |
| [UX-F11](#ux-f11) | minor | confirmed | Mockup 07 dropped the built-in pack tier that SNP-01 and SNP-06 require on first run |
| [UX-F12](#ux-f12) | minor | modified | Runtime Problems items in imported modules never clear unless the line is edited |
| [UX-F13](#ux-f13) | minor | confirmed | Esc in the Paused window: tests contradict each other, and text fields other than the console are not exempt |
| [UX-F14](#ux-f14) | minor | modified | The Paused window mockup lacks Run to cursor and sends condition editing to a tab that is blocked while paused |
| [UX-F17](#ux-f17) | minor | confirmed | J1 teaches learners a gesture that permanently stops the `sel` alias from refreshing, with no indication |
| [UX-F15](#ux-f15) | minor | refuted | Default trust omits the engine's own auto-executing sys.path roots, so J3's setup prompts and reload-on-save is blocked |
| [UX-F16](#ux-f16) | minor | refuted | Every run of 0.3–1 s flashes a focus-taking full Run window, and the obvious hidden-modal fix would kill Stop |
| [UX-F18](#ux-f18) | minor | refuted | Diff views carry their meaning by colour alone, against A11Y-05 |
| [UX-F19](#ux-f19) | minor | refuted | Run numbers restart each session, so runs from earlier sessions are ambiguous in recovery and local history |

### UX-F01

**Paused, and tab-closed Running, status text is specified on surfaces the engine cannot paint** — major, modified

**Where:** SPEC 7.1 WS-07 (line 197); SPEC 9.2 Paused row (line 667); SPEC 7.7 REPL-07 (line 318); TDD 11.6.2 state table (lines 7134-7142); TDD T-SNP-012 (line 7410), T-UEI-002 (line 7417); mockups/05-debugger-paused.html:153,162; mockups/09-unreal-integration.html:474,526

**Problem.** WS-07 lists `Sumi · Paused · Run #15` as a shown text. The SPEC 9.2 Paused row says what shows is the Paused window, "REPL suspended while paused" and WS-07 `Sumi · Paused · Run #15`. REPL-07 says the REPL "shows the message". TDD 11.6.2 gives the Paused row a click that "Focuses the Paused window". But the Paused window is a blocking modal (DBG-03). While it is open the main frame (status bar, Sumi tab, REPL) is never drawn, and clicks on it are consumed by the platform, so none of these can be seen or clicked. The same applies to mockup 09's "SUMI TAB CLOSED" panel, which shows a live `Running 41 s · Run #22 · 312 / 1,204 ■ Stop` chip, and to callout 12 ("reports shelf and menu runs even when the Sumi tab is not open"). It also applies to T-SNP-012, which expects "WS-07 shows Running" with the editor tab closed. With the tab closed, TDD 5.5.1 forces the Run window, so the item is frozen for the whole run. The Running pattern can only be seen in Live (docked) Preview with the tab on screen. Mockup 05 paints `Paused · Run #14` and a WS-07 Paused chip behind the modal.

**Evidence.** Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:1476-1497: `if (ActiveModalWindow.IsValid()) { DrawWindowAndChildren( ActiveModalWindow.ToSharedRef(), ...` then only ToolTip and notification windows are drawn. Input: SlateApplication.cpp:4894-4900 (ShouldProcessUserInputMessages is true only for the modal hierarchy), and Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:3195-3205 ("consume input messages"). TDD line 2478: "Live (docked) chosen, but the Sumi tab's run banner is not visible on screen (closed tab, ...) | Run window for this run". SPEC WS-07 itself: "It paints only when the editor paints; during a Run-window run the Run window shows instead." 05-debugger-paused.html:162: `Sumi · Paused · Run #14 · batch_rename.py:27 ■ Stop`.

**Fix (as modified by the refuter).** SPEC 7.1 WS-07 (line 197): delete '`Sumi · Paused · Run #15`,' from the examples. Replace 'It paints only when the editor paints; during a Run-window run the Run window shows instead.' with 'It paints only when the editor paints. While the Run window or the Paused window is open, only the modal is drawn (SlateApplication.cpp:1476-1497), so the item keeps its last painted text (normally Starting, or Running in Live (docked) Preview) and cannot be clicked. The Running pattern is live only in Live (docked) Preview.' SPEC 9.2 Paused row, What shows: 'Paused window; its footer says the REPL is suspended (REPL-07)'. SPEC REPL-07 requirement: 'While paused, REPL input is refused (the editor is not drawn while the Paused window is open); the Paused window footer says "The REPL is suspended; use Evaluate".' REPL-07 acceptance: 'While paused, the Paused window footer shows the message; after Continue the REPL accepts input.' TDD 11.6.2: delete the Paused row, and keep Paused in FSumiStatusSnapshot for accessibility announcements and the API. T-UEI-002: 'the four strings match U14'. T-SNP-012 expected: 'an item that runs longer than 300 ms opens the Run window centred on the main frame; after the run WS-07 shows the outcome'. T-E2E-005 expected: replace 'the REPL shows "REPL suspended while paused"' with 'the Paused window footer says the REPL is suspended'. Mockup 05: paint the main frame as it was before the pause (no Paused chip, no WS-07 Paused). Mockup 09: caption the Running chip 'Live (docked) Preview only'; callout 12 unchanged.

**Refuter.** Real. SlateApplication.cpp:1476-1497 draws only the active modal hierarchy, tooltips and notification windows while a modal is up. WindowsApplication.cpp:3195-3205, with SlateApplication.cpp:4886-4901, consumes input to every window outside the modal hierarchy. A blocking AddModalWindow (SlateApplication.cpp:2124 ff.) runs its own loop under the same draw rule. So none of these can ever be seen or clicked: the SPEC 9.2 Paused row (SPEC line 667, 'WS-07 `Sumi · Paused · Run #15`'), the WS-07 Paused example (line 197), and the TDD 11.6.2 Paused row with its click 'Focuses the Paused window' (TDD 7139). U14 (08 line 1647) defines only four strings and has no Paused string, yet T-UEI-002 (TDD 7417) says 'the five strings match U14'. T-SNP-012 (TDD 7410: 'WS-07 shows Running' with the tab closed) contradicts SPEC WS-07's own acceptance ('shows the Run window, then the Idle text'). The reviewer missed one case: T-E2E-005 (TDD 7901) expects 'the REPL shows "REPL suspended while paused"' while paused, and the engine never paints that. Two corrections to the fix. (1) 'Keeps its pre-run text' is wrong for a pause in Live (docked) Preview, where the last painted frame shows Running; say 'last painted text'. (2) The mockup 09 part is weak: its chip column is captioned 'The same item in other states' (09-unreal-integration.html:472), and callout 12 is true for outcomes and Task running, so a caption is enough.

### UX-F04

**The shadowed-shortcut list is incomplete in U23, the TDD golden list and mockup 10, and SPEC 9.3 misses Ctrl+M** — major, modified

**Where:** 08 §8 U23 (line 1671); TDD 11.1.4 golden list (line 6651); mockups/10-settings-and-trust.html:279-280 and the Shadowed Unreal shortcuts page; SPEC 9.3 keymap (lines 677-707); SPEC CFG-06 (line 505), CFG-08 (line 507), EDT-10 (line 226)

**Problem.** SPEC 9.3 says F4 shadows "Level Editor Show Actor Details" and F8 shadows "PIE Possess or Eject Player". It also lists Ctrl+W, Ctrl+PageUp/PageDown, Ctrl+D, Ctrl+S, Esc and the consume list. CFG-06 requires the page to list "every Unreal chord Sumi shadows", with the acceptance "Every chord in §9.3's 'Shadow…' column". The TDD golden list that the page test uses holds only "Ctrl+Space, Ctrl+H, Ctrl+Shift+F, Ctrl+G, F10, F11, Shift+F11", copied from U23's enumeration. So a page that omits F4 and F8 passes. Mockup 10 shows "—" in the Shadows column for F4/Shift+F4 and F8/Shift+F8, and its Shadowed page omits F4, F8, Ctrl+W, Ctrl+PageUp/PageDown and Esc. CFG-08's Ctrl+M ("shadows the Level Editor's Ctrl+M ... listed on the CFG-06 page") has no row in SPEC 9.3 and none in the mockup. EDT-10's Ctrl+0 reset is also missing from the 9.3 Zoom row. A user pressing F4 in Sumi to open actor details gets the error-link walk instead, with no listing anywhere.

**Evidence.** Engine/Source/Editor/LevelEditor/Private/LevelEditorActions.cpp:4701 `UI_COMMAND( ShowSelectedDetails, "Show Actor Details", ... FInputChord( EKeys::F4 ) )`; Engine/Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp:365 `PossessEjectPlayer ... FInputChord(EKeys::F8)`; LevelEditorActions.cpp:4653 `MoveSelectedToCurrentLevel ... FInputChord( EModifierKey::Control, EKeys::M )`; LevelEditorActions.cpp:4491-4492 (Ctrl+PageUp/PageDown), :4597 (Esc Unselect All), :4367 (Ctrl+S Save Current Level); DebuggerCommands.cpp:358 (Esc Stop). 10-settings-and-trust.html:279 `Next · previous error link ... F4 ... <span class="no">—</span>`.

**Fix (as modified by the refuter).** TDD 11.1.4 line 6645: 'ComputeShadowed() matches our chords against the commands that can act while a Sumi widget has focus: the main-frame global bindings, the Level Editor command list (Level Editor, PlayWorld and debugger commands) and the D8 consume list. Other contexts are not listed.' Line 6651: 'Golden list for Charcoal: Ctrl+Space, Ctrl+H, Ctrl+Shift+F, Ctrl+G, F4, F8, F10, F11, Shift+F11, Ctrl+PageUp, Ctrl+PageDown, Ctrl+M. Fixed rows: Esc (run-time Stop), and the consume list Ctrl+Z/Y, Ctrl+C/V/A, Ctrl+D, Ctrl+S, Ctrl+W, Delete, Backspace, End. PyCharm adds Ctrl+B; VS Code adds Ctrl+P.' T-CFG-011 stays as written; its equality check now holds. SPEC 9.3: add the row 'Tab moves focus (toggle, CFG-08) | **Ctrl+M** (proposed) | | | Ctrl+M: Level Editor Move Selection to Current Level', and add 'Ctrl+0 reset (EDT-10)' to the Zoom row. 08 U23: add F4, F8, Ctrl+PageUp/PageDown and Ctrl+M to the enumeration; this completes U23, which already says the page 'lists all of them'. Mockup 10: the F4 row shadows 'Show Actor Details' and the F8 row 'Possess or Eject Player'; add a Ctrl+M row.

**Refuter.** Real, and worse than stated. The engine bindings are verified: LevelEditorActions.cpp:4701 (F4 Show Actor Details), :4653 (Ctrl+M Move Selection to Current Level), :4491-4492 (Ctrl+PageUp/PageDown), :4597 (Esc Unselect All), :4367 (Ctrl+S); DebuggerCommands.cpp:358 (Esc Stop) and :365 (F8 Possess or Eject Player). The test that uses the golden list is T-CFG-011 (TDD 7371). It asserts 'Rows equal the golden list per preset', an equality check, so a correct page fails it as soon as it lists F4 or F8. The TDD's own algorithm also disagrees with SPEC 9.3. TDD 6645 says ComputeShadowed 'walks GetKnownInputContexts() ... and matches every active chord against ours'. That would list chords from contexts that never act while Sumi has focus, for example F9 GraphEditor Toggle Breakpoint (GraphEditorActions.cpp:48), F9 viewport Screen Capture (EditorViewportCommands.cpp:146) and F5 Outliner Refresh (ActorBrowsingModeCommands.cpp:14), all of which SPEC 9.3 marks '—'. So the golden list, the algorithm and SPEC 9.3 disagree three ways, and CFG-06's acceptance cannot be met. In addition, SPEC 9.3 has no row for CFG-08's Ctrl+M, and mockup 10 shows '—' for F4 and F8 (10-settings-and-trust.html:279-280). The fix must also scope the algorithm.

### UX-F05

**J5's flagship workload (a per-asset reimport) breaks both task-mode interactivity and the undo rules** — major, modified

**Where:** SPEC 5.2 J5 (line 140); SPEC 7.4 RUN-07 (line 247); SPEC 9.2 Task running row (line 668); TDD 5.11 step supervision (line 2926) and failure row (line 3024); TDD 14.5 T-E2E-006 (line 7902)

**Problem.** J5 reads: "A 20-minute reimport ... yields per asset; the editor stays interactive and viewports render; ... each step is an undo step". First, a per-asset reimport often takes more than 1 s, and SPEC 9.2 gives "the Run window for a step over 1 s". At 300 assets in 20 minutes (about 4 s per step), the modal opens for almost every step, closes for one frame and opens again. The editor is blocked most of the time, and the user cannot yield inside a native import call. TDD 3024 already knows this case ("Task steps consistently > 1 s ... 'Yield more often'"), but J5 is built on it. Second, RUN-07 says "Imports, file writes and PIE edits are never undoable". Yet J5 promises an undo step per step, and T-E2E-006 expects "Undo Last Run reports and reverts its step count" after reimports.

**Evidence.** SPEC line 668: "Task banner; WS-07 ...; the Run window for a step over 1 s". TDD line 2926: "The watchdog arms at 1 s for steps (RT-09). A step that runs longer is supervised like a run: pump, protections, presentation and Stop." SPEC line 247: "Imports, file writes and PIE edits are never undoable, and the block says so." TDD line 7902: "A `@sumi_editor.task` generator reimports 300 fixture assets ... Undo Last Run reports and reverts its step count".

**Fix (as modified by the refuter).** SPEC J5 (line 140): replace 'each step is an undo step' with 'each step runs in its own transaction `Sumi: task #N`, so property edits in a step can be undone; the reimports themselves are never undoable (RUN-07), and the run block says so'. TDD T-E2E-006: 'A `@sumi_editor.task` generator reimports each of 300 fixture assets, then sets `lod_group` with `set_editor_property`, yielding per asset ... Undo Last Run reports its step count and reverts those steps' `lod_group` edits; the run block says imports are not undoable.' TDD 5.10.4 stays as written.

**Refuter.** The undo contradiction is real. J5 (SPEC line 140) says each step of the reimport job is an undo step. RUN-07 (line 247) says 'Imports, file writes and PIE edits are never undoable'. FReimportManager::Reimport (Editor/UnrealEd/Private/Editor.cpp:328-345) opens no transaction, and REPL-06 says the engine discards empty transactions. So T-E2E-006 (TDD 7902), which expects 'Undo Last Run reports and reverts its step count' after per-asset reimports, cannot pass as specified. The interactivity half is speculative. Its 4 s per step comes from combining J5's 20 minutes with T-E2E-006's 300 assets, which no document does. TDD 5.10.4 (the finding says 5.11, but line 2926 sits in 5.10.4, which starts at line 2907) already handles long steps with the 'Yield more often' banner (line 2927). The proposed extra design, keeping one Run window open across consecutive supervised steps, would hold a modal up between steps and block the interactivity U10 promises. It should be dropped from the fix.

### UX-F02

**Mockup 02's 'first-pump frame' behind the Run window needs a full Slate tick inside the user's script, which is the RT-03 hazard** — minor, modified

**Where:** mockups/02-run-and-cancel.html:159,216,228,234,408,409; mockup-v2-report.md 'SPEC wording' items 3 and 4; TDD 5.4.4 pump (lines 2399-2417); TDD 5.5.3 Run window opening (lines 2499-2510)

**Problem.** Behind the modal, the editor shows `Running 0.3 s · Run #22`, a WS-07 chip `Running 0.3 s · Run #22 · 0 / 1,204 ■ Stop`, "Read-only while Run #22 runs" and a hatched "Input blocked during the run · last frame shown" cue. A comment labels the scene "as last painted at the first pump". The TDD pump opens the Run window in OnPumpBegin before App.Tick, so after the run starts the main frame is never drawn again. Its last frame is the pre-run frame: "Starting…" on the Run button (SPEC 9.2 Starting row). To paint the mockup's frame, the first pump would have to tick and draw every window with no modal up. That runs every editor widget's Tick in the middle of the user's script, including UMG and Editor Utility Widget Blueprint NativeTick, which can call Python. RT-03 put exactly this hazard class behind the Live (docked) Preview gate. The same frame would also flash the whole editor dim on every run that lasts 0.3–1 s.

**Evidence.** TDD line 2407 `Presentations.OnPumpBegin(Ctx); // first pump: protections and the Run window` runs before line 2410 `App.Tick(ESlateTickType::All)`. SlateApplication.cpp:1476-1497: only the modal hierarchy is drawn while a modal is active. Engine/Source/Runtime/UMG/Private/Slate/SObjectWidget.cpp:126-129 `WidgetObject->NativeTick(AllottedGeometry, InDeltaTime)`. TDD line 2463: "In docked mode, widget ticks in every window, including UMG and EUW Blueprint ticks that call Python". 08 §7 RT-03 (line 1574). 02-run-and-cancel.html:408: "Proposed: on the first pump Sumi paints one dimmed frame with this cue, then opens the Run window."

**Fix (as modified by the refuter).** Mockup 02: relabel line 159 'as last painted before the run started'. Show 'Starting…' on the Run button, and WS-07 in its last painted state (`Sumi · Starting · Run #22` if the open item on WS-07 strings is adopted). Remove the hatching, both blocked-cue labels (line 234 onward) and 'Read-only while Run #22 runs'. Callout 8: 'The editor behind keeps the frame it last painted; only the modal repaints until the run ends.' Callout 9: 'keeps its last painted text'. TDD 5.5.3 Opening: add 'Before the modal is added, the pump draws and ticks no other window; the editor keeps its last painted frame.' Do not mirror report items 3 and 4 into the SPEC.

**Refuter.** The engine facts hold. Painting a window ticks its widgets (SWidget.cpp:1505-1511 calls Tick during Paint), and SObjectWidget::Tick calls NativeTick (SObjectWidget.cpp:126-129). With a modal up, only the modal is drawn. A dimmed frame of the main editor therefore needs a full draw of the main frame, docked EUW tabs included, inside the user's script, which is the RT-03 hazard class. But the design documents do not adopt it. Callout 8 says 'Proposed' (02-run-and-cancel.html:408), and scratchpad report item 3 says 'This is a proposal. It is not in 08'. TDD 5.4.4 (line 2407) opens the modal in OnPumpBegin before App.Tick, and SPEC 9.5's description of image 02 (line 727) does not mention the dim frame. The defect is a mockup that shows an unadopted, hazardous behaviour, plus a callout 9 that states it as fact. It is contained to the mockup: minor.

### UX-F03

**Policy keys are spelled three different ways, and unknown keys only warn, so a policy written from the SPEC fails open** — minor, modified

**Where:** SPEC 7.10 DBG-07 (line 369); SPEC 7.11 SEC-01 (line 484), SEC-02 (line 485); SPEC 8.5 SEC-NF-01 (line 600); TDD 11.2.2 schema (lines 6680-6719), unknown-key rule (line 6763); mockups/10-settings-and-trust.html:249,257; mockup-v2-report.md 'SPEC wording' item 11

**Problem.** The SPEC uses ini-style PascalCase names: "`AllowDebugServer=false` forbids it" (DBG-07), "`AllowUserTrust=false`" (SEC-01), and the SEC-02 acceptance "machine `AllowDebugServer=false` beats project `true`". The TDD schema uses `debug.allow_debug_server`, `trust.allow_user_trust` and `run.presentation` with values `run_window` and `live_docked`. Mockup 10 shows `debug.allowDebugpy: false` and `"run-window"`, and the report proposes `trust.allowUserTrust` and `languageServer.allowed`. Under TDD 6763, an unknown key produces "one warning" and is otherwise ignored. A machine policy written from the SPEC or from the mockup therefore leaves the debugpy socket allowed and user trust enabled. The security setting fails open while the admin believes it is locked.

**Evidence.** SPEC line 485: "Keys: ... `AllowUserTrust`, prompts, bridge and agent runs, `AllowDebugServer` ...". TDD line 6702: "`debug.allow_debug_server` (SPEC `AllowDebugServer`) | bool | true". TDD line 6763: "Unknown key: one warning." 10-settings-and-trust.html:257 `debug.allowDebugpy: false`, and line 249 `"run-window"`.

**Fix (as modified by the refuter).** Mockup 10: line 257 `debug.allow_debug_server: false`; line 249 `"run_window" · locked`. SPEC SEC-02: after the key list, add '(file keys are the dotted names of TDD 11.2.2, for example `debug.allow_debug_server`, `trust.allow_user_trust`)'. TDD 11.2.3 'Smaller problems': replace 'Unknown key: one warning.' with 'Unknown key: one warning. In a layer whose `schema` is not newer than this build's, an unknown key under `trust.`, `debug.`, `bridge.`, `mcp.`, `python.` or `telemetry.` also makes that layer's C keys under the same prefix take their closed values, and raises the persistent policy banner with the file and line (SEC-09).' Add a policy test case: a machine policy with `debug.allow_debugserver: false` disables the debug server and shows the banner.

**Refuter.** Partly refuted. The SPEC-to-TDD naming is already mapped. TDD 11.2.2 line 6702 reads '`debug.allow_debug_server` (SPEC `AllowDebugServer`)', and line 6737 reads 'SPEC names map to keys as follows: `AllowDebugServer` is `debug.allow_debug_server` and `AllowUserTrust` is `trust.allow_user_trust`'. An admin writes the shipped schema, not SPEC prose. Two parts are real. First, mockup 10 shows `debug.allowDebugpy: false` (10-settings-and-trust.html:257) and `"run-window"` (line 249), which are not schema keys or values. Second, under the unknown-key rule (TDD 6763, 'Unknown key: one warning'), a typo in a security key fails open. The proposed fix, failing closed on any unknown key under a security prefix, would break the deliberate forward-compatibility rule on the same line ('A newer `schema` gets known keys applied and one warning'). It needs a schema qualifier. Contained: minor.

### UX-F06

**SUM102's default scope and message contradict each other across the mockups, the report and the TDD** — minor, modified

**Where:** SPEC 7.8 INT-08 (line 333); 08 §8 U5 (line 1633); TDD SUM102 rule (line 4231); mockups/03-completion-and-docs.html:304; mockups/04-traceback-to-source.html:269,321,413; mockups/02-run-and-cancel.html (code lines 28-30); mockup-v2-report.md 'SPEC wording' item 7

**Problem.** Mockup 03 fires SUM102 by default on `tex.lod_bias = 1` with "Attribute assignment skips the editor's change notifications". LODBias is BlueprintReadWrite, and TDD 4231 flags such writes only in an "off-by-default variant". Mockup 04 fires SUM102 on `mesh.lod_group = …` with "skips change notifications". LODGroup is edit-only and has no Python attribute, so that line raises AttributeError: the message gives the wrong reason, and a certain runtime failure is shown as a Warning. Mockup 02 assigns `task.filename`, `task.destination_path` and `task.replace_existing` (BlueprintReadWrite) with no SUM102, which matches the TDD but contradicts mockup 03. Report item 7 asks the SPEC to adopt the notification rationale, which would make SUM102 fire on most Unreal Python code and teach J7's users to ignore lint.

**Evidence.** Engine/Source/Runtime/Engine/Classes/Engine/Texture.h:1507 `UPROPERTY(EditAnywhere, BlueprintReadWrite, ...) int32 LODBias`. StaticMesh.h:699-700 `UPROPERTY(EditAnywhere, AssetRegistrySearchable, Category=LodSettings) FName LODGroup;` (not BlueprintVisible). PythonScriptPlugin Private/PyGenUtil.cpp:1609-1612: IsScriptExposedProperty requires `CPF_BlueprintVisible | CPF_BlueprintAssignable`, and :1805-1808 ShouldExportProperty; a grep of PythonScriptPlugin Private finds no `tp_dictoffset`, so assigning a non-exported name raises AttributeError. Engine/Source/Editor/UnrealEd/Public/AssetImportTask.h:35-48: BlueprintReadWrite Filename, DestinationPath and bReplaceExisting. TDD 4231: "It fires when `p` is not a getset of C (edit-only, so the assignment raises) ... An off-by-default variant also flags `[Read-Write]` attributes".

**Fix (as modified by the refuter).** TDD 4231 SUM102 row: add the message texts. Edit-only: '`<p>` is not a Python attribute of `<C>` (editor-only property); this assignment raises AttributeError. Use set_editor_property.' Read-only mismatch: '`<p>` is read-only as an attribute; use set_editor_property.' Variant: 'Attribute writes skip edit-change notifications; set_editor_property sends them.' Mockup 03 (lines 295, 304, 314): use an edit-only example such as `mesh.lod_group = "LargeProp"`, with the edit-only message. Mockup 04 (lines 269, 321): '`lod_group` is not a Python attribute of StaticMesh; the assignment raises AttributeError'. Keep Warning severity. Do not adopt report item 7 as the default rationale.

**Refuter.** The engine facts hold. LODBias is BlueprintReadWrite (Texture.h:1506-1507), and LODGroup is EditAnywhere only (StaticMesh.h:699-700). PyGenUtil.cpp:1609-1612 exports only CPF_BlueprintVisible and CPF_BlueprintAssignable properties. PythonScriptPlugin Private has no tp_dictoffset and no tp_setattro override, so `mesh.lod_group = …` raises AttributeError. So mockup 03 shows default SUM102 firing on a write that TDD 4231 flags only in the off-by-default variant, and mockup 04 gives the wrong reason for a line that will raise. However, TDD 4231 is explicit, and it is what the sumi-check builder implements. The 'severity Error' change in the proposed fix is a new design decision (the TDD and U5 say Warning) and is not needed to remove the defect. The SPEC INT-08 wording ('attribute assignment where set_editor_property is needed') is not wrong. The real gap is that only the mockups give message text, and that text is wrong. Minor.

### UX-F07

**Persona must-haves with no requirement: P3 'friendly errors', P6 'plain errors', P5 'running tests'** — minor, modified

**Where:** SPEC 5.1 personas P3 (line 125), P5 (line 127), P6 (line 128); SPEC 7.13 SNP-04 (line 406), SNP-10 (line 413); SPEC WS-07 (line 197); SPEC 9.4 (line 714); mockups/07-snippets-and-shelves.html panel B; TDD T-SNP-014 (line 7412)

**Problem.** No requirement delivers P3's "friendly errors" or P6's "plain errors". When an artist's shelf item fails from the Sumi Shelf tab with the Sumi tab closed, the only surface is `Sumi · Run #N error`, and clicking it opens the full traceback in the Sumi tab. The Sumi Shelf tab (mockup 07 B) has no Undo this run or Undo Last Run control. Ctrl+Alt+Z works "only inside Sumi widgets", and nothing says whether the Shelf tab counts. The artist usability test T-SNP-014 has no failure case. P5's "running tests" has no requirement, no Later row and no non-goal.

**Evidence.** SPEC line 125: "P3 Artist ... Shelf buttons, parameter forms, progress, Stop, undo, friendly errors". Line 128: "P6 ... plain errors". Line 127: "P5 ... running tests". A grep of SPEC for 'friendly', 'toast', 'notification', 'pytest' and 'test runner' finds only the persona rows. WS-07: "`Sumi · Run #23 error` (a click opens the traceback)". Notification windows are drawn even while a modal is up (SlateApplication.cpp:1491-1496), so a toast is a reliable surface.

**Fix (as modified by the refuter).** SPEC SNP-10: append 'A shelf, menu or Run on selection run that ends error, cancelled or not executed shows an editor notification with the item name, the outcome and the exception's last line (no traceback), plus [Details] (opens the Sumi tab at the run block) and [Undo this run]. The Sumi Shelf tab also has Undo Last Run and counts as a Sumi widget for Ctrl+Alt+Z.' SNP-10 acceptance: 'With only the Shelf tab open, a failing item shows its message in a notification, and Undo this run reverts it.' T-SNP-014: add a step with a failing item. SPEC 5.1 P5: either add a Later row 'TST-01 Run Python tests (unittest discovery under a root)' or remove 'running tests' from the must-haves.

**Refuter.** Partly real. SPEC 9.4 (line 714) already states a principle: 'Errors are inline and actionable, never "check the log"'. But no requirement says what an artist sees when a shelf run fails while only the Sumi Shelf tab is open. Mockup 07 callout 10 says 'The result goes to the Sumi output and to the Unreal status bar item', and the only route to the message is a full traceback in the Sumi tab. The 'no undo' part is overstated. The shelf run's transaction `Sumi: <item> #N` is on top of the editor undo stack, so Ctrl+Z works. But T-SNP-014 has the artist 'use Undo Last Run' from the Shelf tab, and mockup 07 B gives that tab no such control. P5's 'running tests' has no requirement and no Later row. These are persona must-haves, not requirements, and the gap is contained: minor.

### UX-F08

**The integrity prompt is drawn and described with a default action, and its labels differ across documents** — minor, confirmed

**Where:** SPEC 7.13 SNP-07 (line 410); SPEC 9.4 (line 717); 08 U39 (line 1707); mockups/07-snippets-and-shelves.html:449,478; TDD 11.5 integrity popover (line 7024)

**Problem.** SNP-07 requires "a non-modal prompt with no default action", and SPEC 9.4 says "No default action on trust and integrity prompts". Mockup 07 styles "Restore published & run" as the primary button, and callout 11 calls it "The safe default". The labels also differ: the SPEC and the mockup use Restore published & run / Run modified copy once, while the TDD uses "[Run this copy once] and [Re-sync from library and run]".

**Evidence.** 07-snippets-and-shelves.html:449 `<span class="btn primary">Restore published &amp; run</span>`; line 478: "The safe default restores the published version". TDD 7024: "offers [Cancel], [Run this copy once] and [Re-sync from library and run] ... Focus is on neither action".

**Fix.** Mockup 07 callout 11: 'No default action (U39): neither button has focus and Enter does nothing; Restore published & run is the recommended choice.' Button styling: make all three secondary, or keep the emphasis without focus, as the mockup owner prefers. TDD 7024: use the SPEC labels '[Cancel], [Run modified copy once] and [Restore published & run]', and keep the rule that Restore is offered only when the share copy matches.

**Refuter.** Real. SNP-07 (SPEC line 410) and SPEC 9.4 (line 717) require no default action. Mockup 07 styles 'Restore published & run' as primary (07-snippets-and-shelves.html:449), and callout 11 (line 478) calls it 'The safe default'. TDD 11.5.4 (line 7024) uses different labels: '[Run this copy once]' and '[Re-sync from library and run]'. A primary-styled button can remain as a visual recommendation as long as it does not take focus; the defect is the callout and the labels.

### UX-F09

**Scratch-buffer naming and the SPEC 9.5 image descriptions do not match the images** — minor, modified

**Where:** SPEC 9.5 table rows for images 03, 04, 05 and 08 (lines 728-733); SPEC RUN-06 (line 246); TDD 6.x examples (lines 3526-3528, 3561); T-OUT-027 (line 3813); mockups 01, 04, 06, 09, 10 (Untitled-N); 04-traceback-to-source.html (<sumi:Untitled-3#a41f>)

**Problem.** The SPEC and the TDD name scratch tabs `scratch-1`, with pseudo files `<sumi:scratch-1#1>` and `<sumi:scratch-1#7>` ("Buffer ID 7"). Every image uses `Untitled-N` and `<sumi:Untitled-3#a41f>` with a hex id. The traceback link parser, undo titles (`Sumi: scratch-1 #22` versus `Sumi: Untitled-3 #31`) and T-OUT-027 depend on this choice, and a decimal buffer id reads like a run number. SPEC 9.5 also describes content the images do not have: 05 "Debug in VS Code disabled with its reason" (absent), 08 "a stale-stub report" (the doctor shows "Stubs fresh"), and 03 a `keyword` badge (no keyword item is drawn).

**Evidence.** SPEC line 729: "Pseudo filenames (`<sumi:scratch-1#1>`)". TDD line 3561: "`File \"<sumi:scratch-1#7>\", line 18` | Buffer ID 7". 04 image frame text: `"<sumi:Untitled-3#a41f>", line 18`. 08-reload-and-environment.html:281 "Stubs fresh". A grep of 05-debugger-paused.html for 'VS Code' and 'debugpy' finds nothing.

**Fix (as modified by the refuter).** Adopt `Untitled-N` as the scratch display name, with a short hex buffer id. SPEC RUN-06: '`<sumi:Untitled-3#a41f>` (the id is the first 4 hex digits of the buffer FGuid, extended until it is unique among known buffers)'. SPEC 9.5 row 04: `<sumi:Untitled-3#a41f>`. TDD 3526-3528: `Untitled-3`. TDD 3561: '`File "<sumi:Untitled-3#a41f>", line 18` | Buffer a41f, even after a rename'. TDD 6212: 'display name such as "Untitled-3"'. T-OUT-027: `<sumi:Untitled-3#a41f>`. SPEC 9.5: delete 'Debug in VS Code disabled with its reason' (05), 'a stale-stub report' (08) and `keyword` (03), or add those elements to the mockups.

**Refuter.** Real, but there are three names, not two. The SPEC 9.5 row for image 04 (line 729) and the TDD (lines 3526-3528 and 3561, and T-OUT-027 at line 3813) use `scratch-1`. TDD section 10 (line 6212) gives the journal display name as 'such as "Scratch 3"'. Mockups 01-04 and 06-10 use `Untitled-N`. The buffer id is an FGuid (TDD 2151 `FGuid BufferId`), so the TDD's '#7 ... Buffer ID 7' (line 3561) is not a valid rendering, and no document defines the short form that ScanLinks (T-OUT-004) must parse. The SPEC 9.5 description gaps are also verified. Image 05 has no 'VS Code' or 'debugpy' text. Image 08 shows 'Stubs fresh' (08-reload-and-environment.html:281). Image 03 draws no keyword item; the only 'keyword' is in callout 2.

### UX-F10

**Copy as Python on a Details row produces code that fails when pasted on its own** — minor, confirmed

**Where:** SPEC 7.12 UEI-01 (line 440), UEI-02 (line 441); TDD 11.6.3 (line 7152); mockups/09-unreal-integration.html:459-463

**Problem.** UEI-01 inserts `obj.set_editor_property("name", <repr>)` with the getter as a comment, and no line that binds `obj`. Mockup 09's clipboard shows `obj.set_editor_property("cast_shadow", True)` and an uncommented `obj.get_editor_property("cast_shadow")`, with only a note that "obj is the owner". Pasted into the REPL, either version raises NameError on `obj`, so UEI-02's acceptance ("The copied expression resolves an equal object in the REPL") cannot hold. The TDD alone gets this right, adding the `obj = unreal.find_object(None, "<path>")  # <label>` line.

**Evidence.** TDD 7152: "`obj = unreal.find_object(None, \"<path>\")  # <label>`, then `obj.set_editor_property(\"<py name>\", <literal>)`, plus the getter form as a comment". 09-unreal-integration.html:461-462 has the two `obj.` lines and no binding line.

**Fix.** SPEC UEI-01: '...they insert `obj = unreal.find_object(None, "<path>")  # <label>`, then `obj.set_editor_property("name", <repr>)`, with `# obj.get_editor_property("name")` as a comment.' Mockup 09: add the binding line before line 461 and comment out line 462. Drop the '`obj` is the owner' note, or keep it as the label comment.

**Refuter.** Real. SPEC UEI-01 (line 440) and D30 (08 line 1550) give only `obj.set_editor_property("name", <repr>)`, with no line that binds `obj`. Mockup 09 (09-unreal-integration.html:461-462) copies that form, with the getter uncommented. Pasted on its own, either form raises NameError, so UEI-02's acceptance ('The copied expression resolves an equal object in the REPL') fails. TDD 11.6.3 (line 7152) already adds the `obj = unreal.find_object(None, "<path>")  # <label>` line. That extends D30's format without contradicting it.

### UX-F11

**Mockup 07 dropped the built-in pack tier that SNP-01 and SNP-06 require on first run** — minor, confirmed

**Where:** SPEC SNP-01 (line 403), SNP-06 (line 409); TDD 11.5.6 (line 7044), T-SNP-010 (line 7408), T-E2E-002 (line 7898); mockups/07-snippets-and-shelves.html:470; mockup-v2-report.md ("The built-in 'Unreal pack' tier was removed")

**Problem.** SNP-01 lists four tiers, including "the read-only built-in pack", and SNP-06 says it is "Present on first run". T-E2E-002 runs "the built-in 'Batch rename' item". Mockup 07's Snippets panel shows only Studio, Project and User, callout 3 says "Three tiers", and the report records the removal as a change. A builder following the image omits the tier that gives P6 learners content on first run.

**Evidence.** 07-snippets-and-shelves.html:470: "Three tiers. User ..., Project ... and Studio". SPEC line 409: "Built-in Unreal pack ... Present on first run, read-only".

**Fix.** Mockup 07: add a read-only 'BUILT-IN' tier group (transaction, slow task, selected assets and actors, Asset Registry query, data validation, Batch rename). Change callout 3 to 'Four tiers: Built-in (read-only, ships with Sumi), User, Project and Studio'. The scratchpad report is not a design document; correct its removal line if the report is kept.

**Refuter.** Real. SNP-01 (SPEC line 403) lists the read-only built-in pack as a tier, and SNP-06 (line 409) requires it to be 'Present on first run, read-only'. T-SNP-010 (TDD 7408) and T-E2E-002 (TDD 7898) depend on it. Mockup 07 callout 3 says 'Three tiers' (07-snippets-and-shelves.html:470), and line 64 of the scratchpad report says 'The built-in "Unreal pack" tier was removed'. No ruling in 08 removes the tier; a grep of 08 finds no ruling on a built-in or Unreal pack.

### UX-F12

**Runtime Problems items in imported modules never clear unless the line is edited** — minor, modified

**Where:** SPEC 7.6 OUT-11 (line 306); 08 U13 (line 1646); mockups/04-traceback-to-source.html callout 7; mockups/01-main-window.html (Problems count)

**Problem.** OUT-11 clears a runtime item "at the next run of that buffer or when the line is edited". In mockup 04, the KeyError is raised in `importer.py:88` by a run of Untitled-3. importer.py is imported, never run, so the item stays until line 88 is edited. If the fix lands elsewhere (the preset JSON, or the caller in Untitled-3), the stale error remains in Problems, the F8 walk and the file badge. Mockup 01 has the opposite gap: Run #14's uncaught exception at batch_rename.py:14 should add a runtime item, but Problems shows 1.

**Evidence.** SPEC line 306: "It clears at the next run of that buffer or when the line is edited." Mockup 04 callout 7: "It clears at the next run of that buffer, or when line 88 is edited." In mockup 01, the status bar reads "⚠ 1 problem" while Run #14 shows an uncaught traceback.

**Fix (as modified by the refuter).** SPEC OUT-11 and the TDD 4758 Clears column: 'The next run started from the same source as the run that raised it (buffer, entry point or shelf item), a reload of its module (RLD-01), an edit of the line, or Problems ▸ Clear runtime items.' Keep OUT-11's acceptance and add 'after the fix, re-running the caller clears the importer.py:88 item'. Mockup 01: show the Run #14 runtime item in Problems with the error icon.

**Refuter.** Real. OUT-11 (SPEC 306) and the TDD Problems source table (TDD 4758: 'The next run of that buffer by any route, or an edit of the line') clear a runtime item only when the buffer where the error was raised is run again. Take an error raised in an imported module: in mockup 04, importer.py:88 raises during a run of Untitled-3. If the fix lands elsewhere and the caller then re-runs cleanly, the stale item stays in Problems and in the F8 walk. The reviewer's fix leaves out the TDD row. The mockup 01 point is weak: its Problems '1' may be the runtime item drawn as a warning, so that is a styling ambiguity.

### UX-F13

**Esc in the Paused window: tests contradict each other, and text fields other than the console are not exempt** — minor, confirmed

**Where:** TDD 5.6.3 (line 2574); T-SUP-027 (line 3118); T-DBG-035 (line 5650); 8.3.5 (line 5379); mockups/05-debugger-paused.html:289

**Problem.** TDD 5.6.3 says Esc does not stop while the evaluate console has focus ("There, Esc clears the input"), and T-SUP-027 asserts "input cleared". T-DBG-035 asserts "First Esc closes completion; second stops", and completion exists only in the evaluate console. The Add watch field and inline watch editing are not exempt, and mockup 05 callout 2 says "Esc also stops, because this is a Sumi modal". A habitual double Esc while typing in a text field ends a long debug session as `cancelled`.

**Evidence.** TDD line 2574: "the Paused window's evaluate console does not have keyboard focus. There, Esc clears the input". TDD line 5650: "T-DBG-035 ... First Esc closes completion; second stops".

**Fix.** TDD 5.6.3, third bullet: 'no text-entry widget in the Paused window has keyboard focus (evaluate console, Add watch, inline watch edit, filter boxes); there, Esc closes completion first, then clears or cancels the input.' TDD 8.3.5 Stop row: 'Shift+F5; Esc when no menu or popup is open and focus is not in a text field'. T-DBG-035: 'With completion open in the evaluate console, the first Esc closes it, the second clears the input, and the run continues; Esc with focus on the stack or toolbar stops.' Mockup 05 callout 2: 'Esc stops when focus is not in a text field.'

**Refuter.** Real. TDD 5.6.3 (line 2574) exempts only the evaluate console ('There, Esc clears the input'). T-DBG-035 (TDD 5650) expects 'First Esc closes completion; second stops'. That can only mean the evaluate console, because live completion is off during runs (TDD 2491) and the source view is read-only. So T-DBG-035 contradicts 5.6.3. TDD 8.3.5 (line 5379) says 'Esc when no menu or popup is open', with no exemption for the console. Mockup 05 has a '＋ Add watch' text row (05-debugger-paused.html:268), and its callout 2 says 'Esc also stops'. RT-05's 'only when' permits the narrowing.

### UX-F14

**The Paused window mockup lacks Run to cursor and sends condition editing to a tab that is blocked while paused** — minor, modified

**Where:** SPEC 9.2 Paused row (line 667); DBG-01 (line 363); DBG-04 (line 366); TDD 8.3.5 key table (lines 5375-5381); mockups/05-debugger-paused.html:174-177,290

**Problem.** The SPEC Paused row offers "Continue, Step, Run to cursor, Stop", and DBG-04 binds Run to cursor to Ctrl+F10. Mockup 05's toolbar has Continue, Step over, Step into, Step out and Stop, with no Run to cursor control. Callout 3 says "Conditions are edited in the tab with Shift+F9", but while paused the tab is behind a modal and its input is consumed. TDD 8.3.5 lists only "Toggle breakpoint in the source view | F9", so a paused user cannot add a conditional breakpoint before continuing.

**Evidence.** 05-debugger-paused.html:290: "Conditions are edited in the tab with Shift+F9." WindowsApplication.cpp:3195-3205: input to windows outside the modal hierarchy is consumed.

**Fix (as modified by the refuter).** Mockup 05 callout 3: 'Shift+F9 or right-click a glyph edits a condition here; the tab's gutter is unreachable while paused.' TDD 8.3.5: add the row 'Edit breakpoint condition (source view) | Shift+F9, or right-click the glyph'. SPEC DBG-01: add 'While paused, breakpoints are toggled and conditions are edited in the Paused window's source view.' No toolbar change.

**Refuter.** Half real. The Run-to-cursor half is refuted. U17 (08 line 1655) lists the Paused window controls as 'Continue / Step over / Step into / Step out / Stop', and mockup 05's toolbar matches that list. Run to cursor is a key action (DBG-04 Ctrl+F10; TDD 8.3.5 'caret in the source view'), and a toolbar button for it is a preference. The condition-editing half is real. Callout 3 says 'Conditions are edited in the tab with Shift+F9' (05-debugger-paused.html:290), but the tab sits behind the modal and its input is consumed (WindowsApplication.cpp:3195-3205). TDD 8.3.5 lists only F9 for the source view. A paused user therefore cannot edit a condition, although DBG-01 offers Shift+F9 and right-click.

### UX-F17

**J1 teaches learners a gesture that permanently stops the `sel` alias from refreshing, with no indication** — minor, confirmed

**Where:** SPEC 5.2 J1 (line 136); UEI-03 (line 442); UEI-09 (line 448); VAR-02 (line 394); mockups/01-main-window.html (Selection aliases section); mockups/09 tooltip

**Problem.** J1, the P2/P6 exploration journey, starts with "press Ctrl+Shift+V to insert the `sel = …` line, run it". Under UEI-09, once that line runs, "a selection change does not rebind `sel`" for the session. The Variables panel still lists `sel` under "Selection aliases · as of …" (mockup 01). A learner who later selects other actors and runs `for a in sel: …` edits the old actors. The alias already holds the selection (U19), so the step adds nothing to J1.

**Evidence.** SPEC line 448: "after it runs, a selection change does not rebind `sel`". SPEC line 136: "press **Ctrl+Shift+V** to insert the ... line (U20), run it, type `sel[0].`".

**Fix.** SPEC J1: 'Select actors and type `sel[0].` in the REPL (the `sel` alias already holds the selection, U19); Ctrl+Shift+V inserts the explicit line when the script must stand alone (U20).' SPEC VAR-02 and TDD 4715: 'Once a name is user-owned, the aliases section and the hover show "sel: your value since Run #N; no longer refreshed".' U20 stays as written.

**Refuter.** A real UX trap, and it does not contradict U20. J1 (SPEC line 136) has a learner insert and run the Ctrl+Shift+V line. Under U19 and U20 (08 lines 1662-1663) and TDD 5.3.8 (line 2309), that makes `sel` user-owned for the session, so later selection changes no longer refresh it. Mockup 01 still lists it under 'Selection aliases · as of 12:41:30' (01-main-window.html:165). Neither the hover (TDD 4715) nor VAR-02 (SPEC 394) shows a user-owned name differently. A learner who selects other actors and runs `for a in sel:` edits the previous actors, which is undoable but unexpected.

### UX-F15

**Default trust omits the engine's own auto-executing sys.path roots, so J3's setup prompts and reload-on-save is blocked** — minor, refuted

**Where:** SPEC 7.11 SEC-01 (line 484); TDD 11.3.1 (lines 6799-6806); TDD line 5836; mockups/08-reload-and-environment.html:129 versus mockups/10-settings-and-trust.html:357-361

**Problem.** The default trusted roots are the project, the Sumi libraries, AdditionalPaths and policy roots. `<Documents>/UnrealEngine/Python` and `UE_PYTHONPATH` entries are not trusted, although the engine adds both to sys.path and runs `init_unreal.py` from every sys.path entry at each start. A P1 TD whose dev root sits on UE_PYTHONPATH (mockup 08: `D:\dev\pipeline\python`, UE_PYTHONPATH, "Reload on save · opted in") is restricted on first use. TDD 5836 says an untrusted root "cannot be enabled" for reload-on-save, and mockup 10's trusted list does not contain that root. The TDD already proposes trusting the engine and plugin Content/Python "because UE runs code from those at startup anyway"; the same reason applies here.

**Evidence.** Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:1267 (UserDir/UnrealEngine/Python added), :1282-1287 (UE_PYTHONPATH added), :1680-1690 (`PySysPath / TEXT("init_unreal.py")` run for every sys.path entry). TDD line 6806: "Proposed: also the engine directory and enabled plugins' Content/Python folders. UE runs code from those at startup anyway".

**Fix.** None. If the team wants fewer first-run prompts, there is a narrow option: promote TDD 6806's proposal for the engine and plugin Content/Python folders, or amend D20 to add only the local `<Documents>/UnrealEngine/Python` folder. Do not trust every UE_PYTHONPATH entry.

**Refuter.** The finding re-argues D20 without naming it, and it shows no failure beyond what D20 already accepts. D20 (08 lines 1201-1206) fixes the default trusted roots: the project, the Sumi libraries, AdditionalPaths and policy roots. It makes 'network shares not named in policy' explicitly untrusted, with a prompt once per folder and a target of at most one prompt per week. The engine facts are right: PythonScriptPlugin.cpp:1267 adds UserDir/UnrealEngine/Python, :1282-1287 adds UE_PYTHONPATH, and :1680-1690 runs init_unreal.py from every sys.path entry. But the scenario costs the TD one 'Trust this folder' click for the dev root, and reload-on-save can be enabled after it. That is the designed flow. Trusting every UE_PYTHONPATH entry would also trust the network share \\studio\pipeline\ue\python in mockup 08 (08-reload-and-environment.html:252) without a policy entry, which D20 forbids. Mockups 08 and 10 show different scenes, so the trusted-folder list in 10 lacking that root is not a contradiction.

### UX-F16

**Every run of 0.3–1 s flashes a focus-taking full Run window, and the obvious hidden-modal fix would kill Stop** — minor, refuted

**Where:** SPEC LRS-01 (line 273); TDD 5.5.3 steps 3-5 (lines 2502-2504); T-SUP-018 (line 3109)

**Problem.** The Run window opens shown and focused at the first pump (about 300 ms) and closes at run end. A REPL or selection run of 0.4 s therefore pops a full window over the output and takes focus away from the main window for about 100 ms. P2's fast-REPL loop gets a flicker on every medium call. Creating the slow-task modal hidden, as the engine does, looks like a fix but is not one. While a hidden modal is active, Windows input aimed at the main window is consumed before Slate's pre-processor runs, so Esc and Shift+F5 would stop working until the window is shown.

**Evidence.** SlateApplication.cpp:2172-2174: slow-task modals are created hidden ("we hide the window by default"). WindowsApplication.cpp:3195-3205: input messages for windows outside the modal hierarchy are consumed (`return 0;`). TDD 2502: `FocusWhenFirstShown(true)`.

**Fix.** None. Optionally, TDD 5.5.3 could state that the window must never stay hidden, because a hidden modal would consume Esc and Shift+F5. Step 5's ShowWindow already implies this.

**Refuter.** RT-04 (08 line 1575) decided that the Run window opens on the first pump, about 300 ms into the run, 'not at 1 s'. A run of 0.3–1 s showing it briefly is therefore by design, and mockup 02 callout 1 states the consequence ('Short runs end before it appears'). The effect is cosmetic. Nothing can be typed during a run anyway, because the GT is blocked and queued input is dropped (TDD 2510), and focus is restored at teardown (TDD 2495). No requirement or metric is broken, and the compact-state layout is a design preference. The point that a hidden modal would drop Esc is correct (WindowsApplication.cpp:3195-3205), but the documents propose no hidden modal.

### UX-F18

**Diff views carry their meaning by colour alone, against A11Y-05** — minor, refuted

**Where:** SPEC 8.4 A11Y-05 (line 595); mockups/06-crash-recovery.html CSS lines 98-100,114-115 and diff rows 203-215; TDD T-A11Y-004 (line 1776)

**Problem.** In the recovery diff, recovered-only text is blue and disk-only text is green (callout 4). In the local-history diff, changes are red and green tints. Neither view has +/− gutter glyphs or labels. A11Y-05 says colour is never the only carrier, but T-A11Y-004 checks only diagnostics and ran markers.

**Evidence.** 06-crash-recovery.html:98-99 `.dr .del { background: rgba(255,82,82,.11); } .dr .add { background: rgba(76,175,80,.13); }` and :114 `.dr .mine { background: rgba(79,195,247,.11); }`; the rows carry no glyph.

**Fix.** None required. Optionally, add +/− gutter glyphs to the diff views and include them in T-A11Y-004.

**Refuter.** Both diffs in mockup 06 are side by side. Rows use a five-column grid (06-crash-recovery.html:90 and the `.dr.two` rule). A line with no counterpart is drawn against a hatched filler cell (`.fill`, line 101), and the middle gutter has hunk checkboxes. Added and removed lines are therefore shown by column position and the hatch pattern, not by colour alone, and changed lines show both texts next to each other. A11Y-05's rule ('errors and states also have icons and text') covers diagnostics and states, and T-A11Y-004 tests those. Adding +/− glyphs would be a nicety, not a fix.

### UX-F19

**Run numbers restart each session, so runs from earlier sessions are ambiguous in recovery and local history** — minor, refuted

**Where:** 08 U8 (line 1641); SPEC RUN-12 (line 252); mockups/06-crash-recovery.html:231,322

**Problem.** Runs are "counted per editor session from 1". After a crash, the new session's output says "Run #22 was running when the editor exited · outcome lost", and the local-history timeline mixes "Before run #22", "#14" and "#13" from the crashed session with runs the new session will also number #13, #14 and so on, with no session label. Recovery triage, audit lookups and "Before run #N" entries become ambiguous.

**Evidence.** 06-crash-recovery.html:231: "Run #22 was running when the editor exited". Line 322: "Before run #22". U8: "counted per editor session from 1".

**Fix.** None.

**Refuter.** The documents already resolve the ambiguity. In mockup 06, every local-history entry has a time, and entries are grouped by day ('Today', 'Yesterday', lines 318-330; for example '14:02:51 Before run #22'). The recovery line reads 'Run #22 was running when the editor exited' (line 231), which can only refer to the previous session. Run logs are filed under `<storage root>/RunLogs/<session start>/run-<N>.log` (SPEC OUT-08, line 303), and the audit log records the run ID, start and end (SPEC SEC-05). Per-session numbering is a ruling (U8), and the finding shows no concrete case where a user would act wrongly.

## QA, traceability and consistency

| ID | Severity | Verdict | Finding |
|---|---|---|---|
| [QA-F02](#qa-f02) | major | confirmed | Studio-tier integrity default blocks modified items with no prompt, contradicting D28, SNP-07, T-E2E-007 and mockup 07; the project-tier rule also contradicts SNP-07 |
| [QA-F03](#qa-f03) | major | modified | The end-to-end tests must use AutomationDriver, which cannot act during a supervised run, a pause or a user modal, because it runs its steps from FTSTicker |
| [QA-F04](#qa-f04) | major | modified | Release-gate and journey tests depend on sleeps and on timing races; T-E2E-002 cannot reach its pass criterion deterministically and contradicts U12 |
| [QA-F05](#qa-f05) | major | modified | Tests have no milestone, so the milestone exit rule "every M1 test" cannot be computed, and the M1 exit test T-E2E-010 depends on an M3 test |
| [QA-F06](#qa-f06) | major | confirmed | SPEC Lands values and TDD landing tables disagree: WS-07 (M1 vs M2), PER-09's audit entry (M1 vs FSumiAudit in M4), local history (M2 vs needed in M1) |
| [QA-F09](#qa-f09) | major | confirmed | 14.7 reports full coverage, but several requirements are "covered" only by tests that check a different property |
| [QA-F01](#qa-f01) | minor | modified | Security policy key names differ across SPEC, TDD and mockup 10; an unknown key only warns, so a policy written from the SPEC is silently ignored |
| [QA-F07](#qa-f07) | minor | modified | The never-cut Stop-latency gate is measured from three different points; B-12 starts the clock after the input has waited for the next pump |
| [QA-F08](#qa-f08) | minor | modified | Automated gate suites contain manual tier-8 tests, so nightly and every-build gate status, and the 28-night docked-default rule, are undefined |
| [QA-F10](#qa-f10) | minor | modified | The TDD narrows RT-17's 10 s deferred-start trigger to "if a Sumi tab is open", so Python (Sumi) console completions never work until the first executed line |
| [QA-F11](#qa-f11) | minor | confirmed | The traceability generator splits cells on escaped pipes; 14.7 shows SNP-02's Lands as "a,b\", and T-GAP-008 reuses the same parser |
| [QA-F12](#qa-f12) | minor | confirmed | Startup and restore budgets are measured in incompatible ways |
| [QA-F13](#qa-f13) | minor | confirmed | Snippet insertion: 11.5.3 opens the parameter form on insert, while 4.7, OI-43 and SPEC SNP-02 say no form appears on insert |
| [QA-F14](#qa-f14) | minor | confirmed | The Charcoal keymap table in 4.9.4 disagrees with SPEC on milestones and leaves out the NAV-03 chords |
| [QA-F15](#qa-f15) | minor | confirmed | INT-07's 400 ms diagnostics debounce appears nowhere in the TDD, which uses 300 ms |
| [QA-F16](#qa-f16) | minor | confirmed | 7.1.5 says language servers never open "plain-mode buffers (files over 5 MB)", but Plain mode is only for files below 1 MB |
| [QA-F17](#qa-f17) | minor | confirmed | PER-03's read-only "Not restored yet" pending state has no TDD design or test; 10.7.1 describes an editable disk-text buffer instead |
| [QA-F18](#qa-f18) | minor | confirmed | Stale v0.9 references and name drift that mislead readers |
| [QA-F19](#qa-f19) | minor | modified | The G5 AltGr table does not test the French backslash that G5 and CFG-07 require |
| [QA-F20](#qa-f20) | minor | confirmed | No CI capacity plan for 430 tier-4 tests, two 8-hour soaks and every-build G2 on an uncounted GPU agent |
| [QA-F21](#qa-f21) | minor | confirmed | SPEC D.2 traces gaps to a mockup report and a QA report that are not in the repository |
| [QA-F22](#qa-f22) | minor | confirmed | Definition of done item 2 (each new test fails at the merge base on the pull request) cannot run for nightly-only tiers |
| [QA-F23](#qa-f23) | minor | refuted | TDD.md (820 KB, 8,757 lines) should be split into a folder of per-section files with an index; trimming alone saves under 10 percent |

### QA-F02

**Studio-tier integrity default blocks modified items with no prompt, contradicting D28, SNP-07, T-E2E-007 and mockup 07; the project-tier rule also contradicts SNP-07** — major, confirmed

**Where:** TDD.md 11.5.4 lines 7022-7026; TDD.md 11.2.2 line 6724; TDD.md 11.12 T-SNP-007 line 7405; TDD.md 14.5 T-E2E-007 line 7903; SPEC.md 7.13 SNP-07 line 410; SPEC.md 9.4 line 717; research/08-decision-log.md D28 line 1489; mockups/07-snippets-and-shelves.html lines 449, 478

**Problem.** D28 says studio "Changed content prompts the user" and SPEC SNP-07 says running a modified studio item opens a non-modal prompt with Run modified copy once (audited). The TDD makes `library.studio.require_pinned` default true, and with it "the item cannot run": no prompt and no Run once. T-SNP-007 tests the blocking behaviour, while the 1.0 GA journey test T-E2E-007 expects "Run once is audited", which cannot happen under the default. The TDD also applies "the same rule" to the project tier when a manifest exists, while SNP-07 says project items show "Unpublished changes" and never block. The prompt's buttons differ (SPEC: Restore published & run / Run modified copy once / Cancel; TDD: Cancel / Run this copy once / Re-sync from library and run), and mockup 07 calls Restore the "safe default" and styles it primary, against 9.4 "No default action on trust and integrity prompts (U39)".

**Evidence.** TDD.md:7023 "With `require_pinned` (the default), the item cannot run."; TDD.md:6724 "`library.studio.require_pinned` | bool | true"; TDD.md:7026 "The project tier follows the same rule when its manifest exists."; TDD.md:7405 T-SNP-007 "A tampered item shows the badge and cannot run when pinned"; TDD.md:7903 T-E2E-007 "a tampered studio item shows Modified since publish and Run once is audited"; SPEC.md:410 "running opens a non-modal prompt with no default action: Restore published & run, Run modified copy once (audited), Cancel ... a difference from the last Publish shows 'Unpublished changes' and never blocks"; 08 D28 line 1489 "Changed content prompts the user (D20)"; mockup 07 line 478 "The safe default restores the published version".

**Fix.** As proposed. TDD 11.2.2: `library.studio.require_pinned` defaults to false ("true lets a studio forbid Run once for modified items"). TDD 11.5.4: the default path is the non-modal prompt [Restore published & run] [Run modified copy once] [Cancel], with focus on none and Enter doing nothing. Replace the project-tier sentence with "Project items with a manifest show 'Unpublished changes' and run without a prompt (SNP-07, D20)." T-SNP-007: test both values of require_pinned. Mockup 07: change callout 11 to "No default action; restoring or running the changed copy is an explicit choice, and the copy run is audited." Remove the `primary` class from Restore, or keep it visual only, since focus and Enter must not pick it.

**Refuter.** Verified. TDD.md:7023 "With `require_pinned` (the default), the item cannot run." TDD.md:6724 sets the default to true. TDD.md:7026 applies the same rule to the project tier. These contradict SNP-07 (SPEC.md:410: prompt with Restore published & run / Run modified copy once / Cancel; project items "never block") and D28 (08 line 1489: "Changed content prompts the user"). Under the default, T-E2E-007 (TDD.md:7903, "Run once is audited") contradicts T-SNP-007 (TDD.md:7405). Mockup 07 line 478 calls Restore "The safe default" and line 449 styles it `btn primary`, against U39 and SPEC 9.4 "No default action". The cut list (08 7.1 item 4) moves the studio tier only if the schedule slips, so it does not excuse this.

### QA-F03

**The end-to-end tests must use AutomationDriver, which cannot act during a supervised run, a pause or a user modal, because it runs its steps from FTSTicker** — major, modified

**Where:** TDD.md 14.5 line 7893; TDD.md 14.1 tier 4 line 7794; TDD.md 14.5 T-E2E-002 line 7898, T-E2E-005 line 7901; TDD.md 4.16.1 RT-05 case 1 line 1932; SPEC.md 7.5 LRS-01 line 273

**Problem.** 14.5 says the journey tests "drive the UI through `AutomationDriver` (keys, clicks, drags), never through internal calls". In UE 5.8, AutomationDriver executes each step from a core FTSTicker callback on the game thread. During a supervised run the pump never ticks FTSTicker (LRS-01), and the blocking modal loop used by the Paused window and by `show_message` ticks Slate but not FTSTicker (TDD C11). So the steps that matter most cannot execute until the run or modal ends: Stop in the Run window (T-E2E-002), F10/F5/Alt+F8 and watch editing in the Paused window (T-E2E-005), and Esc into a script dialog (G5 RT-05 case 1, every build). As written, these tests hang or measure nothing.

**Evidence.** Engine/Source/Developer/AutomationDriver/Private/StepExecutor.cpp:70-79 `AsyncTask(ENamedThreads::GameThread, ...) FTSTicker::GetCoreTicker().AddTicker(... &FStepExecutor::ExecuteStep ...)` and :171 (each next step re-registered with `FTSTicker::GetCoreTicker().AddTicker`). Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:2238-2272: the modal loop calls `ModalLoopTickEvent.Broadcast`, `TickPlatform`, `TickTime`, `TickAndDrawWidgets`, and no ticker. SPEC.md:273 "never ticks FTSTicker or GEngine". TDD.md:7893 "drive the UI through `AutomationDriver` ... never through internal calls". TDD 14.2 (line 7854) already has `FSumiPausedWindowTestDriver` on `GetOnModalLoopTickEvent`, and 14.2 Input fixtures (line 7861) has an OS-level `WM_KEYDOWN` injector, but 14.5 forbids both.

**Fix (as modified by the refuter).** TDD 14.5, replace the last sentence with: "Idle steps use AutomationDriver. Steps during a supervised run or inside a user modal use the G5 input-bot FRunnable (§4.16.1, SendInput), synchronised on run-state probes and latches, never on sleeps. Steps in the Paused window use FSumiPausedWindowTestDriver (§14.2)." Extend the 14.2 Input fixtures row to name the input bot, including mouse clicks at widget geometry the game thread publishes. Name the mechanism in T-E2E-002, T-E2E-005 and T-E2E-006. Leave RT-05 case 1 unchanged. Note in 14.1 that SendInput needs an unlocked interactive desktop session on the GPU agent.

**Refuter.** The core claim holds. In UE 5.8, AutomationDriver's executor starts and re-arms every step with FTSTicker (Engine/Source/Developer/AutomationDriver/Private/StepExecutor.cpp:70-79 and :171). Element lookups marshal to the game thread with AsyncTask (DriverElement.cpp:1165-1181). The blocking modal loop ticks no FTSTicker (SlateApplication.cpp:2238-2276), and the only engine ModalLoopTickEvent listeners are ContentBrowserDataSubsystem and EditorAnalyticsSessionSummary. So TDD 14.5's rule (TDD.md:7893, "drive the UI through `AutomationDriver` ... never through internal calls") cannot run the run-time and paused steps of T-E2E-002, T-E2E-005 and T-E2E-006. The finding is wrong about G5 RT-05 case 1, though. G5 already injects with "OS-level `SendInput` from an input-bot `FRunnable`, which also works while GT sits in a modal loop" (TDD.md:1913), and RT-05 case 1's "bot" is that FRunnable, not AutomationDriver.

### QA-F04

**Release-gate and journey tests depend on sleeps and on timing races; T-E2E-002 cannot reach its pass criterion deterministically and contradicts U12** — major, modified

**Where:** TDD.md 4.16.1 RT-05 case 1 line 1932 (T-KEY-013, G5); TDD.md 14.5 T-E2E-002 line 7898; TDD.md 5.15 T-SUP-016 line 3107; TDD.md 11.5.2 line 6950; SPEC.md 7.13 SNP-06 line 409; research/08-decision-log.md U12 line 1645

**Problem.** (1) G5 runs on every build, and RT-05 case 1 sends Esc "300 ms" after a marker file is written, assuming the dialog is open by then. On a loaded agent the dialog is not yet modal, and Esc with no modal stops the run (RT-05), so the outcome becomes `cancelled`, not `ok`: a flaky release gate. (2) T-E2E-002 runs a "built-in Batch rename" item. The built-in pack in SNP-06 has no such item, and the only Batch rename body in the TDD (the 11.5.2 example) never calls `progress()`, so no `5 / 12`, rate or ETA can appear. Twelve renames also finish well inside the 300 ms arm delay, so by T-SUP-016's own rule the Run window never opens. "Stop at item 5" has no synchronisation point, and "Undo this run reverts the 4 renames" contradicts U12 (i = items completed, so a Stop shown at 5 / 12 leaves at least 5 renames). (3) T-SUP-016 relies on a 100 ms margin between a "200 ms run" and the ~300 ms arm.

**Evidence.** TDD.md:1932 "The bot waits for the marker, then 300 ms, then sends Esc. The call returns CANCEL and the outcome is `ok`."; TDD.md:7898 "run the built-in 'Batch rename' item ... Stop at item 5 ... the Run window opens on the first pump with `5 / 12`, rate and ETA ... Undo this run reverts the 4 renames"; TDD.md:6950 body `"for a in ${param:targets}:", "    rename(a, ${param:prefix})"` (no progress call); SPEC.md:409 built-in pack: "transaction, slow task, selected assets and actors, tick callbacks ..., Asset Registry query, data validation, editor utility subsystem"; TDD.md:3107 "a 200 ms run | ... the short run never pumps or opens a window"; 08 U12 "`i` is the count of items completed (0…n)". 14.2 (line 7854) lists `SleepNative` and `TickCoreTicker` but no latch fixture.

**Fix (as modified by the refuter).** TDD 14.2: add a `SumiTestLatch` fixture: `_sumi_test.wait(name)`, a pumped wait loop built on `sumi_editor.sleep(0.01)`, released by the driver with `release(name)`. State that gate and E2E tests synchronise on latches and state probes, never on fixed sleeps. T-E2E-002: use a fixture item `e2e-batch-rename` in SumiTestProject that calls `sumi_editor.progress(i, 12)` after each completed item and then waits on a latch. "The driver waits for `5 / 12`, clicks Stop through the input bot and releases the latch; Undo this run reverts exactly the 5 completed renames." T-SUP-016: replace the 200 ms busy loop with a latch-held run released at 150 ms and at 400 ms. Leave RT-05 case 1 as written. Before fixing the T-E2E-002 assertion, confirm that asset renames are undoable at all inside a run transaction (see notes); if they are not, rename actor labels instead.

**Refuter.** Part (2) holds. TDD.md:7898 T-E2E-002 has no synchronisation point for "Stop at item 5". It expects `5 / 12` on screen and then "Undo this run reverts the 4 renames", but under U12 (08 line 1645, i = items completed) showing 5/12 means at least 5 renames are done. The only Batch rename body (TDD.md:6950) never calls progress(), so no counter, rate or ETA could appear. Part (1) is refuted. Between the marker write and AddModalWindow the game thread is inside the native show_message call, and pumps run only from pending calls at bytecode boundaries, so the Esc stays in the OS queue until the modal loop's TickPlatform (SlateApplication.cpp:2259) delivers it to the dialog. The proposed fix for (1) also has the input-bot FRunnable call GetActiveModalWindow(), which is game-thread-only Slate state. Part (3), the 100 ms margin in T-SUP-016, is a minor flakiness risk and does not change the severity.

### QA-F05

**Tests have no milestone, so the milestone exit rule "every M1 test" cannot be computed, and the M1 exit test T-E2E-010 depends on an M3 test** — major, modified

**Where:** TDD.md 1.5 lines 256-258; TDD.md 14.5 line 7893 and T-E2E-010 line 7906; TDD.md 15.2 lines 8221-8223; TDD.md 14.6 T-GAP-008 line 7923

**Problem.** 15.2 exits M1, M2 and M3 on "every M1 test in 14.7", "every M2 test" and "every M3 test", but the test-table format (1.5) has no milestone column. 14.7 maps requirements to tests, and many tests cover requirements of several milestones (for example T-GAP-002 covers BUF-03's M1 and M2 parts). 14.5 says the journey tests run "from the milestone in the Lands column", but its table has no Lands column. T-E2E-010 is marked M1 and is an M1 exit criterion, yet it kills the editor "During T-E2E-001 and T-E2E-006": T-E2E-001 runs from M2 and T-E2E-006 needs task mode (M3). The M1 gate cannot run as specified.

**Evidence.** TDD.md:256 header "| ID | Tier | What it checks | Pass criterion | Covers |"; TDD.md:7893 "nightly from the milestone in the **Lands** column" (table at 7895 has no such column); TDD.md:7906 T-E2E-010 "(M1). During T-E2E-001 and T-E2E-006, kill -9 the editor"; TDD.md:7897 T-E2E-001 "(from M2 ...)"; TDD.md:7902 T-E2E-006 "(M3). A `@sumi_editor.task` generator"; TDD.md:8221 M1 exit "T-E2E-008 (without trust), T-E2E-010; every M1 test in §14.7".

**Fix (as modified by the refuter).** TDD 1.5: add a `Lands` column to every test table. Its value is the latest milestone among the features the test exercises; the generator takes it from the inline (Mx) markers or an explicit cell, and a split such as `M1 (M4: trust step)` is allowed. Extend T-GAP-008 to fail when a test's Lands is earlier than the Lands of any requirement it lists. Give 14.5 that column. T-E2E-010: "(M1) During the M1 variant of T-E2E-001 and a supervised 60 s Run-window run, kill -9 ...; from M3 also during T-E2E-006." 15.2: change "every M1 test in §14.7" to "every test whose Lands is M1", and likewise for M2 to M4.

**Refuter.** Verified. The 1.5 table format (TDD.md:256) has no milestone column. 14.5 cites a Lands column (TDD.md:7893) that its table lacks. T-E2E-010 (M1, TDD.md:7906) runs "During T-E2E-001 and T-E2E-006", and T-E2E-006 is M3 task mode. Computing "every M1 test in §14.7" from 14.7 picks up later tests: the M1 row WS-07 (TDD.md:7951) lists T-E2E-006 (M3) and T-SNP-012 (shelf, M4). Some rows carry an inline "(M2)" in What it checks (for example T-GAP-006), but not consistently. The proposed fix is backwards: generating Lands as the earliest milestone among covered requirements would give T-E2E-006 M1 (WS-07), and the proposed T-GAP-008 check would then fail.

### QA-F06

**SPEC Lands values and TDD landing tables disagree: WS-07 (M1 vs M2), PER-09's audit entry (M1 vs FSumiAudit in M4), local history (M2 vs needed in M1)** — major, confirmed

**Where:** SPEC.md 7.1 WS-07 line 197; TDD.md 11 lands table lines 6531-6533; TDD.md 15.2 M1 and M2 scope lines 8221-8222; SPEC.md 7.14 PER-09 line 432, PER-04 line 427, PER-03 line 426; SPEC.md 7.18 SEC-05 line 488; TDD.md 10.2.5 line 6208; TDD.md 10.12 T-PER-016 line 6478; SPEC.md 7.2 BUF-04 line 206

**Problem.** (1) SPEC WS-07 lands in M1 (the acceptance uses `Sumi.RunFile` with the tab closed), and 14.7 lists T-UEI-002 as an M1 test. TDD 11's table puts "the WS-07 item" in M2, and neither the M1 nor the M2 scope in 15.2 lists 11.6.2. (2) PER-09 (M1) requires that Run without snapshot "writes an audit entry" (T-PER-016), but FSumiAudit (SEC-05) lands in M4, and the M1 scope omits 11.4. M1 exit requires every M1 test green, so M1 cannot exit. (3) PER-04 local history lands in M2, but the M1 rows PER-03 ("discarded text is in local history") and BUF-04 ("a clean buffer reloads ... with a local-history entry") need the history store in M1. The TDD silently puts all of section 10 in M1.

**Evidence.** SPEC.md:197 WS-07 Lands "M1; M3 (task state)"; TDD.md:6531 "| Drag insertion, Copy as Python, Ctrl+Shift+V, Open in Sumi, Open in external editor, the WS-07 item | M2 |"; TDD.md:6533 "FSumiPolicy, FSumiTrust, FSumiAudit; ... | M4"; TDD.md:8221 M1 sections "... §10, §11.1 (project and user classes), §11.8.1, §12"; SPEC.md:432 PER-09 "Run without snapshot, which is audited (SEC-05)" Lands M1; SPEC.md:488 SEC-05 Lands M4; TDD.md:6478 T-PER-016 "Runs; audit entry records the skip"; SPEC.md:427 PER-04 Lands "M2; M4 (retention policy)"; SPEC.md:426 PER-03 (M1) "discarded text is in local history".

**Fix.** As proposed, plus one addition. TDD 11 lands table: move "the WS-07 item" to M1 (task state M3), and add 11.6.1 and 11.6.2 to the 15.2 M1 sections. SPEC SEC-05 Lands: "M1 (writer; run and snapshot-skip events); M4 (Run once, trust and policy events)". TDD 11 table: "FSumiAudit writer: M1; trust and policy events: M4", and add §11.4 to the M1 sections. SPEC PER-04 Lands: "M1 (store; entries on save, run, external reload and recovery); M2 (timeline, diff, Restore); M4 (retention policy)". 15.2 M2 sections: replace §10.4 with "§10.4 UI".

**Refuter.** Verified. (1) SPEC.md:197 WS-07 lands in M1, and its acceptance runs Sumi.RunFile with the tab closed. TDD.md:6531 puts "the WS-07 item" in M2. 15.2's M1 sections (TDD.md:8221) list neither 11.6.2 (WS-07 item, line 7127) nor 11.6.1 (tab spawners and menus, line 7114), which the M1 docked tab also needs. (2) PER-09 is M1 and T-PER-016 (TDD.md:6478) expects an audit entry, but SEC-05 lands in M4 and FSumiAudit is in the M4 row (TDD.md:6533). (3) PER-04 lands in M2, yet the M1 rows PER-03 ("discarded text is in local history") and BUF-04 ("with a local-history entry") need the store. 15.2 lists §10 in M1 and §10.4 again in M2.

### QA-F09

**14.7 reports full coverage, but several requirements are "covered" only by tests that check a different property** — major, confirmed

**Where:** TDD.md 14.7 summary line 7941; SPEC.md 7 preamble line 185; SPEC.md BUF-02 line 204, TEL-01 line 535, WS-05 line 195, RUN-12 line 252, BUF-04 line 206; TDD.md T-CORE-013 line 1102, T-LSP-004 line 4926, T-VIEW-012 line 1515, T-OUT-025 line 3811, T-E2E-005 line 7901

**Problem.** The SPEC promises "Each criterion maps to at least one automated test". T-GAP-008 counts any SPEC ID in a Covers cell, so a requirement passes when a test names it without testing it. Examples: BUF-02 (badges, path tooltip, read-only lock, restricted and recovered markers, middle-click close, reorder, pin) is covered only by T-CORE-013, a tier-1 dirty-flag test. TEL-01 (`LogSumi` with verbosity controls; "Both logs open from the editor") is covered only by T-LSP-004, a stderr framing and rotation test. WS-05's run-state, namespace, journal-age and debug-listener fields and its "no Cancelling or In native call" rule have no test (T-VIEW-012, T-LSP-029 and T-DIAG-025 cover the other fields). RUN-12's "History keeps the last 500 runs" has no test. BUF-04's "appears within 2 s" and "A change during a run applies after it" are not in its tests. T-E2E-005 lists DBG-05 and DBG-08 but raises no uncaught exception and enters no unsafe context.

**Evidence.** SPEC.md:185 "Each criterion maps to at least one automated test"; TDD.md:7941 "0 required IDs have no test"; TDD.md:1102 "| T-CORE-013 | 1 | Dirty flag across save, undo and a pruned save node | Clean only at the save node | BUF-02 |"; TDD.md:4926 "| T-LSP-004 | 7 | 1 MB of stderr interleaved with frames ... | 0 frame errors; log rotates at 10 MB ... | D4, TEL-01 |"; SPEC.md:535 TEL-01 acceptance "Both logs open from the editor."; SPEC.md:252 "History keeps the last 500 runs."; TDD.md:7901 T-E2E-005 steps (F9 condition, F5, watch, Alt+F8, F10, F5, Undo Last Run) with Covers "... DBG-05 ... DBG-08 ...".

**Fix.** As proposed. Add T-GAP-021 (BUF-02 tab states and gestures against golden paints with a stated pixel tolerance), T-GAP-022 (TEL-01: LogSumi verbosity and both log files open from the editor), T-GAP-023 (WS-05 run-state, namespace, journal-age and listener fields within 250 ms; a UI string scan finding no "Cancelling" or "In native call" state label), T-GAP-024 (RUN-12: 501 runs keep the last 500) and T-GAP-025 (BUF-04: an external edit is visible within 2 s; one made during a run applies after it). Remove DBG-05 and DBG-08 from T-E2E-005's Covers. Either add an acceptance-criterion cross-check to T-GAP-008, or reword SPEC 7's preamble to "Each requirement maps to at least one automated test".

**Refuter.** Verified against the rows. SPEC.md:185 promises "Each criterion maps to at least one automated test", and 14.7 (TDD.md:7941) reports 0 untested IDs. But BUF-02 (M1 badges and close; acceptance "Visual states match mockups") is covered only by T-CORE-013 (TDD.md:1102), a tier-1 dirty-flag test. TEL-01 ("Both logs open from the editor") is covered only by T-LSP-004 (TDD.md:4926), a stderr framing and rotation test. No test checks WS-05's run state, namespace, journal age or debug-listener fields, or its "no Cancelling/In native call" rule: T-VIEW-012 covers Ln/Col, indent, encoding and EOL, T-LSP-029 health, T-DIAG-025 counts. No test checks RUN-12's "History keeps the last 500 runs". BUF-04's "within 2 s" and "A change during a run applies after it" are not in T-CORE-030, T-PER-017 or T-E2E-003. The gate that is meant to prove coverage gives false assurance on M1 requirements.

### QA-F01

**Security policy key names differ across SPEC, TDD and mockup 10; an unknown key only warns, so a policy written from the SPEC is silently ignored** — minor, modified

**Where:** SPEC.md 7.10 DBG-07 line 369; SPEC.md 7.18 SEC-01 line 484; SPEC.md 7.18 SEC-02 line 485; SPEC.md 8.5 SEC-NF-01 line 600; TDD.md 11.2.2 lines 6696, 6702, 6737; TDD.md 11.2.3 line 6763; mockups/10-settings-and-trust.html lines 248-257 (images/10)

**Problem.** The SPEC tells administrators to forbid the debug server with `AllowDebugServer=false` and to lock folder trust with `AllowUserTrust=false`. The TDD schema keys are `debug.allow_debug_server` and `trust.allow_user_trust`, and mockup 10 shows a third spelling, `debug.allowDebugpy: false` (and `run-window` where the schema value is `run_window`). TDD 11.2.3 treats an unknown key as one warning, and both keys default to true. A studio that writes the SPEC spelling into the machine Policy.json gets a warning and an allowed, unauthenticated debugpy listener (U15): the control fails open.

**Evidence.** SPEC.md:369 "`AllowDebugServer=false` forbids it"; SPEC.md:485 acceptance "machine `AllowDebugServer=false` beats project `true`"; SPEC.md:484 "lock folder trust (`AllowUserTrust=false`)"; TDD.md:6702 "`debug.allow_debug_server` (SPEC `AllowDebugServer`) | bool | true (U15)"; TDD.md:6737 "SPEC names map to keys as follows" (a mapping note, not a parser alias); TDD.md:6763 "Unknown key: one warning."; mockups/10-settings-and-trust.html:257 "debug.allowDebugpy: false", :249 "run-window". TDD 17.2 A7 (line 8417) already records four earlier spellings.

**Fix (as modified by the refuter).** SPEC DBG-07, SEC-01, SEC-02 (key list and acceptance), SEC-NF-01: write the schema keys `debug.allow_debug_server` and `trust.allow_user_trust`. You may keep the friendly name in parentheses. Mockup 10: change line 257 to `debug.allow_debug_server: false` and line 249 to `"run_window"`, then re-render images/10. TDD 11.2.3 Smaller problems: "Unknown key: one warning, shown in the policy banner, the doctor and the effective-value inspector. If the file's `schema` is not newer than the build's and the unknown key is under `trust`, `debug`, `python`, `bridge`, `mcp` or `telemetry`, it is reported as an error naming the nearest known key." Add a T-POL case: a machine file containing `AllowDebugServer` shows that error and suggests `debug.allow_debug_server`.

**Refuter.** The spellings do differ (SPEC.md:369/484/485/600 AllowDebugServer and AllowUserTrust; TDD.md:6702 debug.allow_debug_server; mockups/10-settings-and-trust.html:257 debug.allowDebugpy; :249 "run-window"). But TDD.md:6737 and 17.2 A7 (line 8417) already map the SPEC names to the schema keys. Builders follow the TDD, and T-GAP-009 builds the user docs from the live setting list. So the fail-open path needs an administrator to copy key names from the SPEC or the mockup into Policy.json. That is a real, contained gap, not major. The proposed parser change also clashes with TDD 11.2.3's forward-compatibility rule ("A newer `schema` gets known keys applied and one warning"): treating an unknown key under a security namespace as an error that closes the namespace would shut every debug/trust key whenever a newer-schema file adds a new key.

### QA-F07

**The never-cut Stop-latency gate is measured from three different points; B-12 starts the clock after the input has waited for the next pump** — minor, modified

**Where:** TDD.md 13.2 B-12 line 7728; TDD.md 5.15 T-CAN-001 line 3037; SPEC.md 8.1 PERF-14 line 564; SPEC.md 4.3 line 109; research/08-decision-log.md 3 G2(1) line 100

**Problem.** G2(1) and PERF-14 define Stop latency "from click to exception". B-12, the CI budget row, measures from the "Pre-processor timestamp of the Stop input". During a run, OS input sits in the queue until the next pump's PumpMessages (10-20 Hz, so up to 100 ms at the 10 Hz floor), and the pre-processor sees it only then. B-12 therefore leaves out up to 40% of the 250 ms budget. T-CAN-001 measures "time from injection", which is the gate's definition, so the budget row and its own test disagree on a release gate and on the headline 99% metric.

**Evidence.** 08 3 G2(1): "Stop latency p99 ≤ 250 ms from click to exception"; SPEC.md:564 "Stop latency, click to exception"; TDD.md:7728 B-12 measuring point "Pre-processor timestamp of the Stop input → `ScriptCancelled` raised in the pending call"; TDD.md:3037 T-CAN-001 "Stop by an injected click; time from injection to the `ScriptCancelled` constructor"; TDD.md:2961 "Pump rate | 10–20 Hz, target 15 Hz"; SPEC.md:273 the pre-processor "swallows input from the first pump".

**Fix (as modified by the refuter).** TDD 13.2 B-12 measuring point: "Injection time of the synthetic Stop click (input bot, §4.16.1) → `ScriptCancelled` constructed in the pending call; the pre-processor timestamp is recorded as a sub-interval." Add to T-CAN-001: "the injection-to-pre-processor interval is reported separately."

**Refuter.** The inconsistency is real. B-12 (TDD.md:7728) measures from the "Pre-processor timestamp of the Stop input", while G2(1), PERF-14 and B-12's own title say click to exception, and T-CAN-001 (TDD.md:3037) measures from injection. During a run the OS input waits for the next pump's PumpMessages, so the B-12 interval leaves out up to one pump period. It cannot make the gate pass falsely, though. T-CAN-001 is in the G2 suite (TDD.md:7871) with "p99 ≤ 250 ms" from injection, and B-12 lists T-CAN-001 as its test, so a release cannot ship on the narrower number. It is a contained labelling and instrument gap, not major.

### QA-F08

**Automated gate suites contain manual tier-8 tests, so nightly and every-build gate status, and the 28-night docked-default rule, are undefined** — minor, modified

**Where:** TDD.md 14.3 G1 and G2 rows lines 7870-7871; TDD.md 5.15 T-SUP-031 line 3122; TDD.md 4.10 T-IME-006 line 1751; TDD.md 13.4 lines 7771-7772; TDD.md 14.1 tier 8 line 7798; TDD.md 5.15 T-SUP-010 line 3050; TDD.md 1.5 line 261

**Problem.** 1.5 says any test whose Covers cell names a gate is in that gate's suite, so the generated G2 suite includes T-SUP-031 (tier 8, manual IME and drag-drop during runs) and the G1 suite includes T-IME-006 (tier 8, manual checklist). Tier 8 runs "each release", but G2 runs "every build from Phase 0" and G1 runs nightly. T-SUP-010 blocks the docked-default change unless "the extended G2 has been green for the last 28 nightly runs", which no night can satisfy while a manual test is in the suite. CI cannot compute the gate it must enforce.

**Evidence.** TDD.md:3122 "| T-SUP-031 | 8 | Manual: IME composition, typing and drag-drop during runs ... | G2(5), D23 |"; TDD.md:7871 G2 suite "... T-SUP-031, T-CAN-011 ..."; TDD.md:7870 G1 suite "... T-IME-006 ..."; TDD.md:7772 G2 "GPU agent; every build from Phase 0; soak nightly"; TDD.md:7798 tier 8 "Each release"; TDD.md:3050 T-SUP-010 "green for the last 28 nightly runs"; TDD.md:261 "A test that covers a gate criterion is part of that gate's suite".

**Fix (as modified by the refuter).** As proposed. TDD 1.5 and 14.3: "Tier-8 tests that name a gate form the gate's manual checklist, run per release candidate; the automated status that CI, T-SUP-010 and T-DBG-028 read excludes them." Split 14.3 into Automated suite and Manual checklist columns, and make T-GAP-008 enforce the split.

**Refuter.** The cited text is accurate. 1.5 (TDD.md:261) puts any test naming a gate into that gate's suite. G2's suite (TDD.md:7871) includes the tier-8 manual T-SUP-031, and G1's includes T-IME-006. Tier 8 runs each release (TDD.md:7798), while G2 runs every build and T-SUP-010 needs 28 green nights. The consequence is a definitional ambiguity a builder resolves on first contact, since CI cannot execute a manual test. 08's G1(3) already pairs automation with "the manual checklist" (TDD.md:7771). No wrong product is built and no gate becomes unrunnable, so this is minor.

### QA-F10

**The TDD narrows RT-17's 10 s deferred-start trigger to "if a Sumi tab is open", so Python (Sumi) console completions never work until the first executed line** — minor, modified

**Where:** TDD.md 3.4 lines 682 and 691; TDD.md 6.6 completions step 5; research/08-decision-log.md RT-17 line 1588; SPEC.md ENV-06 line 462, A5 line 778, UEI-08 line 447

**Problem.** RT-17 and ENV-06 start Python work at first Sumi focus or keystroke, or 10 s after `OnFEngineLoopInitComplete`, whichever comes first. TDD 3.4 adds "if a Sumi tab is open", and its first-use list (run request, console Exec, API call) leaves out console completion requests. The executor's completions come only from a cache that an idle Python helper fills, which needs `sumi_editor` booted. With the tab closed, typing `unreal.EditorAss` in the Output Log's Python (Sumi) box returns nothing until something is executed, which breaks UEI-08's acceptance. The rule also departs from an 08 section 7 amendment without recording it as an open item.

**Evidence.** TDD.md:682 "else 10 s after OnFEngineLoopInitComplete with a Sumi tab open"; TDD.md:691 "10 s after `OnFEngineLoopInitComplete` if a Sumi tab is open; first use (run request, console `Exec`, API call)"; 08 RT-17 "or 10 s after `OnFEngineLoopInitComplete`, whichever comes first"; SPEC.md:447 UEI-08 acceptance "`unreal.EditorAss` suggests `EditorAssetLibrary`"; TDD 6.6 "an idle helper fills the cache".

**Fix (as modified by the refuter).** TDD 3.4 deferred-start trigger: add "a live-completion request from FSumiConsoleExecutor (§6.6 step 2 miss)" to the first-use list, and in §6.6 step 2 state that the miss triggers RequestStart when state is Cold. Add a 17.1 item recording the tab-open condition as an RT-17 refinement. T-REPL-013: add a case "Sumi tab closed and Python Cold: the second keystroke after `unreal.EditorAss` has results".

**Refuter.** The trigger list does omit completion. TDD.md:691 lists the first-use triggers "run request, console `Exec`, API call". The executor's miss path (TDD.md:3735) calls RequestLiveCompletion, which the D6 Python resolver serves, and prewarm (TDD.md:3741, step 5) needs an idle Python helper. So with the Sumi tab closed, Output Log completions return nothing until the first Exec. The "if a Sumi tab is open" condition (TDD.md:682/691) is a defensible reading of RT-17, whose context is the restored tab (08 line 1588; ENV-06 acceptance "A restored, never-focused tab"), so it is not clearly a departure that needs a CA ruling.

### QA-F11

**The traceability generator splits cells on escaped pipes; 14.7 shows SNP-02's Lands as "a,b\", and T-GAP-008 reuses the same parser** — minor, confirmed

**Where:** TDD.md 14.7 line 8077; SPEC.md SNP-02 line 405; TDD.md 14.6 T-GAP-008 line 7923

**Problem.** SPEC SNP-02 contains `${1\|a,b\|}` inside its Requirement cell. The assembly script split on `\|`, so the generated row reads "SNP-02 | a,b\". 14.7 says "T-GAP-008 runs the same check in CI", so CI will misread any escaped pipe and could take a wrong Lands value as Later or 1.0.

**Evidence.** TDD.md:8077 "| SNP-02 | a,b\ | T-CORE-032, T-CORE-035, T-SNP-001, T-E2E-002 |"; SPEC.md:405 "`${1\|a,b\|}` ... | M4 |"; TDD.md:7939 "Generated by the assembly script ... T-GAP-008 runs the same check in CI". My parser, which splits only on unescaped pipes, gets M4.

**Fix.** As proposed. Split rows on `(?<!\\)\|` in the assembly script and T-GAP-008, regenerate 14.7 (SNP-02 Lands M4), and add an escaped-pipe fixture row to T-GAP-008's pass criterion.

**Refuter.** Verified. TDD.md:8077 reads "| SNP-02 | a,b\ | ..." while SPEC.md:405's Lands cell is M4, and the escaped `${1\|a,b\|}` was split as a cell boundary. 14.7 says T-GAP-008 runs the same check.

### QA-F12

**Startup and restore budgets are measured in incompatible ways** — minor, confirmed

**Where:** TDD.md 3.4 line 699; TDD.md 3.9 T-LIFE-001 line 889; TDD.md 13.2 B-01 line 7717 and B-18 line 7734; TDD.md 13.3 line 7760; SPEC.md PERF-01 line 551, PERF-02 line 552, PER-02 line 425; TDD.md 10.3 lines 6259-6260

**Problem.** B-01 and 13.3 use "median of 5 cold starts", while T-LIFE-001, the test that enforces it, uses "p95 over 20 boots". PERF-02 (20 tabs ≤ 300 ms) is "restore start → last tab strip paint" in B-18 and T-PER-021, but the 3.4 table states it as "Remaining 19 tabs ≤ 300 ms in total, on activation". SPEC PERF-01 says no "journal scan before the deferred start", but PER-02 and TDD 3.4 and 10.3 open the journal and queue the recovery scan at layout restore. Three teams could implement three different gates.

**Evidence.** TDD.md:889 "B1 ≤ 10 ms; B1 + B2 ≤ 30 ms; p95 over 20 boots"; TDD.md:7717 "median of 5 cold starts per variant"; TDD.md:699 "| Remaining 19 tabs | ≤ 300 ms in total, on activation | PERF-02 |"; TDD.md:7734 "Restore start → last tab strip paint"; SPEC.md:551 "no Python, child process or journal scan before the deferred start"; TDD.md:6259 "FSumiJournal Open: root, locks, writer, recovery scan queued".

**Fix.** As proposed. T-LIFE-001: "median of 5 cold starts per variant (B-01)". TDD 3.4 PERF-02 row: "20 tabs: restore start → last tab-strip paint ≤ 300 ms (B-18); other tabs load on activation". SPEC PERF-01: "no Python or child process before the deferred start; journal I/O only on the writer thread".

**Refuter.** Verified. T-LIFE-001 (TDD.md:889) uses p95 over 20 boots, while B-01 (TDD.md:7717) uses the median of 5 cold starts. The 3.4 PERF-02 row (TDD.md:699) reads "Remaining 19 tabs ≤ 300 ms in total, on activation", while B-18 (TDD.md:7734) measures restore start → last tab strip paint. SPEC PERF-01 (SPEC.md:551) forbids a journal scan before the deferred start, while SPEC PER-02, TDD 3.4 (FSumiJournal::Open at layout restore) and TDD 10.3 (TDD.md:6259-6260, recovery scan queued and the writer reading namespaces) do one.

### QA-F13

**Snippet insertion: 11.5.3 opens the parameter form on insert, while 4.7, OI-43 and SPEC SNP-02 say no form appears on insert** — minor, confirmed

**Where:** TDD.md 11.5.3 line 6981; TDD.md 4.7 line 1387; TDD.md 17.1 OI-43 line 8351; SPEC.md SNP-02 line 405

**Problem.** The two TDD sections that own snippet behaviour give opposite rules for inserting a parameterised item into a buffer, and OI-43 asks 11 to confirm the 4.7 rule that 11 contradicts.

**Evidence.** TDD.md:6981 "Insert mode: VS Code semantics. ... Parameters open the form first."; TDD 4.7 "Each `${param:name}` becomes a mirrored group, initialised with the literal for its default ... No form appears on insert (proposed; open item)."; SPEC.md:405 "Inserted into a buffer, each becomes a tab stop holding its default literal; run, it is filled from the form".

**Fix.** As proposed. TDD 11.5.3 Insert mode: "Parameters become mirrored tab stops holding their default literals; no form appears (§4.7, SNP-02)." Close OI-43 as decided by SNP-02.

**Refuter.** Verified. TDD.md:6981 says "Parameters open the form first." TDD 4.7 (line 1387) says "No form appears on insert." SPEC SNP-02 (SPEC.md:405) settles it: inserting gives a tab stop holding the default literal.

### QA-F14

**The Charcoal keymap table in 4.9.4 disagrees with SPEC on milestones and leaves out the NAV-03 chords** — minor, confirmed

**Where:** TDD.md 4.9.4 lines 1632, 1649; SPEC.md INT-09 line 334, EDT-04 line 220, NAV-03 line 350, 9.3 line 696

**Problem.** QuickFix (Ctrl+.) lands M2 in the TDD, but SPEC INT-09 lands the SUM102 quick fix and `# noqa` in M1, with acceptance "Ctrl+. on SUM102 applies the rewrite". MoveLineUp/Down and DeleteLine land M1 in the TDD, but EDT-04 puts move and delete in M2. SPEC's Charcoal Go to file and symbol chords (Ctrl+Shift+N and Ctrl+Shift+O) have no command row, so the consume extension swallows them as unbound Ctrl+Shift letters.

**Evidence.** TDD.md:1632 "| QuickDocs; QuickFix; FormatDocument (U22) | Ctrl+Q; Ctrl+.; Shift+Alt+F | M1; M2; M2 |"; SPEC.md:334 INT-09 Lands "M1 (SUM102, noqa); M2 (rest)"; TDD.md:1649 "MoveLineUp / Down; DeleteLine | ... | M1"; SPEC.md:220 EDT-04 "M1 (comment, duplicate); M2 (rest)"; SPEC.md:696 "Go to file / symbol | Ctrl+Shift+N / Ctrl+Shift+O (proposed)"; grep finds no Ctrl+Shift+N in TDD.md.

**Fix.** As proposed. TDD 4.9.4: QuickFix Lands "M1 (SUM102, noqa); M2 (rest)". Add the row "GoToFile / GoToSymbol | Ctrl+Shift+N / Ctrl+Shift+O (proposed) | M2 | —". Either align MoveLine and DeleteLine to M2 or change EDT-04.

**Refuter.** Verified. TDD.md:1632 lands QuickFix in M2, but INT-09 (SPEC.md:334) lands the SUM102 fix and noqa in M1, and its acceptance uses Ctrl+.. TDD.md:1649 lands MoveLine and DeleteLine in M1, while EDT-04 has them in M2; that mismatch is harmless because it is earlier. There is no Ctrl+Shift+N or Ctrl+Shift+O row (grep finds none in TDD.md), so a builder working from this table never binds NAV-03's Go to file and symbol chords, and the extension (TDD.md:1661) swallows them unbound.

### QA-F15

**INT-07's 400 ms diagnostics debounce appears nowhere in the TDD, which uses 300 ms** — minor, confirmed

**Where:** SPEC.md INT-07 line 332; TDD.md 7.3.3 line 4212

**Problem.** The SPEC fixes a 400 ms debounce. The TDD analyses 300 ms after the last change and has no 400 ms value for any server, so the tested timing differs from the specified one.

**Evidence.** SPEC.md:332 "**Diagnostics as you type** (400 ms debounce)"; TDD.md:4212 "A document is analysed 300 ms after its last change (*proposed*, to stay inside PERF-08's 1 s)"; grep finds no "400 ms" debounce in TDD 7.

**Fix.** As proposed. Make one value normative, either SPEC INT-07 "(300 ms debounce, TDD §7.3.3)" or the TDD at 400 ms. State in TDD §7.3 whether the type server and ruff get a client-side didChange debounce.

**Refuter.** Verified. SPEC.md:332 says "(400 ms debounce)", which comes from research 05 line 429. TDD.md:4212 says sumi-check analyses 300 ms after the last change, and TDD.md:4075 sends one didChange per frame with no client debounce. No 400 ms debounce appears in TDD 7.

### QA-F16

**7.1.5 says language servers never open "plain-mode buffers (files over 5 MB)", but Plain mode is only for files below 1 MB** — minor, confirmed

**Where:** TDD.md 7.1.5 line 4012; SPEC.md BUF-09 line 211, EDT-15 line 231

**Problem.** Plain mode is the per-buffer IME fallback for files below 1 MB. Read literally, 7.1.5 removes diagnostics and static completion from every Plain-mode buffer, which means the CJK users who fall back to it, and says nothing about large normal-mode files.

**Evidence.** TDD.md:4012 "for plain-mode buffers (files over 5 MB, BUF-09). Those buffers are never opened in any server."; SPEC.md:211 "Plain mode only below 1 MB"; SPEC.md:231 EDT-15 "for files below 1 MB; the fallback if G1's IME criterion fails".

**Fix.** As proposed. TDD 7.1.5: "for files over 5 MB (BUF-09, matching SUM902). Plain-mode buffers are opened in servers like any other buffer."

**Refuter.** Verified. TDD.md:4012 says "plain-mode buffers (files over 5 MB, BUF-09)", while Plain mode exists only below 1 MB (SPEC.md:211, 231; TDD.md:1828, 1903). Read literally, it strips language services from the IME-fallback users.

### QA-F17

**PER-03's read-only "Not restored yet" pending state has no TDD design or test; 10.7.1 describes an editable disk-text buffer instead** — minor, confirmed

**Where:** SPEC.md PER-03 line 426; TDD.md 10.7.1 line 6367

**Problem.** The SPEC keeps recovered tabs read-only under "Not restored yet" until the user chooses. The TDD shows the disk or clean text with a "Recovered changes available" bar and never says the buffer is read-only. No test checks either behaviour. A user can then edit and save the disk version while the recovery is pending.

**Evidence.** SPEC.md:426 "Recovered tabs stay read-only under 'Not restored yet' until the user chooses."; TDD.md:6367 "Until the user acts, a counted file buffer shows its disk text and a counted scratch buffer its clean text, each with the bar 'Recovered changes available ...'"; "Not restored yet" does not appear in TDD.md.

**Fix.** As proposed. TDD 10.7.1 Pending: add "and the buffer is read-only with the label 'Not restored yet' (PER-03) until Restore, Compare-and-keep or Discard". Add a T-PER test: typing into a pending buffer is refused and Ctrl+S is a no-op. If editing is intended, change SPEC PER-03 instead.

**Refuter.** Verified. SPEC.md:426 says "Recovered tabs stay read-only under 'Not restored yet' until the user chooses." TDD.md:6367 shows the disk or clean text with a "Recovered changes available" bar and never makes it read-only. "Not restored yet" does not appear in TDD.md, and no test covers the pending state.

### QA-F18

**Stale v0.9 references and name drift that mislead readers** — minor, confirmed

**Where:** TDD.md 10.2.5 lines 6197-6198; TDD.md 12.5 open item 1 line 7678; TDD.md 12.4 lines 7634-7635; TDD.md 9.5 line 5906; SPEC.md ENV-04 line 460

**Problem.** 10.2.5 still cites PER-01's "1–2 s" and "forced every 30 s", which SPEC v1.0 removed. 12.5 open item 1 says 12.4 proposes `%LOCALAPPDATA%\Sumi\Library\` and `trust.json`, but 12.4 and 11.5.1 now say `%APPDATA%\Sumi\Library\` and `Trust.json`. The doctor's palette command is "Sumi: Run Doctor" in the SPEC and "Sumi: Run environment doctor" in the TDD, and T-GAP-009 checks command names against the docs.

**Evidence.** TDD.md:6197 "the low end of PER-01's 1–2 s"; TDD.md:6198 "This replaces PER-01's 'forced every 30 s'" (SPEC.md:424 now reads "1.0 s after its last edit or 1.0 s after its oldest uncommitted edit"); TDD.md:7678 "§12.4 proposes engine-version-independent `%LOCALAPPDATA%\Sumi\Library\` and `trust.json`" vs TDD.md:7635 "user `%APPDATA%\Sumi\Library\`" and 7634 "`%LOCALAPPDATA%\Sumi\Trust.json`"; SPEC.md:460 "**Sumi: Run Doctor**" vs TDD.md:5906 "`Sumi: Run environment doctor`".

**Fix.** As proposed.

**Refuter.** Verified. TDD.md:6197-6198 cite PER-01's "1–2 s" and "forced every 30 s", but SPEC.md:424 now says 1.0 s / 1.0 s. TDD.md:7678 says 12.4 proposes `%LOCALAPPDATA%\Sumi\Library\` and `trust.json`, but 12.4 says user `%APPDATA%\Sumi\Library\` (line 7635) and `%LOCALAPPDATA%\Sumi\Trust.json` (line 7634). The palette name is "Sumi: Run Doctor" in SPEC.md:460 and "Sumi: Run environment doctor" in TDD.md:5906, and T-GAP-009 checks command names.

### QA-F19

**The G5 AltGr table does not test the French backslash that G5 and CFG-07 require** — minor, modified

**Where:** TDD.md 4.16.1 line 1927; SPEC.md CFG-07 line 506; research/08-decision-log.md 3 G5 line 103

**Problem.** G5 and CFG-07 require AltGr on German, French and Polish layouts to type `@ { [ \`. The TDD's French row sends AltGr+0, AltGr+4 and AltGr+5 and AltGr+7 (dead backtick) but not AltGr+8, the AZERTY backslash. On Polish Programmers `[` and `\` are unshifted keys, so the gate text cannot apply there as written.

**Evidence.** TDD.md:1927 "| French 0000040C | AltGr+0, AltGr+4, AltGr+5; AltGr+7 then Space | `@`, `{`, `[`; a backtick through the dead key |"; SPEC.md:506 "AltGr on German, French and Polish layouts types `@`, `{`, `[` and the backslash".

**Fix (as modified by the refuter).** TDD 4.16.1 French row: add AltGr+8 → `\`, and optionally AltGr+6 → `|`. Leave SPEC CFG-07 and 08 G5 unchanged.

**Refuter.** The TDD gap is real. The French row (TDD.md:1927) lacks AltGr+8 → `\`, which G5 (08 line 103) and CFG-07 require. The SPEC change is unnecessary: the Polish Programmers row already tests that layout's `@ { `` keys plus AltGr+Z/X. Rewording CFG-07 to point at a TDD table would also change gate text that comes from 08 §3.

### QA-F20

**No CI capacity plan for 430 tier-4 tests, two 8-hour soaks and every-build G2 on an uncounted GPU agent** — minor, confirmed

**Where:** TDD.md 14.1 lines 7794, 7800; TDD.md T-SUP-009 line 3049, T-OUT-017 line 3803, T-TASK-008 line 3177; TDD.md 13.3 line 7758

**Problem.** Tier 4 holds 430 of the 769 tests (counted from the tables), including two separate 8-hour soaks (T-SUP-009 and T-OUT-017), a 20-minute task soak, 1,000-cycle PIE runs, 2 × 20 cold-boot startup runs and ten journey tests. Absolute performance checks must run on "the reference agent". 14.1 sizes the G6 pool (8-10 agents) but not the GPU agents or a nightly wall-time budget, so the nightly and the docked-default history may not fit in one night.

**Evidence.** Tier tally from the test tables: tier 4 = 430, tier 1 = 127, tier 2 = 126, tier 3 = 33; TDD.md:3049 "an 8-hour run streaming"; TDD.md:3803 "8-hour soak, 10M mixed lines"; TDD.md:7800 "GPU agent (Windows 11, discrete GPU, a 3840×2160 desktop; the perf reference)" (no count) vs "8–10 recovery agents for G6".

**Fix.** As proposed. TDD 14.1 Agents: state the GPU-agent count and job placement, for example one perf reference and two functional/soak agents, with a nightly wall-time budget per agent. Merge T-SUP-009 and T-OUT-017 into one 8-hour soak that checks both criteria, or place them on separate agents.

**Refuter.** Verified. My own count of the test tables gives tier 4 = 430 (plus 2 mixed rows). There are two separate 8-hour soaks (T-SUP-009 at TDD.md:3049 and T-OUT-017 at TDD.md:3803), and T-TASK-008 adds a 20-minute task soak. 14.1 (TDD.md:7800) counts the 8-10 G6 agents but gives no GPU-agent count and no nightly wall-time budget.

### QA-F21

**SPEC D.2 traces gaps to a mockup report and a QA report that are not in the repository** — minor, confirmed

**Where:** SPEC.md D.2 line 925

**Problem.** Every row of D.2 cites gap IDs such as 02-1 and QA-4 from documents that do not exist under docs/, so the claim "Every gap in both reports is listed" cannot be checked.

**Evidence.** SPEC.md:925 "`02-1` is mockup report 02, item 1; `QA-n` is item n of the QA report's SPEC list. Every gap in both reports is listed."; docs/ and docs/research/ contain no such reports (research holds 01-06, 07a-07e, 08, 09 only).

**Fix.** As proposed. Add the two reports under docs/research/ and link them from D.2 and README, or drop the gap IDs and keep the ruling and requirement links.

**Refuter.** Verified. SPEC.md:923-925 cite "mockup report 02" and the "QA report's SPEC list". No such files exist anywhere under C:/Users/anima/Desktop/UnrealPythonEditor (find over the tree), so the claim "Every gap in both reports is listed" cannot be checked.

### QA-F22

**Definition of done item 2 (each new test fails at the merge base on the pull request) cannot run for nightly-only tiers** — minor, confirmed

**Where:** TDD.md 14.4 item 2 line 7882; TDD.md 14.1 tier 4 and 6 cadence lines 7794, 7796; SPEC.md MAINT-03 line 609

**Problem.** The pull-request check "runs new and changed tests against the merge base", but tier 4 (430 tests) runs nightly with only a 10-minute smoke subset per commit, and tier 6 needs the recovery pool. The DoD that MAINT-03 requires is not executable as written for most tests, and no test covers it.

**Evidence.** TDD.md:7882 "The pull-request check runs new and changed tests against the merge base and expects them to fail"; TDD.md:7794 tier 4 "Nightly ... A 10-minute smoke subset on every commit".

**Fix.** As proposed, plus: "For tiers 1-3 and 7 (fake server), a compile or import failure at the merge base counts as the expected failure."

**Refuter.** Verified. TDD.md:7882 has the PR check run new and changed tests against the merge base, while tier 4 runs nightly with only a smoke subset per commit (TDD.md:7794) and tier 6 needs the recovery pool. A further point the finding misses: new C++ or Python tests that reference new APIs will not even compile or import at the merge base, so "expects them to fail" needs a definition.

### QA-F23

**TDD.md (820 KB, 8,757 lines) should be split into a folder of per-section files with an index; trimming alone saves under 10 percent** — minor, refuted

**Where:** TDD.md (whole file); SPEC.md lines 6, 132, 149, 185, 547, 607, 609, 741, 789, 805, 813, 920; README.md line 10; research/08-decision-log.md line 5; TDD.md 1.5 line 262 and 14.7 line 7939 (traceability script input)

**Problem.** Measured: 820,529 bytes, 121,433 words; 414 KB of tables, 311 KB of prose, 55 KB of code, 21 KB of Mermaid. Repetition is low (0 duplicated lines over 80 characters, 3 repeated sentences, about 1 KB). Generated material is about 63 KB: Contents 10.2, 14.7 13.3, Appendix A 17.2 and Appendix B 22.7 KB. The size comes from content, so trimming cannot fix it. The largest sections are 5 (107 KB), 7 (94 KB), 11 (80 KB) and 4 (79 KB), and a single file this size is slow to review, diff and render.

**Evidence.** size.py and size2.py in the scratchpad: per-H2 sizes 1 Overview 10.0 KB, 2 29.6, 3 30.6, 4 78.9, 5 106.9, 6 59.5, 7 94.2, 8 55.9, 9 36.1, 10 43.7, 11 80.2, 12 23.5, 13 13.3, 14 38.0, 15 10.0, 16 10.8, 17 23.7, App A 17.2, App B 22.7, App C 3.5. Links into TDD.md: SPEC 15 (14 anchored, 12 distinct anchors), README 1, 08 1; 938 internal anchor links (170 distinct) inside TDD.md.

**Fix.** No change required. Optionally record the split plan as a MAINT proposal in TDD 17.1.

**Refuter.** The measurements are accurate: 820,529 bytes, 8,757 lines, 14 TDD.md# links from SPEC, README and 08. But this is a structural preference, not a defect. Nothing in the file misleads a reader or makes a requirement, test or gate wrong, and the brief excludes formatting and organisation nits that do not mislead. The finding itself says repetition is negligible, and its main cost argument (a GitHub render cutoff) was not verified. If the team wants the split, the plan is sound, but it should be logged as a maintenance proposal, not a review defect.
