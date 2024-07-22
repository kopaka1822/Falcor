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

/** Minimal path tracer.

    The purpose is to use it for validation of more complex renderers.
    The implementation here should be kept as simple/naive as possible.

    At each hit point (including the primary hit loaded from the V-buffer),
    analytic light sources (point, directional) are sampled uniformly using
    1 shadow ray, and 1 scatter ray is traced to sample the hemisphere.
    At hit/miss the scatter ray includes light from emissive surface and
    the environment map, respectively. Traversal stops at a fixed path length.

    Each type of light (analytic, emissive, env map) can be individually
    enabled/disabled from the host. This clutters the code a bit, but it is
    important as not all other renderes may support all three light types.

    The host sets the following defines:

    MAX_BOUNCES             Maximum number of indirect bounces (0 means no indirect).
    COMPUTE_DIRECT          Nonzero if direct illumination should be included.
    USE_IMPORTANCE_SAMPLING Nonzero if importance sampling should be used for sampling materials.
    USE_ANALYTIC_LIGHTS     Nonzero if Falcor's analytic lights should be used.
    USE_EMISSIVE_LIGHTS     Nonzero if emissive geometry should be used as lights.
    USE_ENV_LIGHT           Nonzero if env map is available and should be used as light source.
    USE_ENV_BACKGROUND      Nonzero if env map is available and should be used as background.
    is_valid_<name>         1 if optional I/O buffer with this name should be used.
*/

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Utils.PixelStats;
import PathSMStructs;
import Rendering.ShadowMaps.Oracle.ShadowMapOracle;

#if SM_GENERATION_RAYTRACING
    import Rendering.ShadowMaps.ShadowMapData;
    import Rendering.ShadowMaps.ShadowTestFunctions;
    import Rendering.ShadowMaps.ShadowMapHelpers;
#else
    import Rendering.ShadowMaps.ShadowMap;
#endif

#ifndef COUNT_SM
#define COUNT_SM 1
#endif
#ifndef USE_SEPERATE_LIGHT_SAMPLER
#define USE_SEPERATE_LIGHT_SAMPLER 0
#endif

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded.
    uint        gPRNGDimension;     // First available PRNG dimension.
    float       gNear;
    float       gFar;
    bool        gUseShadowMap;
    uint        gRayShadowMapRes;
    float       gLtBoundsMaxReduction;  //Max reduction per hit
    uint        gIterationCount;        //Current number of still iterations
}

cbuffer ShadowVPs
{
    float4x4 gRayShadowMapVP[COUNT_SM];
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional
Texture2D<float2> gRayShadowMap[COUNT_SM];
Texture2D<float2> gRayShadowMapMinMax[COUNT_SM];

// Outputs
RWTexture2D<float4> gOutputColor;
RWTexture2D<float4> gDebug;
RWTexture2D<uint> gShadowAccessDebugTex[COUNT_SM];

//Texture Sampler
SamplerState gShadowSamplerPoint;
SamplerState gShadowSamplerLinear;

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kMaxBounces = MAX_BOUNCES;
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const float kRayTMax = FLT_MAX;
static const bool kUseRussianRoulette = USE_RUSSIAN_ROULETTE;
static const float kLeakTracingBoundsStart = LT_BOUNDS_START;
static const uint kSeperateLightSamplerBlockSize = LIGHT_SAMPLER_BLOCK_SIZE;
static const bool kEnableShadowRay = USE_SHADOW_RAY;
static const bool kUseMinMaxSM = USE_MIN_MAX_SM;
static const bool kEnableDebug = USE_DEBUG;
static const bool kWriteDebug = WRITE_TO_DEBUG;
static const uint kDebugMode = DEBUG_MODE;
static const bool kDebugAccumulate = DEBUG_ACCUMULATE;

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    bool visible;
};

/** Payload for ray (up to 16B).
*/
struct RayHitData{
    PackedHitInfo packedHit;

    __init(){
        packedHit = {};
    }
}

