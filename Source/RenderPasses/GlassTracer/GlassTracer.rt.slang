#include "Shared.slang"

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
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
    bool gPreventVbufferInternalReflection; // prevents total internal reflections (and succeeding vertices) on the primary path from being taken as vbuffer reference
    int gMaxVbufferRefractions; // after this many refractions along the primary path, mvec and bmvec will be written (even if the surface is specular)
    float gShadowTransmissionMultiplier;      // multiplied to material transmission for shadows, to make them darker
}

#define FLAG_PREVENT_BMVEC_WRITE 1
// disabling mvec automatically disables bmvec (1 | 2 => 3)
#define FLAG_PREVENT_MVEC_WRITE 3
// disabling vbuffer also disables motion vector writes (1 | 2 | 4 => 7)
#define FLAG_PREVENT_VBUFFER_WRITE 7
#define FLAG_PREVENT_PATH_LENGTH_INCREMENT 8

#define WRITE_MVEC(flags) ((flags & FLAG_PREVENT_MVEC_WRITE) != FLAG_PREVENT_MVEC_WRITE)
#define WRITE_BMVEC(flags) ((flags & FLAG_PREVENT_BMVEC_WRITE) != FLAG_PREVENT_BMVEC_WRITE)
#define WRITE_VBUFFER(flags) ((flags & FLAG_PREVENT_VBUFFER_WRITE) != FLAG_PREVENT_VBUFFER_WRITE)

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float2> gMotion;
RWTexture2D<float2> gBackupMotion;
RWTexture2D<float4> gColor;  // shaded vbuffer
RWTexture2D<float> gDepth;   // non-linear depth for DLSS
RWTexture2D<float> gPosDiff;     // expected position difference when moving one pixel
RWTexture2D<float4> gLastRayDir; // direction to P (incomming ray direction) + linear depth in last component
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
        float3 brdf = mi.eval(sd, ls.dir, sg); // f_r from light to view dir
        float3 unshadowed = max(brdf * ls.Li, 0.0);
        if(any(unshadowed > MIN_VISIBILITY))
        {
            float3 shadow = traceShadowRay(pixel, sd.computeNewRayOrigin(!transmission), i);
            color += unshadowed * shadow;
        }
    }
    return gAnalyticIntensity * color;
}

struct VBufferReservoir
{
    float weightSum;
    // data
    float depth;
    float2 mvec;
    #if USE_OPTICAL_FLOW
    float2 bmvec; // backup motion vector
    #endif
    #if USE_VBUFFER
    PackedHitInfo vbuffer;
    #endif
    #if USE_OPTICAL_FLOW
    float3 lastRayDir;
    float posDiff;
    uint localPathLength;
    bool isBackupMotion; // true if path is allowed as backup path (first hit or first refractive hit)
    #endif

    // update function, returns true if reservoir should be updated, false if old data should be kept
    [mutating]
    bool update(float3 localContribution, float rng, uint sflags /*flags from stack frame*/)
    {
        if(!WRITE_VBUFFER(sflags))
            return false;

        float local = dot(localContribution, 1.0);

        // if max weight
        weightSum = max(weightSum, local);
        return weightSum == local; // true if local is max, false otherwise
    }

