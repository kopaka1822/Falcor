// no transmission
#define SHADOW_TEST_HARD 0
// transmission
#define SHADOW_TEST_TRANSMIT_FRESNEL 1
#define SHADOW_TEST_TRANSMIT_FRESENL2 2
#define SHADOW_TEST_CAUSTIC_POS_TO_LIGHT 3
#define SHADOW_TEST_CAUSTIC_LIGHT_TO_POS 4
#define SHADOW_TEST_FINAL_GATHER 5

#ifndef SHADOW_TEST
#define SHADOW_TEST SHADOW_TEST_HARD
#endif


float getFresnelTransmission(float3 rayDir, float3 normal, float eta)
{
    // simple fresenl
    float cosTheta = abs(dot(rayDir, normal));
    float F0 = pow(abs((1.0 - eta) / (1.0 + eta)), 2.0);
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
    return 1.0 - fresnel;
}

float getMcGuireFresnelTransmission(float3 originalRayDir, float3 normal, float eta)
{
    float s = saturate((1.0 / eta - 1.0) / 2.0);                                              // in [0, 1]
    float g = 2 * saturate(1.0 - pow(abs(dot(normal, originalRayDir)), 128.0 * s * s)) - 1.0; // in [-1, 1]
    // return 1.0 - g * pow(s, 0.2);                                                             // in [0, 2]
    return 1.0 / (1.0 + g * pow(s, 0.2));
}

// P: position on the origin surface
// L: position of the light
// dPdx: change in position on the origin surface
float3 computeDDforLight(float3 P, float3 L, float3 dPdx)
{
    float3 d = L - P; // unnormalized ray direction
    float dd = dot(d, d);
    float3 dddx = -dPdx; // differential of d (L is constant)
    // differential of normalized direction d
    return (dd * dddx - dot(d, dddx) * d) / pow(dd, 1.5);
}

float3 computeDPDirLight(float3 D, float3 dPdx)
{
    // Computes the initial dPdx for a parallel ray going in direction D => Essentiallially orthogonalizes dPdx with respect to D
    return dPdx - dot(D, dPdx) * D;
}

#if SHADOW_TEST == SHADOW_TEST_CAUSTIC_POS_TO_LIGHT