/** Struct for used path data
*/
struct PathData
{
    float3  radiance;       ///< Accumulated outgoing radiance from path.
    bool    terminated;     ///< Set to true when path is terminated.
    float3  thp;            ///< Current path throughput. This is updated at each path vertex.
    uint    pathLength;     ///< Path length in number of path segments (0 at origin, 1 at first secondary hit, etc.). Max 2^31.
    float3  origin;         ///< Next path segment origin.
    float   leakTracingBounds; ///<Bounds for leakTracing
    float3  direction;      ///< Next path segment direction.
    SampleGenerator sg;     ///< Per-ray state for the sample generator (up to 16B).
#if USE_SEPERATE_LIGHT_SAMPLER
    SampleGenerator lightSg;///< Per-Ray sample generator for lights
#endif
#if USE_ORACLE_FUNCTION
    float distance;         ///< Distance the path traveled
    float lobeDistFactor;   ///< Factor how the lobe affects the distance of the ray 
#endif

    /** Initializes ray payload with default parameters.
    */
    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.pathLength = 0;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.leakTracingBounds = 0.0;
        this.sg = sg;

        #if USE_ORACLE_FUNCTION
        this.lobeDistFactor = 1.0;
        this.distance = 0.0;
        #endif
    }
};

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/
ShadingData loadShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDir, const ITextureSampler lod)
{
    VertexData v = {};
    uint materialID = {};

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedTriangleHit = hit.getDisplacedTriangleHit();
        v = gScene.getVertexData(displacedTriangleHit, -rayDir);
        materialID = gScene.getMaterialID(displacedTriangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        v = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfGridHit = hit.getSDFGridHit();
        v = gScene.getVertexDataFromSDFGrid(sdfGridHit, rayOrigin, rayDir);
        materialID = gScene.getMaterialID(sdfGridHit.instanceID);
    }
#endif

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    logPathVertex();

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

/** Traces a shadow ray towards a light source.
    \param[in] origin Ray origin for the shadow ray.
    \param[in] dir Direction from shading point towards the light source (normalized).
    \param[in] distance Distance to the light source.
    \return True if light is visible, false otherwise.
*/
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    logTraceRay(PixelStatsRayType::Visibility);
    ShadowRayData rayData;
    rayData.visible = false;    // Set to true by miss shader if ray is not terminated before
    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, ray, rayData);

    return rayData.visible;
}

/** Traces a scatter ray based on ray parameters stored in the ray payload.
    \param[in] rayData Describes the ray parameters. The struct is modified based on the result.
*/
void traceScatterRay(inout PathData rayData, inout RayHitData hitData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    uint rayFlags = 0;      // TODO: Set cull mode from the app
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, hitData);
}

