//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
Texture2D<float2> gBackupMotion;
RWTexture2D<float2> gMotionOut;
RWTexture2D<uint> gMotionError;
Texture2D<float3> gCurPos;
Texture2D<float3> gPrevPos;
Texture2D<float> gPosDiff; // estimated max position difference when moving one pixel
Texture2D<float3> gLastRayDir; // direction to Pos (incomming ray direction)
Texture2D<float3> gLastRayDirPrev; // from last frame (for gPrevPos)
Texture2D<uint> gCurPathLen;
Texture2D<uint> gPrevPathLen;
Texture2D<float> gLinearDepth;
Texture2D<float> gLinearDepthPrev;
RWTexture2DArray<float4> gDebugTex;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
    float2 gPrevJitter;
    float2 gCurJitter;
    uint gIterations;
    float gMaxMovement; // max movement in pixels
    bool gForceMotionVectorCalculation;
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

uint2 clampPixel(int2 coord)
{
    return uint2(clamp(coord, 0, int2(gFrameDim) - 1));
}

// non-normalized ray length from the camera ray (previous frame) that lies on the unit-distance plane
float calcNonNormalizedRayLength(float2 uv)
{
    // clones computation from float3 computeNonNormalizedRayDirPinhole(int2 pixel, uint2 frameDim, bool applyJitter = true)
    uv += float2(-gPrevJitter.x, gPrevJitter.y);
    float2 ndc = float2(2, -2) * uv + float2(-1, 1);
    return length(float3(ndc, 1.0));
}

// global constants
static uint2 pixel;
static float2 texC;
static float3 curPos;
static const float2 step = 1.0 / float2(gFrameDim);
static const float2 stepX = float2(step.x, 0.0);
static const float2 stepY = float2(0.0, step.y);

float3 calcProjectedError(float3 Cv, float3 S, float3 Sx, float3 Sy, float l, float lx, float ly, float fracX, float fracY)
{
    float3 dir = normalize(S - Cv) * l;
    float3 dirX = normalize(Sx - Cv) * lx;
    float3 dirY = normalize(Sy - Cv) * ly;
    float3 dirP = curPos - Cv; // non-normalized direction from Cp to P

    float3 dirCur = dir + (dirX - dir) * fracX + (dirY - dir) * fracY;
    float3 err = dirP - dirCur * (length(dirP) / length(dirCur));
    return err;
}

void optimizeMotionVector(float w, float3 Cv, float3 S, float3 Sx, float3 Sy, float l, float lx, float ly, float signDx, float signDy, inout float2x2 AtA, inout float2 Atb)
{
    float3 dir = normalize(S - Cv) * l;
    float3 dirX = normalize(Sx - Cv) * lx;
    float3 dirY = normalize(Sy - Cv) * ly;
    float3 dirP = curPos - Cv; // non-normalized direction from Cp to P

    // project dirP on plane of dir-dirX-dirY
    float3 n = cross(dirX - dir, dirY - dir); // no need to normalize
    // solve: (t * dirP - dir) * n = 0 => t = dot(dir, n) / dot(dirP, n)
    float t = dot(dir, n) / dot(dirP, n);
    dirP = dirP * t;

    float3 I_t = dirP - dir;
    float3 I_x = (dirX - dir) * signDx;
    float3 I_y = (dirY - dir) * signDy;

    AtA += w * float2x2(dot(I_x, I_x), dot(I_x, I_y),
                        dot(I_x, I_y), dot(I_y, I_y));
    Atb += w * float2(dot(I_t, I_x), dot(I_t, I_y));
}

