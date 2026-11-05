> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 07d: Verification of stub generation, runtime docstrings, Python tests and pip packaging (UE 5.8 source)

Date: 2026-10-28. Method: read the local UE 5.8 install (`C:\Program Files\Epic Games\UE_5.8\Engine`, read-only). No web searches. Every claim cites `Engine/<path>:<line>`.

To keep the citations short, this file uses one alias:

- `PSP/` = `Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/`

## Summary

1. **Stub.** Developer Mode makes the editor write one file, `<Project>/Intermediate/PythonStub/unreal.py`. Either the project-wide or the per-user setting turns it on.
   - It is written about 2 s after Python finishes starting, and again 2 s after any module loads or unloads. Every write shows a modal "Generating unreal.py..." slow-task dialog.
   - The stub has docstrings for classes, methods, properties and constants. It has PEP 484 type hints unless `TypeHintingMode` is `Off` (the default is `AutoCompletion`).
   - The generator knows each type's C++ module and plugin. Every class docstring ends with a `**C++ Source:** Plugin / Module / File` block, and the registry keeps a module-to-types map.
   - There is **no console command and no Python or public C++ API** to regenerate on demand. The one headless route is the `-run=PythonOnlineDocs` commandlet. It writes the same file whether or not Developer Mode is on, but it also tries to write Sphinx files into the engine plugin folder.
2. **Runtime docstrings.** They are real and rich:
   - Classes carry the tooltip, the C++ source block and a full `**Editor Properties:**` list that includes inherited properties.
   - Methods start with a signature line, `x.name(a, b=1) -> T` (`X.` for static), then a Google-style docstring with `Args:` and `Returns:`.
   - Properties read `(Type): [Read-Write] tooltip`.
   - `inspect.signature` **fails** (`ValueError`) on wrapped UFunctions and on native glue methods, because no `__text_signature__` exists in the format CPython expects. Signature help has to parse the docstring's first line, or use stubs.
3. **`PyGenUtilAccessor`.** It is in the module's `Internal/` folder and its header comment says "Not available for non-Engine use". Its functions are still exported (`PYTHONSCRIPTPLUGIN_API`).
   - UBT adds `Internal/` include paths only for modules in the same rules scope, or modules with `bTreatAsEngineModule`. A project or Fab plugin cannot include the header by default.
   - The accessor only covers class, function and property names, property types and export filters. Struct, enum and delegate names are missing.
   - **Easiest third-party route:** walk the already-wrapped types at runtime. Every native type sits in a per-C++-module Python module `_unreal_<module>` in `sys.modules`, and each has the rich `__doc__` above. This needs no Developer Mode and no engine-internal header.
4. **Tests.** There are two in-engine mechanisms:
   - **PythonAutomationTest** (`Engine/Plugins/Tests`, Beta, off by default) makes one automation test per `test_*.py` file found under the project's and every enabled plugin's `Content/Python`. Tests are named `Editor.Python.<Project|Plugin>.<dotted path>`.
   - **`unreal.PythonTestRunner`** lives in the **Experimental ToolsetRegistry plugin**, not in PythonScriptPlugin. It is off by default and marked `"NoRedist": true`. It registers individual `unittest` cases as automation tests. The default timeout is 5 s per test.
   - `-run=PythonScript -Script=...` runs a file or literal code unattended, after the startup scripts. It returns 0 or -1.
5. **pip.**
   - Format: `.uplugin` `"PythonRequirements": [{Platform, Architecture, Requirements[], ExtraIndexUrls[]}]`.
   - Location: a venv at `<Project>/Intermediate/PipInstall`, which the `UE_PIPINSTALL_PATH` environment variable can override.
   - Command: `pip install --only-binary=:all:`, plus `--require-hashes` by default.
   - Settings: `bRunPipInstallOnStartup` (true), `bPipStrictHashCheck` (true), `bOfflineOnly` (false, adds `--no-index`), `OverrideIndexURL`, `ExtraInstallArgs`.
   - Plugins can also vendor packages under `Content/Python/Lib[/<Platform>[/<Arch>]]/site-packages`.
   - Nothing stops a plugin from shipping native executables (ruff, ty, or node plus basedpyright) and launching them with `FPlatformProcess::CreateProc` using separate stdout and stderr pipes. `FInteractiveProcess` is the wrong tool for LSP traffic (see Q5).
6. **Settings.** The full field list and defaults are in §6. Nothing new for 5.8 beyond what file 04 lists, apart from the cvars, command-line flags and environment variables listed there.
7. **Bundled Python.** Python 3.11.8, with pip 24.0 and setuptools 65.5.0 in site-packages, plus the stdlib `unittest`, `pdb`/`bdb`, `inspect`, `tkinter`, `idlelib`, `venv` and `ensurepip`.
   - **No jedi, parso, debugpy, pylint, black, ruff or pyright exists anywhere in the engine.**
   - `debugpy_unreal.install_debugpy()` pip-installs debugpy into the engine's own interpreter. That install sits under Program Files.

---

## Q1. Stub generation

**Answer.**

- **Writer.** `FPyWrapperTypeRegistry::GenerateStubCodeForWrappedTypes()` writes the stub to `FPaths::ProjectIntermediateDir()/PythonStub/unreal.py`. It writes through `FPyFileWriter::SaveFile` → `PyGenUtil::SaveGeneratedTextFile`, which only rewrites the file when the contents differ.
- **Trigger.** `FPythonScriptPlugin::GenerateStubCode()` calls the writer only when `IsDeveloperModeEnabled() && GIsEditor && !IsRunningCommandlet()`. `IsDeveloperModeEnabled()` is `UPythonScriptPluginSettings::bDeveloperMode || UPythonScriptPluginUserSettings::bDeveloperMode`. It reads the CDOs on every call.
- **Scheduling.** `RequestStubCodeGeneration()` debounces with a 2-second ticker. It is called:
  - at the end of Python initialization;
  - on every `EModuleChangeReason::ModuleLoaded` and `ModuleUnloaded`.
- **Cost.** Generation runs on the game thread under a modal `FScopedSlowTask` ("can be very slow"). It suspends the heartbeat and hitch detectors.
- **Docstrings and hints:** both present.
  - Each class gets `tp_doc`. Each method gets its `MethodDoc`. Each property getter gets its docstring. Constants get `#:` comments or docstrings. Docstrings are written as `r"""..."""`.
  - When type hinting is on, the file starts with `from __future__ import annotations` and a `typing` import block with TypeVars.
  - Method signatures come from reflection (`PythonizeMethodParam` / `PythonizeMethodReturnType`), not from the docstring.
  - Static UFunctions are emitted as `@classmethod`.
  - **`AutoCompletion`** mode: no `Union` coercions, no `Optional`.
  - **`TypeChecker`** mode: adds `WithTypeCoercion | WithOptionalType`.
  - **`Off`** mode: emits `return <default value>` bodies instead of hints.
- **Module and plugin knowledge: yes.**
  - `GeneratedWrappedTypesForModule` is a `TStripedMultiMap<FName module, FSoftObjectPath>`.
  - When a class is generated, it is also added to a real Python module `_unreal_<module>` via `PyImport_AddModule`.
  - Every type docstring ends with `**C++ Source:**` / `- **Plugin**: X` / `- **Module**: Y` / `- **File**: Z.h`, or `**Asset Source:** Package` for Blueprint types. The module comes from the package name after `/Script/`; the plugin from `IPluginManager` module descriptors.
  - The stub generator itself can filter types into Project, Engine and Restricted using `FSourceCodeNavigation::FindModulePath`.
  - So a post-processor can split Epic's `unreal.py` per module or plugin by parsing each class docstring. A runtime walker can split by `sys.modules['_unreal_*']`.
