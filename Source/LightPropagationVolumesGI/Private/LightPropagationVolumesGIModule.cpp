// Copyright (c) LPVPGI contributors. Open source (see LICENSE).

#include "LightPropagationVolumesGIModule.h"
#include "GlobalIlluminationPluginMirror.h"
#include "LPVGISceneCapture.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "PixelShaderUtils.h"
#include "RHIStaticStates.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "SceneView.h"
#include "RendererInterface.h"
#include "TextureResource.h"
#include "FXRenderingUtils.h"
#include "GlobalDistanceFieldParameters.h"
#include "Containers/StridedView.h"
#include "RenderUtils.h"

#define LOCTEXT_NAMESPACE "FLightPropagationVolumesGIModule"

DEFINE_LOG_CATEGORY_STATIC(LogLPVGI, Log, All);

// keep these in sync with the shaders (LPVGICommon.ush)
static constexpr int32 GLPVGIGridSize = 32;
static constexpr int32 GLPVGIPackPerCell = 17;
static constexpr float GLPVGIFixedPointScale = 4096.0f;

static TAutoConsoleVariable<int32> CVarLPVGIEnable(
	TEXT("r.LPVGI.Enable"), 1,
	TEXT("Enable Light Propagation Volumes GI composite (requires r.DynamicGlobalIlluminationMethod=3)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGICellSize(
	TEXT("r.LPVGI.CellSize"), 50.0f,
	TEXT("World-space cell size (cm) of the first (finest) LPV cascade."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGINumCascades(
	TEXT("r.LPVGI.NumCascades"), 3,
	TEXT("Number of nested LPV cascades (1-4). Each is 2x coarser/larger than the previous."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIEdgeFade(
	TEXT("r.LPVGI.EdgeFade"), 0.15f,
	TEXT("Fraction of a cascade's outer region used to fade into the next coarser cascade."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIIntensity(
	TEXT("r.LPVGI.Intensity"), 4.0f,
	TEXT("Overall intensity multiplier for the indirect light composited into scene colour."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGISpecular(
	TEXT("r.LPVGI.Specular"), 1.0f,
	TEXT("Glossy/specular GI strength sampled from the volume in the reflection direction (0 = diffuse only)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIPropagationSteps(
	TEXT("r.LPVGI.PropagationSteps"), 8,
	TEXT("Number of light propagation iterations (0 = injection only, no spreading)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIOcclusion(
	TEXT("r.LPVGI.Occlusion"), 1,
	TEXT("Use the geometry volume to occlude propagation (reduces light leaking)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIOcclusionStrength(
	TEXT("r.LPVGI.OcclusionStrength"), 1.0f,
	TEXT("Strength of geometry-volume occlusion during propagation."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIOcclusionSDF(
	TEXT("r.LPVGI.Occlusion.SDF"), 1,
	TEXT("Build the occlusion geometry volume from the Global Distance Field (1) instead of injected surfaces (0). Greatly reduces light leaking."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIOcclusionThickness(
	TEXT("r.LPVGI.Occlusion.Thickness"), 1.0f,
	TEXT("Half-width (in cells) of the surface band treated as a blocker when building the SDF occlusion volume."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGITemporal(
	TEXT("r.LPVGI.Temporal"), 1,
	TEXT("Temporally accumulate the light field across frames (greatly reduces flicker)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGITemporalBlend(
	TEXT("r.LPVGI.TemporalBlend"), 0.1f,
	TEXT("Weight of the current frame in the temporal blend (lower = smoother/more stable, slower response)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarLPVGIFeedback(
	TEXT("r.LPVGI.Feedback"), 0.6f,
	TEXT("Multi-bounce feedback strength (0 = single bounce). Re-injects the previous frame's light field off surfaces for richer indirect lighting."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIDebugView(
	TEXT("r.LPVGI.DebugView"), 0,
	TEXT("Debug visualization (replaces scene colour):\n")
	TEXT("  0 = off (normal additive composite)\n")
	TEXT("  1 = indirect light only (with albedo)\n")
	TEXT("  2 = raw light field (no albedo)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIInjectMode(
	TEXT("r.LPVGI.InjectMode"), 1,
	TEXT("Light injection source: 0 = screen-space GBuffer (legacy/fallback), 1 = RSM sun capture (view-independent)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarLPVGIRSMResolution(
	TEXT("r.LPVGI.RSM.Resolution"), 512,
	TEXT("Resolution of the square Reflective Shadow Map captured from the sun."),
	ECVF_RenderThreadSafe);

static const TCHAR* GLPVGIShaderVirtualDir = TEXT("/Plugin/LightPropagationVolumesGI");

// shaders
class FLPVGIInjectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIInjectCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIInjectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<int>, RWAccum)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER(FVector3f, SunDirection)
		SHADER_PARAMETER(FVector3f, SunColor)
		SHADER_PARAMETER(FVector3f, SkyColor)
		SHADER_PARAMETER(FVector3f, VolumeSnapOffset)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, FixedPointScale)
		SHADER_PARAMETER(FIntPoint, ViewportSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGIInjectRSMCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIInjectRSMCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIInjectRSMCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RSMTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<int>, RWAccum)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, FeedbackR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, FeedbackG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, FeedbackB)
		SHADER_PARAMETER(FVector3f, CaptureOrigin)
		SHADER_PARAMETER(FVector3f, AxisRight)
		SHADER_PARAMETER(FVector3f, AxisUp)
		SHADER_PARAMETER(FVector3f, AxisForward)
		SHADER_PARAMETER(FVector3f, VolumeMin)
		SHADER_PARAMETER(FVector3f, SunColor)
		SHADER_PARAMETER(float, OrthoWidth)
		SHADER_PARAMETER(float, MaxDepth)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, FixedPointScale)
		SHADER_PARAMETER(float, Feedback)
		SHADER_PARAMETER(int32, HasFeedback)
		SHADER_PARAMETER(int32, Resolution)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGIConvertCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIConvertCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIConvertCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<int>, AccumTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWCurR)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWCurG)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWCurB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumR)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumG)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWGeometryVolume)
		SHADER_PARAMETER(float, InvFixedPointScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGIBuildGVCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIBuildGVCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIBuildGVCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, GlobalDistanceFieldParameters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWGeometryVolume)
		SHADER_PARAMETER(FVector3f, VolumeSnapOffset)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, Thickness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGIPropagateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGIPropagateCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGIPropagateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, CurR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, CurG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, CurB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, GeometryVolume)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, AccumPrevR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, AccumPrevG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, AccumPrevB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWNextR)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWNextG)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWNextB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumNextR)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumNextG)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWAccumNextB)
		SHADER_PARAMETER(int32, UseOcclusion)
		SHADER_PARAMETER(float, OcclusionStrength)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGITemporalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGITemporalCS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGITemporalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ThisR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ThisG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ThisB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, HistoryR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, HistoryG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, HistoryB)
		SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWOutR)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWOutG)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWOutB)
		SHADER_PARAMETER(FVector3f, CellShift)
		SHADER_PARAMETER(float, Alpha)
		SHADER_PARAMETER(int32, HasHistory)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FLPVGILookupPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FLPVGILookupPS);
	SHADER_USE_PARAMETER_STRUCT(FLPVGILookupPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume0R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume0G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume0B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume1R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume1G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume1B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume2R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume2G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume2B)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume3R)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume3G)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, Volume3B)
		SHADER_PARAMETER_SAMPLER(SamplerState, VolumeSampler)
		SHADER_PARAMETER_ARRAY(FVector4f, CascadeParams, [4])
		SHADER_PARAMETER(int32, NumCascades)
		SHADER_PARAMETER(float, EdgeFadeFrac)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER(float, SpecularIntensity)
		SHADER_PARAMETER(int32, DebugView)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLPVGIInjectCS,    "/Plugin/LightPropagationVolumesGI/Private/LPVGIInject.usf",    "InjectCS",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGIInjectRSMCS, "/Plugin/LightPropagationVolumesGI/Private/LPVGIInjectRSM.usf", "InjectRSMCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGIConvertCS,   "/Plugin/LightPropagationVolumesGI/Private/LPVGIConvert.usf",   "ConvertCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGIBuildGVCS,   "/Plugin/LightPropagationVolumesGI/Private/LPVGIBuildGV.usf",   "BuildGVCS",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGIPropagateCS, "/Plugin/LightPropagationVolumesGI/Private/LPVGIPropagate.usf", "PropagateCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGITemporalCS,  "/Plugin/LightPropagationVolumesGI/Private/LPVGITemporal.usf",  "TemporalCS",  SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLPVGILookupPS,    "/Plugin/LightPropagationVolumesGI/Private/LPVGILookup.usf",    "LookupPS",    SF_Pixel);

// helpers
namespace
{
	FRDGTextureRef CreateVolume(FRDGBuilder& GraphBuilder, const TCHAR* Name)
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create3D(
			FIntVector(GLPVGIGridSize, GLPVGIGridSize, GLPVGIGridSize),
			PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);
		return GraphBuilder.CreateTexture(Desc, Name);
	}

	FIntVector VolumeGroupCount()
	{
		return FIntVector(
			FMath::DivideAndRoundUp(GLPVGIGridSize, 4),
			FMath::DivideAndRoundUp(GLPVGIGridSize, 4),
			FMath::DivideAndRoundUp(GLPVGIGridSize, 4));
	}
}

