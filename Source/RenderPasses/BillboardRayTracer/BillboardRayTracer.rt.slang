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
#include "Scene/Material/MaterialDefines.slangh"
#include "Scene/Lights/Lights.slang"
//#include "RenderPasses/Shared/PathTracer/RayFootprint.slang"

import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Scene.Lights.LightData;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;
import Scene.TextureSampler;
import Scene.Material.MaterialData;
import Scene.HitInfo;
import RenderPasses.Shared.PathTracer.RayFootprintData;

#include "Helper.slang"

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded.
}



// early out settings
#define TRANSMIT_EPSILON 0.02
#define REFLECT_EPSILON 0.02

// Outputs
RWTexture2D<float4> gOutputColor;

struct RayPayload
{
    PackedHitInfo hit;
    float depth;
};

struct BoxIntersectionAttributes
{
    int aabbOffset;
};

/** ********************* Ray index 0 ************************ */

[shader("miss")]
void miss(inout RayPayload rayData : SV_RayPayload)
{
    
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
    rayData.depth = RayTCurrent();
    HitInfo hit;
    hit.type = InstanceType::TriangleMesh;
    hit.instanceID = hitParams.getGlobalHitID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    rayData.hit = hit.encode();

  
    
}

/***************** Billboards *****************/



[shader("intersection")]
void boxIntersect(uniform HitShaderParams hitParams)
{
    const uint aabbOffset = gScene.proceduralPrimitives[hitParams.geometryIndex].AABBOffset;
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    
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
        if(s <= RayTMin()) return;
        const float2 X = origin.xy + s * d.xy;
    
        // check if on plane
        if(any(X < -0.5 * dim) || any(X > 0.5 * dim)) return;
	    if(round && (4.0 * X.x * X.x / (dim.x * dim.x) + 4.0 * X.y  * X.y  / (dim.y * dim.y) > 1.0)) return;
    
        BoxIntersectionAttributes attribs;
        attribs.aabbOffset = aabbOffset;
        ReportHit(s, 0, attribs);
    
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
    rayData.depth = RayTCurrent();
    HitInfo hit;
    hit.type = InstanceType::Curve;
    hit.instanceID = 0;
    hit.primitiveIndex = attribs.aabbOffset;
    hit.barycentrics = 0.0;
    rayData.hit = hit.encode();
    //
    //rayData.transmit *= (1.0 - sd.opacity);
    //
    //// copy most ray data for simple transmission
    //rayData.nextRay.Direction = WorldRayDirection();
    //if(USE_REFRACTION_CORRECTION) {
    //    rayData.nextRay.Origin = WorldRayOrigin();
    //    rayData.nextRay.TMin = RayTCurrent();
    //} else {
    //    rayData.nextRay.Origin = sd.posW;
    //    rayData.nextRay.TMin = 0.0001;
    //}
}

float4 shade(ShadingData sd)
{
    float3 curColor = sd.emissive;

    // forward render all light sources
    const uint lightCount = gScene.getLightCount();
    for(uint i = 0; i < lightCount; ++i) // TODO add shadow map sample
        curColor += evalMaterial(sd, gScene.getLight(i), /*shadow*/ 1.0).color;

    curColor *= sd.opacity;
    // determine actual transmittance (mix of opacity coverage and specular transmission)
    const float curTransmit = sd.opacity * sd.specularTransmission + (1.0 - sd.opacity);

    return float4(curColor, curTransmit);
}



