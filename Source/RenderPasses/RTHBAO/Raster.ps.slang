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
RWTexture2DArray<float> gRasterAO;
RWTexture2DArray<float> gRayDepth;
RWTexture2DArray<float> gRayAO;
RWTexture2DArray<uint> gInstanceIDOut;
RWTexture2DArray<uint> gForceRay; // 1 ray tracing needs to be forced. Either because the raster sample is outside the screen space or because the raster hit a double sided material before the sampling sphere starts.
RWTexture2DArray<float> gSphereStart;

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

// like sign, but returns 1 for v=0
float signBinary(float v)
{
    if (v < 0.0)
        return -1.0;
    return 1.0;
}

float2 getRaySphereIntersections(float3 rayStart, /*normalized*/ float3 rayDir, float3 sphereCenter, float radius)
{
    // distance to the closest point near the sphere center (if d > radius, there are no intersections)
    float d = dot(rayDir, sphereCenter - rayStart);
    float delta = d * d + radius * radius - dot(sphereCenter - rayStart, sphereCenter - rayStart);
    delta = max(delta, 0.0); // this is important to have a valid square root (negative values would mean no intersection, but we just assume that this will only be the case for precision reasons)
    return float2(d - delta, d + delta);
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

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);
    if (linearDepth >= gCamera.data.farZ * 0.99)
        return 1.0f;
    
    // view space position of current pixel
    float3 posV = UVToViewSpace(texC, linearDepth);
    float3 posW = mul(float4(posV, 1.0), invViewMat).xyz;
    
    // view space normal of current pixel
    float3 normalW = normalize(gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz);
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0) // front face normals
    {
        normalW = -normalW;
        normalV = -normalV;
    }

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
    float3 V = -posV / posVLength;
    float3 bitangent = normalize(cross(V, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, V);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, V));

    // can be used to determine on which side of the sampling plane
    // TODO i think this needs to be done with the sampling plane instead of V...
    float3 halfspacePlane = cross(normalV, cross(V, normalV));

    bool allSamplesInScreen = true; // assume all samples are in the screen, will be overwritten if the opposite occurs
    float visibility = 0.0f;
    //uint zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));
    //uint i = (zCurveIndex + (frameIndex)) % KERNEL_SIZE; // JenkinsHash
    [unroll] for (uint i = 0; i < NUM_DIRECTIONS; ++i)
    {
        float angle = (2.0 * 3.141 * i) / NUM_DIRECTIONS;
        float2 direction = float2(sin(angle), cos(angle)); // TODO use the random direction from the noise field?


        [unroll] for (int step = 0; step < NUM_STEPS; ++step)
        {
            const uint sampleIndex = i * NUM_STEPS + step;
            
            // calc 's' sampling pos
            float2 s = direction * (step + 0.5 + randJitter) / (NUM_STEPS + 1); // TODO add jitter
            
            s *= gData.radius; // multiply 2D position with sample radius
        
            // determine distance within [-sphereHeight, +sphereHeight]
            /*float sphereStart = sqrt(gData.radius * gData.radius - dot(s, s));; // in object coordinates (bigger is closer to the camera)
            float zIntersect = -dot(s, normalO.xy) / makeNonZero(normalO.z, 0.0001);// intersection with hemisphere plane
            float sphereEnd = clamp(zIntersect, -sphereStart, sphereStart);
            
            // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
            if (sphereStart - sphereEnd < 0.01)
            {
                gInvalid += 1;
                visibility += 1.0;
                gInstanceIDOut[uint3(XY, sampleIndex)] = 0;
                //gInstanceIDOut[uint3(XY, sampleIndex)] = curInstanceID;
                continue;
            }*/

            // calculate view position of sample and project to uv coordinates
            float3 S = tangent * s.x + bitangent * s.y; // sample position relative to posV
            float3 initialSamplePosV = posV + S; // absolute sample position in view space
            float initialSampleT = length(initialSamplePosV); // serves as reference for ray/raster depths
            float2 sphereStartEnd = getRaySphereIntersections(0.0, normalize(initialSamplePosV), posV, gData.radius);
            // TODO determine if the sphere start is below the hemisphere => if this is the case, no depth sample needs to be taken
            float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);

            // clip sample position uv and snap to pixel center
            float2 screenUv = saturate(samplePosUV); // clip to screen border
            const bool isInScreen = all(samplePosUV == screenUv);
            if (!isInScreen)
                allSamplesInScreen = false; // remember for sample evaluation with ML
            bool requireRay = !isInScreen;
            float2 rasterSamplePosUV = getSnappedUV(screenUv);

            /*if (isSamePixel(texC, rasterSamplePosUV))
            {
                visibility += 1.0;
                continue;
            }*/
            
            float3 hitV = UVToViewSpace(rasterSamplePosUV, gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0.0));
            float hitT = min(length(hitV), gCamera.data.farZ); // distance to camera origin (origin is 0 because of view space)
            gRasterDepth[uint3(XY, sampleIndex)] = (initialSampleT - hitT) / gData.radius;
            gSphereStart[uint3(XY, sampleIndex)] = (initialSampleT - sphereStartEnd.x) / gData.radius;
            gRayDepth[uint3(XY, sampleIndex)] = (initialSampleT - hitT) / gData.radius;
            gInstanceIDOut[uint3(XY, sampleIndex)] = gInstanceID[UVToPixel(rasterSamplePosUV)];
            
            float3 P = normalize(hitV - posV); // depth buffer hit vector (P) from posV to hitpoint

            //float angleSign = signBinary(dot(normalV, S) + 0.01) * signBinary(dot(P, halfspacePlane));
            //float alpha = acos(max(dot(normalV, P), 0.0)) * 2.0 / 3.141;
            float alpha = 1 - max(dot(normalV, P), 0.0);

            // TODO test if sample point is below the sphere
            
            float curVisibility = 1.0;
            if (hitT > sphereStartEnd.y) // intersection behind the sphere (assume not occluded)
            {
                // do nothing
            }
            else if(hitT < sphereStartEnd.x)
            //else if ((initialSampleT - hitT) > gData.radius)
            {
                requireRay = true;
                curVisibility = alpha;
            }
            else
            {
                // evalulate inside 
                curVisibility = alpha;
            }

            gRasterAO[uint3(XY, sampleIndex)] = curVisibility;
            gRayAO[uint3(XY, sampleIndex)] = curVisibility;
            
            //if (hitT < sphereStartEnd.y)
            //    curVisibility = alpha;
            requireRay = true;
            //requireRay = false;

            if (DEPTH_MODE == DEPTH_MODE_RAYTRACED && requireRay)
            {
                gRaysTraced += 1;
                samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center

                float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
                
                RayDesc ray;
                ray.Origin = gCamera.data.posW;
                ray.Direction = mul(sampleDirV, float3x3(invViewMat));
                ray.TMin = 0.0;
                ray.TMax = gCamera.data.farZ;

                float tLastFaceOutside = ray.TMin;
                float tFirstFaceInside = ray.TMax;
                int occlusionStack = 0; // > 1 means occluded (counted for outside front faces)
                
                // skip procedural and force all triangle to be handled by any-hit traversal
                RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE > rayQuery;
                rayQuery.TraceRayInline(gScene.rtAccel, RAY_FLAG_NONE, 0xff, ray);
                
                while (rayQuery.Proceed())
                {
                    if (rayQuery.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
                        continue;
                    
                    // extract hit properties
                    float t = rayQuery.CandidateTriangleRayT();

                    bool frontFace = rayQuery.CandidateTriangleFrontFace();
                    const TriangleHit hit = getCandidateTriangleHit(rayQuery);
                    const uint materialID = gScene.getMaterialID(hit.instanceID);
                    const MaterialHeader header = gScene.materials.materialData[materialID].header;

                    bool isAlphaTested = header.getAlphaMode() == AlphaMode::Mask;
                    frontFace = frontFace || isAlphaTested || header.isDoubleSided();

                    /*if(frontFace) // for testing if the results between ray and raster are identical
                    {
                        if (isAlphaTested)
                        {
                            const VertexData v = gScene.getVertexData(hit);
                            if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
                                continue; // alpha test failed => ignore this triangle
                        }
                        
                        tLastFaceOutside = t;
                        occlusionStack = 1;
                        rayQuery.CommitNonOpaqueTriangleHit();
                    }*/
                    
                    if(t < sphereStartEnd.x) // in front of the sphere
                    {
                        // intersection in front of the trusted area

                        if (isAlphaTested || header.isDoubleSided()) // ignore alpha tested materials (too thin for occlusion at this distance)
                            continue;
                        occlusionStack += frontFace ? 1 : -1; // update occlusion stack
                        //if (frontFace) // TODO comment out => use backfaces as well for HBAO (since those can be intersection from other perspectives)
                        tLastFaceOutside = max(tLastFaceOutside, t);
                    }
                    else // inside of the trusted area
                    {
                        // needs alpha testing?
                        if (isAlphaTested)
                        {
                            const VertexData v = gScene.getVertexData(hit);
                            if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
                                continue; // alpha test failed => ignore this triangle
                        }

                        tFirstFaceInside = min(tFirstFaceInside, t);
                        rayQuery.CommitNonOpaqueTriangleHit(); // since we save the min, we can commit here
                    }                
                    
                } // RAY QUERY WHILE

                float tFinalHit = tLastFaceOutside;
                if (occlusionStack <= 0)
                    tFinalHit = tFirstFaceInside;

                gRayDepth[uint3(XY, sampleIndex)] = (initialSampleT - tFinalHit) / gData.radius;
                float3 hitV = mul(float4(ray.Origin + ray.Direction * tFinalHit, 1.0), gCamera.data.viewMat).xyz; 
                float3 P = normalize(hitV - posV);
                float alpha = 1 - max(dot(normalV, P), 0.0);
                curVisibility = alpha;
                
                if (tFinalHit > sphereStartEnd.y) // intersection behind the sphere (assume not occluded)
                {
                    curVisibility = 1.0;
                }

                gRayAO[uint3(XY, sampleIndex)] = curVisibility;
            }

            visibility += curVisibility;
            
        }
    }

    //float meanAO = (visibility - gRaysTraced) / max(1, NUM_DIRECTIONS * NUM_STEPS - gRaysTraced);
    //visibility += meanAO * gRaysTraced - 1.0 * gRaysTraced;

    if(!allSamplesInScreen)
    {
        // exlude all samples from ml by setting the forced ray flag for all samples
        for (uint sampleIndex = 0; sampleIndex < NUM_DIRECTIONS * NUM_STEPS; ++sampleIndex)
            gForceRay[uint3(XY, sampleIndex)] = 1;
    }
    
    
    float AO = visibility / float(NUM_DIRECTIONS * NUM_STEPS);

    AO = saturate(AO);
    //AO = saturate(2 * AO - 1);

    // do artistic modifications
    AO = pow(AO, gData.exponent);
    return AO;
}
