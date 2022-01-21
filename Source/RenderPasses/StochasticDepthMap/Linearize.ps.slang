Texture2DMS<float> sdepth;

cbuffer CameraCB
{
    float zNear;
    float zFar;
};

struct PsIn
{
    float2 texC : TEXCOORD;
    float4 posH : SV_POSITION;
    uint sampleIndex : SV_SampleIndex;
};

float main(PsIn i) : SV_TARGET0
{
    uint2 xy = uint2(i.posH.xy);
    float nonLinear = sdepth.Load(xy, i.sampleIndex);
    float linearDepth = zNear * zFar / (zFar + nonLinear * (zNear - zFar));
#ifdef NORMALIZE
    // scale to 0, 1 for debug purposes
    return (linearDepth - zNear) / (zFar - zNear);
#endif
    return linearDepth;
}
