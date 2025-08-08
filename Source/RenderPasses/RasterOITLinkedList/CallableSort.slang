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

    uint callableIndex = 7;
    if (listSize <= 128)
        callableIndex = 6;
    if (listSize <= 64)
        callableIndex = 5;
    if (listSize <= 32)
        callableIndex = 4;
    if (listSize <= 16)
        callableIndex = 3;
    if (listSize <= 8)
        callableIndex = 2;
    if (listSize <= 4)
        callableIndex = 1;
    if (listSize == 0)
        callableIndex = 0;

    SortParams params;
    params.head = head;
    
    CallShader(callableIndex, params);
}

[shader("callable")]
void SortList_0(inout SortParams p)
{
    gColor[DispatchRaysIndex().xy] = float4(0.0, 0.0, 0.0, 1.0);
}

[shader("callable")]
void SortList_4(inout SortParams p)
{
#define MAX_FRAGMENT 4
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_8(inout SortParams p)
{
#define MAX_FRAGMENT 8
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_16(inout SortParams p)
{
#define MAX_FRAGMENT 16
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_32(inout SortParams p)
{
#define MAX_FRAGMENT 32
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_64(inout SortParams p)
{
#define MAX_FRAGMENT 64
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_128(inout SortParams p)
{
#define MAX_FRAGMENT 128
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}

[shader("callable")]
void SortList_256(inout SortParams p)
{
#define MAX_FRAGMENT 256
#include "CallableInternal.h"
#undef MAX_FRAGMENT
}
