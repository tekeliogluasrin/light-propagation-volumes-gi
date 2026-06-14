// Copyright (c) LPVPGI contributors. Open source (see LICENSE).
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Templates/RefCounting.h"
#include "RenderGraphFwd.h"

class FScene;
class FViewInfo;
class FRDGBuilder;
class FGlobalIlluminationPluginResources;
struct IPooledRenderTarget;
struct FLPVGICascadeContext;

// hooks into UE's "Plugin" dynamic GI path and renders a cascaded LPV light field
// (inject -> convert -> propagate -> temporal -> composite). no engine changes
class FLightPropagationVolumesGIModule : public IModuleInterface
{
public:
	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// bound to the gi hook, runs on the render thread
	void OnRenderDiffuseIndirectLight(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		FGlobalIlluminationPluginResources& Resources);

	// does one cascade: inject -> convert -> propagate -> temporal, gives back the rgb volumes
	void RenderCascade(
		FRDGBuilder& GraphBuilder,
		int32 CascadeIndex,
		float CellSize,
		const FVector& SnappedCenter,
		const FVector3f& SnapOffset,
		const FVector3f& VolumeMin,
		const FLPVGICascadeContext& Ctx,
		FRDGTextureRef OutVolumes[3]);

	FDelegateHandle RenderDiffuseIndirectLightHandle;

	// keep in sync with LPVGI_MAX_CASCADES in the shaders
	static constexpr int32 MaxCascades = 4;

	// per-cascade history kept across frames for temporal + multi-bounce
	// indexed [cascade*3 + channel], render thread only
	TRefCountPtr<IPooledRenderTarget> HistoryVolume[MaxCascades * 3];
	FVector PrevSnappedCenter[MaxCascades] = {}; // last frame center, for reprojection
	float PrevCellSize[MaxCascades] = {};
	bool bHasHistory[MaxCascades] = {};
};
