#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;

#ifndef AVSM_K
#define AVSM_K 4
#endif

#define AVSM_NUM_TEX AVSM_K/4

#ifndef AVSM_UNDERESTIMATE
#define AVSM_UNDERESTIMATE 1
#endif

#define AVSM_REJECTION_TRIANGLE_AREA 0
#define AVSM_REJECTION_RECTANGLE_AREA 1
#define AVSM_REJECTION_HEIGHT 2
#define AVSM_REJECTION_HEIGHT_AND_HEURISTIC 3


#ifndef AVSM_REJECTION_MODE
#define AVSM_REJECTION_MODE AVSM_REJECTION_TRIANGLE_AREA
#endif

/** Payload for shadow ray.
*/
struct RayData
{
    float firstDepth;
    float lastDepth;
    float transmittanceStart;
    float transmittanceAverage;
    float transmittanceEnd;
    uint count;
    
    __init() {
        this.count = 0;
        this.firstDepth = 0;
        this.lastDepth = 1;
        this.transmittanceStart = 1;
        this.transmittanceAverage = 1;
        this.transmittanceEnd = 1;
    }
};

cbuffer CB
{
    uint        gFrameCount;       //Current frame count
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    uint2       _pad;
    float4x4    gViewProj;      // View Projection for current light
    float4x4    gInvViewProj;   // Inverse view projection
}

RWTexture2D<float4> gResDepths[AVSM_NUM_TEX];
RWTexture2D<float4> gResTransmittance[AVSM_NUM_TEX];
RWTexture2D<float4> gResCount[AVSM_NUM_TEX];

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
    //TODO write
}

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

    //Get min max depth for the current used area
    float minDepth = 0.0;
    float maxDepth = 1.0;
    //TODO fetch from last frame
    float depthStep = (maxDepth - minDepth) / float(AVSM_K); 

    RayDesc ray;
    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toNear = posWPixelNear.xyz - gLightPos;
    float distToNear = length(toNear); 
    float fullRayDistance = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w) - distToNear;

    ray.TMin = distToNear;                                        // Always start at depth 0
    ray.TMax = distToNear + fullRayDistance * (minDepth + depthStep); //Trace first step
    ray.Origin = gLightPos;
    ray.Direction = normalize(toNear); // World direction
    
    RayData rayData = RayData();
    //rayData.sg = SampleGenerator(pixel, gFrameCount * 2 + 8)

    uint flags = RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object
    //uint flags = RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object
    float maxDepth = 0;
    float minTransmittance = 1;

    for(uint i=0 ; i<AVSM_K; i++){
        
        TraceRay(gScene.rtAccel, flags, kShadowRayInstanceMask /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
        
        maxDepth = max(maxDepth, rayData.lastDepth);
        minTransmittance = min(minTransmittance, rayData.transmittanceEnd);

        //Update steps
        ray.TMin = ray.TMax + 1e-10;
        if(i == AVSM_K-2){
            ray.TMax = distToNear + fullRayDistance;
        }else{
            ray.TMax += fullRayDistance * depthStep;
        }
        
    }


    

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
        gResDepths[i][pixel] = depths;
        float4 transmittance = float4(rayData.transmittances[payIdx], rayData.transmittances[payIdx + 1], rayData.transmittances[payIdx + 2], rayData.transmittances[payIdx + 3]);
        gResTransmittance[i][pixel] = float4(transmittance);
    }
}
