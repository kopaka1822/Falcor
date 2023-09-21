#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Rendering.Lights.LightHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;
import Rendering.ShadowMaps.ShadowMap;


cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    bool gAlphaTest;
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<float> gRayDist;

//Outputs
RWTexture2D<float4> gDirectLight;

static const bool kUseShadowMap = GI_USE_SHADOW_MAP;

struct ShadowTraceData
{
    bool visible;
};

struct LightSample
{
    float3 Li; ///< Incident radiance at the shading point (unshadowed). This is already divided by the pdf.
    float pdf; ///< Pdf with respect to solid angle at the shading point.
    float3 origin; ///< Ray origin for visibility evaluation (offseted to avoid self-intersection).
    float distance; ///< Ray distance for visibility evaluation (shortened to avoid self-intersection).
    float3 dir; ///< Ray direction for visibility evaluation (normalized).
    uint lightType = 0; ///< smLightType
        
    RayDesc getVisibilityRay(float3 posW)
    {
        return {posW, 0.f, dir, distance};
    }
};

SMLightData lightSampleToLightData(const in LightSample ls){
    SMLightData lightData;
    lightData.posW = ls.origin;
    lightData.dirW = ls.dir;
    lightData.type = ls.lightType;
    return lightData;
}

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

    /** Generates a light sample on the analytic lights.
        \param[in] shading data.
        \param[in,out] sg Sample generator.
        \param[out] ls Struct describing valid samples.
        \return True if the sample is valid and has nonzero contribution, false otherwise.
    */
bool evalAnalyticLight(const in ShadingData sd, inout SampleGenerator sg, const uint lightIndex, out LightSample ls)
{
    ls = { }; // Default initialization to avoid divergence at returns.
    
    // Sample local light source.
    AnalyticLightSample lightSample;
    LightData lightData = gScene.getLight(lightIndex);
    if (!sampleLight(sd.posW,lightData , sg, lightSample))
        return false;

    ls.lightType = getShadowMapLightType(lightData);
    // Setup returned sample.
    ls.pdf = lightSample.pdf;
    ls.Li = lightSample.Li;
        // Offset shading position to avoid self-intersection.
    ls.origin = lightData.posW;
    
    // Analytic lights do not currently have a geometric representation in the scene.
    // Do not worry about adjusting the ray length to avoid self-intersections at the light.
    ls.distance = lightSample.distance;
    ls.dir = lightSample.dir;

    return any(ls.Li > 0.f);
}

[shader("miss")]
void miss(inout ShadowTraceData rayData : SV_RayPayload)
{
    rayData.visible = true;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout ShadowTraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (gAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}


[shader("closesthit")]
void closestHit(inout ShadowTraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
}


[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    HitInfo hitInfo = HitInfo(gVBuffer[launchIndex]);
    float3 view = -gView[launchIndex].xyz;
    const float rayDist = gRayDist[launchIndex];
   
    ShadowTraceData traceData;
    traceData.visible = false;
    SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount + 7 * 13);
    let lod = ExplicitLodTextureSampler(0.f);
    
    ShadingData sd = loadShadingData(hitInfo, view, lod);
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
    let miProperties = mi.getProperties(sd);

    float3 directLightOut = float3(0);
    for (uint i = 0; i < gScene.getLightCount(); i++)
    {
        LightSample ls = { };
        if(evalAnalyticLight(sd, sg, i, ls))
        {
            float3 Lr = mi.eval(sd, ls.dir, sg) * ls.Li; 
            //Shadow test
            if (any(Lr > 0))
            {
                bool doShadowTest = true;
                float shadow = 0.f;
                if(kUseShadowMap){
                    //Fill the shadow map light data struct
                    SMLightData lightData = lightSampleToLightData(ls);
                    float oracle = 0.f;
                    float oracleMip = 0.f;
                    bool useNormalSM = !gShadowMap.kUseHybridShadowMap;
                    if (gShadowMap.kUseOracle){
                        oracle = gShadowMap.oracleFunction(i, lightData, sd, rayDist, oracleMip);
                        useNormalSM = oracle <= gShadowMap.kOracleCompValue;
                    }

                    if (gShadowMap.kUseHybridShadowMap)
                        doShadowTest = !gShadowMap.evalHybridShadowMap(i, sd.posW, lightData, sg, oracleMip, shadow) && !useNormalSM;
                    else if (useNormalSM)
                        doShadowTest = !gShadowMap.evalShadowMap(i, sd.posW, lightData, sg, oracleMip, shadow);
                }
                if(doShadowTest){
                    shadow = 0.f;
                    ShadowTraceData shadowData;
                    shadowData.visible = false;
                    RayDesc shadowRay = ls.getVisibilityRay(sd.computeNewRayOrigin());
                          
                    TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, shadowRay, shadowData);
                    if(shadowData.visible)
                        shadow = 1.f;
                }
                                     
                directLightOut += Lr * shadow;
            }        
        }
    }
    //Write
    gDirectLight[launchIndex] = float4(directLightOut,1);
}
