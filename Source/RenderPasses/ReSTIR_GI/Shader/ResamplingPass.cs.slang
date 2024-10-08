#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Scene.Material.ShadingUtils;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;

import Reservoir;
import SurfaceDataGI;
import PathData;

#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
    Texture2D<uint2> gReservoirPrev;
#else
    RWTexture2D<uint4> gReservoir;
    Texture2D<uint4> gReservoirPrev;
#endif

RWStructuredBuffer<GIData> gGIPathData;
StructuredBuffer<GIData> gGIPathDataPrev;
StructuredBuffer<PackedSurfaceDataGI> gSurface;
StructuredBuffer<PackedSurfaceDataGI>gSurfacePrev;
Texture2D<float2> gMVec;
Texture2D<float4> gView;
Texture2D<float4> gPrevView;

#define BIAS_MODE_BASIC 1
#define BIAS_MODE_RAYTRACE 2
//This is for syntax highlighting
#ifndef BIAS_CORRECTION_MODE
    #define BIAS_CORRECTION_MODE 2
#endif

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
}

cbuffer Constant
{
    uint2 gFrameDim;                //Frame Dimensions
    uint gMaxAge;                   //M limit            
    uint gSpatialSamples;           //Number of spatial samples
    float gSamplingRadius;          //Sampling radius
    float gDepthThreshold;          //Relative depth offset
    float gNormalThreshold;         //Maximum cosine between both Normals that is allowed
    float gMatThreshold;            //Differences between the diffuse probabilities of the materials
    uint gDisocclusionBoostSamples; //Number of spartial samples if no temporal surface was found
    float gAttenuationRadius;       //Radius for the attenuation
    float2 gJacobianMinMax;         //Minimum and Maximum values for the jacobian
}

struct LightSample{
    float3 dir;
    float Li;
};

static const float kRayEpsilon = 0.99f;
static const float kRayNearEpsilon = 0.01f;

uint index2Dto1D(uint2 idx, uint width){
    return idx.x + width * idx.y;
}

SurfaceGI getSurface(uint2 idx, bool prev)
{
    float3 viewDir;
    if (prev)
        viewDir = gPrevView[idx].xyz;
    else
        viewDir = gView[idx].xyz;

    PackedSurfaceDataGI surfaceData;
    if (prev)
        surfaceData = gSurfacePrev[index2Dto1D(idx, gFrameDim.x)];
    else
        surfaceData = gSurface[index2Dto1D(idx, gFrameDim.x)];
    
    return SurfaceGI(surfaceData, viewDir);
}

bool getLightSample(in GIData giData, in float3 worldPos, out LightSample ls, out float sampleDistance)
{
    //Get light position    
    ls.dir = giData.posW - worldPos;
    sampleDistance = length(ls.dir);
    ls.dir = ls.dir / sampleDistance; //normalize
    
    ls.Li = luminance(giData.flux);
    float distAttenuation = vplAttenuation(sampleDistance, gAttenuationRadius);
    float cosTheta = saturate(dot(normalize(giData.normal), -ls.dir));
    float jacobian = (cosTheta * distAttenuation) / giData.jacobianDeterminant;

    //Jacobian is outside of valid values (Solid angle ratio too different)
    if(jacobian < gJacobianMinMax.x || jacobian > gJacobianMinMax.y)
        return false;

    ls.Li *= jacobian;

    return ls.Li >= 0; //true if sample is valid
}

