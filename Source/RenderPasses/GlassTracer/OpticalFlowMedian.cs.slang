//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
Texture2D<float2> gBackupMotion;
RWTexture2D<float2> gMotionOut;
Texture2D<uint> gPathLength;

// error pass
Texture2D<float> gErrorIn;
RWTexture2D<float> gErrorOut;

// pre-blur
Texture2D<uint> gMotionError;
RWTexture2D<uint> gMotionErrorOut;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
}

uint2 clampPixel(int2 coord)
{
    return uint2(clamp(coord, 0, int2(gFrameDim) - 1));
}

// main function for median pre-blur (before applying bilateral)
[numthreads(16, 16, 1)]
void medianPreMain(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint3 localId: SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 mvec = gMotion[pixel];
    uint err = gMotionError[pixel];

    // uv coords
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    uint2 pixelDim = uint2(640, 360);
    pixelDim.y = (pixelDim.x * gFrameDim.y) / gFrameDim.x; // match with frame dim aspect

    // get median of 3x3 neighborhood
#define RADIUS 1
#define COUNT ((1 + 2 * RADIUS) * (1 + 2 * RADIUS))

    float4 A[COUNT];
    uint i = 0;
    [unroll] for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        [unroll] for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float2 m = gMotion[clampPixel(pixel + int2(dx, dy))];
            uint e = gMotionError[clampPixel(pixel + int2(dx, dy))];
            // xy = motion vector, z = length in pixels (sort key), w = motion error
            A[i++] = float4(m, length(m * gFrameDim), float(e));
        }

    // sort registers (length) (insertion sort)
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
    uint newErr = uint(A[COUNT / 2].w);

    gMotionOut[pixel] = mvec;
    gMotionErrorOut[pixel] = max(newErr, err);
}

// main function for error blur
[numthreads(16, 16, 1)]
void errorMedianMain(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint3 localId: SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float curError = gErrorIn[pixel];

    // uv coords
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

#define RADIUS 1
#define COUNT ((1 + 2 * RADIUS) * (1 + 2 * RADIUS))

    float A[COUNT];
    uint i = 0;
    [unroll] for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        [unroll] for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float2 off = dx * stepX + dy * stepY;
            A[i++] = gErrorIn.SampleLevel(S, texC + off, 0.0);
        }

    // sort registers (insertion sort)
    [unroll] for (i = 1; i < COUNT; ++i)
    {
        let e = A[i];
        [unroll] for (uint j = i; j > 0 && A[j - 1] > e; --j)
        {
            A[j] = A[j - 1];
            A[j - 1] = e;
        }
    }

    curError = min(curError, A[COUNT / 2]);
    gErrorOut[pixel] = curError;
}

// main function for post-process blur
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 mvec = gMotion[pixel];

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

    //#define SORT_LENGTH
    #ifdef SORT_LENGTH
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

    // sort registers (length) (insertion sort)
    [unroll] for (i = 1; i < COUNT; ++i)
    {
        let m = A[i];
        [unroll] for (uint j = i; j > 0 && A[j - 1].z > m.z; --j)
        {
            A[j] = A[j - 1];
            A[j - 1] = m;
        }
    }

    #else // not sort length => sort uv

    float2 A[COUNT];
    uint i = 0;
    [unroll] for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        [unroll] for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float2 off = dx * stepX + dy * stepY;
            float2 m = gMotion.SampleLevel(S, texC + off, 0.0);
            A[i++] = m;
        }

    // sort registers u,v
    [unroll] for (i = 1; i < COUNT; ++i)
    {
        let m = A[i];
        [unroll] for (uint j = i; j > 0 && A[j - 1].x > m.x; --j)
        {
            A[j].x = A[j - 1].x;
            A[j - 1].x = m.x;
        }
    }
    [unroll] for (i = 1; i < COUNT; ++i)
    {
        let m = A[i];
        [unroll] for (uint j = i; j > 0 && A[j - 1].y > m.y; --j)
        {
            A[j].y = A[j - 1].y;
            A[j - 1].y = m.y;
        }
    }
    #endif

    mvec = A[COUNT / 2].xy; // median

    gMotionOut[pixel] = mvec;
}
