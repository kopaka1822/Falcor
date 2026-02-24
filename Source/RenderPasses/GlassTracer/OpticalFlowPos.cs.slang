//import Utils.Math.MathHelpers;

Texture2D<float2> gMotion;
Texture2D<float2> gBackupMotion;
RWTexture2D<float2> gMotionOut;
RWTexture2D<uint> gMotionError;
Texture2D<float3> gCurPos;
Texture2D<float3> gPrevPos;
Texture2D<float> gPosDiff; // estimated max position difference when moving one pixel
Texture2D<float> gPrevPosDiff; // from last frame (for gPrevPos)
Texture2D<float3> gLastRayDir; // direction to Pos (incomming ray direction)
Texture2D<float3> gLastRayDirPrev; // from last frame (for gPrevPos)
Texture2D<uint> gCurPathLen;
Texture2D<uint> gPrevPathLen;
Texture2D<float> gLinearDepth;
Texture2D<float> gLinearDepthPrev;
RWTexture2DArray<float4> gDebugTex;

#define OPT_MODE_SIMPLE_TANGENT 0
#define OPT_MODE_NORMALIZED_DERIVS 1
#define OPT_MODE_VIRTUAL_CAMERA 2

#define OPT_MODE 2

#ifndef PROJECT_TO_TANGENT
#define PROJECT_TO_TANGENT 1
#endif

#if PROJECT_TO_TANGENT
#define PROJECT(v) v.xy = mul(TB, v); v.z = 0.0;
#else
#define PROJECT(v)
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
    float3 camPos;      // camera position current frame
    bool gForceMotionVectorCalculation;
    bool gCompareWithBackupMotion;
}

// get derivative with smallest magnitude (+step/-step)
float3 getMinDeriv(float3 prevPos, int2 uvPrev, int2 step, uint reqPathLen, inout bool invalidDiffs)
{
    // [x + 1] - [x]
    float3 deriv1 = gPrevPos[clampPixel(uvPrev + step)] - prevPos;
    bool valid1 = (gPrevPathLen[clampPixel(uvPrev + step)] == reqPathLen);
    float3 deriv2 = prevPos - gPrevPos[clampPixel(uvPrev - step)];
    bool valid2 = (gPrevPathLen[clampPixel(uvPrev - step)] == reqPathLen);

    // only return derivatives from the same path length as prevPos
    if (valid1 && !valid2) return deriv1;
    if (!valid1 && valid2) return deriv2;
    if (!valid1 && !valid2) invalidDiffs = true;
    return (length(deriv1) <= length(deriv2)) ? deriv1 : deriv2;
}

