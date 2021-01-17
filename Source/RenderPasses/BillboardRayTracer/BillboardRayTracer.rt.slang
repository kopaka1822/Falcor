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

import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Scene.Lights.LightData;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;
import Scene.TextureSampler;
import Scene.Material.MaterialData;
import Scene.HitInfo;
import RenderPasses.Shared.PathTracer.RayFootprintData;
import Scene.Material.BRDF;

// early out settings
#define TRANSMIT_EPSILON 0.001
#define REFLECT_EPSILON 0.001
#define SHADOW_MIN 0.2
#define AMBIENT_LIGHT 0.3

cbuffer CB
{
    uint gFrameCount; // Frame count since scene was loaded.
}

// Outputs
RWTexture2D<float4> gOutputColor;

// Default ray payload
struct RayPayload
{
    PackedHitInfo hit; // instanceID, primitiveIndex and compressed barycentrics
    float depth; // rayTCurrent of intersection
};

// Payload for spherical billboards
struct BillboardPayload
{
    float3 colorSum; // weighted-blended OIT data
    float alphaSum; // weighted-blended OIT data
    float transmit; // total transmittance (visibility)
    float rayTMax; // ray T to next triangle
    RayFootprint footprint; // ray differentials for texture filtering
};


#include "Helper.slang" // contains general helper function to retrieve billboard vertex data, billboard tangent plane and shading related data
#include "Shadow.slang" // contains the trace shadow ray function (should be replaced with shadow maps for real-time)
#include "RayDifferentials.slang" // contains helper functions to modify the ray differentials and filtered shading textures


/****** Triangle Hit Shaders ******/

[shader("miss")]
void miss(inout RayPayload rayData : SV_RayPayload) {}

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
    // store hit information (shading is done in ray generation shader)
    rayData.depth = RayTCurrent();
    HitInfo hit;
    hit.type = InstanceType::TriangleMesh;
    hit.instanceID = hitParams.getGlobalHitID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    rayData.hit = hit.encode();
}


/****** Billboard Hit Shaders ******/
struct BoxIntersectionAttributes
{
    int aabbOffset;
};

[shader("intersection")]
void boxIntersect()
{
    const uint aabbOffset = gScene.proceduralPrimitives[GeometryIndex()].AABBOffset;
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy; // w = dim.x, h = dim.y
    const float3 P = bbox.center();

    switch(BILLBOARD_TYPE)
    {
    case BILLBOARD_TYPE_IMPOSTOR:
    case BILLBOARD_TYPE_PARTICLE: {
        const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;
        const bool particle = BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE;
    
        // billboard plane base
        const float3x3 planeBase = GetBillboardTangentSpace(normalize(WorldRayOrigin() - P), yAlign);
        // ray origin and direction in plane space
        const float3 dp = mul(planeBase, WorldRayDirection());
        const float3 Op = mul(planeBase, WorldRayOrigin() - P);
    
        // ray intersection parameter:
	    // Op + s * dp = (Xp.x, Xp.y, 0) with Xp = intersection point of plane
        const float s = -Op.z / dp.z;
        if(s <= RayTMin()) return; // use <= instead of < to avoid self intersection when casting a new ray (because we set ray.TMin = s before casting a new ray)
        const float2 Xp = Op.xy + s * dp.xy;
    
        // check if on plane
        if(any(Xp < -0.5 * dim) || any(Xp > 0.5 * dim)) return;
        // more general formula for particle test that also works with elliptical particles (dim.x/2 == r != dim.y/2)
	    if(particle && (4.0 * Xp.x * Xp.x / (dim.x * dim.x) + 4.0 * Xp.y  * Xp.y  / (dim.y * dim.y) > 1.0)) return;
    
        BoxIntersectionAttributes attribs;
        attribs.aabbOffset = aabbOffset;
        ReportHit(s, 0, attribs);
    
    } break;
    case BILLBOARD_TYPE_SPHERICAL: {
        const float r = 0.5 * dim.x;
        const float t = dot(WorldRayDirection(), P - WorldRayOrigin());
        const float l = length(WorldRayOrigin() - P + t * WorldRayDirection());
        if(l >= r) return;
        const float w = sqrt(r * r - l * l);
        const float rayTIn = max(RayTMin(), t - w);
        const float ds = min(/*TMax*/ RayTCurrent(), t + w) - rayTIn;
        if(ds <= 0.0) return;

        BoxIntersectionAttributes attribs;
        attribs.aabbOffset = aabbOffset;
        ReportHit(rayTIn, 0, attribs);
    } break;
    }
}

