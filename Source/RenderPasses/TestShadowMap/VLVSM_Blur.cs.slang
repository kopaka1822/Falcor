#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

Texture2D<SM_FORMAT> gSrcTex;

RWTexture2D<SM_FORMAT> gDstTex;

RWTexture2D<uint2> gLayersMinMaxOut;
Texture2D<uint2> gLayersMinMax;

Buffer<float> gWeights;

static const float kOverlap = _OVERLAP;

float2 minMaxPass(int2 pixel)
{
#ifdef _HORIZONTAL_DIR
    const int2 dir = int2(1, 0);
#elif defined _VERTICAL_DIR
    const int2 dir = int2(0, 1);
#else
#error Please define either _HORIZONTAL_DIR or _VERTICAL_DIR
#endif

    const int2 offset = -((_KERNEL_WIDTH+2) / 2) * dir;

    float2 minMax = float2(1,0);
    [unroll]
    for(int i=0; i<(_KERNEL_WIDTH+2); i++)
    {
        int2 idx = clamp(pixel + offset + i * dir, int2(0), _TEX_SIZE);
        #ifdef _HORIZONTAL_DIR
        float depth = gSrcTex[idx].x;
        minMax = float2(min(minMax.x,depth),max(minMax.y,depth));
        #elif defined _VERTICAL_DIR
        float2 prevMinMax = gSrcTex[idx];
        minMax = float2(min(minMax.x,prevMinMax.x),max(minMax.y,prevMinMax.x));
        minMax = float2(min(minMax.x,prevMinMax.y),max(minMax.y,prevMinMax.y));
        #else
        #error Please define either _HORIZONTAL_DIR or _VERTICAL_DIR
        #endif
    }
    return minMax;
}

/* Warp function. p,q,depth all in [0,1]
*/
float warp(float p, float q, float depth){
    float low = max(0.0,p-kOverlap);
    float high = min(1.0, q+kOverlap);
    float warpedDepth = (depth-low) / (high-low);
    warpedDepth = depth <= low ? 0.0 : warpedDepth;
    warpedDepth = high <= depth ? 1.0 : warpedDepth;

    return warpedDepth;
}

SM_FORMAT blurPass(int2 pixel, uint2 minMaxLayer)
{
    const int2 offset = -(_KERNEL_WIDTH / 2);
    const float layerStep = 1.0/LVSM_LAYERS;
    float2 moments = float2(0);
    float weight = 1.0/(_KERNEL_WIDTH * _KERNEL_WIDTH);   //Box Filter for now

    [unroll]
    for(int x=0; x<_KERNEL_WIDTH; x++){
        for(int y=0; y<_KERNEL_WIDTH; y++){
            int2 idx = clamp(pixel + offset + int2(x,y), int2(0), _TEX_SIZE);
            float depth = gSrcTex[idx].x;
            depth = warp(minMaxLayer.x * layerStep,minMaxLayer.y * layerStep + layerStep, depth);
            moments += float2(depth, depth * depth) * weight;
        }
    }

    return moments;
}



[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    #ifdef _MINMAX_PASS
        float2 minMax = minMaxPass(pixel);
        #ifdef _HORIZONTAL_DIR
            gDstTex[pixel] = minMax;
        #elif defined _VERTICAL_DIR
            //Convert to layer and stor in the minMax tex
            const float layerStep = 1.0/LVSM_LAYERS;
            uint2 layerMinMax = uint2(floor(minMax / layerStep));
            gLayersMinMaxOut[pixel] = layerMinMax;
        #endif

    #elif defined _BLUR_PASS
        SM_FORMAT blurredDepth = SM_FORMAT(1.f);
        uint2 minMax = gLayersMinMax[pixel];
        blurredDepth = blurPass(pixel,minMax);

        gDstTex[pixel] = blurredDepth;
    #else
    #error Please define either _MINMAX_PASS or _BLUR_PASS
    #endif
}
