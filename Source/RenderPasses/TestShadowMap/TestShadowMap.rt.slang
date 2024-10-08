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

/** Shadow Map test shader
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
import SMStructs;

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

#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded..
    bool        gUseShadowMap;
    uint        gRayShadowMapRes;
    uint        gIterationCount;        //Current number of still iterations
    float       gDebugFactor;           //Factor for the debug value
    bool        gUseRayForDirect;       //Always uses Ray for direct light
}

cbuffer ShadowNearFar{
    float2 gSMNearFar[COUNT_SM];
}
cbuffer ShadowVPs
{
    float4x4 gRayShadowMapVP[COUNT_SM];
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional
Texture2D<SM_FORMAT> gRayShadowMap[COUNT_SM];
Texture2D<float> gReverseShadowMap;
Texture2D<float> gUseRayMask;

// Outputs
RWTexture2D<float4> gOutputColor;
RWTexture2D<float4> gDebug;
RWTexture2D<uint> gShadowAccessDebugTex[COUNT_SM];

//Texture Sampler
SamplerState gShadowSamplerPoint;
SamplerState gShadowSamplerLinear;

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const bool kEnableShadowRay = USE_SHADOW_RAY;
static const bool kEnableDebug = USE_DEBUG;
static const bool kWriteDebug = WRITE_TO_DEBUG;
static const uint kDebugMode = DEBUG_MODE;
static const bool kDebugAccumulate = DEBUG_ACCUMULATE;
static const uint kLightSampleMode = PATHSM_LIGHT_SAMPLE_MODE;
static const bool kDistributeRayOutsideOfShadowMap = DISTRIBUTE_RAY_OUTSIDE_SM;
static const bool kCheckForNaN = CHECK_FOR_NAN;

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
    SampleGenerator sg;     ///< Per-ray state for the sample generator (up to 16B).

    /** Initializes ray payload with default parameters.
    */
    __init(SampleGenerator sg)
    {
        this.sg = sg;
    }
};

//For RIS light sampling
struct RISReservoir{
    uint lightIdx;      //Light Index
    float wSum;       //Weight and Weight sum
    //uint M;         //Unused, as all possible Samples are seen

    /** Initializes ray payload with default parameters.
    */
    __init(){
        lightIdx = 0;
        wSum = 0.0;
        //M = 0;
    }

    [mutating] bool update(uint index, float risWeight, float rnd){
        wSum += risWeight;
        //M += 1;
        bool selectSample = (rnd * wSum < risWeight);
        if(selectSample){
            lightIdx = index;
        }
        return selectSample;
    }

