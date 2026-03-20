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
static float2 xy;
static float3 P;
static const float2 step = 1.0 / float2(gFrameDim);
static const float2 stepX = float2(step.x, 0.0);
static const float2 stepY = float2(0.0, step.y);
static uint reqPathLen;

float3 calcProjectedError(float3 Cp, float3 Pp, float3 Ppx, float3 Ppy, float l, float lx, float ly, float s, float t)
{
    float3 omega = normalize(Pp - Cp) * l;
    float3 omegaX = normalize(Ppx - Cp) * lx;
    float3 omegaY = normalize(Ppy - Cp) * ly;
    float3 omegaP = P - Cp; // non-normalized direction from Cp to P

    float3 omegaST = omega + (omegaX - omega) * s + (omegaY - omega) * t;
    float3 err = omegaP - omegaST * (length(omegaP) / length(omegaST));
    return err;
}

void optimizeMotionVector(float w, float3 Cp, float3 Pp, float3 Ppx, float3 Ppy, float l, float lx, float ly, float signDu, float signDv, inout float2x2 AtA, inout float2 Atb)
{
    float3 omega = normalize(Pp - Cp) * l;
    float3 omegaX = normalize(Ppx - Cp) * lx;
    float3 omegaY = normalize(Ppy - Cp) * ly;
    float3 omegaP = P - Cp; // non-normalized direction from Cp to P

    // project omegaP on plane of omega-omegaX-omegaY
    float3 n = cross(omegaX - omega, omegaY - omega); // no need to normalize
    // solve: (t * omegaP - omega) * n = 0 => t = dot(omega, n) / dot(omegaP, n)
    float t = dot(omega, n) / dot(omegaP, n);
    omegaP = omegaP * t; // omegaP on image plane

    float3 b = omegaP - omega;
    float3 ax = (omegaX - omega) * signDu;
    float3 ay = (omegaY - omega) * signDv;

    AtA += w * float2x2(dot(ax, ax), dot(ax, ay),
                        dot(ax, ay), dot(ay, ay));
    Atb += w * float2(dot(b, ax), dot(b, ay));
}

float calcEstimatedError(float2 mvec)
{
    float2 uv = xy + adjustJitter(mvec);
    // 4 rounded uv coordinates
    float2 uv00 = snapTopLeft(uv);
    float2 uv10 = uv00 + stepX;
    float2 uv01 = uv00 + stepY;
    float2 uv11 = uv00 + step;

    // pixel coordinates
    int2 uv00pixel = int2(uv00 * float2(gFrameDim));
    int2 uv10pixel = clampPixel(uv00pixel + int2(1, 0));
    int2 uv01pixel = clampPixel(uv00pixel + int2(0, 1));
    int2 uv11pixel = clampPixel(uv00pixel + int2(1, 1));
    uv00pixel = clampPixel(uv00pixel);

    // corresponding barycentric weights
    float s = (uv.x - uv00.x) / step.x; // [0, 1] distance of uv to uv00 in x direction
    float t = (uv.y - uv00.y) / step.y; // [0, 1] distance of uv to uv00 in y direction
    float w00 = (1.0 - s) * (1.0 - t);
    float w10 = s * (1.0 - t);
    float w01 = (1.0 - s) * t;
    float w11 = s * t;

    uint prevPathLen00 = gPrevPathLen[uv00pixel];
    uint prevPathLen10 = gPrevPathLen[uv10pixel];
    uint prevPathLen01 = gPrevPathLen[uv01pixel];
    uint prevPathLen11 = gPrevPathLen[uv11pixel];
    // verify same path length (othewise an invalid, possibly too low, error is estimated which can result in large motion vectors being allowed)
    if (prevPathLen00 != reqPathLen || prevPathLen10 != reqPathLen || prevPathLen01 != reqPathLen || prevPathLen11 != reqPathLen) return 3.402823466e+38F; // path length must be consistent => return max error

    // fetch position, direction and depth for the 4 neighboring pixels
    float3 Pp00 = gPrevPos.SampleLevel(S, uv00, 0.0);
    float3 Pp10 = gPrevPos.SampleLevel(S, uv10, 0.0);
    float3 Pp01 = gPrevPos.SampleLevel(S, uv01, 0.0);
    float3 Pp11 = gPrevPos.SampleLevel(S, uv11, 0.0);
    float3 dp00 = gLastRayDirPrev.SampleLevel(S, uv00, 0.0);
    float3 dp10 = gLastRayDirPrev.SampleLevel(S, uv10, 0.0);
    float3 dp01 = gLastRayDirPrev.SampleLevel(S, uv01, 0.0);
    float3 dp11 = gLastRayDirPrev.SampleLevel(S, uv11, 0.0);
    float tp00 = gLinearDepthPrev.SampleLevel(S, uv00, 0.0);
    float tp10 = gLinearDepthPrev.SampleLevel(S, uv10, 0.0);
    float tp01 = gLinearDepthPrev.SampleLevel(S, uv01, 0.0);
    float tp11 = gLinearDepthPrev.SampleLevel(S, uv11, 0.0);
    float l00 = calcNonNormalizedRayLength(uv00);
    float l10 = calcNonNormalizedRayLength(uv10);
    float l01 = calcNonNormalizedRayLength(uv01);
    float l11 = calcNonNormalizedRayLength(uv11);
    // calulate virtual camera position
    float3 Cp00 = Pp00 - dp00 * tp00;
    float3 Cp10 = Pp10 - dp10 * tp10;
    float3 Cp01 = Pp01 - dp01 * tp01;
    float3 Cp11 = Pp11 - dp11 * tp11;
    float3 pe00 = calcProjectedError(Cp00, Pp00, Pp10, Pp01, l00, l10, l01, s, t);
    float3 pe10 = calcProjectedError(Cp10, Pp10, Pp00, Pp11, l10, l00, l11, 1.0 - s, t);
    float3 pe01 = calcProjectedError(Cp01, Pp01, Pp11, Pp00, l01, l11, l00, s, 1.0 - t);
    float3 pe11 = calcProjectedError(Cp11, Pp11, Pp01, Pp10, l11, l01, l10, 1.0 - s, 1.0 - t);

    // bilinear interpolation
    float3 pe = w00 * pe00 + w10 * pe10 + w01 * pe01 + w11 * pe11;
    return length(pe);
}


