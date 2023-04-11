Texture2D<uint> gInput;

uint main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    uint2 pixel = uint2(svPos.xy);
    uint center = gInput[pixel];

    uint rayCount = countbits(center);

    //uint numWaves = WaveGetLaneCount();
    uint numWaves = WaveActiveSum(1);

    // count of all other threads (not including self)
    uint otherRayCount = WaveActiveSum(rayCount) - rayCount;

    float threshold = 0.4f;
    
    //if (otherRayCount < uint(threshold * numWaves))
    if(rayCount == 1)
        center = 0;
    
    return center;
}
