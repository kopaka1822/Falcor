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

RWStructuredBuffer<uint> gCounts; // all passes
StructuredBuffer<uint> gPrefix; // start indices for each list (record only)

struct BufferData // 4 * 4 byte 
{
    uint2 color;
    float depth;
    uint pad;
};

int packHalf(float a, float b)
{
    return f32tof16(a) | (f32tof16(b) << 16);
}

uint2 packHalf4(float4 color)
{
    return uint2(packHalf(color.r, color.g), packHalf(color.b, color.a));
}

globallycoherent RWStructuredBuffer<BufferData> gBuffer; // (record only)

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
    uint index = pixel.y * gFrameDim.x + pixel.x;

    #if ALPHA_TEXTURE_LOD != 0
    let lod = ImplicitLodTextureSampler();
    #else
    let lod = ExplicitLodTextureSampler(0.0);
    #endif
    
    let material = gScene.materials.getMaterial(v.materialID);
    float alpha = material.evalOpacity(gScene.materials, v.texC, lod);
    if (alpha <= 0.01)
        discard; // no hit

    #ifdef COUNT
    InterlockedAdd(gCounts[index], 1u); // increase counter
    #else

    // list start index
    uint listStart = 0;
    if (index > 0)
        listStart = gPrefix[index - 1];

    // current offset in list
    uint offset = 0;
    InterlockedAdd(gCounts[index], uint(-1), offset); // count down
    offset -= 1;

    uint insertIdx = listStart + offset;
    if (insertIdx >= maxElements)
        discard; 
    
    TriangleHit triangleHit;
    triangleHit.instanceID = v.instanceID;
    triangleHit.primitiveIndex = triangleIndex;
    triangleHit.barycentrics = barycentrics.yz;
    
    float3 viewDir = -gScene.camera.computeRayPinhole(pixel, gFrameDim).dir;
    float4 shadedColor = shadeVBuffer(triangleHit, pixel, gFrameDim, viewDir);
    shadedColor.a = alpha;

    BufferData data = {};
    data.depth = v.posH.w;
    data.color = packHalf4(shadedColor);

    // store buffer data
    gBuffer[insertIdx] = data;
#endif
}
