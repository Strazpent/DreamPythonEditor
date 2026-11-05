> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 07c. Verification: Slate text widgets, IME, fonts, CodeEditor plugin, CEF (UE 5.8 source)

Date: 2026-10-28. Method: read-only inspection of the local UE 5.8 install at `C:\Program Files\Epic Games\UE_5.8\Engine`. Every citation is `Engine/<relative path>:<line>`. No web sources were used. `04-unreal-python-internals.md` was not present when this pass ran.

## Summary

1. **The stock syntax-highlighting path is O(document) per edit, and the cost is worse than the research says.** At the end of every edit transaction, `FSlateEditableTextLayout` does five things:
   - serialises the whole document;
   - string-compares it with the old text;
   - snapshots the whole text into the undo stack (up to 99 levels);
   - when `RequiresLiveUpdate()` is true (always true for the syntax marshallers), calls `ForceRefreshTextLayout`. That clears every line, re-tokenises the whole string and rebuilds every `FLineModel`;
   - because every `FLineModel` is new and has a fresh `FShapedTextCache`, re-shapes every line.

   Layout is not virtualised. Paint is culled per line. There are two real incremental paths:
   - `FTextLayout::InsertAt`/`RemoveAt`, which dirty one line, when the marshaller does not need live update;
   - append-only `AddLines` on a clean layout, which is what the Output Log uses.
2. **IME.**
   - The reference implementation is `FSlateEditableTextLayout::FTextInputMethodContext`. `FCEFTextInputMethodContext` is a second, standalone one.
   - A custom leaf widget registers through `FSlateApplication::Get().GetTextInputMethodSystem()->RegisterContext()` and `ActivateContext()`.
   - Windows (IMM + TSF) and Mac (Cocoa `NSTextInputClient`) have implementations.
   - **Linux has no `ITextInputMethodSystem` at all.** SDL committed text arrives as `OnKeyChar`, and the preedit (`SDL_EVENT_TEXT_EDITING`) is dropped.
   - 5.8 contains new TSF fixes: a single `\n` line terminator in the ACP space, and minimal `NotifyTextChanged` ranges. Our context must copy both.
3. **CodeEditor plugin.**
   - It is a 0.1 beta, off by default, ~1.7k lines, one `Editor` module, and **all code is Private with no exported API**.
   - It is a thin `SMultiLineEditableText` subclass plus a C++-only highlighter and a project tree over `Source/`. It has no line numbers, find, completion, folding or LSP.
   - It cannot be linked or extended by a third-party plugin; it can only be studied.
4. **Better internal references** are the HLSL editors, which all derive from the exported `FHLSLSyntaxHighlighterMarshaller` in UnrealEd. The best widget reference is **`SOptimusShaderTextDocumentTextBox`** (Optimus/DeformerGraph), with its PCG twin `SPCGCodeEditorTextBox`: toggle comment, indent/dedent, auto-indent, brace pairing and a search bar. **`FPCGHLSLSyntaxHighlighter`** shows compiler-diagnostic underlines.
5. **CEF and the web browser.**
   - Shipped version: **CEF 128.4.13+ge76af7e, Chromium 128.0.6613.138** (Win64 uses a `+v2` rebuild). An **experimental CEF 147.1.5 / Chromium 147.0.7727.49** path exists but is switched off.
   - Rendering is off-screen. The default OSR frame rate is 24 fps. It rises to 60 fps only with accelerated paint, which requires D3D11/D3D12; it is never used on Linux and is disabled under automation.
   - Key handling is asynchronous: `OnKeyDown` always returns handled, and unhandled keys are re-injected into Slate later.
   - `SWebBrowser` auto-binds IME in 5.8; `SWebBrowserView` does not; CEF IME is compiled out on Linux.
   - The UMG `WebBrowserWidget` plugin is **off by default**. The `WebBrowser` engine module (with CEF) is always loaded in the editor, because MainFrame depends on it.
6. **Text painting.**
   - `FSlateDrawElement::MakeShapedText` and `MakeText` are public `SLATECORE_API` functions.
   - `FSlateFontCache::Shape*Text` does **not** cache. Caching lives in Slate's per-layout `FShapedTextCache`, and `FSlateFontMeasure` keeps a 500-entry LRU per font.
   - **The only monospace font file shipped is `Engine/Content/Slate/Fonts/DroidSansMono.ttf`**. It has one face only: no bold or italic. CJK falls back to `DroidSansFallback.ttf`.
7. **Accessibility.**
   - It is off by default (`Accessibility.Enable` = false).
   - `IAccessibleText` has one method (`GetText`) and is marked "currently incomplete".
   - **`SMultiLineEditableText`/`Box` expose no accessible text at all.** Only `SEditableText`, `SEditableTextBox` and `STextBlock` do.
   - The Windows UIA text-range provider returns `E_NOTIMPL` for word, line, Move and Select. Caret-level screen-reader reading of code is not achievable in 5.8 through Slate's bridge.

---

## Q1. SMultiLineEditableText + syntax marshallers: does every keystroke re-parse and re-set the whole document?

**Answer: confirmed. Every committed edit re-tokenises, rebuilds and re-shapes the whole document.**

The per-edit sequence:

1. `EndEditTransaction` calls `GetEditableText()`, which runs `Marshaller->GetText` over every line (O(doc)). It then compares the result with the pre-edit string (O(doc)) [Engine/Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp:3257-3260, 468-472].
2. It saves the text and pushes an undo state whose `FUndoState` holds the **entire `FText`** [Engine/Source/Runtime/Slate/Public/Widgets/Text/SlateEditableTextTypes.h:102-105]. The stack holds up to `MaxUndoLevels = 99` [Engine/Source/Runtime/Slate/Private/Framework/Text/TextEditHelper.h:15; SlateEditableTextLayout.cpp:3321-3341].
3. On Windows/TSF, when not composing, it rebuilds the full `\n`-space text and diffs it to compute a minimal range (O(doc)) [SlateEditableTextLayout.cpp:3273-3291].
4. Then the live-update step runs [SlateEditableTextLayout.cpp:3309-3314]:
   ```cpp
   if (Marshaller->RequiresLiveUpdate())
   {
       ForceRefreshTextLayout(EditedText);
   }
   ```
5. `ForceRefreshTextLayout` calls `SetEditableText(CurrentText, true)` [SlateEditableTextLayout.cpp:681-695]. That runs `TextLayout->ClearLines()`, `ClearLineHighlights()` and `ClearRunRenderers()`, then `Marshaller->SetText(TextToSetString, *TextLayout)` [SlateEditableTextLayout.cpp:423-428].
6. `FSyntaxHighlighterTextLayoutMarshaller::SetText` tokenises the whole `SourceString` and calls `ParseTokens` [Engine/Source/Runtime/Slate/Private/Framework/Text/SyntaxHighlighterTextLayoutMarshaller.cpp:18-30].
   - `RequiresLiveUpdate()` returns `bSyntaxHighlightingEnabled`, which defaults to true [same file:32-35, 50].
   - `ParseTokens` takes `TArray<FTokenizedLine>` **by value**, so the whole token array is deep-copied per edit [Engine/Source/Runtime/Slate/Public/Framework/Text/SyntaxHighlighterTextLayoutMarshaller.h:36].
   - It ends with `TargetTextLayout.AddLines(LinesToAdd)` for all lines [SyntaxHighlighterTextLayoutMarshaller.cpp:82-83, 213-214].
