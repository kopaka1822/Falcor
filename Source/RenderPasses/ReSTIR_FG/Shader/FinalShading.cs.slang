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
#ifndef USE_CAUSTIC_FILTER_RESERVOIR
#define USE_CAUSTIC_FILTER_RESERVOIR 1
#endif

#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif


StructuredBuffer<FGData> gFGSampleData;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<float4> gCausticRadiance;
Texture2D<float4> gThp;

Texture2D<PackedHitInfo> gVBufferDI;
Texture2D<float4> gViewDI;
Texture2D<float4> gThpDI;

RWTexture2D<float4> gOutColor;
RWTexture2D<float4> gOutEmission;
RWTexture2D<float4> gOutDiffuseRadiance;
RWTexture2D<float4> gOutSpecularRadiance;
Texture2D<float4> gOutDiffuseReflectance;
Texture2D<float4> gOutSpecularReflectance;
RWTexture2D<float4> gOutResidualRadiance;

//For view copy
RWTexture2D<float4> gViewDIPrev;
RWTexture2D<float4> gViewPrev;

// Caustic Reservoir (optinal)
StructuredBuffer<CausticSample> gCausticSample;
#if USE_REDUCED_RESERVOIR_FORMAT
RWTexture2D<uint2> gCausticReservoir;
#else
RWTexture2D<uint4> gCausticReservoir;
#endif

#define is_valid(name) (is_valid_##name != 0)

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
    float gAttenuationRadius;           //Current global photon radius
    uint2 gFrameDim; //Frame Dimensions for camera dir
}

static const float kRayEpsilon = 0.98f;
static const bool kUseEnvBackground = USE_ENV_BACKROUND;
static const bool kEmissionToCausticFilter = EMISSION_TO_CAUSTIC_FILTER;

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
    //Load Reflectance (was set before)
    float3 diffuseReflectance = { };
    float3 specularReflectance = { };
    if (is_valid(gOutDiffuseReflectance))
        diffuseReflectance = gOutDiffuseReflectance[pixel].xyz;
    if (is_valid(gOutSpecularReflectance))
        specularReflectance = gOutSpecularReflectance[pixel].xyz;
    
    
    //Outputs
    float3 color = { };
    float3 emission = { };
    float3 diffuseIllumination = { };
    float3 specularIllumination = { };
    float hitT = 1.f;
    uint hitTSamples = 0;

    // Variables for RTXDI and ReSTIRFG
    float3 diffuse = { };
    float3 specular = { };
    float3 caustic =  { };
    float distance = 1.f;
    float3 dir = { };
    float3 Li = { };

    bool validSample = false;
    
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 8 * 13); //TODO add seed offset
    let lod = ExplicitLodTextureSampler(0.f);
    
    float3 viewDir;
    HitInfo hitInfo;
#if USE_RTXDI
    viewDir = -gViewDI[pixel].xyz;
    
    gViewDIPrev[pixel] = float4(-viewDir,1); //copy view
    
    hitInfo = HitInfo(gVBufferDI[pixel]);
    if (hitInfo.isValid())
    {
        validSample |= true;
        float3 thp = gThpDI[pixel].xyz;
            
        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        emission += bsdfProperties.emission * thp; //Emission
        //Get Final Sample from RTXDI
        bool rtxdiValid = gRTXDI.getFinalSample(pixel, dir, distance, Li);
        // Check light visibility.
        if (rtxdiValid)
        {
        // Create a DXR 1.1 query object to trace a ray (the <1> means use alpha testing)
            SceneRayQuery < 1 > rayQuery;
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance * kRayEpsilon);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                rtxdiValid = false;
        }

        if (rtxdiValid)
        {
            sd.mtl.setActiveLobes((uint) LobeType::DiffuseReflection);
            diffuse += bsdf.eval(sd, dir, sg) * Li * thp;

            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular += bsdf.eval(sd, dir, sg) * Li * thp;

            hitTSamples++;
            hitT = distance;
        }
    }
#endif //RTXDI

    //Load final gather and ReSTIR FG data
    float4 viewTmp = gView[pixel];
    viewDir = -viewTmp.xyz;

#if USE_RESTIRFG
        bool needEmissive = viewTmp.w > 1.1 && !kEmissionToCausticFilter; //Direct light for indirect flag
        gViewPrev[pixel] = float4(-viewDir,1); //copy view
#endif //USE_RESTIRFG
    
    hitInfo = HitInfo(gVBuffer[pixel]);
    bool wasTraced = false;
    
    if (hitInfo.isValid())
    {
        validSample |= true;
        float4 thpTex = gThp[pixel];
        float3 thp = thpTex.xyz;
        wasTraced = thpTex.w > 0.5;
  
        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
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
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance * kRayEpsilon);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                validIndirectReservoir = false;
        }
        //TODO check if this adds no bias
        /*
        if (!validIndirectReservoir) {
            r.weightSum = 0;
            gReservoir[pixel] = r.pack();
        }
        */
        if (validIndirectReservoir)
        {
            sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
            diffuse += bsdf.eval(sd, dir, sg) * Li * thp;
            
            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular += bsdf.eval(sd, dir, sg) * Li * thp;

            hitTSamples++;
            hitT += distance;
            hitT *= 1.f / hitTSamples;
        }
       
        //Add Emissive to additionally traced materials
        if(needEmissive)
            emission += bsdfProperties.emission * thp;
        
#endif //RESTIRFG

        caustic = gCausticRadiance[pixel].xyz;
#if USE_CAUSTIC_FILTER_RESERVOIR
        sd.mtl.setActiveLobes((uint)LobeType::All);
        Reservoir causticReservoir = Reservoir(gCausticReservoir[pixel]);
        if (causticReservoir.M > 0) {
            CausticSample causticSample = gCausticSample[index2Dto1D(pixel, gFrameDim.x)];
            float3 f_r = bsdf.eval(sd, causticSample.dir, sg);
            float NdotL = dot(bsdfProperties.guideNormal, causticSample.dir);
            float3 causticReservoirRadiance = float3(0);
            if (NdotL > 0)
                causticReservoirRadiance += max(0.f, (f_r / NdotL) * causticSample.flux * causticReservoir.weightSum);
            caustic += causticReservoirRadiance * thp;
        }
        
#endif // USE_CAUSTIC_FILTER_RESERVOIR
        
            
    //If only RTXDI is used, add the previous calculated light color
    #if USE_RTXDI && !USE_RESTIRFG
            float3 fgRadiance = gOutColor[pixel].xyz * thp;
            diffuse += fgRadiance;  //TODO do this properly
    #endif
    
    }   

    if(validSample)
    {
        color += diffuse + specular + caustic;    //Add diffuse and specular parts together
        color += emission;  //Do this or demodulate it?

        // NRD
        if(wasTraced){
            float3 combined = (diffuse + specular + caustic);
            specularIllumination = combined / specularReflectance;
        }
        else
        {
            diffuseIllumination = ((diffuse + caustic)) / diffuseReflectance;
            specularIllumination = (specular) / specularReflectance;
        }
    }else
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
    if (is_valid(gOutResidualRadiance))  gOutResidualRadiance[pixel] = float4(0.f); //TODO 
}
