#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;

import Reservoir;
import FinalGatherData;

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
void main(uint2 dTid : SV_DispatchThreadID)
{
    bool valid = true;
    //Get reservoir
    Reservoir r = Reservoir(gReservoir[dTid]);
    SampleGenerator sg = SampleGenerator(dTid, gFrameCount + 8 * 13);    //TODO add seed offset

    //Get shading data
    PackedHitInfo packedHitInfo = gVBuffer[dTid];
    HitInfo hitInfo = HitInfo(packedHitInfo);
    if(!hitInfo.isValid())
        return;
    
    let lod = ExplicitLodTextureSampler(0.f);
    //Get view buffer
    float3 viewDir;
    viewDir = -gView[dTid].xyz;
    /*
    if (is_valid(gView))
    {
        viewDir = -gView[dTid].xyz;
    }
    else
    {
        viewDir = gScene.camera.computeRayPinhole(dTid, gFrameDim).dir;
    }
    */
    //Get Material properties
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);

    if (luminance(bsdfProperties.emission) > 0) //Emissive surfaces are not handled here
        return;
    
    if (r.weightSum < 0 || r.M == 0)
    {
        valid = false;
    }
    
    //Get to light direction
    float distance = 0;
    float3 toLight = { };
    float3 Li = { };
    getEmissiveLight(index2Dto1D(dTid, gFrameDim.x),sd.posW  ,distance , toLight, Li);
    Li = max(0.f, Li * r.weightSum);

    //Check light visibility with a shadow ray
    if (valid)
    {
        distance *= kRayEpsilon;
        // Ray Query for shadow test; <1> means enabled alpha testing; TODO: Add static const for alpha test
        SceneRayQuery<1> rayQuery;
        const Ray ray = Ray(sd.computeNewRayOrigin(), toLight, 0.f, distance);
        if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
        {
            valid = false;           
        }
    }
    
    float3 outColor = { };
    if(valid){
        outColor += bsdf.eval(sd, toLight, sg) * Li;
    }

    outColor += gCausticRadiance[dTid].xyz;     //Add caustic radiance
    
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
    //gDiffuseReflectance[dTid] = debugReservoir;
    
    if (is_valid(gDiffuseReflectance)) gDiffuseReflectance[dTid] = float4(diffuseReflectance, 1.f);
    if (is_valid(gDiffuseIllumination)) gDiffuseIllumination[dTid] = float4(diffuseIllumination, distance);
    if (is_valid(gSpecularReflectance)) gSpecularReflectance[dTid] = float4(specularReflectance, 1.f);
    if (is_valid(gSpecularIllumination)) gSpecularIllumination[dTid] = float4(specularIllumination, distance);
    */
    
    gColorOut[dTid] = float4(outColor, 1.0f);

   
}
