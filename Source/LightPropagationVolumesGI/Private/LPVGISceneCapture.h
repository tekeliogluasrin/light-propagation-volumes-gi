// Copyright (c) LPVPGI contributors. Open source (see LICENSE).
#pragma once

#include "CoreMinimal.h"

class FTextureRenderTargetResource;

// sun + sky grabbed on the game thread, read by the inject pass on the render thread
struct FLPVGISunSkyState
{
	FVector3f SunDirection = FVector3f(0.0f, 0.0f, -1.0f); // direction light travels
	FVector3f SunColor = FVector3f::ZeroVector;            // linear, ~unit (no lux scaling)
	FVector3f SkyColor = FVector3f::ZeroVector;            // linear ambient approximation
	bool bValidSun = false;
	bool bValidSky = false;
};

// rsm capture output, game thread -> render thread.
// we run an ortho scenecapture2d aimed along the sun, grabbing lit color (= flux) + depth.
// the render thread rebuilds each texel's world pos from this basis and injects it
struct FLPVGIRSMState
{
	FTextureRenderTargetResource* Resource = nullptr; // RGBA16f: RGB=flux, A=linear depth (cm)
	FVector3f CaptureOrigin = FVector3f::ZeroVector;  // world-space capture camera location
	FVector3f AxisRight = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f AxisUp = FVector3f(0.0f, 1.0f, 0.0f);
	FVector3f AxisForward = FVector3f(0.0f, 0.0f, 1.0f); // == sun travel direction
	float OrthoWidth = 1000.0f;   // world size covered (square)
	float MaxDepth = 8000.0f;     // far cutoff (cm) for background rejection
	int32 Resolution = 512;
	bool bValid = false;
};

// where the render thread wants the capture centered (follows the camera)
struct FLPVGIVolumeFocus
{
	FVector WorldCenter = FVector::ZeroVector;
	float WorldExtent = 1280.0f; // GridSize * CellSize
	bool bValid = false;
};

void LPVGI_StartSceneCapture();
void LPVGI_StopSceneCapture();

FLPVGISunSkyState LPVGI_GetSunSkyState();

// render thread tells the game thread where to aim the capture this frame
void LPVGI_SetVolumeFocus(const FLPVGIVolumeFocus& Focus);

// render thread reads the latest capture basis + target
FLPVGIRSMState LPVGI_GetRSMState();

// rsm resolution, picked up next time the rt is (re)created
void LPVGI_SetRSMResolution(int32 Resolution);
