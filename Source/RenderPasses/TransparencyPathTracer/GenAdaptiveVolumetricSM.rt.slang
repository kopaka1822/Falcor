#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;

#ifndef AVSM_K
#define AVSM_K 4
#endif

#define AVSM_NUM_TEX AVSM_K/4

/** Payload for shadow ray.
*/
struct RayData
{
    float depths[AVSM_K];
    float transmittances[AVSM_K];

    __init() {
        for (uint i = 0; i < AVSM_K; i++) {
            depths[i] = -1.0;
            transmittances[i] = 1;
        }
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

RWTexture2D<float4> gAVSMDepths[AVSM_NUM_TEX];
RWTexture2D<float4> gAVSMTransmittance[AVSM_NUM_TEX];

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

//Area of a triangle with three points as input
float areaTriangle(const float2 A,const float2 B,const float2 C) {
    float2 b = B - A;
    float2 c = C - A;
    float dotBC = dot(b, c);
    float area = sqrt(dot(b, b) * dot(c, c) - (dotBC * dotBC)); // Area parallelogram
    return area * 0.5; 
}

//Shaders
[shader("miss")]
void miss(inout RayData rayData)
{
    
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

    //TODO special case opacity = 1

    // Abort if opacity is too low
    if (sd.opacity < kMinOpacity) {
        IgnoreHit();
        return;
    }
    
    //Get depth
    float4 posH = mul(gViewProj, float4(sd.posW, 1.f));
    float depth = (posH.w - gNear) / (gFar - gNear); // Normalized depth
    const float transmittance = (1.0 - sd.opacity);
    float currentTransmittance = transmittance;

    //Add bias to depth
    float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
    depth = saturate(depth + kDepthBias + normalBias);

    float tmpDepths[AVSM_K + 1];
    float tmpTransmittance[AVSM_K + 1];
    float lastTransmittance = 1.0;
    bool currentWasSwapped = false;

    for (int i = 0; i < AVSM_K; i++) {
        if (lastTransmittance <= kMinOpacity)
        {
            tmpDepths[i] = -1.0;
            tmpTransmittance[i] = 0;
        }else if (currentWasSwapped)
        {
            tmpDepths[i] = rayData.depths[i - 1];
            tmpTransmittance[i] = rayData.transmittances[i - 1] * transmittance;
            lastTransmittance = tmpTransmittance[i];
        } else
        {
            if (depth < rayData.depths[i] || rayData.depths[i] < 0) {
                tmpDepths[i] = depth;
                currentTransmittance = transmittance * lastTransmittance;
                tmpTransmittance[i] = currentTransmittance;
                currentWasSwapped = true;
            } else {
                tmpDepths[i] = rayData.depths[i];
                tmpTransmittance[i] = rayData.transmittances[i];
                lastTransmittance = rayData.transmittances[i];
            }
        }
    }
    // Last Element
    if (lastTransmittance <= kMinOpacity)
    {
        tmpDepths[AVSM_K] = -1.0;
        tmpTransmittance[AVSM_K] = 0;
    }
    else if (currentWasSwapped) {
        tmpDepths[AVSM_K] = rayData.depths[AVSM_K - 1];
        tmpTransmittance[AVSM_K] = rayData.transmittances[AVSM_K - 1] * transmittance;
    } else {
        tmpDepths[AVSM_K] = depth;
        tmpTransmittance[AVSM_K] = lastTransmittance * transmittance;
    }

    // Check if last point is invalid or transmittance is zero
    uint removeIndex = AVSM_K;
    //Check which element has the smalles tri area
    if (tmpDepths[AVSM_K] >= 0) {
        float minArea = 1.0; //1 should be max possible area
        [unroll]
        for (uint i = 1; i < AVSM_K; i++) {
            float triArea = areaTriangle(float2(tmpDepths[i - 1], tmpTransmittance[i - 1]), float2(tmpDepths[i], tmpTransmittance[i]), float2(tmpDepths[i + 1], tmpTransmittance[i + 1]));
            if (triArea < minArea) {
                minArea = triArea;
                removeIndex = i;
            }
        }
    }

    // Copy to payload
    for (uint i = 0; i < AVSM_K; i++) {
        uint idx = i < removeIndex ? i : i + 1;
        rayData.depths[i] = tmpDepths[idx];
        rayData.transmittances[i] = tmpTransmittance[idx];
    }

    //Commit RayTMax if current element has 0 transmittance
    if (transmittance <= kMinOpacity)
        return;
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

    uint flags = RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object
    //uint flags = RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object
    TraceRay(gScene.rtAccel, flags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    // Fill undefined points
    float fillDepth = 1;
    float fillTransmittance = 1;
    [unroll]
    for (uint i = 0; i < AVSM_K; i++) {
        if (rayData.depths[i] < 0) {
            rayData.depths[i] = fillDepth;
            rayData.transmittances[i] = fillTransmittance;
        }
        fillDepth = rayData.depths[i];
        fillTransmittance = rayData.transmittances[i];
    }

    // Write to textures
    for (uint i = 0; i < AVSM_NUM_TEX; i++) {
        uint payIdx = i * 4;
        float4 depths = float4(rayData.depths[payIdx], rayData.depths[payIdx + 1], rayData.depths[payIdx + 2], rayData.depths[payIdx + 3]);
        gAVSMDepths[i][pixel] = depths;
        float4 transmittance = float4(rayData.transmittances[payIdx], rayData.transmittances[payIdx + 1], rayData.transmittances[payIdx + 2], rayData.transmittances[payIdx + 3]);
        gAVSMTransmittance[i][pixel] = transmittance;
    }
}
