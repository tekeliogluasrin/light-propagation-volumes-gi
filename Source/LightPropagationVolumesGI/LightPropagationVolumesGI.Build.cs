// Copyright (c) LPVPGI contributors. Open source (see LICENSE).

using UnrealBuildTool;

public class LightPropagationVolumesGI : ModuleRules
{
	public LightPropagationVolumesGI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Projects",   // IPluginManager (locate our shader directory)
			"RenderCore", // RDG, global shaders, FPixelShaderUtils
			"RHI",        // GMaxRHIFeatureLevel, static blend states
			"Renderer",   // links the RENDERER_API GI-plugin delegate accessors
		});
	}
}
