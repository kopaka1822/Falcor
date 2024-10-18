//#define SMAA_DEPTH_THRESHOLD 0.1
#include "SMAA.hlsl"

#ifndef EDGE_MODE
#define EDGE_MODE 0
#endif

Texture2D gColor;
Texture2D gDepth;

float2 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(texC, offset);
#if EDGE_MODE == 0
    return SMAALumaEdgeDetectionPS(texC, offset, gColor);
#elif EDGE_MODE == 1
    return SMAAColorEdgeDetectionPS(texC, offset, gColor);
#elif EDGE_MODE == 2
    return SMAADepthEdgeDetectionPS(texC, offset, gDepth);
#endif
}