7. `FSyntaxTokenizer::Process` computes line ranges for the whole input. It sets an ICU word-break iterator on the whole string, then tries every rule with `Strncmp` at every offset (O(chars × rules)) [Engine/Source/Runtime/Slate/Private/Framework/Text/SyntaxTokenizer.cpp:16-28, 37-38, 52-81].
8. Each new `FLineModel` builds a new `FShapedTextCache` with `DirtyFlags(ELineModelDirtyState::All)` [Engine/Source/Runtime/Slate/Private/Framework/Text/TextLayout.cpp:2801-2809]. After `ClearLines()` (which dirties the layout, :2337-2342), `UpdateLayout` → `FlowLayout` iterates **all** line models, flushes each shaping cache and re-flows [TextLayout.cpp:1270-1285, 552-564, 1202-1211]. **Every line is re-shaped on every edit.**
9. The interface documents this cost itself [Engine/Source/Runtime/Slate/Public/Framework/Text/ITextLayoutMarshaller.h:28-31]: "Returning true will cause SetText to be called every time the source text is changed, which is costly".

**Rendering is partly virtualised.** `FSlateTextLayout::OnPaint` skips lines outside the culling rect [Engine/Source/Runtime/Slate/Private/Framework/Text/SlateTextLayout.cpp:85-93, 109-119]. Layout and shaping are not virtualised.

**Incremental paths that do exist:**

- **Plain edits without live update.** `FTextLayout::InsertAt`, `RemoveAt`, `SplitLineAt` and `JoinLineWithNextLine` are public `SLATE_API` [Engine/Source/Runtime/Slate/Public/Framework/Text/TextLayout.h:537-549] and dirty only the touched line model (e.g. TextLayout.cpp:1685, 1750). Other lines keep their shaping cache. The re-flow still walks all lines, but only cheaply.
- **Append-only.** `AddLines` on a clean layout lays out only the new lines, "immediately (and efficiently)" [TextLayout.cpp:2279-2333]. `FOutputLogTextLayoutMarshaller` keeps the `FTextLayout*` it gets in `SetText` and appends pending messages with `AddLines` [Engine/Source/Developer/OutputLog/Private/SOutputLog.cpp:937-942, 997-1141].
- **Extension points.**
  - `SMultiLineEditableText` accepts a `CreateSlateTextLayout` factory [Engine/Source/Runtime/Slate/Public/Widgets/Text/SMultiLineEditableText.h:222], and `FSlateTextLayout::OnPaint` is virtual [Engine/Source/Runtime/Slate/Public/Framework/Text/SlateTextLayout.h:28].
  - A marshaller with `RequiresLiveUpdate()==false` could keep the layout pointer (Output Log pattern) and restyle changed lines. This is unsupported, though: no "replace runs of line N" API exists, so it would mean `RemoveAt` plus `InsertAt(run)` behind the editable layout's back.
- **Reusing the editable layout directly.** `FSlateEditableTextLayout` (cursor, selection, undo, IME) can be hosted by any widget that implements `ISlateEditableTextWidget`. Its constructor is a template and `Construct` is `SLATE_API` [Engine/Source/Runtime/Slate/Public/Widgets/Text/SlateEditableTextLayout.h:37, 100-112].

**Confidence:** high (read directly from source).

**Implication:**
- Our A2 decision (a custom line-virtualised view with incremental tokenising) is justified by the source, not only by the API docs.
- Use stock `SMultiLineEditableText` + marshaller only for the REPL input and small text, as 05 already says.
- For the Output/console view, copy the Output Log's append-only `AddLines` pattern.
- Do not hold full-text undo snapshots in our own model: use a piece table with operation-based undo.

---

## Q2. IME: who implements the context, how a custom leaf registers, platform caveats

**Answer.**

**Interfaces.** Three are declared in Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/ITextInputMethodSystem.h:
- `ITextInputMethodContext`, with 14 pure virtuals: `IsComposing`, `IsReadOnly`, `GetTextLength`, `Get/SetSelectionRange`, `Get/SetTextInRange`, `GetCharacterIndexFromPoint`, `GetTextBounds`, `GetScreenBounds`, `GetWindow`, `BeginComposition`, `UpdateCompositionRange` and `EndComposition` [ITextInputMethodSystem.h:14-139];
- `ITextInputMethodChangeNotifier`: `NotifyLayoutChanged`, `NotifySelectionChanged`, `NotifyTextChanged`, `CancelComposition` [ITextInputMethodSystem.h:146-163];
- `ITextInputMethodSystem`: `RegisterContext` → notifier, plus `UnregisterContext`, `ActivateContext`, `DeactivateContext` and `IsActiveContext` [ITextInputMethodSystem.h:168-218, 187, 195, 202, 209].

**Reference implementations.**
1. `FSlateEditableTextLayout::FTextInputMethodContext`, a private nested class [Engine/Source/Runtime/Slate/Public/Widgets/Text/SlateEditableTextLayout.h:592-640; methods at SlateEditableTextLayout.cpp:3996-4300]. `SEditableText` and `SMultiLineEditableText` use it through `FSlateEditableTextLayout`.
2. `FCEFTextInputMethodContext` [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFTextInputMethodContext.h:42], with `FCEFImeHandler`. This is the closer model for a custom leaf, because it is not tied to `FTextLayout`.

**Registration sequence, as Epic does it** [SlateEditableTextLayout.cpp:609-635]:
1. `ITextInputMethodSystem* IMS = FSlateApplication::Get().GetTextInputMethodSystem()` [Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:362]. This can be null.
2. On first focus, `Notifier = IMS->RegisterContext(Ctx)` then `Notifier->NotifyLayoutChanged(Created)`.
3. Cache the native window, call `NativeWindow->SetWindowFocus()`, then `IMS->ActivateContext(Ctx)`. This is called from focus-received [SlateEditableTextLayout.cpp:893].
4. On focus lost: if `IsComposing()`, call `Notifier->CancelComposition()`, then `DeactivateContext` [SlateEditableTextLayout.cpp:945-953].
5. On destruction: deactivate, then `UnregisterContext` [SlateEditableTextLayout.cpp:249-263].
6. On geometry change during Tick: `NotifyLayoutChanged(Changed)` [SlateEditableTextLayout.cpp:3532-3534].
7. After non-IME edits: `NotifyTextChanged(begin, oldLen, newLen)` [SlateEditableTextLayout.cpp:3291]. Epic's code never calls `NotifySelectionChanged` (grep shows no call site).
8. The composition underline is drawn by the widget itself, through a line highlighter [SlateEditableTextLayout.cpp:179, 2797].

**New in 5.8 (must copy).**
- All TSF ACP indices use a single-character `\n` terminator. A `\r\n` terminator desyncs the Korean IME [SlateEditableTextLayout.cpp:21-28].
- Change notifications send only the minimal changed span, because TSF misbehaves on full-document reports [SlateEditableTextLayout.cpp:30-60, 3265-3291].

