#include "Scene/Lighting/RayShadow.slangh"

Texture2D<float4> gPos;
Texture2D<float4> gNormal;

cbuffer PerLightBuffer
{
    uint gLightIndex;
};

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_Target
{
    float4 posW = gPos[int2(svPos.xy)];
    if (posW.w == 0.0)
        return 1.0; // background
    
    if (gLightIndex >= gScene.getLightCount())
        return 1.0;

    float3 normalW = gNormal[int2(svPos.xy)].xyz;
    return traceShadowRay(uint2(svPos.xy), posW.xyz, normalW, gLightIndex);
}
