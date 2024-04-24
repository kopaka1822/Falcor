#pragma once
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Rendering.Lights.LightHelpers;
import Utils.Color.ColorHelpers;
__exported import Rendering.ShadowMaps.ShadowMapData;
__exported import Rendering.ShadowMaps.ShadowMapHelpers;

struct ShadowMapOracle
{
    StructuredBuffer<float> gShadowMapNPSBuffer;                                    //Buffer for the Normalized Pixel Size [Point | Spot | Cascaded]

    float gCameraNPS;                                   //Normalized Pixel Size for the Camera

    //Static Constants
    static const uint kNPSOffsetSpot = NPS_OFFSET_SPOT;                     //Spotlight offset for gShadowMapNPSBuffer
    static const uint kNPSOffsetCasc = NPS_OFFSET_CASCADED;                 //Cascaded offset for gShadowMapNPSBuffer
    static const uint kOracleDistFunctionMode = ORACLE_DIST_FUNCTION_MODE;  //Mode for the Oracle Distance Function

    static const bool kUseSMMips = USE_SM_MIP;
    static const float kMipBias = SM_MIP_BIAS; // angle bias used in mip calculation (cos theta^bias)
        
    /** Oracle Funktion for determening if a shadow map can be used without quality loss or if a ray should be used instead
    \param[in] analytical light index.
    \param[in] light information.
    \param[in] shading data of the current surface.
    \param[in] current distance from camera to hit. Can be modified with the oracleDistanceFactor for better results with indirect bounces
    \param[out] Optional Mip Level. Written if kUseSMMips == true
    \return a oracle value that indicates the proportion of the seen camera pixel area to the seen shadow map pixel area
    */
    float oracleFunction(const in SMLightData lightData, const in ShadingData sd, float rayDistance, out float mipLevel)
    {
        mipLevel = 0.f; //Default value
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
    float oracleFunction(const in SMLightData lightData, const in ShadingData sd, float rayDistance)
    {
        float mipLevel;
        return oracleFunction(lightData, sd, rayDistance, mipLevel);
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

ParameterBlock<ShadowMapOracle> gShadowMapOracle;