    [mutating] void finalize(float targetFunction){
        /* //When not looping through all lights
        if(targetFunction > 0 && M > 0){
            wSum = (1.0 / targetFunction) * ((1.0/M) * wSum);
            r.M = 1; 
        */
        if(targetFunction > 0){
            wSum = wSum / targetFunction;
        }else{
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
    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    return rayData.visible;
}

AnalyticLightSample sampleLightIndex(float3 posW, uint lightCount, inout PathData pathData, out uint lightIndex, out float invPdf, out uint lightType) {
    AnalyticLightSample ls = {};

    if (kLightSampleMode == (uint)SMLightSampleMode::Uniform) {
        float lightSelect = 0.0;
        lightSelect = sampleNext1D(pathData.sg);

        // Pick one of the analytic light sources randomly with equal probability.
        lightIndex = min(uint(lightSelect * lightCount), lightCount - 1);
        invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

        // Sample local light source.
        LightData lightData = gScene.getLight(lightIndex);
        lightType = getShadowMapLightType(lightData);
        sampleLight(posW, lightData, pathData.sg, ls);
    } else if (kLightSampleMode == (uint)SMLightSampleMode::RIS) {
        RISReservoir r = RISReservoir();
        LightData selectedLightData = {};
        //Loop through all lights
        for(uint i=0; i<lightCount; i++){
            float randomNum = i == 0 ? 0.5 : sampleNext1D(pathData.sg);     //Random number
            LightData lightData = gScene.getLight(i);
            AnalyticLightSample itLs;
            sampleLight(posW, lightData, pathData.sg, itLs);
            float w = luminance(itLs.Li); //Take the Light attenuation as weight
            if(r.update(i, w, randomNum)){
                ls = itLs;
                selectedLightData = lightData;
            }
        }
        r.finalize(luminance(ls.Li));  //targetFunction
        //Out data
        lightIndex = r.lightIdx;
        invPdf = r.wSum;    //W
        lightType = getShadowMapLightType(selectedLightData);
    }
    else { // Invalid type
        lightIndex = 0;
        invPdf = 1.0;
        lightType = 0;
    } 

    return ls;
}

#if SM_GENERATION_RAYTRACING
    bool evalShadowMap(uint lightIdx, float3 posW ,out float shadow){
        shadow = 0.0;
        float2 nearFar = gSMNearFar[lightIdx];
        float4 shadowPos = mul(gRayShadowMapVP[lightIdx], float4(posW, 1));
        shadowPos.xyz /= shadowPos.w;

        shadowPos.y *= -1;  //Flip
        shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
        float lightDist = (shadowPos.w - nearFar.x) / (nearFar.y - nearFar.x);

        if(any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
            return kDistributeRayOutsideOfShadowMap;    //TODO set per define

        SM_FORMAT shadowValue = gRayShadowMap[lightIdx].SampleLevel(gShadowSamplerLinear, shadowPos.xy, 0);
        float minDepth = 1.0;
        #if FILTER_SM_VARIANCE
            shadow = ChebyshevUpperBound(shadowValue, lightDist, 1e-7f);
        #elif FILTER_SM_ESVM
            float2 shadowEvsm =  exponentialVarianceShadowTest(shadowValue, lightDist);
            shadow = min(shadowEvsm.x, shadowEvsm.y);
        #elif FILTER_SM_MSM
            shadow = msmHamburger(shadowValue, lightDist);
        #endif
        
        return false;       //No Ray
    }
#endif

//Reverse shadow map test pass
#if SM_GENERATION_RAYTRACING
    bool evalShadowMapReverse(uint lightIdx, float3 posW ,out float shadow){
        shadow = 0.0;
        //Only supports variance
        #if !FILTER_SM_VARIANCE
        return false;
        #endif

        float2 nearFar = gSMNearFar[lightIdx];
        float4 shadowPos = mul(gRayShadowMapVP[lightIdx], float4(posW, 1));
        shadowPos.xyz /= shadowPos.w;

        shadowPos.y *= -1;  //Flip
        shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
        float lightDist = (shadowPos.w - nearFar.x) / (nearFar.y - nearFar.x);

        if(any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
            return kDistributeRayOutsideOfShadowMap;    //TODO set per define

        float2 gatherUV;
        float4 w = getGatherWeights(shadowPos.xy, gRayShadowMapRes, gatherUV);
        float4 reverseValues = gReverseShadowMap.GatherRed(gShadowSamplerPoint, gatherUV);
        float reverseValue = gReverseShadowMap.SampleLevel(gShadowSamplerPoint, shadowPos.xy, 0);
        float rayMask = gUseRayMask.SampleLevel(gShadowSamplerPoint, shadowPos.xy, 0);
        float4 shadowValueM1 = gRayShadowMap[lightIdx].GatherRed(gShadowSamplerLinear, gatherUV);
        float minDepth = 1.0;

        float4 shadowValueM2 = gRayShadowMap[lightIdx].GatherGreen(gShadowSamplerLinear,gatherUV);
        float2 shadowValue = float2(0.0);
        [unroll]
        for (uint i = 0; i < 4; i++) {
            minDepth = min(minDepth, shadowValueM1[i]);
            shadowValue += float2(shadowValueM1[i], shadowValueM2[i]) * w[i];
        }
        shadow = ChebyshevUpperBound(shadowValue, lightDist, 1e-7f);

        if (rayMask > 0) {
            bool anySmaller = false;
            for (uint i = 0; i < 4; i++) {
                anySmaller |= reverseValues[i] > 0 && ((reverseValues[i]) > minDepth);
            }
            shadow = anySmaller ? 0.0 : shadow;
            if (false) {
                float2 revShadowValue = float2(0.0);
                float2 revShadowValueLoop = float2(0);
                // Interpolate
                float wSum = 0;
                for (uint i = 0; i < 4; i++) {
                    if (reverseValues[i] > 0) {
                        revShadowValueLoop += float2(reverseValues[i], reverseValues[i] * reverseValues[i]) * w[i];
                        wSum += w[i];
                    }

                    revShadowValue = revShadowValueLoop / wSum;
                }
                shadow = ChebyshevUpperBound(revShadowValue, lightDist, 1e-7f);
            }
            if (kDebugMode == (uint)PathSMDebugModes::SparseShadowDepths) {
                uint2 pixel = uint2(1024, 1024) * shadowPos.xy;
                gDebug[pixel] = float4(reverseValue, shadowValue.x, shadowValue.y, lightDist);
            }
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

    // Sample a light
    uint lightIndex = 0;
    float invPdf = 1.0;
    uint lightType = 0;
    AnalyticLightSample ls = sampleLightIndex(sd.posW, lightCount, pathData, lightIndex, invPdf, lightType);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection)  || all(ls.Li <= 0)) // 
        return float3(0.f);

    bool useRay = !gUseShadowMap || (gUseRayForDirect);

    #if !SM_GENERATION_RAYTRACING
        //Create SM Data struct
    SMLightData smLightData;
        smLightData.posW = ls.posW;
        smLightData.dirW = ls.dir;
        smLightData.type = lightType;
        smLightData.index = lightIndex;
    #endif    

    float shadow = 0.0;

    if(!useRay){
        #if SM_GENERATION_RAYTRACING
            #if USE_REVERSE_SM
                useRay = evalShadowMapReverse(lightIndex, sd.posW,shadow);
            #else
                useRay = evalShadowMap(lightIndex, sd.posW,shadow);
            #endif 
        #else //SM_GENERATION_RAYTRACING
            bool isDynamic;
            useRay = !gShadowMap.evalShadowMap(sd.posW, smLightData, 0.5f /*random (unused)*/, 1.0 /*MipLevel*/, shadow, isDynamic, true);
        #endif //SM_GENERATION_RAYTRACING
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
            bool rayV = traceShadowRay(origin, ls.dir, ls.distance);
            float raySVal = rayV ? 1.0 : 0.0;
            float diff = abs(raySVal - shadow);
            float3 brdf = mi.eval(sd, ls.dir, pathData.sg);
            if(diff > 0.1 && all(brdf > 0)){
                debug.x += 10000.0;
                if(kDebugMode == (uint)PathSMDebugModes::ShadowMapRayDiff)
                    addShadowMapAccessEntry(lightIndex, sd.posW);
            }
            else if(rayV)
                debug += (brdf * shadow);
        }else if(kDebugMode == (uint)PathSMDebugModes::ShadowMapAccess){ // Add an entry for every access
            addShadowMapAccessEntry(lightIndex, sd.posW);
        }          
    }

    // Evaluate contribution.
    return mi.eval(sd, ls.dir, pathData.sg) * ls.Li * invPdf * shadow;

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
        let lod = ExplicitLodTextureSampler(0.f);
        // Load shading data.
        ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir, lod);

        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod);

        // Create sample generator.
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount);

        // Compute ray origin for new rays spawned from the G-buffer.
        float3 rayOrigin = sd.computeNewRayOrigin();

        // Prepare ray payload.
        PathData pathData = PathData(sg);
        
        // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
        outColor += mi.getProperties(sd).emission;

        // Add contribution of direct light from analytic lights.
        // Light probe and mesh lights are handled by the scatter ray hit/miss shaders.
        outColor += evalDirectAnalytic(sd, mi,pathData, debug);
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
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

//
// Shader entry points for TriangleMesh hit groups.
//
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

    if(kCheckForNaN){
        if(any(isnan(color) || isinf(color))){
            color = float3(0);
        }
    }

    gOutputColor[pixel] = float4(color, 1.f);
    if(kWriteDebug)
    {
        debug *= gDebugFactor;
        float count = float(gIterationCount);
        if (kDebugAccumulate) {
            float4 lastFrame = gDebug[pixel];
            float curWeight = 1.0 / (count + 1);
            if(count > 0){
                debug = ((lastFrame.xyz * count) + debug) * curWeight;
            } 
        }
        gDebug[pixel] = float4(debug, 1.f);
    }
        
}
