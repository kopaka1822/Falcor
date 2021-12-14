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
    return saturate(1.0 - dsquared * gData.invRadiusSquared);
    //return 1.0f;
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

        float weight = 1.0;
        bool validSample = distSq < gData.radius * gData.radius || cosTheta < 0.001; // inside radius or scene is below the horizon (in the last case, occlusion and weights evaluate to 0)
        hasValidSamples = hasValidSamples || validSample;
        if(validSample)
        {   // only update horizon angle for samples inside the sampling radius
            maxHorizonCosTheta = max(maxHorizonCosTheta, cosTheta);
        }
        else useRay = true;//if(!hasValidSamples) useRay = true; // we need at least one valid sample before the first invalid sample, to consider this evaluateable

        //float weight = cosTheta * cosTheta * (1.0 - cosTheta * cosTheta);
        
        //sampleSum += weight * distanceWeight(distSq);
        sampleSum += cosTheta * cosTheta * distanceWeight(distSq);
        weightSum += weight;
    }
    //maxHorizonCosTheta = 1.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RASTER || useRay == false) return maxHorizonCosTheta * maxHorizonCosTheta * saturate(sampleSum / max(weightSum, 0.000001));
    if(SHADER_VARIANT == SHADER_VARIANT_RASTER || useRay == false) return saturate(sampleSum / max(weightSum, 0.000001));
    //if(SHADER_VARIANT == SHADER_VARIANT_RASTER || useRay == false) return pow(0.5 * sampleSum / weightSum, 0.5);
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

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
}

float2 Rotate90(float2 vec) // CCW
{
    return float2(-vec.y, vec.x);
}

// uv: uv coordinates [0, 1]
// viewDepth: linear depth in view space (positive z)
// return: view space position (negative z)
float3 UVToViewSpace(float2 uv, float viewDepth)
{
    float2 ndc = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0; // normalized device coordinates [-1, 1]
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    return float3(ndc * viewDepth * imageScale, -viewDepth);
}

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

float4 main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if(linearDepth <= 0.0) return 1.0f;

    int zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));

    // view space position of current pixel
    float3 posV = UVToViewSpace(texC, linearDepth);
    //float4 posW = gPositionTex.SampleLevel(gTextureSampler, texC, 0).xyzw;
    //float3 posV2 = mul(posW, gCamera.data.viewMat).xyz;

    // view space normal of current pixel
    float3 normalW = normalize(gNormalTex.Sample(gTextureSampler, texC).xyz);
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));

    // Calculate tangent space (use random direction for tangent orientation)
    float2 randDir = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale).xy;
    //randDir = float2(1.0f, 0.0f);
    float3 tangent = float3(randDir, 0.0f);
    float3 bitangent = float3(Rotate90(randDir), 0.0f);
    //float3x3 tbn = float3x3(tangent, bitangent, float3(0.0f, 0.0f, -1.0f));

    //float occlusion = 0.0f;
    float visibility = 0.0f;
    for (uint i = 0; i < gData.kernelSize; i++)
    //uint i = (zCurveIndex + (frameIndex)) % gData.kernelSize; // JenkinsHash
    {
        // obtain sample position on disc around view space position
        float4 rand = gData.sampleKernel[i]; // xy = random location on unit sphere, zw = uniform in 0,1
        float3 initialSamplePosV = posV + tangent * rand.x * gData.radius + bitangent * rand.y * gData.radius;
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
        // clip sample position uv and snap to pixel center
        samplePosUV = saturate(samplePosUV); // clip to screen border
        samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center

        // obtain actual view space position of the sample
        float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, samplePosUV, 0).r;
        //float3 samplePosV = UVToViewSpace(samplePosUV, linearSampleDepth);

        // height of the sphere at the requested sample position (not the actual sampling position)
        float sphereHeight = sqrt(1.0 - dot(rand.xy, rand.xy)) * gData.radius;

        float sampleRange = max(min(linearSampleDepth, linearDepth + sphereHeight) - (linearDepth - sphereHeight), 0);

        float samplingPdf = 2.0 * sphereHeight;
        visibility += sampleRange / samplingPdf;
    }

    float AO = visibility / float(gData.kernelSize);
    float4 res = AO;
    res.x = 0.0;
    //res.y = 0.0;
    res.z = 0.0;
    //AO = saturate(AO * 1.5);
    return res;

    //float red = gRaysTraced / float(gData.kernelSize);
    //float red = gRaysTraced;
    //float green = 1.0 - red;

    //res.r *= red;
    //res.g *= green;
    //res.b = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RASTER) res.yz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_RAYTRACING) res.xz = 0.0;
    //if(SHADER_VARIANT == SHADER_VARIANT_HYBRID) res.xy = 0.0;
    //return res;
}
