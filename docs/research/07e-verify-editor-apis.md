> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 07e: Verifying editor framework APIs, keybinding conflicts, processes and 5.8 features against UE 5.8 source

Lane: verification. Date: 2026-10-28. Source: local UE 5.8 install (`C:\Program Files\Epic Games\UE_5.8\Engine`). Paths below are relative to `Engine/`. Line numbers come from that install. Web search was not used.

---

## Summary

1. **`ScopedEditorTransaction.cancel()` throws away the undo record. It does not revert anything.**
   - `cancel()` calls `GEditor->CancelTransaction(id)`, which leads to `UTransBuffer::Cancel`. That function pops the pending transaction off the undo buffer and puts the redo stack back. It never applies the transaction, so every object change made inside the scope stays in place and can no longer be undone.
   - Cancelling a *nested* transaction cancels the **whole outer transaction**, and logs a warning.
   - `__exit__` always commits, even when an exception is propagating.
   - If the Python object is destroyed without `__exit__` running, the transaction is left open.
2. **Keyboard routing.** Every key goes through these stages, in order:
   1. editor pre-input listeners;
   2. Slate **input preprocessors**;
   3. tunnelling `OnPreviewKeyDown` (root to leaf);
   4. bubbling `OnKeyDown` (leaf to root). On the way up it passes `SLevelEditor` (mode toolkits first, then Level Editor commands) and `SDockingTabStack` (Ctrl+W);
   5. finally the **MainFrame "unhandled key" handler**. That handler holds the editor-wide commands, Undo/Redo and the Play-In-Editor (PIE) commands.

   Our focused widget can shadow anything from stage 4 onward by returning `Handled`. It **cannot** shadow preprocessors: the AI Assistant plugin, when enabled, takes F1 and Alt+F1 globally.

   Of the chords asked about, these are already bound:
   - **editor-wide:** Ctrl+Space, Ctrl+P, Ctrl+B, Ctrl+Tab;
   - **Level Editor:** Ctrl+D, Ctrl+G, Ctrl+Shift+F;
   - **dock tab stack:** Ctrl+W;
   - **conditional:** F10/F11 step commands, which only run while stopped at a Blueprint breakpoint.

   These are free editor-wide: Ctrl+Enter, Ctrl+Shift+Enter, Ctrl+Alt+Enter, Ctrl+/, Ctrl+Shift+P (viewport only), F5, F9, Ctrl+T, Ctrl+F, Alt+Up/Down. Shift+Shift cannot be expressed as an `FInputChord` at all.
3. **Child processes.**
   - `FPlatformProcess::CreateProc` (all overloads) **never puts the child in a Job Object**. Only the blocking `ExecProcess(..., bShouldEndWithParentProcess=true)` uses a kill-on-close job.
   - `FInteractiveProcess` and `FMonitoredProcess` are line-based. Both merge stderr into stdout. `FMonitoredProcess` has no stdin at all.
   - `WritePipe(FString)` adds a `\n` to every write, and `ReadPipe(FString)` can split UTF-8 characters across reads.
   - For a language server: use `CreateProc(FProcessStartInfo)` with **separate** stdout and stderr pipes, use the binary `ReadPipeToArray`/`WritePipe(uint8*)`, run it on our own thread, and create our own Job Object.
4. **File watching and checkout.**
   - `IDirectoryWatcher` is per directory, built on `ReadDirectoryChangesW`. It is ticked from `UEditorEngine::Tick`, so callbacks arrive on the game thread.
   - `USourceControlHelpers::CheckOut*` (Python: `unreal.SourceControl`) **blocks** until the operation completes. `ISourceControlProvider::Execute(FCheckOut, ..., EConcurrency::Asynchronous)` is the non-blocking path.
   - `FEditorFileUtils` checkout APIs work on packages only. They are not useful for `.py` files.
5. **Docking and menus.**
   - Nomad tabs register through `FGlobalTabmanager::RegisterNomadTabSpawner`.
   - Layouts persist to `EditorLayout.ini` in the **engine-version-wide, project-agnostic** user Saved dir.
   - Menu names confirmed: `LevelEditor.MainMenu.Tools` (parent `MainFrame.MainMenu.Tools`), `ContentBrowser.AssetContextMenu[.<Class>]`, `ContentBrowser.ItemContextMenu.<DataSource>`, `ContentBrowser.FolderContextMenu`, and `LevelEditor.ActorContextMenu`. The Level Editor also has Component and Element variants, chosen by what is selected.
6. **5.8 AI surfaces.**
   - **Unreal MCP** (`Plugins/Experimental/ModelContextProtocol`): Experimental, off by default, HTTP on `localhost:8000/mcp`. It checks the `Origin` header but has no authentication. Requests are dispatched from a game-thread ticker.
   - **27 toolset plugins** live under `Plugins/Experimental/Toolsets`. Toolsets are authored in Python as `unreal.ToolsetDefinition` subclasses with `@toolset_registry.tool_call` static methods.
   - The only Python-executing tool is `ProgrammaticToolset.execute_tool_script`, which runs scripts against a module allowlist with `exec`/`eval`/`compile` removed.
   - The **AI Assistant** does run toolset tools. It can optionally wrap each call in a transaction on a **swapped-in, per-conversation `UTransBuffer`**.
   - **Output Log console:** a multi-line box where Shift+Enter adds a new line. History is kept per executor in `Saved/Config/<Platform>/ConsoleHistory.ini`, capped at about 64 entries.
   - **Recent Python Scripts:** implemented in `PythonScriptPlugin.cpp` (`FPythonCommandMenuImpl`). It stores up to 10 files in `EditorPerProjectUserSettings.ini` `[Python]`.
7. **Settings layering.**
   - `config=Editor, defaultconfig`: Project Settings, saved to `Config/DefaultEditor.ini`. The save silently fails if the file is read-only.
   - `config=EditorPerProjectUserSettings`: Editor Preferences, saved to `Saved/Config/<Platform>/EditorPerProjectUserSettings.ini`.
   - For settings that follow a user across projects: `config=EditorSettings` (per engine version) or `globaluserconfig` (`%LOCALAPPDATA%/Unreal Engine/Engine/Config/User<Config>.ini`).

---

## Q1. `unreal.ScopedEditorTransaction` and `cancel()`

### Answer

The Python type is a thin wrapper:
- `__enter__` calls `GEditor->BeginTransaction(desc)` and stores the returned index.
- `__exit__` calls `GEditor->EndTransaction()` **unconditionally**. It ignores any exception and returns `None`, so the exception still propagates, but the partial work is committed as one undo entry.
- `cancel()` calls `GEditor->CancelTransaction(index)`, which forwards to `UTransBuffer::Cancel`.

`UTransBuffer::Cancel`:
- broadcasts `TransactionCanceled`;
- ends the operation;
- clears `GUndo`;
- **pops the transaction record off `UndoBuffer`**;
- restores any redo entries that were set aside when the transaction began;
- resets `ActiveCount`.

It never calls `Apply()` or `Undo()`. **Object changes made inside the scope stay as they are, and the snapshots that would have reverted them are deleted.** Cancelling therefore means "make these edits permanent and un-undoable", not "roll back". Epic's own Blueprint doc for the same operation says the same: "Cancel the current transaction, and no longer capture actions to be placed in the undo buffer."

Further semantics that matter for our "Run in transaction" wrapper:
- **Nesting merges.** `Begin` returns the `ActiveCount` from *before* the increment: 0 for the outermost transaction, 1 or more for nested ones. Only the outermost `End` finalizes. A user `ScopedEditorTransaction` inside our wrapper therefore becomes part of our single undo entry, and the user's description is lost.
- **Partial cancel is not supported.** `Cancel(StartIndex != 0)` logs "Canceling transaction partially is unsupported. Canceling … entirely." and cancels the **outermost** transaction. If user code calls `t.cancel()` on a nested scope, it wipes out our wrapper's undo record for everything done so far in the run. Edits made after that point are not recorded either, because `ActiveCount` is now 0. When our wrapper later calls `End()` with `ActiveCount == 0`, it is a safe no-op.
- **Map loads purge the buffer.** `UEditorEngine::ResetTransaction` is called on map load ("LoadingMap"). It calls `UTransBuffer::Reset`, which detects a non-zero active count, logs it and calls `Cancel(0)`. A script that opens a level inside our wrapper silently loses the wrapper.
- **Dangling transactions.** `FPyScopedEditorTransaction::Free` and `Deinit` only reset the id. They never call `EndTransaction`. If code calls `__enter__()` by hand and drops the object, the transaction stays open. `UEditorEngine::IsTransactionActive()` can detect this.
- **No begin during load.** `BeginTransaction` returns `INDEX_NONE` when `!CanTransact()`, which covers package loading and PostLoad. Exit then does nothing.
- **Undo is refused while a transaction is open.** `UTransBuffer::CanUndo` returns false when `ActiveCount` is non-zero. To get a real rollback, a run must first End, then Undo: `GEditor->UndoTransaction()` in C++, or the console command `TRANSACTION UNDO` (which is also what Ctrl+Z maps to in MainFrame).
- **A precedent for isolated undo exists.** The 5.8 AI Assistant can swap `GEditor->Trans` for a per-conversation `UTransBuffer` while a tool runs, then restore the global one (Q6).

