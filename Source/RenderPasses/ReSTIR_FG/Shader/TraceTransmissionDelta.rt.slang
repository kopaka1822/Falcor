#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;


cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    uint gMaxBounces;       // Max number of bounces
    bool gRequDiffParts;
    bool gAlphaTest;
}

// Inputs
Texture2D<PackedHitInfo> gInVBuffer;

//Outputs
RWTexture2D<PackedHitInfo> gOutVBuffer;
RWTexture2D<float4> gOutViewDir;
RWTexture2D<float> gOutRayDist;
RWTexture2D<float4> gOutThp;
RWTexture2D<float4> gOutDiffuseReflectance;
RWTexture2D<float4> gOutSpecularReflectance;

#define is_valid(name) (is_valid_##name != 0)

static const float kDemodulateMinReflectance = 0.01f;
static const float kRoughnessCutoff = TRACE_TRANS_SPEC_ROUGH_CUTOFF;

struct TraceData
{
    PackedHitInfo packedHitInfo; //< Reflection albedo of the current hit point

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
    
    /** Create ray payload with default parameters.
    */
    static TraceData create()
    {
        TraceData d;
        d.packedHitInfo = {};
        return d;
    }
};

uint index2Dto1D(uint2 idx, uint width){
    return idx.x + width * idx.y;
}

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    HitInfo hitInfo = { };
    rayData.packedHitInfo = hitInfo.getData();  //Invalid hit
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (gAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("closesthit")]
void closestHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    float3 diffuseReflectance = { };
    float3 specularReflectance = { };
    
    HitInfo hitInfo = HitInfo(gInVBuffer[launchIndex]);
    float3 viewDir = -gScene.camera.computeRayPinhole(launchIndex, launchDim).dir;
    float rayDist = 0.f;
    float3 thp = float3(1);
    TraceData traceData = TraceData::create();
    traceData.sg = SampleGenerator(launchIndex, gFrameCount + 7 * 13);
    let lod = ExplicitLodTextureSampler(0.f);
    float3 originPos = gScene.camera.getPosition();  //For ray dist
    uint wasTracedMask = 0;
    
    for (uint i = 0; i <= gMaxBounces; i++)
    {
        //End loop if the scene is missed
        if (!hitInfo.isValid())
            break;

        ShadingData sd = loadShadingData(hitInfo, -viewDir, lod);
        rayDist = distance(originPos, sd.posW);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        //Write the reflectance on first hit
        if (i == 0 && (is_valid(gOutDiffuseReflectance) || is_valid(gOutDiffuseReflectance)))
        {
            diffuseReflectance = max(kDemodulateMinReflectance, bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo);
            // For specular an approximate shading model with a single specular lobe is used.
            const float NdotV = saturate(dot(bsdfProperties.guideNormal, sd.V));
            const float ggxAlpha = bsdfProperties.roughness * bsdfProperties.roughness;
            if(bsdfProperties.isTransmissive)
                specularReflectance = float3(1);
            else
            {
                specularReflectance = approxSpecularIntegralGGX(bsdfProperties.specularReflectionAlbedo, ggxAlpha, NdotV);
                specularReflectance = max(kDemodulateMinReflectance, specularReflectance);
            }
        }

        if (i == gMaxBounces)
            break;
        
        BSDFSample bsdfSample;

        if (!bsdf.sample(sd, traceData.sg, bsdfSample))
            break;

        bool isTransmission = bsdfSample.isLobe(LobeType::Transmission);
        bool hasDiffuseParts = gRequDiffParts ? any(bsdfProperties.diffuseReflectionAlbedo > 0) || bsdfProperties.roughness > kRoughnessCutoff : true;
        //bool isDiffuse = bsdfSample.isLobe(LobeType::Diffuse) && !isTransmission;
        bool isDiffuse = bsdfSample.isLobe(LobeType::NonDeltaReflection) && hasDiffuseParts;

        if(isDiffuse)
            break;
        
        thp *= bsdfSample.weight;   //Update troughput
                
        RayDesc tRay;
        tRay.Origin = sd.computeNewRayOrigin(!isTransmission);
        tRay.TMin = 0.0001f;
        tRay.TMax = 10000.f;
        tRay.Direction = bsdfSample.wo;

        TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, tRay, traceData);

        //Update info
        hitInfo = HitInfo(traceData.packedHitInfo);
        viewDir = -bsdfSample.wo;
        originPos = sd.posW;
        wasTracedMask = 1;
    }


    //Write
    gOutVBuffer[launchIndex] = hitInfo.getData();
    gOutViewDir[launchIndex] = float4(viewDir, 1);
    gOutRayDist[launchIndex] = rayDist;
    gOutThp[launchIndex] = float4(thp, wasTracedMask);
    if (is_valid(gOutDiffuseReflectance)) gOutDiffuseReflectance[launchIndex] = float4(diffuseReflectance, 1.f);
    if (is_valid(gOutSpecularReflectance)) gOutSpecularReflectance[launchIndex] = float4(specularReflectance, 1.f);
}
