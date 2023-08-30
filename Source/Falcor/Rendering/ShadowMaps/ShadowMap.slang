#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Sampling.SampleGenerator;
import ShadowMapTypes; 

//Set Defines if they where not handled on the CPU
#ifndef NUM_SHADOW_MAPS_CUBE
    #define NUM_SHADOW_MAPS_CUBE 1
#endif
#ifndef NUM_SHADOW_MAPS_MISC
    #define NUM_SHADOW_MAPS_MISC 1
#endif
#ifndef NUM_SHADOW_MAPS_CASCADE
    #define NUM_SHADOW_MAPS_CASCADE 1
#endif
#ifndef MULTIPLE_SHADOW_MAP_TYPES
    #define MULTIPLE_SHADOW_MAP_TYPES 1
#endif

#ifndef CASCADED_SLICE_BUFFER_SIZE
    #define CASCADED_SLICE_BUFFER_SIZE 4
#endif

struct ShadowMap
{
    SamplerState gShadowSampler;
    TextureCube<float> gShadowMapCube[NUM_SHADOW_MAPS_CUBE];
    Texture2D<float> gShadowMap[NUM_SHADOW_MAPS_MISC];
    Texture2DArray<float> gCascadedShadowMap[NUM_SHADOW_MAPS_CASCADE];
    StructuredBuffer<uint> gShadowMapIndexMap;
    StructuredBuffer<float4x4> gShadowMapVPBuffer;
    StructuredBuffer<float> gShadowMapNPSBuffer;

    float gShadowMapFarPlane;    
    uint gShadowMapRes;
    float gCameraNPS;
    float gPoissonDiscRad;    

    //CASCADED_SLICE_BUFFER_SIZE is either 4 or 8
    float gCascadedZSlices[CASCADED_SLICE_BUFFER_SIZE]; //Camera Z distance for cascaded level calculation
    
