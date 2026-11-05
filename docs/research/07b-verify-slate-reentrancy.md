> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 07b — Verification: Slate re-entrancy, debugging mode, modal ticks and slow tasks (UE 5.8 source)

Verification lane, 2026-10-28. Every claim below was checked against the local UE 5.8 install at `C:\Program Files\Epic Games\UE_5.8` (read only; nothing under it was modified). Engine evidence is cited as `Engine/<relative path>:<line>`. Web search was not used.

Path shorthands, written out in full once and shortened afterwards:

- **SA** = `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp`
- **SAH** = `Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h`
- **PSP** = `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/`
- **FCE** = `Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp`

This lane complements `07a-verify-python-runtime.md`, which already covers the Python tick-callback gate (`CanTickPython`). Where both lanes touch the same code, this file adds the Slate and engine side.

---

## Summary

1. **`EnterDebuggingMode` is a Kismet-only, Slate-only nested loop.** Its only caller is `FKismetDebugUtilities::AttemptToBreakExecution`, which uses it for Blueprint breakpoints both in PIE ("World" mode) and outside PIE ("Engine" mode). Each loop iteration calls `FSlateApplication::Tick()` (all tick types), `Renderer->EndFrame()`, `FlushCommands()` and `Sync()`.
   - It does **not** tick `FTSTicker`, `GEngine->Tick` (worlds, editor tickables, asset registry, editor timers, viewport scene rendering), GC or `ModalLoopTickEvent`.
   - It does pump OS messages, because `GIntraFrameDebuggingGameThread` is set. `Renderer->Sync()` also drains queued **game-thread task-graph tasks** every iteration.
   - The loop has no frame throttle. It returns only when someone calls `LeaveDebuggingMode()`, which is one shared flag.
2. **The biggest surprise: debugging mode turns `UObject::ProcessEvent` into a silent no-op.** `ProcessEvent` returns early while `GIntraFrameDebuggingGameThread` is true, and `EnterDebuggingMode` sets that flag after its first iteration.
   - Every reflected `unreal.*` method call from Python goes through `ProcessEvent`. In a paused debugger, evaluate and watch expressions such as `actor.get_actor_location()` would **silently return default values**.
   - Python-bound delegates and Blueprint events do not fire either.
   - If we use debugging mode, every evaluation must wrap `TGuardValue<bool>(GIntraFrameDebuggingGameThread, false)`.
3. **`EnterDebuggingMode` refuses silently in common cases.** The function returns `void` and gives no failure status. It refuses:
   - when **any modal window** is open, including an `unreal.ScopedSlowTask` dialog (the most common UE scripting idiom);
   - when called inside Tick or Paint of a widget in the main editor window;
   - during tab restore, or when already in debugging mode.

   On refusal it shows a notification and returns at once. With a native debugger attached it also calls `UE_DEBUG_BREAK()`.
4. **Context safety (Q2).** Entering from inside a Slate input handler, an FTSTicker callback or a Slate pre/post-tick delegate passes every guard. Each context still has a concrete hazard:
   - **Input handler.** `SButton` still holds mouse capture while `OnClicked` runs, so the debugger panel cannot be clicked.
   - **FTSTicker callback.** `FTSTicker::Tick` is not re-entrant; nesting it trips a `checkf`.
   - **Slate post-tick delegate.** The same Python tick callback can re-run every nested frame. There can also be a double RHI `EndFrame`.
   - **Modal loop.** Refused outright.
5. **Modal loops.** Blocking `AddModalWindow` runs its own 60 Hz loop. Each iteration does `ModalLoopTickEvent` → `TickPlatform` (which pumps) → `TickTime` → `TickAndDrawWidgets` → `EndFrame` → `Sync`.
   - Slow-task windows are modal but non-blocking, and they never run that loop.
   - `ModalLoopTickEvent` is a hook for *other* systems to tick (Content Browser data, analytics). It is not how Slate ticks.
6. **Slow tasks never repaint on their own.** The dialog ticks Slate only when the game thread calls `EnterProgressFrame`, `TickProgress` or `ShouldCancel`, at most every 0.2 s.
   - The tick is `FSlateApplication::Tick()` plus a guarded RHI `EndFrame` plus `Renderer->Sync()`, and only while the slow-task window is the top modal.
   - Only the modal window (plus tooltips and notifications) is drawn. Input to every other window, including ours, is dropped at the OS-message level.
   - Cancel is a bool set by the dialog's button, seen only on the next throttled tick.
   - `unreal.ScopedSlowTask` is a thin wrapper over `FSlowTask` on `GWarn`, which is the `FFeedbackContextEditor` in the editor.
7. **Input pre-processors cannot block everything.** `IInputProcessor` sees key down/up, mouse, wheel, analog and motion events. It does **not** see:
   - `KeyChar` (typed characters, `WM_CHAR`);
   - synthetic mouse moves;
   - IME, drag-drop, or window close/activation.

   The engine's own "block input while busy" mechanisms are different:
   - modal windows, filtered at the platform layer (`ShouldProcessUserInputMessages`);
   - widgets binding `IsEnabled` to `GetNormalExecutionAttribute()`, which is false in debugging mode (61 call sites).
8. **There is a safe, public "Slate-only tick" recipe.** It is used mid-frame by `FFeedbackContextEditor`, `FScopedSourceControlProgress` and `ScopedLocalizationServiceProgress`:

   ```cpp
   const bool bWasTicking = FSlateApplication::Get().IsTicking();
   FSlateApplication::Get().Tick();   // ESlateTickType::All
   if (!bWasTicking && GIsRHIInitialized) { ENQUEUE_RENDER_COMMAND(EndFrameCmd)(...RHICmdList.EndFrame()...); }
   FSlateApplication::Get().GetRenderer()->Sync();
   ```

   Two conditions apply:
   - Add `FSlateApplication::Get().PumpMessages()` when no modal window is active. `TickPlatform` pumps only for a modal or debugging mode.
   - Call it only on the game thread.
9. **Python callbacks during a mid-script Slate tick (Q7).**
   - Python Slate and ticker callbacks are gated by `GIsRunningUserScript` (see 07a). That gate covers only code entered through `ExecPythonCommandEx`/`EvalString`; a Python tick callback that is itself running does not set it.
   - Python-bound `unreal` delegates are **not** gated. They do fire in our own pump, but **not** in debugging mode, because `ProcessEvent` is blocked there.
   - FTSTicker-based Python callbacks never fire in Slate-only loops.
   - Game-thread task-graph tasks do run, inside `Renderer->Sync()`.

**Design consequence.** Use a **custom pause loop** that we own:
- the recipe in item 8, throttled to 60 Hz;
- `FScopedPreventDebuggingMode` and `SCOPE_STALL_DETECTOR_PAUSE`;
- release pointer capture first;
- a `RegisterCanCloseEditor` veto.

Avoid `EnterDebuggingMode`, or use it only with the evaluation guard. When a modal (slow task) is already active, fall back to a **blocking modal debugger window** parented to that modal. Always start user runs from a clean FTSTicker point, never from inside an input handler.

---

## Q1. `EnterDebuggingMode` / `LeaveDebuggingMode`: loop, what ticks, what is suppressed, callers, `FScopedPreventDebuggingMode`

**Answer.**

*Preconditions (in order), SA:3815-3893:*
1. It must be on the game thread. Otherwise `ensureMsgf(false, "Can only enter Debugging Mode while on the game thread.")` fires and it returns.
2. If `GetActiveModalWindow().IsValid()`, it fails with "A modal window is open."
3. (Editor builds) If `bIsTicking` **and** the current debug-context widget's window is the main editor root window, it fails with "Tried to enter debugging mode inside Tick in the main editor window…".
4. (Editor builds) If `PreventDebuggingModeStack` is non-empty, it fails with the top reason.
5. It then pushes its own `FScopedPreventDebuggingMode("Already in debug mode.")`, which makes re-entry fail at step 4.

Each failure goes through a lambda that shows a notification, logs "Enter Debugging Mode failed." and calls `UE_DEBUG_BREAK()`. That macro breaks only when a native debugger is attached (`Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMisc.h:48`). Then it returns. The return type is `void` (SAH:944), so the caller is not told.

