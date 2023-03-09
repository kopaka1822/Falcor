#include "Common.slang"
#include "NeuralNetDefines.slangh"
#include "NeuralNetDefines2.slangh"

struct PSOut
{
    float ao1 : SV_TARGET0;
    uint stencil : SV_TARGET1;

};

RWTexture2D<uint> gDepthAccess; // indicates depth accesses for the secondary pass
#define MTL_DOUBLE_SIDED 1
Texture2D<uint> gMatDoubleSided; // double sided flag

PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION)
{
    PSOut output;
    output.ao1 = 0.0;
    output.stencil = 0;

    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);
    if (linearDepth >= gCamera.data.farZ)
    {
        output.ao1 = 1.0;
        return output;
    }

    // view space position of current pixel
    const float3 posV = UVToViewSpace(texC, linearDepth);
    const float posVLength = length(posV);

    // view space normal of current pixel
    float3 normalW = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0)
        normalV = -normalV;

    // Calculate tangent space (use random direction for tangent orientation)
    float randRotation = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0) * 2.0 * 3.141;
    float2 randDir = float2(sin(randRotation), cos(randRotation));
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 normal = -posV / posVLength;
    float3 bitangent = normalize(cross(normal, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, normal);

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, normal));
    
    float visibility = 0.0f;

    #if PRIMARY_DEPTH_MODE >= DEPTH_MODE_MACHINE_CLASSIFY
    float rasterHeights[8];
    float tmpAoValues[8];
    #endif

    uint forceRayMask = 0; // forces rays due to double sided materials
    
    [unroll] for (uint i = 0; i < NUM_DIRECTIONS; i++)
    {
        // random angle on view space disc
        float alpha = (float(i) / NUM_DIRECTIONS) * 2.0 * 3.141;
        float radius = sampleRadius[i] * gData.radius; // radius on sampling unit sphere * world space radius
        float2 dir = radius * float2(sin(alpha), cos(alpha));  // world space direction

        const float sphereHeight = sqrt(gData.radius * gData.radius - radius * radius);
        const float pdf = 2.0 * sphereHeight;
        
        // determine distance within [-sphereHeight, +sphereHeight]
        float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
        float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

        { // HEMISPHERE SAMPLING
            //float zIntersect = -dot(rand.xy, normalO.xy) / normalO.z;
            float zIntersect = -dot(dir.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            sphereEnd = zIntersectClamped;
        }
        
        // sample position calculate uv position of sample
        float2 samplePosUV = ViewSpaceToUV(posV + tangent * dir.x + bitangent * dir.y);

        float curVisibility = 0.0f;
        bool requireRay = false;
        bool forceRay = false;

        float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
        if (any(samplePosUV != screenUv) && SECONDARY_DEPTH_MODE == DEPTH_MODE_RAYTRACING)
        {
            forceRay = true; // always shoot rays for screen border   
        }
            
        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

        float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex);
        curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd, pdf);

        #if PRIMARY_DEPTH_MODE >= DEPTH_MODE_MACHINE_CLASSIFY
        rasterHeights[i] = clamp(objectSpaceZ / gData.radius, -16.0, 16.0);
        tmpAoValues[i] = 0.0;
        #endif

        if ((sphereStart - sphereEnd) / pdf <= 0.1)
        {
            continue; // skip sample (no visibility)
        }
        
        // require hybrid ray if intersection is outside of sphere radius
        if (PRIMARY_DEPTH_MODE != DEPTH_MODE_DUAL && objectSpaceZ > sphereStart + CONST_RADIUS)
        {
            requireRay = true;
            #if PRIMARY_DEPTH_MODE == DEPTH_MODE_MACHINE_CLASSIFY
            // fetch material data to check for double sided flag
            if (gMatDoubleSided[UVToPixel(rasterSamplePosUV)] & MTL_DOUBLE_SIDED)
                forceRay = true;
            #endif
        }

        if (PRIMARY_DEPTH_MODE == DEPTH_MODE_DUAL && objectSpaceZ > sphereStart + CONST_RADIUS)
        {
            float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex2);
            curVisibility = COMBINE_VIS(curVisibility, calcVisibility(objectSpaceZ, sphereStart, sphereEnd, pdf));
            if (objectSpaceZ > sphereStart + CONST_RADIUS) // both samples were before the sphere starts
                requireRay = true;
        }

        #if PRIMARY_DEPTH_MODE >= DEPTH_MODE_MACHINE_CLASSIFY
        tmpAoValues[i] = curVisibility;
        #endif

        if (forceRay && (PRIMARY_DEPTH_MODE >= DEPTH_MODE_MACHINE_CLASSIFY)) 
        {
            forceRayMask |= (1u << i); 
        }
        else if(requireRay || forceRay)
        {
            // remember that this was a hybrid ray
            output.stencil |= 1u << i;
#if SECONDARY_DEPTH_MODE == DEPTH_MODE_STOCHASTIC
            // remember that an access was requested
            gDepthAccess[uint2(rasterSamplePosUV * gData.resolution)] = 1u;
#endif
        }
        else
        {
            output.ao1 += curVisibility;
        }
    }

    #if PRIMARY_DEPTH_MODE == DEPTH_MODE_MACHINE_CLASSIFY
    if (output.stencil != 0)
    {
        uint machineMask = evalClassifier(rasterHeights);
        output.stencil = output.stencil & machineMask;
    }
    output.stencil |= forceRayMask; // always trace the forced rays

    output.ao1 = 0.0;
    [unroll]
    for (uint i = 0; i < NUM_DIRECTIONS; ++i)
    {
        if (!(output.stencil & (1u << i)))
             output.ao1 += tmpAoValues[i];
    }
    #endif
    #if PRIMARY_DEPTH_MODE == DEPTH_MODE_MACHINE_PREDICT
    if(output.stencil != 0)
    {
        //output.stencil |= forceRayMask;
        evalRegressor(rasterHeights);

        [unroll]
        for (uint i = 0; i < NUM_DIRECTIONS; ++i)
        {
            if ((output.stencil & (1u << i)))
            {
                // calc new ao value from new height
                float alpha = (float(i) / NUM_DIRECTIONS) * 2.0 * 3.141;
                float radius = sampleRadius[i] * gData.radius; // radius on sampling unit sphere * world space radius
                float2 dir = radius * float2(sin(alpha), cos(alpha)); // world space direction

                const float sphereHeight = sqrt(gData.radius * gData.radius - radius * radius);
                const float pdf = 2.0 * sphereHeight;
        
                // determine distance within [-sphereHeight, +sphereHeight]
                float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
                float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

                float zIntersect = -dot(dir.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
                float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
                sphereEnd = zIntersectClamped;

                float objectSpaceZ = rasterHeights[i];
                float curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd, pdf);
                output.ao1 += curVisibility;
            }
        }
    }
    output.stencil = forceRayMask; // replace with forceRayMask
    #endif

    output.ao1 *= 2.0 / float(NUM_DIRECTIONS);

    if(output.stencil == 0)
        output.ao1 = pow(output.ao1, gData.exponent);

    return output;
}