float3 traceShadowRay(uint2 pixel, float3 posW, float3 faceNormalW, uint lightIndex, RayDiff rdIn)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);

    // ray cone setup
    RayDesc ray;
    ray.Origin = posW.xyz; // assume posW already offset with computeNewRayOrigin
    ray.TMin = 0.0;
    float TMax = 0.0;
    RayDiff rd;
    if (light.type == uint(LightType::Point))
    {
        ray.Direction = normalize(light.posW - posW.xyz);
        TMax = distance(light.posW, posW.xyz);
        ray.TMax = TMax - gPointLightClip;

        float3 dDdx = computeDDforLight(posW, light.posW, rdIn.dOdx);
        float3 dDdy = computeDDforLight(posW, light.posW, rdIn.dOdy);
        rd = RayDiff(rdIn.dOdx, rdIn.dOdy, dDdx, dDdy);
    }
    else if (light.type == uint(LightType::Directional))
    {
        ray.Direction = -light.dirW;
        ray.TMax = TMax = gScene.camera.data.farZ;

        float3 dPdx = computeDPDirLight(ray.Direction, rdIn.dOdx);
        float3 dPdy = computeDPDirLight(ray.Direction, rdIn.dOdy);
        rd = RayDiff(dPdx, dPdy, float3(0.f), float3(0.f));
    }

    // calc reference ray diff
     float areaRefDiff = length(cross(rdIn.dOdx, rdIn.dOdy));

    float3 visibility = 1.0;
    float visibilityCutoff = 0.01; // visibility below this threshold is considered to be zero

    // track last value of in/out ray dir
    float3 rayDir = ray.Direction;
    float3 rayDirOut = ray.Direction;

    while (any(visibility > visibilityCutoff))
    {
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;
        const uint instanceMask = getBestParticleShadowPlane(ray.Direction); // not used
        uint rayFlags = RAY_FLAG_NONE;
        //if (gShadowTransmissionMultiplier > 0.0) rayFlags |= RAY_FLAG_FORCE_NON_OPAQUE;
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
                if (opacity > visibilityCutoff)
                    rayQuery.CommitNonOpaqueTriangleHit();
            }
        }

        if (rayQuery.CommittedStatus() == COMMITTED_NOTHING)
            break; // handled all occluders

        // handle current occluder
        float t = rayQuery.CommittedRayT();
        const TriangleHit hit = getCommittedTriangleHit(rayQuery);
        const uint materialID = gScene.getMaterialID(hit.instanceID);
        float3 barycentrics = hit.getBarycentricWeights();
        StaticVertexData triangleVertices[3];
        const VertexData v = gScene.getVertexData(hit.instanceID, hit.primitiveIndex, barycentrics, triangleVertices);

        let material = gScene.materials.getMaterial(materialID);
        let lod = ExplicitLodTextureSampler(gShadowLodBias);
        float opacity = material.evalOpacity(gScene.materials, v, lod);

        if (opacity == 1.0 && gShadowTransmissionMultiplier > 0.0) // check transmission
        {
            // hardcoded for good looking visual
            float eta = gScene.materials.getMaterialHeader(materialID).getIoR();
            float3 normal = v.normalW;
            float virtualT = t - ray.TMin; // distance travelled since last update
            if (rayQuery.CommittedTriangleFrontFace()) // enter medium
            {
                eta = 1.0 / eta; // flip ior
                rayDir = ray.Direction;
                rayDirOut = refract(rayDir, normal, eta); // should always work because eta < 1.0
            }
            else // exit medium
            {
                normal = -normal;
                rayDirOut = ray.Direction;
                rayDir = refract(ray.Direction, normal, 1.0 / eta); // pretend ray came from previously refracted direction
                //virtualT *= dot(normal, rayDirOut) / dot(normal, rayDir); // account for shorter ray length
            }

            float transmissionFactor = getFresnelTransmission(rayDir, normal, eta);
            visibility = visibility * material.evalTransmission(gScene.materials, v.texC, lod) * transmissionFactor * gShadowTransmissionMultiplier;

            // ray diff update
            rd = rd.propagate(rayDir, virtualT, normal);
            //rd = rd.propagate(rayDir, virtualT, -rayDir);
            float2 dUVdx, dUVdy, dBarydx, dBarydy;
            float3 unnormalizedN;
            float3 normals[3];
            prepareRayDiffAtHitPoint(v, triangleVertices, barycentrics, rayDir,
                                     gScene.getWorldMatrix(hit.instanceID), gScene.getInverseTransposeWorldMatrix(hit.instanceID),
                                     rd, unnormalizedN, normals, dBarydx, dBarydy, dUVdx, dUVdy
            );
            // normals[0] = normals[1] = normals[2] = normalize(unnormalizedN);
            float normalLerp = 0.0; 
            normals[0] = lerp(normals[0], normalize(unnormalizedN), normalLerp);
            normals[1] = lerp(normals[1], normalize(unnormalizedN), normalLerp);
            normals[2] = lerp(normals[2], normalize(unnormalizedN), normalLerp);
            float posFac = 1.0; // this affects the size of the caustic
            // rd.dOdx *= posFac; rd.dOdy *= posFac;
            dBarydx *= posFac; dBarydy *= posFac;
            refractRayDifferential(rd, rayDir, unnormalizedN, normalize(unnormalizedN), dBarydx, dBarydy, normals, eta, rayDirOut);
            //rd.dOdx /= posFac; rd.dOdy /= posFac;
            
        }
        else
        {
            visibility = visibility * (1.0 - opacity);
            rd = rd.propagate(ray.Direction, t - ray.TMin, -ray.Direction);
        }

        ray.TMin = t; // update ray T
    }

    float finalT = TMax - ray.TMin;

    // actual ray diff
    rd = rd.propagate(rayDirOut, finalT, -ray.Direction);
    float areaActualDiff = length(cross(rd.dOdx + rdIn.dOdx, rd.dOdy + rdIn.dOdy));
    //float areaActualDiff = length(cross(removeAxis(rd.dOdx - rdIn.dOdx, ray.Direction), removeAxis(rd.dOdy - rdIn.dOdy, ray.Direction)));

    float rayIntensity = areaRefDiff / areaActualDiff;
    //  rayIntensity = 1.0;
    rayIntensity = pow(rayIntensity, 0.25);
    //rayIntensity = min(rayIntensity, 1.0);
    

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility * rayIntensity;
}

#endif