void temporalResampling(uint2 pixel, inout Reservoir r, inout GIData giSample, in SurfaceGI surface)
{
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 5 * 13);

    uint historyLimit = gMaxAge * r.M;

    //Reproject to the previous pixel using the motion vectors
    float2 motionOffset = gMVec[pixel] * float2(gFrameDim);
    float2 prevIdxF = motionOffset + float2(pixel);
    float2 rndOffset = sampleNext2D(sg) - 0.5f;
    prevIdxF += rndOffset;
    
    uint2 prevIdx = uint2(round(prevIdxF));

    //Check if a neighbor surface matches up
    //Try to find a matching surface in the neighborhood of the reprojected pixel
    SurfaceGI temporalSurface;
    bool selectPrevSample = false;
    uint previousM = 0;
    bool foundTemporalSurface = false;
    int2 idx = (0, 0);
    float temporalSearchRadius = 4.f; //actual radius is 2 due to random value in [-0.5,0.5]
    
    for (uint i = 0; i < 9; i++)
    {
        int2 offset = int2(0, 0);
        if (i > 0)
        {
            offset = int2((sampleNext2D(sg) - 0.5f) * temporalSearchRadius);
        }

        idx = prevIdx + offset;

        //Clamp to edge
        min(max(uint2(0), idx), gFrameDim - 1);
        
        //Get Hit sample from last frame
        temporalSurface = getSurface(idx, true);
        if (!temporalSurface.valid)
            continue;

        //Test surface similarity and discard if too different
        if (!surface.isValidNeighbor(temporalSurface, gDepthThreshold, gNormalThreshold, gMatThreshold))
            continue;

        //Valid hit was found
        foundTemporalSurface = true;
        break;
    }

    //If temporal surface was found, resample the previous sample
    if (foundTemporalSurface)
    {
        Reservoir prev = Reservoir(gReservoirPrev[idx]);
        prev.M = min(prev.M, historyLimit);
        previousM = prev.M;

        bool sampleValid = true;
        GIData giPrev = gGIPathDataPrev[index2Dto1D(idx, gFrameDim.x)];
        //Kill the reservoir if the sample is no longer valid or was replaced
        if (!giPrev.valid)
        {
            prev.weightSum = 0;
            prev.M = 0;
            sampleValid = false;
        }

        //Get target pdf at current location if reservoir and sample are valid
        float targetPdf = 0;
        bool combine = true;
        if (prev.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis = 0;
            if (!getLightSample(giPrev, surface.posW, ls, dis))
                combine = false;
            targetPdf = surface.getPdf(ls.dir, ls.Li);
        }

        if(combine)
            selectPrevSample = r.combineReservoir(prev, targetPdf, sampleNext1D(sg));
        if (selectPrevSample)
            giSample = giPrev;
    }


    //Bias correction via MIS
#if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
    float pi = r.targetPdf;
    float piSum = r.targetPdf; //piSum = target * M_initial ; M_initial is always 1 for us

    if (foundTemporalSurface)
    {
        LightSample ls = { };
        float dist = 0;
        bool valid = getLightSample(giSample, temporalSurface.posW, ls, dist);
        float ps = valid ? temporalSurface.getPdf(ls.dir, ls.Li) : 0.f;

#if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
        if (ps > 0)
        {
            SceneRayQuery < 0 > rayQuery;
            dist *= kRayEpsilon;
            float rayNear = dist * kRayNearEpsilon;
            dist -= rayNear;
            const Ray ray = Ray(computeRayOrigin(temporalSurface.posW, temporalSurface.normal), ls.dir, rayNear, dist);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
            {
                ps = 0;
            }
        }
#endif // BIAS MODE RAYTRACE
        
        pi = selectPrevSample ? ps : pi;
        piSum += ps * previousM;
    }
    r.finalizeSample(pi, piSum);
    
#else    
    r.finalizeSample(1.0f, float(r.M));
#endif //BIAS_MODE_BASIC
}

