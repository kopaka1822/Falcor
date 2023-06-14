#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Rendering.Materials.IsotropicGGX;
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

#if USE_REDUCED_RESERVOIR_FORMAT
    Texture2D<uint2> gReservoir;
#else
    Texture2D<uint4> gReservoir;
#endif


StructuredBuffer<FGData> gFGSampleData;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float2> gMVec;
Texture2D<float4> gView;
Texture2D<float4> gCausticRadiance;
RWTexture2D<float4> gOutColor;
RWTexture2D<float4> gOutEmission;
RWTexture2D<float4> gOutDiffuseRadiance;
RWTexture2D<float4> gOutSpecularRadiance;
RWTexture2D<float4> gOutDiffuseReflectance;
RWTexture2D<float4> gOutSpecularReflectance;
RWTexture2D<float4> gOutResidualRadiance;

#define is_valid(name) (is_valid_##name != 0)

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
    float gAttenuationRadius;           //Current global photon radius
    uint2 gFrameDim; //Frame Dimensions for camera dir
}

static const float kRayEpsilon = 0.99f;
static const float kDemodulateMinReflectance = 0.01f;
static const bool kUseEnvBackground = USE_ENV_BACKROUND;


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

    //Outputs
    float3 color = { };
    float3 emission = { };
    float3 diffuseReflectance = { };
    float3 diffuseIllumination = { };
    float3 specularReflectance = { };
    float3 specularIllumination = { };
    float3 resuidualIllumination = { };
    float hitT = 1.f;
    uint hitTSamples = 0;
    
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 8 * 13); //TODO add seed offset

    //Get shading data
    //Get view buffer
    float3 viewDir = -gView[pixel].xyz;
    PackedHitInfo packedHitInfo = gVBuffer[pixel];
    HitInfo hitInfo = HitInfo(packedHitInfo);
    if (hitInfo.isValid())
    {    
        let lod = ExplicitLodTextureSampler(0.f);
        
    
        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

   

        // Variables for RTXDI and ReSTIRFG
        float3 diffuse = { };
        float3 specular = { };
        float distance = 1.f;
        float3 dir = { };
        float3 Li = { };

    #if USE_RTXDI
        //Get Final Sample from RTXDI
        bool rtxdiValid = gRTXDI.getFinalSample(pixel, dir, distance, Li);
        // Check light visibility.
        if (rtxdiValid)
        {
        // Create a DXR 1.1 query object to trace a ray (the <1> means use alpha testing)
            SceneRayQuery < 1 > rayQuery;
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                rtxdiValid = false;
        }

        if (rtxdiValid)
        {
            sd.mtl.setActiveLobes((uint) LobeType::DiffuseReflection);
            diffuse = bsdf.eval(sd, dir, sg) * Li;

            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular = bsdf.eval(sd, dir, sg) * Li;

            hitTSamples++;
            hitT = distance;

            color += diffuse + specular;
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
        // Ray Query for shadow test; <1> means enabled alpha testing; TODO: Add static const for alpha test
            SceneRayQuery < 1 > rayQuery;
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                validIndirectReservoir = false;
        }
    
    
        if (validIndirectReservoir)
        {
            sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
            diffuse = bsdf.eval(sd, dir, sg) * Li;

            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular = bsdf.eval(sd, dir, sg) * Li;

            hitTSamples++;
            hitT += distance;
            hitT *= 1.f / hitTSamples;

            color += diffuse + specular;
        }

        color += gCausticRadiance[pixel].xyz; //Add caustic radiance
    #endif
    
        emission += bsdfProperties.emission; //Emission
        color += emission;  //Do this or demodulate it?

        //If only RTXDI is used, add the previous calculated light color
    #if USE_RTXDI && !USE_RESTIRFG
            float3 fgRadiance = gOutColor[pixel].xyz;
            color += fgRadiance;
            diffuse += fgRadiance;  //TODO do this properly
    #endif
        

         // Use sum of reflection/transmission albedo as they are denoised together.
        diffuseReflectance = max(kDemodulateMinReflectance, bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo);
        diffuseIllumination = diffuse / diffuseReflectance;
        
        // For specular an approximate shading model with a single specular lobe is used.
        const float NdotV = saturate(dot(bsdfProperties.guideNormal, sd.V));
        const float ggxAlpha = bsdfProperties.roughness * bsdfProperties.roughness;
        specularReflectance = approxSpecularIntegralGGX(bsdfProperties.specularReflectionAlbedo, ggxAlpha, NdotV);
        specularReflectance = max(kDemodulateMinReflectance, specularReflectance);
        specularIllumination = specular / specularReflectance;        
    }
    else
    {
        if (kUseEnvBackground)
        {
            bool invalid = viewDir.x == 0 && viewDir.y == 0 && viewDir.z == 0;
            if (!invalid)
            {
                emission = gScene.envMap.eval(viewDir);
                color = emission;
            }

        }
    }
    
    
    if (is_valid(gOutColor)) gOutColor[pixel] = float4(color, 1.f);
    if (is_valid(gOutEmission)) gOutEmission[pixel] = float4(emission, 1.f);
    if (is_valid(gOutDiffuseRadiance)) gOutDiffuseRadiance[pixel] = float4(diffuseIllumination, hitT);
    if (is_valid(gOutSpecularRadiance))gOutSpecularRadiance[pixel] = float4(specularIllumination, hitT);
    if (is_valid(gOutDiffuseReflectance)) gOutDiffuseReflectance[pixel] = float4(diffuseReflectance, 1.f);
    if (is_valid(gOutSpecularReflectance)) gOutSpecularReflectance[pixel] = float4(specularReflectance, hitT);
    if (is_valid(gOutResidualRadiance))  gOutResidualRadiance[pixel] = float4(0.f); //TODO 
}
