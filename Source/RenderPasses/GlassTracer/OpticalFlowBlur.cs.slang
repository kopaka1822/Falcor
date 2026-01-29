// input/output
Texture2D<float2> gMotionIn;
RWTexture2D<float2> gMotionOut;
// guidance
RWTexture2D<uint> gMotionError; // 0 = frozen (readonly), 1 = unreliable (dont read, overwrite), 2 = interpolated (read+write)
#define STATE_FROZEN 0
#define STATE_UNRELIABLE 1
#define STATE_INTERPOLATED 2
Texture2D<uint> gPathLength;
Texture2D<float3> gCurPos;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    int2 gFrameDim;
    int2 gDirection; // (1, 0) or (0,1)
    int gRadius;
}

#define IS_FIRST_PASS (gDirection.x != 0)
#define IS_SECOND_PASS (gDirection.y != 0)

// Strategy: Pixels will be markes as FROZEN or UNRELIABLE
// After applying the bilateral filter:
//     FROZEN pixels will retain their original values,
//     UNRELIABLE pixels will have values from surrounding FROZEN pixels
// 
// First Pass:
// A Filter over X is applied for ALL pixels.
// Only FROZEN pixels will be used in the filter.
// UNREALIABLE pixels with newly interpolated values will be marked as INTERPOLATED
// Outputs will be written into a temporaray render target.
//
// Second Pass:
// A Filter over Y is applied for non-FROZEN pixels.
// Only FROZEN and INTERPOLATED pixels will be used in the filter.
// FROZEN pixels will write nothing at all, so that they keep their original value from before the first pass.
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const int2 pixel = id.xy;
    const uint state = gMotionError[pixel];
    if (IS_SECOND_PASS && state == STATE_FROZEN)
    {
        return; // nothing should be written (retains original value from before the first pass)
    }

    const uint pathLen = gPathLength[pixel];

    const int2 cstart = max(0, pixel - gDirection * gRadius);
    const int2 cend = min(gFrameDim, pixel + gDirection * (gRadius + 1));

    float2 mSum = 0.0;
    float wSum = 0.0;
    for (int2 pos = cstart; dot(pos, gDirection) < dot(cend, gDirection); pos += gDirection)
    {
        float2 m = gMotionIn[pos];
        float w = 1.0;
        if (gMotionError[pos] == STATE_UNRELIABLE) w = 0.0;
        mSum += w * m;
        wSum += w;
    }

    if (wSum == 0.0) return; // keep previous value and state
    if(state == STATE_UNRELIABLE)
        gMotionError[pixel] = STATE_INTERPOLATED; // mark as interpolated (can be used in second pass)
    float2 mvec = mSum / wSum;
    gMotionOut[pixel] = mvec;
}