*Setup, SA:3895-3937:*
- Deactivate any game viewport (`SetActive(false)`, `GameViewportWidget.Reset()`) so mouse capture is released and the cursor is visible.
- If the call came from inside a window's prepass or paint (`CurrentDebugContextWidget` valid), record that window as `CurrentDebuggingWindow`. It will be skipped by prepass and draw (SA:1236-1242, SA:1345-1351). The comment says "only if more than one window", but the code checks `SlateWindows.Num() > 0`.
- `Renderer->EndFrame(); Renderer->FlushCommands(); Renderer->Sync();`, then set `GFirstFrameIntraFrameDebugging = true`.

*The loop, SA:3940-3959:*
```cpp
while (!bRequestLeaveDebugMode) {
    Tick();  Renderer->EndFrame();  Renderer->FlushCommands();  Renderer->Sync();
    GFirstFrameIntraFrameDebugging = false;
    GIntraFrameDebuggingGameThread = !bRequestLeaveDebugMode;
}
```

*What runs each iteration:*

`FSlateApplication::Tick(ESlateTickType::All)` (SA:1621-1669) holds the recursive `SlateTickCriticalSection` and sets `bIsTicking`. It runs:
- `TickPlatform` (SA:1689-1744):
  - OS message pumping, **only if** a modal window exists or `GIntraFrameDebuggingGameThread` is true (SA:1705-1708);
  - in that case also `FCoreDelegates::StarvedGameLoop` → `FlushRenderingCommands()` (SA:1710-1713; bound at `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:4740-4744, 4936`);
  - `PlatformApplication->Tick`, `ProcessDeferredEvents`, cursor and tooltip updates, synthesized mouse moves and gestures.
- `TickTime`.
- `TickAndDrawWidgets` (SA:1746-1840):
  - `PreTickEvent.Broadcast`;
  - notifications, then `DrawWindows` with all widget Tick, active timers and paint (skipped while Slate is "asleep", SA:1800-1822);
  - `PostTickEvent.Broadcast`.

After the tick:
- `FlushCommands()` = `FlushRenderingCommands()` (`Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp:1837-1843`).
- `Sync()` = `FFrameEndSync::Sync(EndFrame)` (`SlateRHIRenderer.cpp:1845-1848`). This calls `FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread)`, unless the game thread is already processing tasks (`Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp:2562-2566`).

*Message pumping on the first iteration:*
- The Kismet caller sets `GIntraFrameDebuggingGameThread = true` *before* entering (`Engine/Source/Editor/UnrealEd/Private/Kismet2/KismetDebugUtilities.cpp:724`), so OS messages are pumped from the first iteration.
- A third-party caller that does not set it gets no OS pump on the first iteration. The loop sets it itself afterwards (SA:3957).

*What does NOT run:*
- `FTSTicker::GetCoreTicker().Tick`, `GEngine->Tick` (world ticks, `FTickableEditorObject::TickObjects`, asset registry tick, editor timers, level-viewport scene rendering), `FThreadManager::Tick`, deferred commands, GC.
- In the main loop those all sit outside Slate (`LaunchEngineLoop.cpp:5859, 6084-6110`; `Engine/Source/Editor/UnrealEd/Private/EditorEngine.cpp:1933`). The asset registry is ticked from `UEditorEngine::Tick` in editor builds (`Engine/Source/Runtime/AssetRegistry/Private/AssetRegistry.cpp:4896-4898`).
- `ModalLoopTickEvent` is not broadcast.
- There is no sleep or throttle, unlike `AddModalWindow`'s 60 Hz cap (SA:2235-2248), and no `SCOPE_STALL_DETECTOR_PAUSE` (compare SA:2221).

*What is suppressed while `GIntraFrameDebuggingGameThread` is true (it is true for the whole pause):*
- **`UObject::ProcessEvent` returns immediately** for every function, native or script (`Engine/Source/Runtime/CoreUObject/Private/UObject/ScriptCore.cpp:2073-2083`). It logs "Cannot call UnrealScript … while stopped at a breakpoint" on the first frame only. This blocks Blueprint events, dynamic delegates (`Engine/Source/Runtime/Core/Public/UObject/ScriptDelegates.h:1011-1016` → `ProcessEvent`) and **every Python reflected call** (`PSP/Private/PyUtil.cpp:645-647`).
- `UWorld::Tick` returns at once (`Engine/Source/Runtime/Engine/Private/LevelTick.cpp:1509-1512`).
- `PollGameDeviceState` is skipped (SA:1536).
- PIE game UI is not painted (`Engine/Source/Runtime/Engine/Private/Slate/SGameLayerManager.cpp:430-437`).
- UMG `CanSafelyRouteCall` and ToolMenus `CanSafelyRouteCall` are false (`Engine/Source/Runtime/UMG/Public/Components/Widget.h:1049`; `Engine/Source/Developer/ToolMenus/Private/ToolMenus.cpp:2492-2495`; `Engine/Source/Developer/ToolMenus/Public/ToolMenuEntryScript.h:173`).
- The Output Log console input box is disabled (`Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp:300-312`).
- 61 editor call sites bind enable state to `GetNormalExecutionAttribute()`/`IsNormalExecution()` (SAH:928-933; SA:941). Examples are the Level Editor toolbar and Content Browser.
- The editor cannot close. `CanCloseEditor` calls `LeaveDebuggingMode()` and defers `CLOSE_SLATE_MAINFRAME` (`Engine/Source/Editor/MainFrame/Private/Frame/MainFrameHandler.h:81-96`).

*Leaving (SA:3961-3981, SA:3984-3988):*
- `LeaveDebuggingMode(bSingleStep)` only sets `bRequestLeaveDebugMode = true` and `bLeaveDebugForSingleStep`.
- On exit, the game viewport is restored. With `bSingleStep == false` it is fully re-registered with `RegisterGameViewport` (mouse capture and lock). With `true` it is put back without capture.

*Callers:*
- `EnterDebuggingMode`: only `FKismetDebugUtilities::AttemptToBreakExecution` (`KismetDebugUtilities.cpp:817`).
  - With a PIE world it uses `EKismetDebuggingMode::World` and pauses PIE (`:789-798`).
  - Without one it uses `Engine` mode (`:799-802`), for example for editor-utility Blueprints.
  - The function itself asserts no re-entrancy: `check(!GIntraFrameDebuggingGameThread)` at `:721`.
- `LeaveDebuggingMode`:
  - Blueprint debugger commands (`Engine/Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp:258-273` and `:1007, 1742, 1758, 1783-1789, 1823`);
  - closing the PIE window (`Engine/Source/Editor/UnrealEd/Private/PlayLevel.cpp:3407-3413`);
  - a Blueprint script exception (`KismetDebugUtilities.cpp:456`);
  - Behavior Tree debugger (`Engine/Source/Editor/BehaviorTreeEditor/Private/BehaviorTreeDebugger.cpp:1216, 1237`);
  - editor close (`MainFrameHandler.h:89`; `Engine/Source/Editor/MainFrame/Private/Frame/MainFrameActions.cpp:637`).

*`FScopedPreventDebuggingMode` (SAH:953-961; SA:3991-4005, `WITH_EDITOR` only):*
- It is a public RAII object. It pushes `(Reason, Id)` onto `PreventDebuggingModeStack` and removes it by id on destruction.
- While any instance is alive, `EnterDebuggingMode` refuses and shows the reason.
- Used by: `FTabManager::RestoreArea_Helper` ("breakpoints in constructors can infinitely stall during restore", `Engine/Source/Runtime/Slate/Private/Framework/Docking/TabManager.cpp:2057`) and by `EnterDebuggingMode` itself (SA:3892). There are no plugin uses.
- The stack is private (SAH:2070), so a caller cannot query it in advance.

**Evidence.** SA:3815-4005; SA:1621-1840; SAH:928-961; `KismetDebugUtilities.cpp:713-822`; `ScriptCore.cpp:2073-2083`; `PyUtil.cpp:606-650`; `RenderingThread.cpp:2480-2573`; grep for `EnterDebuggingMode|LeaveDebuggingMode|FScopedPreventDebuggingMode` over `Engine/Source` and `Engine/Plugins` (all hits listed above).

**Confidence.** High. All of this was read directly. The 61-site count comes from a grep.

**Implication.**
- If we use `EnterDebuggingMode`, the evaluate/watch REPL must run under `TGuardValue<bool> G(GIntraFrameDebuggingGameThread, false)`. Otherwise every `unreal.*` method call returns defaults with no error.
- Our debugger will be resumed externally whenever the user closes the PIE window, closes the editor or presses Blueprint Resume. The Python side must treat an unexpected return as a "resume" and re-check state.
- Use `LeaveDebuggingMode(true)` for Python steps during PIE, so each step does not recapture the mouse into the PIE viewport.
- We can, and should, construct `FScopedPreventDebuggingMode` ourselves during our run pump and our custom pause loop. A Blueprint breakpoint hit inside our nested tick then refuses cleanly instead of nesting a second loop.

