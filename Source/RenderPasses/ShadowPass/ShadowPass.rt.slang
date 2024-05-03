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
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Materials.IsotropicGGX;
import Rendering.Materials.Fresnel;
import Rendering.ShadowMaps.ShadowMap;

import ShadowPassData;
import LTTMask;

//Inputs
Texture2D<float4> gPosW;
Texture2D<float4> gFaceNormalW;
Texture2D<float2> gMVec;   //Optional
Texture2D<float4> gEmissive;   //Optional

//Optional Inputs Simplified Shading
Texture2D<float4> gGuideNormalW;
Texture2D<float4> gDiffuse;
Texture2D<float4> gSpecRough;

//Outputs
RWTexture2D<float4> gColor;
RWTexture2D<float4> gDebug;

cbuffer CB
{
    uint gFrameCount;   //Current Frame Count for the sample generator
    bool gLTTMaskValid;  //Tells if the hybrid mask is valid
}

//Defines for syntax highlighting
#ifndef SIMPLIFIED_SHADING
#define SIMPLIFIED_SHADING 0
#endif

#ifndef DEBUG_MODE
#define DEBUG_MODE 0
#endif

#ifndef HYBRID_ALPHA_ONLY
#define HYBRID_ALPHA_ONLY 0
#endif

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kShadowMode = SP_SHADOW_MODE;
static const bool kRayAlphaTest = ALPHA_TEST;
static const float kAmbientFactor = SP_AMBIENT;
static const float kEnvMapFactor = SP_ENV_FACTOR;
static const float kEmissiveFactor = SP_EMISSIVE;
static const bool kUseEnvMap= USE_ENV_MAP;
static const bool kUseEmissive = true;
static const bool kMipsEnabled = SHADOW_MIPS_ENABLED;
static const float kDisableAlphaTestAtDistance = DISABLE_ALPHATEST_DISTANCE;
static const bool kEnableHybridBlending = HYBRID_USE_BLENDING;
static const float2 kHybridBlendingRange = HYBRID_BLENDING_RANGE;   //x=Min; y=Range
static const bool kStochasticCascadedLevelEnabled = DEBUG_STOCH_CASC_ENABLED;
static const uint kDebugLightIndex = DEBUG_LIGHT_INDEX;

static const float kShadowCutoff = 0.5f;

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    bool visible;
    bool disableAlphaTest;
};

 float3 computeDdxPosW(float3 posW, float3 normW, float2 invFrameDim)
{
    float3 projRight = normalize(cross(normW, cross(normW, gScene.camera.data.cameraV)));
    float distanceToHit = length(posW - gScene.camera.data.posW);
    float2 ddNdc = float2(2.f, -2.f) * invFrameDim;
    float distRight = distanceToHit * ddNdc.x / dot(normalize(gScene.camera.data.cameraV), projRight);
    return distRight * projRight;
}

float3 computeDdyPosW(float3 posW, float3 normW, float2 invFrameDim)
{
    float3 projUp = normalize(cross(normW, cross(normW, gScene.camera.data.cameraU)));
    float distanceToHit = length(posW - gScene.camera.data.posW);
    float2 ddNdc = float2(2.f, -2.f) * invFrameDim;
    float distUp = distanceToHit * ddNdc.y / dot(normalize(gScene.camera.data.cameraU), projUp);
    return distUp * projUp;
}

bool sampleLight(const in LightData lightData, const float3 posW, const float3 N, out AnalyticLightSample ls){
    //Sample the light
    if (!evalLightApproximate(posW, lightData, ls))
        return false;

    //Quick check if point is shaded
    float NdotL = dot(N, ls.dir);
    if(NdotL <= 0 || all(ls.Li <= 0))
        return false;

    return true;
}

/** Return a value between 0 and 1 that represents how shadowed the current light sample is
*/
bool evalShadowMap(const in SMLightData smLightData, float3 posW, float3 faceN, float2 mVec , float random,out float shadow, out bool isDynamic){
    //Shadow test
    shadow = 0.f;
    isDynamic = false;
    bool useRay = (kShadowMode == (uint)SPShadowMode::RayShadows);
    float lod = 0.f;
    //Eval shadow map if mode is enabled
    if(!useRay){
        //Get lod mip level
        if(kMipsEnabled){
            float2 invFrameDim = 1.f / float2(DispatchRaysDimensions().xy);
            float3 posWDdx = computeDdxPosW(posW, faceN, invFrameDim);
            float3 posWDdy = computeDdyPosW(posW, faceN, invFrameDim);
            lod = gShadowMap.getSMLODLevel(smLightData, posW, posWDdx, posWDdy);    
        }
        if(kShadowMode == (uint)SPShadowMode::LeakTracing)
            useRay = !gShadowMap.evalShadowMap(posW, smLightData, float4(random) /*random (only x is used currently)*/, lod /*MipLevel*/, shadow, isDynamic, true);
        else
            useRay = !gShadowMap.evalShadowMap(posW, smLightData, float4(random) /*random (only x is used currently)*/, lod /*MipLevel*/, shadow);
    }
#if DISABLE_DYNAMIC_GEOMETRY_CHECK
    isDynamic = false;
#endif
    return useRay;
}

