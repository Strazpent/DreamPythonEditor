> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 04 — Unreal Engine Python integration and editor-side engine APIs (UE 5.6 / 5.7 / 5.8)

Research lane: UE's embedded Python (PythonScriptPlugin) and the editor engine APIs a C++/Slate script-editor plugin will build on.
Date: 2026-10-28. Citations use `[Sn]`, which map to **Sources** at the end. **UNVERIFIED** marks claims I could not confirm from a primary source this session.

> **How this was checked.** A Launcher build of **UE 5.8** was being installed on this machine at `C:\Program Files\Epic Games\UE_5.8` during the session. I read the shipped plugin and engine **source and headers** directly [S1]. I also decompressed the install manifest to get the complete 5.8 file list [S2]. Most 5.8 statements below come from shipped code, not documentation; file paths are given relative to `Engine/`. The shared web-search budget ran out partway through. After that I used only direct page fetches of Epic and python.org docs, plus the local engine.

---

## Summary

1. **Overlap check: no built-in Python script editor in 5.6, 5.7 or 5.8.** Python input in the editor is still the Output Log console line:
   - two executors, "Python" and "Python (REPL)";
   - Shift+Enter for multi-line, history, **no completion and no highlighting**;
   - a Tools > Python menu (Execute Python Script / Recent Python Scripts), and an opt-in Content Browser ".py" view with "Run…" [S1].

   Epic's new 5.7/5.8 AI stack sits next to our product and does not overlap with it:
   - an **AI Assistant** web panel (Experimental) [S4][S1];
   - an **Unreal MCP** server (Experimental, 5.8) [S3][S21];
   - a **Terminal** plugin (native Slate terminal emulator, 5.8) [S1][S21];
   - Python "toolsets" via a **ToolsetRegistry** [S1].

   The only "run Python" tool Epic ships for agents is a **sandboxed tool-orchestration script runner**. Its docstring says it is not general Python execution [S1]. Details in §0.
2. **CPython is 3.11.8 from 5.4 through 5.8.** 5.3 was 3.9.7 [S7–S11][S6]. The 5.8 install has `python311.dll`, `NEWS.txt` = 3.11.8, pip 24.0 and setuptools 65.5.0 [S1]. Epic said it could not move to 3.13 for 5.8, and the next upgrade will land in main first [S22].
3. **Execution model.** Everything runs on the **game thread**:
   - The GIL is released after init (`PyEval_SaveThread`) and re-taken per call with `PyGILState_Ensure` (`FPyScopedGIL`) [S1].
   - A long script freezes Slate. Only a `ScopedSlowTask` dialog pumps Slate, and only for that **modal** window [S1].
   - Since 5.6, Python tick callbacks are skipped while a user script is running [S5][S1].
   - Python is initialized with **`install_signal_handlers = 0`**, so Ctrl+C / `PyErr_SetInterrupt` has no Python SIGINT handler. Cancellation has to be engineered, for example with `Py_AddPendingCall` (§2.6).
4. **The public C++ surface is small.** `IPythonScriptPlugin` provides:
   - `ExecPythonCommand[Ex]`, `IsPythonAvailable/Configured/Initialized`;
   - `ForceEnable/DisablePythonAtRuntime`, `GetInterpreterExecutablePath`;
   - three delegates and their `RegisterOn…` helpers;
   - **new in 5.8**: `Add/RemoveStartupScriptAllowPath` for a WIP "restrictive mode" [S1][S13].

   Log capture, the eval stack and the console dicts are **private**. `FPythonCommandEx` returns only `CommandResult` and the captured `LogOutput` after the run, with nothing streamed during it [S1].
5. **Gotchas in the public exec path** [S1]:
   - `ExecuteFile` mode with *literal code* looks for the first `.py` in the text. If it is followed by whitespace, the text before it is treated as a **file path**. A comment like `# see helper.py` followed by a newline makes the run fail.
   - Literal code is compiled as `"<string>"` and always runs in the **shared console globals**.
   - Errors from literal code go to `CommandResult` but **not** into `LogOutput`; file runs add both.

   Recommendation: run buffers through **our own Python runner module** (a real filename, a namespace per document, structured results) called via `EvaluateStatement`, or by linking the shipped `Python3` third-party module directly. The Python headers and `python311.lib` ship in the Launcher build [S2].
6. **Slate's stock code-text stack does not fit big files.** `SMultiLineEditableText` plus `FSyntaxHighlighterTextLayoutMarshaller` re-tokenizes and **rebuilds the whole text layout on every edit**, because `RequiresLiveUpdate()` is true. Undo stores **full-text snapshots, 99 levels deep** [S1].

   Epic's own 5.8 Terminal is an `SLeafWidget` that paints a character grid in `OnPaint` with `FSlateDrawElement::MakeText`. That is a precedent for a custom, virtualized code view [S1]. The other path is **CEF 128.4.13 / Chromium 128** through `SWebBrowser`, as the 5.8 AI Assistant does [S2][S1].
7. **Subprocesses (LSP).** `FInteractiveProcess` and `FMonitoredProcess` are **line-oriented**. They split on `\n` per read chunk, drop empty lines, and `FInteractiveProcess` logs every line. That breaks JSON-RPC `Content-Length` framing. Use `FPlatformProcess::CreatePipe/CreateProc/ReadPipeToArray/WritePipe`, or the `UE::HAL::FProcessStartInfo` overload, on our own thread [S1].
8. **Headless/CI is solid.**
   - `-run=pythonscript -script=…` returns **0/-1** and enables source control as of 5.8 [S1][S3].
   - `-ExecutePythonScript=` with `-ScriptErrorsAreFatal`, plus `SetKeepPythonScriptAlive`, covers tick-driven scripts [S1].
   - 5.8 adds a C++ **`FPythonAutomationTest`** (exposed as `unreal.PythonTestRunner`) in the Experimental ToolsetRegistry plugin. It turns Python `unittest` cases into Automation tests [S1].

---

## 0. Critical check: does UE 5.6/5.7/5.8 already ship an overlapping editor?

| Feature | Version / status | Python editing? | Evidence |
|---|---|---|---|
| Output Log console, "Python" and "Python (REPL)" executors | Since 4.2x, still in 5.8 | One input box. Shift+Enter adds a new line [S1 `Source/Developer/OutputLog/Private/SOutputLog.cpp`: `ModiferKeyForNewLine(EModifierKey::Shift)`]. History via `IConsoleManager` history. **`GetSuggestedCompletions` is empty** for both executors, so there is no completion and no highlighting. | [S1 `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp` L322–480] |
| Tools menu > section "Python": *Execute Python Script…*, *Recent Python Scripts* | 5.8 (menu `LevelEditor.MainMenu.Tools`). The docs still say "File menu" [S6]. | Runs files only | [S1 PythonScriptPlugin.cpp L586–660]. 5.8 fix: the recent list no longer includes files run at startup [S3] |
| Content Browser ".py" data source with a **Run…** context entry | Opt-in user setting `bEnableContentBrowserIntegration` | Run only; no editing | [S1 PythonScriptPlugin.cpp L1405–1450, L2224+] |
| **Code Editor** plugin (`Plugins/Experimental/CodeEditor`) | Old UE4 experiment, still shipped. `"IsBetaVersion": true`, off by default | Edits **C++** project source with C++ keyword highlighting (`CPPRichTextSyntaxHighlighterTextLayoutMarshaller`, `SCodeEditableText : SMultiLineEditableText`). No Python. | [S1 `Plugins/Experimental/CodeEditor/…`] |
| **AI Assistant** panel | 5.7 Experimental: slide-out panel to ask questions and generate C++, F1 starts a contextual chat [S4][S23][S24]. 5.8: plugin `AIAssistant` is Experimental, `NoRedist`, `EnabledByDefault: false`, and depends on PythonScriptPlugin, EditorScriptingUtilities and ToolsetRegistry [S1]. | Not an editor. It is an **embedded web app** (`SWebBrowser` loading `https://dev.epicgames.com/community/assistant/embedded`) with a JS↔C++ binder [S1 `AIAssistantConfig.cpp`, `AIAssistantWebBrowser.h`]. A Sept 2025 reading of `ue5-main` reported an `AIAssistantPythonExecutor.h` with `ExecutePythonScript` [S25]. **In shipped 5.8 that file does not exist, and no non-test code calls `ExecPythonCommand*`.** Only a code comment mentions `executepythonscriptviajavascript` as a binding example [S1]. | [S1][S25][S29] |
| **Unreal MCP** server (`ModelContextProtocol`) | **New in 5.8**, Experimental | No editor. HTTP/SSE on `127.0.0.1:8000/mcp`, **no authentication**, tool calls run **serially on the game thread** [S21]. Toolsets can be written in Python (`unreal.ToolsetDefinition`, `@toolset_registry.tool_call`) [S21]. 5.8 ships ~27 toolset plugins under `Plugins/Experimental/Toolsets` [S1]. | [S3][S21][S1] |
| `EditorToolset` `execute_tool_script(script)` | 5.8 Experimental | Runs agent-written Python, **sandboxed**: module allowlist, restricted `open()`. Its docstring says it batches tool calls and is not general Python execution. | [S1 `Plugins/Experimental/Toolsets/EditorToolset/Content/Python/editor_toolset/toolsets/programmatic.py` L881–937] |
| **Terminal** plugin | **New in 5.8**, Experimental, `NoRedist`: "Native Slate terminal emulator" (ConPTY / POSIX PTY, VT parser) | Not a code editor. Epic's MCP doc suggests using it to run an AI agent inside the editor [S21]. | [S1 `Plugins/Experimental/Terminal/…`] |
| PCG **Python Interop** plugin: "Execute Python Script" PCG node | 5.7 | Runs Python inside PCG graphs; no editor | [S4] |

