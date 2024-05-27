Texture2D<float2> gTexC;
Texture2D<uint4> gMaterialTex;

cbuffer PerFrameCB
{
    uint curMaterial;
};

SamplerState gSampler;

float2 main(float2 texC : TEXCOORD) : SV_TARGET0
{
    // material id in x
    float2 texCoord = gTexC.SampleLevel(gSampler, texC, 0).xy;
    float2 dim;
    gMaterialTex.GetDimensions(dim.x, dim.y);
    uint4 material = gMaterialTex[int2(dim * texC)];

    if (material.x != curMaterial)
        return 0.0;
    return texCoord;
}
