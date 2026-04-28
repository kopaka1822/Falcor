
#define SHADOW_TEST_DEFAULT 0
#define SHADOW_TEST_CAUSTIC 1
#define SHADOW_TEST SHADOW_TEST_CAUSTIC

#if SHADOW_TEST == SHADOW_TEST_CAUSTIC
float3 traceShadowRay(uint2 pixel, float3 posW, uint lightIndex)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);
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
        ray.Origin = posW.xyz + light.dirW * gScene.camera.data.farZ;
        ray.Direction = light.dirW;
        ray.TMax = gScene.camera.data.farZ;
    }

    float3 visibility = 1.0;
    float visibilityCutoff = 0.01; // visibility below this threshold is considered to be zero

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
        const VertexData v = gScene.getVertexData(hit);

        let material = gScene.materials.getMaterial(materialID);
        let lod = ExplicitLodTextureSampler(gShadowLodBias);
        float opacity = material.evalOpacity(gScene.materials, v, lod);

        if (opacity == 1.0 && gShadowTransmissionMultiplier > 0.0) // check transmission
        {
            // hardcoded for good looking visual
            float eta = gScene.materials.getMaterialHeader(materialID).getIoR();
            float3 rayDir = ray.Direction;
            float3 normal = v.normalW;
            if (rayQuery.CommittedTriangleFrontFace())
            {
                eta = 1.0 / eta; // flip ior
            }
            else
            {
                normal = -normal;
                rayDir = refract(rayDir, -v.faceNormalW, 1.0 / eta); // pretend ray came from previously refracted direction
            }
            // simple fresenl
            float cosTheta = abs(dot(rayDir, v.faceNormalW));
            float F0 = pow(abs((1.0 - eta) / (1.0 + eta)), 2.0);
            float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
            float transmissionFactor = 1.0 - fresnel;
            // transmissionFactor = 1.0;
            visibility = visibility * material.evalTransmission(gScene.materials, v.texC, lod) * transmissionFactor * gShadowTransmissionMultiplier;

            // ray cone update
            rayCone = rayCone.propagateDistance(t - rayConeTMin); // travelled distance
            float3 outDir = 0.0;
            refractRayCone(rayCone, ray.Origin, rayDir, v.posW, normal, 0.0, eta, outDir);
        }
        else
        {
            visibility = visibility * (1.0 - opacity);
            rayCone = rayCone.propagateDistance(t - rayConeTMin); // travelled distance
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

    float rayIntensity = areaRef / areaActual;
    //rayIntensity = 1.0;

    visibility = saturate((visibility - visibilityCutoff) / (1.0 - visibilityCutoff));
    return visibility * rayIntensity;
}
#endif

#if SHADOW_TEST == SHADOW_TEST_DEFAULT
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

#endif
