#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;

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

    float gShadowMapFarPlane;
    float gSMworldAcneBias;
    bool gUsePCF;
    uint gShadowMapRes;
    
    float3 gSceneCenter;
    float gDirectionalOffset;


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
    
    float evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        float shadow = 0.f;
        float lightDist;
        float3 shadowUV;
        bool isCube;
        if (!getDistanceAndUV(index, posW, lightData, shadowUV, lightDist, isCube))
            return shadow;

        float farPlane = lightData.type == uint(LightType::Directional) ? gDirectionalOffset * 2 : gShadowMapFarPlane;
        
        //Shadow Test with shadow map
        if (!gUsePCF)
        {
            float shadowDist = getShadowValue(index, shadowUV, isCube);
            shadowDist *= farPlane;
            if (shadowDist + gSMworldAcneBias > lightDist)
                shadow += 1.f;
        }
        else //PCF using gather (practically free)
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
    
        return shadow;
    }

};
