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

import Rendering.ShadowMaps.ShadowTestFunctions;
import Rendering.ShadowMaps.ShadowMapHelpers;

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

/*float4 getGatherWeights(const float2 uv, const uint smRes, out float2 gatherUV) {
    gatherUV = (floor(uv * float(smRes) - 0.5) + 1.0f) / float(smRes);
    float2 f = frac(uv * float(smRes) - 0.5f);
    // (-,+),(+,+)(+,-)(-,-)
    return float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
}*/

struct ListNode
{
    float depth;
    float visibility;
    uint next;
};

struct ListNode2
{
    uint prev;
    uint right;
    uint bot;
}

struct ArrayNode
{
    float depth;
    float visibility;
};

StructuredBuffer<ListNode> gLinkedList[COUNT_LIGHTS];
StructuredBuffer<ListNode2> gLinkedListNeighbors[COUNT_LIGHTS];
StructuredBuffer<ArrayNode> gLinkedListArrays[COUNT_LIGHTS];
Texture2D<uint2> gLinkedListArrayOffsets[COUNT_LIGHTS]; // stores offsets for the array: (start, count) pairs.

float getLinkedVisibility(ListNode n, float dist, uint lightIdx)
{
    float visibility = 1.0;
    // from the way we construct the linked list neighbors, we know that we start with n.depth < depth
    while (n.depth < dist)
    {
        visibility = n.visibility;
        if (n.next == 0) break;
        n = gLinkedList[lightIdx][n.next];
    }

    return visibility;
}

struct LinkedPcfData
{
    float depth;
    float visibilityFront; // visibility in front of the depth
    float visibilityBack; // visibility behind the depth
};

LinkedPcfData getLinkedPcfData(ListNode n, float dist, uint lightIdx)
{
    LinkedPcfData data;
    data.depth = n.depth;
    data.visibilityFront = 1.0;
    data.visibilityBack = n.visibility;
    //return data;
    float prevVisibility = n.visibility;

    // find closest depth
    float minDist = abs(n.depth - dist);
    while(n.next != 0 && n.depth < dist)
    {
        n = gLinkedList[lightIdx][n.next];
        float curDist = abs(n.depth - dist); 
        if(curDist < minDist)
        {
            minDist = curDist;
            data.depth = n.depth;
            data.visibilityFront = prevVisibility;
            data.visibilityBack = n.visibility;
        }
        prevVisibility = n.visibility;
    }

    return data;
}

