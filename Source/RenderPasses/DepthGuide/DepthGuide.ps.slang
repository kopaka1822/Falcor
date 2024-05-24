RWBuffer<uint> gMinMaxDepth;

Texture2D<float4> gDepthTex;

SamplerState gSampler;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float curDepth = gDepthTex.Sample(gSampler, texC).x;
    if (curDepth <= 0.0) return 0.0; // background hit => max depth

    float minDepth = asfloat(gMinMaxDepth[0]);
    float maxDepth = asfloat(gMinMaxDepth[1]);
    // increase max depth slighly to still distinguish between background and max depth
    maxDepth = maxDepth + 0.2 * (maxDepth - minDepth);
    
    float d = 1.0 - saturate((curDepth - minDepth) / (maxDepth - minDepth));
    return pow(d, 2.2); // gamma correction (sRGB)
    //return d;
}
