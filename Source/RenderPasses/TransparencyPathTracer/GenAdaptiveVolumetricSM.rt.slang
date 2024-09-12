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

    //TODO special case opacity = 1

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

    // add bias to depth
    float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
    depth = saturate(depth + kDepthBias + normalBias);

    //If there is no previous element
    if (rayData.count == 0) {
        rayData.lastDepth = depth;
        rayData.lastTransmittance = currentTransmittance;
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
            for (uint i = insertIndex; i < rayData.count; i++) {
                float2 tmp = float2(rayData.depths[i], rayData.transmittances[i]);
                rayData.depths[i] = depthTmp;
                rayData.transmittances[i] = transmittanceTmp;
                depthTmp = tmp.x;
                transmittanceTmp = tmp.y * currentTransmittance;
            }
            rayData.lastTransmittance *= currentTransmittance; //Update last element
        }
    } else {// More than 5 elements
        // Find index to be inserted
        uint insertIndex = 4;
        for (uint i = 0; i < 4; i++) {
            if (depth < rayData.depths[i]) {
                insertIndex = i;
                break;
            }
        }
        // Create a list with 6 elements
        float tmpDepths[6];
        float tmpTransparencies[6];
        // Fill the list with the existing and current element and update transmittance; TODO optimize 
        bool currentElementAdded = false;
        for (int i = 0; i < insertIndex; i++) {
            tmpDepths[i] = rayData.depths[i];
            tmpTransparencies[i] = rayData.transmittances[i];
        }
        float depthTmp = depth;
        float transmittanceTmp = insertIndex > 0 ? rayData.transmittances[insertIndex - 1] * currentTransmittance : currentTransmittance;
        for (uint i = insertIndex; i < 5; i++) {
            float2 tmp = float2(rayData.depths[i], rayData.transmittances[i]);
            tmpDepths[i] = depthTmp;
            tmpTransparencies[i] = transmittanceTmp;
            depthTmp = tmp.x;
            transmittanceTmp = tmp.y * currentTransmittance;
        }
        // Check last element
        tmpTransparencies[5] = rayData.lastTransmittance * currentTransmittance;
        if (depth > rayData.lastDepth) { // Swap
            tmpDepths[5] = depth;
            tmpDepths[4] = rayData.lastDepth;
            tmpDepths[4] = rayData.lastTransmittance;
        } else {
            tmpDepths[5] = rayData.lastDepth;
        }

        // Get all triangle areas of the points between
        float minArea = 1;
        uint minIdx = 4;
        [unroll]
        for (uint i = 1; i < 5; i++) {
            float triArea = areaTriangle(float2(tmpDepths[i - 1], tmpTransparencies[i - 1]), float2(tmpDepths[i], tmpTransparencies[i]), float2(tmpDepths[i + 1], tmpTransparencies[i + 1]));
            if (triArea < minArea) {
                minArea = triArea;
                minIdx = i;
            }
        }

        // Copy tmp list to payload without the min element
        [unroll]
        for (uint i = 0; i < 4; i++) {
            uint idx = i < minIdx ? i : i + 1;
            rayData.depths[i] = tmpDepths[idx];
            rayData.transmittances[i] = tmpTransparencies[idx];
        }
        rayData.lastDepth = tmpDepths[5];
        rayData.lastTransmittance = tmpTransparencies[5];
        
    }
    rayData.count++;
    // End search
    if (currentTransmittance < kMinOpacity)
        // AcceptHitAndEndSearch();
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

    uint flags = RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; //Call anyHit for every type of object
    TraceRay(gScene.rtAccel, flags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    // Fill undefined points
    float fillDepth = 1;
    float fillTransparency = 1;
    [unroll]
    for (uint i = 0; i < 4; i++) {
        if (rayData.depths[i] < 0) {
            rayData.depths[i] = fillDepth;
            rayData.transmittances[i] = fillTransparency;
        }
        fillDepth = rayData.depths[i];
        fillTransparency = rayData.transmittances[i];
    }
    
    // Write to textures
    gAVSMDepths[pixel] = rayData.depths;
    gAVSMTransparency[pixel] = rayData.transmittances;
    gAVSMLast[pixel] = float2(rayData.lastDepth, rayData.lastTransmittance);
}
