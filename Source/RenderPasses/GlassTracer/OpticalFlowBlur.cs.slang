//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
RWTexture2D<float2> gMotionBackupAndOut;
Texture2D<uint> gPathLength;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
}

bool isPathEdge(int2 pixel)
{
    uint len = gPathLength[pixel];
    bool hasSameX = false;
    bool hasSameY = false;
    if(pixel.x > 0)
        hasSameX |= (gPathLength[pixel + int2(-1, 0)] == len);
    if(pixel.x < gFrameDim.x - 1)
        hasSameX |= (gPathLength[pixel + int2(1, 0)] == len);
    if(pixel.y > 0)
        hasSameY |= (gPathLength[pixel + int2(0, -1)] == len);
    if(pixel.y < gFrameDim.y - 1)
        hasSameY |= (gPathLength[pixel + int2(0, 1)] == len);

    return !(hasSameX && hasSameY); // if we have one valid neighbor for x, and one valid for y, we are not an edge
}

uint getPathLenSafe(int2 pixel)
{
    if (pixel.x < 0 || pixel.x >= gFrameDim.x ||
        pixel.y < 0 || pixel.y >= gFrameDim.y)
        return 0;
    return gPathLength[pixel];
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 mvec = gMotion[pixel];
    float2 backup_mvec = gMotionBackupAndOut[pixel];
    uint pathLen = gPathLength[pixel];

    // uv coords
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    uint2 pixelDim = uint2(640, 360);
    pixelDim.y = (pixelDim.x * gFrameDim.y) / gFrameDim.x; // match with frame dim aspect

    float2 mSum = 0.0;
    float wSum = 0.0;

    int radius = 3; // smallest is 1 (1 pixel)
    int nSamples = 1 + (radius - 1) * 2;
    nSamples = nSamples * nSamples; // number of samples in the window
    float percentValid = 0.0;

    for (int dx = -radius + 1; dx < radius; ++dx)
        for (int dy = -radius + 1; dy < radius; ++dy)
        {
            float2 off = dx * stepX + dy * stepY;

            float2 m = gMotion.SampleLevel(S, texC + off, 0.0);
            // float w = max(0.0, 1.0 / (length(m * pixelDim)) - 1.0);
            float w = getPathLenSafe(int2(pixel) + int2(dx, dy)) == pathLen ? 1.0 : 0.0;
            if (w > 0) percentValid += (1.0 / nSamples);
            float rdiff = dot(abs(m - mvec) / abs(mvec), 0.5);
            w *= exp(-rdiff * rdiff); // favor similar motion vectors

            mSum += w * m;
            wSum += w;
        }
    // TODO smooth mvec or choose backup_mvec

    //mvec = mSum / max(wSum, 1e-8);

    // if (isPathEdge(pixel))
    //     mvec = backup_mvec;
    // if (percentValid < 0.9)
    //     mvec = backup_mvec;
    //float localPixelLen = length(mvec * pixelDim);
    //if (localPixelLen > 0.5)
    //    mvec = backup_mvec;

    gMotionBackupAndOut[pixel] = mvec;
}
