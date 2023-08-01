#include "Scene/VertexAttrib.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;

cbuffer CB
{
    float4x4 VPMat;
    float farPlane;
    float3 lightPos;
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
    vOut.posH = mul(VPMat, float4(posW, 1.f));
        
    return vOut;
}

float psMain(shadowVSOut vsOut) : SV_Target0
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - lightPos);
    //Map to [0,1]
    lightDist /= farPlane;

    return lightDist;
}