**Cost note.** Epic's `GetTextLength` and `GetTextInRange` materialise the whole document on every IME query [SlateEditableTextLayout.cpp:4019-4029, 4119-4131]. Our context should answer from the text model directly.

**Platform caveats in code.**
- **Default:** `GenericApplication::GetTextInputMethodSystem()` returns `NULL` [Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericApplication.h:550]. Only Windows [WindowsApplication.h:390] and Mac [MacApplication.h:198] override it.
- **Windows:** `FWindowsTextInputMethodSystem` implements both IMM and TSF [Engine/Source/Runtime/ApplicationCore/Public/Windows/WindowsTextInputMethodSystem.h:62-121]. Slate draws the IMM composition string itself only when the IME has no special UI and draws at the caret [Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsTextInputMethodSystem.cpp:388-392]. `GetTextBounds` must therefore be accurate so the candidate window lands correctly.
- **Mac:** `ActivateContext` requires `Context->GetWindow()` to resolve to an `FCocoaWindow` whose GL view is an `FCocoaTextView`. It hops to the main thread with `MainThreadReturn` [Engine/Source/Runtime/ApplicationCore/Private/Mac/MacTextInputMethodSystem.cpp:155-190]. Cocoa `NSTextInputClient` callbacks then call our context on the game thread through blocking `GameThreadCall` [Engine/Source/Runtime/ApplicationCore/Private/Mac/CocoaTextView.cpp:253-275].
- **Linux:** there is no IME system.
  - `SDL_EVENT_TEXT_INPUT` is forwarded character by character to `MessageHandler->OnKeyChar` [Engine/Source/Runtime/ApplicationCore/Private/Linux/LinuxApplication.cpp:391-400].
  - `SDL_EVENT_TEXT_EDITING` (the preedit) is only used to look up the window ID [LinuxApplication.cpp:1433-1437].
  - `SDL_StartTextInput` is enabled per window [Engine/Source/Runtime/ApplicationCore/Private/Linux/LinuxWindow.cpp:428-433].
  - No `SDL_SetTextInputArea` call exists anywhere in the runtime, so the candidate window cannot follow the caret.
  - CEF IME is also compiled out on Linux [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFImeHandler.h:7; CEFWebBrowserWindow.cpp:3026-3038].

**Confidence:** high.

**Implication:**
- Implement our own `ITextInputMethodContext` in sprint 1, modelled on Epic's but with O(1)/O(line) queries.
- Use the `\n` ACP space and minimal-range notifications.
- Always keep an `OnKeyChar` path that accepts multi-character commits. It is the only path on Linux, and the fallback when `GetTextInputMethodSystem()` is null.
- Document that Linux has no inline preedit. That is an engine limit, not ours.
- On Mac, never block the game thread while focused: IME callbacks wait on it.

---

## Q3. Epic's experimental CodeEditor plugin

**Answer: a small 0.1 experimental C++ source viewer/editor. It is not reusable through linking.**

**Descriptor** [Engine/Plugins/Experimental/CodeEditor/CodeEditor.uplugin]:
- `"VersionName" : "0.1"`, `"EnabledByDefault" : false`, `"IsBetaVersion" : true`;
- one module, `CodeEditor`, of type `Editor`, loading phase `Default`;
- prebuilt `Binaries/Win64/UnrealEditor-CodeEditor.dll` is present.

**Build and exports.**
- `CodeEditor.Build.cs` has only `PrivateDependencyModuleNames` [Engine/Plugins/Experimental/CodeEditor/Source/CodeEditor/CodeEditor.Build.cs:9-26].
- There is **no `Public/` folder and no `CODEEDITOR_API`** (grep finds none).
- 17 `.cpp` files, 1,666 lines (2,206 including headers).

**Widgets and classes:**
- **`SCodeEditableText : public SMultiLineEditableText`** [Private/SCodeEditableText.h:11]. Its only additions are a fixed style, no wrap, and `Tab` inserting `\t` [Private/SCodeEditableText.cpp:7-48].
- **`SCodeEditor`** [Private/SCodeEditor.cpp]:
  - loads the file synchronously with `FFileHelper::LoadFileToString` (:22);
  - uses the C++ marshaller (:24) plus scroll bars;
  - keeps a dirty flag (:67-70), `Save` (:72-85) and `GotoLineAndColumn` (:92-97).
- **`FCPPRichTextSyntaxHighlighterTextLayoutMarshaller`**:
  - a C++ keyword list [Private/CPPRichTextSyntaxHighlighterTextLayoutMarshaller.cpp:8-93] and operator list (:96+);
  - a parse-state machine for strings, chars, `//`, `/* */` and `#` preprocessor, carried across lines (:198-294);
  - it inherits the full-document live update (Q1).
- **`FWhiteSpaceTextRun`**: a custom run that measures tabs as N spaces [Private/WhiteSpaceTextRun.cpp:13-48].
- **Project tree.**
  - `UCodeProject` roots at `FPaths::GameSourceDir()` [Private/CodeProject.cpp:7-11].
  - `UCodeProjectItem` children are found by `FDirectoryScanner` on the queued thread pool, with no extension filter [Private/DirectoryScanner.cpp:21-37; Private/CodeProjectItem.cpp:13-60].
  - The tree is shown in an `STreeView` [Private/SCodeProjectEditor.cpp:30].
  - `FCodeProjectEditor` is a `FWorkflowCentricApplication` with a `FDocumentTracker` tab per file [Private/CodeProjectEditor.cpp:209, 246-250, 287-290].
- **Entry points:** a "Tools → Edit Source Code" menu entry and a global tab spawner [Private/CodeEditor.cpp:34-67].
- **Style:** `DEFAULT_FONT("Mono", 9)` [Private/CodeEditorStyle.cpp:59]. The bundled `Resources/Font/DroidSansMono.ttf` is never referenced in source.

**Languages:** C++ only. Every file under `Source/` opens with C++ highlighting.

**Completeness:** a minimal viewer/editor. It has no line numbers, gutter, find/replace, completion, folding, diagnostics, LSP, multi-caret or encoding/EOL handling. Its only undo is the stock 99-snapshot undo.

**Reuse:** we cannot link it. Nothing is exported, and its classes are private to the module. The only way to reuse it is to copy code, which falls under the Unreal EULA (see risks).

**Confidence:** high.

**Implication:** treat it as a sample of the "A1" pattern, not a foundation. It confirms Epic never built a scalable Slate code widget in-tree. Our plugin's tab and style-set names must not collide with `CodeEditor` / `CodeEditorStyle` if both plugins are enabled.

---

## Q4. Other code-like editors in the engine; best internal reference

**Answer.**

