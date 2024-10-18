#include "SMAA.hlsl"

Texture2D gColor;

float2 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(texC, offset);
    return SMAALumaEdgeDetectionPS(texC, offset, gColor);
}
