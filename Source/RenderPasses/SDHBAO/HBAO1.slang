#include "Common.slang"

RWTexture2D<uint> gRayMinAccess;
RWTexture2D<uint> gRayMaxAccess;

RWTexture2DArray<unorm float> gAmbientOut;
RWTexture2DArray<uint> gAoMask;

struct PSOut
{
    float ao1 : SV_TARGET0;
    STENCIL(uint stencil : SV_TARGET1);
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    PSOut output;
    output.ao1 = 0.0;
    STENCIL(output.stencil = 0);

    float2 svPos = id.xy * 4.0 + uint2(id.z % 4, id.z / 4) + 0.5 + guardBand;
    float2 texC = svPos.xy * gData.invResolution;
    float linearDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, float3(texC, id.z), 0).r;
    // radius in full-res pixels
    float RadiusInPixels = GetAORadiusInPixels(linearDepth);
    
    // early out when radius is too small
    [branch]
    if (linearDepth >= gCamera.data.farZ || RadiusInPixels < 1.0)
    {
        output.ao1 = 1.0;
    }
    else
    {
        const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

        // view space normal of current pixel
        float3 WorldNormal = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
        float3 ViewNormal = mul(float3x3(gCamera.data.viewMat), WorldNormal);
        if (dot(ViewPosition, ViewNormal) > 0.0)
            ViewNormal = -ViewNormal;

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
            float2 Direction = Rotate2D(Rand[id.z].xy, Angle);

            // Jitter starting sample within the first step
            float RayPixels = (Rand[id.z].z * StepSizePixels + 1.0);

            // sample NUM_STEPS steps for each direction
            for (int step = 0; step < NUM_STEPS; ++step)
            {
                // calculate sample position in uv space (round ray pixels to sample in pixel center)
                float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invQuarterResolution;
                float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, float3(snappedSampleUV, id.z), 0);
                float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                float ao = 0.0;
                if (RecomputeAO(ViewPosition, ViewNormal, S, ao) && DEPTH_MODE == DEPTH_MODE_STOCHASTIC && RayPixels * 4.0 > 6.0)
                {
                    // calculate intersection with sphere
                    IntersectionResult ir = CalculateIntersections(ViewPosition, ViewNormal, S, gData.radius);
                    if (ir.valid)
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
                RayPixels += StepSizePixels;
            }
        }

        output.ao1 /= (NUM_DIRECTIONS * NUM_STEPS);

        STENCIL(if(output.stencil == 0))
        {
            float res = saturate(1.0 - output.ao1 * 2.0);
            output.ao1 = pow(res, gData.powerExponent);
        }
    }

    id.xy += guardBand / 4;
    gAmbientOut[id] = output.ao1;
    STENCIL(gAoMask[id] = output.stencil);
}