void spatialResampling(uint2 pixel, inout Reservoir r, inout GIData giSample, in SurfaceGI surface)
{
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 5 * 13);
    
    uint numSamples = clamp(gSpatialSamples + 1, 1, 8); //Cap at 8 samples for the later bias correction
    uint cachedIndices[8]; //Store all indeces. 32 Bits per 2D index (16 per element)
    uint cachedM[4];
    int selectedSample = -1; //Index of the selected sample

    //Go through all neighbors
    for (uint i = 0; i < numSamples; i++)
    {
        cachedIndices[i] = 0xFFFFFFFF; //Set to max int
        if (i % 2 == 0)
            cachedM[i / 2] = 0;
        
        int2 idx = int2(pixel);

        float2 randSamp = (sampleNext2D(sg) * 2.f) - 1.f; //random sample in [-1,1]
        int2 spartialOffset = int2(round(randSamp * gSamplingRadius));
        idx += spartialOffset;

        //Outside index is invalid
        if(any(idx < 0) || any(idx >= gFrameDim))
            continue;

        //Get surface and check if valid
        SurfaceGI neighSurface = getSurface(idx, false); //Get surface
        if (!neighSurface.valid)
            continue;
        
        if (!surface.isValidNeighbor(neighSurface, gDepthThreshold, gNormalThreshold, gMatThreshold))
            continue;

        //Neighbor passed the test, start resampling
        Reservoir sampledReservoir = Reservoir(gReservoirPrev[idx]);    //Current reservoir is stored in prev
        
        bool sampleValid = true;
        GIData giNeigh = gGIPathDataPrev[index2Dto1D(idx, gFrameDim.x)];
        //Kill the reservoir if the sample is no longer valid or was replaced
        if (!giNeigh.valid)
        {
            sampledReservoir.weightSum = 0;
            sampledReservoir.M = 0;
            sampleValid = false;
        }
        //Store indices
        cachedIndices[i] = (idx.x & 0xFFFF) << 16 | (idx.y & 0xFFFF);
        uint M = 0xFFFF & sampledReservoir.M;
        if (i % 2 != 0)
            M = M << 16;
        cachedM[i / 2] |= M;

        //Get neightbor target PDF for current surface
        float sampleTargetPdf = 0.f;
        if (sampledReservoir.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis;
            if (!getLightSample(giNeigh, surface.posW, ls, dis))
                continue;
            sampleTargetPdf = surface.getPdf(ls.dir, ls.Li);
        }
        //Combine Reservoirs
        if (r.combineReservoir(sampledReservoir, sampleTargetPdf, sampleNext1D(sg)))
        {
            selectedSample = i;
            giSample = giNeigh;
        }
    }

    //Bias correction
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
        
        float pi = r.targetPdf;
        float piSum = r.targetPdf; //piSum = target * M_initial ; M_initial is always 1 for us
        
        //go through all neighbors again
        for (int i = 0; i < numSamples; i++)
        {
            //Skip if we skipped in last loop
            if ((cachedIndices[i]) == 0xFFFFFFFF)
                continue;
            //Get Index
            uint2 idx = uint2(cachedIndices[i] >> 16, (cachedIndices[i] & 0xFFFF));
            //Get Surface
            SurfaceGI biasSurface = getSurface(idx, false);
            if (!biasSurface.valid)
                continue;
        
            //Get light pdf
            float ps = 0.0f;
            LightSample ls = { };
            float dist = 0;

            //Skip Sample if it was rejected by jacobian
            if (!getLightSample(giSample, biasSurface.posW, ls, dist))
                continue;
            ps = biasSurface.getPdf(ls.dir, ls.Li);
        
            //Trace ray
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
            if (ps > 0)
            {
                SceneRayQuery < 0 > rayQuery;
                dist *= kRayEpsilon;
                float rayNear = dist * kRayNearEpsilon;
                dist -= rayNear;
                const Ray ray = Ray(computeRayOrigin(biasSurface.posW, biasSurface.normal), ls.dir, rayNear, dist);
                if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_CULL_NON_OPAQUE, 0xff))
                {
                    ps = 0.0f;
                }
            }
    #endif // BIAS MODE RAYTRACE
        
            uint M = cachedM[i / 2];
            if (i % 2 == 0)
                M = M & 0xFFFF;
            else
                M = M >> 16;
        
            pi = selectedSample == i ? ps : pi;
            piSum += ps * M;
        }
        r.finalizeSample(pi, piSum);
        
    #else //BIAS MODE BASIC
        r.finalizeSample(1.0f, r.M);
    #endif //BIAS MODE BASIC    
}