### Evidence
- `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyEditor.cpp:88-95`: `__enter__`, `PendingTransactionId = GEditor->BeginTransaction(InSelf->Description);`
- `PyEditor.cpp:97-106`: `__exit__`, unconditional `GEditor->EndTransaction();` then `Py_RETURN_NONE`.
- `PyEditor.cpp:108-117`: `cancel()`, `GEditor->CancelTransaction(InSelf->PendingTransactionId);`
- `PyEditor.cpp:45-49`: `Deinit` resets the id only; there is no `EndTransaction` on free.
- `Source/Editor/UnrealEd/Private/EditorServer.cpp:1086-1097, 1104-1111, 1113-1123, 1125-1131`: Begin, CanTransact, End and Cancel forward to `Trans`.
- `Source/Editor/UnrealEd/Private/ScopedTransaction.cpp:42-49`: `FScopedTransaction::Cancel` makes the same `GEditor->CancelTransaction(Index)` call.
- `Source/Editor/UnrealEd/Private/EditorTransaction.cpp:1411-1462`: `UTransBuffer::Cancel`, with the partial-cancel warning at `:1422` and `UndoBuffer.Pop(EAllowShrinking::No);` at `:1437`. It restores `RemovedTransactions` (the redo entries) at `:1438-1447` and contains no Apply/Undo call.
- `Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h:70-90`: `BeginInternal` returns `Result = ActiveCount; if (ActiveCount++ == 0) …`.
- `EditorTransaction.cpp:1299-1336`: `End()` tolerates `ActiveCount == 0` ("the code path may still call end"), and finalizes only when the count reaches 0.
- `EditorTransaction.cpp:1382-1394`: `Reset` with a non-zero active count calls `Cancel(0)`. Callers include `EditorServer.cpp:2456` (map load).
- `EditorTransaction.cpp:1468`: `CanUndo` is false while `ActiveCount || CurrentTransaction`.
- `Source/Runtime/Engine/Classes/Kismet/KismetSystemLibrary.h:2141, 2164-2170`: Blueprint/Python `Begin`/`CancelTransaction` docs ("increments that transaction's action counter"; "no longer capture actions").
- `EditorServer.cpp:1328-1331`: `IsTransactionActive()`.

**Confidence:** high.

### Implication for our design
- "Run in transaction" commits a single undo entry. We must **not** describe `cancel()` as rollback.
- For a "Stop and revert" action: End the wrapper, then call `GEditor->UndoTransaction()` once. Do this only if the wrapper is still the top entry and was not purged. Compare `Trans->GetQueueLength()` and the undo context title before and after the run.
- After every run, if `GEditor->IsTransactionActive()` is still true, close any dangling transactions. Warn the user.
- Detect, and lint-warn on, `ScopedEditorTransaction.cancel()` inside our wrapper. Detect map loads during a run, which purge the buffer, and report that undo is unavailable.
- Optional, advanced: a per-run isolated `UTransBuffer`, as in the AI Assistant precedent. This is high risk, because the global undo history would not see those edits.

---

## Q2. Keybinding conflicts and command routing

### How a key event is routed (5.8)
`FSlateApplication::ProcessKeyDownEvent`:
1. `OnApplicationPreInputKeyDownListenerEvent` broadcast. Editor only; observe only.
2. **`InputPreProcessors.HandleKeyDownEvent`**. If any preprocessor returns true, the event stops here.
3. **Tunnel:** `OnPreviewKeyDown`, root to focused leaf.
4. **Bubble:** `OnKeyDown`, focused leaf to root.
5. If still unhandled: **`UnhandledKeyDownEventHandler`**. MainFrame installs this. It runs `FMainFrameCommands::ActionList`, which includes the global editor commands, Undo/Redo and the global PIE actions. It is skipped during slow tasks.

The ancestors that act during bubbling, when our tab is docked inside the Level Editor area:
- **`SLevelEditor::OnKeyDown`**. It first offers the key to every hosted **mode toolkit** (Modeling, Landscape and so on), then to `LevelEditorCommands`. That list combines the global Level Editor actions with `FGenericCommands` mappings (Duplicate, Delete, Copy, Paste, SelectAll) and `GlobalPlayWorldActions`.
- **`SDockingTabStack::OnKeyDown`**: `FTabCommands` (Ctrl+W closes the foreground *major* tab).

In a floating window only stage 5 applies, because `SLevelEditor` is not in the widget path.

`FUICommandList::ConditionalProcessCommandBindings`:
- looks up the *active* (user-rebound) chord in each context;
- returns true only when the action can execute. If a command is bound but cannot execute, it returns false, so the key keeps bubbling.

The defaults are worth knowing:
- **Stock text widgets absorb printable keys.** `FSlateEditableTextLayout::HandleKeyDown` returns `Handled` for any key with a character and no Ctrl/Alt (Tab excluded), and also for AltGr+character, so these never reach commands.
- **Base widgets turn arrows and Tab into navigation.** `SWidget::OnKeyDown` converts arrows and Tab into Slate *navigation* requests (`FNavigationConfig`).

`FInputChord` is just a key plus Shift/Ctrl/Alt/Cmd bits. It has no sequences and no double-tap.

### Chord table (Windows defaults)
Scope: **global** = MainFrame unhandled handler (anywhere in the editor). **LE** = bubbles through `SLevelEditor` (tab docked in the Level Editor area). **Viewport** = only when a level viewport has focus. **Local** = only inside that tool's own widget.

| Chord | Already bound? | Binding (evidence) |
|---|---|---|
| Ctrl+Enter | **No** (global/LE) | Local only: Sequencer `GoToFrame` (`Source/Editor/Sequencer/Private/ToolableTimeline/ToolableTimelineCommands.cpp:38`), Avalanche Rundown `PreviewPlay` |
| Ctrl+Shift+Enter | **No** | none found |
| Ctrl+Alt+Enter | **No** | none found |
| Alt+Enter / Shift+Enter | No (global) | Local: PCG code editor `ApplyChanges` Alt+Enter (`Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:69`); Output Log console uses Shift+Enter for a new line |
| Ctrl+Space | **Yes, global**: Open Content Browser drawer | `Source/Editor/UnrealEd/Private/Toolkits/GlobalEditorCommonCommands.cpp:71` |
| Ctrl+Shift+Space | Viewport/local only: "Next" view option; UMG anim drawer | `Source/Editor/UnrealEd/Private/EditorViewportCommands.cpp:51`; `Source/Editor/UMGEditor/Private/UMGEditorActions.cpp:28` |
| Ctrl+/ | **No** (global/LE) | Local: Sequencer `SplitSection` (`SequencerCommands.cpp:108`), PCG and Optimus code boxes `ToggleComment`. **Bare `/` and Shift+/ are LE bindings** (particle reset, `Source/Editor/LevelEditor/Private/LevelEditorActions.cpp:4707-4708`) |
| Ctrl+D | **Yes, LE**: `FGenericCommands::Duplicate` → `DUPLICATE` (duplicates actors) | `Source/Runtime/Slate/Private/Framework/Commands/GenericCommands.cpp:17`; `Source/Editor/LevelEditor/Private/LevelEditor.cpp:1089-1092` |
| Ctrl+Shift+P | Viewport only: Pilot Selected Actor | `Source/Editor/LevelEditor/Private/LevelViewportActions.cpp:58` |
| Shift+Shift (double) | Not expressible | `Source/Runtime/Slate/Public/Framework/Commands/InputChord.h:23-45` (key + 4 modifier bits) |
| F5 | **No** (global/LE) | Local: Scene Outliner Refresh (`Source/Editor/SceneOutliner/Private/ActorBrowsingModeCommands.cpp:14`) |
| F9 | **No** (global/LE) | Local: graph `ToggleBreakpoint` (`Source/Editor/GraphEditor/Private/GraphEditorActions.cpp:48`); viewport ScreenCapture (`EditorViewportCommands.cpp:146`) |
| F10 | **Conditional global**: PIE StepOver (only when stopped at a Blueprint breakpoint); Viewport: Sidebar All Tabs | `Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp:370`, CanExecute `IsStoppedAtBreakpoint` `:549-564`; `LevelViewportActions.cpp:51` |
| F11 | **Conditional global**: PIE StepInto; Viewport: Immersive; **Shift+F11 global** Fullscreen | `DebuggerCommands.cpp:369`; `LevelViewportActions.cpp:44-49`; `Source/Editor/MainFrame/Private/Frame/MainFrameActions.cpp:261` |
| Ctrl+Tab | **Yes, global**: tab switcher (Ctrl+Shift+Tab backwards) | `GlobalEditorCommonCommands.cpp:48, 51` |
| Ctrl+W | **Yes, dock tab stack**: Close Major Tab (Ctrl+Shift+W = Widget Reflector, global) | `Source/Runtime/Slate/Private/Framework/Docking/TabCommands.cpp:9`; `SDockingTabStack.cpp:563-565, 1571, 1585-1590`; `MainFrameActions.cpp:269` |
| Ctrl+T | **No** (global/LE) | Local: Sequencer, Physics Asset Editor |
| Ctrl+F | **No** (global/LE) | Local: find in asset editors (Blueprint, Material, Niagara…) |
| Ctrl+Shift+F | **Yes, LE**: Open Content Browser | `LevelEditorActions.cpp:4664` |
| Ctrl+G | **Yes, LE**: Regroup actors | `LevelEditorActions.cpp:4570` |
| Ctrl+B | **Yes, global**: Browse to Asset in Content Browser | `GlobalEditorCommonCommands.cpp:56` |
| Ctrl+P | **Yes, global**: Open Asset picker (also Alt+Shift+O) | `GlobalEditorCommonCommands.cpp:54-55` |
| Alt+Up / Alt+Down | **No** | Local: Chooser table editor only |

