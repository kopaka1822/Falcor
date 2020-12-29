struct ShadowPayload
{
    float transmit;
};

float TraceShadowRay(float3 origin, LightData l)
{
    if(!USE_SHADOWS) return 1.0;
    
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

    ShadowPayload rayData;    // Set to true by miss shader if ray is not terminated before
    rayData.transmit = 1.0;
    TraceRay(gRtScene, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xff /* instanceInclusionMask */, 1 /* hitIdx */, hitProgramCount, 1 /* missIdx */, ray, rayData);

    return rayData.transmit;
}

[shader("miss")]
void missShadow(inout ShadowPayload rayData : SV_RayPayload)
{
    // empty
}

[shader("anyhit")]
void triangleAnyHitShadow(
    uniform HitShaderParams hitParams,
    inout ShadowPayload rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -WorldRayDirection(), 0.f);

    float transmit =  sd.opacity * sd.specularTransmission + (1.0 - sd.opacity);

    rayData.transmit *= transmit;
    if(rayData.transmit < TRANSMIT_EPSILON)
    {
        rayData.transmit = 0.0;
        AcceptHitAndEndSearch();
    }
    IgnoreHit(); // accumulate transmittance (order independent)
}

[shader("closesthit")]
void triangleClosestHitShadow(
    uniform HitShaderParams hitParams,
    inout ShadowPayload rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // skipped
}

/***************** Billboards *****************/

[shader("anyhit")]
void boxAnyHitShadow(
    uniform HitShaderParams hitParams,
    inout ShadowPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // TODO
}

[shader("closesthit")]
void boxClosestHitShadow(
    uniform HitShaderParams hitParams,
    inout ShadowPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // skipped
}
