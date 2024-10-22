#ifndef SMAA_REPROJECTION
#define SMAA_REPROJECTION 1
#endif
#include "SMAA.hlsl"

Texture2D gColor;
Texture2D gPrevColor;
Texture2D gVelocity;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    return SMAAResolvePS(texC, gColor, gPrevColor, gVelocity);
}
