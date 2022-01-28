import Scene.Raster;

RasterizerOrderedTexture2D<float> primaryDepth; // this is the foremost depth buffer
RasterizerOrderedTexture2D<float> secondaryDepth; // this is the second depth buffer

float main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Depth
{
    if (alphaTest(vsOut, triangleIndex)) discard;

    const int2 uv = int2(vsOut.posH.xy);

    // read and sort depths
    float depths[3];
    depths[0] = vsOut.posH.z;
    depths[1] = primaryDepth[uv];
    depths[2] = secondaryDepth[uv];

    // 1-pass bubble sort of current depth
    [unroll] for (int i = 0; i < 2; ++i)
    {
        [flatten] if(depths[i] > depths[i + 1])
        {
            float tmp = depths[i];
            depths[i] = depths[i + 1];
            depths[i + 1] = tmp;
        }
    }

    // write back in correct order
    primaryDepth[uv] = depths[0];
    secondaryDepth[uv] = depths[1];
    return depths[1]; // write secondary depth
}
