import Scene.Camera.Camera;
import Scene.Intersection;
import Scene.Shading;

#include "../VAONonInterleaved/VAOData.slang"
#include "Scene/Material/MaterialDefines.slangh"

// single depth texture
#define DEPTH_MODE_SINGLE 0
// two depth textures
#define DEPTH_MODE_DUAL 1
// single or dual depth texture + stochastic depth texture
#define DEPTH_MODE_STOCHASTIC 2
// raytraced
#define DEPTH_MODE_RAYTRACING 3

#define HALO_RADIUS (gData.radius * 3.0)
#define CONST_RADIUS (gData.radius * 1.0)

#define NUM_DIRECTIONS 8
// normalized radius for each of the NUM_DIRECTION samples (distributed with radical inverse => see SSAO::setKernel() radius)
static const float sampleRadius[NUM_DIRECTIONS] = { 0.608308673, 0.776627183, 0.417753726, 0.866025388, 0.518647850, 0.692805171, 0.291845083, 0.917883337 };
// same as above, but distributed uniformly (0.5/8, 1.5/8, 2.5/8, ... 7.5/8) => shuffled afterwards because the rotation follows an increasing angle
//static const float sampleRadius[NUM_DIRECTIONS] = { 0.917883, 0.470149, 0.205215, 0.650919, 0.734504, 0.359545, 0.564429, 0.820004 };

cbuffer StaticCB
{
    VAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
    float2 Rand;
    uint2 quarterOffset;
    uint sliceIndex;
}

SamplerState gTextureSampler;

//Texture2D<float> gDepthTex;
//Texture2D<float> gDepthTex2;
Texture2D<float> gDepthTexQuarter;
Texture2D<float> gDepthTex2Quarter;

Texture2D gNormalTex;

float2 getScreenClampedUV(float2 uvstart, float2 uvend)
{
    return saturate(uvend); // this actually does not make much of a difference but costs a little bit more...
}

float2 getSnappedUV(float2 uv)
{
    float2 pixelCoord = floor(uv * gData.resolution);
    return float2((pixelCoord.x + 0.5f) / gData.resolution.x, (pixelCoord.y + 0.5f) / gData.resolution.y);
}

bool isSamePixel(float2 uv1, float2 uv2)
{
    //return false;
    return all(abs(uv1 - uv2) < gData.invResolution * 0.9);
    //return all(abs(uv1 - uv2) < gData.invResolution * 1.1); // this also ignores 1-pixel differences
}

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
}

float2 Rotate90(float2 vec) // CCW
{
    return float2(-vec.y, vec.x);
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

float makeNonZero(float value, float epsilon)
{
    float absValue = max(abs(value), epsilon);
    return value >= 0 ? absValue : -absValue;
}

// get rid of shadowing around edges
// introduce a linear falloff function that starts with 0.0 when the sample depth intersects the front sphere exactly,
// and falls of to 1.0 when it gets further away from the sphere but closer to the camera.
// this also includes the constant radius, where visibility remains 0
float calcHaloVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return saturate((objectSpaceZ - sphereStart - CONST_RADIUS) / HALO_RADIUS)
        * (sphereStart - sphereEnd); // this adjust the visibility to the sampling (hemi-)sphere
}

float calcSphereVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    float sampleRange = max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
    return sampleRange;
}

float calcVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return calcSphereVisibility(objectSpaceZ, sphereStart, sphereEnd)
         + calcHaloVisibility(objectSpaceZ, sphereStart, sphereEnd);
}

float getAORadiusInPixels(float viewZ)
{
    float uvRadius = (gData.radius * gCamera.data.focalLength) / (viewZ * gCamera.data.frameWidth);
    return uvRadius * gData.resolution.x;
}

float calcObjectSpaceZ(float3 posV, float3 normal, float2 uv, Texture2D<float> depthTex)
{
    float linearSampleDepth = depthTex.SampleLevel(gTextureSampler, uv, 0);
    float3 samplePosV = UVToViewSpace(uv, linearSampleDepth);
            // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
    float objectSpaceZ = dot(samplePosV - posV, normal);
    return objectSpaceZ;
}