- **Order.** Types are sorted by inheritance depth, then by path (parents first). Native glue types come first. "Additional Code" is appended last: the contents of every on-disk `unreal_<module>.py`, such as `unreal_core.py`, with non-`unreal` imports hoisted to the top.
- **On-demand regeneration: none exposed.**
  - The plugin registers no console command for stubs (its only cvars are listed in §6).
  - No `unreal.*` function exists for it. The module method tables in `PyCore.cpp`, `PyEditor.cpp`, `PyEngine.cpp` and `PySlate.cpp` have none, and neither does `UPythonScriptLibrary`.
  - `IPythonScriptPlugin` has nothing stub-related.
  - `GenerateStubCode` is a private member, and `FPyWrapperTypeRegistry` is a Private, unexported class.
- **Headless alternative.** `UnrealEditor-Cmd <proj> -run=PythonOnlineDocs [-IncludeEngine|-IncludeProject|-IncludeInternal] [-NoHTML]`. It calls the same generator with filter flags (none means `IncludeAll`), forces `AutoCompletion` hints and ignores Developer Mode. After saving the stub it:
  - copies it into `Engine/Plugins/Experimental/PythonScriptPlugin/SphinxDocs/modules`, which is read-only under Program Files;
  - validates it by running it in a subprocess;
  - runs Sphinx unless `-NoHTML` is given.
- **Hacky in-editor triggers (untested).**
  - Set the user-settings CDO's `bDeveloperMode` by reflection. The flag is read live, although it is marked `ConfigRestartRequired`.
  - Then cause any module load or unload, for example load a tiny empty module that our plugin ships.

**Evidence.**

- `PSP/Private/PyWrapperTypeRegistry.cpp:2969-2981`: entry point; heartbeat and hitch suspension; modal "Generating unreal.py..." dialog.
- `PSP/Private/PyWrapperTypeRegistry.cpp:2986-2990`, `:3101-3111`: `from __future__ import annotations` and the typing header.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3013-3095`, `:3297-3305`: additional code from `unreal_<module>.py`.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3147-3234`: filter by module path (Project, Engine, Restricted).
- `PSP/Private/PyWrapperTypeRegistry.cpp:3245-3259`: sort by inheritance depth.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3307-3309`: output path `ProjectIntermediateDir()/PythonStub/unreal.py`.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3361-3371`: hint flags per mode.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3399`: class docstring.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3487-3510`: properties with docstrings.
- `PSP/Private/PyWrapperTypeRegistry.cpp:3512-3583`: methods with docstrings; `@classmethod` at :3570.
- `PSP/Private/PyFileWriter.cpp:87-90`: save goes through `SaveGeneratedTextFile`, which writes only on change (`PSP/Private/PyGenUtil.cpp` `SaveGeneratedTextFile`). `WriteDocString` emits `r"""` (`PyFileWriter.cpp:68,73`).
- `PSP/Private/PythonScriptPlugin.cpp:1567-1601`: debounce, `static const float Delay = 2.0f` at :1575-1576.
- `PSP/Private/PythonScriptPlugin.cpp:1603-1612`:
  ```cpp
  if (IsDeveloperModeEnabled() && GIsEditor && !IsRunningCommandlet())
  ```
- `PSP/Private/PythonScriptPlugin.cpp:1729-1730`: request at the end of initialization.
- `PSP/Private/PythonScriptPlugin.cpp:2008-2021`: request on module load and unload.
- `PSP/Private/PythonScriptPlugin.cpp:2098-2101`: `IsDeveloperModeEnabled` reads both CDOs.
- `PSP/Private/PyWrapperTypeRegistry.h:544-545`: `GeneratedWrappedTypesForModule`.
- `PSP/Private/PyWrapperTypeRegistry.cpp:2044-2058`: `PyImport_AddModule("_unreal_<module>")` plus `PyModule_AddObject` per class.
- `PSP/Private/PyGenUtil.cpp:2727-2764`: `GetFieldModule` and `GetFieldPlugin`.
- `PSP/Private/PyGenUtil.cpp:2766-2789`: Python module name mapping, e.g. CoreUObject→core and UnrealEd→editor.
- `PSP/Private/PyGenUtil.cpp:3513-3550`: the `**C++ Source:**` block (at :3539).
- `PSP/Private/PythonOnlineDocsCommandlet.cpp:31-57`: filter flags, IncludeAll fallback, forced `AutoCompletion`, generator call.
- `PSP/Private/PyOnlineDocsWriter.cpp:67-70`: SphinxDocs path inside the engine plugins folder.
- `PSP/Private/PyOnlineDocsWriter.cpp:283-301`, `:316-334`: copy, validation subprocess, `-NoHTML`.
- `PSP/Public/IPythonScriptPlugin.h:25-123`: no stub API.
- `PSP/Private/PythonScriptLibrary.h:20-71`: only execute and availability functions.

**Confidence.** High. The hacky trigger is untested: medium-low.

**Implication for our design.**

- Ship two stub paths:
  - **(A) Runtime walker (default).** Python inside UE iterates `sys.modules['_unreal_*']`, reads `__doc__`, `__mro__` and getset descriptors, and emits a split per-module `.pyi` package. It needs no Developer Mode and no engine-internal headers.
  - **(B) Post-processor (higher-fidelity hints).** If `Intermediate/PythonStub/unreal.py` exists, split it by each class's `**C++ Source:**` Module or Plugin line, adding cross-module base-class imports.
- Run the walker time-sliced, or in a `-run=PythonScript` child process, to avoid a long game-thread stall.
- Never force Developer Mode on artists. With it on, every plugin or module load in-editor brings up a modal regeneration dialog.

## Q2. Runtime docstrings, `inspect.signature`, editor-property lists

**Answer.**

- **Types.** `tp_doc` = pythonized class tooltip + C++ or asset source block + `**Editor Properties:** (see get_editor_property/set_editor_property)`. The list includes superclass property docs, one line per property: `` - ``name`` (Type): [Read-Write] tooltip ``.
  - `__doc__` is refreshed on re-finalization, because `PyType_Modified` doesn't update it.
- **Methods.** `MethodDoc` = `BuildFunctionDocString(...)` + newline + `PythonizeFunctionTooltip(...)`.
  - The first line is `x.func_name(param, param2=default) -> ReturnType`. It uses `X.` when static. Output tuples read `(a=T, b=U)`; a bool return plus outs reads `... or None`.
  - Parameter types do **not** appear on line 1. They are in a Google-style `Args:` block (`    name (Type): comment`) and a `Returns:` block.
  - Deprecated aliases get `deprecated: '<old>' was renamed to '<new>'.`.
  - Native glue methods (`cast`, `get_editor_property`, `static_class`...) use a different hand-written format: `name(self, x: T) -> R -- description`.
- **Properties.** The getset doc is `(Type): [Read-Write|Read-Only] tooltip`, Google style.
- **`inspect.signature`: does not work on wrapped functions.**
  - The bound-method object type `builtin_function_or_method_with_closure` sets `tp_base = &PyCFunction_Type` as "a hack so that the doc gen code will think this is a built-in function". Its own getsets are only `__doc__`, `__name__` and `__self__`.
  - The `__text_signature__` it inherits from `PyCFunction_Type` parses `ml_doc` with CPython's internal `name(...)\n--\n\n` convention. The struct layouts are compatible, so this does not crash. UE docs start with `x.`/`X.` and have no `--` marker, so the attribute is `None`.
  - `inspect.signature` therefore raises `ValueError: no signature found for builtin`. The unbound descriptor type has no `__text_signature__` at all, so it fails the same way.
  - Native glue `PyMethodDef` docs also lack the marker.
  - `help()`, `inspect.getdoc` and `__doc__` all work.
