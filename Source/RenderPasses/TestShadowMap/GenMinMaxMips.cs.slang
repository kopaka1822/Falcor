import SMStructs;

Texture2D<float2> gMinMaxSrc;
RWTexture2D<float2> gMinMaxDst;

#define FLT_MAX 3.40282e38

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    uint2 srcPixel = pixel * 2;
    float2 val[4];
    val[0] = gMinMaxSrc[srcPixel];
    val[1] = gMinMaxSrc[uint2(srcPixel.x + 1, srcPixel.y)];
    val[2] = gMinMaxSrc[uint2(srcPixel.x, srcPixel.y + 1)];
    val[3] = gMinMaxSrc[uint2(srcPixel.x + 1, srcPixel.y + 1)];

    bool oneValid = false;
    float2 minMax = float2(FLT_MAX, 0);
    [unroll]
    for (uint i = 0; i < 4; i++) {
        if (any(val[i] >= 0)) {
            minMax.x = min(val[i].x, minMax.x);
            minMax.y = max(val[i].y, minMax.y);
            oneValid = true;
        }
    }

    if (!oneValid)
        minMax = float2(-1.0);

    gMinMaxDst[pixel] = minMax;
}