[shader("anyhit")]
void boxAnyHit(inout BillboardPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
// only used for sphericalbillboards
#if BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL
    // get vertex data, transfer ray differentials and obtain shading data
    VertexData v = getSphericalBillboardVertexData(attribs.aabbOffset, WorldRayOrigin(), WorldRayDirection(), RayTCurrent(), RayTMin(), rayData.rayTMax);
    RayFootprint volumeFootprint = rayData.footprint; // copy footprint and transfer to hit surface
    const AABB bbox = gScene.proceduralPrimitiveAABBs[attribs.aabbOffset];
    volumeFootprint.hitSphere(/*reconstruct original ray origin*/ WorldRayOrigin() + RayTMin() * WorldRayDirection(), WorldRayDirection(), bbox.center(), 0.5 * bbox.extent().x); // transfer to surface
    ShadingData sd = prepareBillboardShadingData(v, volumeFootprint, attribs.aabbOffset, WorldRayOrigin(), WorldRayDirection());
    float4 curColor = shade(sd, BILLBOARD_SHADOW_SAMPLES > 1, v.billboardDepth);

    // do color accumulation as in weighted-blended OIT
    float alpha = 1.0 - curColor.a;
    float weight = getDepthWeight(alpha, RayTCurrent());
    rayData.colorSum += curColor.rgb * weight;
    rayData.alphaSum += alpha * weight;
    rayData.transmit *= curColor.a; // curColor.a = transmittance

    if(curColor.a == 0.0) return; // opaque object => abort (hopefully ignores most of the objects behind this one)
    IgnoreHit(); // do not accept the hit in general => keep any-hit traversal going
#endif
}

[shader("closesthit")]
void boxClosestHit(inout RayPayload rayData : SV_RayPayload, BoxIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // this shader is skipped for BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL
    // store hit information in ray payload
    rayData.depth = RayTCurrent();
    HitInfo hit;
    hit.type = InstanceType::Curve;
    hit.instanceID = 0;
    hit.primitiveIndex = attribs.aabbOffset; // this is the only relevant data for now
    hit.barycentrics = 0.0;
    rayData.hit = hit.encode();
}

/****** Ray Kernel for Bilboards ******/

#if BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL

void traceBillboards(inout float3 color, inout float transmit, RayDesc ray, RayFootprint footprint)
{
    // trace single ray and handle everything in any-hit shaders
    BillboardPayload volumePayload;
    volumePayload.footprint = footprint;
    volumePayload.rayTMax = ray.TMax;
    volumePayload.colorSum = 0.0;
    volumePayload.alphaSum = 0.0;
    volumePayload.transmit = 1.0;

    TraceRay(gRtScene, RAY_FLAG_SKIP_TRIANGLES | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, hitProgramCount, 0 /* missIdx */, ray, volumePayload);

    // use weighted-blended OIT to calculate the color
    if(volumePayload.transmit < 1.0) {
        color += transmit * (volumePayload.colorSum / volumePayload.alphaSum) * (1.0 - volumePayload.transmit);
        transmit *= volumePayload.transmit;
    }
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

        if(!volumeHit.decode(volumePayload.hit)) break; // no more billboard intersections

        VertexData v = getBillboardVertexData(volumeHit.primitiveIndex, ray.Origin, ray.Direction, volumePayload.depth, prevRayMin, ray.TMax);

        // transfer ray differentials and get shading data
        float travelledDistance = volumePayload.depth - prevRayMin;
        RayFootprint volumeFootprint = footprint; // copy footprint and transfer to hit surface
        volumeFootprint.hitSurface(v, travelledDistance, ray.Direction); // transfer to surface
        ShadingData sd = prepareBillboardShadingData(v, volumeFootprint, volumeHit.primitiveIndex, ray.Origin, ray.Direction);
        //float4 curColor = shade(sd); // rgb = (pre-multiplied) color, a = transmittance (1 - alpha)
        float4 curColor = shade(sd, BILLBOARD_SHADOW_SAMPLES > 1, v.billboardDepth, v.billboardDepth * 0.5);


        color += transmit * curColor.rgb; // update color
        transmit *= curColor.a; // update visibility

        // prepare next ray
        ray.TMin = volumePayload.depth;
    } while(++iteration < 64 && transmit > TRANSMIT_EPSILON);
}

