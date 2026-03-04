#include "Shared.slang"

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
    float gRoughnessCutoff;             // no reflections for surfaces with higher roughness
    bool gForceZeroRoughness;           // forces roughness to be zero for reflection/refraction rays
    bool gForceMotionVectorCalculation; // forces motion vectors to be calculated even if neither camera nor vertex was moving
    uint gMaxStack;                     // maxiumum stack size for indirections
    float gTextureGradientScaling;      // scaling factor for texture gradients
    bool gPreventVbufferCurvedReflection; // prevents the paths of curved reflection from being taken as vbuffer reference position
    bool gPreventVbufferRefractiveReflection; // prevents the paths of reflections from being taken as vbuffer reference, if the surface does also refract
    float gShadowTransmissionMultiplier; // multiplied to material transmission for shadows, to make them darker
}
#define FLAG_PREVENT_VBUFFER_WRITE 1
#define FLAG_PREVENT_PATH_LENGTH_INCREMENT 2

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float2> gMotion;
RWTexture2D<float2> gBackupMotion;
RWTexture2D<float4> gColor;  // shaded vbuffer
RWTexture2D<float> gDepth;   // non-linear depth for DLSS
RWTexture2D<float> gLinearDepth;
RWTexture2D<float> gPosDiff;     // expected position difference when moving one pixel
RWTexture2D<float3> gLastRayDir; // direction to P (incomming ray direction)
RWTexture2D<uint> gLocalPathLength; // integer path length (bounces)
RWTexture2D<unorm float> gIsBackupMotion; // 1 if mvec == bmvec

SamplerState S; // linear sampler




float4 shadeEnvmap(float3 viewDir)
{
    return float4(gScene.envMap.eval(-viewDir) * gEnvmapIntensity, 1.0);
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, float3 prevPosW, float3x4 viewTransform, bool camMotion)
{
    if (!camMotion)
        return 0.0;
    
    float4 prevPosTWorld = float4(mul(viewTransform, float4(prevPosW, 1.0)), 1.0);
    float4 prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, prevPosTWorld);

    float2 prevCrd = prevPosH.xy / prevPosH.w;           // in [-1, 1]
    float2 prevUV = (prevCrd * float2(0.5, -0.5)) + 0.5; // in [ 0, 1]
    float2 mvec = prevUV - CurrentUVJittered();

    return mvec;
}

float linearToNonLinearZ(float l)
{
    float n = gScene.camera.data.nearZ;
    float f = gScene.camera.data.farZ;
    l = clamp(l, n, f);
    return ((n * f) / l - f) / (n - f);
}

// deep shadow ray
float3 traceShadowRay(uint2 pixel, float3 posW, uint lightIndex)
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

    float3 visibility = 1.0;
    float visibilityCutoff = 0.01; // visibility below this threshold is considered to be zero
    
    RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH > rayQuery;
    const uint instanceMask = getBestParticleShadowPlane(ray.Direction);
    uint rayFlags = RAY_FLAG_NONE;
    if (gShadowTransmissionMultiplier > 0.0) rayFlags |= RAY_FLAG_FORCE_NON_OPAQUE;
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

            if (opacity == 1.0 && gShadowTransmissionMultiplier > 0.0) // check transmission
            {
                // hardcoded for good looking visual
                float IoR = 1.8;
                float3 rayDir = -ray.Direction;
                if (rayQuery.CandidateTriangleFrontFace()) IoR = 1.0 / IoR; // flip ior
                else rayDir = refract(rayDir, -v.faceNormalW, 1.0 / IoR); // pretend ray came from previously refracted direction

                // simple fresenl
                float cosTheta = abs(dot(rayDir, v.faceNormalW));
                float F0 = pow(abs((1.0 - IoR) / (1.0 + IoR)), 2.0);
                float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
                float transmissionFactor = 1.0 - fresnel;
                //transmissionFactor = 1.0;
                visibility = visibility * material.evalTransmission(gScene.materials, v.texC, lod) * transmissionFactor * gShadowTransmissionMultiplier;
            }
            else
                visibility = visibility * (1.0 - opacity);
                
            if (all(visibility <= visibilityCutoff))
                return 0.0;
        }
    }

    if (rayQuery.CommittedStatus() != COMMITTED_NOTHING)
        return 0.0; // hit opaque => occluded

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility;
}

