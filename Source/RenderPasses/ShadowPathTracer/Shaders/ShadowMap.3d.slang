#include "Scene/VertexAttrib.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;

cbuffer CB
{
    float4x4 gviewProjection;

    float3 gLightPos;
    float gFarPlane;
};

struct shadowVSOut
{
    linear float3 posW : POSW;
    
    float4 posH : SV_POSITION;
};

shadowVSOut vsMain(VSIn vsIn)
{
    shadowVSOut vOut;
    const GeometryInstanceID instanceID = { vsIn.instanceID };
    float4x4 worldMat = gScene.getWorldMatrix(instanceID);
    float3 posW = mul(worldMat, float4(vsIn.pos, 1.f)).xyz;
    vOut.posW = posW;
    vOut.posH = mul(gviewProjection, float4(posW, 1.f));
        
    return vOut;
}

float psMainMisc(shadowVSOut vsOut) : SV_Depth
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}

float psMainCube(shadowVSOut vsOut) : SV_Target0
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}