// returns closest position to prevPos from either +step or -step. Fourth component is 1 if +step is close, -1 if -step is closer.
float4 getMinDerivPos(float3 prevPos, int2 uvPrev, int2 step, uint reqPathLen, inout bool invalidDiffs)
{
    float3 pos1 = gPrevPos[clampPixel(uvPrev + step)];
    bool valid1 = (gPrevPathLen[clampPixel(uvPrev + step)] == reqPathLen);
    float3 pos2 = gPrevPos[clampPixel(uvPrev - step)];
    bool valid2 = (gPrevPathLen[clampPixel(uvPrev - step)] == reqPathLen);

    // only return derivatives from the same path length as prevPos
    if (valid1 && !valid2) return float4(pos1, 1.0);
    if (!valid1 && valid2) return float4(pos2, -1.0);
    if (!valid1 && !valid2) invalidDiffs = true;

    return (length(pos1 - prevPos) <= length(prevPos - pos2)) ? float4(pos1, 1.0) : float4(pos2, -1.0);
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

float3x3 inverse3x3(float3x3 M)
{
    float a00 = M[0][0], a01 = M[0][1], a02 = M[0][2];
    float a10 = M[1][0], a11 = M[1][1], a12 = M[1][2];
    float a20 = M[2][0], a21 = M[2][1], a22 = M[2][2];

    float b00 = a11 * a22 - a12 * a21;
    float b01 = a02 * a21 - a01 * a22;
    float b02 = a01 * a12 - a02 * a11;

    float b10 = a12 * a20 - a10 * a22;
    float b11 = a00 * a22 - a02 * a20;
    float b12 = a02 * a10 - a00 * a12;

    float b20 = a10 * a21 - a11 * a20;
    float b21 = a01 * a20 - a00 * a21;
    float b22 = a00 * a11 - a01 * a10;

    float det = a00 * b00 + a01 * b10 + a02 * b20;
    float invDet = 1.0 / det;

    return float3x3(
        b00 * invDet, b01 * invDet, b02 * invDet,
        b10 * invDet, b11 * invDet, b12 * invDet,
        b20 * invDet, b21 * invDet, b22 * invDet
    );
}

float3x3 outerProduct(float3 a, float3 b)
{
    return float3x3(
        a.x * b.x, a.x * b.y, a.x * b.z,
        a.y * b.x, a.y * b.y, a.y * b.z,
        a.z * b.x, a.z * b.y, a.z * b.z
    );
}

// from: https://math.stackexchange.com/questions/61719/finding-the-intersection-point-of-many-lines-in-3d-point-closest-to-all-lines ~ Alberto
float3 ClosestPointTo4Lines(
    float3 S00, float3 d00, float w00,
    float3 S10, float3 d10, float w10,
    float3 S01, float3 d01, float w01,
    float3 S11, float3 d11, float w11
)
{
    float3x3 I = float3x3(
        1, 0, 0,
        0, 1, 0,
        0, 0, 1
    );

    d00 = normalize(d00);
    d10 = normalize(d10);
    d01 = normalize(d01);
    d11 = normalize(d11);

    float3x3 A =
        w00 * (I - outerProduct(d00, d00)) +
        w10 * (I - outerProduct(d10, d10)) +
        w01 * (I - outerProduct(d01, d01)) +
        w11 * (I - outerProduct(d11, d11));

    float3 b =
        w00 * mul(I - outerProduct(d00, d00), S00) +
        w10 * mul(I - outerProduct(d10, d10), S10) +
        w01 * mul(I - outerProduct(d01, d01), S01) +
        w11 * mul(I - outerProduct(d11, d11), S11);

    // float lambda = 1e-8;
    // A += lambda * I;

    return mul(inverse3x3(A), b);
}

// global constants
static uint2 pixel;
static float2 texC;
static float3 curPos;
static float2x3 TB;
static float3 w_i; // incomming ray direction on P
static float linearDepth;
static float3 cameraVirtual;
static const float2 step = 1.0 / float2(gFrameDim);
static const float2 stepX = float2(step.x, 0.0);
static const float2 stepY = float2(0.0, step.y);


#if OPT_MODE == OPT_MODE_VIRTUAL_CAMERA


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
    //float3 Cv = (cam00 + cam10 + cam01 + cam11) * 0.25;
    float3 Cv = w00 * cam00 + w10 * cam10 + w01 * cam01 + w11 * cam11;
    //float3 Cv = ClosestPointTo4Lines(S00, dir00, w00, S10, dir10, w10, S01, dir01, w01, S11, dir11, w11);
    //gDebugTex[uint3(pixel, 0)] = float4(w00 * cam00 + w10 * cam10 + w01 * cam01 + w11 * cam11, 1.0);
    //gDebugTex[uint3(pixel, 1)] = float4(ClosestPointTo4Lines(S00, dir00, w00, S10, dir10, w10, S01, dir01, w01, S11, dir11, w11), 1.0);

    float3 err00 = calcProjectedError(Cv, S00, S10, S01, l00, l10, l01, fracX, fracY);
    float3 err10 = calcProjectedError(Cv, S10, S00, S11, l10, l00, l11, 1.0 - fracX, fracY);
    float3 err01 = calcProjectedError(Cv, S01, S11, S00, l01, l11, l00, fracX, 1.0 - fracY);
    float3 err11 = calcProjectedError(Cv, S11, S01, S10, l11, l01, l10, 1.0 - fracX, 1.0 - fracY);

    // bilinear interpolation
    float3 err = w00 * err00 + w10 * err10 + w01 * err01 + w11 * err11;
    gDebugTex[uint3(pixel, 0)] = float4(err, 1.0);
    return length(err);
    
    // old approach
    /*
    float3 dir00 = gPrevPos.SampleLevel(S, uv00, 0.0) - cameraVirtual;
    float3 dir10 = gPrevPos.SampleLevel(S, uv10, 0.0) - cameraVirtual;
    float3 dir01 = gPrevPos.SampleLevel(S, uv01, 0.0) - cameraVirtual;
    float3 dir11 = gPrevPos.SampleLevel(S, uv11, 0.0) - cameraVirtual;

    // projected positions (to unit distance plane)
    float2 proj00 = mul(TB, dir00 / dot(dir00, w_i));
    float2 proj10 = mul(TB, dir10 / dot(dir10, w_i));
    float2 proj01 = mul(TB, dir01 / dot(dir01, w_i));
    float2 proj11 = mul(TB, dir11 / dot(dir11, w_i));

    float2 proj = w00 * proj00 + w10 * proj10 + w01 * proj01 + w11 * proj11;
    return length(proj) * linearDepth; // from unit distance to world space unit
    */
}

