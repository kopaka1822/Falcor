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

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
    float2 gPhotonRadius; // x->Global Radius, y-> Caustic Radius
    uint _pad;
}

// Inputs
Buffer<uint> gLightCounter;
RWStructuredBuffer<AABB> gLightTraceAABB;
RWStructuredBuffer<float4> gLightTraceData;

// Outputs
RWTexture2D<float4> gColor;

//Acceleration Structure
RaytracingAccelerationStructure gLtAS;


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
    float4 L = gLightTraceData[primIndex];
            
    rayData.radiance += L.xyz;
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
    /*
    //Get start point and prim index
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();
    
    //Get Photon AABB. Instance Index 0 is caustic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gPhotonRadius.x : gPhotonRadius.y;
    
    //Check for Sphere intersection
    bool tHit = hitSphere(photonAABB.center(), radius, origin);
    */
    //Attribs are needed by the api. Because we have no need for additional attributes it is empty
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();
    AABB photonAABB = gLightTraceAABB[primIndex];

    SphereAttribs attribs;
    attribs.pad = float2(0);

    bool valid = true;
    valid &= primIndex<gLightCounter[0];
    valid &= photonAABB.contains(origin);

    //Report the hit if the point is in the sphere
    if (valid)
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

    float2 pixelCenter = float2(launchIndex) + 0.5;

    RayData rayData = RayData();

    //Ray description 
    RayDesc ray;
    ray.Origin = float3(pixelCenter, 0);
    ray.TMin = 0.0;
    ray.TMax = 0.001;
    ray.Direction = float3(0,0,1); //we take the view dir as direction to save payload space

    //Closest hit shader can be skipped
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;
    float3 radiance = float3(0);

    TraceRay(gLtAS, rayFlags, 0xFF /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
    float w = 1 / (M_PI * gPhotonRadius.x * gPhotonRadius.x);
    radiance += rayData.radiance;
      
    gColor[launchIndex] = float4(radiance, 1.f);


}
