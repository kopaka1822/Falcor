#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Sampling.SampleGenerator;

//Set Defines if they where not handled on the CPU
#ifndef NUM_SHADOW_MAPS_CUBE
    #define NUM_SHADOW_MAPS_CUBE 1
#endif
#ifndef NUM_SHADOW_MAPS_MISC
    #define NUM_SHADOW_MAPS_MISC 1
#endif
#ifndef MULTIPLE_SHADOW_MAP_TYPES
    #define MULTIPLE_SHADOW_MAP_TYPES 1
#endif

struct ShadowMap
{
    static const bool kMultipleSMTypes = MULTIPLE_SHADOW_MAP_TYPES;

    SamplerState gShadowSampler;
    TextureCube<float> gShadowMapCube[NUM_SHADOW_MAPS_CUBE];
    Texture2D<float> gShadowMap[NUM_SHADOW_MAPS_MISC];
    StructuredBuffer<uint> gShadowMapIndexMap;
    StructuredBuffer<float4x4> gShadowMapVPBuffer;

    float gShadowMapNearPlane;
    float gShadowMapFarPlane;
    float gSMworldAcneBias;
    float gDirectionalOffset;
    
    uint gShadowMapRes;
    float3 gSceneCenter;

    uint gShadowMapResCube;
    float gPoissonDiscRad;
    float gPoissonMiscRad;
    uint _pad;

    static const bool kUsePCF = SM_USE_PCF;
    static const bool kUsePoissonSampling = SM_USE_POISSON_SAMPLING;