void spatiotemporalResampling(uint2 pixel, inout Reservoir r, inout GIData giSample, in SurfaceGI surface)
{
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 5 * 13);
    
    uint historyLimit = gMaxAge;

    //Check if motion vectors are inside of the frame
    float2 motionOffset = gMVec[pixel] * float2(gFrameDim);
    float2 prevIdxF = motionOffset + float2(pixel);
    float2 rndOffset = sampleNext2D(sg) - 0.5f;
    prevIdxF += rndOffset;
    
    int2 prevIdx = int2(round(prevIdxF));

    //Get surface from last frame
    bool foundTemporalSurface = false;
    int2 temporalOffset = int2(0, 0);
    const float temporalSearchRadius = 4.f; //actual radius is 2 due to random value in [-0.5,0.5]

    //Try to find a matching surface in the neighborhood of the reprojected pixel
    for (uint i = 0; i < 9; i++)
    {
        int2 offset = int2(0, 0);
        if (i > 0)
        {
            offset = int2((sampleNext2D(sg) - 0.5f) * temporalSearchRadius);
        }

        int2 idx = prevIdx + offset;

        //Clamp to edge
        min(max(uint2(0), idx), gFrameDim - 1);
        
        //Get Hit sample from last frame
        SurfaceGI temporalSurface = getSurface(idx, true);
        if (!temporalSurface.valid)
            continue;

        //Test surface similarity and discard if too different
        if (!surface.isValidNeighbor(temporalSurface, gDepthThreshold, gNormalThreshold, gMatThreshold))
            continue;
        
        //Valid hit was found
        temporalOffset = offset;
        foundTemporalSurface = true;
        break;
    }

    //
    //Spartio Temporal Resampling
    //
    
    uint numSamples = foundTemporalSurface ? gSpatialSamples + 1 : max(gSpatialSamples + 1, gDisocclusionBoostSamples);
    numSamples = clamp(numSamples, 1, 8); //Cap at 8 samples for the later bias correction
    uint cachedIndices[8]; //Store all indeces. 32 Bits per 2D index (16 per element)
    uint cachedM[4];
    
    int selectedSample = -1;
    bool validSample = true;

    //Go through all neighbors
    for (uint i = 0; i < numSamples; i++)
    {
        cachedIndices[i] = 0xFFFFFFFF; //Set to max int
        if (i % 2 == 0)
            cachedM[i / 2] = 0;
        int2 idx = int2(0);

        //Take temporal sample or generate a new neighbor sample
        if (i == 0 && foundTemporalSurface)
        {
            idx = prevIdx + temporalOffset;
        }
        else
        {
            //Get sample from sampling mask
            float2 randSamp = (sampleNext2D(sg) * 2.f) - 1.f; //random sample in [-1,1]
            int2 spartialOffset = int2(round(randSamp * gSamplingRadius));
            idx = prevIdx + spartialOffset;

            //Outside index is invalid
            if(any(idx < 0) || any(idx >= gFrameDim))
                continue;
            
             //Get surface and check if valid
            SurfaceGI neighSurface = getSurface(idx, true);
            if (!neighSurface.valid)
                continue;
        
            if (!surface.isValidNeighbor(neighSurface, gDepthThreshold, gNormalThreshold, gMatThreshold))
                continue;
        }
            
        //If neighbor surface has passed the tests get reservoir and compair them
        Reservoir sampledReservoir = Reservoir(gReservoirPrev[idx]);
        sampledReservoir.M = min(sampledReservoir.M, historyLimit);

        GIData giPrev = gGIPathDataPrev[index2Dto1D(idx, gFrameDim.x)];

        //Kill the reservoir if it is not valid
        bool sampleValid = true;
        if (!giPrev.valid)
        {
            sampledReservoir.weightSum = 0;
            sampledReservoir.M = 0;
            sampleValid = false;
        }

        //Store indices
        cachedIndices[i] = (idx.x & 0xFFFF) << 16 | (idx.y & 0xFFFF);
        uint M = 0xFFFF & sampledReservoir.M;
        if (i % 2 != 0)
            M = M << 16;
        cachedM[i / 2] |= M;
        
        //Get neightbor light target PDF for current surface
        float sampleTargetPdf = 0.f;
        if (sampledReservoir.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis;
            if (!getLightSample(giPrev, surface.posW, ls, dis))
                continue;
            sampleTargetPdf = surface.getPdf(ls.dir, ls.Li);
        }
        //Combine Reservoirs
        if (r.combineReservoir(sampledReservoir, sampleTargetPdf, sampleNext1D(sg)))
        {
            selectedSample = i;
            giSample = giPrev;
        }
    }

  //Bias correction
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
        
        float pi = r.targetPdf;
        float piSum = r.targetPdf; //piSum = target * M_initial
        //go through all neighbors again
        for (int i = 0; i < numSamples; i++)
        {
            //Skip if we skipped in last loop
            if ((cachedIndices[i]) == 0xFFFFFFFF)
                continue;
            //Get Index
            uint2 idx = uint2((cachedIndices[i] >> 16) & 0xFFFF, (cachedIndices[i] & 0xFFFF));
            //Get Surface
            SurfaceGI biasSurface = getSurface(idx, true);
        
            //Get light pdf
            float ps = 0.0f;
            LightSample ls = { };
            float dist = 0;

            if (biasSurface.valid)
            {
                if(getLightSample(giSample, biasSurface.posW, ls, dist))
                    ps = biasSurface.getPdf(ls.dir, ls.Li);
            }
        
            //Trace ray
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
            if (ps > 0)
            {
                SceneRayQuery < 0 > rayQuery;
                dist *= kRayEpsilon;
                float rayNear = dist * kRayNearEpsilon;
                dist -= rayNear;
                const Ray ray = Ray(computeRayOrigin(biasSurface.posW, biasSurface.normal), ls.dir, rayNear, dist);
                if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_CULL_NON_OPAQUE, 0xff))
                {
                    ps = 0.0f;
                }
            }
    #endif // BIAS MODE RAYTRACE
            uint M = cachedM[i / 2];
            if (i % 2 == 0)
                M = M & 0xFFFF;
            else
                M = (M >> 16) & 0xFFFF;
        
            pi = selectedSample == i ? ps : pi;
            piSum += ps * M;
        }
        r.finalizeSample(pi, piSum);
        
    #else //BIAS MODE BASIC
        r.finalizeSample(1.0f, r.M);
    #endif //BIAS MODE BASIC  
}

