import Scene.Camera.Camera;

Texture2D<float2> gMotionVec;
Texture2D<float> gDepth;
Texture2D<float> gPrevDepth;
Texture2D<float> gPrevDepth2;

SamplerState gLinearSampler;
SamplerState gPointSampler;

cbuffer PerFrameCB
{
    Camera gCamera;
    float4x4 prevViewToCurView; // viewMat * Inverse(prevViewMat)
    float4x4 curViewToPrevView; // prevViewMat * Inverse(viewMat)
    uint2 resolution;
    float minSeparationDist;
    int maxIterations;
}

#define INSIDE 0
#define LEFT 1
#define RIGHT 2
#define BOTTOM 4
#define TOP 8

uint ComputeRegionCode(float u, float v)
{
    uint code = INSIDE;

    if (u < 0.0f)
        code |= LEFT;
    else if (u > 1.0f)
        code |= RIGHT;

    if (v < 0.0f)
        code |= BOTTOM;
    else if (v > 1.0f)
        code |= TOP;

    return code;
}

bool CohenSutherlandClipping(inout float2 p0, inout float2 p1)
{
    uint code0 = ComputeRegionCode(p0.x, p0.y);
    uint code1 = ComputeRegionCode(p1.x, p1.y);

    while (true) // max number of iterations = 4
    {
        if ((code0 | code1) == 0)
        {
            // Both points are inside the clipping window, the line is fully visible
            return true;
        }
        else if ((code0 & code1) != 0)
        {
            // Both points are outside the same region, and the line is fully outside the window
            // In this case, we discard the line
            return false;
        }
        else
        {
            // The line is partially visible, we need to clip it

            // Select one of the points outside the window
            uint codeOutside = (code0 != 0) ? code0 : code1;

            float u, v; // Intersection point with the clipping window
            float2 p;

            // Find the intersection point with the corresponding edge of the window
            if ((codeOutside & TOP) != 0)
            {
                u = p0.x + (p1.x - p0.x) * (1.0f - p0.y) / (p1.y - p0.y);
                v = 1.0f;
            }
            else if ((codeOutside & BOTTOM) != 0)
            {
                u = p0.x + (p1.x - p0.x) * (0.0f - p0.y) / (p1.y - p0.y);
                v = 0.0f;
            }
            else if ((codeOutside & RIGHT) != 0)
            {
                v = p0.y + (p1.y - p0.y) * (1.0f - p0.x) / (p1.x - p0.x);
                u = 1.0f;
            }
            else if ((codeOutside & LEFT) != 0)
            {
                v = p0.y + (p1.y - p0.y) * (0.0f - p0.x) / (p1.x - p0.x);
                u = 0.0f;
            }

            // Update the point outside the window with the intersection point
            if (codeOutside == code0)
            {
                p0.x = u;
                p0.y = v;
                code0 = ComputeRegionCode(p0.x, p0.y);
            }
            else
            {
                p1.x = u;
                p1.y = v;
                code1 = ComputeRegionCode(p1.x, p1.y);
            }
        }
    }
}



// uv: uv coordinates [0, 1]
// viewDepth: linear depth in view space (positive z)
// return: view space position (negative z)
float3 UVToViewSpace(float2 uv, float viewDepth)
{
    float2 ndc = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0; // normalized device coordinates [-1, 1]
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    return float3(ndc * viewDepth * imageScale, -viewDepth);
}

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

struct PsOut
{
    float depth2 : SV_Target0;
};

// perspective correct interpolation
float zlerp(float z0, float z1, float t)
{
    return 1.0 / lerp(1.0 / z0, 1.0 / z1, t);
}

float zlerp(float4 z, float4 w)
{
    return 1.0 / dot(w, 1.0 / z);
}

float4 GetTexelWeights(float2 uv)
{
    // calculate bilinear interpolation weights from uv coordinate
    float2 f = frac(uv * float2(resolution) - 0.5);
    // (-,+),(+,+),(+,-),(-,-)
    float4 w = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    return w;
}

