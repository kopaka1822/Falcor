Texture2D<float2> gMotionVec;
SamplerState s;

struct PSInput
{
    float2 texCoord : TEXCOORD0;
};

cbuffer PerFrameCB
{
    float scale;
};

// Convert HSV (h:0–1, s:0–1, v:0–1) to RGB
float3 HSVtoRGB(float3 hsv)
{
    float h = hsv.x;
    float s = hsv.y;
    float v = hsv.z;

    float3 rgb = clamp(abs(frac(h + float3(0, 2.0 / 6.0, 4.0 / 6.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return v * lerp(float3(1, 1, 1), rgb, s);
}

float4 main(float2 uv: TEXCOORD, float4 svPos: SV_POSITION) : SV_TARGET
{
    float2 mvec = gMotionVec.Sample(s, uv).xy;

    float len = saturate(length(mvec) * scale);
    float angle = atan2(mvec.y, mvec.x);            // radians (-pi..pi)
    float hue = (angle / (2.0 * 3.14159265)) + 0.5; // normalize to 0–1
    float saturation = 1.0;
    float3 rgb = HSVtoRGB(float3(hue, saturation, len));

    return float4(rgb, 1.0);
}
