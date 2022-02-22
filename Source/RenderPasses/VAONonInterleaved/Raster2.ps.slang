#include "Common.slang"

// additional depth textures
Texture2D gDepthTex2;
Texture2DMS<float> gsDepthTex;

// inputs from previous stage
Texture2D<uint> aoMask;
Texture2D<float> ao2;
Texture2D<float> aoPrev;

#define USE_OLD_DEPTH

[earlydepthstencil]
float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET
{
    uint mask = aoMask[uint2(svPos.xy)];

    // fetch linear depth a second time
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    
    float oldAo = ao2.SampleLevel(gTextureSampler, texC, 0);
    // scale old ao with number of actual samples
    oldAo *= float(NUM_DIRECTIONS) / float(countbits(mask));

    // this cannot happen (masked)
    /*if (linearDepth >= gCamera.data.farZ)
    {
        return 1.0;
    }*/

    // view space position of current pixel
    const float3 posV = UVToViewSpace(texC, linearDepth);
    const float posVLength = length(posV);

    // view space normal of current pixel
    float3 normalW = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0)
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

    uint i = 0;
    //[loop] while(mask != 0u)
    [unroll] for (uint j = 0; j < NUM_DIRECTIONS; j++)
    {
        if (mask == 0u) break; // no bits set anymore

        // modify loop to only go through the set bits in mask
        //[loop] while ((mask & 1u) == 0u)
        //for (uint k = 0; k < (NUM_DIRECTIONS - j) && (mask & 1u) == 0u; k++) // this is too complicated for current compiler..  
        [unroll] for (uint k = 0; k < NUM_DIRECTIONS && k < NUM_DIRECTIONS - j && (mask & 1u) == 0u; k++) // first condition is for unrolling, second is for better unrolling
        {
            // shift mask an increase i
            mask = mask >> 1;
            ++i;
        }


        // random angle on view space disc
        float alpha = (float(i) / NUM_DIRECTIONS) * 2.0 * 3.141;
        float radius = sampleRadius[i] * gData.radius; // radius on sampling unit sphere * world space radius
        float2 dir = radius * float2(sin(alpha), cos(alpha));  // world space direction

        const float sphereHeight = sqrt(gData.radius * gData.radius - radius * radius);

        // determine distance within [-sphereHeight, +sphereHeight]
        float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
        float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

        { // HEMISPHERE SAMPLING
            //float zIntersect = -dot(rand.xy, normalO.xy) / normalO.z;
            float zIntersect = -dot(dir.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            if (normalO.z >= 0.0)
                sphereEnd = zIntersectClamped;
            else
                sphereStart = zIntersectClamped;
        }

        // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
        if (sphereStart - sphereEnd < 0.01)
        {
            continue; // skip sample (no visibility)
        }

        // sample position calculate uv position of sample
        float2 samplePosUV = ViewSpaceToUV(posV + tangent * dir.x + bitangent * dir.y);

        float curVisibility = 0.0f;
        // assume this for now
        //curVisibility = 1.0;
        curVisibility = oldAo;
        bool hybridRay = false;

        float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
        //if (any(samplePosUV != screenUv))
        //    hybridRay = true; // always shoot rays for screen border
            
        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

        if (DEPTH_MODE == DEPTH_MODE_STOCHASTIC) 
        {
            #ifdef USE_OLD_DEPTH
            float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
            float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
            float objectSpaceZ = dot(samplePosV - posV, normal);
            curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            #endif

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
        }

        /*if (USE_RAYS && hybridRay)
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
        }*/

        const float pdf = 2.0 * sphereHeight;
        visibility += curVisibility / pdf;
        
        // advance mask for next iteration
        mask = mask >> 1;
        ++i;
    }

    visibility *= 2.0 / float(NUM_DIRECTIONS);
    float prevAo = aoPrev.SampleLevel(gTextureSampler, texC, 0);
    return prevAo + min(visibility, oldAo);
}
