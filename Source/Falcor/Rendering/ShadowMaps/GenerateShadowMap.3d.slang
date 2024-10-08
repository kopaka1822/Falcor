#include "Scene/VertexAttrib.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;

cbuffer CB
{
    float4x4 gviewProjection;   //View Projection

    float3 gLightPos;           //Light Position; Near is stored in x for spotlights
    bool gDisableAlpha;

    float gNearPlane;           //Near Plane
    float gFarPlane;            //Far plane of the shadow map
};

#ifndef SM_EXPONENTIAL_CONSTANT
#define SM_EXPONENTIAL_CONSTANT 80.f
#endif

#ifndef SM_NEGATIVE_EXPONENTIAL_CONSTANT
#define SM_NEGATIVE_EXPONENTIAL_CONSTANT 5.f
#endif

#ifndef INTERPOLATION_MODE
#define INTERPOLATION_MODE linear
#endif

#ifndef SM_VARIANCE_SELFSHADOW
#define SM_VARIANCE_SELFSHADOW 1
#endif

struct ShadowVsOut
{
    INTERPOLATION_MODE float3 normalW : NORMAL; ///< Shading normal in world space (not normalized!).
    INTERPOLATION_MODE float4 tangentW : TANGENT; ///< Shading tangent in world space (not normalized!).
    INTERPOLATION_MODE float2 texC : TEXCRD; ///< Texture coordinate.
    INTERPOLATION_MODE float3 posW : POSW; ///< Position in world space.

    // Per-triangle data
    nointerpolation GeometryInstanceID instanceID : INSTANCE_ID; ///< Geometry instance ID.
    nointerpolation uint materialID : MATERIAL_ID; ///< Material ID.

    float4 posH : SV_POSITION;
};

//Vertex Shaders
ShadowVsOut vsMain(VSIn vIn)
{
    ShadowVsOut vOut;
    const GeometryInstanceID instanceID = { vIn.instanceID };

    float4x4 worldMat = gScene.getWorldMatrix(instanceID);
    float3 posW = mul(worldMat, float4(vIn.pos, 1.f)).xyz;
    vOut.posW = posW;
    vOut.posH = mul(gviewProjection, float4(posW, 1.f));
    
    vOut.instanceID = instanceID;
    vOut.materialID = gScene.getMaterialID(instanceID);

    vOut.texC = vIn.texC;
    vOut.normalW = mul(gScene.getInverseTransposeWorldMatrix(instanceID), vIn.unpack().normal);
    float4 tangent = vIn.unpack().tangent;
    vOut.tangentW = float4(mul((float3x3) worldMat, tangent.xyz), tangent.w);
    
    return vOut;
}

// Pixel Shaders
bool shadowAlphaTest(ShadowVsOut vsOut, uint triangleIndex)
{
    let lod = ImplicitLodTextureSampler();
    //Set vertex data
    VertexData v;
    v.posW = vsOut.posW;
    v.texC = vsOut.texC;
    v.normalW = normalize(vsOut.normalW);
    v.faceNormalW = gScene.getFaceNormalW(vsOut.instanceID, triangleIndex);;
    v.tangentW.xyz = normalize(vsOut.tangentW.xyz);
    v.tangentW.w = sign(vsOut.tangentW.w); // Preserve zero to indicate invalid tangent.

    return gScene.materials.alphaTest(v, vsOut.materialID, lod);
}

//
// Pixel Shaders
//

//Rescales depth from [0,1] to [-1,1]
float rescaleDepth(float depth){
    return (2.f * depth) - 1.f;
}
float2 rescaleDepth(float2 depth){
    return (2.f * depth) - 1.f;
}

// Classic Shadow Maps
void psMain(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID)
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif
}

// Classic Shadow Maps with linear depth
float psMainLinearDepth(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Depth
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif
    float depth = (vsOut.posH.w - gNearPlane)/(gFarPlane - gNearPlane); //Linear depth from [0,1]
    return depth;
}

float psMainCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;
    return lightDist;
}