#if SM_GENERATION_RAYTRACING
    bool evalShadowMap(uint lightIdx, float3 posW, float leakTracingBound ,out float shadow){
        shadow = 0.0;
        float4 shadowPos = mul(gRayShadowMapVP[lightIdx], float4(posW, 1));
        shadowPos.xyz /= shadowPos.w;

        shadowPos.y *= -1;  //Flip
        shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
        float lightDist = (shadowPos.w - gNear) / (gFar - gNear);

        if(any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
            return true;    //ray needed

        if (kUseMinMaxSM) {
            float2 gatherUV;
            float4 w = getGatherWeights(shadowPos.xy, gRayShadowMapRes, gatherUV);
            float4 minSMValues = gRayShadowMapMinMax[lightIdx].GatherRed(gShadowSamplerPoint, gatherUV);
            float4 maxSMValues = gRayShadowMapMinMax[lightIdx].GatherGreen(gShadowSamplerPoint, gatherUV);

            #if FILTER_SM_VARIANCE
                float2 minShadowValue = float2(0);
                float2 maxShadowValue = float2(0);
                for (uint i = 0; i < 4; i++) {
                    minShadowValue += float2(minSMValues[i], minSMValues[i] * minSMValues[i]) * w[i];
                    maxShadowValue += float2(maxSMValues[i], maxSMValues[i] * maxSMValues[i]) * w[i];
                }
                float minShadow = ChebyshevUpperBound(minShadowValue.xy, lightDist, 1e-9f);
                float maxShadow = ChebyshevUpperBound(maxShadowValue.xy, lightDist, 1e-9f);
            #elif FILTER_SM_ESVM
                float4 minShadowValueEvsm = float4(0);
                float4 maxShadowValueEvsm = float4(0);
                for (uint i = 0; i < 4; i++) {
                    float rescaledDepth = (2.f * minSMValues[i]) - 1.f;
                    float4 evsm = recreateEVSMFromSingleDepth(rescaledDepth);
                    minShadowValueEvsm += w[i] * evsm;
                    rescaledDepth = (2.f * maxSMValues[i]) - 1.f;
                    evsm = recreateEVSMFromSingleDepth(rescaledDepth);
                    maxShadowValueEvsm += w[i] * evsm;
                }
                float2 minShadowEvsm =  exponentialVarianceShadowTest(minShadowValueEvsm, lightDist);
                 float2 maxShadowEvsm =  exponentialVarianceShadowTest(maxShadowValueEvsm, lightDist);
                float minShadow = min(minShadowEvsm.x, minShadowEvsm.y);
                float maxShadow = min(maxShadowEvsm.x, maxShadowEvsm.y);
            #else // FILTER_SM_MSM
                float4 minShadowValueMsm = float4(0);
                float4 maxShadowValueMsm = float4(0);
                for (uint i = 0; i < 4; i++) {
                    minShadowValueMsm += float4(minSMValues[i], minSMValues[i] * minSMValues[i], minSMValues[i] * minSMValues[i] * minSMValues[i],minSMValues[i] * minSMValues[i] * minSMValues[i] * minSMValues[i]) * w[i];
                    maxShadowValueMsm += float4(maxSMValues[i], maxSMValues[i] * maxSMValues[i], maxSMValues[i] * maxSMValues[i]* maxSMValues[i],maxSMValues[i] * maxSMValues[i] * maxSMValues[i] * maxSMValues[i]) * w[i];
                }
                float minShadow = msmHamburger(minShadowValueMsm, lightDist);
                float maxShadow = msmHamburger(maxShadowValueMsm, lightDist);
            #endif
            if (minShadow <= leakTracingBound && maxShadow <= leakTracingBound)
                shadow = 0.0;
            else if (minShadow >= (1.0 - leakTracingBound) && maxShadow >= (1.0 - leakTracingBound))
                shadow = 1.0;
            else{
                shadow = 0.5;
                return true;
            }     
        } else {
            float2 gatherUV;
            float4 w = getGatherWeights(shadowPos.xy, gRayShadowMapRes, gatherUV);
            float4 shadowValues = gRayShadowMap[lightIdx].GatherRed(gShadowSamplerPoint, gatherUV);
            #if FILTER_SM_VARIANCE
                float2 shadowValue = float2(0.0);
                //Interpolate
                [unroll]
                for (uint i = 0; i < 4; i++) {
                    shadowValue += float2(shadowValues[i], shadowValues[i] * shadowValues[i]) * w[i];
                }
                shadow = ChebyshevUpperBound(shadowValue, lightDist, 1e-9f);
            #elif FILTER_SM_ESVM
                float4 shadowValueEvsm = float4(0);
                [unroll]
                for (uint i = 0; i < 4; i++) {
                    float rescaledDepth = (2.f * shadowValues[i]) - 1.f;
                    float4 evsm = recreateEVSMFromSingleDepth(rescaledDepth);
                    shadowValueEvsm += w[i] * evsm;
                }
                float2 shadowEvsm =  exponentialVarianceShadowTest(shadowValueEvsm, lightDist);
                shadow = min(shadowEvsm.x, shadowEvsm.y);
            #else // FILTER_SM_MSM
                float4 shadowValueMsm = float4(0);
                for (uint i = 0; i < 4; i++) {
                    shadowValueMsm += float4(shadowValues[i], shadowValues[i] * shadowValues[i], shadowValues[i] * shadowValues[i] * shadowValues[i],shadowValues[i] * shadowValues[i] * shadowValues[i] * shadowValues[i]) * w[i];
                }
                shadow = msmHamburger(shadowValueMsm, lightDist);
            #endif
            //Leak Tracing Test
            if (shadow <= leakTracingBound)
                shadow = 0.0;
            else if (shadow >= (1.0 - leakTracingBound))
                shadow = 1.0;
            else
                return true;
        }
    
        return false;       //No Ray
    }
#endif
/* Makes an entry in the shadow map access texture for the specified light if called
*/
void addShadowMapAccessEntry(uint lightIdx, float3 posW){
    float4 shadowPos = mul(gRayShadowMapVP[lightIdx], float4(posW, 1));
    shadowPos.xyz /= shadowPos.w;

    shadowPos.y *= -1;  //Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
    //Outside
    if(any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
        return;    

    uint2 shadowMapPixel = uint2(gRayShadowMapRes * shadowPos.xy);
    //Call an atomic add for the shadow map pixel
    uint origValue = 0;
    InterlockedAdd(gShadowAccessDebugTex[lightIdx][shadowMapPixel], 1u, origValue);
}

/** Evaluates the direct illumination from analytic lights.
    This function samples Falcor's light list uniformly with one shadow ray.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in,out] sg SampleGenerator object.
    \return Outgoing radiance in view direction.
*/
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout PathData pathData, inout float3 debug)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return float3(0.f);

    //Select a light
    float lightSelect = 0.0;
    #if USE_SEPERATE_LIGHT_SAMPLER
        lightSelect = sampleNext1D(pathData.lightSg);
    #else
        lightSelect = sampleNext1D(pathData.sg);
    #endif

    // Pick one of the analytic light sources randomly with equal probability.
    const uint lightIndex = min(uint(lightSelect * lightCount), lightCount - 1);
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    // Sample local light source.
    AnalyticLightSample ls;
    LightData lightData = gScene.getLight(lightIndex);
    if (!sampleLight(sd.posW,lightData , pathData.sg, ls)) return float3(0.f);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection)  || all(ls.Li <= 0)) // 
        return float3(0.f);

    bool useRay = !gUseShadowMap;

    #if USE_ORACLE_FUNCTION || !SM_GENERATION_RAYTRACING
        //Create SM Data struct
        SMLightData smLightData;
        smLightData.posW = lightData.posW;
        smLightData.dirW = lightData.dirW;
        smLightData.type = getShadowMapLightType(lightData);
        smLightData.index = lightIndex;
    #endif 
    //Oracle
    #if USE_ORACLE_FUNCTION
        bool disableOracle = ShadowMapOracle::kIgnoreOracleForDirect && pathData.pathLength == 0;
        if(gUseShadowMap && !disableOracle){
            float oracle = ShadowMapOracle::oracleFunction(smLightData, sd, pathData.distance);
            useRay = ShadowMapOracle::oracleNeedRay(oracle);
        }else if(disableOracle){
            useRay = true;
        }
    #endif

    float shadow = 0.0;
    if(!useRay){
        #if SM_GENERATION_RAYTRACING
            useRay = evalShadowMap(lightIndex, sd.posW, pathData.leakTracingBounds,shadow);
        #else //SM_GENERATION_RAYTRACING
            bool isDynamic;
            useRay = !gShadowMap.evalShadowMap(sd.posW, smLightData, 0.5f /*random (unused)*/, 1.0 /*MipLevel*/, shadow, isDynamic, true);
        #endif //SM_GENERATION_RAYTRACING

        #if USE_ORACLE_FUNCTION
            #if !SMORACLE_USE_LEAK_TRACING
                useRay = false;
                shadow = shadow < 0.5 ? 0.0 : 1.0;
            #endif //!SMORACLE_USE_LEAK_TRACING
        #endif //USE_ORACLE_FUNCTION
    }

    float smShadow = shadow;
    if(useRay && kEnableShadowRay){
        // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
        const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
        // Test visibility by tracing a shadow ray.
        bool V = traceShadowRay(origin, ls.dir, ls.distance);
        shadow = V ? 1.0 : 0.0;
    }

    if(kEnableDebug){
        if(kDebugMode == (uint)PathSMDebugModes::LeakTracingMask){
            debug += float3(shadow, useRay ? 1.0 : 0.0, smShadow) * invPdf;
        }else if (kDebugMode == (uint)PathSMDebugModes::RaySMDiff || kDebugMode == (uint)PathSMDebugModes::ShadowMapRayDiff){
            // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
            const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
            // Test visibility by tracing a shadow ray.
            bool RayV = traceShadowRay(origin, ls.dir, ls.distance);
            bool shadowV = shadow > 0.5;
            float3 brdf = mi.eval(sd, ls.dir, pathData.sg);
            if(RayV != shadowV && all(brdf > 0)){
                debug.x += 10000.0;
                if(kDebugMode == (uint)PathSMDebugModes::ShadowMapRayDiff)
                    addShadowMapAccessEntry(lightIndex, sd.posW);
            }
            else if(RayV)
                debug += (brdf * shadow);
        }else if(kDebugMode == (uint)PathSMDebugModes::ShadowMapAccess){ // Add an entry for every access
            addShadowMapAccessEntry(lightIndex, sd.posW);
        }             
    }

    //if(shadow <= 0.0)
    //    return float3(0);
    // Evaluate contribution.
    return mi.eval(sd, ls.dir, pathData.sg) * ls.Li * invPdf * shadow;

}