float calcEstimatedError(float2 mvec)
{
    float2 uv = texC + adjustJitter(mvec);
    // 4 rounded uv coordinates
    float2 uv00 = snapTopLeft(uv);
    float2 uv10 = uv00 + stepX;
    float2 uv01 = uv00 + stepY;
    float2 uv11 = uv00 + step;
    // corresponding barycentric weights
    float fracX = (uv.x - uv00.x) / step.x; // [0, 1] distance of uv to uv00 in x direction
    float fracY = (uv.y - uv00.y) / step.y; // [0, 1] distance of uv to uv00 in y direction
    float w00 = (1.0 - fracX) * (1.0 - fracY);
    float w10 = fracX * (1.0 - fracY);
    float w01 = (1.0 - fracX) * fracY;
    float w11 = fracX * fracY;

    // fetch position, direction and depth for the 4 neighboring pixels
    float3 S00 = gPrevPos.SampleLevel(S, uv00, 0.0);
    float3 S10 = gPrevPos.SampleLevel(S, uv10, 0.0);
    float3 S01 = gPrevPos.SampleLevel(S, uv01, 0.0);
    float3 S11 = gPrevPos.SampleLevel(S, uv11, 0.0);
    
    // new approach
    float3 dir00 = gLastRayDirPrev.SampleLevel(S, uv00, 0.0);
    float3 dir10 = gLastRayDirPrev.SampleLevel(S, uv10, 0.0);
    float3 dir01 = gLastRayDirPrev.SampleLevel(S, uv01, 0.0);
    float3 dir11 = gLastRayDirPrev.SampleLevel(S, uv11, 0.0);
    float depth00 = gLinearDepthPrev.SampleLevel(S, uv00, 0.0);
    float depth10 = gLinearDepthPrev.SampleLevel(S, uv10, 0.0);
    float depth01 = gLinearDepthPrev.SampleLevel(S, uv01, 0.0);
    float depth11 = gLinearDepthPrev.SampleLevel(S, uv11, 0.0);
    float l00 = calcNonNormalizedRayLength(uv00);
    float l10 = calcNonNormalizedRayLength(uv10);
    float l01 = calcNonNormalizedRayLength(uv01);
    float l11 = calcNonNormalizedRayLength(uv11);
    //l00 = l10 = l01 = l11 = 1.0;
    // calulate common camera position
    float3 cam00 = S00 - dir00 * depth00;
    float3 cam10 = S10 - dir10 * depth10;
    float3 cam01 = S01 - dir01 * depth01;
    float3 cam11 = S11 - dir11 * depth11;
    float3 err00 = calcProjectedError(cam00, S00, S10, S01, l00, l10, l01, fracX, fracY);
    float3 err10 = calcProjectedError(cam10, S10, S00, S11, l10, l00, l11, 1.0 - fracX, fracY);
    float3 err01 = calcProjectedError(cam01, S01, S11, S00, l01, l11, l00, fracX, 1.0 - fracY);
    float3 err11 = calcProjectedError(cam11, S11, S01, S10, l11, l01, l10, 1.0 - fracX, 1.0 - fracY);

    // bilinear interpolation
    float3 err = w00 * err00 + w10 * err10 + w01 * err01 + w11 * err11;
    //gDebugTex[uint3(pixel, 0)] = float4(err, 1.0);
    return length(err);
}


