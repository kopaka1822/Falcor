#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

struct HBAOData
{
    float2 invResolution; // (1/width, 1/height)
    float2 resolution;
    float radius = 1.0f;
    float negInvRsq = -1.0f;
    float NdotVBias = 0.1f;
    float powerExponent = 2.0f;
    float2 noiseScale = float2(1, 1);
};

END_NAMESPACE_FALCOR
