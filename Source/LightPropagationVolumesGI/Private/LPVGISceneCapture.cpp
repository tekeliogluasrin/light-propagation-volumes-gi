// Copyright (c) LPVPGI contributors. Open source (see LICENSE).

#include "LPVGISceneCapture.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Camera/CameraTypes.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogLPVGICapture, Log, All);

static FCriticalSection GLPVGIStateLock;
static FLPVGISunSkyState GLPVGISunSky;
static FLPVGIVolumeFocus GLPVGIFocus[LPVGI_MAX_CASCADES];
static int32 GLPVGINumCascades = 0;
static FLPVGIRSMState GLPVGIRSM[LPVGI_MAX_CASCADES];
static int32 GLPVGIRSMResolution = 512;

static FTSTicker::FDelegateHandle GLPVGITickerHandle;
static FDelegateHandle GLPVGIWorldCleanupHandle;
static TWeakObjectPtr<ASceneCapture2D> GLPVGICaptureActor;
static UTextureRenderTarget2D* GLPVGIRT[LPVGI_MAX_CASCADES] = {};
static int32 GLPVGICurrentRTResolution = 0;
static int32 GLPVGIUpdateCursor = 0;

static UWorld* LPVGI_PickWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}

	UWorld* EditorWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
		{
			if (UWorld* World = Context.World())
			{
				return World;
			}
		}
		else if (Context.WorldType == EWorldType::Editor && EditorWorld == nullptr)
		{
			EditorWorld = Context.World();
		}
	}
	return EditorWorld;
}

static void LPVGI_UpdateSunSky(UWorld* World, FLPVGISunSkyState& OutState)
{
	const ADirectionalLight* BestSun = nullptr;
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		const UDirectionalLightComponent* Comp = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		if (!Comp || !Comp->IsVisible())
		{
			continue;
		}
		BestSun = *It;
		if (Comp->IsUsedAsAtmosphereSunLight())
		{
			break;
		}
	}
	if (BestSun)
	{
		const UDirectionalLightComponent* Comp = Cast<UDirectionalLightComponent>(BestSun->GetLightComponent());
		const FLinearColor Color = Comp->GetLightColor();
		OutState.SunDirection = (FVector3f)Comp->GetDirection().GetSafeNormal();
		OutState.SunColor = FVector3f(Color.R, Color.G, Color.B);
		OutState.bValidSun = true;
	}

	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		const USkyLightComponent* Comp = It->GetLightComponent();
		if (!Comp || !Comp->IsVisible())
		{
			continue;
		}
		const FLinearColor Color = Comp->GetLightColor() * Comp->Intensity;
		OutState.SkyColor = FVector3f(Color.R, Color.G, Color.B);
		OutState.bValidSky = true;
		break;
	}
}

static void LPVGI_ReleaseRenderTargets()
{
	for (int32 c = 0; c < LPVGI_MAX_CASCADES; ++c)
	{
		if (GLPVGIRT[c])
		{
			GLPVGIRT[c]->RemoveFromRoot();
			GLPVGIRT[c] = nullptr;
		}
	}
	GLPVGICurrentRTResolution = 0;
}

static ASceneCapture2D* LPVGI_EnsureCapture(UWorld* World, int32 NumCascades, int32 Resolution)
{
	ASceneCapture2D* Actor = GLPVGICaptureActor.Get();
	if (!Actor || Actor->GetWorld() != World)
	{
		if (Actor)
		{
			Actor->Destroy();
		}
		LPVGI_ReleaseRenderTargets();

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Actor = World->SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		GLPVGICaptureActor = Actor;

		if (USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D())
		{
			Capture->CaptureSource = SCS_SceneColorSceneDepth;
			Capture->ProjectionType = ECameraProjectionMode::Orthographic;
			Capture->bCaptureEveryFrame = true;
			Capture->bCaptureOnMovement = false;
			Capture->bAlwaysPersistRenderingState = true;
			Capture->bRenderInMainRenderer = false;
			Capture->bAutoCalculateOrthoPlanes = true;
		}
	}

	// one render target per cascade, rooted so GC keeps them alive
	if (GLPVGICurrentRTResolution != Resolution)
	{
		LPVGI_ReleaseRenderTargets();
	}
	for (int32 c = 0; c < NumCascades; ++c)
	{
		if (!GLPVGIRT[c])
		{
			// outer to the transient package, NOT the world actor, otherwise the rooted
			// rt keeps the actor (and its world) alive and the editor flags a world leak
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RT->RenderTargetFormat = RTF_RGBA16f;
			RT->ClearColor = FLinearColor::Black;
			RT->bAutoGenerateMips = false;
			RT->InitCustomFormat(Resolution, Resolution, PF_FloatRGBA, /*bForceLinearGamma=*/true);
			RT->UpdateResourceImmediate(true);
			RT->AddToRoot();
			GLPVGIRT[c] = RT;
		}
	}
	GLPVGICurrentRTResolution = Resolution;
	return Actor;
}

