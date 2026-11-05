// SPDX-License-Identifier: Apache-2.0
// TDD §2.3.3 minus what slice A does not use. Each name was checked against a .Build.cs in the local UE 5.8 install.
// Deferred (M2): ContentBrowser and ContentBrowserData (the Content Browser integration; PropertyEditor is in since PO-04,
// for the theme dropdown, marked (theme) below). Deferred (M4):
// SourceControl (the async checkout, §11.6.5), Json and JsonUtilities (the diagnostics bundle). Deferred (1.1): Sockets and
// Networking (the bridge).
using System.IO;
using UnrealBuildTool;

public class DreamEditor : ModuleRules
{
	public DreamEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// Headers are included as "Run/DreamRunService.h" etc. from Private/ (TDD §2.2 layout).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"DreamCore",                 // text model, lexer, statement extent, output ring, journal format, keymap (D7)
			"Python3",                  // CPython 3.11.8 headers and python311.lib; WITH_PYTHON (Python3.Build.cs:82)
			"PythonScriptPlugin",       // IPythonScriptPlugin: ExecPythonCommandEx, lifecycle delegates
			"CoreUObject",              // UE::IsSavingPackage, FUObjectThreadContext (RT-02), settings UCLASSes
			"Engine",                   // IsGarbageCollecting, GEngine-free editor types
			"Slate",                    // FSlateApplication pump, modal, pre-processor, SMultiLineEditableText (plain mode)
			"SlateCore",
			"InputCore",                // EKeys, FKey
			"ApplicationCore",          // clipboard, window messages, FGenericWindow actions
			"UnrealEd",                 // GEditor transactions (UTransBuffer), FFeedbackContextEditor, UImportSubsystem
			"EditorFramework",          // types reached through UnrealEd and LevelEditor headers
			"MainFrame",                // RegisterCanCloseEditor veto, main frame command bindings
			"LevelEditor",              // toolbar and the LevelEditor.StatusBar.ToolBar menu (WS-07)
			"ToolMenus",                // Tools and Window menus, status-bar entry
			"WorkspaceMenuStructure",   // the Window-menu group of the nomad tab (§11.6.1)
			"DesktopPlatform",          // Open / Save As file dialogs
			"Projects",                 // IPluginManager: plugin base dir (style resources, dream_editor path check)
			"DeveloperSettings",        // UDreamUserSettings, UDreamPerProjectUserSettings, UDreamProjectSettings (D21)
			"AssetRegistry",            // IAssetRegistry::OnAssetRenamed for the rename note (§5.9.2)
			"RenderCore",               // ENQUEUE_RENDER_COMMAND in the pump (§5.4.4)
			"RHI",                      // GIsRHIInitialized, RHICmdList.EndFrame (07b Q6)
			"DirectoryWatcher",         // (buffers, B80) external changes: IDirectoryWatcher per open file's directory (§10.15)
			"TraceLog",                 // (run group, B39) the DreamPump and DreamRun Insights channels (Diagnostics/DreamTrace.cpp, TEL-03)
			"PropertyEditor",           // (theme, PO-04) the ThemeId dropdown in Editor Preferences (Shell/DreamThemeDetails.cpp)
		});

		// (instances, B23) ISettingsModule::ShowViewer for [Open storage settings] (Persistence/DreamInstances.cpp); an interface
		// reached through FModuleManager, so its headers suffice (no link dependency).
		PrivateIncludePathModuleNames.Add("Settings");
	}
}
