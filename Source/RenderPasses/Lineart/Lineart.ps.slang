import Utils.Color.ColorHelpers; // RGBToYCgCo(c); YCgCoToRGB(c);

cbuffer StaticCB
{
    float gDiffuseStrength;
    float gPosStrength;
};

Texture2D<float4> gDiffuseTex;
Texture2D<float4> gPosTex;
Texture2D<float4> gNormalTex;

SamplerState gSampler;

float4 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    float4 currentDiffuse = gDiffuseTex.Sample(gSampler, texC);
    float3 currentPosition = gPosTex.Sample(gSampler, texC).xyz;
    float3 currentNormal = gNormalTex.Sample(gSampler, texC).xyz;

    if (all(currentNormal == 0))
        return 1.0;
    
    float outline = 0.0;

    // Get the dimensions of the diffuse texture
    uint width, height;
    gDiffuseTex.GetDimensions(width, height);
    float2 texelSize = float2(1.0 / width, 1.0 / height);
    
    // Offsets to check neighboring pixels
    float2 offsets[4] = { float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };

    for (int i = 0; i < 4; i++)
    {
        float2 neighborC = texC + offsets[i] * texelSize; // Adjust the size based on your texture resolution
        float4 neighborDiffuse = gDiffuseTex.Sample(gSampler, neighborC);
        float3 neighborPosition = gPosTex.Sample(gSampler, neighborC).xyz;
        float3 neighborNormal = gNormalTex.Sample(gSampler, neighborC).xyz;


        float posDiff = abs(dot(currentNormal, normalize(neighborPosition - currentPosition))) * gPosStrength;
        if (all(neighborNormal == 0))
            posDiff = 1.0; // background
        outline += max(0.0, posDiff - 0.6) / 0.4 * 4.0;

        float3 colorDiff = abs(currentDiffuse.rgb - neighborDiffuse.rgb);
        outline += dot(colorDiff, 1.0) * gDiffuseStrength;
    }

    outline = 1.0 - saturate(outline * 0.25);
    
    // Output the final color, white for no outline, black for outline
    return float4(outline, outline, outline, 1.0);
}