float evalShadowRay(in const AnalyticLightSample ls, float3 posW, float3 faceN, bool disableAlphaTest = false){
    float rtShadow = 0.f;
    RayDesc ray;
    ray.Origin = computeRayOrigin(posW, faceN);
    ray.Direction = ls.dir;
    ray.TMin = 0.0f;
    ray.TMax = ls.distance;   //No offset needed, as the light is analytic and does not exist in the scene as a physical object

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if ray is not terminated before
    rayData.disableAlphaTest = disableAlphaTest;

    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kShadowRayInstanceMask /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
    
    rtShadow = rayData.visible ? 1.f : 0.f;

    return rtShadow;
}

float3 handleDebugTex(bool useRay, bool isSMIsShadowed, float shadow, const in SMLightData smLightData, float3 posW, float3 faceN, float random){
    float3 debug = float3(0);
    #if DEBUG_MODE == 0 //Use Ray
        debug.x += useRay && kShadowMode != (uint) SPShadowMode::ShadowMap ? 1.f : 0.f;
        
    #elif DEBUG_MODE == 1 //Lod level debug
        float2 invFrameDim = 1.f / float2(DispatchRaysDimensions().xy);
        float3 posWDdx = computeDdxPosW(posW, faceN, invFrameDim);
        float3 posWDdy = computeDdyPosW(posW, faceN, invFrameDim);
        lod = gShadowMap.getSMLODLevel(smLightData, posW, posWDdx, posWDdy);    
        debug = float3(1,0,0);
        if(lod > 1)
            debug = float3(0,1,0);
        if(lod > 2)
            debug = float3(0,0,1);
        if(lod > 3)
            debug = float3(1,0,1);
        if(lod > 4)
            debug = float3(0,1,1);
        if(lod > 5)
            debug = float3(1,0,1);
            
    #elif DEBUG_MODE == 2 //Cacaded Level Debug
        if (smLightData.type == (uint) LightTypeSM::Directional)
        {
            uint cascLevel = 0;
            float2 tmpUV;
            float tmpDist;
            bool valid;
            if(kStochasticCascadedLevelEnabled)
                valid = gShadowMap.directionalDistanceAndSampleVec(posW,random, cascLevel, tmpUV, tmpDist);
            else
                valid = gShadowMap.directionalDistanceAndSampleVec(posW, cascLevel, tmpUV, tmpDist);

            if (valid)
            {
                debug = float3(1, 0, 0);            //Level 0: Red
                if(cascLevel == 1)
                    debug = float3(0, 0, 1);        //Level 1 : Blue
                else if(cascLevel == 2)
                    debug = float3(1, 0, 1);        //Level 2 : Purple
                else if(cascLevel == 3)
                    debug =float3(0, 1, 1);         //Level 3 : Cyan
                else if (cascLevel > 3)
                    debug = float3(1, 0.678, 0.118);//Level 4+ : Orange
            }
            else
            {
                debug = float3(0,1,0);  //Green Ray needed
            }
            
        }
    #elif DEBUG_MODE == 3   //Hybrid mask texture
        debug.x += useRay ? 1.f : 0.f;
        debug.y += isSMIsShadowed ? 1.f : 0.f;
        debug.z += useRay && (shadow < 0.5) ? 1.f : 0.f;
    #endif
        return debug;
}

/** Adjusts the normal of the supplied shading frame to reduce black pixels due to back-facing view direction.
*/
void adjustShadingNormal(const float3 V, const float3 faceN, inout float3 N)
{
    //Flip the shading normal to the same side as the face normal
    float signN = dot(N, faceN) >= 0.f ? 1.f : -1.f;
    N = signN * N;

    // Blend the shading normal towards the geometric normal at grazing angles.
    // This is to avoid the view vector from becoming back-facing.
    const float kCosThetaThreshold = 0.1f;
    float cosTheta = dot(V, N);
    if (cosTheta <= kCosThetaThreshold)
    {
        float t = saturate(cosTheta * (1.f / kCosThetaThreshold));
        N = normalize(signN * lerp(faceN, N, t));
    }
}