float3 evalAnalytic(uint2 pixel, const ShadingData sd, const IMaterialInstance mi, inout UniformSampleGenerator sg, out float importance)
{
    importance = 0.0;
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
            float3 shadow = traceShadowRay(pixel, sd.computeNewRayOrigin(!transmission), i);
            color += unshadowed * shadow;
            importance += dot(shadow * ls.Li, 0.33); // importance without material texture
        }
    }
    importance = gAnalyticIntensity * importance;
    return gAnalyticIntensity * color;
}

struct VBufferReservoir
{
    float weightSum;
    // data
    float depth;
    float2 mvec;
    float2 bmvec; // backup motion vector
    PackedHitInfo vbuffer;
    float3 lastRayDir;
    float posDiff;
    uint localPathLength;
    bool isBackupMotion; // true if path is allowed as backup path (first hit or first refractive hit)

    // update function, returns true if reservoir should be updated, false if old data should be kept
    [mutating]
    bool update(float3 localContribution, float rng, uint sflags /*flags from stack frame*/)
    {
        if (sflags & FLAG_PREVENT_VBUFFER_WRITE)
            return false;

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
        gBackupMotion[pixel] = bmvec;
        gDepth[pixel] = linearToNonLinearZ(depth);
        gLinearDepth[pixel] = depth;
        gPosDiff[pixel] = posDiff;
        gLastRayDir[pixel] = lastRayDir;
        gLocalPathLength[pixel] = localPathLength;
        gIsBackupMotion[pixel] = isBackupMotion ? 1.0 : 0.0;
    }
};

// stack frame for indirections (if reflection and refraction occur at once)
struct StackFrame
{
    float3 rayOrigin;
    uint curPathLength;
    float3 rayDir;
    float linearZ;
    float3 thp; // path throughput
    uint flags;
    // ^ 12 floats
    
#if USE_TEXTURE_LOD != 0
    RayDiff rd; // 12 floats
#endif
#ifdef USE_VIEW_TRANSFORM
    float3x4 viewTransform; // 12 floats
#endif
#if MVEC == MVEC_RAY_DIFF
    float3 ray2origin; // direction is the same as ray.dir
    RayDiff rd2; // 12 floats
    // ^ 15 floats
#endif
#if MVEC == MVEC_REVERSE_RD
    float3x3 Jpp;
    float3x3 Jpd;
    float3x3 Jdp;
    float3x3 Jdd;
    float3 prevNormal;
    // ^ 39 floats
#endif
};

///////////////// Stack implementation /////////////////////
RWStructuredBuffer<StackFrame> gStack;
static uint curStack = 0;

uint GetStackIndex(uint index)
{
    uint2 size = DispatchRaysDimensions().xy;
    uint offset = DispatchRaysIndex().y * size.x + DispatchRaysIndex().x;
    return size.x * size.y * index + offset;
}

void PushStack(StackFrame f)
{
    gStack[GetStackIndex(curStack++)] = f;
}

StackFrame PopStack()
{
    return gStack[GetStackIndex(--curStack)];
}

bool StackEmpty()
{
    return curStack == 0;
}

bool StackFull()
{
    return curStack >= gMaxStack;
}
////////////////////////////////////////////////////////

float3 calcBarycentrics(float3 p, StaticVertexData v[3])
{
    float3 a = v[0].position;
    float3 b = v[1].position;
    float3 c = v[2].position;
    float3 v0 = b - a;
    float3 v1 = c - a;
    float3 v2 = p - a;
    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d11 = dot(v1, v1);
    float d20 = dot(v2, v0);
    float d21 = dot(v2, v1);
    float denom = d00 * d11 - d01 * d01;
    float vcoord = (d11 * d20 - d01 * d21) / denom;
    float wcoord = (d00 * d21 - d01 * d20) / denom;
    float ucoord = 1.0 - vcoord - wcoord; // same as barycentric calculation in falcor
    return float3(ucoord, vcoord, wcoord);
}

