import Scene.Camera.Camera;
import SSAOData;

// single depth texture
#define DEPTH_MODE_SINGLE 0
// two depth textures
#define DEPTH_MODE_DUAL 1
// single depth texture + stochastic depth texture
#define DEPTH_MODE_STOCHASTIC 2

#define AO_MITTRING 0
#define AO_FILION 1
#define AO_HBAO 2
#define AO_HBAOPlus 3
#define AO_VAO 4

#ifndef DEPTH_MODE
#define DEPTH_MODE DEPTH_MODE_DUAL
#endif

#ifndef AO_ALGORITHM
#define AO_ALGORITHM AO_MITTRING
#endif

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 1
#endif

cbuffer StaticCB
{
    SSAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

Texture2D<float> gDepthTex;
Texture2D<float> gDepthTex2;
Texture2DMS<float> gsDepthTex;

Texture2D gNormalTex;
Texture2D gNoiseTex; // xy = random normalized direciton, zw = normalized random number => SNORM texture format

Texture1D gSpherePositions; // random positions inside the unit sphere

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
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

// snap uv to pixel center (and clip to screen)
float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelCoord = floor(saturate(uv) * float2(width, height));
    return float2((pixelCoord.x + 0.5f) / width, (pixelCoord.y + 0.5f) / height);
}

float GetAORadiusInPixels(float ViewDepth)
{
    float uvRadius = (gData.radius * gCamera.data.focalLength) / (ViewDepth * gCamera.data.frameWidth);
    return uvRadius * gData.resolution.x;
}

// distance falloff function
float Falloff(float DistanceSquare)
{
    // 1.0 - (d*d)/(r*r)
    return DistanceSquare * gData.negInvRsq + 1.0;
    //return 1.0 - DistanceSquare / (gData.radius * gData.radius);

}

// ambient occlusion kernel
// P: view space position
// N: view space normal
// S: view space sample position
float ComputeAO(float3 P, float3 N, float3 S)
{
    float3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * rsqrt(VdotV);

    return saturate(NdotV - gData.NdotVBias) * saturate(Falloff(VdotV));
}

