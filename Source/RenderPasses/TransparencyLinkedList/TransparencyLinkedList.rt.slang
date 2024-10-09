/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Utils.Color.ColorHelpers;
import SharedTypes;

#ifndef COUNT_LIGHTS
#define COUNT_LIGHTS 1
#endif

#ifndef AVSM_K
#define AVSM_K 4
#endif

#define AVSM_K_4 AVSM_K/4
// Count of shadow maps is lights*(k/4)
#define COUNT_SM COUNT_LIGHTS *AVSM_K_4 

//Shadow Eval Modes
#define SHADOW_EVAL_MODE_RAYTRACING 0
#define SHADOW_EVAL_MODE_StochSM 1
#define SHADOW_EVAL_MODE_LINKED_LIST 2

#ifndef SHADOW_EVAL_MODE
#define SHADOW_EVAL_MODE SHADOW_EVAL_MODE_RAYTRACING
#endif

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded.
}

cbuffer SMCB
{
    uint gSMSize;
    float gNear;
    float gFar;
    uint _pad;
}

cbuffer ShadowVPs
{
    float4x4 gShadowMapVP[COUNT_LIGHTS];
}

cbuffer ShadowView
{
    float4x4 gShadowMapView[COUNT_LIGHTS];
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

// Optional
Texture2D<float4> gStochDepths[COUNT_SM];
Texture2D<float4> gStochTransmittance[COUNT_SM];

// Outputs
RWTexture2D<float4> gOutputColor;

// Sampler
SamplerState gPointSampler;

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kMaxBounces = MAX_BOUNCES;
static const uint kMaxAlphaBounces = MAX_ALPHA_BOUNCES; // Max number of allowed alpha bounces
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const float kRayTMax = FLT_MAX;
static const float kAlphaTMinOffset = 1e-7;
static const uint kLightSampleMode = LIGHT_SAMPLE_MODE;
static const bool kUseAlphaRussianRoulette = USE_RUSSIAN_ROULETTE_FOR_ALPHA;
static const bool kUsePathRussianRoulette = USE_RUSSIAN_ROULETTE_PATH;
static const bool kUsePCF = USE_AVSM_PCF;
static const bool kAVSMUseInterpolation = USE_AVSM_INTERPOLATION;

static const float kMinOpacity = 1e-7;

/** Payload for shadow ray. (4B)
*/
struct ShadowRayData
{
    float visibility;

    __init() {
        visibility = 1.0;
    }
};

/** Payload for alpha ray (16B)
*/
struct TransparencyRayData {
    PackedHitInfo hit; //< Packed Hit
    float dist;

    __init()
    {
        hit = {}; // Init emtpy
        dist = -1.0;
    }
}

/** Struct with data for the current path
*/
struct PathData
{
    float3  radiance;       ///< Accumulated outgoing radiance from path.
    bool    terminated;     ///< Set to true when path is terminated.
    float3  thp;            ///< Current path throughput. This is updated at each path vertex.
    uint    pathLength;     ///< Path length in number of path segments (0 at origin, 1 at first secondary hit, etc.). Max 2^31.
    float3 origin;          ///< Next path segment origin.
    float rayTransparency;  ///< Full transparency for the ray
    float3 direction;       ///< Next path segment direction.
    float alphaDist;        ///< Distance when the alpha ray should start

    SampleGenerator sg;     ///< Per-ray state for the sample generator (up to 16B).

    /** Initializes ray payload with default parameters.
    */
    __init()
    {
        this.terminated = false;
        this.pathLength = 0;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.alphaDist = 0.0;
    }
};

// For RIS light sampling
struct RISReservoir {
    uint lightIdx; // Light Index
    float wSum;    // Weight and Weight sum
    // uint M;         //Unused, as all possible Samples are seen

    /** Initializes ray payload with default parameters.
     */
    __init() {
        lightIdx = 0;
        wSum = 0.0;
        // M = 0;
    }

    [mutating] bool update(uint index, float risWeight, float rnd) {
        wSum += risWeight;
        // M += 1;
        bool selectSample = (rnd * wSum < risWeight);
        if (selectSample) {
            lightIdx = index;
        }
        return selectSample;
    }

    [mutating] void finalize(float targetFunction) {
        /* //When not looping through all lights
        if(targetFunction > 0 && M > 0){
            wSum = (1.0 / targetFunction) * ((1.0/M) * wSum);
            r.M = 1;
        */
        if (targetFunction > 0) {
            wSum = wSum / targetFunction;
        } else {
            wSum = 0;
        }
    }
};

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    return sd;
}