**The shared exported base** is `FHLSLSyntaxHighlighterMarshaller : FSyntaxHighlighterTextLayoutMarshaller` in UnrealEd:
- exported with `UNREALED_API` `Create`, `CreateTokenizer` and `ParseTokens`;
- a per-line hook, `ProcessTokenizedLine(..., LineNumber, ..., EParseState&)` [Engine/Source/Editor/UnrealEd/Public/Text/HLSLSyntaxHighlighterMarshaller.h:17, 54, 67-73];
- a custom `ISyntaxTokenizer`, `FHlslSyntaxTokenizer` [Engine/Source/Editor/UnrealEd/Public/Text/HLSLSyntaxTokenizer.h:10-29].

**Users of that base:**

| Editor | Evidence |
|---|---|
| Material **Custom node** HLSL box | `FHLSLSyntaxHighlighterMarshaller::Create(CodeStyle)` → `SMultiLineEditableTextBox.Marshaller(...)` [Engine/Source/Editor/MaterialEditor/Private/MaterialNodes/SGraphNodeMaterialCustom.cpp:31, 49-55] |
| Material Stats shader view | [Engine/Source/Editor/MaterialEditor/Private/MaterialStats.cpp:1321] |
| Niagara Custom HLSL node, generated-code view, stack inputs | `FNiagaraHLSLSyntaxHighlighter : FHLSLSyntaxHighlighterMarshaller` [Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/Widgets/NiagaraHLSLSyntaxHighlighter.h:10]; used at SNiagaraGraphNodeCustomHlsl.cpp:22, 108-112, SNiagaraGeneratedCodeView.cpp:37 |
| Optimus (Deformer Graph) shader text editor | `FOptimusHLSLSyntaxHighlighter` [Engine/Plugins/Animation/DeformerGraph/Source/OptimusEditor/Private/OptimusHLSLSyntaxHighlighter.h:16]; widgets `SOptimusShaderTextEditor`, `SOptimusShaderTextDocumentTab`, `SOptimusShaderTextDocumentTextBox`, `SOptimusShaderTextSearchWidget` |
| PCG compute HLSL editor | `FPCGHLSLSyntaxHighlighter : FHLSLSyntaxHighlighterMarshaller, FTickableEditorObject` with `SetCompilerMessages()` [Engine/Plugins/PCG/Source/PCGEditor/Private/PCGHLSLSyntaxHighlighter.h:20-42]; widgets `SPCGCodeEditor`, `SPCGCodeEditorTextBox`, `SPCGShaderTextSearchWidget` |
| ComputeFramework graph HLSL | [Engine/Plugins/Runtime/ComputeFramework/Source/EditableComputeGraphEditor/Private/ComputeGraphHlslSyntaxHighlighter.cpp:10] |

**Other `ISyntaxTokenizer` users:**
- `TemplateStringSyntaxHighlighterMarshaller` [Engine/Source/Editor/EditorWidgets/Private];
- `NamingTokensStringSyntaxHighlighterMarshaller` [Engine/Plugins/Developer/NamingTokens];
- the Slate test suite's rich-text demo [Engine/Source/Runtime/AppFramework/Private/Widgets/Testing/STestSuite.cpp].
- **No marshaller in the engine overrides `RequiresLiveUpdate`.** Grep finds only the base classes, so every highlighter shares the Q1 cost.
- No Python highlighter exists in the engine.

**Best internal reference for a code-editor widget: `SOptimusShaderTextDocumentTextBox`** [Engine/Plugins/Animation/DeformerGraph/Source/OptimusEditor/Private/Widgets/SOptimusShaderTextDocumentTextBox.cpp]:
- It wraps `SMultiLineEditableText` with `OnKeyCharHandler`/`OnKeyDownHandler`. It overrides `OnIsTypedCharValid` so Tab is not consumed as navigation, sets `ClearTextSelectionOnFocusLoss(false)` and extends the context menu (:109-125).
- It registers a command list (:174) and handles `OnPreviewKeyDown` (:200).
- Search bar: show, hide, next and previous, through `SSearchBox` result data (:230-326).
- Toggle comment inside `FScopedEditableTextTransaction` (:345-445).
- Brace-pair delete (:476-500), Tab/Shift+Tab indent and dedent to 4 spaces (:507-585), and auto-indent on Enter (:589-700).
- **`SPCGCodeEditorTextBox`** (767 lines) is a near-copy with a `Ctrl+/` ToggleComment UI command [Engine/Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:75, 138, 188].
- **`FPCGHLSLSyntaxHighlighter::ProcessTokenizedLine`** is the reference for squiggle-style diagnostic underlines, built with `FSlateTextUnderlineLineHighlighter` and column ranges [Engine/Plugins/PCG/Source/PCGEditor/Private/PCGHLSLSyntaxHighlighter.cpp:94-120].
- The editor's own code styles are `SyntaxHighlight.SourceCode.{Normal, Operator, Keyword, String, Number, Comment, PreProcessorKeyword, Error}`, built on `DEFAULT_FONT("Mono", 9)` [Engine/Source/Editor/EditorStyle/Private/StarshipStyle.cpp:8845-8875].
- None of these editors has a line-number gutter.

**Confidence:** high.

**Implication:**
- Copy the Optimus/PCG *behaviours* into our command layer: comment toggle, indent rules, brace pairing, search UX.
- Copy the PCG diagnostics-as-line-highlights *idea*.
- Do not inherit their text stack for buffers.
- Default our theme tokens to the `SyntaxHighlight.SourceCode.*` palette so we look native.

---

## Q5. WebBrowser / CEF in 5.8

**Answer.**

**Version.**
- `CEF3.build.cs`, Win64: `"128.4.13+ge76af7e+chromium-128.0.6613.138+v2"`. The experimental alternative is `"147.1.5+g68529a5+chromium-147.0.7727.49"`, gated by `bUseExperimentalVersion = false` [Engine/Source/ThirdParty/CEF3/CEF3.build.cs:16, 33].
- Mac pins 128.4.13 (:39); Linux and WinArm64 use 128 by default (:27, 45).
- Source folders: `Engine/Source/ThirdParty/CEF3/cef_binary_128.4.13+ge76af7e+chromium-128.0.6613.138+v2_windows64`, `..._linux64` and `..._windowsarm64`.
- The runtime is at `Engine/Binaries/ThirdParty/CEF3/Win64/128.4.13+ge76af7e+chromium-128.0.6613.138+v2/libcef.dll`, with helper `Engine/Binaries/Win64/EpicWebHelper.exe`.
- This install ships only the Win64 runtime.

**Enabled by default?**
- **The UMG `WebBrowserWidget` plugin: no** [Engine/Plugins/Runtime/WebBrowserWidget/WebBrowserWidget.uplugin:13]. The same holds for `WebBrowserNativeProxy`.
- **The `WebBrowser` runtime module: yes, in practice.**
  - It is an engine module, not a plugin, and MainFrame lists it as a private dependency [Engine/Source/Editor/MainFrame/MainFrame.Build.cs:18, 50]. Its `StartupModule` loads the CEF libraries [Engine/Source/Runtime/WebBrowser/Private/WebBrowserModule.cpp:60-67].
  - The browser singleton and subprocesses start lazily on `GetSingleton()` [WebBrowserModule.cpp:126-133].
  - `bCompileCEF3` defaults to true [Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1087-1088]. It is disabled only for null-RHI Linux builds [Engine/Source/Programs/UnrealBuildTool/Platform/Linux/UEBuildLinux.cs:321-324].
