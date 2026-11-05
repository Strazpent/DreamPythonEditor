> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 07a — Verification: PythonScriptPlugin runtime internals (UE 5.8 source)

Verification lane, 2026-10-28. Every claim below was checked against the local UE 5.8 install at `C:\Program Files\Epic Games\UE_5.8` (read only; nothing under it was modified). Engine evidence is cited as `Engine/<relative path>:<line>`.

Two path prefixes recur. They are written out in full the first time and shortened afterwards:

- **PSP** = `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/`
- **PSPC** = `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/`

There is also one empirical probe. The engine's own `python311.dll` was embedded in a throwaway host process (PowerShell + C# P/Invoke) with `Py_InitializeEx(0)`, which matches UE's `install_signal_handlers = 0`. It ran a script that tests interrupt, pending-call, async-exception and stdout-buffering behaviour. The script and output are in the session scratchpad: `C:\Users\anima\AppData\Local\Temp\claude\C--Users-anima-Desktop-reactionchess\cef630df-881f-4396-849f-f12554ae0865\scratchpad\pyprobe\` (`probe.py`, `run_probe.ps1`). Results are quoted where relevant as **[Probe]**. The probe runs CPython 3.11.8 exactly as shipped, but not inside the editor, so in-editor confirmation is still listed as a follow-up where it matters.

---

## Summary

1. **Init.** CPython 3.11.8 is initialized on the **game thread** from `OnPostEngineInit`, using `Py_PreInitialize` + `PyConfig_InitIsolatedConfig` + `Py_InitializeFromConfig`. The config has **`install_signal_handlers = 0`**. The plugin has **no** interrupt or cancel mechanism: no `PyErr_SetInterrupt*`, `Py_AddPendingCall`, `PyThreadState_SetAsyncExc`, `KeyboardInterrupt` or `PyErr_CheckSignals` anywhere. The only cancel is cooperative (`ScopedSlowTask.should_cancel`).
   - **[Probe]** With `initsigs=0`, `PyErr_SetInterruptEx(SIGINT)` does nothing useful until something imports `signal`. Importing `signal` on the main thread *automatically* installs `default_int_handler`, and after that the interrupt works.
   - **[Probe]** `Py_AddPendingCall` from a worker thread runs on the initializing thread, which is the game thread in UE.
   - **Corrected 2026-10-28 (RV-01).** That worker held the GIL. A pending call posted from a thread that does not hold the GIL is not noticed by a pure-Python loop on 3.11; see the note under Q1.
   - **[Probe]** `PyThreadState_SetAsyncExc` interrupts a pure-Python loop in about 0.3 s.
2. **GIL.** The GIL is released after init (`PyEval_SaveThread`) and taken per entry point (`FPyScopedGIL` = `PyGILState_Ensure`). The plugin **releases the GIL around every reflected UFunction call** (`Py_BEGIN_ALLOW_THREADS` around `ProcessEvent`). It also releases it around find/load object, `unreal.log*`, GC, property-change notifications and class finalization.
   - Game-thread enforcement is one check in `PyUtil::InvokeFunctionCall`, which raises `RuntimeError("Attempted to access Unreal API from outside the main game thread.")`.
   - Property access and `find/load_object` are **not** gated.
   - The source cannot date when the check was added. The "5.6" date in the research still rests on the forum thread alone.
3. **Public API (5.8).** `IPythonScriptPlugin` exposes `ExecPythonCommand[Ex]` and Is/Force enable/disable. It has three delegates (Configured, Initialized, Shutdown) with `RegisterOn…` helpers, plus `GetInterpreterExecutablePath` and the startup-script allow-path functions.
   - `FPythonCommandEx` has these fields: `Flags`, `ExecutionMode`, `FileExecutionScope`, `Command`, `CommandResult`, `LogOutput`.
   - **Literal code always runs in the shared console dict.** The console dict is a *copy* of `__main__.__dict__` made at init. `FileExecutionScope` applies only to `.py` file runs.
   - `EvaluateStatement` does **not** print. It returns `repr()` in `CommandResult`. `ExecuteStatement` (`Py_single_input`) prints through CPython's default `sys.displayhook`.
4. **Output.** `sys.stdout` and `sys.stderr` are Python `io.TextIOWrapper(line_buffering=True)` objects over a Python `_Logger(io.BufferedIOBase)`, which calls the C functions `unreal.log` / `unreal.log_error` → `UE_LOGF(LogPython, …)`.
   - Output is **flushed per line**. **[Probe]**: one write and one flush per `print`.
   - Each flush calls `GLog->Flush()`, a synchronous hand-off to the log file writer, on every printed line.
   - The complaint "output only appears after the script ends" is caused by **Slate not ticking**, not by Python buffering. `SOutputLog::Serialize` only queues messages; `SOutputLog::Tick` moves them into the view. The only real buffering is of partial lines with no newline.
5. **Console executors.** Two are registered: `"Python"` (display "Python", `ExecuteFile`) and `"PythonREPL"` (display **"Python (REPL)"**, `ExecuteStatement`). Both have **empty completion**. The engine adds `"Cmd"`.
   - `IConsoleCommandExecutor` is a public `IModularFeature`. A third-party plugin can register its own executor. The Output Log picks it up live and filters it through a permission list.
6. **Delegates and version.** `OnPythonInitialized` fires at the end of `RunStartupScripts`, after every `init_unreal.py`, on the first core-ticker tick. `OnPythonShutdown` fires inside `ShutdownPython` with the GIL held, before `Py_Finalize`. **Both delegates are `Clear()`ed after broadcasting.** The bundled Python is **3.11.8** (NEWS.txt, `patchlevel.h`, `python311.dll`).
7. **Slate tick callbacks.** `unreal.register_slate_pre/post_tick_callback` add lambdas to `FSlateApplication::OnPreTick/OnPostTick`. Every lambda first checks **`PyUtil::CanTickPython()`**, which returns false while `GIsRunningUserScript` is true. `EvalString` sets that flag for the whole run of any `ExecPythonCommandEx`.
   - So **foreign Python tick callbacks are suppressed automatically during our mid-script pump**, provided the user code was entered through `ExecPythonCommandEx`.
   - Delegates bound to Python callables through `unreal` multicast delegates are not gated.
8. **Tooling.** `unreal.get_interpreter_executable_path()` returns `<UE_PYTHON_DIR>/python.exe`. `debugpy_unreal.py` ships but debugpy itself does not; its `install_debugpy()` pip-installs into the engine's own Python.
   - `remote_execution.py` is protocol v1 with magic `ue_py`, types `ping`/`pong`/`open_connection`/`close_connection` (UDP) and `command`/`command_result` (TCP).
   - The multicast group is `239.0.0.1:6766`. 5.8 defaults: bind `127.0.0.1`, TTL `0`, command endpoint `127.0.0.1:6776`, and the feature is **off**.
   - It has **no authentication**, and returns one result per command after completion.

---

## Q1. CPython initialization, thread, signal handling, existing interrupt mechanisms

**Answer.**

**Where and when.**
- `FPythonScriptPlugin::StartupModule` hooks `FCoreDelegates::GetOnPostEngineInit()`. `OnPostEngineInit` calls `ConfigureAndInitializePython()` → `InitializePython()`.
- `OnPostEngineInit` is broadcast from `FEngineLoop::Init` on the game thread.
- `ForceEnablePythonAtRuntime()` can trigger the same path later, for example from the "Enable Python" menu entry, which also runs on the game thread.
- So the thread that calls `Py_PreInitialize`, and therefore CPython's "main thread", is the **game thread**.

**How (Python ≥ 3.11 branch, the one compiled for 5.8).**
- `PyPreConfig_InitIsolatedConfig`, then `parse_argv=0`, `utf8_mode=1`, and `isolated` / `use_environment` from `bIsolateInterpreterEnvironment` (default **true**). Then `Py_PreInitialize`.
- `PyConfig_InitIsolatedConfig(&ModulePyConfig)`, then `program_name`, `home` (= `UE_PYTHON_DIR`), `stdio_encoding="utf-8"`, **`install_signal_handlers = 0`**, `safe_path = 0`. Then `Py_InitializeFromConfig`.
- The `#else` branch for Python < 3.11 uses `Py_InitializeEx(0); // 0 so Python doesn't override any signal handling`. It is not compiled for 3.11.
- At the end, `PyMainThreadState = PyEval_SaveThread()` releases the GIL.