/** Returns the primary ray's direction.
*/
float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    if (is_valid(gViewW))
    {
        // If we have the view vector bound as a buffer, just fetch it. No need to compute anything.
        return -gViewW[launchIndex].xyz;
    }
    else
    {
        // Compute the view vector. This must exactly match what the G-buffer pass is doing (jitter etc.).
        // Note that we do not take depth-of-field into account as it would require exactly matching the
        // sample generator between the passes, which is error prone. The host side will issue a warning instead.
        return camera.computeRayPinhole(launchIndex, launchDim).dir;
    }
}

/** Russian Roulette function that can abort a path early
    Return "true" if the path should be aborted (absorbed)
*/
bool russianRoulette(float prob, inout float3 thp, inout SampleGenerator sg) {
    float rnd = sampleNext1D(sg);
    bool rr = rnd > prob;
    if (!rr) {
        thp /= prob;
    }
    return rr;
}

/** Traces a shadow ray towards a light source.
    \param[in] origin Ray origin for the shadow ray.
    \param[in] dir Direction from shading point towards the light source (normalized).
    \param[in] distance Distance to the light source.
    \return True if light is visible, false otherwise.
*/
float traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData = ShadowRayData();
    rayData.visibility = 1.0;
    uint flags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE;
    TraceRay(gScene.rtAccel, flags, kShadowRayInstanceMask /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, ray, rayData);

    return rayData.visibility;
}

/** Traces an alpha ray
 */
void traceAlphaRay(inout PathData pathData, inout TransparencyRayData rayData) {
    RayDesc ray;
    ray.Origin = pathData.origin;
    ray.Direction = pathData.direction;
    ray.TMin = pathData.alphaDist + kAlphaTMinOffset;
    ray.TMax = kRayTMax;

    uint rayFlags = 0;
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}

float3 getShadowUVAndLightDist(uint lightIdx, float3 posW) {
    float4 shadowPos = mul(gShadowMapVP[lightIdx], float4(posW, 1));
    shadowPos.xyz /= shadowPos.w;

    shadowPos.y *= -1;                                        // Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5;                  // UV from [-1,1] to [0,1]
    shadowPos.z = (shadowPos.w - gNear) / (gFar - gNear); // Linear distance

    return shadowPos.xyz;
}

float evalAVTransmittanceFunction(float depth, float depthSamples[AVSM_K], float transmittanceSamples[AVSM_K]) {
    float shadow = 1.0;
    if (depth <= depthSamples[0])
        return shadow;

    uint choosenIdx = AVSM_K - 1;
    for (uint i = 1; i < AVSM_K; i++) {
        if (depth < depthSamples[i]) {
            choosenIdx = i - 1;
            break;
        }
    }
    if ((!kAVSMUseInterpolation) || (choosenIdx == (AVSM_K - 1)))
        shadow = transmittanceSamples[choosenIdx];
    else
    {
        // Create Interpolation points
        float2 A = float2(depthSamples[choosenIdx], transmittanceSamples[choosenIdx]);
        float2 B = float2(depthSamples[choosenIdx + 1], transmittanceSamples[choosenIdx + 1]);
        float t = B.y > 0.0 ? (depth - A.x) / (B.x - A.x) : 0.0; // Normalized t
        shadow = lerp(A.y, B.y, t);
    }

    return shadow;
}

float4 getGatherWeights(const float2 uv, const uint smRes, out float2 gatherUV) {
    gatherUV = (floor(uv * float(smRes) - 0.5) + 1.0f) / float(smRes);
    float2 f = frac(uv * float(smRes) - 0.5f);
    // (-,+),(+,+)(+,-)(-,-)
    return float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
}