#endif

#if OPT_MODE == OPT_MODE_NORMALIZED_DERIVS
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

    float3 dir00 = gPrevPos.SampleLevel(S, uv00, 0.0) - cameraVirtual;
    float3 dir10 = gPrevPos.SampleLevel(S, uv10, 0.0) - cameraVirtual;
    float3 dir01 = gPrevPos.SampleLevel(S, uv01, 0.0) - cameraVirtual;
    float3 dir11 = gPrevPos.SampleLevel(S, uv11, 0.0) - cameraVirtual;

    // projected positions (to unit distance plane)
    float2 proj00 = mul(TB, dir00 / dot(dir00, w_i));
    float2 proj10 = mul(TB, dir10 / dot(dir10, w_i));
    float2 proj01 = mul(TB, dir01 / dot(dir01, w_i));
    float2 proj11 = mul(TB, dir11 / dot(dir11, w_i));

    float2 proj = w00 * proj00 + w10 * proj10 + w01 * proj01 + w11 * proj11;
    return length(proj) * linearDepth; // from unit distance to world space unit
    
}
#endif

// non normalized derivatives (deprecated)
#if OPT_MODE == OPT_MODE_SIMPLE_TANGENT 
float calcEstimatedError(float2 mvec)
{
    float2 uv = texC + adjustJitter(mvec);
    // 4 rounded uv coordinates
    float2 uv00 = snapTopLeft(uv);
    float2 uv10 = uv00 + stepX;
    float2 uv01 = uv00 + stepY;
    float2 uv11 = uv00 + step;
    // corresponding barycentric weights
    float w00 = (1.0 - (uv.x - uv00.x) / step.x) * (1.0 - (uv.y - uv00.y) / step.y);
    float w10 = ((uv.x - uv00.x) / step.x) * (1.0 - (uv.y - uv00.y) / step.y);
    float w01 = (1.0 - (uv.x - uv00.x) / step.x) * ((uv.y - uv00.y) / step.y);
    float w11 = ((uv.x - uv00.x) / step.x) * ((uv.y - uv00.y) / step.y);

    // directions from camera to sample points
    float3 pos00 = gPrevPos.SampleLevel(S, uv00, 0.0);
    float3 pos10 = gPrevPos.SampleLevel(S, uv10, 0.0);
    float3 pos01 = gPrevPos.SampleLevel(S, uv01, 0.0);
    float3 pos11 = gPrevPos.SampleLevel(S, uv11, 0.0);

    float3 newPos = w00 * pos00 + w10 * pos10 + w01 * pos01 + w11 * pos11;

    //float3 newPos = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0);
    float3 posDiff = newPos - curPos;
    PROJECT(posDiff);
    float newError = length(posDiff);
    return newError;
}
#endif


