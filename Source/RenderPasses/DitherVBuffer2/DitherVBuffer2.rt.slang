#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"
//#include "Scene/Lighting/Lighting.slangh"
//#include "Scene/Lighting/RayShadow.slangh"

import Scene.Raytracing;
import Scene.RaytracingInline;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Utils.Math.HashUtils;

#include "Dither2.slangh"

import Utils.Sampling.SampleGeneratorInterface;
import Utils.Sampling.Pseudorandom.Xoshiro;
import Utils.Sampling.Pseudorandom.SplitMix64;
import Utils.Math.BitTricks;

#define DITHER

struct UniformSampleGenerator : ISampleGenerator
{
#if DITHER_MODE == DITHER_MODE_RUSSIAN_ROULETTE || DITHER_MODE == DITHER_MODE_DISABLED 
    __init(uint2 pixel, uint sampleNumber)
    {
        SplitMix64 rng = createSplitMix64(interleave_32bit(pixel), sampleNumber);
        uint64_t s0 = nextRandom64(rng);
        uint64_t s1 = nextRandom64(rng);
        uint seed[4] = { uint(s0), uint(s0 >> 32), uint(s1), uint(s1 >> 32) };
        this.rng = createXoshiro128StarStar(seed);
    }

    [mutating]
    uint next()
    {
        return nextRandom(rng);
    }

    [mutating] void updateRefractMask(uint mask) {}

    Xoshiro128StarStar rng;
#endif
    uint floatRngToUint(float rng)
    {
        // rng function expects uniform random value in the upper 24 bits
        //return uint(rng * 16777216.0) << 8u;
        // return uint(rng / 0x1p-24) << 8u;
        // return uint(rng * 4294967295);
        return uint(rng * 255) << 24;
    }
#if DITHER_MODE == DITHER_MODE_STBN
    __init(uint2 pixel, uint frameIndex)
    {
        initial = uint3(pixel, frameIndex);
        offset = 0;
    }

    [mutating]
    uint next()
    {
        uint3 p = initial;
        p.x = (p.x + offset % 128u) % 128u;
        p.y = (p.y + (offset / 128u) % 128u) % 128u;
        p.z = (p.z + (offset / 16384u) % 64u) % 64u;
        // p.z = (p.z + offset % 64u) % 64u;
        this.offset = jenkinsHash(offset); // next offset
        //this.offset += 0;
        return floatRngToUint(gSpatioTemporalBlueNoiseTex[p]);
    }

    [mutating]
    void updateRefractMask(uint mask) {
        this.offset = jenkinsHash(mask + offset);
    }

    uint3 initial;
    uint offset;
#endif
#if DITHER_MODE == DITHER_MODE_PER_PIXEL_2X2
    __init(uint2 pixel, uint frameIndex)
    {
        this.pixel = pixel;
        this.frameIndex = frameIndex;
        offset = 0;
    }

    [mutating]
    uint next()
    {
        uint4 mask = get2x2Permutation(offset); // mask with [0, 1, 2, 3] permutation
        uint2 blueNoiseOffset = uint2(offset % 5, offset / 5) * 12u; // additional offset into the 64x64 blue noise texture, to pick different thresholds for each matrix
        offset += 5; // 5 is coprime to 24 (number of matrices for 2x2 dither)
        uint2 p = pixel;
        if (gRotatePattern)
            p = applyRotation(p, frameIndex, 2, 2); // rotate lookup based on frame index
        uint lookup = (p.x % 2u) + 2u * (p.y % 2u);
        uint v = mask[lookup]; // 0, 1, 2 or 3
        float rng = (v + gBlueNoise64x64Tex[(pixel + blueNoiseOffset) % 64u]) / 4.0;
        return floatRngToUint(rng);
    }

    [mutating] void updateRefractMask(uint mask) {}

    uint2 pixel;
    uint frameIndex;
    uint offset;
#endif
#if DITHER_MODE == DITHER_MODE_PER_PIXEL_3X3
    __init(uint2 pixel, uint frameIndex)
    {
        this.pixel = pixel;
        this.frameIndex = frameIndex;
        offset = 0;
    }

    [mutating]
    uint next()
    {
        uint3x3 mask = get3x3Permutation(offset); // mask with [0, 1, 2, 3] permutation
        uint2 blueNoiseOffset = uint2(offset % 5, offset / 5) * 12u; // additional offset into the 64x64 blue noise texture, to pick different thresholds for each matrix
        offset += 7;
        uint2 p = pixel;
        if (gRotatePattern)
            p = applyRotation(p, frameIndex, 3, 3); // rotate lookup based on frame index
        p = p % 3u;
        uint v = mask[p.y][p.x]; // 0-8
        float rng = (v + gBlueNoise64x64Tex[(pixel + blueNoiseOffset) % 64u]) / 9.0;
        return floatRngToUint(rng);
    }

