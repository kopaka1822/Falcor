#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"
#include "Scene/Lighting/Lighting.slangh"
#include "Scene/Lighting/RayShadow.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

#ifndef ALPHA_TEXTURE_LOD
#define ALPHA_TEXTURE_LOD 0
#endif

RWTexture2D<uint> gHead;
RWTexture2D<uint> gPixelCount; // count list lengths

struct BufferData // 4 * 4 byte 
{
    uint next;
    float depth;
    uint2 color;
    //uint2 color;
    //float2 color;
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

cbuffer PerFrame
{
    uint2 gFrameDim;
    uint maxElements;
}

VSOut vsMain(VSIn vIn)
{
    return defaultVS(vIn);
}

float computeShadowImpl(uint2 pixel, uint lightIndex, float3 posW, float3 normalW)
{
    return traceShadowRay(pixel, posW, normalW, lightIndex);
}

[earlydepthstencil]
void psMain(VSOut v, uint triangleIndex : SV_PrimitiveID, float3 barycentrics : SV_Barycentrics, bool isFrontFace : SV_IsFrontFace)
{
    uint2 pixel = uint2(v.posH.xy);
    
    TriangleHit triangleHit;
    triangleHit.instanceID = v.instanceID;
    triangleHit.primitiveIndex = triangleIndex;
    triangleHit.barycentrics = barycentrics.yz;

    //return;

    #if ALPHA_TEXTURE_LOD != 0
    let lod = ImplicitLodTextureSampler();
    #else
    let lod = ExplicitLodTextureSampler(0.0);
    #endif
    
    let material = gScene.materials.getMaterial(v.materialID);
    float alpha = material.evalOpacity(gScene.materials, v.texC, lod);
    if (alpha <= 0.01)
        return;
        //discard; // no hit
    
    float3 viewDir = -gScene.camera.computeRayPinhole(pixel, gFrameDim).dir;
    float4 shadedColor = shadeVBuffer(triangleHit, pixel, gFrameDim, viewDir);
    shadedColor.a = alpha;
    
    uint idx = uint(-1);

    InterlockedAdd(gCount[0], 1, idx);
    if (idx >= maxElements)
        return;
        //discard;

    BufferData data;
    data.depth = v.posH.w;
    data.color = packHalf4(shadedColor);
    //  change head pointer to this element, and use previous head as next from this element
    InterlockedExchange(gHead[pixel], idx, data.next);

    // store buffer data
    gBuffer[idx] = data;
    
#if OPTIMIZE_SORT != 0
    InterlockedAdd(gPixelCount[pixel], 1);
#endif
}
