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

// early out settings
#define TRANSMIT_EPSILON 0.001
#define REFLECT_EPSILON 0.001

struct BoxIntersectionAttributes
{
    int aabbOffset;
};

#include "Helper.slang"
#include "Shadow.slang"

cbuffer CB
{
    uint        gFrameCount;        // Frame count since scene was loaded.
}

// Outputs
RWTexture2D<float4> gOutputColor;

struct RayPayload
{
    PackedHitInfo hit;
    float depth;
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
void boxIntersect()
{
    const uint aabbOffset = gScene.proceduralPrimitives[GeometryIndex()].AABBOffset;
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 P = bbox.center();

    switch(BILLBOARD_TYPE)
    {
    case BILLBOARD_TYPE_IMPOSTOR:
    case BILLBOARD_TYPE_PARTICLE: {
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
    case BILLBOARD_TYPE_SPHERICAL: {
        const float r = 0.5 * dim.x;
        const float rayT = dot(WorldRayDirection(), P - WorldRayOrigin());
        const float d = length(WorldRayOrigin() + rayT * WorldRayDirection() - P);
        if(d >= r) return;
        const float w = sqrt(r * r - d * d);
        const float rayTIn = max(RayTMin(), rayT - w);
        const float ds = min(/*TMax*/ RayTCurrent(), rayT + w) - rayTIn;
        if(ds <= 0.0) return;

        BoxIntersectionAttributes attribs;
        attribs.aabbOffset = aabbOffset;
        ReportHit(rayTIn, 0, attribs);
    } break;
    }
}

[shader("anyhit")]
void boxAnyHit(inout VolumePayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
#if BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL
    // do color computation here
    VolumeNode n;
    
    VertexData v = getSphericalBillboardVertexData(attribs.aabbOffset, WorldRayOrigin(), WorldRayDirection(), RayTCurrent(), RayTMin(), rayData.rayTMax);

    RayFootprint volumeFootprint = rayData.footprint; // copy footprint and transfer to hit surface
    volumeFootprint.hitSurface(v, /*reconstruct original ray origin*/ WorldRayOrigin() + RayTMin() * WorldRayDirection(), WorldRayDirection()); // transfer to surface
    ShadingData sd = prepareBillboardShadingData(v, volumeFootprint, attribs.aabbOffset, WorldRayOrigin(), WorldRayDirection());
    float4 curColor = shade(sd, BILLBOARD_DEPTH_SHADOWS, WorldRayOrigin() + (RayTCurrent() + v.billboardDepth) * WorldRayDirection());
    n.color = curColor.rgb;
    n.transmit = curColor.a;
    if(curColor.a > 0.999) IgnoreHit(); // invisible
    n.depthStart = RayTCurrent();
    n.depthEnd = RayTCurrent() + v.billboardDepth;

    if(rayData.node.IsEmpty()) rayData.node = n; // simply replace node
    else rayData.node = VolumeNode.MergeVolumes(rayData.node, n);
    
    IgnoreHit();
    // TODO accept ray interval if occlusion too high?
#endif
}

[shader("closesthit")]
void boxClosestHit(inout RayPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // this shader is skipped for BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL
    rayData.depth = RayTCurrent();
    HitInfo hit;
    hit.type = InstanceType::Curve;
    hit.instanceID = 0;
    hit.primitiveIndex = attribs.aabbOffset;
    hit.barycentrics = 0.0;
    rayData.hit = hit.encode();
}

#if BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL

void traceBillboards(inout float3 color, inout float transmit, RayDesc ray, RayFootprint footprint)
{
    // trace single ray and handle everything in any-hit shaders
    VolumePayload volumePayload;
    volumePayload.footprint = footprint;
    volumePayload.rayTMax = ray.TMax;
    volumePayload.node = VolumeNode.Empty();

    TraceRay(gRtScene, RAY_FLAG_SKIP_TRIANGLES | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, volumePayload);

    color += transmit * volumePayload.node.color;
    transmit *= volumePayload.node.transmit;
}

#else // BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR || BILLBOARD_TYPE_PARTICLE

void traceBillboards(inout float3 color, inout float transmit, RayDesc ray, RayFootprint footprint)
{
    RayPayload volumePayload;
    volumePayload.depth = 0.0;
    float prevRayMin = ray.TMin; // save original ray min (ray.TMin will be modified at the end of the loop)

    uint iteration = 0;
    do // trace volumetric data
    {
        HitInfo volumeHit;
        volumePayload.hit = HitInfo::kInvalidIndex;
        TraceRay(gRtScene, RAY_FLAG_SKIP_TRIANGLES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, volumePayload);
        // handle volume data
        if(!volumeHit.decode(volumePayload.hit)) break; // finished with the volumetric part

        VertexData v = getBillboardVertexData(volumeHit.primitiveIndex, ray.Origin, ray.Direction, volumePayload.depth, prevRayMin, ray.TMax);

        RayFootprint volumeFootprint = footprint; // copy footprint and transfer to hit surface
        volumeFootprint.hitSurface(v, /*reconstruct original ray origin*/ ray.Origin + prevRayMin * ray.Direction, ray.Direction); // transfer to surface
        ShadingData sd = prepareBillboardShadingData(v, volumeFootprint, volumeHit.primitiveIndex, ray.Origin, ray.Direction);
        float4 curColor = shade(sd);

        color += transmit * curColor.rgb;
        transmit *= curColor.a;

        // prepare next ray
        ray.TMin = volumePayload.depth;
    } while(++iteration < 64 && transmit > TRANSMIT_EPSILON);
}

#endif

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
        // (only required for spherical and soft particles, could be merged with procedural path otherwise)
        RayPayload trianglePayload;
        trianglePayload.depth = FLT_MAX;
        trianglePayload.hit = HitInfo::kInvalidIndex;
        ray.TMax = FLT_MAX;
        TraceRay(gRtScene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, trianglePayload);

        // handle billboards
        ray.TMax = trianglePayload.depth; // set max range for billboard ray
        traceBillboards(color, transmit, ray, footprint);

        // evaluate triangle geometry
        HitInfo hit;
        if(transmit <= TRANSMIT_EPSILON || !hit.decode(trianglePayload.hit)) break;
      
        // unpack hit data 
        StaticVertexData triangleVertices[3];
        VertexData v = gScene.getVertexData(hit, triangleVertices);
        uint materialID = gScene.getMaterialID(hit.instanceID);

        // transfer ray differentials and get shading data
        footprint.hitSurface(v, /*reconstruct original ray origin*/ ray.Origin + ray.TMin * ray.Direction, ray.Direction);
        ShadingData sd = prepareShadingData(v, footprint, triangleVertices, hit, /*reconstruct original ray origin*/ ray.Origin + ray.TMin * ray.Direction, ray.Direction);
        float4 curColor = shade(sd);

        color += transmit * curColor.rgb;

        // prepare next ray
        bool isTransmittive = (curColor.a * transmit > TRANSMIT_EPSILON);
        bool isInternalReflection = sd.eta > 1.0 && sd.eta * sd.eta * (1.0 - dot(sd.N, sd.V) * dot(sd.N, sd.V)) >= 1.0;
        bool isReflective = dot(sd.specular, 0.33) * transmit > REFLECT_EPSILON && sd.linearRoughness < 0.02;
        const float3 reflected = reflect(ray.Direction, sd.N);

        if(isTransmittive) // transmit ray
        {
            if(isReflective) // envmap lookup for reflection
                color += transmit * sd.specular * gScene.envMap.eval(reflected);
        
            transmit *= curColor.a;
            float3 refracted = ray.Direction;
            if(sd.IoR != 1.0 && !isInternalReflection) // workaround for internal reflection that looks good and is not too expensive (avoids back and forth bounces)
                refracted = refract(ray.Direction, sd.N, sd.eta);
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
    } while(++iteration < 16 && transmit > TRANSMIT_EPSILON);

    if(transmit > TRANSMIT_EPSILON) // sample environment map
    {
        color += transmit * gScene.envMap.eval(ray.Direction);
    }

    gOutputColor[launchIndex] = float4(color, 1);
}