Other bindings that matter because our widget would receive them first:
- **Global (MainFrame and `SystemWideCommands`):**
  - Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z → `TRANSACTION UNDO/REDO` (`MainFrameActions.cpp:184, 186`; `GenericCommands.cpp:19-20`);
  - Ctrl+Shift+S Save All, Ctrl+Alt+Shift+S Choose Files to Save (`MainFrameActions.cpp:103, 106`);
  - Ctrl+O Open Level (`GlobalEditorCommonCommands.cpp:65`);
  - `` ` `` open console, Ctrl+`` ` `` next executor, Alt+`` ` `` Output Log drawer (`:58-61`);
  - Shift+F1 docs (`:63`);
  - Ctrl+Alt+E/K/P open Preferences, Shortcuts and Project Settings (`:82-84`);
  - PIE: Alt+P, Alt+S, Esc stop, F8, Pause (`DebuggerCommands.cpp:342-365`).
- **LE:**
  - Ctrl+S Save Current Level, Ctrl+Alt+S Save As, Ctrl+N New Level (`LevelEditorActions.cpp:4366-4368`);
  - Ctrl+E Edit Asset (`:4466`), Ctrl+H Show All (`:4581`), Ctrl+L (`:4710`), Ctrl+M (`:4653`);
  - Ctrl+Shift+D / Ctrl+Alt+D (`:4600-4601`);
  - Esc Unselect All (`:4597`), End Snap to Floor (`:4499`), F4 Show Details (`:4701`);
  - Delete and Backspace → `DELETE` actors (`LevelEditor.cpp:1095`), Ctrl+C/V/A (`:1113, 1119, 1433`).
- **AI Assistant (if enabled):** F1 (query UI under mouse) and Alt+F1 (summon panel), taken in an **input preprocessor** before any widget sees them (`Plugins/Experimental/AIAssistant/Source/AIAssistant/Private/AIAssistantCommands.cpp:21-22`; `AIAssistant.cpp:100`; `AIAssistantInputProcessor.cpp:34-45`).
- **Output Log console (`SConsoleInputBox`):**
  - Up shows a history popup only when the box is empty, or with Ctrl+Up;
  - Tab and Up/Down step through suggestions;
  - Esc clears the box, or closes it when already empty;
  - the open-console chord closes it (`Source/Developer/OutputLog/Private/SOutputLog.cpp:488-548, 859-870`).
- **User rebinding:** user rebinds are stored in `EditorKeyBindings.ini` (`Source/Runtime/Slate/Private/Framework/Commands/InputBindingManager.cpp:149`). Conflict lookup only walks the context's parents and children (`InputBindingManager.cpp:741-768`), so a parentless "ScriptEditor" context never shows conflicts with MainFrame.

### Routing evidence
- `Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:4975-4978` (pre-listener), `:4982` (preprocessors), `:5025-5042` (tunnel), `:5047-5065` (bubble), `:5069-5071` (unhandled handler).
- `MainFrameActions.cpp:97` (handler installed), `:272` (global editor commands mapped into the MainFrame list), `:275-289` (`OnUnhandledKeyDownEvent`: MainFrame list, then `GlobalPlayWorldActions`; skipped if `GIsSlowTask`).
- `Source/Editor/LevelEditor/Private/SLevelEditor.cpp:151, 154` (appends global Level Editor and PlayWorld actions), `:1962-1985` (`OnKeyDown`: hosted toolkits, then Level Editor commands).
- `Source/Runtime/Slate/Private/Framework/Commands/UICommandList.cpp:203-251` (active chord only; returns false if `!CanExecute`).
- `Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp:1273-1294` (absorb typing keys and AltGr, then a local command list).
- `Source/Runtime/SlateCore/Private/Widgets/SWidget.cpp:416-429`; `Source/Runtime/Slate/Private/Framework/Application/NavigationConfig.cpp:9-29`.

**Confidence:** high for the bindings listed. Medium for "none found": the scan covered `UI_COMMAND` statements in `Source/Editor`, `Source/Developer`, `Source/Runtime/Slate*`, `Source/Runtime/Engine` and `Plugins`. Code that hard-codes keys in `OnKeyDown` could still exist.

### Implication for our design
- `SCodeView::OnKeyDown` must return `Handled` for the following, whether or not we have an action for them:
  - every printable key and every AltGr combination;
  - Tab and Shift+Tab, arrows, Home/End, PageUp/PageDown;
  - Delete and Backspace, Esc;
  - Ctrl+Z/Y/Shift+Z, Ctrl+C/V/X/A, Ctrl+D, Ctrl+S, Ctrl+W.

  Otherwise these fall through to actor delete, level save, transaction undo or major-tab close.
- Safe defaults:
  - Ctrl+Enter: run selection or buffer;
  - Ctrl+Shift+Enter: run file;
  - Ctrl+/ toggle comment;
  - F5 / Shift+F5: run / stop;
  - F9 breakpoint, F10/F11 step. These are only shadowed by PIE stepping while Blueprint-paused, and we take them while focused;
  - Ctrl+Shift+P command palette;
  - Ctrl+F / Ctrl+H find and replace (Ctrl+H shadows the Level Editor's Show All);
  - Ctrl+Shift+F find in files (shadows the Level Editor's Open Content Browser);
  - Ctrl+G go to line;
  - Ctrl+Space completion (shadows the Content Browser drawer only while we have focus);
  - Ctrl+Shift+Space parameter info.
- Leave Ctrl+Tab, Ctrl+P and Ctrl+B to UE by default. UE users rely on them editor-wide. Offer PyCharm-style variants (Ctrl+Shift+N go to file, F12 / Ctrl+Click go to definition) in an optional "PyCharm" keymap preset.
- Implement Shift+Shift ("search everywhere") inside our widget by timing two bare Shift key-ups. Use a global input preprocessor only if it must work editor-wide, and make it pass every other key through.
- F1 is unusable while the AI Assistant is enabled. Use Ctrl+Q for quick docs, as PyCharm does.
- Register all our chords as a `TCommands` context named "ScriptEditor", so users can rebind them in Editor Preferences.

---

## Q3. Processes for language servers

### Answer
- **No automatic job object.** `FWindowsPlatformProcess::CreateProcInternal`, which every `CreateProc` overload uses:
  - always sets `CREATE_NEW_PROCESS_GROUP`;
  - adds `CREATE_BREAKAWAY_FROM_JOB` only when `bBreakawayFromJob` is set;
  - limits handle inheritance to the given pipes with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (when `!bInheritHandles`);
  - **never calls `AssignProcessToJobObject`**.

  The only kill-on-close job in Core is created lazily inside the blocking `ExecProcess` when `bShouldEndWithParentProcess` is true (the default is false). `SetProcessMemoryLimit`, a commented-out debug helper, creates the only other job. A language server started with `CreateProc` **outlives an editor crash or force-kill**.
- **Stderr merges into stdout by default.** The legacy 10-argument `CreateProc(..., PipeWriteChild, PipeReadChild)` deliberately sends stderr into the stdout pipe. `FProcessStartInfo` defaults `StdErr = StdOut`. Either way, server logs on stderr would corrupt the JSON-RPC stream unless we pass a separate stderr pipe.
- **Pipe helpers:**
  - `CreatePipe(Read, Write, bWritePipeLocal)` creates an anonymous, synchronous pipe and marks one end non-inheritable.
  - `ReadPipe` returns `FString`, and the source itself warns that multi-byte characters split across reads become corrupt.
  - `ReadPipeToArray` is binary and polls with `PeekNamedPipe`, without blocking.
  - `WritePipe(FString)` **appends `\n`** to every message. `WritePipe(const uint8*, int32)` is binary and exact.
  - `WriteFile` on an anonymous pipe blocks when the pipe buffer is full.
- **`FInteractiveProcess` and `FMonitoredProcess` don't fit LSP framing:**
  - `FInteractiveProcess` uses the legacy overload, so stdout and stderr are merged. It splits reads on `\n`, drops empty lines, logs every line in both directions to `LogInteractiveProcess`, and busy-polls with `SleepTime = 0` unless `LongTime` is set.
  - `FMonitoredProcess` creates **only a stdout pipe and no stdin**. It splits on `\r` or `\n`, and its Cancel can kill the whole process tree.
- **Killing a tree.** `TerminateProc(Handle, KillTree=true)` walks a Toolhelp snapshot and kills the child processes too.

### Evidence
- `Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp:511-518`: the legacy overload ("stdout and stderr would be piped into … preserve that behaviour").
- `WindowsPlatformProcess.cpp:529-534`: the `FProcessStartInfo` overload.
- `WindowsPlatformProcess.cpp:538-698`: `CreateProcInternal`, with `CREATE_NEW_PROCESS_GROUP` at `:568`, breakaway at `:590-593`, the handle list at `:624-667`, `CreateProcess` at `:684`, and no job assignment.
- `WindowsPlatformProcess.cpp:1076-1097, 1168-1171`: `ExecProcess`, `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and `AssignProcessToJobObject`, only if `bShouldEndWithParentProcess`.
- `Source/Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:992-1003`: the debug-only memory-limit job.
- `Source/Runtime/Core/Public/GenericPlatform/GenericPlatformProcess.h:196-209`: `FProcessStartInfo` (`FWriteHandle StdErr = StdOut;`, `bBreakawayFromJob`).
- `GenericPlatformProcess.h`: `:629` `CreateProc(StartInfo)`, `:717-719` `ExecProcess` (`bShouldEndWithParentProcess = false`), `:829, 838, 848, 859, 871` pipe API.
- `WindowsPlatformProcess.cpp:1979-1994` (`CreatePipe`), `:1996-2012` (`ReadPipe`, corruption note at `:1998`), `:2014-2037` (`ReadPipeToArray`), `:2039-2066` (`WritePipe(FString)` appends `'\n'` at `:2052`), `:2068-2087` (binary `WritePipe`), `:751-790` (`TerminateProc` KillTree).
- `Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp`: `:56-63` (sleep), `:95-108` (pipes and legacy `CreateProc`), `:133-148` (split, drop, log), `:170-178` (logs writes), `:215-245` (poll loop).
- `Source/Runtime/Core/Private/Misc/MonitoredProcess.cpp:111-116` (stdout pipe only, `nullptr` stdin) and `:151-175` (line split).

