#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"
#include "Scene/Lighting/Lighting.slangh"
#include "Scene/Lighting/RayShadow.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;

#ifndef ALPHA_TEXTURE_LOD
#define ALPHA_TEXTURE_LOD 0
#endif
#ifndef CULL_BACK_FACES
#define CULL_BACK_FACES 1
#endif
#ifndef TRANSPARENCY_WHITELIST
#define TRANSPARENCY_WHITELIST 0
#endif

#define MIN_VISIBILITY 0.001

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
}

StructuredBuffer<uint> gTransparencyWhitelist;

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float2> gMotion;
//RWTexture2D<float4> gTransparent; // rgba transparent color
RWTexture2D<float4> gColor;

struct RayData
{
    float4 color;
    float rayT; // input: TMax, output: TCurrent
};

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gScene.camera.data.frameWidth / gScene.camera.data.focalLength, gScene.camera.data.frameHeight / gScene.camera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    HitInfo hitInfo = { };
    float3 rayDirNoJitter = gScene.camera.computeRayPinhole(pixel, frameDim, /*no jitter*/false).dir;
    float3 prevEnvMapDir = mul(gScene.camera.data.prevViewMat, float4(rayDirNoJitter, 0.0)).xyz;
    float2 prevUV = ViewSpaceToUV(prevEnvMapDir);
    gVBuffer[pixel] = hitInfo.getData();
    gMotion[pixel] = prevUV - (float2(pixel)+0.5) / float2(frameDim);
    
    rayData.color.rgb += rayData.color.w * shadeEnvmap(-WorldRayDirection()).rgb;
    rayData.color.w = 0.0;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    uint materialID = gScene.getMaterialID(instanceID);

    // lod
    #if ALPHA_TEXTURE_LOD != 0
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    float2 ddx, ddy;
    computeRayDifferentials(triangleHit, WorldRayDirection(), RayTCurrent(), gScene.camera, float2(frameDim), ddx, ddy);
    let lod = ExplicitGradientTextureSampler(ddx, ddy);
    #else
    let lod = ExplicitLodTextureSampler(0.0);
    #endif

    #if TRANSPARENCY_WHITELIST != 0
    uint isTransparent = (gTransparencyWhitelist[materialID / 32u] & (1u << (materialID % 32u)));
    if(isTransparent == 0u)
    {
        // use default alpha test if not marked as transparent
        if (gScene.materials.alphaTest(v, materialID, lod))
            IgnoreHit();
        return;
    }
    #endif

    let material = gScene.materials.getMaterial(materialID);   
    float alpha = material.evalOpacity(gScene.materials, v, lod);
    if(alpha < 0.01) IgnoreHit(); // no hit
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, TriangleHit triangleHit)
{
    float3 prevPosW = gScene.getPrevPosW(triangleHit);

    float2 pixelPos = float2(pixel) + 0.5;
    float4 prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, float4(prevPosW, 1.0));
    return calcMotionVector(pixelPos, prevPosH, frameDim) + float2(gScene.camera.data.jitterX, -gScene.camera.data.jitterY); // Remove camera jitter from motion vector
}

float computeShadowImpl(uint2 pixel, uint lightIndex, float3 posW, float3 normalW)
{
    // posW + normal?
    return traceShadowRay(pixel, posW, normalW, lightIndex);
}

[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;  
    
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    // lod
    #if ALPHA_TEXTURE_LOD != 0
    float2 ddx, ddy;
    computeRayDifferentials(triangleHit, WorldRayDirection(), RayTCurrent(), gScene.camera, float2(frameDim), ddx, ddy);
    let lod = ExplicitGradientTextureSampler(ddx, ddy);
    #else
    let lod = ExplicitLodTextureSampler(0.0);
    #endif  

    // eval alpha to decide wheter transparent hit or final vbuffer entry (opaque)
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    uint materialID = gScene.getMaterialID(instanceID);
    let material = gScene.materials.getMaterial(materialID);   
    float alpha = material.evalOpacity(gScene.materials, v, lod);

    rayData.color.rgb += rayData.color.w * alpha * shadeVBuffer(triangleHit, pixel, frameDim, -WorldRayDirection()).rgb;
    rayData.color.w = rayData.color.w * (1.0 - alpha);
    rayData.rayT = RayTCurrent(); // continue ray here (if color.w > 0)

    if(rayData.color.w <= MIN_VISIBILITY)
    {
        gVBuffer[pixel] = HitInfo(triangleHit).getData();
        gMotion[pixel] = calcMotionVector(pixel, frameDim, triangleHit);
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);

    RayData data;
    data.color = float4(0,0,0,1); // rgb+visibility
    uint rayFlags = 0;
    if (CULL_BACK_FACES) rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
    
    for(uint i = 0; i < 256; ++i)
    {
        data.rayT = ray.tMax; // overwrite with max t
        TraceRay(gScene.rtAccel, rayFlags, kInstanceCameraParticle, 0, rayTypeCount, 0, ray.toRayDesc(), data);
        ray.tMin = data.rayT; // set ray t from hitpoint to new start
        if(data.color.w <= MIN_VISIBILITY || ray.tMin >= ray.tMax) break;
    }
    
    //gTransparent[pixel] = data.color;
    gColor[pixel] = data.color;
}