- **Editor-property lists at runtime: yes, per class, in `__doc__`.**
  - These are properties with `CPF_Edit` (or deprecated) that are not `ScriptNoExport`, listed only when `GIsEditor`.
  - Blueprint-visible properties are also real getset attributes, visible in `dir()`.
  - Edit-only properties are reachable only through `get_editor_property()` and `set_editor_property()`. Neither the Python nor the reflection side has a separate "list properties" API.
- **`__module__`.** `tp_name` is the bare Python name (e.g. `"StaticMesh"`) and no `__module__` is set in the type dict. For these static types CPython therefore reports `__module__ == 'builtins'`. Inferred from source; verify live.

**Evidence.**

- `PSP/Private/PyWrapperTypeRegistry.cpp:2009-2021`: class docstring assembly, including inherited `PropertyDocs` and the source info.
- `PSP/Private/PyGenUtil.cpp:624-680`: the `**Editor Properties:**` list format (header at :640).
- `PSP/Private/PyGenUtil.cpp:602-609`: property doc, runtime and editor read-only flavours.
- `PSP/Private/PyGenUtil.cpp:1994-2009`:
  ```cpp
  // Use Google style docstrings
  PythonizedTooltip += TEXT('('); AppendPropertyPythonType(...); PythonizedTooltip += TEXT("): ");
  ```
- `PSP/Private/PyGenUtil.cpp:2098-2120`: `Args:` block (header at :2102).
- `PSP/Private/PyGenUtil.cpp:2122-2165`: `Returns:` block (header at :2127).
- `PSP/Private/PyGenUtil.cpp:3478`: `[Read-Only]`/`[Read-Write]`.
- `PSP/Private/PyGenUtil.cpp:1468-1535`:
  ```cpp
  FString::Printf(TEXT("%s.%s("), (bIsStatic ? TEXT("X") : TEXT("x")), *InFuncPythonName)
  ```
- `PSP/Private/PyWrapperTypeRegistry.cpp:1928-1932`: `MethodDoc` = declaration line + tooltip.
- `PSP/Private/PyWrapperTypeRegistry.cpp:1959`: deprecated alias doc.
- `PSP/Private/PyWrapperObject.cpp:1116-1135`: native glue doc format, `"... -> bool -- inform that ..."`.
- `PSP/Private/PyMethodWithClosure.cpp:443-475`: getsets `__doc__`, `__name__`, `__self__` only.
- `PSP/Private/PyMethodWithClosure.cpp:483`:
  ```cpp
  PyType.tp_base = &PyCFunction_Type; // This is a hack so that the doc gen code will think this is a built-in function
  ```
- `PSP/Private/PyMethodWithClosure.cpp:520-533`: descriptor type, `__doc__` only.
- `PSP/Private/PyMethodWithClosure.h:34-49`, `:65-77`: layouts that match `PyMethodDef`/`PyCFunctionObject`.
- `PSP/Private/PyGenUtil.cpp:748-752`: `__doc__` re-set on reset.
- `PSP/Private/PyGenUtil.cpp` `ShouldExportEditorOnlyProperty`: `GIsEditor && (CPF_Edit || deprecated)`.
- `PSP/Private/PyWrapperTypeRegistry.cpp:1408-1445`: getset only when `ShouldExportProperty`; doc entry for both.
- `PSP/Private/PyWrapperTypeRegistry.cpp:1978`: `TypeName` = bare Python class name.

**Confidence.**

- Docstring formats and the editor-property list: high.
- `inspect.signature` failure: high for bound and class methods (it follows from CPython 3.11 `inspect._signature_from_builtin` and `find_signature`), medium-high for unbound descriptors. Confirm with one live call.
- `__module__ == 'builtins'`: medium.

**Implication for our design.**

- Live signature help and hover should parse the docstring's first line plus the `Args:`/`Returns:` blocks. Never call `inspect.signature` on `unreal` objects; wrap any call in `try`.
- Treat the `X.`/`x.` prefix as static or instance.
- The runtime walker (Q1-A) can recover:
  - per-parameter types (from `Args:`) and defaults (from line 1);
  - property types (from getset docs, or the class "Editor Properties" list);
  - module and plugin (from the `C++ Source` block or `_unreal_*` membership).
- Do not group types by `__module__`.

## Q3. Is `PyGenUtilAccessor` usable by a third-party plugin? What would our own generator need?

**Answer.**

- **What the accessor is.** `PSP/Internal/PyGenUtilAccessor.h` is the only generation helper outside `Private/`, and it is explicitly "Not available for non-Engine use". It exports, with `PYTHONSCRIPTPLUGIN_API`:
  - `ExtractFunctionParamsAsProperties`;
  - `GetClassPythonName`, `GetFunctionPythonName`, `GetPropertyPythonName`, `GetPropertyPythonType`;
  - `ShouldExportClass`, `ShouldExportFunction`, `ShouldExportProperty`, `ShouldExportEditorOnlyProperty`.
- **Why a plugin can't include it by default.**
  - UBT collects `Internal/` into `InternalIncludePaths`.
  - Those paths go to a dependent module only when `Rules.Context.Scope == SourceModule.Rules.Context.Scope || SourceModule.Rules.bTreatAsEngineModule`.
  - Engine plugins live in the "Engine Plugins" scope; Marketplace (Fab) and project plugins get their own scopes. So a Fab or project plugin cannot `#include` it by default.
- **Workarounds.** Both are against Epic's intent and their stability is unknown:
  - set `bTreatAsEngineModule = true` in our Build.cs;
  - re-declare the prototypes and link against the exported symbols.
- **Gaps even if included.** No struct, enum, enum-entry or delegate names. No `ScriptMethod`, `ScriptConstant` or `ScriptOperator` hoisting, no deprecated aliases, no default-value pythonizing, no tooltip pythonizing. All of these live in Private `PyGenUtil` (≈3.6k lines) and `PyWrapperTypeRegistry` (≈4.2k lines).
- **What a from-reflection C++ generator would need:**
  - `TObjectIterator<UClass/UScriptStruct/UEnum>` and delegate `UFunction`s;
  - `TFieldIterator<FProperty/UFunction>` with `ExcludeSuper`;
  - re-implementing the export filters: `BlueprintType`/`NotBlueprintType`/`BlueprintSpawnableComponent` metadata, `HasScriptExposedFields`, `ScriptNoExport`;
  - re-implementing the naming rules: `ScriptName` metadata, `PythonizeName` and `PythonizePropertyName` (snake_case plus prefix stripping), deprecated names;
  - script-method and script-constant hoisting;
  - default-value conversion;
  - the module-name map (CoreUObject→core, SlateCore→slate, UnrealEd→editor, PythonScriptPlugin→python);
  - module to plugin through `IPluginManager` module descriptors.
- **Cheaper, Epic-exact route.** Walk the generated wrapped types from Python at runtime, as in Q1-A and Q2. The names, hoisting, deprecation aliases and docs are then exactly what Epic generated.

**Evidence.**