[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    pixel = id.xy;
    xy = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 bmvec = gBackupMotion[pixel];
    reqPathLen = gCurPathLen[pixel]; // required path length: other positions should be taken from this exact path length, otherwise they are likeley invalid
    P = gCurPos.SampleLevel(S, xy, 0.0);

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
    for (uint i = 0; i < iterations; ++i)
    {
        float2 mvec0 = adjustJitter(mvec); // adjust jitter of motion vector
        float2 uvPrev = xy + mvec0; // actual uv
        float2 uv00 = snapTopLeft(xy + mvec0); // snap uv to top left (pixel center)
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
        //float2 uv00 = snapTopLeft(xy + mvec0); // declared on top
        float2 uv10 = uv00 + stepX;
        float2 uv01 = uv00 + stepY;
        float2 uv11 = uv00 + step;
        // corresponding barycentric weights
        float s = (xy.x + mvec0.x - uv00.x) / step.x; // [0, 1] distance of uv to uv00 in x direction
        float t = (xy.y + mvec0.y - uv00.y) / step.y; // [0, 1] distance of uv to uv00 in y direction
        float w00 = (1.0 - s) * (1.0 - t);
        float w10 = s * (1.0 - t);
        float w01 = (1.0 - s) * t;
        float w11 = s * t;

        // fetch position, direction and depth for the 4 neighboring pixels
        float3 Pp00 = gPrevPos[uv00pixel];
        float3 Pp10 = gPrevPos[uv10pixel];
        float3 Pp01 = gPrevPos[uv01pixel];
        float3 Pp11 = gPrevPos[uv11pixel];
        float3 dp00 = gLastRayDirPrev[uv00pixel];
        float3 dp10 = gLastRayDirPrev[uv10pixel];
        float3 dp01 = gLastRayDirPrev[uv01pixel];
        float3 dp11 = gLastRayDirPrev[uv11pixel];
        float tp00 = gLinearDepthPrev[uv00pixel];
        float tp10 = gLinearDepthPrev[uv10pixel];
        float tp01 = gLinearDepthPrev[uv01pixel];
        float tp11 = gLinearDepthPrev[uv11pixel];
        float l00 = calcNonNormalizedRayLength(uv00);
        float l10 = calcNonNormalizedRayLength(uv10);
        float l01 = calcNonNormalizedRayLength(uv01);
        float l11 = calcNonNormalizedRayLength(uv11);
        //l00 = l10 = l01 = l11 = 1.0;
        // calulate common camera position
        float3 Cp00 = Pp00 - dp00 * tp00;
        float3 Cp10 = Pp10 - dp10 * tp10;
        float3 Cp01 = Pp01 - dp01 * tp01;
        float3 Cp11 = Pp11 - dp11 * tp11;

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;
        optimizeMotionVector(w00, Cp00, Pp00, Pp10, Pp01, l00, l10, l01, 1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w10, Cp10, Pp10, Pp00, Pp11, l10, l00, l11, -1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w01, Cp01, Pp01, Pp11, Pp00, l01, l11, l00, 1.0, -1.0, AtA, Atb);
        optimizeMotionVector(w11, Cp11, Pp11, Pp01, Pp10, l11, l01, l10, -1.0, -1.0, AtA, Atb);
        
        if (all(AtA == 0.0)) break; // bad conditioned, retain original motion vector

        // invert AtA
        float det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
        if (abs(det) < 1e-16) break; // bad conditioned, retain original motion vector

        float2x2 invAtA = float2x2(AtA[1][1], -AtA[0][1],
                               -AtA[1][0], AtA[0][0]) / det;

        float2 uvPixel = mul(invAtA, Atb);
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
    float deltap = gPosDiff[pixel];
    uint error = 0;
    if (iterations > 0 && (newError > deltap * 0.05))
    {
        mvec = bmvec; // store backup motion
        error = 1;
    }
    //mvec = bmvec;
    gMotionError[pixel] = error;
    gMotionOut[pixel] = mvec;
}
