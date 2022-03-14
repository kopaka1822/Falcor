#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

struct RTAOData
{
    float2 noiseScale = float2(1, 1);
    float2 resolution;
    float2 invResolution;
    float radius = 0.5f;
    float exponent = 1.0f;
    float rayNormalOffset = 0.05f;
};

END_NAMESPACE_FALCOR
