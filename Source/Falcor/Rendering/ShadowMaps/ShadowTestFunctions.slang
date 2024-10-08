#include "Utils/Math/MathConstants.slangh"
import ShadowMapData;

#ifndef SM_EXPONENTIAL_CONSTANT
#define SM_EXPONENTIAL_CONSTANT 20.0
#endif
#ifndef SM_NEGATIVE_EXPONENTIAL_CONSTANT
#define SM_NEGATIVE_EXPONENTIAL_CONSTANT -5.0
#endif
#ifndef MSM_DEPTH_BIAS
#define MSM_DEPTH_BIAS 0.0
#endif
#ifndef MSM_MOMENT_BIAS
#define MSM_MOMENT_BIAS 0.005
#endif

static const float kExponentialSMConstant = SM_EXPONENTIAL_CONSTANT;    // Constant for Exponential Shadow Maps
static const float kEVSMNegConstant = SM_NEGATIVE_EXPONENTIAL_CONSTANT; // Negative Constant used in EVSM
static const float kMSMDepthBias = MSM_DEPTH_BIAS * 0.001f;   // Depth bias for the moment shadow map
static const float kMSMMomentBias = MSM_MOMENT_BIAS * 0.001f; // Moment bias for the moment shadow map


/** Exponential shadow test
    \param[in] local shadow map texture index.
    \param[in] cascaded level.
    \param[in] shadow map sample vector.
    \param[in] shadow map light type.
    \param[in] distance value of the current world position. The shadow map value is compaired against this
    \return 1 -> lit ; 0 -> shadowed
    */
float exponentialShadowTest(in const float shadowDepth, in const float lightDist)
{
    float depth = (lightDist * 2.f) - 1.f;
    return saturate(exp(-depth * kExponentialSMConstant) * shadowDepth);
}

/** ChebyshevUpperBound for Variance Shadow Mapping
    \param[in] M1 and M2 from Variance Shadow Mapping
    \param[in] depth of the current position
    \return shadow value between 1 and 0.
    */
float ChebyshevUpperBound(float2 moments, float mean, float minVariance)
{
    // Compute variance
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);

    // Compute probabilistic upper bound
    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));

    // One-tailed Chebyshev
    return (mean <= moments.x ? 1.0f : pMax);
}

/** Recreates EVSM float4 from a single depth value
    \param[in] Depth in [-1,1]
    \return float4 EVSM values
*/
float4 recreateEVSMFromSingleDepth(float depth){
    float4 evsm;
    evsm.x = exp(depth * kExponentialSMConstant);       //Moment1
    evsm.y = evsm.x * evsm.x;                           //Moment2
    evsm.z = -exp(-(depth * kEVSMNegConstant));         //Negative Moment1
    evsm.w = evsm.z * evsm.z;                           //Negative Moment2

    return evsm;
}

/** Exponential Variance shadow test
    \param[in] Exponentially warped shadow map depths.
    \param[in] camera distance from the perspective of the light.
    \return Upper-bound approximation of the positive exponent (x) and negative exponent (y)
    */
float2 exponentialVarianceShadowTest(in float4 sv, in const float lightDist)
{
    float depth = (lightDist * 2.f) - 1.f;
    float2 warpedDepth = float2(exp(depth * kExponentialSMConstant), -exp(-depth * kEVSMNegConstant));
    float2 minVariance = float2(0.0001f * kExponentialSMConstant * warpedDepth.x, 0.0001f * kEVSMNegConstant * warpedDepth.y);
    minVariance *= minVariance;

    float pPosMax = ChebyshevUpperBound(sv.xy, warpedDepth.x, minVariance.x);
    float pNegMax = ChebyshevUpperBound(sv.zw, warpedDepth.y, minVariance.y);

    return float2(pNegMax, pPosMax);
}

//
// Moment Shadow Maps
//

