#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

struct TSSRRData
{
    float2 resolution;
    float2 invResolution;
    float depthSigma = 1.0f;
    uint numMantissaBits = 10;
};

struct MeanVarianceCB
{
    uint2 textureDim;
    uint kernelWidth;
    uint kernelRadius;
};
END_NAMESPACE_FALCOR