**Signal handler.** None is installed at init, and the plugin never touches signals.
- **[Probe]** Embedding the shipped `python311.dll` with `initsigs=0`:
  - `signal` is not imported after startup, and `_thread.interrupt_main()` (= `PyErr_SetInterruptEx(SIGINT)`) **does not raise**. CPython prints an unraisable `OSError: Signal 2 ignored due to race condition`, which in UE would land in LogPython as an error.
  - After a plain `import signal` on the main thread, `signal.getsignal(SIGINT)` is `default_int_handler`. CPython's `_signal` module exec installs it when the C-level handler is `SIG_DFL`, and UE on Windows uses `SetConsoleCtrlHandler`, not `signal()`. From then on, `interrupt_main()` raises `KeyboardInterrupt` in about 0.31 s.
  - So **whether `PyErr_SetInterrupt` works in a live editor depends on whether anything has imported `signal`**. `subprocess` and `asyncio` both import it.

**Existing interrupt or cancel mechanisms.** No preemptive mechanism exists.
- The searches returned only the init lines above: `PyErr_SetInterrupt`, `PyErr_SetInterruptEx`, `Py_AddPendingCall`, `PyThreadState_SetAsyncExc`, `KeyboardInterrupt`, `PyErr_CheckSignals` and `SIGINT` over PSP and PSPC.
- What exists is cooperative only: `unreal.ScopedSlowTask` with `make_dialog(can_cancel=…)` and `should_cancel()`.
- The `-ExecutePythonScript` notification's Cancel button only calls `RequestExit()` between ticks.

**Console Ctrl+C clash (research concern).** In non-shipping x86 builds that have a console, UE **patches `SetConsoleCtrlHandler`** to a no-op after registering its own handler, "so no one else can register a handler". So the CRT handler that Python's `signal.signal(SIGINT, …)` would register cannot steal Ctrl+C from UE in `-Cmd` runs. The GUI editor has no console, so the patch is skipped, but there are also no console Ctrl+C events.

