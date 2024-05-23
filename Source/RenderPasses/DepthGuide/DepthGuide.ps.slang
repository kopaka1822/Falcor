cbuffer StaticCB
{
    float gMinDepth;
    float gMaxDepth;
};

Texture2D<float4> gDepthTex;

SamplerState gSampler;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float curDepth = gDepthTex.Sample(gSampler, texC).x;
    if (curDepth <= 0.0) return 0.0; // background hit => max depth
    return 1.0 - saturate((curDepth - gMinDepth) / (gMaxDepth - gMinDepth));
}
