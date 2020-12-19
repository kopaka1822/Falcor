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
#define REFLECT_EPSILON 0.02

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

struct BoxIntersectionAttributes
{
    int dummy;
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

/** ********************* Ray index 0 ************************ */

[shader("miss")]
void miss(inout RayPayload rayData : SV_RayPayload)
{
    float3 Le = gScene.envMap.eval(WorldRayDirection());
    rayData.color += Le;
    rayData.transmit = 0.0;
}

[shader("anyhit")]
void triangleAnyHit(
    uniform HitShaderParams hitParams,
    inout RayPayload rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // Alpha test for non-opaque geometry.
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f)) IgnoreHit();
}

[shader("closesthit")]
void triangleClosestHit(
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
    // determine actual transmittance (mix of opacity coverage and specular transmission)
    const float transmit = sd.opacity * sd.specularTransmission + (1.0 - sd.opacity);

    bool isTransmittive = (rayData.transmit * transmit > TRANSMIT_EPISLON);
    bool isReflective = dot(sd.specular, 0.33) * rayData.transmit > REFLECT_EPSILON && sd.linearRoughness < 0.02;
    const float3 reflected = reflect(WorldRayDirection(), sd.N);

    if(isTransmittive)
    {
        if(isReflective) // envmap lookup for reflection
            rayData.color += rayData.transmit * sd.specular * gScene.envMap.eval(reflected);

        rayData.transmit = transmit;
        const float3 refracted = sd.IoR == 1.0 ? WorldRayDirection() : refract(WorldRayDirection(), sd.N, sd.eta);
        rayData.nextRay.Direction = refracted;
        if(USE_REFRACTION_CORRECTION) {
            rayData.nextRay.Origin = sd.computeNewRayOrigin(false) - refracted * RayTCurrent();
            rayData.nextRay.TMin = RayTCurrent();
        } else {
            rayData.nextRay.Origin = sd.computeNewRayOrigin(false);
            rayData.nextRay.TMin = 0.0;
        }
    }
    else if(isReflective)
    {
        rayData.transmit *= dot(sd.specular, 0.33);
        rayData.nextRay.Direction = reflected;
        if(USE_REFLECTION_CORRECTION) {
            rayData.nextRay.Origin = sd.computeNewRayOrigin() - reflected * RayTCurrent();
            rayData.nextRay.TMin = RayTCurrent();
        } else {
            rayData.nextRay.Origin = sd.computeNewRayOrigin();
            rayData.nextRay.TMin = 0.0;
        }
    }
    else
    {
        rayData.transmit = 0.0; // stop tracing rays
    }
}

/***************** Billboards *****************/

float3x3 GetBillboardPlane(float3 normal, bool yAlign) {
	float3x3 planeBase;
	planeBase[2] = normal;
	
	// y axis alignment
	if(yAlign) planeBase[2] = normalize(float3(planeBase[2].x, 0.0, planeBase[2].z));

	float3 up = float3(0.0, 1.0, 0.0);
	// x, y are plane tangents
	planeBase[0] = normalize(cross(up, planeBase[2]));
	planeBase[1] = cross(planeBase[2], planeBase[0]);
	return planeBase;
}

[shader("intersection")]
void boxIntersect(uniform HitShaderParams hitParams)
{
    const uint aabbOffset = gScene.proceduralPrimitives[hitParams.geometryIndex].AABBOffset;
    AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];

    switch(BILLBOARD_TYPE)
    {
    case BILLBOARD_TYPE_IMPOSTOR:
    case BILLBOARD_TYPE_PARTICLE: {
        const float2 dim = bbox.extent().xy;
        const float3 P = bbox.center();
        const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;
        const bool round = BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE;

        // billboard plane base
        const float3x3 planeBase = GetBillboardPlane(normalize(WorldRayOrigin() - P), yAlign);
        // ray origin and direction in plane space
        const float3 d = mul(planeBase, WorldRayDirection());
        const float3 origin = mul(planeBase, WorldRayOrigin() - P);

        // ray intersection parameter:
	    // origin + s * d = (X.x, X.y, 0) with X = intersection point of plane
        const float s = -origin.z / d.z;
        if(s < RayTMin()) return;
        const float2 X = origin.xy + s * d.xy;

        // check if on plane
        if(any(X < -0.5 * dim) || any(X > 0.5 * dim)) return;
	    if(round && (4.0 * X.x * X.x / (dim.x * dim.x) + 4.0 * X.y  * X.y  / (dim.y * dim.y) > 1.0)) return;

        BoxIntersectionAttributes attr;
        attr.dummy = 0;
        ReportHit(s, 0, attr);

    } break;
    case BILLBOARD_TYPE_SPHERICAL: break;
    }
}

[shader("anyhit")]
void boxAnyHit(
    uniform HitShaderParams hitParams,
    inout RayPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    
}

[shader("closesthit")]
void boxClosestHit(
    uniform HitShaderParams hitParams,
    inout RayPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    rayData.color += 1.0; //rayData.transmit;
    rayData.transmit = 0.0;
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
        // set ray t max accordingly
        rayData.nextRay.TMax = ray.TMax;
    } while(++iteration < 256 && rayData.transmit > TRANSMIT_EPISLON);

    gOutputColor[launchIndex] = float4(rayData.color, 1);
}
