//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
RWTexture2D<float2> gMotionOut;
Texture2D<float3> gCurPos;
Texture2D<float3> gPrevPos;
Texture2D<float> gPosDiff;       // estimated max position difference when moving one pixel
Texture2D<float3> gLastRayDir;   // direction to Pos (incomming ray direction)

#ifndef PROJECT_TO_TANGENT
#define PROJECT_TO_TANGENT 1
#endif

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
    float2 gPrevJitter;
    float2 gCurJitter;
    uint gIterations;
    float gMaxMovement; // max movement in pixels
    int gWindowRadius;  // radius in pixels (1 = default)
    float3 camPos; // camera position current frame
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
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    const float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
    const float2 mvec_backup = mvec; // original value

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    #if PROJECT_TO_TANGENT
    float3 tangent = normalize(cross(float3(0.0, 1.0, 0.0), gLastRayDir[pixel]));
    float3 bitangent = cross(gLastRayDir[pixel], tangent);
    float2x3 TB = float2x3(tangent, bitangent);
    #endif

    for (uint i = 0; i < gIterations; ++i)
    {
        float2 mvec0 = adjustJitter(mvec); // adjust jitter of motion vector
        float2 uvPrev = texC + mvec0; // actual uv
        float2 uv00 = snapTopLeft(texC + mvec0); // snap uv to top left (pixel center)

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

        // for radius=1 => 2x2: dx = 0, 1; dy = 0, 1
        // for radius=2 => 4x4: dx = -1,0,1,2; dy = -1,0,1,2
        for (int dx = -gWindowRadius + 1; dx <= gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy <= gWindowRadius; ++dy)
            {
                float2 off = dx * stepX + dy * stepY;
                float3 prevPos = gPrevPos.SampleLevel(S, uv00 + off, 0.0);

                // build derivatives
                float3 I_t = curPos - prevPos; // [t+1] - [t]
                float3 I_x = getMinDeriv(prevPos, uv00 + off, stepX);
                float3 I_y = getMinDeriv(prevPos, uv00 + off, stepY);

                float2 distInPixels = abs(uv00 + off - uvPrev) * float2(gFrameDim);
                float w = 1.0;
                w *= 1.0 - (distInPixels.x / gWindowRadius); // barycentric weights for radius=1
                w *= 1.0 - (distInPixels.y / gWindowRadius);

                #if PROJECT_TO_TANGENT
                I_x.xy = mul(TB, I_x);
                I_y.xy = mul(TB, I_y);
                I_t.xy = mul(TB, I_t);
                I_x.z = I_y.z = I_t.z = 0.0;
                #endif

                AtA += w * float2x2(dot(I_x, I_x), dot(I_x, I_y),
                                    dot(I_x, I_y), dot(I_y, I_y));
                Atb += w * float2(dot(I_t, I_x), dot(I_t, I_y));
                
            }

        if (all(AtA == 0.0)) break; // bad conditioned, retain original motion vector

        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        if (det == 0.0) break; // bad conditioned, retain original motion vector

        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        // float2 uvPixel = solveLinearEq(I_x, I_y, I_t);
        float2 uvPixel = mul(invAtA, Atb);

        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);

        mvec = mvec + (uvPixel / float2(gFrameDim));
    }

    // verify error
    float3 newPos = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0);
    float newError = distance(curPos, newPos);
    float onePixelPosDiff = gPosDiff[pixel];
    
    if (newError > onePixelPosDiff * 0.1) mvec = mvec_backup;

    gMotionOut[pixel] = mvec;
}

// derivative of normalized direction d_unit
float3 calcdd_unit(float3 /*not normalized*/ d, float3 /*not normalized*/ dd)
{
    float dDotd = dot(d, d);
    return (dDotd * dd - dot(d, dd) * d) / (pow(dDotd, 1.5));
}

[numthreads(16, 16, 1)]
void angleMain(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint3 localId: SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    const float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
    const float3 w_unit = normalize(curPos - camPos); // reference direction
    const float2 mvec_backup = mvec; // original value

    const float2 step = 1.0 / float2(gFrameDim);
    const float2 stepX = float2(step.x, 0.0);
    const float2 stepY = float2(0.0, step.y);

    // calculate current Target function value
    float Tcur = dot(w_unit, normalize(gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0) - camPos));

    for (uint i = 0; i < gIterations; ++i)
    {
        float2 mvec0 = adjustJitter(mvec); // adjust jitter of motion vector
        float2 uv0 = texC + mvec0;

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

        for (int dx = -gWindowRadius + 1; dx < gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy < gWindowRadius; ++dy)
            {
                float2 off = dx * stepX + dy * stepY;
                float3 prevPos = gPrevPos.SampleLevel(S, uv0 + off, 0.0);

                float3 d = prevPos - camPos;
                float3 d_unit = normalize(d);

                // target function
                float T = dot(w_unit, d_unit);
                // float3 ddx = gPrevPos.SampleLevel(S, uv0 + off + stepX, 0.0) - gPrevPos.SampleLevel(S, uv0 + off - stepX, 0.0);
                //float3 ddx = gPrevPos.SampleLevel(S, uv0 + off + stepX, 0.0) - prevPos;
                //float3 ddy = gPrevPos.SampleLevel(S, uv0 + off + stepY, 0.0) - prevPos;
                float3 ddx = getMinDeriv(prevPos, uv0 + off, stepX);
                float3 ddy = getMinDeriv(prevPos, uv0 + off, stepY);
                float t_x = dot(w_unit, calcdd_unit(d, ddx));
                float t_y = dot(w_unit, calcdd_unit(d, ddy));

                float w = 1.0;
                w *= exp(-0.5 * (dx * dx + dy * dy) / 50.0);
                if (T < Tcur - 0.01) w = 0.0; // ignore pixels with far worse target function
                //float maxPosDiff = 2.0 * gPosDiff.SampleLevel(S, texC, 0.0);
                //if (length(ddx) > maxPosDiff || length(ddy) > maxPosDiff)
                //    w = 0.0;

                AtA += w * float2x2(t_x * t_x, t_x * t_y,
                                    t_x * t_y, t_y * t_y);
                Atb += w * float2((1.0 - T) * t_x, (1.0 - T) * t_y);
            }

        if (all(AtA == 0.0)) break; // bad conditioned, retain original motion vector

        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        if (det == 0.0) break; // bad conditioned, retain original motion vector

        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        float2 uvPixel = mul(invAtA, Atb);

        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);

        // only use nextMvec if it improves the target function
        float alpha = 1.0;
        float Tnext = -1.0;
        float2 nextMvec = 0.0;
        while (Tnext < Tcur)
        {
            if (alpha < 0.01) break; // give up (~7 tries)
            nextMvec = mvec + alpha * (uvPixel / float2(gFrameDim));
            Tnext = dot(w_unit, normalize(gPrevPos.SampleLevel(S, texC + adjustJitter(nextMvec), 0.0) - camPos));
            alpha *= 0.5;
        }
        Tcur = Tnext;
        mvec = nextMvec;
    }

    // verify error
    float3 newPos = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0);
    float newError = distance(curPos, newPos);
    float onePixelPosDiff = gPosDiff[pixel];

    if (newError > onePixelPosDiff * 0.1) mvec = mvec_backup;
    //if (Tcur < 0.9999) mvec = mvec_backup;

    gMotionOut[pixel] = mvec;
}
