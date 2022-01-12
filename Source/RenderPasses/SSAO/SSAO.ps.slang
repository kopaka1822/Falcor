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

// only uses rasterization
#define SHADER_VARIANT_RASTER 0
// only uses raytracing
#define SHADER_VARIANT_RAYTRACING 1
// mixes ray tracing and rasterization
#define SHADER_VARIANT_HYBRID 2

#ifndef SHADER_VARIANT
#error please define shader variant to either raster, raytracing or hybrid
#endif

#define TANGENT_SPACE_VIEW_SPACE 0
#define TANGENT_SPACE_CAMERA_ORIENTED 1

// shoots only a small ray inside the sphere between [+sphereHeight, -sphereHeight]
#define RAY_MODE_SHORT_RAYS_APPROX 0
// shoots s small ray from the center, which is known to be unoccluded, to +sphereHeight, then continues with RAY_MODE_SHORT_RAYS_APPROX
// (this way, additional blocckers can be detected)
#define RAY_MODE_SHORT_RAYS_REF 1
// traces a single ray from the camera position to -sphereHeight
#define RAY_MODE_LONG_RAYS_REF 2
// traces a single ray from the camera position to -sphereHeight but stops at the first hit (should be very similar to rasterizer)
#define RAY_MODE_LONG_RAYS_FIRST_HIT 3


#define TANGENT_SPACE TANGENT_SPACE_CAMERA_ORIENTED
#define HEMISPHERE_SAMPLING true
#define PREVENT_DARK_HALOS true
#define RAY_MODE RAY_MODE_SHORT_RAYS_REF

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

Texture2D gDepthTex;
Texture2D gNormalTex;
Texture2D gNoiseTex;

static int gRaysTraced = 0;
static int gInvalid = 0;

float distanceWeight(float dsquared)
{
    //return saturate(1.0 - sqrt(dsquared) / gData.radius); // closer to HBAO
    return saturate(1.0 - dsquared * gData.invRadiusSquared);
    //return 1.0f;
}

// clamps uvend to screen space [0, 1]
// uvstart: uv coordinate that is in screen space
// uvend: target uv coordinate that might be outside of the screen space
// return: linear combination t*uvstart + (1-t)*uvend, that is as close to uvend as possible and is in the range of [0, 1]
float2 getScreenClampedUV(float2 uvstart, float2 uvend)
{
    return saturate(uvend); // this actually does not make much of a difference but costs a little bit more...
    float2 satuv = saturate(uvend);
    if (all(satuv == uvend))
        return uvend;

    // clip x
    float dist = abs(uvend.x - uvstart.x);
    if(dist > 0.0)
    {
        float t = abs(uvend.x - satuv.x) / dist;
        uvend.x = satuv.x;
        uvend.y = t * uvstart.y + (1.0 - t) * uvend.y;
    }

    // clip y
    satuv = saturate(uvend);
    dist = abs(uvend.y - uvstart.y);
    if(dist > 0.0)
    {
        float t = abs(uvend.y - satuv.y) / dist;
        uvend.y = satuv.y;
        uvend.x = t * uvstart.x + (1.0 - t) * uvend.x;
    }
    
    return uvend;
}

float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelCoord = floor(uv * float2(width, height));
    return float2((pixelCoord.x + 0.5f) / width, (pixelCoord.y + 0.5f) / height);
}

float4 getPosition(float2 uv)
{
    float4 pos;
    pos.x = uv.x * 2.0f - 1.0f;
    pos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    pos.z = gDepthTex.SampleLevel(gTextureSampler, uv, 0).r;
    pos.w = 1.0f;

    float4 posW = mul(pos, gCamera.data.invViewProj);
    posW /= posW.w;

    return posW;
}

