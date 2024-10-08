#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;


Texture2D<PackedHitInfo> gVBuffer;

RWTexture2D<uint> gOutLayersNeededMax;
RWTexture2D<uint> gOutLayersNeededMin;

cbuffer CB
{
    uint gSMSize;   //Pixel size
    float2 gSMNearFar;
    float gOverlap; //Layer overlap
    float4x4 gViewProj;   //SM View Projection
}

//Texture Sampler
SamplerState gShadowSamplerPoint;

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    HitInfo hit = HitInfo(gVBuffer[pixel]);
    if(hit.isValid()){
        VertexData v = {};
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);

        float4 shadowPos = mul(gViewProj, float4(v.posW, 1));
        shadowPos.xyz /= shadowPos.w;
        shadowPos.y *= -1;  //Flip
        shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]

        float depth = (shadowPos.w - gSMNearFar.x) / (gSMNearFar.y - gSMNearFar.x);   //Linear depth in [0,1]

        //Check if outside
        if(any(shadowPos.xy < 0.0) || any(shadowPos.xy > 1.0))
            return;

        //Get layered shadow value
        const float step = 1.0/LVSM_LAYERS;
        const uint selectedLayer = uint(floor(depth / step));

        //SM pixel
        uint2 smPixel = uint2(shadowPos.xy * gSMSize);

        //Execute Min and Max
        uint origMin;
        InterlockedMin(gOutLayersNeededMin[smPixel], selectedLayer, origMin);
        uint origMax;
        InterlockedMax(gOutLayersNeededMax[smPixel], selectedLayer, origMax);
    }
}
