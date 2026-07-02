using UnrealBuildTool;
using System.Collections.Generic;

// Game target for the UnrealMCP test host project. Kept minimal and
// version-agnostic (BuildSettingsVersion.Latest) so it compiles against the
// engine pinned via UNREAL_ENGINE_ROOT without per-version edits.
public class TestProjectTarget : TargetRules
{
	public TestProjectTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("TestProject");
	}
}
