#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Color.ColorHelpers;
import Rendering.Materials.Microfacet;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;

#define USE_SUBGROUP_STORE 0

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
}

cbuffer CB
{
    uint gMaxRecursion; //Max photon recursion depths
    uint gFlags; //Flags for the pass. See GenerationFlags enum
};


#if USE_EMISSIVE_LIGHT  //Buffer is only valid if emissive light is enabled
cbuffer Light{
    EmissiveLightSampler gEmissiveSampler;
}
#endif

RWStructuredBuffer<AABB> gLightTraceAABB;
RWStructuredBuffer<float4> gLightTraceData;
RWBuffer<uint> gPhotonCounter;
Texture2D<uint> gPhotonCullingMask;

enum class GenerationFlags // : uint32_t
{
    None                = 0x00,
    AlphaTest           = 0x01,     //Enable Alpha test
    AdjShadingNormals   = 0x02,     //Adjust the shading normals
    EnableCaustics      = 0x04,     //Enable caustic photons.
    DeltaRejection      = 0x08,     //Accept every non delta material as diffuse
    DeltaRejReqDiff     = 0x10,     //Reqire diffuse part for delta rejection
    AnalyticLight       = 0x20,     //Use analytic light photons
};

static const uint kPhotonBufferSize = uint(PHOTON_BUFFER_SIZE_GLOBAL);
static const uint kRayFlagTerminatedMiss = 1;
static const uint kRayFlagTerminatedHit = 2;



/** Payload for scatter ray (80B).
*/
struct RayData
{
    float3 thp; ///< Current path throughput. This is updated at each path vertex.
    uint packedNormal; ///< Face normal encoded in 16 bit polar coordinates
    float3 origin; ///< Next path segment origin.
    uint terminated; ///< Set to true when path is terminated.
    float3 direction; ///< Next path segment direction.
    bool diffuseHit; ///< saves if the his is diffuse
    bool isEmissive; ///<If the hit is emissive

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).

    /** Create ray payload with default parameters.
    */
    static RayData create()
    {
        RayData d;
        d.terminated = 0;
        d.packedNormal = 0;
        d.thp = float3(1);
        d.origin = float3(0);
        d.direction = float3(0);
        d.isEmissive = false;
        return d;
    }
};

struct ShadowData
{
    bool shadowed;
};

struct PhotonInfo
{
    float3 flux;
    float faceNTheta;
    float3 dir;
    float faceNPhi;
};

struct PhotonLightSample
{
    float3 posW;
    float3 normal;
    float3 flux;
};

float3 fromWordlToLocal(const in float3 normal, float3 w)
{
    // reconstruct tangent frame based off worldspace normal
    // this is ok for isotropic BRDFs
    // for anisotropic BRDFs, we need a user defined tangent
    float3 bitangent = perp_stark(normal);
    float3 tangent = cross(bitangent, normal);

    return float3(dot(bitangent, w), dot(tangent, w), dot(normal, w));
}

//transforms direction from local to world space
float3 fromLocalToWorld(const in float3 normal, float3 w)
{
    //transform from local space -> random dir vector in world space
    float3 bitangent = perp_stark(normal);
    float3 tangent = cross(bitangent, normal);

    return w.x * tangent + w.y * bitangent + w.z * normal;
}

#if USE_EMISSIVE_LIGHT  //Function is only valid if the emissive sampler is set
bool createLightSample(inout SampleGenerator sg, out PhotonLightSample pls ){

    if (gScene.lightCollection.isEmpty())
        return false;

    //Randomly pick a triangle out of the global list with uniform probability
    const uint triangleCount = gScene.lightCollection.triangleCount;
    // Safety precaution as the result of the multiplication may be rounded to triangleCount even if uLight < 1.0 when triangleCount is large.
    uint triangleIndex = min((uint)(sampleNext1D(sg) * triangleCount), triangleCount - 1);

    uint2 packed = gEmissiveSampler._emissivePower.triangleAliasTable[triangleIndex];
    float threshold = f16tof32(packed.x >> 16u);
    uint  selectAbove = ((packed.x & 0xFFFFu) << 8u) | ((packed.y >> 24u) & 0xFFu);
    uint  selectBelow = packed.y & 0xFFFFFFu;

    // Test the threshold in the current table entry; pick one of the two options
    triangleIndex = (sampleNext1D(sg) >= threshold) ? selectAbove : selectBelow;

    float triangleSelectionPdf = gScene.lightCollection.fluxData[triangleIndex].flux * gEmissiveSampler._emissivePower.invWeightsSum;
    
    const EmissiveTriangle tri = gScene.lightCollection.getTriangle(triangleIndex);
    const float3 barycentrics = sample_triangle(sampleNext2D(sg));
    pls.posW = tri.getPosition(barycentrics);
    pls.normal = tri.normal;

    //Get Flux
    float2 uv = tri.getTexCoord(barycentrics);
    pls.flux = gScene.materials.evalEmissive(tri.materialID, uv) * tri.area;
    

    //Check if the material is double sided (infinetly thin)
    MaterialHeader mltHeader = gScene.materials.getMaterialHeader(tri.materialID);
    //If the material is double sided, randomly flip the normal
    if (mltHeader.isDoubleSided())
    {
        if (sampleNext1D(sg) >= 0.5f)
            pls.normal = -tri.normal;
    }

    pls.flux /= triangleSelectionPdf;

    return true;
}
#endif

