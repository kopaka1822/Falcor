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
import Rendering.Materials.TexLODHelpers;

import Utils.Sampling.SampleGeneratorInterface;
import Utils.Sampling.Pseudorandom.Xoshiro;
import Utils.Sampling.Pseudorandom.SplitMix64;
import Utils.Math.BitTricks;

RWTexture2DArray<float4> gDebugTex;

#define MVEC_FIRST_HIT 0
#define MVEC_HALFWAY_REFLECT 1
#define MVEC_RAY_DIFF 2
#ifndef MVEC
#define MVEC MVEC_HALFWAY_REFLECT
#endif


struct UniformSampleGenerator : ISampleGenerator
{
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

    Xoshiro128StarStar rng;
};

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
    // hit (return)
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, float3 prevPosW, float4x4 viewTransform, bool camMotion)
{
    if (!camMotion)
        return 0.0;
    
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

    return mvec;
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
        float3 unshadowed = max(mi.eval(sd, ls.dir, sg) * ls.Li, 0.0);
        if(any(unshadowed > MIN_VISIBILITY))
        {
            float shadow = traceShadowRay(pixel, sd.computeNewRayOrigin(!transmission), i);
            color += unshadowed * shadow;
        }
    }
    return gAnalyticIntensity * color;
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

float4x4 matrixIdentity()
{
    return float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );
}

// solves t * T + b * B = R and returns (t, b)
float2 solveLinearEq(float3 T, float3 B, float3 R)
{
    // Compute which axis to drop
    float3 sum = abs(T) + abs(B);
    uint drop = (sum.x < sum.y)
                    ? ((sum.x < sum.z) ? 0u : 2u)
                    : ((sum.y < sum.z) ? 1u : 2u);

    float2 T2, B2, R2;
    [branch]
    switch (drop)
    {
        case 0: T2 = T.yz; B2 = B.yz; R2 = R.yz; break; // drop x
        case 1: T2 = T.xz; B2 = B.xz; R2 = R.xz; break; // drop y
        default: T2 = T.xy; B2 = B.xy; R2 = R.xy; break; // drop z
    }

    // Solve the 2×2 linear system
    float det = T2.x * B2.y - T2.y * B2.x;
    if (abs(det) < 1e-8)
        return float2(0.0, 0.0); // Degenerate or parallel T/B

    float invDet = rcp(det);
    return float2(
        (R2.x * B2.y - R2.y * B2.x) * invDet,  // t
        (T2.x * R2.y - T2.y * R2.x) * invDet   // b
    );
}

struct VBufferReservoir
{
    float weightSum;
    // data
    float depth;
    float2 mvec;
    PackedHitInfo vbuffer;

    // update function, returns true if reservoir should be updated, false if old data should be kept
    [mutating]
    bool update(float3 localContribution, float rng)
    {
        float local = dot(localContribution, 1.0);

        // if max weight
        weightSum = max(weightSum, local);
        return weightSum == local; // true if local is max, false otherwise

        // if reservoir sampling
        //weightSum += local;
        //return rng * weightSum <= local; // true if update/replace, false if keep old
    }

    void writeData(uint2 pixel, float3 color)
    {
        gColor[pixel] = float4(color, 1.0);
        gVBuffer[pixel] = vbuffer;
        gMotion[pixel] = mvec;
        gDepth[pixel] = depth;
    }
};

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    float3 thp = 1.0; // path throughput
    float3 color = 0.0; // color along path
    float linearZ = 0.0;
    bool camMovement = any(gScene.camera.data.viewMat != gScene.camera.data.prevViewMat);

    VBufferReservoir curVBuffer;
    curVBuffer.weightSum = 0.0;
    curVBuffer.mvec = 0.0;
    curVBuffer.depth = 0.0;
    curVBuffer.vbuffer = {};

    #if MVEC == MVEC_HALFWAY_REFLECT
    float4x4 viewTransform = matrixIdentity();
    #endif

    UniformSampleGenerator sg = UniformSampleGenerator(pixel, gFrameCount);
    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);

    #if MVEC == MVEC_RAY_DIFF
    Ray ray2; // ray for previous frame
    ray2.origin = gScene.camera.data.prevPosW;
    ray2.dir = ray.dir; // use parallel ray

    // create tangent frame for ray2
    float3 ray2T = normalize(cross(ray2.dir, float3(0.0, 1.0, 0.0)));
    float3 ray2B = normalize(cross(ray2.dir, ray2T));
    float3 dO = 0.0; // zero dOdx, dOdy
    RayDiff rd = RayDiff(dO, dO, ray2T, ray2B);
    #endif

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
        
        let v = gScene.getVertexData(triangleHit);
        bool vertexMoved = any(v.posW != v.prevPosW);
        if (i == 0 && MVEC == MVEC_FIRST_HIT) // motion vector from initial intersection
            curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, matrixIdentity(), camMovement || vertexMoved);
        
        let materialID = gScene.getMaterialID(triangleHit.instanceID);
        let lod = ExplicitLodTextureSampler(0.0);
        ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -ray.dir, lod);
        linearZ += distance(ray.origin, v.posW);

        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod, (uint) MaterialInstanceHints::AdjustShadingNormal);
        let bsdf = mi.getProperties(sd);

        // compute local color contribution
        float3 localContribution = thp * (
            bsdf.emission * gEmissionIntensity +
            bsdf.diffuseReflectionAlbedo * gAmbientIntensity +
            evalAnalytic(pixel, sd, mi, sg)
        );
        color += localContribution;

        if (curVBuffer.update(localContribution, sampleNext1D(sg)))
        {
            curVBuffer.vbuffer = data.hit;
            curVBuffer.depth = linearToNonLinearZ(linearZ);

            #if MVEC == MVEC_HALFWAY_REFLECT
            curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, viewTransform, camMovement || vertexMoved);
