#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import ShadowMapData;

//Set important defines if they where not handled on the CPU
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
    TextureCube<float> gShadowMapCube[NUM_SHADOW_MAPS_CUBE];                        //(Point)ShadowMap with type float. Can be Null
    Texture2D<float> gShadowMap[NUM_SHADOW_MAPS_MISC];                              //(Spot)ShadowMap with type float. Can be Null
    Texture2DArray<float> gCascadedShadowMap[NUM_SHADOW_MAPS_CASCADE];              //CascadedShadowMap with type float. Can be Null
    TextureCube<float2> gShadowMapVarianceCube[NUM_SHADOW_MAPS_CUBE];               //(Point)ShadowMap with type float2. Used in Variance SM. Can be Null
    Texture2D<float2> gShadowMapVariance[NUM_SHADOW_MAPS_MISC];                     //(Spot)ShadowMap with type float2. Used in Variance SM. Can be Null
    Texture2DArray<float2> gCascadedShadowMapVariance[NUM_SHADOW_MAPS_CASCADE];     //CascadedShadowMap with type float2. Used in Variance SM. Can be Null
    StructuredBuffer<uint> gShadowMapIndexMap;                                      //Maps global light index to local buffer index. Only used if kMultipleSMTypes == true
    StructuredBuffer<float4x4> gShadowMapVPBuffer;                                  //Buffer for the ViewProjection Matrices [Spot | Cascaded]           
    StructuredBuffer<float> gShadowMapNPSBuffer;                                    //Buffer for the Normalized Pixel Size [Point | Spot | Cascaded]

    float gShadowMapFarPlane;                           //Far plane of the shadow map //TODO move to defines
    uint gShadowMapRes;                                 //Resolution of the Shadow Map TODO Move to defines
    float gCameraNPS;                                   //Normalized Pixel Size for the Camera
    float gPoissonDiscRad; //Radius for Poisson Sampling (TODO add a seperate for cubemaps?; Move to defines)

    //CASCADED_SLICE_BUFFER_SIZE is either 4 or 8
    float gCascadedZSlices[CASCADED_SLICE_BUFFER_SIZE];  //Camera Z distance for cascaded level calculation
    
    static const bool kMultipleSMTypes = MULTIPLE_SHADOW_MAP_TYPES;         //Enabled if more than one type is present (e.g. point, spot, cascaded)
    static const uint kShadowMapMode = SHADOW_MAP_MODE;                     //Mode of the Shadow Map (e.g. normal, variance, exponential)
    static const uint kCascadedMatOffset = CASCADED_MATRIX_OFFSET;          //Cascaded offset for gShadowMapVPBuffer
    static const uint kCascadedLevels = CASCADED_LEVEL;                     //Number of Cascaded Levels
    static const bool kUsePCF = SM_USE_PCF;                                 //Enables PCF (only supported for "normal" SM)
    static const bool kUsePoissonSampling = SM_USE_POISSON_SAMPLING;        //Enables Statisfied Poisson Sampling (only supported for "normal" SM)
    static const float kCubeEpsilon = 1e-5f;                                //Epsilon used in CubePointLights as they do not support Bias and Slope Bias
    static const uint kNPSOffsetSpot = NPS_OFFSET_SPOT;                     //Spotlight offset for gShadowMapNPSBuffer
    static const uint kNPSOffsetCasc = NPS_OFFSET_CASCADED;                 //Cascaded offset for gShadowMapNPSBuffer
    static const uint kOracleDistFunctionMode = ORACLE_DIST_FUNCTION_MODE;  //Mode for the Oracle Distance Function
    static const float kExponentialSMConstant = SM_EXPONENTIAL_CONSTANT;    //Constant for Exponential Shadow Maps
    static const float kShadowMapNear = SM_NEAR;                            //Near plane for Spot and Directional lights
    

    //Static Arrays for Poission Disc Sampling
    static const float2 kPoissonDisc[16] =
    {
        float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
        float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
        float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
        float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
        float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
        float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
        float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
        float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)                                      
    };
    static const float3 kPoissonCube[20] =
    {
        float3(1, 1, 1), float3(1, -1, 1), float3(-1, -1, 1), float3(-1, 1, 1),
        float3(1, 1, -1), float3(1, -1, -1), float3(-1, -1, -1), float3(-1, 1, -1),
        float3(1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(-1, 1, 0),
        float3(1, 0, 1), float3(-1, 0, 1), float3(1, 0, -1), float3(-1, 0, -1),
        float3(0, 1, 1), float3(0, -1, 1), float3(0, -1, -1), float3(0, 1, -1)
    };

    
    /** Gets the value from the shadow map depending on type. R32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float getShadowValue(uint index, float3 sampleVec, const uint lightType, uint level = 0, float mipLevel = 0.f)
    {
        float sv = 0.f;
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapCube[index].SampleLevel(gShadowSampler, sampleVec, mipLevel);
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].SampleLevel(gShadowSampler, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap[index].SampleLevel(gShadowSampler, float3(sampleVec.xy, level), mipLevel);
                break;
            default:
                break;
        }

        return sv;
    }

    /** Gets the value from the shadow map depending on type. RG32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float2 getShadowVarianceValue(uint index, float3 sampleVec, const uint lightType, uint level = 0, float mipLevel = 0.f)
    {
        //TODO: Get Mip Map level
        float2 sv = 0.f;
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapVarianceCube[index].SampleLevel(gShadowSampler, sampleVec, mipLevel);
                break;
            case LightTypeSM::Spot:
                sv = gShadowMapVariance[index].SampleLevel(gShadowSampler, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMapVariance[index].SampleLevel(gShadowSampler, float3(sampleVec.xy, level), mipLevel);
                break;
            default:
                break;
        }
        return sv;
    }

    /** Converts Falcors light type to our Shadow Map type
    \param[in] light info.
    \return Shadow Map type.
    */
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
    
    /** Gets the current cascaded level from the world position
    \param[in] world position.
    \return cascaded level between 0 and kCascadedLevels.
    */
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
        return clamp(level, 0, kCascadedLevels);
    }
    
    /** Converts world Position to the shadow map sample vector and the distance to light
    \param[in] world position.
    \param[in] light information.
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return always true.
    */
    bool pointDistanceAndSampleVec(in float3 posW, const in LightData lightData, out float3 shadowUV, out float lightDist)
    {
        shadowUV = posW - lightData.posW;
        lightDist = (length(shadowUV) / gShadowMapFarPlane) + kCubeEpsilon;
        shadowUV.y *= -1; //Flip
        return true;
    }

    /** Converts world position to shadow map light projection space.
    \param[in] local shadow map texture index.
    \param[in] world position.
    \param[in] light information.
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return returns if the position is visible from the shadow map.
    */
    bool spotDistanceAndSampleVec(in uint index, in float3 posW, const in LightData lightData, out float2 shadowUV, out float lightDist)
    {
        float4x4 viewProjectMat = gShadowMapVPBuffer[index];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        projPos.xyz = (projPos.xyz / projPos.w); //This is actually not necessary for Directional Lights
        if (kShadowMapMode == (uint) ShadowMapType::ShadowMap)
            lightDist = projPos.z;
        else
            lightDist = (projPos.w - kShadowMapNear)/gShadowMapFarPlane;
        shadowUV = projPos.xy;
        shadowUV.y *= -1; //Flip 
        shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        if (any(shadowUV.xy < 0.f) || any(shadowUV.xy > 1.f))   //Return if outside of the shadow map
            return false;
        return true;
    }

    /** Converts world position to shadow map light projection space.
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] world position.
    \param[in] light information.
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return returns if the position is visible from the shadow map.
    */
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

    /** Standard shadow test for shadow map
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
    float standardShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist)
    {
        float shadowDist = getShadowValue(index, shadowUV, lightType, level);
        if (shadowDist > lightDist)
            return 1.f;
        return 0.f;
    }
    
    /** Evaluates the shadow map for the given light. rng is only used in Poission sampling
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapClassic(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
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
                valid = pointDistanceAndSampleVec(posW, lightData, shadowUV, lightDist);
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
         
        //Shadow Test with shadow map
        if (!kUsePCF && !kUsePoissonSampling)
        {
            shadow += standardShadowTest(index, cascLevel, shadowUV, lightType, lightDist);
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
        else //PCF using gather (relatively cheap)
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
                if (lightType == (uint) LightTypeSM::Spot)
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

    /** Hybrid Shadow Mapping for the classic shadow map. Evaluates the shadow and tells if a ray should be used. rng is only used in Poission sampling
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool hybridShadowMapClassic(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
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
                valid = pointDistanceAndSampleVec(posW, lightData, shadowUV, lightDist);
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

       
        float maxDist = FLT_MAX; //Maximum distance possible in scene
        float minDist = 0.f;

        //get min dist and max dist
        //if (!kUsePoissonSampling) //Use PCF
        if (true)
        {
            float4 shadowVals;
            float4 w = float4(0.25);
            if (lightType == (uint) LightTypeSM::Point)
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
                shadow += lightDist <= shadowDepth ? w[k] : 0.f; //Add to shadow in pcf style
            }
        }
        else
        {
            //TODO implement
        }

        //Check if ray tracing is needed or if we can skip
        if ((lightDist > maxDist) && (lightDist < minDist))
            return false;
        
        return true;
    }

    /** Variance shadow test
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
    float varianceShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist)
    {
        float2 varianceSV = getShadowVarianceValue(index, shadowUV, lightType, level);
        if (lightDist < varianceSV.x)
            return 1.f;
            
        float variance = varianceSV.y - (varianceSV.x * varianceSV.x);

        float diff = (lightDist - varianceSV.x);
        float pmax = variance / (variance + diff * diff);
        
        return pmax;
    }

    /** Variance Shadow Mapping. https://igm.univ-mlv.fr/~biri/Enseignement/MII2/Donnees/variance_shadow_maps.pdf
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapVariance(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
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
                valid = pointDistanceAndSampleVec(posW, lightData, shadowUV, lightDist);
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
         
        //Shadow Test with shadow map
        shadow = varianceShadowTest(index, cascLevel, shadowUV, lightType, lightDist);
        
        return true;
    }

    /** Exponential shadow test
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
    float exponentialShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist)
    {
        float shadowDepth = getShadowValue(index, shadowUV, lightType, level);
        float c = kExponentialSMConstant;
        return saturate(exp(-lightDist * c) * shadowDepth);
    }

    /** Exponential Shadow Mapping. https://jankautz.com/publications/esm_gi08.pdf
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapExponential(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
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
                valid = pointDistanceAndSampleVec(posW, lightData, shadowUV, lightDist);
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
         
        //Shadow Test with shadow map
        shadow = exponentialShadowTest(index, cascLevel, shadowUV, lightType, lightDist);
        
        return true;
    }

    /** Shadow Map overview function. Calls the corresponding implementation based on the current mode
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
    {
        switch (ShadowMapType(kShadowMapMode))
        {
            case ShadowMapType::ShadowMap:
                return evalShadowMapClassic(lightIdx, posW, lightData, sg, shadow);
            case ShadowMapType::Variance:
                return evalShadowMapVariance(lightIdx, posW, lightData, sg, shadow);
            case ShadowMapType::Exponential:
                return evalShadowMapExponential(lightIdx, posW, lightData, sg, shadow);
            default:
                shadow = 1.f;
                return false;
        }
    }

    /** Hybrid Shadow Map overview function. Calls the corresponding implementation based on the current mode
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalHybridShadowMap(uint lightIdx, float3 posW, const in LightData lightData, inout SampleGenerator sg, out float shadow)
    {
        switch (ShadowMapType(kShadowMapMode))
        {
            case ShadowMapType::ShadowMap:
                return hybridShadowMapClassic(lightIdx, posW, lightData, sg, shadow);
            default:
                shadow = 1.f;
                return false;
        }
     }
    
    /** Oracle Funktion for determening if a shadow map can be used without quality loss or if a ray should be used instead
    \param[in] analytical light index.
    \param[in] light information.
    \param[in] shading data of the current surface.
    \param[in] current distance from camera to hit. Can be modified with the oracleDistanceFactor for better results with indirect bounces
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    float oracleFunction(uint lightIdx, const in LightData lightData, const in ShadingData sd, float rayDistance)
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
            distanceToLight = 1.f; //Distance is always 1, because the area does not change in an orthographic projection
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

     /** Calculate the Oracle distance factor based on the currently selected mode. Should be used on bounces only
    \param[in] material/bsdf properties.
    \param[in] current reflection sample.
    \return a factor the indirect bounce distance should be multiplied with. Used to the oracle function as distance
    */
    float getOracleDistanceFunctionFactor(const in BSDFProperties bsdfProperties,const in BSDFSample bsdfSample)
    {
        switch (OracleDistFunction(kOracleDistFunctionMode))
        {
            case OracleDistFunction::RoughnessLinear:
            {
                    return lerp(1.f, M_2PI, bsdfProperties.roughness);
                }
            case OracleDistFunction::RoughnessSquare:
            {
                    float alpha = bsdfProperties.roughness * bsdfProperties.roughness;
                    return lerp(1.f, M_2PI, alpha);
                }
            case OracleDistFunction::Lobe:
            {
                    float oDistFac = bsdfSample.isLobe(LobeType::Diffuse) ? M_2PI : 1.f;
                    return oDistFac;
                }
            case OracleDistFunction::DiffSpecProp:
            {
                    float diffusePart = luminance(bsdfProperties.diffuseReflectionAlbedo);
                    float specularPart = luminance(bsdfProperties.specularReflectionAlbedo);
                    float diffuse = 0;
                    if (diffusePart > 0)
                    {
                        diffuse = diffusePart / (diffusePart + specularPart);
                    }
                    return lerp(1.f, M_2PI, diffuse);
                }
            default:;
        }
        //default case 
        return 1.f;
    }
    
    //EXPERIMENTAL Confidence Function used in https://www.cg.tuwien.ac.at/research/publications/2007/Scherzer-2007-PCS/Scherzer-2007-PCS-Preprint.pdf
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
        shadowPixelPos -= floor(shadowPixelPos); //Convert to pixel pos [0,1]

        //difference
        shadowPixelPos = abs(shadowPixelPos - 0.5f);
        
        return 1.f - max(shadowPixelPos.x, shadowPixelPos.y) * 2.f; //confidence
    }

};

ParameterBlock<ShadowMap> gShadowMap;
