//import Utils.Math.MathHelpers;

RWTexture2D<float2> gMotion;
Texture2D<float2> gBackupMotion;
Texture2D<uint> gMotionError;
Texture2D<float3> gColor;
Texture2D<float3> gPrevColor;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
    float2 gPrevJitter;
    float2 gCurJitter;
}

// snaps uv to top left (smallest) uv within the 2x2 pixel area
float2 snapTopLeft(float2 uv)
{
    return (floor(uv * float2(gFrameDim) - 0.5) + 0.5) / float2(gFrameDim);
}

// adjust jitter for prev-lookup
float2 adjustJitter(float2 mvec)
{
    return mvec - gPrevJitter + gCurJitter;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    //if (gMotionError[pixel] == 0) return; // frozen => dont write
    float3 color = gColor[pixel];
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 bmvec = gBackupMotion[pixel];

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    float3 mvecColor = gPrevColor.SampleLevel(S, texC + adjustJitter(mvec), 0.0);
    float3 bmvecColor = gPrevColor.SampleLevel(S, texC + adjustJitter(bmvec), 0.0);

    if (distance(color, bmvecColor) < 0.9 * distance(color, mvecColor))
    {
        mvec = bmvec;
    }

    gMotion[pixel] = mvec;
}
