#include "Scene/VertexAttrib.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;

cbuffer CB
{
    float4x4 gviewProjection;

    float3 gLightPos;
    float gFarPlane;
};


#ifndef INTERPOLATION_MODE
#define INTERPOLATION_MODE linear
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

// Pixel Shaders
float defaultPixelShader(float3 posW)
{
    //Distance between point and light
    float lightDist = length(posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}

// Pixel Shaders
void psMain(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID)
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    //return defaultPixelShader(vsOut.posW);
}

float psMainCube(ShadowVsOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Target
{
    #if USE_ALPHA_TEST
        if (shadowAlphaTest(vsOut, triangleIndex)) discard;
    #endif
    return defaultPixelShader(vsOut.posW);
}
