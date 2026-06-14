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
static FLPVGIRSMState GLPVGIRSM;
static FLPVGIVolumeFocus GLPVGIFocus;
static int32 GLPVGIRSMResolution = 512;

static FTSTicker::FDelegateHandle GLPVGITickerHandle;
static TWeakObjectPtr<ASceneCapture2D> GLPVGICaptureActor;
static int32 GLPVGICurrentRTResolution = 0;

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

static ASceneCapture2D* LPVGI_EnsureCaptureActor(UWorld* World, int32 DesiredResolution)
{
	ASceneCapture2D* Actor = GLPVGICaptureActor.Get();
	const bool bNeedNew = !Actor || Actor->GetWorld() != World;

	if (bNeedNew)
	{
		if (Actor)
		{
			Actor->Destroy();
			GLPVGICaptureActor.Reset();
		}

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Actor = World->SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		GLPVGICaptureActor = Actor;
		GLPVGICurrentRTResolution = 0; // force RT (re)creation below
	}

	USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();
	if (!Capture)
	{
		return nullptr;
	}

	// remake the rt if the resolution changed
	UTextureRenderTarget2D* RT = Capture->TextureTarget;
	if (!RT || GLPVGICurrentRTResolution != DesiredResolution)
	{
		RT = NewObject<UTextureRenderTarget2D>(Actor);
		RT->RenderTargetFormat = RTF_RGBA16f;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->InitCustomFormat(DesiredResolution, DesiredResolution, PF_FloatRGBA, /*bForceLinearGamma=*/true);
		RT->UpdateResourceImmediate(true);

		Capture->TextureTarget = RT;
		Capture->CaptureSource = SCS_SceneColorSceneDepth;
		Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		Capture->bCaptureEveryFrame = true;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		// bRenderInMainRenderer (the cheaper path) encodes depth differently and
		// doesn't write TextureTarget reliably, so use the normal separate capture.
		// that way depth is plain CalcSceneDepth in cm which is what we reconstruct with.
		// could revisit for perf
		Capture->bRenderInMainRenderer = false;
		Capture->bAutoCalculateOrthoPlanes = true;

		GLPVGICurrentRTResolution = DesiredResolution;
	}

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

	FLPVGIVolumeFocus Focus;
	int32 Resolution;
	{
		FScopeLock Lock(&GLPVGIStateLock);
		GLPVGISunSky = SunSky;
		Focus = GLPVGIFocus;
		Resolution = FMath::Clamp(GLPVGIRSMResolution, 64, 2048);
	}

	FLPVGIRSMState RSM;
	RSM.bValid = false;

	// need a focus (camera pos from the render thread) and a sun before we can capture
	if (Focus.bValid && SunSky.bValidSun)
	{
		if (ASceneCapture2D* Actor = LPVGI_EnsureCaptureActor(World, Resolution))
		{
			USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();

			const FVector Forward = ((FVector)SunSky.SunDirection).GetSafeNormal();
			const FRotator Rot = Forward.Rotation(); // +X aligned to Forward
			const FVector Right = Rot.RotateVector(FVector::RightVector);
			const FVector Up = Rot.RotateVector(FVector::UpVector);

			// make it wide enough to cover the box diagonal from any sun angle
			// and keep the scene between the near/far planes
			const float Extent = FMath::Max(100.0f, Focus.WorldExtent);
			const float OrthoWidth = Extent * 1.8f;
			const float Back = Extent * 1.0f;
			const FVector Origin = Focus.WorldCenter - Forward * Back;

			Actor->SetActorLocationAndRotation(Origin, Rot);
			Capture->OrthoWidth = OrthoWidth;

			UTextureRenderTarget2D* RT = Capture->TextureTarget;
			RSM.Resource = RT ? RT->GameThread_GetRenderTargetResource() : nullptr;
			RSM.CaptureOrigin = (FVector3f)Origin;
			RSM.AxisRight = (FVector3f)Right;
			RSM.AxisUp = (FVector3f)Up;
			RSM.AxisForward = (FVector3f)Forward;
			RSM.OrthoWidth = OrthoWidth;
			RSM.MaxDepth = Extent * 2.2f;
			RSM.Resolution = Resolution;
			RSM.bValid = (RSM.Resource != nullptr);
		}
	}

	{
		FScopeLock Lock(&GLPVGIStateLock);
		GLPVGIRSM = RSM;
	}
	return true;
}

void LPVGI_StartSceneCapture()
{
	if (!GLPVGITickerHandle.IsValid())
	{
		GLPVGITickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&LPVGI_Tick), 0.0f);
	}
}

void LPVGI_StopSceneCapture()
{
	if (GLPVGITickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GLPVGITickerHandle);
		GLPVGITickerHandle.Reset();
	}
	if (ASceneCapture2D* Actor = GLPVGICaptureActor.Get())
	{
		Actor->Destroy();
	}
	GLPVGICaptureActor.Reset();
}

FLPVGISunSkyState LPVGI_GetSunSkyState()
{
	FScopeLock Lock(&GLPVGIStateLock);
	return GLPVGISunSky;
}

void LPVGI_SetVolumeFocus(const FLPVGIVolumeFocus& Focus)
{
	FScopeLock Lock(&GLPVGIStateLock);
	GLPVGIFocus = Focus;
}

FLPVGIRSMState LPVGI_GetRSMState()
{
	FScopeLock Lock(&GLPVGIStateLock);
	return GLPVGIRSM;
}

void LPVGI_SetRSMResolution(int32 Resolution)
{
	FScopeLock Lock(&GLPVGIStateLock);
	GLPVGIRSMResolution = Resolution;
}
