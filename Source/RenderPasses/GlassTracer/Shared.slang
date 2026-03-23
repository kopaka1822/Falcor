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
#define MVEC_FIRST_REFRACT 1
#define MVEC_HALFWAY_REFLECT 2
#ifndef MVEC
#define MVEC MVEC_HALFWAY_REFLECT
#endif
#ifndef BMVEC
#define BMVEC MVEC_FIRST_REFRACT
#endif

// for simplicity of implementation (ideally the first refract does not need the view transform in the stack)
#if MVEC == MVEC_HALFWAY_REFLECT || MVEC == MVEC_FIRST_REFRACT || BMVEC == MVEC_FIRST_REFRACT
#define USE_VIEW_TRANSFORM
#endif

#ifndef USE_OPTICAL_FLOW
#define USE_OPTICAL_FLOW 1
#endif

#ifndef USE_TEXTURE_LOD
#define USE_TEXTURE_LOD 1
#endif

#ifndef IGNORE_NORMAL_DIFFS
#define IGNORE_NORMAL_DIFFS 0
#endif

#define IT_NONE 0
#define IT_FORWARD 1
#define IT_REVERSE 2
#ifndef IT_TECH
#define IT_TECH IT_FORWARD
#endif

#if IT_TECH == IT_FORWARD
#define IF_FORWARD(x) x
#define IF_REVERSE(x)
#elif IT_TECH == IT_REVERSE
#define IF_FORWARD(x)
#define IF_REVERSE(x) x
#else
#define IF_FORWARD(x)
#define IF_REVERSE(x)
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

#ifndef CULL_BACK_FACES
#define CULL_BACK_FACES 1
#endif
#ifndef TRANSPARENCY_WHITELIST
#define TRANSPARENCY_WHITELIST 0
#endif

#define MIN_VISIBILITY 0.01

StructuredBuffer<uint> gTransparencyWhitelist;

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gScene.camera.data.frameWidth / gScene.camera.data.focalLength, gScene.camera.data.frameHeight / gScene.camera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
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

struct RayData
{
    PackedHitInfo hit; // RGBA32UINT
};

[shader("miss")]
void miss(inout RayData rayData: SV_RayPayload)
{
    HitInfo hitInfo = {};
    rayData.hit = hitInfo.getData();
}

// Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
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

    if (isAlphaTested(materialID))
    {
        // use default alpha test if not marked as transparent
        if (gScene.materials.alphaTest(v, materialID, lod))
            IgnoreHit();
        return;
    }

    let material = gScene.materials.getMaterial(materialID);
    float alpha = material.evalOpacity(gScene.materials, v, lod);

    if (alpha <= 0.01) IgnoreHit(); // no hit
    // hit (return)
}

[shader("closesthit")]
void closestHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes) {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    rayData.hit = HitInfo(triangleHit).getData();
}

// mirrors scene at plane with location O and normal N
float3x4 matrixMirror(float3 O, float3 N)
{
    float3x4 M = float3x4(
        1 - 2 * N.x * N.x, -2 * N.y * N.x, -2 * N.z * N.x, 2 * dot(N, O) * N.x,
        -2 * N.x * N.y, 1 - 2 * N.y * N.y, -2 * N.z * N.y, 2 * dot(N, O) * N.y,
        -2 * N.x * N.z, -2 * N.y * N.z, 1 - 2 * N.z * N.z, 2 * dot(N, O) * N.z,
    );

    return M;
}

// translates scene by P
float3x4 matrixTranslate(float3 P)
{
    return float3x4(
        1.0, 0.0, 0.0, P.x,
        0.0, 1.0, 0.0, P.y,
        0.0, 0.0, 1.0, P.z
    );
}

float3x4 matrixIdentity()
{
    return float3x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
    );
}

// multiplies two affine transforms A and B (3x4 matrices)
float3x4 mulAffine(float3x4 A, float3x4 B)
{
    float3x3 RA = (float3x3)A;
    float3x3 RB = (float3x3)B;
    float3 tA = float3(A[0][3], A[1][3], A[2][3]);
    float3 tB = float3(B[0][3], B[1][3], B[2][3]);

    float3x3 AB = mul(RA, RB);
    float3 t = mul(RA, tB) + tA;

    return float3x4(
        AB[0][0], AB[0][1], AB[0][2], t[0],
        AB[1][0], AB[1][1], AB[1][2], t[1],
        AB[2][0], AB[2][1], AB[2][2], t[2]
    );
}

// solves t * T + b * B = R and returns (t, b)
float2 solveLinearEq(float3 T, float3 B, float3 R)
{
    const float epsilon = 1e-6;

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

    if (abs(R2.x) < epsilon) R2.x = 0.0;
    if (abs(R2.y) < epsilon) R2.y = 0.0;

    // Solve the 2×2 linear system
    float det = T2.x * B2.y - T2.y * B2.x;
    if (abs(det) < 1e-8)
        return 0.0; // Degenerate or parallel T/B

    float invDet = 1.0 / det;
    return float2(
        (R2.x * B2.y - R2.y * B2.x) * invDet, // t
                  (T2.x * R2.y - T2.y * R2.x) * invDet  // b
    );
}

// returns current UV coordinate with jitter applied according to Camera::computeRayPinhole
float2 CurrentUVJittered()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    return (float2(pixel) + 0.5) / frameDim + float2(-gScene.camera.data.jitterX, gScene.camera.data.jitterY);
}
