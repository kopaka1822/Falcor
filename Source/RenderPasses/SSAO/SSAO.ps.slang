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
float ComputeAORay(float3 P, float3 N, float3 Dir)
{
    const float depthBias = 0.1;
    float occlusion = 0.0;

#if SHADER_VARIANT == SHADER_VARIANT_RASTER || SHADER_VARIANT == SHADER_VARIANT_HYBRID
    // Calculate sample world space pos
    float3 samplePosW = P + (Dir * gData.radius * 0.8);
    float sampleDepth = length(samplePosW - gCamera.data.posW);

    // Get screen space pos of sample
    float4 samplePosProj = mul(float4(samplePosW, 1.0f), gCamera.data.viewProjMat);
    samplePosProj /= samplePosProj.w;

    // Sample depth buffer at the same place as sample
    float2 unclippedSampleUV = float2(samplePosProj.x, -samplePosProj.y) * 0.5f + 0.5f;
    float2 sampleUV = saturate(unclippedSampleUV);
    float3 scenePosW = getPosition(sampleUV).xyz;
    float sceneDepth = length(scenePosW - gCamera.data.posW);

    float sampleDistance = distance(P, scenePosW);
    if(SHADER_VARIANT == SHADER_VARIANT_HYBRID && any(unclippedSampleUV != sampleUV))
    {
    }
    else if(sampleDistance <= gData.radius || sceneDepth > sampleDepth) // only evaluate if within radius
    {
        if(sceneDepth < sampleDepth - depthBias) occlusion = 1.0;
        return occlusion;
    }

#endif

#if SHADER_VARIANT == SHADER_VARIANT_RAYTRACING || SHADER_VARIANT == SHADER_VARIANT_HYBRID
    RayDesc ray;
    ray.Origin = P;
    ray.Direction = Dir;
    ray.TMin = 0.01;
    ray.TMax = gData.radius;

    SceneRayQuery<1> sceneRayQuery; // 1 = Use Alpha Test
    HitInfo hit;
    float hitT;
    gRaysTraced += 1;
    //if (!sceneRayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
        //return 0.0; // no occlusion (nothing hit)
    if(sceneRayQuery.traceRay(ray, hit, hitT, RAY_FLAG_NONE, 0xff))
    {
        occlusion = 1.0;
    }
#endif

    return occlusion;
}

// Ambient occlusion factor as in hbao+
// P = position
// N = normal
// S = sample position
float ComputeAO(float3 P, float3 N, float3 S)
{
    const float invR2 = 1.0 / (gData.radius * gData.radius);
    const float NDotVBias = 0.1;

    float3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * rsqrt(VdotV);

    float angleFactor = saturate(NdotV - NDotVBias);
    float distanceFactor = saturate(1.0 - VdotV * invR2);

    //return angleFactor * distanceFactor;

    [branch]
    if(distanceFactor > 0.0) return angleFactor * distanceFactor;
    else
    {
    // else: try raytracing
    RayDesc ray;
    ray.Origin = P;
    ray.Direction = normalize(V);
    ray.TMin = 0.01;
    ray.TMax = gData.radius;

    SceneRayQuery<1> sceneRayQuery; // 1 = Use Alpha Test
    HitInfo hit;
    float hitT;

    //RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    //q.TraceRayInline(gScene.rtAccel, RAY_FLAG_NONE, 0xff, ray);

    //if (!sceneRayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
        //return 0.0; // no occlusion (nothing hit)
    if(!sceneRayQuery.traceRay(ray, hit, hitT, RAY_FLAG_NONE, 0xff))
        return 0.0; // no occlusion (nothing hit)

    distanceFactor = saturate(1.0 - hitT * hitT * invR2);

    // TODO ray distance?
    return angleFactor * distanceFactor;
    }
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

    // Calculate tangent space (use random direction for tangent orientation)
    //float originDist = length(posW - gCamera.data.posW);
    float3 randDir = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale).xyz * 2.0f - 1.0f;

    float3 tangent = normalize(randDir - normal * dot(randDir, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0f;
    //for (uint i = 0; i < gData.kernelSize; i++)
    uint i = (JenkinsHash(zCurveIndex + frameIndex)) % gData.kernelSize;
    {
        // Orient sample
        float3 kernelPos = mul(gData.sampleKernel[i].xyz, tbn);

        occlusion += ComputeAORay(posW, normal, normalize(kernelPos)); // TODO is normalized?
    }

    //float AO = occlusion / float(gData.kernelSize);
    float AO = occlusion;
    float4 res = pow(saturate(1.0 - AO * 1.5), 2.0);

    //float red = gRaysTraced / float(gData.kernelSize);
    float red = gRaysTraced;
    float green = 1.0 - red;

    res.r *= red;
    res.g *= green;
    res.b = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RASTER) res.yz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RAYTRACING) res.xz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_HYBRID) res.xy = 0.0;
    return res;
}
