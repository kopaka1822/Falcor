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
import Rendering.Lights.LightHelpers;
import Rendering.Materials.TexLODHelpers;
import Rendering.Materials.TexLODTypes;
import Rendering.ShadowMaps.ShadowTestFunctions;
import Rendering.ShadowMaps.ShadowMapHelpers;

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
    uint        gFrameCount;   // Frame count since scene was loaded.
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    float4x4    gViewProj;     // View Projection for current light
    float4x4    gInvViewProj;     // View Projection for current light
    bool        gCalcNearFar;   //Used for nearFar calculations
}

RWTexture2D<SM_FORMAT> gRayShadowMap;
RWTexture2D<float2> gRayShadowMapMinMax; //Min Max optimizer shadow map. Else nullptr

//Shaders

[shader("miss")]
void miss(inout RayData rayData)
{
    if (gCalcNearFar)
        rayData.depth = -1.0; //Set to invalid value
    else
        rayData.depth = 1.0;
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
    if(gCalcNearFar)
        rayData.depth = posH.w;
    else
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

    float2 pixelSample = pixel + 0.5;

    float2 relFramePos = pixelSample / frameDim;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

    RayDesc ray;

    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toPoint = posWPixelNear.xyz - gLightPos;

    ray.TMin = gCalcNearFar ? 0.0 : length(toPoint);                                        // Real near for the pixel
    ray.TMax = gCalcNearFar ? FLT_MAX : distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
    ray.Origin = gLightPos;
    ray.Direction = normalize(toPoint); // World direction

    RayData rayData;
    rayData.depth = 1.0;

    TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

   
    if(gCalcNearFar){
         gRayShadowMapMinMax[pixel] = float2(rayData.depth);
         return;
    }
    rayData.depth = saturate(rayData.depth);
    SM_FORMAT shadowValue = {};
    #if FILTER_SM_VARIANCE || FILTER_SM_LAYERED_VARIANCE
        shadowValue = float2(rayData.depth,rayData.depth * rayData.depth);
    #elif FILTER_SM_ESVM
        float rescaledDepth = (2.f * rayData.depth) - 1.f;
        shadowValue = recreateEVSMFromSingleDepth(rescaledDepth);
    #elif FILTER_SM_MSM
        const float d = rayData.depth;
        shadowValue = float4(d, d*d, d*d*d, d*d*d*d);
    #endif
   

    gRayShadowMap[pixel] = shadowValue;
}