/** Generate a new scatter ray or terminate.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in] isCurveHit True if on curve hit.
    \param[in] rayOrigin Ray origin for the new ray.
    \param[in,out] rayData Ray payload.
    \return True if the path continues.
*/
bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, bool isCurveHit, float3 rayOrigin, inout PathData pathData)
{
    // Sample material.
    BSDFSample bsdfSample;
    if (mi.sample(sd, pathData.sg, bsdfSample, kUseImportanceSampling))
    {
        pathData.origin = rayOrigin;
        if (!isCurveHit && bsdfSample.isLobe(LobeType::Transmission))
        {
            pathData.origin = sd.computeNewRayOrigin(false);
        }
        pathData.direction = bsdfSample.wo;
        pathData.thp *= bsdfSample.weight;
        let miProperties = mi.getProperties(sd);
        float roughSq = miProperties.roughness * miProperties.roughness;
        pathData.leakTracingBounds += gLtBoundsMaxReduction * roughSq;
        pathData.leakTracingBounds = min(0.5, pathData.leakTracingBounds);
        #if USE_ORACLE_FUNCTION
            pathData.lobeDistFactor = ShadowMapOracle::getOracleDistanceFunctionFactor(miProperties, bsdfSample);
        #endif //USE_ORACLE_FUNCTION

        return any(pathData.thp > 0.f);
    }

    return false;
}