**Confidence:** high.

### Implication for our design
Build an `FLspTransport` on its own `FRunnable`:
1. Create three pipes: stdin with `bWritePipeLocal=true`, plus stdout and stderr.
2. Start the server with `CreateProc(FProcessStartInfo{…, StdIn, StdOut, StdErr, bHidden=true})`.
3. Immediately `CreateJobObject` with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and `AssignProcessToJobObject`, using Windows headers inside `AllowWindowsPlatformTypes`.
4. Do framing ourselves on bytes from `ReadPipeToArray`, and write with the binary `WritePipe`.
5. Drain stderr into our log view.
6. Never write from the game thread.

`CreateProc` cannot start the process suspended. A server that spawns grandchildren within microseconds could therefore escape the job. If that turns out to matter, add a native Windows path that uses `CREATE_SUSPENDED` (the remaining risk is listed under New risks).

---

## Q4. File watching, source control, checkout-on-save

### Answer
- **`IDirectoryWatcher`** (module `DirectoryWatcher`):
  - `RegisterDirectoryChangedCallback_Handle(Dir, FDirectoryChanged, OutHandle, Flags)` and `UnregisterDirectoryChangedCallback_Handle`.
  - Flags: `IncludeDirectoryChanges`, `IgnoreChangesInSubtree`.
  - The callback receives `TArray<FFileChangeData>` with actions `FCA_Added`, `FCA_Modified`, `FCA_Removed` or `FCA_RescanRequired`; the last means overflow, so rescan.
  - It watches directories, not files. The Windows implementation uses `ReadDirectoryChangesW` with a 16,384-entry buffer and completes in `Tick` through an alertable `SleepEx(0)`.
  - `UEditorEngine::Tick` calls `Tick`, so callbacks run on the game thread and are **not** delivered while a script blocks the game thread.
  - The PythonScriptPlugin does not watch Python files itself.
- **Source control** (module `SourceControl`):
  - `ISourceControlModule::Get().IsEnabled()`, `GetProvider()`, `RegisterProviderChanged`, and `GetSourceControlFileStatusMonitor()`. The monitor throttles and batches `FUpdateStatus` calls for files we keep open.
  - Provider methods: `GetState(File, EStateCacheUsage)`; `Execute(ISourceControlOperation::Create<FCheckOut|FMarkForAdd|FRevert|FUpdateStatus>(), Files, EConcurrency::Synchronous|Asynchronous, Delegate)`; `UsesCheckout()`, which is false for Git; and `UsesLocalReadOnlyState()`.
  - State queries: `CanCheckout`, `IsCheckedOut`, `IsCheckedOutOther(Who)`, `IsSourceControlled`, `IsCurrent`, `CanAdd`, `CanEdit`, `IsModified`.
  - Script and Blueprint helpers live in `USourceControlHelpers` (Python name `unreal.SourceControl`): `CheckOutFile`, `CheckOutOrAddFile`, and the C++-only `CheckoutOrMarkForAdd`. All of them say "**Blocks until action is complete**".
- **`FEditorFileUtils`** checkout APIs take `UPackage*` or package names: `PromptForCheckoutAndSave`, `PromptToCheckoutPackages`, `CheckoutPackages`. They do not apply to `.py` files.
- **Related:** `UObject::TryUpdateDefaultConfigFile` writes nothing and only warns when `Default*.ini` is read-only, as it is under Perforce (Q7).

### Evidence
- `Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:8-40, 52-55, 59, 72, 80`.
- `Source/Developer/DirectoryWatcher/Private/Windows/DirectoryWatchRequestWindows.cpp:15, 22-23`.
- `Source/Developer/DirectoryWatcher/Private/Windows/DirectoryWatcherWindows.cpp:115-153`.
- `Source/Editor/UnrealEd/Private/EditorEngine.cpp:1948` (`DirectoryWatcherModule.Get()->Tick(DeltaSeconds)`).
- `Source/Developer/SourceControl/Public/ISourceControlModule.h:195, 200, 261, 387`.
- `Source/Developer/SourceControl/Public/ISourceControlProvider.h:35-42, 264, 309-329, 423, 438`.
- `Source/Developer/SourceControl/Public/ISourceControlState.h:150-217`.
- `Source/Developer/SourceControl/Public/SourceControlOperations.h:131, 239, 275, 477`.
- `Source/Developer/SourceControl/Public/SourceControlFileStatusMonitor.h:17-54`.
- `Source/Developer/SourceControl/Public/SourceControlHelpers.h:244-245` (`ScriptName = "SourceControl"`), `:304-313, 326-335, 348-359`.
- `Source/Editor/UnrealEd/Public/FileHelpers.h:530-585`.

**Confidence:** high.

### Implication for our design
- **File watching.** Watch the parent directory of every open file, and debounce events. Treat `FCA_RescanRequired` as "stat every open file". Because callbacks stop during long runs, re-stat buffers when a run ends.
- **Saving a read-only file:**
  1. If `IsEnabled() && UsesCheckout()`, show the non-modal bar "Check out / Make writable / Save as…".
  2. On Check out, run `Execute(FCheckOut, {File}, EConcurrency::Asynchronous, cb)`.
  3. Write the file in the callback.
  4. Show a `IsCheckedOutOther` warning, via the status monitor, when the file is opened.
- **Never** call `unreal.SourceControl.check_out_file` or `CheckOutOrAddFile` from the save path on the game thread. With a slow Perforce server it freezes the editor.

---

## Q5. Docking and menus

### Answer
- **Nomad tab.** Register with:

  ```cpp
  FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab, FCanSpawnTab)
  ```

  It returns an `FTabSpawnerEntry&` with `SetDisplayName`, `SetTooltipText`, `SetIcon`, `SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory() / GetDeveloperToolsMiscCategory())`, `SetMenuType(Hidden)` and `SetCanSidebarTab`.
  - Registering again replaces the old entry. Epic does this on purpose to support "Window > Load Layout".
  - Open the tab with `TryInvokeTab(FTabId)`.
  - One TabId means one live tab. For multiple windows, use a major tab with its own `FTabManager`, as the CodeEditor plugin does (04 §6.4).
  - The AI Assistant registers its nomad tab with `SetMenuType(Hidden)` and `SetCanSidebarTab(false)`, because sidebar tabs misbehave in the panel drawer.
- **Layout persistence.**
  - `FGlobalTabmanager::SaveAllVisualState()` calls `SavePersistentLayout()` for the global manager and each sub-manager, and the layout is written through `FLayoutSaveRestore::SaveToConfig` to `GEditorLayoutIni`.
  - **`EditorLayout.ini`, `EditorSettings.ini` and `EditorKeyBindings.ini` live in `FPaths::EngineEditorSettingsDir()`, the project-agnostic engine Saved/Config folder.** For an installed engine that is `%LOCALAPPDATA%/UnrealEngine/5.8/Saved/Config/…`. Docking layouts are therefore shared across all projects on the same engine version.
  - On restore, a tab whose spawner is not registered is **not displayed**, but it is kept in the saved layout ("keep the unknown widget saved in the layout"). Some code paths show an "unrecognized tab" placeholder instead.
  - Modules loaded at `Default` or `PostEngineInit` are registered before `CreateDefaultMainFrame`, because `EditorInit` runs `EngineLoop.Init()` first. Spawners registered in such a module's `StartupModule` therefore restore correctly.
