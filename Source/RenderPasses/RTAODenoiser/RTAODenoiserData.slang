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

struct TSSBlurData
{
    float stdDevGamma = 0.6f;           //0.1 - 10.f
    bool clampCachedValues = true;
    float clamping_minStdDevTolerance = 0.05f;  //0.f - 1.f
    float padding;

    float clampDifferenceToTsppScale = 4.f; //0.f - 10.f
    bool forceUseMinSmoothingFactor = false;
    float minSmoothingFactor = 0.03f;
    uint minTsppToUseTemporalVariance = 4; //1 - 40

    uint blurStrength_MaxTspp = 12;     // 0 - 100
    float blurDecayStrength = 1.0f;     //0.1 - 32
    float2 padding2;
};
END_NAMESPACE_FALCOR
