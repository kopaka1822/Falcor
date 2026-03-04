// input/output
Texture2D<float2> gMotionIn;
RWTexture2D<float2> gMotionOut;
Texture2D<unorm float> gIsBackupMotionIn;
RWTexture2D<unorm float> gIsBackupMotionOut;
// guidance
RWTexture2D<uint> gMotionError; // 0 = frozen (readonly), 1 = unreliable (dont read, overwrite), 2 = interpolated (read+write)
#define STATE_FROZEN 0
#define STATE_UNRELIABLE 1
#define STATE_INTERPOLATED 2
Texture2D<uint> gPathLength;
Texture2D<float3> gCurPos;
Texture2D<float3> gPrevPos;
Texture2D<float> gPosDiff;
Texture2D<float3> gLastRayDir; // direction to Pos (incomming ray direction)


SamplerState S; // linear sampler

cbuffer PerFrame
{
    int2 gFrameDim;
    int2 gDirection; // (1, 0) or (0,1)
    float2 gPrevJitter;
    float2 gCurJitter;
    int gRadius;
    bool gCompareBilateralOutput;
}

// adjust jitter for prev-lookup
float2 adjustJitter(float2 mvec)
{
    return mvec - gPrevJitter + gCurJitter;
}

float calcEstimatedError(float2 texC, float2 mvec, float3 curPos)
{
    float3 newPos = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0);
    float3 posDiff = newPos - curPos;
    float newError = length(posDiff);
    return newError;
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
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    const uint state = gMotionError[pixel];
    if (IS_SECOND_PASS && state == STATE_FROZEN)
    {
        return; // nothing should be written (retains original value from before the first pass)
    }

    // guidance data
    const uint localPathLength = gPathLength[pixel];
    const float3 localPos = gCurPos[pixel];
    const float posDiff = gPosDiff[pixel];

    const int2 cstart = max(0, pixel - gDirection * gRadius);
    const int2 cend = min(gFrameDim, pixel + gDirection * (gRadius + 1));

    float2 mSum = 0.0;
    float wSum = 0.0;
    float backupSum = 0.0;
    uint count = 0;
    for (int2 pos = cstart; dot(pos, gDirection) < dot(cend, gDirection); pos += gDirection)
    {
        float2 m = gMotionIn[pos];
        float isBackup = gIsBackupMotionIn[pos];
        float w = 1.0;
        if (gMotionError[pos] == STATE_UNRELIABLE) w = 0.0;
        // geometric distance
        w *= exp(-0.5 * distance(gCurPos[pos], localPos)/posDiff);
        // weigh each sample by the length of its vector
        //w *= 1.0 / (pow(length(m), 2.0) + 1e-12);
        mSum += w * m;
        wSum += w;
        backupSum += isBackup;
        count += 1;
    }

    if (wSum == 0.0) return; // keep previous value and state
    if(state == STATE_UNRELIABLE)
        gMotionError[pixel] = STATE_INTERPOLATED; // mark as interpolated (can be used in second pass)
    float2 mvec = mSum / wSum;
    float isBackup = saturate(backupSum / count);

    if (gCompareBilateralOutput && IS_SECOND_PASS)
    {
        float2 bmvec = gMotionOut[pixel]; // output of the LK-algorithm before the first pass

        //const float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
        //if (calcEstimatedError(texC, bmvec, curPos) < calcEstimatedError(texC, mvec, curPos))
        //    mvec = bmvec;

        if (isBackup > 0.2)
            mvec = bmvec;
    }

    gMotionOut[pixel] = mvec;
    if(IS_FIRST_PASS) // can be written for debug in pass two
        gIsBackupMotionOut[pixel] = isBackup;
}
