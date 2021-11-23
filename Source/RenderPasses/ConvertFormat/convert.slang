Texture2D<float> I0;
Texture2D<float> I1;
Texture2D<float> I2;
Texture2D<float> I3;

SamplerState s;

float4 main(float2 uv : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    int2 xy = int2(svPos.xy);
    float4 result = (FORMULA);
    return result;
}
