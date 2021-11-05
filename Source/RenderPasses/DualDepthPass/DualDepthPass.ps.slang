import Scene.Raster;

RasterizerOrderedTexture2D<float> primaryDepth; // this is the foremost depth buffer

float main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Depth
{
    if (alphaTest(vsOut, triangleIndex)) discard;

    float depth = vsOut.posH.z;
    const int2 uv = int2(vsOut.posH.xy);

    // compare depth value with primary depth
    float bufferDepth = primaryDepth[uv];
    [branch] if(bufferDepth > depth)
    {
        // swap if values are smaller
        primaryDepth[uv] = depth;
        depth = bufferDepth;
    }

    return depth;
}
