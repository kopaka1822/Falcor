#include "Scene/VertexAttrib.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import ShadowMapData;

cbuffer CB
{
    float4x4 gViewProjection;   //View Projection
    float4x4 gInvViewProjection;   //Inverse View Projection
    
    float3 gLightPos;           //Light Position; Near is stored in x for spotlights
    float gFarPlane;            //Far plane of the shadow map
    float gNearPlane;           //Near Plane
};

static const float kExponentialConstant = SM_EXPONENTIAL_CONSTANT;
static const float kNegativeExponentialConstant = SM_NEGATIVE_EXPONENTIAL_CONSTANT;
static const bool kVarianceSelfShadow = SM_VARIANCE_SELFSHADOW;
static const bool kUseAlphaTest = USE_ALPHA_TEST;

static const LightTypeSM kShadowMapMode = LightTypeSM(SMRAY_MODE);  //Point,Spot,Cascaded,
static const ShadowMapType kShadowMapType = ShadowMapType(SMRAY_TYPE);  //Classic, Variance, Exponential, Exponential Variance

//Output textures. Only one of them is set
RWTexture2D<float> gOutSM;
RWTexture2D<float2> gOutSMF2;
RWTexture2D<float4> gOutSMF4;

struct TraceData
{
    float linZ;
};

//Rescales depth from [0,1] to [-1,1]
float rescaleDepth(float depth){
    return (2.f * depth) - 1.f;
}
float2 rescaleDepth(float2 depth){
    return (2.f * depth) - 1.f;
}

float4 getExponentialVarianceMoments(float depth){
    float4 evsm;
    evsm.x = exp(depth * kExponentialConstant);              //Moment1
    evsm.y = evsm.x * evsm.x;                                   //Moment2
    evsm.z = -exp(-(depth * kNegativeExponentialConstant)); //Negative Moment1
    evsm.w = evsm.z * evsm.z;                                   //Negative Moment2

    return evsm;
}

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{

}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if(kUseAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("closesthit")]
void closestHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    float3 posW = WorldRayOrigin() + WorldRayDirection() * RayTCurrent(); //Get the current world positon

    switch(kShadowMapMode)
    {
        case LightTypeSM::Point:
        {
            rayData.linZ = length(posW - gLightPos)/gFarPlane; //Distance mapped to [0,1]
            break;   
        }
        case LightTypeSM::Spot:
        {
            float4 posH = mul(gViewProjection, float4(posW, 1.f));
            if(kShadowMapType == ShadowMapType::ShadowMap)
                rayData.linZ = posH.z/posH.w;   //Non Linear depth
            else
                rayData.linZ = (posH.w - gNearPlane) / ( gFarPlane - gNearPlane);   //Linear depth in [0,1]
            break;
        }
        case LightTypeSM::Directional:
        {
            float4 posH = mul(gViewProjection, float4(posW, 1.f));
            rayData.linZ = posH.z;   //Linear depth in [0,1] (Orthographic projection)
            break;
        }
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    float2 pixelCenter = pixel + 0.5f;
    
    float2 relFramePos = pixelCenter/frameDim;
    relFramePos = relFramePos * 2.f - 1.f;  //From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

    RayDesc ray;
    
    float4 posWPixelNear = mul(gInvViewProjection, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProjection, float4(relFramePos, 1.f, 1.f));

    //Shadow Map over projection
    if(kShadowMapMode != LightTypeSM::Directional)
    {
        posWPixelNear.xyz /= posWPixelNear.w;
        float3 toPoint = posWPixelNear.xyz - gLightPos;
        
        ray.TMin = length(toPoint); //Real near for the pixel
        ray.TMax = distance(gLightPos, posWPixelFar.xyz/ posWPixelFar.w); //Real far for the pixel
        ray.Origin = gLightPos;
        ray.Direction = normalize(toPoint);   //World direction
    }
    else //Orthographic Projection
    {
        float3 dir = normalize(gLightPos);  //Pos is used for dir here
        ray.TMin = 0.f; 
        ray.TMax = distance(posWPixelNear.xyz, posWPixelFar.xyz);
        ray.Origin = posWPixelNear.xyz;
        ray.Direction = dir;  
    }
    

    TraceData rayData;
    rayData.linZ = 1.f;
    
    TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

    //Store the distance in an shadow map
    switch(kShadowMapType)
    {
        case ShadowMapType::ShadowMap:
        {
            gOutSM[pixel] = rayData.linZ;
            break;
        }
        case ShadowMapType::Variance:
        {
            gOutSMF2[pixel] = float2(rayData.linZ, rayData.linZ * rayData.linZ);
            break;
        }
        case ShadowMapType::Exponential:
        {
            float depth = rescaleDepth(rayData.linZ);
            gOutSM[pixel] = exp(depth * kExponentialConstant);
            break;
        }
        case ShadowMapType::ExponentialVariance:
        {
            float depth = rescaleDepth(rayData.linZ);
            gOutSMF4[pixel] = getExponentialVarianceMoments(depth);
            break;
        }
    }
}
