#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;

#ifndef MAX_ELEMENTS
#define MAX_ELEMENTS 256
#endif

#ifndef NUM_LIGHTS
#define NUM_LIGHTS 1
#endif

#define MAX_IDX 512 * 512 * 16

/** Payload for shadow ray.
*/
struct RayData
{
    float dist;
    float depth;
    float currentTransmittance;
    float totalTransmittance;


    __init() {
        this.dist = -1.0;
        this.depth = 1.0;
        this.currentTransmittance = 1.0;
        this.totalTransmittance = 1.0;
    }
};

cbuffer CB
{
    uint gFrameCount; // Current frame count
    float3 gLightPos; // Current Light Position
    float gNear;
    float gFar;
    uint2 _pad;
    float4x4 gViewProj;    // View Projection for current light
    float4x4 gInvViewProj; // Inverse view projection
    float4x4 gView;
}

RWStructuredBuffer<AABB> gAABB;
RWBuffer<uint> gCounter;
RWStructuredBuffer<float> gData;

static const float kDepthBias = AVSM_DEPTH_BIAS;
static const float kNormalBias = AVSM_NORMAL_DEPTH_BIAS;
static const float kMinOpacity = 1e-7;

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    return sd;
}

//Shaders
[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.dist = -1.0;
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f, kMinOpacity)) IgnoreHit();
}


[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -WorldRayDirection(), lod);

    rayData.currentTransmittance = rayData.totalTransmittance * (1.0 - sd.opacity);
    // float4 posH = mul(gViewProj, float4(sd.posW, 1.f));
    //  rayData.depth = posH.w; // Linear depth
    // float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
    // rayData.depth = rayData.depth + (kDepthBias * gFar) + (normalBias * gFar);
    rayData.depth = -1.0 * mul(gView, float4(sd.posW, 1.f)).z;
    //float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
    //rayData.depth = rayData.depth + (kDepthBias * gFar) + (normalBias * gFar);

     //Add bias to depth
    
    rayData.dist = RayTCurrent();
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    float2 pixelSample = pixel + 0.5; //+ kMSAAPattern[gFrameCount % MSAA_SAMPLES];

    float2 relFramePos = pixelSample / frameDim;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toNear = posWPixelNear.xyz - gLightPos;

    RayDesc ray;
    ray.TMin = length(toNear); ;                                                                  // Always start at depth 0
    ray.TMax = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
    ray.Origin = gLightPos;
    ray.Direction = normalize(toNear); // World direction

    RayData rayData = RayData();
    bool useRayTracing = true;
    float lastMinDepth = 0.0;
    // Loop to get all possible samples
    for (uint i = 0; (i < MAX_ELEMENTS) && useRayTracing ; i++) {
        rayData.depth = -1;
        rayData.dist = -1;
        rayData.currentTransmittance = 0.0;

        //Trace ray
        uint rayFlags = 0;
        TraceRay(gScene.rtAccel, rayFlags, kShadowRayInstanceMask /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        // A miss or opaque object ends the ray
        if (i == 0 && rayData.dist < 0)
            return;

        if (rayData.currentTransmittance <= kMinOpacity || rayData.dist < 0) {
            useRayTracing = false;
        }

        if (rayData.dist > 0 && i > 0) {
            // Write output
            float3 minPoint = float3(pixel.x + 0.1, pixel.y + 0.1, lastMinDepth);
            float3 maxPoint = float3(pixel.x + 0.9, pixel.y + 0.9, rayData.depth);

            uint insertIdx = 0;
            InterlockedAdd(gCounter[0], 1u, insertIdx);
            if (insertIdx > MAX_IDX)
                return;

            gAABB[insertIdx] = AABB(minPoint, maxPoint);
            gData[insertIdx] = rayData.totalTransmittance; // Transmittance last sample

        }
        // Update for next iteration
        if (rayData.dist > 0) {
            ray.TMin = rayData.dist;
            lastMinDepth = rayData.depth;
            rayData.totalTransmittance = rayData.currentTransmittance;
        }
           
    }

    // Insert last element
    if (lastMinDepth < gFar) {
        float3 minPoint = float3(pixel.x + 0.1, pixel.y + 0.1, lastMinDepth);
        float3 maxPoint = float3(pixel.x + 0.9, pixel.y + 0.9, gFar);

        uint insertIdx = 0;
        InterlockedAdd(gCounter[0], 1u, insertIdx);
        if (insertIdx > MAX_IDX)
            return;

        gAABB[insertIdx] = AABB(minPoint, maxPoint);
        gData[insertIdx] = rayData.totalTransmittance; // Transmittance last sample
    }
}