// "Explodes" an integer, i.e. inserts a 0 between each bit. 
//      For example, 0b11111111 -> 0b1010101010101010
uint IntegerExplode(uint x)
{
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

// helper math for zcurve
uint ZCurveToLinearIndex(uint2 xy)
{
    return IntegerExplode(xy[0]) | (IntegerExplode(xy[1]) << 1);
}

uint JenkinsHash(uint a)
{
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
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

float4 main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if(linearDepth <= 0.0) return 1.0f;

    // view space position of current pixel
    const float3 posV = UVToViewSpace(texC, linearDepth);
    const float posVLength = length(posV);

    // view space normal of current pixel
    float3 normalW = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    
    // Calculate tangent space (use random direction for tangent orientation)
    float2 randDir = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale).xy;
    randDir = normalize(randDir); // should be normalized by default, but precision is lost in texture format
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 normal;
    float3 tangent;
    float3 bitangent;
    if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
    {
        normal = float3(0.0, 0.0, 1.0);
        tangent = float3(randDir, 0.0f);
        bitangent = float3(Rotate90(randDir), 0.0f);
    }
    else // TANGENT_SPACE_CAMERA_ORIENTED
    {
        // consistent with notation above when normal = (0, 0, 1)
        normal = -posV / posVLength;
        bitangent = normalize(cross(normal, float3(randDir, 0.0f)));
        tangent = cross(bitangent, normal);
    }

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, normal));
    
    float visibility = 0.0f;
    //uint zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));
    //uint i = (zCurveIndex + (frameIndex)) % gData.kernelSize; // JenkinsHash
    for (uint i = 0; i < gData.kernelSize; i++)
    {
        // obtain sample position on disc around view space position
        float4 rand = gData.sampleKernel[i]; // xy = random location on unit disc, zw = uniform in 0,1
        rand.xy *= gData.radius; // multiply 2D position with sample radius
        
        // height of the sphere at the requested sample position (not at the actual sampling position)
        float sphereHeight = sqrt(gData.radius * gData.radius - dot(rand.xy, rand.xy));
        
        // determine distance within [-sphereHeight, +sphereHeight]
        float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
        float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

        if (HEMISPHERE_SAMPLING) // determine correct sphereStart and sphereEnd for hemisphere
        {
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
            gInvalid += 1;
            continue;
        }

        // calculate view position of sample and project to uv coordinates
        float3 initialSamplePosV = posV + tangent * rand.x + bitangent * rand.y;
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
        float objectSpaceZ; // needs to be calculated by the respective variant

        bool hybridRay = false;
        if (SHADER_VARIANT == SHADER_VARIANT_RASTER || SHADER_VARIANT == SHADER_VARIANT_HYBRID)
        {
            // clip sample position uv and snap to pixel center
            float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
            if (any(samplePosUV != screenUv))
                hybridRay = true; // always shoot rays for screen border
            samplePosUV = screenUv;
            samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center

            // obtain actual view space position of the sample
            float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, samplePosUV, 0).r;
            float3 samplePosV = UVToViewSpace(samplePosUV, linearSampleDepth);

            // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
            if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
                objectSpaceZ = linearDepth - linearSampleDepth;
            else // TANGENT_SPACE_CAMERA_ORIENTED
                objectSpaceZ = dot(samplePosV - posV, normal);

            if(objectSpaceZ > sphereStart)
                hybridRay = true;
        }
        if (SHADER_VARIANT == SHADER_VARIANT_RAYTRACING || (SHADER_VARIANT == SHADER_VARIANT_HYBRID && hybridRay))
        {
            gRaysTraced += 1;
            
            // to be consistent with the rasterizer, we snap the uv coordinate as well to the pixel center,
            // but we do not clip it since we can shoot outside of the screen space
            samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center
            float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
            float initialSamplePosLength = length(initialSamplePosV);

            bool occluded = false;
            if (RAY_MODE == RAY_MODE_SHORT_RAYS_REF)
            { // THis assumes TANGENT_SPACE_VIEW_SPACE for now

                #if RAY_MODE == RAY_MODE_SHORT_RAYS_REF
                #if TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE
                #error TANGENT_SPACE_VIEW_SPACE not supported with RAY_MODE_SHORT_RAYS_REF
                #endif
                #endif
                
                // cast an initial ray to the start of the sampling range
                RayDesc ray;
                float3 followUpRayDir = mul(sampleDirV, float3x3(invViewMat));
                float followUpRayMin = 0.0;
                if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
                    followUpRayMin = (linearDepth - sphereStart) * initialSamplePosLength / linearDepth;
                else // TANGENT_SPACE_CAMERA_ORIENTED
                    followUpRayMin = (posVLength - sphereStart) * initialSamplePosLength / posVLength;

                float3 followUpRayStart = gCamera.data.posW + followUpRayDir * followUpRayMin;
                
                ray.Origin = mul(float4(posV + normal * gData.radius, 1.0), invViewMat).xyz; // start of the sphere
                ray.Direction = normalize(followUpRayStart - ray.Origin);
                ray.TMin = 0.0;
                ray.TMax = distance(ray.Origin, followUpRayStart);

                SceneRayQuery < 1 > sceneRayQuery; // 1 = Use Alpha Test
                HitInfo hit;
                float hitT;

                objectSpaceZ = sphereEnd; // assume nothing hit
                if (sceneRayQuery.traceRay(ray, hit, hitT, RAY_FLAG_NONE, 0xff))
                {
                    // this means we have an occluder
                    occluded = true;
                }
            }
            if (!occluded && (RAY_MODE == RAY_MODE_LONG_RAYS_FIRST_HIT || RAY_MODE == RAY_MODE_SHORT_RAYS_APPROX || RAY_MODE == RAY_MODE_SHORT_RAYS_REF))
            {
                // shoot a ray through the sample position
                RayDesc ray;
                ray.Origin = gCamera.data.posW; // offset a little bit in normal direction
                ray.Direction = mul(sampleDirV, float3x3(invViewMat));

                ray.TMin = 0.0;
                if (RAY_MODE == RAY_MODE_SHORT_RAYS_APPROX || RAY_MODE == RAY_MODE_SHORT_RAYS_REF)
                {
                    if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
                        ray.TMin = (linearDepth - sphereStart) * initialSamplePosLength / linearDepth;
                    else // TANGENT_SPACE_CAMERA_ORIENTED
                        ray.TMin = (posVLength - sphereStart) * initialSamplePosLength / posVLength;
                }

                
                if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
                    ray.TMax = (linearDepth - sphereEnd) * initialSamplePosLength / linearDepth;
                else // TANGENT_SPACE_CAMERA_ORIENTED
                    ray.TMax = (posVLength - sphereEnd) * initialSamplePosLength / posVLength;

                SceneRayQuery < 1 > sceneRayQuery; // 1 = Use Alpha Test
                HitInfo hit;
                float hitT;

                objectSpaceZ = sphereEnd; // assume nothing hit
                if (sceneRayQuery.traceRay(ray, hit, hitT, RAY_FLAG_NONE, 0xff)) // TODO value of hitT
                {
                    if (TANGENT_SPACE == TANGENT_SPACE_VIEW_SPACE)
                    {
                        float projectedHitT = hitT * linearDepth / initialSamplePosLength;
                        objectSpaceZ = linearDepth - projectedHitT;
                    }
                    else
                    { // TANGENT_SPACE_CAMERA_ORIENTED
                        float projectedHitT = hitT * posVLength / initialSamplePosLength;
                        objectSpaceZ = posVLength - projectedHitT;
                    }
                }
            }
            if(occluded) objectSpaceZ = sphereStart; // assume occlusion at entry point
        }

        float sampleRange = max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
        float maxSampleRange = 2.0 * sphereHeight;
        visibility += sampleRange / maxSampleRange; // divide sampled range by pdf (monte carlo)

        if (PREVENT_DARK_HALOS)
        {
            // get rid of shadowing around edges
            // introduce a linear falloff function that starts with 0.0 when the sample depth intersects the front sphere exactly,
            // and falls of to 1.0 when it gets further away from the sphere but closer to the camera
            visibility += saturate((objectSpaceZ - sphereHeight) / (gData.radius * 4.0))
                * (sphereStart - sphereEnd) / maxSampleRange // this adjust the visibility to the sampling (hemi-)sphere
            ;
        }
    }

    float AO = visibility / float(gData.kernelSize);
    // since fully visibile are all values in [0.5, 1.0], scale accordingly
    AO = saturate(AO * 2.0);
    // do artistic modifications
    //AO = pow(AO, 2.0);

    float4 res = AO;
    float red = gRaysTraced / float(gData.kernelSize);
    float blue = gInvalid / float(gData.kernelSize);
    //float red = gRaysTraced;
    float green = 1.0 - red - blue;

    res.r *= red;
    res.g *= green;
    res.b *= blue;
    return res;
}
