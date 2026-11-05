// SPDX-License-Identifier: Apache-2.0
using UnrealBuildTool;

public class DreamCore : ModuleRules
{
	public DreamCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bEnableExceptions = false;
		bUseRTTI = false;
		PublicIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Public"));
		PrivateDependencyModuleNames.Add("Core");
	}
}