StackFrame prepareNextRay(StackFrame s, BSDFSample bsdfSample, ShadingData sd, StaticVertexData triangleVertices[3], float3 triangleBarycentrics, GeometryInstanceID instanceID)
{
    s.thp *= bsdfSample.weight;
    bool transmission = bsdfSample.isLobe(LobeType::Transmission);

    if (!transmission && gPreventVbufferCurvedReflection)
    {
        if (any(triangleVertices[0].normal != triangleVertices[1].normal) || any(triangleVertices[0].normal != triangleVertices[2].normal))
            s.flags |= FLAG_PREVENT_VBUFFER_WRITE;
    }

    #ifdef USE_VIEW_TRANSFORM
    float3 H = -s.rayDir + bsdfSample.wo;
    float lenH = length(H);
    if (lenH > 0.001) // lenH = 0 => raydir = wo => simple transmission, no reflection or refraction
        s.viewTransform = mulAffine(s.viewTransform, matrixMirror(sd.posW, H / lenH));
    #endif

    #if MVEC == MVEC_RAY_DIFF
    // transfer to the current (virtual) plane
    float ray2t = dot(sd.faceN, sd.posW - s.ray2origin) / dot(sd.faceN, s.rayDir);
    s.rd2 = s.rd2.propagate(s.ray2origin, s.rayDir, ray2t, sd.faceN);
    s.ray2origin = s.ray2origin + ray2t * s.rayDir;
    // prepare ray differential data at the virtual intersection
    float3 barycentrics = calcBarycentrics(s.ray2origin, triangleVertices);
    float3 unnormalizedN; // will be initialized by the prepareRayDiffAtHitPoint
    float3 normals[3];
    float2 dUVdx, dUVdy;
    float2 dBarydx, dBarydy;
    prepareRayDiffAtHitPoint(sd.faceN, triangleVertices, barycentrics, s.rayDir,
        gScene.getWorldMatrix(instanceID), gScene.getInverseTransposeWorldMatrix(instanceID),
        s.rd2, unnormalizedN, normals, dBarydx, dBarydy, dUVdx, dUVdy
    );
    float3 ShadingN = normalize(unnormalizedN);

    if (IGNORE_NORMAL_DIFFS) // overwrite normals with shading normal to force zero normal differentials
    {
        normals[0] = ShadingN;
        normals[1] = ShadingN;
        normals[2] = ShadingN;
    }

    // reflect or refract
    if (transmission)
    {
        // calculate eta from bsdfSamople.wo instead of sd.IoR to account for glossy surfaces
        float NdD = dot(ShadingN, s.rayDir);
        float NdT = dot(ShadingN, bsdfSample.wo);
        float eta = sqrt(1.0 - NdT * NdT) / sqrt(1.0 - NdD * NdD);
        refractRayDifferential(s.rd2, s.rayDir, unnormalizedN, ShadingN, dBarydx, dBarydy, normals, eta, bsdfSample.wo);
        //s.rd2 = s.rd2.refract(s.rayDir, bsdfSample.wo, sd.faceN);
    }
    else // reflection
    {
        reflectRayDifferential(s.rd2, s.rayDir, unnormalizedN, ShadingN, dBarydx, dBarydy, normals);
        //s.rd2 = s.rd2.reflect(s.rayDir, bsdfSample.wo); // uses halfway vector as normal
    }
    #endif

    #if MVEC == MVEC_REVERSE_RD
    float3x3 M = m33Zero(); // zero if we ignore normal differentials
    float3 ShadingN = sd.frame.N; // will be overwritten by getShapeMatrix with the correctly flipped version
    if (!IGNORE_NORMAL_DIFFS)
        M = getShapeMatrix(sd.faceN, triangleVertices, triangleBarycentrics, -bsdfSample.wo,
            gScene.getWorldMatrix(instanceID), gScene.getInverseTransposeWorldMatrix(instanceID),
            ShadingN);
    
    if (transmission)
    {
        float NdD = dot(ShadingN, s.rayDir);
        float NdT = dot(ShadingN, bsdfSample.wo);
        float etaInv = sqrt(1.0 - NdD * NdD) / sqrt(1.0 - NdT * NdT);
        refractRRDiff(s.Jpp, s.Jpd, s.Jdp, s.Jdd,
            -bsdfSample.wo, -s.rayDir, ShadingN, etaInv, M);
    }
    else // reflection
    {
        reflectRRDiff(s.Jpp, s.Jpd, s.Jdp, s.Jdd, -bsdfSample.wo, ShadingN, M);
    }
    #endif

    #if USE_TEXTURE_LOD != 0
    // transfer to posW (done in main)
    // s.rd = s.rd.propagate(s.rayOrigin, s.rayDir, distance(s.rayOrigin, sd.posW), sd.faceN);
    // reflect or refract TODO use functions that respect dNdx and dNdy
    if(transmission) s.rd = s.rd.refract(s.rayDir, bsdfSample.wo, sd.faceN);
    else s.rd = s.rd.reflect(s.rayDir, bsdfSample.wo); // uses halfway vector as normal
    #endif

    s.rayOrigin = sd.computeNewRayOrigin(!transmission); // move origin a bit along normal (note: using sd.posW might result in self-intersection)
    #ifdef USE_VIEW_TRANSFORM
    s.viewTransform = mulAffine(s.viewTransform, matrixTranslate(sd.posW - s.rayOrigin));
    #endif
    #if MVEC == MVEC_RAY_DIFF
    s.ray2origin += s.rayOrigin - sd.posW; // account for the above error by also moving the ray2origin accordingly
    #endif
    s.rayDir = bsdfSample.wo;
    return s;
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    StackFrame s;
    s.thp = 1.0;
    s.linearZ = 0.0;
    s.curPathLength = 0;
    s.flags = 0;
    float3 color = 0.0; // color along path
    bool writeMvec = true; // can be set to false (after first refraction) to disable mvec writes
    bool writeBMvec = true; // same for backup motion vector

    bool camMovement = any(gScene.camera.data.viewMat != gScene.camera.data.prevViewMat);
    if (gForceMotionVectorCalculation) camMovement = true;
    
    VBufferReservoir curVBuffer;
    curVBuffer.weightSum = 0.0;
    curVBuffer.mvec = 0.0;
    curVBuffer.bmvec = 0.0;
    curVBuffer.depth = 0.0;
    curVBuffer.vbuffer = {};

    #ifdef USE_VIEW_TRANSFORM
    s.viewTransform = matrixIdentity();
    #endif
    #if MVEC == MVEC_REVERSE_RD
    s.Jpp = m33Identity();
    s.Jpd = m33Zero();
    s.Jdp = m33Zero();
    s.Jdd = m33Identity();
    float3 virtualC1 = 0.0; // will be initialized in first iteration
    #endif

    UniformSampleGenerator sg = UniformSampleGenerator(pixel, gFrameCount);
    s.rayOrigin = gScene.camera.data.posW;
    const float3 nonNormalizedRayDir = gScene.camera.computeNonNormalizedRayDirPinhole(pixel, frameDim);
    s.rayDir = normalize(nonNormalizedRayDir);

    #if USE_TEXTURE_LOD != 0
    float3 dDdx, dDdy;
    computeRayDirectionDifferentials(nonNormalizedRayDir, gScene.camera.data.cameraU, gScene.camera.data.cameraV, frameDim, dDdx, dDdy);
    s.rd = RayDiff(float3(0.f), float3(0.f), dDdx, dDdy);
    #endif

    #if MVEC == MVEC_RAY_DIFF
    s.ray2origin = gScene.camera.data.prevPosW;

    // create tangent frame for ray2
    float3 ray2T = normalize(cross(s.rayDir, float3(0.0, 1.0, 0.0)));
    float3 ray2B = normalize(cross(s.rayDir, ray2T));
    float3 dO = 0.0; // zero dOdx, dOdy
    s.rd2 = RayDiff(dO, dO, ray2T, ray2B);
    #endif

    RayData data;
    uint rayFlags = 0;
    if (CULL_BACK_FACES) rayFlags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

    for (uint i = 0; i < 128 && s.curPathLength < gPathLength; ++i)
    {
        if (s.flags & FLAG_PREVENT_PATH_LENGTH_INCREMENT) s.flags &= ~FLAG_PREVENT_PATH_LENGTH_INCREMENT; // reset flag
        else s.curPathLength++;

        RayDesc ray;
        ray.Origin = s.rayOrigin;
        ray.Direction = s.rayDir;
        ray.TMin = 0.0;
        ray.TMax = gScene.camera.data.farZ;
        TraceRay(gScene.rtAccel, rayFlags, kInstanceCameraParticle, 0, rayTypeCount, 0, ray, data);

        HitInfo hit = HitInfo(data.hit);
        if (!hit.isValid())
        {
            ////////// MISS /////////////
            float3 localContribution = s.thp * shadeEnvmap(-s.rayDir).rgb;
            color += localContribution;
            if (curVBuffer.update(localContribution, sampleNext1D(sg), s.flags))
            {
                float3 rayDirNoJitter = gScene.camera.computeRayPinhole(pixel, frameDim, /*no jitter*/false).dir;
                float3 prevEnvMapDir = mul(gScene.camera.data.prevViewMat, float4(rayDirNoJitter, 0.0)).xyz;
                float2 prevUV = ViewSpaceToUV(prevEnvMapDir);
                if (writeMvec)
                {
                    curVBuffer.mvec = prevUV - (float2(pixel) + 0.5) / float2(frameDim);
                    if (!camMovement)
                        curVBuffer.mvec = 0.0;
                    if (writeBMvec) curVBuffer.bmvec = curVBuffer.mvec;
                }
                curVBuffer.isBackupMotion = writeMvec && writeBMvec;
                    
                RayMissHit rmh;
                rmh.posW = ray.Origin + ray.Direction * ray.TMax; // far plane position
                curVBuffer.vbuffer = rmh.pack();
                curVBuffer.posDiff = 1e38;
                curVBuffer.depth = s.linearZ + ray.TMax; // + far plane
                curVBuffer.localPathLength = s.curPathLength;
                curVBuffer.lastRayDir = s.rayDir;
            }

            if (StackEmpty()) break;

            s = PopStack();
            continue;
        }

        if (hit.getType() != HitType::Triangle)
            return;

        /////////// TRIANGLE HIT /////////////
        TriangleHit triangleHit = hit.getTriangleHit();

        float3 barycentrics = triangleHit.getBarycentricWeights();
        StaticVertexData triangleVertices[3];
        let v = gScene.getVertexData(triangleHit.instanceID, triangleHit.primitiveIndex, barycentrics, triangleVertices);
        bool vertexMoved = any(v.posW != v.prevPosW);
        s.linearZ += distance(s.rayOrigin, v.posW);

        let materialID = gScene.getMaterialID(triangleHit.instanceID);

        // texture LOD calculation
        #if USE_TEXTURE_LOD == 0
        let lod = ExplicitLodTextureSampler(0.0);
        float posDiff = 0.0;
        #else
        // transfer rd to surface
        let posDiffRd = s.rd.propagate(s.rayOrigin, s.rayDir, distance(s.rayOrigin, v.posW), s.rayDir);
        //float posDiff = max(length(posDiffRd.dOdx), length(posDiffRd.dOdy));
        s.rd = s.rd.propagate(s.rayOrigin, s.rayDir, distance(s.rayOrigin, v.posW), v.faceNormalW);
        float posDiff = max(length(s.rd.dOdx), length(s.rd.dOdy));
        float3 unnormalizedN;
        float3 normals[3];
        float2 dUVdx, dUVdy;
        float2 dBarydx, dBarydy;
        prepareRayDiffAtHitPoint(v, triangleVertices, barycentrics, s.rayDir,
            gScene.getWorldMatrix(triangleHit.instanceID), gScene.getInverseTransposeWorldMatrix(triangleHit.instanceID),
            s.rd, unnormalizedN, normals, dBarydx, dBarydy, dUVdx, dUVdy
        );
        let lod = ExplicitGradientTextureSampler(dUVdx * gTextureGradientScaling, dUVdy * gTextureGradientScaling);
        #endif

        #if MVEC == MVEC_REVERSE_RD
        if (s.curPathLength <= 1) // for the first hit, define a virtual plane around C0 as intersection
        {
            float virtualT = dot(gScene.camera.data.prevPosW - v.posW, -ray.Direction);
            virtualC1 = v.posW - ray.Direction * virtualT;
            transferRRDiff(s.Jpp, s.Jpd, s.Jdp, s.Jdd, -ray.Direction, ray.Direction, virtualT);
        }
        else // transfer from current vertex to previous vertex
        {
            float t = distance(s.rayOrigin, v.posW);
            transferRRDiff(s.Jpp, s.Jpd, s.Jdp, s.Jdd,
                -s.rayDir, s.prevNormal, t);
        }
        s.prevNormal = v.faceNormalW;
        #endif

        ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -s.rayDir, lod);
        float opacity = 1.0;
        if (!isAlphaTested(materialID)) opacity = sd.opacity;

        // Create material instance at shading point.
        IMaterialInstance mi = gScene.materials.getMaterialInstance(sd, lod, (uint)MaterialInstanceHints::AdjustShadingNormal);
        let bsdf = mi.getProperties(sd);

        float analyticImportance = 0.0;
        // compute local color contribution
        float3 localContribution = s.thp * opacity * (
            bsdf.emission * gEmissionIntensity +
            (bsdf.diffuseReflectionAlbedo + 0.2 * bsdf.specularReflectionAlbedo) * gAmbientIntensity +
            evalAnalytic(pixel, sd, mi, sg, analyticImportance)
        );
        color += localContribution;

        // compute path importance slightly differently
        // IMaterialInstance miImp = gScene.materials.getMaterialInstance( sd, ExplicitLodTextureSampler(255.0), (uint)MaterialInstanceHints::AdjustShadingNormal);
        // let bsdfImp = miImp.getProperties(sd);
        float pathImportance = dot(s.thp * opacity * (bsdf.emission * gEmissionIntensity +
            //(bsdfImp.diffuseReflectionAlbedo + 0.2 * bsdfImp.specularReflectionAlbedo) * gAmbientIntensity +
            analyticImportance
        ), 0.33);
        // this will also overwrite with pathImportance==0, if nothing else was contributed yet (weightSum = 0.0)
        if (curVBuffer.update(pathImportance, sampleNext1D(sg), s.flags))
        {
            curVBuffer.vbuffer = data.hit;
            curVBuffer.depth = s.linearZ;
            curVBuffer.posDiff = posDiff;
            curVBuffer.localPathLength = s.curPathLength;
            curVBuffer.lastRayDir = s.rayDir;

            if (s.curPathLength <= 1 && MVEC == MVEC_FIRST_HIT) // motion vector from initial intersection
            {
                writeMvec = false;
                curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, matrixIdentity(), camMovement || vertexMoved);
            }

            #ifdef USE_VIEW_TRANSFORM
            if(writeMvec)
                curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, s.viewTransform, camMovement || vertexMoved);
            #endif

            #if MVEC == MVEC_RAY_DIFF
            // transfer to the final plane with point prevPosW and surface normal -s.rayDir
            float ray2t = dot(s.rayDir, v.prevPosW - s.ray2origin);
            // make copies of variables to avoid changing stack data (stack will be updated in prepareNextRay)
            let rd2 = s.rd2.propagate(s.ray2origin, s.rayDir, ray2t, -s.rayDir);
            let ray2origin = s.ray2origin + ray2t * s.rayDir;

            // solve the equation ray2.origin + t * rd2.dOdx + b * rd2.dOdy = prevPosW
            // => t * rd2.dOdx + b * rd2.dOdy = v.prevPosW - ray2.origin
            float2 tb = solveLinearEq(rd2.dOdx, rd2.dOdy, v.prevPosW - ray2origin);
            float3 initDirW = gScene.camera.computeRayPinhole(pixel, frameDim, true /*jitter*/).dir;
            float3 prevDirW = normalize(initDirW + tb[0] * ray2T + tb[1] * ray2B);
            float2 prevUV = ViewSpaceToUV(mul(gScene.camera.data.prevViewMat, float4(prevDirW, 0.0)).xyz);
            if(writeMvec)
                curVBuffer.mvec = prevUV - CurrentUVJittered();
            #endif

            #if MVEC == MVEC_REVERSE_RD
            // final step: solve dD0 = Jpd^-1 * (C0 - virtualC1)
            float3 dPn = gScene.camera.data.prevPosW - virtualC1;
            float3 dD0 = solveForDD0(s.Jpd, dPn, -s.rayDir);
            float3 dDn = mul(s.Jdd, dD0);

            float3 initDirW = gScene.camera.computeRayPinhole(pixel, frameDim, true /*jitter*/).dir;
            float2 prevUV = ViewSpaceToUV(mul(gScene.camera.data.prevViewMat, float4(normalize(initDirW - dDn), 0.0)).xyz);
            if(writeMvec)
                curVBuffer.mvec = prevUV - CurrentUVJittered();
            #endif

            if (writeMvec && !camMovement && !vertexMoved) curVBuffer.mvec = 0.0;
            if (writeBMvec) curVBuffer.bmvec = curVBuffer.mvec;
            curVBuffer.isBackupMotion = writeBMvec;
        }

        // prevent mvec writes in the future, if:
        if (s.curPathLength <= 1 && BMVEC == MVEC_FIRST_HIT) writeBMvec = false;

        // obtain 1 bsdf sample for reflection, and one for refraction
        BSDFSample bsdfSampleReflect;
        BSDFSample bsdfSampleRefract;

        if (bsdf.roughness > gRoughnessCutoff || (s.curPathLength == gPathLength))
        {
            // force absorb
            bsdfSampleReflect.weight = 0.0;
            bsdfSampleRefract.weight = 0.0;

        }
        else
        {
            if (gForceZeroRoughness) mi.setRoughness(0.0);
            // only delta reflections, refractions for next bounce
            uint activeLobes = (uint)LobeType::Delta | (uint)LobeType::Specular;
            sd.mtl.setActiveLobes(activeLobes & (uint) LobeType::Transmission);
            if (!mi.sample(sd, sg, bsdfSampleRefract, true /*Importance sampling*/))
                bsdfSampleRefract.weight = 0.0;
            sd.mtl.setActiveLobes(activeLobes & (uint) LobeType::Reflection);
            if (!mi.sample(sd, sg, bsdfSampleReflect, true /*Importance sampling*/))
                bsdfSampleReflect.weight = 0.0;
            sd.mtl.setActiveLobes((uint) LobeType::All); // allow all types for brdf eval (otherwise direct light (NEE) will be always black)
        }

        bool doReflection = any(s.thp * bsdfSampleReflect.weight > MIN_VISIBILITY);
        bool doRefraction = any(s.thp * bsdfSampleRefract.weight > MIN_VISIBILITY);

        // prevent furhter mvec writes, if:
        if (doRefraction && MVEC == MVEC_FIRST_REFRACT) writeMvec = false;
        if (doRefraction && BMVEC == MVEC_FIRST_REFRACT) writeBMvec = false;

        if (opacity < 1.0)  // this should not affect first refract if it is a simple transmission
        {
            // overwrite refraction with transmission
            bsdfSampleRefract.weight = 1.0 - opacity;
            bsdfSampleRefract.wo = s.rayDir; // keep direction
            bsdfSampleRefract.pdf = 1.0;
            bsdfSampleRefract.lobeType = (uint)LobeType::Transmission;
            s.flags |= FLAG_PREVENT_PATH_LENGTH_INCREMENT;
            bsdfSampleReflect.weight = 0.0; // prevent reflection for now (TODO should be weighted with opacity instead and path increment enabled)
        }

        if (doReflection && doRefraction)
        {
            // put one on the stack
            if (!StackFull())
                PushStack( prepareNextRay(s, bsdfSampleRefract, sd, triangleVertices, barycentrics, triangleHit.instanceID));
            doRefraction = false;
            if (gPreventVbufferRefractiveReflection)
                s.flags |= FLAG_PREVENT_VBUFFER_WRITE; // for the next reflections
        }

        if (doReflection)
            s = prepareNextRay(s, bsdfSampleReflect, sd, triangleVertices, barycentrics, triangleHit.instanceID);
        if (doRefraction)
            s = prepareNextRay(s, bsdfSampleRefract, sd, triangleVertices, barycentrics, triangleHit.instanceID);
        
        if(!doReflection && !doRefraction)
        {
            if (StackEmpty()) break;
            
            s = PopStack();
        }
    }

    // write data
    curVBuffer.writeData(pixel, color);
}
