#include "Common.slang"

struct PSOut
{
    float ao1 : SV_TARGET0;
    float ao2 : SV_TARGET1;
    uint stencil : SV_TARGET2;
};

RWTexture2D<uint> gDepthAccess; // indicates depth accesses for the secondary pass

// additional depth textures (TODO remove?)
Texture2D gDepthTex2;
Texture2DMS<float> gsDepthTex;

PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION)
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
    {
        PSOut output;
        output.ao1 = 1.0;
        output.ao2 = 0.0;
        output.stencil = 0;
        return output;
    }

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
    PSOut output;
    output.ao1 = 0.0;
    output.ao2 = 0.0;
    output.stencil = 0;

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
        //if (any(samplePosUV != screenUv))
        //    hybridRay = true; // always shoot rays for screen border
            
        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

        { // RASTER PART
            // clip sample position uv and snap to pixel center
            
            //if (isSamePixel(samplePosUV, texC))
                //objectSpaceZ = 0.0;            

                float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, rasterSamplePosUV, 0).r;
                float3 samplePosV = UVToViewSpace(rasterSamplePosUV, linearSampleDepth);
                // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
                float objectSpaceZ = dot(samplePosV - posV, normal);
                curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
                // require hybrid ray if intersection is outside of sphere radius
                if (objectSpaceZ > sphereStart + CONST_RADIUS)
                    hybridRay = true;
        }


        const float pdf = 2.0 * sphereHeight;
        if(hybridRay)
        {
            output.ao2 += curVisibility / pdf;
            // remember that this was a hybrid ray
            output.stencil |= 1u << i;
            // remember that an access was requested
            gDepthAccess[uint2(rasterSamplePosUV * gData.resolution)] = 1u;
            //gDepthAccess[uint2(rasterSamplePosUV * gData.resolution)] += 1u;
            //InterlockedAdd(gDepthAccess[uint2(rasterSamplePosUV * gData.resolution)], 1u);
        }
        else
        {
            output.ao1 += curVisibility / pdf;
        }
        //visibility += curVisibility / pdf;
    }



    //float AO = visibility / float(NUM_DIRECTIONS);
    // since fully visibile are all values in [0.5, 1.0], scale accordingly
    //AO = saturate(AO * 2.0);
    // do artistic modifications
    //AO = pow(AO, gData.exponent);

    //return AO;

    output.ao1 *= 2.0 / float(NUM_DIRECTIONS);
    output.ao2 *= 2.0 / float(NUM_DIRECTIONS);

    //output.ao1 += output.ao2;
    return output;
}
