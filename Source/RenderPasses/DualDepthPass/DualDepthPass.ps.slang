import Scene.Raster;

void main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_TARGET
{
    if (alphaTest(vsOut, triangleIndex)) discard;
}