[numthreads(16, 16, 1)]
void main(uint2 dTid : SV_DispatchThreadID)
{    
    //Check if surface is valid
    SurfaceGI surface = getSurface(dTid, false);
    if (!surface.valid)
        return;
        
    //Load reservoir from current iteration
    Reservoir r = { };
    GIData giData = GIData();

    //Load current reservoir in 
#if MODE_SPATIAL
    Reservoir current = Reservoir(gReservoirPrev[dTid]);
    if (current.weightSum > 0)
    {
        giData = gGIPathDataPrev[index2Dto1D(dTid, gFrameDim.x)];
    }
#else //Mode Temporal or Spatiotemporal
    Reservoir current = Reservoir(gReservoir[dTid]);
    if (current.weightSum > 0)
    {
        giData = gGIPathData[index2Dto1D(dTid, gFrameDim.x)];
    }
#endif //MODE_SPATIAL
    
    r.combineReservoir(current, current.targetPdf, 0.5 /* random */);
    
    //Do resampling
#if MODE_SPATIOTEMPORAL
    spatiotemporalResampling(dTid, r, giData, surface);
#elif MODE_TEMPORAL
    temporalResampling(dTid, r, giData, surface);
#elif MODE_SPATIAL
    spatialResampling(dTid, r, giData, surface);
#endif //Resampling Mode

    //Store reservoir and sample data    
#if USE_REDUCED_RESERVOIR_FORMAT
    gReservoir[dTid] = r.packReduced();
#else
    gReservoir[dTid] = r.pack();
#endif
    gGIPathData[index2Dto1D(dTid, gFrameDim.x)] = giData;
}
