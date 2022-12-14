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
import SSAOData;
import Scene.RaytracingInline;
import Scene.Intersection;
import Scene.Shading;
import Scene.Raster;

//#include "Scene/Material/MaterialDefines.slangh"

#ifndef NUM_STEPS
#define NUM_STEPS 4
#endif
#ifndef NUM_DIRECTIONS
#define NUM_DIRECTIONS 8
#endif

// single depth texture
#define DEPTH_MODE_SINGLE 0
// two depth textures
#define DEPTH_MODE_DUAL 1
// single depth texture + stochastic depth texture
#define DEPTH_MODE_STOCHASTIC 2
// single depth texture + ray tracing
#define DEPTH_MODE_RAYTRACED 3

#ifndef DEPTH_MODE
#error please define DEPTH_MODE
#endif

#define COMPENSATE_STOCHASTIC_SAMPLES true

#define PREVENT_DARK_HALOS 1
// full radius of the halo
//#define HALO_RADIUS (gData.radius * 3.0)
#define HALO_RADIUS (sphereStart - sphereEnd)
// area where the halo effect remains constant at 0.0
#if PREVENT_DARK_HALOS
//#define CONST_RADIUS (gData.radius * 0.5)
#define CONST_RADIUS ((1.0 + gData.thickness) * gData.radius - sphereStart)
#else
#define CONST_RADIUS 0.0
#endif

//#define ENABLED(value) (value != 0)

