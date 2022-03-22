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
import CrossBilateralWeights;
import RTAODenoiserHelper;

cbuffer StaticCB
{
    TSSRRData gData;
}


//Input
Texture2D<float4> gNormalTex;
Texture2D<float> gDepthTex;
Texture2D<float2> gLinearDepthTex;
Texture2D<float2> gMVecTex;

//Internal
Texture2D<uint> gInCachedTspp;
Texture2D<float> gInCachedValue;
Texture2D<float> gInCachedValueSquaredMean;
Texture2D<float> gInCachedRayHitDepth;

//Out
RWTexture2D<float4> gInOutCachedNormalDepth;
RWTexture2D<uint> gOutCachedTspp;
RWTexture2D<uint4> gOutReprojectedCachedValues;

SamplerState gSampler;

static const float kInvalidAOCoefficientValue = INVALID_AO_COEFFICIENT_VALUE;

float4 BilateralResampleWeights(in float TargetDepth, in float3 TargetNormal, in float4 SampleDepths, in float3 SampleNormals[4], in float2 TargetOffset, in uint2 TargetIndex, in int2 sampleIndices[4], in float2 Ddxy)
{
    bool4 withinBounds = bool4(
        IsWithinBounds(sampleIndices[0], gData.resolution),
        IsWithinBounds(sampleIndices[1], gData.resolution),
        IsWithinBounds(sampleIndices[2], gData.resolution),
        IsWithinBounds(sampleIndices[3], gData.resolution));

    float4 weight;
    weight = getBilinearWeight(TargetOffset);
    weight *= getNormalWeight(TargetNormal, SampleNormals, 1.1, 32);
    weight *= getDepthWeight(TargetDepth, Ddxy, SampleDepths, gData.depthSigma, 0.5, gData.numMantissaBits);
    
    return weight;
}


[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    
    uint2 dTid = dispatchThreadId.xy;
    float3 normal = gNormalTex[dTid].xyz;
    float depth = gDepthTex[dTid].x;
    float2 mVec = gMVecTex[dTid].xy;

    if (depth == 0 || mVec.x > 1e2f)
    {
        gOutCachedTspp[dTid] = 0;
        return;
    }

    float2 texturePos = (dTid.xy + 0.5f) * gData.invResolution;
    float2 cacheFrameTexturePos = texturePos - mVec;

    // Find the nearest integer index smaller than the texture position.
    // The floor() ensures the that value sign is taken into consideration.
    int2 topLeftCacheFrameIndex = floor(cacheFrameTexturePos * gData.resolution - 0.5);
    float2 adjustedCacheFrameTexturePos = (topLeftCacheFrameIndex + 0.5) * gData.invResolution;

    float2 cachePixelOffset = cacheFrameTexturePos * gData.resolution - 0.5 - topLeftCacheFrameIndex;

    const int2 srcIndexOffsets[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    
    

    int2 cacheIndices[4] =
    {
        topLeftCacheFrameIndex + srcIndexOffsets[0],
        topLeftCacheFrameIndex + srcIndexOffsets[1],
        topLeftCacheFrameIndex + srcIndexOffsets[2],
        topLeftCacheFrameIndex + srcIndexOffsets[3]
    };

    float3 cacheNormals[4];
    float4 vCacheDepths;

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        float4 cacheNormalDepth = gInOutCachedNormalDepth[cacheIndices[i]].xyzw;
        cacheNormals[i] = cacheNormalDepth.xyz;
        vCacheDepths[i] = cacheNormalDepth.w;
    }

    float2 ddxy = gLinearDepthTex[dTid];
    
    float4 weights;
    weights = BilateralResampleWeights(depth, normal, vCacheDepths, cacheNormals, cachePixelOffset, dTid, cacheIndices, ddxy);
    
    float4 vCachedValues = gInCachedValue.GatherRed(gSampler, adjustedCacheFrameTexturePos).wzxy;
    weights = vCachedValues != kInvalidAOCoefficientValue ? weights : 0;
    float weightSum = dot(1, weights);

    float cachedValue = kInvalidAOCoefficientValue;
    float cachedValueSquaredMean = 0;
    float cachedRayHitDepth = 0;

    uint tspp;
    bool areCacheValuesValid = weightSum > 1e-3f;
    
    
    if (areCacheValuesValid)
    {
        uint4 vCachedTspp = gInCachedTspp.GatherRed(gSampler, adjustedCacheFrameTexturePos).wzxy;
        // Enforce tspp of at least 1 for reprojection for valid values.
        // This is because the denoiser will fill in invalid values with filtered 
        // ones if it can. But it doesn't increase tspp.
        vCachedTspp = max(1, vCachedTspp);

        float4 nWeights = weights / weightSum; // Normalize the weights.

        // Scale the tspp by the total weight. This is to keep the tspp low for 
        // total contributions that have very low reprojection weight. While its preferred to get 
        // a weighted value even for reprojections that have low weights but still
        // satisfy consistency tests, the tspp needs to be kept small so that the Target calculated values
        // are quickly filled in over a few frames. Otherwise, bad estimates from reprojections,
        // such as on disocclussions of surfaces on rotation, are kept around long enough to create 
        // visible streaks that fade away very slow.
        // Example: rotating camera around dragon's nose up close. 
        float TsppScale = 1; // TODO saturate(weightSum); 

        float cachedTspp = TsppScale * dot(nWeights, vCachedTspp);
        tspp = round(cachedTspp);

        if (tspp > 0)
        {
            float4 vCacheValues = gInCachedValue.GatherRed(gSampler, adjustedCacheFrameTexturePos).wzxy;
            cachedValue = dot(nWeights, vCacheValues);

            float4 vCachedValueSquaredMean = gInCachedValueSquaredMean.GatherRed(gSampler, adjustedCacheFrameTexturePos).wzxy;
            cachedValueSquaredMean = dot(nWeights, vCachedValueSquaredMean);

            float4 vCachedRayHitDepths = gInCachedRayHitDepth.GatherRed(gSampler, adjustedCacheFrameTexturePos).wzxy;
            cachedRayHitDepth = dot(nWeights, vCachedRayHitDepths);
        }
    }
    else
    {
        // No valid values can be retrieved from the cache.
        tspp = 0;
    }
    gOutCachedTspp[dTid] = tspp;
    gOutReprojectedCachedValues[dTid] = uint4(tspp, f32tof16(float3(cachedValue, cachedValueSquaredMean, cachedRayHitDepth)));

    //TODO: Check if slower
    //Copy here instead of a copy command
    gInOutCachedNormalDepth[dTid] = float4(normal, depth);
}
