#include "Scene/Lighting/Lighting.slangh"
#include "Scene/Lighting/RayShadow.slangh"
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

RWTexture2D<uint> gHead;
RWTexture2D<uint> gPixelCount;
Texture2D<float> gDepth;

struct BufferData // 4 * 4 byte 
{
    uint next;
    float depth;
    uint2 color;
};

int packHalf(float a, float b)
{
    return f32tof16(a) | (f32tof16(b) << 16);
}

uint2 packHalf4(float4 color)
{
    return uint2(packHalf(color.r, color.g), packHalf(color.b, color.a));
}

globallycoherent RWStructuredBuffer<BufferData> gBuffer;
RWBuffer<uint> gCount; // current count for the buffer

#ifndef TRANSPARENCY_WHITELIST
#define TRANSPARENCY_WHITELIST 0
#endif
StructuredBuffer<uint> gTransparencyWhitelist;

bool isAlphaTested(uint materialID)
{
#if TRANSPARENCY_WHITELIST == 0
    return false;
#else
    uint isTransparent = (gTransparencyWhitelist[materialID / 32u] & (1u << (materialID % 32u)));
    return isTransparent == 0u;
#endif
}

cbuffer PerFrame
{
    uint2 gFrameDim;
    uint maxElements;
}

struct RayData
{
    uint head;
    uint count;
};

[shader("miss")]
void miss(inout RayData rayData: SV_RayPayload)
{

}

float computeShadowImpl(uint2 pixel, uint lightIndex, float3 posW, float3 normalW)
{
    // posW + normal?
    return traceShadowRay(pixel, posW, normalW, lightIndex);
}

[shader("anyhit")]
void anyHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    uint materialID = gScene.getMaterialID(instanceID);
    if (isAlphaTested(materialID))
        IgnoreHit(); // this was handled by the opaque vbuffer
    
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);

    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    let lod = ExplicitLodTextureSampler(0.0);

    let material = gScene.materials.getMaterial(materialID);
    float alpha = material.evalOpacity(gScene.materials, v.texC, lod);
    if (alpha <= 0.01)
        IgnoreHit(); // no hit

    // shading
    float4 shadedColor = shadeVBuffer(triangleHit, pixel, frameDim, -WorldRayDirection());
    //float4 shadedColor = 1.0;
    shadedColor.a = alpha;

    // reserve buffer slot
    uint idx = uint(-1);
    InterlockedAdd(gCount[0], 1, idx);
    if (idx >= maxElements)
        return;

    BufferData data;
    data.depth = RayTCurrent();
    data.color = packHalf4(shadedColor);
    data.next = rayData.head;
    rayData.head = idx;
    gBuffer[idx] = data;

    rayData.count++;
    IgnoreHit(); // continue tracing
}

[shader("closesthit")]
void closestHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{

}


[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);
    float zNear = gScene.camera.data.nearZ;
    float zFar = gScene.camera.data.farZ;
    float nonLinearZ = gDepth[pixel];
    float linearZ = zNear * zFar / (zFar + nonLinearZ * (zNear - zFar));
    float invCos = 1.f / dot(normalize(gScene.camera.data.cameraW), ray.dir);
    ray.tMax = linearZ * 0.999f * invCos;

    RayData rayData;
    rayData.head = uint(-1);
    rayData.count = 0;

    TraceRay(gScene.rtAccel,
        RAY_FLAG_CULL_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        kInstanceCameraParticle, 0, rayTypeCount, 0, ray.toRayDesc(), rayData);

    gHead[pixel] = rayData.head;
#if OPTIMIZE_SORT != 0
    gPixelCount[pixel] = rayData.count;
#endif
}

