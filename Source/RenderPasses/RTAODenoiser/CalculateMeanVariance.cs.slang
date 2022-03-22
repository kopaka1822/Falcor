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

cbuffer CB
{
    MeanVarianceCB cb;
}

Texture2D<float> gInAOTex;
RWTexture2D<float2> gMeanVar;

static const uint kPixelStepY = 1;  //To be closer to the original code, is always one as checkerboard sampling is not supported currently
static const float kInvalidAOCoefficientValue = INVALID_AO_COEFFICIENT_VALUE;

// Group shared memory cache for the row aggregated results.
groupshared uint PackedRowResultCache[16][8]; // 16bit float valueSum, squaredValueSum.
groupshared uint NumValuesCache[16][8];

// Load up to 16x16 pixels and filter them horizontally.
// The output is cached in Shared Memory and contains NumRows x 8 results.
void FilterHorizontally(in uint2 Gid, in uint GI)
{
    const uint2 GroupDim = uint2(8, 8);
    const uint NumValuesToLoadPerRowOrColumn = GroupDim.x + (cb.kernelWidth - 1);

    // Processes the thread group as row-major 4x16, where each sub group of 16 threads processes one row.
    // Each thread loads up to 4 values, with the sub groups loading rows interleaved.
    // Loads up to 4x16x4 == 256 input values.
    uint2 GTid4x16_row0 = uint2(GI % 16, GI / 16);
    const int2 KernelBasePixel = (Gid * GroupDim - int(cb.kernelRadius)) * int2(1, kPixelStepY);
    const uint NumRowsToLoadPerThread = 4;
    const uint Row_BaseWaveLaneIndex = (WaveGetLaneIndex() / 16) * 16;

    [unroll]
    for (uint i = 0; i < NumRowsToLoadPerThread; i++)
    {
        uint2 GTid4x16 = GTid4x16_row0 + uint2(0, i * 4);
        if (GTid4x16.y >= NumValuesToLoadPerRowOrColumn)
        {
            if (GTid4x16.x < GroupDim.x)
            {
                NumValuesCache[GTid4x16.y][GTid4x16.x] = 0;
            }
            break;
        }

        // Load all the contributing columns for each row.
        int2 pixel = KernelBasePixel + GTid4x16 * int2(1, kPixelStepY);
        float value = kInvalidAOCoefficientValue;

        // The lane is out of bounds of the GroupDim + kernel, 
        // but could be within bounds of the input texture,
        // so don't read it from the texture.
        // However, we need to keep it as an active lane for a below split sum.
        if (GTid4x16.x < NumValuesToLoadPerRowOrColumn && IsWithinBounds(pixel, cb.textureDim))
        {
            value = gInAOTex[pixel];
        }

        // Filter the values for the first GroupDim columns.
        {
            // Accumulate for the whole kernel width.
            float valueSum = 0;
            float squaredValueSum = 0;
            uint numValues = 0;

            // Since a row uses 16 lanes, but we only need to calculate the aggregate for the first half (8) lanes,
            // split the kernel wide aggregation among the first 8 and the second 8 lanes, and then combine them.
            
            // Initialize the first 8 lanes to the first cell contribution of the kernel. 
            // This covers the remainder of 1 in cb.kernelWidth / 2 used in the loop below. 
            if (GTid4x16.x < GroupDim.x && value != kInvalidAOCoefficientValue)
            {
                valueSum = value;
                squaredValueSum = value * value;
                numValues++;
            }

            // Get the lane index that has the first value for a kernel in this lane.
            uint Row_KernelStartLaneIndex =
                Row_BaseWaveLaneIndex
                + 1 // Skip over the already accumulated first cell of the kernel.
                + (GTid4x16.x < GroupDim.x
                    ? GTid4x16.x
                    : (GTid4x16.x - GroupDim.x) + cb.kernelRadius);

            for (uint c = 0; c < cb.kernelRadius; c++)
            {
                uint laneToReadFrom = Row_KernelStartLaneIndex + c;
                float cValue = WaveReadLaneAt(value, laneToReadFrom);
                if (cValue != kInvalidAOCoefficientValue)
                {
                    valueSum += cValue;
                    squaredValueSum += cValue * cValue;
                    numValues++;
                }
            }
            
            // Combine the sub-results.
            uint laneToReadFrom = min(WaveGetLaneCount() - 1, Row_BaseWaveLaneIndex + GTid4x16.x + GroupDim.x);
            valueSum += WaveReadLaneAt(valueSum, laneToReadFrom);
            squaredValueSum += WaveReadLaneAt(squaredValueSum, laneToReadFrom);
            numValues += WaveReadLaneAt(numValues, laneToReadFrom);

            // Store only the valid results, i.e. first GroupDim columns.
            if (GTid4x16.x < GroupDim.x)
            {
                PackedRowResultCache[GTid4x16.y][GTid4x16.x] = Float2ToHalf(float2(valueSum, squaredValueSum));
                NumValuesCache[GTid4x16.y][GTid4x16.x] = numValues;
            }
        }
    }
}

void FilterVertically(uint2 DTid, in uint2 GTid)
{
    float valueSum = 0;
    float squaredValueSum = 0;
    uint numValues = 0;

    uint2 pixel = uint2(DTid.x, DTid.y * kPixelStepY);

    float4 val1, val2;
    // Accumulate for the whole kernel.
    for (uint r = 0; r < cb.kernelWidth; r++)
    {
        uint rowID = GTid.y + r;
        uint rNumValues = NumValuesCache[rowID][GTid.x];

        if (rNumValues > 0)
        {
            float2 unpackedRowSum = HalfToFloat2(PackedRowResultCache[rowID][GTid.x]);
            float rValueSum = unpackedRowSum.x;
            float rSquaredValueSum = unpackedRowSum.y;

            valueSum += rValueSum;
            squaredValueSum += rSquaredValueSum;
            numValues += rNumValues;
        }
    }

    // Calculate mean and variance.
    float invN = 1.f / max(numValues, 1);
    float mean = invN * valueSum;

    // Apply Bessel's correction to the estimated variance, multiply by N/N-1, 
    // since the true population mean is not known; it is only estimated as the sample mean.
    float besselCorrection = numValues / float(max(numValues, 2) - 1);
    float variance = besselCorrection * (invN * squaredValueSum - mean * mean);

    variance = max(0, variance); // Ensure variance doesn't go negative due to imprecision.

    gMeanVar[pixel] = numValues > 0 ? float2(mean, variance) : kInvalidAOCoefficientValue;
}

[numthreads(8, 8, 1)]
void main(uint2 Gid : SV_GroupID, uint2 GTid : SV_GroupThreadID, uint GI : SV_GroupIndex, uint2 DTid : SV_DispatchThreadID)
{
    FilterHorizontally(Gid, GI);
    GroupMemoryBarrierWithGroupSync();

    FilterVertically(DTid, GTid);
}
