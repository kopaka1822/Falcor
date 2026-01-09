//import Utils.Math.MathHelpers;

RWTexture2D<float2> gMotion;
Texture2D<float3> gCurColor;
Texture2D<float3> gPrevColor;

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
float3 getMinDeriv(float3 prevColor, float2 uvPrev, float2 step)
{
    // [x + 1] - [x]
    float3 deriv1 = gPrevColor.SampleLevel(S, uvPrev + step, 0.0) - prevColor;
    //return deriv1;
    float3 deriv2 = prevColor - gPrevColor.SampleLevel(S, uvPrev - step, 0.0);
    return (length(deriv1) <= length(deriv2)) ? deriv1 : deriv2;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    for (uint i = 0; i < gIterations; ++i)
    {
        float2 mvec0 = mvec - gPrevJitter + gCurJitter; // adjust jitter of motion vector
        //float2 mvec0 = mvec + gCurJitter;
        float2 uv0 = texC + mvec0;

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

        for (int dx = -gWindowRadius + 1; dx < gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy < gWindowRadius; ++dy)
            {
                float2 off = dx * stepX + dy * stepY;
                float3 curColor = gCurColor.SampleLevel(S, texC + off, 0.0);
                float3 prevColor = gPrevColor.SampleLevel(S, uv0 + off, 0.0);

                // build derivatives
                float3 I_t = curColor - prevColor; // [t+1] - [t]
                float3 I_x = getMinDeriv(prevColor, uv0 + off, stepX);
                float3 I_y = getMinDeriv(prevColor, uv0 + off, stepY);

                // matrix components
                float I_xSq = dot(I_x, I_x);
                float I_ySq = dot(I_y, I_y);
                float I_xy = dot(I_x, I_y);

                float trace = I_xSq + I_ySq; // A[0][0] + A[1][1]
                float det = I_xSq * I_ySq - I_xy * I_xy; // determinant
                // eigenvalues
                float sqrtDisc = sqrt(max(0.0, trace * trace - 4.0 * det));
                float lambda1 = 0.5 * (trace + sqrtDisc);
                float lambda2 = 0.5 * (trace - sqrtDisc); // lambda1 >= lambda2 (by construction)

                const float lambdaMin = 1e-8;
                const float anisotropyMax = 50.0;
                const float lambdaMax = 1e8;

                // test if well conditioned
                //if (lambda1 <= lambdaMax && lambda2 >= lambdaMin && lambda1 <= lambda2 * anisotropyMax)
                {
                    // update matrices
                    AtA[0][0] += I_xSq;
                    AtA[0][1] += I_xy;
                    AtA[1][0] += I_xy;
                    AtA[1][1] += I_ySq;
                    // use positive sign because our motion vector goes from current to previous
                    Atb[0] += dot(I_x, I_t);
                    Atb[1] += dot(I_y, I_t);
                } //else mvec = 0.0;
            }

        if (all(AtA == 0.0)) break; // bad conditioned, retain original motion vector

        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        float2 uvPixel = mul(invAtA, Atb);

        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);

        mvec = mvec + (uvPixel / float2(gFrameDim));
    }

    gMotion[pixel] = mvec;
}