#endif

            #if MVEC == MVEC_RAY_DIFF
            // transfer to the final plane with point prevPosW and surface normal -ray2.dir
            float ray2t = dot(ray2.dir, v.prevPosW - ray2.origin);
            rd = rd.propagate(ray2.origin, ray2.dir, ray2t, -ray2.dir);
            ray2.origin = ray2.origin + ray2t * ray2.dir;

            // solve the equation ray2.origin + t * rd.dOdx + b * rd.dOdy = prevPosW
            // => t * rd.dOdx + b * rd.dOdy = v.prevPosW - ray2.origin
            float2 tb = solveLinearEq(rd.dOdx, rd.dOdy, v.prevPosW - ray2.origin);
            // float3 initDirW = gScene.camera.computeRayPinhole(pixel, frameDim).dir;
            float3 initDirW = gScene.camera.computeRayPinhole(pixel, frameDim, false /*no jitter*/) .dir; // reconstruct a vector without jitter
            float3 prevDirW = normalize(initDirW + tb[0] * ray2T + tb[1] * ray2B);
            float2 prevUV = ViewSpaceToUV(mul(gScene.camera.data.prevViewMat, float4(prevDirW, 0.0)).xyz);
            curVBuffer.mvec = prevUV - (float2(pixel) + 0.5) / float2(frameDim);
            if(!camMovement && !vertexMoved) curVBuffer.mvec = 0.0;
#endif
        }

        // use this as vbuffer?


        // only delta reflections, refractions for next bounce
        sd.mtl.setActiveLobes((uint)LobeType::Delta | (uint)LobeType::Specular);
        float rng = sampleNext1D(sg); // rng for next path decision
        BSDFSample bsdfSample;
        if (!mi.sample(sd, sg, bsdfSample, true /*Importance sampling*/))
            bsdfSample.weight = 0.0;
        if (bsdf.roughness > gRoughnessCutoff)
            bsdfSample.weight = 0.0; // force absorb
        if (i == gPathLength - 1)
            bsdfSample.weight = 0.0; // force absorb

        sd.mtl.setActiveLobes((uint) LobeType::All); // allow all types for brdf eval (otherwise direct light (NEE) will be always black)

        // check if additional ray or last intersection

        if (all(thp * bsdfSample.weight <= MIN_VISIBILITY))
        {
            curVBuffer.writeData(pixel, color);
            return;
        }
        else
        {
            // use reflection/refraction path
            thp *= bsdfSample.weight;
            ray.tMin = 1e-6;
            bool transmission = bsdfSample.isLobe(LobeType::Transmission);

            #if MVEC == MVEC_HALFWAY_REFLECT
            float3 H = -ray.dir + bsdfSample.wo;
            float lenH = length(H);
            if (lenH > 0.001) // lenH = 0 => raydir = wo => simple transmission, no reflection or refraction
                viewTransform = mul(viewTransform, matrixMirror(v.posW, H / lenH));
            #endif

            #if MVEC == MVEC_RAY_DIFF
            // transfer to the current plane
            float ray2t = dot(v.faceNormalW, v.posW - ray2.origin) / dot(v.faceNormalW, ray2.dir);
            rd = rd.propagate(ray2.origin, ray2.dir, ray2t, v.faceNormalW);
            ray2.origin = ray2.origin + ray2t * ray2.dir;
            // reflect or refract
            if (transmission) rd = rd.refract(ray2.dir, bsdfSample.wo, v.faceNormalW);
            else rd = rd.reflect(ray2.dir, bsdfSample.wo); // uses halway vector as normal
            ray2.dir = bsdfSample.wo;
            #endif

            ray.origin = sd.computeNewRayOrigin(!transmission);
            ray.dir = bsdfSample.wo;
        }
    }

    // envmap hit
    float3 localContribution = thp * shadeEnvmap(-ray.dir).rgb;
    color += localContribution;
    if(curVBuffer.update(localContribution, sampleNext1D(sg)))
    {
        if (i == 0 || MVEC != MVEC_FIRST_HIT) // use envmap motion vector if no hit at all (If the final hit is an envmap, we can use this with the assumption that the last ray reflected on the same surfaces)
        {
            float3 rayDirNoJitter = gScene.camera.computeRayPinhole(pixel, frameDim, /*no jitter*/ false).dir;
            float3 prevEnvMapDir = mul(gScene.camera.data.prevViewMat, float4(rayDirNoJitter, 0.0)).xyz;
            float2 prevUV = ViewSpaceToUV(prevEnvMapDir);
            curVBuffer.mvec = prevUV - (float2(pixel) + 0.5) / float2(frameDim);
            if (!camMovement)
                curVBuffer.mvec = 0.0;
        }
        curVBuffer.vbuffer = data.hit;
        curVBuffer.depth = linearToNonLinearZ(linearZ);
    }

    curVBuffer.writeData(pixel, color);
}