#endif

/****** Ray Generation ******/
[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    RayDesc ray = gScene.camera.computeRayPinhole(launchIndex, launchDim).toRayDesc();
    ray.TMin = 0.0; // set to 0 to make handling with ray differentials and origin correction somewhat easier
    RayFootprint footprint = RayFootprint.create(launchIndex, launchDim); // init ray differential

    float3 color = 0.0; // color sum
    float transmit = 1.0; // current visibility

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
        traceBillboards(color, transmit, ray, footprint); // updates color and transmittance based on encountered billboards

        // evaluate triangle geometry (shading was deferred to ray generation shader)
        HitInfo hit;
        if(transmit <= TRANSMIT_EPSILON || !hit.decode(trianglePayload.hit)) break;
      
        // unpack hit data 
        StaticVertexData triangleVertices[3];
        VertexData v = gScene.getVertexData(hit, triangleVertices);
        const uint materialID = gScene.getMaterialID(hit.instanceID);

        // transfer ray differentials and get shading data:
        // compute the travelled distance: When the ray origin correction is used, the ray distance must be subtracted from ray.TMin
        const float travelledDistance = trianglePayload.depth - ray.TMin;
        footprint.hitSurface(v, travelledDistance, ray.Direction); // transfer ray differentials onto surface
        ShadingData sd = prepareShadingData(v, footprint, triangleVertices, hit, travelledDistance, ray.Direction);
        float4 curColor = shade(sd); // rgb = (pre-multiplied) color, a = transmittance (1 - alpha)

        color += transmit * curColor.rgb;

        // calc fresnel to see how much gets reflected/refracted
        const float3 reflected = reflect(ray.Direction, sd.N); // reflected ray direction
        const float3 F = fresnelSchlick(sd.specular, 1, saturate(dot(reflected, sd.N)));
        curColor.a *= (1.0 - dot(F, 0.33)); // adjust fragment transmittance with fresnel
        // prepare next ray
        bool isTransmittive = (curColor.a * transmit > TRANSMIT_EPSILON);
        bool isInternalReflection = sd.eta > 1.0 && sd.eta * sd.eta * (1.0 - dot(sd.N, sd.V) * dot(sd.N, sd.V)) >= 1.0;
        bool isReflective = dot(F, 0.33) * transmit > REFLECT_EPSILON && sd.linearRoughness < 0.02;

        if(isTransmittive) // prefer transmission over reflections
        {
            if(isReflective) // envmap lookup for reflection
                color += F * transmit * gScene.envMap.eval(reflected);
        
            transmit *= curColor.a; // update visibility
            float3 refracted = ray.Direction;
            if(sd.IoR != 1.0 && !isInternalReflection) // workaround for internal reflection that looks good and is not too expensive (avoids back and forth bounces)
                refracted = refract(ray.Direction, sd.N, sd.eta);
            // update ray differentials
            footprint.bounceOnSurface(v, triangleVertices, hit, sd.N, ray.Direction, refracted, true);
            ray.Direction = refracted;
            if(USE_REFRACTION_CORRECTION) { // apply ray origin correction for billboards
                ray.Origin = sd.computeNewRayOrigin(false) - refracted * trianglePayload.depth;
                ray.TMin = trianglePayload.depth;
            } else { // cast new ray from intersection point
                ray.Origin = sd.computeNewRayOrigin(false);
                ray.TMin = 0.0;
            }
        }
        else if(isReflective) // reflect ray
        {
            transmit *= dot(F, 0.33); // update visibility
            // update ray differentials
            footprint.bounceOnSurface(v, triangleVertices, hit, sd.N, ray.Direction, reflected, false);
            ray.Direction = reflected;
            if(USE_REFLECTION_CORRECTION) { // apply ray origin correction for billboards
                ray.Origin = sd.computeNewRayOrigin() - reflected * trianglePayload.depth;
                ray.TMin = trianglePayload.depth;
            } else { // cast new ray from intersection point
                ray.Origin = sd.computeNewRayOrigin();
                ray.TMin = 0.0;
            }
        }
        else transmit = 0.0; // stop tracing rays
    } while(++iteration < 16 && transmit > TRANSMIT_EPSILON);

    if(transmit > TRANSMIT_EPSILON) // sample environment map (miss)
    {
        color += transmit * gScene.envMap.eval(ray.Direction);
    }

    gOutputColor[launchIndex] = float4(color, 1);
}
