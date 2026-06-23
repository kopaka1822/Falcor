import Scene.Camera.Camera;
import RTAOData;
import Scene.Intersection;
import Scene.Raytracing;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.MathHelpers;
// sample generator
import Utils.Sampling.SampleGeneratorInterface;
import Utils.Sampling.Pseudorandom.Xoshiro;
import Utils.Sampling.Pseudorandom.SplitMix64;
import Utils.Math.BitTricks;

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
// sample generator end

Texture2D<float4> gWPosTex;
Texture2D<float3> gFaceNormalTex;
RWTexture2D<float> ambientOut;

StructuredBuffer<uint> gTransparencyWhitelist;

#ifndef TRANSPARENCY_WHITELIST
#define TRANSPARENCY_WHITELIST 0
#endif

bool isAlphaTested(uint materialID)
{
#if TRANSPARENCY_WHITELIST == 0
    return true;
#else
    uint isTransparent = (gTransparencyWhitelist[materialID / 32u] & (1u << (materialID % 32u)));
    return isTransparent == 0u;
#endif
}

Texture1D<float4> gSamples;

cbuffer StaticCB
{
    RTAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
    uint frameIndex;
}

struct RayData
{
    float t; // ray intersection
};

[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.t = 1;
}

// Hash function from "Improved Alpha Testing Using Hashed Sampling"
float hash(float2 v)
{
    return frac(1.0e4 * sin(17.0 * v.x + 0.1 * v.y) * (0.1 + abs(sin(13.0 * v.y + v.x))));
}

// 3D hash function from "Improved Alpha Testing Using Hashed Sampling"
float hash3D(float3 v)
{
    return hash(float2(hash(v.xy), v.z));
}

// 4D hash function based on the 3D hash function
float hash4D(float4 v)
{
    // return hash(float2(hash3D(v.xyz), v.w));
    return hash(float2(hash(v.xy), hash(v.zw)));
}

// true => opaque hit, false => miss
bool winOpaqueRoulette(float alpha, float3 posW)
{
    alpha = pow(alpha, 0.25); // strenthen hit chance to produce darker AO
    // float rng = hash4D(float4(posW, 0 * frameIndex));
    float rng = hash3D(posW);
    if (rng > alpha) return false;
    return true;
}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);

    let lod = ExplicitLodTextureSampler(0.0);

    if (isAlphaTested(materialID))
    {
        if (gScene.materials.alphaTest(v, materialID, lod))
            IgnoreHit(); // TODO proper lod
        return;
    }

    // not alpha tested => transparent: eval alpha
    let material = gScene.materials.getMaterial(materialID);
    float alpha = material.evalOpacity(gScene.materials, v, lod);
    if (alpha <= 0.01) IgnoreHit(); // no hit
    if (alpha >= 1.0) return; // opaque

    // decide based on white noise russian roulette if it is a hit
    if (!winOpaqueRoulette(alpha, v.posW))
        IgnoreHit();

    return;
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.t = RayTCurrent();
}

float3 getTangentVector(float3 n)
{
    return abs(n.x) > abs(n.y) ? float3(-n.z, 0.0, n.x) : float3(0.0, n.z, -n.y); // from pbrt
}

[shader("raygeneration")]
void rayGen()
{
    const uint2 svPos = DispatchRaysIndex().xy;
    
    // non-linear depth
    float4 wPos = gWPosTex[svPos];
    if (wPos.w <= 0.0)
    {
        ambientOut[svPos] = 1;
        return;
    }

    // view space normal of current pixel
    float3 faceNormal = normalize(gFaceNormalTex[svPos]);
    // face forward
    if (dot(faceNormal, wPos.xyz - gCamera.data.posW) > 0)
    {
        faceNormal = -faceNormal;
    }

    // determine tangent space
    float3 bitangent = getTangentVector(faceNormal);
    float3 tangent = cross(bitangent, faceNormal);

    // Init sample generator
    UniformSampleGenerator sg = UniformSampleGenerator(svPos, frameIndex);// % 30u);

    RayDesc ray;
    ray.Origin = wPos.xyz;
    ray.Origin += faceNormal * gData.normalScale; // push origin in the direction of the face normal to avoid self intersection
    ray.TMin = 0.00;
    ray.TMax = gData.radius;

    RayData rayData;

    float tSum = 0.0;
    for (uint i = 0; i < gData.spp; i++)
    {
        float pdf; // needed by the sample function
        float3 randDir = sample_cosine_hemisphere_polar(sampleNext2D(sg), pdf);
        ray.Direction = normalize(tangent * randDir.x + bitangent * randDir.y + faceNormal * randDir.z);

        //rayData.t = gData.radius; // set to miss

        TraceRay(
            gScene.rtAccel,
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, // | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 kInstanceCameraParticle /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
            ray, rayData
        );

        // tSum += 1.0-saturate(rayData.t / gData.radius);
        tSum += saturate(rayData.t / gData.radius);
    }

    tSum /= gData.spp;
    ambientOut[svPos] = tSum;
}
