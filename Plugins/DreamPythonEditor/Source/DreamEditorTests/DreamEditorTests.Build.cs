// SPDX-License-Identifier: Apache-2.0
// Tier-4 automation specs and fixtures (TDD §2.2, §14.1, §14.2). Repository only: release builds use a descriptor
// without this module (its fixtures tick the core ticker, RT-01), and T-ARCH-004 fails a package that contains it.
// (slice A choice) The specs reach DreamEditor's services through its Private headers (added below) and the
// DREAMEDITOR_API exports on the classes in shared headers; CQTest (§2.3.3) is not used yet, because it is a plugin the
// release descriptor would have to list; the specs use Core's Automation Spec and AutomationDriver.
using System.IO;
using UnrealBuildTool;

public class DreamEditorTests : ModuleRules
{
	public DreamEditorTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "DreamEditor", "Private"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"Json",                     // the user-settings guard's run baseline (Saved/DreamTests/settings-snapshot.json)
			"ApplicationCore",
			"UnrealEd",
			"LevelEditor",              // the G5 fixture: the Level Editor tab and its viewport (TDD 4.16.1)
			"EditorFramework",
			"DeveloperSettings",
			"Projects",                 // IPluginManager (Dream.Language specs locate the plugin folder)
			"PropertyEditor",           // (theme) Dream.Theme.Dropdown builds an IDetailsView on UDreamUserSettings (the ThemeId dropdown)
			"DreamCore",
			"DreamEditor",
			"PythonScriptPlugin",
			"Python3",                  // the _dream_test fixture module (DreamTestLatch, §14.2)
			"AutomationDriver",
		});
	}
}
