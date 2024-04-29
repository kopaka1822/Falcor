#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Color.ColorHelpers;
__exported import Rendering.ShadowMaps.ShadowMapData;
__exported import Rendering.ShadowMaps.ShadowMapHelpers;
__exported import Rendering.ShadowMaps.ShadowTestFunctions;

//Set important defines if they where not handled on the CPU
#ifndef NUM_SHADOW_MAPS_CUBE
#define NUM_SHADOW_MAPS_CUBE 1
#endif
#ifndef NUM_SHADOW_MAPS_MISC
#define NUM_SHADOW_MAPS_MISC 1
#endif
#ifndef MULTIPLE_SHADOW_MAP_TYPES
#define MULTIPLE_SHADOW_MAP_TYPES 1
#endif

#ifndef CASCADED_SLICE_BUFFER_SIZE
#define CASCADED_SLICE_BUFFER_SIZE 4
#endif

#ifndef CASC_USE_STOCH_LEVEL
#define CASC_USE_STOCH_LEVEL 0
#endif

struct ShadowMap
{
    SamplerState gShadowSamplerPoint;
    SamplerState gShadowSamplerLinear;
    TextureCube<float> gShadowMapCube[NUM_SHADOW_MAPS_CUBE];                        //(Point)ShadowMap with type float. Can be Null
    Texture2D<float> gShadowMap[NUM_SHADOW_MAPS_MISC];                              //(Spot)ShadowMap with type float. Can be Null
    Texture2DArray<float> gCascadedShadowMap;                                       //CascadedShadowMap with type float. Can be Null
    TextureCube<float2> gShadowMapVarianceCube[NUM_SHADOW_MAPS_CUBE];               //(Point)ShadowMap with type float2. Used in Variance SM. Can be Null
    Texture2D<float2> gShadowMapVariance[NUM_SHADOW_MAPS_MISC];                     //(Spot)ShadowMap with type float2. Used in Variance SM. Can be Null
    Texture2DArray<float2> gCascadedShadowMapVariance;                              //CascadedShadowMap with type float2. Used in Variance SM. Can be Null
    TextureCube<float4> gCubeShadowMapF4[NUM_SHADOW_MAPS_CUBE];                     //(Point)ShadowMap with type float4. Used in filtered SM. Can be Null
    Texture2D<float4> gShadowMapF4[NUM_SHADOW_MAPS_MISC];                           //(Spot)ShadowMap with type float4. Used in filtered SM. Can be Null
    Texture2DArray<float4> gCascadedShadowMapF4;                                    //CascadedShadowMap with type float4. Used in filtered SM. Can be Null
    StructuredBuffer<uint> gShadowMapIndexMap;                                      //Maps global light index to local buffer index. Only used if kMultipleSMTypes == true
    StructuredBuffer<float4x4> gShadowMapVPBuffer;                                  //Buffer for the ViewProjection Matrices [Spot]           
    StructuredBuffer<float4x4> gSMCascadedVPBuffer;                                 //Buffer for the ViewProjection Matrices [Cascaded] 

    float gShadowMapFarPlane;                           //Far plane of the shadow map //TODO move to defines
    float gPoissonDiscRad; //Radius for Poisson Sampling (TODO add a seperate for cubemaps?; Move to defines)
    float gPoissonDiscRadCube;    

    //CASCADED_SLICE_BUFFER_SIZE is either 4 or 8
    float gCascadedZSlices[CASCADED_SLICE_BUFFER_SIZE];  //Camera Z distance for cascaded level calculation

