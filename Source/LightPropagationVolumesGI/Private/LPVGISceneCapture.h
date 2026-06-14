// Copyright (c) LPVPGI contributors. Open source (see LICENSE).
#pragma once

#include "CoreMinimal.h"

class FTextureRenderTargetResource;

// must match MaxCascades in the module
static constexpr int32 LPVGI_MAX_CASCADES = 4;

// sun + sky grabbed on the game thread, read by the inject pass on the render thread
struct FLPVGISunSkyState
{
	FVector3f SunDirection = FVector3f(0.0f, 0.0f, -1.0f); // direction light travels
	FVector3f SunColor = FVector3f::ZeroVector;            // linear, ~unit (no lux scaling)
	FVector3f SkyColor = FVector3f::ZeroVector;            // linear ambient approximation
	bool bValidSun = false;
	bool bValidSky = false;
};

// rsm capture output for one cascade, game thread -> render thread.
// each cascade has its own capture aimed along the sun, so it samples at the right
// density. the render thread rebuilds each texel's world pos from this basis and injects
struct FLPVGIRSMState
{
	FTextureRenderTargetResource* Resource = nullptr; // RGBA16f: RGB=flux, A=linear depth (cm)
	FVector3f CaptureOrigin = FVector3f::ZeroVector;
	FVector3f AxisRight = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f AxisUp = FVector3f(0.0f, 1.0f, 0.0f);
	FVector3f AxisForward = FVector3f(0.0f, 0.0f, 1.0f); // == sun travel direction
	float OrthoWidth = 1000.0f;
	float MaxDepth = 8000.0f;
	int32 Resolution = 512;
	bool bValid = false;
};

// where the render thread wants each cascade's capture centered (follows the camera)
struct FLPVGIVolumeFocus
{
	FVector WorldCenter = FVector::ZeroVector;
	float WorldExtent = 1280.0f;
	bool bValid = false;
};

void LPVGI_StartSceneCapture();
void LPVGI_StopSceneCapture();

FLPVGISunSkyState LPVGI_GetSunSkyState();

// render thread tells the game thread where each cascade should be aimed this frame.
// the game thread captures one cascade per frame (round robin), reusing the rest
void LPVGI_SetVolumeFocus(const FLPVGIVolumeFocus* Focuses, int32 NumCascades);

// render thread reads the latest capture for a given cascade
FLPVGIRSMState LPVGI_GetRSMState(int32 CascadeIndex);

// rsm resolution, picked up next time the rts are (re)created
void LPVGI_SetRSMResolution(int32 Resolution);
