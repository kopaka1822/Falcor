RWBuffer<uint> gMinMaxDepth;

Texture2D<float4> gDepthTex;

SamplerState gSampler;

void main(float2 texC : TEXCOORD)
{
    float curDepth = gDepthTex.Sample(gSampler, texC).x;
    float minDepth = curDepth;
    float maxDepth = curDepth;
    if (minDepth <= 0.0) // bg
        minDepth = 3.402823466e+38F; // FLT_MAX
    
    
    minDepth = WaveActiveMin(minDepth);
    maxDepth = WaveActiveMax(maxDepth);
    
    if(WaveIsFirstLane())
    {
        uint iMin = asuint(minDepth);
        uint iMax = asuint(maxDepth);

        InterlockedMin(gMinMaxDepth[0], iMin);
        InterlockedMax(gMinMaxDepth[1], iMax);
    }
}
