Texture2DArray<float4> src;
SamplerState s;

cbuffer CBuffer
{
    int slice;
};

float4 main(float2 uv : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    return src.SampleLevel(s, float3(uv, float(slice)), 0.0);
}