- The AIAssistant experimental plugin depends on `WebBrowser` directly, without the UMG plugin, and states this in a comment [Engine/Plugins/Experimental/AIAssistant/Source/AIAssistant/Private/AIAssistantWebBrowser.cpp:67-86]. It is the best in-tree example of "editor tab hosting `SWebBrowser` with a JS bridge".

**JS bindings.**
- `IWebBrowserWindow::ExecuteJavascript(const FString&)` [Engine/Source/Runtime/WebBrowser/Public/IWebBrowserWindow.h:347].
- `BindUObject(Name, Object, bIsPermanent)` exposes the object as `window.ue.{Name}`. Its doc comment says return values are "wrapped into JS Future objects", because the call is asynchronous [IWebBrowserWindow.h:357-366].
- `UnbindUObject` [IWebBrowserWindow.h:375]. Both are mirrored on `SWebBrowser` [Engine/Source/Runtime/WebBrowser/Public/SWebBrowser.h:199, 219, 228] and `SWebBrowserView` [Engine/Source/Runtime/WebBrowser/Public/SWebBrowserView.h:246, 266, 275].
- A UFUNCTION parameter of type `FWebJSResponse` acts as a promise [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFJSScripting.cpp:254-307].

**Custom schemes.**
- `IWebBrowserSingleton::RegisterSchemeHandlerFactory(Scheme, Domain, Factory)`. Its doc comment: "The domain is ignored if the scheme is not a browser built in scheme" [Engine/Source/Runtime/WebBrowser/Public/IWebBrowserSingleton.h:128-141].
- Handler interface: `ProcessRequest`, `GetResponseHeaders`, `ReadResponse` and `Cancel` [Engine/Source/Runtime/WebBrowser/Public/IWebBrowserSchemeHandler.h:10-101].
- Implemented with `CefRegisterSchemeHandlerFactory` [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFSchemeHandler.cpp:171-176].
- **No `OnRegisterCustomSchemes`/`AddCustomScheme` exists anywhere in WebBrowser** (grep). A non-built-in scheme is therefore not registered as standard or secure. Serve assets through `https://<our-domain>/` with a domain-filtered factory.

**Off-screen rendering.**
- `windowless_rendering_enabled = true` [Engine/Source/Runtime/WebBrowser/Private/WebBrowserSingleton.cpp:348].
- When no OS window handle is passed: `SetAsWindowless` plus `shared_texture_enabled` when accelerated paint is available. The frame rate is raised from 24 to 60 only in that case [WebBrowserSingleton.cpp:686-698].
- The default `BrowserFrameRate = 24` [IWebBrowserSingleton.h:62; SWebBrowser.h:39].
- Accelerated paint is false on Linux and with `-nocefaccelpaint` [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFWebBrowserWindow.cpp:2696-2708]. On Windows it requires a D3D11 or D3D12 RHI and is off under automation or on build machines [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFWebBrowserWindowRHIHelper.cpp:95-103].
- DPI: `device_scale_factor` is taken from the parent window [Engine/Source/Runtime/WebBrowser/Private/CEF/CEFBrowserHandler.cpp:368-374].

**Key routing.**
- `FCEFWebBrowserWindow::OnKeyDown` forwards the key to CEF and always returns `true` [CEFWebBrowserWindow.cpp:1212-1228].
- Unhandled keys come back later and are re-injected with `FSlateApplication::Get().ProcessKeyDownEvent`. Epic's comment calls this "an ugly hack" [CEFWebBrowserWindow.cpp:1302-1327].
- `OnUnhandledKeyDown/Up/Char` delegates exist [IWebBrowserWindow.h:492-501].
- `SWebBrowserView::SetBrowserKeyboardFocus` exists [SWebBrowserView.h:302].

**IME.**
- `BindInputMethodSystem` is a no-op by default on `IWebBrowserWindow` [IWebBrowserWindow.h:377-379]. It is implemented by `FCEFImeHandler` and `FCEFTextInputMethodContext`, except on Linux [CEFImeHandler.h:7; CEFWebBrowserWindow.cpp:3026-3038].
- **`SWebBrowser::Construct` auto-binds** the Slate IME system, with the comment "Make the IME ... work with HTML text input boxes" [Engine/Source/Runtime/WebBrowser/Private/SWebBrowser.cpp:185-193]. The UMG `UWebBrowser` builds an `SWebBrowser` [Engine/Plugins/Runtime/WebBrowserWidget/Source/WebBrowserWidget/Private/WebBrowser.cpp:93], so it gets this too.
- `SWebBrowserView` alone does not auto-bind [SWebBrowserView.cpp:697].

**Accessibility:** WebBrowser has no CEF accessibility handler (grep finds none).

**Confidence:** high for version, bindings, OSR, keys and IME wiring. Real-world IME quality is not testable from source.

**Implication:**
- If Monaco or CM6 is ever spiked, it would have to:
  - use `SWebBrowser` (for the IME auto-bind) or call `BindInputMethodSystem` explicitly;
  - set `BrowserFrameRate=60` explicitly, because Vulkan and Linux users would otherwise get 24 fps echo;
  - serve assets from `https://` with a domain handler;
  - keep editor chords out of `preventDefault` so the re-injection path fires.
- Linux has no IME in CEF, and screen readers see nothing. This strengthens 05's decision not to use CEF for v1.

---

## Q6. Custom text painting, shaping cache, measure service, monospace fonts

**Answer.**

**Draw calls.**
- `FSlateDrawElement::MakeText` takes an `FString` with an optional range, plus `FSlateFontInfo` [Engine/Source/Runtime/SlateCore/Public/Rendering/DrawElementTypes.h:134-136].
- `MakeShapedText` takes a pre-shaped `FShapedGlyphSequenceRef`, a base tint and an outline tint, plus `FTextOverflowArgs` [DrawElementTypes.h:153].
- Slate's own runs paint through `MakeShapedText` with sub-sequences from the shaped cache [Engine/Source/Runtime/Slate/Private/Framework/Text/SlateTextRun.cpp:135, 178, 191].

**Shaping.**
- `FShapedGlyphSequence` [Engine/Source/Runtime/SlateCore/Public/Fonts/FontCache.h:333] provides `GetGlyphsToRender()` (:374) and `GetMeasuredWidth()`/range width (:419, 426).
- `FSlateFontCache::ShapeBidirectionalText`/`ShapeUnidirectionalText` are `SLATECORE_API` [FontCache.h:791-809]. They forward straight to `TextShaper` with **no caching** [Engine/Source/Runtime/SlateCore/Private/Fonts/FontCache.cpp:1229-1232].
- The glyph-atlas `FCharacterList` cache is separate [FontCache.h:666, 823, 1074].
- **Shaping cache:** Slate's `FShapedTextCache` (`SLATE_API`) is keyed by `FCachedShapedTextKey` (range, scale, context, font), stored in a `TMap` [Engine/Source/Runtime/Slate/Public/Framework/Text/ShapedTextCache.h:13, 54, 97-111, 137].
  - Helpers: `MeasureShapedText`, `FindCharacterIndexAtOffset` and `GetShapedTextSubSequence` [ShapedTextCache.h:155, 168, 195].
  - Each `FTextLayout` line owns one [TextLayout.cpp:2803].