**Evidence.**
- `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:942` (hook PostEngineInit), `:986` (`ConfigureAndInitializePython()`), `:1011` (`InitializePython()`), `:1133` (`"Using Python %d.%d.%d"`), `:1144-1158` (PreConfig), `:1161-1162`, `:1176-1179`:
  ```cpp
  ModulePyConfig.install_signal_handlers = 0;
  ModulePyConfig.safe_path = 0;
  ```
  `:1181` (`Py_InitializeFromConfig`), `:1191` (`Py_InitializeEx(0)` for < 3.11 only), `:1396` (`PyMainThreadState = PyEval_SaveThread();`).
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:4062-4064` (`OnPostEngineInit.Broadcast()` in `FEngineLoop::Init`).
- PSP `Private/PythonScriptPluginSettings.h:81` (`bIsolateInterpreterEnvironment = true`).
- PSP `Private/EditorUtilities/EditorPythonExecuter.cpp:66-70` (Cancel → `RequestExit()`); PSP `Private/PyCore.cpp:649-690` (slow-task `make_dialog_delayed(…, can_cancel …)`).
- `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1172-1205` (`SetGracefulTerminationHandler`, patch of `SetConsoleCtrlHandler`, `!UE_BUILD_SHIPPING && PLATFORM_CPU_X86_FAMILY`, skipped when `GetConsoleWindow()==nullptr`).
- Headers present for our use: `Engine/Source/ThirdParty/Python3/Win64/include/ceval.h:46` (`Py_AddPendingCall`), `include/pyerrors.h:234,236` (`PyErr_SetInterrupt`, `PyErr_SetInterruptEx`), `include/pystate.h:67` (`PyThreadState_SetAsyncExc`).
- **[Probe]** results: `1_interrupt_main_before_signal_import: "no interrupt"`, `2_sigint_handler_after_import: default_int_handler`, `2_interrupt_main_after_import: "KeyboardInterrupt after 0.31s"`, `4_pending_call: {add_rc: 0, ran_on_main: true}`, `5_async_exc: "ScriptCancelled after 0.32s (rc=1)"`, `3b_bare_except_swallowed_count: 1`.

**Confidence.**
- High for the source facts (init path, thread, `install_signal_handlers=0`, no cancel mechanism).
- High for the CPython 3.11.8 behaviour (probe on the shipped DLL).
- Medium for "in a running editor, `signal` may or may not already be imported": that is runtime state, so check it at startup.

**Note added 2026-10-28 (v1.0 review, [RV-01](08-decision-log.md#9-amendments-after-the-v10-review-rv-01--rv-24)).** The `4_pending_call` probe above posted from a Python thread through `ctypes.pythonapi`, so the poster held the GIL. It recorded only `add_rc` and `ran_on_main`, never latency. In CPython 3.11 a pending call posted from a thread other than the main thread does not set the eval breaker. The pending-call bit is computed on the posting thread, and it is gated by `_Py_ThreadCanHandlePendingCalls()`, which is `_Py_IsMainThread()` (`Engine/Source/ThirdParty/Python3/Win64/include/internal/pycore_pystate.h:57-61`). The main thread notices the call only when it next takes the GIL. A pure-Python loop never re-takes the GIL by itself, so **a pending call posted from a thread that does not hold the GIL is not noticed by a pure-Python loop** until the loop ends.
- **[Probe]** [`probes/pending_native.py`](probes/pending_native.py), on the shipped 3.11.8 `python.exe -I -B`: a native timer-queue thread with no thread state posts during a 2 s pure-Python loop. Result: `fired_during_loop_at_s` null, `fired_after_loop` true. With `time.sleep(0)` in the loop body, which releases and re-takes the GIL, the call fired at 0.308 s.
- **[Probe]** [`probes/pending_native_fix.py`](probes/pending_native_fix.py): a poster that takes the GIL (a ctypes callback) fired inside the pure-Python loop at 0.325 s.
- **Design consequence.** After every successful post, the watchdog forces one GIL hand-off: it takes the GIL on its own thread state and releases it at once. That costs one switch interval (5 ms) rounded up to the OS timer resolution: about 5–7 ms under the editor's 1 ms resolution (`timeBeginPeriod(1)`, `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1076`), and about 16 ms at Windows' default 15.6 ms resolution, which Windows 11 may apply to a minimised or occluded editor per post (TDD §3.2 GIL-4, §5.4.1). `PyThreadState_SetAsyncExc` is not a liveness mechanism; the hand-off is.

**Implication for our design.**
- The **`Py_AddPendingCall` pump is viable** and should be the primary Stop path: it needs no GIL, it runs on the game thread, and it needs no signal handler.
  - *Amended 2026-10-28 (RV-01):* posting needs no GIL, but the game thread notices a post only when it next takes the GIL. The poster must therefore force a GIL hand-off after each post (note above).
  - Implement the callback in C++, and **set an exception before returning -1**. In the probe, a callback that returned -1 with no exception set (ctypes cleared it) aborted the loop with `SystemError: error return without exception set`. So -1 does abort, but it must carry our `ScriptCancelled`.
  - Keep **at most one pending call outstanding** behind an atomic flag. CPython's pending-call queue is small (32 slots in 3.11, from CPython source knowledge; not verifiable from the shipped headers), and it does not drain while the game thread is inside a native call.
- **`PyThreadState_SetAsyncExc` works as the fallback.** Clear it with `SetAsyncExc(tid, NULL)` if the run ends before delivery.
- **SIGINT is optional.** If we use it, install our *own* Python SIGINT handler that raises only for the current run ID, rather than relying on `default_int_handler`. A stale trip would otherwise raise `KeyboardInterrupt` in whatever Python runs next.
  - Never call `PyErr_SetInterruptEx` before a handler exists, because it logs a spurious error.
- The console-Ctrl+C clash is not a blocker on Win64 editor builds.

---

## Q2. GIL handling and game-thread enforcement

**Answer.**

**GIL model.**
- After init the GIL is released (`PyEval_SaveThread`). Every C++ entry point takes it on demand with `FPyScopedGIL` (`PyGILState_Ensure` / `PyGILState_Release`).
- `RunString` / `RunFile` hold the GIL for the whole user run.
- `TPyAutoGILPtr` lets delegates and lambdas safely hold Python objects without the GIL.

**The GIL is released during reflected C++ calls.**
- `PyUtil::InvokeFunctionCall`, the single funnel for UFunction calls from Python (methods, static functions, struct operators, constant getters, break functions), wraps `InObj->ProcessEvent(...)` in `Py_BEGIN_ALLOW_THREADS … Py_END_ALLOW_THREADS`.
- The GIL is also released around:
  - `StaticFindObject` / `StaticLoadObject` / `StaticLoadClass` / `FindPackage` / `LoadPackage` (`find_object`, `load_object`, `find_asset`, `load_asset`, …);
  - `UE_LOGF` in `unreal.log/log_warning/log_error` and `GLog->Flush()` in `log_flush`;
  - `CollectGarbage`;
  - `FlushGeneratedTypeReinstancing`;
  - property pre/post-change notifications;
  - class finalization and GC in Python-generated class building;
  - enum and struct generation;
  - boot-time multithreaded wrapper generation.
- **Not released**: plain property get/set without change notification, and the conversion code.

**Game-thread enforcement.**
- `InvokeFunctionCall` begins with:
  ```cpp
  if (!IsInGameThread())
  { ... SetPythonError(PyExc_RuntimeError, ..., TEXT("Attempted to access Unreal API from outside the main game thread.")); return false; }
  ```
  The comment explains the reason: `FEditorScriptExecutionGuard` "isn't thread-safe", and threads that touch the API "will always hit this case. Instead of crashing, raise a Python Exception".
- Other checks are `check(IsInGameThread())` in pip-install helpers, in `FMultithreadedGenerationContext` (boot) and in `FSlateApplication::Tick`.
- **There is no thread check** on property access (`get/set_editor_property`, attribute access) or on `find/load_object`. These still "work" off-thread and are unsafe.

**When it was tightened.** The 5.8 source contains the check but no version marker or comment. No older engine source is installed, and there is no git history. The "5.6" date comes only from the research's forum citation [06: S43]. It is consistent with the code but **not verifiable locally**.

**Evidence.**
- PSP `Private/PyGIL.h:11-33` (`FPyScopedGIL` = `PyGILState_Ensure`/`Release`), `:35-134` (`TPyAutoGILPtr`).
- PSP `Private/PythonScriptPlugin.cpp:1394-1396` (release after init; the comment says "all other code will lock on-demand via FPyScopedGIL"), `:1485-1486` (re-acquired before `Py_Finalize`), `:1845` and `:1937` (GIL held across `RunString`/`RunFile`).
- PSP `Private/PyUtil.cpp:606-616` (thread check), `:644-647`:
  ```cpp
  FEditorScriptExecutionGuard ScriptGuard;
  Py_BEGIN_ALLOW_THREADS
  InObj->ProcessEvent((UFunction*)InFunc, InBaseParamsAddr);
  Py_END_ALLOW_THREADS
  ```
- Callers of `InvokeFunctionCall`: PSP `Private/PyWrapperObject.cpp:198,258,267,314,393`, `Private/PyWrapperStruct.cpp:491,522,592,663`, `Private/PyGenUtil.cpp:552,2550`, `Private/PyWrapperTypeRegistry.cpp:3596`.
- Other releases: PSP `Private/PyCore.cpp:1184-1186, 1204-1206, 1224-1226` (log), `:1240-1242` (log_flush), `:1431-1445, 1488-1490, 1562-1619` (find/load), `:1770-1781` (reinstancing, GC); `Private/PyConversion.cpp:1251-1258` (change notify); `Private/PyWrapperObject.cpp:1396-1501` (class finalize); `Private/PyWrapperTypeRegistry.cpp:974,1063` (boot generation).
- `check(IsInGameThread())`: PSP `Private/PyGenUtil.cpp:98`, `Private/PipInstallHelpers.cpp:235,250,291`; `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:1629`.

**Confidence.** High for the GIL behaviour and the enforcement point. Low for the *date* of tightening.

**Implications.**
- (a) **Background Python threads run during every UFunction call**, because the GIL is released there. A C++ watchdog can take the GIL at those moments, for example for `PyThreadState_SetAsyncExc`. Delivery still waits until control returns to Python bytecode.
- (b) Our "worker lane" for pure-Python work (I/O, REST) is legal, but it must marshal every `unreal.*` call back to the game thread.
  - Use a queue drained from a ticker or from our pump; there is no `call_on_game_thread` API.
  - Our linter should flag `unreal.*` property access inside `threading` targets, because it is unguarded.
- (c) Our Slate pump must run on the game thread (`FSlateApplication::Tick` checks). Pending calls run there.

---

## Q3. The 5.8 `IPythonScriptPlugin` interface, scopes and result printing

**Answer.**

**Interface (complete).**
- `Get()`, `IsPythonAvailable()`, `IsPythonConfigured()`, `IsPythonInitialized()`.
- `ForceEnablePythonAtRuntime(UE::FSourceLocation)`, `ForceDisablePythonAtRuntime(UE::FSourceLocation)`.
- `ExecPythonCommand(const TCHAR*)`, `ExecPythonCommandEx(FPythonCommandEx&)`, `GetInterpreterExecutablePath()`.
- `OnPythonConfigured()`, `OnPythonInitialized()`, `OnPythonShutdown()`.
- Inline `RegisterOnPythonConfigured(FSimpleDelegate&&)`, `RegisterOnPythonInitialized(FSimpleDelegate&&)`, which call immediately if the state has already been reached.
- `AddStartupScriptAllowPath(FName Owner, const FString& Path)`, `RemoveStartupScriptAllowPath(FName Owner)`.

**Types (`PythonScriptTypes.h`).**
- `EPythonLogOutputType {Info, Warning, Error}`.
- `EPythonCommandFlags {None=0, Unattended=1<<0}`. `Unattended` sets `GIsRunningUnattendedScript` for the run.
- `EPythonCommandExecutionMode {ExecuteFile, ExecuteStatement, EvaluateStatement}`.
- `EPythonFileExecutionScope {Private, Public}`.
- `FPythonLogOutputEntry {EPythonLogOutputType Type = Info; FString Output;}` (USTRUCT).
- `FPythonCommandEx { Flags = None; ExecutionMode = ExecuteFile; FileExecutionScope = Private; FString Command; FString CommandResult; TArray<FPythonLogOutputEntry> LogOutput; }`.
- `LexToString` / `LexTryParseString` for the mode; remote execution uses these strings.

**Execution.**
- `ExecuteFile` mode first sniffs for a `.py` path. If it finds one it calls `RunFile`; otherwise `RunString`. The sniffing gotcha in research 04 §1.5 is confirmed.
- Modes map to `Py_file_input`, `Py_single_input` and `Py_eval_input`.
- `RunString` compiles with filename `"<string>"` and always evaluates in **`PyConsoleGlobalDict`**, whatever `FileExecutionScope` says.

**Dict creation.**
- `PyDefaultGlobalDict` = `__main__.__dict__` (a live reference).
- `PyConsoleGlobalDict` = **`PyDict_Copy(__main__.__dict__)`** taken at init. Then `unreal` is inserted into it.
- `RunFile` with `Public` uses the console dict.
- With `Private`, it uses a fresh `PyDict_Copy(PyDefaultGlobalDict)`, a copy of `__main__` at run time.
- `__file__` is set for the run and deleted afterwards. `sys.argv` is set from the arguments and reset to `[""]`.
- Startup `init_unreal.py` files run in `Public` scope.
- So the "console namespace" is **not** `__main__`: `import __main__` does not see console variables. Because it is a copy, `__name__ == "__main__"` inside it.

**How results print.**
- `EvaluateStatement` does **not** print. `CommandResult = repr(result)` via `PyObjectToUEStringRepr`.
- `ExecuteStatement` (the "Python (REPL)" executor) uses `Py_single_input`. CPython itself prints non-`None` expression values through the **default `sys.displayhook`** (UE does not override it) to `sys.stdout` → LogPython, and sets `builtins._`. `CommandResult` is then `"None"`.
- On failure, `CommandResult` is the formatted traceback.
- Tracebacks also go to `LogPython` Error line by line through `UE_LOGF`, not through `sys.stderr`.
- For `RunString`, tracebacks are **not** added to `LogOutput`. `RunFile` adds them.
- `SystemExit(0/None)` is swallowed.
- A user-replaced `sys.excepthook` is called as well.

**`LogOutput` capture.**
- `RunString`/`RunFile` subscribe a lambda to the private `PyCore::GetPythonLogCapture()` delegate for the duration of the eval.
- The delegate is fired by `unreal.log/log_warning/log_error`, so by `print`, stderr and warnings.
- It is private (PSP `Private/PyCore.h:401-402`), so third parties cannot subscribe live.
- A Blueprint/Python wrapper exists: `unreal.PythonScriptLibrary.execute_python_command_ex(cmd, mode, scope)`, which returns `(ok, result, log_output)`.

**Evidence.**
- `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Public/IPythonScriptPlugin.h:15-123`.
- PSP `Public/PythonScriptTypes.h:12-21, 24-32, 35-44, 47-54, 57-69, 72-91, 109-143`.
- PSP `Private/PythonScriptPlugin.cpp:786-791` (`ExecPythonCommand` → Ex with defaults), `:793-913` (Ex, `.py` sniffing, `RunFile` vs `RunString`), `:1246-1257` (`__main__`, `PyDefaultGlobalDict`, `PyConsoleGlobalDict = PyDict_Copy(...)`), `:1304` (`unreal` into the console dict), `:1814-1817` (`EvalString` defaults to the console dict), `:1823-1836` (`Py_CompileString` + `PyEval_EvalCode`, `GIsRunningUserScript` guard at `:1831`), `:1849-1861` (mode mapping), `:1865-1877` (`"<string>"`, capture, `repr` into `CommandResult`), `:1940-1950` (Private vs Public dicts, `__file__`), `:1960-1981` (`RunFile` capture and error lines into `LogOutput`), `:1636-1641` (startup scripts run Public).
- PSP `Private/PyUtil.cpp:102-110` (`PyObjectToUEStringRepr`), `:1603-1750` (`FetchPythonError`: `traceback.format_exception`, `SystemExit` swallow `:1620-1648`, excepthook `:1735-1748`), `:1753-1777` (`LogPythonError` → `UE_LOGF(LogPython, Error)` per line).
- PSP `Private/PythonScriptLibrary.h:60-61` (`ExecutePythonCommandEx` UFUNCTION).
- No `displayhook` override: grep over PSP and PSPC finds only the excepthook code.

**Confidence.** High.

**Implication.**
- Do not use `RunString` semantics for editor buffers: they bring `"<string>"` filenames, `.py` sniffing and the console namespace.
- Enter user code as `ExecPythonCommandEx({EvaluateStatement, "__se_runner.run(<run_id>)"})`. Our runner module compiles the buffer itself, with a real or virtual filename, `linecache` registration and our chosen namespace.
- Going through `ExecPythonCommandEx`/`EvalString` is what sets `GIsRunningUserScript` (Q7), which we need, plus the encoding guard and the eval-stack context.
- To share the console namespace with the Output Log, capture it once from Python: a statement run through `ExecPythonCommandEx` sees `globals()` = the console dict.
- Our "Session" namespace can be either that dict or our own.

---

## Q4. stdout/stderr redirection, buffering and the "output appears at the end" complaint

**Answer.**

**There is no C++ stream type.**
- `unreal_core.py`, imported at init via `ImportUnrealModule("core")`, replaces the streams with Python objects:
  - `sys.stdout = io.TextIOWrapper(_Logger('utf-8', log, log_flush), encoding='utf-8', line_buffering=True, write_through=_unbuffered_stdout)`;
  - `sys.stderr` uses `log_error`;
  - `warnings.showwarning` goes to `log_warning`.
- `_Logger(io.BufferedIOBase).write(b)` calls `unreal.log(b.decode())`, and `flush()` calls `unreal.log_flush()`.
- `unreal.log` is the C function `PyCore::Log`: `UE_LOGF(LogPython, Log, …)` with the GIL released, then `GetPythonLogCapture().Broadcast(Info, …)`. stderr maps to **Error** verbosity.
- `PYTHONUNBUFFERED` (read from `os.environ`, so it works even in isolated mode) switches the wrapper to write-through.

**Flush granularity.** It is per line.
- **[Probe]** with the same `TextIOWrapper` setup:
  - `print('a')` → `write("a\r\n")` and 1 flush;
  - `print('b', end='')` → nothing yet;
  - `write('c\nd\ne')` → `write("bc\r\nd\r\ne")` and 1 flush;
  - 5 prints → 5 writes and 5 flushes.
- On Windows, newlines become `\r\n`.
- Whitespace-only payloads are dropped (`ShouldLog`), so `print()` blank lines **vanish** from LogPython and `LogOutput`.
- Every flush calls **`GLog->Flush()`**. That flushes every output device. The file device's `FAsyncWriter::FlushBuffer` **spin-waits** until the writer thread has serialized the buffer. So each printed line costs a synchronous log-thread hand-off and file write on the game thread.

**Root cause of the complaint.** It is not Python buffering.
- `SOutputLog::Serialize` only appends to `PendingMessages` under a lock.
- `SOutputLog::Tick` calls `SubmitPendingMessages()` to move them into the text layout.
- While a script blocks the game thread, Slate never ticks, so nothing appears until the script returns. The disk log is up to date line by line.
- Exceptions to "per line":
  - partial lines, i.e. text without a newline;
  - `FPythonCommandEx::LogOutput`, which is handed back only when the call returns;
  - remote execution `command_result`, which arrives after completion.
- Slow tasks that show a dialog *do* tick Slate (`FFeedbackContextEditor::TickSlate`), which is why output sometimes appears mid-run.
- In the editor, log lines reach buffered devices through a dedicated primary log thread (`GLog->TryStartDedicatedPrimaryThread()`), but display still waits for the widget's Tick.

**Evidence.**
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/unreal_core.py:33-49` (`_Logger`), `:51-59` (comment "the payload is buffered, up to an end line" and the two `TextIOWrapper` lines), `:61-63` (warnings).
- PSP `Private/PyCore.cpp:1158-1173` (`ShouldLog` drops whitespace-only), `:1176-1192` (`Log`: `UE_LOGF(LogPython, Log, …)` + Broadcast), `:1196-1232` (warning, error), `:1234-1244` (`LogFlush` → `GLog->Flush()`), `:2066-2069` (method table).
- PSP `Private/PythonScriptPlugin.cpp:1304-1308` (`unreal` into the console dict, then `ImportUnrealModule("core")`).
- `Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp:1586-1592` (`Serialize` → `AppendPendingMessage`), `:949-963` (pending queue and `SubmitPendingMessages` with `TryLock`), `:1485-1493` (`Tick` → `SubmitPendingMessages`).
- `Engine/Source/Runtime/Core/Private/Misc/OutputDeviceRedirector.cpp:969-978` (`Flush` broadcasts `FOutputDevice::Flush` to all devices), `:440-442` (devices with `CanBeUsedOnMultipleThreads()` are unbuffered), `:904-907` (unbuffered devices receive `Serialize` synchronously on the caller's thread); `Engine/Source/Runtime/Core/Private/Misc/OutputDeviceFile.cpp:409-415, 270-275, 139-158` (`FlushBuffer` spin-wait `while (SerializeRequestCounter.GetValue() != 0) SleepNoStats(0)`).
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:2791` (`TryStartDedicatedPrimaryThread`).
- `Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp:419-440` (`TickSlate`: `FSlateApplication::Get().Tick()`, `EndFrame`, `Renderer->Sync()`).

**Confidence.**
- High for the mechanism.
- Medium for the per-line flush *cost*, which is not measured here. Measure it in the editor with a 100k-line print.

**Implication.**
- Our streaming design needs **no** change to UE's buffering. Lines are available on the game thread immediately; we just need our pump to paint them.
- During our runs, temporarily swap `sys.stdout`/`sys.stderr` for our own writer.
  - It should push to our MPSC queue and forward to LogPython in **batches, without `log_flush` per line**. This removes the per-line disk hand-off.
  - It should keep blank lines and record the stream and run ID.
- Also add an `FOutputDevice` with `CanBeUsedOnMultipleThreads()=true`, filtered to `LogPython`. It catches direct `unreal.log*` calls, UE's own traceback lines, and libraries that captured the old `sys.stderr` earlier (for example `logging.StreamHandler`). De-duplicate with a thread-local "our writer is forwarding" flag.
- Strip trailing `\r\n`.
- Treat stderr as "stderr", not as "Error", in our UI. UE's mapping makes tqdm-style progress output look like errors.
- This refines research 05 §2.3: "chained to the previous stream" would reintroduce the per-line `GLog->Flush()`.

---

## Q5. Console executors and `IConsoleCommandExecutor`

**Answer.**

**Executors registered by PythonScriptPlugin**, after a successful `InitializePython`:

| Name | Display name | Mode |
|---|---|---|
| `"Python"` | "Python" | `ExecPythonCommand`, i.e. `ExecuteFile` (accepts files or literal code) |
| `"PythonREPL"` | **"Python (REPL)"** | `FPythonCommandEx` with `ExecutionMode = ExecuteStatement` |

- Both log the input line to LogPython, keep history in `IConsoleManager` under the keys `"Python"` / `"PythonREPL"`, return `AllowMultiLine() == true` and `AllowHotKeyClose() == true`, and use the editor's `OpenConsoleCommandBox` / `SelectNextConsoleExecutor` chords.
- **`GetSuggestedCompletions` is empty for both.**
- Both are unregistered in `ShutdownPython`.
- The engine's own executor is `"Cmd"` (`FConsoleCommandExecutor`).

**Interface.**
- `class IConsoleCommandExecutor : public IModularFeature` with feature name `"ConsoleCommandExecutor"`.
- Pure virtuals: `GetName`, `GetDisplayName`, `GetDescription`, `GetHintText`, `GetSuggestedCompletions(const TCHAR*, TArray<FConsoleSuggestion>&)`, `GetExecHistory`, `Exec`, `AllowHotKeyClose`, `AllowMultiLine`, `GetHotKey`, `GetIterateExecutorHotKey`.

**Third-party registration.** It is supported.
- `IModularFeatures::Get().RegisterModularFeature(IConsoleCommandExecutor::ModularFeatureName(), &MyExec)`.
- `SConsoleInputBox` subscribes to `OnModularFeatureRegistered/Unregistered` and re-syncs.
- Executors are filtered by `FOutputLogModule::GetConsoleCommandExecutorPermissionList()` and sorted by display name.
- The user's choice persists as `[OutputLog] PreferredCommandExecutor` in `EditorPerProjectUserSettings`.

**Evidence.**
- PSP `Private/PythonScriptPlugin.h:33,58` (classes), `:219-220` (members).
- PSP `Private/PythonScriptPlugin.cpp:327-330` (`"Python"`), `:337-350` (display, description, hint), `:362-371` (`Exec` → `ExecPythonCommand`), `:378-381` (multi-line), `:406-409` (`"PythonREPL"`), `:417-420` (`"Python (REPL)"`), `:441-452` (`ExecuteStatement`), `:352-355, 431-434` (empty completions), `:1022-1023` (register), `:1479-1480` (unregister).
- `Engine/Source/Runtime/Core/Public/HAL/IConsoleManager.h:894-964`.
- `Engine/Source/Editor/UnrealEd/Private/Toolkits/FConsoleCommandExecutor.cpp:13-15` (`"Cmd"`).
- `Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp:52-70` (enumerate + permission filter), `:198-209` (preferred executor, modular-feature listeners), `:707-730` (`SyncActiveCommandExecutor`), `:734` (persist preference); `Engine/Source/Developer/OutputLog/Public/OutputLogModule.h:124`.

**Confidence.** High. This confirms the item research 06 Open Q3 marked UNVERIFIED.

**Implication.**
- Register a third executor, for example `"PythonScriptEditor"` / "Python (Script Editor)".
  - It should provide **real completions** from our live and static engine.
  - It should send input through our runner, so console lines get the same namespace, output capture and Stop.
- This is cheap, and it upgrades the console people already use.
- Studios can hide executors with the permission list, so do not assume ours is visible.

---

## Q6. Lifecycle delegates and bundled Python version

**Answer.**

**`OnPythonConfigured`** is broadcast in `ConfigureAndInitializePython` once `bIsEnabled` is decided, then cleared.

**`OnPythonInitialized`** is broadcast at the **end of `RunStartupScripts`**:
- That function runs on the first `FTSTicker` core tick after init, or in `OnCommandletPreMain` for commandlets.
- It runs after every `init_unreal.py` on `sys.path`, the configured StartupScripts and deferred `py` commands.
- `IsPythonInitialized()` reflects `bIsFullyInitialized`. `ExecPythonCommandEx` is already allowed once `bIsInterpreterInitialized` is true.

**`OnPythonShutdown`** is broadcast in `ShutdownPython`:
- The order inside `ShutdownPython`:
  1. Remote execution is reset and the console executors are unregistered.
  2. The **GIL is re-acquired** (`PyEval_RestoreThread`).
  3. `OnPythonShutdown` is broadcast and cleared.
  4. The PyEditor/PyEngine/PySlate/PyCore modules shut down, which clears Python Slate/ticker callbacks.
  5. `Py_Finalize`.
- `ShutdownPython` is reached from `FCoreDelegates::OnPreExit`, `ShutdownModule`, or **`ForceDisablePythonAtRuntime`**.

**Both `Initialized` and `Shutdown` are `.Clear()`ed after broadcast.**

**5.8 lifecycle switches.** These mean Python can be off at boot or started or stopped mid-session:
- CVars `Engine.Python.IsEnabledByDefault`, `Engine.Python.IsPythonLifeCycleDynamic` (read only) and `Engine.Python.IsPythonInRestrictiveMode`;
- `-EnablePython`, `-DisablePython`, `-ForceEnablePython`;
- user setting `EnablePythonOverride`;
- commandlet deny-list.

**Version.**
- Bundled **CPython 3.11.8**: `Engine/Binaries/ThirdParty/Python3/Win64/NEWS.txt:5` ("What's New in Python 3.11.8 final?", release date 2024-02-06), `Engine/Source/ThirdParty/Python3/Win64/include/patchlevel.h:21,26` (`PY_MICRO_VERSION 8`, `PY_VERSION "3.11.8"`), `python311.dll`.
- **[Probe]** `sys.version` = `3.11.8 (tags/v3.11.8:db85d51, Feb 6 2024) [MSC v.1937 64 bit (AMD64)]`.
- `site-packages` holds only pip 24.0 and setuptools 65.5.0 (no debugpy).
- Link library: only `Engine/Source/ThirdParty/Python3/Win64/libs/python311.lib`. `python3.dll` is present in Binaries, but there is no `python3.lib` import library.

**Evidence.**
- PSP `Private/PythonScriptPlugin.cpp:989-1035` (configure/init, `OnPythonConfiguredDelegate` broadcast and clear at `:998-999`), `:1376-1381` (core ticker), `:1384-1391` (commandlet path), `:1614-1731` (`RunStartupScripts`; broadcast and clear `:1718-1719`), `:1734-1739` (`Tick`), `:1455-1564` (`ShutdownPython`; `:1486` restore GIL; `:1497-1498` broadcast and clear; `:1553` `Py_Finalize`), `:727-784` (force enable/disable), `:98-104` (CVars), `:108-180` (`IsPythonEnabled` order).
- PSP `Private/PythonScriptPlugin.h:236-237` (`bIsInterpreterInitialized` vs `bIsFullyInitialized`).
- PSP `Private/PyCore.cpp:2090-2091` (`unreal.register_python_shutdown_callback`).
- `Engine/Plugins/Experimental/PythonScriptPlugin/PythonScriptPlugin.uplugin:13,15,26-27` (`EnabledByDefault false`, beta, `UncookedOnly`/`PreDefault`).

**Confidence.** High.

**Implication.**
- Gate on `RegisterOnPythonInitialized`.
- In `OnPythonShutdown`, drop **every** `PyObject` we hold. The GIL is held at that point, which is the right time.
- **Re-register after any `ForceEnablePythonAtRuntime`**, because our earlier delegate bindings were cleared.
- If Python is disabled, show a "Python is disabled — Enable for this session" banner that calls `ForceEnablePythonAtRuntime()`.
- Our module adds `"Python3"` to its Build.cs dependencies (PythonScriptPlugin keeps it private) and links `python311.lib`, so we rebuild per engine Python version.
  - Stable-ABI linking would need our own `python3.lib`, or `GetProcAddress` against `python3.dll`.
- Design for 3.11: no `sys.monitoring`.

---

## Q7. Python Slate tick callbacks: implementation, timing, suppression

**Answer.**

**Implementation.**
- `register_slate_pre_tick_callback(callable)` / `register_slate_post_tick_callback(callable)` add a lambda to `FSlateApplication::Get().OnPreTick()` / `OnPostTick()`.
- The lambda keeps the callable alive with `FPyAutoGILPtr`. Its handle is tracked in `PythonPreTickCallbackHandles` / `PythonPostTickCallbackHandles` and returned as a `_DelegateHandle`.
- Each invocation does three things:
  1. `if (!PyUtil::CanTickPython()) return;`
  2. `FPyScopedGIL`
  3. `callable(delta)`, logging any error.
- `unreal.register_ticker_callback(callable, delay)` (FTSTicker) has the same gate.
- All of them are cleared in `PySlate::ShutdownModule` / `PyCore` shutdown.
- The post-tick docstring wrongly says "as a pre-tick callback" (cosmetic).

**When they fire.**
- `PreTickEvent.Broadcast(DeltaTime)` runs at the start of `FSlateApplication::TickAndDrawWidgets`, and `PostTickEvent.Broadcast` at its end.
- That happens on every Slate widget tick: the normal editor frame, `FFeedbackContextEditor::TickSlate` during slow-task dialogs, and the `EnterDebuggingMode` loop.

**Suppression.** It already exists.
- `CanTickPython()` returns false while `UE::Python::Private::GIsRunningUserScript` is true ("Do not tick into Python while Python is already running on the stack"). It also returns false while saving packages, during GC and while routing PostLoad.
- `GIsRunningUserScript` is set with a `TGuardValue` in **`FPythonScriptPlugin::EvalString`**, i.e. for every `ExecPythonCommandEx` run.
- It is also set when C++ calls a Python-implemented UFunction or struct override.

**What is *not* gated.**
- Python callables bound to `unreal` delegates and multicast delegates (`PyWrapperDelegate`): no `CanTickPython` check exists there.
- Any C++ (non-Python) OnPreTick/OnPostTick listeners.
- `GIsRunningUserScript` is in a private namespace and not exported, so a third-party module cannot set it directly.

**Evidence.**
- PSP `Private/PySlate.cpp:26-54` (register; gate at `:31`), `:88-156` (four API functions; `OnPreTick` `:99`, `OnPostTick` `:134`), `:197-202` (method table), `:217-226` (clear on shutdown).
- PSP `Private/PyCore.cpp:40-79` (ticker callback with the same gate at `:45`), `:2099` (`register_ticker_callback`).
- PSP `Private/PyUtil.cpp:54-57` (`GIsRunningUserScript`), `:1822-1838` (`CanTickPython`); `Private/PythonScriptPlugin.cpp:1831` (guard in `EvalString`); `Private/PyGenUtil.cpp:1384`, `Private/PyWrapperObject.cpp:2035`, `Private/PyWrapperStruct.cpp:1691` (other guards).
- `Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:569-573`; `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:1746-1758, 1825-1827`.
- grep: `CanTickPython` appears only in `PyCore.cpp:45`, `PySlate.cpp:31` and `PyUtil.cpp` (not in `PyWrapperDelegate.cpp`).

**Confidence.**
- High for the gating behaviour.
- Medium that ungated delegate callbacks are a practical re-entrancy source during our pump. The code path exists; impact is untested.

**Implication.**
- This answers research 05 Open Q4. **No engine patch is needed.** If every user run enters through `ExecPythonCommandEx`, which our runner already does (Q3), then other tools' Python Slate/ticker callbacks will **not** run during our mid-script Slate pump. The setting "Run Slate callbacks during script pump" can only be "off".
- Remaining hazards during a pump:
  - (a) `unreal` delegate callbacks fired by whatever the Slate tick triggers, for example selection-changed delegates bound in Python;
  - (b) our own pump re-entering itself.
- Keep the input pre-processor that blocks everything except Stop and scroll, plus a re-entrancy guard.
- Never execute user code from our own C path, for example with `PyEval_EvalCode` directly. That would bypass the guard.

---

## Q8. `get_interpreter_executable_path`, debugpy helper, `remote_execution.py`

**Answer.**

**`unreal.get_interpreter_executable_path()`** (also `IPythonScriptPlugin::GetInterpreterExecutablePath()`):
- It returns `UE_PYTHON_DIR` with `{ENGINE_DIR}` expanded, plus `python.exe` on Windows or `bin/python3` on Mac/Linux, made absolute.
- For this install that is `Engine/Binaries/ThirdParty/Python3/Win64/python.exe`.

**`debugpy_unreal.py` ships, but debugpy does not.**
- The module calls `unreal.get_interpreter_executable_path()` at import.
- `install_debugpy()` runs `python -m ensurepip` and `pip install debugpy` against **the engine's interpreter**. That writes into the engine install, which needs admin rights for a Launcher install under Program Files.
- `start(port=5678)` calls `debugpy.configure(python=<interpreter>)` (the comment says otherwise debugpy "tries to run another UE instance as an adapter"), then `debugpy.listen(port)`, then **`debugpy.wait_for_client()`**, which blocks the game thread.
- `breakpoint()` calls `debugpy.breakpoint()`.
- The doc note says `redirectOutput` must be true "to avoid hanging both the debugger and the UE Editor".

**`remote_execution.py` and its C++ server.**
- **Protocol:** `version: 1`, `magic: "ue_py"`, `source`/`dest` node IDs (GUIDs; the editor uses `FApp::GetInstanceId()`). JSON over UDP and TCP.
- **UDP message types:** `ping`; `pong` (data: user, machine, engine_version, engine_root, project_root, project_name); `open_connection` (data: `command_ip`, `command_port`); `close_connection`.
- **TCP message types:** `command` (data: `command`, `unattended`, `exec_mode` ∈ `ExecuteFile|ExecuteStatement|EvaluateStatement`) and `command_result` (`success`, `command`, `result`, `output: [{type, output}]`).
- **Defaults** (5.8 settings and the Python client agree):
  - multicast group `239.0.0.1:6766`;
  - bind `127.0.0.1`;
  - TTL `0` ("limited to the local host");
  - send and receive buffers 2 MiB;
  - client command endpoint `127.0.0.1:6776`.
- The client hosts the TCP server. The editor *connects out* to whatever `command_ip:command_port` the UDP message names; there is no loopback check on that address.
- **Off by default:** `bRemoteExecution` has no initializer and no ini override.
- **Authentication:** none. There is only version/magic validation and `dest` filtering.
- **Execution:** `ExecPythonCommandEx` runs from the plugin's core-ticker `Tick` on the game thread. Output comes back once, in `command_result`, **after** completion. Nothing streams.

**Evidence.**
- PSP `Private/PyUtil.cpp:1151-1176`; `Private/PyCore.cpp:2095`; `Private/PythonScriptPlugin.cpp:909-915`.
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/debugpy_unreal.py:3-29` (usage, `redirectOutput` note `:27`), `:31-38` (install), `:40-54` (start, `wait_for_client` `:53`), `:56-59`. No `debugpy` found under `Engine/Binaries/ThirdParty/Python3/Win64/Lib/site-packages` (only pip, setuptools, `_distutils_hack`, `pkg_resources`).
- PSPC `remote_execution.py:12-19` (constants), `:21-28` (timeouts, TTL 0, group, bind `127.0.0.1`, command endpoint `127.0.0.1:6776`, recv buffer 8192), `:31-33` (modes), `:263-267` (socket options), `:434-439` (`command` → `command_result`), `:482` (TCP listen bind), `:541-546, 577-580` (version/magic).
- PSP `Private/PythonScriptRemoteExecution.cpp:28-86` (protocol doc), `:93-100` (constants), `:280-307` (UDP socket bound to the configured address and port, joined group, TTL), `:375-405` (`open_connection` handling, any IPv4 accepted), `:455-466` (TCP connect-out), `:540-580` (`command` → `ExecPythonCommandEx`, result message), `:603-620` (result fields), `:636-648` (start only if `bRemoteExecution`), `:685-696` (tick).
- PSP `Private/PythonScriptPluginSettings.cpp:15-19` (defaults); `Private/PythonScriptPluginSettings.h:127-148` (settings incl. `bRemoteExecution`).

**Confidence.** High.

**Implication.**
- Our "Debug in VS Code" button should:
  - install debugpy into a **project or user** location (the `Intermediate/PipInstall` site or a user site), never the engine's;
  - call `debugpy.configure(python=get_interpreter_executable_path())` + `listen`;
  - **never** call `wait_for_client()` on the game thread. Poll `debugpy.is_client_connected()` from a ticker instead.
- For external-IDE integration, do not extend Epic's protocol. It is unauthenticated and non-streaming, and its connect-out target is arbitrary.
- If we support it for compatibility, keep the 5.8 loopback defaults and warn if a studio sets bind `0.0.0.0` or TTL ≥ 1.
- Our own bridge stays loopback-only with a token.

---

## Corrections to research

| File | Claim | Correction |
|---|---|---|
| 06-user-voice-and-landscape.md | "output is buffered until the script ends" (pain point 1); "`unreal.log` output stays buffered until the script exits" (pain point 10) | Refuted as to cause. `unreal.log` calls `UE_LOGF` synchronously (PSP `Private/PyCore.cpp:1176-1192`) and `print` is line-buffered with a flush per line (PSPC `unreal_core.py:58-59`; [Probe]). The Output Log shows lines only when `SOutputLog::Tick` runs (`Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp:1485-1493`). The game thread is blocked, so Slate does not tick. Only partial lines and `LogOutput`/remote results are held until the end. |
| 05-architecture-and-robustness.md | `PyThreadState_SetAsyncExc` thread "can get the GIL during pure-Python loops… but not while the game thread holds the GIL in C++" | Partly wrong. The game thread **releases** the GIL around every reflected UFunction call (`ProcessEvent`, PSP `Private/PyUtil.cpp:644-647`), find/load object, `unreal.log` and GC. So the watchdog *can* get the GIL during native UE calls. Delivery still waits for the return to bytecode. |
| 05-architecture-and-robustness.md | `PyErr_SetInterruptEx`: "Whether UE installs [a SIGINT handler] is UNVERIFIED. Installing `default_int_handler` ourselves may clash with console Ctrl+C handling in `-Cmd` runs" | UE does **not** install one (`install_signal_handlers = 0`, PSP `Private/PythonScriptPlugin.cpp:1178`). [Probe]: any `import signal` on the main thread auto-installs `default_int_handler`, after which `PyErr_SetInterruptEx` works. The Ctrl+C clash is neutralized on Win64 non-shipping console builds, because UE patches `SetConsoleCtrlHandler` so later handlers cannot register (`Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1180-1205`). |
| 04-unreal-python-internals.md | §2.6: "there is no Python SIGINT handler. CPython ignores `PyErr_SetInterrupt[Ex]` when SIGINT is not handled by Python [UNVERIFIED]… installing `signal.signal(SIGINT, default_int_handler)`… **Not recommended**" | Verified with nuance. Before `signal` is imported, the call is not silently ignored: it produces an unraisable `OSError: Signal 2 ignored due to race condition` (an error line in LogPython). After any `import signal`, the handler exists automatically, so many live sessions already have one. The "process-wide C handler" concern is mitigated as above. Keeping `Py_AddPendingCall` primary is still right. |
| 04-unreal-python-internals.md | §2.6: `Py_AddPendingCall` runs on the game thread — "UNVERIFIED in-engine; prototype first" | Verified with the shipped `python311.dll` embedded with `initsigs=0`: a pending call added from a worker ran on the initializing thread ([Probe] `ran_on_main: true`). UE initializes on the game thread (`PythonScriptPlugin.cpp:942,986,1100`; `LaunchEngineLoop.cpp:4062-4064`). The callback must set an exception before returning -1; otherwise CPython raises `SystemError` ([Probe] 4b). *Amended 2026-10-28 (RV-01):* the probe's worker held the GIL and latency was not measured. A post from a thread that does not hold the GIL is noticed only when the game thread next takes the GIL, so a pure-Python loop never sees it (note under Q1). |
| 04-unreal-python-internals.md | §2.1: Python threads "must not touch `unreal` objects (UNVERIFIED for every API; there is no thread check in the wrappers I read)" | There **is** a thread check, for all reflected UFunction calls: `RuntimeError("Attempted to access Unreal API from outside the main game thread.")` in `PyUtil::InvokeFunctionCall` (PSP `Private/PyUtil.cpp:606-616`). Property access and `find/load_object` are unchecked. |
| 03-ide-features-lsp-dap.md | B8: "The GIL is free while the editor is idle… UNVERIFIED as a guarantee" | Verified: `PyEval_SaveThread()` after init (PSP `Private/PythonScriptPlugin.cpp:1394-1396`); all entry points use `PyGILState_Ensure` (PSP `Private/PyGIL.h:15-24`). The GIL is also free *during* reflected UFunction calls inside a script. |
| 05-architecture-and-robustness.md | Open question 4 / §2.2 item 8: suppressing foreign Python Slate callbacks during our pump is UNVERIFIED and may need engine help | Answered: they are already suppressed. Every Python Slate and ticker callback checks `CanTickPython()`, which is false while `GIsRunningUserScript` is set by `EvalString` during `ExecPythonCommandEx` (PSP `Private/PySlate.cpp:31`, `Private/PyCore.cpp:45`, `Private/PyUtil.cpp:1822-1838`, `Private/PythonScriptPlugin.cpp:1831`). Requirement: user code must be entered through `ExecPythonCommandEx`. Python callables bound to `unreal` delegates are not gated. |
| 05-architecture-and-robustness.md | §2.4 / item 4: shared namespace "like Maya's and UE's console (`PUBLIC` scope)"; `ExecutePythonCommandEx` "supports PUBLIC/PRIVATE file scopes" | Refinement. `FileExecutionScope` applies only when the command resolves to a `.py` file. Literal code always runs in the console dict (`PythonScriptPlugin.cpp:1814-1817, 1866`). The console dict is `PyDict_Copy(__main__.__dict__)` taken at init (`:1256`), so it is not `__main__` itself. |
| 05-architecture-and-robustness.md | §2.3: C++ stdout writer "chained to the previous stream so `LogPython` still works" | Refinement. The previous stream calls `unreal.log_flush()` → `GLog->Flush()` on every line, a synchronous log-writer hand-off (PSP `Private/PyCore.cpp:1234-1244`; `Engine/Source/Runtime/Core/Private/Misc/OutputDeviceFile.cpp:139-158`). Forward to `unreal.log` in batches instead of chaining to the old `TextIOWrapper`. |
| 05-architecture-and-robustness.md | §2.7: "The multicast bind address `0.0.0.0` binds all adapters, and TTL 1 reaches the subnet" (as a description of the exposure) | Clarification. 5.8 **defaults** are bind `127.0.0.1`, TTL `0` and remote execution **disabled** (PSP `Private/PythonScriptPluginSettings.cpp:15-19`, `Private/PythonScriptRemoteExecution.cpp:644-647`). The LAN exposure exists only if a studio changes those settings. There is still no authentication. |
| 06-user-voice-and-landscape.md | Pain point 1(a): Stop "raises `KeyboardInterrupt` through a trace hook or `PyErr_SetInterrupt`" | `PyErr_SetInterrupt` is reliable only after a Python SIGINT handler exists, which UE does not install at init. Prefer the `Py_AddPendingCall` pump with a `BaseException` subclass, and use `PyThreadState_SetAsyncExc` as the fallback. |
| 06-user-voice-and-landscape.md | "UE 5.6 quietly broke background-thread Python… 5.6 began enforcing more strictly" | Source confirms the enforcement exists in 5.8 (`PyUtil.cpp:606-616`). The **5.6 date cannot be verified locally**: there is no older source and no version marker. Keep it attributed to the forum source. |
| 06-user-voice-and-landscape.md | Open Q3: "confirm the 'Python (REPL)' executor… UNVERIFIED" | Confirmed: name `PythonREPL`, display "Python (REPL)", `ExecuteStatement`, empty completions (`PythonScriptPlugin.cpp:406-452`). |

---

## New risks

1. **Per-line `GLog->Flush()`.** Every `print` line triggers `unreal.log_flush()` → `GLog->Flush()`, which makes the game thread wait on the log-file writer. Scripts that print heavily are slowed by UE's own stream, independent of our UI. Mitigation: our run-scoped `sys.stdout` replacement with batched forwarding. Measure the cost in the editor.
2. **Blank lines are dropped.** `ShouldLog` discards whitespace-only messages, so `print()` blank lines vanish from LogPython and `LogOutput`. If we capture only through LogPython we lose formatting. Capture at the Python stream level.
3. **stderr maps to Error verbosity.** Progress bars and library chatter on stderr show as red errors and can trip "errors" heuristics. Our UI should label them "stderr", not "Error".
4. **Stale interrupts.** A tripped SIGINT flag, or a pending async exception, that is delivered after the run finishes will raise `KeyboardInterrupt` in the next unrelated Python code: another tool's tick callback or the next run. Guard with run IDs, use a custom SIGINT handler if we use signals at all, and clear async exceptions with `PyThreadState_SetAsyncExc(tid, NULL)` at run end.
5. **Unraisable error spam.** Calling `PyErr_SetInterruptEx` before any `signal` import logs `OSError: Signal 2 ignored due to race condition` to LogPython.
6. **Pending-call queue saturation.** The queue is small (32 in CPython 3.11, from CPython source knowledge and not verified here) and is drained only at bytecode boundaries. A watchdog that enqueues every 100 ms during a long native call gets `-1` and may starve other users of the API (debuggers). Keep one outstanding.
7. **Ungated re-entrancy through `unreal` delegates.** Python callables bound to engine multicast delegates are not filtered by `CanTickPython`. Our mid-script Slate pump can fire them, for example selection or asset delegates triggered by UI work during the pump.
8. **The guard only applies on the ExecPythonCommandEx path.** `GIsRunningUserScript` is private and set only in `EvalString`. Any design that runs user code by calling the C API directly loses callback suppression. Make "enter via `ExecPythonCommandEx`" an invariant, with a test.
9. **Dynamic Python lifecycle in 5.8.** Python can be disabled at boot (`IsEnabledByDefault`, `EnablePythonOverride`, `IsPythonLifeCycleDynamic`) and shut down mid-session (`ForceDisablePythonAtRuntime` → `Py_Finalize`). `OnPythonInitialized`/`OnPythonShutdown` are cleared after broadcast, so one-time bindings stop working after a disable/enable cycle.
10. **Off-thread property access is unguarded.** Background threads still run, because the GIL is released during UFunction calls. `get/set_editor_property` and `load_object` from a thread do not raise and can corrupt or crash the editor. Our linter or runtime guard should warn.
11. **`EnterDebuggingMode` preconditions** (relevant to the in-process debugger design). It refuses, and calls `UE_DEBUG_BREAK()`, when a modal window is active or when entered inside Tick of the main editor window. It also ensures on re-entry (`Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:3815-3910`). A breakpoint hit inside a slow-task dialog or a modal cannot use it.
12. **`debugpy_unreal.install_debugpy()` writes into the engine install** (Program Files), and `start()` blocks the game thread in `wait_for_client()`. Do not wrap Epic's helper as-is.
13. **ABI coupling.** Only `python311.lib` ships, so our compiled module is tied to 3.11 and must be rebuilt at the 3.13 upgrade. Limited-API builds need our own import library for `python3.dll`.
14. **The console namespace is not `__main__`.** Users who `import __main__` to share state, or tools that inject into `__main__`, will not see console variables, and the reverse holds too. Our "Session" namespace must pick one explicitly and document it.
15. **Pending calls posted without the GIL are not noticed by pure Python** (added 2026-10-28, RV-01). In 3.11 a post from a non-main thread does not set the eval breaker (`Engine/Source/ThirdParty/Python3/Win64/include/internal/pycore_pystate.h:57-61`), so the game thread sees it only when it next takes the GIL. A watchdog that posts from a native thread must force a GIL hand-off after each post, or Stop never reaches a pure-Python loop (note under Q1; [`probes/pending_native.py`](probes/pending_native.py)).

---

## Files read

**Research:** `docs/research/03-ide-features-lsp-dap.md` (Unreal sections), `04-unreal-python-internals.md` (§0–§4), `05-architecture-and-robustness.md` (§0, §2.2–2.3, risks, open questions), `06-user-voice-and-landscape.md` (summary, pain-point table); grep over `01`/`02` for Unreal claims.

**Engine (all under `C:\Program Files\Epic Games\UE_5.8\`):**
- `Engine/Plugins/Experimental/PythonScriptPlugin/PythonScriptPlugin.uplugin`
- PSP `PythonScriptPlugin.Build.cs`
- PSP `Public/IPythonScriptPlugin.h`, `Public/PythonScriptTypes.h`
- PSP `Private/PythonScriptPlugin.cpp` (1–1076, 1080–1420, 1455–1565, 1614–1760, 1800–1996), `Private/PythonScriptPlugin.h` (grep)
- PSP `Private/PyGIL.h`; `Private/PyUtil.cpp` (30–60, 580–660, 1100–1180, 1595–1790, 1810–1844); `Private/PyUtil.h` (grep)
- PSP `Private/PyCore.cpp` (25–80, 1158–1250, 1426–1440, 2060–2100, 2191), `Private/PyCore.h` (grep)
- PSP `Private/PySlate.cpp` (all)
- PSP `Private/PyConversion.cpp` (1240–1262); `Private/PyWrapperObject.cpp` (1380–1505); `Private/PyGenUtil.cpp` (92–135); `Private/PyWrapperTypeRegistry.cpp` (965–1030)
- PSP `Private/PythonScriptLibrary.h` (grep), `Private/PythonScriptLibrary.cpp` (180–215)
- PSP `Private/PythonScriptRemoteExecution.cpp` (28–100, 275–312, 370–470, 540–620, 636–720)
- PSP `Private/PythonScriptPluginSettings.cpp` (1–60), `Private/PythonScriptPluginSettings.h` (grep, 120–148)
- PSP `Private/EditorUtilities/EditorPythonExecuter.cpp` (20–120)
- `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPluginPreload/Private/PythonScriptPluginPreload.cpp` (1–80)
- PSPC `unreal_core.py`, `debugpy_unreal.py`, `remote_execution.py` (grep, constants)
- `Engine/Source/Runtime/Core/Public/HAL/IConsoleManager.h` (890–965)
- `Engine/Source/Runtime/Core/Private/Misc/OutputDeviceRedirector.cpp` (790–1000), `Engine/Source/Runtime/Core/Private/Misc/OutputDeviceFile.cpp` (83–160, 268–292, 409–415)
- `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp` (1150–1215)
- `Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp` (48–70, 198–209, 707–736, 941–1010, 1483–1494, 1584–1594), `SOutputLog.h` (grep), `Public/OutputLogModule.h` (grep)
- `Engine/Source/Editor/UnrealEd/Private/Toolkits/FConsoleCommandExecutor.cpp` (grep)
- `Engine/Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp` (405–640)
- `Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h` (grep), `Engine/Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp` (1621–1660, 1746–1830, 3815–3985)
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp` (grep)
- `Engine/Source/ThirdParty/Python3/Python3.Build.cs` (1–140), `Win64/include/patchlevel.h`, `ceval.h`, `pyerrors.h`, `pystate.h` (grep), `Win64/libs/` (listing)
- `Engine/Binaries/ThirdParty/Python3/Win64/NEWS.txt` (head), `Win64/Lib/site-packages` (listing)

**Probe (executed, read-only use of the engine DLL):** `…\scratchpad\pyprobe\probe.py`, `run_probe.ps1`, output `probe_out.json`.
