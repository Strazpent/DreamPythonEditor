> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 09 — Red team: objections to the decision log (D1–D30)

| | |
|---|---|
| **Document** | Red-team review of [08-decision-log.md](08-decision-log.md) |
| **Date** | 2026-10-28 |
| **Engine** | Local UE 5.8.4 install (`Engine/Build/Build.version`), embedded CPython 3.11.8 |
| **Method** | Every engine claim challenged here was re-read in the local source. CPython behaviour was checked by running the engine's own `python.exe -I`. debugpy facts come from the debugpy 1.8.20 bundle on this machine (`%USERPROFILE%\.vscode\extensions\ms-python.debugpy-2026.6.0-win32-x64\bundled\libs\debugpy`) |
| **Result** | **1 blocker, 9 major, 10 minor.** Decisions not named below survived the attack. Claims that I re-checked and found accurate are not listed |

---

## 1. Verdict

The log is well evidenced, and most of its engine claims hold up when re-checked. The weak spot is the run model (D2, D8, D14, D24), which is also the product's headline feature.

- **The blocker (RT-01)** is easy to fix but must be fixed before the TDD locks D14. D14 makes "start every run from a one-shot FTSTicker callback" an invariant. The core ticker is not re-entrant, though, and several engine functions that Python can call tick it synchronously. Material baking is one, reached through `merge_static_mesh_actors`. Today those calls work from the Output Log. Under D14 they would crash the editor.
- **Four majors challenge D2's liveness design:**
  - RT-02: missing pump guards.
  - RT-03: the docked presentation is not the configuration the engine has proven.
  - RT-04: an unprotected input window in the Run-window presentation.
  - RT-06: re-delivery cuts into `finally` cleanup.
- **Two majors are about undo:**
  - RT-08: one transaction per run has no size bound and pins objects against GC.
  - RT-09: a `@task` transaction that stays open across frames.
- **The remaining majors:**
  - RT-05: Esc is taken away from script dialogs.
  - RT-07: `unreal.log` grows `LogOutput` without bound.
  - RT-10: Phase 0 is overloaded.

All of the majors can be fixed inside the agreed architecture. None of them reopens D1, D5, D7 or the no-CEF decision.

---

## 2. Objection index

| ID | Decision | Severity | One line |
|---|---|---|---|
| RT-01 | D14 (D2, D10, D24) | **Blocker** | Runs execute inside `FTSTicker::Tick`. Any engine call that ticks the core ticker (material baking, HLOD builds) then crashes the editor |
| RT-02 | D2 | Major | The pump fires inside Python that C++ called mid-operation. The engine's own unsafe-state guards (save, GC, PostLoad) are missing |
| RT-03 | D2 | Major | "Live (docked)" ticks and paints every window mid-script, which the engine never does. UMG/EUW ticks and Sync() tasks re-enter Python. G2 does not test this |
| RT-04 | D2 | Major | Run-window presentation: pumps start about 700 ms before the modal exists, and nothing protects input in that gap |
| RT-05 | D2, D8 | Major | A global Esc means Stop, which steals Esc (Cancel) from every dialog a script opens |
| RT-06 | D2 | Major | Re-delivery every 250 ms interrupts `finally` blocks, `__exit__` and our own runner cleanup. That contradicts "finally blocks still run" |
| RT-07 | D19 / G2(3) | Major | Users' own `unreal.log*` calls still grow `FPythonCommandEx::LogOutput` for the whole run. G2(3) and G2(7) cannot pass for idiomatic scripts |
| RT-08 | D8 | Major | Nothing bounds a run-wide transaction. It pins every modified object against GC, and the next edit can evict all undo history |
| RT-09 | D24 | Major | A `@task` transaction stays open across frames. PIE start cancels it, user edits merge into it, and Level Editor undo is blocked |
| RT-10 | D9 | Major | Phase 0 must settle G1–G4 in 6–8 weeks with 3–4 engineers, but G3 and G4 do not gate M1 |
| RT-11 | D3 | Minor | `debugpy.listen()` blocks the game thread (30 s timeout) and is one-shot. Its adapter escapes our process containment. It also vendors process-injection binaries |
| RT-12 | D4 | Minor | Two supported servers at 1.0 doubles the qualification work and ships Node |
| RT-13 | D4, D5, D9 | Minor | M1 `sumi-check` lints need `unreal` API data that only arrives with the M2 walker |
| RT-14 | D8 | Minor | Dedenting selections misplaces 3.11 traceback carets and breaks on triple-quoted strings |
| RT-15 | D14 | Minor | The entry command depends on the name `sumi_editor` in the user-mutable Session dict |
| RT-16 | D8 / G5 | Minor | Undo Last Run (Ctrl+Alt+Z) shares a chord with AltGr+Z ("ż" on Polish keyboards). G5 does not test it |
| RT-17 | D4, D7, D16 | Minor | "Never at editor start" contradicts layout restore of the tab |
| RT-18 | D20 | Minor | A "machine policy" that is located through a user environment variable is not machine policy |
| RT-19 | D12 | Minor | "We hold the copyright" is false under a DCO |
| RT-20 | D15, D9 | Minor | There is no budget for the next engine release (likely Python 3.13) inside the 1.0 window |

---

## 3. Objections

### RT-01 — Runs inside `FTSTicker::Tick` crash on engine paths that tick the core ticker (Blocker)

**Decision.** D14(1): every run starts from a one-shot FTSTicker callback. D2 §1 says the same. D24 steps `@task` generators from FTSTicker, and D10 queues bridge runs the same way.

