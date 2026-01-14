Texture2D<uint> gPathLength;
RWTexture2D<float4> gColor;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const int2 pixel = id.xy;

    int2 frameDim = 0u;
    gPathLength.GetDimensions(frameDim.x, frameDim.y);

// path radius
#define PR 1
#define PR_COUNT ((1 + 2 * PR) * (1 + 2 * PR))
    // compute path variance
    float pathMean = 0.0;
    float pathMeanSq = 0.0;

    [unroll] for (int dx = -PR; dx <= PR; ++dx)
        [unroll] for (int dy = -PR; dy <= PR; ++dy)
        {
            int2 coord = clamp(pixel + int2(dx, dy), int2(0, 0), frameDim - int2(1, 1));
            let pathLen = gPathLength[coord];
            pathMean += pathLen;
            pathMeanSq += pathLen * pathLen;
        }

    pathMean /= float(PR_COUNT);
    pathMeanSq /= float(PR_COUNT);

    float pathVar = pathMeanSq - pathMean * pathMean;

// median filter for color
#define MR 2
#define MR_COUNT ((1 + 2 * MR) * (1 + 2 * MR))
    float4 colors[MR_COUNT]; // RGB color with avg color as alpha
    uint i = 0;
    [unroll] for (int dx = -MR; dx <= MR; ++dx)
        [unroll] for (int dy = -MR; dy <= MR; ++dy)
        {
            int2 coord = clamp(pixel + int2(dx, dy), int2(0, 0), frameDim - int2(1, 1));
            float3 col = gColor[coord].rgb;
            colors[i++] = float4(col, (col.r + col.g + col.b) / 3.0);
        }

    // sort registers (insertion sort)
    [unroll] for (i = 1; i < MR_COUNT; ++i)
    {
        let c = colors[i];
        [unroll] for (uint j = i; j > 0 && colors[j - 1].w > c.w; --j)
        {
            colors[j] = colors[j - 1];
            colors[j - 1] = c;
        }
    }

    float4 medianColor = 0.0;
    float weightSum = 0.0;
    // average of all colors
    [unroll] for (i = 2; i < MR_COUNT - 2; ++i)
    {
        let c = colors[i];
        float w = exp(-abs(int(i) - MR_COUNT / 2) * 0.1);
        medianColor += w * c;
        weightSum += w;
    }

    medianColor.rgb = medianColor.rgb / weightSum;
    medianColor.rgb = colors[MR_COUNT / 2].rgb;
    medianColor.a = 1.0;

    // blend between color and median color based on variance
    float4 color = gColor[pixel];

    float varianceMin = 0.4;
    float varianceMax = 100.0;
    // normalize path variance
    pathVar = saturate((pathVar - varianceMin) / (varianceMax - varianceMin));
    //pathVar = sqrt(pathVar);
    //gColor[pixel] = float4(pathVar.xxx, 1.0);
    gColor[pixel] = lerp(color, medianColor, pathVar);
    //gColor[pixel] = medianColor;
}
