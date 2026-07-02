using UnrealBuildTool;
using System.Collections.Generic;

// Editor target — this is what the harness builds (TestProjectEditor) and what
// `UnrealEditor[-Cmd].exe <uproject>` loads. Building it compiles the project
// module plus every enabled plugin, including UnrealMCP and its dependencies.
public class TestProjectEditorTarget : TargetRules
{
	public TestProjectEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("TestProject");
	}
}