**Objection.** The log knows that a nested `FTSTicker::Tick` is fatal: D1 cites `Ticker.cpp:101`, and 07b Q2(b) covers it. But it guards only against *our pump* ticking the ticker. Under D14, the whole user script runs inside `FTSTicker::Tick`. Ordinary engine APIs that Python can call tick the core ticker synchronously, and the ticker has no re-entrancy guard.

**Evidence.**
- **`FTSTicker::Tick` has no guard** (`Engine/Source/Runtime/Core/Private/Containers/Ticker.cpp:60-143`).
  - The outer loop holds `FElementPtr& Element = Elements[ElementIdx]` (`:99`) while firing. It moves fired and not-yet-due elements out with `MoveTemp` (`:117`, `:128`).
  - A nested `Tick` walks the same `Elements` array from index 0. It dereferences the moved-from (null) `TSharedPtr`s, or it reaches the executing element and fails `checkf(GetThreadId(PrevState) == 0)` (`:102`).
  - Its `PumpAddedElementsQueue` (`:81`) can also reallocate `Elements` underneath the outer reference.
- **Material baking is reachable from Python.**
  - `merge_static_mesh_actors` and `create_proxy_mesh_actor` are BlueprintCallable (`Engine/Source/Editor/StaticMeshEditor/Public/StaticMeshEditorSubsystem.h:628-629, 638-639`). They call `MergeComponentsToStaticMesh` and `CreateProxyMesh` (`Engine/Source/Editor/StaticMeshEditor/Private/StaticMeshEditorSubsystem.cpp:2506, 2609`).
  - With material merging on, that leads to `BakeMaterials` (`Engine/Source/Developer/MeshMergeUtilities/Private/MeshMergeUtilities.cpp:2233, 3122-3126`), then `CreateMaterialProxies` (`Engine/Source/Developer/MaterialBaking/Private/MaterialBakingModule.cpp:496, 669`).
  - `CreateMaterialProxies` calls `FTSTicker::GetCoreTicker().Tick()` in its texture-streaming wait loop (`:710-717`).
- **Other engine paths that tick the core ticker:**
  - HLOD proxy build: `Engine/Source/Editor/UnrealEd/Private/HierarchicalLOD.cpp:896-902`.
  - World Partition HLOD utilities: `Engine/Plugins/Editor/WorldPartitionHLODUtilities/Source/Private/WorldPartition/HLOD/Utilities/WorldPartitionHLODUtilities.cpp:879-885`.
  - Datasmith exporter: `Engine/Source/Developer/Datasmith/DatasmithExporter/Private/DatasmithExporterManager.cpp:182`.
  - A widget that does it in its own Tick, `SSourceControlSubmitWidget::Tick` (`Engine/Source/Editor/SourceControlWindows/Private/SSourceControlSubmit.cpp:795-801`). That is fatal if our pump draws that window.
- **Today's interactive paths do not run inside the ticker.**
  - Output Log commands, EUW buttons and menus are dispatched from `PumpMessages`, which runs outside both Slate Tick and FTSTicker (07b Q2(a); `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5783` against `:6103`).
  - Only `init_unreal.py` and Epic Remote Execution run inside the ticker (`Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:1376-1381, 1734-1743`; `.../PythonScriptRemoteExecution.cpp:577`).
  - D14 moves every Sumi run into the riskier context.

**Failure scenario.** A TA selects 40 static mesh actors and runs a script that calls `merge_static_mesh_actors` with material merging while textures are still streaming. The nested `FTSTicker::Tick` causes an access violation or a `checkf`, and the editor crashes. The journal restores the buffer, but the level edits are lost. The same script works from the Output Log.

**Suggested change.**
- **Start runs outside the ticker.** Use a context that is outside `FTSTicker::Tick`, outside the Slate tick and free of pointer capture: a one-shot `FCoreDelegates::OnEndFrame` handler (`LaunchEngineLoop.cpp:6127`). The editor broadcasts `OnEndFrame` only there, after the core ticker (`:6103`) and the deferred commands (`:6108`). `OnBeginFrame` (`:5682`) is an alternative. Both keep E10's properties: `IsTicking()` is false and no capture is held.
- **Apply the same context everywhere runs start:** `@task` steps (D24) and bridge dispatch (D10).
- **Add G2 cases:**
  - `merge_static_mesh_actors` with material merging on;
  - `create_proxy_mesh_actor`;
  - a fixture UFUNCTION that ticks the core ticker.
- **Restate the rationale.** Once runs start outside the ticker, a user call that ticks it will dispatch Remote Execution, MCP and other ticker clients in a nested way. So "refused while a run is active" (D11, D14) must be enforced inside our handlers, and D2's sentence "Nothing can re-enter a script" must be dropped.

---

### RT-02 — The pump guard misses the engine's unsafe states and nested native re-entry (Major)

**Decision.** D2 §1: the pump does nothing while `IsTicking()` is true, while paused, or when re-entered.

**Objection.** The pending-call pump fires at *any* bytecode boundary on the game thread. That includes Python which C++ invoked in the middle of an engine operation: delegates bound with `add_callable`, and Python UFUNCTION overrides. The engine's own rule for calling Python from a tick is stricter than D2's: it also refuses while a package is saving, during GC and while PostLoad is being routed. The engine precedents D2 cites tick Slate only at points the engine author chose. They never tick at an arbitrary bytecode inside a nested callback.

