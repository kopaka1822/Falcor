#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

struct TSSRRData
{
    float2 resolution;
    float2 invResolution;
    float sigma;
    uint numMantissaBits;
};

END_NAMESPACE_FALCOR
