// Copyright (c) LPVPGI contributors. Open source (see LICENSE).

#include "LPVGIVolumetric.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "SceneView.h"
#include "LPVGISceneCapture.h"
#include "FXRenderingUtils.h"
#include "GlobalDistanceFieldParameters.h"
#include "Containers/StridedView.h"
#include "RenderUtils.h"
#include "RendererInterface.h"
#include "SystemTextures.h"

static TAutoConsoleVariable<int32> CVarLPVGIVolumetric(
	TEXT("r.LPVGI.Volumetric"), 0,
	TEXT("Volumetric light / fog composited after opaque lighting. Off by default (it is the heaviest feature)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricDensity(
	TEXT("r.LPVGI.Volumetric.Density"), 0.00002f,
	TEXT("Fog density per cm along the view ray."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricAmbient(
	TEXT("r.LPVGI.Volumetric.Ambient"), 1.0f,
	TEXT("Ambient (never shadowed) in-scatter. Lower it toward 0 for sharper light shafts."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricScatterScale(
	TEXT("r.LPVGI.Volumetric.ScatterScale"), 4.0f,
	TEXT("Multiplies in-scatter independently of density, so beams can be bright while the fog (extinction) stays thin and the scene stays clear."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricSideScatter(
	TEXT("r.LPVGI.Volumetric.SideScatter"), 0.15f,
	TEXT("View-independent floor added to the sun phase. Makes shafts visible from the side without raising sun intensity or blowing out the sun glow."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricMaxDistance(
	TEXT("r.LPVGI.Volumetric.MaxDistance"), 30000.0f,
	TEXT("How far (cm) the volumetric raymarch goes, so the sky doesn't become a flat fog wall."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricSteps(
	TEXT("r.LPVGI.Volumetric.Steps"), 32,
	TEXT("Number of raymarch steps."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricPhase(
	TEXT("r.LPVGI.Volumetric.Phase"), 0.4f,
	TEXT("Henyey-Greenstein anisotropy (0 = uniform, ->1 = strong forward sun glow)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricSunIntensity(
	TEXT("r.LPVGI.Volumetric.SunIntensity"), 1.0f,
	TEXT("Sun in-scatter intensity for the volumetrics."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricDownSample(
	TEXT("r.LPVGI.Volumetric.DownSample"), 2,
	TEXT("Raymarch resolution divisor (1 = full res, 2 = half res, 4 = quarter). Bigger = much cheaper, the fog is upsampled afterwards."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricGI(
	TEXT("r.LPVGI.Volumetric.GI"), 1,
	TEXT("Let the fog pick up bounce-light colour from the LPV light field (gi-coloured fog). Needs the LPV GI itself to be running. This is the feature stock volumetrics don't have."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricGIIntensity(
	TEXT("r.LPVGI.Volumetric.GIIntensity"), 1.0f,
	TEXT("Scales the LPV bounce light fed into the fog."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricGIDebug(
	TEXT("r.LPVGI.Volumetric.GIDebug"), 0,
	TEXT("Debug the gi-coloured fog. 1 = show whether the LPV volumes are bound/on (green = yes, red = no). 2 = show the raw bounce-light colour sampled at each surface point (boosted)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricSunShadow(
	TEXT("r.LPVGI.Volumetric.SunShadow"), 1,
	TEXT("God-ray shadows: trace the global sdf toward the sun at each step so geometry casts light shafts. Needs distance fields on."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIVolumetricSunShadowSteps(
	TEXT("r.LPVGI.Volumetric.SunShadow.Steps"), 16,
	TEXT("Max sdf sphere-trace steps toward the sun per raymarch sample."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricSunShadowMaxDist(
	TEXT("r.LPVGI.Volumetric.SunShadow.MaxDist"), 8000.0f,
	TEXT("How far (cm) to trace toward the sun looking for an occluder."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIVolumetricSunShadowSoftness(
	TEXT("r.LPVGI.Volumetric.SunShadow.Softness"), 8.0f,
	TEXT("Soft shadow penumbra. Higher = sharper shafts, lower = softer."),
	ECVF_RenderThreadSafe);

class FLPVGIVolumetricPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIVolumetricPS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIVolumetricPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, GlobalDistanceFieldParameters)
		SHADER_PARAMETER(FVector3f, SunDirection)
		SHADER_PARAMETER(FVector3f, SunColor)
		SHADER_PARAMETER(FVector3f, FogScatterColor)
		SHADER_PARAMETER(float, FogDensity)
		SHADER_PARAMETER(float, ScatterScale)
		SHADER_PARAMETER(float, MaxDistance)
		SHADER_PARAMETER(float, PhaseG)
		SHADER_PARAMETER(float, SideScatter)
		SHADER_PARAMETER(int32, NumSteps)
		SHADER_PARAMETER(FIntVector4, ViewportRect)
		SHADER_PARAMETER(int32, SunShadow)
		SHADER_PARAMETER(int32, SunShadowSteps)
		SHADER_PARAMETER(float, SunShadowMaxDist)
		SHADER_PARAMETER(float, SunShadowSoftness)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume0R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume0G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume0B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume1R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume1G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume1B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume2R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume2G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume2B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume3R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume3G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GIVolume3B)
		SHADER_PARAMETER_SAMPLER(SamplerState, GIVolumeSampler)
		SHADER_PARAMETER_ARRAY(FVector4f, GICascadeParams, [4])
		SHADER_PARAMETER(int32, GINumCascades)
		SHADER_PARAMETER(float, GIIntensity)
		SHADER_PARAMETER(int32, UseGI)
		SHADER_PARAMETER(int32, GIDebug)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLPVGIVolumetricPS, "/Plugin/LightPropagationVolumesGI/Private/LPVGIVolumetric.usf", "MainPS", SF_Pixel);

// second pass: upsample the low-res volumetric and composite it over full-res scene colour
class FLPVGIVolumetricUpsamplePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIVolumetricUpsamplePS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIVolumetricUpsamplePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VolumetricTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VolumetricDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, VolumetricSampler)
		SHADER_PARAMETER(FVector2f, VolBufferSize)
		SHADER_PARAMETER(FIntVector4, ViewportRect)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLPVGIVolumetricUpsamplePS, "/Plugin/LightPropagationVolumesGI/Private/LPVGIVolumetric.usf", "UpsamplePS", SF_Pixel);

FLPVGIVolumetricViewExtension::FLPVGIVolumetricViewExtension(const FAutoRegister& AutoRegister, TSharedPtr<FLPVGIVolumetricGIState, ESPMode::ThreadSafe> InGIState)
	: FSceneViewExtensionBase(AutoRegister)
	, GIState(MoveTemp(InGIState))
{
}

bool FLPVGIVolumetricViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& /*Context*/) const
{
	return CVarLPVGIVolumetric.GetValueOnAnyThread() != 0;
}

void FLPVGIVolumetricViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// before bloom, in HDR
	if (PassId == EPostProcessingPass::MotionBlur)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FLPVGIVolumetricViewExtension::PostProcessPass_RenderThread));
	}
}

FScreenPassTexture FLPVGIVolumetricViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid())
	{
		return SceneColor;
	}

	// render into a fresh target. we read scene colour as an srv and composite it in
	// the shader, so the scene colour texture is never both an srv and a render target
	// at once (that aliasing was what turned the whole frame black)
	const bool bHadOverride = Inputs.OverrideOutput.IsValid();
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, View.GetOverwriteLoadAction(), TEXT("LPVGI.Volumetric"));
	}

	// the pooled scene-colour texture can be larger than the view rect, and some present
	// paths show the whole extent. when we own the target (no override) cover the full
	// extent so no border stays unfogged. the off-view border is never displayed anyway.
	// if there is an override target we must respect its view rect (could be split screen)
	const FIntRect DrawRect = bHadOverride
		? Output.ViewRect
		: FIntRect(FIntPoint::ZeroValue, Output.Texture->Desc.Extent);

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// low-res raymarch target. it only covers the view (no border), so the upsample can
	// sample it with a plain 0..1 view uv. the divisor is what makes this cheap
	const int32 DownSample = FMath::Clamp(CVarLPVGIVolumetricDownSample.GetValueOnRenderThread(), 1, 4);
	const FIntPoint ViewSize = SceneColor.ViewRect.Size();
	const FIntPoint VolExtent(
		FMath::Max(1, FMath::DivideAndRoundUp(ViewSize.X, DownSample)),
		FMath::Max(1, FMath::DivideAndRoundUp(ViewSize.Y, DownSample)));

	const FRDGTextureDesc VolDesc = FRDGTextureDesc::Create2D(
		VolExtent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef VolTexture = GraphBuilder.CreateTexture(VolDesc, TEXT("LPVGI.VolumetricLowRes"));

	// low-res linear depth alongside the fog, for the depth-aware upsample
	const FRDGTextureDesc VolDepthDesc = FRDGTextureDesc::Create2D(
		VolExtent, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef VolDepthTexture = GraphBuilder.CreateTexture(VolDepthDesc, TEXT("LPVGI.VolumetricLowResDepth"));

	const FLPVGISunSkyState SunSky = LPVGI_GetSunSkyState();
	const float SunIntensity = FMath::Max(0.0f, CVarLPVGIVolumetricSunIntensity.GetValueOnRenderThread());

	// pass 1: raymarch at low res into VolTexture
	{
		FLPVGIVolumetricPS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIVolumetricPS::FParameters>();
		Params->View = View.ViewUniformBuffer;
		Params->SceneTextures = Inputs.SceneTextures;
		Params->SunDirection = SunSky.bValidSun ? SunSky.SunDirection : FVector3f(0.0f, 0.0f, -1.0f);
		Params->SunColor = (SunSky.bValidSun ? SunSky.SunColor : FVector3f(1.0f, 1.0f, 1.0f)) * SunIntensity;
		Params->FogScatterColor = FVector3f(0.08f, 0.10f, 0.14f) * FMath::Max(0.0f, CVarLPVGIVolumetricAmbient.GetValueOnRenderThread());
		Params->FogDensity = FMath::Max(0.0f, CVarLPVGIVolumetricDensity.GetValueOnRenderThread());
		Params->ScatterScale = FMath::Max(0.0f, CVarLPVGIVolumetricScatterScale.GetValueOnRenderThread());
		Params->MaxDistance = FMath::Max(100.0f, CVarLPVGIVolumetricMaxDistance.GetValueOnRenderThread());
		Params->PhaseG = FMath::Clamp(CVarLPVGIVolumetricPhase.GetValueOnRenderThread(), -0.95f, 0.95f);
		Params->SideScatter = FMath::Max(0.0f, CVarLPVGIVolumetricSideScatter.GetValueOnRenderThread());
		Params->NumSteps = FMath::Clamp(CVarLPVGIVolumetricSteps.GetValueOnRenderThread(), 4, 256);
		// the low-res target only holds the view, so its view rect is the whole extent
		Params->ViewportRect = FIntVector4(0, 0, VolExtent.X, VolExtent.Y);

		// global sdf for god-ray shadows. same public api the gi hook uses. if distance
		// fields are off SDFData is null and we bind a zeroed set, so the trace finds no
		// occluders and the shader just skips the shadow (bShadow goes to 0)
		const FGlobalDistanceFieldParameterData* SDFData =
			UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData(MakeConstStridedView<FSceneView>((int32)sizeof(FSceneView), &View, 1));
		const bool bShadowOn = (SDFData != nullptr) && (CVarLPVGIVolumetricSunShadow.GetValueOnRenderThread() != 0);

		FGlobalDistanceFieldParameterData DummySDF;
		Params->GlobalDistanceFieldParameters = SetupGlobalDistanceFieldParameters_Minimal(SDFData ? *SDFData : DummySDF);
		{
			FRHISamplerState* SDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Params->GlobalDistanceFieldParameters.GlobalDistanceFieldPageAtlasTextureSampler = SDFSampler;
			Params->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTextureSampler = SDFSampler;
			Params->GlobalDistanceFieldParameters.GlobalDistanceFieldMipTextureSampler = SDFSampler;
			if (!Params->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTexture)
			{
				Params->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTexture = GBlackVolumeTexture->TextureRHI;
			}
		}

		Params->SunShadow = bShadowOn ? 1 : 0;
		Params->SunShadowSteps = FMath::Clamp(CVarLPVGIVolumetricSunShadowSteps.GetValueOnRenderThread(), 1, 64);
		Params->SunShadowMaxDist = FMath::Max(100.0f, CVarLPVGIVolumetricSunShadowMaxDist.GetValueOnRenderThread());
		Params->SunShadowSoftness = FMath::Max(0.5f, CVarLPVGIVolumetricSunShadowSoftness.GetValueOnRenderThread());

		// gi-coloured fog: feed the propagated lpv light field into the raymarch. the volumes
		// are shared from the gi hook and are one frame behind, which is invisible for fog. if
		// the lpv gi isn't running the state is invalid, so we bind black dummies and UseGI = 0
		{
			constexpr int32 GIMaxCascades = FLPVGIVolumetricGIState::MaxCascades;
			FRDGTextureRef BlackVol = GSystemTextures.GetVolumetricBlackDummy(GraphBuilder);
			FRDGTextureRef GIVol[GIMaxCascades * 3];
			for (int32 i = 0; i < GIMaxCascades * 3; ++i)
			{
				GIVol[i] = BlackVol;
			}
			for (int32 c = 0; c < GIMaxCascades; ++c)
			{
				Params->GICascadeParams[c] = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
			}

			const bool bGIOn = GIState.IsValid() && GIState->bValid && GIState->NumCascades > 0
				&& CVarLPVGIVolumetricGI.GetValueOnRenderThread() != 0;
			int32 N = 0;
			if (bGIOn)
			{
				N = FMath::Clamp(GIState->NumCascades, 1, GIMaxCascades);
				for (int32 c = 0; c < N; ++c)
				{
					for (int32 ch = 0; ch < 3; ++ch)
					{
						const int32 Idx = c * 3 + ch;
						if (GIState->Volumes[Idx].IsValid())
						{
							GIVol[Idx] = GraphBuilder.RegisterExternalTexture(GIState->Volumes[Idx]);
						}
					}
					Params->GICascadeParams[c] = GIState->CascadeParams[c];
				}
			}

			Params->GIVolume0R = GIVol[0]; Params->GIVolume0G = GIVol[1]; Params->GIVolume0B = GIVol[2];
			Params->GIVolume1R = GIVol[3]; Params->GIVolume1G = GIVol[4]; Params->GIVolume1B = GIVol[5];
			Params->GIVolume2R = GIVol[6]; Params->GIVolume2G = GIVol[7]; Params->GIVolume2B = GIVol[8];
			Params->GIVolume3R = GIVol[9]; Params->GIVolume3G = GIVol[10]; Params->GIVolume3B = GIVol[11];
			Params->GIVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Params->GINumCascades = N;
			Params->GIIntensity = FMath::Max(0.0f, CVarLPVGIVolumetricGIIntensity.GetValueOnRenderThread());
			Params->UseGI = (N > 0) ? 1 : 0;
			Params->GIDebug = CVarLPVGIVolumetricGIDebug.GetValueOnRenderThread();
		}

		Params->RenderTargets[0] = FRenderTargetBinding(VolTexture, ERenderTargetLoadAction::ENoAction);
		Params->RenderTargets[1] = FRenderTargetBinding(VolDepthTexture, ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FLPVGIVolumetricPS> PixelShader(ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder, ShaderMap, RDG_EVENT_NAME("LPVGI::Volumetric(%dx%d)", VolExtent.X, VolExtent.Y),
			PixelShader, Params, FIntRect(FIntPoint::ZeroValue, VolExtent));
	}

	// pass 2: upsample + composite over full-res scene colour
	{
		FLPVGIVolumetricUpsamplePS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIVolumetricUpsamplePS::FParameters>();
		Params->View = View.ViewUniformBuffer;
		Params->SceneTextures = Inputs.SceneTextures;
		Params->SceneColorTexture = SceneColor.Texture;
		Params->VolumetricTexture = VolTexture;
		Params->VolumetricDepthTexture = VolDepthTexture;
		Params->VolumetricSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->VolBufferSize = FVector2f((float)VolExtent.X, (float)VolExtent.Y);
		Params->ViewportRect = FIntVector4(SceneColor.ViewRect.Min.X, SceneColor.ViewRect.Min.Y, SceneColor.ViewRect.Max.X, SceneColor.ViewRect.Max.Y);
		Params->RenderTargets[0] = Output.GetRenderTargetBinding();

		TShaderMapRef<FLPVGIVolumetricUpsamplePS> PixelShader(ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder, ShaderMap, RDG_EVENT_NAME("LPVGI::VolumetricUpsample"),
			PixelShader, Params, DrawRect);
	}

	return MoveTemp(Output);
}