    void writeData(uint2 pixel, float3 color)
    {
        gColor[pixel] = float4(color, 1.0);
        gMotion[pixel] = mvec;
        gDepth[pixel] = linearToNonLinearZ(depth);
        #if USE_VBUFFER
        gVBuffer[pixel] = vbuffer;
        #endif
        #if USE_OPTICAL_FLOW
        gBackupMotion[pixel] = bmvec;
        gPosDiff[pixel] = posDiff;
        gLastRayDir[pixel] = float4(lastRayDir, depth);
        gLocalPathLength[pixel] = localPathLength;
        gIsBackupMotion[pixel] = isBackupMotion ? 1.0 : 0.0;
        #endif
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
struct TriangleData
{
    ShadingData sd;
    // ray differential data:
    StaticVertexData triangleVertices[3];
    float3 unnormalizedN;
    float3 normals[3];
    float2 dBarydx, dBarydy;
}

StackFrame prepareNextRay(StackFrame s, BSDFSample bsdfSample, TriangleData tri)
{
    s.thp *= bsdfSample.weight;
    bool transmission = bsdfSample.isLobe(LobeType::Transmission);

    bool curvedSurface = any(tri.triangleVertices[0].normal != tri.triangleVertices[1].normal) || any(tri.triangleVertices[0].normal != tri.triangleVertices[2].normal);
    if (!transmission && gPreventVbufferCurvedReflection && curvedSurface) // curved reflection
        s.flags |= FLAG_PREVENT_VBUFFER_WRITE;
    if (curvedSurface)
        s.flags |= FLAG_PREVENT_BMVEC_WRITE; // always disable backup motion vector writes for curved surfaces, since those will always be an invalid bacckup (since householder only models planar surfaces)
 
    #ifdef USE_VIEW_TRANSFORM
    float3 H = -s.rayDir + bsdfSample.wo;
    float lenH = length(H);
    if (lenH > 0.001) // lenH = 0 => raydir = wo => simple transmission, no reflection or refraction
        s.viewTransform = mulAffine(s.viewTransform, matrixMirror(tri.sd.posW, H / lenH));
    #endif

    #if USE_TEXTURE_LOD != 0
    // transfer to posW (done in main)
    // s.rd = s.rd.propagate(s.rayOrigin, s.rayDir, distance(s.rayOrigin, sd.posW), sd.faceN);
    // reflect or refract TODO use functions that respect dNdx and dNdy


    float3 ShadingN = normalize(tri.unnormalizedN);
    if (transmission)
    {
        // calculate eta
        float NdD = dot(ShadingN, s.rayDir);
        float NdT = dot(ShadingN, bsdfSample.wo);
        float eta = sqrt(1.0 - NdT * NdT) / sqrt(1.0 - NdD * NdD);
        refractRayDifferential(s.rd, s.rayDir, tri.unnormalizedN, ShadingN, tri.dBarydx, tri.dBarydy, tri.normals, eta, bsdfSample.wo);
    }
    else // reflection
    {
        reflectRayDifferential(s.rd, s.rayDir, tri.unnormalizedN, ShadingN, tri.dBarydx, tri.dBarydy, tri.normals);
    }
    #endif

    s.rayOrigin = tri.sd.computeNewRayOrigin(!transmission); // move origin a bit along normal (note: using sd.posW might result in self-intersection)
    #ifdef USE_VIEW_TRANSFORM
    s.viewTransform = mulAffine(s.viewTransform, matrixTranslate(tri.sd.posW - s.rayOrigin));
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
    int numRefractions = 0; // number of refraction along the primary path (StackEmpty() == true)
    const uint invalidID = uint(-1);
    uint firstRefractionID = invalidID;

    bool camMovement = any(gScene.camera.data.viewMat != gScene.camera.data.prevViewMat);
    if (gForceMotionVectorCalculation) camMovement = true;
    
    VBufferReservoir curVBuffer;
    curVBuffer.weightSum = 0.0;
    curVBuffer.mvec = 0.0;
    curVBuffer.depth = 0.0;
    #if USE_VBUFFER
    curVBuffer.vbuffer = {};
    #endif
    #if USE_OPTICAL_FLOW
    curVBuffer.bmvec = 0.0;
    curVBuffer.lastRayDir = 0.0;
    curVBuffer.posDiff = 0.0;
    curVBuffer.localPathLength = 0;
    curVBuffer.isBackupMotion = false;
    #endif

    #ifdef USE_VIEW_TRANSFORM
    s.viewTransform = matrixIdentity();
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
                if (WRITE_MVEC(s.flags))
                {
                    curVBuffer.mvec = prevUV - (float2(pixel) + 0.5) / float2(frameDim);
                    if (!camMovement)
                        curVBuffer.mvec = 0.0;                   
                }
                    
                curVBuffer.depth = s.linearZ + ray.TMax; // + far plane

                #if USE_VBUFFER
                RayMissHit rmh;
                rmh.posW = ray.Origin + ray.Direction * ray.TMax; // far plane position
                curVBuffer.vbuffer = rmh.pack();
                #endif

                #if USE_OPTICAL_FLOW
                if (WRITE_BMVEC(s.flags)) curVBuffer.bmvec = curVBuffer.mvec;
                curVBuffer.isBackupMotion = WRITE_BMVEC(s.flags);
                curVBuffer.localPathLength = s.curPathLength;
                curVBuffer.lastRayDir = s.rayDir;
                #if USE_TEXTURE_LOD == 0
                curVBuffer.posDiff = 0.0;
                #else
                let posDiffRd = s.rd.propagate(s.rayOrigin, s.rayDir, ray.TMax, s.rayDir);
                curVBuffer.posDiff = max(length(posDiffRd.dOdx), length(posDiffRd.dOdy));
                #endif
                #endif
            }

            if (StackEmpty()) break;

            s = PopStack();
            continue;
        }