bool createLightSampleAnalytic(inout SampleGenerator sg, out PhotonLightSample pls, out float3 sampledDir)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return false;
    //Uniformly sample one of the analytic lights
    float rndLightIdx = float(lightCount) * sampleNext1D(sg);
    uint lightIdx = min(uint(floor(rndLightIdx)), lightCount - 1);
    LightData lightData = gScene.getLight(lightIdx);

    //Only Point(+ Spot) lights are supported (due to RTXDI)
    if (lightData.type != uint(LightType::Point))
        return false;
    float invLightPdf;
    float3 woLocal;
    float3 rnd = sampleNext3D(sg);
    rnd.z = 0;
    float spotAngle = lightData.cosOpeningAngle;
    if (lightData.openingAngle < M_2PI) //If the opening angle is smaller than 2PI it is a spotlight
    {
        spotAngle = cos(lightData.openingAngle - lightData.penumbraAngle * rnd.z);
        woLocal = sample_cone(rnd.xy, spotAngle);
    }
    else //Pointlight
    {
        woLocal = sample_sphere(rnd.xy);
    }
    invLightPdf = M_2PI * (1.f - spotAngle);
    
    if (invLightPdf <= 0.f)
        return false;

    invLightPdf *= lightCount; //Take the select pdf into account
    
    //Fill the photon light struct
    pls.posW = lightData.posW;
    pls.normal = lightData.dirW;
    pls.flux = lightData.intensity * invLightPdf;
    
    sampledDir = fromLocalToWorld(pls.normal, woLocal);
    return true;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    rayData.terminated |= kRayFlagTerminatedMiss;
}

[shader("miss")]
void shadowMiss(inout ShadowData shadowData : SV_RayPayload)
{
    shadowData.shadowed = false;
}

