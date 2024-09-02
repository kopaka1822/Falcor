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
import Rendering.ShadowMaps.ShadowMapData;
import Rendering.ShadowMaps.ShadowTestFunctions;
import Rendering.ShadowMaps.ShadowMapHelpers;

#ifndef SM_FORMAT
#define SM_FORMAT float
#endif

#ifndef LVSM_LAYERS
#define LVSM_LAYERS 2
#endif

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

Texture2D<uint> gOutLayersNeededMax;
Texture2D<uint> gOutLayersNeededMin;
Texture2D<SM_FORMAT> gShadowMap;
Texture2D<SM_FORMAT> gVirtualLayers;

RWTexture2D<float4> gOutputColor;
RWTexture2D<float4> gDebug;

cbuffer CB
{
    uint gSMSize;   //Pixel size
    float2 gSMNearFar;
    float gOverlap; //Layer overlap
    float gFrameCount;
    uint3 _pad;
    float4x4 gViewProj;   //SM View Projection
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

ShadingData loadShadingData(const HitInfo hit, const float3 view, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -view, lod);
    return sd;
}

float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg, inout float3 debugOut)
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

    uint2 smPixel = uint2((shadowPos.xy * gSMSize) / 2.0);
    uint minLayer = gOutLayersNeededMin[smPixel];
    uint maxLayer = gOutLayersNeededMax[smPixel];
    debugOut = float3(minLayer/255.0, maxLayer/255.0, 0);
    //Get layered shadow value
    const float step = 1.0/LVSM_LAYERS;
    const uint selectedLayer = uint(floor(lightDist / step));
    float p = minLayer * step;
    float q  = maxLayer * step;
    if(minLayer == 0 && maxLayer == 255){
        p = selectedLayer * step;
        q = p;
    }

    lightDist = warp(p,q + step, lightDist);
    //Warp the shadow map values
    float2 gatherUV;
    float4 w = getGatherWeights(shadowPos.xy, gSMSize, gatherUV);
    float4 shadowMapGather = gShadowMap.GatherRed(gShadowSamplerPoint, gatherUV);
    float4 virtualLayerGather = gVirtualLayers.GatherRed(gShadowSamplerPoint, gatherUV);

    float2 shadowValue = float2(0);
    float2 virtualShadowValue = float2(0);
    //Warp and interpolate
    [unroll]
    for(uint i=0 ; i<4;i++){
        float warpedDepth = warp(p,q + step, shadowMapGather[i]);
        shadowValue += float2(warpedDepth, warpedDepth * warpedDepth) * w[i];
        warpedDepth = warp(p,q + step, virtualLayerGather[i]);
        virtualShadowValue += float2(warpedDepth, warpedDepth * warpedDepth) * w[i];
    }

    shadow = min(ChebyshevUpperBound(shadowValue, lightDist, 1e-7f), ChebyshevUpperBound(virtualShadowValue, lightDist, 1e-7f));

    // Evaluate contribution.
    return mi.eval(sd, ls.dir, sg) * ls.Li * shadow;
}


[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    float3 outColor = float3(0.00);
    float3 debugOut = float3(0.00);
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

        

        outColor += evalDirectAnalytic(sd, mi, sg, debugOut);
        //outColor = debugOut;

    }else{
        outColor = float3(0.01); //BG color
    }
    gOutputColor[pixel] = float4(outColor, 1);
}