/** Process a hit.
    Loads the shading data, samples analytic lights and samples a new scatter ray.
    Terminates the path if maximum number of bounces is reached.
    \param[in] hit Hit info.
    \param[in,out] rayData Ray payload.

*/
void handleHit(const HitInfo hit, inout PathData pathData, inout float3 debug)
{
    const bool isCurveHit = hit.getType() == HitType::Curve;
    let lod = ExplicitLodTextureSampler(0.f);

    // Load shading data.
    ShadingData sd = loadShadingData(hit, pathData.origin, pathData.direction, lod);

    #if USE_ORACLE_FUNCTION
        pathData.distance += distance(sd.posW, pathData.origin) * pathData.lobeDistFactor;
    #endif

    // Create material instance.
    uint hints = 0;
    hints |= (uint) MaterialInstanceHints::AdjustShadingNormal;
    let mi = gScene.materials.getMaterialInstance(sd, lod, hints);

    // Add emitted light.
    if (kUseEmissiveLights && (kComputeDirect || pathData.pathLength > 0))
    {
        pathData.radiance += pathData.thp * mi.getProperties(sd).emission;
    }

    // Check whether to terminate based on max depth.
    if (pathData.pathLength >= kMaxBounces)
    {
        pathData.terminated = true;
        return;
    }

    // Compute ray origin for new rays spawned from the hit.
    float3 rayOrigin;
    if (isCurveHit)
    {
        // For curves, we set the new origin at the sphere center.
        rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
    }
    else
    {
        rayOrigin = sd.computeNewRayOrigin();
    }

    // Add contribution of direct light from analytic lights.
    if (kUseAnalyticLights)
    {        
        float3 Lr = evalDirectAnalytic(sd, mi, pathData, debug);
        pathData.radiance += pathData.thp * Lr;
    }

    // Generate scatter ray for the next path segment.
    // The raygen shader will continue the path based on the returned payload.
    if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, pathData))
    {
        pathData.terminated = true;
        return;
    }

    pathData.pathLength++;
}

