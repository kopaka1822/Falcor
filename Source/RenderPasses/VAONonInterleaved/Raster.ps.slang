/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
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
import Scene.Camera.Camera;
import VAOData;
import Scene.RaytracingInline;
import Scene.Intersection;
import Scene.Shading;

#include "Scene/Material/MaterialDefines.slangh"

// single depth texture
#define DEPTH_MODE_SINGLE 0
// two depth textures
#define DEPTH_MODE_DUAL 1
// single depth texture + stochastic depth texture
#define DEPTH_MODE_STOCHASTIC 2

#ifndef DEPTH_MODE
#define DEPTH_MODE 1
#endif
#ifndef USE_RAYS
#define USE_RAYS true
#endif

#define HALO_RADIUS (gData.radius * 4.0)

#define NUM_DIRECTIONS 8

cbuffer StaticCB
{
    VAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

// TODO change type to float
Texture2D gDepthTex;
Texture2D gDepthTex2;
Texture2DMS<float> gsDepthTex;

Texture2D gNormalTex;
Texture2D gNoiseTex;

static int gRaysTraced = 0;

float2 getScreenClampedUV(float2 uvstart, float2 uvend)
{
    return saturate(uvend); // this actually does not make much of a difference but costs a little bit more...
}

float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelCoord = floor(uv * float2(width, height));
    return float2((pixelCoord.x + 0.5f) / width, (pixelCoord.y + 0.5f) / height);
}

bool isSamePixel(float2 uv1, float2 uv2)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelSize = float2(rcp(width), rcp(height));
    return all(abs(uv1 - uv2) < pixelSize);
}

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
}

float2 Rotate90(float2 vec) // CCW
{
    return float2(-vec.y, vec.x);
}

// uv: uv coordinates [0, 1]
// viewDepth: linear depth in view space (positive z)
// return: view space position (negative z)
float3 UVToViewSpace(float2 uv, float viewDepth)
{
    float2 ndc = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0; // normalized device coordinates [-1, 1]
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    return float3(ndc * viewDepth * imageScale, -viewDepth);
}

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

float makeNonZero(float value, float epsilon)
{
    float absValue = max(abs(value), epsilon);
    return value >= 0 ? absValue : -absValue;
}

// get rid of shadowing around edges
// introduce a linear falloff function that starts with 0.0 when the sample depth intersects the front sphere exactly,
// and falls of to 1.0 when it gets further away from the sphere but closer to the camera
float calcHaloVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return saturate((objectSpaceZ - sphereStart) / HALO_RADIUS)
        * (sphereStart - sphereEnd); // this adjust the visibility to the sampling (hemi-)sphere
}

float calcSphereVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    float sampleRange = max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
    return sampleRange;
}

float calcVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return calcSphereVisibility(objectSpaceZ, sphereStart, sphereEnd)
         + calcHaloVisibility(objectSpaceZ, sphereStart, sphereEnd);
}

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
        return 1.0f;

    // view space position of current pixel
    const float3 posV = UVToViewSpace(texC, linearDepth);
    const float posVLength = length(posV);

    // view space normal of current pixel
    float3 normalW = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0) // front face normals
        normalV = -normalV;

    // Calculate tangent space (use random direction for tangent orientation)
    float2 randDir = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale).xy;
    randDir = normalize(randDir); // should be normalized by default, but precision is lost in texture format
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 normal = -posV / posVLength;
    float3 bitangent = normalize(cross(normal, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, normal);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, normal));
    
    float visibility = 0.0f;

    for (uint i = 0; i < NUM_DIRECTIONS; i++)
    {
        // obtain sample position on disc around view space position
        float2 rand = gData.samples[i].xy; // xy = random location on unit disc
        rand.xy *= gData.radius; // multiply 2D position with sample radius
        
        // height of the sphere at the requested sample position (not at the actual sampling position)
        const float sphereHeight = sqrt(gData.radius * gData.radius - dot(rand.xy, rand.xy));
        // probability for choosing this sample
        const float pdf = 2.0 * sphereHeight;
        //float adaptiveHaloHeight = sqrt((gData.radius + HALO_RADIUS) * (gData.radius + HALO_RADIUS) - dot(rand.xy, rand.xy));

        // determine distance within [-sphereHeight, +sphereHeight]
        float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
        float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

        { // HEMISPHERE SAMPLING
            //float zIntersect = -dot(rand.xy, normalO.xy) / normalO.z;
            float zIntersect = -dot(rand.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            if (normalO.z >= 0.0)
                sphereEnd = zIntersectClamped;
            else
                sphereStart = zIntersectClamped;
        }

        // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
        if (sphereStart - sphereEnd < 0.01)
        {
            continue;
        }

        // calculate view position of sample and project to uv coordinates
        float3 initialSamplePosV = posV + tangent * rand.x + bitangent * rand.y;
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
        float curVisibility = 0.0f;

        bool hybridRay = false;

        { // RASTER PART
            // clip sample position uv and snap to pixel center
            float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
            if (any(samplePosUV != screenUv))
                hybridRay = true; // always shoot rays for screen border
            float2 rasterSamplePosUV = screenUv;
            rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center
            //if (isSamePixel(samplePosUV, texC))
                //objectSpaceZ = 0.0;            

            if (DEPTH_MODE == DEPTH_MODE_SINGLE)
            {
                float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
                // TODO? could check for depth == farZ
                float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
                float objectSpaceZ = dot(samplePosV - posV, normal);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                // require hybrid ray if intersection is outside of sphere radius
                if (objectSpaceZ > sphereStart)
                    hybridRay = true;
            }
            else if (DEPTH_MODE == DEPTH_MODE_DUAL)
            {
                float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
                float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
                float objectSpaceZ = dot(samplePosV - posV, normal);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                // look at second sample in this case
                if (objectSpaceZ > sphereStart)
                {
                    float linearSampleDepth = gDepthTex2.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
                    float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                    // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
                    float objectSpaceZ = dot(samplePosV - posV, normal);
                    curVisibility = min(curVisibility, calcVisibility(objectSpaceZ, sphereStart, sphereEnd));
                    if (objectSpaceZ > sphereStart) // both samples were before the sphere starts
                        hybridRay = true;
                }
            }
            else // DEPTH_MODE_STOCHASTIC
            {
                float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
                // TODO? could check for depth == farZ
                float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
                float objectSpaceZ = dot(samplePosV - posV, normal);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                // look at stochastic samples int this case
                if (objectSpaceZ > sphereStart)
                {
                    float width, height;
                    gDepthTex.GetDimensions(width, height);
                    // TODO try to use sampler
                    int2 pixelCoord = int2(floor(rasterSamplePosUV * float2(width, height)));

                    const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
                    const float depthOffset = gCamera.data.nearZ;
                    [unroll]
                    for (uint i = 0; i < MSAA_SAMPLES; ++i)
                    {
                        float linearSampleDepth = gsDepthTex.Load(pixelCoord, i);
                        // linearSampleDepth is in [0, 1] => scale accordingly
                        linearSampleDepth = linearSampleDepth * depthRange + depthOffset;
                        float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                        float objectSpaceZ = dot(samplePosV - posV, normal);
                        float newVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                        curVisibility = min(curVisibility, newVisibility);
                    }
                    // TODO when to trace hybrid rays?
                }
            }
        }

        if (USE_RAYS && hybridRay)
        {
            gRaysTraced += 1;
            
            // to be consistent with the rasterizer, we snap the uv coordinate as well to the pixel center,
            // but we do not clip it since we can shoot outside of the screen space
            //samplePosUV = getScreenClampedUV(texC, samplePosUV);
            samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center
            
            float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
            float initialSamplePosLength = length(initialSamplePosV);
            
            RayDesc ray;
            ray.Origin = gCamera.data.posW; // offset a little bit in normal direction
            ray.Direction = mul(sampleDirV, float3x3(invViewMat));

            ray.TMin = 0.0; // TODO set to depth buffer value
            float tSphereStart = (posVLength - sphereStart) * initialSamplePosLength / posVLength;
            float tSphereEnd = (posVLength - sphereEnd) * initialSamplePosLength / posVLength;
            float tHaloStart = (posVLength - sphereHeight - HALO_RADIUS) * initialSamplePosLength / posVLength;
            ray.TMin = max(tHaloStart, 0.0);
            ray.TMax = tSphereEnd;

            // skip procedural and force all triangle to be handled by any-hit traversal
            RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE> rayQuery;
            rayQuery.TraceRayInline(gScene.rtAccel, RAY_FLAG_NONE, 0xff, ray);

            // stack that gets increased for front faces, and decreased for back faces => 0 = unoccluded, > 0 = occluded
            float tFirstFrontFaceInside = tSphereEnd;
            float tLastFrontFaceHalo = tHaloStart;

            while(rayQuery.Proceed())
            {
                if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                {
                    // extract hit properties
                    float t = rayQuery.CandidateTriangleRayT();
                    bool frontFace = rayQuery.CandidateTriangleFrontFace();
                    const TriangleHit hit = getCandidateTriangleHit(rayQuery);
                    const uint materialID = gScene.getMaterialID(hit.instanceID);
                    const MaterialData md = gScene.materials[materialID];
                    bool isAlphaTested = EXTRACT_ALPHA_MODE(md.flags) == AlphaModeMask;

                    // needs alpha testing?
                    if (isAlphaTested)
                    {
                        const VertexData v = gScene.getVertexData(hit);
                        if (alphaTest(v, md, gScene.materialResources[materialID], 0.f))    
                            continue; // alpha test failed => ignore this triangle
                    }
                    bool isDoubleSided = EXTRACT_DOUBLE_SIDED(md.flags);
                    frontFace = frontFace || isAlphaTested || isDoubleSided;

                    if (frontFace)
                    {
                        if (t <= tSphereStart)
                            tLastFrontFaceHalo = max(tLastFrontFaceHalo, t);
                        else
                        {
                            tFirstFrontFaceInside = min(tFirstFrontFaceInside, t);
                            rayQuery.CommitNonOpaqueTriangleHit(); // since we save the min, we can commit here
                        }
                    }   
                }
            }
            // calculate object z fron first front face hit
            float projectedHitT = tFirstFrontFaceInside * posVLength / initialSamplePosLength;
            float sphereZ = posVLength - projectedHitT;
            //projectedHitT = max(tLastFrontFaceHalo, tLastBackFaceHalo) * posVLength / initialSamplePosLength;
            projectedHitT = tLastFrontFaceHalo * posVLength / initialSamplePosLength;
            float haloZ = posVLength - projectedHitT;

            float sphereVisibility = calcSphereVisibility(sphereZ, sphereStart, sphereEnd);
            float haloVisibility = calcHaloVisibility(haloZ, sphereStart, sphereEnd);

            curVisibility =  min(sphereVisibility, haloVisibility); // divide sampled range by pdf (monte carlo)
        }

        visibility += curVisibility / pdf;
    }

    float AO = visibility / float(NUM_DIRECTIONS);
    // since fully visibile are all values in [0.5, 1.0], scale accordingly
    AO = saturate(AO * 2.0);
    // do artistic modifications
    AO = pow(AO, gData.exponent);

    return AO;
}
