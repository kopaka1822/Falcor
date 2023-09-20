#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
__exported import Rendering.ShadowMaps.ShadowMapData;
__exported import Rendering.ShadowMaps.ShadowMapHelpers;

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
    TextureCube<float4> gCubeShadowMapF4[NUM_SHADOW_MAPS_CUBE];                     //(Point)ShadowMap with type float4. Used in Variance SM. Can be Null
    Texture2D<float4> gShadowMapF4[NUM_SHADOW_MAPS_MISC];                           //(Spot)ShadowMap with type float4. Used in Variance SM. Can be Null
    Texture2DArray<float4> gCascadedShadowMapF4[NUM_SHADOW_MAPS_CASCADE];           //CascadedShadowMap with type float4. Used in Variance SM. Can be Null
    StructuredBuffer<uint> gShadowMapIndexMap;                                      //Maps global light index to local buffer index. Only used if kMultipleSMTypes == true
    StructuredBuffer<float4x4> gShadowMapVPBuffer;                                  //Buffer for the ViewProjection Matrices [Spot | Cascaded]           
    StructuredBuffer<float> gShadowMapNPSBuffer;                                    //Buffer for the Normalized Pixel Size [Point | Spot | Cascaded]

    float gShadowMapFarPlane;                           //Far plane of the shadow map //TODO move to defines
    float gCameraNPS;                                   //Normalized Pixel Size for the Camera
    float gPoissonDiscRad; //Radius for Poisson Sampling (TODO add a seperate for cubemaps?; Move to defines)
    float gPoissonDiscRadCube;    

    //CASCADED_SLICE_BUFFER_SIZE is either 4 or 8
    float gCascadedZSlices[CASCADED_SLICE_BUFFER_SIZE];  //Camera Z distance for cascaded level calculation

    //Static Constants
    static const bool kMultipleSMTypes = MULTIPLE_SHADOW_MAP_TYPES;         //Enabled if more than one type is present (e.g. point, spot, cascaded)
    static const uint kShadowMapMode = SHADOW_MAP_MODE;                     //Mode of the Shadow Map (e.g. normal, variance, exponential)
    static const uint kCascadedMatOffset = CASCADED_MATRIX_OFFSET;          //Cascaded offset for gShadowMapVPBuffer
    static const uint kCascadedLevels = CASCADED_LEVEL;                     //Number of Cascaded Levels
    static const bool kUsePCF = SM_USE_PCF;                                 //Enables PCF (only supported for "normal" SM)
    static const bool kUsePoissonSampling = SM_USE_POISSON_SAMPLING;        //Enables Statisfied Poisson Sampling (only supported for "normal" SM)
    static const float kCubeBias = CUBE_WORLD_BIAS;                         //World space bias for cube shadow maps
    static const uint kNPSOffsetSpot = NPS_OFFSET_SPOT;                     //Spotlight offset for gShadowMapNPSBuffer
    static const uint kNPSOffsetCasc = NPS_OFFSET_CASCADED;                 //Cascaded offset for gShadowMapNPSBuffer
    static const uint kOracleDistFunctionMode = ORACLE_DIST_FUNCTION_MODE;  //Mode for the Oracle Distance Function
    static const float kExponentialSMConstant = SM_EXPONENTIAL_CONSTANT;    //Constant for Exponential Shadow Maps
    static const float kEVSMNegConstant = SM_NEGATIVE_EXPONENTIAL_CONSTANT; //Negative Constant used in EVSM
    static const float kShadowMapNear = SM_NEAR;                            //Near plane for Spot and Directional lights                                                       
    static const float2 kHSMFilteredThreshold = HYBRID_SMFILTERED_THRESHOLD;//Hybrid Shadow Map: Threshold for filtered SM(Variance, Exponential). Ray is needed for [TH.x, TH.y]             
    static const bool kUseRayOutsideSM = USE_RAY_OUTSIDE_SM;                //Determines if a ray should be used outside of the shadow map
    static const uint kCascadedSMRes = CASCADED_SM_RESOLUTION;             //Resolution for Cascaded Shadow Maps
    static const uint k2DSMRes = SM_RESOLUTION;                            //Resolution for (Spot) Shadow Maps
    static const uint kCubeSMRes = CUBE_SM_RESOLUTION;                     //Resolution for Cube Shadow Maps
    static const bool kCascadedStochasticBlend = CASCADED_STOCHASTIC_BLEND;//Stochastic blend between cascaded levels
    static const float kCascadedStochBlendStr = CASCADED_STOCH_BLEND_STR;  //Strength of the stochastic blend
    
    static const bool kUseHybridShadowMap = USE_HYBRID_SM;
    static const bool kUseOracle = USE_ORACLE_FUNCTION;
    static const float kOracleCompValue = ORACLE_COMP_VALUE;
    static const bool kUseOracleDistFunction = USE_ORACLE_DISTANCE_FUNCTION;
    static const bool kUseSMMips = USE_SM_MIP;
    static const float kMipBias = SM_MIP_BIAS;      //angle bias used in mip calculation (cos theta^bias)

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
                sv = gShadowMapCube[index].SampleLevel(gShadowSampler, sampleVec, 0.f);//TODO fix mip level
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

    /** Gets the gathered shadow value from the shadow map depending on type. Always MIP = 0. R32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float4 getShadowValueGather(uint index, float3 sampleVec, const uint lightType, uint level = 0)
    {
        float4 sv = float4(0);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapCube[index].Gather(gShadowSampler, sampleVec);
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].Gather(gShadowSampler, sampleVec.xy);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap[index].Gather(gShadowSampler, float3(sampleVec.xy, level));
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
        float2 sv = float2(0.f);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapVarianceCube[index].SampleLevel(gShadowSampler, sampleVec, 0.f); //TODO fix mip level
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

    /** Gets the value from the shadow map depending on type. Gather Case. Always uses MIP = 0. RG32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float4 getShadowVarianceValueGather(uint index, float3 sampleVec, const uint lightType, uint level = 0, out float4 greenComponent)
    {
        float4 redComp = float4(0.f);
        switch (lightType)
        {
            case LightTypeSM::Point:
                redComp = gShadowMapVarianceCube[index].GatherRed(gShadowSampler, sampleVec);
                greenComponent = gShadowMapVarianceCube[index].GatherGreen(gShadowSampler, sampleVec);
                break;
            case LightTypeSM::Spot:
                redComp = gShadowMapVariance[index].GatherRed(gShadowSampler, sampleVec.xy);
                greenComponent = gShadowMapVariance[index].GatherGreen(gShadowSampler, sampleVec.xy);
                break;
            case LightTypeSM::Directional:
                redComp = gCascadedShadowMapVariance[index].GatherRed(gShadowSampler, float3(sampleVec.xy, level));
                greenComponent = gCascadedShadowMapVariance[index].GatherGreen(gShadowSampler, float3(sampleVec.xy, level));
                break;
            default:
                break;
        }
        return redComp;
    }

    /** Gets the value from the shadow map depending on type. RG32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float4 getShadowValueF4(uint index, float3 sampleVec, const uint lightType, uint level = 0, float mipLevel = 0.f)
    {
        float4 sv = float4(0.f);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gCubeShadowMapF4[index].SampleLevel(gShadowSampler, sampleVec, 0.f); //TODO fix mip level
                break;
            case LightTypeSM::Spot:
                sv = gShadowMapF4[index].SampleLevel(gShadowSampler, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMapF4[index].SampleLevel(gShadowSampler, float3(sampleVec.xy, level), mipLevel);
                break;
            default:
                break;
        }
        return sv;
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

     /** Gets the current cascaded level from the world position. Blends stochasticly between levels depending on kCascadedStochBlendStr
    \param[in] world position.
    \param[in] random variable between [0,1].
    \return cascaded level between 0 and kCascadedLevels.
    */
    uint getCascadedLevelStochastic(float3 posW, float random)
    {
        float randomOffsetMult = 1.f - kCascadedStochBlendStr * random; //Stoch blend between [0.5, 1]
        float linearZ = abs(mul(gScene.camera.data.viewMat, float4(posW, 1.f)).z);
        uint level = 0;
        for (uint i = 0; i < kCascadedLevels - 1; i++)
        {
            if (linearZ < (gCascadedZSlices[i] * randomOffsetMult))
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
    bool pointDistanceAndSampleVec(in float3 posW, const in SMLightData lightData, out float3 shadowUV, out float lightDist)
    {
        shadowUV = posW - lightData.posW;
        lightDist = (length(shadowUV) / gShadowMapFarPlane);
        lightDist += kCubeBias;
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
    bool spotDistanceAndSampleVec(in uint index, in float3 posW, const in SMLightData lightData, out float2 shadowUV, out float lightDist)
    {
        float4x4 viewProjectMat = gShadowMapVPBuffer[index];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        projPos.xyz = (projPos.xyz / projPos.w); //This is actually not necessary for Directional Lights
        if (kShadowMapMode == (uint) ShadowMapType::ShadowMap)
            lightDist = projPos.z;
        else
        lightDist = (projPos.w - kShadowMapNear) / (gShadowMapFarPlane - kShadowMapNear);
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
    bool directionalDistanceAndSampleVec(in uint index, in uint level, in float3 posW, const in SMLightData lightData, out float2 shadowUV, out float lightDist)
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
    bool evalShadowMapClassic(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, out float shadow)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        shadow = 1.f;
        float lightDist = 0.f;
        float3 shadowUV = float3(0);
        uint cascLevel;
        if(kCascadedStochasticBlend)
            cascLevel = getCascadedLevelStochastic(posW, sampleNext1D(sg));
        else
            cascLevel = getCascadedLevel(posW);
                
        bool valid = false;
        switch (lightData.type)
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
            return !kUseRayOutsideSM;   //Outside of shadow map

        //Shadow Test with shadow map
        shadow = 0.f;
        if (!kUsePCF && !kUsePoissonSampling)
        {
            shadow += standardShadowTest(index, cascLevel, shadowUV, lightData.type, lightDist);
        }
        else if (kUsePoissonSampling)
        {
            if (lightData.type == (uint) LightTypeSM::Point)
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(19.9999f * sampleNext1D(sg));
                    float3 uv = shadowUV + kPoissonCube[poisIdx] * gPoissonDiscRadCube; //TODO Better solution (angle?) 
                    shadow += standardShadowTest(index, cascLevel, uv, lightData.type, lightDist);
                }
                shadow *= 0.25f;
            }
            else
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(15.99999f * sampleNext1D(sg));
                    float3 uv = shadowUV;
                    const uint shadowMapRes = lightData.type == (uint) LightTypeSM::Directional ? kCascadedSMRes :  k2DSMRes;
                    uv.xy += kPoissonDisc[poisIdx] * ((gPoissonDiscRad) / shadowMapRes);
                    shadow += standardShadowTest(index, cascLevel, uv, lightData.type, lightDist);
                }
                shadow *= 0.25f;
            }
        }
        else //PCF using gather (relatively cheap)
        {
            float4 shadowVals;
            float4 w = float4(0.25);
            float3 gatherUV = shadowUV;
            const uint shadowMapRes = lightData.type == (uint) LightTypeSM::Directional ? kCascadedSMRes : k2DSMRes;
            if (lightData.type != (uint) LightTypeSM::Point)
            {
                gatherUV.xy = (floor(shadowUV.xy * float(shadowMapRes) - 0.5) + 1.0f) / float(shadowMapRes);
                float2 f = frac(shadowUV.xy * float(shadowMapRes) - 0.5f);
                // (-,+),(+,+)(+,-)(-,-)
                w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
            }
            shadowVals = getShadowValueGather(index, gatherUV, lightData.type, cascLevel);

            for (uint k = 0; k < 4; k++)
            {
                if (shadowVals[k] > lightDist)
                    shadow += w[k];
            }
        }
    
        return true; //No ray needed
    }

    /** Hybrid Shadow Mapping for the classic shadow map. Evaluates the shadow and tells if a ray should be used. rng is only used in Poission sampling
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool hybridShadowMapClassic(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, out float shadow)
    {
        uint index = lightIdx;
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            index = gShadowMapIndexMap[lightIdx];
        }
        
        shadow = kUseRayOutsideSM ? -1.f : 1.f;     //Make shadow value invalid if a ray should be shot outside of the shadow map
        float lightDist = 0.f;
        float3 shadowUV = float3(0);
        uint cascLevel;
        if(kCascadedStochasticBlend)
            cascLevel = getCascadedLevelStochastic(posW, sampleNext1D(sg));
        else
            cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightData.type)
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
            return !kUseRayOutsideSM;   //Outside of shadow map

       
        float maxDist = FLT_MAX; //Maximum distance possible in scene
        float minDist = 0.f;

        //get min dist and max dist
        shadow = 0.f;
        float4 shadowVals;
        float4 w = float4(0.25);
        if (!kUsePoissonSampling) //Use PCF
        {
            float3 gatherUV = shadowUV;
            const uint shadowMapRes = lightData.type == (uint) LightTypeSM::Directional ? kCascadedSMRes : k2DSMRes;
            if (lightData.type != (uint) LightTypeSM::Point)
            {
                gatherUV.xy = (floor(shadowUV.xy * float(shadowMapRes) - 0.5) + 1.0f) / float(shadowMapRes);
                float2 f = frac(shadowUV.xy * float(shadowMapRes) - 0.5f);
                // (-,+),(+,+)(+,-)(-,-)
                w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
            }
            shadowVals = getShadowValueGather(index, gatherUV, lightData.type, cascLevel);
        }
        else //Poisson Sampling
        {
            if (lightData.type == (uint) LightTypeSM::Point)
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(19.9999f * sampleNext1D(sg));
                    float3 uv = shadowUV + kPoissonCube[poisIdx] * gPoissonDiscRadCube; //TODO Better solution (angle?) 
                    shadowVals[k] = getShadowValue(index, uv, lightData.type, cascLevel);
                }
            }
            else
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(15.99999f * sampleNext1D(sg));
                    float3 uv = shadowUV;
                    const uint shadowMapRes = lightData.type == (uint) LightTypeSM::Directional ? kCascadedSMRes :  k2DSMRes;
                    uv.xy += kPoissonDisc[poisIdx] * ((gPoissonDiscRad) / shadowMapRes);
                    shadowVals[k] = getShadowValue(index, uv, lightData.type, cascLevel);
                }
            }
        }

        //Use all 4 gathered samples for the shadow test
        for (uint k = 0; k < 4; k++)
        {
           float shadowDepth = shadowVals[k];
           maxDist = min(maxDist, shadowDepth);
           minDist = max(minDist, shadowDepth);
           shadow += lightDist <= shadowDepth ? w[k] : 0.f; //Add to shadow in pcf style
        }

        //Check if ray tracing is needed or if we can skip
        if ((lightDist > maxDist) && (lightDist < minDist))
            return false; //inbetween; Ray is needed
        
        return true;    //No ray needed
    }

    /**ChebyshevUpperBound for Variance Shadow Mapping
    \param[in] M1 and M2 from Variance Shadow Mapping
    \param[in] depth of the current position
    \return shadow value between 1 and 0. 
    */
    float ChebyshevUpperBound(float2 moments, float mean, float minVariance)
    {
    // Compute variance
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);

    // Compute probabilistic upper bound
    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));
    
    // One-tailed Chebyshev
    return (mean <= moments.x ? 1.0f : pMax);
    }
    
    /** Variance shadow test
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
    float varianceShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist, const float mipLevel = 0.f)
    {
        float2 varianceSV = getShadowVarianceValue(index, shadowUV, lightType, level, mipLevel);

        
        return ChebyshevUpperBound(varianceSV, lightDist, 1e-10f);
    }

    float varianceShadowTestPCF(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist)
    {
        float3 gatherUV = shadowUV;
        float4 w = float4(0.25f);
        const uint shadowMapRes = lightType == (uint) LightTypeSM::Directional ? kCascadedSMRes : k2DSMRes;
        
        if (lightType != (uint) LightTypeSM::Point)
        {
            gatherUV.xy = (floor(shadowUV.xy * float(shadowMapRes) - 0.5) + 1.0f) / float(shadowMapRes);
            float2 f = frac(shadowUV.xy * float(shadowMapRes) - 0.5f);
            // (-,+),(+,+)(+,-)(-,-)
            w = float4((1.f - f.x) * f.y, f.x * f.y, f.x * (1.f - f.y), (1.f - f.x) * (1.f - f.y));
        }
        
        float4 variances = float4(0);
        float4 sv = getShadowVarianceValueGather(index, gatherUV, lightType, level, variances);

        bool allLit = true;
        for (uint i = 0; i < 4; i++)
        {
            if (lightDist >= sv[i])
                allLit = false;
        }
        if (allLit)
            return 1.f;

        float pMax = 0.f;
        for (uint i = 0; i < 4; i++)
        {
            float variance = variances[i] - (sv[i] * sv[i]);

            float diff = (lightDist - sv[i]);
            pMax += w[i] * (variance / (variance + diff * diff));
        }
       
        return saturate(pMax);
    }

    /** Variance Shadow Mapping. https://igm.univ-mlv.fr/~biri/Enseignement/MII2/Donnees/variance_shadow_maps.pdf
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapVariance(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, const float mipLevel,const bool hybridSM , out float shadow)
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
        uint cascLevel;
        if(kCascadedStochasticBlend)
            cascLevel = getCascadedLevelStochastic(posW, sampleNext1D(sg));
        else
            cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightData.type)
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
            return !kUseRayOutsideSM; //Ray is needed
         
        //Shadow Test with shadow map
        if (kUsePCF)
            shadow = varianceShadowTestPCF(index, cascLevel, shadowUV, lightData.type, lightDist);
        else
            shadow = varianceShadowTest(index, cascLevel, shadowUV, lightData.type, lightDist, mipLevel);

        if (hybridSM)
        {
            if (shadow > kHSMFilteredThreshold.x && shadow < kHSMFilteredThreshold.y)
                return false;   //Shoot ray
        }
            
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
    float exponentialShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist, const float mipLevel = 0.f)
    {
        float shadowDepth = getShadowValue(index, shadowUV, lightType, level, mipLevel);
        float depth = (lightDist * 2.f) - 1.f;
        return saturate(exp(-depth * kExponentialSMConstant) * shadowDepth);
    }

    /** Exponential Shadow Mapping. https://jankautz.com/publications/esm_gi08.pdf
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapExponential(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, const float mipLevel,const bool hybridSM , out float shadow)
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
        uint cascLevel;
        if(kCascadedStochasticBlend)
            cascLevel = getCascadedLevelStochastic(posW, sampleNext1D(sg));
        else
            cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightData.type)
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
            return !kUseRayOutsideSM;   //Outside of shadow map
         
        //Shadow Test with shadow map
        shadow = exponentialShadowTest(index, cascLevel, shadowUV, lightData.type, lightDist, mipLevel);

        if (hybridSM)
        {
            if (shadow > kHSMFilteredThreshold.x && shadow < kHSMFilteredThreshold.y)
                return false;   //Shoot ray
        }
        
        return true;
    }

    /** Exponential Variance shadow test
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
    float exponentialVarianceShadowTest(in const uint index, in const uint level, in const float3 shadowUV, in const uint lightType, in const float lightDist, const float mipLevel = 0.f)
    {
        float4 sv = getShadowValueF4(index, shadowUV, lightType, level, mipLevel);
        float depth = (lightDist * 2.f) - 1.f;
        float2 warpedDepth = float2(exp(depth * kExponentialSMConstant),  -exp(-depth * kEVSMNegConstant));
        float2 minVariance = float2(0.0001f * kExponentialSMConstant * warpedDepth.x, 0.0001f * kEVSMNegConstant * warpedDepth.y); 
        minVariance *= minVariance;
        
        float pPosMax = ChebyshevUpperBound(sv.xy, warpedDepth.x, minVariance.x);
        float pNegMax = ChebyshevUpperBound(sv.zw, warpedDepth.y, minVariance.y);
        
        return min(pNegMax,pPosMax);
    }
    
    /** Exponential (Layered) Variance Shadow Mapping. https://dl.acm.org/doi/pdf/10.5555/1375714.1375739
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMapExponentialVariance(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, const float mipLevel,const bool hybridSM , out float shadow)
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
        uint cascLevel;
        if(kCascadedStochasticBlend)
            cascLevel = getCascadedLevelStochastic(posW, sampleNext1D(sg));
        else
            cascLevel = getCascadedLevel(posW);
        
        bool valid = false;
        switch (lightData.type)
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
            return !kUseRayOutsideSM;   //Outside of shadow map
         
        //Shadow Test with shadow map
        shadow = exponentialVarianceShadowTest(index, cascLevel, shadowUV, lightData.type, lightDist, mipLevel);

        if (hybridSM)
        {
            if (shadow > kHSMFilteredThreshold.x && shadow < kHSMFilteredThreshold.y)
                return false;   //Shoot ray
        }
        
            return true;
        }
    
    /** Shadow Map overview function. Calls the corresponding implementation based on the current mode
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[in] mip level.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool evalShadowMap(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, const float mipLevel , out float shadow)
    {
        switch (ShadowMapType(kShadowMapMode))
        {
            case ShadowMapType::ShadowMap:
                return evalShadowMapClassic(lightIdx, posW, lightData, sg, shadow);
            case ShadowMapType::Variance:
                return evalShadowMapVariance(lightIdx, posW, lightData, sg, mipLevel, false ,shadow);
            case ShadowMapType::Exponential:
                return evalShadowMapExponential(lightIdx, posW, lightData, sg, mipLevel, false, shadow);
            case ShadowMapType::ExponentialVariance:
                return evalShadowMapExponentialVariance(lightIdx, posW, lightData, sg, mipLevel, false, shadow);
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
    bool evalHybridShadowMap(uint lightIdx, float3 posW, const in SMLightData lightData, inout SampleGenerator sg, const float mipLevel, out float shadow)
    {
        switch (ShadowMapType(kShadowMapMode))
        {
            case ShadowMapType::ShadowMap:
                return hybridShadowMapClassic(lightIdx, posW, lightData, sg, shadow);
            case ShadowMapType::Variance:
                return evalShadowMapVariance(lightIdx, posW, lightData, sg, mipLevel ,true ,shadow);
            case ShadowMapType::Exponential:
                return evalShadowMapExponential(lightIdx, posW, lightData, sg, mipLevel, true , shadow);
            case ShadowMapType::ExponentialVariance:
                return evalShadowMapExponentialVariance(lightIdx, posW, lightData, sg, mipLevel, true, shadow);
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
    \param[out] Optional Mip Level. Written if kUseSMMips == true
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    float oracleFunction(uint lightIdx, const in SMLightData lightData, const in ShadingData sd, float rayDistance, out float mipLevel)
    {
        mipLevel = 0.f; //Default value
        //Get the right index if multiple shadow map types are used
        if (kMultipleSMTypes)
        {
            lightIdx = gShadowMapIndexMap[lightIdx];
        }

        if (lightData.type == (uint) LightTypeSM::Spot)
            lightIdx += kNPSOffsetSpot;
        
        float distanceToLight;
        float3 toLight;

        //Get LightDir and Distance
        if (lightData.type == (uint) LightTypeSM::Directional)
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
        
        const float3 N = sd.faceN;
        
        //Get the cos theta between normal and vectors
        float angleCamera = abs(dot(sd.V, N));
        float angleLight = abs(dot(toLight, N));
        
        float projAreaCam = gCameraNPS * rayDistance * rayDistance;
        float pixelAreaSM = gShadowMapNPSBuffer[lightIdx];
        float projSMArea = pixelAreaSM * distanceToLight * distanceToLight;

        //Calculate MIP
        if(kUseSMMips){
            mipLevel = 0.5f * log2(( projAreaCam * pow(angleLight,kMipBias) ) / ( projSMArea * pow(angleCamera,kMipBias) + 1e-10f) ); 
        }
        
        return ( projSMArea * angleCamera) / ( projAreaCam * angleLight + 1e-10f);  //Divide the projected areas
    }

    /** Oracle Funktion for determening if a shadow map can be used without quality loss or if a ray should be used instead
    \param[in] analytical light index.
    \param[in] light information.
    \param[in] shading data of the current surface.
    \param[in] current distance from camera to hit. Can be modified with the oracleDistanceFactor for better results with indirect bounces
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    float oracleFunction(uint lightIdx, const in SMLightData lightData, const in ShadingData sd, float rayDistance)
    {
        float mipLevel;
        return oracleFunction(lightIdx, lightData, sd, rayDistance, mipLevel);
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
                    return lerp(1.f, M_PI, bsdfProperties.roughness);
                }
            case OracleDistFunction::RoughnessSquare:
            {
                    float alpha = bsdfProperties.roughness * bsdfProperties.roughness;
                    return lerp(1.f, M_PI, alpha);
                }
            case OracleDistFunction::Lobe:
            {
                    float oDistFac = bsdfSample.isLobe(LobeType::Diffuse) ? M_PI : 1.f;
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
                    return lerp(1.f, M_PI, diffuse);
                }
            default:;
        }
        //default case 
        return 1.f;
    }    
};

ParameterBlock<ShadowMap> gShadowMap;
