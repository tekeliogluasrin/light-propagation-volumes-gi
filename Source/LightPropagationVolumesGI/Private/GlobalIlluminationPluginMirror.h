// Copyright (c) LPVPGI contributors. Open source (see LICENSE).

// the gi plugin hook lives in a private engine header (DeferredShadingRenderer.h)
// so rather than dragging the whole thing in we just redeclare what we need.
// the accessors are RENDERER_API so the linker resolves them by name, and the
// struct layout matches the engine so the offsets line up. needs to match the
// engine version (5.6). if epic changes it this fails to link, which is fine
#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"

// private renderer types, we only pass these through by reference
class FScene;
class FViewInfo;
class FRDGBuilder;

// mirror of FGlobalIlluminationPluginResources
class FGlobalIlluminationPluginResources : public FRenderResource
{
public:
	FRDGTextureRef GBufferA;
	FRDGTextureRef GBufferB;
	FRDGTextureRef GBufferC;
	FRDGTextureRef SceneDepthZ;
	FRDGTextureRef SceneColor;
	FRDGTextureRef LightingChannelsTexture;
};

// mirror of FGlobalIlluminationPluginDelegates
class FGlobalIlluminationPluginDelegates
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FAnyRayTracingPassEnabled, bool& /*bAnyRayTracingPassEnabled*/);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FPrepareRayTracing, const FViewInfo& /*View*/, TArray<FRHIRayTracingShader*>& /*OutRayGenShaders*/);
	DECLARE_MULTICAST_DELEGATE_FourParams(FRenderDiffuseIndirectLight, const FScene& /*Scene*/, const FViewInfo& /*View*/, FRDGBuilder& /*GraphBuilder*/, FGlobalIlluminationPluginResources& /*Resources*/);

	static RENDERER_API FAnyRayTracingPassEnabled& AnyRayTracingPassEnabled();
	static RENDERER_API FPrepareRayTracing& PrepareRayTracing();
	static RENDERER_API FRenderDiffuseIndirectLight& RenderDiffuseIndirectLight();
};
