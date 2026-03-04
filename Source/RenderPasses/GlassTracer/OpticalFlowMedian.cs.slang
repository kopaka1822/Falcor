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
    float2 bmvec = gBackupMotion[pixel];
    uint err = gMotionError[pixel];
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
#define COUNT ((1 + 2 * RADIUS) * (1 + 2 * RADIUS))

    uint anyError = 0;
    float4 A[COUNT];
    uint i = 0;
    [unroll] for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        [unroll] for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float2 m = gMotion[clampPixel(pixel + int2(dx, dy))];
            uint e = gMotionError[clampPixel(pixel + int2(dx, dy))];
            // xy = motion vector, z = length in pixels (sort key), w = motion error
            A[i++] = float4(m, length(m * gFrameDim), float(e));
            anyError |= e;
        }

    uint newErr = err;
    // only apply median filter if there was at least one error pixel in the neighborhood
    if (anyError != 0 && pathLen > 1)
    {
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
        newErr = uint(A[COUNT / 2].w);
        if (newErr != 0) mvec = bmvec; // if new motion vec is from backups, use backup motion vec instead
    }
    
    gMotionOut[pixel] = mvec;
    gMotionErrorOut[pixel] = max(newErr, err);
}