---

## Q2. Is it safe to call `EnterDebuggingMode` from a Python trace callback in each context?

**Answer.** In all four cases the trace function runs on the game thread, holding the GIL, inside CPython's eval loop.

**(a) Inside a Slate input handler.** The guards pass, but there is a hazard.
- In the main loop, OS input is dispatched from `FPlatformApplicationMisc::PumpMessages(true)` (`LaunchEngineLoop.cpp:5784`), which is *outside* `FSlateApplication::Tick`, so `bIsTicking` is false.
- Messages are processed immediately rather than deferred, because `GPumpingMessagesOutsideOfMainLoop` is false there (`Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp:4188-4199`; `Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsPlatformApplicationMisc.cpp:234-240`).
- With no modal and nothing preventing it, `EnterDebuggingMode` proceeds. Nested Win32 `DispatchMessage` is legal. `ProcessDeferredEvents` explicitly supports re-entry by copying its queue first ("This function can be reentered when entering a modal tick loop", `WindowsApplication.cpp:4215-4227`).
- **Hazard: pointer capture.**
  - `SButton::OnMouseButtonUp` calls `ExecuteOnClick()` while the button still has mouse capture, and only adds `ReleaseMouseCapture()` to the reply afterwards (`Engine/Source/Runtime/Slate/Private/Widgets/Input/SButton.cpp:442, 458-461`).
  - Mouse-down events in the nested loop are routed to the captor first (SA:5336). The debugger panel's buttons can therefore stop receiving clicks.
  - `AddModalWindow` defends against exactly this with `ReleaseAllPointerCapture()`, `PressedMouseButtons.Empty()` and `SetCapture(nullptr)` (SA:2204-2212). `EnterDebuggingMode` does not.
  - `FSlateApplication::IsProcessingInput()` (SAH:1416) detects this context.

**(b) Inside an FTSTicker / timer callback.** Safe as far as Slate is concerned.
- `FTSTicker::GetCoreTicker().Tick` runs after the Slate tick and the frame sync (`LaunchEngineLoop.cpp:6084, 6103`), so `bIsTicking` is false and no capture is held.
- The debugging loop does not tick FTSTicker, so the ticker is not re-entered.
- **Hazard: FTSTicker is not re-entrant.** `Tick` holds `FElementPtr& Element = Elements[ElementIdx]` while firing, then moves and `Exchange`s the arrays (`Engine/Source/Runtime/Core/Private/Containers/Ticker.cpp:99-140`). A nested `Tick` would reach the executing element and fail `checkf(GetThreadId(PrevState) == 0, "Invalid state …")` (`Ticker.cpp:101-102`). Any custom pause loop must therefore never call `FTSTicker::GetCoreTicker().Tick`.
- Because FTSTicker is frozen during the pause, any part of our UI that polls through FTSTicker (IPC, LSP) also freezes. That includes Python's own `register_ticker_callback`.

**(c) Inside a Slate pre/post-tick delegate.** Allowed, with re-entrancy hazards.
- `bIsTicking` is true, but `CurrentDebugContextWidget` is only set during prepass and paint (SA:1251, SA:1357). At pre/post-tick it is null, so the main-window guard (SA:3874-3885) does not trigger.
- Nested `Tick()` re-acquires `SlateTickCriticalSection`, which is `FCriticalSection` = `UE::FPlatformRecursiveMutex` (`Engine/Source/Runtime/Core/Public/HAL/CriticalSection.h:53`), so there is no deadlock.
- It re-broadcasts the same `PostTickEvent`. Re-entrant broadcast is supported: there is a lock counter, deferred compaction and a recursive access detector (`Engine/Source/Runtime/Core/Public/Delegates/MulticastDelegateBase.h:288-312, 454-473`; `Engine/Source/Runtime/Core/Public/Delegates/DelegateAccessHandler.h:104-170`).
- **Hazard 1: the paused callback runs again.**
  - The Python Slate-callback lambda does not set `GIsRunningUserScript` (`PSP/Private/PySlate.cpp:28-46`). If the paused code is itself a Python tick callback, *every* Python tick callback runs again on every nested frame, **including the paused one**.
  - In CPython 3.11 a trace function runs with tracing paused for nested frames (CPython behaviour; not verifiable from the shipped headers). So those nested runs probably will not hit breakpoints, but they will mutate state under the paused frame.