- `ETextShapingMethod::Auto` uses KerningOnly for left-to-right text and FullShaping for right-to-left [FontCache.h:51-74].

**Measure service.**
- `FSlateFontMeasure` (from `FSlateApplication::Get().GetRenderer()->GetFontMeasureService()`) provides `Measure` (whole or range), `FindCharacterIndexAtOffset`, `GetMaxCharacterHeight` and `GetBaseline` [Engine/Source/Runtime/SlateCore/Public/Fonts/FontMeasure.h:13-194].
- It keeps an LRU string cache per font key [FontMeasure.h:10, 233-237] of `MeasureCacheSize = 500` entries [Engine/Source/Runtime/SlateCore/Private/Fonts/FontMeasure.cpp:12].
- `FWhiteSpaceTextRun` (CodeEditor) is a worked example of measuring tabs as spaces with it.

**Monospace fonts shipped.**
- In `Engine/Content/Slate/Fonts`: **`DroidSansMono.ttf`** (78,296 bytes) is the only monospace font. The other files are `DroidSansFallback.ttf`, `NotoNaskhArabicUI-Regular.ttf`, `NotoSansThai-Regular.ttf` and the Roboto family: `Roboto-Black.ttf`, `-BlackItalic`, `-Bold`, `-BoldCondensed`, `-BoldCondensedItalic`, `-BoldItalic`, `-Italic`, `-Light`, `-Medium` and `-Regular`.
- Duplicates and assets: `Engine/Plugins/Experimental/CodeEditor/Resources/Font/DroidSansMono.ttf`, and the UFont assets `Engine/Content/EngineFonts/DroidSansMono.uasset` and `EngineFonts/Faces/DroidSansMono.uasset`.
- `Engine/Content/Editor/Slate/Fonts` holds no monospace font (GenEi Gothic, Nanum Gothic, Noto, FontAwesome).
- The default composite font maps typeface `"Mono"` → `DroidSansMono.ttf`, with a **single face**. The fallback typeface is `DroidSansFallback.ttf` [Engine/Source/Runtime/SlateCore/Private/Fonts/LegacySlateFontInfoCache.cpp:122, 126-128].
- `DEFAULT_FONT(...)` is `FCoreStyle::GetDefaultFontStyle` [Engine/Source/Runtime/SlateCore/Public/Styling/SlateStyleMacros.h:21].
- Loading our own TTF from plugin resources: `FSlateFontInfo(TSharedPtr<const FCompositeFont>, Size, Typeface)` or the path constructor [Engine/Source/Runtime/SlateCore/Public/Fonts/SlateFontInfo.h:227, 246].

**Confidence:** high.

**Implication:**
- Our `SCodeView` should:
  - shape per line (or per token run) through `FSlateFontCache::ShapeUnidirectionalText` with KerningOnly for code;
  - keep our own LRU of `FShapedGlyphSequenceRef` keyed by (line hash, style, scale);
  - paint with `MakeShapedText`.
- Ship our own OFL mono family with real Bold and Italic faces (e.g. JetBrains Mono) as an `FCompositeFont`, with DroidSansFallback/Noto CJK as fallback. DroidSansMono has no bold or italic face for PyCharm-style keyword and comment styling.
- Compute column width from the measured advance of `' '`. Treat fallback CJK glyphs as measured widths, not grid cells.

---

## Q7. Accessibility for text

**Answer.**

**Interfaces.**
- `IAccessibleWidget` exposes `AsText()`, `AsProperty()` and similar. `IAccessibleText` is documented as "currently incomplete" and has only `GetText()` [Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/Accessibility/GenericAccessibleInterfaces.h:388-401, 472].
- Slate implementations: `FSlateAccessibleEditableText` and `FSlateAccessibleEditableTextBox`, both `IAccessibleText` + `IAccessibleProperty` [Engine/Source/Runtime/Slate/Public/Widgets/Accessibility/SlateAccessibleWidgets.h:54-107], and `FSlateAccessibleTextBlock` (:154).
- The overrides of `CreateAccessibleWidget` are in `SEditableText.cpp:779`, `SEditableTextBox.cpp:498`, `STextBlock.cpp:548`, `SButton`, `SCheckBox` and `SSlider` [Engine/Source/Runtime/Slate/Private/Widgets/...].
- **`SMultiLineEditableText` and `SMultiLineEditableTextBox` have no override** (grep), so they appear as generic widgets with no text.

**Is it on by default?** No.
- `bool GAllowAccessibility = false;` behind cvar `Accessibility.Enable` [Engine/Source/Runtime/ApplicationCore/Private/GenericPlatform/Accessibility/GenericAccessibleInterfaces.cpp:11-15, 24-31]. No engine `.ini` sets it.
- Compile-time `WITH_ACCESSIBILITY 1` [Engine/Source/Runtime/Core/Public/Misc/Build.h:106].
- The `SlateScreenReader` plugin is experimental with `"EnabledByDefault" : false` [Engine/Plugins/Experimental/SlateScreenReader/SlateScreenReader.uplugin:13].

**Windows UIA bridge.**
- The Text pattern is offered whenever `AsText()` is non-null [Engine/Source/Runtime/ApplicationCore/Private/Windows/Accessibility/WindowsUIAWidgetProvider.cpp:387-390].
- The range provider is mostly stubbed [Engine/Source/Runtime/ApplicationCore/Private/Windows/Accessibility/WindowsUIAControlProvider.cpp]:
  - `ExpandToEnclosingUnit` returns `E_NOTIMPL` for Format, Word, Line, Paragraph and Page (:103-130);
  - `FindAttribute`, `GetAttributeValue` and `GetBoundingRectangles` return `E_NOTIMPL` (:140-176);
  - `Move` and `MoveEndpointByUnit` return `E_NOTIMPL` (:207-214);
  - `Select`, `AddToSelection` and `RemoveFromSelection` return `E_NOTIMPL` (:244-256);
  - `get_SupportedTextSelection` returns `SupportedTextSelection_None` (:470-473).
- Mac has an accessibility layer (`Engine/Source/Runtime/ApplicationCore/Private/Mac/Accessibility/`). Linux has none.

**Hook for us:** `SWidget::CreateAccessibleWidget()` is `SLATECORE_API virtual` under `WITH_ACCESSIBILITY` [Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:657-659]. `FSlateAccessibleWidget` is exported for subclassing [Engine/Source/Runtime/SlateCore/Public/Widgets/Accessibility/SlateCoreAccessibleWidgets.h:18-23].

**Confidence:** high.

**Implication:**
- Our `SCodeView` should override `CreateAccessibleWidget` to return a subclass with `IAccessibleText` + `IAccessibleProperty` (value = current line or selection; read-only flag). That puts us **ahead of** stock multi-line text.
- Caret-level navigation by word or line cannot work through Slate's 5.8 UIA bridge. Spec the a11y target as:
  - "focusable, named, value = current line";
  - announcements through `FSlateApplication` accessible events;
  - not full screen-reader editing.