**5.8 release-note items about Python and scripting** [S3]:
- **New:**
  - Optional property support in Python, including Python-generated types;
  - CSV string-table import/export for Blueprint/Python;
  - Level Editor Subsystem exposure (viewport camera transform and FOV);
  - `CreateAsset` gained `bOverwriteExisting`;
  - a **batch processor framework** (Python classes registered via `init_unreal.py`, launchable from script or `BatchProcessCommandlet`);
  - Editor Utility **naming tokens** can execute Python nodes;
  - `UAssetActionUtility` must declare `SupportedClasses`;
  - Editor Preferences search can find CVars and keybinds;
  - Output Log category performance improved;
  - the experimental **MCP Server** plugin.
- **Fixes:**
  - Python-defined function return values;
  - crashes when re-instancing Python-generated classes;
  - `FPyWrapperSet` intersection;
  - the PythonScript commandlet now **enables source control** before running;
  - the Recently Executed Python Files menu;
  - the AI-assistant tab label in the Panel Drawer.

**Verdict.** Nothing in 5.8 gives users a multi-file Python editor with highlighting, completion, diagnostics, run/stop or a debugger. The real competition is still external IDEs over remote execution, plus third-party plugins (other lanes cover those). The strategic signal is Epic's **AI/MCP/toolset direction**:
- it is Python-heavy;
- it uses asyncio pumped by editor ticks (`toolset_registry/_asyncio/editor_thread_ticker.py`) [S1];
- it ships a terminal.

**We should integrate with it rather than duplicate it.** For example, expose "open/run/diagnose buffer" as a ToolsetRegistry toolset so MCP agents can drive our editor.

---

## 1. PythonScriptPlugin

### 1.1 Embedded CPython per release

| Engine | CPython | Source |
|---|---|---|
| 5.3 | 3.9.7 | [S7] |
| 5.4 | 3.11.8 | [S8] |
| 5.5 | 3.11.8 | [S9] |
| 5.6 | 3.11.8 | [S10] |
| 5.7 | 3.11.8 | [S11] |
| 5.8 | **3.11.8**. Verified: `Binaries/ThirdParty/Python3/Win64/python311.dll`, `NEWS.txt` heading "3.11.8 final (2024-02-06)"; Win64, WinArm64, Linux and Mac builds are present | [S6][S1][S2][S22] |

- **Consequences.** There is no `sys.monitoring` (3.12+), no PEP 667/768, and no free-threading. If 5.9 or 6.0 moves to 3.13, any compiled extension we ship has to be rebuilt; Epic says the upgrade lands in main first, with no timeline [S22].
- The interpreter's own `site-packages` contains **pip 24.0** and **setuptools 65.5.0** [S1].
- `UE_PYTHON_DIR` plus a source build is the only way to embed a different Python [S6].

### 1.2 Bundled packages (5.8)

- **PythonScriptPlugin** ships these wheels under `Content/Python/Lib/wheels/` [S2]:
  - `packaging 23.1`, `pip-requirements-parser 32.0.1`, `pyparsing 3.1.1`, `wheel 0.38.4`;
  - `ue_pipinstall_utils 0.1.5`.

  They exist for the pip installer and are **not** user libraries.
- **Plugin `Content/Python` modules** [S1]:
  - `unreal_core.py`: decorators, plus stdout/stderr redirection;
  - `remote_execution.py`;
  - **`debugpy_unreal.py`**: a VS Code attach helper. It calls `debugpy.configure(python=unreal.get_interpreter_executable_path())`, needs `redirectOutput: true`, and defaults to port 5678;
  - tests.
- **PythonFoundationPackages** (`Plugins/Experimental`) is a *descriptor-only* plugin. Its `PythonRequirements` pins (5.8) [S1]:
  - numpy 1.26.4, scipy 1.11.4, Pillow 10.1.0;
  - requests 2.28.1, urllib3 1.25.11, certifi 2023.7.22, chardet 3.0.4, charset-normalizer 2.1.1, idna 2.10;
  - Jinja2 3.1.2, MarkupSafe 2.1.3;
  - sympy 1.13.1, mpmath 1.3.0, networkx 3.2.1;
  - protobuf 4.24.0, filelock 3.13.1, fsspec 2023.10.0, typing-extensions 4.12.2.

  These are installed by pip at editor start; they are not in the depot. Epic's docs example still shows torch 2.1.0 and numpy 1.24.4 [S12]. **The 5.8 descriptor has no torch**, so the docs are stale.

### 1.3 `IPythonScriptPlugin` (5.8, `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Public/IPythonScriptPlugin.h`) [S1][S13]

```cpp
static IPythonScriptPlugin* Get();                       // FModuleManager::GetModulePtr("PythonScriptPlugin")
virtual bool IsPythonAvailable() const = 0;              // may be false until IsPythonConfigured()
virtual bool IsPythonConfigured() const = 0;             // 5.6+
virtual bool IsPythonInitialized() const = 0;            // 5.6+ (interpreter up AND startup scripts done)
virtual bool ForceEnablePythonAtRuntime(UE::FSourceLocation = Current()) = 0;   // 5.6+
virtual void ForceDisablePythonAtRuntime(UE::FSourceLocation = Current()) = 0;
virtual bool ExecPythonCommand(const TCHAR*) = 0;
virtual bool ExecPythonCommandEx(FPythonCommandEx&) = 0;
virtual FString GetInterpreterExecutablePath() const = 0;
virtual FSimpleMulticastDelegate& OnPythonConfigured() = 0;   // 5.6+
virtual FSimpleMulticastDelegate& OnPythonInitialized() = 0;
virtual FSimpleMulticastDelegate& OnPythonShutdown() = 0;
void RegisterOnPythonConfigured(FSimpleDelegate&&);      // call-now-or-later helpers (5.6+)
void RegisterOnPythonInitialized(FSimpleDelegate&&);
virtual void AddStartupScriptAllowPath(FName Owner, const FString& Path) = 0;  // 5.8: "restrictive mode"
virtual void RemoveStartupScriptAllowPath(FName Owner) = 0;
```

The 5.6 release notes list the configured/initialized callbacks, lazy initialization via **`Engine.Python.IsEnabledByDefault=false`**, `ForceEnablePythonAtRuntime` from C++/Blueprint, an Output Log "Enable Python" entry, and the per-user `EnablePythonOverride` [S5]. Things to know:
- `OnPythonInitialized` fires **after startup scripts** (`init_unreal.py`) run [S1].
- `ExecPythonCommandEx` called before init returns false with a warning [S1].
- If Python is unavailable it calls **`ensureAlwaysMsgf`**, so callers must check `IsPythonAvailable()` first [S1].

### 1.4 `FPythonCommandEx` and enums (`Public/PythonScriptTypes.h`) [S1][S14][S15]

| Field | Type / default | Notes |
|---|---|---|
| `Flags` | `EPythonCommandFlags::None` | `Unattended` (1<<0) sets `GIsRunningUnattendedScript`, which suppresses some UI |
| `ExecutionMode` | `ExecuteFile` | `ExecuteFile` (`Py_file_input`, literal multi-statement code or a file with args) · `ExecuteStatement` (`Py_single_input`, prints the result like the REPL) · `EvaluateStatement` (`Py_eval_input`, returns the result) |
| `FileExecutionScope` | `Private` | **Applies to files only.** `Private` copies the default `__main__` globals; `Public` uses the shared console dict |
| `Command` | `FString` | code, or "path.py args" |
| `CommandResult` | `FString` | `repr()` of the result (EvaluateStatement); otherwise `None`; on failure, the traceback text |
| `LogOutput` | `TArray<FPythonLogOutputEntry{Type: Info/Warning/Error, Output}>` | Captured by an internal multicast delegate (`PyCore::GetPythonLogCapture()`, private) **for the duration of the call only** |

### 1.5 What the exec path really does (5.8 source) [S1 PythonScriptPlugin.cpp L793–1990, PyUtil.cpp L1603–1800]

