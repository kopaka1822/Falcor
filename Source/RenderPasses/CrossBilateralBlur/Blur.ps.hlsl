Texture2D gSrcTex;
Texture2D gDepthTex;

SamplerState gSampler;

struct BlurPSIn
{
    float2 texC : TEXCOORD;
    float4 pos : SV_POSITION;
};

// static constants
static float2 gUV; // uv of current sample
static float2 gDUV; // current delta uv
static float gDepth; // depth of current sample

void AddSample(float sampleAO, float sampleDepth, float depthSlope, uint d, inout float AO, inout float weightSum)
{
    // weights from HBAO+
    const float BlurSigma = ((float)KERNEL_RADIUS+1.0) * 0.5;
    const float BlurFalloff = 1.0 / (2.0*BlurSigma*BlurSigma);
    const float Sharpness = 16.0;

    sampleDepth -= depthSlope * d;
    float dz = abs(sampleDepth - gDepth) * Sharpness;

    float w = exp2(-float(d * d)*BlurFalloff - dz * dz);
    //float w = exp2(- dz * dz);
    //float w = 1.0;
    AO += w * sampleAO;
    weightSum += w;
}

void BlurDirection(inout float AO, inout float weightSum)
{
    float depthSlope;
    [unroll]
    for(uint d = 1; d <= KERNEL_RADIUS; d++)
    {
        float2 sampleUv = gUV + d * gDUV;
        float sampleAO = gSrcTex.SampleLevel(gSampler, sampleUv, 0).x;
        float sampleDepth = gDepthTex.SampleLevel(gSampler, sampleUv, 0).x;
        // set depth slope after obtaining the first depth sample
        if(d == 1) depthSlope = sampleDepth - gDepth;

        AddSample(sampleAO, sampleDepth, depthSlope, d, AO, weightSum);
    }
}

float4 main(BlurPSIn pIn) : SV_TARGET
{
    gUV = pIn.texC;

    gSrcTex.GetDimensions(gDUV.x, gDUV.y);
    //duv = rcp(duv) * float2(DIR);
    gDUV = float2(1.0 / gDUV.x, 1.0 / gDUV.y) * float2(DIR);
    
    // initial weight (of center sample)
    float AO = gSrcTex.SampleLevel(gSampler, gUV, 0).x;
    gDepth = gDepthTex.SampleLevel(gSampler, gUV, 0).x;
    float weightSum = 1.0;

    // compute ao and weights for positive and negative direction
    BlurDirection(AO, weightSum);
    gDUV = -gDUV; // reverse direction
    BlurDirection(AO, weightSum);

    return AO / weightSum;
}