- State this honestly in SPEC.md.

---

## Corrections to research

| Research file | Claim | Correction from 5.8 source |
|---|---|---|
| 05-architecture-and-robustness.md (A1) | "the stock highlighter retokenizes and rebuilds the whole layout on every keystroke" | **Confirmed, and worse.** It also re-serialises and compares the whole text, deep-copies the token array (`ParseTokens` takes it by value), **re-shapes every line** (new `FLineModel` → fresh `FShapedTextCache`), and stores a full-text undo snapshot (99 levels) [SlateEditableTextLayout.cpp:3257-3314; TextLayout.cpp:2801-2809; SyntaxHighlighterTextLayoutMarshaller.h:36; SlateEditableTextTypes.h:102-105]. |
| 05 (A1 verdict) | "there is no virtualization" | **Partly wrong.** Paint is culled per line [SlateTextLayout.cpp:85-119]. Layout and shaping are not virtualised. |
| 05 (A1) | Output Log is built on the same multi-line text stack (UNVERIFIED for 5.8) | **Confirmed:** `SNew(SMultiLineEditableTextBox)` with `FOutputLogTextLayoutMarshaller`. It appends incrementally via `AddLines` and only full-rebuilds on `MakeDirty` (filter change etc.) [SOutputLog.cpp:937-942, 997-1141, 1316]. |
| 05 (A2) | IME by registering our own `ITextInputMethodContext`; Epic's is the reference | **Confirmed.** Additions: (a) `FCEFTextInputMethodContext` is a second, standalone reference; (b) 5.8 requires the `\n` ACP space and minimal `NotifyTextChanged` ranges for TSF/Korean [SlateEditableTextLayout.cpp:21-60]; (c) an alternative is hosting `FSlateEditableTextLayout` via `ISlateEditableTextWidget`, which brings IME for free but keeps the full-text model. |
| 05 (A2 cons) / R2, 03 C1 row "UE text input system handles IME" | Linux CJK IME has "open problems" | **Stronger than stated:** Linux has **no** `ITextInputMethodSystem` (the generic one returns NULL). Preedit (`SDL_EVENT_TEXT_EDITING`) is ignored; commits arrive as `OnKeyChar` [GenericApplication.h:550; LinuxApplication.cpp:391-400, 1433-1437]. Inline composition on Linux is impossible for any Slate widget. |
| 03-ide-features-lsp-dap.md (C1) and 05 (B) | UMG Web Browser had IME disabled (UE-75056); `BindInputMethodSystem` exists | In 5.8 `SWebBrowser::Construct` **auto-binds IME** [SWebBrowser.cpp:185-193], so the UMG widget gets it too. Raw `SWebBrowserView` does not. CEF IME is compiled out on Linux [CEFImeHandler.h:7]. |
| 05 (B, key routing) | Page decides asynchronously; "you end up keeping duplicate key maps" (UNVERIFIED for UE) | **Async confirmed:** `OnKeyDown` always returns true, and unhandled keys are re-injected later into Slate [CEFWebBrowserWindow.cpp:1212-1228, 1302-1327]. Editor chords therefore do eventually reach Slate if the page does not consume them. Duplicate maps are not strictly required, but ordering and latency differ. |
| 05 (B, High-DPI UNVERIFIED) | Blurry CEF possible | The UE integration sets `device_scale_factor` from the parent window's DPI [CEFBrowserHandler.cpp:368-374]. It is handled if the parent window is set. |
| 05 (B, accessibility UNVERIFIED) | Screen readers likely see nothing in windowless mode | **Confirmed:** WebBrowser has no CEF accessibility handler. |
| 05 (B) | Input lag and cost of OSR | Add: the default OSR frame rate is **24 fps**. It becomes 60 only with accelerated paint (D3D11/12 only; never on Linux; off under automation) [IWebBrowserSingleton.h:62; WebBrowserSingleton.cpp:688-698; CEFWebBrowserWindowRHIHelper.cpp:95-103]. |
| 03 (Recommendation 1) / C1 | Serve Monaco assets via `RegisterSchemeHandlerFactory` | Works, but no custom-scheme registration (`OnRegisterCustomSchemes`) exists. Use `https` with a domain-filtered factory, not a private scheme [IWebBrowserSingleton.h:128-135; CEFSchemeHandler.cpp:171-176]. |
| 03 (C1) | UE 5.8 ships CEF 128 / Chromium 128.0.6613.138 | **Confirmed** (Win64 `+v2` rebuild). Add: an experimental CEF 147.1.5 / Chromium 147.0.7727.49 path is present but switched off [CEF3.build.cs:16-45]. |
| 05 (A1, IME and accessibility) | "Screen-reader support exists in Slate (Accessibility.Enable=1 plus the Slate Screen Reader plugin)" | Confirmed that it is off by default (`GAllowAccessibility = false`; plugin `EnabledByDefault:false`). But `SMultiLineEditableText`/`Box` expose **no** accessible text, `IAccessibleText` is "currently incomplete", and UIA text navigation is mostly `E_NOTIMPL`. |
| 06-user-voice-and-landscape.md (open question 5) | CodeEditor plugin internals and reusability UNVERIFIED | **Resolved:** 0.1 beta, off by default, one Editor module, all Private, no exported API, C++-only highlighting, project tree over `Source/`. Not reusable by linking. |
| 05 (A1) | Epic uses this pattern for HLSL in UnrealEd and Niagara | Confirmed. Also used by the Material Custom node, Material Stats, Optimus, PCG and ComputeFramework, all through exported `FHLSLSyntaxHighlighterMarshaller`. No engine marshaller avoids live update. |

## New risks