#if SHADOW_TEST == SHADOW_TEST_CAUSTIC_LIGHT_TO_POS
float3 traceShadowRay(uint2 pixel, float3 posW, float3 faceNormalW, uint lightIndex, RayDiff rdIn)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);

    float lastTMin = 0.0; // must start at zero to track proper distance
    RayDesc ray;
    RayDiff rd;

    if (light.type == uint(LightType::Point))
    {
        ray.TMin = gPointLightClip;
        ray.Origin = light.posW;
        ray.Direction = normalize(posW.xyz - light.posW); // light -> pos
        ray.TMax = distance(light.posW, posW.xyz);        // assume posW already offset with computeNewRayOrigin

        float3 dDdx = -computeDDforLight(posW, light.posW, rdIn.dOdx);
        float3 dDdy = -computeDDforLight(posW, light.posW, rdIn.dOdy);
        rd = RayDiff(float3(0.f), float3(0.f), dDdx, dDdy);
    }
    else if (light.type == uint(LightType::Directional))
    {
        ray.TMin = 0.0;
        ray.Origin = posW.xyz - light.dirW * gScene.camera.data.farZ;
        ray.Direction = light.dirW;
        ray.TMax = gScene.camera.data.farZ;

        float3 dPdx = computeDPDirLight(ray.Direction, rdIn.dOdx);
        float3 dPdy = computeDPDirLight(ray.Direction, rdIn.dOdy);
        rd = RayDiff(dPdx, dPdy, float3(0.f), float3(0.f));
    }

    // calc reference ray diff
    // RayDiff refDiff = rd.propagate(ray.Direction, ray.TMax, -ray.Direction);
    // float areaRefDiff = length(cross(refDiff.dOdx, refDiff.dOdy));
    float areaRefDiff = length(cross(rdIn.dOdx, rdIn.dOdy));

    float3 visibility = 1.0;
    float visibilityCutoff = 0.01; // visibility below this threshold is considered to be zero

    // track last value of in/out ray dir
    float3 rayDir = ray.Direction;
    float3 rayDirOut = ray.Direction;

    while (any(visibility > visibilityCutoff))
    {
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rayQuery;
        const uint instanceMask = getBestParticleShadowPlane(ray.Direction); // not used
        uint rayFlags = RAY_FLAG_NONE;
        // if (gShadowTransmissionMultiplier > 0.0) rayFlags |= RAY_FLAG_FORCE_NON_OPAQUE;
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
                if (opacity > visibilityCutoff)
                    rayQuery.CommitNonOpaqueTriangleHit();
            }
        }

        if (rayQuery.CommittedStatus() == COMMITTED_NOTHING)
            break; // handled all occluders

        // handle current occluder
        float t = rayQuery.CommittedRayT();
        const TriangleHit hit = getCommittedTriangleHit(rayQuery);
        const uint materialID = gScene.getMaterialID(hit.instanceID);
        float3 barycentrics = hit.getBarycentricWeights();
        StaticVertexData triangleVertices[3];
        const VertexData v = gScene.getVertexData(hit.instanceID, hit.primitiveIndex, barycentrics, triangleVertices);

        let material = gScene.materials.getMaterial(materialID);
        let lod = ExplicitLodTextureSampler(gShadowLodBias);
        float opacity = material.evalOpacity(gScene.materials, v, lod);

        if (opacity == 1.0 && gShadowTransmissionMultiplier > 0.0) // check transmission
        {
            // hardcoded for good looking visual
            float eta = gScene.materials.getMaterialHeader(materialID).getIoR();
            float3 normal = v.normalW;
            float virtualT = t - lastTMin;          // distance travelled since last update
            if (rayQuery.CommittedTriangleFrontFace()) // enter medium
            {
                eta = 1.0 / eta; // flip ior
                rayDir = ray.Direction;
                rayDirOut = refract(rayDir, normal, eta); // should always work because eta < 1.0
            }
            else // exit medium
            {
                normal = -normal;
                rayDirOut = ray.Direction;
                rayDir = refract(ray.Direction, normal, 1.0 / eta); // pretend ray came from previously refracted direction
                //virtualT *= dot(normal, rayDirOut) / dot(normal, rayDir); // account for shorter ray length
            }

            float transmissionFactor = getFresnelTransmission(rayDir, normal, eta);
            visibility = visibility * material.evalTransmission(gScene.materials, v.texC, lod) * transmissionFactor * gShadowTransmissionMultiplier;

            // ray diff update
            rd = rd.propagate(rayDir, virtualT, normal);
            //rd = rd.propagate(rayDir, virtualT, -rayDir);
            //let rdRef = RayDiff(float3(0.0), float3(0.0), rd.dDdx, rd.dDdy).propagate(ray.Direction, t, -ray.Direction);

            // rd = rd.propagate(rayDir, virtualT, -rayDir);
            float2 dUVdx, dUVdy, dBarydx, dBarydy;
            float3 unnormalizedN;
            float3 normals[3];
            prepareRayDiffAtHitPoint(v, triangleVertices, barycentrics, rayDir,
                                     gScene.getWorldMatrix(hit.instanceID), gScene.getInverseTransposeWorldMatrix(hit.instanceID),
                                     rd, unnormalizedN, normals, dBarydx, dBarydy, dUVdx, dUVdy
            );
            // normals[0] = normals[1] = normals[2] = normalize(unnormalizedN);
            float normalLerp = 0.0;
            normals[0] = lerp(normals[0], normalize(unnormalizedN), normalLerp);
            normals[1] = lerp(normals[1], normalize(unnormalizedN), normalLerp);
            normals[2] = lerp(normals[2], normalize(unnormalizedN), normalLerp);
            // float posFac = 4.0;
            // dBarydx *= posFac; dBarydy *= posFac;
            //dBarydx *= length(rdRef.dOdx) / length(rd.dOdx);
            //dBarydy *= length(rdRef.dOdy) / length(rd.dOdy);
            refractRayDifferential(rd, rayDir, unnormalizedN, normalize(unnormalizedN), dBarydx, dBarydy, normals, eta, rayDirOut);
        }
        else
        {
            visibility = visibility * (1.0 - opacity);
            rd = rd.propagate(rayDirOut, t - lastTMin, -rayDirOut);
        }

        ray.TMin = t; // update ray T
        lastTMin = t;
    }

    // actual ray diff
    rd = rd.propagate(rayDirOut, ray.TMax - lastTMin, faceNormalW);
    float areaActualDiff = length(cross(rd.dOdx, rd.dOdy));

    // float rayIntensity = areaRef / areaActual;
    float rayIntensity = areaRefDiff / areaActualDiff;
    // rayIntensity = 1.0;
    rayIntensity = pow(rayIntensity, 0.25);

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility * rayIntensity;
}
#endif

