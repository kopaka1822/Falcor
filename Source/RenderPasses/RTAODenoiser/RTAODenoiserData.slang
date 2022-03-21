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
    float stdDevGamma = 0.6f;          
    bool clampCachedValues = true;
    float clamping_minStdDevTolerance = 0.05f;  
    float padding;

    float clampDifferenceToTsppScale = 4.f;
    bool forceUseMinSmoothingFactor = false;
    float minSmoothingFactor = 0.03f;
    uint minTsppToUseTemporalVariance = 4;

    uint blurStrength_MaxTspp = 12;
    float blurDecayStrength = 1.0f;
    float2 padding2;
};

//TODO: Add to UI
struct AtrousWaveletTransformFilterData
{
    uint2 textureDim;
    float depthWeightCutoff = 0.2f; //0.0 - 2.0f
    float fovy = 45.0f;  

    bool useAdaptiveKernelSize = true;
    float kernelRadiusLerfCoef;
    uint minKernelWidth = 3; //3-101
    uint maxKernelWidth;

    float rayHitDistanceToKernelWidthScale;
    float rayHitDistanceToKernelSizeScaleExponent;
    bool perspectiveCorrectDepthInterpolation = true;
    float minVarianceToDenoise = 0.0f;  //0-1.f

    float valueSigma = 1.0f;    //0 - 30f
    float depthSigma = 1.0f;    //0 - 10f;
    float normalSigma = 64.0f; // 0 - 256; 4 Stepsize ; UI value is int in original
    uint DepthNumMantissaBits = 10; //Same as TSSRData
};

END_NAMESPACE_FALCOR
