import Scene.Raster;

cbuffer CameraCB
{
    float zNear;
    float zFar;
};

#ifdef LINEARIZE_DEPTHS
float main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_Depth
#else
void main(VSOut vsOut, uint triangleIndex : SV_PrimitiveID) : SV_TARGET
#endif
{
    if (alphaTest(vsOut, triangleIndex)) discard;

    #ifdef LINEARIZE_DEPTHS
    //return vsOut.posH.w;
    return (vsOut.posH.w - zNear) / (zFar - zNear); // normalize to [0, 1] linear, because the depth buffer clamps all values between [0, 1]
    #endif
}
