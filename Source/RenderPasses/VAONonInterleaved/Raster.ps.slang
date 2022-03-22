#include "Common.slang"

struct PSOut
{
    float ao1 : SV_TARGET0;
    uint stencil : SV_TARGET1;

};

RWTexture2D<uint> gDepthAccess; // indicates depth accesses for the secondary pass

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

    [unroll] for (uint i = 0; i < NUM_DIRECTIONS; i++)
    {
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
        bool hybridRay = false;

        float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
        if (any(samplePosUV != screenUv) && SECONDARY_DEPTH_MODE == DEPTH_MODE_RAYTRACING)
            hybridRay = true; // always shoot rays for screen border
            
        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

      
        if (isSamePixel(rasterSamplePosUV, texC))
        {
            curVisibility = calcSphereVisibility(0.0, sphereStart, sphereEnd); // saves a texture fetch (but does not make a measuarbale diff on my device)
        }
        else
        {
            float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex);
            curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            // require hybrid ray if intersection is outside of sphere radius
            if (PRIMARY_DEPTH_MODE != DEPTH_MODE_DUAL && objectSpaceZ > sphereStart + CONST_RADIUS)
                hybridRay = true;

            if (PRIMARY_DEPTH_MODE == DEPTH_MODE_DUAL && objectSpaceZ > sphereStart + CONST_RADIUS)
            {
                float objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex2);
                curVisibility = min(curVisibility, calcVisibility(objectSpaceZ, sphereStart, sphereEnd));
                if (objectSpaceZ > sphereStart + CONST_RADIUS) // both samples were before the sphere starts
                    hybridRay = true;
            }
        }


        const float pdf = 2.0 * sphereHeight;
        if(hybridRay)
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
            output.ao1 += curVisibility / pdf;
        }
    }

    output.ao1 *= 2.0 / float(NUM_DIRECTIONS);

    if(output.stencil == 0)
        output.ao1 = pow(output.ao1, gData.exponent);

    return output;
}
