#include "Scene/VertexAttrib.slangh"

import Scene.Raster;
import Utils.Math.MathHelpers;

cbuffer CB
{
    float4x4 gviewProjection;

    float3 gLightPos;
    float gFarPlane;
};

cbuffer CBGeo
{
    float4x4 gviewProjectionGeo[6];
};

struct shadowVSOut
{
    linear float3 posW : POSW;
    float4        posH : SV_POSITION;
};

struct geometryShaderIn
{
    float4 posH : SV_POSITION;
};

struct geometryShaderOut
{
    linear float3 posW : POSW;
    float4        posH : SV_POSITION;
    uint          rtvId : SV_RenderTargetArrayIndex;
};

//Vertex Shaders
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

geometryShaderIn vsGeoCube(VSIn vsIn)
{
    geometryShaderIn vOut;
    const GeometryInstanceID instanceID = { vsIn.instanceID };
    float4x4 worldMat = gScene.getWorldMatrix(instanceID);
    float3 posW = mul(worldMat, float4(vsIn.pos, 1.f)).xyz;
    vOut.posH = float4(posW, 1.f);
        
    return vOut;
}

//Geometry Shaders
[maxvertexcount(18)]
void gsCube(triangle geometryShaderIn input[3], inout TriangleStream<geometryShaderOut> OutputStream)
{
    geometryShaderOut output;
    for (uint face = 0; face < 6; face++)
    {
        for (uint i = 0; i < 3; i++)
        {
            output.posW = input[i].posH.xyz;
            output.posH = mul(gviewProjectionGeo[face], float4(output.posW, 1.f));
            output.rtvId = face;
            OutputStream.Append(output);
        }
        OutputStream.RestartStrip();
    }
}

// Pixel Shaders
float psMainMisc(shadowVSOut vsOut) : SV_Depth
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}

// Pixel Shaders
float psGeoCube(geometryShaderOut vsOut) : SV_Depth
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}

float psMainCube(shadowVSOut vsOut) : SV_Target
{
    //Distance between point and light
    float lightDist = length(vsOut.posW - gLightPos);
    //Map to [0,1]
    lightDist /= gFarPlane;

    return lightDist;
}