#if SHADOW_TEST == SHADOW_TEST_TRANSMIT_FRESNEL || SHADOW_TEST == SHADOW_TEST_TRANSMIT_FRESENL2
// deep shadow ray
float3 traceShadowRay(uint2 pixel, float3 posW, float3 faceNormalW, uint lightIndex, RayDiff rdIn)
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
                float eta = gScene.materials.getMaterialHeader(materialID).getIoR();
                float3 rayDir = -ray.Direction;
                if (rayQuery.CandidateTriangleFrontFace()) eta = 1.0 / eta; // flip ior
                else rayDir = refract(rayDir, -v.normalW, 1.0 / eta); // pretend ray came from previously refracted direction

                float transmissionFactor = 1.0;
                if (SHADOW_TEST == SHADOW_TEST_TRANSMIT_FRESNEL) transmissionFactor = getFresnelTransmission(rayDir, v.normalW, eta);
                if (SHADOW_TEST == SHADOW_TEST_TRANSMIT_FRESENL2) transmissionFactor = getMcGuireFresnelTransmission(ray.Direction, v.normalW, eta);
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

#endif

#if SHADOW_TEST == SHADOW_TEST_HARD
// deep shadow ray
float3 traceShadowRay(uint2 pixel, float3 posW, float3 faceNormalW, uint lightIndex, RayDiff rdIn)
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

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
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

#endif

#if SHADOW_TEST == SHADOW_TEST_FINAL_GATHER
// hard shadow test
float3 traceShadowRay(uint2 pixel, float3 posW, float3 faceNormalW, uint lightIndex, RayDiff rdIn)
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

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
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

bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    return dot(radiusTest, radiusTest) < radius * radius;
}

// photon radiance estimate for final gather
float3 photonRadianceEstimate(ShadingData sd, IMaterialInstance mi, float3 N, inout UniformSampleGenerator sg)
{
    float3 outColor = float3(0);

    // Prepare ray
    RayDesc ray;
    ray.Origin = sd.posW;
    ray.Direction = sd.faceN;
    ray.TMin = 0;
    ray.TMax = 1e-10; // Small constant

    RayQuery<RAY_FLAG_NONE> rayQuery;
    rayQuery.TraceRayInline(gPhotonAS, RAY_FLAG_NONE, 0xFF, ray);
    while (rayQuery.Proceed()) {

        if (rayQuery.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
            continue;
        const uint primIdx = rayQuery.CandidatePrimitiveIndex();

        // Check if ray is inside photon radius
        AABB photonAABB = gCausticAABB[primIdx];
        float radius = (photonAABB.maxPoint.x - photonAABB.minPoint.x) / 2.0;
        if (!hitSphere(photonAABB.center(), radius, sd.posW))
            continue;

        CausticData cd = gCausticData[primIdx];
        float NdotL = dot(-cd.wi, N);

        float3 bsdf = mi.eval(sd, -cd.wi, sg);
        float3 photonRadiance = max(0, (cd.flux * bsdf) / (M_PI * radius * radius * NdotL)); // NdotL needs to be taken out of the bsdf for Photon Mapping
        outColor += photonRadiance;
    }

    return outColor;
}

#endif