    [mutating] void updateRefractMask(uint mask) {}

    uint2 pixel;
    uint frameIndex;
    uint offset;
#endif
};

float getObjectHash()
{
    if(gObjectHashType == OBJECT_HASH_QUADS) return hash3D(float3(InstanceID(), GeometryIndex(), PrimitiveIndex() / 2u));
    return hash3D(float3(InstanceID(), GeometryIndex(), HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE));
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

    int gAlignMotionVectors; // align motion vectors to grid spacing when moving
    uint gPathLength; // 1 = only primary ray

    float gAmbientIntensity;
    float gAnalyticIntensity;
    float gEmissionIntensity;
    float gEnvmapIntensity;

    float gPointLightClip; // shadow
    float gShadowLodBias;
    bool gEnableShadows;

    float gRoughnessCutoff; // no reflections for surfaces with higher roughness
}

float4 shadeEnvmap(float3 viewDir)
{
    return float4(gScene.envMap.eval(-viewDir) * gEnvmapIntensity, 1.0);
}

#define MIN_VISIBILITY 0.001

StructuredBuffer<uint> gTransparencyWhitelist;

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float2> gMotion;
RWTexture2D<float4> gColor; // shaded vbuffer
RWTexture2D<float> gDepth; // non-linear depth for DLSS

struct RayData
{
    PackedHitInfo hit; // RGBA32UINT
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
    HitInfo hitInfo = { };
    rayData.hit = hitInfo.getData();
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

    let lod = ExplicitLodTextureSampler(0.0);

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
    #if COVERAGE_CORRECTION == COVERAGE_CORRECTION_XESS
    float xessAlpha = pow(alpha, 0.423);
    //float xessAlpha = saturate(0.0512 + 3.0635 * alpha - 4.3486 * alpha * alpha + 2.2454 * alpha * alpha * alpha);
    alpha = lerp(alpha, xessAlpha, gDLSSCorrectionStrength * 0.5); // take 0.5 by default, because it is too strong during motion otherwise
    #endif

    DitherInfo d = {};
    d.pixel = pixel;
    d.frameIndex = gFrameCount;
    d.posW = v.posW;

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
    if(alpha < getRouletteThreshold(d))
        IgnoreHit();
    #endif
    #if DITHER_MODE == DITHER_MODE_STBN
    if(alpha < getSTBNThreshold(d))
        IgnoreHit();
    #endif
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, TriangleHit triangleHit, bool isTransparent, float4x4 viewTransform)
{
    float3 prevPosW = gScene.getPrevPosW(triangleHit);

    float2 pixelPos = float2(pixel) + 0.5;
    float4 prevPosTWorld = mul(viewTransform, float4(prevPosW, 1.0));
    float4 prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, prevPosTWorld);

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

[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;    

    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    rayData.hit = HitInfo(triangleHit).getData();

}

float linearToNonLinearZ(float l)
{
    float n = gScene.camera.data.nearZ;
    float f = gScene.camera.data.farZ;
    l = clamp(l, n, f);
    return ((n * f) / l - f) / (n - f);
}

// deep shadow ray
float traceShadowRay(uint2 pixel, float3 posW, uint lightIndex)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);
    RayDesc ray;
    ray.Origin = posW.xyz; // assume posW already offset with computeNewRayOrigin
    ray.TMin = 0.0;
    if (light.type == uint(LightType::Point))
    {
        ray.Direction = normalize(light.posW - posW.xyz);
        ray.TMax = distance(light.posW, posW.xyz) - gPointLightClip;
    }
    else if (light.type == uint(LightType::Directional))
    {
        ray.Direction = -light.dirW;
        ray.TMax = gScene.camera.data.farZ;
    }

    float visibility = 1.0;
    float visibilityCutoff = 0.01; // visibility below this threshold is considered to be zero
    
    RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH > rayQuery;
    const uint instanceMask = getBestParticleShadowPlane(ray.Direction);
    uint rayFlags = RAY_FLAG_NONE;
    rayQuery.TraceRayInline(gScene.rtAccel, rayFlags, instanceMask, ray);
    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            float t = rayQuery.CandidateTriangleRayT();
            // alpha test
            const TriangleHit hit = getCandidateTriangleHit(rayQuery);
            const uint materialID = gScene.getMaterialID(hit.instanceID);
            const VertexData v = gScene.getVertexData(hit);
           
            let material = gScene.materials.getMaterial(materialID);
            let lod = ExplicitLodTextureSampler(gShadowLodBias);
            float opacity = material.evalOpacity(gScene.materials, v, lod);
            
            visibility = visibility * (1.0 - opacity);
            if (visibility <= visibilityCutoff)
                return 0.0;
        }
        else
            return 0.0; // hit opaque => occluded
    }

    if (rayQuery.CommittedStatus() != COMMITTED_NOTHING)
        return 0.0; // hit opaque => occluded

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility;
}

