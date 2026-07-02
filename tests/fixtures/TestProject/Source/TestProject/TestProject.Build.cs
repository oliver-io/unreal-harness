using UnrealBuildTool;

// Empty primary game module. The project carries no game code of its own — it
// exists purely as a compilable host so the UnrealMCP editor plugin has a
// target to be built and loaded into. Keep dependencies minimal.
public class TestProject : ModuleRules
{
	public TestProject(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });
	}
}