**Evidence.**
- **The engine's own guard.** `CanTickPython` is in `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyUtil.cpp:1822-1838`. It checks `IsSavingPackage`, `IsGarbageCollecting` and `IsRoutingPostLoad`.
- **Python run from native code:**
  - UFUNCTION overrides (`.../Private/PyWrapperObject.cpp:2035`);
  - delegates (`.../Private/PyWrapperDelegate.cpp:21-37`; 07a risk 7).
- **Python-bindable events that fire mid-operation:**
  - `ImportSubsystem` OnAssetPreImport and OnAssetPostImport (`Engine/Source/Editor/UnrealEd/Public/Subsystems/ImportSubsystem.h:87-92`);
  - `EditorActorSubsystem` OnDeleteActorsBegin (`Engine/Source/Editor/UnrealEd/Public/Subsystems/EditorActorSubsystem.h:89`).
- **Precedent pumps tick only at chosen points:** `Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp:419-439` and `Engine/Source/Developer/SourceControl/Private/ScopedSourceControlProgress.cpp:234-254`.

**Failure scenario.**
1. A studio's Python `on_asset_post_import` hook normalises texture settings.
2. A TA's script imports 300 textures through `import_asset_tasks`.
3. The watchdog fires inside the hook. Our pump ticks Slate (in docked mode, every window) from inside the AssetTools import call stack while the package is only partly built.
4. If Stop is pressed at that moment, `ScriptCancelled` is raised inside the studio's hook. The C++ delegate boundary swallows it and logs it as an error, and the user's script keeps going until a later re-delivery.

**Suggested change.**
1. **Mirror `CanTickPython`.** Skip the pump while `UE::IsSavingPackage()` (`Engine/Source/Runtime/CoreUObject/Public/UObject/UObjectGlobals.h:246`), `IsGarbageCollecting()` or `FUObjectThreadContext::Get().IsRoutingPostLoad` is true.
2. **Skip on native re-entry.** Skip the pump, and defer delivery, when Python has been re-entered from native code since `_run` began.
   - On 3.11 and 3.12, compare `PyThreadState_Get()->cframe` with the value captured at `_run` entry. The struct is in the shipped header `Engine/Source/ThirdParty/Python3/Win64/include/cpython/pystate.h:38-53, 110`. It is internal, so the check sits behind the D15 compatibility layer.
   - 3.13 needs its own entry-frame check.
   - A skipped pump simply re-arms.
3. **Add G2(2) cases:**
   - a Python `on_asset_post_import` hook that takes 1 s per asset during `import_asset_tasks`;
   - an `on_delete_actors_begin` hook;
   - a Python UFUNCTION override called from C++.

---

### RT-03 — "Live (docked)" is not the engine-proven configuration, and G2 does not test the difference (Major)

**Decision.** D2 §2 makes Live (docked) the provisional default. The rationale calls the pump "the engine's own public mid-frame recipe" and says "Nothing can re-enter a script".

**Objection.** Both precedents tick Slate only while *their own modal window* is the active modal. While a modal is active, Slate runs prepass and paint only on the modal tree (plus tooltips and notifications), so widgets in other windows do not tick at all. Docked mode has no modal. Every pump runs prepass and paint on every window, and widget ticks run arbitrary game code mid-script.

D2 lists four protections the modal gets from the platform and builds docked equivalents for three of them. The fourth, draw isolation, has no equivalent, and draw isolation is also tick isolation.

**Evidence.**
- **The precedents tick only under their own modal.**
  - `FeedbackContextEditor.cpp:419-423`: "We only tick slate if we are the active modal window or a child of the active modal window".
  - Source-control progress adds its window with `AddModalWindow(..., true)` (`ScopedSourceControlProgress.cpp:215`) before ticking (`:234-254`).
- **With a modal active, only the modal is drawn and ticked:** prepass in `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:1379-1405`, paint at `:1476-1497`.
- **UMG ticks run game code during paint.** `SObjectWidget::Tick` calls `UUserWidget::NativeTick`, which runs latent actions and the Blueprint Event Tick (`Engine/Source/Runtime/UMG/Private/Slate/SObjectWidget.cpp:114-129`; `Engine/Source/Runtime/UMG/Private/UserWidget.cpp:2101-2140`).
  - An EUW's Event Tick can call Execute Python Command or a Python override. That is nested, ungated Python: `CanTickPython` is checked only at `.../Private/PyCore.cpp:45` and `.../Private/PySlate.cpp:31`.
- **Foreign game-thread tasks run in both presentations.** `Renderer->Sync()` runs `ProcessThreadUntilIdle(GameThread)` (`Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp:1845-1848`; `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp:2562-2566`). 07b Q7 already lists these hazards, so "Nothing can re-enter a script" is false.
- **Window-frame actions bypass the input pre-processor.** Title-bar drag, minimise, maximise and close go through `OnWindowAction`, not the pre-processor (`Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:3057-3095`; `SlateApplication.cpp:7594-7615`). A title-bar drag enters the Win32 move loop inside our `PumpMessages` call.
- **Async delivery can hit the wrong code.** `SetAsyncExc` targets the thread. While the pump has released the GIL, foreign Python nested on the game thread (an EUW tick, a Sync task) receives `ScriptCancelled` instead of the user's script.

**Failure scenario.**
1. A docked EUW dashboard is open, with an Event Tick that calls a Python refresh function.
2. A TD runs a 2-minute batch. On every pump, the EUW's Python runs on top of the suspended batch frame and changes module globals that the batch uses.
3. At Stop, `ScriptCancelled` lands in the EUW tick, and the batch keeps running.

