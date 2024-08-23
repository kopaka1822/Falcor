#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Utils.PixelStats;
import SMStructs;
import Rendering.ShadowMaps.ShadowMapData;
import Rendering.ShadowMaps.ShadowTestFunctions;
import Rendering.ShadowMaps.ShadowMapHelpers;

#ifndef SM_FORMAT
#define SM_FORMAT float
#endif

#ifndef LVSM_LAYERS
#define LVSM_LAYERS 2
#endif

Texture2D<SM_FORMAT> gShadowMap;
RWTexture2D<float2> gLayeredVarianceOut[LVSM_LAYERS];

Texture2D<float2> gLayeredVariance[LVSM_LAYERS];

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

RWTexture2D<float4> gOutputColor;
RWTexture2D<float4> gDebug;

cbuffer CB
{
    uint gSMSize;   //Pixel size
    float2 gSMNearFar; //Near|Far
    float gOverlap; //Overlap of the layers
    float gFrameCount;
    uint3 _pad;
    float4x4 gViewProj;
}

//Texture Sampler
SamplerState gShadowSamplerPoint;
SamplerState gShadowSamplerLinear;

/* Warp function. p,q,depth all in [0,1]

*/
float warp(float p, float q, float depth){
    float low = max(0.0,p-gOverlap);
    float high = min(1.0, q+gOverlap);
    float warpedDepth = (depth-low) / (high-low);
    warpedDepth = depth <= low ? 0.0 : warpedDepth;
    warpedDepth = high <= depth ? 1.0 : warpedDepth;

    return warpedDepth;
}

[numthreads(16, 16, 1)]
void generate(uint2 pixel: SV_DispatchThreadID)
{
   float depth = gShadowMap[pixel].x;
   //Loop through all layers and enter variance depths
   const float step = 1.0/LVSM_LAYERS;
   float layerStart = 0.0;
   float layerEnd = step;
   [unroll]
   for(uint layers = 0; layers < LVSM_LAYERS; layers++){
       float layerDepth = warp(layerStart, layerEnd, depth);
       gLayeredVarianceOut[layers][pixel] = float2(layerDepth, layerDepth * layerDepth);
       layerStart += step;
       layerEnd += step;
   }
}

ShadingData loadShadingData(const HitInfo hit, const float3 view, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -view, lod);
    return sd;
}

float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    LightData lightData = gScene.getLight(0); // Get light at index 0
    AnalyticLightSample ls = {};
    sampleLight(sd.posW, lightData, sg, ls);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection)  || all(ls.Li <= 0)) // 
        return float3(0.0);

    //Eval layer shadow map
    float shadow = 0.0;
    //Get position in light space
    float4 shadowPos = mul(gViewProj, float4(sd.posW, 1));
    shadowPos.xyz /= shadowPos.w;
    shadowPos.y *= -1;  //Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
    float lightDist = (shadowPos.w - gSMNearFar.x) / (gSMNearFar.y - gSMNearFar.x); //Linear depth

    if(any(shadowPos.xyz < 0.0) || any(shadowPos.xyz > 1.0))
        return float3(0.0); 

    //Get layered shadow value
    const float step = 1.0/LVSM_LAYERS;
    const uint selectedLayer = uint(floor(lightDist / step));
    float2 shadowValue = gLayeredVariance[selectedLayer].SampleLevel(gShadowSamplerLinear, shadowPos.xy, 0);
    float p = selectedLayer * step;
    lightDist = warp(p,p+step, lightDist);
    shadow = ChebyshevUpperBound(shadowValue, lightDist, 1e-7f);

    // Evaluate contribution.
    return mi.eval(sd, ls.dir, sg) * ls.Li * shadow;
}

[numthreads(16, 16, 1)]
void evaluate(uint2 pixel: SV_DispatchThreadID)
{
    float3 outColor = float3(0.00);
    const float3 view = -gViewW[pixel].xyz;
    HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (hit.isValid())
    {
        // Pixel represents a valid primary hit. Compute its contribution.
        let lod = ExplicitLodTextureSampler(0.f);
        // Load shading data.
        ShadingData sd = loadShadingData(hit,view, lod);
         // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod);
        // Create sample generator.
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount);

         // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
        outColor += mi.getProperties(sd).emission;

        outColor += evalDirectAnalytic(sd, mi, sg);

    }else{
        outColor = float3(0.01); //BG color
    }
    gOutputColor[pixel] = float4(outColor, 1);
}
