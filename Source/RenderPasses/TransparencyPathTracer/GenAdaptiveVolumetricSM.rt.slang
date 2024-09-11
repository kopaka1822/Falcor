#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;


/** Payload for shadow ray.
*/
struct RayData
{
    float4 depths;
    float4 transmittances;
    float lastDepth;
    float lastTransmittance;
    uint count;

    __init() {
        this.depths = float4(-1);
        this.transmittances = float4(1);
        this.lastDepth = -1;
        this.lastTransmittance = 1;
        this.count = 0;
    }
};

cbuffer CB
{
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    uint3       _pad;
    float4x4    gViewProj;      // View Projection for current light
    float4x4    gInvViewProj;   // Inverse view projection
}

RWTexture2D<float4> gAVSMDepths;
RWTexture2D<float4> gAVSMTransparency;
RWTexture2D<float2> gAVSMLast;

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
    if (rayData.count == 0) {
        rayData.lastDepth = 1;
        rayData.lastTransmittance = 1;
    }
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Get shading data
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -WorldRayDirection(), lod);

    // Abort if opacity is too low
    if (sd.opacity < kMinOpacity) {
        IgnoreHit();
        return;
    }

    float currentTransmittance = (1.0  - sd.opacity);
    
    //Get depth
    float4 posH = mul(gViewProj, float4(sd.posW, 1.f));
    float depth = (posH.w - gNear) / (gFar - gNear); // Normalized depth
    float transmittance = currentTransmittance;

    //If there is no previous element
    if (rayData.count == 0) {
        rayData.lastDepth = depth;
        rayData.lastTransmittance = currentTransmittance;
        rayData.count++;
    } // Count bigger than 0
    else if (rayData.count < 5) {
        // Check if it is the new last element
        if (depth > rayData.lastDepth) {
            rayData.depths[rayData.count - 1] = rayData.lastDepth;
            rayData.transmittances[rayData.count - 1] = rayData.lastTransmittance;
            rayData.lastDepth = depth;
            rayData.lastTransmittance *= currentTransmittance;
        } else {
            // Find index to be inserted
            uint insertIndex = 3;
            for (uint i = 0; i < 3; i++) {
                if (depth < rayData.depths[i] || rayData.depths[i] < 0) {
                    insertIndex = i;
                    break;
                }
            }
            // Create new list
            float depthTmp = depth;
            float transmittanceTmp = insertIndex > 0 ? rayData.transmittances[insertIndex - 1] * currentTransmittance : currentTransmittance;
            for (uint i = insertIndex; i < 4; i++) {
                float2 tmp = float2(rayData.depths[i], rayData.transmittances[i]);
                rayData.depths[i] = depthTmp;
                rayData.transmittances[i] = transmittanceTmp;
                depthTmp = tmp.x;
                transmittanceTmp = tmp.y * currentTransmittance;
            }
            rayData.lastTransmittance *= currentTransmittance; //Update last element
        }

        rayData.count++;
    } else {// More than 5 elements
        rayData.lastTransmittance *= currentTransmittance; // TODO remove
    }

    // End search
    if (rayData.lastTransmittance < kMinOpacity)
        AcceptHitAndEndSearch();
    else
        IgnoreHit();
}

/*
[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    float3 posW = WorldRayOrigin() + WorldRayDirection() * RayTCurrent(); //Get the current world positon
    float4 posH = mul(gViewProj, float4(posW, 1.f));
    if(gCalcNearFar)
        rayData.depth = posH.w;
    else
        rayData.depth = (posH.w - gNear) / (gFar - gNear);   //Linear depth in [0,1]
}
*/
//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    float2 pixelSample = pixel + 0.5;

    float2 relFramePos = pixelSample / frameDim;
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

    uint flags = RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH; //Call anyHit for every type of object
    TraceRay(gScene.rtAccel, flags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    // Convert opacity to transparency
    // TODO
    // Write to textures
    gAVSMDepths[pixel] = rayData.depths;
    gAVSMTransparency[pixel] = rayData.transmittances;
    gAVSMLast[pixel] = float2(rayData.lastDepth, rayData.lastTransmittance);
}