- **`.py` sniffing in `ExecuteFile`.**
  - It finds the first `.py`. If that is followed by whitespace or end-of-string (or a closing quote, when the text starts with one), everything before it is treated as a filename. That file is resolved against the CWD, then each `sys.path` entry, and run via `RunFile`.
  - Otherwise the text goes to `RunString`.
  - So any buffer whose first `.py` is followed by a space or newline (for example `import x  # uses foo.py` on line 1) is misread as a file path and fails with "Could not load Python file".
- **`RunString`** compiles with filename **`"<string>"`** and executes in the **console global/local dict**, so user code leaks into the Output Log console namespace. On error, the traceback goes to `CommandResult` but is **not** appended to `LogOutput`.
- **`RunFile`** reads the file with `FFileHelper` (deliberately not `PyRun_File`, because of CRT mismatch). It then:
  - sets `__file__` and `sys.argv` (`FPythonScopedArgv`);
  - compiles with the real path, so tracebacks are clickable;
  - adds each traceback line to `LogOutput` as Error;
  - records an analytics event `PythonScriptPlugin{Duration}`;
  - updates the "Recent Python Scripts" list.
- Both paths:
  - set `GIsRunningUserScript = true`, which blocks Python tick callbacks (`PyUtil::CanTickPython`);
  - push an eval-stack context;
  - call `FPyWrapperTypeReinstancer::Get().ProcessPending()` afterwards.
- **Tracebacks.**
  - `FetchPythonError` normalizes the exception and formats it with `traceback.format_exception`. Each line is logged as `LogPython: Error:`.
  - `SyntaxError` without a traceback gets a caret line.
  - **`SystemExit(0)` and `SystemExit(None)` are swallowed silently**; a non-zero exit is an error.
  - If user code replaced `sys.excepthook`, UE **calls the custom hook** too. That is a supported way to capture structured exceptions, but the hook is global and shared.
  - `bInteractive` errors also open a modal "Python Error" message box.

### 1.6 Lifecycle, CVars and command-line switches (5.8) [S1 PythonScriptPlugin.cpp L84–180, L947–1100, L1614–1730]

**Start-up order:**
1. `StartupModule` hooks `FCoreDelegates::GetOnPostEngineInit()`.
2. `OnPostEngineInit` → `ConfigureAndInitializePython`, which decides `bIsEnabled` and broadcasts `OnPythonConfigured`.
3. `InitializePython`:
   - shows a slow-task "Initializing Python…";
   - `Py_PreInitialize` with UTF-8 mode;
   - `Py_InitializeFromConfig` with `isolated`/`use_environment` taken from settings, `install_signal_handlers=0`, `safe_path=0`;
   - sets up the `sys.path` entries;
   - builds the `unreal` module (core/slate/engine/editor);
   - starts the pip-install task asynchronously, then **waits** for it and may run pip **synchronously on the game thread**;
   - generates the wrapped types (multi-threaded since 5.6 [S5]);
   - **releases the GIL**.
4. On the first ticker tick, `RunStartupScripts` runs **every `init_unreal.py` found on any `sys.path` entry**, in the **Public** scope, then the configured StartupScripts, then any deferred commands. It broadcasts `OnPythonInitialized` and schedules stub generation.
5. In commandlets, startup scripts run in `OnCommandletPreMain`.

**Switches:**
- **Command line:**
  - `-EnablePython`, `-DisablePython`, `-ForceEnablePython`;
  - `-ExecutePythonScript=…`, `-ScriptErrorsAreFatal`;
  - `-ForcePipInstallOnInit`.
  - The documented `-DisablePipInstall` [S12] **does not appear in the 5.8 plugin source**; treat it as UNVERIFIED and use the `bRunPipInstallOnStartup` setting instead.
- **CVars:**
  - `Engine.Python.IsEnabledByDefault`;
  - `Engine.Python.IsPythonLifeCycleDynamic` (read-only);
  - **`Engine.Python.IsPythonInRestrictiveMode`**. Described as WIP: it disables pip and filters startup scripts to permitted content folders plus `AddStartupScriptAllowPath` prefixes.
- **Config:** `[PythonScriptPlugin] +DisablePythonForCommandlet=…` in BaseEditor.ini. It defaults to Cook, CookShaders, GatherText and GatherTextFromAssetsWorker [S1 `Config/BaseEditor.ini`].

**Descriptor facts** (`PythonScriptPlugin.uplugin`):
- `EnabledByDefault: false`, `IsBetaVersion: true`, although the docs label it Experimental [S6];
- module `PythonScriptPlugin` is **UncookedOnly / PreDefault**, plus `PythonScriptPluginPreload` (Runtime, EarliestPossible);
- dependency on `ContentBrowserFileDataSource`;
- `SupportedPrograms: LiveLinkHub` [S1].

Our plugin must list `PythonScriptPlugin` in `"Plugins"`, load at `Default` or later, and gate everything on `RegisterOnPythonInitialized`.

### 1.7 Console executors, the `py` command, and registering our own

- **The interface:**
  - `IConsoleCommandExecutor : IModularFeature`, feature name `"ConsoleCommandExecutor"` [S1 `Source/Runtime/Core/Public/HAL/IConsoleManager.h` L894–964].
  - Its methods: `GetName/GetDisplayName/GetDescription/GetHintText`, `GetSuggestedCompletions(const TCHAR*, TArray<FConsoleSuggestion>&)`, `GetExecHistory`, `Exec`, `AllowHotKeyClose`, `AllowMultiLine`, `GetHotKey`, `GetIterateExecutorHotKey`.
  - Register it with `IModularFeatures::Get().RegisterModularFeature(IConsoleCommandExecutor::ModularFeatureName(), &MyExec)`, as the Python plugin does at L1022–1023.
- **The two built-in executors:**
  - **"Python"** (`FName "Python"`): logs the input, then calls `ExecPythonCommand(Input)` (ExecuteFile semantics, including the `.py` sniffing).
  - **"Python (REPL)"** (`"PythonREPL"`): `ExecuteStatement`.
  - Both use console history keys `"Python"` and `"PythonREPL"`, `AllowMultiLine()==true`, and empty completions [S1].
- **The Output Log** enumerates executors through `IModularFeatures`, filtered by `FOutputLogModule::GetConsoleCommandExecutorPermissionList()`. It exposes:
  - `MakeConsoleInputBox(...)`;
  - **`MakeOutputLogWidget(const FOutputLogCreationParams&)`**, with default category selection, a logging limit and settings-menu flags;
  - `UpdateOutputLogFilter(...)`, `FocusOutputLog()`.

  So we can embed a filtered Output Log (for example LogPython only) or a console box inside our tab [S1 `Source/Developer/OutputLog/Public/OutputLogModule.h`, `OutputLogCreationParams.h`].
- **`py` console command.** `FPythonScriptPlugin` is an `FSelfRegisteringExec`. `Exec_Runtime` handles `PY <code|file args>` [S1 L1058–1075]. The docs warn not to use `py` in startup `ExecCmds`, because the editor may not be ready [S6]. Since 5.6, `-execcmds` Python commands are deferred until Python is initialized [S5].
- **Opportunity.** Register a third executor, for example "Python (Script Editor)", that returns **real completions** in `GetSuggestedCompletions`. That is a cheap upgrade to the console the editor already has.

---

## 2. Execution context

### 2.1 Threads and the GIL
- All reflected `unreal.*` calls must happen on the **game thread**. Python is initialized there, and `__main__` and the console dicts live there [S1].
- After init, `PyEval_SaveThread()` stores the main thread state and releases the GIL. Every entry point then takes it on demand with `FPyScopedGIL` (`PyGILState_Ensure/Release`), so background UE threads can enter Python, for example the GC's `FPyReferenceCollector` [S1 `Private/PyGIL.h`, PythonScriptPlugin.cpp L1396].
- `unreal.log*` drops the GIL (`Py_BEGIN_ALLOW_THREADS`) around `UE_LOG`. Python threads (`threading`) therefore run, but **must not touch `unreal` objects** (UNVERIFIED for every API; there is no thread check in the wrappers I read).
- **asyncio precedent (5.8).** ToolsetRegistry runs an asyncio loop that is *polled from the editor's main-thread ticker* (`EditorThreadTicker` → `register_slate_post_tick_callback`/ticker) and marshals cross-thread work through it [S1 `Plugins/Experimental/ToolsetRegistry/Content/Python/toolset_registry/_asyncio/*.py`]. This is Epic's own pattern for "async" Python in the editor.

### 2.2 Long scripts
- A running script blocks the game thread, so Slate does not tick and the **editor freezes**. No UI repaints unless the script uses a slow task.
- `FFeedbackContextEditor::TickSlate` ticks Slate **only when the slow-task window is the active modal window**. Input to other windows, including ours, is blocked [S1 `Source/Editor/UnrealEd/Private/FeedbackContextEditor.cpp` L419–440].
- **Hang/hitch detection.** UE's own long Python operation (stub generation) wraps itself in `FSlowHeartBeatScope` and `FDisableHitchDetectorScope` [S1 PyWrapperTypeRegistry.cpp L2975]. Our runner should do the same for user runs. Whether the editor hang detector would otherwise fire is UNVERIFIED.