        if (hit.getType() != HitType::Triangle)
            return;

        /////////// TRIANGLE HIT /////////////
        TriangleHit triangleHit = hit.getTriangleHit();

        TriangleData tri;
        float3 barycentrics = triangleHit.getBarycentricWeights();
        let v = gScene.getVertexData(triangleHit.instanceID, triangleHit.primitiveIndex, barycentrics, tri.triangleVertices);
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
        float posDiff = max(length(posDiffRd.dOdx), length(posDiffRd.dOdy));
        s.rd = s.rd.propagate(s.rayOrigin, s.rayDir, distance(s.rayOrigin, v.posW), v.faceNormalW);
        // float posDiff = max(length(s.rd.dOdx), length(s.rd.dOdy));
        float2 dUVdx, dUVdy;
        prepareRayDiffAtHitPoint(v, tri.triangleVertices, barycentrics, s.rayDir,
            gScene.getWorldMatrix(triangleHit.instanceID), gScene.getInverseTransposeWorldMatrix(triangleHit.instanceID),
            s.rd, tri.unnormalizedN, tri.normals, tri.dBarydx, tri.dBarydy, dUVdx, dUVdy
        );
        let lod = ExplicitGradientTextureSampler(dUVdx * gTextureGradientScaling, dUVdy * gTextureGradientScaling);
        #endif

        tri.sd = gScene.materials.prepareShadingData(v, materialID, -s.rayDir, lod);
        float opacity = 1.0;
        if (!isAlphaTested(materialID)) opacity = tri.sd.opacity;

        // Create material instance at shading point.
        IMaterialInstance mi = gScene.materials.getMaterialInstance(tri.sd, lod, (uint)MaterialInstanceHints::AdjustShadingNormal);
        let bsdf = mi.getProperties(tri.sd);

        // compute local color contribution
        float3 localContribution = s.thp * opacity * (
            bsdf.emission * gEmissionIntensity +
             (bsdf.diffuseReflectionAlbedo + 0.2 * bsdf.specularReflectionAlbedo) * gAmbientIntensity +
             evalAnalytic(pixel, tri.sd, mi, sg)
        );
        color += localContribution;