- **`UToolMenus` extension points.** Register them inside `UToolMenus::RegisterStartupCallback(...)`, use `FToolMenuOwnerScoped` with `UnregisterOwner` on shutdown, then `ExtendMenu(Name)` and add a section or entry.
  - **Tools menu:**
    - `LevelEditor.MainMenu.Tools` is registered with parent `MainFrame.MainMenu.Tools`. The parent is the base Tools menu that menus in other editors inherit from.
    - The PythonScriptPlugin adds section `"Python"` (*Execute Python Script…*, *Recent Python Scripts*) to `LevelEditor.MainMenu.Tools`.
    - Extending `MainFrame.MainMenu.Tools` makes an entry visible beyond the Level Editor.
  - **Content Browser asset context menu:**
    - `ContentBrowser.AssetContextMenu` is the base. Per-class submenus are generated from the class hierarchy as `ContentBrowser.AssetContextMenu.<Class>`.
    - For non-asset data sources, the menu name is `ContentBrowser.ItemContextMenu.<DataSourceName>`. The Python plugin extends `ContentBrowser.ItemContextMenu.PythonData`, which is the right hook for "Open in Script Editor" on `.py` items.
    - Context object: `UContentBrowserAssetContextMenuContext`.
  - **Content Browser folder context menu:** `ContentBrowser.FolderContextMenu`, with context `UContentBrowserFolderContext`.
  - **Level Editor actor context menu:**
    - `LevelEditor.ActorContextMenu`, with context `ULevelEditorContextMenuContext`.
    - Which menu opens depends on the selection: `LevelEditor.ComponentContextMenu` if components are selected, `ActorContextMenu` for actors, `ElementContextMenu` for other typed elements.
    - The outliner uses `LevelEditor.SceneOutlinerContextMenu` / `LevelEditor.LevelEditorSceneOutliner.ContextMenu`.
  - Also present: `OutputLog.ConsoleInputBox.CmdExecMenu`, the executor dropdown.

### Evidence
- `Source/Runtime/Slate/Public/Framework/Docking/TabManager.h:221, 294-338, 1087, 1561-1563, 1603`.
- `Source/Runtime/Slate/Private/Framework/Docking/TabManager.cpp:3559-3576, 3719-3731, 2104-2108, 2620-2657`.
- `Source/Runtime/Core/Private/Misc/ConfigCacheIni.cpp:6647-6654` ("Project agnostic editor ini files"), `Source/Runtime/Core/Private/Misc/ConfigContext.cpp:476-482`.
- `Source/Runtime/Core/Private/Misc/Paths.cpp:204-213, 238-246, 117-120`.
- `Source/Runtime/Slate/Private/Framework/Docking/LayoutService.cpp:244`.
- `Source/Editor/UnrealEd/Private/UnrealEdGlobals.cpp:244, 326`; `Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:4861-4863`.
- `Source/Editor/WorkspaceMenuStructure/Public/WorkspaceMenuStructure.h:65, 83`.
- `Source/Developer/ToolMenus/Public/ToolMenus.h:123, 145-151, 161, 169`.
- `Source/Editor/MainFrame/Private/Menus/MainMenu.cpp:548`; `Source/Editor/LevelEditor/Private/LevelEditorMenu.cpp:369, 372`.
- `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:611-630, 1445`.
- `Source/Editor/ContentBrowser/Private/AssetContextMenu.cpp:195-199, 357-361`; `Source/Editor/ContentBrowser/Private/Menus/ContentBrowserMenus.cpp:72`; `Source/Editor/ContentBrowser/Private/SContentBrowser.cpp:5277`; `Source/Editor/ContentBrowser/Public/ContentBrowserMenuContexts.h:41, 299`.
- `Source/Editor/LevelEditor/Private/LevelEditorContextMenu.cpp:192, 264, 640, 785, 890-907`; `Source/Editor/LevelEditor/Public/LevelEditorMenuContext.h:59`.
- `Source/Developer/OutputLog/Private/SOutputLog.cpp:811`.
- `Plugins/Experimental/AIAssistant/Source/AIAssistant/Private/AIAssistant.cpp:66-75`.

**Confidence:** high.

### Implication for our design
- Register our spawners in `StartupModule` (LoadingPhase `Default` or `PostEngineInit`), never lazily, or the docked tab disappears on restart.
- Because `EditorLayout.ini` is cross-project, store **per-project session state** ourselves in `UScriptEditorUserSettings` (`EditorPerProjectUserSettings`): open documents, cursor positions, splits.
- Put "Script Editor" under the Window > Tools group, and add a Tools-menu entry next to the Python section.
- Context menus:
  - "Open in Script Editor" on `ContentBrowser.ItemContextMenu.PythonData`;
  - "Copy as Python / Insert reference" on `ContentBrowser.AssetContextMenu`, `ContentBrowser.FolderContextMenu` and `LevelEditor.ActorContextMenu`, plus the Component and Element variants for completeness.

---

## Q6. UE 5.8 AI surfaces, the Output Log console, and Recent Python Files

### Answer: Unreal MCP

**Plugin.** `Engine/Plugins/Experimental/ModelContextProtocol`:
- FriendlyName "Unreal MCP", Description "Anthropic MCP (Model Context Protocol) server implementation for Unreal Engine.";
- Experimental, `NoRedist`, `EnabledByDefault:false`;
- modules: `ModelContextProtocol`, `…Engine`, `…Editor`, plus tests;
- depends on `ToolsetRegistry`.

**Settings** (`UModelContextProtocolSettings`, stored as `config=EditorPerProjectUserSettings`, so per user):
- `ServerUrlPath` defaults to `/mcp`;
- `ServerPortNumber = 8000`;
- `bAutoStartServer = false`;
- `bEnableToolSearch = true`. In this mode `tools/list` exposes only the meta-tools `list_toolsets`, `describe_toolset` and `call_tool`.

**Transport and security:**
- The engine `HTTPServer` binds POST, GET (SSE) and DELETE routes. The listener's default `BindAddress` is `"localhost"`, so it is loopback only.
- The server **validates `Origin`**: it allows requests with no Origin, or Origin `localhost` / `127.0.0.1` / `[::1]`, to stop DNS rebinding.
- There is **no authentication**. No token, header or key check exists in the MCP sources.

**Threading:**
- `FHttpServerModule` is an `FTSTickerObjectBase`, so it is ticked by the core ticker on the game thread. Requests are dispatched from there, and `Tool->RunAsync(...)` is called on the game thread.
- Results may be posted from any thread. The server moves them back to the game thread (`AsyncTask(ENamedThreads::GameThread…)`, with `checkf(IsInGameThread())` in the handler).
- Synchronous tools are therefore effectively serialized on the game thread. Async tools (`ToolCallAsyncResult`, asyncio coroutines) can be in flight at the same time.
- While a script blocks the game thread, MCP requests are not processed. Conversely, anything that ticks `FTSTicker` *during* a run could execute agent tool calls re-entrantly.

**Toolsets.**
- `Engine/Plugins/Experimental/ToolsetRegistry` (Editor, Experimental, `NoRedist`; depends on PythonScriptPlugin, EditorScriptingUtilities and FileSandbox) provides the registry.
- `Engine/Plugins/Experimental/Toolsets/` contains 27 plugin folders: AIModuleToolset, AllToolsets, AnimationAssistantToolset, AutomationTestToolset, ChaosClothAssetToolset, ConfigSettingsToolset, ConversationToolset, DataRegistryToolset, DataflowAgent, EditorToolset, GASToolsets, GameFeaturesToolset, GameplayTagsToolset, LiveCodingToolset, MCPClientToolset, MVVMToolset, MetaHumanGenerator, NiagaraToolsets, PCGToolset, PhysicsToolsets, PluginToolset, SemanticSearchToolset, SequencerAnimMixerToolset, SlateInspectorToolset, StateTreeToolset, UMGToolSet, WorldConditionsToolset.
- `EditorToolset` defines ActorTools, AssetTools, BlueprintTools, CurveTableTools, DataAssetTools, DataTableTools, MaterialTools, MaterialInstanceTools, ObjectTools, PrimitiveTools, ProgrammaticToolset, SceneTools, SkeletalMeshTools, StaticMeshTools, StringTableTools and TextureTools.

**Authoring a toolset in Python:**
1. Write a `@unreal.uclass()` subclass of `unreal.ToolsetDefinition`.
2. Give it methods decorated `@toolset_registry.tool_call` + `@staticmethod`, with type hints and Google-style docstrings. Input and output schemas come from the types; `unreal.ustruct` / `uproperty` metadata such as `ToolTip` and `ClampMin`/`ClampMax` becomes schema fields.
3. Register the classes with `toolset_registry.Registration([...]).register()` from the plugin's `Content/Python/init_unreal.py`.

The registry pumps an asyncio loop from `unreal.register_ticker_callback`. C++ function-library toolsets `ensureAlways(IsInGameThread())` and invoke the `UFunction` directly.

