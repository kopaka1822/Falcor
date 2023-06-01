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

import Reservoir;
import FinalGatherData;
import SurfaceData;
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
RWStructuredBuffer<PackedSurfaceData> gSurfaceData;
RWTexture2D<PackedHitInfo> gFinalGatherHit;
RWTexture2D<uint> gPhotonCullingMask;

//Outputs
#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif

static const bool kUsePhotonCulling = USE_PHOTON_CULLING;

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

#if CULLING_ALT_VERSION
//Alternative Version to fill the culling grid. Only marks the cells 
void cullingGetArrayOfCells(float3 position, float radius, inout uint arrCounter, out int3 outCells[16])
{
    float3 cell = (position.xyz * gHashScaleFactor);
    float radInScale = radius * gHashScaleFactor;
    float3 cellFloor = floor(cell);
    int3 startCell = int3(cellFloor);
    float3 relCellPos = abs(cell - cellFloor);
    //Get offset direction
    int3 offsetCell;
    offsetCell.x = relCellPos.x < 0.5 ? -1 : 1;
    offsetCell.y = relCellPos.y < 0.5 ? -1 : 1;
    offsetCell.z = relCellPos.z < 0.5 ? -1 : 1;

    const int3 cellPattern[7] = { int3(1, 0, 0), int3(0, 1, 0), int3(0, 0, 1), int3(1, 1, 0), int3(1, 0, 1), int3(0, 1, 1), int3(1, 1, 1) };
    int3 cellSet = int3(0);
    
    //Fill the cell array
    outCells[arrCounter] = startCell; // Cell 0
    arrCounter++;

    //Go through the first 3 cells (either x,y or z set)
    [unroll]
    for (uint i = 0; i < 3; i++)
    {
        int3 tmpCell = int3(floor(cell + radInScale * float3(offsetCell * cellPattern[i])));
        bool skip = (tmpCell.x == startCell.x) && (tmpCell.y == startCell.y) && (tmpCell.z == startCell.z);
        if (!skip)
        {
            outCells[arrCounter] = tmpCell;
            arrCounter++;
            cellSet[i] = 1;
        }
    }
    //Go through cell 3-6 (two axis set)
    [unroll]
    for (uint i = 3; i < 6; i++)
    {
        int3 shouldBeSet = cellSet * cellPattern[i];
        if (shouldBeSet.x + shouldBeSet.y + shouldBeSet.z >= 2)
        {
            outCells[arrCounter] = int3(floor(cell + radInScale * float3(offsetCell * cellPattern[i])));
            arrCounter++;
        }
    }
    //Case all 3 axis set
    if (cellSet.x + cellSet.y + cellSet.z == 3)
    {
        outCells[arrCounter] = int3(floor(cell + radInScale * float3(offsetCell * cellPattern[6])));
        arrCounter++;
    }
}

//Culling with Global and Caustic collection pos. hitPos1 -> Global, hitPos2 -> Caustic, 
void photonCulling(float3 hitPos1, float3 hitPos2)
{
    uint counter = 0;
    int3 cells[16]; //Possible cells
    cullingGetArrayOfCells(hitPos1, gCollectionRadius.x, counter, cells);
    cullingGetArrayOfCells(hitPos2, gCollectionRadius.y, counter, cells);


    uint hashes[16];
    [unroll]
    for (uint i = 0; i < 16; i++)
    {
        if (i < counter) //Abort if we have written all cells
        {
            hashes[i] = hash(cells[i]) & (gHashSize - 1); //calculateHash TODO: Test other hashes
        }
    }
    //Insert them into the hash buffer
    [unroll]
    for (uint i = 0; i < 16; i++)
    {
        if (i < counter) //Abort if we have written all cells
        {
            gPhotonCullingMask[ReSTIR_LinearIndexToZCurve(hashes[i])] = 1;
        }
    }
}
#else //CULLING_ALT_VER
void cullingGetArrayOfCells(float3 position, float radius, uint startIdx , out int3 outCells[16]){
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
    outCells[startIdx] = int3(cellFloor);
    outCells[startIdx + 1] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, 0)));
    outCells[startIdx + 2] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, 0)));
    outCells[startIdx + 3] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, offsetCell.z)));
    outCells[startIdx + 4] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, offsetCell.z)));
    outCells[startIdx + 5] = int3(floor(cell + radInScale * float3(0, offsetCell.y, 0)));
    outCells[startIdx + 6] = int3(floor(cell + radInScale * float3(0, offsetCell.y, offsetCell.z)));
    outCells[startIdx + 7] = int3(floor(cell + radInScale * float3(0, 0, offsetCell.z)));
}

void photonCulling(float3 hitPos1, float3 hitPos2){
    //Get the 7 nearest neighbors (photon can still be in it)
    int3 cells[16];
    cullingGetArrayOfCells(hitPos1, gCollectionRadius.x, 0, cells);
    cullingGetArrayOfCells(hitPos2, gCollectionRadius.y, 8, cells);

    //Insert them into the hash buffer
    [unroll]
    for(uint i=0; i<16; i++){
        uint h = hash(cells[i]) & (gHashSize - 1);    //calculateHash TODO: Test other hashes
        gPhotonCullingMask[ReSTIR_LinearIndexToZCurve(h)] = 1;
    }
}
#endif//CULLING_ALT_VER

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
        rayData.packedNormal = encodeNormal2x16(sd.frame.N);
        rayData.packedHitInfo = HitInfo(triangleHit).getData();
        rayData.NdotL = saturate(dot(sd.frame.N, incomingRayDir));
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
    Surface surface = Surface();
    Reservoir reservoir = Reservoir();
    TraceData traceData = TraceData::create();
    
    if (hitInfo.isValid())
    {
        float3 view = -gView[launchIndex].xyz;
        float linZ = gLinZ[launchIndex];
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

        //Fill Surface Buffer used for calculating the target PDF
        surface = Surface::create(sd.computeNewRayOrigin(), bsdfProperties.guideNormal, sd.V, linZ, bsdfProperties.roughness,
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
                photonCulling(hitPos, sd.posW);
            }
        }

    
        if (!validSample)
        {
            traceData.packedHitInfo = { };
            reservoir.weightSum = 0.f; //invalidate reseroir
        }
    }
        
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
