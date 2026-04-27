
#define SHADOW_TEST_DEFAULT 0
#define SHADOW_TEST_CAUSTIC 1
#define SHADOW_TEST SHADOW_TEST_CAUSTIC

#if SHADOW_TEST == SHADOW_TEST_CAUSTIC
float3 traceShadowRay(uint2 pixel, float3 posW, uint lightIndex)
{
    if (!gEnableShadows) return 1.0;
    LightData light = gScene.getLight(lightIndex);
    RayDesc ray;
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

            if (opacity == 1.0 && gShadowTransmissionMultiplier > 0.0) // check transmission
            {
                // hardcoded for good looking visual
                float IoR = 1.8;
                float3 rayDir = ray.Direction;
                if (rayQuery.CandidateTriangleFrontFace()) IoR = 1.0 / IoR; // flip ior
                else rayDir = refract(rayDir, -v.faceNormalW, 1.0 / IoR); // pretend ray came from previously refracted direction

                // simple fresenl
                float cosTheta = abs(dot(rayDir, v.faceNormalW));
                float F0 = pow(abs((1.0 - IoR) / (1.0 + IoR)), 2.0);
                float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
                float transmissionFactor = 1.0 - fresnel;
                // transmissionFactor = 1.0;
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
