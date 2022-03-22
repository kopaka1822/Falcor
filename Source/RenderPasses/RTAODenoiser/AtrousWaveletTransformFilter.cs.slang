/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

import RTAODenoiserData;
import RTAODenoiserHelper;
#if defined(USE_GAUSSIAN_3X3)
import GaussianKernel3x3;
#elif defined(USE_GAUSSIAN_5X5)
import GaussianKernel5x5;
#endif //USE_GAUSSIAN

#define PI              3.1415926535897f

cbuffer CB
{
    AtrousWaveletTransformFilterData cb;
}

Texture2D<float> gInValue;
Texture2D<float3> gInNormal;
Texture2D<float> gInDepth;
Texture2D<float> gInVariance;
Texture2D<float> gInRayHitDistance;
Texture2D<float2> gInLinearZ;
Texture2D<float> gInTspp;

RWTexture2D<float> gOutValue;

static const float kInvalidAOCoefficientValue = INVALID_AO_COEFFICIENT_VALUE;
static const float kHitDistanceOnMiss = 0;
static const bool kUsingBilateralDownsampledBuffers = false;    //An option taken out of constant buffer as it is only used if quater res AO image was used as imput (not supported here)

float DepthThreshold(float depth, float2 ddxy, float2 pixelOffset)
{
    float depthThreshold;

    if (cb.perspectiveCorrectDepthInterpolation)
    {
        float2 newDdxy = RemapDdxy(depth, ddxy, pixelOffset);
        depthThreshold = dot(1, abs(newDdxy));
    }
    else
    {
        depthThreshold = dot(1, abs(pixelOffset * ddxy));
    }

    return depthThreshold;
}

void AddFilterContribution(
    inout float weightedValueSum,
    inout float weightSum,
    in float value,
    in float stdDeviation,
    in float depth,
    in float3 normal,
    in float2 ddxy,
    in uint row,
    in uint col,
    in uint2 kernelStep,
    in uint2 DTid)
{
    const float valueSigma = cb.valueSigma;
    const float normalSigma = cb.normalSigma;
    const float depthSigma = cb.depthSigma;
 
    int2 pixelOffset;
    float kernelWidth;
    float varianceScale = 1;

    pixelOffset = int2(row - FilterKernelRadius, col - FilterKernelRadius) * kernelStep;
    int2 id = int2(DTid) + pixelOffset;

    if (IsWithinBounds(id, cb.textureDim))
    {
        float iDepth = gInDepth[id];
        float3 iNormal = gInNormal[id];
        float iValue = gInValue[id];

        bool iIsValidValue = iValue != kInvalidAOCoefficientValue;
        if (!iIsValidValue || iDepth == 0)
        {
            return;
        }

        // Calculate a weight for the neighbor's contribtuion.
        // Ref:[SVGF]
        float w;
        {
            // Value based weight.
            // Lower value tolerance for the neighbors further apart. Prevents overbluring sharp value transitions.
            // Ref: [Dammertz2010]
            const float errorOffset = 0.005f;
            float valueSigmaDistCoef = 1.0 / length(pixelOffset);
            float e_x = -abs(value - iValue) / (valueSigmaDistCoef * valueSigma * stdDeviation + errorOffset);
            float w_x = exp(e_x);

            // Normal based weight.
            float w_n = pow(max(0, dot(normal, iNormal)), normalSigma);

            // Depth based weight.
            float w_d;
            {
                float2 pixelOffsetForDepth = pixelOffset;

                // Account for sample offset in bilateral downsampled partial depth derivative buffer.
                if (kUsingBilateralDownsampledBuffers)
                {
                    float2 offsetSign = sign(pixelOffset);
                    pixelOffsetForDepth = pixelOffset + offsetSign * float2(0.5, 0.5);
                }

                float depthFloatPrecision = FloatPrecision(max(depth, iDepth), cb.DepthNumMantissaBits);
                float depthThreshold = DepthThreshold(depth, ddxy, pixelOffsetForDepth);
                float depthTolerance = depthSigma * depthThreshold + depthFloatPrecision;
                float delta = abs(depth - iDepth);
                delta = max(0, delta - depthFloatPrecision); // Avoid distinguising initial values up to the float precision. Gets rid of banding due to low depth precision format.
                w_d = exp(-delta / depthTolerance);

                // Scale down contributions for samples beyond tolerance, but completely disable contribution for samples too far away.
                w_d *= w_d >= cb.depthWeightCutoff;
            }

            // Filter kernel weight.
            float w_h = FilterKernel[row][col];

            // Final weight.
            w = w_h * w_n * w_x * w_d;
        }

        weightedValueSum += w * iValue;
        weightSum += w;
    }
}