/** This is the main entry point for the minimal path tracer.

    One path per pixel is generated, which is traced into the scene.
    The path tracer is written as a for-loop over path segments.

    Built-in light sources (point, directional) are sampled explicitly at each
    path vertex. The contributions from area lights (env map and mesh lights)
    are explicitly added by the scatter ray hit/miss shaders.

    \param[in] pixel Pixel to trace a path for.
    \param[in] frameDim Dimension of the frame in pixels.
    \return Returns the estimated color (radiance).
*/
float3 tracePath(const uint2 pixel, const uint2 frameDim ,out float3 debug)
{
    float3 outColor = float3(0.f);
    debug = float3(0.f);

    const float3 primaryRayOrigin = gScene.camera.getPosition();
    const float3 primaryRayDir = getPrimaryRayDir(pixel, frameDim, gScene.camera);

    HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (hit.isValid())
    {
        // Pixel represents a valid primary hit. Compute its contribution.

        const bool isCurveHit = hit.getType() == HitType::Curve;
        let lod = ExplicitLodTextureSampler(0.f);

        // Load shading data.
        ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir, lod);

        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod);

        // Create sample generator.
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount);
    #if USE_SEPERATE_LIGHT_SAMPLER 
        SampleGenerator lightSg = SampleGenerator(pixel / kSeperateLightSamplerBlockSize, gFrameCount + 1 * 32);
    #endif //USE_SEPERATE_LIGHT_SAMPLER

        // Advance the generator to the first available dimension.
        // TODO: This is potentially expensive. We may want to store/restore the state from memory if it becomes a problem.
        for (uint i = 0; i < gPRNGDimension; i++){
            sampleNext1D(sg);
        #if USE_SEPERATE_LIGHT_SAMPLER 
            sampleNext1D(lightSg);
        #endif
        }

        // Compute ray origin for new rays spawned from the G-buffer.
        float3 rayOrigin;
        if (isCurveHit)
        {
            // For curves, we set the new origin at the sphere center.
            rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
        }
        else
        {
            rayOrigin = sd.computeNewRayOrigin();
        }

        
        // Prepare ray payload.
        PathData pathData = PathData(sg);
        pathData.leakTracingBounds = kLeakTracingBoundsStart;
        #if USE_SEPERATE_LIGHT_SAMPLER
            pathData.lightSg = lightSg;
        #endif
        #if USE_ORACLE_FUNCTION
            pathData.distance = distance(gScene.camera.data.posW, sd.posW);  //set the currently traveled distance
        #endif

        if (kComputeDirect)
        {
            // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
            outColor += mi.getProperties(sd).emission;

            // Add contribution of direct light from analytic lights.
            // Light probe and mesh lights are handled by the scatter ray hit/miss shaders.
            outColor += kUseAnalyticLights ? evalDirectAnalytic(sd, mi,pathData, debug) : float3(0.f);
        }

        // Generate scatter ray.
        if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, pathData)) pathData.terminated = true;

        RayHitData hitData = RayHitData();
        // Follow path into the scene and compute its total contribution.
        uint pathDepth;
        for (pathDepth = 0; pathDepth <= kMaxBounces && !pathData.terminated; pathDepth++)
        {
            // Trace scatter ray. If it hits geometry, the closest hit shader samples
            // direct illumination and generates the next scatter ray.
            traceScatterRay(pathData, hitData);
            hit = HitInfo(hitData.packedHit);
            //When scene is missed, add the env map color to the path
            if(!hit.isValid()){
                float3 envColor = kUseEnvBackground ? gScene.envMap.eval(pathData.direction) : kDefaultBackgroundColor;
                pathData.radiance += envColor * pathData.thp;
                break;
            }
            logTraceRay(PixelStatsRayType::ClosestHit);
            handleHit(hit, pathData, debug);

            //Russian Roulette
            if(kUseRussianRoulette){
                const float rrVal = luminance(pathData.thp);
                const float prob = max(0.f, 1.f - rrVal);
                if (sampleNext1D(pathData.sg) < prob)
                {
                    break;
                }
                pathData.thp /= 1.f - prob;
            }
        }
        logPathLength(pathDepth);
        // Store contribution from scatter ray.
        outColor += pathData.radiance;
    }
    else
    {
        // Background pixel.
        outColor = kUseEnvBackground ? gScene.envMap.eval(primaryRayDir) : kDefaultBackgroundColor;
        logPathLength(0);
    }

    return outColor;
}