[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    pixel = id.xy;
    texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 bmvec = gBackupMotion[pixel];
    const uint reqPathLen = gCurPathLen[pixel]; // required path length: other positions should be taken from this exact path length, otherwise they are likeley invalid
    curPos = gCurPos.SampleLevel(S, texC, 0.0);
    linearDepth = gLinearDepth[pixel]; // travelled distance between current camera and curPos
    w_i = gLastRayDir[pixel];         // incomming ray direction on P
    cameraVirtual = curPos - w_i * linearDepth;

    float3 tangent = normalize(cross(float3(0.0, 1.0, 0.0), w_i));
    float3 bitangent = cross(w_i, tangent);
    TB = float2x3(tangent, bitangent);

    uint iterations = gIterations;

    if (!gForceMotionVectorCalculation) // skip iterations in trivial cases
    {
        // if (all(mvec == bmvec)) iterations = 0; // backup will always be correct under static assumptions
        if (all(mvec == 0.0)) iterations = 0; // force nothing moved
        if (reqPathLen <= 1) iterations = 0; // path length 1 is trival reprojection => force correct
    }
   
    if (gCompareWithBackupMotion)
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

        float2x2 AtA = 0.0;
        float2 Atb = 0.0;

#if OPT_MODE == OPT_MODE_VIRTUAL_CAMERA
        // supports only window radius = 1

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
        // float3 Cv = (cam00 + cam10 + cam01 + cam11) * 0.25;
        float3 Cv = w00 * cam00 + w10 * cam10 + w01 * cam01 + w11 * cam11;

        optimizeMotionVector(w00, Cv, S00, S10, S01, l00, l10, l01, 1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w10, Cv, S10, S00, S11, l10, l00, l11, -1.0, 1.0, AtA, Atb);
        optimizeMotionVector(w01, Cv, S01, S11, S00, l01, l11, l00, 1.0, -1.0, AtA, Atb);
        optimizeMotionVector(w11, Cv, S11, S01, S10, l11, l01, l10, -1.0, -1.0, AtA, Atb);
        #else
        // for radius=1 => 2x2: dx = 0, 1; dy = 0, 1
        // for radius=2 => 4x4: dx = -1,0,1,2; dy = -1,0,1,2
        for (int dx = -gWindowRadius + 1; dx <= gWindowRadius; ++dx)
            for (int dy = -gWindowRadius + 1; dy <= gWindowRadius; ++dy)
            {
                int2 off = int2(dx, dy);
                float3 prevPos = gPrevPos[clampPixel(uv00pixel + off)];
                uint prevPathLen = gPrevPathLen[clampPixel(uv00pixel + off)];
                if(prevPathLen != reqPathLen) continue;

                // barycentric weights based on distance to center pixel uv00
                float2 distInPixels = abs(uv00 + dx * stepX + dy * stepY - uvPrev) * float2(gFrameDim);
                float w = 1.0;
                w *= 1.0 - (distInPixels.x / gWindowRadius); // barycentric weights for radius=1
                w *= 1.0 - (distInPixels.y / gWindowRadius);

                // check wheter I_x and I_y can be trusted (not much larger than expected pos diff)
                float expectedOnePixelDiff = gPrevPosDiff[clampPixel(uv00pixel + off)];
                float posDiffThreshold = 2.0;

                #if OPT_MODE == OPT_MODE_NORMALIZED_DERIVS
                // build derivatives
                float3 w_0 = prevPos - cameraVirtual;
                float2 S0 = mul(TB, w_0 / dot(w_0, w_i));
                float2 I_t = -S0;

                bool invalidDiffs = false;
                float4 xpos = getMinDerivPos(prevPos, uv00pixel + off, int2(1, 0), prevPathLen, invalidDiffs);
                float4 ypos = getMinDerivPos(prevPos, uv00pixel + off, int2(0, 1), prevPathLen, invalidDiffs);
                if (invalidDiffs) continue; // based on path length disc.

                float3 w_1x = xpos.xyz - cameraVirtual;
                float3 w_1y = ypos.xyz - cameraVirtual;
                float2 S1x = mul(TB, w_1x / dot(w_1x, w_i));
                float2 S1y = mul(TB, w_1y / dot(w_1y, w_i));
                float2 I_x = (S1x - S0) * xpos.w; // w flips the derivate if -step was taken
                float2 I_y = (S1y - S0) * ypos.w;

                //invalidDiffs = false;
                //if (length(I_x) * linearDepth > posDiffThreshold * expectedOnePixelDiff) invalidDiffs = true;
                //if (length(I_y) * linearDepth > posDiffThreshold * expectedOnePixelDiff) invalidDiffs = true;
                // if (invalidDiffs) continue; // based on posDiffThreshold
                #endif

                #if OPT_MODE == OPT_MODE_SIMPLE_TANGENT
                // build derivatives
                float3 I_t = curPos - prevPos; // [t+1] - [t]
                bool invalidDiffs = false;
                float3 I_x = getMinDeriv(prevPos, uv00pixel + off, int2(1, 0), prevPathLen, invalidDiffs);
                float3 I_y = getMinDeriv(prevPos, uv00pixel + off, int2(0, 1), prevPathLen, invalidDiffs);
                if (invalidDiffs) continue; // based on path length disc.

                invalidDiffs = false;
                if (length(I_x) > posDiffThreshold * expectedOnePixelDiff) invalidDiffs  = true;
                if (length(I_y) > posDiffThreshold * expectedOnePixelDiff) invalidDiffs = true;
                //if (invalidDiffs) continue; // based on posDiffThreshold

                PROJECT(I_x);
                PROJECT(I_y);
                PROJECT(I_t);
                #endif

                AtA += w * float2x2(dot(I_x, I_x), dot(I_x, I_y),
                                    dot(I_x, I_y), dot(I_y, I_y));
                Atb += w * float2(dot(I_t, I_x), dot(I_t, I_y));
            }
        #endif
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

        float3 curDiff = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec), 0.0) - curPos;
        PROJECT(curDiff);
        float curError = length(curDiff);
        float newError = curError;

        float alpha = 1.0;
        for (; alpha > 0.1; alpha *= 0.5)
        {
            float3 newDiff = gPrevPos.SampleLevel(S, texC + adjustJitter(mvec + alpha * uvDiff), 0.0) - curPos;
            PROJECT(newDiff);
            newError = length(newDiff);
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

    // test for path length discontinuety
    bool pathLenDiscont = false;
    int2 pathOffset[4] = { int2(-1, 0), int2(1, 0), int2(0, 1), int2(0, -1) };
    for (uint i = 0; i < 4; ++i)
        if(gCurPathLen[clampPixel(pixel + pathOffset[i]) ] != reqPathLen)
            pathLenDiscont = true;
    pathLenDiscont = false;

    if (iterations > 0 && (newError > onePixelPosDiff * 0.05 || pathLenDiscont))
    {
        mvec = originalMvec;
        gMotionError[pixel] = 1;
    }
    else
    {
        gMotionError[pixel] = 0;
    }

    gMotionOut[pixel] = mvec;
    //gMotionOut[pixel] = 0.0;
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
    // TODO deprecated, update
    /*const uint2 pixel = id.xy;
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    const float3 curPos = gCurPos.SampleLevel(S, texC, 0.0);
    const float3 w_unit = normalize(curPos - camPos); // reference direction

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

    if (newError > onePixelPosDiff * 0.1) mvec = gBackupMotion[pixel];
    //if (Tcur < 0.9999) mvec = mvec_backup;

    gMotionOut[pixel] = mvec;*/
}