// module
void FLightPropagationVolumesGIModule::StartupModule()
{
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LightPropagationVolumesGI")))
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(GLPVGIShaderVirtualDir, ShaderDir);
	}

	LPVGI_StartSceneCapture();

	RenderDiffuseIndirectLightHandle =
		FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().AddRaw(
			this, &FLightPropagationVolumesGIModule::OnRenderDiffuseIndirectLight);

	UE_LOG(LogLPVGI, Log, TEXT("Light Propagation Volumes GI: bound to RenderDiffuseIndirectLight hook."));
}

void FLightPropagationVolumesGIModule::ShutdownModule()
{
	if (RenderDiffuseIndirectLightHandle.IsValid())
	{
		FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().Remove(RenderDiffuseIndirectLightHandle);
		RenderDiffuseIndirectLightHandle.Reset();
	}

	LPVGI_StopSceneCapture();

	for (int32 i = 0; i < MaxCascades * 3; ++i)
	{
		HistoryVolume[i].SafeRelease();
	}
	for (int32 i = 0; i < MaxCascades; ++i)
	{
		bHasHistory[i] = false;
	}
}

struct FLPVGICascadeContext
{
	const FSceneView* SceneView = nullptr;
	FGlobalIlluminationPluginResources* Resources = nullptr;
	FIntPoint ViewportSize = FIntPoint::ZeroValue;
	FLPVGISunSkyState SunSky;
	bool bRSMMode = false; // inject from per-cascade RSM (vs screen-space fallback)
	int32 PropagationSteps = 0;
	int32 UseOcclusion = 0;
	float OcclusionStrength = 1.0f;
	bool bUseSDFOcclusion = false;
	float OcclusionThickness = 1.0f;
	FGlobalDistanceFieldParameters2 SDFParams;
	float Feedback = 0.0f;
	bool bDoTemporal = true;
	float TemporalAlpha = 0.1f;
};