**Arbitrary Python.**
- No shipped toolset calls `ExecPythonCommand*` or console exec. The only C++ call to `ExecPythonCommandEx` in these plugins is the test runner.
- `ProgrammaticToolset.execute_tool_script(script)` runs agent-written Python with `exec`. It is restricted by:
  - an AST plus runtime **module allowlist** (json, math, datetime, copy, re, time…);
  - builtins with `exec`, `eval`, `compile`, `input`, `breakpoint` and `help` removed;
  - a restricted read-only `open()` that resolves paths against allowed roots.

  Its docstring frames it as batching tool calls. It is an in-process allowlist, not a security boundary.
- None of the toolsets wraps calls in editor transactions. Only a test uses `ScopedEditorTransaction`.

### Answer: AI Assistant
`Engine/Plugins/Experimental/AIAssistant`:
- Editor, Experimental, `NoRedist`, `EnabledByDefault:false`; depends on PythonScriptPlugin, EditorScriptingUtilities and ToolsetRegistry.
- It is an embedded web app loading `https://dev.epicgames.com/community/assistant/embedded`.
- **It executes toolset tools.** `FWebApplication::ProcessToolCallContent` calls `ToolsetRegistry.ExecuteTool(Name, ArgsJson)`.
- With the dev option `bEnableUndoBuffer`, it swaps `GEditor->Trans` for a per-conversation `UTransBuffer` (256 MB default), opens "Tool Transaction", then ends it and restores the global buffer.
- It registers a hidden nomad tab and a global **input preprocessor** that runs its command list: F1 runs the UI query, Alt+F1 summons the panel.
- There is no direct `ExecPythonCommand` call in the plugin.

### Answer: Output Log console (5.8)
- **Input box.** `SConsoleInputBox` is an `SMultiLineEditableTextBox` with:
  - `.AllowMultiLine(GetActiveCommandExecutorAllowMultiLine)` and `.ModiferKeyForNewLine(EModifierKey::Shift)`, so Shift+Enter adds a line;
  - `MaxDesiredWidth(600)`;
  - typed tabs allowed.
- **Executors.** Both Python executors (`"Python"`, which uses ExecuteFile semantics, and `"PythonREPL"`, display name "Python (REPL)", which uses `ExecuteStatement`) return `AllowMultiLine() == true`.
- **History is per executor**, under `IConsoleManager` keys `"Python"` and `"PythonREPL"`:
  - stored in `FPaths::GeneratedConfigDir()/ConsoleHistory.ini` (`<Project>/Saved/Config/<Platform>/`), in sections `[ConsoleHistory<Key>]` with `History=` entries;
  - duplicates are removed, and the list is trimmed to about 64 entries;
  - **the whole file is rewritten on every entry**.
  - The history popup appears on Up only when the box is empty, or with Ctrl+Up.

### Answer: "Recent Python Scripts"
- Implemented by `FPythonCommandMenuImpl` in `PythonScriptPlugin.cpp`.
- It extends `LevelEditor.MainMenu.Tools`, section `"Python"`, with "Execute Python Script…" and the "Recent Python Scripts" submenu.
- It stores `[Python] RecentsFiles=` (up to 10) and `LastDirectory` in `GEditorPerProjectIni`.
- Entries run through `GEngine->Exec("py \"<file>\"")`.
- A file is added only when `bIsFullyInitialized`, so startup scripts are excluded. That is the 5.8 fix. A file that fails to load is removed.
- Minor bug: the "Clear Recent Python Scripts" entry is added to the "Files" section, while the "Clear" section it was meant for stays empty.

### Evidence
- `Plugins/Experimental/ModelContextProtocol/ModelContextProtocol.uplugin`.
- `Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocolEngine/Public/ModelContextProtocolSettings.h:17, 24-46`.
- `Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocol/Private/ModelContextProtocolServer.cpp:69-118` (Origin), `:432-447` (routes, `StartAllListeners`), `:862-962` (tool dispatch and the game-thread hop; `checkf(IsInGameThread()…)` at `:865`).
- `Source/Runtime/Online/HTTPServer/Private/HttpServerConfig.h:13` (`BindAddress = "localhost"`), `Source/Runtime/Online/HTTPServer/Public/HttpServerModule.h:24-26` (`FTSTickerObjectBase`), `Source/Runtime/Online/HTTPServer/Private/HttpServerModule.cpp:182-193`.
- `Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocol/Public/IModelContextProtocolTool.h:30-34, 74, 91, 97`.
- `Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocolEditor/Private/ModelContextProtocolToolsetRegistryAdapter.cpp:36-66, 177, 258`.
- `Plugins/Experimental/ToolsetRegistry/ToolsetRegistry.uplugin`.
- `Plugins/Experimental/ToolsetRegistry/Content/Python/toolset_registry/tests/demo_toolset.py:41-57`, `…/toolset_registry/registration.py:11-41`, `…/toolset_registry/_asyncio/editor_thread_ticker.py:179`.
- `Plugins/Experimental/Toolsets/EditorToolset/Content/Python/init_unreal.py`, `…/editor_toolset/toolsets/programmatic.py:81, 366-386, 708, 906-946`.
- `Plugins/Experimental/ToolsetRegistry/Source/ToolsetRegistry/Private/ToolsetRegistry/FunctionLibraryToolset.cpp:161, 172`, `…/RunOnMainThread.h`, `…/ObjectFunctionToolCall.cpp:464`, `…/Toolset.cpp:32-47`, `…/ToolsetRegistry/PythonTestRunner.cpp:315`.
- `Plugins/Experimental/AIAssistant/AIAssistant.uplugin`; `…/Private/AIAssistantConfig.cpp:18`; `…/AIAssistantWebApplication.cpp:427-470`; `…/AIAssistantTransactionBufferManager.cpp:43, 172-201`; `…/AIAssistant.cpp:50-100`; `…/AIAssistantCommands.cpp:20-22`; `…/AIAssistantInputProcessor.cpp:34-45`.
- `Source/Developer/OutputLog/Private/SOutputLog.cpp:258-268, 466, 488-548, 580-617, 757-762`.
- `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPlugin.cpp:338-399` ("Python"), `:401-480` ("Python (REPL)", `ExecuteStatement`), `:484-687` (recent menu; config keys `:546-549`; `GEditorPerProjectIni` `:555, 575`; menu `:586-630`; exec `:637`), `:1920-1924` (startup exclusion).
- `Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:2632-2693, 3518-3542`.

**Confidence:** high for structure and threading. Medium for "no arbitrary-Python tool", which is based on grep across all Toolsets, ToolsetRegistry, AIAssistant and MCP sources.

### Implication for our design
- Integrate as an **optional** dependency on ToolsetRegistry:
  - ship a Python `ScriptEditorTools(unreal.ToolsetDefinition)` with tools like `open_buffer`, `get_buffer`, `propose_edit` (a diff the user accepts) and `run_buffer`;
  - gate `run_buffer` behind a per-session user opt-in, because MCP has no authentication and any local process can call it;
  - echo every agent call in our output.
- Our run pump must **not** tick `FTSTicker::GetCoreTicker()` during a user run. That prevents MCP and HTTP from re-entering mid-script.
- The Output Log console has no completion. An extra `IConsoleCommandExecutor` with real `GetSuggestedCompletions` (04 §1.7) is still a cheap win. Do not route large buffers through console history: about 64 entries, with the whole ini rewritten each time.
- Runs through `ExecPythonCommandEx(ExecuteFile)` with a real file path add that file to "Recent Python Scripts". Either accept this or use our own runner module.

---

## Q7. `UDeveloperSettings` config layering

### Answer
**Container and category** come from `ClassConfigName`:

| `config=` value | Container | Category |
|---|---|---|
| `EditorSettings` or `EditorPerProjectUserSettings` | **"Editor"** (Editor Preferences) | "General" |
| `Engine`, `Input` | "Project" (Project Settings) | "Engine" |
| `Editor`, `EditorLayout`, `EditorKeyBindings` | "Project" | "Editor" |
| `Game` | "Project" | "Game" |

Any class can override the category with `CategoryName`, for example "Plugins". The SettingsEditor module auto-registers every non-abstract `UDeveloperSettings` CDO.

**Save behaviour (`FSettingsSection::Save`):**
- `defaultconfig`: `TryUpdateDefaultConfigFile()` writes `<Project>/Config/Default<Config>.ini`. **It returns false and only logs a warning if the file is read-only.**
- `globaluserconfig`: `UpdateGlobalUserConfigFile()` writes `<UserSettingsDir>/Unreal Engine/Engine/Config/User<Config>.ini`. This is cross-project and cross-version.
- `projectuserconfig`: writes `<Project>/Config/User<Config>.ini`.
- Otherwise: `SaveConfig()` writes the generated ini.

"Reset to defaults" and "Set as default" are offered only for classes without default, global-user or project-user config.

**Where the inis live:**
- `EditorPerProjectUserSettings`: `<Project>/Saved/Config/<Platform>/` (per user, per project).
- `EditorSettings`, `EditorKeyBindings`, `EditorLayout`: the project-agnostic engine Saved/Config dir (per user, per engine version).

