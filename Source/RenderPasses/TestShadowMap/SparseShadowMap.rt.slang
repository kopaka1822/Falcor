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
    float depth;
};

cbuffer CB
{
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    uint        gSMSize;
    float       gOverlap;
    float4x4    gViewProj;     // View Projection for current light
    float4x4    gInvViewProj;     // View Projection for current light
   
}

Texture2D<SM_FORMAT> gShadowMap;
RWTexture2D<float2> gVirtualLayers;

Texture2D<uint> gLayersNeededMin;
Texture2D<uint> gLayersNeededMax;

//Texture Sampler
SamplerState gShadowSamplerPoint;
SamplerState gShadowSamplerLinear;

[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.depth = -1;
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
    float3 posW = WorldRayOrigin() + WorldRayDirection() * RayTCurrent(); //Get the current world positon
    float4 posH = mul(gViewProj, float4(posW, 1.f));
    rayData.depth = (posH.w - gNear) / (gFar - gNear);   //Linear depth in [0,1]
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    uint minLayer = gLayersNeededMin[pixel/2];
    uint maxLayer = gLayersNeededMax[pixel/2];

    float2 pixelSample = pixel + 0.5;

    float2 relFramePos = pixelSample / frameDim;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;


    float depth = gShadowMap[pixel].x;

    //TODO test if first depth is already in the needed

    //Only trace an additional depth if not in layer
    if(maxLayer >= minLayer){
        const float step = 1.0/LVSM_LAYERS;
        const float totalRange = (gFar - gNear);
        float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
        posWPixelNear.xyz /= posWPixelNear.w;
        float3 toPoint = posWPixelNear.xyz - gLightPos;
        RayDesc ray;
        ray.TMin = max(0.0, step * minLayer - gOverlap) * totalRange + gNear; 
        ray.TMax = min(1.0, step * maxLayer + gOverlap) * totalRange + gNear; ;
        ray.Origin = gLightPos;
        ray.Direction = normalize(toPoint); // World direction

        RayData rayData;
        rayData.depth = 1.0;

        TraceRay(gScene.rtAccel, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        depth = rayData.depth < 0 ? depth : min(rayData.depth,1.0);
    }   

    gVirtualLayers[pixel] = float2(depth, depth * depth);  
}