float evalLinkedListSM(uint lightIdx, float3 posW) {
    float4 shadowPos = mul(gShadowMapVP[lightIdx], float4(posW, 1));
    shadowPos.xyz /= shadowPos.w;

    shadowPos.y *= -1;                       // Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5; // UV from [-1,1] to [0,1]
    shadowPos.z = (shadowPos.w - gNear) / (gFar - gNear); // Linear distance

    // Check if inside
    if (any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
        return 0.0;

    float dist = shadowPos.w; //-1.0 * mul(gShadowMapView[lightIdx], float4(posW, 1.0)).z;
    float visibility = 1.0;

    #ifdef USE_LINKED_LIST_ARRAY
    dist = dist - 1e-2;       // TODO better bias
    uint2 shadowPixel = uint2(floor(shadowPos.xy * gSMSize));
    uint2 offsets = gLinkedListArrayOffsets[lightIdx][shadowPixel];
    for(uint index = offsets.x; index < offsets.x + offsets.y; index++)
    {
        ArrayNode n = gLinkedListArrays[lightIdx][index];
        if (n.depth >= dist) break;
        visibility = n.visibility;
    }
    #else // USE_LINKED_LIST_ARRAY
    if (USE_LINKED_PCF)
    {
        float2 gatherUV;
        // (-,+),(+,+)(+,-)(-,-) => BotLeft (x), BotRight (y), TopRight (z), TopLeft (w)
        float4 w = getGatherWeights(shadowPos.xy, gSMSize, gatherUV);
        
        //dist = dist - 1e-8;
        //dist = dist - 1e-2;
        int2 shadowPixel = int2(floor(shadowPos.xy * gSMSize - 0.5));
        // TODO special case for borders
        if (shadowPixel.x < 0 || shadowPixel.y < 0 || shadowPixel.x >= gSMSize - 1 || shadowPixel.y >= gSMSize - 1)
            return 0.0;

        uint index = shadowPixel.y * gSMSize.x + shadowPixel.x;
        let wData = getLinkedPcfData(gLinkedList[lightIdx][index], dist, lightIdx);
        let zData = getLinkedPcfData(gLinkedList[lightIdx][index + 1], dist, lightIdx);
        let yData = getLinkedPcfData(gLinkedList[lightIdx][index + gSMSize.x + 1], dist, lightIdx);
        let xData = getLinkedPcfData(gLinkedList[lightIdx][index + gSMSize.x], dist, lightIdx);

        float c = 80.0; // exponential shadow map
        float visFront = dot(w, float4(xData.visibilityFront, yData.visibilityFront, zData.visibilityFront, wData.visibilityFront));
        float visBack = dot(w, float4(xData.visibilityBack, yData.visibilityBack, zData.visibilityBack, wData.visibilityBack));
        float4 rawDepths = float4(xData.depth, yData.depth, zData.depth, wData.depth);

        // reformat to [0, 1]
        float4 linearDepths = saturate(rawDepths / gFar);
        dist = saturate(dist / gFar);
        //linearDepths = linearDepths * 2.0 - 1.0;
        //dist = dist * 2.0 - 1.0;

        float2 moments = float2(dot(w, linearDepths), dot(w, linearDepths * linearDepths));
        float shadowTest = ChebyshevUpperBound(moments, dist, 1e-10);

        //float4 moments = float4(dot(w, linearDepths), dot(w, linearDepths * linearDepths), dot(w, linearDepths * linearDepths * linearDepths), dot(w, linearDepths * linearDepths * linearDepths * linearDepths));
        //float shadowTest = msmHamburger(moments, dist);

        //float filteredDepth = dot(w, exp(c * linearDepths));
        //float shadowTest = exp(-c * dist) * filteredDepth;

        shadowTest = saturate(shadowTest);
        visibility = shadowTest * visFront + (1.0 - shadowTest) * visBack;
        //visibility = visFront;
        /*ListNode n = gLinkedList[lightIdx][index];
        float4 v = 1.0; // (-,+),(+,+)(+,-)(-,-) => BotLeft (x), BotRight (y), TopRight (z), TopLeft (w)
        while (n.depth < dist) {
            v.w = n.visibility;
            if (n.next == 0) break;
            uint nextIndex = n.next;
            n = gLinkedList[lightIdx][n.next];
            if (n.depth < dist) index = nextIndex; // only update if the next node is still in front (because index will be used to obtain the neighbors)
        }
        uint zIndex = gLinkedListNeighbors[lightIdx][index].right;
        uint xIndex = gLinkedListNeighbors[lightIdx][index].bot;
        ListNode nZ = gLinkedList[lightIdx][zIndex];
        ListNode nX = gLinkedList[lightIdx][xIndex];
        // TODO trace for correct visibility
        v.z = getLinkedVisibility(nZ, dist, lightIdx);
        v.x = getLinkedVisibility(nX, dist, lightIdx);
        uint yIndex = gLinkedListNeighbors[lightIdx][zIndex].bot;
        ListNode nY = gLinkedList[lightIdx][yIndex];
        v.y = getLinkedVisibility(nY, dist, lightIdx);
        visibility = dot(w, v);
        // visibility = (v.x + v.y + v.z + v.w) * 0.25;
        //visibility = max(max(v.x, v.y), max(v.z, v.w));
        */
    }
    else
    {
        dist = dist - 1e-2;       // TODO better bias
        uint2 shadowPixel = uint2(floor(shadowPos.xy * gSMSize));
        uint index = shadowPixel.y * gSMSize.x + shadowPixel.x;
        ListNode n = gLinkedList[lightIdx][index];
        // traverse visibility until dist is reached
        while (n.depth < dist) {
            visibility = n.visibility;
            if (n.next == 0) break;
            n = gLinkedList[lightIdx][n.next];
        }
    }
    #endif
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
