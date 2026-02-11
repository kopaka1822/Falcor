//import Utils.Math.MathHelpers;

RWTexture2D<float2> gMotion;
Texture2D<float2> gBackupMotion;
Texture2D<uint> gMotionError;
Texture2D<float3> gColor;
Texture2D<float3> gPrevColor;

SamplerState S; // linear sampler

cbuffer PerFrame
{
    uint2 gFrameDim;
    float2 gPrevJitter;
    float2 gCurJitter;
}

// snaps uv to top left (smallest) uv within the 2x2 pixel area
float2 snapTopLeft(float2 uv)
{
    return (floor(uv * float2(gFrameDim) - 0.5) + 0.5) / float2(gFrameDim);
}

// adjust jitter for prev-lookup
float2 adjustJitter(float2 mvec)
{
    return mvec - gPrevJitter + gCurJitter;
}

void calcStats(float2 texC, float2 step, Texture2D<float3> tex, out float mu, out float sigma)
{
    float sum = 0.0;
    float sumSq = 0.0;
    float wSum = 0.0;
    const float3 lum = float3(0.299, 0.587, 0.114);
#define RADIUS 5
    for (int dx = -RADIUS; dx <= RADIUS; ++dx)
        for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            float variance = 1.5 * 1.5;
            float w = exp(-0.5 * (dx * dx + dy * dy) / variance); // gaussian weight
            float3 c = tex.SampleLevel(S, texC + step * float2(dx, dy), 0.0);
            float g = dot(c, lum);
            sum += w * g;
            sumSq += w * g * g;
            wSum += w;
        }

    mu = sum / wSum;
    sigma = sumSq / wSum - mu * mu;
}

// similar to SSIM but without the structure term (since covariance is a bother to compute)
// returns a similarity score in [0,1], higher is more similar
float calcSim(float mu1, float mu2, float sigma1, float sigma2)
{
    float C1 = 0.0001; // for dynamic range of [0,1]
    float C2 = 0.0009;
    float lum = (2.0 * mu1 * mu2 + C1) / (mu1 * mu1 + mu2 * mu2 + C1);
    float contrast = (2.0 * sqrt(sigma1) * sqrt(sigma2) + C2) / (sigma1 + sigma2 + C2);
    return lum * contrast;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    if (gMotionError[pixel] == 0) return; // frozen => dont write
    
    const float2 texC = (float2(pixel) + 0.5) / float2(gFrameDim);
    float2 mvec = gMotion[pixel];
    float2 bmvec = gBackupMotion[pixel];

    const float2 step = 1.0 / float2(gFrameDim);

    // obtain local statistics
    float mu, sigma;
    calcStats(texC, step, gColor, mu, sigma);
    float mvecMu, mvecSigma;
    calcStats(texC + adjustJitter(mvec), step, gPrevColor, mvecMu, mvecSigma);
    float bmvecMu, bmvecSigma;
    calcStats(texC + adjustJitter(bmvec), step, gPrevColor, bmvecMu, bmvecSigma);

    float mSim = calcSim(mu, mvecMu, sigma, mvecSigma);
    float bSim = calcSim(mu, bmvecMu, sigma, bmvecSigma);

    if (bSim > (mSim))
    {
        mvec = bmvec;
    }

    gMotion[pixel] = mvec;
}
