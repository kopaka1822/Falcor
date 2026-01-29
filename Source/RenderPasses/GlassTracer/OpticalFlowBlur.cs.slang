// input/output
Texture2D<float2> gMotionIn;
RWTexture2D<float2> gMotionOut;
// guidance
Texture2D<uint> gMotionError;
Texture2D<uint> gPathLength;
Texture2D<float3> gCurPos;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    int2 gFrameDim;
    int2 gDirection; // (1, 0) or (0,1)
    int gRadius;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const int2 pixel = id.xy;
    if (gMotionError[pixel] == 0)
        return; // nothing to filter

    const uint pathLen = gPathLength[pixel];

    const int2 cstart = max(0, pixel - gDirection * gRadius);
    const int2 cend = min(gFrameDim, pixel + gDirection * (gRadius + 1));

    float2 mSum = 0.0;
    float wSum = 0.0;
    for (int2 pos = cstart; dot(pos, gDirection) < dot(cend, gDirection); pos += gDirection)
    {
        float2 m = gMotionIn[pos];
        float w = 1.0;
        if (gMotionError[pos]) w = 0.0;
        mSum += w * m;
        wSum += w;
    }

    if (wSum == 0.0) return; // keep previous value
    float2 mvec = mSum / wSum;
    gMotionOut[pixel] = mvec;
}