[numthreads(16, 16, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 Gid : SV_GroupID)
{
    if (!IsWithinBounds(DTid, cb.textureDim))
    {
        return;
    }

    // Initialize values to the current pixel / center filter kernel value.
    float value = gInValue[DTid];
    float3 normal = gInNormal[DTid];
    float depth = gInDepth[DTid];
    
    bool isValidValue = value != kInvalidAOCoefficientValue;
    float filteredValue = value;
    float variance = gInVariance[DTid];

    if (depth != kHitDistanceOnMiss)
    {
        float2 ddxy = gInLinearZ[DTid];
        float weightSum = 0;
        float weightedValueSum = 0;
        float stdDeviation = 1;

        if (isValidValue)
        {
            float w = FilterKernel[FilterKernelRadius][FilterKernelRadius];
            weightSum = w;
            weightedValueSum = weightSum * value;
            stdDeviation = sqrt(variance);
        }

        // Adaptive kernel size
        // Scale the kernel span based on AO ray hit distance. 
        // This helps filter out lower frequency noise, a.k.a. boiling artifacts.
        // Ref: [RTGCH19]
        uint2 kernelStep = 0;
        if (cb.useAdaptiveKernelSize && isValidValue)
        {
            float avgRayHitDistance = gInRayHitDistance[DTid];

            float perPixelViewAngle = (cb.fovy / cb.textureDim.y) * PI / 180.0;
            float tan_a = tan(perPixelViewAngle);
            float2 projectedSurfaceDim = ApproximateProjectedSurfaceDimensionsPerPixel(depth, ddxy, tan_a);

            // Calculate a kernel width as a ratio of hitDistance / projected surface dim per pixel.
            // Apply a non-linear factor based on relative rayHitDistance. This is because
            // average ray hit distance grows large fast if the closeby occluders cover only part of the hemisphere.
            // Having a smaller kernel for such cases helps preserve occlusion detail.
            float t = min(avgRayHitDistance / 22.0, 1); // 22 was selected empirically
            float k = cb.rayHitDistanceToKernelWidthScale * pow(t, cb.rayHitDistanceToKernelSizeScaleExponent);
            kernelStep = max(1, round(k * avgRayHitDistance / projectedSurfaceDim));

            uint2 targetKernelStep = clamp(kernelStep, (cb.minKernelWidth - 1) / 2, (cb.maxKernelWidth - 1) / 2);

            // TODO: additional options to explore
            // - non-uniform X, Y kernel radius cause visible streaking. Use same step across both X, Y? That may overblur one dimension at sharp angles.
            // - use larger kernel on lower tspp. 
            // - use varying number of cycles for better spatial coverage over time, depending on the target kernel step. More cycles on larger kernels.
            uint2 adjustedKernelStep = lerp(1, targetKernelStep, cb.kernelRadiusLerfCoef);
            kernelStep = adjustedKernelStep;
        }

        if (variance >= cb.minVarianceToDenoise)
        {
            // Add contributions from the neighborhood.
            [unroll]
            for (uint r = 0; r < FilterKernelWidth; r++)
            [unroll]
                for (uint c = 0; c < FilterKernelWidth; c++)
                    if (r != FilterKernelRadius || c != FilterKernelRadius)
                        AddFilterContribution(
                        weightedValueSum,
                        weightSum,
                        value,
                        stdDeviation,
                        depth,
                        normal,
                        ddxy,
                        r,
                        c,
                        kernelStep,
                        DTid);
        }

        float smallValue = 1e-6f;
        if (weightSum > smallValue)
        {
            filteredValue = weightedValueSum / weightSum;
        }
        else
        {
            filteredValue = kInvalidAOCoefficientValue;
        }
    }

    gOutValue[DTid] = filteredValue;

}
