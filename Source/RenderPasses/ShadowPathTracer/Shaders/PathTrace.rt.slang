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

cbuffer CB
{
    uint gFrameCount;
    bool gUseRayTracedShadows;
    float gShadowMapFarPlane;
    float gSMworldAcneBias;
    
    float gPCFdiskRadius;
    bool gUsePCF;   
};

static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);

// Inputs
SamplerState gShadowSampler;
TextureCube<float> gShadowMap[NUM_SHADOW_MAPS_POINT];
Texture2D<PackedHitInfo> gVBuffer;

// Outputs
RWTexture2D<float4> gColor;


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
    uint pad; ///< free space.
    PackedHitInfo packedHitInfo; ///< Hit info from vBuffer; Up to 16B

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.radiance = float3(0);
        this.pad = 0;
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

float evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData)
{
    float shadow = 0.f;
    float3 lightToPoint = posW - lightData.posW;
    float lightDist = length(lightToPoint);
    lightToPoint.y *= -1;
    //Shadow Test with shadow map
    if (!gUsePCF)
    {
        float shadowDist = gShadowMap[lightIdx].SampleLevel(gShadowSampler, lightToPoint, 0.f);
        shadowDist *= gShadowMapFarPlane; //TODO Set per uniform (Hardcoded farplane for shadow map)
        if (shadowDist + gSMworldAcneBias > lightDist)
            shadow += 1.f;
    }
    else
    {
        //Proper PCF        
        float samples = 2.0;
        float offset = gPCFdiskRadius;
        
        for (float x = -offset; x < offset; x += offset / (samples * 0.5))
        {
            for (float y = -offset; y < offset; y += offset / (samples * 0.5))
            {
                for (float z = -offset; z < offset; z += offset / (samples * 0.5))
                {
                    float shadowDist = gShadowMap[lightIdx].SampleLevel(gShadowSampler, lightToPoint + float3(x,y,z), 0.f);
                    shadowDist *= gShadowMapFarPlane; // undo mapping [0;1]
                    if (lightDist - gSMworldAcneBias < shadowDist)
                        shadow += 1.0;
                }
            }
        }
        shadow /= (samples * samples * samples);
    }
    
    return shadow;
}

/** Evaluates the direct illumination from analytic lights.
    This function samples Falcor's light list uniformly with one shadow ray.
    \param[in] sd Shading data.
    \param[in] mi Material instance.
    \param[in,out] sg SampleGenerator object.
    \return Outgoing radiance in view direction.
*/
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return float3(0.f);

    // Pick one of the analytic light sources randomly with equal probability.
    const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    LightData lightData = gScene.getLight(lightIndex);
    // Sample local light source.
    AnalyticLightSample ls;
    if (!sampleLight(sd.posW, lightData, sg, ls))
        return float3(0.f);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
        return float3(0.f);

    // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
    const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

    // Test visibility by tracing a shadow ray.
    float shadow = 1.f;

    if (gUseRayTracedShadows)
        shadow = traceShadowRay(origin, ls.dir, ls.distance);
    else
        shadow = evalShadowMap(lightIndex, origin, lightData);
    
    
    // Evaluate contribution.
    return mi.eval(sd, ls.dir, sg) * ls.Li * invPdf * shadow;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}


[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    //Empty. Is needed for compilation
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

    RayData rayData;
    
    if (hitInfo.isValid())
    {
        ShadingData sd = loadShadingData(hitInfo, view, lod);
        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod);
        // Create sample generator.
        rayData.sg = SampleGenerator(launchIndex, gFrameCount);

        outColor += evalDirectAnalytic(sd, mi, rayData.sg);
    }
    else
    {
        outColor = kUseEnvBackground ? gScene.envMap.eval(view) : kDefaultBackgroundColor;
    }
       
    gColor[launchIndex] = float4(outColor, 1.f);
}
