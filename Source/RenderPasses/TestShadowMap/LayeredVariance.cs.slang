import SMStructs;

#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

#ifndef LVSM_LAYERS
#define LVSM_LAYERS 2
#endif

Texture2D<SM_FORMAT> gShadowMap;
RWTexture2D<float2> gLayeredVarianceOut[LVSM_LAYERS];

Texture2D<float2> gLayeredVariance[LVSM_LAYERS];

cbuffer CB
{
    uint gSMSize;   //Pixel size
    float2 gSMNearFar; //Near|Far
    float gOverlap; //Overlap of the layers
    float4x4 gViewProj;
}

//Texture Sampler
SamplerState gShadowSamplerPoint;

/* Warp function. p,q,depth all in [0,1]

*/
float warp(float p, float q, float depth){
    float low = max(0.0,p-gOverlap);
    float high = min(1.0, q+gOverlap);
    float warpedDepth = (depth-low) / (high-low);
    warpedDepth = depth <= low ? 0.0 : warpedDepth;
    warpedDepth = high <= depth ? 1.0 : warpedDepth;

    return warpedDepth;
}

[numthreads(16, 16, 1)]
void generate(uint2 pixel: SV_DispatchThreadID)
{
   
}

[numthreads(16, 16, 1)]
void evaluate(uint2 pixel: SV_DispatchThreadID)
{
   
}