[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    pixel = id.xy;
    texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 bmvec = gBackupMotion[pixel];
    const uint reqPathLen = gCurPathLen[pixel]; // required path length: other positions should be taken from this exact path length, otherwise they are likeley invalid
    curPos = gCurPos.SampleLevel(S, texC, 0.0);

    uint iterations = gIterations;

    if (!gForceMotionVectorCalculation) // skip iterations in trivial cases
    {
        // if (all(mvec == bmvec)) iterations = 0; // backup will always be correct under static assumptions
        if (all(mvec == 0.0)) iterations = 0; // force nothing moved
        if (reqPathLen <= 1) iterations = 0; // path length 1 is trival reprojection => force correct
    }
   
    // compare with backup motion vector (sometimes better)
    if (calcEstimatedError(bmvec) < calcEstimatedError(mvec))
        mvec = bmvec;

    //iterations = 0;
    const float2 originalMvec = mvec; // mvec from before iterations
    for (uint i = 0; i < iterations; ++i)
    {
        float2 mvec0 = adjustJitter(mvec); // adjust jitter of motion vector
        float2 uvPrev = texC + mvec0; // actual uv
        float2 uv00 = snapTopLeft(texC + mvec0); // snap uv to top left (pixel center)
        int2 uv00pixel = int2(uv00 * float2(gFrameDim));

        // pixel coordinates
        int2 uv10pixel = clampPixel(uv00pixel + int2(1, 0));
        int2 uv01pixel = clampPixel(uv00pixel + int2(0, 1));
        int2 uv11pixel = clampPixel(uv00pixel + int2(1, 1));
        uv00pixel = clampPixel(uv00pixel);

        // fetch path lengths
        uint prevPathLen00 = gPrevPathLen[uv00pixel];
        uint prevPathLen10 = gPrevPathLen[uv10pixel];
        uint prevPathLen01 = gPrevPathLen[uv01pixel];
        uint prevPathLen11 = gPrevPathLen[uv11pixel];

        // verify same path length
        if(prevPathLen00 != reqPathLen || prevPathLen10 != reqPathLen || prevPathLen01 != reqPathLen || prevPathLen11 != reqPathLen) break; // path length must be consistent

        // 4 rounded uv coordinates
        //float2 uv00 = snapTopLeft(texC + mvec0); // declared on top
        float2 uv10 = uv00 + stepX;
        float2 uv01 = uv00 + stepY;
        float2 uv11 = uv00 + step;
        // corresponding barycentric weights
        float fracX = (texC.x + mvec0.x - uv00.x) / step.x; // [0, 1] distance of uv to uv00 in x direction
        float fracY = (texC.y + mvec0.y - uv00.y) / step.y; // [0, 1] distance of uv to uv00 in y direction
        float w00 = (1.0 - fracX) * (1.0 - fracY);
        float w10 = fracX * (1.0 - fracY);
        float w01 = (1.0 - fracX) * fracY;
        float w11 = fracX * fracY;

        // fetch position, direction and depth for the 4 neighboring pixels
        float3 S00 = gPrevPos[uv00pixel];
        float3 S10 = gPrevPos[uv10pixel];
        float3 S01 = gPrevPos[uv01pixel];
        float3 S11 = gPrevPos[uv11pixel];
        float3 dir00 = gLastRayDirPrev[uv00pixel];
        float3 dir10 = gLastRayDirPrev[uv10pixel];
        float3 dir01 = gLastRayDirPrev[uv01pixel];
        float3 dir11 = gLastRayDirPrev[uv11pixel];
        float depth00 = gLinearDepthPrev[uv00pixel];
        float depth10 = gLinearDepthPrev[uv10pixel];
        float depth01 = gLinearDepthPrev[uv01pixel];
        float depth11 = gLinearDepthPrev[uv11pixel];
        float l00 = calcNonNormalizedRayLength(uv00);
        float l10 = calcNonNormalizedRayLength(uv10);
        float l01 = calcNonNormalizedRayLength(uv01);
        float l11 = calcNonNormalizedRayLength(uv11);
        //l00 = l10 = l01 = l11 = 1.0;
        // calulate common camera position
        float3 cam00 = S00 - dir00 * depth00;
        float3 cam10 = S10 - dir10 * depth10;
        float3 cam01 = S01 - dir01 * depth01;
        float3 cam11 = S11 - dir11 * depth11;

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;
        optimizeMotionVector(w00, cam00, S00, S10, S01, l00, l10, l01, 1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w10, cam10, S10, S00, S11, l10, l00, l11, -1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w01, cam01, S01, S11, S00, l01, l11, l00, 1.0, -1.0, AtA, Atb);
        optimizeMotionVector(w11, cam11, S11, S01, S10, l11, l01, l10, -1.0, -1.0, AtA, Atb);
        
        if (all(AtA == 0.0)) break; // bad conditioned, retain original motion vector

        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        if (abs(det) < 1e-16) break; // bad conditioned, retain original motion vector

        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        float2 uvPixel = mul(invAtA, Atb);
        if (length(uvPixel) > gMaxMovement) // restrict pixel step size
            uvPixel = gMaxMovement * normalize(uvPixel);

        float2 uvDiff = uvPixel / float2(gFrameDim);

        float curError = calcEstimatedError(mvec);
        float newError = curError;

        // backtracking line search
        float alpha = 1.0;
        for (; alpha > 0.1; alpha *= 0.5)
        {
            newError = calcEstimatedError(mvec + alpha * uvDiff);
            if (newError < curError) break; // use motion vector with this alpha
        }
        if (newError >= curError) break; // cannot improve anymore
        
        mvec = mvec + alpha * uvDiff;
    }

    // verify error
    float newError = calcEstimatedError(mvec);
    //newError = 0.0;
    //iterations = 1;
    float onePixelPosDiff = gPosDiff[pixel];
    uint error = 0;
    if (iterations > 0 && (newError > onePixelPosDiff * 0.05))
    {
        mvec = originalMvec;
        error = 1;
    }

    gMotionError[pixel] = error;
    gMotionOut[pixel] = mvec;
}
