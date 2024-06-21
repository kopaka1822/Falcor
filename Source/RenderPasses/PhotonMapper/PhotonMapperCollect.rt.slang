#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;

import PhotonData;

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
    float2 gPhotonRadius; // x->Global Radius, y-> Caustic Radius
    uint _pad;
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
StructuredBuffer<AABB> gPhotonAABB[2];
StructuredBuffer<PhotonDataPacked> gPackedPhotonData[2];
Texture2D<PackedHitInfo> gVBufferFirstHit;
Texture2D<float4> gThp;

// Outputs
RWTexture2D<float4> gColor;

//Acceleration Structure
RaytracingAccelerationStructure gPhotonAS;


/** Payload for ray (48B).
*/
struct RayData
{
    float3 radiance; ///< Accumulated outgoing radiance from path.
    bool _pad; ///< free space.
    PackedHitInfo packedHitInfo; ///< Hit info from vBuffer; Up to 16B

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.radiance = float3(0);
        this._pad = false;
    }
};

struct SphereAttribs
{
    float2 pad;
};

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

uint index2Dto1D(uint2 idx, uint width)
{
    return idx.x + width * idx.y;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}


[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    //Get Photon data
    const uint primIndex = PrimitiveIndex();
    PhotonData pd = PhotonData(gPackedPhotonData[InstanceIndex()][primIndex]);
            
    //Get hit data from payload
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    let lod = ExplicitLodTextureSampler(0.f);
    //World Direction is set to the viewDir we get from the vBuffer
    ShadingData sd = loadShadingData(hit, WorldRayDirection(), lod);
    //Check for different surfaces
    
    if (dot(pd.normal, sd.faceN) < 0.6)
    {
        return;
    }
    
    //Photon contribution
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);
    float3 f_r = bsdf.eval(sd, pd.dir, rayData.sg); //right sign?
    float NdotL = saturate(dot(bsdfProperties.guideNormal, pd.dir));
    if (NdotL > 0)
        rayData.radiance += max(0.f, (f_r * pd.flux) / NdotL);  //Take out the NdotL that is included in the eval function
}

//Checks if the ray start point is inside the sphere.
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    radiusTest = radiusTest * radiusTest;
    float radiusTestF = radiusTest.x + radiusTest.y + radiusTest.z;
    if (radiusTestF < radius * radius)
        return true;
    return false;
}

[shader("intersection")]
void intersection()
{
    //Get start point and prim index
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();
    
    //Get Photon AABB. Instance Index 0 is caustic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gPhotonRadius.x : gPhotonRadius.y;
    
    //Check for Sphere intersection
    bool tHit = hitSphere(photonAABB.center(), radius, origin);

    //Attribs are needed by the api. Because we have no need for additional attributes it is empty
    SphereAttribs attribs;
    attribs.pad = float2(0);

    //Report the hit if the point is in the sphere
    if (tHit)
    {
        ReportHit(RayTCurrent(), 0, attribs);
    }
}


[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    let lod = ExplicitLodTextureSampler(0.f);

    //Prepare payload
    RayData rayData = RayData();
    rayData.packedHitInfo = gVBuffer[launchIndex];
    rayData.sg = SampleGenerator(launchIndex, gFrameCount + 3 * 12);

    HitInfo hit = HitInfo(rayData.packedHitInfo);
    bool valid = hit.isValid(); //Check if the ray is valid
    
    //Get vertex data for the world position
    TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    float3 view = -gView[launchIndex].xyz;
    
    //Ray description 
    RayDesc ray;
    ray.Origin = v.posW;
    ray.TMin = 0.0;
    ray.TMax = 0.001;
    ray.Direction = view; //we take the view dir as direction to save payload space

    //Closest hit shader can be skipped
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;
    float3 radiance = float3(0);
    
    //Collect Caustic Photon
    if (valid)
    {
        TraceRay(gPhotonAS, rayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float w = 1 / (M_PI * gPhotonRadius.x * gPhotonRadius.x);
        radiance += w * rayData.radiance;
        
        rayData.radiance = float3(0);   //Clear Radiance
        TraceRay(gPhotonAS, rayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        w = 1 / (M_PI * gPhotonRadius.y * gPhotonRadius.y);
        radiance += rayData.radiance * w;
    }

    float3 thp = gThp[launchIndex].xyz;
    ShadingData sd = loadShadingData(hit, view, lod);
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);

    radiance += bsdfProperties.emission;

    radiance *= thp;

    gColor[launchIndex] = float4(radiance, 1.f);


}