//Calculates the BRDF for the simplified shading model (DGF Microfaset BRDF)
float3 simplifiedShading(const float3 posW, const float3 N,const float3 viewDir ,const float3 diffuse,
                         const float3 specular, float roughness, const float3 toLight)
{
    const float3 H = normalize(viewDir + toLight);
    const float NdotV = saturate(dot(N, viewDir));
    const float NdotL = saturate(dot(N, toLight));
    const float NdotH = saturate(dot(N, H));
    const float LdotH = saturate(dot(toLight, H));

    if(NdotL == 0)
        return 0.f;
    
    //Disable delta lobes
    roughness = max(roughness, 0.08);
    
    //DGF Microfaset BRDF
    float D = evalNdfGGX(roughness * roughness, NdotH);
    float G = evalMaskingSmithGGXSeparable(roughness, NdotL, NdotV);
    float3 F = luminance(specular) < 1e-8f ? float3(0.f) :  evalFresnelSchlick(specular, 1.f, LdotH);

    //eval brdf terms
    float3 diffuseBRDF = diffuse * NdotL * M_1_PI;
    float3 specularBRDF = D * G * F / (4.f * NdotV);

    float3 sumWeights = diffuse + specular;
    float3 diffuseProb = luminance(sumWeights) < 1e-7f ? float3(1.0f) : saturate(diffuse / sumWeights);
    
    //Mix diffuse and specular BRDF
    float3 brdf = max(0.f, lerp(specularBRDF, diffuseBRDF, diffuseProb));
    return max(0.f, specularBRDF + diffuseBRDF);
}

float roughnessToLod(float roughness)
{
    float nEnvLods = log2(float(gScene.envMap.getDimensions().x));
    float maxEnvLod = max(nEnvLods - 4.0, 0.0);
    return saturate(roughness) * maxEnvLod;
}

// limit maximal intensity of rough materials (avoids bright sparkles)
float roughnessToIntensity(float roughness)
{
    return saturate(1.0 / (roughness + 0.1));
}

[shader("miss")]
void miss(inout ShadowRayData rayData)
{
    rayData.visible = true;
}

