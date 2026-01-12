//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
RWTexture2D<float2> gMotionBackupAndOut;
Texture2D<uint> gPathLength;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 backup_mvec = gMotionBackupAndOut[pixel];
    uint pathLen = gPathLength[pixel];

    // TODO smooth mvec or choose backup_mvec

    gMotionBackupAndOut[pixel] = mvec;
}