cbuffer StaticCB
{
    SSAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
    uint frameIndex;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

// inputs
Texture2D<float> gDepthTex;
Texture2D<float> gDepthTex2;
Texture2DMS<float> gsDepthTex;
Texture2D<uint> gInstanceID;

Texture2D gNormalTex;
Texture2D<float> gNoiseTex;

// outputs for ML
RWTexture2DArray<float> gRasterDepth;
RWTexture2DArray<float> gRayDepth;
RWTexture2DArray<uint> gInstanceIDOut;



static int gRaysTraced = 0;
static int gInvalid = 0;

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

int2 UVToPixel(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    return int2(floor(uv * float2(width, height)));
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
    if (!PREVENT_DARK_HALOS)
        return 0.0;
    
    return saturate((objectSpaceZ - sphereStart - CONST_RADIUS) / HALO_RADIUS)
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

float calcObjectSpaceZ(float3 posV, float3 normal, float2 uv, Texture2D<float> depthTex)
{
    float linearSampleDepth = depthTex.SampleLevel(gTextureSampler, uv, 0);
    float3 samplePosV = UVToViewSpace(uv, linearSampleDepth);
            // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
    float objectSpaceZ = dot(samplePosV - posV, normal);
    return objectSpaceZ;
}

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);
    if (linearDepth >= gCamera.data.farZ * 0.99)
        return 1.0f;
    
    // view space position of current pixel
    float3 posV = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 normalW = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0) // front face normals
        normalV = -normalV;

    // obtain current pixels XY coordinate          
    float width, height;
    gDepthTex.GetDimensions(width, height);
    const int2 XY = UVToPixel(texC);
    uint curInstanceID = gInstanceID[XY];
    
    const float posVLength = length(posV);

    // Calculate tangent space (use random direction for tangent orientation)
    float randJitter = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0);
    float randRotation = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0) * 2.0 * 3.141;
    float2 randDir = float2(sin(randRotation), cos(randRotation));
    randDir = normalize(randDir); // should be normalized by default, but precision is lost in texture format
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 normal = -posV / posVLength;
    float3 bitangent = normalize(cross(normal, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, normal);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, normal));
    
    float visibility = 0.0f;
    //uint zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));
    //uint i = (zCurveIndex + (frameIndex)) % KERNEL_SIZE; // JenkinsHash
    [unroll] for (uint i = 0; i < NUM_DIRECTIONS; ++i)
    {
        float angle = (2.0 * 3.141 * i) / NUM_DIRECTIONS;
        float2 direction = float2(sin(angle), cos(angle)); // TODO use the random direction from the noise field?


        [unroll] for (int step = 0; step < NUM_STEPS; ++step)
        {
            float curVisibility = 1.0f;
            
            // calc 's' sampling pos
            float2 s = direction * (step + 0.5 + randJitter) / (NUM_STEPS + 1); // TODO add jitter
            
            s *= gData.radius; // multiply 2D position with sample radius
        
            // height of the sphere at the requested sample position (not at the actual sampling position)
            const float sphereHeight = sqrt(gData.radius * gData.radius - dot(s, s));
            // probability for choosing this sample
            const float pdf = 2.0 * sphereHeight;
            //float adaptiveHaloHeight = sqrt((gData.radius + HALO_RADIUS) * (gData.radius + HALO_RADIUS) - dot(rand.xy, rand.xy));

            // determine distance within [-sphereHeight, +sphereHeight]
            float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
            float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

            float zIntersect = -dot(s, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            sphereEnd = zIntersectClamped;

            // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
            if (sphereStart - sphereEnd < 0.01)
            {
                gInvalid += 1;
                visibility += 1.0;
                gInstanceIDOut[uint3(XY, i)] = curInstanceID;
                continue;
            }

            // calculate view position of sample and project to uv coordinates
            float3 initialSamplePosV = posV + tangent * s.x + bitangent * s.y;
            float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);

            // clip sample position uv and snap to pixel center
            float2 screenUv = saturate(samplePosUV); // clip to screen border
            const bool isInScreen = all(samplePosUV == screenUv);
            float2 rasterSamplePosUV = screenUv;

            float3 hitV = UVToViewSpace(rasterSamplePosUV, gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0.0));
            float3 wV = hitV - posV;
            float lenW = length(wV);

            float NdotW = dot(normalV, wV / lenW);
            if(lenW > gData.radius && dot(wV, -posV) < 0.0)
                NdotW = 0.0; // exit hemisphere before any occlusion => no occlusion
            if (lenW > gData.radius && dot(wV, -posV) > 0.0)
            {
                NdotW = 0.0; // TODO ray tracing   
            }
            
            visibility += (1.0 - saturate(NdotW));
            
            /*rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

            // object space z of the primary depth buffer or the secondary depth buffer if available
            float objectSpaceZ = sphereHeight + CONST_RADIUS + HALO_RADIUS;
        
            // primary depth sample
            objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex);
            curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            bool primaryIsOccluded = (objectSpaceZ > sphereStart + CONST_RADIUS);

            gRasterDepth[uint3(XY, i)] = objectSpaceZ / gData.radius;
            gRayDepth[uint3(XY, i)] = objectSpaceZ / gData.radius;
            gInstanceIDOut[uint3(XY, i)] = gInstanceID[UVToPixel(rasterSamplePosUV)];*/
        
            /*if (DEPTH_MODE == DEPTH_MODE_DUAL && primaryIsOccluded)
            {
            // obtain secondary sample if primary sample is occluded
                objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex2);
                curVisibility = min(curVisibility, calcVisibility(objectSpaceZ, sphereStart, sphereEnd));
            }
            else if (DEPTH_MODE == DEPTH_MODE_STOCHASTIC && primaryIsOccluded)
            {
            // TODO try to use sampler
                int2 pixelCoord = int2(floor(rasterSamplePosUV * float2(width, height)));

                const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
                const float depthOffset = gCamera.data.nearZ;
                float prevVisibility = curVisibility;
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
            }
            else if (DEPTH_MODE == DEPTH_MODE_RAYTRACED && (primaryIsOccluded || !isInScreen))
            {
                gRaysTraced += 1;

                if (!isInScreen) // reset visibility to 1 if the raster sample was not in screen
                    curVisibility = 1.0;

                // to be consistent with the rasterizer, we snap the uv coordinate as well to the pixel center,
                // but we do not clip it since we can shoot outside of the screen space
                //samplePosUV = getScreenClampedUV(texC, samplePosUV);
                samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center
            
                float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
                float initialSamplePosLength = length(initialSamplePosV);

                bool occluded = false;

                RayDesc ray;
                ray.Origin = gCamera.data.posW;
                ray.Direction = mul(sampleDirV, float3x3(invViewMat));

                float tSphereStart = (posVLength - sphereStart) * initialSamplePosLength / posVLength;
                float tSphereEnd = (posVLength - sphereEnd) * initialSamplePosLength / posVLength;
                float tHaloStart = (posVLength - sphereHeight - CONST_RADIUS - HALO_RADIUS) * initialSamplePosLength / posVLength;
                float tConstRadiusStart = (posVLength - sphereHeight - CONST_RADIUS) * initialSamplePosLength / posVLength;
                ray.TMin = max(tHaloStart, 0.0); // TODO include buffer value?
                ray.TMax = tSphereEnd;

                // include the value of the depth buffer when choosing TMin to save some traversal time
                if (isInScreen)
                {
                    const float epsilon = gData.radius * 0.01;
                    // objectSpaceZ already contains the depth buffer or secondary depth buffer value (fetched in the raster part)
                    ray.TMin = max(ray.TMin, (posVLength - objectSpaceZ) * initialSamplePosLength / posVLength + epsilon);
                }

                // skip procedural and force all triangle to be handled by any-hit traversal
                RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE > rayQuery;
                rayQuery.TraceRayInline(gScene.rtAccel, RAY_FLAG_NONE, 0xff, ray);

                float tFirstFrontFaceInside = tSphereEnd;
                float tLastFrontFaceHalo = tHaloStart;

                while (rayQuery.Proceed())
                {
                    if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                    {
                        // extract hit properties
                        float t = rayQuery.CandidateTriangleRayT();
                        if (t < tLastFrontFaceHalo)
                            continue; // we can skip this since it would not contribute anyways (unfortunately there is no option to set the ray.min afterwards)

                        bool frontFace = rayQuery.CandidateTriangleFrontFace();
                        const TriangleHit hit = getCandidateTriangleHit(rayQuery);
                        const uint materialID = gScene.getMaterialID(hit.instanceID);
                        const MaterialHeader header = gScene.materials.materialData[materialID].header;

                        bool isAlphaTested = header.getAlphaMode() == AlphaMode::Mask;

                        // needs alpha testing?
                        if (isAlphaTested)
                        {
                            const VertexData v = gScene.getVertexData(hit);
                            if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
                                continue; // alpha test failed => ignore this triangle
                        }

                        frontFace = frontFace || isAlphaTested || header.isDoubleSided();
                        if (!frontFace)
                            continue; // this is just for rasterizer compability

                        if (t <= tSphereStart)
                        {
                            tLastFrontFaceHalo = max(tLastFrontFaceHalo, t);
                            if (t >= tConstRadiusStart)
                                break; // we can stop the query, because this will set the visibility to zero
                        }
                        else // inside sphere
                        {
                            tFirstFrontFaceInside = min(tFirstFrontFaceInside, t);
                            rayQuery.CommitNonOpaqueTriangleHit(); // since we save the min, we can commit here
                        }
                        
                    }
                }

                // calculate visibility inside and outside of sphere
                float sphereZ = posVLength - tFirstFrontFaceInside * posVLength / initialSamplePosLength;
                float haloZ = posVLength - tLastFrontFaceHalo * posVLength / initialSamplePosLength;
                float sphereVisibility = calcSphereVisibility(sphereZ, sphereStart, sphereEnd);
                float haloVisibility = calcHaloVisibility(haloZ, sphereStart, sphereEnd);

                if (min(sphereVisibility, haloVisibility) < curVisibility)
                {
                    gRayDepth[uint3(XY, i)] = (sphereVisibility <= haloVisibility ? sphereZ : haloZ) / gData.radius;
                }
            
                curVisibility = min(curVisibility, min(sphereVisibility, haloVisibility));
            }*/


        }
    }

    float AO = visibility / float(NUM_DIRECTIONS * NUM_STEPS);

    AO = saturate(AO);
    //AO = saturate(2 * AO - 1);

    // do artistic modifications
    AO = pow(AO, gData.exponent);
    return AO;
}