    //Static Constants
    static const bool kMultipleSMTypes = MULTIPLE_SHADOW_MAP_TYPES;         //Enabled if more than one type is present (e.g. point, spot, cascaded)
    static const uint kShadowMapMode = SHADOW_MAP_MODE;                     //Mode of the Shadow Map (e.g. normal, variance, exponential)
    static const uint kCascadedLevels = CASCADED_LEVEL;                     //Number of Cascaded Levels
    static const bool kUsePCF = SM_USE_PCF;                                 //Enables PCF (only supported for "normal" SM)
    static const bool kUsePoissonSampling = SM_USE_POISSON_SAMPLING;        //Enables Statisfied Poisson Sampling (only supported for "normal" SM)
    static const float kCubeBias = CUBE_WORLD_BIAS;                         //World space bias for cube shadow maps
    //static const float kExponentialSMConstant = SM_EXPONENTIAL_CONSTANT;    //Constant for Exponential Shadow Maps
    //static const float kEVSMNegConstant = SM_NEGATIVE_EXPONENTIAL_CONSTANT; //Negative Constant used in EVSM
    static const float kShadowMapNear = SM_NEAR;                            //Near plane for Spot and Directional lights                                                       
    static const float2 kHSMFilteredThreshold = HYBRID_SMFILTERED_THRESHOLD;//Hybrid Shadow Map: Threshold for filtered SM(Variance, Exponential). Ray is needed for [TH.x, TH.y]             
    static const bool kUseRayOutsideSM = USE_RAY_OUTSIDE_SM;                //Determines if a ray should be used outside of the shadow map
    static const uint kCascadedSMRes = CASCADED_SM_RESOLUTION;             //Resolution for Cascaded Shadow Maps
    static const uint k2DSMRes = SM_RESOLUTION;                            //Resolution for (Spot) Shadow Maps
    static const uint kCubeSMRes = CUBE_SM_RESOLUTION;                     //Resolution for Cube Shadow Maps
    //static const float kMSMDepthBias = MSM_DEPTH_BIAS * 0.001f;            //Depth bias for the moment shadow map
    //static const float kMSMMomentBias = MSM_MOMENT_BIAS * 0.001f;          //Moment bias for the moment shadow map
    static const float kAdditionalVarianceTestThreshold = MSM_VARIANCE_TEST_THRESHOLD; //Threshold for additional variance test in MSM
    static const float kCascadedStochasticRange = CASC_STOCH_RANGE;         //Stochastic range for cascaded shadow maps
    static const uint  kCascadedLevelTrace = CASCADE_LEVEL_TRACE;           //Traces only for the first X levels on hybrid
    static const bool kCascadedRayTracingAfterHybrid = CASCADE_RAYTRACING_AFTER_HYBRID; // Uses ray tracing for the higher cascaded levels (when higher than option directly above)
    static const bool kUseEVSMExtraTest = EVSM_EXTRA_TEST;

    static const bool kUseSMMips = USE_SM_MIP;
    static const float kMipBias = SM_MIP_BIAS;      //angle bias used in mip calculation (cos theta^bias)

    static const bool kUseDynamicSM = USE_DYNAMIC_SM;

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

    /* Converts Falcors Light Data struct to SMLightData (is much more compact)
    \param[in] light data struct from falcor
    \return Shadow Map light data struct
    */
    SMLightData getShadowMapLightData(const in LightData lightData, const uint index)
    {
        SMLightData smLightData = {};
        smLightData.posW = lightData.posW;
        smLightData.dirW = lightData.dirW;
        smLightData.type = getShadowMapLightType(lightData);

        //Get the shadow map index. For multiple types the shadow map needs to be mapped
        if (kMultipleSMTypes)
            smLightData.index = gShadowMapIndexMap[index];
        else
            smLightData.index = index;
            
        return smLightData;
    }
    
