import Scene.Raster;

Texture2D<float> prevDepth;

void main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_TARGET
{
    if (alphaTest(vsOut, triangleIndex)) discard;

    float depth = vsOut.posH.z;
    const int2 uv = int2(vsOut.posH.xy);
    float prev = prevDepth[uv];

    // reject when smaller than previous depth
    if (depth <= prev)
        discard;
}
