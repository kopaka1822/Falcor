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
import Scene.Camera.Camera;
import SSAOData;
import Scene.RaytracingInline;

// only uses rasterization
#define SHADER_VARIANT_RASTER 0
// only uses raytracing
#define SHADER_VARIANT_RAYTRACING 1
// mixes ray tracing and rasterization
#define SHADER_VARIANT_HYBRID 2

#ifndef SHADER_VARIANT
#error please define shader variant to either raster, raytracing or hybrid
#endif

#define NUM_STEPS 4

cbuffer StaticCB
{
    SSAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
    uint frameIndex;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

Texture2D gDepthTex;
Texture2D gNormalTex;
Texture2D gNoiseTex;

static int gRaysTraced = 0;

float distanceWeight(float dsquared)
{
    //return saturate(1.0 - sqrt(dsquared) / gData.radius); // closer to HBAO
    //return saturate(1.0 - dsquared * gData.invRadiusSquared);
    return 1.0f;
}

float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelCoord = floor(uv * float2(width, height));
    return float2((pixelCoord.x + 0.5f) / width, (pixelCoord.y + 0.5f) / height);
}

float4 getPosition(float2 uv)
{
    float4 pos;
    pos.x = uv.x * 2.0f - 1.0f;
    pos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    pos.z = gDepthTex.SampleLevel(gTextureSampler, uv, 0).r;
    pos.w = 1.0f;

    float4 posW = mul(pos, gCamera.data.invViewProj);
    posW /= posW.w;

    return posW;
}

// P = world position
// N = world surface normal
// Dir = sample direction
// DirTangent = sample direction projected to XY plane of tangent space (tangent and bitangent)
float ComputeAORay(float3 P, float3 N, float3 Dir, float3 DirTangent, float2 uv)
{
    const float depthBias = 0.1;
    const float radiusBias = 0.8; // 1.0 = no bias, 0.8 = use 80% of radius as max distance to sample
    float occlusion = 0.0;

#if SHADER_VARIANT == SHADER_VARIANT_RASTER || SHADER_VARIANT == SHADER_VARIANT_HYBRID
    bool useRay = false;
    // Calculate sample world space pos
    float3 finalSamplePosW = P + (DirTangent * gData.radius * radiusBias);
    //float sampleDepth = length(samplePosW - gCamera.data.posW);

    // Get screen space pos of sample
    float4 finalSamplePosProj = mul(float4(finalSamplePosW, 1.0f), gCamera.data.viewProjMat);
    finalSamplePosProj /= finalSamplePosProj.w;

    // Sample depth buffer at the same place as sample
    float2 finalUnclippedSampleUV = float2(finalSamplePosProj.x, -finalSamplePosProj.y) * 0.5f + 0.5f;

    float weightSum = 0.0f;
    float sampleSum = 0.0f;
    float maxHorizonCosTheta = 0.0f; // theta = pi / 2
    bool hasValidSamples = false;

    // step along ray for num step times
    [unroll] for(int i = 1; i <= NUM_STEPS; ++i)
    {
        float f = float(i) / float(NUM_STEPS); // linear factor (0.25, 0.5, 0.75, 1.0)
        f -= abs(Dir[i % 3]) / float(NUM_STEPS); // add some noise to the step (TODO needs to be replaced with some independent random value)
        // depth to sample (this is on the tangent plane)
        //float sampleDepth = length(lerp(P, finalSamplePosW, f) - gCamera.data.posW);

        // actual depth in depth buffer
        float2 unclippedSampleUV = lerp(uv, finalUnclippedSampleUV, f);
        unclippedSampleUV = getSnappedUV(unclippedSampleUV);
        float2 sampleUV = saturate(unclippedSampleUV);
        float3 scenePosW = getPosition(sampleUV).xyz;
        float3 V = scenePosW - P;
        float distSq = dot(V, V);
        float sceneDepth = length(scenePosW - gCamera.data.posW);
        float cosTheta = max(dot(N, V / distSq), 0);

        // screen border
        if(any(unclippedSampleUV != sampleUV) && occlusion == 0.0) useRay = true;

        bool validSample = distSq < gData.radius * gData.radius || cosTheta < 0.1; // inside radius or scene is below the horizon (in the last case, occlusion and weights evaluate to 0)
        hasValidSamples = hasValidSamples || validSample;
        if(validSample)
        {   // only update horizon angle for samples inside the sampling radius
            maxHorizonCosTheta = max(maxHorizonCosTheta, cosTheta);
        }
        else if(!hasValidSamples) useRay = true; // we need at least one valid sample before the first invalid sample, to consider this evaluateable

        float weight = cosTheta * cosTheta * (1.0 - cosTheta * cosTheta);
        //float weight = cosTheta;
        sampleSum += weight * distanceWeight(distSq);
        weightSum += weight;
    }
    maxHorizonCosTheta = 1.0;
    if(SHADER_VARIANT == SHADER_VARIANT_RASTER || useRay == false) return maxHorizonCosTheta * maxHorizonCosTheta * saturate(sampleSum / max(weightSum, 0.000001));
#endif

#if SHADER_VARIANT == SHADER_VARIANT_RAYTRACING || SHADER_VARIANT == SHADER_VARIANT_HYBRID
    RayDesc ray;
    ray.Origin = P; // offset a little bit in normal direction
    ray.Direction = Dir;
    ray.TMin = 0.0; // no bias required because of offset in normal direction
    ray.TMax = gData.radius;

    SceneRayQuery<1> sceneRayQuery; // 1 = Use Alpha Test
    HitInfo hit;
    float hitT;
    gRaysTraced += 1;
    //if (!sceneRayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
        //return 0.0; // no occlusion (nothing hit)
    if(sceneRayQuery.traceRay(ray, hit, hitT, RAY_FLAG_NONE, 0xff)) // TODO value of hitT
    {
        occlusion = distanceWeight(hitT * hitT);
    }
#endif

    return occlusion;
}