1. **Linux has no IME at all** in Slate, and none in CEF. CJK users on Linux get commit-only input with no preedit and a misplaced candidate window. Mitigation: accept multi-character `OnKeyChar` commits, document it as an engine limit, and do not promise Linux inline IME in SPEC.
2. **TSF coordinate-space pitfalls.** Our context must use `\n`-only ACP indices and send minimal change ranges, or Korean and other composition IMEs desync. Epic fixed this in 5.8; a naive implementation would regress it.
3. **IME query cost.** If our context materialises the document per call (as Epic's does), large buffers will stutter during composition. Answer queries from the piece table.
4. **Mac IME blocks on the game thread** (`GameThreadCall`). While a Python script hogs the game thread, Cocoa input callbacks stall. Defocus or deactivate the IME context while a blocking run is in progress, or accept the stall.
5. **No bold or italic monospace face ships.** PyCharm-like styling needs a bundled mono family: licence check (OFL) and an `FCompositeFont` setup. CJK fallback glyphs break the fixed grid.
6. **Accessibility ceiling.** Even a correct `IAccessibleText` cannot give word or line navigation through 5.8's UIA bridge. Studios with accessibility requirements may need to be told this up front.
7. **CEF latency floor**, if Monaco is reconsidered. The default 24 fps OSR, async key re-injection, no accelerated paint on Vulkan or Linux, and accelerated paint disabled under automation (CI would test a different render path than users get).
8. **Copying engine code.** Optimus, PCG and CodeEditor are good references, but copying Epic source into a plugin whose code is public (e.g. a public GitHub repo) conflicts with the Unreal EULA. Re-implement the behaviours instead.
9. **Name collisions.** If Epic's CodeEditor is also enabled, avoid the tab ID `CodeEditor`, the style set `CodeEditorStyle` and the style keys `TextEditor.*`/`SyntaxHighlight.CPP.*` in our module.
10. **Undo memory in stock widgets.** If any stock `SMultiLineEditableText` is used for large content (REPL history or pastes), each of up to 99 undo levels stores the full text.

## Files read

Research:
- `<repo>\docs\research\03-ide-features-lsp-dap.md` (C1, recommendation, source list)
- `<repo>\docs\research\05-architecture-and-robustness.md` (A1, A2, B, risks, questions)
- `<repo>\docs\research\06-user-voice-and-landscape.md` (CodeEditor mentions)

Engine (all under `C:\Program Files\Epic Games\UE_5.8\Engine\`):
- `Source/Runtime/Slate/Public/Framework/Text/{ITextLayoutMarshaller.h, SyntaxHighlighterTextLayoutMarshaller.h, SyntaxTokenizer.h, TextLayout.h, SlateTextLayout.h, ShapedTextCache.h}`
- `Source/Runtime/Slate/Private/Framework/Text/{SyntaxHighlighterTextLayoutMarshaller.cpp, SyntaxTokenizer.cpp, TextLayout.cpp, SlateTextLayout.cpp, SlateTextRun.cpp, TextEditHelper.h}`
- `Source/Runtime/Slate/Public/Widgets/Text/{SMultiLineEditableText.h, SlateEditableTextLayout.h, SlateEditableTextTypes.h, ISlateEditableTextWidget.h}`
- `Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp`
- `Source/Runtime/Slate/Public/Widgets/Accessibility/SlateAccessibleWidgets.h`
- `Source/Runtime/Slate/Private/Widgets/{Input/SEditableText.cpp, Input/SEditableTextBox.cpp, Text/STextBlock.cpp}` (grep)
- `Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h`
- `Source/Runtime/ApplicationCore/Public/GenericPlatform/{ITextInputMethodSystem.h, GenericApplication.h, Accessibility/GenericAccessibleInterfaces.h}`
- `Source/Runtime/ApplicationCore/Private/GenericPlatform/Accessibility/GenericAccessibleInterfaces.cpp`
- `Source/Runtime/ApplicationCore/Public/Windows/{WindowsTextInputMethodSystem.h, WindowsApplication.h}` and `Private/Windows/{WindowsTextInputMethodSystem.cpp, WindowsApplication.cpp, Accessibility/WindowsUIAWidgetProvider.cpp, Accessibility/WindowsUIAControlProvider.cpp}`
- `Source/Runtime/ApplicationCore/Private/Mac/{MacTextInputMethodSystem.cpp, CocoaTextView.cpp}`, `Public/Mac/MacApplication.h`
- `Source/Runtime/ApplicationCore/Private/Linux/{LinuxApplication.cpp, LinuxWindow.cpp}`
- `Source/Runtime/SlateCore/Public/{Rendering/DrawElementTypes.h, Fonts/FontCache.h, Fonts/FontMeasure.h, Fonts/SlateFontInfo.h, Styling/SlateStyleMacros.h, Widgets/SWidget.h, Widgets/Accessibility/SlateCoreAccessibleWidgets.h}`
- `Source/Runtime/SlateCore/Private/{Fonts/FontCache.cpp, Fonts/FontMeasure.cpp, Fonts/LegacySlateFontInfoCache.cpp, Styling/StarshipCoreStyle.cpp, Styling/CoreStyle.cpp}`
- `Source/Runtime/Core/Public/Misc/Build.h`
- `Source/Runtime/WebBrowser/Public/{IWebBrowserWindow.h, IWebBrowserSingleton.h, IWebBrowserSchemeHandler.h, SWebBrowser.h, SWebBrowserView.h}`
- `Source/Runtime/WebBrowser/Private/{SWebBrowser.cpp, SWebBrowserView.cpp, WebBrowserModule.cpp, WebBrowserSingleton.cpp, CEF/CEFWebBrowserWindow.cpp, CEF/CEFWebBrowserWindowRHIHelper.cpp, CEF/CEFBrowserHandler.cpp, CEF/CEFImeHandler.h, CEF/CEFTextInputMethodContext.h, CEF/CEFSchemeHandler.cpp, CEF/CEFJSScripting.cpp}`
- `Source/ThirdParty/CEF3/CEF3.build.cs`; `Binaries/ThirdParty/CEF3/Win64/128.4.13+ge76af7e+chromium-128.0.6613.138+v2/` (listing); `Binaries/Win64/EpicWebHelper.exe` (listing)
- `Source/Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs`, `Source/Programs/UnrealBuildTool/Platform/Linux/UEBuildLinux.cs`
- `Source/Editor/MainFrame/MainFrame.Build.cs`, `Source/Editor/MainFrame/Private/HomeScreen/SHomeScreen.cpp`
- `Source/Editor/UnrealEd/Public/Text/{HLSLSyntaxHighlighterMarshaller.h, HLSLSyntaxTokenizer.h}`
- `Source/Editor/MaterialEditor/Private/{MaterialNodes/SGraphNodeMaterialCustom.cpp, MaterialStats.cpp}`
- `Source/Editor/EditorStyle/Private/StarshipStyle.cpp`
- `Source/Developer/OutputLog/Private/{SOutputLog.cpp, SOutputLog.h}`
- `Plugins/Experimental/CodeEditor/` (uplugin, Build.cs, all Private sources, Resources listing)
- `Plugins/Experimental/CodeView/CodeView.uplugin`
- `Plugins/Experimental/AIAssistant/Source/AIAssistant/Private/AIAssistantWebBrowser.cpp`, `AIAssistant.uplugin`
- `Plugins/Experimental/SlateScreenReader/SlateScreenReader.uplugin`
- `Plugins/Runtime/WebBrowserWidget/{WebBrowserWidget.uplugin, Source/WebBrowserWidget/Private/WebBrowser.cpp}`, `Plugins/Runtime/WebBrowserNativeProxy/WebBrowserNativeProxy.uplugin`
- `Plugins/Animation/DeformerGraph/Source/OptimusEditor/Private/{OptimusHLSLSyntaxHighlighter.h, Widgets/SOptimusShaderTextDocumentTextBox.cpp}`
- `Plugins/PCG/Source/PCGEditor/Private/{PCGHLSLSyntaxHighlighter.h, PCGHLSLSyntaxHighlighter.cpp, Widgets/SPCGCodeEditorTextBox.cpp}`
- `Plugins/FX/Niagara/Source/NiagaraEditor/{Public/Widgets/NiagaraHLSLSyntaxHighlighter.h, Private/Widgets/SNiagaraGraphNodeCustomHlsl.cpp}`
- `Content/Slate/Fonts/`, `Content/Editor/Slate/Fonts/`, `Content/EngineFonts/` (listings)
