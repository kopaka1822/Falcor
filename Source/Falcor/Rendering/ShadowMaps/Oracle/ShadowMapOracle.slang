#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Color.ColorHelpers;
import Rendering.ShadowMaps.ShadowMapData;
import Rendering.ShadowMaps.ShadowMapHelpers;

#ifndef USE_ORACLE_FUNCTION
#define USE_ORACLE_FUNCTION 0
#endif

#ifndef SMORACLE_NUM_LIGHTS
#define SMORACLE_NUM_LIGHTS 1
#endif

cbuffer ShadowMapOracleCB {
    float kLightsNPS[SMORACLE_NUM_LIGHTS];
}

struct ShadowMapOracle
{
#if USE_ORACLE_FUNCTION                                  
    static const float kCameraNPS = SMORACLE_CAMERA_NPS;                                 //Normalized Pixel Size for the Camera

    //Static Constants
    static const uint kNPSOffsetSpot = SMORACLE_NPS_OFFSET_SPOT;                     //Spotlight offset for kLightsNPS
    static const uint kNPSOffsetCasc = SMORACLE_NPS_OFFSET_CASCADED;                 //Cascaded offset for kLightsNPS
    static const uint kOracleDistFunctionMode = SMORACLE_DIST_FUNCTION_MODE;  //Mode for the Oracle Distance Function
    
    //Static Settings
    static const float kOracleCompaireValue = ORACLE_COMP_VALUE;        //Shoot an ray when the Oracle function is lower than this value
    static const bool kOracleUseLeakTracing = SMORACLE_USE_LEAK_TRACING;        //Setting for leak trace usage
    static const bool kIgnoreOracleForDirect = SMORACLE_IGNORE_FOR_DIRECT;

    #ifdef USE_SM_MIP
    static const bool kUseSMMips = USE_SM_MIP;
    static const float kMipBias = SM_MIP_BIAS; // angle bias used in mip calculation (cos theta^bias)
    #endif //USE_SM_MIP

#endif //USE_ORACLE_FUNCTION 
    /** Oracle Funktion for determening if a shadow map can be used without quality loss or if a ray should be used instead
    \param[in] analytical light index.
    \param[in] light information.
    \param[in] shading data of the current surface.
    \param[in] current distance from camera to hit. Can be modified with the oracleDistanceFactor for better results with indirect bounces
    \param[out] Optional Mip Level. Written if kUseSMMips == true
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    static float oracleFunction(const in SMLightData lightData, const in ShadingData sd, float rayDistance, out float mipLevel)
    {
        mipLevel = 0.f; //Default value
    #if !USE_ORACLE_FUNCTION
        return 0.0;
    #else
        uint lightIdx = lightData.index;

        if (lightData.type == (uint) LightTypeSM::Spot)
            lightIdx += kNPSOffsetSpot;
        
        float distanceToLight;
        float3 toLight;

        //Get LightDir and Distance
        if (lightData.type == (uint) LightTypeSM::Directional)
        {
            uint level = 0; //TODO set properly
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
        
        const float3 N = sd.frame.N;    //TODO add option to switch between face and shading normal
        
        //Get the cos theta between normal and vectors
        float angleCamera = abs(dot(sd.V, N));
        float angleLight = abs(dot(toLight, N));
        
        float projAreaCam = kCameraNPS * rayDistance * rayDistance;
        float pixelAreaSM = kLightsNPS[lightIdx];
        /*
        float pixelAreaSM = 1.0;
        [unroll]
        for (int i = 0; i < SMORACLE_NUM_LIGHTS; i++){
            if (i == lightIdx)
                pixelAreaSM = kLightsNPS[i];
        }
        */
        float projSMArea = pixelAreaSM;

        #ifdef USE_SM_MIP
        //Calculate MIP
        if(kUseSMMips){
            mipLevel = 0.5f * log2(( projAreaCam * pow(angleLight,kMipBias) ) / ( projSMArea * pow(angleCamera,kMipBias) + 1e-10f) ); 
        }
        #endif //USE_SM_MIP
        
        return ( projSMArea * angleCamera) / ( projAreaCam * angleLight + 1e-10f);  //Divide the projected areas
       
    #endif//USE_ORACLE_FUNCTION
    }

    /** Oracle Funktion for determening if a shadow map can be used without quality loss or if a ray should be used instead
    \param[in] analytical light index.
    \param[in] light information.
    \param[in] shading data of the current surface.
    \param[in] current distance from camera to hit. Can be modified with the oracleDistanceFactor for better results with indirect bounces
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    static float oracleFunction(const in SMLightData lightData, const in ShadingData sd, float rayDistance)
    {
    #if !USE_ORACLE_FUNCTION
        return 0.0;
    #else
        uint lightIdx = lightData.index;

        if (lightData.type == (uint) LightTypeSM::Spot)
            lightIdx += kNPSOffsetSpot;
        
        float distanceToLight;
        float3 toLight;

        //Get LightDir and Distance
        if (lightData.type == (uint) LightTypeSM::Directional)
        {
            uint level = 0; //TODO set properly
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
        
        const float3 N = sd.frame.N;    //TODO add option to switch between face and shading normal
        
        //Get the cos theta between normal and vectors
        float angleCamera = abs(dot(sd.V, N));
        float angleLight = abs(dot(toLight, N));
        
        float projAreaCam = kCameraNPS * rayDistance * rayDistance;
        float pixelAreaSM = kLightsNPS[lightIdx];
        float projSMArea = pixelAreaSM * distanceToLight * distanceToLight;
                
        return ( projSMArea * angleCamera) / ( projAreaCam * angleLight + 1e-10f);  //Divide the projected areas
    #endif //USE_ORACLE_FUNCTION
    }

     /** Bool if the oracle value should result in a ray or not
     */
    static bool oracleNeedRay(float oracleValue){
        #if !USE_ORACLE_FUNCTION
        return true;
        #else
        return oracleValue > kOracleCompaireValue;
        #endif
    }

     /** Calculate the Oracle distance factor based on the currently selected mode. Should be used on bounces only
    \param[in] material/bsdf properties.
    \param[in] current reflection sample.
    \return a factor the indirect bounce distance should be multiplied with. Used to the oracle function as distance
    */
    static float getOracleDistanceFunctionFactor(const in BSDFProperties bsdfProperties,const in BSDFSample bsdfSample)
    {
    #if !USE_ORACLE_FUNCTION
        return 1.0;
    #else
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
    #endif //USE_ORACLE_FUNCTION
    }   
};
