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
import Utils.Geometry.GeometryHelpers;
import Utils.Color.ColorHelpers;
import Rendering.Lights.LightHelpers;
import Rendering.Utils.PixelStats;


#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

/** Payload for shadow ray.
*/
struct RayData
{
    float distance;
};

cbuffer CB
{
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    float4x4    gViewProj;     // View Projection for current light
    float4x4    gInvViewProj;     // View Projection for current light
    uint        gSMSize;
}

RWTexture2D<uint> gReverseSM;
Texture2D<SM_FORMAT> gShadowMap;

Texture2D<float> gUseRayMask;
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

// Texture Sampler
SamplerState gShadowSamplerPoint;
SamplerState gShadowSamplerLinear;

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/

ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    VertexData v = {};
    uint materialID = {};

    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    return sd;
}

//Return UV in shadow map space
float2 toLightSpace(float3 pos){
    float4 shadowPos = mul(gViewProj, float4(pos, 1));
    shadowPos.xyz /= shadowPos.w;
    shadowPos.y *= -1;  //Flip
    shadowPos.xy = shadowPos.xy * 0.5 + 0.5; //UV from [-1,1] to [0,1]
    
    return shadowPos.xy;
}

//Shaders

[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.distance = -1;
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    
    if (true)
    {
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        const uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
    
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.distance = RayTCurrent();
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    HitInfo hit = HitInfo(gVBuffer[pixel]);
    const float3 primaryRayDir = -gViewW[pixel].xyz;

    if(hit.isValid()){
        // Pixel represents a valid primary hit. Compute its contribution.
        let lod = ExplicitLodTextureSampler(0.f);
        // Load shading data.
        ShadingData sd = loadShadingData(hit, primaryRayDir, lod);

        // Compute ray origin for new rays spawned from the G-buffer.
        float3 rayOrigin = sd.computeNewRayOrigin();
        //Check if ray is needed
        float2 originUV = toLightSpace(rayOrigin);
       
        if(any(originUV.xy < 0.0) || any(originUV.xy > 1.0))
            return;  

        uint2 originPixel = uint2(gSMSize * originUV);

        float smDepth = gShadowMap.SampleLevel(gShadowSamplerPoint, originUV,0).x; //Linear Depth
        float mask = gUseRayMask.SampleLevel(gShadowSamplerLinear, originUV, 0).x;
        if(mask <= 0)
            return;

        float smWorldDist = (smDepth * (gFar - gNear)) + gNear; //Linear Depth to distance
        float3 toLight = gLightPos - rayOrigin;
        float distToLight = length(toLight);
        toLight /= distToLight;
        distToLight -= smWorldDist * 1.05; //Shorten distance by the distance from the light to the first hit point

        if (distToLight <= 0)
            return;

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = toLight;
        ray.TMin = distToLight * 0.01;
        ray.TMax = distToLight * 0.99;

        RayData rayData;
        rayData.distance = 0.0;
        uint flags = RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
        TraceRay(gScene.rtAccel, flags, 0xff, 0 , rayTypeCount, 0 , ray, rayData);

        if(rayData.distance < 0)
            return;

        float3 hitPos = rayOrigin + toLight * rayData.distance;
        float lightDist = distance(gLightPos,hitPos);
        lightDist = (lightDist - gNear) / (gFar - gNear);

        uint uLightDist = asuint(lightDist);
        uint origMin;
        InterlockedMax(gReverseSM[originPixel], uLightDist, origMin);
    }
    
}
