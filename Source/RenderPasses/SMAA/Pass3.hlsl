#include "SMAA.hlsl"

Texture2D gColor;
Texture2D gBlendTex;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(texC, offset);
    return SMAANeighborhoodBlendingPS(texC, offset,
        gColor, gBlendTex);
}
