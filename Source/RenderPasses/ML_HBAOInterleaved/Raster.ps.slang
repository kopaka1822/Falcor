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

#include "NeuralNetDefines.slangh"

#define ml_float float

#define NUM_STEPS 4
#define NUM_DIRECTIONS 8

cbuffer StaticCB
{
    SSAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
    float Rand; // the random values are fixed for a quarter res texture (4x4 grid)
    uint sliceIndex;
    uint2 quarterOffset; // offset inside the quarter resolution texture
}

SamplerState gTextureSampler;

// inputs
Texture2D<float> gDepthTexQuarter;
#define MTL_DOUBLE_SIDED 1
Texture2D<uint> gMaterialDataQuarter;
Texture2D gNormalTex; // normals do not need the extra effor for quarter texture since they are read once anyways (for sampling the local hemisphere)

float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTexQuarter.GetDimensions(width, height);
    float2 pixelCoord = floor(uv * float2(width, height)) + 0.25 * float2(quarterOffset);
    return float2((pixelCoord.x + 0.125f) / width, (pixelCoord.y + 0.125f) / height);
}

bool isSamePixel(float2 uv1, float2 uv2)
{
    float width, height;
    gDepthTexQuarter.GetDimensions(width, height);
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

int2 UVToPixelFull(float2 uv)
{
    float width, height;
    gNormalTex.GetDimensions(width, height);
    return int2(floor(uv * float2(width, height)));
}

int2 UVToPixelQuarter(float2 uv)
{
    float width, height;
    gDepthTexQuarter.GetDimensions(width, height);
    return int2(floor(uv * float2(width, height)));
}

float2 getRaySphereIntersections(float3 rayStart, /*normalized*/ float3 rayDir, float3 sphereCenter, float radius)
{
    // distance to the closest point near the sphere center (if d > radius, there are no intersections)
    float d = dot(rayDir, sphereCenter - rayStart);
    float delta = d * d + radius * radius - dot(sphereCenter - rayStart, sphereCenter - rayStart);
    delta = max(delta, 0.0); // this is important to have a valid square root (negative values would mean no intersection, but we just assume that this will only be the case for precision reasons)
    return float2(d - delta, d + delta);
}

struct PSOut
{
    float ao : SV_Target0;
    uint rayMask : SV_Target1;
};

PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    PSOut o;
    o.ao = 0.0;
    o.rayMask = 0;
    
    // fix texc
    texC = getSnappedUV(texC); // snappping to the quarter resolution texture
    
    float linearDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, texC, 0);
    if (linearDepth >= gCamera.data.farZ * 0.99)
        return o;
    
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
    
    const float posVLength = length(posV);

    // Calculate tangent space (use random direction for tangent orientation)
    float randJitter = Rand;
    float randRotation = Rand * 2.0 * 3.141;
    float2 randDir = float2(sin(randRotation), cos(randRotation));
    
    // determine tangent space
    float3 V = -posV / posVLength;
    float3 bitangent = normalize(cross(V, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, V);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, V));

    // can be used to determine on which side of the sampling plane
    // TODO i think this needs to be done with the sampling plane instead of V...
    float3 halfspacePlane = cross(normalV, cross(V, normalV));

    float visibility = 0.0f;
    //uint zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));
    //uint i = (zCurveIndex + (frameIndex)) % KERNEL_SIZE; // JenkinsHash
    [unroll] for (uint i = 0; i < (NUM_DIRECTIONS / 2); ++i)
    {
        float angle = (2.0 * 3.141 * i) / NUM_DIRECTIONS;
        float2 direction = float2(sin(angle), cos(angle));

        // temporary array with raster heights for current line of 8 samples
        float rasterHeights[NUM_STEPS * 2];
        uint finalRayMask = 0; // bits that need ray tracing (100% sure)
        uint pendingRayMask = 0; // bits that might need ray tracing
        int step;
        [unroll] for (step = -NUM_STEPS; step < NUM_STEPS; ++step)
        {
            // calc 's' sampling pos
            float jitterSign = step < 0 ? -1 : 1;
            float2 s = direction * (step + 0.5 + jitterSign * randJitter) / (NUM_STEPS + 1);
            s *= gData.radius; // multiply 2D position with sample radius

            // calculate view position of sample and project to uv coordinates
            float3 S = tangent * s.x + bitangent * s.y; // sample position relative to posV
            float3 initialSamplePosV = posV + S; // absolute sample position in view space
            float initialSampleT = length(initialSamplePosV); // serves as reference for ray/raster depths
            float2 sphereStartEnd = getRaySphereIntersections(0.0, normalize(initialSamplePosV), posV, gData.radius);

            bool validSample = true;
            float3 sphereStartVector = (initialSamplePosV / initialSampleT * sphereStartEnd.x - posV);
            if (dot(normalV, sphereStartVector) <= 0.01)
            {
                // below surface
                validSample = false; // this should always evaluate to visibility = 1.0
            }
            
            // TODO determine if the sphere start is below the hemisphere => if this is the case, no depth sample needs to be taken
            float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);

            // clip sample position uv and snap to pixel center
            float2 screenUv = saturate(samplePosUV); // clip to screen border
            const bool isInScreen = all(samplePosUV == screenUv);
                
            bool forceRay = false;
            forceRay = !isInScreen;
            bool requireRay = false;
            float2 rasterSamplePosUV = getSnappedUV(screenUv);

   
            float3 hitV = UVToViewSpace(rasterSamplePosUV, gDepthTexQuarter.SampleLevel(gTextureSampler, rasterSamplePosUV, 0.0));
            float hitT = min(length(hitV), gCamera.data.farZ); // distance to camera origin (origin is 0 because of view space)
            //uint4 mtlData = gMaterialData.SampleLevel(gTextureSampler, rasterSamplePosUV, 0.0);
            rasterHeights[step + NUM_STEPS] = clamp((initialSampleT - hitT) / gData.radius, -16.0, 16.0);

            if (isSamePixel(texC, rasterSamplePosUV))
            {
                validSample = false; // also evaluate to visibility = 1.0
                //rasterHeights[step + NUM_STEPS] = 0.0;
            }
            
            float3 P = normalize(hitV - posV); // depth buffer hit vector (P) from posV to hitpoint

            float alpha = 1 - max(dot(normalV, P), 0.0);
            
            float curVisibility = 1.0;
            if (!validSample || hitT > sphereStartEnd.y) // intersection behind the sphere (assume not occluded)
            {
                // do nothing
            }
            else if (hitT < sphereStartEnd.x)
            //else if ((initialSampleT - hitT) > gData.radius)
            {
                requireRay = true;
                curVisibility = alpha;
                // check if it was a double sided hit
                if (gMaterialDataQuarter[UVToPixelQuarter(rasterSamplePosUV)] & MTL_DOUBLE_SIDED)
                    forceRay = true; // force rays for double sided materials in front of the sphere => they cannot be occluders by definition
            }
            else
            {
                // evalulate inside 
                curVisibility = alpha;
            }

            //forceRay = forceRay || requireRay;
            //requireRay = false;
            //forceRay = requireRay = false;
            
            if (forceRay && validSample)
                finalRayMask |= 1u << (step + NUM_STEPS);
            else if(requireRay && validSample)
                pendingRayMask |= 1u << (step + NUM_STEPS);
            else
                visibility += curVisibility;

        }
        
        // use machine learning to evaluate questionable steps
        step = -NUM_STEPS;
        //while (pendingRayMask != 0u)
        [unroll]
        for (; step < NUM_STEPS; step += NUM_STEPS)
        {
            /*while ((pendingRayMask & 1u) == 0)
            {
                step++;
                pendingRayMask = pendingRayMask >> 1;
            }*/
            
            if (!(pendingRayMask & (0xF << (step + NUM_STEPS))))
                continue; // only work with marked samples
            
            ml_float inputs[5];
            if (step < 0)
            {
                inputs[0] = rasterHeights[3];
                inputs[1] = rasterHeights[2];
                inputs[2] = rasterHeights[1];
                inputs[3] = rasterHeights[0];
                inputs[4] = rasterHeights[4];
            }
            else
            {
                inputs[0] = rasterHeights[4];
                inputs[1] = rasterHeights[5];
                inputs[2] = rasterHeights[6];
                inputs[3] = rasterHeights[7];
                inputs[4] = rasterHeights[3];
            }

            ml_float layer1Output[KERNEL0_COLUMNS];
            [unroll]
            for (int outIdx = 0; outIdx < KERNEL0_COLUMNS; outIdx++)
                layer1Output[outIdx] = BIAS0(outIdx);
            
            [unroll]
            for (int inIdx = 0; inIdx < KERNEL0_ROWS; inIdx++)
                [unroll]
                for (int outIdx = 0; outIdx < KERNEL0_COLUMNS; outIdx++)
                    layer1Output[outIdx] = mad(KERNEL0(inIdx, outIdx), inputs[inIdx], layer1Output[outIdx]);
            
            [unroll]
            for (int outIdx = 0; outIdx < KERNEL0_COLUMNS; ++outIdx)
                layer1Output[outIdx] = max(layer1Output[outIdx], 0); // RELU

#if NUM_LAYERS == 2
            ml_float layer2Output[KERNEL1_COLUMNS];
            [unroll] for (int outIdx = 0; outIdx < KERNEL1_COLUMNS; outIdx++)
                layer2Output[outIdx] = BIAS1(outIdx);
            
            [unroll] for (int inIdx = 0; inIdx < KERNEL1_ROWS; inIdx++)
                [unroll] for (int outIdx = 0; outIdx < KERNEL1_COLUMNS; outIdx++)
                    layer2Output[outIdx] = mad(KERNEL1(inIdx, outIdx), layer1Output[inIdx], layer2Output[outIdx]);

#if KERNEL1_COLUMNS != 4
            #error expected 4 output neurons for layer 2
#endif
            
            uint bitmask = 0;
            [unroll] for (int outIdx = 0; outIdx < KERNEL1_COLUMNS; outIdx++)
                if (layer2Output[outIdx] > ml_float(0.0))
                    bitmask = bitmask | (1u << outIdx);
                    

            if (step == -NUM_STEPS)
                bitmask = reversebits(bitmask) >> (32 - 4);
            
            // mask bitmask with requested flags
            bitmask = (bitmask << (step + NUM_STEPS));
            bitmask = pendingRayMask & bitmask;
            
            finalRayMask |= bitmask;
#endif

            
            //step++;
            //pendingRayMask = pendingRayMask >> 1;
        }

        // defer ray tracing to other pass
        o.rayMask = o.rayMask | (finalRayMask << (i * 8));
        
        // for simplicity start in same shader
        /*step = -NUM_STEPS;
        while(finalRayMask != 0u)
        //[unroll] for (; step < NUM_STEPS; ++step)
        {
            while ((finalRayMask & 1u) == 0)
            {
                step++;
                finalRayMask = finalRayMask >> 1;
            }
            
            //if (!(finalRayMask & (1u << (step + NUM_STEPS)))) continue; // only work with marked samples
            
            // calc 's' sampling pos
            float jitterSign = step < 0 ? -1 : 1;
            float2 s = direction * (step + 0.5 + jitterSign * randJitter) / (NUM_STEPS + 1);
            s *= gData.radius; // multiply 2D position with sample radius

            // calculate view position of sample and project to uv coordinates
            float3 S = tangent * s.x + bitangent * s.y; // sample position relative to posV
            float3 initialSamplePosV = posV + S; // absolute sample position in view space
            float initialSampleT = length(initialSamplePosV); // serves as reference for ray/raster depths
            float2 sphereStartEnd = getRaySphereIntersections(0.0, normalize(initialSamplePosV), posV, gData.radius);

            //bool validSample = true;
            //float3 sphereStartVector = (initialSamplePosV / initialSampleT * sphereStartEnd.x - posV);
            //if (dot(normalV, sphereStartVector) <= 0.01)
            //{
            //    // below surface
            //    validSample = false; // this should always evaluate to visibility = 1.0
            //}
            
            float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
            
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
                    
                if (t < sphereStartEnd.x) // in front of the sphere
                {
                    // intersection in front of the trusted area
                    if (isAlphaTested || header.isDoubleSided()) // ignore alpha tested materials (too thin for occlusion at this distance)
                        continue;
                    occlusionStack += frontFace ? 1 : -1; // update occlusion stack
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

            //gRayDepth[uint3(XY, sampleIndex)] = (initialSampleT - tFinalHit) / gData.radius;
            float3 hitV = mul(float4(ray.Origin + ray.Direction * tFinalHit, 1.0), gCamera.data.viewMat).xyz;
            float3 P = normalize(hitV - posV);
            float alpha = 1 - max(dot(normalV, P), 0.0);
            float curVisibility = alpha;
                
            if (tFinalHit > sphereStartEnd.y) // intersection behind the sphere (assume not occluded)
            {
                curVisibility = 1.0;
            }
            visibility += curVisibility;

            // prepare for next iteration
            step++;
            finalRayMask = finalRayMask >> 1;
        }*/
    }
    
    float AO = visibility / float(NUM_DIRECTIONS * NUM_STEPS);
    if (o.rayMask == 0u)
        AO = pow(AO, gData.exponent); // apply exponent here since it will be ignored in the ray shader
    
    o.ao = AO;
    return o;
}