[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    // primary ray
    RayDesc ray = gScene.camera.computeRayPinhole(launchIndex, launchDim).toRayDesc();
    ray.TMin = 0.0;
    RayFootprint footprint = RayFootprint.create(launchIndex, launchDim);

    //footprint.create();
    float3 color = 0.0;
    float transmit = 1.0;

    uint iteration = 0;
    do
    {
        // first trace triangles to determine depth range
        // (only required for spherical particles, could be merged with procedural path otherwise)
        RayPayload trianglePayload;
        trianglePayload.depth = FLT_MAX;
        trianglePayload.hit = HitInfo::kInvalidIndex;
        ray.TMax = FLT_MAX;
        TraceRay(gRtScene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, trianglePayload);
        // set max range for volume ray
        ray.TMax = trianglePayload.depth;
        const float prevRayMin = ray.TMin;

        do // trace volumetric data
        {
            HitInfo volumeHit;
            RayPayload volumePayload;
            volumePayload.hit = HitInfo::kInvalidIndex;
            TraceRay(gRtScene, RAY_FLAG_SKIP_TRIANGLES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, volumePayload);
            // handle volume data
            if(!volumeHit.decode(volumePayload.hit)) break; // finished with the volumetric part

            VertexData v = getBillboardVertexData(volumeHit.primitiveIndex, ray.Origin, ray.Origin + ray.Direction * volumePayload.depth);

            RayFootprint volumeFootprint = footprint; // copy footprint
            volumeFootprint.hitSurface(v, volumeHit, /*reconstruct original ray origin*/ ray.Origin + prevRayMin * ray.Direction, ray.Direction); // transfer to surface
            ShadingData sd = prepareBillboardShadingData(v, volumeFootprint, volumeHit, ray.Origin, ray.Direction);
            float4 curColor = shade(sd);

            color += transmit * curColor.rgb;
            transmit *= curColor.a;

            // prepare next ray (TODO update ray differentials)
            ray.TMin = volumePayload.depth;
        } while(++iteration < 256 && transmit > TRANSMIT_EPSILON);

        HitInfo hit;
        // evaluate triangle geometry
        if(transmit <= TRANSMIT_EPSILON || !hit.decode(trianglePayload.hit)) break;
      
        // unpack hit data & TODO determine ray differentials
        StaticVertexData triangleVertices[3];
        VertexData v = gScene.getVertexData(hit, triangleVertices);
        uint materialID = gScene.getMaterialID(hit.instanceID);

        // transfer ray differential and get shading data
        footprint.hitSurface(v, hit, /*reconstruct original ray origin*/ ray.Origin + prevRayMin * ray.Direction, ray.Direction);
        ShadingData sd = prepareShadingData(v, footprint, triangleVertices, hit, /*reconstruct original ray origin*/ ray.Origin + prevRayMin * ray.Direction, ray.Direction);
        float4 curColor = shade(sd);

        color += transmit * curColor.rgb;

        // prepare next ray
        bool isTransmittive = (curColor.a * transmit > TRANSMIT_EPSILON);
        bool isReflective = dot(sd.specular, 0.33) * transmit > REFLECT_EPSILON && sd.linearRoughness < 0.02;
        const float3 reflected = reflect(ray.Direction, sd.N);
        
        if(isTransmittive) // transmit ray
        {
            if(isReflective) // envmap lookup for reflection
                color += transmit * sd.specular * gScene.envMap.eval(reflected);
        
            transmit *= curColor.a;
            const float3 refracted = sd.IoR == 1.0 ? ray.Direction : refract(ray.Direction, sd.N, sd.eta);
            footprint.bounceOnSurface(v, triangleVertices, hit, sd.N, ray.Direction, refracted, true);
            ray.Direction = refracted;
            if(USE_REFRACTION_CORRECTION) {
                ray.Origin = sd.computeNewRayOrigin(false) - refracted * trianglePayload.depth;
                ray.TMin = trianglePayload.depth;
            } else {
                ray.Origin = sd.computeNewRayOrigin(false);
                ray.TMin = 0.0;
            }
        }
        else if(isReflective) // reflect ray
        {
            transmit *= dot(sd.specular, 0.33);
            footprint.bounceOnSurface(v, triangleVertices, hit, sd.N, ray.Direction, reflected, false);
            ray.Direction = reflected;
            if(USE_REFLECTION_CORRECTION) {
                ray.Origin = sd.computeNewRayOrigin() - reflected * trianglePayload.depth;
                ray.TMin = trianglePayload.depth;
            } else {
                ray.Origin = sd.computeNewRayOrigin();
                ray.TMin = 0.0;
            }
        }
        else transmit = 0.0; // stop tracing rays
    } while(++iteration < 256 && transmit > TRANSMIT_EPSILON);

    if(transmit > TRANSMIT_EPSILON) // sample environment map
    {
        color += transmit * gScene.envMap.eval(ray.Direction);
    }

    gOutputColor[launchIndex] = float4(color, 1);
}
