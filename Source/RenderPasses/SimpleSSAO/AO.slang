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
Texture1D<float2> gDiscPositions; // random positions inside the unit disc

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

float2 GetRandomVector2D(float rand)
{
    return float2(sin(rand * 2.0 * 3.141), cos(rand * 2.0 * 3.141));
}

float3 GetRandomVector3D(float2 rand)
{
    float phi = rand.x * 2.0 * 3.141;
    float theta = acos(rand.y * 2.0 - 1.0);
    return float3(
        sin(theta) * cos(phi),
        sin(theta) * sin(phi),
        cos(theta)
    );
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

float makeNonZero(float value, float epsilon)
{
    float absValue = max(abs(value), epsilon);
    return value >= 0 ? absValue : -absValue;
}

float calcObjectSpaceZ(float3 posV, float3 normal, float2 uv, Texture2D<float> depthTex)
{
    float linearSampleDepth = depthTex.SampleLevel(gTextureSampler, uv, 0);
    float3 samplePosV = UVToViewSpace(uv, linearSampleDepth);
    // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
    float objectSpaceZ = dot(samplePosV - posV, normal);
    return objectSpaceZ;
}

// VAO methods -- start

#define HALO_RADIUS (sphereStart - sphereEnd)
#define CONST_RADIUS (gData.radius - sphereStart)

// get rid of shadowing around edges
// introduce a linear falloff function that starts with 0.0 when the sample depth intersects the front sphere exactly,
// and falls of to 1.0 when it gets further away from the sphere but closer to the camera
float calcHaloVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    //if (!PREVENT_DARK_HALOS)
    //    return 0.0;
    
    return saturate((objectSpaceZ - sphereStart - CONST_RADIUS) / HALO_RADIUS)
        * (sphereStart - sphereEnd); // this adjust the visibility to the sampling (hemi-)sphere
}

float calcSphereVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    float sampleRange = max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
    return sampleRange;
}

float calcVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return calcSphereVisibility(objectSpaceZ, sphereStart, sphereEnd)
         + calcHaloVisibility(objectSpaceZ, sphereStart, sphereEnd);
}


