/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Materials.IsotropicGGX;
import Rendering.Materials.Fresnel;
import Rendering.ShadowMaps.ShadowMap;

import ShadowPassData;

//Inputs
Texture2D<float4> gPosW;
Texture2D<float4> gNormalW;
Texture2D<float4> gFaceNormalW;

//Optional Inputs Complex Shading
Texture2D<float4> gTangentW;
Texture2D<float2> gTexCoord;
Texture2D<float4> gTexGrads;
Texture2D<uint4> gMaterialInfo;

//Optional Inputs Simplified Shading
Texture2D<float4> gDiffuse;
Texture2D<float4> gSpecRough;

//Outputs
RWTexture2D<float4> gColor;

cbuffer CB
{
    uint gFrameCount;   //Current Frame Count for the sample generator
}

//Defines for syntax highlighting
#ifndef SIMPLIFIED_SHADING
#define SIMPLIFIED_SHADING 1
#endif

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kShadowMode = SP_SHADOW_MODE;
static const bool kRayAlphaTest = ALPHA_TEST;
static const bool kNormalTest = SP_NORMAL_TEST;

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    bool visible;
};

//Calculates the shadowed Li for a given light
bool sampleLight(const float3 posW,const float3 N, const float3 faceN, const uint lightIdx, out float3 toLight, out float3 Li){
    Li = float3(0);
    toLight = float3(0);
    //Sample the light
    AnalyticLightSample ls;
    LightData lightData = gScene.getLight(lightIdx);
    if (!evalLightApproximate(posW, lightData, ls))
        return false;

    //Quick check if point is shaded
    float NdotL = dot(N, ls.dir);
    if(NdotL <= 0)
        return false;
        
    //Shadow test
    float shadow = 0.f;
    bool useRay = kShadowMode == (uint)SPShadowMode::RayTraced;

    //Eval shadow map if mode is enabled
    if(!useRay){
        SMLightData smLightData = getShadowMapLightData(lightData);
        useRay = !gShadowMap.evalHybridShadowMap(lightIdx, posW, smLightData, float4(0) /*random*/, 0.f /*MipLevel*/, shadow);
    }

    //Shoot Shadow ray
    if(useRay && kShadowMode != (uint)SPShadowMode::ShadowMap){
        RayDesc ray;
        ray.Origin = computeRayOrigin(posW, faceN);
        ray.Direction = ls.dir;
        ray.TMin = 0.0f;
        ray.TMax = ls.distance;   //No offset needed, as the light is analytic and does not exist in the scene as a physical object

        ShadowRayData rayData;
        rayData.visible = false;  // Set to true by miss shader if ray is not terminated before
            
        TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        shadow = rayData.visible ? 1.f : 0.f;                
    }

    toLight = ls.dir;
    Li = ls.Li * shadow;
    return true;
}

/** Adjusts the normal of the supplied shading frame to reduce black pixels due to back-facing view direction.
*/
void adjustShadingNormal(const float3 V, const float3 faceN, inout float3 N)
{
    //Flip the shading normal to the same side as the face normal
    float signN = dot(N, faceN) >= 0.f ? 1.f : -1.f;
    N = signN * N;

    // Blend the shading normal towards the geometric normal at grazing angles.
    // This is to avoid the view vector from becoming back-facing.
    const float kCosThetaThreshold = 0.1f;
    float cosTheta = dot(V, N);
    if (cosTheta <= kCosThetaThreshold)
    {
        float t = saturate(cosTheta * (1.f / kCosThetaThreshold));
        N = normalize(signN * lerp(faceN, N, t));
    }
}

