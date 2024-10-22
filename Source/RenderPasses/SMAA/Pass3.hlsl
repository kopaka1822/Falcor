
float4 LinearToSrgb(float4 color)
{
    float4 result;
    result.rgb = (color.rgb <= 0.0031308) ? (12.92 * color.rgb) :
        (1.055 * pow(color.rgb, 1.0 / 2.4) - 0.055);
    result.a = color.a; // Preserve alpha
    return result;
}

float4 SrgbToLinear(float4 color)
{
    float4 result;
    result.rgb = (color.rgb <= 0.04045) ? (color.rgb / 12.92) :
        pow((color.rgb + 0.055) / 1.055, 2.4);
    result.a = color.a; // Preserve alpha
    return result;
}

//#define PASS3_COLOR_READ(color) LinearToSrgb(color)
//#define PASS3_COLOR_WRITE(color) SrgbToLinear(color)
//#define PASS3_COLOR_READ(color) SrgbToLinear(color)
//#define PASS3_COLOR_WRITE(color) LinearToSrgb(color)
#include "SMAA.hlsl"

Texture2D gColor;
Texture2D gBlendTex;
Texture2D gVelocity;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(texC, offset);
#if SMAA_REPROJECTION == 0
    return SMAANeighborhoodBlendingPS(texC, offset,
        gColor, gBlendTex);
#elif SMAA_REPROJECTION == 1
    return SMAANeighborhoodBlendingPS(texC, offset,
        gColor, gBlendTex, gVelocity);
#endif
}