// VAO methods -- end

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
        float3 randPlaneNormal = GetRandomVector3D(Rand.xy);
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
        float3 randPlaneNormal = GetRandomVector3D(Rand.xy);
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
            float curOcclusion = 0.0;
            
            if (dz > epsilon)
                //curOcclusion = saturate(Falloff(dz * dz)); // (not the same falloff as in the paper)
                curOcclusion = saturate(exp(-(dz - epsilon) / gData.radius * 4.6 * 0.5)); // e^-4.6 = 0.01 ~ 0.0;
             

            if (DEPTH_MODE == DEPTH_MODE_DUAL)
            {
                linearSampleDepth = gDepthTex2.SampleLevel(gTextureSampler, sampleUV, 0);
                dz = -samplePosition.z - linearSampleDepth;
                if (dz > epsilon)
                    curOcclusion = max(curOcclusion, saturate(exp(-(dz - epsilon) / gData.radius * 4.6 * 0.5))); // e^-4.6 = 0.01 ~ 0.0;    
            
            }
            occlusion += curOcclusion;
        }

        return pow(saturate(1.0 - occlusion / gData.numSamples), gData.powerExponent);
    }

    if (AO_ALGORITHM == AO_HBAO)
    {
        const int NUM_STEPS = 4; // steps per direction
        const int NUM_DIRECTIONS = (gData.numSamples + NUM_STEPS - 1) / NUM_STEPS;
        float StepSizePixels = RadiusInPixels / (NUM_STEPS + 1);
        const float Alpha = 2.0 * 3.141f / NUM_DIRECTIONS;
        float AO = 0.0;

        for (int i = 0; i < NUM_DIRECTIONS; ++i)
        {
            float Angle = Alpha * float(i); // + AngleOffset;

            // random normalized 2D direction
            float2 rndDir = GetRandomVector2D(Rand.x);
            float2 Direction = Rotate2D(rndDir, Angle);

            // Jitter starting sample within the first step
            float RayPixels = (Rand.z * StepSizePixels + 1.0);

            // sample NUM_STEPS steps for each direction
            float minTheta = 1.0; // corresponds to the max horizon angle
            float curFalloff = 1.0;
            for (int step = 0; step < NUM_STEPS; ++step)
            {
                // calculate sample position in uv space (round ray pixels to sample in pixel center)
                float2 snappedSampleUV = texC + round(RayPixels * Direction) * gData.invResolution;
                if (DEPTH_MODE == DEPTH_MODE_SINGLE)
                {
                    float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                    float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                    float dist = distance(S, ViewPosition);
                    float NdotV = max(dot(S - ViewPosition, ViewNormal) / dist, 0.0);
                    float falloff = saturate(1.0 - dist / gData.radius);
                    AO += NdotV * falloff;
                    //if(NdotV < minTheta)
                    //{
                    //    minTheta = NdotV;
                    //    curFalloff = falloff;
                    //}
                }
                else if (DEPTH_MODE == DEPTH_MODE_DUAL)
                {
                    float linearSampleDepth = gDepthTex.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                    float3 S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                    float dist = distance(S, ViewPosition);
                    float NdotV = max(dot(S - ViewPosition, ViewNormal) / dist, 0.0);
                    float falloff = saturate(1.0 - dist / gData.radius);
                    float ao = NdotV * falloff;
                    if(dist > gData.radius && NdotV > 0.0)
                    {
                        linearSampleDepth = gDepthTex2.SampleLevel(gTextureSampler, snappedSampleUV, 0);
                        S = UVToViewSpace(snappedSampleUV, linearSampleDepth);
                        dist = distance(S, ViewPosition);
                        NdotV = max(dot(S - ViewPosition, ViewNormal) / dist, 0.0);
                        falloff = saturate(1.0 - dist / gData.radius);
                        ao = max(ao, NdotV * falloff); // take max occlusion of both depth samples
                    }
                    AO += ao;
                }
                RayPixels += StepSizePixels;
            }
            //AO += minTheta * curFalloff * NUM_STEPS;
        }
        AO /= (NUM_DIRECTIONS * NUM_STEPS);
        return pow(saturate(1.0 - 2.0 * AO), gData.powerExponent);
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
            float2 rndDir = GetRandomVector2D(Rand.x);
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

    if (AO_ALGORITHM == AO_VAO)
    {

        
        // determine tangent space
        float3 normal = -normalize(ViewPosition);
        float3 bitangent = normalize(cross(normal, float3(GetRandomVector2D(Rand.x), 0.0f)));
        float3 tangent = cross(bitangent, normal);

        // transfer view space normal to normal in object coordinates of the sampling sphere
        float3 normalO = float3(dot(ViewNormal, tangent), dot(ViewNormal, bitangent), dot(ViewNormal, normal));

        float visibility = 0.0f;

        for (uint i = 0; i < gData.numSamples; i++)
        {
            // obtain sample position on disc around view space position
            float2 rand = gDiscPositions[i]; // xy = random location on unit disc, zw = uniform in 0,1
            rand *= gData.radius; // multiply 2D position with sample radius
        
            // height of the sphere at the requested sample position (not at the actual sampling position)
            const float sphereHeight = sqrt(gData.radius * gData.radius - dot(rand, rand));
            // probability for choosing this sample
            const float pdf = 2.0 * sphereHeight;

            // determine distance within [-sphereHeight, +sphereHeight]
            float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
            float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)

            // determine correct sphereStart and sphereEnd for hemisphere
            float zIntersect = -dot(rand.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            if (normalO.z >= 0.0)
                sphereEnd = zIntersectClamped;
            else
                sphereStart = zIntersectClamped;

            // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
            if (sphereStart - sphereEnd < 0.01)
                continue;
            

            // calculate view position of sample and project to uv coordinates
            float3 initialSamplePosV = ViewPosition + tangent * rand.x + bitangent * rand.y;
            float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
            float curVisibility = 1.0f;

            // clip sample position uv and snap to pixel center
            float2 screenUv = getSnappedUV(samplePosUV); // clip to screen border and pixel center

            // primary depth sample
            float objectSpaceZ = calcObjectSpaceZ(ViewPosition, normal, samplePosUV, gDepthTex);
            curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);
            bool primaryIsOccluded = (objectSpaceZ > sphereStart + CONST_RADIUS);

            if (DEPTH_MODE == DEPTH_MODE_DUAL && primaryIsOccluded)
            {
            // obtain secondary sample if primary sample is occluded
                objectSpaceZ = calcObjectSpaceZ(ViewPosition, normal, samplePosUV, gDepthTex2);
                curVisibility = min(curVisibility, calcVisibility(objectSpaceZ, sphereStart, sphereEnd));
            }
            visibility += curVisibility / pdf;
        }
        // since fully visibile are all values in [0.5, 1.0], scale accordingly
        return pow(saturate(visibility / gData.numSamples * 2.0), gData.powerExponent);
    }
    return 0.0;
}
