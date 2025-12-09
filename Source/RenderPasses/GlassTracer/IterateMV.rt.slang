#include "Shared.slang"

cbuffer PerFrame
{
    uint gIterations;
    bool gForcePathLength; // if true forces identical path length
};

#define EXTRA_BOUNCES 4

Texture2D<PackedHitInfo> gVBuffer; // final hit
Texture2D<uint> gReflectiveMask;
Texture2D<uint> gLocalPathLength;
RWTexture2D<float3> gNewRayDir; // TODO remove this and calculate it from gMotion
RWTexture2D<float3> gLastRayDir; // direction to P
RWTexture2D<float2> gMotion;

struct PlaneHit
{
    float3 P;
    float T;
};

float sqr(float x)
{
    return x * x;
}

float3 getRayTangent(float3 rayDir)
{
    return normalize(cross(rayDir, float3(0.0, 1.0, 0.0)));
}
float3 getRayBitangent(float3 rayDir, float3 rayT)
{
    return normalize(cross(rayDir, rayT));
}

[shader("raygeneration")]
void rayGen()
{
    if (gIterations < 2) return; // nothing to do

    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    const uint n = gLocalPathLength[pixel];
    const uint reflectiveMask = gReflectiveMask[pixel];
    UniformSampleGenerator sg = UniformSampleGenerator(pixel, 0); // only used for deterministic brdf sampling interface

    HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (!hit.isValid()) return; // nothing to iterate
    if (hit.getType() != HitType::Triangle) return; // unsupported hit type
    TriangleHit triangleHit = hit.getTriangleHit();
    let vP = gScene.getVertexData(triangleHit.instanceID, triangleHit.primitiveIndex, triangleHit.getBarycentricWeights());
    const float3 P = vP.posW; // TODO prevPosW?
    const float3 N = -gLastRayDir[pixel];
    float3 lastPlaneN = vP.faceNormalW;
    if (dot(lastPlaneN, N) < 0.0) lastPlaneN = -lastPlaneN;

    float3 newDir = gNewRayDir[pixel];
    float3 C0 = gScene.camera.data.prevPosW;

    // remember best result so far
    float bestDiff = 3.402823e+38; // FLT_MAX
    float3 bestDir = newDir;
    float2 nextS = 0.0;
    uint fails = 0;
    float stepMultiplier = 1.0; // beta from wenzel et al.

    for (uint i = 1; i < gIterations; ++i)
    {
        RayDesc ray;
        ray.Origin = C0;
        ray.Direction = bestDir;
        ray.TMin = 0.0;
        ray.TMax = gScene.camera.data.farZ;
        if (fails > 0) stepMultiplier = stepMultiplier * 0.5;
        else stepMultiplier = min(1.0, stepMultiplier * 2.0);

        // shift ray with stepMultiplier
        float3 rayT = getRayTangent(ray.Direction);
        float3 rayB = getRayBitangent(ray.Direction, rayT);
        ray.Direction = ray.Direction + stepMultiplier * (nextS.x * rayT + nextS.y * rayB);
        // update rayT, rayB
        rayT = getRayTangent(ray.Direction);
        rayB = getRayBitangent(ray.Direction, rayT);
        RayDiff rd = RayDiff(float3(0.0), float3(0.0), rayT, rayB);

        float3 initial_dir = ray.Direction;
        bool foundBetter = false;
        uint maxHits = n;
        if (!gForcePathLength) maxHits += EXTRA_BOUNCES;

        RayData data;
        uint rayFlags = 0;
        if (CULL_BACK_FACES) rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

        // iterate through hits
        for (uint iHit = 0; iHit < maxHits; ++iHit)
        {
            // calculate hit with plane (P, N)
            PlaneHit phit; // hit with virtual P-plane
            PlaneHit hit;  // actual hit
            float3 nextRayDir = 0.0;
            bool hasHit = false;
            phit.T = dot(P - ray.Origin, N) / dot(ray.Direction, N);
            phit.P = ray.Origin + phit.T * ray.Direction;

            float diff = 3.402823e+38;

            if (gForcePathLength && iHit == maxHits - 1)
            {
                diff = length(phit.P - P);
            }
            else // continue tracing rays
            {
                TraceRay(gScene.rtAccel, rayFlags, kInstanceCameraParticle, 0, rayTypeCount, 0, ray, data);
                if (HitInfo(data.hit).isValid())
                {
                    hasHit = true;
                    TriangleHit triangleHit = HitInfo(data.hit).getTriangleHit();
                    float3 barycentrics = triangleHit.getBarycentricWeights();
                    StaticVertexData triangleVertices[3];
                    let v = gScene.getVertexData(triangleHit.instanceID, triangleHit.primitiveIndex, barycentrics, triangleVertices);
                    hit.P = v.posW;
                    hit.T = distance(v.posW, ray.Origin);
                    // transfer ray and differential
                    rd = rd.propagate(ray.Origin, ray.Direction, hit.T, v.faceNormalW);
                    // shading data needs to be loaded for brdf evaluation
                    let materialID = gScene.getMaterialID(triangleHit.instanceID);
                    let lod = ExplicitLodTextureSampler(0.0);
                    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -ray.Direction, lod);
                    IMaterialInstance mi = gScene.materials.getMaterialInstance(sd, lod, (uint)MaterialInstanceHints::AdjustShadingNormal);
                    let bsdf = mi.getProperties(sd);
                    BSDFSample bsdfSample;
                    mi.setRoughness(0.0); // set zero roughness for ray differentials (use the most probable path)
                    uint activeLobes = (uint)LobeType::Delta | (uint)LobeType::Specular;
                    bool doReflection = (reflectiveMask & (1 << iHit)) != 0;
                    if (!doReflection)
                    {
                        sd.mtl.setActiveLobes(activeLobes & (uint)LobeType::Transmission);
                        if (!mi.sample(sd, sg, bsdfSample, true /*Importance sampling*/))
                            doReflection = true; // fallback to reflection
                    }
                    if (doReflection)
                    {
                        sd.mtl.setActiveLobes(activeLobes & (uint)LobeType::Reflection);
                        if (!mi.sample(sd, sg, bsdfSample, true /*Importance sampling*/))
                            bsdfSample.weight = 0.0;
                    }
                    // prepare ray differentials after reflection/refraction
                    float3 unnormalizedN; // will be initialized by the prepareRayDiffAtHitPoint
                    float3 normals[3];
                    float2 dUVdx, dUVdy;
                    float2 dBarydx, dBarydy;
                    prepareRayDiffAtHitPoint(sd.faceN, triangleVertices, barycentrics, ray.Direction,
                        gScene.getWorldMatrix(triangleHit.instanceID), gScene.getInverseTransposeWorldMatrix(triangleHit.instanceID),
                        rd, unnormalizedN, normals, dBarydx, dBarydy, dUVdx, dUVdy
                    );
                    float3 ShadingN = normalize(unnormalizedN);
                    if (doReflection)
                    {
                        reflectRayDifferential(rd, ray.Direction, unnormalizedN, ShadingN, dBarydx, dBarydy, normals);
                    }
                    else // refraction
                    {
                        // calculate eta from bsdfSamople.wo instead of sd.IoR to account for glossy surfaces
                        float NdD = dot(ShadingN, ray.Direction);
                        float NdT = dot(ShadingN, bsdfSample.wo);
                        float eta = sqrt(1.0 - NdT * NdT) / sqrt(1.0 - NdD * NdD);
                        refractRayDifferential(rd, ray.Direction, unnormalizedN, ShadingN, dBarydx, dBarydy, normals, eta, bsdfSample.wo);
                    }

                    // prepare next ray
                    nextRayDir = bsdfSample.wo;
                    ray.Origin = sd.computeNewRayOrigin(doReflection);
                }
            }

            if (!gForcePathLength)
            {
                // eval every hit error
                bool testPPlane = phit.T > 0.0 && dot(N, ray.Direction) < 0.0;
                if (testPPlane)
                {
                    diff = sqr(length(phit.P - P));
                    if (hasHit)
                        diff += sqr( max(0.0, dot(lastPlaneN, hit.P - P))); // penalize if in front of geometric plane (but not on or behind)
                    diff *= (1 + 0.1 * abs(n - (iHit + 1))); // larger errors if throughput / path length differs
                }
            }

            if (diff < bestDiff)
            {
                bestDiff = diff;
                bestDir = ray.Direction;
                // calc current solution for ray2 differential (phit.P + st * dO = P <=> st * dO = P - phit.P)
                nextS = solveLinearEq(rd.dOdx, rd.dOdy, P - phit.P);
                foundBetter = true;
            }

            if (!hasHit) break; // finished with this iteration
            ray.Direction = nextRayDir;
            if (all(nextRayDir == 0.0)) break; // terminated
        }

        if (!foundBetter) fails += 1;
        else fails = 0;
    }

    // write final results
    gNewRayDir[pixel] = bestDir;
    float2 prevUV = ViewSpaceToUV(mul(gScene.camera.data.prevViewMat, float4(bestDir, 0.0)).xyz);
    float2 mvec = prevUV - (float2(pixel) + 0.5) / float2(frameDim);
    gMotion[pixel] = mvec;
}
