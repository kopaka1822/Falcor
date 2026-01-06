Texture2D<float2> gMotionVec;
Texture2D<float3> gCurPosW;
Texture2D<float3> gPrevPosW;

SamplerState s;

struct PSInput
{
    float2 texCoord : TEXCOORD0;
};

cbuffer PerFrameCB
{
    float2 prevJitter;
    float2 curJitter;
    float scale;
};

float4 main(float2 uv: TEXCOORD, float4 svPos: SV_POSITION) : SV_TARGET
{
    float2 mvec = gMotionVec.Sample(s, uv).xy;

    float3 posW = gCurPosW.Sample(s, uv).xyz;
    float3 prevPosW = gPrevPosW.Sample(s, uv + mvec - prevJitter + curJitter).xyz;

    return float4(abs(posW - prevPosW) * scale, 1.0);
    //return float4(distance(posW, prevPosW).xxx, 1.0);
}