    //Static Arrays for Poission Disc Sampling
    static const float2 kPoissonDisc[16] ={float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
                                          float2(-0.094184101, -0.92938870),float2(0.34495938, 0.29387760),
                                          float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
                                          float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),
                                          float2(0.44323325, -0.97511554),  float2(0.53742981, -0.47373420),
                                          float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
                                          float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),
                                          float2(0.19984126, 0.78641367),   float2(0.14383161, -0.14100790)};

    static const float3 kPoissonCube[20] ={float3(1, 1, 1), float3(1, -1, 1), float3(-1, -1, 1), float3(-1, 1, 1),
                                                     float3(1, 1, -1), float3(1, -1, -1), float3(-1, -1, -1), float3(-1, 1, -1),
                                                     float3(1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(-1, 1, 0),
                                                     float3(1, 0, 1), float3(-1, 0, 1), float3(1, 0, -1), float3(-1, 0, -1),
                                                     float3(0, 1, 1), float3(0, -1, 1), float3(0, -1, -1), float3(0, 1, -1)};

    //Gets the shadow value
    // sample vec is either the directional vec (isCube = true) or the uvCoordinates
    float getShadowValue(uint index, float3 sampleVec, bool isCube)
    {
        if (isCube)
            return gShadowMapCube[index].SampleLevel(gShadowSampler, sampleVec, 0.f);
        else
            return gShadowMap[index].SampleLevel(gShadowSampler, sampleVec.xy, 0.f);
    }

    bool getDistanceAndUV(in uint index, in float3 posW, const in LightData lightData, out float3 shadowUV, out float lightDist, out bool isCube)
    {
        isCube = lightData.type == uint(LightType::Point) && lightData.openingAngle > M_PI_2;
        //Set Position and far plane depending on the type
        float3 lightPosW = lightData.type == uint(LightType::Directional) ? gSceneCenter - normalize(lightData.dirW) * gDirectionalOffset : lightData.posW;
        
   
        shadowUV = posW - lightPosW;
        lightDist = length(shadowUV);
        //If it is not a cube texture, calculate the UV coordinates of the light
        if (!isCube)
        {
            float4x4 viewProjectMat = gShadowMapVPBuffer[index];
            float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
            shadowUV.xy = (projPos.xy / projPos.w); //This is actually not necessary for Directional Lights
        }
        
        shadowUV.y *= -1; //Flip 
        
        if (!isCube)
        {
            shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]
            if (any(shadowUV.xy < 0.f) || any(shadowUV.xy > 1.f))   //Return if outside of the shadow map
                return false;
        }
        return true;
    }

    float standardShadowTest(in const uint index, in const float3 shadowUV, in const bool isCube, in const float farPlane, in const float lightDist)
    {
        float shadowDist = getShadowValue(index, shadowUV, isCube);
        shadowDist *= farPlane;
        if (shadowDist + gSMworldAcneBias > lightDist)
            return 1.f;
        return 0.f;
    }
    
    //Evaluates the shadow map for the given light. rng is only used in Poission sampling
    bool evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
    {
        //TODO include checks and return false if shadow mapping is not possible
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        shadow = 0.f;
        float lightDist;
        float3 shadowUV;
        bool isCube;
        if (!getDistanceAndUV(index, posW, lightData, shadowUV, lightDist, isCube))
            return true;

        float farPlane = lightData.type == uint(LightType::Directional) ? gDirectionalOffset * 2 : gShadowMapFarPlane;
 
        //Shadow Test with shadow map
        if (!kUsePCF && !kUsePoissonSampling) 
        {
            shadow += standardShadowTest(index, shadowUV, isCube, farPlane, lightDist);
        }
        else if (kUsePoissonSampling)
        {
            if (isCube)
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(19.9999f * sampleNext1D(sg));
                    float3 uv = shadowUV + kPoissonCube[poisIdx] * gPoissonDiscRad * 0.25f;
                    shadow += standardShadowTest(index, uv, isCube, farPlane, lightDist);    //TODO Get a better solution than sampleNext3D
                }
                shadow *= 0.25f;
            }
            else
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(15.9999f * sampleNext1D(sg));
                    float3 uv = shadowUV;
                    uv.xy += kPoissonDisc[poisIdx] * ((gPoissonDiscRad * 32.f) / gShadowMapRes);
                    shadow += standardShadowTest(index, uv, isCube, farPlane, lightDist);
                }
                shadow *= 0.25f;
            }
        }
        else    //PCF using gather (relatively cheap)
        {
            if (isCube)
            {
                float4 shadowVals = gShadowMapCube[index].Gather(gShadowSampler, shadowUV);
                shadowVals *= farPlane;
                for (uint k = 0; k < 4; k++)
                {
                    if (shadowVals[k] + gSMworldAcneBias > lightDist)
                        shadow += 1.f;
                }
                shadow *= 0.25f;
            }
            else
            {
                float2 gatherUV = (floor(shadowUV.xy * float(gShadowMapRes) - 0.5) + 1.0f) / float(gShadowMapRes);
                float4 shadowVals = gShadowMap[index].Gather(gShadowSampler, gatherUV);
                shadowVals *= farPlane;
                //Interpolation weights
                float2 f = frac(shadowUV.xy * float(gShadowMapRes) - 0.5f);
                // (-,+),(+,+)(+,-)(-,-)
                float4 w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
                for (uint k = 0; k < 4; k++)
                {
                    if (shadowVals[k] + gSMworldAcneBias > lightDist)
                        shadow += 1.f * w[k];
                }
            }
        }
    
        return true;
    }

    //Return true
    bool hybridShadowMap(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        shadow = 0.f;
        float lightDist;
        float3 shadowUV;
        bool isCube;
        if (!getDistanceAndUV(index, posW, lightData, shadowUV, lightDist, isCube))
            return true;

        float farPlane = lightData.type == uint(LightType::Directional) ? gDirectionalOffset * 2 : gShadowMapFarPlane;

        float maxDist = farPlane;
        float minDist = 0.f;

        //get min dist and max dist
        //if (!kUsePoissonSampling) //Use PCF
        if(true)
        {
            float4 shadowVals;
            if (isCube)
                shadowVals = gShadowMapCube[index].Gather(gShadowSampler, shadowUV);
            else
            {
                float2 gatherUV = (floor(shadowUV.xy * float(gShadowMapRes) - 0.5) + 1.0f) / float(gShadowMapRes);
                shadowVals = gShadowMap[index].Gather(gShadowSampler, gatherUV);
            }
                
            shadowVals *= farPlane;
            for (uint k = 0; k < 4; k++)
            {
                maxDist = min(maxDist, shadowVals[k] + gSMworldAcneBias);
                minDist = max(minDist, shadowVals[k] + gSMworldAcneBias);
            }
        }
        else
        {
            //TODO implement
        }

        //Check if ray tracing is needed or if we can skip
        if ((lightDist > maxDist)  && (lightDist < minDist))
            return false;

        if (lightDist <= maxDist) // Lit
            shadow = 1.f;

        return true;
    }

    //TODO support different near for each shadow map?
    //TODO Use Projected area/seen area (use the normal of the hit point)
    
    //Oracle Funktion for determening if a shadow map should be used
    //
    float oracleFunction(float pixelUVArea, float nearCamera, float distanceToCamera, float distanceToLight ,bool isCube)
    {
        float projAreaCam = pixelUVArea * distanceToCamera;
        float pixelAreaSM = isCube ? 1.f / gShadowMapResCube : 1.f / gShadowMapRes;
        pixelAreaSM *= pixelAreaSM;
        float projSMArea = pixelAreaSM * distanceToLight;

        return projSMArea / projAreaCam;
    }

};

ParameterBlock<ShadowMap> gShadowMap;