float3 evalAnalytic(uint2 pixel, const ShadingData sd, const IMaterialInstance mi, inout UniformSampleGenerator sg)
{
    if (gAnalyticIntensity <= 0.0)
        return 0.0;
    
    const uint lightCount = gScene.getLightCount();
    float3 color = 0.0;
    for (uint i = 0; i < lightCount; ++i)
    {
        AnalyticLightSample ls;
        evalLightApproximate(sd.posW, gScene.getLight(i), ls);
        // check which side
        const uint lobeTypes = mi.getLobeTypes(sd);
        const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
        const bool hasTransmission = (lobeTypes & uint(LobeType::Transmission)); // || (sd.opacity < 1.0);
        float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
        const float kMinCosTheta = 0.0001;
        if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
            continue;

        // shadow ray
        bool transmission = NdotL < 0.0;
        float V = traceShadowRay(pixel, sd.computeNewRayOrigin(!transmission), i);
        color += max(V * mi.eval(sd, ls.dir, sg) * ls.Li, 0.0);
    }
    return gAnalyticIntensity * color;
}

float getAnalyticWeight(uint2 pixel, const ShadingData sd, const IMaterialInstance mi, UniformSampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    float maxW = 0.0;
    for (uint i = 0; i < lightCount; ++i)
    {
        AnalyticLightSample ls;
        // check which side
        const uint lobeTypes = mi.getLobeTypes(sd);
        const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
        const bool hasTransmission = (lobeTypes & uint(LobeType::Transmission)); // || (sd.opacity < 1.0);
        evalLightApproximate(sd.posW, gScene.getLight(i), ls);
        float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
        const float kMinCosTheta = 0.0001;
        if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
            continue;
        maxW = max(maxW, dot(mi.eval(sd, ls.dir, sg), 0.33));
    }
    return maxW;
}