void FLightPropagationVolumesGIModule::RenderCascade(
	FRDGBuilder& GraphBuilder,
	int32 CascadeIndex,
	float CellSize,
	const FVector& SnappedCenter,
	const FVector3f& SnapOffset,
	const FVector3f& VolumeMin,
	const FLPVGICascadeContext& Ctx,
	FRDGTextureRef OutVolumes[3])
{
	RDG_EVENT_SCOPE(GraphBuilder, "LPVGI::Cascade%d", CascadeIndex);

	const FRDGTextureDesc AccumIntDesc = FRDGTextureDesc::Create3D(
		FIntVector(GLPVGIGridSize, GLPVGIGridSize, GLPVGIGridSize * GLPVGIPackPerCell),
		PF_R32_SINT, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef AccumInt = GraphBuilder.CreateTexture(AccumIntDesc, TEXT("LPVGI.AccumInt"));

	FRDGTextureRef VolA[3] = { CreateVolume(GraphBuilder, TEXT("LPVGI.VolA.R")), CreateVolume(GraphBuilder, TEXT("LPVGI.VolA.G")), CreateVolume(GraphBuilder, TEXT("LPVGI.VolA.B")) };
	FRDGTextureRef VolB[3] = { CreateVolume(GraphBuilder, TEXT("LPVGI.VolB.R")), CreateVolume(GraphBuilder, TEXT("LPVGI.VolB.G")), CreateVolume(GraphBuilder, TEXT("LPVGI.VolB.B")) };
	FRDGTextureRef AccA[3] = { CreateVolume(GraphBuilder, TEXT("LPVGI.AccA.R")), CreateVolume(GraphBuilder, TEXT("LPVGI.AccA.G")), CreateVolume(GraphBuilder, TEXT("LPVGI.AccA.B")) };
	FRDGTextureRef AccB[3] = { CreateVolume(GraphBuilder, TEXT("LPVGI.AccB.R")), CreateVolume(GraphBuilder, TEXT("LPVGI.AccB.G")), CreateVolume(GraphBuilder, TEXT("LPVGI.AccB.B")) };
	FRDGTextureRef GeometryVolume = CreateVolume(GraphBuilder, TEXT("LPVGI.GV"));

	FRDGTextureUAVRef AccumUAV = GraphBuilder.CreateUAV(AccumInt);
	AddClearUAVPass(GraphBuilder, AccumUAV, 0u);

	// last frame's light field for this cascade, used for the bounce feedback and temporal
	const int32 H = CascadeIndex * 3;
	const bool bValidHistory = bHasHistory[CascadeIndex] && FMath::IsNearlyEqual(PrevCellSize[CascadeIndex], CellSize)
		&& HistoryVolume[H + 0].IsValid() && HistoryVolume[H + 1].IsValid() && HistoryVolume[H + 2].IsValid();
	FRDGTextureRef HistR = bValidHistory ? GraphBuilder.RegisterExternalTexture(HistoryVolume[H + 0]) : nullptr;
	FRDGTextureRef HistG = bValidHistory ? GraphBuilder.RegisterExternalTexture(HistoryVolume[H + 1]) : nullptr;
	FRDGTextureRef HistB = bValidHistory ? GraphBuilder.RegisterExternalTexture(HistoryVolume[H + 2]) : nullptr;
	// this cascade's own rsm (captured round-robin, so it may be a frame or two old)
	const FLPVGIRSMState Rsm = LPVGI_GetRSMState(CascadeIndex);
	FRHITexture* RsmRHI = (Ctx.bRSMMode && Rsm.bValid && Rsm.Resource != nullptr) ? Rsm.Resource->GetRenderTargetTexture() : nullptr;
	const bool bUseRSM = (RsmRHI != nullptr);
	const bool bFeedback = bValidHistory && Ctx.Feedback > 0.0f && bUseRSM;

	if (bUseRSM)
	{
		FRDGTextureRef BlackVol = RegisterExternalTexture(GraphBuilder, GBlackVolumeTexture->TextureRHI, TEXT("LPVGI.BlackVol"));
		FRDGTextureRef RSMTexture = RegisterExternalTexture(GraphBuilder, RsmRHI, TEXT("LPVGI.RSM"));

		FLPVGIInjectRSMCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIInjectRSMCS::FParameters>();
		Params->RSMTexture = RSMTexture;
		Params->RWAccum = AccumUAV;
		Params->FeedbackR = bFeedback ? HistR : BlackVol;
		Params->FeedbackG = bFeedback ? HistG : BlackVol;
		Params->FeedbackB = bFeedback ? HistB : BlackVol;
		Params->CaptureOrigin = Rsm.CaptureOrigin;
		Params->AxisRight = Rsm.AxisRight;
		Params->AxisUp = Rsm.AxisUp;
		Params->AxisForward = Rsm.AxisForward;
		Params->VolumeMin = VolumeMin;
		Params->SunColor = Ctx.SunSky.bValidSun ? Ctx.SunSky.SunColor : FVector3f(1.0f, 1.0f, 1.0f);
		Params->OrthoWidth = Rsm.OrthoWidth;
		Params->MaxDepth = Rsm.MaxDepth;
		Params->CellSize = CellSize;
		Params->FixedPointScale = GLPVGIFixedPointScale;
		Params->Feedback = Ctx.Feedback;
		Params->HasFeedback = bFeedback ? 1 : 0;
		Params->Resolution = Rsm.Resolution;

		TShaderMapRef<FLPVGIInjectRSMCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector GroupCount(FMath::DivideAndRoundUp(Rsm.Resolution, 8), FMath::DivideAndRoundUp(Rsm.Resolution, 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::InjectRSM"), ComputeShader, Params, GroupCount);
	}
	else
	{
		FLPVGIInjectCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIInjectCS::FParameters>();
		Params->View = Ctx.SceneView->ViewUniformBuffer;
		Params->RWAccum = AccumUAV;
		Params->SceneDepthTexture = Ctx.Resources->SceneDepthZ;
		Params->GBufferATexture = Ctx.Resources->GBufferA;
		Params->GBufferCTexture = Ctx.Resources->GBufferC;
		Params->SunDirection = Ctx.SunSky.SunDirection;
		Params->SunColor = Ctx.SunSky.bValidSun ? Ctx.SunSky.SunColor : FVector3f::ZeroVector;
		Params->SkyColor = Ctx.SunSky.bValidSky ? Ctx.SunSky.SkyColor : FVector3f::ZeroVector;
		Params->VolumeSnapOffset = SnapOffset;
		Params->CellSize = CellSize;
		Params->FixedPointScale = GLPVGIFixedPointScale;
		Params->ViewportSize = Ctx.ViewportSize;

		TShaderMapRef<FLPVGIInjectCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector GroupCount(FMath::DivideAndRoundUp(Ctx.ViewportSize.X, 8), FMath::DivideAndRoundUp(Ctx.ViewportSize.Y, 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::Inject"), ComputeShader, Params, GroupCount);
	}

	{
		FLPVGIConvertCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIConvertCS::FParameters>();
		Params->AccumTexture = AccumInt;
		Params->RWCurR = GraphBuilder.CreateUAV(VolA[0]);
		Params->RWCurG = GraphBuilder.CreateUAV(VolA[1]);
		Params->RWCurB = GraphBuilder.CreateUAV(VolA[2]);
		Params->RWAccumR = GraphBuilder.CreateUAV(AccA[0]);
		Params->RWAccumG = GraphBuilder.CreateUAV(AccA[1]);
		Params->RWAccumB = GraphBuilder.CreateUAV(AccA[2]);
		Params->RWGeometryVolume = GraphBuilder.CreateUAV(GeometryVolume);
		Params->InvFixedPointScale = 1.0f / GLPVGIFixedPointScale;

		TShaderMapRef<FLPVGIConvertCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::Convert"), ComputeShader, Params, VolumeGroupCount());
	}

	// replace the injected geometry volume with a denser one from the global sdf
	if (Ctx.bUseSDFOcclusion)
	{
		FLPVGIBuildGVCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIBuildGVCS::FParameters>();
		Params->View = Ctx.SceneView->ViewUniformBuffer;
		Params->GlobalDistanceFieldParameters = Ctx.SDFParams;
		Params->RWGeometryVolume = GraphBuilder.CreateUAV(GeometryVolume);
		Params->VolumeSnapOffset = SnapOffset;
		Params->CellSize = CellSize;
		Params->Thickness = Ctx.OcclusionThickness;

		TShaderMapRef<FLPVGIBuildGVCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::BuildGV"), ComputeShader, Params, VolumeGroupCount());
	}

	FRDGTextureRef Cur[3]     = { VolA[0], VolA[1], VolA[2] };
	FRDGTextureRef Next[3]    = { VolB[0], VolB[1], VolB[2] };
	FRDGTextureRef Acc[3]     = { AccA[0], AccA[1], AccA[2] };
	FRDGTextureRef AccNext[3] = { AccB[0], AccB[1], AccB[2] };

	for (int32 Step = 0; Step < Ctx.PropagationSteps; ++Step)
	{
		FLPVGIPropagateCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGIPropagateCS::FParameters>();
		Params->CurR = Cur[0]; Params->CurG = Cur[1]; Params->CurB = Cur[2];
		Params->GeometryVolume = GeometryVolume;
		Params->AccumPrevR = Acc[0]; Params->AccumPrevG = Acc[1]; Params->AccumPrevB = Acc[2];
		Params->RWNextR = GraphBuilder.CreateUAV(Next[0]); Params->RWNextG = GraphBuilder.CreateUAV(Next[1]); Params->RWNextB = GraphBuilder.CreateUAV(Next[2]);
		Params->RWAccumNextR = GraphBuilder.CreateUAV(AccNext[0]); Params->RWAccumNextG = GraphBuilder.CreateUAV(AccNext[1]); Params->RWAccumNextB = GraphBuilder.CreateUAV(AccNext[2]);
		Params->UseOcclusion = Ctx.UseOcclusion;
		Params->OcclusionStrength = Ctx.OcclusionStrength;

		TShaderMapRef<FLPVGIPropagateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::Propagate(%d)", Step), ComputeShader, Params, VolumeGroupCount());

		for (int32 ch = 0; ch < 3; ++ch) { Swap(Cur[ch], Next[ch]); Swap(Acc[ch], AccNext[ch]); }
	}

	FRDGTextureRef FinalAccum[3] = { Acc[0], Acc[1], Acc[2] };

	if (Ctx.bDoTemporal)
	{
		FVector3f CellShift(0.0f, 0.0f, 0.0f);
		if (bValidHistory)
		{
			const FVector ShiftWorld = (SnappedCenter - PrevSnappedCenter[CascadeIndex]) / (double)CellSize;
			CellShift = FVector3f((float)ShiftWorld.X, (float)ShiftWorld.Y, (float)ShiftWorld.Z);
		}

		FRDGTextureRef Blended[3] = { CreateVolume(GraphBuilder, TEXT("LPVGI.Blended.R")), CreateVolume(GraphBuilder, TEXT("LPVGI.Blended.G")), CreateVolume(GraphBuilder, TEXT("LPVGI.Blended.B")) };

		FLPVGITemporalCS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGITemporalCS::FParameters>();
		Params->ThisR = FinalAccum[0]; Params->ThisG = FinalAccum[1]; Params->ThisB = FinalAccum[2];
		Params->HistoryR = bValidHistory ? HistR : FinalAccum[0];
		Params->HistoryG = bValidHistory ? HistG : FinalAccum[1];
		Params->HistoryB = bValidHistory ? HistB : FinalAccum[2];
		Params->HistorySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->RWOutR = GraphBuilder.CreateUAV(Blended[0]); Params->RWOutG = GraphBuilder.CreateUAV(Blended[1]); Params->RWOutB = GraphBuilder.CreateUAV(Blended[2]);
		Params->CellShift = CellShift;
		Params->Alpha = Ctx.TemporalAlpha;
		Params->HasHistory = bValidHistory ? 1 : 0;

		TShaderMapRef<FLPVGITemporalCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LPVGI::Temporal"), ComputeShader, Params, VolumeGroupCount());

		OutVolumes[0] = Blended[0]; OutVolumes[1] = Blended[1]; OutVolumes[2] = Blended[2];

		GraphBuilder.QueueTextureExtraction(Blended[0], &HistoryVolume[H + 0]);
		GraphBuilder.QueueTextureExtraction(Blended[1], &HistoryVolume[H + 1]);
		GraphBuilder.QueueTextureExtraction(Blended[2], &HistoryVolume[H + 2]);
		PrevSnappedCenter[CascadeIndex] = SnappedCenter;
		PrevCellSize[CascadeIndex] = CellSize;
		bHasHistory[CascadeIndex] = true;
	}
	else
	{
		OutVolumes[0] = FinalAccum[0]; OutVolumes[1] = FinalAccum[1]; OutVolumes[2] = FinalAccum[2];
		bHasHistory[CascadeIndex] = false;
		HistoryVolume[H + 0].SafeRelease();
		HistoryVolume[H + 1].SafeRelease();
		HistoryVolume[H + 2].SafeRelease();
	}
}

void FLightPropagationVolumesGIModule::OnRenderDiffuseIndirectLight(
	const FScene& /*Scene*/,
	const FViewInfo& View,
	FRDGBuilder& GraphBuilder,
	FGlobalIlluminationPluginResources& Resources)
{
	if (CVarLPVGIEnable.GetValueOnRenderThread() == 0)
	{
		return;
	}
	if (!Resources.SceneColor || !Resources.SceneDepthZ || !Resources.GBufferA || !Resources.GBufferC)
	{
		return;
	}

	// fviewinfo's first base is fsceneview so reading it as one is fine, gives us
	// the view uniform buffer + matrices without any private headers
	const FSceneView& SceneView = reinterpret_cast<const FSceneView&>(View);

	// skip captures (incl our own rsm capture) so we don't recurse or feed gi back into the flux
	if (SceneView.bIsSceneCapture || SceneView.bIsReflectionCapture)
	{
		return;
	}

	const FLPVGISunSkyState SunSky = LPVGI_GetSunSkyState();
	const float BaseCellSize = FMath::Max(1.0f, CVarLPVGICellSize.GetValueOnRenderThread());
	const FIntPoint ViewportSize = Resources.SceneColor->Desc.Extent;
	const int32 DebugView = CVarLPVGIDebugView.GetValueOnRenderThread();
	const int32 InjectMode = CVarLPVGIInjectMode.GetValueOnRenderThread();
	const int32 NumCascadesReq = FMath::Clamp(CVarLPVGINumCascades.GetValueOnRenderThread(), 1, MaxCascades);
	const bool bTemporalEnabled = CVarLPVGITemporal.GetValueOnRenderThread() != 0;

	const FVector CamWorld = SceneView.ViewMatrices.GetViewOrigin();
	auto SnapToGrid = [](const FVector& P, double Cell)
	{
		return FVector(FMath::FloorToDouble(P.X / Cell) * Cell, FMath::FloorToDouble(P.Y / Cell) * Cell, FMath::FloorToDouble(P.Z / Cell) * Cell);
	};

	const int32 NumCascades = (InjectMode != 0) ? NumCascadesReq : 1;

	// each cascade gets its own rsm sized to it, captured one per frame (round robin).
	// tell the game thread where to aim each
	FLPVGIVolumeFocus Focuses[MaxCascades];
	for (int32 c = 0; c < NumCascades; ++c)
	{
		const float CellSizeC = BaseCellSize * (float)(1 << c);
		Focuses[c].WorldCenter = SnapToGrid(CamWorld, (double)CellSizeC);
		Focuses[c].WorldExtent = GLPVGIGridSize * CellSizeC;
		Focuses[c].bValid = true;
	}
	LPVGI_SetVolumeFocus(Focuses, NumCascades);
	LPVGI_SetRSMResolution(CVarLPVGIRSMResolution.GetValueOnRenderThread());

	RDG_EVENT_SCOPE(GraphBuilder, "LPVGI");

	FLPVGICascadeContext Ctx;
	Ctx.SceneView = &SceneView;
	Ctx.Resources = &Resources;
	Ctx.ViewportSize = ViewportSize;
	Ctx.SunSky = SunSky;
	Ctx.bRSMMode = (InjectMode != 0);
	Ctx.PropagationSteps = FMath::Clamp(CVarLPVGIPropagationSteps.GetValueOnRenderThread(), 0, 64);
	Ctx.UseOcclusion = (CVarLPVGIOcclusion.GetValueOnRenderThread() != 0) ? 1 : 0;
	Ctx.OcclusionStrength = CVarLPVGIOcclusionStrength.GetValueOnRenderThread();

	// occlusion from the global sdf, all public api
	const FGlobalDistanceFieldParameterData* SDFData =
		(Ctx.UseOcclusion != 0 && CVarLPVGIOcclusionSDF.GetValueOnRenderThread() != 0)
		? UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData(MakeConstStridedView<FSceneView>((int32)sizeof(FSceneView), &SceneView, 1))
		: nullptr;
	Ctx.bUseSDFOcclusion = (SDFData != nullptr);
	if (Ctx.bUseSDFOcclusion)
	{
		// the full Setup isn't exported so use the inline _Minimal one, then fill in
		// the samplers it doesn't set or the atlas sampling reads garbage
		Ctx.SDFParams = SetupGlobalDistanceFieldParameters_Minimal(*SDFData);
		FRHISamplerState* SDFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Ctx.SDFParams.GlobalDistanceFieldPageAtlasTextureSampler = SDFSampler;
		Ctx.SDFParams.GlobalDistanceFieldCoverageAtlasTextureSampler = SDFSampler;
		Ctx.SDFParams.GlobalDistanceFieldMipTextureSampler = SDFSampler;
		if (!Ctx.SDFParams.GlobalDistanceFieldCoverageAtlasTexture)
		{
			Ctx.SDFParams.GlobalDistanceFieldCoverageAtlasTexture = GBlackVolumeTexture->TextureRHI;
		}
	}
	Ctx.OcclusionThickness = CVarLPVGIOcclusionThickness.GetValueOnRenderThread();
	Ctx.Feedback = FMath::Clamp(CVarLPVGIFeedback.GetValueOnRenderThread(), 0.0f, 0.95f);

	Ctx.bDoTemporal = bTemporalEnabled;
	Ctx.TemporalAlpha = FMath::Clamp(CVarLPVGITemporalBlend.GetValueOnRenderThread(), 0.01f, 1.0f);

	FRDGTextureRef CascadeVolumes[MaxCascades][3] = {};
	FVector4f CascadeParams[MaxCascades];
	for (int32 c = 0; c < MaxCascades; ++c)
	{
		CascadeParams[c] = FVector4f(0.0f, 0.0f, 0.0f, BaseCellSize);
	}

	for (int32 c = 0; c < NumCascades; ++c)
	{
		const float CellSize = BaseCellSize * (float)(1 << c);
		const FVector Center = SnapToGrid(CamWorld, (double)CellSize);
		const FVector DeltaW = Center - CamWorld;
		const FVector3f SnapOffset((float)DeltaW.X, (float)DeltaW.Y, (float)DeltaW.Z);
		const float Extent = GLPVGIGridSize * CellSize;
		const FVector MinW = Center - FVector(0.5 * Extent);
		const FVector3f VolumeMin((float)MinW.X, (float)MinW.Y, (float)MinW.Z);

		FRDGTextureRef Volumes[3] = {};
		RenderCascade(GraphBuilder, c, CellSize, Center, SnapOffset, VolumeMin, Ctx, Volumes);

		CascadeVolumes[c][0] = Volumes[0];
		CascadeVolumes[c][1] = Volumes[1];
		CascadeVolumes[c][2] = Volumes[2];
		CascadeParams[c] = FVector4f(SnapOffset.X, SnapOffset.Y, SnapOffset.Z, CellSize);
	}

	// fill the leftover cascade slots with the last valid one, they never get sampled anyway
	for (int32 c = NumCascades; c < MaxCascades; ++c)
	{
		CascadeVolumes[c][0] = CascadeVolumes[NumCascades - 1][0];
		CascadeVolumes[c][1] = CascadeVolumes[NumCascades - 1][1];
		CascadeVolumes[c][2] = CascadeVolumes[NumCascades - 1][2];
		CascadeParams[c] = CascadeParams[NumCascades - 1];
	}

	// lookup + composite into scene color
	{
		FLPVGILookupPS::FParameters* Params = GraphBuilder.AllocParameters<FLPVGILookupPS::FParameters>();
		Params->View = SceneView.ViewUniformBuffer;
		Params->SceneDepthTexture = Resources.SceneDepthZ;
		Params->GBufferATexture = Resources.GBufferA;
		Params->GBufferBTexture = Resources.GBufferB;
		Params->GBufferCTexture = Resources.GBufferC;
		Params->Volume0R = CascadeVolumes[0][0]; Params->Volume0G = CascadeVolumes[0][1]; Params->Volume0B = CascadeVolumes[0][2];
		Params->Volume1R = CascadeVolumes[1][0]; Params->Volume1G = CascadeVolumes[1][1]; Params->Volume1B = CascadeVolumes[1][2];
		Params->Volume2R = CascadeVolumes[2][0]; Params->Volume2G = CascadeVolumes[2][1]; Params->Volume2B = CascadeVolumes[2][2];
		Params->Volume3R = CascadeVolumes[3][0]; Params->Volume3G = CascadeVolumes[3][1]; Params->Volume3B = CascadeVolumes[3][2];
		Params->VolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		for (int32 c = 0; c < MaxCascades; ++c)
		{
			Params->CascadeParams[c] = CascadeParams[c];
		}
		Params->NumCascades = NumCascades;
		Params->EdgeFadeFrac = FMath::Clamp(CVarLPVGIEdgeFade.GetValueOnRenderThread(), 0.0f, 0.49f);
		Params->Intensity = CVarLPVGIIntensity.GetValueOnRenderThread();
		Params->SpecularIntensity = FMath::Max(0.0f, CVarLPVGISpecular.GetValueOnRenderThread());
		Params->DebugView = DebugView;
		Params->RenderTargets[0] = FRenderTargetBinding(Resources.SceneColor, ERenderTargetLoadAction::ELoad);

		const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FLPVGILookupPS> PixelShader(GlobalShaderMap);

		// debug views overwrite scene color, normal path adds onto it
		FRHIBlendState* Blend = (DebugView == 0)
			? TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_One>::GetRHI()
			: TStaticBlendState<>::GetRHI();

		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GlobalShaderMap,
			RDG_EVENT_NAME("LPVGI::Lookup"),
			PixelShader,
			Params,
			FIntRect(0, 0, ViewportSize.X, ViewportSize.Y),
			Blend);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLightPropagationVolumesGIModule, LightPropagationVolumesGI)