**Suggested change.**
- **Preferred:** make the Run window the provisional default (the QA lead's dissent) until G2 proves docked mode against these hazard classes.
- **If docked mode is kept:**
  - (a) Add G2 cases:
    - an open EUW whose Blueprint Tick calls Python;
    - a Python-subclassed EUW that overrides tick;
    - a queued `AsyncTask(GameThread)` that fires a Python-bound delegate;
    - title-bar drag, minimise and maximise during a run.
  - (b) Register an `OnWindowAction` veto for the duration of the run (`FSlateApplication::RegisterOnWindowActionNotification`, `Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:1841`).
  - (c) Never call `SetAsyncExc` while the pump depth is above 0.
  - (d) When our tab floats in its own window, consider `ForceRedrawWindow` on that window (`SlateApplication.h:1066`) instead of `Tick(All)`.

---

### RT-04 — Run-window presentation: about 700 ms of pumping with no input protection (Major)

**Decision.** D2 §1–2: the watchdog arms at about 300 ms and pumps at 10–20 Hz. The Run window is "opened after about 1 s". The swallow-all `IInputProcessor` is described only under Live (docked). In Run-window mode only Esc is intercepted.

**Objection.** Between arming and the modal opening, no modal is active. The pump therefore calls `PumpMessages()` and `Tick(All)`, and OS input reaches the whole editor. Safe mode (D17) and a failed G2 both force this presentation, so it has to be the safe one.

**Evidence.**
- D2 §1–2 (timings above).
- `FSlateApplication::TickPlatform` pumps OS messages itself only "when a modal window … is up" (`SlateApplication.cpp:1689-1712`). Without a modal, the PumpMessages call in our pump dispatches input normally.

**Failure scenario.** A user starts a 5-second script, clicks the viewport and presses Delete at 0.5 s. The selected actors are deleted mid-script, and the deletion is recorded inside the run's transaction.

**Suggested change.**
- Open the Run window at arm time, on the first pump. Alternatively, install the swallow-all pre-processor from the first pump in both presentations.
- Run the G2(5) input-leak suite inside the window between arming and the modal opening.

---

### RT-05 — A global "Esc = Stop" steals Cancel from script-opened dialogs (Major)

**Decision.** D2 says: "With a user's modal or slow task already open … Only keyboard pre-processing reaches us, so Esc stops the run". The D8 keymap makes Esc a global Stop "in both presentations" and calls swallowing it "harmless".

**Objection.** Esc is the standard way to cancel a dialog, and UE's script-facing dialogs bind it. Input pre-processors run before any widget routing, including routing to the active modal. So while a run is active, Esc can never reach a dialog the script opened, and it turns into a Stop of the whole script.

**Evidence.**
- `unreal.EditorDialog.show_message` → `FMessageDialog::Open` (`Engine/Plugins/Editor/EditorScriptingUtilities/Source/EditorScriptingUtilities/Private/EditorDialogLibrary.cpp:21-23`).
- `SChoiceDialog::OnKeyDown` maps Escape to `EAppReturnType::Cancel` (`Engine/Source/Editor/UnrealEd/Private/Dialogs/Dialogs.cpp:253-258`). The modal dialog does the same (`:770-775`).
- Pre-processors get the event first: `ProcessKeyDownEvent` calls `InputPreProcessors.HandleKeyDownEvent` before any routing (`SlateApplication.cpp:4961-4985`).

**Failure scenario.**
1. A shelf tool asks "Overwrite 300 assets? Yes / No / Cancel", and the artist presses Esc to cancel.
2. The dialog stays open, and a `ScriptCancelled` is queued.
3. The artist clicks No. The script receives `ScriptCancelled` as soon as the dialog returns, part-way through its own branching.

**Suggested change.**
- The run pre-processor passes Esc through whenever the active modal window is not one of ours (`GetActiveModalWindow()` is not the Run or Paused window) or a menu is open.
- Shift+F5, the banner and the Run window's Stop button remain the run-level Stop.
- Add "Esc in `show_message` during a run returns Cancel" to G5.

---

### RT-06 — Re-delivery interrupts `finally`, `__exit__` and the runner's own cleanup (Major)

**Decision.** D2 §3:
- re-deliver every 250 ms;
- from 1 s, also deliver through `PyThreadState_SetAsyncExc` on each period;
- "`finally` blocks and context managers still run".

**Objection.** The re-delivery rule cannot tell "the user swallowed the exception and kept looping" from "the user is legitimately cleaning up in `except`, `finally` or `__exit__`". Any cleanup longer than 250 ms is itself interrupted. So is the runner's own `except BaseException` and `finally`, which restore streams, `__file__` and `sys.path`, until the run ID is invalidated in C++ after `ExecPythonCommandEx` returns. A re-raise in our handler escapes `_run`, which defeats the "EvalString always succeeds" invariant in D14(3).

**Evidence.**
- D2 §3 text.
- On the engine's 3.11.8 `python.exe`, during `finally` and `__exit__` on the exception path the handled exception is visible, and after a swallowing bare `except:` it is cleared:
  - in `__exit__`, the handled exception is `C`;
  - in `finally`, `exc_info` is `C`;
  - after a swallowing bare `except:`, `exc_info` is `(None, None, None)`.
- `PyErr_GetHandledException` ships in `Engine/Source/ThirdParty/Python3/Win64/include/pyerrors.h:22`. The tstate variant is in `.../include/cpython/pyerrors.h:94`.

**Failure scenario.** A user stops a batch whose `finally:` saves 50 dirty packages, which takes 3 s. At 250 ms a re-delivered `ScriptCancelled` interrupts the save loop. Half the packages are saved and the rest are left dirty. Or the exception lands while `_run` is restoring `sys.stdout`: the run ends with an engine traceback, and the stream still points at our writer.

**Suggested change.**
- **Check before re-delivering.** In the pending call, which holds the GIL on the game thread, check `PyErr_GetHandledException()`.
  - If it is our `ScriptCancelled` instance, the stop is being handled, so do not re-deliver.
  - Re-deliver only after it has been cleared, which means it was swallowed.
- **Gate the alternate channel.** The watchdog uses `SetAsyncExc` only when the last pending call recorded "not handling".
- **Turn long cleanup into a choice.** If cleanup lasts longer than N seconds (default 10), show an explicit "Interrupt cleanup" choice instead of re-raising automatically.
- **Stop delivery before our cleanup.** `_run` calls `_sumi.user_code_done(run_id)` as the first statement of its handler, so that all delivery stops before our own cleanup.

---

### RT-07 — Users' `unreal.log*` calls grow `LogOutput` without bound, so G2(3) and G2(7) cannot pass for idiomatic scripts (Major)

**Decision.** E3, D19, and G2(3) ("`LogOutput` stays bounded").

**Objection.** E3 is correct, and D19 does avoid `unreal.log` for our own forwarding. But the capture lambda records *every* `unreal.log`, `unreal.log_warning` and `unreal.log_error` call made during the run, the user's own included. `unreal.log` is the idiom Epic documents and most TD scripts use. For any script that logs this way, memory grows for the whole run and is released only when `ExecPythonCommandEx` returns.

**Evidence.**
- The lambda is attached for the whole `EvalString` (`.../Private/PythonScriptPlugin.cpp:1865-1867`).
- `Log`, `LogWarning` and `LogError` broadcast every line that is not blank (`.../Private/PyCore.cpp:1176-1227`).
- These functions are hoisted into the `unreal` module dict, so the dict entries can be rebound (`.../Private/PythonScriptPlugin.cpp:1789-1806`).

**Failure scenario.** A 6-hour asset audit logs 1,000 `unreal.log` lines per second. That is about 21.6 M `FPythonLogOutputEntry` records (each an `FString` plus a type), held in memory until the run returns: several GB. The G2(7) soak fails as soon as its workload uses `unreal.log`.

**Suggested change.**
- For the duration of a run, rebind `unreal.log`, `unreal.log_warning` and `unreal.log_error` in the `unreal` module dict to `_sumi` shims.
  - The shims keep `ShouldLog` semantics and emit through our C++ batch path at the matching verbosity.
  - Compare-and-restore the originals at run end (the D25 pattern).
- Document that `from unreal import log`, bound before the run, bypasses the shim.
- Add an `unreal.log`-heavy loop to G2(3) and G2(7).

---

### RT-08 — Nothing bounds a run-wide transaction, it pins objects against GC, and the next edit can evict all undo history (Major)

**Decision.** D8 "Undo": one editor transaction per run, on by default for every route, Run File included. D8 Consequences: "show a tip after long runs".

**Objection.** Today a script that does not use `ScopedEditorTransaction` records nothing, because `UObject::Modify` saves to the transaction buffer only while a transaction is active. D8 changes the memory behaviour of every existing batch script.

**Evidence.**
- **Today's scripts record nothing without a transaction.** `Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp:1652-1676`: "The save will fail if there isn't a valid transactor".
- **The undo budget never applies to an open transaction.** It is checked only when a new top-level transaction begins, and then it evicts *older* transactions (`Engine/Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h:91-121`).
  - The default budget is 256 MB (`Engine/Config/BaseEditorPerProjectUserSettings.ini:490-492`; `Engine/Source/Editor/UnrealEd/Private/EditorServer.cpp:1255-1265`).
  - The eviction loop removes transactions from the oldest until the total is under budget. If the run's own transaction is over budget, that loop removes it too, along with every earlier transaction.
- **Recorded objects cannot be collected.** The buffer reports its objects to GC with reference elimination disabled: "We cannot support undoing across GC…" (`Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp:1263-1281`).
- **R06 does not ask for this default on batch runs.** It recommends a transaction toggle "on by default for editor buffers" (R06 §3 row 11), and it names batch asset processing as "the most common TA job" (R06 §5).
- **R05 already flagged the risk but under-rated it.** R05 R15 rates "undo transactions around huge batch scripts consume memory" Medium/Medium and mitigates it with a tip. The engine facts above (no bound, GC pinning, history eviction) make the cost an out-of-memory crash rather than a slowdown.

**Failure scenario.**
1. A Run File script fixes 10,000 material instances with `set_editor_property`.
2. Each change is serialised into one ever-growing transaction, and each touched asset is pinned. `collect_garbage()` inside the loop frees nothing, and the editor runs out of memory where it did not before Sumi was installed.
3. If the script finishes, the user's next actor move starts a new transaction. That evicts the oversized run *and* the user's whole undo history, so "Undo Last Run" quietly disappears.

**Suggested change.**
- **Keep the default for interactive runs:** on for selection, line, cell, REPL and shelf runs.
- **Change the default for batch runs:** off for Run File, with a one-click "Run in transaction".
- **Watch the size during runs.** On every pump, read the open transaction's size through `GEditor->Trans`.
  - At 50% of `UndoBufferSize`, warn in the banner.
  - At 100%, offer "Stop recording undo for the rest of this run", which ends the transaction and labels the run as partly undoable.
- **Make Undo Last Run honest.** Show the record size, and warn that the next edit will evict it once it exceeds the budget.
- **Document the GC pinning.**

---

### RT-09 — A `@task` transaction stays open across frames, which the engine treats as an error (Major)

**Decision.** D24: a `@task` is "its own run, with its own transaction". It is stepped once per frame, and the editor "ticks fully" between steps (journey J5).

**Objection.** An editor transaction held open across frames while the editor stays interactive is exactly the state the engine treats as a bug. D24 also does not say whether user input is blocked during a `@task`. Either answer fails:
- if input is blocked, the editor is unusable for the whole task, which can be 20 minutes;
- if input is allowed, the failures below happen.

**Evidence.**
- **PIE start cancels any open transaction.** The engine shows the notice "Cancelling open '{TransactionName}' operation to start Play In Editor" and explains that "transactions can get stuck open due to implementation errors" (`Engine/Source/Editor/UnrealEd/Private/PlayLevel.cpp:2585-2613`).
- **Level Editor undo is refused while a transaction is active**: "Can't undo while '{0}' is in progress" (`EditorTransaction.cpp:1466-1474`).
- **Nested Begins join the open transaction** (`TransBuffer.h:76`). The user's own edits therefore join the task's transaction.
- **Mode exit ensures no transaction is open**: `ensureMsgf(!GEditor->IsTransactionActive())` on foliage-mode exit (`Engine/Source/Editor/FoliageEdit/Private/FoliageEdMode.cpp:521`).

**Failure scenario.** During a 20-minute `@task`:
- the user moves three actors, and those moves are recorded into "Sumi: task #N";
- the user presses Ctrl+Z in the viewport, and it is refused;
- the user presses Play, and the engine cancels the task's transaction.

Undo Last Run then either reverts the user's manual moves or does nothing.

**Suggested change.**
- **Bound the transaction per step.** Each step (each `next()`) opens and closes its own transaction titled "Sumi: task #N". Undo Last Run walks back consecutive transactions with that title and stops at the first foreign one.
- **State the input policy.** D24 should say explicitly that the editor stays interactive between steps.
- **Add tests:** the `@task` soak should include PIE start and a user edit in the middle of the task.

---

### RT-10 — Phase 0 is overloaded, and the gates that do not block M1 sit in it anyway (Major)

**Decision.** D9: Phase 0 lasts 6–8 weeks with 3–4 engineers and a part-time QA engineer. It exits only when "G1–G4 pass, or their fallbacks are invoked". It also delivers CI for every test tier (D22), the SumiCore skeleton and the dogfood harness.

**Objection.**
- **G3 and G4 do not gate M1.**
  - G3 "fixes the D4 default before M2".
  - G4's fixture suite "gates 1.0 at M3 exit".
  - Even so, Phase 0 forces their outcome early.
- **G3 alone is a large project:**
  - the walker, which is otherwise M2 work (D5);
  - a fixture project with 100 or more plugins;
  - both servers integrated;
  - a 200-case golden corpus.
- **G2 alone needs** a 1,000-cycle run/stop suite with PIE running and an 8-hour soak.
- **G1 has no slack.** Its 6-week time box equals Phase 0's minimum length.
- **CI is itself a project:** D22's tiers 1–7 on three OSes, plus GPU agents.
- **The risk is already rated.** R05 R1 rates the custom code view's schedule overrun as High likelihood and High impact (R05, risk table).
- **The fallback becomes the default.** If G3 is unfinished at week 8, the exit rule invokes its fallback (Epic's stub through a commandlet) by default rather than on evidence.

**Suggested change.**
- **Narrow the exit.** Phase 0 exits on G1 and G2, plus harness prototypes for G5 and G7, plus CI tiers 1–3.
- **Run G3 during M1.** Run it in parallel with walker work, and require it to exit before M2 starts.
- **Move the G4 spike to M2.**
- **Publish a 1.0 cut list now,** for example the second LSP server (RT-12) and the Linux Preview moving to 1.1. Then a slip lands on named items, not on gate quality.

---

### RT-11 — debugpy integration details (Minor)

**Decision.** D3, M2: vendor debugpy, call `debugpy.configure(python=…)`, listen on 127.0.0.1, run the adapter "in our Job Object". D12 lists the pydevd licence as unconfirmed.

**Objection.**
1. **`listen()` blocks the game thread.**
   - In the default mode it spawns `python -m debugpy.adapter` with `subprocess.Popen`, then blocks on `accept()` with a 30 s timeout until the adapter connects back.
   - It can be called only once per process.
   - Because the adapter is started by Python's `subprocess`, not by our `FLspTransport`, it cannot be created suspended inside our Job Object.
2. **The vendored tree contains process-injection binaries.**
   - It ships `inject_dll_amd64.exe`, `run_code_on_dllmain_*.dll`, x86 variants, `.pdb` files and `winappdbg`, plus `.pyd` files for cp310 to cp314.
   - All of this sits under `Content/Python`, which bypasses the D4 manifest and signing pipeline.
   - Only `attach_<arch>.dll` is needed, because tracing other threads loads it.

**Evidence.**
- **The adapter path:**
  - one call per process: `debugpy/server/api.py:143-145`;
  - 30 s listener timeout: `:166`;
  - `Popen`: `:215`;
  - blocking `accept`: `:231-249`.
- **The in-process option:** `public_api.py:110-125` documents `in_process_debug_adapter`.
- **What tracing needs:** `_vendored/pydevd/pydevd_tracing.py:299, 387`, where `set_trace_to_threads` loads `attach_*.dll`.
- **The binaries:** the list from `pydevd_attach_to_process/`. These copies are Authenticode-signed by "Microsoft 3rd Party Application Component". Wheels from other sources may not be.

**Suggested change.**
- Call `debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)`. That spawns no adapter process, needs no `configure(python=…)`, does not block on `accept`, and needs no Job Object.
- Show "Debug in VS Code" as once per editor session.
- Prune `inject_dll_*`, `run_code_on_dllmain_*`, the x86 files, the `.pdb` files, `winappdbg` and every non-cp311 `.pyd`.
- Route what remains through the D4 SHA-256 manifest.
- **D12 open item settled:** pydevd is **EPL-1.0** (`debugpy/ThirdPartyNotices.txt:21-25`), which D12 already places under "allowed with conditions".

---

### RT-12 — Two supported language servers at 1.0 (Minor)

**Decision.** D4: Pyrefly and basedpyright are both supported at 1.0, and G3 picks the default.

**Objection.** D4 itself says two servers "double the integration tests". Its reason for rejecting basedpyright as the default, Node's AV, allow-list, notarisation and size friction, still applies when basedpyright ships as a supported alternate. D9 keeps third-party binaries out of M1 for the same reason. Supporting the G3 loser at 1.0 is scope without user evidence: R03 C2 contains no UE-stub benchmark.

**Suggested change.**
- Ship the G3 winner as the only supported server at 1.0.
- Treat the other like ty: experimental, with a user-supplied or policy-supplied binary, and no SLA.
- Promote it in 1.1 if pilots ask for it.

---

### RT-13 — M1 `sumi-check` lints have no `unreal` data source (Minor)

**Decision.**
- D4 ships `sumi-check` in M1, out of process on `python.exe -I`.
- Two of its lints need knowledge of the `unreal` API:
  - unknown `unreal.*` names;
  - attribute assignment where `set_editor_property` is needed, which requires property flags.
- D5's walker, the only source of that knowledge, lands in M2 (D9).

**Objection.** An out-of-process interpreter cannot import `unreal`. The `unreal` module exists only inside the editor process. So in M1 these two lints have nothing to check against. D11's "AI-safe human workflow" relies on the unknown-name lint.

**Suggested change.**
- Pull D5 Stage 1 (the snapshot JSON of names, descriptor kinds and docstrings) into M1 as `sumi-check`'s data file.
- Otherwise, move those two lints to M2 and say so in the M1 release notes.

---

### RT-14 — Dedenting a selection misplaces 3.11 traceback carets (Minor)

**Decision.** D8: selections and lines are dedented and compiled with `ast.increment_lineno`. The buffer text is registered in `linecache`.

**Objection.**
- **Carets land on the wrong tokens.** Python 3.11 draws `^~~` carets from `co_positions()` columns, measured against the text in `linecache` (`Engine/Binaries/ThirdParty/Python3/Win64/Lib/traceback.py:468-479`). The columns come from the dedented text, but the line shown is the original indented one.
- **Some selections fail outright.** `textwrap.dedent` also refuses to dedent a selection that contains a triple-quoted string with lines at a lower indentation, so that selection raises IndentationError.

**Evidence.** Checked on the engine's `python.exe`. For `    y = x["k"] + 1`:
- the dedent approach puts `^^^^^^` under `y = x[`;
- wrapping the selection in `if True:` puts `~^^^^^` under `x["k"]`, which is correct.

**Suggested change.**
- Parse `"if True:\n" + selection`, keeping the original indentation.
- Take `body[0].body` as the module body, and shift line numbers with `increment_lineno(start_line - 2)`.
- Columns stay exact, and the triple-quoted case compiles.

---

### RT-15 — The entry command depends on a user-mutable name (Minor)

**Decision.** D14(2): the entry is `ExecPythonCommandEx("sumi_editor._run(<id>)")`.

**Objection.** Literal commands evaluate in the console dict (`.../Private/PythonScriptPlugin.cpp:1817`), and D8 makes that dict the user's Session. The name `sumi_editor` must therefore live in that dict. If user code runs `del sumi_editor`, rebinds the name, or clears `globals()`, then every later run fails with a NameError. The decision log does not define what "Reset session" should do with the name either.

**Suggested change.**
- Use `__import__('sumi_editor')._run(<id>)` as the entry expression, so that it does not depend on any Session name.
- Keep `sumi_editor` out of the keys that Reset session restores.

---

### RT-16 — Undo Last Run (Ctrl+Alt+Z) shares a chord with AltGr+Z ("ż" on Polish keyboards) (Minor)

**Decision.** D8: Undo Last Run is Ctrl+Alt+Z. AltGr text is consumed when `IsRightAltDown` is true. G5 tests AltGr only for `@ { [ \``.

**Objection.** Windows reports AltGr as Ctrl+Alt. The stock text widget checks AltGr *before* it processes command bindings (`Engine/Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp:1280-1290`). The design copies that order, but the only binding that can do damage here (reverting the user's last run) has no test.

**Suggested change.**
- Add AltGr+Z (Polish "ż") and AltGr+X ("ź") to G5, and assert that no Undo Last Run fires.
- Never make Undo Last Run a global chord (outside our widget), because AltGr+Z is typed into every text field in the editor.

---

### RT-17 — "Never at editor start" contradicts layout restore (Minor)

**Decision.**
- D4: language servers "spawn on first buffer open, never at editor start".
- D7: `sumi_editor` is "imported lazily on first tab open".
- D16: the tab restores in the saved layout.

**Objection.** Most users keep the tab docked. For them, the first tab open and the first buffer open both happen *during editor start*. That brings the package import and up to three child processes (the type server, ruff and `sumi-check`) into the startup window, against the D22 budget of 30 ms of added startup.

**Suggested change.** Defer both until the first focus or keystroke in a restored buffer, or until N seconds after `FCoreDelegates::OnFEngineLoopInitComplete`, whichever comes first.

---

### RT-18 — "Machine policy" located through a user environment variable is not machine policy (Minor)

**Decision.** D20: precedence runs machine policy, then project policy, then user settings. The machine policy path comes from `SUMI_POLICY_PATH`.

**Objection.** Any user can unset or redirect an environment variable, so the top layer of the precedence chain is under user control. D20 itself says the model is not a sandbox. Even so, the word "machine" will be read by studio IT as "enforced".

**Suggested change.**
- Read machine policy from a fixed location that only administrators can write: `%ProgramData%\Sumi\Policy.json`, `/etc/sumi/policy.json`, or `/Library/Application Support/Sumi`.
- Treat `SUMI_POLICY_PATH` as an extra pointer with lower precedence.
- Show the source of each value in the settings inspector (D21).

---

### RT-19 — "We hold the copyright" is false under a DCO (Minor)

**Decision.** D12: contributions come in under a DCO. Its Consequences add: "The Fab build may carry Fab's standard licence … workable because we hold the copyright".

**Objection.** Under a DCO, contributors keep their copyright, so the project will not hold the copyright on contributed code. Any dual-licensing has to rest on Apache-2.0's own sublicensing grant and its notice conditions, or on a CLA.

**Suggested change.**
- Either adopt a CLA, or rewrite the Fab plan to rely on Apache-2.0 §2 and §4: ship the LICENSE and NOTICE files inside the Fab package.
- Put it on the legal review list D12 already has.

---

### RT-20 — No budget for the next engine release inside the 1.0 window (Minor)

**Decision.**
- D15: 1.0 supports UE 5.8, and each new engine release is supported within 6 weeks.
- D9: 1.0 GA is at month 11–12.

**Objection.** The local install is already 5.8.4 (`Engine/Build/Build.version`). R04 expects early signs of Python 3.13 in 5.9 previews (R04 "5.8-specific unknowns", item 12), and R06 records Epic saying that 3.13 will come through main (R06 §3 row 6). Under D15's own 6-week rule, a 5.9 release before GA would mean doing all of the following during 1.0 hardening:
- rebuilding `_sumi`;
- the `sys.monitoring` tracer;
- re-checking the cframe-based guards (RT-02);
- debugpy cp313;
- the docstring golden tests.

**Suggested change.**
- Either scope 1.0 explicitly as "5.8 only, with 5.9 in 1.0.x", or budget the next-engine port as a named M4 item.
- Run the 3.13 canary (D15) from M2, not just "before 1.0".

---

## 4. Consolidated gate additions

| Gate | Add |
|---|---|
| **G2(2)** | `merge_static_mesh_actors` with material merging and `create_proxy_mesh_actor` (RT-01). A fixture UFUNCTION that ticks the core ticker (RT-01). A Python `on_asset_post_import` hook taking 1 s per asset during `import_asset_tasks` (RT-02). An `on_delete_actors_begin` hook (RT-02). A Python UFUNCTION override called from C++ (RT-02) |
| **G2(5)** | Input-leak suite run in the window between arming and the modal opening (RT-04). Title-bar drag, minimise and maximise mid-run (RT-03) |
| **G2(6)** | An open EUW with a Blueprint Tick that calls Python. A Python-overridden EUW tick. A queued `AsyncTask(GameThread)` that fires a Python delegate. Pass condition: zero nested Python runs, or a documented policy (RT-03) |
| **G2(1)** | Stop during a `finally` that takes 3 s: the cleanup completes and no `ScriptCancelled` is re-raised inside it (RT-06) |
| **G2(3)/(7)** | An `unreal.log`-heavy loop at 1,000 lines/s (RT-07) |
| **G5** | Esc in `show_message` during a run returns Cancel (RT-05). AltGr+Z and AltGr+X on a Polish layout fire no Undo Last Run (RT-16) |
| **D8 tests** | A Run File script that modifies 10,000 objects: memory against the transaction size warning, and undo-history eviction (RT-08) |
| **D24 soak** | PIE start and a manual user edit in the middle of a task (RT-09) |

## 5. Facts settled during this review

- **pydevd licence:** EPL-1.0, from debugpy 1.8.20's `ThirdPartyNotices.txt:21-25`. This closes a D3 and D12 open item.
- **debugpy without an adapter process:** `in_process_debug_adapter=True` is public API (`public_api.py:110-125`). It removes the adapter process and the `configure(python=…)` requirement.
- **CPython 3.11.8 exception visibility:** the handled exception is visible inside `finally` and `__exit__` during propagation, and is cleared after a swallowing `except:`. Checked on the engine's interpreter. This makes the RT-06 guard possible.
- **`unreal.log` can be rebound:** `unreal.log*` are plain entries in the `unreal` module dict (`PythonScriptPlugin.cpp:1789-1806`). This makes the RT-07 shim possible.
- **Where `OnEndFrame` fires:** in the editor it is broadcast only at `LaunchEngineLoop.cpp:6127`, which is after the core ticker. This makes the RT-01 fix possible.
