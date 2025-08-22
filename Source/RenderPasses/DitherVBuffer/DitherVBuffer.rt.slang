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

#include "Dither.slangh"

float getObjectHash()
{
    if(gObjectHashType == OBJECT_HASH_QUADS) return hash3D(float3(InstanceID(), GeometryIndex(), PrimitiveIndex() / 2u));
    return hash3D(float3(InstanceID(), GeometryIndex(), HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE));
    // raster ver:
    //if(gObjectHashType == OBJECT_HASH_QUADS) return hash(float2(InstanceID() + GeometryIndex(), PrimitiveIndex() / 2u));
    //return hash(float2(InstanceID() + GeometryIndex(), HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE));
}

#ifndef ALPHA_TEXTURE_LOD
#define ALPHA_TEXTURE_LOD 0
#endif
#ifndef CULL_BACK_FACES
#define CULL_BACK_FACES 1
#endif
#ifndef TRANSPARENCY_WHITELIST
#define TRANSPARENCY_WHITELIST 0
#endif

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    float gDLSSCorrectionStrength;

    float gMinVisibility; // everything below (and including) this transparency will be resolved via russian roulette or dither
    int gAlignMotionVectors; // align motion vectors to grid spacing when moving
}

#define MIN_VISIBILITY 0.001

StructuredBuffer<uint> gTransparencyWhitelist;

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float2> gMotion;
RWTexture2D<float> gOpacity; // mask for TAA (0 = no transparency)
#define OPAQUE_MASK 0.9
RWTexture2D<float4> gColor; // shaded vbuffer

struct RayData
{
#ifndef FULL_STOCHASTIC
    float4 color;
    float rayT; // input: TMax, output: TCurrent
#endif
    float minAlphaT; // RayT value of closest alpha-blended hit (For TAA mask, not required for DLSS)
};

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gScene.camera.data.frameWidth / gScene.camera.data.focalLength, gScene.camera.data.frameHeight / gScene.camera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

float2 getAlignedMotionVector(float2 mvec, bool isTransparent)
{
    uint2 frameDim = DispatchRaysDimensions().xy;
    uint2 pixel = DispatchRaysIndex().xy;

    float align = 0.0;
    #if DITHER_MODE == DITHER_MODE_PER_PIXEL_2X2
    align = 2.0;
    #endif
    #if DITHER_MODE == DITHER_MODE_PER_PIXEL_3X3
    align = 3.0;
    #endif
    #if DITHER_MODE == DITHER_MODE_PER_PIXEL_4X4
    align = 4.0;
    #endif
    #if DITHER_MODE == DITHER_MODE_DITHER_TEMPORAL_AA
    align = 5.0;
    #endif
    
    if (!isTransparent || align == 0.0 || !gAlignMotionVectors) return mvec;

    // Apply dithering to motion vector
    if(abs(mvec.x) < 1e-4) mvec.x = 0;
    if(abs(mvec.y) < 1e-4) mvec.y = 0;
    float2 pixelMvec = mvec * frameDim;
    float2 pixelMin = floor(pixelMvec / align) * align;
    float2 pixelMax = ceil(pixelMvec / align) * align;
    float2 f = frac(pixelMvec / align);

    float rng = hash3D(float3(pixel, gFrameCount));
    // choose between min and max based on rng
    if (rng > f.x)
    {
        if((rng - f.x) / (1 - f.x) > f.y) pixelMvec = pixelMin;
        else pixelMvec = float2(pixelMin.x, pixelMax.y);
    }
    else // rng <= f.x
    {
        if(rng/f.x > f.y) pixelMvec = float2(pixelMax.x, pixelMin.y);
        else pixelMvec = pixelMax;
    }
    //pixelMvec = round(mvec * frameDim / align) * align; // round to grid
    mvec = pixelMvec / frameDim;

    return mvec;
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
    gMotion[pixel] = getAlignedMotionVector(prevUV - (float2(pixel) + 0.5) / float2(frameDim), rayData.minAlphaT <= RayTCurrent());
    gOpacity[pixel] = rayData.minAlphaT < RayTCurrent() ? OPAQUE_MASK : 0.0;

#ifdef FULL_STOCHASTIC
    gColor[pixel] = shadeEnvmap(-WorldRayDirection());
#else
    rayData.color.rgb += rayData.color.w * shadeEnvmap(-WorldRayDirection()).rgb;
    rayData.color.w = 0.0;
    gColor[pixel] = rayData.color;
#endif
}