//Calculates the BRDF for the simplified shading model (DGF Microfaset BRDF)
float3 simplifiedShading(const float3 posW, const float3 N,const float3 viewDir ,const float3 diffuse,
                         const float3 specular, float roughness, const float3 toLight)
{
    const float3 H = normalize(viewDir + toLight);
    const float NdotV = saturate(dot(N, viewDir));
    const float NdotL = saturate(dot(N, toLight));
    const float NdotH = saturate(dot(N, H));
    const float LdotH = saturate(dot(toLight, H));

    roughness = max(roughness, 0.08);
    
    //DGF Microfaset BRDF
    float D = evalNdfGGX(roughness * roughness, NdotH);
    float G = evalMaskingSmithGGXSeparable(roughness, NdotL, NdotV);
    float3 F = luminance(specular) < 1e-8f ? float3(0.f) :  evalFresnelSchlick(specular, 1.f, LdotH);

    //eval brdf terms
    float3 diffuseBRDF = diffuse * NdotL * M_1_PI;
    float3 specularBRDF = D * G * F / (4.f * NdotV);

    float3 sumWeights = diffuse + specular;
    float3 diffuseProb = luminance(sumWeights) < 1e-7f ? float3(1.0f) : saturate(diffuse / sumWeights);
    
    //Mix diffuse and specular BRDF
    float3 brdf = max(0.f, lerp(specularBRDF, diffuseBRDF, diffuseProb));
    return max(0.f, brdf);
}

[shader("miss")]
void miss(inout ShadowRayData rayData)
{
    rayData.visible = true;
}

[shader("anyhit")]
void anyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    if (kRayAlphaTest)
    {
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        const uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    //Load the texture info for shading
    float3 posW = gPosW[pixel].xyz;
    float3 normalW = gNormalW[pixel].xyz;
    float3 faceNormalW = gFaceNormalW[pixel].xyz;
    float4 tangentW = gTangentW[pixel];
    
    //Get view dir from camera
    float3 view = -gScene.camera.computeRayPinhole(pixel, frameDim).dir;
    
    float3 color = float3(0.f);
    
#if SIMPLIFIED_SHADING
    float3 diffuse = gDiffuse[pixel].xyz;
    float4 specRough = gSpecRough[pixel];

    //Flip Face Normal of double sided mats
    if(dot(faceNormalW, view) <= 0.f)
    {
        faceNormalW = -faceNormalW;
    }

    adjustShadingNormal(view, faceNormalW, normalW);
    
     //Loop over all lights
    const uint lightCount = gScene.getLightCount();
    uint validLights = 0;
    for(uint i=0 ; i<lightCount; i++){
        float3 Li, toLight;
        if(!sampleLight(posW, normalW, faceNormalW, i, toLight, Li))
            continue;
        color += simplifiedShading(posW,normalW, view, diffuse, specRough.xyz, specRough.w, toLight) * Li;
    }
#else
    float2 texCoord = gTexCoord[pixel];
    uint4 materialInfo = gMaterialInfo[pixel];
    float4 grads = gTexGrads[pixel];
    
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 2 * 7);

    //Fill Vertex Data struct for the shading
    VertexData v = {posW, normalW, tangentW, texCoord, faceNormalW, 0.f, 0.f};
    uint materialID = materialInfo.x;
    
    let lod = ExplicitGradientTextureSampler(grads.xy, grads.zw);
    
    //Create
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, view, lod);
    sd.mtl.setActiveLobes((uint) LobeType::DiffuseReflection | (uint) LobeType::SpecularReflection);

    // Create material instance.
    uint hints = (uint)MaterialInstanceHints::AdjustShadingNormal;
    let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = mi.getProperties(sd);
    
    float useRayTest = 0.f;
    //Loop over all lights
    const uint lightCount = gScene.getLightCount();
    uint validLights = 0;
    for(uint i=0 ; i<lightCount; i++){
        float3 Li, toLight;
        if(!sampleLight(sd.posW, bsdfProperties.guideNormal , sd.getOrientedFaceNormal(), i, toLight, Li))
            continue;

        color += mi.eval(sd, toLight, sg) * Li;
        //color += mi.evalPdf(sd, toLight);
    }
#endif
    
    gColor[pixel] = float4(color, 1.f);
}
