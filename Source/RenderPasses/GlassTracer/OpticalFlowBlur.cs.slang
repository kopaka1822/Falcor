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


    // get median of 3x3 neighborhood
#define RADIUS 1
#define COUNT ((1 + 2 * RADIUS)*(1 + 2 * RADIUS))

    float3 A[COUNT];
    uint i = 0;
    [unroll] for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        [unroll] for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float2 off = dx * stepX + dy * stepY;
            float2 m = gMotion.SampleLevel(S, texC + off, 0.0);
            // xy = motion vector, z = length in pixels (sort key)
            A[i++] = float3(m, length(m * gFrameDim));
        }

    // sort registers (insertion sort)
    [unroll] for (i = 1; i < COUNT; ++i)
    {
        let m = A[i];
        [unroll] for (uint j = i; j > 0 && A[j - 1].z > m.z; --j)
        {
            A[j] = A[j - 1];
            A[j - 1] = m;
        }
    }

    mvec = A[COUNT / 2].xy; // median

    gMotionBackupAndOut[pixel] = mvec;
}