- `PSP/Internal/PyGenUtilAccessor.h:18`: "Not available for non-Engine use."
- `PSP/Internal/PyGenUtilAccessor.h:22-35`: the exported functions.
- `PSP/Private/PyGenUtilAccessor.cpp:13-78`: thin forwards to `PyGenUtil`.
- `Engine/Source/Programs/UnrealBuildTool/Configuration/UEBuildModuleCPP.cs:464-468`: `Internal` → `InternalIncludePaths`.
- `Engine/Source/Programs/UnrealBuildTool/Configuration/UEBuildModule.cs:736-740`: the scope condition.
- `Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/RulesCompiler.cs:123-125`, `:244`: Engine, Engine Plugins and Marketplace scopes.
- `Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/ModuleRules.cs:849-853`: `bTreatAsEngineModule`.
- `PSP/Private/PyGenUtil.h:40-44`: script metadata keys.
- `PSP/Private/PyGenUtil.h:1118-1241`: the full Private naming and filter API.
- `PSP/Private/PyGenUtil.cpp` `IsScriptExposedClass` / `ShouldExportClass`: the export rules.

**Confidence.** High.

**Implication for our design.**

- Do not build on `PyGenUtilAccessor`.
- Default to the Python runtime walker, with Epic-stub post-processing as an upgrade.
- If C++ reflection is ever needed, for example in a commandlet with no Python, accept that we would be re-implementing Epic's rules and must test against Epic's stub on every engine release.

## Q4. Python test infrastructure in 5.8

**Answer.**

- **`PyTest.cpp`/`PyTest.h`** (in PythonScriptPlugin) are not a test runner. They are reflected fixture types (`UPyTestObject`, `FPyTestStruct`, `UPyTestTypeHint`...) used by the plugin's own scripts `Content/Python/test_wrapper_types.py`, `test_type_hints.py` and `test_source_control.py`.
  - They are not compile-gated, so they always appear in `unreal` and the stub (category `Python|Internal`).
  - `UPyTestTypeHint` exists to eyeball stub type hints in an IDE.
- **PythonAutomationTest plugin** (`Engine/Plugins/Tests/PythonAutomationTest`; `IsBetaVersion`, `EnabledByDefault: false`; depends on PythonScriptPlugin and FunctionalTestingEditor; compiled DLL ships):
  - `IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FPythonAutomationTest, ..., "Editor.Python", EditorContext | EngineFilter)`.
  - `GetTests` recursively finds `test_*.py` under `<Project>/Content/Python` and every **enabled** plugin's `Content/Python`. It skips `/site-packages/`.
  - Names are `Editor.Python.<ProjectOrPlugin>.<dotted path>`.
  - `RunTest` runs the file through `ExecPythonCommandEx` (ExecuteFile). It then waits on a latent command until Python clears `unreal.PyAutomationTest.set_is_running_py_latent_command`, with a 300 s default timeout.
  - The Python helpers are `automation_test.unittest_utilities.TestRunner` (a custom runner, **not** `unittest`) and `unreal_pythonautomationtest.AutomationScheduler`.
  - One test per file means no per-case granularity.
- **`unreal.PythonTestRunner`** (ToolsetRegistry plugin, `Engine/Plugins/Experimental/ToolsetRegistry`; `IsExperimentalVersion: true`, `NoRedist: true`, `EnabledByDefault: false`; depends on PythonScriptPlugin, EditorScriptingUtilities and FileSandbox; DLL ships):
  - Registration is called from Python: `unreal.PythonTestRunner.create(base_name, unreal.PythonTestRunnerSearchOptions(root_module=..., test_module_glob=..., test_name_globs=[...]), timeout_in_seconds=5.0)`.
  - It creates a C++ `UE::ToolsetRegistry::FPythonAutomationTest` and calls `FAutomationTestFramework::RegisterAutomationTest`. Each discovered `unittest` case becomes an automation test (flags `EditorContext | ProductFilter | CriticalPriority`).
  - Its Python side is `toolset_registry.tests.unittest_runner`, which runs suites on an editor-thread asyncio loop.
  - Class- and module-level setup and teardown run around every case.
  - The runner object must be kept alive in a global.
  - ToolsetRegistry's own `init_unreal.py` is the reference usage.
- **`-run=PythonScript`** (`UPythonScriptCommandlet`). It parses `-Script=` by hand, quoted or token. Then it:
  - calls `ForceEnablePythonAtRuntime()`;
  - runs the startup scripts;
  - opens `FScopedSourceControl`;
  - executes with the `Unattended` flag in the default `ExecuteFile` mode.
  - The value can be a path to a `.py` file (with arguments, quotes allowed, or found on `sys.path`) or literal code.
  - It returns 0 on success and -1 on error.
  - The Developer Mode stub is **not** generated in commandlets.
- **Related commands.**
  - `-ExecutePythonScript=` for the full editor.
  - `Automation RunTests <filter>` is a real automation console command, usable via `-ExecCmds="Automation RunTests Editor.Python; Quit"`.

**Evidence.**