// returns the weight with the highest magnitude (1, 0, 0, 0) or (0, 1, 0, 0) or (0, 0, 1, 0) or (0, 0, 0, 1)
float4 GetTexelPointWeight(float4 w)
{
    int maxIdx = 0;
    float maxVal = w[0];
    for (int i = 1; i < 4; ++i)
    {
        if(w[i] > maxVal)
        {
            maxIdx = i;
            maxVal = w[i];
        }
    }
    float4 res = 0.0;
    for (int i = 1; i < 4; ++i)
        if(i == maxIdx)
            res[i] = 1.0;

    return res;
}

float GetRectifiedDepth(float2 uv, Texture2D<float> depthTex)
{
    float2 gatherUV = (floor(uv * float2(resolution) - 0.5) + 1.0) / resolution; // snap to center between 4 texels
    float4 n = depthTex.Gather(gLinearSampler, gatherUV, 0.0);
    float4 w = GetTexelWeights(uv);
    //float depth = dot(n, w);
    float depth = zlerp(n, w);
    float depthPoint = dot(GetTexelPointWeight(GetTexelWeights(uv)), n);
    // if any depth value contains values from the far plane, DO NOT interpolate (values will be very large). Instead return the closest point without interpolation
    if(any(n  > 0.99 * gCamera.data.farZ))
        return depthPoint;
    
    return depth;
}

struct SearchResult
{
    float depth2;
    float error;
};

SearchResult SearchDepth(float primaryDepth, float2 minUV, float2 maxUV, float minZ, float maxZ, Texture2D<float> prevDepthTex)
{
    SearchResult res;
    res.depth2 = 0.0;
    res.error = 1e10;
    
    float2 bestUV = 0.0;
    float bestZ = 0.0;
    float t = 0.0; // interpolation between min and max
    float tmin = 0.0;
    float tmax = 1.0;
    
    int i = 0;
    float uvEpsilon = 0.5 / resolution.x;
    
    for (; i < maxIterations; ++i)
    {
        t = (tmin + tmax) * 0.5;
        float2 uv = lerp(minUV, maxUV, t);
        float zRef = zlerp(minZ, maxZ, t); // reference z value of the current pixels ray

        // obtain rectified depth values from the previous frame
        float d = GetRectifiedDepth(uv, prevDepthTex);

        // remember best (here last) values so far
        float err = abs(zRef - d);
        if(err < res.error)
        {
            bestZ = d;
            bestUV = uv;
            res.error = err;
        }
        
        // early out if search interval is smaller than epsilon
        if (distance(lerp(minUV, maxUV, tmin), lerp(minUV, maxUV, tmax)) < uvEpsilon)
            break;
        
        if (zRef < d) // increase t
            tmin = t;
        else
            tmax = t;

    }

    if (bestZ == 0.0)
        return res; // no information

    // calc view position in previouse view space
    float3 bestPosV = UVToViewSpace(bestUV, bestZ);
    // calc view position in current view space
    bestPosV = mul(prevViewToCurView, float4(bestPosV, 1.0)).xyz;
    // use z component as depth
    if (-bestPosV.z > primaryDepth + minSeparationDist && -bestPosV.z < 0.99 * gCamera.data.farZ)
    {
        res.depth2 = -bestPosV.z;
    }
        
    return res;
}

PsOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION)
{
    float depth = gDepth.SampleLevel(gLinearSampler, texC, 0.0);
    PsOut o;
    o.depth2 = depth;

    // calc min and max posV in current view space
    float3 minPosV = UVToViewSpace(texC, depth + minSeparationDist);
    float3 maxPosV = UVToViewSpace(texC, gCamera.data.farZ);
    // transform to previous view space
    minPosV = mul(curViewToPrevView, float4(minPosV, 1.0)).xyz;
    maxPosV = mul(curViewToPrevView, float4(maxPosV, 1.0)).xyz;
    // transform to uv coordinates in previous view space
    float2 minUV = ViewSpaceToUV(minPosV);
    float2 maxUV = ViewSpaceToUV(maxPosV);
    float minZ = -minPosV.z;
    float maxZ = -maxPosV.z;
    
    // clip to screen (uvmax may be outside)
    CohenSutherlandClipping(minUV, maxUV);

    SearchResult r2 = SearchDepth(depth, minUV, maxUV, minZ, maxZ, gPrevDepth2);
    
    if (r2.depth2 > depth + minSeparationDist)
        o.depth2 = r2.depth2;
    
    return o;
}
