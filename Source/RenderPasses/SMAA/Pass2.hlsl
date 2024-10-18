#include "SMAA.hlsl"

Texture2D gEdgesTex;
Texture2D gAreaTex;
Texture2D gSearchTex;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float2 pixcoord = 0.0;
    float4 offset[3] = {};
    SMAABlendingWeightCalculationVS(texC, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(
        texC,
        pixcoord,
        offset,
        gEdgesTex,
        gAreaTex,
        gSearchTex,
        float4(0.0, 0.0, 0.0, 0.0));
}