- **Hazard 2: double `EndFrame`.** `EnterDebuggingMode` calls `Renderer->EndFrame()` unconditionally (SA:3927, 3945). `FFeedbackContextEditor` and `FScopedSourceControlProgress` deliberately skip their `EndFrame` when `IsTicking()`, with the note "to prevent a double EndFrame in case the user wrongly uses FSlateApplication::OnPreTick" (FCE:424-434; `Engine/Source/Developer/SourceControl/Private/ScopedSourceControlProgress.cpp:238-247`). Kismet tolerates this for breakpoints inside widget ticks in floating windows, so it is probably benign, but it is untested by us.
- A breakpoint inside **widget Tick or Paint** in the main window (for example Python called from our own docked widget's `Tick`) is refused (SA:3874-3885).

**(d) Inside a modal window loop.** Refused.
- `GetActiveModalWindow().IsValid()` fails immediately (SA:3864-3868). The comment at SA:3847-3852 names this exact case.
- This includes **slow-task windows**. `AddModalWindow(..., bSlowTaskWindow=true)` pushes onto `ActiveModalWindows` before returning (SA:2158), and `FFeedbackContextEditor::StartSlowTask` uses it (FCE:547-550).
- So a Python breakpoint inside `with unreal.ScopedSlowTask(...) as t: t.make_dialog()` cannot use debugging mode.
- Before a delayed dialog has actually opened (`make_dialog_delayed` threshold not reached) there is no modal, and entry is allowed.

**Other guards that apply in every case:**
- Game thread only (SA:3817-3821). A breakpoint in a Python worker thread needs a different pause mechanism, such as blocking that thread on an event.
- Re-entry is refused through the prevent stack (SA:3887-3892). Kismet asserts `check(!GIntraFrameDebuggingGameThread)` before its own call (`KismetDebugUtilities.cpp:721`). That check is effectively unreachable during our pause, because `ProcessEvent` blocks Blueprint execution while the flag is true.

**Evidence.** As cited inline. Also: `LaunchEngineLoop.cpp:5784, 5921, 5991, 6103` (where each context sits in the frame), and SA:1631-1633.

**Confidence.**
- High for the guard logic and the capture behaviour.
- Medium for the practical impact of the double `EndFrame` and of the CPython nested-tracing behaviour.

**Implication.**
- Never run user code from inside an input handler. Queue it, and start it from a one-shot FTSTicker callback, which is the cleanest point in the frame. This confirms 05's "defer to a clean point".
- Before any pause, call `FSlateApplication::Get().ReleaseAllPointerCapture()`.
- Detect refusal. For example, count `OnPreTick` broadcasts during the call: if zero ticks happened and our resume flag is unset, it failed.
- Have a fallback for an active modal (Q3).
- Python tick callbacks must never be the entry point for debugged code. Our "Run" path goes through `ExecPythonCommandEx`, so `GIsRunningUserScript` suppresses foreign Python tick callbacks during the pause (07a Q7).

---

## Q3. Modal ticking: `ModalLoopTickEvent`, `AddModalWindow`, and how modal loops tick Slate

**Answer.** `FSlateApplication::AddModalWindow(Window, Parent, bSlowTaskWindow)` (SA:2124-2281) proceeds as follows:
1. Bail if `!CanAddModalWindow()`, which requires a renderer with shaders (SA:2066-2076).
2. In unattended-script mode, refuse non-slow-task modals (SA:2134-2146).
3. Broadcast `FCoreDelegates::PreSlateModalWithContext` (and the deprecated `PreSlateModal`) (SA:2148-2156).
4. `ActiveModalWindows.AddUnique`, close all tooltips ("Tooltips … can re-enter into code that shouldn't execute in a modal state"), `SetAsModalWindow`, `Renderer->FlushCommands()`.
5. Create the native window; it is hidden initially for slow tasks.
6. Fire `ModalWindowStackStartedDelegate` for the first modal.
7. Release all pointer capture, clear `PressedMouseButtons`, `SetCapture(nullptr)`, and turn off high-precision mouse mode (SA:2157-2215).
8. **Only if `!bSlowTaskWindow`**, block in a loop (SA:2218-2280):

```cpp
SCOPE_STALL_DETECTOR_PAUSE();  ...  Renderer->EndFrame();
while (InSlateWindow == GetActiveModalWindow()) {
    /* sleep to cap at 60 Hz */
    ModalLoopTickEvent.Broadcast(DeltaTime);
    TickPlatform(DeltaTime);            // pumps OS messages because a modal exists (SA:1705)
    if (InSlateWindow != GetActiveModalWindow()) break;
    TickTime();  TickAndDrawWidgets(DeltaTime);  Renderer->EndFrame();
    Renderer->Sync();
}
```

**While any modal is active:**
- **Drawing:** only the active modal, tooltips and notification windows are drawn and prepassed (SA:1386-1405, SA:1476-1497). Every other window shows its last frame.
- **Input:** OS input messages for windows that are neither the modal, its descendants nor an interactive tooltip are consumed before Slate sees them (`ShouldProcessUserInputMessages`, SA:4886-4901; `WindowsApplication.cpp:3195-3205`). `IsInputMessage` covers keyboard (including `WM_CHAR`), mouse and raw input (`WindowsApplication.cpp:4109-4123`).
- **Editor close:** the editor refuses to close while a modal is on screen (`MainFrameHandler.h:125-126`).
- **Teardown:** the modal is removed and other windows are re-enabled when it is destroyed (SA:3255-3280).

**`ModalLoopTickEvent`.**
- It is `DECLARE_EVENT_OneParam(..., FOnModalLoopTickEvent, float)` with a `GetOnModalLoopTickEvent()` accessor (SAH:1844-1851).
- It is broadcast only from that blocking loop (SA:2253): not by slow-task windows and not by `EnterDebuggingMode`.
- Subscribers in 5.8:
  - `UContentBrowserDataSubsystem::Tick` (`Engine/Source/Editor/ContentBrowserData/Private/ContentBrowserDataSubsystem.cpp:197`);
  - `FEditorAnalyticsSessionSummary` (`Engine/Source/Runtime/Engine/Private/Analytics/EditorAnalyticsSessionSummary.cpp:100`);
  - the ChaosVD and RewindDebugger session-browser modals.
- Editor code usually reaches the loop through `UEditorEngine::EditorAddModalWindow`, which parents the modal to the active top-level window (`EditorEngine.cpp:4157-4173`).

**Evidence.** As above.

**Confidence.** High.

**Implication.**
- A blocking modal is the engine's most-exercised nested loop. It is legal from inside a slow task (message boxes over progress dialogs) and from input handlers.
- It is therefore the right **fallback** when a Python breakpoint is hit while a modal or slow task is active. Open a blocking "Paused" debugger window parented to `GetActiveModalWindow()`. It becomes the top modal, so it is drawn and receives input.
- Cost: our docked editor tab is not repainted during that pause, so the modal must carry the call stack, variables, source excerpt and controls.
- Respect `GIsRunningUnattendedScript`: modals are refused in that mode.

---

## Q4. `FSlowTask` / `FScopedSlowTask` and `FFeedbackContextEditor`

**Answer.**

**Repainting while the game thread is busy: it does not repaint by itself.**
- The dialog repaints only when game-thread code calls into the slow task:
  - `EnterProgressFrame` → `TickProgress` (`Engine/Source/Runtime/Core/Private/Misc/SlowTask.cpp:177-216`);
  - `ShouldCancel` (`SlowTask.cpp:248-260`);
  - the legacy `StatusUpdate` / `UpdateProgress`.
- Each of these calls `Context.RequestUpdateUI()`. That is **throttled to 0.2 s (5 Hz)** with a function-static timestamp (`Engine/Source/Runtime/Core/Private/Misc/FeedbackContext.cpp:184-197`), and then calls `UpdateUI()` → `ProgressReported(fraction, message)` (`FeedbackContext.cpp:199-206`).
- `FSlowTask::ForceRefresh` forces two updates on dialog create and destroy, "to account for r.oneframethreadlag" (`SlowTask.cpp:113-119`).
- `FPlatformMisc::PumpMessagesForSlowTask()` (`SlowTask.cpp:210`) is a no-op on Windows. Only the generic empty version exists (`Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMisc.h:1957-1959`); `WindowsPlatformMisc.h` does not override it.
- So `time.sleep(10)` or one long native call inside a slow task freezes the dialog too.

**What `ProgressReported` does (FCE:583-631):**
- At most once a second, if deferred-cleanup objects are pending, it calls `FlushRenderingCommands()`.
- If the slow-task window is valid and `CanDisplayWindows()`, it calls `TickSlate(SlowTaskWindow)`.
- `TickSlate` (FCE:419-439) ticks **only if** the slow-task window is the active modal or one of its descendants ("Avoid re-entrancy by ticking the active modal window again"). It then calls `FSlateApplication::Get().Tick()` (all types; it pumps because a modal exists), enqueues an RHI `EndFrame` unless Slate was already ticking, and calls `Renderer->Sync()`.
- The widget keeps an active timer for its whole life (FCE:67-68), so Slate never sleeps while it is up.

**Dialog creation (`FSlowTask::MakeDialog`, `SlowTask.cpp:231-246`).** A dialog is created only if all of these hold:
- the task is enabled and on the game thread;
- not `GIsSilent`, not a commandlet, not `Invisible`;
- not in PIE unless `bAllowInPIE` (`IsPlayingInEditor` = `GIsPlayInEditorWorld || GEditor->PlayWorld`, FCE:761-764);
- **no slow-task dialog is open already** (`!GIsSlowTask`), so nested tasks share the outermost dialog.

`FFeedbackContextEditor::StartSlowTask` (FCE:456-564) adds more conditions:
- It suppresses the dialog during drag-drop or while a menu is open.
- It parents the dialog to the active PIE window, then the active top-level regular window, then the main frame.
- It creates an `SWindow` containing `SSlowTaskWidget`, calls `AddModalWindow(Window, Parent, /*bSlowTask*/true)` (non-blocking), shows it and ticks once.
- The base class sets `GIsSlowTask = true` and broadcasts `OnStartSlowTask` (`Engine/Source/Runtime/Core/Public/Misc/FeedbackContext.h:78-86`).

**Delay (`MakeDialogDelayed`, `SlowTask.cpp:170-175`, `SlowTask.cpp:61-97`).**
- It only stores the threshold.
- On each later `TickProgress`, `MakeRecursiveDialogIfNeeded` checks elapsed time since construction and calls `MakeDialog` once the threshold passes.
- There is no timer. If the first progress call comes late, the dialog appears late.

**Cancel plumbing.**
1. The Cancel button exists only if `bShowCancelButton` (FCE:521-526).
2. A click goes to `SSlowTaskWidget::OnCancel` → `OnCancelClickedDelegate` → `FFeedbackContextEditor::OnUserCancel()` → `HasTaskBeenCancelled = true` (FCE:378-381, FCE:707-710).
3. `FSlowTask::ShouldCancel()` returns false unless `GIsSlowTask`. Otherwise it calls `RequestUpdateUI()`, which runs the throttled tick so the click can be processed, and then returns `Context.ReceivedUserCancel()` (`SlowTask.cpp:248-260`; FCE:702-705).
4. The flag is reset at the next `StartSlowTask` (FCE:471).
5. `OnUserCancel()` is **public** and exported (`Engine/Source/Editor/UnrealEd/Public/FeedbackContextEditor.h:51-53`), so our Stop button can set it.

**Python mapping.**
- `unreal.ScopedSlowTask(work, desc='', enabled=True)` allocates `new FSlowTask(work, desc, enabled)` with the default context `*GWarn` (`PSP/Private/PyCore.cpp:263-270`; `Engine/Source/Runtime/Core/Public/Misc/SlowTask.h:103`).
- In the editor, `GWarn` is the static `FFeedbackContextEditor UnrealEdWarn` (`LaunchEngineLoop.cpp:263, 2755`).
- Method mapping (`PyCore.cpp:624-800`):

  | Python | C++ |
  |---|---|
  | `__enter__` | `Initialize()` |
  | `__exit__` | `Destroy()` |
  | `make_dialog_delayed(delay, can_cancel=False, allow_in_pie=False)` | `MakeDialogDelayed` |
  | `make_dialog(can_cancel=False, allow_in_pie=False)` | `MakeDialog` |
  | `enter_progress_frame(work=1.0, desc='')` | `EnterProgressFrame` |
  | `should_cancel()` | `ShouldCancel` |
  | `tick_progress()` | `TickProgress` |

- None of these release the GIL (there is no `Py_BEGIN_ALLOW_THREADS` in that range). The Slate tick inside `enter_progress_frame` therefore runs with the GIL held, and nested Python callbacks re-take it re-entrantly on the same thread.
- `FScopedSlowTask` is just RAII over `Initialize`/`Destroy` (`Engine/Source/Runtime/Core/Public/Misc/ScopedSlowTask.h:31-51`).

**Evidence.** As cited.

**Confidence.** High.

**Implication.**
- 04's "Running… [Stop]" modal pump has exactly the slow-task profile: modal-filtered input, only the modal drawn, `Tick` + guarded `EndFrame` + `Sync`. It is a proven engine pattern (also `ScopedSourceControlProgress.cpp:213-254`, `Engine/Source/Developer/LocalizationService/Private/ScopedLocalizationServiceProgress.cpp:230`).
- While any user `ScopedSlowTask` dialog is open, our docked panel cannot be clicked or repainted. Stop interop must call `static_cast<FFeedbackContextEditor*>(GWarn)->OnUserCancel()`, guarded so it runs only when `GWarn` is the editor context, plus our own pending-call interrupt.

---

## Q5. `IInputProcessor` / `RegisterInputPreProcessor`: can it swallow everything except clicks on specific widgets?

**Answer.**

**Mostly, but not completely.**
- `IInputProcessor` (`Engine/Source/Runtime/Slate/Public/Framework/Application/IInputProcessor.h:17-54`) has handlers for `KeyDown`, `KeyUp`, `AnalogInput`, `MouseMove`, `MouseButtonDown`, `MouseButtonUp`, `MouseButtonDoubleClick`, `MouseWheelOrGesture` and `MotionDetected`, plus a required `Tick`. Returning `true` swallows the event.
- Registration (SAH:1544-1574; SA:4065-4093) takes an `EInputPreProcessorType` bucket. Buckets are evaluated in ascending order and "may block input from later buckets": `Overlay`, `PreEngine`, `Engine`, `PreEditor`, `Editor`, `PreGame`, `Game` (default) (SAH:187-207). A priority index sets the order within a bucket.
- Pre-processors run first in each `Process*Event` (key down SA:4982, mouse down SA:5324, mouse up SA:6148, double click SA:6041, wheel SA:6232). That means they run before widget routing, for every window and including the game viewport.

**Gaps:**
- `ProcessKeyCharEvent` (`WM_CHAR`, the actual typed characters) has **no** pre-processor call (SA:4909-4950). Swallowing `KeyDown` does not stop text entry into a focused `SEditableText` or `SMultiLineEditableText`.
- Synthetic mouse moves bypass pre-processors (`if (!bIsSynthetic)`, SA:6399-6401). Hover, tooltips and cursor queries keep running.
- IME composition, OLE drag-drop, window close/minimize/resize and activation are not input events at all.
- Before the pre-processor runs on mouse down, Slate has already called `PlatformApplication->SetCapture(PlatformWindow)` and `PressedMouseButtons.Add` (SA:5313-5321). Editor-only `OnApplicationPreInputKeyDownListenerEvent` and `OnApplicationMousePreInputButtonDownListenerEvent` also fire before pre-processors (SA:4973-4979, SA:5302-5308).
- If a pre-processor swallows a mouse-up, Slate clears the matching mouse-down state **only for touch events** (SA:6148-6152). For a real mouse, a blocker must swallow down and up as a pair.

**Whitelisting specific widgets.**
- The precedent is the Translation Picker, which registers at index 0 (`Engine/Source/Developer/TranslationEditor/Private/TranslationPickerFloatingWindow.cpp:56-153, 198`).
- It finds what is under the cursor with `FSlateApplication::Get().LocateWindowUnderMouse(MousePos, GetInteractiveTopLevelWindows(), true)` (`:244`). `LocateWindowUnderMouse` is public (SAH:1674). `LocateWidgetInWindow` is in the protected section (SAH:1275), so use `LocateWindowUnderMouse`.
- A blocking pre-processor can do the same: let the event through if the widget path contains a whitelisted widget (Stop, scroll bar, debugger panel), otherwise return `true`. It must swallow down and up for the same pointer consistently.

**Engine patterns for blocking input while busy. None uses a pre-processor:**
1. **Modal windows.** Platform-level filtering drops all input messages, `WM_CHAR` included, for non-modal windows (Q3).
2. **`GetNormalExecutionAttribute()`.** In debugging mode, 61 editor sites disable themselves this way.
3. **Per-widget `CanSafelyRouteCall`** in UMG and ToolMenus.

**Evidence.** As cited.

**Confidence.** High.

**Implication.**
- 05's "pre-processor swallows all input except Stop, Esc and scroll" does not by itself stop the user from editing buffers. Typed characters still arrive. While a run or pause is active we must also:
  - set our code views read-only;
  - register `IMainFrameModule::RegisterCanCloseEditor` returning false (`Engine/Source/Editor/MainFrame/Public/Interfaces/IMainFrameModule.h:226-238`; consulted at `MainFrameHandler.h:150`). A pre-processor does not block window close; only a modal or debugging mode does.
  - accept that hover and tooltips still run.
- If a modal is active, keyboard focus is on the modal. A key-down pre-processor (for example Esc to stop) still sees those events, but mouse clicks on our non-modal panel are dropped before any pre-processor.
- Use the `PreEngine` or `Overlay` bucket so game or editor pre-processors cannot pre-empt Stop.

---

## Q6. Is there a safe Slate-only tick usable mid-frame? Exact calls of the slow-task path

**Answer.**

**Yes. `FSlateApplication::Tick(ESlateTickType)` is public (SAH:394).** It is also safe nested:
- It asserts the game thread unless the type is `Time` (SA:1629).
- It locks a recursive mutex (SA:1631; `CriticalSection.h:53`) and sets `bIsTicking` with a guard that restores on exit (SA:1633). `IsTicking()` is public (SAH:397).
- Tick types are `Time`, `PlatformAndInput`, `Widgets`, `TimeAndWidgets` and `All` (SAH:167-185). The main loop uses `PlatformAndInput` (`LaunchEngineLoop.cpp:5921`) and then `TimeAndWidgets` (`:5991`).
- `PumpMessages()` is public (SAH:400; SA:1842-1845).
- `TickPlatform`, `TickTime`, `TickAndDrawWidgets` and `DrawWindows` are **protected** (SAH:1180-1283).
- The renderer's `EndFrame`, `FlushCommands` and `Sync` are public virtuals on `FSlateRenderer` (`Engine/Source/Runtime/SlateCore/Public/Rendering/SlateRenderer.h:443-455`).

**Exact calls on the slow-task path.** For `unreal.ScopedSlowTask.enter_progress_frame` → `FSlowTask::EnterProgressFrame`:
1. `check(IsInGameThread())`; update work counters (`SlowTask.cpp:181-197`).
2. `TickProgress()` → `FPlatformMisc::PumpMessagesForSlowTask()` (a no-op on Windows) (`:210`).
3. `MakeRecursiveDialogIfNeeded()`. This may call `MakeDialog` → `FFeedbackContextEditor::StartSlowTask` → `AddModalWindow(..., true)` + `ShowWindow()` + `TickSlate()` (`:212`, FCE:530-552).
4. `Context.RequestUpdateUI()`. It returns unless 0.2 s have passed (`FeedbackContext.cpp:184-197`).
5. `UpdateUI()` → `FFeedbackContextEditor::ProgressReported` → (at most 1 Hz) `FlushRenderingCommands()` → `TickSlate(SlowTaskWindow)` (FCE:592-630).
6. `TickSlate`:
   1. Check that the window is the top modal or a descendant (FCE:422).
   2. `bool bIsTicking = IsTicking()`.
   3. `FSlateApplication::Get().Tick()`. That is: `TickPlatform` (with the modal active: `PumpMessages`, `StarvedGameLoop` → `FlushRenderingCommands`, `PlatformApplication->Tick`, `ProcessDeferredEvents`, cursor and tooltip updates), then `TickTime`, then `TickAndDrawWidgets` (`PreTick`, notifications, `DrawWindows` of the modal, tooltips and notifications only, `PostTick`).
   4. If `!bIsTicking && GIsRHIInitialized`: `ENQUEUE_RENDER_COMMAND(EndFrameCmd)` → `RHICmdList.EndFrame()`.
   5. `GetRenderer()->Sync()` → `FFrameEndSync::Sync(EndFrame)`. This processes game-thread task-graph tasks (`RenderingThread.cpp:2562-2566`) and waits on the render and RHI fences, processing game-thread tasks while waiting (`RenderingThread.cpp:1158-1252`).

Not called: `ModalLoopTickEvent`, `FTSTicker`, `GEngine->Tick`.

**Other mid-frame precedents:**
- `FScopedSourceControlProgress::Tick` is identical (`ScopedSourceControlProgress.cpp:234-254`).
- Commandlets use `PumpMessages(); Tick();` (`Engine/Source/Runtime/Engine/Private/Commandlets/Commandlet.cpp:139-143`).
- So does `Engine/Source/Developer/Datasmith/DatasmithExporter/Private/DatasmithExporterManager.cpp:183-184`.

**Recommended pause/pump loop for our plugin.** Derived from the above; prototype required:
```cpp
FSlateApplication::FScopedPreventDebuggingMode NoBP(LOCTEXT("PyPause","Python debugger is paused"));
SCOPE_STALL_DETECTOR_PAUSE();
FSlateApplication& App = FSlateApplication::Get();
App.ReleaseAllPointerCapture();
while (!bResume) {                                   // cap at ~60 Hz like SA:2235-2248
    if (!App.GetActiveModalWindow().IsValid()) App.PumpMessages();   // TickPlatform won't pump otherwise (SA:1705)
    const bool bWasTicking = App.IsTicking();
    App.Tick();
    if (!bWasTicking && GIsRHIInitialized) { ENQUEUE_RENDER_COMMAND(...EndFrame...); }
    App.GetRenderer()->Sync();
}
```

**Evidence.** As cited.

**Confidence.**
- High for the API surface and the slow-task call list.
- Medium for "safe in every context". The engine runs these calls mid-frame only from slow tasks, progress windows and commandlets; not from inside FTSTicker callbacks or Slate tick delegates.

**Implication.** The pump in 05 and our debugger pause loop can be built entirely on public API, with no engine patch:
- `Tick(All)`, plus `PumpMessages` when no modal is active, plus a guarded `EndFrame`, plus `Sync`;
- run at a clean entry point;
- a 60 Hz throttle;
- `FScopedPreventDebuggingMode`;
- `SCOPE_STALL_DETECTOR_PAUSE`. The stall detector is compiled into editor builds (`Engine/Source/Runtime/Core/Public/ProfilingDebugging/StallDetector.h:20-25`).

`Sync()` will run queued `AsyncTask(GameThread)` work. Budget for that, and make sure our own async completions are re-entrancy-safe.

---

## Q7. If we tick Slate mid-script, which Python callbacks can fire? Concrete hazards

**Answer.**

| Callback kind | Fires in our custom pump? | Fires in `EnterDebuggingMode`? | Evidence |
|---|---|---|---|
| `register_slate_pre/post_tick_callback` | Only if `GIsRunningUserScript` is false, i.e. the paused or pumping code was **not** entered through `ExecPythonCommandEx`/`EvalString`, or is itself inside a Python tick/ticker callback | Same rule. The broadcast happens each iteration (SA:1758, SA:1827) | `PSP/Private/PySlate.cpp:28-46` (gate at `:31`; the lambda does not set the flag); `PyUtil.cpp:1822-1838`; `PythonScriptPlugin.cpp:1831` |
| `register_ticker_callback` (FTSTicker) | No. FTSTicker is not ticked | No | `PyCore.cpp:40-66`; `LaunchEngineLoop.cpp:6103` |
| Python callables bound to `unreal` delegates (`add_callable`, editor-subsystem events, selection, asset delegates, UMG/EUW events) | **Yes**, whenever the nested tick triggers the C++ event. **Not** gated by `CanTickPython`. They set `GIsRunningUserScript` themselves | **No**. Dynamic delegates go through `ProcessEvent`, which is blocked | `PSP/Private/PyWrapperDelegate.cpp:21-37`; `PyGenUtil.cpp:1278-1384`; `ScriptDelegates.h:1011-1016`; `ScriptCore.cpp:2073-2083` |
| Python `unreal.uclass` UFUNCTION overrides, ToolMenu script entries | Yes, if a UI action invokes them (menu click, button) | No (`ProcessEvent` blocked; ToolMenus `CanSafelyRouteCall` false) | `PyWrapperObject.cpp:2220-2259`; `ToolMenus.cpp:2492-2495` |
| `AsyncTask(ENamedThreads::GameThread, …)` lambdas, Python plugin's own included (for example pip-install completion, `PSP/Private/PipInstall.cpp:807`) | **Yes**, inside `Renderer->Sync()` | Yes, inside `Sync()` and `FlushCommands()` | `RenderingThread.cpp:2562-2566, 1219-1222` |
| Our own C++ widgets that call Python (completion, hover, lint) | Yes. Any Python they run executes on top of the paused frame | Yes, but their `unreal.*` calls are no-ops | design |

**Concrete re-entrancy hazards:**
1. **A paused frame inside a Python tick callback.** Every Python tick callback, including the paused one, runs again on every nested frame. They mutate module globals under the paused frame. The trace function is not re-armed for them; nested frames are probably untraced (CPython 3.11 behaviour).
2. **Python-bound delegates during our custom pump.** For example, the user clicks the outliner (if not blocked) → `OnSelectionChanged` → Python handler runs mid-script. It sees half-updated state, or calls `unreal.*` APIs that the paused script is in the middle of using, for example an open `ScopedEditorTransaction`.
3. **GIL ownership.**
   - Our pause runs inside the trace function with the GIL held. Every other Python thread (debug adapter, user threads) is starved for the whole pause, unless we wrap the loop in `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS`.
   - Nested callbacks re-take the GIL with `PyGILState_Ensure` (`FPyScopedGIL`). This is the same pattern `InvokeFunctionCall` already relies on (`PyUtil.cpp:645-647`).
4. **`unreal.*` calls silently no-op in debugging mode** (Q1). Watch values would be wrong, and side-effecting evaluations would silently do nothing.
5. **External resume.** PIE-window close, editor close and Blueprint Resume all call `LeaveDebuggingMode()`. The Python side must handle "the loop returned but the user did not press Continue".
6. **A user `ScopedSlowTask` dialog is active.** Our pump draws only that modal and our panel is dead. `EnterDebuggingMode` refuses. We need the blocking-modal fallback.
7. **Close requests during a custom pump.** `WM_CLOSE` is not an input message. Without a modal or debugging mode, `CanCloseEditor` takes the normal exit path, so we must veto through `RegisterCanCloseEditor`.

**Evidence.** As cited in the table.

**Confidence.**
- High for which mechanisms are gated.
- Medium for how often they fire in practice; this needs a prototype.

**Implication.** Rules for the design:
- Enter all user code through `ExecPythonCommandEx` (07a).
- Never pause inside Python tick or ticker callbacks: offer "disable breakpoints in tick callbacks", or document the behaviour.
- Block or disable UI that broadcasts Python-bound delegates during a pump: modal, read-only state, pre-processor.
- Release the GIL during the pause.
- Guard evaluation with the `GIntraFrameDebuggingGameThread` override if debugging mode is used.
- Add a re-entrancy counter around our own pump.

---

## Corrections to research

| File | Claim | Correction |
|---|---|---|
| 05-architecture-and-robustness.md | §2.6 Prior art: "Modal loops tick Slate in place via `ModalLoopTickEvent` [S42]" | Refuted as worded. `ModalLoopTickEvent` is a notification that the blocking `AddModalWindow` loop broadcasts for *other* systems (SA:2253; subscribers include `ContentBrowserDataSubsystem.cpp:197`). The loop ticks Slate itself with `TickPlatform`/`TickTime`/`TickAndDrawWidgets` (SA:2259-2278). Slow-task modals never run that loop, and `EnterDebuggingMode` never broadcasts the event. |
| 05-architecture-and-robustness.md | §2.2 step 3: the input pre-processor "uses the same idea as slow-task and modal loops: FSlateApplication exposes a modal-loop tick event" | Different mechanism. Modal and slow-task loops block input at the **platform-message** layer (`ShouldProcessUserInputMessages`, SA:4886-4901; `WindowsApplication.cpp:3195-3205`), including `WM_CHAR`, and draw only the modal (SA:1476-1497). An `IInputProcessor` cannot see `KeyChar` (SA:4909-4950), synthetic mouse moves (SA:6399-6401), IME, drag-drop or window close. Our pre-processor needs read-only buffers and a `RegisterCanCloseEditor` veto as well. |
| 05-architecture-and-robustness.md | §2.6: "This looks purpose-built for script breakpoints such as the Blueprint debugger; which engine callers use it in 5.8 is UNVERIFIED" | Verified. The sole caller is `FKismetDebugUtilities::AttemptToBreakExecution` (`KismetDebugUtilities.cpp:817`), in PIE ("World") and non-PIE ("Engine") modes. `LeaveDebuggingMode` is also called by PIE-window close, editor close, Blueprint debugger commands, the BT debugger and Blueprint script exceptions (Q1). |
| 05-architecture-and-robustness.md | §2.6: "`FScopedPreventDebuggingMode`-style guards refuse to pause inside nested pumps or during PIE transitions" | The real class is public (`WITH_EDITOR`, SAH:953-961) and should be used directly in our pump. The engine uses it only for tab restore (`TabManager.cpp:2057`) and for self re-entry (SA:3892). It does not cover PIE transitions or modals; modals are a separate check (SA:3864). |
| 05-architecture-and-robustness.md | §2.6: "On pause it enters a restricted nested pump: `EnterDebuggingMode`, or our equivalent Slate-only tick loop" | `EnterDebuggingMode` has large side effects for Python. It makes `UObject::ProcessEvent` a no-op (`ScriptCore.cpp:2073-2083`), so evaluate/watch `unreal.*` calls silently return defaults. It refuses whenever a modal or slow-task dialog is open (SA:3864). It has no frame throttle. Prefer our own loop (Q6), with a blocking-modal fallback. |
| 05-architecture-and-robustness.md | Open question 2: "Does PythonScriptPlugin release the GIL during long reflected C++ calls?" | Yes: `Py_BEGIN_ALLOW_THREADS` around `ProcessEvent` in `PyUtil::InvokeFunctionCall` (`PSP/Private/PyUtil.cpp:645-647`). Consistent with 07a. |
| 05-architecture-and-robustness.md | Open question 3: semantics of `EnterDebuggingMode`; does a Slate-only tick repaint all windows safely mid-script? | Answered in Q1/Q2/Q6. All windows are repainted except a window being debugged from inside its own paint, or all but the modal when a modal is active. Hazards are the capture, ticker, delegate and modal cases listed in Q2. |
| 03-ide-features-lsp-dap.md | B8: "(world tick is suspended; Slate and Python Slate-tick callbacks may still run)" | World tick suspension is confirmed (no `GEngine->Tick` in the loop; `LevelTick.cpp:1509-1512`). Python Slate callbacks are broadcast but skipped while `GIsRunningUserScript` is set. Unmentioned and critical: every reflected `unreal.*` call is a silent no-op in this mode (`ScriptCore.cpp:2073-2083`), FTSTicker and the asset registry do not tick, and game-thread task-graph tasks do run (`RenderingThread.cpp:2562-2566`). |
| 03-ide-features-lsp-dap.md | Part D §4: "On stop it calls a C++ hook that enters `FSlateApplication::EnterDebuggingMode()`, so our Slate debugger panel stays interactive. Continue/step calls `LeaveDebuggingMode()`" | Only partly true. It is silently refused (void return) with any modal open, including an `unreal.ScopedSlowTask` dialog, inside a main-window widget Tick/Paint, during tab restore, or when nested (SA:3864-3891). It does not release Slate pointer capture, so a pause triggered from a button click leaves capture on that button (`SButton.cpp:442, 458-461`). Steps during PIE should use `LeaveDebuggingMode(true)` to avoid recapturing the PIE viewport (SA:3964-3979). |
| 03-ide-features-lsp-dap.md | Summary item 3: "UE 5.8 has exactly such a hook … the key enabler" | Qualified. The hook exists, but it is Blueprint-oriented and blocks `ProcessEvent`. The real enabler is the public mid-frame tick recipe used by slow tasks (Q6). `EnterDebuggingMode` is at best an option that needs an evaluation guard. |
| 04-unreal-python-internals.md | §1.3 and §2.2: "Only a `ScopedSlowTask` dialog pumps Slate, and only for that modal window"; "No UI repaints unless the script uses a slow task" | Refined. Any **blocking modal** the script opens (for example a message dialog via `AddModalWindow`) also runs a full Slate loop (SA:2218-2280). A slow task pumps only when the script calls `enter_progress_frame`/`should_cancel`/`tick_progress`, at most every 0.2 s (`FeedbackContext.cpp:184-197`). On Windows nothing pumps between calls (`GenericPlatformMisc.h:1957-1959`). |
| 04-unreal-python-internals.md | §2.4: callbacks are skipped "while a user script is on the stack" | Precise rule: the gate is `GIsRunningUserScript`. It is set by `EvalString`, by Python-implemented UFunction or delegate thunks and by `PostInitInstance`, but **not** by the Slate or ticker callback lambdas (`PySlate.cpp:28-46`, `PyCore.cpp:43-66`). If the running code is itself a Python tick callback, other Python tick callbacks, and the same one, run in nested ticks. |
| 04-unreal-python-internals.md | §2.2: "Whether the editor hang detector would otherwise fire is UNVERIFIED" | The heartbeat hang detector is compiled out of editor builds: `USE_HANG_DETECTION` requires `!WITH_EDITORONLY_DATA` (`Engine/Source/Runtime/Core/Public/Misc/Build.h:447`). The **stall detector** is compiled in for Windows editor builds (`StallDetector.h:20-25`), and blocking modals pause it (SA:2221). Long pauses and pumps should use `SCOPE_STALL_DETECTOR_PAUSE()`. The hitch detector was not checked. |
| 04-unreal-python-internals.md | §2.6: pumping a modal "Running… [Stop]" slow-task window "is the same re-entrancy profile as a script calling `enter_progress_frame`" | Confirmed (FCE:419-439; `ScopedSourceControlProgress.cpp:234-254`). Caveat: with that modal up, only the modal is drawn and interactive, so Stop and live output must live in the modal, not in our docked tab. This is a different profile from 05's no-modal, pre-processor pump, which needs an explicit `PumpMessages()` (SA:1705) and has no platform input filter. |
| 07a-verify-python-runtime.md | New risk 11: `EnterDebuggingMode` "refuses, and calls `UE_DEBUG_BREAK()`, … also ensures on re-entry" | Refinement. `UE_DEBUG_BREAK()` breaks only when a native debugger is attached (`GenericPlatformMisc.h:48`). In editor builds re-entry is refused through the prevent stack ("Already in debug mode", SA:3887-3892) before the `ensureMsgf` at SA:3910 is reached. |

---

## New risks

1. **Silent wrong values in the debugger.** In `EnterDebuggingMode`, every reflected `unreal.*` call returns defaults with no error (`ScriptCore.cpp:2073-2083` + `PyUtil.cpp:645-647`). Mitigation: our own pause loop, or `TGuardValue` on `GIntraFrameDebuggingGameThread` around each evaluation. Add a test: watch `unreal.SystemLibrary.get_engine_version()` while paused.
2. **Breakpoints inside `ScopedSlowTask` dialogs cannot use debugging mode, and our docked UI is dead while such a dialog is up.** UE scripts commonly wrap work in slow tasks with dialogs. Mitigation: a blocking modal debugger window parented to the active modal (Q3), carrying all debugger UI.
3. **Pointer capture survives into the pause** when the run started from a button click (`SButton.cpp:442, 458-461`; SA:5336). Mitigation: defer runs to a one-shot FTSTicker callback, and call `ReleaseAllPointerCapture()` before any pause.
4. **FTSTicker is non-reentrant** (`Ticker.cpp:99-102` `checkf`). Never tick the core ticker from our loops. Nothing on FTSTicker updates while paused, so our debugger UI must poll through Slate active timers or `OnPreTick` in C++.
5. **The shared `LeaveDebuggingMode` flag.** PIE-window close, editor close and Blueprint Resume end our pause if we use debugging mode (Q1 callers). The Python side must treat an early return as a "resume".
6. **`Renderer->Sync()` runs queued game-thread tasks** in every pump or pause iteration (`RenderingThread.cpp:2562-2566`). Arbitrary engine and plugin completions run mid-script.
7. **Python-bound delegates are ungated** in a custom pump (`PyWrapperDelegate.cpp:21-37`). UI that can trigger them (outliner, details, menus) must be blocked during a pump.
8. **Pre-processors miss `KeyChar`, IME, synthetic mouse moves and window close.** Read-only buffers plus a `RegisterCanCloseEditor` veto are required.
9. **No throttle in `EnterDebuggingMode`.** It can spin a core while Slate is asleep. Our loop should sleep to about 60 Hz, as `AddModalWindow` does (SA:2235-2248).
10. **A double RHI `EndFrame` when pausing inside a Slate tick.** `EnterDebuggingMode` does not guard with `IsTicking()` (SA:3927, 3945), unlike the slow-task code. Our loop must use the guarded `EndFrame`.
11. **GIL starvation during the pause.** The pause runs inside the trace function with the GIL held. Release it around the loop.
12. **Worker-thread breakpoints.** `EnterDebuggingMode` and `Tick` are game-thread only (SA:3817, SA:1629). Breakpoints in Python threads must block that thread instead, and the UI must show those threads as "paused while the editor runs".
13. **Developer annoyance.** With Visual Studio attached, each refused `EnterDebuggingMode` triggers `UE_DEBUG_BREAK()` (SA:3839-3860). The `static volatile bool bDoDebugBreak` cannot be turned off from code.
14. **Breakpoints inside Python tick callbacks re-run the paused callback.** Every nested frame calls it again (Q2c). Mitigation: skip breakpoints, or refuse to pause, when the paused frame was entered from a Slate or ticker callback. Detect this with our own flag set by a wrapper, since `GIsRunningUserScript` is not readable.

---

## Files read

**Research:**
- `docs/research/03-ide-features-lsp-dap.md` (summary, B8, Part D §4, open questions);
- `04-unreal-python-internals.md` (§1.3, §2.1-2.6, risk table);
- `05-architecture-and-robustness.md` (§0, §2.2, §2.6, risks, open questions);
- `02-other-dcc-and-engine-editors.md` (open question 1);
- `06-user-voice-and-landscape.md` (grep);
- `07a-verify-python-runtime.md` (summary, Q7, corrections, risks).

**Engine (all under `C:\Program Files\Epic Games\UE_5.8\Engine\`):**

*Slate:*
- `Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp` (1200-1540, 1610-1860, 2066-2330, 2806, 3250-3290, 3780-4105, 4886-4990, 5300-5340, 6130-6150, 6385-6406);
- `Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h` (167-230, 388-406, 569-573, 796-806, 900-980, 1180-1283, 1416, 1541-1590, 1844-1851, 2030-2075, 2205-2250);
- `Source/Runtime/Slate/Public/Framework/Application/IInputProcessor.h`;
- `Source/Runtime/Slate/Private/Framework/Docking/TabManager.cpp` (2045-2065);
- `Source/Runtime/Slate/Private/Widgets/Input/SButton.cpp` (403-465);
- `Source/Runtime/SlateCore/Public/Rendering/SlateRenderer.h` (access levels, 443-455);
- `Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp` (1837-1856).

*Editor:*
- `Source/Editor/UnrealEd/Private/Kismet2/KismetDebugUtilities.cpp` (420-480, 690-850);
- `Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp` (250-280);
- `Source/Editor/UnrealEd/Private/PlayLevel.cpp` (3395-3420);
- `Source/Editor/MainFrame/Private/Frame/MainFrameHandler.h` (70-150);
- `Source/Editor/MainFrame/Public/Interfaces/IMainFrameModule.h` (226-238);
- `Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp` (1-70, 180-200, 376-400, 415-764);
- `Source/Editor/UnrealEd/Public/FeedbackContextEditor.h`;
- `Source/Editor/UnrealEd/Private/EditorEngine.cpp` (1933, 4157-4173);
- `Source/Editor/ContentBrowserData/Private/ContentBrowserDataSubsystem.cpp` (197).

*Core and engine runtime:*
- `Source/Runtime/Core/Private/Misc/SlowTask.cpp` (all);
- `Source/Runtime/Core/Public/Misc/ScopedSlowTask.h`;
- `Source/Runtime/Core/Public/Misc/SlowTask.h` (103);
- `Source/Runtime/Core/Private/Misc/FeedbackContext.cpp` (180-280);
- `Source/Runtime/Core/Public/Misc/FeedbackContext.h` (50-100);
- `Source/Runtime/Core/Private/Containers/Ticker.cpp` (55-150);
- `Source/Runtime/Core/Public/Delegates/MulticastDelegateBase.h` (285-315, 450-480);
- `Source/Runtime/Core/Public/Delegates/DelegateAccessHandler.h` (100-175);
- `Source/Runtime/Core/Public/HAL/CriticalSection.h` (53);
- `Source/Runtime/Core/Public/UObject/ScriptDelegates.h` (980-1016);
- `Source/Runtime/Core/Public/CoreGlobals.h` (205-222);
- `Source/Runtime/Core/Private/Misc/CoreGlobals.cpp` (197-203);
- `Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMisc.h` (46-48, 1953-1959);
- `Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp` (4437-4445);
- `Source/Runtime/Core/Private/HAL/ThreadHeartBeat.cpp` (90-110, 380-432);
- `Source/Runtime/Core/Public/Misc/Build.h` (444-447);
- `Source/Runtime/Core/Public/ProfilingDebugging/StallDetector.h` (15-67);
- `Source/Runtime/CoreUObject/Private/UObject/ScriptCore.cpp` (2055-2090);
- `Source/Runtime/Engine/Private/LevelTick.cpp` (1500-1520);
- `Source/Runtime/Engine/Private/Slate/SGameLayerManager.cpp` (425-445);
- `Source/Runtime/Engine/Private/Commandlets/Commandlet.cpp` (130-150);
- `Source/Runtime/AssetRegistry/Private/AssetRegistry.cpp` (4895-4915);
- `Source/Runtime/RenderCore/Private/RenderingThread.cpp` (710-740, 1158-1316, 2440-2573).

*Launch and platform:*
- `Source/Runtime/Launch/Private/LaunchEngineLoop.cpp` (263, 2745-2760, 4730-4744, 4936, 5575, 5780-5790, 5905-6115);
- `Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp` (3190-3205, 4109-4123, 4180-4245);
- `Source/Runtime/ApplicationCore/Private/Windows/WindowsPlatformApplicationMisc.cpp` (215-260).

*Developer tools:*
- `Source/Developer/OutputLog/Private/SOutputLog.cpp` (295-315);
- `Source/Developer/ToolMenus/Private/ToolMenus.cpp` (2488-2498);
- `Source/Developer/SourceControl/Private/ScopedSourceControlProgress.cpp` (215-255);
- `Source/Developer/TranslationEditor/Private/TranslationPickerFloatingWindow.cpp` (grep: 56-153, 198, 244).

*PythonScriptPlugin (PSP):*
- `Private/PySlate.cpp` (20-160);
- `Private/PyCore.cpp` (25-120, 246-285, 571-800, 2020-2045);
- `Private/PyUtil.cpp` (50-58, 600-660, 1815-1840);
- `Private/PyUtil.h` (30-40);
- `Private/PythonScriptPlugin.cpp` (1810-1870);
- `Private/PyGenUtil.cpp` (1278-1395);
- `Private/PyWrapperDelegate.cpp` (15-45);
- `Private/PyWrapperObject.cpp` (2020-2045);
- `Public/IPythonScriptPlugin.h`.

**Greps:**
- `EnterDebuggingMode|LeaveDebuggingMode|FScopedPreventDebuggingMode`
- `GIntraFrameDebuggingGameThread|GFirstFrameIntraFrameDebugging`
- `ModalLoopTickEvent`
- `StarvedGameLoop`
- `GetNormalExecutionAttribute|IsNormalExecution()` (61 hits)
- `public IInputProcessor`
- `FSlateApplication::Get().Tick(|PumpMessages()`
- `GIsRunningUserScript`
- `CanTickPython`
- `ProcessEvent(` in PSP
- `InvokeFunctionCall`
- `Py_BEGIN_ALLOW_THREADS` in PSP
