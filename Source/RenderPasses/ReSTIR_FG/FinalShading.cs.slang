#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;

import Reservoir;
import FinalGatherData;

//For Syntax highlighting
#ifndef USE_RTXDI
    #define USE_RTXDI 1
#endif
#ifndef USE_RESTIRFG
    #define USE_RESTIRFG 1
#endif



#ifdef USE_REDUCED_RESERVOIR_FORMAT
    Texture2D<uint2> gReservoir;
#else
    Texture2D<uint4> gReservoir;
#endif


StructuredBuffer<FGData> gFGSampleData;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float2> gMVec;
Texture2D<float4> gView;
Texture2D<float4> gCausticRadiance;
RWTexture2D<float4> gColorOut;
/*
RWTexture2D<float4> gDiffuseIllumination;
RWTexture2D<float4> gDiffuseReflectance;
RWTexture2D<float4> gSpecularIllumination;
RWTexture2D<float4> gSpecularReflectance;
RWTexture2D<float4> gDebug;
*/

#define is_valid(name) (is_valid_##name != 0)

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
    float gAttenuationRadius;           //Current global photon radius
    uint2 gFrameDim; //Frame Dimensions for camera dir
}

static const float kRayEpsilon = 0.99f;

uint index2Dto1D(uint2 idx, uint width){
    return idx.x + width * idx.y;
}

ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

void getEmissiveLight(uint idx,in float3 worldPos, out float distance ,out float3 toLight, out float3 Li){
    //Get light position
    FGData fgData = gFGSampleData[idx];
    
    toLight = fgData.posW - worldPos;
    distance = length(toLight);
    toLight = toLight/ distance;    //normalize

    Li = fgData.flux;
    float distAttenuation = vplAttenuation(distance, gAttenuationRadius);
    float cosTheta = saturate(dot(normalize(fgData.normal), -toLight));
    Li *= (cosTheta * distAttenuation)/fgData.jacobianDeterminant;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    bool valid = true;
    
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 8 * 13); //TODO add seed offset

    //Get shading data
    PackedHitInfo packedHitInfo = gVBuffer[pixel];
    HitInfo hitInfo = HitInfo(packedHitInfo);
    if(!hitInfo.isValid())      //TODO: write zero for invalid surfaces
        return;
    
    let lod = ExplicitLodTextureSampler(0.f);
    //Get view buffer
    float3 viewDir;
    viewDir = -gView[pixel].xyz;
    
    //Get Material properties
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);

   

    // Variables for RTXDI and ReSTIRFG
    float3 outColor = { };
    float distance = 0;
    float3 dir = { };
    float3 Li = { };

#if USE_RTXDI
    //Get Final Sample from RTXDI
    bool rtxdiValid = gRTXDI.getFinalSample(pixel, dir, distance, Li);
    // Check light visibility.
    if (rtxdiValid)
    {
        // Create a DXR 1.1 query object to trace a ray (the <1> means use alpha testing)
        SceneRayQuery<1> rayQuery;
        const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance);
        if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
            rtxdiValid = false;
    }

    if(rtxdiValid){
        outColor += bsdf.eval(sd, dir, sg) * Li;
    }
    
#endif

#if USE_RESTIRFG
    //Get reservoir
    bool validIndirectReservoir = true;
    Reservoir r = Reservoir(gReservoir[pixel]);
    
    
    if (r.weightSum < 0 || r.M == 0)
    {
        validIndirectReservoir = false;
    }
    
    getEmissiveLight(index2Dto1D(pixel, gFrameDim.x), sd.posW, distance, dir, Li);
    Li = max(0.f, Li * r.weightSum);

    //Check light visibility with a shadow ray
    if (validIndirectReservoir)
    {
        distance *= kRayEpsilon;
        // Ray Query for shadow test; <1> means enabled alpha testing; TODO: Add static const for alpha test
        SceneRayQuery<1> rayQuery;
        const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance);
        if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
            validIndirectReservoir = false;
    }
    
    
    if (validIndirectReservoir)
        outColor += bsdf.eval(sd, dir, sg) * Li;

    outColor += gCausticRadiance[pixel].xyz; //Add caustic radiance
 #endif
    
    outColor += bsdfProperties.emission;    //TODO demodulate this
    
    if (any(isnan(outColor)) || any(isinf(outColor)))
        return;

    /*
    //Debug
    float3 diffuseReflectance = bsdfProperties.diffuseReflectionAlbedo;
    float3 diffuseFactor = diffuseReflectance <= 0.f ? 0.f : 1.f / diffuseReflectance;
    float3 diffuseIllumination = diffuse * diffuseFactor;

    float NdotV = saturate(dot(sd.N, sd.V));
    float ggxAlpha = bsdfProperties.roughness * bsdfProperties.roughness;
    float3 specularReflectance = approxSpecularIntegralGGX(bsdfProperties.specularReflectance, ggxAlpha, NdotV);
    float3 specularFactor = specularReflectance <= 0.f ? 0.f : 1.f / specularReflectance;
    float3 specularIllumination = specular * specularFactor;

    //DEBUG
    //float4 debugReservoir = float4(float(r.lightIdx), float(r.M), r.weightSum, r.targetPdf);
    //gDiffuseReflectance[pixel] = debugReservoir;
    
    if (is_valid(gDiffuseReflectance)) gDiffuseReflectance[pixel] = float4(diffuseReflectance, 1.f);
    if (is_valid(gDiffuseIllumination)) gDiffuseIllumination[pixel] = float4(diffuseIllumination, distance);
    if (is_valid(gSpecularReflectance)) gSpecularReflectance[pixel] = float4(specularReflectance, 1.f);
    if (is_valid(gSpecularIllumination)) gSpecularIllumination[pixel] = float4(specularIllumination, distance);
    */

    //If only RTXDI is used, add the previous calculated light color
#if USE_RTXDI && !USE_RESTIRFG
    outColor += gColorOut[pixel].xyz;
#endif
    
    gColorOut[pixel] = float4(outColor, 1.0f);
}
