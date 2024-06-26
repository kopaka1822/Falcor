#include "Common.slang"

RWTexture2D<uint> gRayMinAccess;
RWTexture2D<uint> gRayMaxAccess;

struct PSOut
{
    float ao1;
    STENCIL(uint stencil);
};

PSOut main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    PSOut output;
    output.ao1 = 0.0;
    STENCIL(output.stencil = 0);

    // adjust texC for quarter resolution texcoord
    svPos.xy = floor(svPos.xy) * 4.0 + quarterOffset + 0.5;
    texC = svPos.xy * gData.invResolution;
    float linearDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
    {
        output.ao1 = 1.0;
        return output;
    }

    const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 WorldNormal = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 ViewNormal = mul(float3x3(gCamera.data.viewMat), WorldNormal);
    if (dot(ViewPosition, ViewNormal) > 0.0)
        ViewNormal = -ViewNormal;

    // radius in full-res pixels
    float RadiusInPixels = GetAORadiusInPixels(linearDepth);
    
    // early out when radius is too small
    [branch]
    if (RadiusInPixels < 1.0)
    {
        output.ao1 = 1.0;
        return output;
    }

    // Compute AO
    // Divide by NUM_STEPS+1 so that the farthest samples are not fully attenuated
    // divide by 4 for step size in quarter res pixels
    float StepSizePixels = (RadiusInPixels / 4.0) / (NUM_STEPS + 1);

    const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;

    // sample NUM_DIRECTIONS directions on the view space disc
    [unroll]
    for (int i = 0; i < NUM_DIRECTIONS; ++i)
    {
        float Angle = Alpha * float(i);

        // random normalized 2D direction
        float2 Direction = Rotate2D(Rand.xy, Angle);

        // Jitter starting sample within the first step
        float RayPixels = (Rand.z * StepSizePixels + 1.0);

        // sample NUM_STEPS steps for each direction
        for (int step = 0; step < NUM_STEPS; ++step)
        {
            // calculate sample position in uv space (round ray pixels to sample in pixel center)
            float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invQuarterResolution;
            float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, snappedSampleUV, 0);
            float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
            float ao = 0.0;
            if(RecomputeAO(ViewPosition, ViewNormal, S, ao) && DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
            {
                // calculate intersection with sphere
                IntersectionResult ir = CalculateIntersections(ViewPosition, ViewNormal, S, gData.radius);
                if(ir.valid)
                {
                    STENCIL(output.stencil |= 1u << (i * NUM_STEPS + step));
                    uint rayMin, rayMax;
                    rayMin = asuint(ir.tStart);
                    rayMax = asuint(ir.tEnd);
                    uint2 pixel = UVToSDPixel(snappedSampleUV);
                    InterlockedMin(gRayMinAccess[pixel], rayMin);
                    InterlockedMax(gRayMaxAccess[pixel], rayMax);
                }
            }
            output.ao1 += ao;
            
            /*else if (DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
            {
                float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                
                float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                float2 ao = 0.0;
                [branch]
                if (RecomputeAO(ViewPosition, ViewNormal, S, ao))
                {
#ifdef STOCHASTIC_ARRAY
                    int2 pixelCoord = int2(floor(snappedSampleUV * gData.resolution * 0.25));
#else
                    int2 pixelCoord = int2(floor(snappedSampleUV * gData.resolution));
#endif
                    const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
                    const float depthOffset = gCamera.data.nearZ;
                    [unroll]
                    for (uint i = 0; i < MSAA_SAMPLES; ++i)
                    {
#ifdef STOCHASTIC_ARRAY
                        float linearSampleDepth = gsDepthTex.Load(int3(pixelCoord, sliceIndex), i);
#else
                        float linearSampleDepth = gsDepthTex.Load(int3(pixelCoord, 0), i);
#endif
                        // linearSampleDepth is in [0, 1] => scale accordingly
                        linearSampleDepth = linearSampleDepth * depthRange + depthOffset;
                        float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                        ao = max(ao, ComputeAO(ViewPosition, ViewNormal, S));
                    }
                }
                AO += ao;
            }*/

            RayPixels += StepSizePixels;
        }
    }

    output.ao1 /= (NUM_DIRECTIONS * NUM_STEPS);

    STENCIL(if(output.stencil == 0))
    {
        float res = saturate(1.0 - output.ao1 * 2.0);
        output.ao1 = pow(res, gData.powerExponent);
    }
    

    return output;
}
