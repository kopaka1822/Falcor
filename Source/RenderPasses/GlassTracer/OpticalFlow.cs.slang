//import Utils.Math.MathHelpers;

RWTexture2D<float2> gMotion;
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
    float2 mvec = gMotion[pixel];

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    for (uint i = 0; i < gIterations; ++i)
    {
        float2 mvec0 = mvec - gPrevJitter + gCurJitter; // adjust jitter of motion vector
        float2 uv0 = texC + mvec0;
        float2 uvSnap = (floor(uv0 * float2(gFrameDim)) + 0.5) / float2(gFrameDim);
        float2 uvDiff = uv0 - uvSnap; // uvSnap + uvDiff = uv0

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

        for (int dx = -gWindowRadius + 1; dx < gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy < gWindowRadius; ++dy)
            {
                float2 off = dx * stepX + dy * stepY;
                float3 curPos = gCurPos.SampleLevel(S, texC + off, 0.0);
                float3 prevPos = gPrevPos.SampleLevel(S, uvSnap + off, 0.0);

                // build derivatives
                float3 I_t = curPos - prevPos; // [t+1] - [t]
                float3 I_x = getMinDeriv(prevPos, uvSnap + off, stepX);
                float3 I_y = getMinDeriv(prevPos, uvSnap + off, stepY);

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
                const float anisotropyMax = 1000.0;
                const float lambdaMax = 1e8;

                // test if well conditioned
                if (lambda1 <= lambdaMax && lambda2 >= lambdaMin && lambda1 <= lambda2 * anisotropyMax)
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

        mvec = mvec + (uvPixel / float2(gFrameDim)) - uvDiff;
    }

    gMotion[pixel] = mvec;
}