float evalStochasticSM(uint lightIdx, float3 posW) {
    float3 shadowPos = getShadowUVAndLightDist(lightIdx, posW);

    // Check if inside
    if (any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
        return 0.0;

    float shadow = 1.0;
    if (kUsePCF) {
        float2 gatherUV;
        float4 w = getGatherWeights(shadowPos.xy, gSMSize, gatherUV);
        // Get all values
        float depths0[AVSM_K];
        float depths1[AVSM_K];
        float depths2[AVSM_K];
        float depths3[AVSM_K];
        float transmittance0[AVSM_K];
        float transmittance1[AVSM_K];
        float transmittance2[AVSM_K];
        float transmittance3[AVSM_K];
        for (uint i = 0; i < AVSM_K_4; i++) {
            uint texIdx = lightIdx * AVSM_K_4 + i;
            uint arrIdx = i * 4;

            float4 depthsSamplesR = gStochDepths[texIdx].GatherRed(gPointSampler, gatherUV.xy);
            float4 depthsSamplesG = gStochDepths[texIdx].GatherGreen(gPointSampler, gatherUV.xy);
            float4 depthsSamplesB = gStochDepths[texIdx].GatherBlue(gPointSampler, gatherUV.xy);
            float4 depthsSamplesA = gStochDepths[texIdx].GatherAlpha(gPointSampler, gatherUV.xy);

            depths0[arrIdx] = depthsSamplesR[0];
            depths0[arrIdx + 1] = depthsSamplesG[0];
            depths0[arrIdx + 2] = depthsSamplesB[0];
            depths0[arrIdx + 3] = depthsSamplesA[0];
            depths1[arrIdx] = depthsSamplesR[1];
            depths1[arrIdx + 1] = depthsSamplesG[1];
            depths1[arrIdx + 2] = depthsSamplesB[1];
            depths1[arrIdx + 3] = depthsSamplesA[1];
            depths2[arrIdx] = depthsSamplesR[2];
            depths2[arrIdx + 1] = depthsSamplesG[2];
            depths2[arrIdx + 2] = depthsSamplesB[2];
            depths2[arrIdx + 3] = depthsSamplesA[2];
            depths3[arrIdx] = depthsSamplesR[3];
            depths3[arrIdx + 1] = depthsSamplesG[3];
            depths3[arrIdx + 2] = depthsSamplesB[3];
            depths3[arrIdx + 3] = depthsSamplesA[3];

            float4 transmittanceSamplesR = gStochTransmittance[texIdx].GatherRed(gPointSampler, gatherUV.xy);
            float4 transmittanceSamplesG = gStochTransmittance[texIdx].GatherGreen(gPointSampler, gatherUV.xy);
            float4 transmittanceSamplesB = gStochTransmittance[texIdx].GatherBlue(gPointSampler, gatherUV.xy);
            float4 transmittanceSamplesA = gStochTransmittance[texIdx].GatherAlpha(gPointSampler, gatherUV.xy);

            transmittance0[arrIdx] = transmittanceSamplesR[0];
            transmittance0[arrIdx + 1] = transmittanceSamplesG[0];
            transmittance0[arrIdx + 2] = transmittanceSamplesB[0];
            transmittance0[arrIdx + 3] = transmittanceSamplesA[0];
            transmittance1[arrIdx] = transmittanceSamplesR[1];
            transmittance1[arrIdx + 1] = transmittanceSamplesG[1];
            transmittance1[arrIdx + 2] = transmittanceSamplesB[1];
            transmittance1[arrIdx + 3] = transmittanceSamplesA[1];
            transmittance2[arrIdx] = transmittanceSamplesR[2];
            transmittance2[arrIdx + 1] = transmittanceSamplesG[2];
            transmittance2[arrIdx + 2] = transmittanceSamplesB[2];
            transmittance2[arrIdx + 3] = transmittanceSamplesA[2];
            transmittance3[arrIdx] = transmittanceSamplesR[3];
            transmittance3[arrIdx + 1] = transmittanceSamplesG[3];
            transmittance3[arrIdx + 2] = transmittanceSamplesB[3];
            transmittance3[arrIdx + 3] = transmittanceSamplesA[3];

            shadow = 0.0;
            shadow += w[0] * evalAVTransmittanceFunction(shadowPos.z, depths0, transmittance0);
            shadow += w[1] * evalAVTransmittanceFunction(shadowPos.z, depths1, transmittance1);
            shadow += w[2] * evalAVTransmittanceFunction(shadowPos.z, depths2, transmittance2);
            shadow += w[3] * evalAVTransmittanceFunction(shadowPos.z, depths3, transmittance3);
        }
    } else {
        // Sample from textures
        float depths[AVSM_K];
        float transmittance[AVSM_K];
        for (uint i = 0; i < AVSM_K_4; i++) {
            uint texIdx = lightIdx * AVSM_K_4 + i;
            uint arrIdx = i * 4;

            float4 depthTex = gStochDepths[texIdx].SampleLevel(gPointSampler, shadowPos.xy, 0);
            depths[arrIdx] = depthTex[0];
            depths[arrIdx + 1] = depthTex[1];
            depths[arrIdx + 2] = depthTex[2];
            depths[arrIdx + 3] = depthTex[3];

            float4 transmittanceTex = gStochTransmittance[texIdx].SampleLevel(gPointSampler, shadowPos.xy, 0);
            transmittance[arrIdx] = transmittanceTex[0];
            transmittance[arrIdx + 1] = transmittanceTex[1];
            transmittance[arrIdx + 2] = transmittanceTex[2];
            transmittance[arrIdx + 3] = transmittanceTex[3];
        }

        shadow = evalAVTransmittanceFunction(shadowPos.z, depths, transmittance);
    }

    return shadow;
}

struct ListNode
{
    float depth;
    float visibility;
    uint next;
    uint pad;
};

RWStructuredBuffer<ListNode> gLinkedList[COUNT_LIGHTS];

float evalLinkedListSM(uint lightIdx, float3 posW) {
    float4 shadowPos = mul(gShadowMapVP[lightIdx], float4(posW, 1));
    shadowPos.xyz /= shadowPos.w;

    shadowPos.y *= -1;                       // Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5; // UV from [-1,1] to [0,1]
    shadowPos.z = (shadowPos.w - gNear) / (gFar - gNear); // Linear distance

    // Check if inside
    if (any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
        return 0.0;

    float dist = -1.0 * mul(gShadowMapView[lightIdx], float4(posW, 1.0)).z;
    dist = dist - 1e-2; // TODO better bias
    uint2 shadowPixel = uint2(floor(shadowPos.xy * gSMSize));
    uint index = shadowPixel.y * gSMSize.x + shadowPixel.x;
    ListNode n = gLinkedList[lightIdx][index];
    // traverse visibility until dist is reached
    float visibility = 1.0;
    while(n.depth < dist) {
        visibility = n.visibility;
        if(n.next == 0) break;
        n = gLinkedList[lightIdx][n.next];
    }

    return visibility;
}

/** Samples a light index either uniform or with RIS (Good if a couple lights are used that dont overlap each other)
 */
AnalyticLightSample sampleLightIndex(float3 posW, uint lightCount, inout SampleGenerator sg, out uint lightIndex, out float invPdf) {
    AnalyticLightSample ls = {};
    if (kLightSampleMode == (uint)TPTLightSampleMode::Uniform) {
        float lightSelect = 0.0;
        lightSelect = sampleNext1D(sg);

        // Pick one of the analytic light sources randomly with equal probability.
        lightIndex = min(uint(lightSelect * lightCount), lightCount - 1);
        invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

        // Sample local light source.
        LightData lightData = gScene.getLight(lightIndex);
        sampleLight(posW, lightData, sg, ls);
    } else if (kLightSampleMode == (uint)TPTLightSampleMode::RIS) {
        RISReservoir r = RISReservoir();
        LightData selectedLightData = {};
        // Loop through all lights
        for (uint i = 0; i < lightCount; i++) {
            float randomNum = i == 0 ? 0.5 : sampleNext1D(sg); // Random number
            LightData lightData = gScene.getLight(i);
            AnalyticLightSample itLs;
            sampleLight(posW, lightData, sg, itLs);
            float w = luminance(itLs.Li); // Take the Light attenuation as weight
            if (r.update(i, w, randomNum)) {
                ls = itLs;
                selectedLightData = lightData;
            }
        }
        r.finalize(luminance(ls.Li)); // targetFunction
        // Out data
        lightIndex = r.lightIdx;
        invPdf = r.wSum; // W
    }
    else { // Invalid type
        lightIndex = 0;
        invPdf = -1.0;
    }

    return ls;
}

/** Evaluates the direct illumination from analytic lights.
    This function samples Falcor's light list uniformly with one shadow ray.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in,out] sg SampleGenerator object.
    \return Outgoing radiance in view direction.
*/
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return float3(0.f);

    // Pick one of the analytic light sources randomly with equal probability.
    uint lightIndex = 0;
    float invPdf = 1.0;

    // Sample local light source.
    AnalyticLightSample ls = sampleLightIndex(sd.posW, lightCount, sg, lightIndex, invPdf);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    bool flipLightDir = (NdotL < kMinCosTheta) && (sd.opacity < 1);
    float3 lightDirection = flipLightDir  ? -ls.dir : ls.dir;

    // Test visibility by tracing a shadow ray.
    float shadow = 1.0;
    #if SHADOW_EVAL_MODE == SHADOW_EVAL_MODE_RAYTRACING
        // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
        const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
        shadow = traceShadowRay(origin, ls.dir, ls.distance);
    #elif SHADOW_EVAL_MODE == SHADOW_EVAL_MODE_StochSM
        shadow = evalStochasticSM(lightIndex, sd.posW);
    #elif SHADOW_EVAL_MODE == SHADOW_EVAL_MODE_LINKED_LIST
        shadow = evalLinkedListSM(lightIndex, sd.posW);
    #endif
    
    if (shadow <= 0) return float3(0.f);

    // Evaluate contribution.
    return mi.eval(sd, lightDirection, sg) * ls.Li * invPdf * shadow;
}

/** Generate a new scatter ray or terminate.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in] rayOrigin Ray origin for the new ray.
    \param[in,out] rayData Ray payload.
    \return True if the path continues.
*/
bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, inout PathData pd)
{
    // Sample material.
    BSDFSample bsdfSample;
    if (mi.sample(sd, pd.sg, bsdfSample, kUseImportanceSampling))
    {
        bool isTransmission = bsdfSample.isLobe(LobeType::Transmission);
        pd.origin = sd.computeNewRayOrigin(!isTransmission);
        pd.direction = bsdfSample.wo;
        pd.thp *= bsdfSample.weight;
        pd.alphaDist = 0.0;
        return any(pd.thp > 0.f);
    }

    return false;
}

