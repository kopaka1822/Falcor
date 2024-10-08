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
import Rendering.Materials.IsotropicGGX;
import RenderPasses.Shared.Denoising.NRDConstants;

//Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;

//Outputs
RWTexture2D<float4> gOutColor;
RWTexture2D<float4> gOutEmission;
RWTexture2D<float4> gOutDiffuseRadiance;
RWTexture2D<float4> gOutSpecularRadiance;
RWTexture2D<float4> gOutDiffuseReflectance;
RWTexture2D<float4> gOutSpecularReflectance;
RWTexture2D<float4> gOutUnshadowed;
RWTexture2D<float> gOutPenumbra;

uniform EmissiveLightSampler gEmissiveSampler; ///< Emissive light sampler. Only valid when kUseEmissiveLights == true.

cbuffer CB
{
    uint gFrameCount;   //Current Frame Count for the sample generator
    uint gSPP;
    float gAmbientFactor;  //Tells if the LTT mask is valid
    float gEmissiveFactor;
    float gEnvMapFactor;
}

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const bool kRayAlphaTest = ALPHA_TEST;
static const bool kUseEnvBackground = USE_ENV_MAP;
static const float3 kDefaultBackgroundColor = float3(0.01);

static const float kFP16Max = 65504.0; //NRD FP16 max

 /** Describes a light sample.
    */
struct LightSample
{
    float3 Li; ///< Incident radiance at the shading point (unshadowed). This is already divided by the pdf.
    float pdf; ///< Pdf with respect to solid angle at the shading point.
    float3 origin; ///< Ray origin for visibility evaluation (offseted to avoid self-intersection).
    float distance; ///< Ray distance for visibility evaluation (shortened to avoid self-intersection).
    float3 dir; ///< Ray direction for visibility evaluation (normalized).
    uint lightType; ///< Light type this sample comes from (0 -> analytic; 1 -> emissive).

    RayDesc getVisibilityRay()
    {
        return {origin, 0.f, dir, distance};
    }
};

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    float distance;
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

    TriangleLightSample tls;
    if (!gEmissiveSampler.sampleLight(sd.posW, sd.faceN, upperHemisphere, sg, tls))
        return false;

        // Setup returned sample.
    ls.Li = tls.pdf > 0.f ? (tls.Le * gEmissiveFactor) / tls.pdf : float3(0);
    ls.pdf = tls.pdf;
        // Offset shading and light position to avoid self-intersection.
    float3 lightPos = computeRayOrigin(tls.posW, tls.normalW);
    ls.origin = computeRayOrigin(sd.posW, dot(sd.faceN, lightPos - sd.posW) >= 0 ? sd.faceN : -sd.faceN);
    float3 toLight = lightPos - ls.origin;
    ls.distance = length(toLight);
    ls.dir = normalize(toLight);
    ls.lightType = 1;

    return any(ls.Li > 0.f);
}

[shader("miss")]
void miss(inout ShadowRayData rayData)
{
    rayData.distance = -1.f; 
}

[shader("closesthit")]
void closestHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs) {
    rayData.distance = RayTCurrent();
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
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 frameDim = DispatchRaysDimensions().xy;

    HitInfo hitInfo = HitInfo(gVBuffer[pixel]);
    const float3 view = -gView[pixel].xyz;
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount);

    float3 outColor = float3(0); //If demodulated, ambient is added
    float3 emission = float3(0);
    float3 diffuse = float3(0);
    float3 specular = float3(0);

    //Demodulation variables
    #if NRD_DEMODULATION
        float3 diffuseReflectance = {};
        float3 specularReflectance = {};
        float hitT = 0.0;
        float penumbra = kFP16Max;
        float3 unshadowedColor = {};
    #endif

    if (hitInfo.isValid())
    {
        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hitInfo, view, lod);

        //uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, 0u);
        let bsdfProperties = bsdf.getProperties(sd);

        //Emissive
        emission = bsdfProperties.emission * gEmissiveFactor;
        //Ambient
        outColor += bsdfProperties.diffuseReflectionAlbedo * gAmbientFactor;

        const float weight = 1.0/gSPP;
        //Sample Lights
        for(uint i=0; i<gSPP; i++){
            LightSample ls = { };
            if(generateEmissiveSample(sd,true ,sg, ls)){
                sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
                float3 LrDiffuse = bsdf.eval(sd, ls.dir, sg) * ls.Li;
                sd.mtl.setActiveLobes((uint) LobeType::Specular);
                float3 LrSpecular = bsdf.eval(sd, ls.dir, sg) * ls.Li;
                #if NRD_DEMODULATION
                    unshadowedColor = LrDiffuse + LrSpecular;
                #endif
                //Shadow test
                if (any(ls.Li > 0.0))
                {
                    ShadowRayData shadowData;
                    shadowData.distance = -1.f;
                    RayDesc shadowRay = ls.getVisibilityRay();
                    //uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
                    uint flags = 0;
                    TraceRay(gScene.rtAccel, flags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, shadowRay, shadowData);

                    if(shadowData.distance < 0){
                        diffuse += LrDiffuse * weight;
                        specular += LrSpecular * weight;
                        #if NRD_DEMODULATION
                            hitT = distance(ls.origin, sd.posW);
                        #endif
                    }
                    #if NRD_DEMODULATION
                    else {
                        hitT = shadowData.distance;
                        penumbra = hitT;
                    }
                    #endif
                }
            }
            #if NRD_DEMODULATION
                else {
                    penumbra = 0.0;
                }
            #endif
        }

        #if NRD_DEMODULATION
            //Create NRD Guide Buffers
            //For diffuse use the sum of the Reflection and Transmission albedo
            diffuseReflectance = max(kNRDMinReflectance, bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo);
            //For specular an approximate shading model with a single specular lobe is used
            const float NdotV = saturate(dot(bsdfProperties.guideNormal, sd.V));
            const float ggxAlpha = bsdfProperties.roughness * bsdfProperties.roughness;
            specularReflectance = approxSpecularIntegralGGX(bsdfProperties.specularReflectionAlbedo, ggxAlpha, NdotV);
            specularReflectance = max(kNRDMinReflectance, specularReflectance);

            //Demodulate specular and diffuse
            diffuse = diffuse / diffuseReflectance;
            specular = specular / specularReflectance;
            
        #else //Add everything to color
            outColor += emission + diffuse + specular;
        #endif
    }
    else{
        //Sample Env Map
        outColor = kUseEnvBackground ? gScene.envMap.eval(view) * gEnvMapFactor : kDefaultBackgroundColor;
    }

    gOutColor[pixel] = float4(outColor, 1.0);
    //Output all demodulated textures
    #if NRD_DEMODULATION
        if(is_valid(gOutEmission)) gOutEmission[pixel] = float4(emission,1.0);
        if(is_valid(gOutDiffuseRadiance)) gOutDiffuseRadiance[pixel] = float4(diffuse,hitT);
        if(is_valid(gOutSpecularRadiance)) gOutSpecularRadiance[pixel] = float4(specular,hitT);
        if(is_valid(gOutDiffuseReflectance)) gOutDiffuseReflectance[pixel] = float4(diffuseReflectance,1.0);
        if (is_valid(gOutSpecularReflectance)) gOutSpecularReflectance[pixel] = float4(specularReflectance, 1.0);
        if (is_valid(gOutUnshadowed)) gOutUnshadowed[pixel] = float4(unshadowedColor, 1.0);
        if (is_valid(gOutPenumbra)) gOutPenumbra[pixel] = penumbra;
    #endif
}
