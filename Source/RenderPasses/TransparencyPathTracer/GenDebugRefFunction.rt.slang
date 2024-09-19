#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;

#define MAX_ELEMENTS 256

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
    uint2       gSelectedPixel; //Currently selected camera pixel
    float       gNear;          // SM near
    float       gFar;           // SM far
    uint        gSMRes;         // Shadow map resolution
    float3      gLightPos;      // World position of current light
    float4x4    gViewProj;      // View Projection for current light
    float4x4    gInvViewProj;   // Inverse view for current light
}

Texture2D<PackedHitInfo> gVBuffer;
RWBuffer<float> gFuncData;

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
    float4 posH = mul(gViewProj, float4(sd.posW, 1.f));
    rayData.depth = (posH.w - gNear) / (gFar - gNear); // Normalized depth
    rayData.dist = RayTCurrent();
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    if (any(DispatchRaysIndex().xy != 0))
        return;

    // Get current position
    HitInfo hit = HitInfo(gVBuffer[gSelectedPixel]);
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);

    float4 shadowPos = mul(gViewProj, float4(v.posW, 1));
    shadowPos.xyz /= shadowPos.w;

    shadowPos.y *= -1;                                    // Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5;              // UV from [-1,1] to [0,1]
    shadowPos.z = (shadowPos.w - gNear) / (gFar - gNear); // Linear distance

    uint2 smPixel = uint2(shadowPos.xy * gSMRes);

    float2 pixelSample = smPixel + 0.5;

    float2 relFramePos = pixelSample / gSMRes;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

    RayDesc ray;

    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toPoint = posWPixelNear.xyz - gLightPos;

    ray.TMin = length(toPoint);                                        // Real near for the pixel
    ray.TMax = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
    ray.Origin = gLightPos;
    ray.Direction = normalize(toPoint); // World direction

    RayData rayData = RayData();
    // Loop to get all possible samples
    for (uint i = 0; i < MAX_ELEMENTS; i++) {
        rayData.depth = 1;
        rayData.dist = 0;
        rayData.currentTransmittance = 0.0;

        //Trace ray
        uint rayFlags = 0;
        TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        uint index = i * 2;
        gFuncData[index] = rayData.depth;
        gFuncData[index + 1] = rayData.currentTransmittance; 
        ray.TMin = rayData.dist;

        if (rayData.currentTransmittance < kMinOpacity || rayData.dist < 0)
            break;

        rayData.totalTransmittance = rayData.currentTransmittance;
    }
}
