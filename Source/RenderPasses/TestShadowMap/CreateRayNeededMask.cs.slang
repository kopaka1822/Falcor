import SMStructs;

#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

Texture2D<SM_FORMAT> gShadowMap;
RWTexture2D<unorm float> gRayNeededMask;

cbuffer CB
{
    uint gSMSize;   //Pixel size 
}

//Texture Sampler
SamplerState gShadowSamplerPoint;

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    float2 relPos = float2(pixel)/gSMSize;
    float center = 1;
    float pixelSums = 0;
    // 3x3 Box filter
    for(int x=-1; x<=1 ; x++){
        for(int y=-1; y<=1; y++){
            float2 offset = float2(x,y)/gSMSize;
            float variance = gShadowMap.SampleLevel(gShadowSamplerPoint, relPos+offset, 0).g;
            if((x==0)&&(y==0))
                center = variance;
            pixelSums += variance;
        }
    }
    //Normalize
    pixelSums /= 9;

    float diff = abs(center - pixelSums);
    float output = diff > 1e-3 ? 1.0 : 0.0;

    gRayNeededMask[pixel] = output;
}
