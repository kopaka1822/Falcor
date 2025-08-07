#include "Utils/Math/MathConstants.slangh"

RWTexture2D<float4> gColor;
Texture2D<uint> gHead;
Texture2D<uint> gPixelCount;

struct BufferData // 4 * 4 byte 
{
    uint next;
    float depth;
    //half4 color;
    uint2 color;
    //float2 color;
};

float2 unpackHalf(uint c)
{
    return float2(f16tof32(c), f16tof32(c >> 16));
}

float4 unpackHalf4(uint2 c)
{
    return float4(unpackHalf(c.r), unpackHalf(c.g));
}

struct FragData
{
    // half4 color;
    uint2 color;
    float depth;
};

StructuredBuffer<BufferData> gBuffer;

cbuffer PerFrame
{
    uint2 gFrameDim;
    uint maxElements;
}

struct SortParams
{
    uint head;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= gFrameDim.x || pixel.y >= gFrameDim.y)
        return;

    uint listSize = gPixelCount[pixel];
    uint head = gHead[pixel];
    if (head >= maxElements)
        listSize = 0;

    // Choose callable shader index based on list size
    uint callableIndex = 0;
    if (listSize > 0)
    {
        uint clampedSize = min(listSize, 256); // Cap at 256
        // Compute the next power-of-two bucket index:
        // 1–4 => 1, 5–8 => 2, ..., 129–256 => 7
        callableIndex = 1 + firstbithigh((clampedSize - 1) >> 2);
    }

    SortParams params;
    params.head = head;

    CallShader(callableIndex, params);
}

[shader("callable")]
void SortList_0(SortParams p)
{
    gColor[DispatchRaysIndex().xy] = float4(0.0, 0.0, 0.0, 1.0);
}

[shader("callable")]
void SortList_4(SortParams p)
{
#define MAX_FRAGMENT 4
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_8(SortParams p)
{
#define MAX_FRAGMENT 8
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_16(SortParams p)
{
#define MAX_FRAGMENT 16
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_32(SortParams p)
{
#define MAX_FRAGMENT 32
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_64(SortParams p)
{
#define MAX_FRAGMENT 64
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_128(SortParams p)
{
#define MAX_FRAGMENT 128
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_256(SortParams p)
{
#define MAX_FRAGMENT 256
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}
