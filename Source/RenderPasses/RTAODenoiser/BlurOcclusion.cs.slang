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

import RTAODenoiserHelper;
#if defined(USE_GAUSSIAN_3X3)
import GaussianKernel3x3;
#elif defined(USE_GAUSSIAN_5X5)
import GaussianKernel5x5;
#endif //USE_GAUSSIAN

#define THREAD_WIDTH 8

cbuffer CB
{
    uint2 gTextureDims;
    float gFilterStep;
}

Texture2D<float> gInDepth;
Texture2D<float> gInBlurStrength;

RWTexture2D<float> gInOutValue;

static const float kInvalidAOCoefficientValue = INVALID_AO_COEFFICIENT_VALUE;
static const float kHitDistanceOnMiss = 0;

// Group shared memory cache for the row aggregated results.
static const uint NumValuesToLoadPerRowOrColumn = THREAD_WIDTH + (FilterKernelWidth - 1);
groupshared uint PackedValueDepthCache[NumValuesToLoadPerRowOrColumn][8]; // 16bit float value, depth.
groupshared float FilteredResultCache[NumValuesToLoadPerRowOrColumn][8]; // 32 bit float filteredValue.

// Find a DTID with steps in between the group threads and groups interleaved to cover all pixels.
uint2 GetPixelIndex(in uint2 Gid, in uint2 GTid)
{
    uint2 GroupDim = uint2(8, 8);
    uint2 groupBase = (Gid / gFilterStep) * GroupDim * gFilterStep + Gid % gFilterStep;
    uint2 groupThreadOffset = GTid * gFilterStep;
    uint2 sDTid = groupBase + groupThreadOffset;

    return sDTid;
}

// Load up to 16x16 pixels and filter them horizontally.
// The output is cached in Shared Memory and contains NumRows x 8 results.
void FilterHorizontally(in uint2 Gid, in uint GI)
{
    const uint2 GroupDim = uint2(8, 8);

    // Processes the thread group as row-major 4x16, where each sub group of 16 threads processes one row.
    // Each thread loads up to 4 values, with the sub groups loading rows interleaved.
    // Loads up to 4x16x4 == 256 input values.
    uint2 GTid4x16_row0 = uint2(GI % 16, GI / 16);
    int2 GroupKernelBasePixel = GetPixelIndex(Gid, 0) - int(FilterKernelRadius * gFilterStep);
    const uint NumRowsToLoadPerThread = 4;
    const uint Row_BaseWaveLaneIndex = (WaveGetLaneIndex() / 16) * 16;

    [unroll]
    for (uint i = 0; i < NumRowsToLoadPerThread; i++)
    {
        uint2 GTid4x16 = GTid4x16_row0 + uint2(0, i * 4);
        if (GTid4x16.y >= NumValuesToLoadPerRowOrColumn)
        {
            break;
        }

        // Load all the contributing columns for each row.
        int2 pixel = GroupKernelBasePixel + GTid4x16 * gFilterStep;
        float value = kInvalidAOCoefficientValue;
        float depth = 0;

        // The lane is out of bounds of the GroupDim + kernel, 
        // but could be within bounds of the input texture,
        // so don't read it from the texture.
        // However, we need to keep it as an active lane for a below split sum.
        if (GTid4x16.x < NumValuesToLoadPerRowOrColumn && IsWithinBounds(pixel, gTextureDims))
        {
            value = gInOutValue[pixel];
            depth = gInDepth[pixel];
        }

        // Cache the kernel center values.
        if (IsInRange(GTid4x16.x, FilterKernelRadius, FilterKernelRadius + GroupDim.x - 1))
        {
            PackedValueDepthCache[GTid4x16.y][GTid4x16.x - FilterKernelRadius] = Float2ToHalf(float2(value, depth));
        }

        // Filter the values for the first GroupDim columns.
        {
            // Accumulate for the whole kernel width.
            float weightedValueSum = 0;
            float weightSum = 0;
            float gaussianWeightedValueSum = 0;
            float gaussianWeightedSum = 0;

            // Since a row uses 16 lanes, but we only need to calculate the aggregate for the first half (8) lanes,
            // split the kernel wide aggregation among the first 8 and the second 8 lanes, and then combine them.


            // Get the lane index that has the first value for a kernel in this lane.
            uint Row_KernelStartLaneIndex =
                (Row_BaseWaveLaneIndex + GTid4x16.x)
                - (GTid4x16.x < GroupDim.x
                    ? 0
                    : GroupDim.x);

            // Get values for the kernel center.
            uint kcLaneIndex = Row_KernelStartLaneIndex + FilterKernelRadius;
            float kcValue = WaveReadLaneAt(value, kcLaneIndex);
            float kcDepth = WaveReadLaneAt(depth, kcLaneIndex);

            // Initialize the first 8 lanes to the center cell contribution of the kernel. 
            // This covers the remainder of 1 in FilterKernel::Width / 2 used in the loop below. 
            if (GTid4x16.x < GroupDim.x && kcValue != kInvalidAOCoefficientValue && kcDepth != kHitDistanceOnMiss)
            {
                float w_h = FilterKernel1D[FilterKernelRadius];
                gaussianWeightedValueSum = w_h * kcValue;
                gaussianWeightedSum = w_h;
                weightedValueSum = gaussianWeightedValueSum;
                weightSum = w_h;
            }

            // Second 8 lanes start just past the kernel center.
            uint KernelCellIndexOffset =
                GTid4x16.x < GroupDim.x
                ? 0
                : (FilterKernelRadius + 1); // Skip over the already accumulated center cell of the kernel.


            // For all columns in the kernel.
            for (uint c = 0; c < FilterKernelRadius; c++)
            {
                uint kernelCellIndex = KernelCellIndexOffset + c;

                uint laneToReadFrom = Row_KernelStartLaneIndex + kernelCellIndex;
                float cValue = WaveReadLaneAt(value, laneToReadFrom);
                float cDepth = WaveReadLaneAt(depth, laneToReadFrom);

                if (cValue != kInvalidAOCoefficientValue && kcDepth != kHitDistanceOnMiss && cDepth != kHitDistanceOnMiss)
                {
                    float w_h = FilterKernel1D[kernelCellIndex];

                    // Simple depth test with tolerance growing as the kernel radius increases.
                    // Goal is to prevent values too far apart to blend together, while having 
                    // the test being relaxed enough to get a strong blurring result.
                    float depthThreshold = 0.05 + gFilterStep * 0.001 * abs(int(FilterKernelRadius) - c);
                    float w_d = abs(kcDepth - cDepth) <= depthThreshold * kcDepth;
                    float w = w_h * w_d;

                    weightedValueSum += w * cValue;
                    weightSum += w;
                    gaussianWeightedValueSum += w_h * cValue;
                    gaussianWeightedSum += w_h;
                }
            }

            // Combine the sub-results.
            uint laneToReadFrom = min(WaveGetLaneCount() - 1, Row_BaseWaveLaneIndex + GTid4x16.x + GroupDim.x);
            weightedValueSum += WaveReadLaneAt(weightedValueSum, laneToReadFrom);
            weightSum += WaveReadLaneAt(weightSum, laneToReadFrom);
            gaussianWeightedValueSum += WaveReadLaneAt(gaussianWeightedValueSum, laneToReadFrom);
            gaussianWeightedSum += WaveReadLaneAt(gaussianWeightedSum, laneToReadFrom);

            // Store only the valid results, i.e. first GroupDim columns.
            if (GTid4x16.x < GroupDim.x)
            {
                float gaussianFilteredValue = gaussianWeightedSum > 1e-6 ? gaussianWeightedValueSum / gaussianWeightedSum : kInvalidAOCoefficientValue;
                float filteredValue = weightSum > 1e-6 ? weightedValueSum / weightSum : gaussianFilteredValue;

                FilteredResultCache[GTid4x16.y][GTid4x16.x] = filteredValue;
            }
        }
    }
}