// helper math for zcurve
uint ZCurveToLinearIndex(uint2 xy)
{
    return IntegerExplode(xy[0]) | (IntegerExplode(xy[1]) << 1);
}

// "Explodes" an integer, i.e. inserts a 0 between each bit. 
//      For example, 0b11111111 -> 0b1010101010101010
uint IntegerExplode(uint x)
{
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

uint JenkinsHash(uint a)
{
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

float4 main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    if (gDepthTex.SampleLevel(gTextureSampler, texC, 0).r >= 1)
    {
        return 1.0f;
    }

    int zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));

    // Calculate world position of pixel
    float3 posW = getPosition(texC).xyz;
    float3 normal = normalize(gNormalTex.Sample(gTextureSampler, texC).xyz);
    posW += 0.01 * normal; // offset starting position slightly along the normal to avoid self intersection

    // Calculate tangent space (use random direction for tangent orientation)
    //float originDist = length(posW - gCamera.data.posW);
    float3 randDir = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale).xyz * 2.0f - 1.0f;
    //float3 randDir = normalize(float3(1.0, 1.0, 1.0));

    float3 tangent = normalize(randDir - normal * dot(randDir, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0f;
    for (uint i = 0; i < gData.kernelSize; i++)
    //uint i = (zCurveIndex + (frameIndex)) % gData.kernelSize; // JenkinsHash
    {
        // Orient sample
        float3 kernelPos = mul(gData.sampleKernel[i].xyz, tbn);
        float3 tangentDir = gData.sampleKernel[i].x * tangent + gData.sampleKernel[i].y * bitangent;

        occlusion += ComputeAORay(posW, normal, normalize(kernelPos), normalize(tangentDir), texC); // TODO is normalized?
        //occlusion += ComputeAORay(posW, normal, normal, normalize(tangentDir), texC); // TODO is normalized?
    }

    float AO = occlusion / float(gData.kernelSize);
    //float AO = occlusion;
    float AOmultiplicator = 1.0;
    //#if SHADER_VARIANT == SHADER_VARIANT_RASTER
    //AOmultiplicator = 1.5;
    //#elif SHADER_VARIANT == SHADER_VARIANT_HYBRID
    //AOmultiplicator = 1.5;
    //#else
    //AOmultiplicator = 0.9;
    //#endif
    //float4 res = pow(saturate(1.0 - AO * AOmultiplicator), 2.0);
    float4 res = saturate(1.0 - AO);

    float red = gRaysTraced / float(gData.kernelSize);
    //float red = gRaysTraced;
    float green = 1.0 - red;

    res.r *= red;
    res.g *= green;
    res.b = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RASTER) res.yz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RAYTRACING) res.xz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_HYBRID) res.xy = 0.0;
    return res;
}