// From https://github.com/TheRealMJP/Shadows
float msmHamburger(in const float4 moments, in const float lightDist)
{
    // Bias input data to avoid artifacts
    float4 b = lerp(moments, float4(0.5f), kMSMMomentBias);
    float3 z;
    z[0] = lightDist - kMSMDepthBias;

    // Compute a Cholesky factorization of the Hankel matrix B storing only non-
    // trivial entries or related products
    float L32D22 = mad(-b[0], b[1], b[2]);
    float D22 = mad(-b[0], b[0], b[1]);
    float squaredDepthVariance = mad(-b[1], b[1], b[3]);
    float D33D22 = dot(float2(squaredDepthVariance, -L32D22), float2(D22, L32D22));
    float InvD22 = 1.0f / D22;
    float L32 = L32D22 * InvD22;

    // Obtain a scaled inverse image of bz = (1,z[0],z[0]*z[0])^T
    float3 c = float3(1.0f, z[0], z[0] * z[0]);

    // Forward substitution to solve L*c1=bz
    c[1] -= b.x;
    c[2] -= b.y + L32 * c[1];

    // Scaling to solve D*c2=c1
    c[1] *= InvD22;
    c[2] *= D22 / D33D22;

    // Backward substitution to solve L^T*c3=c2
    c[1] -= L32 * c[2];
    c[0] -= dot(c.yz, b.xy);

    // Solve the quadratic equation c[0]+c[1]*z+c[2]*z^2 to obtain solutions
    // z[1] and z[2]
    float p = c[1] / c[2];
    float q = c[0] / c[2];
    float D = (p * p * 0.25f) - q;
    float r = sqrt(D);
    z[1] = -p * 0.5f - r;
    z[2] = -p * 0.5f + r;

    // Compute the shadow intensity by summing the appropriate weights
    float4 switchVal = (z[2] < z[0]) ? float4(z[1], z[0], 1.0f, 1.0f) :
                          ((z[1] < z[0]) ? float4(z[0], z[1], 0.0f, 1.0f) :
                          float4(0.0f, 0.0f, 0.0f, 0.0f));
    float quotient = (switchVal[0] * z[2] - b[0] * (switchVal[0] + z[2]) + b[1]) / ((z[2] - switchVal[1]) * (z[0] - z[1]));
    float shadowIntensity = switchVal[2] + switchVal[3] * quotient;
    return 1.0f - saturate(shadowIntensity);
}

// From https://github.com/TheRealMJP/Shadows
float msmHausdorff(in const float4 moments, in const float lightDist)
{
    // Bias input data to avoid artifacts
    float4 b = lerp(moments, float4(0.5f), kMSMMomentBias);
    float3 z;
    z[0] = lightDist - kMSMDepthBias;

    // Compute a Cholesky factorization of the Hankel matrix B storing only non-
    // trivial entries or related products
    float L32D22 = mad(-b[0], b[1], b[2]);
    float D22 = mad(-b[0], b[0], b[1]);
    float squaredDepthVariance = mad(-b[1], b[1], b[3]);
    float D33D22 = dot(float2(squaredDepthVariance, -L32D22), float2(D22, L32D22));
    float InvD22 = 1.0f / D22;
    float L32 = L32D22 * InvD22;

    // Obtain a scaled inverse image of bz=(1,z[0],z[0]*z[0])^T
    float3 c = float3(1.0f, z[0], z[0] * z[0]);

    // Forward substitution to solve L*c1=bz
    c[1] -= b.x;
    c[2] -= b.y + L32 * c[1];

    // Scaling to solve D*c2=c1
    c[1] *= InvD22;
    c[2] *= D22 / D33D22;

    // Backward substitution to solve L^T*c3=c2
    c[1] -= L32 * c[2];
    c[0] -= dot(c.yz, b.xy);

    // Solve the quadratic equation c[0]+c[1]*z+c[2]*z^2 to obtain solutions z[1]
    // and z[2]
    float p = c[1] / c[2];
    float q = c[0] / c[2];
    float D = ((p * p) / 4.0f) - q;
    float r = sqrt(D);
    z[1] = -(p / 2.0f) - r;
    z[2] = -(p / 2.0f) + r;

    float shadowIntensity = 1.0f;

    // Use a solution made of four deltas if the solution with three deltas is invalid
    if (z[1] < 0.0f || z[2] > 1.0f)
    {
        float zFree = ((b[2] - b[1]) * z[0] + b[2] - b[3]) / ((b[1] - b[0]) * z[0] + b[1] - b[2]);
        float w1Factor = (z[0] > zFree) ? 1.0f : 0.0f;
        shadowIntensity = (b[1] - b[0] + (b[2] - b[0] - (zFree + 1.0f) * (b[1] - b[0])) * (zFree - w1Factor - z[0])
                                                    / (z[0] * (z[0] - zFree))) / (zFree - w1Factor) + 1.0f - b[0];
    }
    // Use the solution with three deltas
    else {
        float4 switchVal = (z[2] < z[0]) ? float4(z[1], z[0], 1.0f, 1.0f) :
                              ((z[1] < z[0]) ? float4(z[0], z[1], 0.0f, 1.0f) :
                              float4(0.0f, 0.0f, 0.0f, 0.0f));
        float quotient = (switchVal[0] * z[2] - b[0] * (switchVal[0] + z[2]) + b[1]) / ((z[2] - switchVal[1]) * (z[0] - z[1]));
        shadowIntensity = switchVal[2] + switchVal[3] * quotient;
    }

    return 1.0f - saturate(shadowIntensity);
}