void FilterVertically(uint2 DTid, in uint2 GTid, in float blurStrength)
{
    // Kernel center values.
    float2 kcValueDepth = HalfToFloat2(PackedValueDepthCache[GTid.y + FilterKernelRadius][GTid.x]);
    float kcValue = kcValueDepth.x;
    float kcDepth = kcValueDepth.y;

    float filteredValue = kcValue;
    if (blurStrength >= 0.01 && kcDepth != kHitDistanceOnMiss)
    {
        float weightedValueSum = 0;
        float weightSum = 0;
        float gaussianWeightedValueSum = 0;
        float gaussianWeightSum = 0;

        // For all rows in the kernel.
        [unroll]
        for (uint r = 0; r < FilterKernelWidth; r++)
        {
            uint rowID = GTid.y + r;

            float2 rUnpackedValueDepth = HalfToFloat2(PackedValueDepthCache[rowID][GTid.x]);
            float rDepth = rUnpackedValueDepth.y;
            float rFilteredValue = FilteredResultCache[rowID][GTid.x];

            if (rDepth != kHitDistanceOnMiss && rFilteredValue != kInvalidAOCoefficientValue)
            {
                float w_h = FilterKernel1D[r];

                // Simple depth test with tolerance growing as the kernel radius increases.
                // Goal is to prevent values too far apart to blend together, while having 
                // the test being relaxed enough to get a strong blurring result.
                float depthThreshold = 0.05 + gFilterStep * 0.001 * abs(int(FilterKernelRadius) - int(r));
                float w_d = abs(kcDepth - rDepth) <= depthThreshold * kcDepth;
                float w = w_h * w_d;

                weightedValueSum += w * rFilteredValue;
                weightSum += w;
                gaussianWeightedValueSum += w_h * rFilteredValue;
                gaussianWeightSum += w_h;
            }
        }
        float gaussianFilteredValue = gaussianWeightSum > 1e-6 ? gaussianWeightedValueSum / gaussianWeightSum : kInvalidAOCoefficientValue;
        filteredValue = weightSum > 1e-6 ? weightedValueSum / weightSum : gaussianFilteredValue;
        filteredValue = filteredValue != kInvalidAOCoefficientValue ? lerp(kcValue, filteredValue, blurStrength) : filteredValue;
    }
    gInOutValue[DTid] = filteredValue;
}

[numthreads(THREAD_WIDTH, THREAD_WIDTH, 1)]
void main(uint2 Gid : SV_GroupID, uint2 GTid : SV_GroupThreadID, uint GI : SV_GroupIndex)
{
    uint2 sDTid = GetPixelIndex(Gid, GTid);
    // Pass through if all pixels have 0 blur strength set.
    float blurStrength;
    {
        if (GI == 0)
            FilteredResultCache[0][0] = 0;
        GroupMemoryBarrierWithGroupSync();

        blurStrength = gInBlurStrength[sDTid];

        float MinBlurStrength = 0.01;
        bool valueNeedsFiltering = blurStrength >= MinBlurStrength;
        if (valueNeedsFiltering)
            FilteredResultCache[0][0] = 1;

        GroupMemoryBarrierWithGroupSync();

        if (FilteredResultCache[0][0] == 0)
        {
            return;
        }
    }


    FilterHorizontally(Gid, GI);
    GroupMemoryBarrierWithGroupSync();

    FilterVertically(sDTid, GTid, blurStrength);
}