// mirrors scene at plane with location O and normal N
float4x4 matrixMirror(float3 O, float3 N)
{
    float4x4 M = float4x4(
        1 - 2 * N.x * N.x, -2 * N.x * N.y, -2 * N.x * N.z, 0,
        -2 * N.y * N.x, 1 - 2 * N.y * N.y, -2 * N.y * N.z, 0,
        -2 * N.z * N.x, -2 * N.z * N.y, 1 - 2 * N.z * N.z, 0,
        2 * dot(N, O) * N.x, 2 * dot(N, O) * N.y, 2 * dot(N, O) * N.z, 1
    ); // TODO needs transpose?
    //return M;
    return transpose(M);
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    float3 thp = 1.0; // path throughput
    float3 emission = 0.0; // emission along path
    float linearZ = 0.0;
    uint refractMask = 1u; // tracks refractions (used for dither mask selection)
    float4x4 viewTransform = float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

    UniformSampleGenerator sg = UniformSampleGenerator(pixel, gFrameCount);
    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);

    RayData data;
    uint rayFlags = 0;
    if (CULL_BACK_FACES) rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

    uint i = 0;
    for (; i < gPathLength; ++i)
    {
        TraceRay(gScene.rtAccel, rayFlags, kInstanceCameraParticle, 0, rayTypeCount, 0, ray.toRayDesc(), data);

        HitInfo hit = HitInfo(data.hit);
        if (!hit.isValid())
            break; // miss

        if (hit.getType() != HitType::Triangle)
            return;

        TriangleHit triangleHit = hit.getTriangleHit();
        if(i == 0) // motion vector from initial intersection
            gMotion[pixel] = calcMotionVector(pixel, frameDim, triangleHit, true, viewTransform);
        
        // decide wheter to accept hit or continue tracing
        let v = gScene.getVertexData(triangleHit);
        let materialID = gScene.getMaterialID(triangleHit.instanceID);
        let lod = ExplicitLodTextureSampler(0.0);
        ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -ray.dir, lod);
        linearZ += distance(ray.origin, v.posW);
        
        // check if viewing from wrong side (since adjust shading normal does not always work as expected)
        //if (!sd.frontFacing)
        {
            //sd.frontFacing = true;
            //sd.faceN = -sd.faceN;
            //sd.frame.N = -sd.frame.N;
        }

        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod, (uint) MaterialInstanceHints::AdjustShadingNormal);
        let bsdf = mi.getProperties(sd);

        // emission and ambience at every step (directly available due to bsdf)
        emission += thp * bsdf.emission * gEmissionIntensity;
        emission += thp * bsdf.diffuseReflectionAlbedo * gAmbientIntensity;

        // only delta reflections, refractions for next bounce
        sd.mtl.setActiveLobes((uint)LobeType::Delta | (uint)LobeType::Specular);
        sg.updateRefractMask(refractMask);
        BSDFSample bsdfSample;
        if (!mi.sample(sd, sg, bsdfSample, true /*Importance sampling*/))
            bsdfSample.weight = 0.0;
        
        sd.mtl.setActiveLobes((uint) LobeType::All); // allow all types for brdf eval (otherwise direct light (NEE) will be always black)
        
        DitherInfo d = { };
        d.pixel = pixel;
        d.frameIndex = gFrameCount + 1; // hack to make it independent from transparency
        d.posW = v.posW;

        // float rng = getRouletteThreshold(d);
        float rng = sampleNext1D(sg);

        //float wReflect = saturate(dot(bsdfSample.weight, 0.33));
        //if (wReflect < 0.01)
        //    wReflect = 0.0;
        //float wLocal = max(0.01, saturate(1.0 - wReflect)); // local light weight (TODO replace with mi.eval())
        float wReflect = saturate(dot(bsdfSample.weight * (bsdf.specularReflectionAlbedo + bsdf.specularTransmissionAlbedo), 0.33));
        if (bsdf.roughness > gRoughnessCutoff) wReflect = 0.0;
        if (i == gPathLength - 1) wReflect = 0.0; // force absorb
        float wLocal = saturate(dot(bsdf.diffuseReflectionAlbedo + bsdf.diffuseTransmissionAlbedo, 0.33));
        //if (sd.mtl.isDeltaSpecular())
        //    wLocal = 0.0;
        //wLocal = max(0.01, getAnalyticWeight(pixel, sd, mi, sg));
        wLocal = max(0.01, wLocal); // make sure wLocal always has a small probability, in case wReflect is 0
        
        float pSum = wLocal + wReflect;
        thp *= pSum; // update throughput
        //if (rng < wLocal / pSum)
        if (rng * pSum <= wLocal) // works if pSum and wLocal are zero
        {
            // use this path => shade and output
            thp *= 1.0 / wLocal;
            //gColor[pixel] = float4(emission + thp * shadeVBuffer(triangleHit, pixel, frameDim, -ray.dir).rgb, 1.0);
            gColor[pixel] = float4(emission + thp * evalAnalytic(pixel, sd, mi, sg), 1.0);
            //gColor[pixel] = float4(sd.materialID);
            //gColor[pixel] = float4(dot(bsdf.specularReflectionAlbedo, 0.33), dot(bsdf.diffuseReflectionAlbedo, 0.33), dot(bsdf.diffuseTransmissionAlbedo, 0.33), 1.0);
            gVBuffer[pixel] = data.hit; // this hit was accepted
            gDepth[pixel] = linearToNonLinearZ(linearZ);

            gMotion[pixel] = calcMotionVector(pixel, frameDim, triangleHit, true, viewTransform);
            return;
        }
        else
        {
            // use reflection/refraction path
            thp *= bsdfSample.weight / wReflect; // adjust for non-monochrome weight
            ray.tMin = 1e-6;
            bool transmission = bsdfSample.isLobe(LobeType::Transmission);
            // update refract mask (shift and add 1 bit if refraction)
            refractMask = refractMask << 1u;
            if (transmission) refractMask |= 1u;

            float3 H = -ray.dir + bsdfSample.wo;
            float lenH = length(H);
            if (lenH > 0.001) // lenH = 0 => raydir = wo => simple transmission, no reflection or refraction
                viewTransform = mul(viewTransform, matrixMirror(v.posW, H / lenH));
            ray.origin = sd.computeNewRayOrigin(!transmission);
            ray.dir = bsdfSample.wo;
        }
    }
    
    // => no hit, eval envmap
    if (i == 0) // use envmap motion vector if no hit at all
    {
        float3 rayDirNoJitter = gScene.camera.computeRayPinhole(pixel, frameDim, /*no jitter*/false).dir;
        float3 prevEnvMapDir = mul(gScene.camera.data.prevViewMat, float4(rayDirNoJitter, 0.0)).xyz;
        float2 prevUV = ViewSpaceToUV(prevEnvMapDir);
        gMotion[pixel] = getAlignedMotionVector(prevUV - (float2(pixel) + 0.5) / float2(frameDim), false);
    }

    gColor[pixel] = float4(emission + thp * shadeEnvmap(-ray.dir).rgb, 1.0);
    gVBuffer[pixel] = data.hit;
    gDepth[pixel] = 1.0; // far plane
}
