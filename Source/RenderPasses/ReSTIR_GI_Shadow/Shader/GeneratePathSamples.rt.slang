#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Materials.Microfacet;
import Rendering.Materials.Fresnel;
import Rendering.Lights.LightHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;
import Rendering.ShadowMaps.ShadowMap;

import Reservoir;
import PathData;
import SurfaceDataGI;


cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    float gAttenuationRadius;
    uint gBounces;
    uint _pad;
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D <float>gLinZ;

//Outputs
RWStructuredBuffer<PackedSurfaceDataGI> gSurfaceData;
RWStructuredBuffer<GIData> gGISample;

//Outputs
#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif

uniform EmissiveLightSampler gEmissiveSampler; ///< Emissive light sampler. Only valid when kUseEmissiveLights == true.

static const bool kNEE = GI_USE_NEE; 
static const bool kUseAnalyticLights = GI_USE_ANALYTIC; 
static const bool kUseEmissiveLights = GI_USE_EMISSIVE;
static const bool kMixedLights = GI_USE_ANALYTIC && GI_USE_EMISSIVE;
static const bool kUseAlphaTest = GI_ALPHA_TEST;
static const bool kUseMIS = GI_MIS;
static const bool kRussianRoulette = GI_RUSSIAN_ROULETTE;
static const bool kUseShadowMap = GI_USE_SHADOW_MAP;

struct TraceData
{
    PackedHitInfo packedHitInfo; //< Packed Hit Info

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
    
    /** Create ray payload with default parameters.
    */
    static TraceData create()
    {
        TraceData d;
        d.packedHitInfo = { };
        return d;
    }
};

struct ShadowTraceData
{
    bool visible;
};

 // Describes a light sample.
struct LightSample
{
    float3 Li; ///< Incident radiance at the shading point (unshadowed). This is already divided by the pdf.
    float pdf; ///< Pdf with respect to solid angle at the shading point.
    float3 origin; ///< Ray origin for visibility evaluation (offseted to avoid self-intersection).
    float distance; ///< Ray distance for visibility evaluation (shortened to avoid self-intersection).
    float3 dir; ///< Ray direction for visibility evaluation (normalized).
    uint packedInfo = 0; ///< Packed info. |24B index | 8B SMType
    
    //shadow map light type 
    property uint smLightType{
        get { return (packedInfo & 0xff); }
        set { packedInfo = (packedInfo & ~0xff) | (newValue); }
    }
    //shadow map light type 
    property uint lightIndex{
        get { return packedInfo >> 8; }
        set { packedInfo = (packedInfo & 0xff) | (newValue << 8); }
    }
    
    RayDesc getVisibilityRay(const float3 posW)
    {
        return {posW, 0.f, dir, distance};
    }
};

uint index2Dto1D(uint2 idx, uint width){
    return idx.x + width * idx.y;
}

SMLightData lightSampleToLightData(const in LightSample ls){
    SMLightData lightData;
    lightData.posW = ls.origin;
    lightData.dirW = ls.dir;
    lightData.type = ls.smLightType;
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

   /** Generates a light sample on the emissive geometry.
        \param[in] shading data.
        \param[in] upperHemisphere True if only upper hemisphere should be considered.
        \param[in,out] sg Sample generator.
        \param[out] ls Struct describing valid samples.
        \return True if the sample is valid and has nonzero contribution, false otherwise.
    */
bool generateEmissiveSample(const in ShadingData sd, const bool upperHemisphere, inout SampleGenerator sg, out LightSample ls)
{
    ls = { }; // Default initialization to avoid divergence at returns.
    if (!kUseEmissiveLights)
        return false;

    TriangleLightSample tls;
    if (!gEmissiveSampler.sampleLight(sd.posW, sd.faceN, upperHemisphere, sg, tls))
        return false;

        // Setup returned sample.
    ls.Li = tls.pdf > 0.f ? tls.Le / tls.pdf : float3(0);
    ls.pdf = tls.pdf;
        // Offset shading and light position to avoid self-intersection.
    float3 lightPos = computeRayOrigin(tls.posW, tls.normalW);
    ls.origin = computeRayOrigin(sd.posW, dot(sd.faceN, lightPos - sd.posW) >= 0 ? sd.faceN : -sd.faceN);
    float3 toLight = lightPos - ls.origin;
    ls.distance = length(toLight);
    ls.dir = normalize(toLight);

    return any(ls.Li > 0.f);
}

    /** Generates a light sample on the analytic lights.
        \param[in] shading data.
        \param[in,out] sg Sample generator.
        \param[out] ls Struct describing valid samples.
        \return True if the sample is valid and has nonzero contribution, false otherwise.
    */
bool generateAnalyticLightSample(const in ShadingData sd, inout SampleGenerator sg, out LightSample ls)
{
    ls = { }; // Default initialization to avoid divergence at returns.

    uint lightCount = gScene.getLightCount();
    if (!kUseAnalyticLights || lightCount == 0)
        return false;

    // Sample analytic light source selected uniformly from the light list.
    // TODO: Sample based on estimated contributions as pdf.
    uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);

    // Sample local light source.
    AnalyticLightSample lightSample;
    LightData lightData = gScene.getLight(lightIndex);
    if (!sampleLight(sd.posW,lightData , sg, lightSample))
        return false;

    ls.smLightType = getShadowMapLightType(lightData);
    ls.lightIndex = lightIndex;
    // Setup returned sample.
    ls.pdf = lightSample.pdf / lightCount;
    ls.Li = lightSample.Li * lightCount;
    //get the lights position
    ls.origin = lightSample.posW;
    
    // Analytic lights do not currently have a geometric representation in the scene.
    // Do not worry about adjusting the ray length to avoid self-intersections at the light.
    ls.distance = lightSample.distance;
    ls.dir = lightSample.dir;

    return any(ls.Li > 0.f);
}

