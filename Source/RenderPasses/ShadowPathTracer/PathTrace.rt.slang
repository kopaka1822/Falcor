#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Color.ColorHelpers;

import Rendering.ShadowMaps.ShadowMap;

cbuffer CB
{
    uint gFrameCount;
    uint gMaxBounces;
    uint gUseShadowMap;
    uint _pad;
};

static const bool kUseEmissve = USE_EMISSIVE_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const float kRayTMax = RAY_TMAX; 
static const bool kEvalAllAnalytic = EVALUATE_ALL_ANALYTIC_LIGHTS;
static const bool kAdjustShadingNormals = ADJUST_SHADING_NORMALS;
static const bool kAlphaTest = ALPHA_TEST;
static const bool kUseHybridShadowMap = USE_HYBRID_SM;

Texture2D<PackedHitInfo> gVBuffer;

// Outputs
RWTexture2D<float4> gColor;

//ShadowMap
ParameterBlock<ShadowMap> gShadowMap;

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    bool visible;
};

/** Payload for ray (48B).
*/
struct RayData
{
    float3 radiance; ///< Accumulated outgoing radiance from path.
    bool terminated; ///< Set to true when path is terminated.
    float3 thp; ///< Current path throughput. This is updated at each path vertex.
    uint pathLength; ///< Path length in number of path segments (0 at origin, 1 at first secondary hit, etc.). Max 2^31.
    float3 origin; ///< Next path segment origin.
    float3 direction; ///< Next path segment direction.

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.terminated = false;
        this.pathLength = 0;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
    }
};

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

float traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if ray is not terminated before
    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, ray, rayData);

    return rayData.visible ? 1.f : 0.f;
}

void traceNextRay(inout RayData rayData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    uint rayFlags = 0; // TODO: Set cull mode from the app
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}

/** Evaluates the direct illumination from analytic lights.
    This function samples Falcor's light list uniformly with one shadow ray.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in,out] sg SampleGenerator object.
    \return Outgoing radiance in view direction.
*/
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg, uint pathLength = 0)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return float3(0);
    
    const uint sampleCount = kEvalAllAnalytic ? lightCount : 1u;

    float3 Li = float3(0);
    
    for (uint i = 0; i < sampleCount; i++)
    {
        // Pick one of the analytic light sources randomly with equal probability.
        const uint lightIndex = kEvalAllAnalytic ? i : min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
        float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

        LightData lightData = gScene.getLight(lightIndex);
        // Sample local light source.
        AnalyticLightSample ls;
        if (!sampleLight(sd.posW, lightData, sg, ls))
            continue;

        // Reject sample if not in the hemisphere of a BSDF lobe.
        const uint lobeTypes = mi.getLobeTypes(sd);
        const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
        const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
        float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
        if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
            continue;

        // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
        const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

        // Test visibility by tracing a shadow ray.
        float shadow;
        if (kUseHybridShadowMap && gUseShadowMap <= pathLength)
        {
            if (!gShadowMap.hybridShadowMap(lightIndex, origin, lightData, sg, shadow))
                shadow = traceShadowRay(origin, ls.dir, ls.distance);
        }
        else
        {
            if (gUseShadowMap <= pathLength)
                shadow = gShadowMap.evalShadowMap(lightIndex, origin, lightData, sg);
            else
                shadow = traceShadowRay(origin, ls.dir, ls.distance);
        }
        
        
        
        
        // Evaluate contribution.
        if (kEvalAllAnalytic)
            Li += mi.eval(sd, ls.dir, sg) * ls.Li * shadow;
        else
            Li += mi.eval(sd, ls.dir, sg) * ls.Li * invPdf * shadow;
    }
    
    return Li;
}

/** Generate a new scatter ray or terminate.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in] rayOrigin Ray origin for the new ray.
    \param[in,out] rayData Ray payload.
    \return True if the path continues.
*/
bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, float3 rayOrigin, inout RayData rayData)
{
    // Sample material.
    BSDFSample bsdfSample;
    if (mi.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        rayData.origin = rayOrigin;
        if (bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeNewRayOrigin(false);
        }
        rayData.direction = bsdfSample.wo;
        rayData.thp *= bsdfSample.weight;
        return any(rayData.thp > 0.f);
    }

    return false;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Get EnvMap (or background color)
    float3 backgroundColor = kUseEnvBackground ? gScene.envMap.eval(rayData.direction) : kDefaultBackgroundColor;
    rayData.radiance += rayData.thp * backgroundColor;
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        const uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
     
}

[shader("anyhit")]
void shadowAnyHit(inout ShadowRayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        const uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}


[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    //Get Hit info
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    const HitInfo hit = HitInfo(triangleHit);

    //Init Matrialsystem for hit
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = loadShadingData(hit, rayData.direction, lod);
    uint hints = kAdjustShadingNormals ? (uint) MaterialInstanceHints::AdjustShadingNormal : 0;
    let mi = gScene.materials.getMaterialInstance(sd, lod, hints);

    //TODO add emissive?
    if (kUseEmissve)
    {
        rayData.radiance += rayData.thp * mi.getProperties(sd).emission;
    }
    
    // Check whether to terminate based on max depth.
    if (rayData.pathLength >= gMaxBounces)
    {
        rayData.terminated = true;
        return;
    }
    rayData.pathLength++;

    //Add light contribution
    float3 Lr = evalDirectAnalytic(sd, mi, rayData.sg, rayData.pathLength);
    rayData.radiance += rayData.thp * Lr;

    //Generate next hit   
    float3 rayOrigin = sd.computeNewRayOrigin();
    if (!generateScatterRay(sd, mi, rayOrigin, rayData))
        rayData.terminated = true;
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float3 outColor = float3(0.f);
    
    let lod = ExplicitLodTextureSampler(0.f);
    HitInfo hitInfo = HitInfo(gVBuffer[launchIndex]);

    float3 view = gScene.camera.computeRayPinhole(launchIndex, launchDim).dir;
    
    if (hitInfo.isValid())
    {
        // Create sample generator.
        RayData rayData;
        rayData.sg = SampleGenerator(launchIndex, gFrameCount);

        //Material and shading information
        ShadingData sd = loadShadingData(hitInfo, view, lod);
        // Create material instance at shading point.
        uint hints = kAdjustShadingNormals ? (uint) MaterialInstanceHints::AdjustShadingNormal : 0;
        let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
        
        //Emissive Light
        outColor += mi.getProperties(sd).emission;

        //First hit direct light
        outColor += evalDirectAnalytic(sd, mi, rayData.sg);

        //Prepare for GI
        float3 rayOrigin = sd.computeNewRayOrigin();
        
        if (!generateScatterRay(sd, mi, rayOrigin, rayData))
            rayData.terminated = true;

        // Follow path into the scene and compute its total contribution.
        for (uint depth = 0; depth <= gMaxBounces && !rayData.terminated; depth++)
        {
            // Trace scatter ray. If it hits geometry, the closest hit shader samples
            // direct illumination and generates the next scatter ray.
            traceNextRay(rayData);
        }

        //Add GI
        outColor += rayData.radiance;
    }
    else
    {
        outColor = kUseEnvBackground ? gScene.envMap.eval(view) : kDefaultBackgroundColor;
    }
       
    gColor[launchIndex] = float4(outColor, 1.f);
}
