#include "Utils/Math/MathConstants.slangh"

RWTexture2D<float4> gColor;

struct BufferFragment // 4 * 4 byte 
{
    uint2 color;
    float depth;
    uint pad; // padding
};

struct Fragment
{
    uint2 color;
    float depth;
};

RWStructuredBuffer<BufferFragment> gBuffer;
StructuredBuffer<uint> gPrefix; // start indices for each list

float2 unpackHalf(uint c)
{
    return float2(f16tof32(c), f16tof32(c >> 16));
}

float4 unpackHalf4(uint2 c)
{
    return float4(unpackHalf(c.r), unpackHalf(c.g));
}

#define FLOAT_MAX 3.402823466e+38



cbuffer PerFrame
{
    uint2 gFrameDim;
    uint maxFragmentCount;
}

#ifndef MAX_FRAGMENT
#define MAX_FRAGMENT 256
#endif
#ifndef MIN_FRAGMENT
#define MIN_FRAGMENT 0
#endif

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    uint2 pixel = id.xy;
    if (pixel.x >= gFrameDim.x || pixel.y >= gFrameDim.y)
        return;

    // get the start of the per-pixel list
    uint index = uint(pixel.y) * gFrameDim.x + uint(pixel.x);
    uint startIdx = index > 0 ? gPrefix[index - 1] : 0;
    uint endIdx = min(gPrefix[index], maxFragmentCount);

    if (startIdx >= endIdx)
    {
        gColor[pixel] = float4(0, 0, 0, 1);
        return;
    }

    const uint numFragments = endIdx - startIdx;
    if (numFragments <= MIN_FRAGMENT) return; // this is handled by a different shader

//#undef MAX_FRAGMENT
//#ifdef MAX_FRAGMENT
    if (numFragments > MAX_FRAGMENT) return; // this is handled by a different shader

#define IN_LIST(i) i < numFragments

	// load fragments into registers
	Fragment f[MAX_FRAGMENT];
	Fragment emptyFrag;
	emptyFrag.depth = FLOAT_MAX;
	emptyFrag.color = 0;
	[unroll] for(uint i = 0; i < MAX_FRAGMENT && IN_LIST(i); ++i)
    {
        [flatten] if (i < numFragments)
        {
            let gf = gBuffer[startIdx + i];
            f[i].depth = gf.depth;
            f[i].color = gf.color;
        }
		else f[i] = emptyFrag;
	}

#if MAX_FRAGMENT < 32

    // sort registers (insertion sort)
    [unroll] for (uint i = 1; i < MAX_FRAGMENT && IN_LIST(i); ++i)
	{
		// i - 1 elements are sorted
		[unroll] for(uint j = i; j > 0 && f[j - 1].depth > f[j].depth; --j)
		{
			Fragment tmp = f[j];
			f[j] = f[j - 1];
			f[j - 1] = tmp;
		}
	}
#else
	// sort registers (shell sort)
	const uint gaps[] = {111, 41, 13, 4, 1};
	const uint maxGaps = 5;
#if MAX_FRAGMENT <= 111
	const uint startGap = 1;
#else
	const uint startGap = 0;
#endif

	[unroll] for(uint gapIdx = startGap; gapIdx < maxGaps; ++gapIdx) 
	{
		const int gap = gaps[gapIdx];
		for(uint i = gap; i < MAX_FRAGMENT && IN_LIST(i); ++i) 
		{
			for(uint j = i; j >= gap && f[j - gap].depth > f[j].depth; j -= gap)
			{
				Fragment tmp = f[j];
				f[j] = f[j - gap];
				f[j - gap] = tmp;
			}
		}
	}


#endif


	float4 color = float4(0.0, 0.0, 0.0, 1.0);

	// now blend together (front to back)
	[unroll] for(uint i = 0; i < MAX_FRAGMENT && IN_LIST(i); ++i)
	{
		float4 c = unpackHalf4(f[i].color);
		color.rgb += color.a * c.a * c.rgb;
		color.a *= (1.0 - c.a);
	}

/*#else // no upper limit for list length
	// sort fragments between start and end
    for (uint i = startIdx + 1; i < endIdx; ++i)
    {
        let curFrag = gBuffer[i];
        uint j = i;
        for (; j > startIdx && gBuffer[j - 1].depth < curFrag.depth; --j)
        {
            gBuffer[j] = gBuffer[j - 1];
        }
        gBuffer[j] = curFrag;
    }

	// blend fragments
	// the fragment with the highest depth will be the first fragment
	// blend together:
    float4 color = unpackHalf4(gBuffer[startIdx].color);
    color.rgb *= color.a;
	// background occlusion
    color.a = (1.0 - color.a);
	
    for (uint i = startIdx + 1; i < endIdx; ++i)
    {
        float4 next = unpackHalf4(gBuffer[i].color);
		// fragment color
        color.rgb = next.a * next.rgb + (1.0 - next.a) * color.rgb;
		// background occlusion
        color.a *= (1.0 - next.a);
    }
#endif*/

    gColor[pixel] = color;
}
