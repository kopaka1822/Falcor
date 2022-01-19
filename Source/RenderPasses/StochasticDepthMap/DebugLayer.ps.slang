Texture2DMS<float> sdepth;

float main(float2 uv : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    uint2 xy = uint2(svPos.xy);

    //float value = sdepth.sample[SAMPLE_INDEX][xy];
    float value = sdepth.Load(xy, SAMPLE_INDEX);
    return value;
}