//Variance Shadow Maps
float2 psVariance(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target0
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif

    float depth = (vsOut.posH.w - gNearPlane)/(gFarPlane - gNearPlane); //Linear depth from [0,1]
    float2 varianceSM = float2(depth, depth * depth);
    
    //From https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps
    #if SM_VARIANCE_SELFSHADOW
    //Get neighbor pixels depth and linearize
    float depthX = (ddx(vsOut.posH.w) - gNearPlane) / (gFarPlane - gNearPlane);
    float depthY = (ddy(vsOut.posH.w) - gNearPlane) / (gFarPlane - gNearPlane);
    
    varianceSM.y += 0.25f * (depthX * depthX + depthY * depthY);
    #endif //SM_VARIANCE_SELFSHADOW
    
    return varianceSM;
}

float2 psVarianceCascaded(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target0
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif
    
    float2 varianceSM = float2(1.f);
    varianceSM.x = vsOut.posH.z;
    varianceSM.y = varianceSM.x * varianceSM.x;

    //From https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps
    #if SM_VARIANCE_SELFSHADOW
    float depthX = ddx(vsOut.posH.z);
    float depthY = ddy(vsOut.posH.z);
    varianceSM.y += 0.25f * (depthX * depthX + depthY * depthY);
    #endif //SM_VARIANCE_SELFSHADOW
    
    return varianceSM;
}

float2 psVarianceCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target0
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    float depth = lightDist / gFarPlane; //Map to [0,1]
    
    float2 varianceSM = float2(depth, depth * depth);

    /* Does not seem to work with cube
    //From https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps
    #if SM_VARIANCE_SELFSHADOW
    float3 neiPosDiffX = ddx(vsOut.posW) - gLightPos;
    float3 neiPosDiffY = ddy(vsOut.posW)- gLightPos;
    float2 neiLightDist = float2(length(neiPosDiffX),length(neiPosDiffY));
    float2 neiDepth = neiLightDist/gFarPlane;
    
    varianceSM.y += 0.25f * (neiDepth.x * neiDepth.x + neiDepth.y * neiDepth.y);
    #endif //SM_VARIANCE_SELFSHADOW
    */
    
    return varianceSM;
}

//Exponential Shadow Maps
float psExponential(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    
    float linZ = (vsOut.posH.w - gNearPlane) / ( gFarPlane - gNearPlane);
    linZ = rescaleDepth(linZ);
    return exp(linZ * SM_EXPONENTIAL_CONSTANT);
}

float psExponentialCascaded(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif

    float depth = rescaleDepth(vsOut.posH.z);
    
    return exp(depth * SM_EXPONENTIAL_CONSTANT); 
}

float psExponentialCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    float depth = lightDist / gFarPlane; //Map to [0,1]
    depth = rescaleDepth(depth);
    return exp(depth * SM_EXPONENTIAL_CONSTANT);
}

float4 getExponentialVarianceMoments(float depth){
    float4 evsm;
    evsm.x = exp(depth * SM_EXPONENTIAL_CONSTANT);              //Moment1
    evsm.y = evsm.x * evsm.x;                                   //Moment2
    evsm.z = -exp(-(depth * SM_NEGATIVE_EXPONENTIAL_CONSTANT)); //Negative Moment1
    evsm.w = evsm.z * evsm.z;                                   //Negative Moment2

    return evsm;
}

//Exponential Variance Shadow Maps
float4 psExponentialVariance(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif

    float linZ = (vsOut.posH.w - gNearPlane) / ( gFarPlane - gNearPlane);
    linZ = rescaleDepth(linZ);           //Convert to [-1,1]
    
    return getExponentialVarianceMoments(linZ);
}

float4 psExponentialVarianceCascaded(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif

    float depth = rescaleDepth(vsOut.posH.z);   //Convert to [-1,1]
    
    return getExponentialVarianceMoments(depth);
}

float4 psExponentialVarianceCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    float depth = lightDist / gFarPlane; //Map to [0,1]
    depth = rescaleDepth(depth);

    return getExponentialVarianceMoments(depth);
}

float4 getFourMoments(float z){
    float z2 = z*z;
    float z3 = z2 * z;
    float z4 = z3 * z;
    return float4(z,z2,z3,z4);
}

float4 psMSM(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif

    float linZ = (vsOut.posH.w - gNearPlane) / ( gFarPlane - gNearPlane);
    
    return getFourMoments(linZ);
}

float4 psMSMCascaded(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
#if USE_ALPHA_TEST
    if(!gDisableAlpha)
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
#endif
        
    return getFourMoments(vsOut.posH.z);
}
float4 psMSMCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif

    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    float depth = lightDist / gFarPlane; //Map to [0,1]

    return getFourMoments(depth);
}