    /** Gets the value from the shadow map depending on type. R32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float getShadowValue(uint index, float3 sampleVec, const uint lightType,  uint level = 0, float mipLevel = 0.f)
    {
        float sv = 0.f;
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapCube[index].SampleLevel(gShadowSamplerPoint, sampleVec, 0.f);//TODO fix mip level
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].SampleLevel(gShadowSamplerPoint, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap.SampleLevel(gShadowSamplerPoint, float3(sampleVec.xy, level), mipLevel);
                if(kUseDynamicSM)
                {
                    float svDyn = gCascadedShadowMap.SampleLevel(gShadowSamplerPoint, float3(sampleVec.xy, kCascadedLevels + level), mipLevel);
                    sv = min(sv, svDyn);
                }
                break;
            default:
                break;
        }

        return sv;
    }

    /** Gets the value from the shadow map depending on type. R32/16 case
    \param[in] local shadow map texture index.
    \param[in] texture sample index. z-component is only used in cube-case.
    \param[in] light type from "LightTypeSM" casted to uint.
    \param[in] cascaded level. Only used if type == LightTypeSM::Directional.
    \param[in] used mipmap level.
    \return value from shadow map.
    */
    float getShadowValueFiltered(uint index, float3 sampleVec, const uint lightType, out bool isDynamic, uint level = 0, float mipLevel = 0.f)
    {
        isDynamic = false;
        float sv = 0.f;
        switch (lightType)
        {
        case LightTypeSM::Point:
                sv = gShadowMapCube[index].SampleLevel(gShadowSamplerLinear, sampleVec, 0.f);//TODO fix mip level
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].SampleLevel(gShadowSamplerLinear, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap.SampleLevel(gShadowSamplerLinear, float3(sampleVec.xy, level), mipLevel);
                if(kUseDynamicSM)
                {
                    float2 gatherUV = float2(0);
                    float4 w = getGatherWeights(sampleVec.xy, kCascadedSMRes, gatherUV);
                    float4 svDyn = gCascadedShadowMap.Gather(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    for (uint i = 0; i < 4; i++){
                        isDynamic |= any(sv > svDyn[i]);
                        svDyn[i] = min(svDyn[i], sv);
                    }
                    sv = 0.f;
                    for (uint i = 0; i < 4; i++)
                        sv += w[i] * svDyn[i];
                }
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
    float4 getShadowValueGather(uint index, float3 sampleVec, const uint lightType, out bool isDynamic , uint level = 0)
    {
        isDynamic = false;
        float4 sv = float4(0);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapCube[index].Gather(gShadowSamplerPoint, sampleVec);
                break;
            case LightTypeSM::Spot:
                sv = gShadowMap[index].Gather(gShadowSamplerPoint, sampleVec.xy);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMap.Gather(gShadowSamplerPoint, float3(sampleVec.xy, level));
                if(kUseDynamicSM)
                {
                    float4 svDyn = gCascadedShadowMap.Gather(gShadowSamplerPoint, float3(sampleVec.xy, kCascadedLevels + level));
                    isDynamic = any(sv > svDyn);
                    sv = min(sv, svDyn);
                }
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
    float2 getShadowVarianceValue(uint index, float3 sampleVec, const uint lightType,out bool isDynamic, uint level = 0, float mipLevel = 0.f)
    {
        isDynamic = false;
        float2 sv = float2(0.f);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gShadowMapVarianceCube[index].SampleLevel(gShadowSamplerLinear, sampleVec, 0.f); //TODO fix mip level
                break;
            case LightTypeSM::Spot:
                sv = gShadowMapVariance[index].SampleLevel(gShadowSamplerLinear, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMapVariance.SampleLevel(gShadowSamplerLinear, float3(sampleVec.xy, level), mipLevel);
                //Load dynamic values, take the min against shadow values and interpolate to recognize the edges
                if(kUseDynamicSM)
                {
                    float2 gatherUV;
                    float4 w = getGatherWeights(sampleVec.xy, kCascadedSMRes, gatherUV);
                    float4 redComp = gCascadedShadowMapVariance.GatherRed(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 greenComp = gCascadedShadowMapVariance.GatherGreen(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float2 svDyn[4];
                    for(uint i=0; i<4;i++){
                        float2 dynamicShadowValue = float2(redComp[i],greenComp[i]);
                        isDynamic |= any(sv > dynamicShadowValue);
                        svDyn[i] = min(dynamicShadowValue, sv);
                    }
                        
                    sv = float2(0);
                    for(uint i=0; i<4;i++)
                        sv += w[i] * svDyn[i];
                }
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
                redComp = gShadowMapVarianceCube[index].GatherRed(gShadowSamplerLinear, sampleVec);
                greenComponent = gShadowMapVarianceCube[index].GatherGreen(gShadowSamplerLinear, sampleVec);
                break;
            case LightTypeSM::Spot:
                redComp = gShadowMapVariance[index].GatherRed(gShadowSamplerLinear, sampleVec.xy);
                greenComponent = gShadowMapVariance[index].GatherGreen(gShadowSamplerLinear, sampleVec.xy);
                break;
            case LightTypeSM::Directional:
                redComp = gCascadedShadowMapVariance.GatherRed(gShadowSamplerLinear, float3(sampleVec.xy, level));
                greenComponent = gCascadedShadowMapVariance.GatherGreen(gShadowSamplerLinear, float3(sampleVec.xy, level));
                //Load dynamic values and take the min of both. Interpolation is handled later
                if(kUseDynamicSM){
                    float4 dynRed = gCascadedShadowMapVariance.GatherRed(gShadowSamplerPoint, float3(sampleVec.xy, kCascadedLevels + level));
                    float4 dynGreen = gCascadedShadowMapVariance.GatherGreen(gShadowSamplerPoint, float3(sampleVec.xy, kCascadedLevels + level));
                    redComp = min(redComp, dynRed);
                    greenComponent = min(greenComponent, dynGreen);
                }
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
    float4 getShadowValueF4(uint index, float3 sampleVec, const uint lightType, out bool isDynamic, uint level = 0, float mipLevel = 0.f)
    {
        isDynamic = false;
        float4 sv = float4(0.f);
        switch (lightType)
        {
            case LightTypeSM::Point:
                sv = gCubeShadowMapF4[index].SampleLevel(gShadowSamplerLinear, sampleVec, 0.f); //TODO fix mip level
                break;
            case LightTypeSM::Spot:
                sv = gShadowMapF4[index].SampleLevel(gShadowSamplerLinear, sampleVec.xy, mipLevel);
                break;
            case LightTypeSM::Directional:
                sv = gCascadedShadowMapF4.SampleLevel(gShadowSamplerLinear, float3(sampleVec.xy, level), mipLevel);
                //Load dynamic values, take the min against shadow values and interpolate to recognize the edges
                if(kUseDynamicSM)
                {
                    float2 gatherUV;
                    float4 w = getGatherWeights(sampleVec.xy, kCascadedSMRes, gatherUV);
                    float4 redComp = gCascadedShadowMapF4.GatherRed(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 greenComp = gCascadedShadowMapF4.GatherGreen(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 blueComp = gCascadedShadowMapF4.GatherBlue(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 alphaComp = gCascadedShadowMapF4.GatherAlpha(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 svDyn[4];
                    for(uint i=0; i<4;i++){
                        float4 dynamicSV = float4(redComp[i],greenComp[i],blueComp[i],alphaComp[i]);
                        isDynamic |= any(sv > dynamicSV);
                        svDyn[i] = min(dynamicSV, sv);
                    }
                        
                    sv = float4(0);
                    for(uint i=0; i<4;i++)
                        sv += w[i] * svDyn[i];
                }
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
    float4 getShadowValueF4Gather(uint index, float3 sampleVec, const uint lightType, out float4 sv[4], uint level = 0)
    {
        float4 red,green,blue,alpha, w;
        float2 gatherUV;
        float4 svAvrg = float4(0);
        switch (lightType)
        {
            case LightTypeSM::Point:
                red = gCubeShadowMapF4[index].GatherRed(gShadowSamplerLinear, sampleVec);
                green = gCubeShadowMapF4[index].GatherGreen(gShadowSamplerLinear, sampleVec);
                blue = gCubeShadowMapF4[index].GatherBlue(gShadowSamplerLinear, sampleVec);
                alpha = gCubeShadowMapF4[index].GatherAlpha(gShadowSamplerLinear, sampleVec);
                for(uint i=0; i<4;i++)
                    sv[i] = float4(red[i], green[i], blue[i],alpha[i]);
                svAvrg = (sv[0]+sv[1]+sv[2]+sv[3])* 0.25;
                break;
            case LightTypeSM::Spot:
                w = getGatherWeights(sampleVec.xy, k2DSMRes, gatherUV);
                red = gShadowMapF4[index].GatherRed(gShadowSamplerLinear, gatherUV);
                green = gShadowMapF4[index].GatherGreen(gShadowSamplerLinear, gatherUV);
                blue = gShadowMapF4[index].GatherBlue(gShadowSamplerLinear, gatherUV);
                alpha = gShadowMapF4[index].GatherAlpha(gShadowSamplerLinear, gatherUV);
                for(uint i=0; i<4;i++){
                    sv[i] = float4(red[i], green[i], blue[i],alpha[i]);
                    svAvrg += sv[i] * w[i];
                }
                    
                break;
            case LightTypeSM::Directional:
                w = getGatherWeights(sampleVec.xy, kCascadedSMRes, gatherUV);
                red = gCascadedShadowMapF4.GatherRed(gShadowSamplerPoint, float3(gatherUV, level));
                green = gCascadedShadowMapF4.GatherGreen(gShadowSamplerPoint, float3(gatherUV, level));
                blue = gCascadedShadowMapF4.GatherBlue(gShadowSamplerPoint, float3(gatherUV, level));
                alpha = gCascadedShadowMapF4.GatherAlpha(gShadowSamplerPoint, float3(gatherUV, level));
                for(uint i=0; i<4;i++){
                    sv[i] = float4(red[i], green[i], blue[i],alpha[i]);
                }
                //Load dynamic values, take the min against shadow values and interpolate to recognize the edges
                if(kUseDynamicSM)
                {
                    float4 redComp = gCascadedShadowMapF4.GatherRed(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 greenComp = gCascadedShadowMapF4.GatherGreen(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 blueComp = gCascadedShadowMapF4.GatherBlue(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    float4 alphaComp = gCascadedShadowMapF4.GatherAlpha(gShadowSamplerPoint, float3(gatherUV, kCascadedLevels + level));
                    for(uint i=0; i<4;i++)
                        sv[i] = min(float4(redComp[i],greenComp[i],blueComp[i],alphaComp[i]), sv[i]);
                }
                for(uint i=0; i<4;i++)
                        svAvrg += w[i] * sv[i]; 
                break;
            default:
                break;
        }
        return svAvrg;
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
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return returns if the position is visible from the shadow map.
    */
    bool spotDistanceAndSampleVec(in uint index, in float3 posW, out float2 shadowUV, out float lightDist)
    {
        float4x4 viewProjectMat = gShadowMapVPBuffer[index];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
        projPos.xyz = (projPos.xyz / projPos.w);

        switch (ShadowMapType(kShadowMapMode)){
        case ShadowMapType::ShadowMap:
        case ShadowMapType::SDVariance:
        case ShadowMapType::SDExponentialVariance:
        case ShadowMapType::SDMSM:
        {
            lightDist = projPos.z;
            break;
        }
        case ShadowMapType::Variance:
        case ShadowMapType::Exponential:
        case ShadowMapType::ExponentialVariance:
        case ShadowMapType::MSMHamburger:
        case ShadowMapType::MSMHausdorff:
        {
            lightDist = (projPos.w - kShadowMapNear) / (gShadowMapFarPlane - kShadowMapNear);
            break;
        }
        default:
            lightDist = projPos.z;
        }

        shadowUV = projPos.xy;
        shadowUV.y *= -1; //Flip 
        shadowUV = shadowUV * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        if (any(shadowUV.xy < 0.f) || any(shadowUV.xy > 1.f))   //Return if outside of the shadow map
            return false;
        return true;
    }

    /** Converts world position to shadow map light projection space. Also calculates the cascaded level.
      * The cascaded level could be calculated a bit faster but this method is more flexible, especially with different
      * shadow map projections. 
    \param[in] world position.
    \param[out] cascaded level.
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return returns if the position is visible from the shadow map.
    */
    bool directionalDistanceAndSampleVec(in float3 posW, out uint level, out float2 shadowUV, out float lightDist)
    {
        //Go through all levels
        for(level = 0; level < kCascadedLevels; level++)
        {
            float4x4 viewProjectMat = gSMCascadedVPBuffer[level];
            float4 projPos = mul(viewProjectMat, float4(posW, 1.f));
           
            projPos.y *= -1; //Flip 
            projPos.xy = projPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]

            shadowUV = projPos.xy;
            lightDist = projPos.z;
            
            if (all(shadowUV >= 0.00f) && all(shadowUV <= 1.f) && lightDist >= 0 && lightDist <= 1)   //Return if outside of the shadow map
                return true;
        }
        return false;
    }

    /** Converts world position to shadow map light projection space. Also calculates the cascaded level.
      * The cascaded level could be calculated a bit faster but this method is more flexible, especially with different
      * shadow map projections. 
    \param[in] world position.
    \param[in] random offset.
    \param[out] cascaded level.
    \param[out] shadow map sample vector.
    \param[out] the distance the shadow map should be compaired to.
    \return returns if the position is visible from the shadow map.
    */
    bool directionalDistanceAndSampleVec(in float3 posW, float random, out uint level, out float2 shadowUV, out float lightDist)
    {
        float stochasticRange = kCascadedStochasticRange * random;
        float near = gScene.camera.data.nearZ;
        float linearZ = abs(mul(gScene.camera.data.viewMat, float4(posW, 1.f)).z);
        for(level = 0; level < kCascadedLevels; level++)
        {
            float far = gCascadedZSlices[level];
            float range = far - near;
            float randomOffset = stochasticRange * range * 2;
            float maxOffset = (kCascadedStochasticRange * range)/2;
            range = (far - maxOffset) + randomOffset;
            if (linearZ < range)
                break;
            near = far;
        }

        float4x4 viewProjectMat = gSMCascadedVPBuffer[level];
        float4 projPos = mul(viewProjectMat, float4(posW, 1.f));

        projPos.y *= -1; //Flip 
        projPos.xy = projPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        shadowUV = projPos.xy;
        lightDist = projPos.z;

        bool valid = (all(shadowUV >= 0.f) && all(shadowUV <= 1.f) && lightDist >= 0 && lightDist <= 1) || (level == (kCascadedLevels-1));
        return valid;
    }
        
    /** Hybrid Shadow Mapping for the classic shadow map. Evaluates the shadow and tells if a ray should be used. rng is only used in Poission sampling
    \param[in] analytical light index.
    \param[in] world position.
    \param[in] light information.
    \param[in] falcor random number generator.
    \param[out] light factor in [0,1]; 1 -> fully lit ; 0 -> shadowed
    \return boolean that indicates if a ray is needed. (Outside of shadow map)
    */
    bool hybridShadowMapClassic(uint index,float lightDist,const uint lightType, float3 shadowUV, uint cascLevel,float4 random, bool useHybridSM, out float shadow ,inout bool isDynamic)
    {      
        float maxDist = FLT_MAX; //Maximum distance possible in scene
        float minDist = 0.f;

        //get min dist and max dist
        shadow = 0.f;
        float4 shadowVals;
        float4 w = float4(0.25);
        if (!kUsePCF && !kUsePoissonSampling && !useHybridSM)
        {
            float shadowDist = getShadowValue(index, shadowUV, lightType, cascLevel);
            shadow += (shadowDist > lightDist) ? 1.0 : 0.0;
            return true;
        }
        else if (!kUsePoissonSampling) //Use PCF
        {
            float3 gatherUV = shadowUV;
            const uint shadowMapRes = lightType == (uint) LightTypeSM::Directional ? kCascadedSMRes : k2DSMRes;
            w = getGatherWeights(shadowUV.xy, shadowMapRes, gatherUV.xy);

            shadowVals = getShadowValueGather(index, gatherUV, lightType,isDynamic, cascLevel);
        }
        else //Poisson Sampling
        {
            if (lightType == (uint) LightTypeSM::Point)
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(19.9999f * random[k]);
                    float3 uv = shadowUV + kPoissonCube[poisIdx] * gPoissonDiscRadCube; //TODO Better solution (angle?) 
                    shadowVals[k] = getShadowValue(index, uv, lightType, cascLevel);
                }
            }
            else
            {
                for (uint k = 0; k < 4; k++)
                {
                    uint poisIdx = uint(15.99999f * random[k]);
                    float3 uv = shadowUV;
                    const uint shadowMapRes = lightType == (uint) LightTypeSM::Directional ? kCascadedSMRes :  k2DSMRes;
                    uv.xy += kPoissonDisc[poisIdx] * ((gPoissonDiscRad) / shadowMapRes);
                    shadowVals[k] = getShadowValue(index, uv, lightType, cascLevel);
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
        if(useHybridSM){
            if ((lightDist > maxDist) && (lightDist < minDist))
            return false; //inbetween; Ray is needed
        }
               
        return true;    //No ray needed
    }
       
    //Eval the single depth version of the filterable shadow maps.
    float evalShadowMapFilterSD(uint index,float lightDist,const uint lightType, float3 shadowUV, uint cascLevel, inout bool isDynamic)
    {
        float shadow = 0;
        float4 lerpWeights = float4(0.25);
        float2 gatherUV = float2(0);
        //Get the gather weights and UV
        switch (lightType)
        {
            case LightTypeSM::Point:
                //TODO
                break;
            case LightTypeSM::Spot:
                lerpWeights = getGatherWeights(shadowUV.xy, k2DSMRes, gatherUV);
                shadowUV.xy = gatherUV;
                break;
            case LightTypeSM::Directional:
                lerpWeights = getGatherWeights(shadowUV.xy, kCascadedSMRes, gatherUV);
                shadowUV.xy = gatherUV;
                break;
            default:
                break;
        }
        float4 gatherDepths = getShadowValueGather(index, shadowUV, lightType,isDynamic ,cascLevel);
        //Evaluate the shadow map
        if(ShadowMapType(kShadowMapMode) == ShadowMapType::SDVariance){
            float2 varianceSV = float2(0);
            for(uint i=0; i<4;i++)
                varianceSV += lerpWeights[i] * float2(gatherDepths[i], gatherDepths[i]* gatherDepths[i]);

            shadow = ChebyshevUpperBound(varianceSV, lightDist, 1e-10f);
        }else if (ShadowMapType(kShadowMapMode) == ShadowMapType::SDExponentialVariance){
            float4 expVarianceSV = float4(0);
            for(uint i=0; i<4;i++){
                float rescaledDepth = (2.f * gatherDepths[i]) - 1.f;
                float4 evsm = recreateEVSMFromSingleDepth(rescaledDepth);
                expVarianceSV += lerpWeights[i] * evsm;
            }
            float2 upperLowerBound = exponentialVarianceShadowTest(expVarianceSV, lightDist);
            shadow = min(upperLowerBound.x, upperLowerBound.y);
        }else{
            float4 moments = float4(0);
            for(uint i=0; i<4;i++)
                moments += lerpWeights[i] * float4(gatherDepths[i], gatherDepths[i]* gatherDepths[i], gatherDepths[i]* gatherDepths[i] * gatherDepths[i],
                                                   gatherDepths[i]* gatherDepths[i] * gatherDepths[i]* gatherDepths[i]);

            shadow = msmHamburger(moments, lightDist);
        }

        return shadow;
    }

    bool evalShadowMap(float3 posW, const in SMLightData lightData, float4 random, float mipLevel, out float shadow)
    {
        bool isDynamic = false;
        return evalShadowMap(posW, lightData, random, mipLevel, shadow, isDynamic, false);
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
    bool evalShadowMap(float3 posW, const in SMLightData lightData, float4 random, float mipLevel, out float shadow, out bool isDynamic, bool useLeakTracing = false)
    {
        mipLevel = kUseSMMips ? mipLevel : 0.f; //Using mip level without mips can lead to a performance hit
        isDynamic = false;
        uint index = lightData.index;
        shadow = 0.0f;
        float lightDist = 0.f;
        float3 shadowUV = float3(0);
        uint cascLevel = 0;
        bool valid = false;
        bool cascadedHybrid = true;
        switch (lightData.type)
        {
            case LightTypeSM::Point:
                valid = pointDistanceAndSampleVec(posW, lightData, shadowUV, lightDist);
                break;
            case LightTypeSM::Spot:
                valid = spotDistanceAndSampleVec(index, posW, shadowUV.xy, lightDist);
                break;
            case LightTypeSM::Directional:
            #if CASC_USE_STOCH_LEVEL
                valid = directionalDistanceAndSampleVec(posW, random , cascLevel, shadowUV.xy, lightDist);
            #else
                valid = directionalDistanceAndSampleVec(posW, cascLevel, shadowUV.xy, lightDist);
            #endif
                if(kCascadedLevelTrace < cascLevel)
                    cascadedHybrid = false;
                break;
            default:
                break;
        }

        if (!valid || (useLeakTracing && !cascadedHybrid && kCascadedRayTracingAfterHybrid))
            return !kUseRayOutsideSM;   //Outside of shadow map

        switch (ShadowMapType(kShadowMapMode)){
            case ShadowMapType::ShadowMap:
            {
                return hybridShadowMapClassic(index, lightDist, lightData.type, shadowUV, cascLevel, random, useLeakTracing, shadow, isDynamic);
            }
            case ShadowMapType::Variance: //Variance Shadow Mapping. https://igm.univ-mlv.fr/~biri/Enseignement/MII2/Donnees/variance_shadow_maps.pdf
            {
                float2 varianceSV = getShadowVarianceValue(index, shadowUV, lightData.type, isDynamic, cascLevel, mipLevel);
                shadow = ChebyshevUpperBound(varianceSV, lightDist, 1e-10f);
                break;
            }
            case ShadowMapType::Exponential: //Exponential Shadow Mapping. https://jankautz.com/publications/esm_gi08.pdf
            {
                float shadowMapDepth = getShadowValueFiltered(index, shadowUV, lightData.type, isDynamic, cascLevel, mipLevel);
                shadow = exponentialShadowTest(shadowMapDepth, lightDist);
                break;
            }
            case ShadowMapType::ExponentialVariance: //Exponential Variance Shadow Mapping. 
            {
                float4 moments = getShadowValueF4(index, shadowUV,  lightData.type, isDynamic, cascLevel, mipLevel);
                float2 upperLowerBound = exponentialVarianceShadowTest(moments, lightDist);
                shadow = min(upperLowerBound.x, upperLowerBound.y);
                break;
            } 
            case ShadowMapType::MSMHamburger: //Moment Shadow Mapping (Hamburger)
            {
                float4 moments = getShadowValueF4(index, shadowUV,  lightData.type, isDynamic, cascLevel, mipLevel);
                shadow = msmHamburger(moments, lightDist);
                break;
            }
            case ShadowMapType::MSMHausdorff: //Moment Shadow Mapping (Hausdorff)
            {
                float4 moments = getShadowValueF4(index, shadowUV,  lightData.type, isDynamic, cascLevel, mipLevel);
                shadow = msmHausdorff(moments, lightDist);
                //shadow = evalShadowMap4Channel(index, lightDist, lightData.type, shadowUV, cascLevel, mipLevel, isDynamic);
                break;
            }
            case ShadowMapType::SDVariance:
            case ShadowMapType::SDExponentialVariance:
            case ShadowMapType::SDMSM:
                shadow = evalShadowMapFilterSD(index, lightDist, lightData.type, shadowUV, cascLevel, isDynamic);
                break;
            default:
                shadow = 1.f;
        }

        //Leak Tracing Test
        if(useLeakTracing && cascadedHybrid && (kShadowMapMode != (uint)ShadowMapType::ShadowMap)){
            //Clamp Shadow Value depending on the threshold
            if(shadow <= kHSMFilteredThreshold.x)
                shadow = 0.0;
            else if (shadow >= kHSMFilteredThreshold.y)
                shadow = 1.f;

            if ((shadow > kHSMFilteredThreshold.x && shadow < kHSMFilteredThreshold.y))
                return false;   //Shoot ray
        }
        return true;

    }

    bool parallel_or_orthogonal(float2 a, float2 b)
    {
        a = normalize(a);
        b = normalize(b);
        float d = abs(dot(a, b));
        if ((d < 1e-5f) || ((1.0f - d) < 1e-5f)) {
            return true;
        } 
        return false;
    }
    
    /* Calculates the LOD level from derivative positions.
    \param[in] lightData
    \param[in] World Position of the current pixel
    \param[in] Difference in position to the neighboring pixel (x-Axis) on the triangle plane of the current hit
    \param[in] Difference in position to the neighboring pixel (y-Axis) on the triangle plane of the current hit
    \return LOD level for the shadow map
    */
    float getSMLODLevel(const in SMLightData lightData, float3 posW ,float3 ddxPosW, float3 ddyPosW){
        //TODO Point lights (CubeTexture) are currently not supported
        if (lightData.type == (uint) LightTypeSM::Point)
            return 0.f;
        
        uint lightIdx = lightData.index;

        float4x4 viewProjectMat;
        uint bufferIdx = lightIdx;
        uint texSize = k2DSMRes;
        if(lightData.type == (uint)LightTypeSM::Directional){
            uint cascLevel = 0;
            float2 tmpUV;
            float tmpDist;
            if (!directionalDistanceAndSampleVec(posW, cascLevel, tmpUV, tmpDist))
                return 0.f;
            texSize = kCascadedSMRes;
            viewProjectMat = gSMCascadedVPBuffer[cascLevel];
        }else{
            viewProjectMat = gShadowMapVPBuffer[bufferIdx];
        }
        
        //Get Projected Position
        float4 projPos      = mul(viewProjectMat, float4(posW, 1.f));
        float4 projPosDdx   = mul(viewProjectMat, float4(posW + ddxPosW, 1.f));
        float4 projPosDdy   = mul(viewProjectMat, float4(posW + ddyPosW, 1.f));
        //UV coords
        float2 uv       = (projPos.xy / projPos.w);
        float2 uvDdx    = (projPosDdx.xy / projPosDdx.w);
        float2 uvDdy    = (projPosDdy.xy / projPosDdy.w);
        //Flip v (Handness) and convert [-1,1] to [0,1]
        uv      = uv * float2(0.5,-0.5) + 0.5;
        uvDdx   = uvDdx * float2(0.5,-0.5) + 0.5;
        uvDdy   = uvDdy * float2(0.5,-0.5) + 0.5;
        //Get Gradient
        uvDdx -= uv;
        uvDdy -= uv;

        //LOD Calculation inspired by https://graemephi.github.io/posts/calculating-lod/
        
        //Get Pixel diff
        uvDdx *= texSize;
        uvDdy *= texSize;
        
    #if 1
        // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#LODCalculation
        if (parallel_or_orthogonal(uvDdx, uvDdy) == false) {
            float A = uvDdx.y * uvDdx.y + uvDdy.y * uvDdy.y;
            float B = -2.0 * (uvDdx.x * uvDdx.y + uvDdy.x * uvDdy.y);
            float C = uvDdx.x * uvDdx.x + uvDdy.x * uvDdy.x;
            float F = uvDdx.x * uvDdy.y - uvDdy.x * uvDdx.y;
            F *= F;

            float p = A - C;
            float q = A + C;
            float t = sqrt(p*p + B*B);

            uvDdx.x = sqrt(F * (t+p) / (t * (q+t)));
            uvDdx.y = sqrt(F * (t-p) / (t * (q+t))) * sign(B);
            uvDdy.x = sqrt(F * (t-p) / (t * (q-t))) * -sign(B);
            uvDdy.y = sqrt(F * (t+p) / (t * (q-t)));
        }
    #endif
    
        //max squared magnitude of the change along x and y
    #if 0
        float delta_max_sqr = min(dot(uvDdx, uvDdx), dot(uvDdy, uvDdy));
    #else
         float delta_max_sqr = max(dot(uvDdx, uvDdx), dot(uvDdy, uvDdy));
    #endif
        // equivalent to log2(sqrt(delta_max_sqr))
        return max(0.0, 0.5 * log2(delta_max_sqr));
    }
};

ParameterBlock<ShadowMap> gShadowMap;
