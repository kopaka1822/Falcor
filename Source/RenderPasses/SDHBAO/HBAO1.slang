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

struct PSOut
{
    float ao1;
    STENCIL(uint stencil);
};

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
RWTexture2D<uint> gRayMinAccess;
RWTexture2D<uint> gRayMaxAccess;

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

struct IntersectionResult {
    float tStart;  // t parameter for the intersection with the sphere
    float tEnd;    // t parameter for the exit point from the sphere or intersection with the plane
    bool valid;    // Indicates if the intersection is valid
};

IntersectionResult CalculateIntersections(float3 P, float3 N, float3 S, float r) {
    IntersectionResult result;
    
    // Normalize S and N to ensure they are unit vectors
    S = normalize(S);
    N = normalize(N);
    
    // Initialize variables
    result.valid = false;
    result.tStart = 0.0f; // Default value
    result.tEnd = 0.0f;   // Default value
    
    // Calculate the intersection with the plane
    float tPlane = dot(N, P) / dot(N, S);
    
    // Calculate the intersection with the sphere
    float a = dot(S, S);
    float b = 2.0 * dot(S, -P);
    float c = dot(P, P) - r * r;
    float discriminant = b * b - 4.0 * a * c;
    
    if (discriminant >= 0) {
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
        if (tPlane < result.tStart) {
            result.valid = false;
        }
    }

    result.tStart = max(0.0, result.tStart);
    result.tEnd = max(0.0, result.tEnd);
    
    return result;
}


PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    PSOut output;
    output.ao1 = 0.0;
    STENCIL(output.stencil = 0);

    // adjust texC for quarter resolution texcoord
    svPos.xy = floor(svPos.xy) * 4.0 + quarterOffset + 0.5;
    texC = svPos.xy * gData.invResolution;
    float linearDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
    {
        output.ao1 = 1.0;
        return output;
    }

    const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 WorldNormal = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 ViewNormal = mul(float3x3(gCamera.data.viewMat), WorldNormal);
    if (dot(ViewPosition, ViewNormal) > 0.0)
        ViewNormal = -ViewNormal;

    // radius in full-res pixels
    float RadiusInPixels = GetAORadiusInPixels(linearDepth);
    
    // early out when radius is too small
    [branch]
    if (RadiusInPixels < 1.0)
    {
        output.ao1 = 1.0;
        return output;
    }

    // Compute AO
    // Divide by NUM_STEPS+1 so that the farthest samples are not fully attenuated
    // divide by 4 for step size in quarter res pixels
    float StepSizePixels = (RadiusInPixels / 4.0) / (NUM_STEPS + 1);

    const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;

    // sample NUM_DIRECTIONS directions on the view space disc
    [unroll]
    for (int i = 0; i < NUM_DIRECTIONS; ++i)
    {
        float Angle = Alpha * float(i);

        // random normalized 2D direction
        float2 Direction = Rotate2D(Rand.xy, Angle);

        // Jitter starting sample within the first step
        float RayPixels = (Rand.z * StepSizePixels + 1.0);

        // sample NUM_STEPS steps for each direction
        for (int step = 0; step < NUM_STEPS; ++step)
        {
            // calculate sample position in uv space (round ray pixels to sample in pixel center)
            float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invQuarterResolution;
            float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, snappedSampleUV, 0);
            float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
            float ao = 0.0;
            if(RecomputeAO(ViewPosition, ViewNormal, S, ao) && DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
            {
                // calculate intersection with sphere
                IntersectionResult ir = CalculateIntersections(ViewPosition, ViewNormal, S, gData.radius);
                if(ir.valid)
                {
                    STENCIL(output.stencil |= 1u << (i * NUM_STEPS + step));
                    uint rayMin, rayMax;
                    rayMin = asuint(ir.tStart);
                    rayMax = asuint(ir.tEnd);
                    uint2 pixel = UVToSDPixel(snappedSampleUV);
                    InterlockedMin(gRayMinAccess[pixel], rayMin);
                    InterlockedMax(gRayMaxAccess[pixel], rayMax);
                }
            }
            output.ao1 += ao;
            
            /*else if (DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
            {
                float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                
                float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                float2 ao = 0.0;
                [branch]
                if (RecomputeAO(ViewPosition, ViewNormal, S, ao))
                {
#ifdef STOCHASTIC_ARRAY
                    int2 pixelCoord = int2(floor(snappedSampleUV * gData.resolution * 0.25));
#else
                    int2 pixelCoord = int2(floor(snappedSampleUV * gData.resolution));
#endif
                    const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
                    const float depthOffset = gCamera.data.nearZ;
                    [unroll]
                    for (uint i = 0; i < MSAA_SAMPLES; ++i)
                    {
#ifdef STOCHASTIC_ARRAY
                        float linearSampleDepth = gsDepthTex.Load(int3(pixelCoord, sliceIndex), i);
#else
                        float linearSampleDepth = gsDepthTex.Load(int3(pixelCoord, 0), i);
#endif
                        // linearSampleDepth is in [0, 1] => scale accordingly
                        linearSampleDepth = linearSampleDepth * depthRange + depthOffset;
                        float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                        ao = max(ao, ComputeAO(ViewPosition, ViewNormal, S));
                    }
                }
                AO += ao;
            }*/

            RayPixels += StepSizePixels;
        }
    }

    output.ao1 /= (NUM_DIRECTIONS * NUM_STEPS);

    STENCIL(if(output.stencil == 0))
    {
        float res = saturate(1.0 - output.ao1 * 2.0);
        output.ao1 = pow(res, gData.powerExponent);
    }
    

    return output;
}
