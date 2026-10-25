using UnrealBuildTool;

public class Generals1 : ModuleRules
{
	public Generals1(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Each .cpp on its own: many files keep helpers of the same name in anonymous namespaces
		// (CellSize, ReadU32, ClientFPS ...), which collide when unity builds merge files.
		bUseUnity = false;

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
			"ApplicationCore",
			"Slate",
			"SlateCore",
			"UMG",
			"AudioMixer",
			"AudioExtensions",
			// Network games: plain TCP between the players and the host's server.
			"Sockets",
			"Networking",
		});

		// Music is MP3; the engine ships dr_mp3 for it.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "dr_libs");
	}
}
