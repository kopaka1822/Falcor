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

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.ShadowMaps.ShadowMap;
import ShadowPassData;

//Inputs
Texture2D<float4> gPosW;
Texture2D<float4> gNormalW;
Texture2D<float4> gTangentW;
Texture2D<float2> gTexCoord;
Texture2D<float4> gFaceNormalW;
Texture2D<uint4> gMaterialInfo;

//Outputs
RWTexture2D<float4> gColor;

cbuffer CB
{
    uint gFrameCount;   //Current Frame Count for the sample generator
}

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
    float4 tangentW = gTangentW[pixel];
    float2 texCoord = gTexCoord[pixel];
    float3 faceNormalW = gFaceNormalW[pixel].xyz;
    uint4 materialInfo = gMaterialInfo[pixel];

    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 2 * 7);
    
    //Get view dir from camera
    float3 view = gScene.camera.computeRayPinhole(pixel, frameDim).dir;
    
    //Fill Vertex Data struct for the shading
    VertexData v = {posW, normalW, tangentW, texCoord, faceNormalW, 0.f, 0.f};
    uint materialID = materialInfo.x;
    let lod = ExplicitLodTextureSampler(0.f);

    //Create
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -view, lod);
    sd.mtl.setActiveLobes((uint) LobeType::DiffuseReflection | (uint) LobeType::SpecularReflection);

    
    // Create material instance.
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
    
    float3 color = float3(0.f);
    float useRayTest = 0.f;
    //Loop over all lights
    const uint lightCount = gScene.getLightCount();
    uint validLights = 0;
    for(uint i=0 ; i<lightCount; i++){
        //Sample the light
        AnalyticLightSample ls;
        LightData lightData = gScene.getLight(i);
        if (!sampleLight(sd.posW, lightData, sg, ls))
            continue;
        //Check if part is not illuminated (Spot only)
        if(lightData.type == (uint)LightType::Point)
        {
            float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
            if(NdotL <= kMinCosTheta)
                continue;
        }

        //Shadow test
        float shadow = 0.f;
        bool useRay = kShadowMode == (uint)SPShadowMode::RayTraced;

        //Eval shadow map if mode is enabled
        if(!useRay){
            SMLightData smLightData = getShadowMapLightData(lightData);

            useRay = !gShadowMap.evalHybridShadowMap(i, posW, smLightData, float4(0) /*random*/, 0.f /*MipLevel*/, shadow);
        }

        //Shoot Shadow ray
        if(useRay && kShadowMode != (uint)SPShadowMode::ShadowMap){
            RayDesc ray;
            ray.Origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
            ray.Direction = ls.dir;
            ray.TMin = 0.0f;
            ray.TMax = ls.distance;   //No offset needed, as the light is analytic and does not exist in the scene as a physical object

            ShadowRayData rayData;
            rayData.visible = false;  // Set to true by miss shader if ray is not terminated before
            
            TraceRay(gScene.rtAccel, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

            shadow = rayData.visible ? 1.f : 0.f;                
        }

        color += mi.eval(sd, ls.dir, sg) * ls.Li * shadow;
    }

        
    gColor[pixel] = float4(color, 1.f);
}