void selectLightType(out uint lightType, out float pdf, inout SampleGenerator sg)
{
    pdf = 0.5f;

    float u = sampleNext1D(sg);

    if (u < pdf)
        lightType = 0;  //analytic
    else
        lightType = 1; //emissive
}

bool generateLightSample(const in ShadingData sd, inout SampleGenerator sg, out bool isEmissive , out LightSample ls)
{
    ls = { };
    isEmissive = false;
    uint lightType = 0; //Set to analytic
    float selectionPdf = 1.f;
    if(kMixedLights)
        selectLightType(lightType, selectionPdf, sg);

    bool valid = false;
    if (kUseEmissiveLights && (!kMixedLights || lightType == 1) )
    {
        // Emissive light samplers have an option to exclusively sample the upper hemisphere.
        valid = generateEmissiveSample(sd, true, sg, ls);
        isEmissive = true;
    }
    if (kUseAnalyticLights && (!kMixedLights || lightType == 0))
    {
        valid = generateAnalyticLightSample(sd, sg, ls);
    }
    if (!valid)
        return false;

    // Reject samples in non-requested hemispheres.
    float NdotL = dot(sd.faceN, ls.dir);
    if (NdotL <= kMinCosTheta)
        return false;

    // Account for light type selection.
    ls.pdf *= selectionPdf;
    ls.Li /= selectionPdf;

    return true;
}

    /** Evaluates MIS Balance Heuristic.
        \param[in] n0 Number of samples taken from the first sampling strategy.
        \param[in] p0 Pdf for the first sampling strategy.
        \param[in] n1 Number of samples taken from the second sampling strategy.
        \param[in] p1 Pdf for the second sampling strategy.
        \return Weight for the contribution from the first strategy (p0).
    */
float evalMIS(float n0, float p0, float n1, float p1)
{
     // Balance heuristic
    float q0 = n0 * p0;
    float q1 = n1 * p1;
    return q0 / (q0 + q1);
}