### 2.3 `unreal.ScopedSlowTask` / `unreal.ScopedEditorTransaction`
- **`ScopedSlowTask(work, desc='', enabled=True)`** is a context manager [S20][S1 PyCore.cpp L571–800]:
  - `enter_progress_frame(work=1.0, desc='')`;
  - `make_dialog(can_cancel=False, allow_in_pie=False)`, `make_dialog_delayed(delay, …)`;
  - `should_cancel()`;
  - a tick helper.

  Cancel is cooperative only: the script must poll `should_cancel()` [S6][S20].
- **`ScopedEditorTransaction(desc)`** calls `GEditor->BeginTransaction` on enter and `EndTransaction` on exit [S1 `Private/PyEditor.cpp`]:
  - **`EndTransaction` runs even when an exception propagates.** Partial changes stay as one undo entry; they are not rolled back.
  - `cancel()` exists.
  - Not every operation is undoable; import, for example, is not [S6].

### 2.4 Tick and shutdown callbacks
- **Available functions:**
  - `register/unregister_slate_pre_tick_callback`, `…_post_tick_callback` (callable taking a float delta);
  - `register_ticker_callback(callable, delay=0.0)` with `unregister_ticker_callback` (FTSTicker; **new in 5.7** [S4]);
  - `register_python_shutdown_callback` [S19][S1].
- **All of these are skipped** when `CanTickPython()` is false [S1 PyUtil.cpp L1821–1838]: while a user script is on the stack (5.6+ [S5]), while saving packages, during GC, or while routing PostLoad.
- **No `AsyncTask` / `call_on_game_thread` exists** in the Python API [S19]. Python threads need their own queue that a tick callback drains (the ToolsetRegistry pattern).
- `parent_external_window_to_slate(handle, …)` lets Qt/Tk windows parent to Slate [S19].

### 2.5 stdout, stderr, warnings and logging [S1 `Content/Python/unreal_core.py`, PyCore.cpp L1180–1240]
- `sys.stdout` is a line-buffered `TextIOWrapper` over `unreal.log`, which writes **`LogPython` Log** verbosity.
- `sys.stderr` goes to `unreal.log_error` (`LogPython` Error).
- `warnings.showwarning` goes to `log_warning`. Setting `PYTHONUNBUFFERED` gives write-through.
- `unreal.log_flush()` flushes to disk.
- Every log line is also broadcast to the private log-capture delegate that fills `FPythonCommandEx::LogOutput`.
- For **streaming** output into our console, choose one:
  - (a) a Python-side `contextlib.redirect_stdout/stderr` in our runner. This is precise; the **recommendation**.
  - (b) an `FOutputDevice` added to `GLog` filtered on `LogPython`. This catches all Python output, can be called off-thread, and is noisy.

### 2.6 Can a running script be interrupted? (design-critical)
- **Signals.** UE sets `PyConfig.install_signal_handlers = 0` [S1 L1178], so there is no Python SIGINT handler. CPython ignores `PyErr_SetInterrupt[Ex]` when SIGINT is not handled by Python [S27, UNVERIFIED wording, not fetched]. One fix is to install `signal.signal(SIGINT, signal.default_int_handler)` from the game thread, but that also installs a process-wide C SIGINT handler. **Not recommended.**
- **`PyThreadState_SetAsyncExc(game_thread_id, KeyboardInterrupt)`.** It must be called with the GIL held [S26]. A watchdog thread can get the GIL because the eval loop hands it over every switch interval (5 ms) when another thread asks. It fires at the next bytecode boundary. It is documented as "for C extensions" and can be swallowed by `except BaseException`.
- **`Py_AddPendingCall(func, arg)` — recommended.**
  - It needs **no GIL and no thread state**, so it can be called from any thread.
  - `func` runs "from the main interpreter thread" at a **bytecode boundary** while holding the GIL. It can raise by returning -1 with an exception set [S26].
  - UE initializes CPython on the game thread, so the game thread should be "the main thread". **UNVERIFIED in-engine; prototype first.**
  - This gives a safe point **inside** the running script where we can (1) pump a small modal "Running… [Stop]" slow-task window, which is the same re-entrancy profile as a script calling `enter_progress_frame`, and (2) raise `KeyboardInterrupt` if the user clicked Stop.
  - It has none of `sys.settrace`'s overhead and does not clash with debugpy.
- **Limits.**
  - None of these can interrupt a long **C++** call (an asset import, a blocking `unreal.*` function). Those only cancel through the engine's own slow-task cancel.
  - User code with `except BaseException` or a bare `except:` swallows the interrupt.
  - There is no way to kill the game thread, so **always autosave buffers before a run**.
- **Linking.** The Python headers and `python311.lib` ship in the Launcher build under `Source/ThirdParty/Python3/Win64/{include,libs}`, and PythonScriptPlugin depends on the `Python3` module [S2][S1 `PythonScriptPlugin.Build.cs`]. Our module can add `"Python3"` to its private dependencies to call these APIs. ABI then tracks the engine's Python; we rebuild per engine version anyway.
- **Nested loop for an in-process debugger.** `FSlateApplication::EnterDebuggingMode()` / `LeaveDebuggingMode()` "tick in place" in the middle of a stack frame (used by Kismet) [S1 `Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h` L940–951]. Lane 03 relies on it [S28]. Re-entrancy rules are undocumented; high risk.

---

## 3. Configuration

### 3.1 Settings (5.8) [S1 `Private/PythonScriptPluginSettings.h/.cpp`]

**Project Settings > Plugins > Python** (`UPythonScriptPluginSettings`, `config=Engine, defaultconfig`):

| Setting | Default | Notes |
|---|---|---|
| `StartupScripts` (array) | — | Restart required. Run as commands after `init_unreal.py` |
| `AdditionalPaths` | — | Added to `sys.path` |
| `bIsolateInterpreterEnvironment` | **true** | `-I`-like: ignores `PYTHON*` env vars, the script dir and user site |
| `bDeveloperMode` ("all users", advanced) | false | Developer warnings + stub generation |
| `bRunPipInstallOnStartup` | true | pip, see §3.3 |
| `bPipStrictHashCheck` | true | |
| `bOfflineOnly` | false | |
| `OverrideIndexURL`, `ExtraInstallArgs` | — | |
| `bRemoteExecution` | **false** | |
| `RemoteExecutionMulticastGroupEndpoint` | `239.0.0.1:6766` | |
| `RemoteExecutionMulticastBindAddress` | `127.0.0.1` | |
| Send/receive buffer sizes | 2 MiB | |
| `RemoteExecutionMulticastTtl` | **0** | |

**Editor Preferences > Plugins > Python** (`UPythonScriptPluginUserSettings`, `EditorPerProjectUserSettings`):

| Setting | Default | Notes |
|---|---|---|
| `EnablePythonOverride` | None | None / Enable / Disable |
| `bDeveloperMode` | false | Per user. The usual way to turn on stubs |
| `TypeHintingMode` | **AutoCompletion** | Off / AutoCompletion / TypeChecker |
| `bEnableContentBrowserIntegration` | false | |

### 3.2 `sys.path`, `init_unreal.py`, `UE_PYTHONPATH` [S1 L1236–1300, L2063–2097][S6]