    static const bool kMultipleSMTypes = MULTIPLE_SHADOW_MAP_TYPES;
    static const uint kCascadedMatOffset = CASCADED_MATRIX_OFFSET;
    static const uint kCascadedLevels = CASCADED_LEVEL;
    static const bool kUsePCF = SM_USE_PCF;
    static const bool kUsePoissonSampling = SM_USE_POISSON_SAMPLING;
    static const float kCubeEpsilon = 1e-5f;
    static const uint kNPSOffsetSpot = NPS_OFFSET_SPOT;
    static const uint kNPSOffsetCasc = NPS_OFFSET_CASCADED;

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
    float getShadowValue(uint index, float3 sampleVec, const uint lightType, uint level = 0)
    {
        float sv = 0.f;
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapCube[index].SampleLevel(gShadowSampler, sampleVec, 0.f);
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].SampleLevel(gShadowSampler, sampleVec.xy, 0.f);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap[index].SampleLevel(gShadowSampler, float3(sampleVec.xy, level), 0.f);
                break;
            default:
                break;
        }

        return sv;
    }

    uint getSMLightType(const in LightData lightData)
    {
        uint lightType = 0;
        //Determine the light type
        if (lightData.type == uint(LightType::Directional))
            lightType = (uint) LightTypeSM::Directional;
        else if (lightData.type == uint(LightType::Point) && lightData.openingAngle > M_PI_4)
            lightType = (uint) LightTypeSM::Point;
        else
            lightType = (uint) LightTypeSM::Spot;

        return lightType;
    }
    
    bool pointDistanceAndSampleVec(in uint index, in float3 posW, const in LightData lightData, out float3 shadowUV, out float lightDist)
    {
        shadowUV = posW - lightData.posW;
        lightDist = (length(shadowUV) / gShadowMapFarPlane) + kCubeEpsilon;
        shadowUV.y *= -1; //Flip
        return true;
    }
    
    bool spotDistanceAndSampleVec(in uint index, in float3 posW, const in LightData lightData, out float2 shadowUV, out float lightDist)
    {
        float4x4 viewProjectMat = gShadowMapVPBuffer[index];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        projPos.xyz = (projPos.xyz / projPos.w); //This is actually not necessary for Directional Lights
        lightDist = projPos.z;
        shadowUV = projPos.xy;
        shadowUV.y *= -1; //Flip 
        shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        if (any(shadowUV.xy < 0.f) || any(shadowUV.xy > 1.f))   //Return if outside of the shadow map
            return false;
        return true;
    }

    uint getCascadedLevel(float3 posW)
    {
        
        float linearZ = abs(mul(gScene.camera.data.viewMat, float4(posW, 1.f)).z);
        uint level = 0;
        for (uint i = 0; i < kCascadedLevels - 1; i++)
        {
            if (linearZ < gCascadedZSlices[i])
                return level;
            level++;
        }
        return level;
    }
    
    bool directionalDistanceAndSampleVec(in uint index, in uint level, in float3 posW, const in LightData lightData, out float2 shadowUV, out float lightDist)
    {
        uint bufferIdx = kCascadedMatOffset + index * kCascadedLevels;
        float4x4 viewProjectMat = gShadowMapVPBuffer[bufferIdx + level];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        projPos.xyz = (projPos.xyz / projPos.w); //This is actually not necessary for Directional Lights
        lightDist = projPos.z;
        shadowUV = projPos.xy;
        shadowUV.y *= -1; //Flip 
        shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        if (any(shadowUV.xy < 0.f) || any(shadowUV.xy > 1.f))   //Return if outside of the shadow map
            return false;
        return true;
    }
    
    float standardShadowTest(in const uint index, in const uint level,in const float3 shadowUV, in const uint lightType, in const float lightDist)
    {
        float shadowDist = getShadowValue(index, shadowUV, lightType, level);
        if (shadowDist > lightDist)
            return 1.f;
        return 0.f;
    }
    
    //Evaluates the shadow map for the given light. rng is only used in Poission sampling
    bool evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        shadow = 0.f;
        float lightDist = 0.f;
        float3 shadowUV = float3(0);
        uint lightType = getSMLightType(lightData);
        uint cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightType)
        {
            case LightTypeSM::Point:
                valid = pointDistanceAndSampleVec(index, posW, lightData, shadowUV, lightDist);
                break;
            case LightTypeSM::Spot:
                valid = spotDistanceAndSampleVec(index, posW, lightData, shadowUV.xy, lightDist);
                break;
            case LightTypeSM::Directional:
                valid = directionalDistanceAndSampleVec(index, cascLevel, posW, lightData, shadowUV.xy, lightDist);
                break;
            default:
                break;
        }
        
        if (!valid)
            return true;    //Ray is needed
         
        //Shadow Test with shadow map
        if (!kUsePCF && !kUsePoissonSampling) 
        {
            shadow += standardShadowTest(index, cascLevel , shadowUV, lightType, lightDist);
        }
        else if (kUsePoissonSampling)
        {
            if (lightType == (uint) LightTypeSM::Point)
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(19.9999f * sampleNext1D(sg));
                    float3 uv = shadowUV + kPoissonCube[poisIdx] * gPoissonDiscRad * 0.25f; //TODO Better solution (angle?) 
                    shadow += standardShadowTest(index, cascLevel, uv, lightType, lightDist); 
                }
                shadow *= 0.25f;
            }
            else
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(15.99999f * sampleNext1D(sg));
                    float3 uv = shadowUV;
                    uv.xy += kPoissonDisc[poisIdx] * ((gPoissonDiscRad) / gShadowMapRes);
                    shadow += standardShadowTest(index, cascLevel, uv, lightType, lightDist);
                }
                shadow *= 0.25f;
            }
        }
        else    //PCF using gather (relatively cheap)
        {
            if (lightType == (uint) LightTypeSM::Point)
            {
                float4 shadowVals = gShadowMapCube[index].Gather(gShadowSampler, shadowUV);
                for (uint k = 0; k < 4; k++)
                {
                    if (shadowVals[k] > lightDist)
                        shadow += 1.f;
                }
                shadow *= 0.25f;
            }
            else
            {
                float2 gatherUV = (floor(shadowUV.xy * float(gShadowMapRes) - 0.5) + 1.0f) / float(gShadowMapRes);
                float4 shadowVals;
                if(lightType == (uint) LightTypeSM::Spot)
                    shadowVals = gShadowMap[index].Gather(gShadowSampler, gatherUV);
                else
                    shadowVals = gCascadedShadowMap[index].Gather(gShadowSampler, float3(gatherUV, cascLevel));
                //Interpolation weights
                float2 f = frac(shadowUV.xy * float(gShadowMapRes) - 0.5f);
                // (-,+),(+,+)(+,-)(-,-)
                float4 w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
                for (uint k = 0; k < 4; k++)
                {
                    if (shadowVals[k] > lightDist)
                        shadow += w[k];
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
        float lightDist = 0.f;
        float3 shadowUV = float3(0);
        uint lightType = getSMLightType(lightData);
        uint cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightType)
        {
            case LightTypeSM::Point:
                valid = pointDistanceAndSampleVec(index, posW, lightData, shadowUV, lightDist);
                break;
            case LightTypeSM::Spot:
                valid = spotDistanceAndSampleVec(index, posW, lightData, shadowUV.xy, lightDist);
                break;
            case LightTypeSM::Directional:
                valid = directionalDistanceAndSampleVec(index, cascLevel, posW, lightData, shadowUV.xy, lightDist);
                break;
            default:
                break;
        }
        
        if (!valid)
            return true; //Ray is needed

       
        float maxDist = FLT_MAX;    //Maximum distance possible in scene
        float minDist = 0.f;

        //get min dist and max dist
        //if (!kUsePoissonSampling) //Use PCF
        if(true)
        {
            float4 shadowVals;
            float4 w = float4(0.25);
            if (lightType == (uint)LightTypeSM::Point)
            {
                shadowVals = gShadowMapCube[index].Gather(gShadowSampler, shadowUV);
            }
            else
            {
                float2 gatherUV = (floor(shadowUV.xy * float(gShadowMapRes) - 0.5) + 1.0f) / float(gShadowMapRes);
                
                if (lightType == (uint) LightTypeSM::Spot)
                    shadowVals = gShadowMap[index].Gather(gShadowSampler, gatherUV);
                else
                    shadowVals = gCascadedShadowMap[index].Gather(gShadowSampler, float3(gatherUV, cascLevel));
                
                float2 f = frac(shadowUV.xy * float(gShadowMapRes) - 0.5f);
                // (-,+),(+,+)(+,-)(-,-)
                w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
            }
                
            
            for (uint k = 0; k < 4; k++)
            {
                float shadowDepth = shadowVals[k];
                maxDist = min(maxDist, shadowDepth);
                minDist = max(minDist, shadowDepth);
                shadow += lightDist <= shadowDepth ? w[k] : 0.f;   //Add to shadow in pcf style
            }
        }
        else
        {
            //TODO implement
        }

        //Check if ray tracing is needed or if we can skip
        if ((lightDist > maxDist)  && (lightDist < minDist))
            return false;
        
        return true;
    }

    //TODO support different near for each shadow map?
    //TODO Use Projected area/seen area (use the normal of the hit point)

    //Oracle Funktion for determening if a shadow map should be used
    //
    float oracleFunction(uint lightIdx, const in LightData lightData, const in ShadingData sd , float rayDistance)
    {
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            lightIdx = gShadowMapIndexMap[lightIdx];
        }
        uint lightType = getSMLightType(lightData);

        if (lightType == (uint) LightTypeSM::Spot)
            lightIdx += kNPSOffsetSpot;
        
        float distanceToLight;
        float3 toLight;

        //Get LightDir and Distance
        if (lightType == (uint) LightTypeSM::Directional)
        {
            uint level = getCascadedLevel(sd.posW);
            distanceToLight = 1.f;  //Distance is always 1, because the area does not change in an orthographic projection
            toLight = -lightData.dirW;
            lightIdx += kNPSOffsetCasc + level; 
        }
        else
        {
            toLight = lightData.posW - sd.posW;
            distanceToLight = length(toLight);
            toLight /= distanceToLight;
        }
        
        const float3 N = sd.frame.N;
        
        //Get the cos theta between normal and vectors
        float angleCamera = max(1e-10f, dot(sd.V, N));
        float angleLight = max(1e-10f, dot(toLight, N));
        
        float projAreaCam = gCameraNPS * rayDistance * rayDistance;
        float pixelAreaSM = gShadowMapNPSBuffer[lightIdx];
        float projSMArea = pixelAreaSM * distanceToLight * distanceToLight;

        return (projSMArea * angleCamera) / (projAreaCam * angleLight);
    }

    //Confidence Function used in https://www.cg.tuwien.ac.at/research/publications/2007/Scherzer-2007-PCS/Scherzer-2007-PCS-Preprint.pdf
    float oracleFunction2(uint lightIdx, float3 posW, bool isCube)
    {
        if (isCube)
            return 1.f;
        
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        float4x4 viewProjectMat = gShadowMapVPBuffer[index];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        float2 shadowUV = (projPos.xy / projPos.w); //This is actually not necessary for Directional Lights        
        shadowUV.y *= -1; //Flip 
        shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]
        float2 shadowPixelPos = shadowUV * gShadowMapRes;
        shadowPixelPos -= floor(shadowPixelPos);    //Convert to pixel pos [0,1]

        //difference
        shadowPixelPos = abs(shadowPixelPos - 0.5f);
        
        return 1.f-max(shadowPixelPos.x, shadowPixelPos.y) * 2.f; //confidence
    }

};

ParameterBlock<ShadowMap> gShadowMap;
