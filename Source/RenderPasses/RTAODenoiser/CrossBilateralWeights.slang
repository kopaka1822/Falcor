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

float4 getBilinearWeight(in float2 TargetOffset)
{
    float4 bilinearWeights =
                float4(
                    (1 - TargetOffset.x) * (1 - TargetOffset.y),
                    TargetOffset.x * (1 - TargetOffset.y),
                    (1 - TargetOffset.x) * TargetOffset.y,
                    TargetOffset.x * TargetOffset.y);

    return bilinearWeights;
}

float4 getNormalWeight(in float3 TargetNormal, in float3 SampleNormals[4], float sigma, float sigmaExponent)
{
    float4 NdotSampleN = float4(
                dot(TargetNormal, SampleNormals[0]),
                dot(TargetNormal, SampleNormals[1]),
                dot(TargetNormal, SampleNormals[2]),
                dot(TargetNormal, SampleNormals[3]));

            // Apply adjustment scale to the dot product. 
            // Values greater than 1 increase tolerance scale 
            // for unwanted inflated normal differences,
            // such as due to low-precision normal quantization.
    NdotSampleN *= sigma;

    float4 normalWeights = pow(saturate(NdotSampleN), sigmaExponent);

    return normalWeights;
}

float4 getDepthWeight(in float TargetDepth,in float2 Ddxy,in float4 SampleDepths, float sigma, float weightCutoff, uint numMantissaBits)
{
    float depthThreshold = dot(1, abs(Ddxy));
    float depthFloatPrecision = FloatPrecision(TargetDepth, numMantissaBits);

    float depthTolerance = sigma * depthThreshold + depthFloatPrecision;
    float4 depthWeights = min(depthTolerance / (abs(SampleDepths - TargetDepth) + depthFloatPrecision), 1);
    depthWeights *= depthWeights >= weightCutoff;

    return depthWeights;
}
