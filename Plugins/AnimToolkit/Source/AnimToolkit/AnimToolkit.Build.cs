// SPDX-License-Identifier: Apache-2.0
using UnrealBuildTool;

public class AnimToolkit : ModuleRules
{
	public AnimToolkit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"ApplicationCore",          // clipboard
			"Json",                     // widget specs and event payloads
			"UnrealEd",                 // FAssetDragDropOp
			"EditorFramework",
			"WorkspaceMenuStructure",   // Window > Tools > Anim Toolkit group
			"ToolMenus",
			"PythonScriptPlugin",       // tab factory commands
		});
	}
}
