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

#ifndef MAX_FRAGMENT
#define MAX_FRAGMENT 256
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    uint2 pixel = id.xy;
    if (pixel.x >= gFrameDim.x || pixel.y >= gFrameDim.y)
        return;

    #ifdef MIN_FRAGMENT
    uint pixCount = gPixelCount[pixel];
    if (pixCount > MAX_FRAGMENT) return;
    if (pixCount <= MIN_FRAGMENT)
    {
        if (MIN_FRAGMENT == 0) gColor[pixel] = float4(0, 0, 0, 1);
        return; // this is handled by a different shader
    }
    #endif

    uint head = gHead[pixel];
    if (head >= maxElements)
        return;

    // load fragments into registers
    FragData f[MAX_FRAGMENT];

    // insert from linked list
    uint next = head;
    uint count = 0;
    [unroll] for (uint i = 0; i < MAX_FRAGMENT && next < maxElements; ++i)
    {
        BufferData data = gBuffer[next];
        next = data.next;
        f[i].depth = data.depth;
        f[i].color = data.color;
        count++;
    }

#define IN_LIST(i) i < count

#if MAX_FRAGMENT < 32

    // sort registers (insertion sort)
    [unroll] for (uint i = 1; i < MAX_FRAGMENT && IN_LIST(i); ++i)
    {
        // i - 1 elements are sorted
        [unroll] for (uint j = i; j > 0 && f[j - 1].depth > f[j].depth; --j)
        {
            let tmp = f[j];
            f[j] = f[j - 1];
            f[j - 1] = tmp;
        }
    }
#else
    // sort registers (shell sort)
    const uint gaps[] = { 111, 41, 13, 4, 1 };
    const uint maxGaps = 5;
#if MAX_FRAGMENT <= 111
    const uint startGap = 1;
#else
    const uint startGap = 0;
#endif

    [unroll] for (uint gapIdx = startGap; gapIdx < maxGaps; ++gapIdx)
    {
        const int gap = gaps[gapIdx];
        for (uint i = gap; i < MAX_FRAGMENT && IN_LIST(i); ++i)
        {
            for (uint j = i; j >= gap && f[j - gap].depth > f[j].depth; j -= gap)
            {
                let tmp = f[j];
                f[j] = f[j - gap];
                f[j - gap] = tmp;
            }
        }
    }

#endif

    float4 color = float4(0.0, 0.0, 0.0, 1.0);

    // now blend together (front to back)
    [unroll] for (uint i = 0; i < MAX_FRAGMENT && IN_LIST(i); ++i)
    {
        float4 c = unpackHalf4(f[i].color);
        color.rgb += color.a * c.a * c.rgb;
        color.a *= (1.0 - c.a);
    }

    gColor[pixel] = color;
}
