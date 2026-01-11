//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion; // only for hornSchunkMain
RWTexture2D<float2> gMotionOut;
Texture2D<float3> gCurPos;
Texture2D<float3> gPrevPos;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
    float2 gPrevJitter;
    float2 gCurJitter;
    uint gIterations;
    float gMaxMovement; // max movement in pixels
    int gWindowRadius; // radius in pixels (1 = default)
}

// solves t * T + b * B = R and returns (t, b)
float2 solveLinearEq(float3 T, float3 B, float3 R)
{
    const float epsilon = 1e-6;

    // Compute which axis to drop
    float3 sum = abs(T) + abs(B);
    uint drop = (sum.x < sum.y)
                    ? ((sum.x < sum.z) ? 0u : 2u)
                    : ((sum.y < sum.z) ? 1u : 2u);

    float2 T2, B2, R2;
    [branch]
    switch (drop)
    {
    case 0: T2 = T.yz; B2 = B.yz; R2 = R.yz; break; // drop x
    case 1: T2 = T.xz; B2 = B.xz; R2 = R.xz; break; // drop y
    default: T2 = T.xy; B2 = B.xy; R2 = R.xy; break; // drop z
    }

    //if (abs(R2.x) < epsilon) R2.x = 0.0;
    //if (abs(R2.y) < epsilon) R2.y = 0.0;

    // Solve the 2×2 linear system
    float det = T2.x * B2.y - T2.y * B2.x;
    if (abs(det) < 1e-8)
        return 0.0; // Degenerate or parallel T/B

    float invDet = 1.0 / det;
    return float2(
        (R2.x * B2.y - R2.y * B2.x) * invDet, // t
        (T2.x * R2.y - T2.y * R2.x) * invDet  // b
    );
}

// get derivative with smallest magnitude (+step/-step)
float3 getMinDeriv(float3 prevPos, float2 uvPrev, float2 step)
{
    // [x + 1] - [x]
    float3 deriv1 = gPrevPos.SampleLevel(S, uvPrev + step, 0.0) - prevPos;
    float3 deriv2 = prevPos - gPrevPos.SampleLevel(S, uvPrev - step, 0.0);
    return (length(deriv1) <= length(deriv2)) ? deriv1 : deriv2;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotionOut[pixel];

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    for (uint i = 0; i < gIterations; ++i)
    {
        float2 mvec0 = mvec - gPrevJitter + gCurJitter; // adjust jitter of motion vector
        float2 uv0 = texC + mvec0;
        float2 uvSnap = (floor(uv0 * float2(gFrameDim)) + 0.5) / float2(gFrameDim);
        uvSnap = uv0;
        float2 uvDiff = uv0 - uvSnap; // uvSnap + uvDiff = uv0

        float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
        float3 prevPos = gPrevPos.SampleLevel(S, uvSnap, 0.0);

        // build derivatives
        float3 I_t = curPos - prevPos; // [t+1] - [t]
        float3 I_x = getMinDeriv(prevPos, uvSnap, stepX);
        float3 I_y = getMinDeriv(prevPos, uvSnap, stepY);

        float2 uvPixel = solveLinearEq(I_x, I_y, I_t);

        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);

        mvec = mvec + (uvPixel / float2(gFrameDim)) - uvDiff;
    }

    gMotionOut[pixel] = mvec;
}

float2 getAverageMotion(float2 texC, float2 stepX, float2 stepY)
{
    float2 sum = gMotion.SampleLevel(S, texC + stepX, 0.0);
    sum += gMotion.SampleLevel(S, texC - stepX, 0.0);
    sum += gMotion.SampleLevel(S, texC + stepY, 0.0);
    sum += gMotion.SampleLevel(S, texC - stepY, 0.0);
    return sum * 0.25;
}

[numthreads(16, 16, 1)]
void hornSchunkMain(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint3 localId: SV_GroupThreadID)
{
    const float alpha = 1.0; // smoothness factor
    const uint2 pixel = id.xy;
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    float2 mvec0 = mvec - gPrevJitter + gCurJitter; // adjust jitter of motion vector
    float2 uv0 = texC + mvec0;
    float2 uvSnap = (floor(uv0 * float2(gFrameDim)) + 0.5) / float2(gFrameDim);
    uvSnap = uv0;
    float2 uvDiff = uv0 - uvSnap; // uvSnap + uvDiff = uv0

    float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
    float3 prevPos = gPrevPos.SampleLevel(S, uvSnap, 0.0);

    // build derivatives
    float3 I_t = curPos - prevPos; // [t+1] - [t]
    float3 I_x = getMinDeriv(prevPos, uvSnap, stepX);
    float3 I_y = getMinDeriv(prevPos, uvSnap, stepY);
    float2 uv_avg = getAverageMotion(texC, stepX, stepY);
    uv_avg = uv_avg - gPrevJitter + gCurJitter;   // adjust jitter of average motion vector
    uv_avg = uv_avg * float2(gFrameDim);        // convert to pixel units
    float2 uv_k = mvec0 * float2(gFrameDim); // convert to pixel units

    // since I_x and I_y are derivatives for one pixel difference, the motion vector should also be in pixel units (mvec and avg)
    float3 common = (I_x * uv_k.x + I_y * uv_k.y - I_t) / (alpha * alpha + I_x * I_x + I_y * I_y);
    float3 u_next = uv_avg.x - I_x * common;
    float3 v_next = uv_avg.y - I_y * common;

    float2 uvPixel = float2(dot(u_next, 1.0 / 3.0), dot(v_next, 1.0 / 3.0)) - uv_k;
    //float2 uvPixel = 0.0;
    if (length(uvPixel) > gMaxMovement) // restrict pixel step size
        uvPixel = gMaxMovement * normalize(uvPixel);

    mvec = mvec + (uvPixel / float2(gFrameDim)) - uvDiff;

    gMotionOut[pixel] = mvec;
}
