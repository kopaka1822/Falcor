Texture2D gSrcTex;
Texture2D gDepthTex;

SamplerState gSampler;

struct BlurPSIn
{
    float2 texC : TEXCOORD;
    float4 pos : SV_POSITION;
};

float4 main(BlurPSIn pIn) : SV_TARGET
{
    float2 uv = pIn.texC;
    return gSrcTex.SampleLevel(gSampler, uv, 0);
}