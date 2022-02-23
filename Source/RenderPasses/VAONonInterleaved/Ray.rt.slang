import Scene.Raytracing;
#include "Common.slang"

// additional depth textures
Texture2DMS<float> gsDepthTex;

// inputs from previous stage
Texture2D<uint> aoMask;

RWTexture2D<float> output;

struct RayData
{
    // TODO compress to half
    float tLastFrontFaceHalo; // ray min
    float tFirstFrontFaceInside; // ray max
    float tConstRadiusStart;
    float tSphereStart;
};

[shader("miss")]
void miss(inout RayData rayData)
{}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // extract hit properties
    float t = RayTCurrent();
    if (t < rayData.tLastFrontFaceHalo)
        IgnoreHit(); // we can skip this since it would not contribute anyways (unfortunately there is no option to set the ray.min afterwards)

    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    TriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    const uint materialID = gScene.getMaterialID(hit.instanceID);
    const MaterialHeader header = gScene.materials.materialData[materialID].header;

    bool isAlphaTested = header.getAlphaMode() == AlphaMode::Mask;

    // needs alpha testing?
    if (isAlphaTested)
    {
        const VertexData v = gScene.getVertexData(hit);
        if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
            IgnoreHit(); // alpha test failed => ignore this triangle
    }

    frontFace = frontFace || isAlphaTested || header.isDoubleSided();
    if (!frontFace)
        IgnoreHit(); // this is just for rasterizer compability

    if (t <= rayData.tSphereStart)
    {
        rayData.tLastFrontFaceHalo = max(rayData.tLastFrontFaceHalo, t);
        if (t >= rayData.tConstRadiusStart)
            AcceptHitAndEndSearch(); // we can stop the query, because this will set the visibility to zero
    }
    else // inside sphere
    {
        rayData.tFirstFrontFaceInside = min(rayData.tFirstFrontFaceInside, t);
        return; // since we save the min, we can commit TMax here
    }
    IgnoreHit(); // continue traversal
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // is skipped
}

RaytracingAccelerationStructure nullAS;

[shader("raygeneration")]
void rayGen()
{
    uint2 svPos = DispatchRaysIndex().xy;
    float2 texC = (float2(svPos) + 0.5) * gData.invResolution;

    uint mask = aoMask[svPos] & 0xFF;
    if (mask == 0u)
        return;

    /*{ // dummy sync
        RayData dummy = { };
        RayDesc dummyDesc = { };
        TraceRay(nullAS, RAY_FLAG_NONE, 0, 0, 0, 0, dummyDesc, dummy);
    }*/

    // fetch linear depth a second time
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);

    // view space position of current pixel
    const float3 posV = UVToViewSpace(texC, linearDepth);
    const float posVLength = length(posV);

    // view space normal of current pixel
    float3 normalW = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0)
        normalV = -normalV;

    // Calculate tangent space (use random direction for tangent orientation)
    float2 randDir = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0).xy;
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
    [unroll]
    for (uint j = 0; j < NUM_DIRECTIONS; j++)
    {
        if (mask == 0u)
            break; // no bits set anymore

        // modify loop to only go through the set bits in mask
        //[loop] while ((mask & 1u) == 0u)
        //for (uint k = 0; k < (NUM_DIRECTIONS - j) && (mask & 1u) == 0u; k++) // this is too complicated for current compiler..  
        [unroll]
        for (uint k = 0; k < NUM_DIRECTIONS && k < NUM_DIRECTIONS - j && (mask & 1u) == 0u; k++) // first condition is for unrolling, second is for better unrolling
        {
        // shift mask an increase i
            mask = mask >> 1;
            ++i;
        }


        // random angle on view space disc
        float alpha = (float(i) / NUM_DIRECTIONS) * 2.0 * 3.141;
        float radius = sampleRadius[i] * gData.radius; // radius on sampling unit sphere * world space radius
        float2 dir = radius * float2(sin(alpha), cos(alpha)); // world space direction

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
        float3 initialSamplePosV = posV + tangent * dir.x + bitangent * dir.y;
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
        float curVisibility = 1.0f;

        float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
        const bool isInScreen = all(samplePosUV == screenUv);

        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

        if (SECONDARY_DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
        {
            // reuse old depth (this does not really cost much)
            if (PRIMARY_DEPTH_MODE == DEPTH_MODE_SINGLE)
            {
                float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            }
            else if (PRIMARY_DEPTH_MODE == DEPTH_MODE_DUAL)
            {
                float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex2);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            }

            float width, height;
            gDepthTex.GetDimensions(width, height);
            
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
        else if (SECONDARY_DEPTH_MODE == DEPTH_MODE_RAYTRACING)
        {
            // to be consistent with the rasterizer, we snap the uv coordinate as well to the pixel center,
            // but we do not clip it since we can shoot outside of the screen space
            //samplePosUV = getScreenClampedUV(texC, samplePosUV);
            samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center
            
            float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
            float initialSamplePosLength = length(initialSamplePosV);
            
            RayDesc ray;
            ray.Origin = gCamera.data.posW; // offset a little bit in normal direction
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
                //const float epsilon = 0.0;
                float objectSpaceZ;
                if (PRIMARY_DEPTH_MODE == DEPTH_MODE_SINGLE)
                    objectSpaceZ = calcObjectSpaceZ(posV, normal, samplePosUV, gDepthTex);
                else // if(PRIMARY_DEPTH_MODE == DEPTH_MODE_DUAL)
                    objectSpaceZ = calcObjectSpaceZ(posV, normal, samplePosUV, gDepthTex2);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                ray.TMin = max(ray.TMin, (posVLength - objectSpaceZ) * initialSamplePosLength / posVLength + epsilon);
            }

            RayData rayData;
            rayData.tLastFrontFaceHalo = tHaloStart; // min
            rayData.tFirstFrontFaceInside = tSphereEnd; // max
            rayData.tConstRadiusStart = tConstRadiusStart;
            rayData.tSphereStart = tSphereStart;
            TraceRay(
                gScene.rtAccel,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
                ray, rayData
            );

            // calculate visibility inside and outside of sphere
            float sphereVisibility = calcSphereVisibility(posVLength - rayData.tFirstFrontFaceInside * posVLength / initialSamplePosLength, sphereStart, sphereEnd);
            float haloVisibility = calcHaloVisibility(posVLength - rayData.tLastFrontFaceHalo * posVLength / initialSamplePosLength, sphereStart, sphereEnd);

            curVisibility = min(curVisibility, min(sphereVisibility, haloVisibility));
        }

        const float pdf = 2.0 * sphereHeight;
        visibility += curVisibility / pdf;
        
        // advance mask for next iteration
        mask = mask >> 1;
        ++i;
    }

    visibility *= 2.0 / float(NUM_DIRECTIONS);
    output[svPos] += visibility;
}