[shader("miss")]
void shadowMiss(inout ShadowTraceData shadowData : SV_RayPayload)
{
    shadowData.visible = true; //Nothing was hit -> visible
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void shadowAnyHit(inout ShadowTraceData shadowData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (kUseAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    rayData.packedHitInfo = {}; //Set to empty -> Invalid
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (kUseAlphaTest)
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
void closestHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    const float3 incomingRayDir = -WorldRayDirection();
    // Evaluate Falcor's material parameters at the hit point.
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    
    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    HitInfo hitInfo = HitInfo(gVBuffer[launchIndex]);
    SurfaceGI surface = SurfaceGI();
    Reservoir reservoir = Reservoir();
    TraceData traceData = TraceData::create();
    GIData giData = GIData();
    
    if (hitInfo.isValid())
    {
        float3 view = -gView[launchIndex].xyz;
        float viewDepth = gLinZ[launchIndex];
        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hitInfo, view, lod);
        
        //Use only BRDF
        sd.mtl.setActiveLobes((uint) LobeType::Reflection);

        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

        traceData.sg = SampleGenerator(launchIndex, gFrameCount + 3 * 13);

        BSDFSample bsdfSample;
        bool validSample = bsdf.sample(sd, traceData.sg, bsdfSample);

        //Fill the RTXDI surface if it is enabled
    #if USE_RTXDI
        gRTXDI.setSurfaceData(launchIndex, sd.computeNewRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness, viewDepth);
    #endif
        
        //Fill Surface Buffer used for calculating the target PDF
        surface = SurfaceGI::create(sd.computeNewRayOrigin(), bsdfProperties.guideNormal, sd.V, viewDepth, bsdfProperties.roughness,
                                      luminance(bsdfProperties.diffuseReflectionAlbedo), luminance(bsdfProperties.specularReflectance));
        
        reservoir.targetPdf = surface.getPdf(bsdfSample.wo);
    
        reservoir.weightSum = 1.f / bsdfSample.pdf; // 1/selectPDF

        float3 thp = float3(1.f);
        float3 radiance = float3(0.f);
        float smDistance = viewDepth; //Distance used in oracle function
        
        
        for (uint i = 0; (i < gBounces) && validSample; i++)
        {
            RayDesc tRay;
            tRay.Origin = sd.computeNewRayOrigin();
            tRay.TMin = 0.0001f;
            tRay.TMax = 10000.f;
            tRay.Direction = bsdfSample.wo;

            TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, tRay, traceData);

            hitInfo = HitInfo(traceData.packedHitInfo);
            validSample = hitInfo.isValid();

            //End loop if it is not valid; TODO add enviroment sample at i>0
            if (!validSample)
                break;

            //Update the shading informations
            sd = loadShadingData(hitInfo, bsdfSample.wo, lod);
            let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
            let miProperties = mi.getProperties(sd);
            const float distanceToSample = distance(sd.posW, tRay.Origin);
            ///Handle Oracle Distance function
            if (gShadowMap.kUseOracleDistFunction)
                smDistance += distanceToSample * gShadowMap.getOracleDistanceFunctionFactor(miProperties, bsdfSample);
            
            if (i == 0)
            {
                //If an emissive light source is hit, return as this case is already covered by ReSTIR
                if (any(miProperties.emission > 0.f))
                    break;
                //Fill GIData for the first hit (the resample point). Radiance is filled in at a later point
                giData.posW = sd.computeNewRayOrigin();
                giData.normal = miProperties.guideNormal;
                giData.jacobianDeterminant = vplAttenuation(distanceToSample, gAttenuationRadius) * saturate(dot(miProperties.guideNormal, sd.V));
            }
            else if(!kNEE)  //Only use direct emissive hits for the non NEE case
            {
                radiance += thp * miProperties.emission;
            }

           
            //Get a light sample
            if (kNEE)
            {
                LightSample ls = { };
                bool isEmissive = false;
                if (generateLightSample(sd, traceData.sg,isEmissive, ls))
                {
                    if (kUseMIS && isEmissive) //Emissive
                    {
                        float scatterPdf = mi.evalPdf(sd, ls.dir, true);
                        ls.Li *= evalMIS(1, ls.pdf, 1, scatterPdf);
                    }
                    
                    float3 Lr = mi.eval(sd, ls.dir, traceData.sg) * ls.Li;
                    bool doShadowTest = true;
                    //Shadow test
                    if (any(Lr > 0))
                    {
                        float shadow = 0.f;
                        if(kUseShadowMap){
                            //Fill the shadow map light data struct
                            SMLightData lightData = lightSampleToLightData(ls);
                            float oracle = 0.f;
                            float oracleMip = 0.f;
                            bool useNormalSM = !gShadowMap.kUseHybridShadowMap;
                            if (gShadowMap.kUseOracle){
                                oracle = gShadowMap.oracleFunction(ls.lightIndex, lightData, sd, smDistance, oracleMip);
                                useNormalSM = oracle <= gShadowMap.kOracleCompValue;
                            }
                                
                            if (gShadowMap.kUseHybridShadowMap)
                            {
                                doShadowTest = !gShadowMap.evalHybridShadowMap(ls.lightIndex, sd.posW, lightData, sampleNext4D(traceData.sg), oracleMip, shadow) && !useNormalSM;
                            }
                            else if(useNormalSM)
                            {
                                doShadowTest = !gShadowMap.evalShadowMap(ls.lightIndex, sd.posW, lightData, sampleNext4D(traceData.sg), oracleMip, shadow);
                            }
                        }
                   
                        if(doShadowTest){
                            shadow = 0.f;
                            ShadowTraceData shadowData;
                            shadowData.visible = false;
                            RayDesc shadowRay = ls.getVisibilityRay(sd.computeNewRayOrigin());
                            
                            TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, rayTypeCount, 1 /* missIdx */, shadowRay, shadowData);

                            if(shadowData.visible)
                                shadow = 1.f;
                        }
                        
                        radiance += Lr * thp * shadow;
                    }
                }
            }
            
            //Generate next sample
            validSample = mi.sample(sd, traceData.sg, bsdfSample);
            thp *= bsdfSample.weight;

            //Terminate Path early with russian roulette
            if (kRussianRoulette)
            {
                const float rrVal = luminance(thp);
                const float prob = max(0.f, 1.f - rrVal);
                const float u = sampleNext1D(traceData.sg);
                if (u < prob)
                    validSample = false;
                    
                thp /= 1.f - prob;
            }
                    
        }
        
        if (giData.valid)
        {
            giData.flux = radiance;
            reservoir.M = 1; 
            reservoir.targetPdf *= luminance(radiance);
        }
        else
        {
            reservoir.weightSum = 0.f; //invalidate reservoir
        }
    }
#if USE_RTXDI
    else
    {
        gRTXDI.setInvalidSurfaceData(launchIndex);
    }
#endif //USE_RTXDI
    
    //Store data in buffers
    gGISample[index2Dto1D(launchIndex, launchDim.x)] = giData;
    gSurfaceData[index2Dto1D(launchIndex, launchDim.x)] = surface.pack();   //TODO better index
    
    #if USE_REDUCED_RESERVOIR_FORMAT
        gReservoir[launchIndex] = reservoir.packReduced();
    #else
        gReservoir[launchIndex] = reservoir.pack();
    #endif
}
