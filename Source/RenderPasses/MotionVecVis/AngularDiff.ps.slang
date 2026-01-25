//Texture2D<float2> gMotionVec;
Texture2D<float3> gCurPosW;
Texture2D<float3> gPrevPosW;

SamplerState s;

struct PSInput
{
    //float2 texCoord : TEXCOORD0;
};

cbuffer PerFrameCB
{
    float3 camPos; // current frame 
    float scale;
};

float4 main(float2 uv: TEXCOORD, float4 svPos: SV_POSITION) : SV_TARGET
{
    // sample center
    uint2 dim = 0u;
    gCurPosW.GetDimensions(dim.x, dim.y);
    float3 posW = gCurPosW[uint2(dim.x / 2u, dim.y / 2u)];
    float3 w_unit = normalize(posW - camPos);

    // sample previous position
    float3 prevPosW = gPrevPosW.Sample(s, uv).xyz;
    float3 d = prevPosW - camPos;
    float3 d_unit = normalize(d);

    // target function
    float t = dot(w_unit, d_unit);
    //t = acos(t);

    return float4(t, t, t, 1.0);
}
