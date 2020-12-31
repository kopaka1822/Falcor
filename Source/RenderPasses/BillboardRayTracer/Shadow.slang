// use ray query to trace approximate shadow ray
float TraceShadowRay(float3 origin, LightData l)
{
    if(!USE_SHADOWS) return 1.0;

    // set up shadow ray
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.01;
    ray.TMax = FLT_MAX;
    switch(l.type)
    {
    case uint(LightType::Point): {
        float3 dir = l.posW - origin;
        ray.TMax = length(dir);
        ray.Direction = dir / ray.TMax;
        } break;
    case uint(LightType::Directional):
        ray.Direction = -l.dirW;
        break;
    default: return 1.0; // not supported
    }

    // prepare ray query (do all computations in the "any-hit shader")
#if BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR
    RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_NON_OPAQUE | /* no shadows for impostors */ RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
#else
    RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_NON_OPAQUE> q;
#endif
    q.TraceRayInline(gRtScene, RAY_FLAG_NONE, 0xff /* instanceInclusionMask */, ray);
    float transmit = 1.0;

    while(q.Proceed())
    {
        if(q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) // triangle
        {
           float2 rayBary = q.CandidateTriangleBarycentrics();
           float3 barycentrics = float3(1.0 - rayBary.x - rayBary.y, rayBary.x, rayBary.y);
           VertexData v = gScene.getVertexData(q.CandidateInstanceID() + q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(), barycentrics);
           const uint materialID = gScene.getMaterialID(q.CandidateInstanceID() + q.CandidateGeometryIndex());
           ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -ray.Direction, 0.f);
           transmit *= 1.0 - sd.opacity;
        }
        else // billboard
        {
            // do constant colored shadows in spherical billboard style
            const uint aabbOffset = gScene.proceduralPrimitives[q.CandidateGeometryIndex()].AABBOffset;
            const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
            const float2 dim = bbox.extent().xy;
            const float3 P = bbox.center();

            const float r = 0.5 * dim.x;
            const float rayT = dot(ray.Direction, P - ray.Origin);
            const float d = length(ray.Origin + rayT * ray.Direction - P);
            if(d >= r) continue; // no hit
            const float w = sqrt(r * r - d * d);
            const float rayTIn = max(ray.TMin, rayT - w);
            const float ds = min(ray.TMax, rayT + w) - rayTIn;
            if(ds <= 0.0) continue;
            
            transmit *= pow(0.3, (1 - (d / r)) * (0.5 * ds / r));
        }

        if(transmit < TRANSMIT_EPSILON)
        {
            q.Abort();
            break;
        }
    }

    return transmit;
}
