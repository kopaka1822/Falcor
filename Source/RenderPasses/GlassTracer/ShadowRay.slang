
#define SHADOW_TEST_DEFAULT 0
#define SHADOW_TEST_CAUSTIC 1
#define SHADOW_TEST SHADOW_TEST_CAUSTIC

#if SHADOW_TEST == SHADOW_TEST_CAUSTIC
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

float3 traceShadowRay(uint2 pixel, float3 posW, uint lightIndex, RayDiff rdIn)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);

    // ray cone setup
    RayDesc ray;
    float RAY_CONE_START_ANGLE = 0.01; // small non-zero start angle
    RayCone rayCone = RayCone(0.0, RAY_CONE_START_ANGLE);
    float rayConeTMin = 0.0; // must start at zero to track proper distance

    if (light.type == uint(LightType::Point))
    {
        ray.TMin = gPointLightClip;
        ray.Origin = light.posW;
        ray.Direction = normalize(posW.xyz - light.posW); // light -> pos
        ray.TMax = distance(light.posW, posW.xyz); // assume posW already offset with computeNewRayOrigin
    }
    else if (light.type == uint(LightType::Directional))
    {
        ray.TMin = 0.0;
        ray.Origin = posW.xyz - light.dirW * gScene.camera.data.farZ;
        ray.Direction = light.dirW;
        ray.TMax = gScene.camera.data.farZ;
    }

    // ray differential setup
    float3 dDdx, dDdy;
    // ray tangent and bitangent
    // dDdx = normalize(cross(ray.Direction, float3(0.0, 1.0, 0.0)));
    // dDdy = normalize(cross(ray.Direction, dDdx));
    dDdx = -computeDDforLight(posW, light.posW, rdIn.dOdx);
    dDdy = -computeDDforLight(posW, light.posW, rdIn.dOdy);
    
    RayDiff rd = RayDiff(float3(0.f), float3(0.f), dDdx, dDdy);

    if (light.type == uint(LightType::Directional))
    {
        rd = RayDiff(dDdx, dDdy, float3(0.f), float3(0.f));
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
            if (rayQuery.CommittedTriangleFrontFace())
            {
                eta = 1.0 / eta; // flip ior
                rayDir = ray.Direction;
                rayDirOut = refract(rayDir, normal, eta); // should always work because eta < 1.0
            }
            else
            {
                normal = -normal;
                rayDirOut = ray.Direction;
                rayDir = refract(ray.Direction, normal, 1.0 / eta); // pretend ray came from previously refracted direction
            }
            // simple fresenl
            float cosTheta = abs(dot(rayDir, normal));
            float F0 = pow(abs((1.0 - eta) / (1.0 + eta)), 2.0);
            float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
            float transmissionFactor = 1.0 - fresnel;
            // transmissionFactor = 1.0; // TODO comment out and check fresnel
            visibility = visibility * material.evalTransmission(gScene.materials, v.texC, lod) * transmissionFactor * gShadowTransmissionMultiplier;

            // ray cone update
            rayCone = rayCone.propagateDistance(t - rayConeTMin); // travelled distance
            refractRayCone(rayCone, v.posW - rayDir * t, rayDir, v.posW, normal, 0.0, eta, rayDirOut);

            // ray diff update
            rd = rd.propagate(rayDir, t - rayConeTMin, normal);
            //rd = rd.propagate(rayDir, t - rayConeTMin, -rayDir);
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
            refractRayDifferential(rd, rayDir, unnormalizedN, normalize(unnormalizedN), dBarydx, dBarydy, normals, eta, rayDirOut);
        }
        else
        {
            visibility = visibility * (1.0 - opacity);
            rayCone = rayCone.propagateDistance(t - rayConeTMin); // travelled distance
            rd = rd.propagate(ray.Direction, t - rayConeTMin, -ray.Direction);
        }

        ray.TMin = t; // update ray T
        rayConeTMin = t;
    }

    // actual ray cone
    rayCone = rayCone.propagateDistance(ray.TMax - rayConeTMin); // propagate remaining distance
    float areaActual = rayCone.getWidth() * rayCone.getWidth();

    // reference ray cone
    RayCone refCone = RayCone(0.0, RAY_CONE_START_ANGLE);
    refCone = refCone.propagateDistance(ray.TMax); // propagate to final destination
    float areaRef = refCone.getWidth() * refCone.getWidth();

    // actual ray diff
    rd = rd.propagate(rayDirOut, ray.TMax - rayConeTMin, -ray.Direction);
    float areaActualDiff = length(cross(rd.dOdx, rd.dOdy));

    //float rayIntensity = areaRef / areaActual;
    float rayIntensity = areaRefDiff / areaActualDiff;
    // rayIntensity = 1.0;
    rayIntensity = pow(rayIntensity, 0.5);

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility * rayIntensity;
}
#endif

#if SHADOW_TEST == SHADOW_TEST_DEFAULT
// deep shadow ray
float3 traceShadowRay(uint2 pixel, float3 posW, uint lightIndex, RayDiff rdIn)
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

#endif
