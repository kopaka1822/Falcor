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

float3 getAvgDeriv(float3 curPos, float3 prevPos, float2 texC, float2 mvec, float2 step)
{
    float3 curPos2 = gCurPos.SampleLevel(S, texC + step, 0.0);
    float3 prevPos2 = gPrevPos.SampleLevel(S, texC + mvec + step, 0.0);
    return 0.5 * ((curPos2 - curPos) + (prevPos2 - prevPos));
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
        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

        for (int dx = -gWindowRadius + 1; dx < gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy < gWindowRadius; ++dy)
            {
                float2 off = dx * stepX + dy * stepY;
                float3 curPos = gCurPos.SampleLevel(S, texC + off, 0.0);
                float3 prevPos = gPrevPos.SampleLevel(S, texC + off + mvec0, 0.0);

                // build derivatives
                float3 I_t = curPos - prevPos; // [t+1] - [t]
                float3 I_x = getAvgDeriv(curPos, prevPos, texC + off, mvec0, stepX);
                float3 I_y = getAvgDeriv(curPos, prevPos, texC + off, mvec0, stepY);

                // update matrices
                AtA[0][0] += dot(I_x, I_x);
                AtA[0][1] += dot(I_x, I_y);
                AtA[1][0] += dot(I_x, I_y);
                AtA[1][1] += dot(I_y, I_y);
                // use positive sign because our motion vector goes from current to previous
                Atb[0] += dot(I_x, I_t);
                Atb[1] += dot(I_y, I_t);
            }


        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        // TODO check for singularity
        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        float2 uvPixel = mul(invAtA, Atb);
        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);
        
        mvec = mvec + (uvPixel / float2(gFrameDim));
    }

    gMotion[pixel] = mvec;
}