- `PSP/Private/PyTest.h:29-383`: fixture types; the comment at :377-380 explains `UPyTestTypeHint`.
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/test_wrapper_types.py:1-30`.
- `Engine/Plugins/Tests/PythonAutomationTest/PythonAutomationTest.uplugin` (Beta, off by default, dependencies).
- `Engine/Plugins/Tests/PythonAutomationTest/Source/PythonAutomationTest/Private/PythonAutomationTest.cpp`:
  - :19 (300 s timeout);
  - :54-72 (latent wait);
  - :117-134 (the `test_*.py` search, skipping site-packages);
  - :140-144 (`"Editor.Python"`, `EditorContext | EngineFilter`);
  - :146-168 (project plus every enabled plugin);
  - :170-193 (ExecuteFile run).
- `.../PythonAutomationTest/Public/PythonAutomationTest.h:11-35`: `UPyAutomationTestLibrary` (ScriptName `PyAutomationTest`).
- `.../PythonAutomationTest/Content/Python/automation_test/unittest_utilities.py:17-113`: the custom `TestRunner`.
- `Engine/Plugins/Experimental/ToolsetRegistry/ToolsetRegistry.uplugin:16-19,28-40`.
- `.../ToolsetRegistry/Source/ToolsetRegistry/Private/ToolsetRegistry/PythonTestRunner.h:19-41` (search options), `:53-127` (`FPythonAutomationTest`), `:146-232` (`UPythonTestRunner::Create` with usage example; `TimeoutInSeconds = 5.0f`).
- `.../ToolsetRegistry/Private/ToolsetRegistry/PythonTestRunner.cpp:125-130` (flags), `:403` (Register), `:429` (Unregister).
- `.../ToolsetRegistry/Content/Python/init_unreal.py:7-12`.
- `PSP/Private/PythonScriptCommandlet.cpp:15-78`.
- `PSP/Private/PythonScriptPlugin.cpp:814-838`: ExecuteFile detects a `.py` path versus literal code.
- `PSP/Private/EditorUtilities/EditorPythonExecuter.cpp:147`: `-ExecutePythonScript=`.
- `Engine/Source/Developer/AutomationController/Private/AutomationCommandline.cpp:610`: `RunTests`.

**Confidence.** High.

**Implication for our design.**

- Our CI:
  - (1) `-run=PythonScript -Script=<our_unittest_main.py>` with the exit code, so there is no Experimental dependency;
  - (2) optionally register with `unreal.PythonTestRunner` when ToolsetRegistry is enabled, behind a feature check;
  - (3) C++ Automation Specs.
- Keep our own test files out of shipped `Content/Python`, or don't name them `test_*.py`. Otherwise any user who runs `Editor.Python` with PythonAutomationTest enabled executes them. The same applies to Epic's `test_source_control.py`, which issues source-control calls.
- A "Run tests" panel in our editor can offer both discovery models: per file, and `unittest` per case.

## Q5. PipInstall in 5.8 and shipping native tool binaries

**Answer.**

- **Format.** A top-level array in `.uplugin`:
  ```json
  "PythonRequirements": [{ "Platform": "All|Win64|Mac|Linux", "Architecture": "All|<arch>",
                           "Requirements": ["pkg==1.2 --hash=sha256:..."], "ExtraIndexUrls": ["..."] }]
  ```
  - Platform and Architecture are optional; when missing they mean All.
  - Engine example: PythonFoundationPackages (numpy 1.26.4, scipy 1.11.4, ... all hash-pinned).
- **Where packages go.**
  - Default: `<Project>/Intermediate/PipInstall`, a **venv** built from the engine interpreter with `-m venv`. Packages land in `.../Lib/site-packages` on Windows.
  - Environment override: `UE_PIPINSTALL_PATH`.
  - Work files: `pyreqs_plugins.list`, `merged_requirements.in`, `merged_requirements.txt` (parsed by `ue_parse_plugin_reqs`), `extra_urls.txt`, and a `.pth` listing plugins' vendored site-packages.
  - The venv is deleted and recreated if `pyvenv.cfg` names a different Python version.
  - The site-packages path is always registered, so pre-populated or deferred installs work.
- **Command.**
  - `-m pip install --disable-pip-version-check --only-binary=:all:`
  - plus `--require-hashes` when `bPipStrictHashCheck`; when hash checking is off, hashes are stripped from all requirements ("all-or-nothing");
  - plus `--no-index` when `bOfflineOnly`, else `--index-url <OverrideIndexURL>`, else one `--extra-index-url` per entry in `ExtraIndexUrls`;
  - plus `ExtraInstallArgs`, then `-r merged_requirements.txt`.
- **When it runs.**
  - `InitPipInstall` runs on a task and is waited on before init finishes.
  - If `bRunPipInstallOnStartup` (default true) or `-ForcePipInstallOnInit` is set and packages are missing, `PipInstallLauncher::StartSync` runs **on the game thread during startup**.
  - It is skipped in restrictive mode.
- **Other entry points.**
  - Public C++: `IPipInstall` (Init, Launch async or sync with a progress notifier, IsInstalling, GetNumPackagesToInstall, GetPackageInstallList, RegisterPipSitePackagesPath), `FPipInstallHelper::ShowPipInstallDialog` / `LaunchHeadlessPipInstall`, and `FPythonScriptInitHelper::InitPythonAndPipInstall`.
  - UBT: `-Mode=PipInstall` with `-PipAction=GenRequirements|Setup|Parse|Install|ViewLicenses`, `-IgnoreHashes`, `-OverrideIndexUrl`, `-AllPlugins`, installing to `<Project>/Intermediate/PipInstall`. This suits CI and offline pre-population.
- **Vendoring instead of pip.** Any plugin's `Content/Python/Lib/<Platform>/<Arch>/site-packages`, `Lib/<Platform>/site-packages` and `Lib/site-packages` are added to `sys.path`. USDCore (`pxr`), HairCardGenerator and CameraCalibrationML do this.
  - The orphan cleanup only removes directories that contain nothing but empty or `__pycache__` folders, so real vendored files are safe.
- **Shipping native binaries.** No engine rule prevents a plugin from carrying `Binaries/ThirdParty/<tool>/Win64/*.exe` (declared through `RuntimeDependencies` in Build.cs) and launching it.
  - Use `FPlatformProcess::CreateProc(URL, Params, ..., PipeWriteChild, PipeReadChild, PipeStdErrChild)` with **separate stdout and stderr pipes**, then `FPlatformProcess::ReadPipeToArray` (binary) for LSP Content-Length framing.
  - Do **not** use `FInteractiveProcess` for LSP. It converts output to `FString`, splits on `\n`, drops empty lines and logs every line to `LogInteractiveProcess`. Its constructor also passes one write pipe for the child, which merges stderr into stdout.
  - PipInstall itself launches `python.exe` this way (`PipRunnable.cpp`).
  - A pip route for language servers is possible too, since pip-installed console scripts land in the venv's `Scripts\`. But that ties tool download to editor startup and needs every transitive wheel hash-pinned per platform.

**Evidence.**

- `PSP/Private/PipInstall.cpp:182-196`: path and `UE_PIPINSTALL_PATH`.
- `PSP/Private/PipInstall.cpp:209-215`: file names.
- `PSP/Private/PipInstall.cpp:227-254`: plugin listing via `CachedJson->HasTypedField("PythonRequirements")`.
- `PSP/Private/PipInstall.cpp:256-306`: Requirements and ExtraIndexUrls.
- `PSP/Private/PipInstall.cpp:884-893`: Platform and Architecture checks.
- `PSP/Private/PipInstall.cpp:396-421`: venv version check and rebuild.
- `PSP/Private/PipInstall.cpp:453`: `-m venv`.
- `PSP/Private/PipInstall.cpp:476-492`: `--disable-hashes` for the parser.
- `PSP/Private/PipInstall.cpp:545-555`: site-packages path.
- `PSP/Private/PipInstall.cpp:739-775`:
  ```cpp
  FString Cmd = TEXT("-m pip install --disable-pip-version-check --only-binary=:all:");
  ```
- `PSP/Private/PipInstall.cpp:308-395`: orphan cleanup.
- `PSP/Private/PipInstall.cpp:628-660`: `.pth` for vendored plugin site-packages.
- `PSP/Private/PythonScriptPlugin.cpp:1353-1371`: pip on startup, StartSync "in game thread", restrictive-mode skip.
- `PSP/Public/IPipInstall.h:23-44`.
- `PSP/Public/PipInstallHelpers.h:22-43`.
- `PSP/Private/PythonScriptPluginSettings.h:93-124`: pip settings and their comments.
- `PSP/Private/PyUtil.cpp:1187-1195`: vendored site-packages subdirectories.
- `Engine/Source/Programs/UnrealBuildTool/Modes/PipInstallMode.cs:49-73,127`.
- `Engine/Plugins/Experimental/PythonFoundationPackages/PythonFoundationPackages.uplugin:26-30`.
- Vendored examples: `Engine/Plugins/Runtime/USDCore/Content/Python/Lib/Win64/X64/site-packages/pxr`.
- `Engine/Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp:108,133-146`.
- `Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformProcess.h:604,622,848`: CreateProc with `PipeStdErrChild`, `ReadPipeToArray`.
- `PSP/Private/PipRunnable.cpp:91-125`.
- `Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/ModuleRules.cs:1424`: `RuntimeDependencies`.

**Confidence.**

- PipInstall behaviour: high.
- Launching native tools: high for the API surface. Fab policy on bundled `.exe` and `node.exe` is UNVERIFIED.

**Implication for our design.**

- Ship the language-server toolchain as plugin `ThirdParty` binaries launched out of process, as file 03 recommends. Never put it in `PythonRequirements`, which would add a game-thread install to every user's startup.
- Use `PythonRequirements` only for small pure-Python helpers, or don't use it at all; vendoring under `Content/Python/Lib/site-packages` avoids startup installs entirely.
- Write our own pipe pump over `CreateProc` with three pipes.

## Q6. `UPythonScriptPluginSettings` and `UPythonScriptPluginUserSettings` (5.8), with defaults

Project settings: `UCLASS(config=Engine, defaultconfig)`. It is stored in `Config/DefaultEngine.ini` under `[/Script/PythonScriptPlugin.PythonScriptPluginSettings]` and appears in Project Settings > Plugins > Python (`PSP/Private/PythonScriptPluginSettings.h:48-49`, `.cpp:12-13`).

| Field | Default | Category, metadata | Line |
|---|---|---|---|
| `StartupScripts` `TArray<FString>` | empty | Python; ConfigRestartRequired, MultiLine | h:66-67 |
| `AdditionalPaths` `TArray<FDirectoryPath>` | empty | Python; ConfigRestartRequired, RelativePath | h:70-71 |
| `bIsolateInterpreterEnvironment` | **true** | Python; ConfigRestartRequired. Ignores PYTHON* environment variables, the script dir and user site-packages; `UE_PYTHONPATH` still honoured | h:73-81 |
| `bDeveloperMode` ("Developer Mode (all users)") | false | Python; ConfigRestartRequired, AdvancedDisplay | h:83-91 |
| `bRunPipInstallOnStartup` | **true** | PythonPipInstall; ConfigRestartRequired | h:93-102 |
| `bPipStrictHashCheck` | **true** | PythonPipInstall; AdvancedDisplay | h:104-108 |
| `bOfflineOnly` | false | PythonPipInstall; AdvancedDisplay | h:110-112 |
| `OverrideIndexURL` | "" | PythonPipInstall; AdvancedDisplay | h:114-118 |
| `ExtraInstallArgs` | "" | PythonPipInstall; AdvancedDisplay | h:120-124 |
| `bRemoteExecution` ("Enable Remote Execution?") | false | PythonRemoteExecution; applied live through `SyncRemoteExecutionToSettings` | h:126-128, cpp:44-63 |
| `RemoteExecutionMulticastGroupEndpoint` | `239.0.0.1:6766` | AdvancedDisplay; editable only when remote execution is on | h:131-132, cpp:15, cpp:24-42 |
| `RemoteExecutionMulticastBindAddress` | `127.0.0.1` | AdvancedDisplay | h:135-136, cpp:16 |
| `RemoteExecutionSendBufferSizeBytes` | 2 MiB | AdvancedDisplay | h:139-140, cpp:17 |
| `RemoteExecutionReceiveBufferSizeBytes` | 2 MiB | AdvancedDisplay | h:143-144, cpp:18 |
| `RemoteExecutionMulticastTtl` | 0 (local host only) | AdvancedDisplay | h:147-148, cpp:19 |

User settings: `UCLASS(config=EditorPerProjectUserSettings)`. It is stored in `Saved/Config/<Platform>/EditorPerProjectUserSettings.ini` under `[/Script/PythonScriptPlugin.PythonScriptPluginUserSettings]` and appears in Editor Preferences > Plugins > Python (`h:152-153`).

| Field | Default | Notes | Line |
|---|---|---|---|
| `EnablePythonOverride` (`EPythonEnabledOverrideState`) | None | None / Enable / Disable; ConfigRestartRequired | h:168-169, enum h:32-43 |
| `bDeveloperMode` | false | ConfigRestartRequired; "enable stub code generation for use with external IDEs" | h:171-178 |
| `TypeHintingMode` (`ETypeHintingMode`) | **AutoCompletion** | Off / AutoCompletion / TypeChecker; ConfigRestartRequired | h:180-187, enum h:10-30 |
| `bEnableContentBrowserIntegration` | false | ConfigRestartRequired | h:189-191 |

Defaults without an initializer are zero-initialized UObject members. No overrides exist in `Engine/Config/*.ini` (grep found none).

Other knobs found in 5.8 source:

- **Cvars:**
  - `Engine.Python.IsPythonLifeCycleDynamic` (false, read-only);
  - `Engine.Python.IsPythonInRestrictiveMode` (false);
  - `Engine.Python.IsEnabledByDefault` (true);
  - `Engine.Python.ForceWrapperGenerationOnSingleThread`;
  - `Engine.Python.WrapperGenerationRaceDetector`.
  - Sources: `PSP/Private/PythonScriptPlugin.cpp:98-106`, `PSP/Private/PyWrapperTypeRegistry.cpp:58-72`.
- **Command line:** `-ForceEnablePython`, `-DisablePython`, `-EnablePython` (`PythonScriptPlugin.cpp:110-156`), `-ForcePipInstallOnInit` (:1364), `-ExecutePythonScript=` (`EditorPythonExecuter.cpp:147`).
- **Editor ini:** `[PythonScriptPlugin] DisablePythonForCommandlet=` array (`PythonScriptPlugin.cpp:122-149`).
- **Environment:** `UE_PYTHONPATH` (`PythonScriptPlugin.cpp:1281-1287`), `UE_PIPINSTALL_PATH` (`PipInstall.cpp:185-196`).
- **Restrictive-mode allow-list API:** `IPythonScriptPlugin::AddStartupScriptAllowPath` / `RemoveStartupScriptAllowPath` (`PSP/Public/IPythonScriptPlugin.h:112-123`).
- **Type hint mode at startup:** read once from the user settings (`PythonScriptPlugin.cpp:1228-1229`, `:2103-2106`).

**Confidence.** High.

**Implication.**

- Our settings panel can show these settings read-only and link to the Project Settings and Editor Preferences pages.
- The settings classes are not `BlueprintType`; `UDeveloperSettings` is `UCLASS(Abstract, MinimalAPI)` (`Engine/Source/Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:22`). So they are probably not reachable from Python, and changing them needs C++ reflection or ini edits.

## Q7. Bundled Python and packages

**Answer.**

- **Interpreter.** `Engine/Binaries/ThirdParty/Python3/Win64` has `python.exe`, `pythonw.exe`, `python311.dll` and `python3.dll`. `NEWS.txt:5` reads "What's New in Python 3.11.8 final?".
- **`Lib/site-packages`** contains only `pip` 24.0, `setuptools` 65.5.0, `pkg_resources` and `_distutils_hack`.
- **Bundled installers.** `Lib/ensurepip/_bundled` has the pip 24.0 and setuptools 65.5.0 wheels. `Scripts/` has `pip.exe`, `pip3.exe` and `pip3.11.exe`.
- **Stdlib extras present:** `unittest`, `pdb`, `bdb`, `inspect`, `typing`, `tomllib`, `asyncio`, `sqlite3`, `venv`, `tkinter` (with `tcl86t.dll`/`tk86t.dll`), `idlelib`, `lib2to3`, `pydoc`, `test`.
- **Plugin helper wheels.** PythonScriptPlugin's own `Content/Python/Lib/wheels` holds `packaging` 23.1, `pip_requirements_parser` 32.0.1, `pyparsing` 3.1.1, `wheel` 0.38.4 and `ue_pipinstall_utils` 0.1.5. They exist for the pip installer.
- **Not bundled.** A full search of `Engine/` (excluding Intermediate) for `jedi`, `parso`, `debugpy`, `_pydevd*`, `pyflakes`, `pylint`, `black`, `ruff*`, `pyright*` and `typeshed` found **nothing**.
- **debugpy.** `debugpy_unreal.install_debugpy()` runs `ensurepip`, then `pip install debugpy`, against the engine interpreter. That writes into the Program Files install.
  - It probably fails without elevation, and pip would fall back to a `--user` install.
  - The default isolation mode ignores user site-packages, so a `--user` install would not be importable.

**Evidence.**

- Directory listings of `Engine/Binaries/ThirdParty/Python3/Win64/{.,Lib,Lib/site-packages,DLLs,Scripts,Lib/ensurepip/_bundled}`.
- `Engine/Binaries/ThirdParty/Python3/Win64/NEWS.txt:5`.
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/Lib/wheels/*`.
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/PipInstallUtils/requirements.txt`.
- `Engine/Plugins/Experimental/PythonScriptPlugin/Content/Python/debugpy_unreal.py:34-38`.
- `PSP/Private/PythonScriptPluginSettings.h:73-79`: isolation ignores the user's site-packages.

**Confidence.**

- Inventory: high.
- The `install_debugpy` failure mode: medium (inferred).

**Implication for our design.**

- Vendor everything we need: jedi/parso if used in-process, debugpy for the debugger bridge, and our pure-Python helpers. Put them under our plugin's `Content/Python/Lib/site-packages`, or under a private `sys.path` entry we add.
- Never pip-install into the engine interpreter.

---

## Corrections to research

1. **04 §5** says "we can regenerate on demand (toggle Developer Mode, or drive `unreal` stub generation)". This is **refuted as stated**:
   - No console, Python or public C++ API regenerates the stub. `FPythonScriptPlugin::GenerateStubCode` is private and `FPyWrapperTypeRegistry` is an unexported Private class.
   - The only triggers are Python init, any module load or unload (2 s debounce) and the `-run=PythonOnlineDocs` commandlet.
   - Toggling Developer Mode takes effect at the next trigger because it is read live, but the setting is marked ConfigRestartRequired.
   - Evidence: `PSP/Private/PythonScriptPlugin.cpp:1567-1612,1729-1730,2008-2021,2098-2101`; `PSP/Private/PythonOnlineDocsCommandlet.cpp:57`.
2. **04 §4** says "There is no stub generation in commandlets". True only for the Developer-Mode auto stub. `-run=PythonOnlineDocs` writes `Intermediate/PythonStub/unreal.py`, whether or not Developer Mode is on, and then tries to write Sphinx files into `Engine/Plugins/Experimental/PythonScriptPlugin/SphinxDocs`. Evidence: `PSP/Private/PythonOnlineDocsCommandlet.cpp:47-57`; `PSP/Private/PyWrapperTypeRegistry.cpp:3307-3315`; `PSP/Private/PyOnlineDocsWriter.cpp:67-70`.
3. **04 §3.2** says the extra package paths are "`Lib/site-packages` and `<Platform>/Lib/site-packages`". In fact the three subdirectories of each `Content/Python` are `Lib/<Platform>/<Arch>/site-packages`, `Lib/<Platform>/site-packages` and `Lib/site-packages`. Evidence: `PSP/Private/PyUtil.cpp:1187-1195`.
4. **05 [S72]** says "`unreal.PythonTestRunner` (5.8, experimental) registers unittest suites". This is confirmed, but the runner lives in the **Experimental ToolsetRegistry plugin** (`EnabledByDefault: false`, `NoRedist: true`, depends on EditorScriptingUtilities and FileSandbox), not in PythonScriptPlugin. It registers individual `unittest` cases, with a 5 s default timeout per test. Evidence: `.../ToolsetRegistry/ToolsetRegistry.uplugin:16-19,28-40`; `.../PythonTestRunner.h:146-232`.
5. **05 [S3]** says "PythonAutomationTest discovers `test_*.py` in `Content/Python`". This is confirmed, with nuances:
   - It scans the project **and every enabled plugin**, skipping `site-packages`.
   - It makes one automation test per file under `Editor.Python.*`, not per `unittest` case.
   - It uses a custom runner rather than `unittest`, with a 300 s latent timeout.
   - Evidence: `.../PythonAutomationTest.cpp:19,117-193`.
6. **03 and 05** say "`PyGenUtilAccessor` is not available for non-Engine use". This is confirmed (comment at `PSP/Internal/PyGenUtilAccessor.h:18`), with nuances:
   - The barrier is UBT's Internal-include scope rule (`UEBuildModule.cs:736-740`), not symbol export; the functions are `PYTHONSCRIPTPLUGIN_API`.
   - The accessor covers only class, function and property names and types plus the export filters, so it would not be enough for a stub generator anyway.
7. **03 B6** says "the stub is regenerated on every editor start". This is confirmed, and the stub is also regenerated after **every module load or unload**. The file is rewritten only when its content changes, and generation shows a modal slow-task dialog. Evidence: `PSP/Private/PythonScriptPlugin.cpp:2008-2021`; `PSP/Private/PyWrapperTypeRegistry.cpp:2979-2981`; `PSP/Private/PyFileWriter.cpp:87-90`.
8. **03 table row 2** suggests a "live fallback from `__doc__`/`inspect`". The `__doc__` part works. **`inspect.signature` does not** work on wrapped UFunctions or native glue methods, so the live fallback must parse docstrings. Evidence: `PSP/Private/PyMethodWithClosure.cpp:443-483`; `PSP/Private/PyGenUtil.cpp:1468-1535`.
9. **05 open question 7** asks "Can we generate stubs without forcing Developer Mode on artists?". **Yes.** A runtime walk over `sys.modules['_unreal_*']` and the rich docstrings recovers names, module and plugin, parameters, defaults and types without Developer Mode. Evidence: `PSP/Private/PyWrapperTypeRegistry.cpp:2044-2058,2009-2021`; `PSP/Private/PyGenUtil.cpp:2098-2165`.
10. **06 pain point 7** says "Offline installs mean copying site-packages". This is incomplete. 5.8 has:
    - `bOfflineOnly` (`--no-index`), with the site-packages path always registered for pre-populated installs;
    - the `UE_PIPINSTALL_PATH` override;
    - the UBT `-Mode=PipInstall` CLI;
    - vendored per-plugin `Lib/.../site-packages`.

    Evidence: `PSP/Private/PipInstall.cpp:185-196,751-754`; `PSP/Private/PythonScriptPluginSettings.h:93-112`; `Engine/Source/Programs/UnrealBuildTool/Modes/PipInstallMode.cs:49-73`.
11. **03 [S53]** says "Python 3.11.8; pip 24.0". This is confirmed, together with setuptools 65.5.0 and the absence of jedi and debugpy. Evidence: `NEWS.txt:5`; the `Lib/site-packages` listing.

## New risks

1. **Developer-Mode stub churn.** With Developer Mode on, every module load or unload brings up a modal "Generating unreal.py..." dialog on the game thread: enabling a plugin, Live Coding or hot reload, or lazily loaded editor modules. If our product turns Developer Mode on for stubs, artists see repeated stalls. (`PSP/Private/PythonScriptPlugin.cpp:2008-2021`; `PyWrapperTypeRegistry.cpp:2979-2981`)
2. **No supported on-demand stub API.** The workarounds are reflection-flipping a ConfigRestartRequired CDO flag plus forcing a module-load event, or a commandlet that writes into the read-only engine folder and then fails validation there. These may break between engine versions.
3. **`inspect.signature` raises on every `unreal` callable.** Any library we embed (jedi's interpreter mode, IPython-style help, pydoc) that relies on it degrades or throws, so wrap such calls. (`PSP/Private/PyMethodWithClosure.cpp:483`)
4. **Wrapped types report `__module__ == 'builtins'`** (medium confidence). Introspection-based completers can mis-group them or treat them as builtins, so group types by `_unreal_*` modules instead. (`PSP/Private/PyWrapperTypeRegistry.cpp:1978`)
5. **Engine test fixture types (`PyTest*`, category `Python|Internal`) always ship in `unreal`.** They pollute completion and stubs, so filter them. (`PSP/Private/PyTest.h`, no compile gate)
6. **Experimental test runner.** `unreal.PythonTestRunner` sits in an Experimental, `NoRedist`, off-by-default plugin with a FileSandbox dependency, so its API or availability may change. Only feature-detect it.
7. **Automatic test discovery.** PythonAutomationTest runs every `test_*.py` in every enabled plugin's `Content/Python`. Shipped test files in our plugin, or Epic's `test_source_control.py`, would execute under `Editor.Python`.
8. **Startup installs.** `PythonRequirements` installs run synchronously on the game thread at startup by default (`bRunPipInstallOnStartup=true`), per project, in `Intermediate/` (which clean scripts delete). The venv is wiped when the engine's Python version changes. Strict hashes require pinning every transitive wheel for each platform. Heavy tool wheels (basedpyright with its Node, ty, ruff) there would add download time to editor startup. (`PSP/Private/PythonScriptPlugin.cpp:1353-1371`; `PipInstall.cpp:396-421`)
9. **`FInteractiveProcess` is unsafe for LSP traffic.** It is line-split `FString` output that drops empty lines, logs every line, and merges stderr. Use `CreateProc` with separate pipes and `ReadPipeToArray`. (`Engine/Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp:108,133-146`)
10. **`debugpy_unreal.install_debugpy()` targets the engine interpreter** under Program Files. In isolated mode the fallback `--user` install is invisible, so users following Epic's helper may hit confusing failures. Our debugger must vendor debugpy. (`.../Content/Python/debugpy_unreal.py:34-38`; `PythonScriptPluginSettings.h:73-79`)
11. **Stub generation runs engine code.** It invokes `ScriptConstant` UFunctions on CDOs to export constant values (`PSP/Private/PyWrapperTypeRegistry.cpp:3590-3597`). A broken plugin constant getter can crash or log during generation, so any runtime walker we build should read `__doc__` and descriptors without calling them.

## Files read

All paths are under `C:\Program Files\Epic Games\UE_5.8\Engine\` unless stated otherwise.

**Python plugin source**
- `Plugins/Experimental/PythonScriptPlugin/PythonScriptPlugin.uplugin`
- `PSP/PythonScriptPlugin.Build.cs`
- `PSP/Internal/PyGenUtilAccessor.h`; `PSP/Private/PyGenUtilAccessor.cpp`
- `PSP/Private/PyWrapperTypeRegistry.cpp` (L55-75, 225-262, 1400-1450, 1915-1960, 2000-2065, 2960-3600) and `.h` (L362-560)
- `PSP/Private/PyGenUtil.h` (L40-44, 924-951, 1110-1300); `PSP/Private/PyGenUtil.cpp` (L195-230, 600-680, 740-790, 1460-1540, 1984-2175, 2727-2790, 3500-3580, plus the export-rule functions)
- `PSP/Private/PyMethodWithClosure.h` (L34-121); `PSP/Private/PyMethodWithClosure.cpp` (L440-540)
- `PSP/Private/PyWrapperObject.cpp` (L1116-1135); `PSP/Private/PyWrapperStruct.cpp` (L1300-1311)
- `PSP/Private/PyFileWriter.cpp`; `PSP/Private/PyOnlineDocsWriter.cpp` (L67-80, 283-340) and `.h`; `PSP/Private/PythonOnlineDocsCommandlet.cpp`
- `PSP/Private/PythonScriptPlugin.cpp` (L95-175, 793-840, 1130-1165, 1210-1245, 1275-1300, 1340-1400, 1567-1615, 1715-1735, 1755-1881, 1990-2030, 2098-2125)
- `PSP/Private/PythonScriptPluginSettings.h`; `PSP/Private/PythonScriptPluginSettings.cpp`
- `PSP/Private/PythonScriptCommandlet.cpp`; `PSP/Private/PythonScriptLibrary.h`; `PSP/Private/EditorUtilities/EditorPythonExecuter.cpp` (L147)
- `PSP/Public/IPythonScriptPlugin.h`; `PSP/Public/IPipInstall.h`; `PSP/Public/PipInstallHelpers.h`; `PSP/Public/PythonScriptTypes.h`
- `PSP/Private/PipInstall.cpp` (L28-60, 171-306, 308-475, 476-555, 628-660, 739-800, 884-893); `PSP/Private/PipInstallLauncher.h`; `PSP/Private/PipRunnable.cpp` (L91-125)
- `PSP/Private/PyUtil.cpp` (L1187-1240); `PSP/Private/PyTest.h`; `PSP/Private/PyCore.cpp` (method tables)
- `Plugins/Experimental/PythonScriptPlugin/Content/Python/{debugpy_unreal.py, test_wrapper_types.py, test_source_control.py}`, `Content/Python/Lib/wheels/`, `Content/Python/PipInstallUtils/requirements.txt`

**Test plugins**
- `Plugins/Tests/PythonAutomationTest/{PythonAutomationTest.uplugin, Source/.../PythonAutomationTest.h, PythonAutomationTest.cpp, Content/Python/unreal_pythonautomationtest.py, Content/Python/automation_test/unittest_utilities.py}`
- `Plugins/Experimental/ToolsetRegistry/{ToolsetRegistry.uplugin, Source/ToolsetRegistry/Private/ToolsetRegistry/PythonTestRunner.h, PythonTestRunner.cpp, Content/Python/init_unreal.py, Content/Python/toolset_registry/tests/unittest_runner.py (outline)}`

**Other plugins and descriptors**
- `Plugins/Experimental/PythonFoundationPackages/PythonFoundationPackages.uplugin`
- Vendored site-packages dirs under `Plugins/Runtime/USDCore`, `Plugins/Experimental/HairCardGenerator` and `Plugins/VirtualProduction/CameraCalibrationML`

**UnrealBuildTool**
- `Source/Programs/UnrealBuildTool/Configuration/{UEBuildModuleCPP.cs (L455-485), UEBuildModule.cs (L575-595, 720-750), Rules/RulesCompiler.cs (L118-140, 236-256), Rules/ModuleRules.cs (L849-853, 1424)}`, `Modes/PipInstallMode.cs`

**Engine runtime**
- `Source/Runtime/Core/Public/Misc/{InteractiveProcess.h, MonitoredProcess.h}`, `Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp`, `Source/Runtime/Core/Public/GenericPlatform/GenericPlatformProcess.h`, `Source/Runtime/Core/Public/Windows/WindowsPlatformProcess.h`
- `Source/Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h`
- `Source/Developer/AutomationController/Private/AutomationCommandline.cpp` (L610)

**Bundled Python**
- `Binaries/ThirdParty/Python3/Win64/` (listing, `NEWS.txt`, `Lib/site-packages`, `Lib/ensurepip/_bundled`, `Scripts`, `DLLs`)
- Full-engine `find` for jedi, parso, debugpy, pydevd, pyflakes, pylint, black, ruff, pyright and typeshed

**Research files** (in `<repo>\docs\research\`)
- `03-ide-features-lsp-dap.md`, `04-unreal-python-internals.md`, `05-architecture-and-robustness.md` and `06-user-voice-and-landscape.md`, read for claims via grep and targeted ranges.
- `01` and `02` were grepped for stub and pip claims only.
