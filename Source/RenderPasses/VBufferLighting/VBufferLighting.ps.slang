#include "Scene/Lighting/Lighting.slangh"
#include "Scene/Lighting/RayShadow.slangh"

Texture2DArray<float> visibilityBuffer;
Texture2D<PackedHitInfo> vbuffer;
Texture2D<float4> rayDir;
Texture2D<float4> transparency;
Texture2D<float> ambient; // ambient occlusion

#ifndef USE_RAY_SHADOW
#define USE_RAY_SHADOW 0
#endif

float computeShadowImpl(uint2 pixel, uint lightIndex, float3 posW, float3 normalW)
{
    #if USE_RAY_SHADOW != 0
    return traceShadowRay(pixel, posW, normalW, lightIndex);
    #else
    uint visWidth, visHeight, visSlices;
    visibilityBuffer.GetDimensions(visWidth, visHeight, visSlices);

    if (lightIndex >= visSlices) return 1.0; // no shadow available
    return visibilityBuffer[uint3(pixel, lightIndex)];
    #endif
}

float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gScene.camera.data.frameWidth / gScene.camera.data.focalLength, gScene.camera.data.frameHeight / gScene.camera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, HitInfo hit)
{
    if (hit.isValid()) // valid hit
    {
        if (hit.getType() != HitType::Triangle)
            return 0.0;
        
        return calcMotionVector(pixel, frameDim, hit.getTriangleHit());
    }
    else // invalid hit => envmap
    {
        float3 rayDirNoJitter = gScene.camera.computeRayPinhole(pixel, frameDim, /*no jitter*/false).dir;
        float3 prevEnvMapDir = mul(gScene.camera.data.prevViewMat, float4(rayDirNoJitter, 0.0)).xyz;
        float2 prevUV = ViewSpaceToUV(prevEnvMapDir);
        float2 mvec = prevUV - (float2(pixel)+0.5) / float2(frameDim);
        return mvec;
    }
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, TriangleHit triangleHit)
{
    float3 prevPosW = gScene.getPrevPosW(triangleHit);

    float2 pixelPos = float2(pixel) + 0.5;
    float4 prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, float4(prevPosW, 1.0));

    float2 prevCrd = prevPosH.xy / prevPosH.w;
#ifdef FALCOR_FLIP_Y
    prevCrd *= float2(0.5, 0.5);
#else
    prevCrd *= float2(0.5, -0.5);
#endif
    prevCrd += 0.5f;
    float2 normalizedCrd = pixelPos / frameDim;
    float2 mvec = prevCrd - normalizedCrd;
    mvec += float2(gScene.camera.data.jitterX, -gScene.camera.data.jitterY); // Remove camera jitter from motion vector
    return mvec;
}

struct PSOut
{
    float4 color : SV_Target0;
    float2 mvec : SV_Target1;
};

PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION)
{
    // pixel coordinates
    uint2 pixel = uint2(svPos.xy);
    uint2 frameDim;
    vbuffer.GetDimensions(frameDim.x, frameDim.y);
    
    const HitInfo hit = HitInfo(vbuffer[pixel]);

#if USE_RAY_DIR == 1
    float3 viewDir = -rayDir[pixel].xyz;
#else
    float3 viewDir = -gScene.camera.computeRayPinhole(pixel, frameDim).dir;
#endif

    float4 color = float4(0, 0, 0, 1);
#if USE_TRANSPARENCY == 1
    color = transparency[pixel];
#endif
    if(color.a > 0.001)
    {
        float4 c = shadeVBuffer(hit, pixel, frameDim, viewDir);
#if USE_AMBIENT == 1
        c.rgb *= ambient[pixel];
#endif
        color.rgb += color.a * c.rgb;
    }
    color.a = 1.0;
    
    PSOut o;
    o.color = color;
    o.mvec = calcMotionVector(pixel, frameDim, hit);
    return o;
}
