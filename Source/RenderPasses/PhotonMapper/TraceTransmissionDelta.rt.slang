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

import Hash;
import PhotonData;

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    uint gMaxBounces;       // Max number of bounces
    bool gRequDiffParts;
    uint gHashSize; //Size is always 2^x
    float gCollectionRadius;
    float gHashScaleFactor; //Hash scale factor. 1/diameter
    bool gAlphaTest;
    uint _pad;
}

// Inputs
Texture2D<PackedHitInfo> gInVBuffer;

//Outputs
RWTexture2D<uint> gPhotonCullingMask;
RWTexture2D<PackedHitInfo> gOutVBuffer;
RWTexture2D<float4> gOutViewDir;
RWTexture2D<float4> gOutThp;

static const bool kUsePhotonCulling = USE_PHOTON_CULLING;

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

void cullingGetArrayOfCells(float3 position, float radius, out int3 outCells[8])
{
    uint startIdx = 0;
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

void photonCulling(float3 hitPos)
{
    //Get the 7 nearest neighbors (photon can still be in it)
    int3 cells[8];
    cullingGetArrayOfCells(hitPos, gCollectionRadius, cells);

    //Insert them into the hash buffer
    [unroll]
    for (uint i = 0; i < 8; i++)
    {
        uint h = hash(cells[i]) & (gHashSize - 1); //calculateHash TODO: Test other hashes
        gPhotonCullingMask[PM_LinearIndexToZCurve(h)] = 1;
    }
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
    
    for (uint i = 0; i < gMaxBounces; i++)
    {
        //End loop if the scene is missed
        if (!hitInfo.isValid())
            break;

        ShadingData sd = loadShadingData(hitInfo, -viewDir, lod);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
                
        BSDFSample bsdfSample;

        if (!bsdf.sample(sd, traceData.sg, bsdfSample))
            break;

        bool isTransmission = bsdfSample.isLobe(LobeType::Transmission);
        bool hasDiffuseParts = gRequDiffParts ?  luminance(bsdfProperties.diffuseReflectionAlbedo) > 0 : true;
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
    }

    //Culling
    if (hitInfo.isValid() && kUsePhotonCulling)
    {
        ShadingData sd = loadShadingData(hitInfo, -viewDir, lod);
        photonCulling(sd.posW);
    }

    //Write
    gOutVBuffer[launchIndex] = hitInfo.getData();
    gOutViewDir[launchIndex] = float4(viewDir, 1);
    gOutThp[launchIndex] = float4(thp, 1);
}
