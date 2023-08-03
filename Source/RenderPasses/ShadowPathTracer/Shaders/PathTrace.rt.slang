#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;



cbuffer CB
{
    uint gFrameCount;
    float gShadowMapFarPlane;
    float gSMworldAcneBias;
    uint _pad;
};

// Inputs
SamplerState gShadowSampler;
TextureCube<float> gShadowMap[NUM_SHADOW_MAPS_POINT];
Texture2D<PackedHitInfo> gVBuffer;

// Outputs
RWTexture2D<float4> gColor;

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


static const float3 smSampleOffsetDirections[20] =
{
   float3(1, 1, 1),  float3(1, -1, 1),  float3(-1, -1, 1),  float3(-1, 1, 1),
   float3(1, 1, -1), float3(1, -1, -1), float3(-1, -1, -1), float3(-1, 1, -1),
   float3(1, 1, 0),  float3(1, -1, 0),  float3(-1, -1, 0),  float3(-1, 1, 0),
   float3(1, 0, 1),  float3(-1, 0, 1),  float3(1, 0, -1),   float3(-1, 0, -1),
   float3(0, 1, 1),  float3(0, -1, 1),  float3(0, -1, -1),  float3(0, 1, -1)
};

float evalShadowMap(uint lightIdx, float3 posW, const in LightData lightData)
{
    float shadow = 0.f;
    float3 lightToPoint = posW - lightData.posW;
    float lightDist = length(lightToPoint);
    lightToPoint.y *= -1;
    //lightToPoint /= lightDist;
    //lightDist -= gSMworldAcneBias; //Shadow acne
    //Shadow Test with shadow map
    
    float shadowDist = gShadowMap[lightIdx].SampleLevel(gShadowSampler, lightToPoint, 0.f);
    shadowDist *= gShadowMapFarPlane; //TODO Set per uniform (Hardcoded farplane for shadow map)
    if (shadowDist + gSMworldAcneBias > lightDist)
        shadow += 1.f;
    
    //PCF
    /*
    int samples = 20;
    //float diskRadius = 0.05;
    float diskRadius = (1.0 + (lightDist / gShadowMapFarPlane)) / 25.0;
    for (int i = 0; i < samples; ++i)
    {
        float shadowDist = gShadowMap[lightIdx].SampleLevel(gShadowSampler, lightToPoint + smSampleOffsetDirections[i] * diskRadius, 0.f);
        shadowDist *= gShadowMapFarPlane; // undo mapping [0;1]
        if (shadowDist + gSMworldAcneBias > lightDist)
            shadow += 1.0;
    }
    shadow /= float(samples);
    */

    
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
    //const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

    // Test visibility by tracing a shadow ray.
    float shadow = evalShadowMap(lightIndex, sd.posW, lightData);
    //bool V = traceShadowRay(origin, ls.dir, ls.distance);
    //if (!V)
        //return float3(0.f);
    
    // Evaluate contribution.
    return mi.eval(sd, ls.dir, sg) * ls.Li * invPdf * shadow;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
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
        outColor += gShadowMap[0].SampleLevel(gShadowSampler, view, 0.f);
    }
       
    gColor[launchIndex] = float4(outColor, 1.f);
}