// returns true if the given sample requires more depth layers
bool RecomputeAO(float3 P, float3 N, float3 S, inout float ao)
{
    float3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * rsqrt(VdotV);

    float angleTerm = saturate(NdotV - gData.NdotVBias);
    float distanceTerm = saturate(Falloff(VdotV));
    ao = angleTerm * distanceTerm;
    return angleTerm > 0.0 && distanceTerm <= 0.0; // sample is in positive hemisphere but further away than the radius
}

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_TARGET0
{
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0).r;
    if (linearDepth >= gCamera.data.farZ)
        return 1.0f;

    const float3 ViewPosition = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 WorldNormal = gNormalTex.Sample(gTextureSampler, texC).xyz;
    float3 ViewNormal = mul(WorldNormal, float3x3(gCamera.data.viewMat));
    if (dot(ViewPosition, ViewNormal) > 0.0)
        ViewNormal = -ViewNormal;

    float RadiusInPixels = GetAORadiusInPixels(linearDepth);
    
    // early out when radius is too small
    //[branch]
    //if (RadiusInPixels < 1.0)
    //    return 1.0;

    // random uniformly distributed numbers
    float4 Rand = gNoiseTex.Sample(gNoiseSampler, texC * gData.noiseScale);
    
    if (AO_ALGORITHM == AO_MITTRING)
    {
        int numOccluded = 0;
        float3 randPlaneNormal = normalize(Rand.xyz * 2.0 - float3(1.0));
        float epsilon = gData.radius * gData.NdotVBias;
        
        // random point in the sphere
        for (int i = 0; i < gData.numSamples; ++i)
        {
            float3 rndDir = gSpherePositions[i].xyz;
            rndDir = reflect(rndDir, randPlaneNormal); // reflect on plane
            float3 samplePosition = ViewPosition + rndDir * gData.radius; // TODO orient in tangent space
            float2 sampleUV = getSnappedUV(ViewSpaceToUV(samplePosition));
            float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, sampleUV, 0);

            if (-samplePosition.z > linearSampleDepth + epsilon) // sample depth is positive, but sample pos is negative (inverse)
                ++numOccluded;

        }

        return pow(1.0 - ((float) numOccluded / gData.numSamples), gData.powerExponent);
    }

    if (AO_ALGORITHM == AO_FILION)
    {
        float occlusion = 0.0;
        float3 randPlaneNormal = normalize(Rand.xyz * 2.0 - float3(1.0));
        float epsilon = gData.radius * gData.NdotVBias;
        
        // random point in the sphere
        for (int i = 0; i < gData.numSamples; ++i)
        {
            float3 rndDir = gSpherePositions[i].xyz;
            rndDir *= (length(rndDir) * 0.5 + 0.5) / length(rndDir); // renormalize sample vector to length [0.5, 1.0] (currently [0.01, 1.0])
            rndDir = reflect(rndDir, randPlaneNormal); // reflect on plane
            if (dot(rndDir, ViewNormal) < 0.0) // flip if below surface normal
                rndDir = -rndDir;
            float3 samplePosition = ViewPosition + rndDir * gData.radius; // TODO orient in tangent space
            float2 sampleUV = getSnappedUV(ViewSpaceToUV(samplePosition));
            float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, sampleUV, 0);

            
            float dz = -samplePosition.z - linearSampleDepth;
            if (dz > epsilon) // sample depth is positive, but sample pos is negative (inverse)
                //occlusion += saturate(Falloff(dz * dz)); // (not the same falloff as in the paper)
                occlusion += saturate(exp(-(dz - epsilon) / gData.radius * 4.6 * 0.5)); // e^-4.6 = 0.01 ~ 0.0
                //occlusion += 1.0;
        }

        return pow(1.0 - occlusion / gData.numSamples, gData.powerExponent);
    }

    

    
    if (AO_ALGORITHM == AO_HBAOPlus)
    {
        const int NUM_STEPS = 4; // steps per direction
        const int NUM_DIRECTIONS = (gData.numSamples + NUM_STEPS - 1) / NUM_STEPS;
        
       // Compute AO
        // Divide by NUM_STEPS+1 so that the farthest samples are not fully attenuated
        float StepSizePixels = RadiusInPixels / (NUM_STEPS + 1);

        // TODO replace with texture lookup
        
        //Rand = float4(1, 0, 0.5, 1);
        const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;
        //const float AngleOffset = 2.0f * 3.141f * Rand.w;

        float AO = 0.0;
        // sample NUM_DIRECTIONS directions on the view space disc


        for (int i = 0; i < NUM_DIRECTIONS; ++i)
        {
            float Angle = Alpha * float(i); // + AngleOffset;

            // random normalized 2D direction
            float2 rndDir = float2(sin(Rand.x * 2.0 * 3.141), cos(Rand.x * 2.0 * 3.141));
            float2 Direction = Rotate2D(rndDir, Angle);

            // Jitter starting sample within the first step
            float RayPixels = (Rand.z * StepSizePixels + 1.0);

            // sample NUM_STEPS steps for each direction
            for (int step = 0; step < NUM_STEPS; ++step)
            {
                // calculate sample position in uv space (round ray pixels to sample in pixel center)
                float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invResolution;
                if (DEPTH_MODE == DEPTH_MODE_SINGLE)
                {
                    float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                    // TODO? could check for depth == farZ
                    float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                    AO += ComputeAO(ViewPosition, ViewNormal, S);
                }
                else if (DEPTH_MODE == DEPTH_MODE_DUAL)
                {
                    float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                    // TODO? could check for depth == farZ
                    float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                    float ao = 0.0;
                    [branch]
                    if (RecomputeAO(ViewPosition, ViewNormal, S, ao))
                    {
                        float linearSampleDepth = gDepthTex2.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                        float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                        ao = max(ao, ComputeAO(ViewPosition, ViewNormal, S));
                    }
                    AO += ao;
                }
                else if (DEPTH_MODE == DEPTH_MODE_STOCHASTIC)
                {
                    float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                    // TODO? could check for depth == farZ
                    float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                    float ao = 0.0;
                    [branch]
                    if (RecomputeAO(ViewPosition, ViewNormal, S, ao))
                    {
                        // TODO try to use sampler
                        int2 pixelCoord = int2(floor(snappedSampleUV * gData.resolution));
                        const float depthRange = gCamera.data.farZ - gCamera.data.nearZ;
                        const float depthOffset = gCamera.data.nearZ;
                        [unroll]
                        for (uint i = 0; i < MSAA_SAMPLES; ++i)
                        {
                            float linearSampleDepth = gsDepthTex.Load(pixelCoord, i);
                        // linearSampleDepth is in [0, 1] => scale accordingly
                            linearSampleDepth = linearSampleDepth * depthRange + depthOffset;
                            float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                            ao = max(ao, ComputeAO(ViewPosition, ViewNormal, S));
                        }
                    }
                    AO += ao;
                }

                RayPixels += StepSizePixels;
            }
        }

        AO /= (NUM_DIRECTIONS * NUM_STEPS);
    // artistic modifications
        float res = saturate(1.0 - AO * 2.0);
        res = pow(res, gData.powerExponent);

        return res;
    }

    return 0.0;
}
