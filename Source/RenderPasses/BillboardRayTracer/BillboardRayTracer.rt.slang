/***************************************************************************
 # Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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

#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded.
}

// early out settings
#define TRANSMIT_EPISLON 0.02

// Outputs
RWTexture2D<float4> gOutputColor;

/** Payload for shadow ray.
*/
struct ShadowRayData
{
    bool visible;
};

struct RayPayload
{
    float3 color; // [in/out] sum of all colors
    float transmit; // [in/out] contains transmittance for the next transmitted or reflected ray
    // TODO ray diffs
    RayDesc nextRay; // [out] for reflection/transmittance
};


/** Traces a shadow ray towards a light source.
    \param[in] origin Ray origin for the shadow ray.
    \param[in] dir Direction from shading point towards the light source (normalized).
    \param[in] distance Distance to the light source.
    \return True if light is visible, false otherwise.
*/
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false;    // Set to true by miss shader if ray is not terminated before
    TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, hitProgramCount, 1 /* missIdx */, ray, rayData);

    return rayData.visible;
}

/** ********************* Ray index 0: Scatter ray ************************ */

[shader("miss")]
void scatterMiss(inout RayPayload rayData : SV_RayPayload)
{
    float3 Le = gScene.envMap.eval(WorldRayDirection());
    rayData.color += Le;
    rayData.transmit = 0.0;
}

[shader("anyhit")]
void scatterAnyHit(
    uniform HitShaderParams hitParams,
    inout RayPayload rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // Alpha test for non-opaque geometry.
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) IgnoreHit();
}

[shader("closesthit")]
void scatterClosestHit(
    uniform HitShaderParams hitParams,
    inout RayPayload rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // TODO: Implement texLOD to enable texture filtering in prepareShadingData().
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -WorldRayDirection(), 0.f);

    // Compute tangent space if it is invalid.
    if (!(dot(sd.T, sd.T) > 0.f))   // Note: Comparison written so that NaNs trigger
    {
        sd.T = perp_stark(sd.N);
        sd.B = cross(sd.N, sd.T);
    }

    // emission
    float3 color = sd.emissive;
    // light probe
    color += evalMaterial(sd, gScene.lightProbe).color;

    const uint lightCount = gScene.getLightCount();
    // forward render all light sources
    for(uint i = 0; i < lightCount; ++i)
    {
        // TODO add shadow map sample
        // Sample local light source.
        color += evalMaterial(sd, gScene.getLight(i), /*shadow*/ 1.0).color;
    }

    // update ray payload
    rayData.color += rayData.transmit * sd.opacity * color;
    rayData.transmit *= (1.0 - sd.opacity);
    //rayData.transmit = 0.0;

    // TODO use impostor billboard correction (maybe make sure that it does not have the same issues)
    // Compute ray origin for new rays spawned from the hit.
    //rayData.nextRay.Origin = sd.computeNewRayOrigin();

    // test if refractive
    //rayData.color = sd.opacity;
    rayData.transmit = 0.0;
    return;

    bool isRefractive = (rayData.transmit > TRANSMIT_EPISLON) && (sd.opacity < 0.6);
    if(isRefractive) rayData.color = float3(1.0, 0.0, 0.0);
    /*if(isRefractive)
    {
        rayData.nextRay.TMin = RayTCurrent();
    }
    else rayData.transmit = 0.0; // finish here*/
    rayData.transmit = 0.0;

    // test if reflections need to be handled
    //if(sd.linearRoughness > 0.05) return;

    //rayData.color = float3(1.0, 0.0, 0.0);
    // TODO handle reflection/refraction
    // use sd.eta or sd.specular
}

/************************** Ray index 1: Shadow ray ************************ */

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData : SV_RayPayload)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

[shader("anyhit")]
void shadowAnyHit(
    uniform HitShaderParams hitParams,
    inout ShadowRayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // Alpha test for non-opaque geometry.
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) IgnoreHit();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // primary ray
    RayDesc ray = gScene.camera.computeRayPinhole(launchIndex, launchDim).toRayDesc();

    RayPayload rayData;
    rayData.color = 0.0;
    rayData.transmit = 1.0;
    rayData.nextRay = ray;

    uint iteration = 0;
    do
    {
        TraceRay(gRtScene, RAY_FLAG_NONE, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, rayData.nextRay, rayData);
        
    } while(++iteration < 256 && rayData.transmit > TRANSMIT_EPISLON);

    gOutputColor[launchIndex] = float4(rayData.color, 1);
}