//
// Shader entry points for miss shaders.
//

[shader("miss")]
void scatterMiss(inout RayHitData rayData)
{
    rayData.packedHit = {};
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void scatterTriangleMeshAnyHit(inout RayHitData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    logTraceRay(PixelStatsRayType::AnyHit);
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f)) IgnoreHit();
}

[shader("closesthit")]
void scatterTriangleMeshClosestHit(inout RayHitData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    rayData.packedHit = HitInfo(triangleHit).getData();
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    logTraceRay(PixelStatsRayType::AnyHitVis);
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f)) IgnoreHit();
}

//
// Shader entry points for DisplacedTriangleMesh hit groups.
//

[shader("intersection")]
void displacedTriangleMeshIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    DisplacedTriangleMeshIntersector::Attribs attribs;
    float t;
    if (DisplacedTriangleMeshIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterDisplacedTriangleMeshClosestHit(inout RayHitData rayData, DisplacedTriangleMeshIntersector::Attribs attribs)
{
    DisplacedTriangleHit displacedTriangleHit;
    displacedTriangleHit.instanceID = getGeometryInstanceID();
    displacedTriangleHit.primitiveIndex = PrimitiveIndex();
    displacedTriangleHit.barycentrics = attribs.barycentrics;
    displacedTriangleHit.displacement = attribs.displacement;
    rayData.packedHit = HitInfo(displacedTriangleHit).getData();
}

//
// Shader entry points for Curve hit groups.
//

[shader("intersection")]
void curveIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    CurveIntersector::Attribs attribs;
    float t;
    if (CurveIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterCurveClosestHit(inout RayHitData rayData, CurveIntersector::Attribs attribs)
{
    CurveHit curveHit;
    curveHit.instanceID = getGeometryInstanceID();
    curveHit.primitiveIndex = PrimitiveIndex();
    curveHit.barycentrics = attribs.barycentrics;
    rayData.packedHit = HitInfo(curveHit).getData();
}

//
// Shader entry points for SDFGrid hit groups.
//

[shader("intersection")]
void sdfGridIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    SDFGridHitData sdfGridHitData;
    float t;
    if (SDFGridIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), sdfGridHitData, t))
    {
        ReportHit(t, 0, sdfGridHitData);
    }
}

[shader("closesthit")]
void scatterSdfGridClosestHit(inout RayHitData rayData, SDFGridHitData sdfGridHitData)
{
    SDFGridHit sdfGridHit;
    sdfGridHit.instanceID = getGeometryInstanceID();
    sdfGridHit.hitData = sdfGridHitData;
    rayData.packedHit = HitInfo(sdfGridHit).getData();
}

//
// Shader entry point for ray generation shader.
//

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    logSetPixel(pixel);
    float3 debug = float3(0);
    float3 color = tracePath(pixel, frameDim, debug);

    gOutputColor[pixel] = float4(color, 1.f);
    if(kWriteDebug)
    {
        float count = float(gIterationCount);
        if(kDebugAccumulate){
            float4 lastFrame = gDebug[pixel];
            float curWeight = 1.0 / (count + 1);
            if(count > 0){
                debug = ((lastFrame.xyz * count) + debug) * curWeight;
            } 
        }
        gDebug[pixel] = float4(debug, 1.f);
    }
        
}