/** Determines if an alpha ray should be distributed and recalculates the origin in this case
    \param[in] sd Shading data.
    \param[in,out] paht data.
    \return True if an alpha ray should be distributed
*/
bool generateAlphaResolveRay(const ShadingData sd, inout PathData pd, float startDist){
    if (sd.opacity < 1) {
        pd.alphaDist = startDist;
        return true;
    }
    return false;
}

bool resolveAlpha(const in TransparencyRayData alphaRay, inout PathData pd, out float opacity) {
    opacity = 1.0;
    const HitInfo hit = HitInfo(alphaRay.hit);
    // Miss
    if (!hit.isValid() || alphaRay.dist < 0) {
        // Ray missed the scene. Mark the ray as terminated.
        pd.terminated = true;

        // Add contribution from distant light (env map) in this direction.
        if (kUseEnvLight && (kComputeDirect || pd.pathLength > 0))
        {
            float3 Le = gScene.envMap.eval(WorldRayDirection());
            pd.radiance += pd.thp * Le;
        }
        return false; 
    }

    let lod = ExplicitLodTextureSampler(0.f);

    //Update shading data
    ShadingData sd = loadShadingData(hit, pd.direction, lod);
    opacity = sd.opacity;

    //Update material instance
    uint hints = (uint)MaterialInstanceHints::AdjustShadingNormal;
    let mi = gScene.materials.getMaterialInstance(sd, lod);

    //Add contribution from emissive lights
    if (kUseEmissiveLights && kComputeDirect)
    {
        pd.radiance += pd.thp * mi.getProperties(sd).emission * opacity;
    }

    // Add contribution of direct light from analytic lights.
    if (kUseAnalyticLights)
    {
        float3 Lr = evalDirectAnalytic(sd, mi, pd.sg);
        pd.radiance += pd.thp * Lr * opacity;
    }

    bool needAlphaTest = generateAlphaResolveRay(sd, pd, alphaRay.dist);

    // Update THP (and use Russian Roulette to abort early)
    float transparency = (1.0 - opacity);
    pd.thp *= transparency; // Update THP
    if (kUseAlphaRussianRoulette) { // Russian Roulette
        pd.rayTransparency *= (1.0 - opacity);
        if (russianRoulette(pd.rayTransparency, pd.thp, pd.sg))
            needAlphaTest = false;
    }

    //Valid hit
    if (!needAlphaTest)
        pd.pathLength++;

    //Generate scatter ray for next hit
    if (!needAlphaTest && (pd.pathLength < kMaxBounces)) {
        pd.terminated = !generateScatterRay(sd, mi, pd);
    }

    return needAlphaTest;
}

