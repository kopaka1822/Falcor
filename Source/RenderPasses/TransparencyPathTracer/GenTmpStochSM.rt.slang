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

#define MSAA_SAMPLES 8

/** Payload for shadow ray.
*/
struct RayData
{
    float depths[AVSM_K];
    float transmittance[AVSM_K];
    uint count;
    float accumulatedVisibility;
    float weight;
    uint _pad;
    SampleGenerator sg;

    __init() {
        for (uint i = 0; i < AVSM_K; i++) {
            depths[i] = 1.0;
            transmittance[i] = 1.0;
        }
        count = 0;
        weight = 0.0;
        accumulatedVisibility = 1.0;
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

RWTexture2D<float4> gStochDepths[AVSM_NUM_TEX];
RWTexture2D<float4> gStochTransmittance[AVSM_NUM_TEX];

static const float kDepthBias = AVSM_DEPTH_BIAS;
static const float kNormalBias = AVSM_NORMAL_DEPTH_BIAS;
static const float kMinOpacity = 1e-7;

// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_standard_multisample_quality_levels?redirectedfrom=MSDN

#if MSAA_SAMPLES == 4
static const float2 kMSAAPattern[4] = { float2(-0.125, 0.375), float2(0.375, -0.125), float2(-0.375, 0.125), float2(0.125, 0.375)
                                      };
#elif MSAA_SAMPLES == 8
static const float2 kMSAAPattern[8] = { float2(0.0625, -0.1875),  float2(-0.0625, 0.1875), float2(0.3125, 0.0625), float2(-0.1875, -0.3125),
                                        float2(-0.3125, 0.3125), float2(-0.4375, -0.0625), float2(0.1875, 0.4375),   float2(0.4375, -0.4375)
                                      };
#elif MSAA_SAMPLES == 16
static const float2 kMSAAPattern[16] = { float2(0.0625, 0.0625),  float2(-0.0625, -0.1875), float2(-0.1875, 0.125),  float2(0.25, -0.0625),
                                         float2(-0.3125, -0.125), float2(0.125, 0.3125), float2(0.3125, 0.1875),  float2(0.1875, -0.3125),sss
                                         float2(-0.125, 0.375), float2(0.0, -0.4375),    float2(-0.25, 0.375), float2(-0.375, 0.25),
                                         float2(-0.5, 0.0), float2(0.4375, -0.25),   float2(0.375, 0.4375), float2(-0.4375, -0.5)
                                       };
#else
#error Invalid MSAA_SAMPLES for this pass!
#endif
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

    const float transmittance = (1.0 - sd.opacity);
    rayData.accumulatedVisibility *= transmittance;

    uint insertIndex = rayData.count;

    rayData.weight += sd.opacity;

    //Get random index
    if (insertIndex >= AVSM_K) {
        /*
        bool selectSample = (sampleNext1D(rayData.sg) * rayData.weight < sd.opacity);
        if (selectSample)
            insertIndex = uint(AVSM_K * sampleNext1D(rayData.sg));
        */
        insertIndex = uint((insertIndex + 1) * sampleNext1D(rayData.sg));
    }
        
    if (insertIndex < AVSM_K) {
        // Get depth
        float4 posH = mul(gViewProj, float4(sd.posW, 1.f));
        float depth = (posH.w - gNear) / (gFar - gNear); // Normalized depth

        // Add bias to depth
        float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
        depth = saturate(depth + kDepthBias + normalBias);
        rayData.depths[insertIndex] = depth;
        rayData.transmittance[insertIndex] = transmittance;
    }

    rayData.count++;
    if (rayData.accumulatedVisibility < kMinOpacity)
        return;
    else
        IgnoreHit();
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    RayData rayData = RayData();
    rayData.sg = SampleGenerator(pixel, gFrameCount * 2 + 8);

    float2 pixelSample = pixel + 0.5; // + kMSAAPattern[gFrameCount % MSAA_SAMPLES];

    float2 relFramePos = pixelSample / frameDim;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toNear = posWPixelNear.xyz - gLightPos;

    RayDesc ray;
    ray.TMin = length(toNear);;                                             // Always start at depth 0
    ray.TMax = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
    ray.Origin = gLightPos;
    ray.Direction = normalize(toNear); // World direction

    uint flags = RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object
    // uint flags = RAY_FLAG_CULL_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; // Call anyHit for every type of object

    TraceRay(gScene.rtAccel, flags, kShadowRayInstanceMask /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    uint validSamples = min(rayData.count, AVSM_K);
    // Sort by depth (bubblesort for now)
    for (int n = validSamples; n > 1; n--) {
        for (int i = 0; i < n - 1; i++) {
            if (rayData.depths[i] > rayData.depths[i + 1]) {
                float2 tmp = float2(rayData.depths[i], rayData.transmittance[i]);
                rayData.depths[i] = rayData.depths[i + 1];
                rayData.transmittance[i] = rayData.transmittance[i + 1];
                rayData.depths[i + 1] = tmp.x;
                rayData.transmittance[i + 1] = tmp.y;
            }
        }
    }

    // Write to texture
    float startTransmittance = 1.0;
    for (uint i = 0; i < AVSM_NUM_TEX; i++) {
        uint payIdx = i * 4;
        float4 depths = float4(rayData.depths[payIdx], rayData.depths[payIdx + 1], rayData.depths[payIdx + 2], rayData.depths[payIdx + 3]);
        gStochDepths[i][pixel] = depths;
        float4 transmittance;
        for (uint j = 0; j < 4; j++) {
            if (rayData.count > AVSM_K) {
                float tmpTransmittance = 1.0 - (float(1 + payIdx + j) / AVSM_K);
                //tmpTransmittance *= (1.0 - rayData.transmittance[payIdx + j]) / rayData.weight;
                transmittance[j] = tmpTransmittance;
            } else {
                startTransmittance *= rayData.transmittance[payIdx + j];
                transmittance[j] = startTransmittance;
            }            
        }
        gStochTransmittance[i][pixel] = transmittance;
    }
}