`sys.path` gets, in this order:
1. `<UserDir>/UnrealEngine/Python` (Documents);
2. `<root>/Python` for **every mounted content root** (engine, project, each plugin's `Content/Python`), plus their `Lib/site-packages` and `<Platform>/Lib/site-packages` subdirectories;
3. `AdditionalPaths`;
4. `UE_PYTHONPATH` entries. These are read even in isolated mode, and Epic says third-party software should never modify this variable [S6].

Other behavior:
- Mount and unmount events add and remove paths live.
- Every `init_unreal.py` found on any of these paths runs at startup **in the shared console namespace**.
- Our plugin's `Content/Python/init_unreal.py` therefore runs automatically. Keep it tiny; never let it import heavy modules.

### 3.3 pip / third-party packages [S12][S1 `Private/PipInstall.cpp`, `PipRunnable.cpp`]
- **Declaring requirements.** A `.uplugin` `"PythonRequirements": [{ "Platform": "All|Win64|Mac|Linux", "Requirements": ["pkg==x --hash=…"], "ExtraIndexUrls": [...] }]` [S12]. Only binary wheels are allowed; strict hashes are on by default.
- **Where packages go.** The editor merges every enabled plugin's requirements into `<Project>/Intermediate/PipInstall/merged_requirements.in` and `.txt`. It then:
  - creates a **venv** at `<Project>/Intermediate/PipInstall` from the engine interpreter, recreating it if the Python version changed;
  - installs the bundled helper wheels with `--no-index`;
  - runs `pip install --disable-pip-version-check --only-binary=:all: …` in a **subprocess** (`FPlatformProcess::CreateProc`).
- The site-packages directory is **always** added to `sys.path`, which supports pre-populated or offline installs [S1].
- **When it runs.** Init is launched asynchronously and waited for. If packages are missing and `bRunPipInstallOnStartup` (or `-ForcePipInstallOnInit`) is set, the install runs **synchronously on the game thread at startup**, with a modal progress dialog [S1].
- A UBT mode also exists: `RunUBT … -Mode=PipInstall` [S12].
- `IPipInstall` (public, [S16]): `InitPipInstall`, `GetNumPackagesToInstall`, `GetPackageInstallList`, `LaunchPipInstall(bool RunAsync, TSharedPtr<ICmdProgressNotifier>, FSimpleDelegate OnCompleted)`, `IsInstalling`, `RegisterPipSitePackagesPath`.
- `FPipInstallHelper` (modal dialog plus background install) and `FPythonScriptInitHelper::InitPythonAndPipInstall(...)` are public helpers [S17][S18].
- **Version history.** pip install exists by 5.6 at least: 5.6 and 5.7 both fixed PipInstall encoding [S5][S4]. The release that **introduced** it is UNVERIFIED (5.4 is likely).
- **Our use:** declare the language-server, formatter and jedi wheels as `PythonRequirements` in our `.uplugin`, so they install into the project venv with hashes. Alternatively, ship our own isolated toolchain outside UE's interpreter, which is better for LSP processes (see lane 03).

---

## 4. Remote execution protocol (`Content/Python/remote_execution.py` and `Private/PythonScriptRemoteExecution.cpp`) [S1]

- **Transport.**
  - **UDP multicast** for discovery and control: group **239.0.0.1:6766**, bind address **127.0.0.1**, **TTL 0** (local host only).
  - **TCP** for commands, and the *client* hosts the TCP server: default **127.0.0.1:6776**. The editor connects *to* the client after an `open_connection` message.
  - Python-side constants: ping every 1 s; a node times out after 5 s.
- **Messages.** UTF-8 JSON `{version:1, magic:"ue_py", type, source, dest?, data?}` with these types:
  - `ping` (UDP);
  - `pong` (UDP) with `{user, machine, engine_version, engine_root, project_root?, project_name?}`;
  - `open_connection` with `{command_ip, command_port}`;
  - `close_connection`;
  - `command` (TCP) with `{command, unattended, exec_mode: ExecuteFile|ExecuteStatement|EvaluateStatement}`;
  - `command_result` (TCP) with `{success, command, result, output:[{type, output}]}`.
- **Framing is naive.** There is no length prefix. The Python client reads until a `recv` returns fewer than 8192 bytes. Large outputs or a slow peer can split a message, so this is fragile for big payloads [S1].
- **Security.** There is **no authentication or encryption**. Enabling it lets any local process execute arbitrary Python in the editor. Binding to `0.0.0.0` with TTL ≥ 1 exposes **LAN-wide remote code execution**. It is off by default [S1].
- **Relevance.** An in-process editor does not need it. We should *not* enable it implicitly. We could offer a compatible client only for "send to another editor instance". Lane 03 notes this channel is the main pain point of VS Code users [S28].

---

## 5. The `unreal.py` stub [S1 `Private/PyWrapperTypeRegistry.cpp` L2969–3310, PythonScriptPlugin.cpp L1567–1612]

- **Location:** a single file, `<Project>/Intermediate/PythonStub/unreal.py`.
- **When it is generated:** only when Developer Mode (project or user) is on, `GIsEditor`, and **not in a commandlet**. It runs after startup scripts, and again **2 s after modules change** (debounced). Generation shows a modal slow task and suspends the heartbeat and hitch detectors.
- **Content:**
  - every wrapped type, with docstrings (the online-docs commandlet `PythonOnlineDocsCommandlet` reuses it for Sphinx);
  - `from __future__ import annotations` at the top when type hinting is on (default **AutoCompletion**; **TypeChecker** gives fuller, `Optional`-aware hints);
  - an "Additional Code" tail.
- **Size:** UNVERIFIED for 5.8. Lane 03 found >21 MB in 5.1, ~24 MB in 2025, and 53 MB on plugin-heavy projects. That is enough to hit Pyright's 50 MB limit and Jedi's 10 MB cropping [S28].
- **How IDEs consume it:**
  - VS Code/Pylance via `python.analysis.extraPaths` pointing at `Intermediate/PythonStub`;
  - PyCharm via an interpreter path entry, with Epic advising a larger file-size limit [S28].
- **Our editor:** we can regenerate on demand (toggle Developer Mode, or drive `unreal` stub generation) and post-process into a split `.pyi` package. **Do not rely on the user having Developer Mode on.**

---

## 6. Slate and editor APIs for the editor UI

### 6.1 Text widgets (`Source/Runtime/Slate/Public/Widgets/Text/SMultiLineEditableText.h`, `…/Input/SMultiLineEditableTextBox.h`) [S1]
- **Arguments:**
  - `Text`, `HintText`, `SearchText`, **`Marshaller`**;
  - `WrapTextAt`, `AutoWrapText`, `WrappingPolicy`, `TextStyle`, `Font`, `Margin`, `LineHeightPercentage`;
  - `IsReadOnly`, `H/VScrollBar`, `AllowContextMenu`, `ContextMenuExtender`;
  - **`OnKeyDownHandler`**, `OnKeyCharHandler`, `OnCursorMoved`, `OnTextChanged`, `OnTextCommitted`;
  - **`CreateSlateTextLayout`**, `ModiferKeyForNewLine`, `TextShapingMethod`, `TextFlowDirection`.
- **Methods:**
  - `GetText/SetText`, `GetTextLine`, `GetCurrentTextLine`;
  - `InsertTextAtCursor`, `GoTo`, `ScrollTo`, `SelectText`, `GetSelection`, `ApplyToSelection`;
  - `BeginSearch`/`AdvanceSearch`, `GetCursorLocation`, `GetCharacterAt`, `Refresh`, `ForceScroll`.
- **Missing pieces:** no line-number gutter, folding, multi-cursor, minimap or incremental-edit API. The experimental CodeEditor's `SCodeEditableText` only overrides `OnKeyChar`, to insert a literal `\t` [S1].

### 6.2 Syntax-highlighting framework (`Framework/Text/SyntaxTokenizer.h`, `SyntaxHighlighterTextLayoutMarshaller.h`) [S1]
- `ISyntaxTokenizer::Process(TArray<FTokenizedLine>&, const FString&)` produces tokens of type `Syntax` or `Literal` with an `FTextRange`.
- `FSyntaxTokenizer::Create(TArray<FRule>)` takes the **first rule in list order** whose text matches at the current offset; otherwise it consumes up to the next word boundary as a literal [S1 `Private/Framework/Text/SyntaxTokenizer.cpp` L52–80]. Two consequences:
  - rules must be ordered longest-first;
  - keyword rules also match identifier *prefixes* (`if` in `iffy`), so the marshaller must check word boundaries.

  It knows nothing of grammar: strings and comments are handled by a state machine in the marshaller.
- `FSyntaxHighlighterTextLayoutMarshaller : FPlainTextLayoutMarshaller` has a pure virtual `ParseTokens(...)` and `EnableSyntaxHighlighting(bool)`.
- `FRichTextSyntaxHighlighterTextLayoutMarshaller` highlights rich-text markup.
- **Engine examples to copy:**
  - **`FHLSLSyntaxHighlighterMarshaller`**: public in UnrealEd (`Source/Editor/UnrealEd/Public/Text/HLSLSyntaxHighlighterMarshaller.h` plus `HLSLSyntaxTokenizer.h`). Eight styles; parse states None, String, Char, single-line and multi-line comment.
  - Niagara `NiagaraHLSLSyntaxHighlighter`.
  - PCG `PCGHLSLSyntaxHighlighter` / `SPCGCodeEditor(TextBox)`; 5.8 added font size.
  - Optimus (Deformer Graph) and ComputeFramework HLSL highlighters.
  - `TemplateStringSyntaxHighlighterMarshaller` and the NamingTokens marshaller [S2][S3].
- **No Python highlighter exists in the engine** [S2].

### 6.3 Performance limits (verified mechanisms; numbers UNVERIFIED)
- `RequiresLiveUpdate()` returns true whenever highlighting is on. After every edit `FSlateEditableTextLayout` calls `ForceRefreshTextLayout`, which:
  - **`ClearLines()`, `ClearLineHighlights()`, `ClearRunRenderers()`**;
  - calls `Marshaller->SetText(full string)`, which **re-tokenizes everything**;
  - rebuilds every line and run [S1 `Private/Widgets/Text/SlateEditableTextLayout.cpp` L420–441, L3309–3314, L681–695; `ITextLayoutMarshaller.h` comment: "costly, but required for things like syntax highlighting"].

  So cost is O(document) per keystroke.
- `OnTextChanged` hands back the **whole document as `FText`**.
- Undo stores a **full `FText` snapshot per step, up to 99** (`EditableTextDefs::MaxUndoLevels = 99`) [S1 `Private/Framework/Text/TextEditHelper.h`].
- **Implications:**
  - SMultiLineEditableText is fine for small scripts (a few thousand lines; UNVERIFIED, benchmark on 5.8).
  - For "production-ready" we need either (a) a **custom Slate code view** with our own piece-table/rope text model, incremental tokenizer and virtualized painting, or (b) **Monaco in CEF**.
  - **Precedent for (a):** 5.8's `STerminal : SLeafWidget` paints a character grid with `FSlateDrawElement::MakeText`/`MakeBox` in `OnPaint` [S1 `Plugins/Experimental/Terminal/Source/Terminal/Public/STerminal.h` L21–56].

### 6.4 Docking, menus, commands and settings
- **Tabs.**
  - `FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab)` with `SDockTab`, and `TryInvokeTab`.
  - For a multi-document editor, an `FTabManager` per major tab, as `CodeEditor` does with `FCodeProjectEditor` / `GetMajorTabForTabManager` [S1].
  - Header: `Source/Runtime/Slate/Public/Framework/Docking/TabManager.h`.
- **Menus.** `UToolMenus` (`Source/Developer/ToolMenus/Public/ToolMenus.h`). Useful 5.8 extension points:
  - `LevelEditor.MainMenu.Tools`, section `"Python"`, which holds the existing Python entries;
  - `ContentBrowser.ItemContextMenu.PythonData`, for "Open in Script Editor" on `.py` items;
  - `OutputLog.ConsoleInputBox.CmdExecMenu` [S1].
- **Keybindings.** `TCommands<T>`, `FUICommandInfo`, `FUICommandList`, `FInputChord` (`Framework/Commands/Commands.h`, `UICommandList.h`). Bindings appear and can be rebound in Editor Preferences > Keyboard Shortcuts; 5.8 adds keybind search there [S3].
- **Preferences.** `UDeveloperSettings` now lives in the **DeveloperSettings** module (`Source/Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h`). Use `config=EditorPerProjectUserSettings` for per-user settings. 5.8 adds "copy associated CVar" on settings [S3].

### 6.5 Web view (Monaco option)
- `SWebBrowser` (module **WebBrowser**) and `IWebBrowserWindow` provide:
  - `ExecuteJavascript(FString)`;
  - **`BindUObject(Name, UObject*, bIsPermanent=true)`**, which exposes the object's UFUNCTIONs as `window.ue.<name>`. **All names are lower-cased on the JS side.** Non-permanent bindings must be registered in `OnLoadStarted`.
  - `UnbindUObject` [S1 `Source/Runtime/WebBrowser/Public/IWebBrowserWindow.h` L347–375, `SWebBrowser.h` L199–217].
- **5.8 CEF is `128.4.13+ge76af7e+chromium-128.0.6613.138`** (Win64/WinArm64/Linux), with helper `Binaries/Win64/EpicWebHelper.exe` [S2].
- The 5.8 AI Assistant itself is built this way: `SAIAssistantWebBrowser`, a JavaScript delegate binder and executor [S1].
- Risks: IME and keyboard shortcuts routing between Slate and CEF, GPU and offscreen rendering, and Chromium version lag. All UNVERIFIED for 5.8; prototype.

### 6.6 Subprocesses (language servers, formatters)
- `FMonitoredProcess(URL, Params, Hidden, bCreatePipes)` has `OnOutput(FString line)`, `OnCompleted(int)`, `OnCanceled`, `Cancel(bKillTree)`.
- `FInteractiveProcess` adds `SendWhenReady(FString | TArray<uint8>)` [S1 `Source/Runtime/Core/Public/Misc/MonitoredProcess.h`, `InteractiveProcess.h`].
- **Both are line-splitters:** `ParseIntoArray(…"\n")` per pipe read. `FInteractiveProcess` drops empty lines and **logs every child line** to `LogInteractiveProcess` [S1 `Private/Misc/InteractiveProcess.cpp` L133–147]. **That makes them unusable for LSP framing.**
- **Use instead:**
  - `FPlatformProcess::CreatePipe(Read, Write, bWritePipeLocal)`;
  - `CreateProc(…, PipeWriteChild, PipeReadChild[, PipeStdErrChild])`, or the newer `CreateProc(const UE::HAL::FProcessStartInfo&)` (StdIn/StdOut/StdErr handles, `bInheritHandles`, `bBreakawayFromJob`);
  - `ReadPipeToArray` (binary), `WritePipe(const uint8*, int32)`, `IsProcRunning`, `TerminateProc(KillTree)`, `GetProcReturnCode`.

  Run these on our own `FRunnable` and marshal results to the game thread [S1 `GenericPlatform/GenericPlatformProcess.h` L196–209, L604–871].

### 6.7 Files, source control, lifecycle, crashes
- **File watching.** `IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle(Dir, FDirectoryChanged, OutHandle, Flags)` and `Unregister…` (module `DirectoryWatcher`) [S1].
- **Source control.** `ISourceControlModule` / `ISourceControlProvider` (module `SourceControl`), plus `USourceControlHelpers`. Note that commandlets must enable source control themselves (`FScopedSourceControl`); the Python commandlet does so since 5.8 [S1][S3].
- **Editor delegates.** `FEditorDelegates::PreBeginPIE/EndPIE`, `OnMapOpened`, `OnAssetsPreDelete`, `OnEditorInitialized`, `OnShutdownPostPackagesSaved` [S1 `Source/Editor/UnrealEd/Public/Editor.h`].
- **Core delegates.** `FCoreDelegates::OnEnginePreExit` (before core modules shut down), `OnPreExit`, `OnExit`, `OnHandleSystemError/Ensure`, `OnShutdownAfterError`.
- **5.8 deprecations:** `FCoreDelegates::OnPostEngineInit` in favor of **`GetOnPostEngineInit()`**, and `PreSlateModal/PostSlateModal` in favor of `…WithContext` [S1 `Misc/CoreDelegates.h` L207–262].
- **Python side:** `IPythonScriptPlugin::OnPythonShutdown` and `unreal.register_python_shutdown_callback`.
- **Crash context.** `FGenericCrashContext::SetGameData/SetEngineData(Key, Value)`, `AddPlugin`, `OnAdditionalCrashContextDelegate()` (an `FCrashContextExtendedWriter&`), `SetCrashTrigger` [S1 `GenericPlatform/GenericPlatformCrashContext.h`]. Use it to stamp "script running: <file>:<line>" into crash reports. An exception in a C extension or a native crash inside Python **takes the editor down**, so autosave matters.

---

## 7. Editor Utility Widgets and Blueprint integration

- **`UPythonScriptLibrary`** (Category "Python|Execution") [S1 `Private/PythonScriptLibrary.h`]:
  - `IsPythonAvailable/Configured/Initialized`, `ForceEnablePythonAtRuntime`;
  - `ExecutePythonCommand(MultiLine string)`;
  - **`ExecutePythonCommandEx(cmd, out CommandResult, out LogOutput, ExecutionMode=ExecuteFile, FileExecutionScope=Private)`** ("Advanced");
  - **"Execute Python Script"**, a `CustomThunk`/variadic node (`K2Node_ExecutePythonScript`) with named string inputs and outputs, mapped to Python variables.

  It is editor-only, because the module type is UncookedOnly [S1][S6].
- **`UEditorPythonScriptingLibrary`**: `SetKeepPythonScriptAlive(bool)` and `GetKeepPythonScriptAlive()`, which keep the editor alive after `-ExecutePythonScript` for tick-driven scripts [S1].
- **Startup and actions.**
  - Since 5.6, Editor Utility classes can opt in to "Run on Start-up" [S5].
  - In 5.8, `UAssetActionUtility` must declare `SupportedClasses` [S3].
  - Python can define `@unreal.uclass()` subclasses of `AssetActionUtility` / `ActorActionUtility` / `EditorUtilityToolMenuEntry` (UNVERIFIED exact class list in 5.8) and register `UToolMenus` entries via `unreal.ToolMenus`.
- **How our editor can expose "run this script from Blueprint, a menu or a shelf":**
  1. A `UBlueprintFunctionLibrary` `RunScriptFile(Path, Args)` that calls our runner, giving structured results and the same namespace and cancel semantics as the editor.
  2. A "shelf" asset or `UDeveloperSettings` list that generates `UToolMenus` entries and `TCommands` hotkeys at startup.
  3. Content Browser context-menu entries on `ContentBrowser.ItemContextMenu.PythonData`.
  4. Optionally a ToolsetRegistry toolset, so MCP agents can call the same entry points (§0).

---

## 8. Commandlets, headless runs and testing

- **`UnrealEditor-Cmd.exe <proj> -run=pythonscript -script="file.py args"` (or literal code)** [S1 `Private/PythonScriptCommandlet.cpp`]:
  - force-enables Python and runs startup scripts;
  - enables source control (5.8);
  - runs the command with `Unattended`;
  - returns **0 on success, -1 on error**, which makes it the CI-friendly path.
  - It does not load levels automatically [S6].
  - There is no stub generation in commandlets [S1].
  - Startup scripts also run in other commandlets through `OnCommandletPreMain`, unless that commandlet is on the `DisablePythonForCommandlet` list [S1].
- **`UnrealEditor.exe <proj> -ExecutePythonScript="file.py args"`** [S1 `Private/EditorUtilities/EditorPythonExecuter.cpp`]:
  - waits for GWorld, GEditor and an asset registry that has finished loading;
  - runs Unattended, then defers `QUIT_EDITOR`;
  - **`-ScriptErrorsAreFatal`** turns an error into a Fatal log, with the crash trigger marked Deliberate;
  - it cannot be used from a commandlet.
- **Automation framework** (5.8 `Source/Runtime/Core/Public/Misc/AutomationTest.h`):
  - `IMPLEMENT_SIMPLE_AUTOMATION_TEST` / `IMPLEMENT_COMPLEX_AUTOMATION_TEST`;
  - **`DEFINE_SPEC` / `BEGIN_DEFINE_SPEC`–`END_DEFINE_SPEC`** (Automation Spec);
  - `DEFINE_LATENT_AUTOMATION_COMMAND`;
  - flags such as `EditorContext`, `EngineFilter`, `ProductFilter`, `SmokeFilter`.

  Plugins `CQTest`, `CQTestExperimental` and `CQTestEnhancedInput` ship [S2]. Gauntlet ships in `Source/Programs/AutomationTool/Gauntlet` [S2]. Run with `-ExecCmds="Automation RunTests <Filter>; Quit" -unattended -nullrhi` (standard usage; UNVERIFIED flags for 5.8).
- **Python tests inside UE:**
  - **PythonAutomationTest** plugin (`Plugins/Tests`, Beta). It provides `automation_test.TestRunner`, an `AutomationScheduler` that runs generators one step per post-tick for latent tests, and `unreal.PyAutomationTest` latent-command helpers [S1].
  - **ToolsetRegistry `FPythonAutomationTest`** (5.8, Experimental, NoRedist) discovers `unittest` cases by module glob and registers them as Automation tests: `unreal.PythonTestRunner.create('My.Tests', unreal.PythonTestRunnerSearchOptions(root_module=…, test_name_globs=[…]))`. Default per-test timeout is 5 s. Class- and module-level setUp/tearDown run around every case [S1 `Plugins/Experimental/ToolsetRegistry/Source/ToolsetRegistry/Private/ToolsetRegistry/PythonTestRunner.h`, `Content/Python/init_unreal.py`].
- **Suggested CI for our plugin:**
  1. C++ Automation Specs for the text model, tokenizer and LSP framing, run under `-nullrhi`.
  2. A Python `unittest` suite for the runner module (namespace, traceback mapping, cancel), run via `-run=pythonscript` (exit code) or wrapped with `PythonTestRunner`, if we accept an Experimental dependency.
  3. Gauntlet only for full-editor UI smoke tests.

---

## Constraints our design must respect

- **Game thread only** for all `unreal.*` work and for Slate. Use the GIL (`PyGILState_Ensure`) for every C-API call from any thread. Never hold the GIL while blocking on the game thread.
- **A running script freezes the editor.** Our UI cannot respond during a run unless we pump a **modal** window from inside the script, through slow tasks or a pending-call safe point. Plan "Stop" around `Py_AddPendingCall` and slow-task cancel. Expect no interruption of long C++ calls, and **autosave before every run**.
- **Python tick callbacks do not fire while a user script runs**, during GC, during package saves or during PostLoad (5.6+). Our own Python-side helpers cannot rely on ticks during execution.
- **Do not send buffers through `ExecuteFile`+literal code** (`.py` sniffing, `"<string>"` filename, shared console namespace). Use our runner module with `compile(src, real_or_virtual_path, 'exec')` and a namespace per document or run.
- **`SystemExit(0)` is swallowed and `sys.excepthook` is shared.** `print` goes to `LogPython` at Log verbosity; stderr goes at Error.
- **`init_unreal.py` runs in the shared console namespace for every plugin.** Keep ours minimal and side-effect-free. Respect restrictive mode (`Engine.Python.IsPythonInRestrictiveMode`) and lazy Python (`IsEnabledByDefault=false`): never force-enable Python without user intent.
- **Python is 3.11.8** (no `sys.monitoring`, no free-threading). Abstract the debugger and any compiled extension against a later 3.13 move. Rebuild per engine version.
- **The stock Slate editable text is O(document) per keystroke and keeps 99 full-text undo snapshots.** It is not acceptable for large files. Use a custom widget or CEF.
- **LSP needs a binary-safe pipe transport.** No `FInteractiveProcess`/`FMonitoredProcess`.
- **Remote execution is unauthenticated.** Never enable it silently; default to loopback and TTL 0.
- **The plugin depends on PythonScriptPlugin** (Beta, UncookedOnly, PreDefault). It is editor-only. Gate on `RegisterOnPythonInitialized`.
- **pip installs run on the game thread at startup** when packages are missing. Declaring `PythonRequirements` in our `.uplugin` affects every user's startup. Prefer an isolated toolchain for heavy LSP dependencies.
- **Experimental/NoRedist 5.8 plugins** (AIAssistant, MCP, Terminal, ToolsetRegistry) can be integration targets, but must stay **optional** dependencies.

## APIs we will depend on

| API | Module | Purpose | Stability / risk |
|---|---|---|---|
| `IPythonScriptPlugin` (`ExecPythonCommandEx`, `RegisterOnPythonInitialized`, `OnPythonShutdown`, `GetInterpreterExecutablePath`) | PythonScriptPlugin (Public) | Run code; lifecycle | Medium. Plugin is Beta. The interface grew in 5.6 and 5.8; additive so far |
| `FPythonCommandEx`, `EPython*` enums | PythonScriptPlugin (`PythonScriptTypes.h`) | Exec options; results | Low. Unchanged in shape for years |
| `Python3` third-party module (`Py_AddPendingCall`, `PyGILState_*`, `PyThreadState_SetAsyncExc`) | ThirdParty/Python3 (headers + `python311.lib` ship) | Cancel injection; direct C-API | Medium. ABI tied to the engine's Python (3.11 now). In-engine pending-call behavior unverified |
| Our Python runner module (`Content/Python/…`) via `EvaluateStatement` | (ours) | Real filenames, per-doc namespaces, stdout capture, structured tracebacks | Low. Pure Python, uses public `unreal` API |
| `IConsoleCommandExecutor` + `IModularFeatures` | Core | Optional Output Log executor with completions | Low |
| `FOutputLogModule::MakeOutputLogWidget/MakeConsoleInputBox/UpdateOutputLogFilter` | OutputLog | Embedded filtered log and console | Low–medium |
| `SMultiLineEditableText`, `FSyntaxHighlighterTextLayoutMarshaller`, `ISyntaxTokenizer` | Slate | MVP editor or small-file mode | Low API risk; **high perf risk** |
| `SLeafWidget`, `FSlateDrawElement::MakeText/MakeBox`, `FSlateFontMeasure`, `FSlateApplication` input routing | SlateCore / Slate | Custom virtualized code view | Low API risk; high effort (IME, accessibility) |
| `FHLSLSyntaxHighlighterMarshaller` (reference) | UnrealEd | Pattern to copy | Low |
| `FGlobalTabmanager::RegisterNomadTabSpawner`, `FTabManager`, `SDockTab` | Slate | Dockable panels; multi-document | Low |
| `UToolMenus` (`LevelEditor.MainMenu.Tools`/"Python", `ContentBrowser.ItemContextMenu.PythonData`) | ToolMenus | Menus and toolbars | Low; menu names can move between versions |
| `TCommands`, `FUICommandList`, `FInputChord` | Slate | Rebindable shortcuts | Low |
| `UDeveloperSettings` | DeveloperSettings | Project and user preferences | Low |
| `SWebBrowser`, `IWebBrowserWindow::BindUObject/ExecuteJavascript` | WebBrowser (CEF 128 in 5.8) | Monaco option | Medium–high. CEF upgrades, IME/focus quirks, lower-cased binding names |
| `FPlatformProcess::CreatePipe/CreateProc(FProcessStartInfo)/ReadPipeToArray/WritePipe/TerminateProc` | Core | LSP/formatter subprocesses | Low |
| `IDirectoryWatcher` | DirectoryWatcher | External file changes | Low |
| `ISourceControlModule/Provider`, `USourceControlHelpers` | SourceControl | Check-out on save, diff | Low–medium (provider differences) |
| `FScopedSlowTask`/`FSlowTask`, `FSlowHeartBeatScope`, `FDisableHitchDetectorScope` | Core | Progress/cancel UI during runs; suppress hang detector | Low |
| `FScopedTransaction` / `GEditor->Begin/EndTransaction` | UnrealEd | Undo grouping for runs | Low |
| `FEditorDelegates`, `FCoreDelegates::OnEnginePreExit/GetOnPostEngineInit` | UnrealEd / Core | Lifecycle, PIE, autosave on exit | Low (note the 5.8 deprecation) |
| `FGenericCrashContext::SetGameData`, `OnAdditionalCrashContextDelegate` | Core | Crash breadcrumbs | Low |
| `FSlateApplication::EnterDebuggingMode/LeaveDebuggingMode` | Slate | In-process debugger nested loop | **High**. Kismet-oriented, undocumented re-entrancy |
| `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, `DEFINE_SPEC`; CQTest | Core / Plugins/Tests | Our C++ tests | Low |
| `unreal.PythonTestRunner` (`FPythonAutomationTest`) | ToolsetRegistry (Experimental, NoRedist) | Python unittest → Automation | **High**. Experimental; header is Private |
| `remote_execution.py` protocol | PythonScriptPlugin | Optional "send to other instance" | Low change risk; **security risk** |
| ToolsetRegistry `@tool_call` / `unreal.ToolsetDefinition` | ToolsetRegistry (Experimental) | Optional MCP/AI integration | High (Experimental) |

## 5.8-specific unknowns to verify against the real engine

1. **`Py_AddPendingCall` on the game thread.** Does the callback run there during a long pure-Python loop started via `ExecPythonCommandEx`? Is pumping a modal slow-task window from inside it safe? Is it already stable on 5.8.0 vs 5.8.2?
2. **Hang detector.** Does a 60 s+ script trip the editor hang or hitch detector without `FSlowHeartBeatScope`?
3. **Stub on 5.8.** Measure the size and generation time of `Intermediate/PythonStub/unreal.py`, with AutoCompletion vs TypeChecker hints, on a stock project and on a plugin-heavy one.
4. **Slate text performance** on 5.8. Measure keystroke latency and memory at 1k / 10k / 50k lines with the HLSL-style marshaller.
5. **CEF 128 in editor tabs.** Check IME, clipboard, shortcut routing (Ctrl+S / Ctrl+Z conflicts), DPI, GPU/offscreen mode and memory per instance.
6. **Restrictive mode.** What does `Engine.Python.IsPythonInRestrictiveMode` block beyond pip and the startup-script filter? Does it affect `ExecPythonCommandEx` from plugins?
7. **`-DisablePipInstall`** is documented [S12] but missing from the 5.8 plugin source. Confirm whether UBT or another module handles it.
8. **AI Assistant.** Does the 5.8 web app trigger Python execution through any bound UFUNCTION? Inspect `AIAssistantSubsystem.h` UFUNCTIONs at runtime, and check what the remote app requests.
9. **MCP / EditorToolset.** Is any shipped tool able to run *unrestricted* Python? `execute_tool_script` is sandboxed.
10. **Output Log executor permission list.** Can a project disallow third-party executors? Does ours appear in the 5.8 Output Log drawer and in the debug console (`~`)?
11. **`FCoreDelegates::OnPostEngineInit` deprecation** and other 5.8 deprecations hit by our code (compile with warnings as errors).
12. **The Python version for 5.8.x hotfixes** (expected 3.11.8 [S22]), and early signals of 3.13 in 5.9 previews.

## Open questions

- Execution path: is a Python-side runner via `EvaluateStatement` enough, or do we link `Python3` directly for cancel and per-document namespaces? (Leaning: runner module + minimal C-API for `Py_AddPendingCall`.)
- Editor surface: a custom Slate code view (own text model) vs Monaco in CEF? This affects IME, theming, accessibility and the effort estimate. It needs the perf benchmark from unknown #4.
- Should we register an `IConsoleCommandExecutor` with completions as an early, low-cost deliverable?
- Do we take optional integration with ToolsetRegistry/MCP (Experimental, NoRedist) in v1, or defer it?
- pip: should we declare our tooling wheels in `PythonRequirements` (affects every user's startup and needs hashes), or ship an out-of-engine toolchain?
- Debugger: debugpy (external, VS Code-compatible, `debugpy_unreal.py` exists) vs an in-process `bdb`-based debugger with a Slate nested loop. Coordinate with lane 03.

## Sources

- [S1] Local Epic Games Launcher install of **Unreal Engine 5.8**, `C:\Program Files\Epic Games\UE_5.8\Engine\…` (installed 2026-10-28). Files read directly. Paths are cited inline, relative to `Engine/`. Key files:
  - `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/{Public,Private}/*`, `PythonScriptPlugin.uplugin`, `Content/Python/{remote_execution,unreal_core,debugpy_unreal}.py`;
  - `Binaries/ThirdParty/Python3/Win64/NEWS.txt`;
  - `Source/Runtime/Core/Public/HAL/IConsoleManager.h`;
  - `Source/Runtime/Slate/{Public,Private}/…`;
  - `Source/Developer/OutputLog/…`;
  - `Plugins/Experimental/{AIAssistant,CodeEditor,Terminal,ToolsetRegistry,Toolsets,ModelContextProtocol,PythonFoundationPackages}/…`;
  - `Plugins/Tests/PythonAutomationTest/…`;
  - `Config/BaseEditor.ini`.
- [S2] UE 5.8 Launcher manifest (`UE_5.8/.egstore/Pending/AD0AA508415007B5627220ABEF1185C8.manifest`), decompressed to get the full 5.8 file list (CEF folder names, wheel names, ThirdParty/Python3 libs, plugin inventory).
- [S3] Unreal Engine 5.8 Release Notes — https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes?lang=en-US
- [S4] Unreal Engine 5.7 Release Notes — https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-7-release-notes?application_version=5.7
- [S5] Unreal Engine 5.6 Release Notes — https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-6-release-notes?application_version=5.6
- [S6] Scripting the Unreal Editor Using Python (5.8) — https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python
- [S7] Same page, 5.3 — https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python?application_version=5.3
- [S8] Same page, 5.4 — …?application_version=5.4
- [S9] Same page, 5.5 — …?application_version=5.5
- [S10] Same page, 5.6 — …?application_version=5.6
- [S11] Same page, 5.7 — …?application_version=5.7
- [S12] Python Plugin Dependency Management — https://dev.epicgames.com/documentation/en-us/unreal-engine/python-plugin-dependency-management-for-unreal-engine
- [S13] IPythonScriptPlugin API (5.8) — https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PythonScriptPlugin/IPythonScriptPlugin
- [S14] FPythonCommandEx API — https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PythonScriptPlugin/FPythonCommandEx
- [S15] Enum and struct API pages: `…/PythonScriptPlugin/EPythonCommandExecutionMode`, `…/EPythonCommandFlags`, `…/EPythonFileExecutionScope`, `…/EPythonLogOutputType`, `…/FPythonLogOutputEntry` (dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PythonScriptPlugin/…)
- [S16] IPipInstall API — https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/PythonScriptPlugin/IPipInstall?lang=en-US
- [S17] FPythonScriptInitHelper API — https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PythonScriptPlugin/FPythonScriptInitHelper
- [S18] PythonScriptPlugin module index — https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PythonScriptPlugin
- [S19] Unreal Python API, `unreal` module (5.8) — https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/module/unreal?application_version=5.8
- [S20] Unreal Python API, ScopedSlowTask — https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/ScopedSlowTask?application_version=5.6
- [S21] Unreal MCP in Unreal Editor — https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor?lang=en-US
- [S22] Epic forums, "Unreal Engine 5.8 is on Python 3.11 when the VFX Reference Platform 2026 recommends Python 3.13…" (staff reply 2026-07-23) — https://forums.unrealengine.com/t/unreal-engine-5-8-is-on-python-3-11-when-the-vfx-reference-platform-2026-recommends-python-3-13-as-minimum-version/2745703
- [S23] Epic, "Unreal Engine 5.7 is now available" — https://www.unrealengine.com/news/unreal-engine-5-7-is-now-available (seen via search-result excerpt only; not fetched)
- [S24] GameFromScratch, "Unreal Engine 5.7 Released" — https://gamefromscratch.com/unreal-engine-5-7-released/ (search-result excerpt only)
- [S25] Mundus Creatus, "Forward Looking: AI Assistant" (2025-09-04, reading of `ue5-main`) — https://www.munduscreatus.be/forward-looking-ai-assistant/
- [S26] Python 3.11 C-API, Initialization, Finalization and Threads (`Py_AddPendingCall`, `PyThreadState_SetAsyncExc`) — https://docs.python.org/3.11/c-api/init.html
- [S27] Python 3.11 C-API, Exception Handling (`PyErr_SetInterruptEx`) — https://docs.python.org/3.11/c-api/exceptions.html (not fetched this session; behavior stated from prior knowledge, UNVERIFIED)
- [S28] Sibling research, `docs/research/03-ide-features-lsp-dap.md` (stub size and LSP limits, debugger notes)
- [S29] Blueprint API, Get AIAssistantSubsystem (5.8) — https://dev.epicgames.com/documentation/unreal-engine/BlueprintAPI/EditorSubsystems/GetAIAssistantSubsystem?lang=en-US
