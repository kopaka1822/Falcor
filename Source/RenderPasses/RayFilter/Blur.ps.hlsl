Texture2D<uint> gInput;

uint main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    uint2 pixel = uint2(svPos.xy);
    uint center = gInput[pixel];

    if (countbits(center) < 2) center = 0;
    return center;
}