[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    const float3 incomingRayDir = -WorldRayDirection();
    // Evaluate Falcor's material parameters at the hit point.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, incomingRayDir, lod);

    //Adjust shading normals if desired

    uint hints = 0;
    if (gFlags & (uint) GenerationFlags::AdjShadingNormals)
    {
        hints |= (uint) MaterialInstanceHints::AdjustShadingNormal;
    }
    
    BSDFSample bsdfSample;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);
    bool hasDiffuseParts = (gFlags & (uint) GenerationFlags::DeltaRejReqDiff) ?
    luminance(bsdfProperties.diffuseReflectionAlbedo) > 0 : true;

    bool valid = bsdf.sample(sd, rayData.sg, bsdfSample, true /*ImportanceSampling*/);

    float3 thpStart = rayData.thp;
    
    bool transmission = bsdfSample.isLobe(LobeType::Transmission);
    float3 rayOrigin = sd.computeNewRayOrigin(!transmission); //false is for transmissions
    rayData.origin = rayOrigin;
    rayData.thp *= valid ? bsdfSample.weight : float3(0);
    rayData.packedNormal = encodeNormal2x16(sd.faceN);
    rayData.direction = bsdfSample.wo;
    rayData.isEmissive = luminance(bsdfProperties.emission) > 0;
    if (gFlags & (uint) GenerationFlags::DeltaRejection)
        rayData.diffuseHit = bsdfSample.isLobe(LobeType::NonDeltaReflection) && hasDiffuseParts;
    else
        rayData.diffuseHit = bsdfSample.isLobe(LobeType::Diffuse) && !transmission;
    
    //if throughput is 0, return
    if (!valid || any(rayData.thp <= 0))
        rayData.terminated |= kRayFlagTerminatedHit;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    //if (gFlags & (uint) GenerationFlags::AlphaTest)
    if (true)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void shadowAnyHit(inout ShadowData shadowData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    //if (gFlags & (uint) GenerationFlags::AlphaTest)
    if (true)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

     // Prepare ray payload.
    RayData rayData = RayData.create();
    rayData.sg = SampleGenerator(launchIndex, gFrameCount + 1 * 13);

    RayDesc ray;
    PhotonLightSample pls;

#if USE_EMISSIVE_LIGHT
    if (gFlags & (uint) GenerationFlags::AnalyticLight)
#endif
    {   //Analytic light sample
        float3 sampleDir;
        if (!createLightSampleAnalytic(rayData.sg, pls, sampleDir))
            return;
    
        ray.Direction = sampleDir;
    }
#if USE_EMISSIVE_LIGHT  
    else { //Emissive light sample
        if (!createLightSample(rayData.sg, pls))
            return;
    
        float lightDirPDF = 1.0;
        float3 woLocal = sample_cosine_hemisphere_concentric(sampleNext2D(rayData.sg), lightDirPDF);

        if (lightDirPDF <= 0.f)
            return;
    
        ray.Direction = fromLocalToWorld(pls.normal, woLocal);
        //pls.flux /= lightDirPDF;
        pls.flux *= M_PI;   //Lambert Emitter
    }
#endif    
    pls.flux /= launchDim.x * launchDim.y; //Division through all dipatched photons

    ray.Origin = pls.posW;
    ray.TMin = 0.001f;
    ray.TMax = 10000.f;
    
    //create photon
    float3 photonPos = float3(0);
    PhotonInfo photon;
    photon.dir = float3(0);
    photon.faceNTheta = 1.f;
    photon.flux = float3(0);
    photon.faceNPhi = 1.f;
    
    uint rayFlags = 0;
        
    //recursion where the photons are shot
    for (uint i = 0; i < gMaxRecursion && !rayData.terminated; i++)
    {        
        photon.flux = pls.flux * rayData.thp;
        TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

         //Stop if a light source was hit
        if (rayData.isEmissive || (rayData.terminated & kRayFlagTerminatedMiss > 0))
            break;

        photonPos = rayData.origin;
        photon.dir = ray.Direction;

        //Check if the point is visible for the camera
        float4 cameraPos = mul(gScene.camera.getViewProj(), float4(photonPos,1));
        cameraPos /= cameraPos.w;
        cameraPos.y *= -1;
        cameraPos.xy = cameraPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
        bool validReprojection = true;
        if(any(cameraPos.xy < 0) || any(cameraPos.xy > 1))
            validReprojection = false;

        bool reprojectionVisible = false;
        float3 pointToCamera = gScene.camera.getPosition() - photonPos;
        float distToCamera = length(pointToCamera);

        //Do a visibility test
        if(validReprojection){
           
            RayDesc shadowRay;
            shadowRay.Origin = photonPos;
            shadowRay.TMin = 0;
            shadowRay.TMax = distToCamera;
            shadowRay.Direction = pointToCamera / shadowRay.TMax;

            ShadowData shadowData;
            shadowData.shadowed = true;

            TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, shadowRay, shadowData);

            reprojectionVisible = !shadowData.shadowed;
        }

        //Store in AABB
        if(validReprojection && reprojectionVisible){
            uint photonIndex = 0;
            InterlockedAdd(gPhotonCounter[0], 1u, photonIndex);
            if (photonIndex <= kPhotonBufferSize)
            {
                float2 screenPos = floor(cameraPos.xy * float2(1920,1080)) + 0.5f;
                AABB photonAABB = AABB(float3(screenPos - 0.25, -0.25), float3(screenPos + 0.25, 0.25));
                gLightTraceAABB[photonIndex] = photonAABB;
                gLightTraceData[photonIndex] = float4(photon.flux * (distToCamera * distToCamera),1);
            }
        }

        //Russian Roulette
        const float rrVal = luminance(rayData.thp);
        const float prob = max(0.f, 1.f - rrVal);
        float rnd = sampleNext1D(rayData.sg);
        if (rnd < prob)
        {
            break; //Photon is absorbed
        }
        else
        {
            rayData.thp /= (1.f - prob);
        }
                        
        //update ray for next iteration
        ray.Origin = rayData.origin;
        ray.Direction = rayData.direction;
    }
    
}
