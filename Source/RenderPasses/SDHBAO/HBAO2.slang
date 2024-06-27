#include "Common.slang"

Texture2D<float> gAO1;
Texture2D<uint> gAoMask;
Texture2DArray<float4> gsDepthTex; // stochastic depth map

RWTexture2D<unorm float> gAmbientOut;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint stencil = gAoMask[id.xy + guardBand / 4];
    if (stencil == 0)
        return;

    float2 svPos = id.xy * 4.0 + quarterOffset + 0.5 + guardBand;
    float2 texC = svPos.xy * gData.invResolution;
    float linearDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, texC, 0).r;
    const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 WorldNormal = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
    float3 ViewNormal = mul(float3x3(gCamera.data.viewMat), WorldNormal);
    if (dot(ViewPosition, ViewNormal) > 0.0)
        ViewNormal = -ViewNormal;

    // radius in full-res pixels
    float RadiusInPixels = GetAORadiusInPixels(linearDepth);

    // Compute AO
    // Divide by NUM_STEPS+1 so that the farthest samples are not fully attenuated
    // divide by 4 for step size in quarter res pixels
    float StepSizePixels = (RadiusInPixels / 4.0) / (NUM_STEPS + 1);

    const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;

    float aoUpdate = 0.0;
    
    uint i = 0; // direction
    uint step = 0; // step
    [loop]
    while(stencil != 0u)
    {
        //for (uint k = 0; k < NUM_DIRECTIONS * NUM_STEPS && (stencil & 1u) == 0u; ++k)
        while (i < NUM_DIRECTIONS && (stencil & 1u) == 0u)
        {
            ++step;
            if (step >= NUM_STEPS)
            {
                step = 0;
                ++i;
            }
            stencil = stencil >> 1;
        }

        float Angle = Alpha * float(i);

        // random normalized 2D direction
        float2 Direction = Rotate2D(Rand.xy, Angle);

        // Jitter starting sample within the first step
        float RayPixels = (Rand.z * StepSizePixels + 1.0) + step * StepSizePixels;

        // handle the step
        {
            // calculate sample position in uv space (round ray pixels to sample in pixel center)
            float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invQuarterResolution;
            float linearSampleDepth = gDepthTexQuarter.SampleLevel(gTextureSampler, snappedSampleUV, 0);
            float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
            float ao = ComputeAO(ViewPosition, ViewNormal, S);
            float oldAo = ao;
            if(snappedSampleUV.x < 0.0 || snappedSampleUV.x > 1.0 ||
                snappedSampleUV.y < 0.0 || snappedSampleUV.y > 1.0)
                ao = 0.0; // reset ao because out of screen

            // add stochastic depth samples
            uint2 pixel = UVToSDPixel(snappedSampleUV);
            const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
            const float depthOffset = gCamera.data.nearZ;
            float4 sddepths = gsDepthTex[uint3(pixel, 0)];
            [unroll]
            for (uint i = 0; i < 4; ++i)
            {
                linearSampleDepth = sddepths[i];
                linearSampleDepth = linearSampleDepth * depthRange + depthOffset;
                float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                ao = max(ao, ComputeAO(ViewPosition, ViewNormal, S));
            }
            aoUpdate += ao - oldAo;
        }

        // update step
        ++step;
        if (step >= NUM_STEPS)
        {
            step = 0;
            ++i;
        }
        stencil = stencil >> 1;
    }

    aoUpdate /= (NUM_DIRECTIONS * NUM_STEPS);

    float res = gAO1[id.xy + guardBand / 4];
    res += aoUpdate;
    res = saturate(1.0 - res * 2.0);
    res = pow(res, gData.powerExponent);
   
    gAmbientOut[id.xy + guardBand / 4] = res;
}
