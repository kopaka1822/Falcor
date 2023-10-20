#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Materials.Microfacet;
import Rendering.Materials.Fresnel;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;

import Reservoir;
import FinalGatherData;
import SurfaceDataFG;
import Hash;

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    float2 gCollectionRadius;
    float gHashScaleFactor; //Hash scale factor. 1/diameter
    float gAttenuationRadius;
    uint3 _pad;
}

cbuffer Constant{
    uint gHashSize;     //Size is always 2^x
    bool gUseAlphaTest; //Alpha test
    bool gDeltaRejection;   //Accept every non delta surface as diffuse
    //bool gCreateFallbackSample; //Creates a reference sample if current sample was invalid
    uint _pad2;
}  

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D <float>gLinZ;

//Outputs
RWStructuredBuffer<PackedSurfaceDataFG> gSurfaceData;
RWTexture2D<PackedHitInfo> gFinalGatherHit;
RWTexture2D<uint> gPhotonCullingMask;

//Outputs
#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif

static const bool kUsePhotonCulling = USE_PHOTON_CULLING;
static const bool kUseCausticPhotons = USE_CAUSTIC_CULLING; 

struct TraceData
{
    PackedHitInfo packedHitInfo; //< Reflection albedo of the current hit point
    uint packedNormal; ///< Face normal encoded in 16 bit polar coordinates
    float distance; ///< 
    float NdotL; ///< NdotL precalculated for targetPDF
    uint _pad;

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
    
    /** Create ray payload with default parameters.
    */
    static TraceData create()
    {
        TraceData d;
        d.NdotL = 0.f;
        d.packedHitInfo = {
        };
        d.packedNormal = 0;
        d.distance = -1;
        d._pad = 0;
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

void cullingGetArrayOfCells(float3 position, float radius, out int3 outCells[8]){
    float3 cell = (position.xyz * gHashScaleFactor);
    float radInScale = radius * gHashScaleFactor;
    float3 cellFloor = floor(cell);
    float3 relCellPos = abs(cell - cellFloor);
    //Get offset direction
    int3 offsetCell;
    offsetCell.x = relCellPos.x < 0.5 ? -1 : 1;
    offsetCell.y = relCellPos.y < 0.5 ? -1 : 1;
    offsetCell.z = relCellPos.z < 0.5 ? -1 : 1;

    //Fill the cell array
    outCells[0] = int3(cellFloor);
    outCells[1] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, 0)));
    outCells[2] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, 0)));
    outCells[3] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, offsetCell.z)));
    outCells[4] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, offsetCell.z)));
    outCells[5] = int3(floor(cell + radInScale * float3(0, offsetCell.y, 0)));
    outCells[6] = int3(floor(cell + radInScale * float3(0, offsetCell.y, offsetCell.z)));
    outCells[7] = int3(floor(cell + radInScale * float3(0, 0, offsetCell.z)));
}

void photonCulling(float3 hitPos, float radius){
    //Get the 7 nearest neighbors (photon can still be in it)
    int3 cells[8];
    cullingGetArrayOfCells(hitPos, radius, cells);

    //Insert them into the hash buffer
    [unroll]
    for(uint i=0; i<8; i++){
        uint h = hash(cells[i]) & (gHashSize - 1);    //calculateHash TODO: Test other hashes
        gPhotonCullingMask[ReSTIR_LinearIndexToZCurve(h)] = 1;
    }
}

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    rayData.distance = -1.f;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (gUseAlphaTest)
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
    const float3 incomingRayDir = -WorldRayDirection();
    // Evaluate Falcor's material parameters at the hit point.
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    VertexData v = getVertexData(triangleHit.instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, incomingRayDir, lod);

    //TODO Add Alternative. Sample a new direction using BSDF and take reflective lobe
    
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    
    let bsdfProperties = bsdf.getProperties(sd);
    bool diffuseHit = luminance(bsdfProperties.diffuseReflectionAlbedo) > 0;
    diffuseHit &= luminance(bsdfProperties.emission) <= 0; //Ignore emissive hits, as they are sampled by ReSTIR
    
    if(diffuseHit){
        rayData.distance = RayTCurrent();
        rayData.packedNormal = encodeNormal2x16(bsdfProperties.guideNormal);
        rayData.packedHitInfo = HitInfo(triangleHit).getData();
        rayData.NdotL = saturate(dot(bsdfProperties.guideNormal, incomingRayDir));
    }else{
        rayData.distance = -1.f;
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    HitInfo hitInfo = HitInfo(gVBuffer[launchIndex]);
    SurfaceFG surface = SurfaceFG();
    Reservoir reservoir = Reservoir();
    TraceData traceData = TraceData::create();
    
    if (hitInfo.isValid())
    {
        float3 view = -gView[launchIndex].xyz;
        float viewDepth = gLinZ[launchIndex];
        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hitInfo, view, lod);
        
        //Use only BRDF
        sd.mtl.setActiveLobes((uint) LobeType::Reflection);

        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

        traceData.sg = SampleGenerator(launchIndex, gFrameCount + 3 * 13);

        BSDFSample bsdfSample;
        bool validSample = bsdf.sample(sd, traceData.sg, bsdfSample);

        //Fill the RTXDI surface if it is enabled
    #if USE_RTXDI
        gRTXDI.setSurfaceData(launchIndex, sd.computeNewRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness, viewDepth);
    #endif
        
        //Fill Surface Buffer used for calculating the target PDF
        surface = SurfaceFG::create(sd.computeNewRayOrigin(), bsdfProperties.guideNormal, sd.V, viewDepth, bsdfProperties.roughness,
                                      luminance(bsdfProperties.diffuseReflectionAlbedo), luminance(bsdfProperties.specularReflectance));
        
        reservoir.targetPdf = surface.getPdf(bsdfSample.wo);
    
        reservoir.weightSum = 1.f / bsdfSample.pdf; // 1/selectPDF

        RayDesc tRay;
        tRay.Origin = sd.computeNewRayOrigin();
        tRay.TMin = 0.0001f;
        tRay.TMax = 10000.f;
        tRay.Direction = bsdfSample.wo;
    
        if (validSample)
        {
            TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, tRay, traceData);
            validSample = traceData.distance > 0;
        }
    
        if (validSample)
        {                      
            reservoir.M = 1; 
            float3 hitPos = tRay.Origin + tRay.Direction * traceData.distance;
    
            if (kUsePhotonCulling)
            {
                photonCulling(hitPos,gCollectionRadius.x);
                if(kUseCausticPhotons)
                    photonCulling(sd.posW, gCollectionRadius.y);
            }
        }

    
        if (!validSample)
        {
            traceData.packedHitInfo = { };
            reservoir.weightSum = 0.f; //invalidate reseroir
        }
    }
#if USE_RTXDI
    else
    {
        gRTXDI.setInvalidSurfaceData(launchIndex);
    }
#endif //USE_RTXDI
    
    //Store data in buffers
    //gLightFactor[launchIndex] = lightFactor;
    gFinalGatherHit[launchIndex] = traceData.packedHitInfo;
    gSurfaceData[index2Dto1D(launchIndex, launchDim.x)] = surface.pack();   //TODO better index
    
    #if USE_REDUCED_RESERVOIR_FORMAT
        gReservoir[launchIndex] = reservoir.packReduced();
    #else
        gReservoir[launchIndex] = reservoir.pack();
    #endif
}
