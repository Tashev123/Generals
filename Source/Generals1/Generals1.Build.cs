using UnrealBuildTool;

public class Generals1 : ModuleRules
{
	public Generals1(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"RenderCore",
			"RHI",
			"MeshDescription",
			"StaticMeshDescription",
			"ProceduralMeshComponent",
			"DeveloperSettings",
			"Slate",
			"SlateCore",
			"UMG",
		});
	}
}
