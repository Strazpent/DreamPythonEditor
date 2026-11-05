using UnrealBuildTool;

public class DreamHostTarget : TargetRules
{
	public DreamHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("DreamHost");
	}
}