[shader("anyhit")]
void anyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    if (kRayAlphaTest)
    {
        if(rayData.disableAlphaTest){
            AcceptHitAndEndSearch();
            return;
        }

        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        const uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    //Load the texture info for shading
    float4 posTex = gPosW[pixel];
    float3 posW = posTex.xyz;
    bool valid = posTex.w > 0;
    float3 faceNormalW = gFaceNormalW[pixel].xyz;
    float2 mvec = gMVec[pixel];
    
    //Get view dir from camera
    float3 view = -gScene.camera.computeRayPinhole(pixel, frameDim).dir;
    
    float3 color = float3(0.f);
    float3 emissive = float3(0.f);
    float3 debugOut = float3(0.f);

    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 2 * 7);

    const uint lightCount = min(gScene.getLightCount(), 10);    //Limit to 10 due to the mask
        
    if (valid)
    {
        float3 normalW = gGuideNormalW[pixel].xyz;
        float3 diffuse = gDiffuse[pixel].xyz;
        float4 specRough = gSpecRough[pixel];

        //Flip Face Normal of double sided mats
        if(dot(faceNormalW, view) <= 0.f)
        {
            faceNormalW = -faceNormalW;
        }

        adjustShadingNormal(view, faceNormalW, normalW);

        uint globalMask = 0;

        //Loop over all lights
        for(uint i=0 ; i<lightCount; i++){
            //Sample light
            LightData lightData = gScene.getLight(i);
            AnalyticLightSample ls;
            if(!sampleLight(lightData, posW, normalW, ls))
                continue;

            //Eval Leak Tracing Test
            float camDist = distance(gScene.camera.getPosition(), posW);
            float random = sampleNext1D(sg);
            float shadow = 0.f;
            bool clampShadow = false;
            LTTProperties lttProps;
            bool useRay = kShadowMode == (uint)SPShadowMode::RayShadows;
            SMLightData smLightData = gShadowMap.getShadowMapLightData(lightData, i);
            if(!useRay){
                bool isDynamic = false;
                useRay = evalShadowMap(smLightData, posW, faceNormalW, mvec, random, shadow, isDynamic);
                #if DISABLE_DYNAMIC_GEOMETRY_CHECK
                    isDynamic = false;
                #endif
                if (kUseLTTMask && gLTTMaskValid) {
                    lttProps.setData(useRay, shadow < kShadowCutoff, camDist, i, isDynamic, smLightData.type == (uint)LightTypeSM::Directional);
                    useRay = evaluateLTTMask(DispatchRaysIndex().xy, mvec, lttProps , gFrameCount);
                }
            }

            #if HYBRID_ALPHA_ONLY
                if (!lttProps.shadowTest)
                    useRay |= true;
            #endif

            //Trace Ray
            float rtShadow = shadow;
            if (useRay)
                rtShadow = evalShadowRay(ls, posW, faceNormalW, camDist > kDisableAlphaTestAtDistance);

            //Write Mask
            if (kUseLTTMask) {
                LTTMask currMask = LTTMask(shadow < kShadowCutoff, useRay, useRay && (rtShadow < kShadowCutoff));
                addMaskToGlobalMask(i, currMask, globalMask);
            }

            // Set shadow value
            if (lttProps.clampShadow && !useRay)
                rtShadow = shadow < 0.5 ? 0.f : 1.f;

            if(kEnableHybridBlending && kShadowMode != (uint)SPShadowMode::RayShadows && smLightData.type == (uint)LightTypeSM::Directional){
                float blendT = saturate((camDist - kHybridBlendingRange.x)/kHybridBlendingRange.y);
                shadow = lerp(rtShadow, shadow, blendT);
            }else
                shadow = rtShadow;

            //Handle Debug
            if (is_valid(gDebug))
            {
                debugOut += handleDebugTex(useRay, shadow < kShadowCutoff, shadow, smLightData, posW, faceNormalW, random);
            }

            #if SHADOW_ONLY
                color += float3(shadow / lightCount);
            #else
            if(shadow > 0)
                color += simplifiedShading(posW,normalW, view, diffuse, specRough.xyz, specRough.w, ls.dir) * ls.Li * shadow;
            #endif //SHADOW_ONLY
        }

        // Write global mask
        writeLTTMask(pixel, globalMask);

        // Debug test
        #if DEBUG_MODE == 4 //Mask per light
        if (is_valid(gDebug)) {
            uint shift = kDebugLightIndex * 3;
            uint testMask = (globalMask >> shift) & 0x7;
            LTTMask debugMask = LTTMask(testMask);
            debugOut = float3(0);
            debugOut.x += debugMask.leakTracingTest ? 1.f : 0.f;
            debugOut.y += debugMask.shadowTest ? 1.f : 0.f;
            debugOut.z += debugMask.rayShadowed ? 1.f : 0.f;
        }
        #endif
        
        //Ambient Color
        #if !SHADOW_ONLY
        color += kAmbientFactor * diffuse;

        //Envmap
        if (kUseEnvMap)
        {
            float3 R = reflect(-view, normalW);
            float diffLod = roughnessToLod(specRough.w * 2.f);
            float specLod = roughnessToLod(specRough.w);
            float envMaxIntensity = roughnessToIntensity(specRough.w);
            //TODO Currently hacked. Do properly by using microfacet N
            float3 F = evalFresnelSchlick(specRough.xyz, 1.0 - specRough.w, max(0.0, dot(view, normalW)));
            color += kEnvMapFactor * diffuse * min(gScene.envMap.eval(normalW, diffLod), 1.0);
            color += kEnvMapFactor * F * min(gScene.envMap.eval(R, specLod), envMaxIntensity);
        }
        
        #if is_valid(gEmissive)
            emissive = gEmissive[pixel].xyz;
        #endif //Valid gEmissive
    
        //Add emissive after shading
        if (kUseEmissive)
            color += kEmissiveFactor * emissive;

        #endif //!SHADOW_ONLY

    //Divide the debug output by light count for some modes
    #if (DEBUG_MODE != 2) && (DEBUG_MODE != 4)
        if(is_valid(gDebug))
            debugOut /= lightCount;
    #endif

    }
    else if(kUseEnvMap)
    {
        color = kEnvMapFactor * gScene.envMap.eval(-view);
    }

    gColor[pixel] = float4(color, 1.f);
    if (is_valid(gDebug)) {
        gDebug[pixel] = float4(debugOut, 1.f);
    }
        
}
