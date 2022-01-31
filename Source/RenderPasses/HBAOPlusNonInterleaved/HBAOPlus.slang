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
#define DEPTH_MODE DEPTH_MODE_DUAL
#endif

cbuffer StaticCB
{
    HBAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

Texture2D<float> gDepthTex;
Texture2D<float> gDepthTex2;

Texture2D gNormalTex;
Texture2D gNoiseTex; // xy = random normalized direciton, zw = normalized random number => SNORM texture format


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
    // 1.0 - (d*d)/(r*r)
    return DistanceSquare * gData.negInvRsq + 1.0;
    //return 1.0 - DistanceSquare / (gData.radius * gData.radius);

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

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
        return 1.0f;

    const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 WorldNormal = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 ViewNormal = mul(WorldNormal, float3x3(gCamera.data.viewMat));
    if (dot(ViewPosition, ViewNormal) > 0.0)
        ViewNormal = -ViewNormal;

    float RadiusInPixels = GetAORadiusInPixels(linearDepth);
    
    // early out when radius is too small
    [branch] if (RadiusInPixels < 1.0) return 1.0;

    // Compute AO
    // Divide by NUM_STEPS+1 so that the farthest samples are not fully attenuated
    float StepSizePixels = RadiusInPixels / (NUM_STEPS + 1);

    // TODO replace with texture lookup
    float4 Rand = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale);
    //Rand = float4(1, 0, 0.5, 1);
    const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;

    float AO = 0.0;
    // sample NUM_DIRECTIONS directions on the view space disc
    [unroll] for (int i = 0; i < NUM_DIRECTIONS; ++i)
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
            float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invResolution;
            if (DEPTH_MODE == DEPTH_MODE_SINGLE)
            {
                float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                // TODO? could check for depth == farZ
                float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                AO += ComputeAO(ViewPosition, ViewNormal, S);
            }
            else if (DEPTH_MODE == DEPTH_MODE_DUAL)
            {
                float linearSampleDepth1 = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                float linearSampleDepth2 = gDepthTex2.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                // TODO? could check for depth == farZ
                float3 S1 = UVToViewSpace(snappedSampleUV, linearSampleDepth1);
                float3 S2 = UVToViewSpace(snappedSampleUV, linearSampleDepth2);
                AO += max(ComputeAO(ViewPosition, ViewNormal, S1), ComputeAO(ViewPosition, ViewNormal, S2));
            }

            RayPixels += StepSizePixels;
        }
    }

    AO /= (NUM_DIRECTIONS * NUM_STEPS);
    // artistic modifications
    float res = saturate(1.0 - AO * 2.0);
    res = pow(res, gData.powerExponent);

    return res;
}