static bool LPVGI_Tick(float /*DeltaTime*/)
{
	UWorld* World = LPVGI_PickWorld();
	if (!World)
	{
		return true;
	}

	FLPVGISunSkyState SunSky;
	LPVGI_UpdateSunSky(World, SunSky);

	FLPVGIVolumeFocus Focuses[LPVGI_MAX_CASCADES];
	int32 NumCascades;
	int32 Resolution;
	{
		FScopeLock Lock(&GLPVGIStateLock);
		GLPVGISunSky = SunSky;
		for (int32 c = 0; c < LPVGI_MAX_CASCADES; ++c)
		{
			Focuses[c] = GLPVGIFocus[c];
		}
		NumCascades = FMath::Clamp(GLPVGINumCascades, 0, LPVGI_MAX_CASCADES);
		Resolution = FMath::Clamp(GLPVGIRSMResolution, 64, 2048);
	}

	if (NumCascades <= 0 || !SunSky.bValidSun)
	{
		return true;
	}

	ASceneCapture2D* Actor = LPVGI_EnsureCapture(World, NumCascades, Resolution);
	if (!Actor)
	{
		return true;
	}
	USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();

	// capture one cascade per frame, round robin
	const int32 K = GLPVGIUpdateCursor % NumCascades;
	GLPVGIUpdateCursor++;
	if (!Focuses[K].bValid || !GLPVGIRT[K])
	{
		return true;
	}

	const FVector Forward = ((FVector)SunSky.SunDirection).GetSafeNormal();
	const FRotator Rot = Forward.Rotation();
	const FVector Right = Rot.RotateVector(FVector::RightVector);
	const FVector Up = Rot.RotateVector(FVector::UpVector);

	const float Extent = FMath::Max(100.0f, Focuses[K].WorldExtent);
	const float OrthoWidth = Extent * 1.8f;
	const float Back = Extent * 1.0f;
	const FVector Origin = Focuses[K].WorldCenter - Forward * Back;

	Actor->SetActorLocationAndRotation(Origin, Rot);
	Capture->OrthoWidth = OrthoWidth;
	Capture->TextureTarget = GLPVGIRT[K];

	FLPVGIRSMState RSM;
	RSM.Resource = GLPVGIRT[K]->GameThread_GetRenderTargetResource();
	RSM.CaptureOrigin = (FVector3f)Origin;
	RSM.AxisRight = (FVector3f)Right;
	RSM.AxisUp = (FVector3f)Up;
	RSM.AxisForward = (FVector3f)Forward;
	RSM.OrthoWidth = OrthoWidth;
	RSM.MaxDepth = Extent * 2.2f;
	RSM.Resolution = Resolution;
	RSM.bValid = (RSM.Resource != nullptr);

	{
		FScopeLock Lock(&GLPVGIStateLock);
		GLPVGIRSM[K] = RSM;
	}
	return true;
}

// release everything when a world is torn down (level change / editor close), so we
// don't keep it alive and trip the editor's world leak check
static void LPVGI_OnWorldCleanup(UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/)
{
	ASceneCapture2D* Actor = GLPVGICaptureActor.Get();
	if (Actor && Actor->GetWorld() == World)
	{
		Actor->Destroy();
		GLPVGICaptureActor.Reset();
		LPVGI_ReleaseRenderTargets();

		FScopeLock Lock(&GLPVGIStateLock);
		GLPVGINumCascades = 0;
		GLPVGIUpdateCursor = 0;
		for (int32 c = 0; c < LPVGI_MAX_CASCADES; ++c)
		{
			GLPVGIRSM[c] = FLPVGIRSMState();
		}
	}
}

void LPVGI_StartSceneCapture()
{
	if (!GLPVGITickerHandle.IsValid())
	{
		GLPVGITickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&LPVGI_Tick), 0.0f);
	}
	if (!GLPVGIWorldCleanupHandle.IsValid())
	{
		GLPVGIWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&LPVGI_OnWorldCleanup);
	}
}

void LPVGI_StopSceneCapture()
{
	if (GLPVGITickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GLPVGITickerHandle);
		GLPVGITickerHandle.Reset();
	}
	if (GLPVGIWorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(GLPVGIWorldCleanupHandle);
		GLPVGIWorldCleanupHandle.Reset();
	}
	if (ASceneCapture2D* Actor = GLPVGICaptureActor.Get())
	{
		Actor->Destroy();
	}
	GLPVGICaptureActor.Reset();
	LPVGI_ReleaseRenderTargets();
}

FLPVGISunSkyState LPVGI_GetSunSkyState()
{
	FScopeLock Lock(&GLPVGIStateLock);
	return GLPVGISunSky;
}

void LPVGI_SetVolumeFocus(const FLPVGIVolumeFocus* Focuses, int32 NumCascades)
{
	FScopeLock Lock(&GLPVGIStateLock);
	GLPVGINumCascades = FMath::Clamp(NumCascades, 0, LPVGI_MAX_CASCADES);
	for (int32 c = 0; c < GLPVGINumCascades; ++c)
	{
		GLPVGIFocus[c] = Focuses[c];
	}
}

FLPVGIRSMState LPVGI_GetRSMState(int32 CascadeIndex)
{
	FScopeLock Lock(&GLPVGIStateLock);
	if (CascadeIndex < 0 || CascadeIndex >= LPVGI_MAX_CASCADES)
	{
		return FLPVGIRSMState();
	}
	return GLPVGIRSM[CascadeIndex];
}

void LPVGI_SetRSMResolution(int32 Resolution)
{
	FScopeLock Lock(&GLPVGIStateLock);
	GLPVGIRSMResolution = Resolution;
}