bool useRussianRouletteForAlphaTest(RayData rayData)
{
#ifdef FULL_STOCHASTIC
    return true;
#else
#ifdef FULL_BASELINE
    return false;
#else
    return rayData.color.w <= max(MIN_VISIBILITY, gMinVisibility);
#endif
#endif
}

bool isAlphaTested(uint materialID)
{
    #if TRANSPARENCY_WHITELIST == 0
    return false;
    #else
    uint isTransparent = (gTransparencyWhitelist[materialID / 32u] & (1u << (materialID % 32u)));
    return isTransparent == 0u;
    #endif
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

    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    #if (ALPHA_TEXTURE_LOD != 0) || (ENABLE_HASH_GRIDS != 0)
    float2 ddx, ddy; // texture differentials: for hash grid coordinates or texture LOD
    computeRayDifferentials(triangleHit, WorldRayDirection(), RayTCurrent(), gScene.camera, float2(frameDim), ddx, ddy);
    #endif

    #if ALPHA_TEXTURE_LOD != 0
    let lod = ExplicitGradientTextureSampler(ddx, ddy);
    #else
    let lod = ExplicitLodTextureSampler(0.0);
    #endif

    if(isAlphaTested(materialID))
    {
        // use default alpha test if not marked as transparent
        if (gScene.materials.alphaTest(v, materialID, lod))
            IgnoreHit();
        return;
    }

    let material = gScene.materials.getMaterial(materialID);
    float alpha = material.evalOpacity(gScene.materials, v, lod);
    if(alpha <= 0.01) IgnoreHit(); // no hit
    if(alpha >= 1.0) return; // opaque

    rayData.minAlphaT = min(rayData.minAlphaT, RayTCurrent()); // store if alpha was encountered

    #ifndef FULL_BASELINE

    if(!useRussianRouletteForAlphaTest(rayData)) return; // accept this as hit
    // below: russian roulette for alpha test

    #if COVERAGE_CORRECTION == COVERAGE_CORRECTION_DLSS
    // measured for RR, but works for STD as well
    float dlssAlpha = saturate(0.0113 + 1.6560 * alpha - 0.8210 * alpha * alpha + 0.1560 * alpha * alpha * alpha);
    alpha = lerp(alpha, dlssAlpha, gDLSSCorrectionStrength);
    #endif
    #if COVERAGE_CORRECTION == COVERAGE_CORRECTION_FSR
    // measured for RR (but not good for STD)
    // float fsrAlpha = saturate(0.0142 + 1.3693 * alpha - 0.1821 * alpha * alpha - 0.2135 * alpha * alpha * alpha);
    float fsrAlpha = alpha;
    // special measuement for STD
    if (alpha > 0.320314) fsrAlpha = saturate(-1.0204 + 6.0689 * alpha - 6.7421 * alpha * alpha + 2.6931 * alpha * alpha * alpha);
    else if (alpha < 0.202436) fsrAlpha = saturate(1.3113 * alpha + 0.6793 * alpha * alpha - 10.9520 * alpha * alpha * alpha);
    alpha = lerp(alpha, fsrAlpha, gDLSSCorrectionStrength);
    #endif

    DitherInfo d = {};
    d.pixel = pixel;
    d.frameIndex = gFrameCount;
    d.posW = v.posW;
    d.alpha = alpha;
    #if (ENABLE_HASH_GRIDS != 0)
    uint2 texDim = material.getDimensions(gScene.materials);
    float2 texAspect = float2(1.0, float(texDim.y) / float(texDim.x));
    d.posUV = v.texC * texAspect;
    d.maxDerivUV = max(length(ddx * texAspect), length(ddy * texAspect));
    #endif

    #if DITHER_MODE == DITHER_MODE_DISABLED

    #endif
    #if (DITHER_MODE == DITHER_MODE_PER_PIXEL_2X2)
    if (alpha < getPixelDitherThreshold2x2(d))
            IgnoreHit();
    #endif
    #if (DITHER_MODE == DITHER_MODE_PER_PIXEL_3X3)
    if (alpha < getPixelDitherThreshold3x3(d))
        IgnoreHit();
    #endif
    #if (DITHER_MODE == DITHER_MODE_PER_PIXEL_4X4)
    if (alpha < getPixelDitherThreshold4x4(d))
            IgnoreHit();
    #endif
    #if (DITHER_MODE == DITHER_MODE_DITHER_TEMPORAL_AA)
    if (alpha < ditherTemporalAAThreshold(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_RUSSIAN_ROULETTE
    if(!winRoulette(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_PERIODIC
    if(!winRotation(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_HASH_GRID
    if(!winHashGrid2D(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_FRACTAL_DITHERING
    if (!winFractalDither(d, ddx * texAspect, ddy * texAspect, gScene.camera.data.projMat))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_BLUE_NOISE3D
    if (!winBlueNoise3D(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_STBN
    if(alpha < getSTBNThreshold(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_SURFACE_STBN
    if (alpha < getHashSTBNThreshold(d))
        IgnoreHit();
    #endif

    #endif // FULL_BASELINE
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, TriangleHit triangleHit, bool isTransparent)
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

    return getAlignedMotionVector(mvec, isTransparent);
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

#ifdef FULL_STOCHASTIC
    float3 color = shadeVBuffer(triangleHit, pixel, frameDim, -WorldRayDirection()).rgb;
    {
        // write to vbuffer (last entry)
        gColor[pixel] = float4(color, 1.0);
#else
    float4 shadedColor = shadeVBuffer(triangleHit, pixel, frameDim, -WorldRayDirection());
    float alpha = 1.0;
    uint materialID = gScene.getMaterialID(getGeometryInstanceID());
    if (!useRussianRouletteForAlphaTest(rayData) && !isAlphaTested(materialID))
        alpha = shadedColor.a;
    rayData.color.rgb += rayData.color.w * alpha * shadedColor.rgb;
    rayData.color.w = rayData.color.w * (1.0 - alpha);
    rayData.rayT = RayTCurrent(); // continue ray here (if color.w > 0)

    if(rayData.color.w <= MIN_VISIBILITY)
    {
        // write to vbuffer (last entry)
        gColor[pixel] = rayData.color;
#endif
        gVBuffer[pixel] = HitInfo(triangleHit).getData();
        gMotion[pixel] = calcMotionVector(pixel, frameDim, triangleHit, rayData.minAlphaT <= RayTCurrent());
        gOpacity[pixel] = rayData.minAlphaT <= RayTCurrent() ? OPAQUE_MASK : 0.0;
    }
    
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);

    RayData data;
    data.minAlphaT = ray.tMax; // no alpha encountered yet
    #ifndef FULL_STOCHASTIC
    data.color = float4(0, 0, 0, 1); // rbb+visibility
    #endif
    uint rayFlags = 0;
    if (CULL_BACK_FACES) rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

    #ifndef FULL_STOCHASTIC
    for (uint i = 0; i < 256; ++i)
    {
        data.rayT = ray.tMax; // overwrite with max t
    #endif
        TraceRay(gScene.rtAccel, rayFlags, kInstanceCameraParticle, 0, rayTypeCount, 0, ray.toRayDesc(), data);
    #ifndef FULL_STOCHASTIC
        ray.tMin = data.rayT; // set ray t from hitpoint to new start
        if(data.color.w <= MIN_VISIBILITY || ray.tMin >= ray.tMax) break;
    }
    #endif
}
