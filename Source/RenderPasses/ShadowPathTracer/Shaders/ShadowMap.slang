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
    float gPCFdiskRadius;
    uint gShadowMapRes;
    
    float3 gSceneCenter;
    float gDirectionalOffset;

    bool gUsePCF;
    uint3 _pad;


    //Gets the shadow value
    // sample vec is either the directional vec (isCube = true) or the uvCoordinates
    float getShadowValue(uint index, float3 sampleVec, bool isCube)
    {
        if (isCube)
            return gShadowMapCube[index].SampleLevel(gShadowSampler, sampleVec, 0.f);
        else
            return gShadowMap[index].SampleLevel(gShadowSampler, sampleVec.xy, 0.f);
    }
    
    float evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }

        bool isCube = lightData.type == uint(LightType::Point) && lightData.openingAngle > M_PI_2;
        bool isDirectional = lightData.type == uint(LightType::Directional);
        //Set Position and far plane depending on the type
        float3 lightPosW = isDirectional ? gSceneCenter - normalize(lightData.dirW) * gDirectionalOffset : lightData.posW;
        float farPlane = isDirectional ? gDirectionalOffset * 2 : gShadowMapFarPlane;
    
    
        float shadow = 0.f;
        float3 lightToPoint = posW - lightPosW;
        float lightDist = length(lightToPoint);
        //If it is not a cube texture, calculate the UV coordinates of the light
        if (!isCube)
        {
            float4x4 viewProjectMat = gShadowMapVPBuffer[index];
            float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
            lightToPoint.xy = (projPos.xy / projPos.w);                 //This is actually not necessary for Directional Lights
        }
        
        lightToPoint.y *= -1;   //Flip 
        
        if (!isCube)
        {
            lightToPoint = lightToPoint * 0.5 + 0.5; //UV from [-1,1] to [0,1]
            if (any(lightToPoint.xy < 0.f) || any(lightToPoint.xy > 1.f))   //Return if outside of the shadow map
                return shadow;
        }
            
    
        //Shadow Test with shadow map
        if (!gUsePCF)
        {
            float shadowDist = getShadowValue(index, lightToPoint, isCube);
            shadowDist *= farPlane;
            if (shadowDist + gSMworldAcneBias > lightDist)
                shadow += 1.f;
        }
        else //Proper PCF
        {
            //Proper PCF        
            float samples = 2.0;
            float offset = isCube ? gPCFdiskRadius : 1.f;
        
            for (float x = -offset; x < offset; x += offset / (samples * 0.5))
            {
                for (float y = -offset; y < offset; y += offset / (samples * 0.5))
                {
                    if (isCube)
                    {
                        for (float z = -offset; z < offset; z += offset / (samples * 0.5))
                        {
                            float shadowDist = getShadowValue(index, lightToPoint + float3(x, y, z), isCube);
                            shadowDist *= farPlane; // undo mapping [0;1]
                            if (lightDist - gSMworldAcneBias < shadowDist)
                                shadow += 1.0;
                        }
                    }
                    else
                    {
                        float2 uvOffset = float2((x * samples) / gShadowMapRes, (y * samples) / gShadowMapRes);
                        float shadowDist = getShadowValue(index, lightToPoint + float3(uvOffset.x, uvOffset.y, 0), isCube);
                        shadowDist *= farPlane; // undo mapping [0;1]
                        if (lightDist - gSMworldAcneBias < shadowDist)
                            shadow += 1.0;
                    }
                }
            }

            float sampleNum = samples * samples;
            if (isCube)
                sampleNum *= samples;
            shadow /= sampleNum;
        }
    
        return shadow;
    }

};
