// Copyright (c) LPVPGI contributors. Open source (see LICENSE).
#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Templates/RefCounting.h"

struct IPooledRenderTarget;

// the propagated lpv light field, handed from the gi hook to the volumetric pass so the
// fog can pick up bounce-light colour. holds last frame's volumes (one frame behind, which
// is invisible for low-frequency fog). written and read on the render thread only
struct FLPVGIVolumetricGIState
{
	static constexpr int32 MaxCascades = 4;
	TRefCountPtr<IPooledRenderTarget> Volumes[MaxCascades * 3];          // [cascade*3 + channel]
	FVector4f CascadeParams[MaxCascades] = { FVector4f(ForceInitToZero), // xyz = snap offset, w = cell size
		FVector4f(ForceInitToZero), FVector4f(ForceInitToZero), FVector4f(ForceInitToZero) };
	int32 NumCascades = 0;
	bool bValid = false;
};

// composites volumetric light/fog into scene colour through a post-process pass.
// off by default (r.LPVGI.Volumetric), so it costs nothing unless enabled
class FLPVGIVolumetricViewExtension : public FSceneViewExtensionBase
{
public:
	FLPVGIVolumetricViewExtension(const FAutoRegister& AutoRegister, TSharedPtr<FLPVGIVolumetricGIState, ESPMode::ThreadSafe> InGIState);

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const struct FPostProcessMaterialInputs& Inputs);

	// shared with the gi hook module; null/invalid until the first lpv frame is rendered
	TSharedPtr<FLPVGIVolumetricGIState, ESPMode::ThreadSafe> GIState;
};