**Epic's own precedents:**
- `UPythonScriptPluginSettings` uses `config=Engine, defaultconfig`, shared through `DefaultEngine.ini`.
- `UPythonScriptPluginUserSettings` uses `config=EditorPerProjectUserSettings`.
- MCP settings also use `EditorPerProjectUserSettings`.

### Evidence
- `Source/Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:22-75`; `Source/Runtime/DeveloperSettings/Private/Engine/DeveloperSettings.cpp:18-34, 36-66`.
- `Source/Developer/Settings/Private/SettingsSection.cpp:66-80, 257-290, 295-306`.
- `Source/Runtime/CoreUObject/Private/UObject/Obj.cpp:3958-3997, 4000-4035, 4037-4054`.
- `Source/Runtime/Core/Private/Misc/ConfigCacheIni.cpp:6640-6654`.
- `Source/Developer/SettingsEditor/Private/SettingsEditorModule.cpp:334-360`.
- `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PythonScriptPluginSettings.h:48, 152`; `Plugins/Experimental/ModelContextProtocol/Source/ModelContextProtocolEngine/Public/ModelContextProtocolSettings.h:17`.

**Confidence:** high.

### Implication for our design
Three settings classes:
- **`UScriptEditorProjectSettings`** (`config=Editor, defaultconfig`, shown in Project Settings > Plugins): studio-shared defaults such as lint rules, the snippet pack path and extra `sys.path` entries. On save, check out `DefaultEditor.ini` first when source control is enabled, and show the result. Otherwise the save is silently lost under Perforce.
- **`UScriptEditorUserSettings`** (`config=EditorPerProjectUserSettings`): session, theme overrides and per-project layout.
- **`UScriptEditorGlobalUserSettings`** (`config=EditorSettings`, or `globaluserconfig`): font, theme and keymap preset that follow the user across projects.

Actual chord rebinds should use Epic's own mechanism, `EditorKeyBindings.ini`, through `TCommands`. Do not store them in our settings.

---

## Corrections to research

| File | Claim | Correction |
|---|---|---|
| 05-architecture-and-robustness.md | Open question 5: "`ScopedEditorTransaction.cancel()`: does it revert changes or only stop recording?" (L597). The run wrapper at L394 assumes one Ctrl+Z undoes a run. | **Only stops recording and discards the record.** `UTransBuffer::Cancel` pops the transaction and never applies it, so changes stay and become un-undoable (`EditorTransaction.cpp:1411-1462`). A nested `cancel()` cancels the **outer** wrapper entirely (`:1419-1424`), and map loads purge it (`:1382-1394`). Also, a Ctrl+Z pressed in our focused editor is our text undo; editor undo needs an explicit "Undo last run" command (`GEditor->UndoTransaction()`). |
| 05-architecture-and-robustness.md | "We are the focused leaf: we handle `OnKeyDown` and return `Handled` to stop bubbling into editor command lists" (L99). | True for bubbling, but **input preprocessors run first** (`SlateApplication.cpp:4982`). The AI Assistant takes F1 and Alt+F1 there (`AIAssistant.cpp:100`). We must also absorb printable and AltGr keys, Tab and arrows (or Slate turns them into navigation), and Ctrl+Z/Y, Ctrl+D, Delete, Ctrl+S and Ctrl+W. Otherwise they reach MainFrame `TRANSACTION UNDO`, Level Editor duplicate, delete and save, or major-tab close. |
| 05-architecture-and-robustness.md | Settings layering: per-user settings under `EditorPerProjectUserSettings`, "Global per-user keymaps and themes, to follow users across projects" (L495-498). | `EditorPerProjectUserSettings` is confirmed. "Follows users across projects" needs `config=EditorSettings` (engine-version-wide; `ConfigCacheIni.cpp:6649-6651`) or `globaluserconfig` (`Obj.cpp:3989-3992`). Chord rebinds already persist in `EditorKeyBindings.ini` (`InputBindingManager.cpp:149`). `defaultconfig` saves fail silently on read-only files (`Obj.cpp:4041-4053`). |
| 05-architecture-and-robustness.md | "Docks, splits and saves layout with `FGlobalTabmanager`" (L59). | Confirmed, but `EditorLayout.ini` is **project-agnostic** (engine Saved/Config; `ConfigCacheIni.cpp:6649-6654`). Per-project session state must be stored by us. |
| 06-user-voice-and-landscape.md | AI Assistant: "Answers questions and generates code as text. A vendor review says it cannot run editor operations… Python execution not confirmed" (L53). | In the 5.8 source, the AI Assistant **executes ToolsetRegistry tools**, which are editor operations (`AIAssistantWebApplication.cpp:427-470`). With a dev option, it uses an isolated per-conversation undo buffer (`AIAssistantTransactionBufferManager.cpp:172-201`). It does not call `ExecPythonCommand` directly. It also installs a global input preprocessor for F1 and Alt+F1. |
| 06-user-voice-and-landscape.md and 04-unreal-python-internals.md | MCP "Calls run **serially on the game thread**. No authentication; loopback" (06 L52; 04 L63 "127.0.0.1:8000/mcp"). | Mostly confirmed, with nuance. Requests are dispatched from the game-thread core ticker (`HttpServerModule.h:24-26`) and results are finalized on the game thread (`ModelContextProtocolServer.cpp:865, 949-962`). Async tools can be outstanding concurrently, so this is not a strict serial queue. Default bind is `"localhost"` (`HttpServerConfig.h:13`), there is an Origin-header check (`ModelContextProtocolServer.cpp:69-118`) and no authentication. Settings are per user (`EditorPerProjectUserSettings`); auto-start is off and tool-search mode is on by default. |
| 06-user-voice-and-landscape.md | "A separate 'Python (REPL)' executor… (**UNVERIFIED**…)" (L51). | **Confirmed.** It is `FPythonREPLCommandExecutor`, name `PythonREPL`, display name "Python (REPL)", using `ExecuteStatement` (`PythonScriptPlugin.cpp:401-480`). History is stored in `ConsoleHistory.ini` sections `[ConsoleHistoryPython]` and `[ConsoleHistoryPythonREPL]`, about 64 entries (`ConsoleManager.cpp:2643, 2691, 3526-3534`). Up shows history only when the box is empty, or with Ctrl+Up (`SOutputLog.cpp:526-528`). |
| 06-user-voice-and-landscape.md | Completion popup suggested on "Ctrl+Space and Ctrl+Tab (the Minimal Effort binding)" (L204). | Ctrl+Space is the **global** Content Browser drawer (`GlobalEditorCommonCommands.cpp:71`) and Ctrl+Tab is the **global** tab switcher (`:48`). We can shadow both only while focused. Shadowing Ctrl+Tab breaks a core UE navigation habit, so default to Ctrl+Space only. |
| 04-unreal-python-internals.md | §6.6: `FInteractiveProcess` / `FMonitoredProcess` are line-splitters, so use `CreatePipe`/`CreateProc`/`ReadPipeToArray`/`WritePipe` (L480-488). | Confirmed, with additions: `FMonitoredProcess` has **no stdin pipe** (`MonitoredProcess.cpp:116`). Both merge stderr into stdout (`WindowsPlatformProcess.cpp:511-518`), and `FProcessStartInfo` defaults `StdErr = StdOut` (`GenericPlatformProcess.h:208`). `WritePipe(FString)` appends `\n` (`WindowsPlatformProcess.cpp:2052`). **No `CreateProc` path uses a Job Object**; only `ExecProcess(..., true)` does (`:1076-1097`). |
| 03-ide-features-lsp-dap.md | "Spawn them with `FPlatformProcess::CreateProc` and pipes, then restart on crash" (L214). | Add: pass a **separate stderr pipe**, use binary pipe I/O, and assign our own kill-on-close Job Object. Otherwise servers leak after an editor crash and stderr corrupts framing. |
| 04-unreal-python-internals.md | §0: "Recent Python Scripts… 5.8 fix: the recent list no longer includes files run at startup" (L59). | Confirmed by the `bIsFullyInitialized` guard (`PythonScriptPlugin.cpp:1920-1924`). Storage is `[Python] RecentsFiles` (max 10) and `LastDirectory` in `EditorPerProjectUserSettings.ini` (`:546-584`). There is a remaining cosmetic bug: the Clear entry is added to the wrong section (`:601-602`). |

---

## New risks