        // determine if we have reflections and refractions
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
            tri.sd.mtl.setActiveLobes(activeLobes & (uint)LobeType::Transmission);
            if (!mi.sample(tri.sd, sg, bsdfSampleRefract, true /*Importance sampling*/))
                bsdfSampleRefract.weight = 0.0;
            tri.sd.mtl.setActiveLobes(activeLobes & (uint)LobeType::Reflection);
            if (!mi.sample(tri.sd, sg, bsdfSampleReflect, true /*Importance sampling*/))
                bsdfSampleReflect.weight = 0.0;
            tri.sd.mtl.setActiveLobes( (uint) LobeType::All); // allow all types for brdf eval (otherwise direct light (NEE) will be always black)
        }

        bool doReflection = any(s.thp * bsdfSampleReflect.weight > MIN_VISIBILITY);
        bool doRefraction = any(s.thp * bsdfSampleRefract.weight > MIN_VISIBILITY);

        float pathImportance = dot(s.thp * opacity, 1.0 / 3.0);

        // if (bsdf.roughness <= gRoughnessCutoff && (firstRefractionID == triangleHit.instanceID.index || firstRefractionID == invalidID))
        if (bsdf.roughness <= gRoughnessCutoff && (numRefractions < gMaxVbufferRefractions || !bsdf.isTransmissive))
            pathImportance = 0.0; // dont stop on zero roughness

        if (gPreventVbufferInternalReflection && bsdf.isTransmissive && !doRefraction)
            s.flags |= FLAG_PREVENT_VBUFFER_WRITE; // prevent vbuffer writes after TIR (Total internal reflection)
        //gDebugTex[uint3(pixel, i)] = float4(pathImportance, dot(localContribution, 0.33), s.curPathLength, 1.0);
        // this will also overwrite with pathImportance==0, if nothing else was contributed yet (weightSum = 0.0)
        if (curVBuffer.update(pathImportance, sampleNext1D(sg), s.flags))
        {
            curVBuffer.depth = s.linearZ;

            if (s.curPathLength <= 1 && MVEC == MVEC_FIRST_HIT) // motion vector from initial intersection
            {
                s.flags |= FLAG_PREVENT_MVEC_WRITE; // dont check this in the if condition to always allow overwrites from alpha material on the first hit
                curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, matrixIdentity(), camMovement || vertexMoved);
                #if USE_OPTICAL_FLOW
                curVBuffer.bmvec = curVBuffer.mvec;
                #endif
            }

            #ifdef USE_VIEW_TRANSFORM
            if(WRITE_MVEC(s.flags))
                curVBuffer.mvec = calcMotionVector(pixel, frameDim, v.prevPosW, s.viewTransform, camMovement || vertexMoved);
            #endif

            if (WRITE_MVEC(s.flags) && !camMovement && !vertexMoved) curVBuffer.mvec = 0.0;

            #if USE_VBUFFER
            curVBuffer.vbuffer = data.hit;
            #endif

            #if USE_OPTICAL_FLOW
            curVBuffer.posDiff = posDiff;
            curVBuffer.localPathLength = s.curPathLength;
            curVBuffer.lastRayDir = s.rayDir;
            if (all(curVBuffer.mvec == 0.0) && vertexMoved) curVBuffer.mvec = 1e-37; // workaround to force motion vector calculation in the LK pass (motion vector calculation is disabled if the mvec is 0.0, however, if a vertex on the path moved we should recompute)
            if (WRITE_BMVEC(s.flags)) curVBuffer.bmvec = curVBuffer.mvec;
            curVBuffer.isBackupMotion = WRITE_BMVEC(s.flags);
            #endif
        }

        // prevent mvec writes in the future, if:
        if (s.curPathLength <= 1 && int(BMVEC) == int(MVEC_FIRST_HIT)) s.flags |= FLAG_PREVENT_BMVEC_WRITE;

        // prevent furhter mvec writes, if:
        if (doRefraction && MVEC == MVEC_FIRST_REFRACT) s.flags |= FLAG_PREVENT_MVEC_WRITE;
        if (doRefraction && BMVEC == MVEC_FIRST_REFRACT) s.flags |= FLAG_PREVENT_BMVEC_WRITE;
        if (doRefraction && StackEmpty()) numRefractions++; // increase counter for primary path refractions
        if (doRefraction && firstRefractionID == invalidID) firstRefractionID = triangleHit.instanceID.index; // ID of first refractive surface

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
                PushStack(prepareNextRay(s, bsdfSampleRefract, tri));
            doRefraction = false;
            if (gPreventVbufferRefractiveReflection)
                s.flags |= FLAG_PREVENT_VBUFFER_WRITE; // for the next reflections
        }

        if (doReflection)
            s = prepareNextRay(s, bsdfSampleReflect, tri);
        if (doRefraction)
            s = prepareNextRay(s, bsdfSampleRefract, tri);
        
        if(!doReflection && !doRefraction)
        {
            if (StackEmpty()) break;
            
            s = PopStack();
        }
    }

    // write data
    curVBuffer.writeData(pixel, color);
}
