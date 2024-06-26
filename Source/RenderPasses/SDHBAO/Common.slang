import Scene.Camera.Camera;
import HBAOData;

#define NUM_STEPS 4
#define NUM_DIRECTIONS 8

// single depth texture
#define DEPTH_MODE_SINGLE 0
// two depth textures
#define DEPTH_MODE_DUAL 1
// single depth texture + stochastic depth texture
#define DEPTH_MODE_STOCHASTIC 2

#ifndef DEPTH_MODE
#define DEPTH_MODE DEPTH_MODE_SINGLE
#endif

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 1
#endif

#if DEPTH_MODE == DEPTH_MODE_SINGLE
// disable all stencil operations if no secondary pass exists
#define STENCIL(x) 
#else
#define STENCIL(x) x
#endif

cbuffer StaticCB
{
    HBAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
    float4 Rand; // the random values are fixed for a quarter res texture (4x4 grid)
    uint2 quarterOffset; // offset inside the quarter resolution texture
    uint sliceIndex;
}

SamplerState gTextureSampler;
Texture2D<float> gDepthTexQuarter;
Texture2DArray<float4> gsDepthTex; // stochastic depth map
Texture2D gNormalTex;

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
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

// z: positive linear depth in view space
// r: radius in view/world space
float2 ViewSpaceRadiusToUVRadius(float z, float r)
{
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    float2 ndc = float2(r) / (imageScale * z); // radius in normalized device coordinates
    return ndc * 0.5; // scale to uv radius
}

float GetAORadiusInPixels(float ViewDepth)
{
    // convert radius to screen pixels
    float2 radiusUV = ViewSpaceRadiusToUVRadius(ViewDepth, gData.radius);
    // convert uv radius to pixel radius
    return lerp(radiusUV.x * gData.resolution.x, radiusUV.y * gData.resolution.y, 0.5); // take mean between width and height radii TODO  test
}

// distance falloff function
float Falloff(float DistanceSquare)
{
    return DistanceSquare * gData.negInvRsq + 1.0;
}

// ambient occlusion kernel
// P: view space position
// N: view space normal
// S: view space sample position
float ComputeAO(float3 P, float3 N, float3 S)
{
    float3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * rsqrt(VdotV);

    return saturate(NdotV - gData.NdotVBias) * saturate(Falloff(VdotV));
}

// returns true if the given sample requires more depth layers
bool RecomputeAO(float3 P, float3 N, float3 S, inout float ao)
{
    float3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * rsqrt(VdotV);

    float angleTerm = saturate(NdotV - gData.NdotVBias);
    float distanceTerm = saturate(Falloff(VdotV));
    ao = angleTerm * distanceTerm;
    return angleTerm > 0.0 && distanceTerm.x <= 0.0; // sample is in positive hemisphere but further away than the radius
}

// converts from screen uv coordinates to pixel coordinates of stochastic depth map.
// UV can be negative or greater than one if the guard band for the SD-map is used (gData.sdGuard)
int2 UVToSDPixel(float2 uv)
{
    int2 pixel = int2(floor(uv * gData.quarterResolution)) + int2(gData.sdGuard);
    return clamp(pixel, 0, int2(gData.quarterResolution) + gData.sdGuard * 2 - 1);
}

struct IntersectionResult
{
    float tStart; // t parameter for the intersection with the sphere
    float tEnd; // t parameter for the exit point from the sphere or intersection with the plane
    bool valid; // Indicates if the intersection is valid
};

IntersectionResult CalculateIntersections(float3 P, float3 N, float3 S, float r)
{
    IntersectionResult result;
    
    // Normalize S and N to ensure they are unit vectors
    S = normalize(S);
    N = normalize(N);
    
    // Initialize variables
    result.valid = false;
    result.tStart = 0.0f; // Default value
    result.tEnd = 0.0f; // Default value
    
    // Calculate the intersection with the plane
    float tPlane = dot(N, P) / dot(N, S);
    
    // Calculate the intersection with the sphere
    float a = dot(S, S);
    float b = 2.0 * dot(S, -P);
    float c = dot(P, P) - r * r;
    float discriminant = b * b - 4.0 * a * c;
    
    if (discriminant >= 0)
    {
        result.valid = true;
        float sqrtDiscriminant = sqrt(discriminant);
        float t1 = (-b + sqrtDiscriminant) / (2.0 * a);
        float t2 = (-b - sqrtDiscriminant) / (2.0 * a);
        
        // Determine the entry (smallest t) and exit (largest t) points
        result.tStart = min(t1, t2);
        float tExit = max(t1, t2);
        
        // Determine the final tEnd value
        result.tEnd = min(tPlane, tExit);
        
        // Validate that the plane intersection is not before the entry point
        if (tPlane < result.tStart)
        {
            result.valid = false;
        }
    }

    result.tStart = max(0.0, result.tStart);
    result.tEnd = max(0.0, result.tEnd);
    
    return result;
}