1. **Fall-through chords cause destructive editor actions.** Unconsumed Ctrl+Z/Y become `TRANSACTION UNDO/REDO`. When docked in the Level Editor, unconsumed Ctrl+D, Delete or Backspace duplicate or delete selected actors, and Ctrl+S saves the current level. Unconsumed Ctrl+W closes the hosting major tab, such as an asset editor. Bare letters, `/`, End and Esc trigger Level Editor and mode-toolkit hotkeys. Mitigation: a "consume list" plus automated key-routing tests.
2. **Global input preprocessors (AI Assistant F1/Alt+F1, and others from CommonUI, VCam and Avalanche when enabled) take keys before our widget.** We cannot override them from the focused widget.
3. **MCP or agent re-entrancy during a run.** The HTTP server is a core-ticker object. If our cancellation pump ticks `FTSTicker`, agent tool calls can run inside a user script. MCP also has no authentication, so exposing our session as a toolset grants code execution to any local process.
4. **User `cancel()` on a nested `ScopedEditorTransaction` silently destroys our run's undo record**, and later edits in the run go unrecorded. A map load inside a run purges the buffer. A dangling `__enter__` without `__exit__` leaves a transaction open across runs.
5. **Orphaned language servers after a crash.** `CreateProc` children are not in a job. Because `CreateProc` cannot start the process suspended, a short race remains in which grandchildren can escape the job we assign.
6. **LSP stream corruption** from stderr merged into stdout by default, the `\n` added by `WritePipe(FString)`, or split UTF-8 in `ReadPipe(FString)`. A write blocked on a full pipe freezes the calling thread.
7. **Blocking source-control helpers.** `unreal.SourceControl.check_out_*` blocks the game thread. On a slow Perforce server, save-on-checkout freezes the editor.
8. **Silent settings-save failure.** `defaultconfig` saves to read-only `DefaultEditor.ini` under Perforce fail with only a log warning.
9. **Cross-project layout.** `EditorLayout.ini` is shared by every project on the same engine version. A project-specific docking expectation will not hold. Tabs whose spawner is missing at main-frame creation vanish silently.
10. **No watcher events while the game thread is blocked.** Files changed during a long run show up only after it ends. A watcher overflow arrives as `FCA_RescanRequired`.
11. **AltGr and IME.** On European layouts, AltGr characters arrive with Ctrl+Alt set. Treating them as Ctrl+Alt chords breaks typing of `@ { [ \`. Stock text handles this with `IsRightAltDown()` (`SlateEditableTextLayout.cpp:1280-1285`); our `SCodeView` must do the same.
12. **`execute_tool_script` is not a security boundary.** It is an in-process allowlist sandbox. If we offer an agent "run" tool, its trust model is ours to define; Epic's sandbox is no precedent for safety.
13. **Silent shortcut conflicts.** The keybinding conflict UI only checks within a context's parent and child chain. A parentless "ScriptEditor" context will never warn users about shadowing global chords, so we must document our shadowing ourselves.

---

## Files read

Repo research:
- `<repo>\docs\research\01..06` (grep plus targeted sections of 04, 05 and 06).

Engine (paths relative to `Engine/`):

**Python plugin**
- `Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Private/PyEditor.cpp`, `PyEditor.h`, `PythonScriptPlugin.cpp`, `PythonScriptPluginSettings.h`

**Transactions and Blueprint helpers**
- `Source/Editor/UnrealEd/Private/EditorServer.cpp`, `ScopedTransaction.cpp`, `EditorTransaction.cpp`, `EditorEngine.cpp`
- `Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h`, `EditorEngine.h`
- `Source/Runtime/Engine/Classes/Kismet/KismetSystemLibrary.h`, `Source/Runtime/Engine/Private/KismetSystemLibrary.cpp`

**Commands and key routing**
- `Source/Editor/UnrealEd/Private/Toolkits/GlobalEditorCommonCommands.cpp`, `Source/Editor/UnrealEd/Private/Kismet2/DebuggerCommands.cpp`, `Source/Editor/UnrealEd/Private/EditorViewportCommands.cpp`
- `Source/Runtime/Slate/Private/Framework/Commands/GenericCommands.cpp`, `UICommandList.cpp`, `InputBindingManager.cpp`, `Source/Runtime/Slate/Public/Framework/Commands/InputChord.h`
- `Source/Editor/MainFrame/Private/Frame/MainFrameActions.cpp`, `Source/Editor/MainFrame/Private/Menus/MainMenu.cpp`
- `Source/Editor/LevelEditor/Private/LevelEditorActions.cpp`, `LevelEditor.cpp`, `SLevelEditor.cpp`, `LevelViewportActions.cpp`, `SLevelViewport.cpp`, `LevelEditorContextMenu.cpp`, `LevelEditorMenu.cpp`
- `Source/Editor/ContentBrowser/Private/ContentBrowserCommands.cpp`
- `Source/Runtime/Slate/Private/Framework/Application/SlateApplication.cpp`, `NavigationConfig.cpp`, `Source/Runtime/SlateCore/Private/Widgets/SWidget.cpp`, `Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp`

**Docking, layout and menus**
- `Source/Runtime/Slate/Private/Framework/Docking/TabCommands.cpp`, `SDockingTabStack.cpp`, `TabManager.cpp`, `LayoutService.cpp`, `Source/Runtime/Slate/Public/Framework/Docking/TabManager.h`
- `Source/Editor/ContentBrowser/Private/AssetContextMenu.cpp`, `Menus/ContentBrowserMenus.cpp`, `SContentBrowser.cpp`, `Source/Editor/ContentBrowser/Public/ContentBrowserMenuContexts.h`
- `Source/Editor/LevelEditor/Public/LevelEditorMenuContext.h`, `Source/Developer/ToolMenus/Public/ToolMenus.h`, `Source/Editor/WorkspaceMenuStructure/Public/WorkspaceMenuStructure.h`

**Output Log and console history**
- `Source/Developer/OutputLog/Private/SOutputLog.cpp`, `Source/Runtime/Core/Private/HAL/ConsoleManager.cpp`

**Processes**
- `Source/Runtime/Core/Private/Windows/WindowsPlatformProcess.cpp`, `WindowsPlatformMisc.cpp`, `Source/Runtime/Core/Public/Windows/WindowsPlatformProcess.h`, `Source/Runtime/Core/Public/GenericPlatform/GenericPlatformProcess.h`
- `Source/Runtime/Core/Private/Misc/InteractiveProcess.cpp`, `MonitoredProcess.cpp`

**File watching and source control**
- `Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h`, `Private/Windows/DirectoryWatchRequestWindows.cpp`, `Private/Windows/DirectoryWatcherWindows.cpp`
- `Source/Developer/SourceControl/Public/ISourceControlModule.h`, `ISourceControlProvider.h`, `ISourceControlState.h`, `SourceControlHelpers.h`, `SourceControlOperations.h`, `SourceControlFileStatusMonitor.h`
- `Source/Editor/UnrealEd/Public/FileHelpers.h`

**Config, paths and startup order**
- `Source/Runtime/Core/Private/Misc/ConfigCacheIni.cpp`, `ConfigContext.cpp`, `Paths.cpp`
- `Source/Runtime/Launch/Private/Launch.cpp`, `LaunchEngineLoop.cpp`, `Source/Editor/UnrealEd/Private/UnrealEdGlobals.cpp`

**Settings**
- `Source/Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h`, `Private/Engine/DeveloperSettings.cpp`
- `Source/Developer/Settings/Private/SettingsSection.cpp`, `Source/Developer/SettingsEditor/Private/SettingsEditorModule.cpp`, `Source/Runtime/CoreUObject/Private/UObject/Obj.cpp`

**Unreal MCP and HTTP server**
- `Plugins/Experimental/ModelContextProtocol/ModelContextProtocol.uplugin`, `Source/ModelContextProtocol/Private/ModelContextProtocolServer.cpp`, `Source/ModelContextProtocol/Public/IModelContextProtocolTool.h`, `Source/ModelContextProtocolEngine/Public/ModelContextProtocolSettings.h`, `Source/ModelContextProtocolEditor/Private/ModelContextProtocolToolsetRegistryAdapter.cpp`
- `Source/Runtime/Online/HTTPServer/Private/HttpServerModule.cpp`, `HttpServerConfig.h`, `HttpListener.cpp`, `Public/HttpServerModule.h`

**ToolsetRegistry and toolsets**
- `Plugins/Experimental/ToolsetRegistry/ToolsetRegistry.uplugin`, `Content/Python/init_unreal.py`, `Content/Python/toolset_registry/registration.py`, `…/tests/demo_toolset.py`, `…/_asyncio/editor_thread_ticker.py`
- `Plugins/Experimental/ToolsetRegistry/Source/ToolsetRegistry/Private/ToolsetRegistry/{Toolset.cpp, ToolsetRegistry.cpp, FunctionLibraryToolset.cpp, ObjectFunctionToolCall.cpp, RunOnMainThread.h, ToolsetRegistrySubsystem.cpp, PythonTestRunner.cpp}`
- `Plugins/Experimental/Toolsets/EditorToolset/Content/Python/init_unreal.py`, `…/editor_toolset/toolsets/programmatic.py`, `…/editor_toolset/toolsets/*.py` (class list)

**AI Assistant**
- `Plugins/Experimental/AIAssistant/AIAssistant.uplugin`, `Source/AIAssistant/Private/{AIAssistant.cpp, AIAssistantCommands.cpp, AIAssistantInputProcessor.cpp, AIAssistantConfig.cpp, AIAssistantWebApplication.cpp, AIAssistantTransactionBufferManager.{h,cpp}, AIAssistantToolset.h}`

**Chord scan:** a Python scan of every `UI_COMMAND` statement under `Source/Editor`, `Source/Developer`, `Source/Runtime/{Slate,SlateCore,Engine}` and `Plugins`. Script: `%USERPROFILE%\AppData\Local\Temp\claude\C--Users-USER-Desktop-reactionchess\cef630df-881f-4396-849f-f12554ae0865\scratchpad\chords.py`. Output: `chords_out.txt` in the same folder.
