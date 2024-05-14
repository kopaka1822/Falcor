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

/** Minimal path tracer.

    The purpose is to use it for validation of more complex renderers.
    The implementation here should be kept as simple/naive as possible.

    At each hit point (including the primary hit loaded from the V-buffer),
    analytic light sources (point, directional) are sampled uniformly using
    1 shadow ray, and 1 scatter ray is traced to sample the hemisphere.
    At hit/miss the scatter ray includes light from emissive surface and
    the environment map, respectively. Traversal stops at a fixed path length.

    Each type of light (analytic, emissive, env map) can be individually
    enabled/disabled from the host. This clutters the code a bit, but it is
    important as not all other renderes may support all three light types.

    The host sets the following defines:

    MAX_BOUNCES             Maximum number of indirect bounces (0 means no indirect).
    COMPUTE_DIRECT          Nonzero if direct illumination should be included.
    USE_IMPORTANCE_SAMPLING Nonzero if importance sampling should be used for sampling materials.
    USE_ANALYTIC_LIGHTS     Nonzero if Falcor's analytic lights should be used.
    USE_EMISSIVE_LIGHTS     Nonzero if emissive geometry should be used as lights.
    USE_ENV_LIGHT           Nonzero if env map is available and should be used as light source.
    USE_ENV_BACKGROUND      Nonzero if env map is available and should be used as background.
    is_valid_<name>         1 if optional I/O buffer with this name should be used.
*/

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Rendering.Materials.TexLODHelpers;
import Rendering.Materials.TexLODTypes;

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
    uint        gSamples;       //Number of samples per shadow map  
    bool        gUseMinMaxSM;      //Use the min max shadow map
    float4x4    gViewProj;     // View Projection for current light
    float4x4    gInvViewProj;     // View Projection for current light
}

RWTexture2D<float2> gShadowMap;

//Shaders

[shader("miss")]
void miss(inout RayData rayData)
{
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
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount * 3 + 64);

    float2 shadowValue = gUseMinMaxSM ? float2(1 , 0) : float2(0);
    float weight = (1.0 / gSamples);

    for (uint i = 0; i < gSamples; i++) {
        float2 random = sampleNext2D(sg);
        if (gSamples == 1)
            random = float2(0.5);

        float2 pixelSample = pixel + random;

        float2 relFramePos = pixelSample / frameDim;
        relFramePos = relFramePos * 2.f - 1.f; // From [0,1] to [-1,1]
        relFramePos.y *= -1.f;

        RayDesc ray;

        float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
        float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

        posWPixelNear.xyz /= posWPixelNear.w;
        float3 toPoint = posWPixelNear.xyz - gLightPos;

        ray.TMin = length(toPoint);                                        // Real near for the pixel
        ray.TMax = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
        ray.Origin = gLightPos;
        ray.Direction = normalize(toPoint); // World direction

        RayData rayData;
        rayData.depth = 1.0;

        TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        if (gUseMinMaxSM) {
            shadowValue = float2(min(shadowValue.x, rayData.depth), max(shadowValue.y, rayData.depth));
        }
        else {
            shadowValue += float2(rayData.depth, rayData.depth * rayData.depth) * weight;
        }
        
    }

    gShadowMap[pixel] = shadowValue;
}