/** This is the main entry point
*/
float3 tracePath(const uint2 pixel, const uint2 frameDim)
{
    float3 outColor = float3(0.f);

    // Prepare ray payload.
    PathData pd = PathData();

    pd.origin = gScene.camera.getPosition();
    pd.direction = normalize(getPrimaryRayDir(pixel, frameDim, gScene.camera));

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (hit.isValid())
    {
        // Create sample generator.
        pd.sg = SampleGenerator(pixel, gFrameCount);

        // Pixel represents a valid primary hit. Compute its contribution.
        let lod = ExplicitLodTextureSampler(0.f);
        // Load shading data.
        ShadingData sd = loadShadingData(hit, pd.direction, lod);
        // Create material instance at shading point.

        let mi = gScene.materials.getMaterialInstance(sd, lod);

        if (kComputeDirect)
        {
            // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
            outColor += mi.getProperties(sd).emission * sd.opacity;

            // Add contribution of direct light from analytic lights.
            // Light probe and mesh lights are handled by the scatter ray hit/miss shaders.
            outColor += kUseAnalyticLights ? evalDirectAnalytic(sd, mi, pd.sg) * sd.opacity : float3(0.f);
        }

        //Handle alpha for the first hit
        float distToPoint = distance(pd.origin, sd.computeNewRayOrigin(false));
        bool needAlphaTest = generateAlphaResolveRay(sd, pd, distToPoint);
        float opacity = sd.opacity;
        uint depth = 0;
        if (needAlphaTest) {
            float transparency = (1.0 - opacity);
            pd.thp *= transparency;
            if (kUseAlphaRussianRoulette)
                needAlphaTest = !russianRoulette(transparency, pd.thp, pd.sg);
        }

        if ((kMaxBounces > 0) && !needAlphaTest) {
            opacity = 0.0;
            pd.terminated = !generateScatterRay(sd, mi, pd);
            needAlphaTest = true;
            depth++;
        }

        for (depth; (depth <= kMaxBounces) && !pd.terminated; ++depth) {
            // Alpha Test
            pd.rayTransparency = (1.0 - opacity);
            for (uint alphaDepth = 0; (alphaDepth < kMaxAlphaBounces) && needAlphaTest; alphaDepth++) {                
                TransparencyRayData alphaRay = TransparencyRayData();
                traceAlphaRay(pd, alphaRay);
                needAlphaTest = resolveAlpha(alphaRay, pd, opacity);
            }
            needAlphaTest = true;
            opacity = 0;
            // Path russian roulette
            if (kUsePathRussianRoulette && depth > 0) {
                const float rrVal = luminance(pd.thp);
                if (russianRoulette(rrVal, pd.thp, pd.sg))
                    pd.terminated = true;
            }

        }
        
        // Store contribution from scatter ray.
        outColor += pd.radiance;
    }
    else
    {
        // Background pixel.
        outColor = kUseEnvBackground ? gScene.envMap.eval(pd.direction) : kDefaultBackgroundColor;
    }

    return outColor;
}

//
// Shader entry points for miss shaders.
//

[shader("miss")]
void alphaMiss(inout TransparencyRayData rayData)
{

}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{

}


//
// Shader entry points for TriangleMesh hit groups.
//
[shader("anyhit")]
void alphaAnyHit(inout TransparencyRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f, 1e-7)) IgnoreHit();
}

[shader("closesthit")]
void alphaClosestHit(inout TransparencyRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    rayData.hit = HitInfo(triangleHit).getData();
    rayData.dist = RayTCurrent();
}

[shader("anyhit")]
void shadowAnyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Get shading data
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -WorldRayDirection(), lod);

    rayData.visibility *= saturate(1.0 - sd.opacity);

    if(rayData.visibility > 0)
        IgnoreHit();
    else
        AcceptHitAndEndSearch();
}

//
// Shader entry point for ray generation shader.
//

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    float3 color = tracePath(pixel, frameDim);

    gOutputColor[pixel] = float4(color, 1.f);
}
