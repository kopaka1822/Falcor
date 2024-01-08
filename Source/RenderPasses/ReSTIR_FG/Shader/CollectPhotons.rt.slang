#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;

import Reservoir;
import SurfaceDataFG;
import FinalGatherData;

//For syntax highlighting purposes
#ifndef MODE_FINAL_GATHER
    #define MODE_FINAL_GATHER 0
#endif

//For syntax highlighting purposes
#ifndef CAUSTIC_COLLECTION_MODE
    #define CAUSTIC_COLLECTION_MODE 3
#endif

//Stochastic Collect
#ifndef USE_STOCHASTIC_COLLECT
#define USE_STOCHASTIC_COLLECT 0
#endif

#ifndef STOCH_NUM_PHOTONS
#define STOCH_NUM_PHOTONS 3
#endif

//Collection Mode defines
#define CAUSTIC_COLLECT_MODE_ALL 0
#define CAUSTIC_COLLECT_MODE_NONE 1
#define CAUSTIC_COLLECT_MODE_GREATERONE 2
#define CAUSTIC_COLLECT_MODE_TEMPORAL 3

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
    float2 gPhotonRadius; // x->Global Radius, y-> Caustic Radius
    float gAttenuationRadius;   //Radius for the distance attenuation
}

//Constant settings for the temporal filter. Can be null
cbuffer TemporalFilter{
    uint gTemporalFilterHistoryLimit;
    float gDepthThreshold;              //Depth Threshold
    float gNormalThreshold;             //Normal Threshold
    float gMatThreshold;                //Material Threshold
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<PackedHitInfo> gFinalGatherHit;
StructuredBuffer<AABB> gPhotonAABB[2];
StructuredBuffer<uint4> gPackedPhotonData[2];
Texture2D<float4> gThp; //Thp for caustic and final gathering

//Inputs Temporal Caustic Filter (can be null)
Texture2D<float2> gMVec;
Texture2D<uint2> gCausticSurface;
Texture2D<uint2> gCausticSurfacePrev;
Texture2D<float4> gCausticPrev;

// Outputs
RWTexture2D<float4> gColor;
RWStructuredBuffer<FGData> gFGSampleData;
RWTexture2D<float4> gCausticOut;


#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif


//Acceleration Structure
RaytracingAccelerationStructure gPhotonAS;


//Constant defines
static const bool kCollectCaustics = CAUSTIC_COLLECTION_MODE != CAUSTIC_COLLECT_MODE_NONE;
static const bool kCollectCausticsForIndirect = CAUSTIC_COLLECTION_INDIRECT;

static const float kRejectDiffuseSurfaces = REJECT_FGSAMPLE_DIFFUSE_SURFACE;    //Uses same definition as photon generation
static const float kTraceRoughnessCutoff = TRACE_TRANS_SPEC_ROUGH_CUTOFF;
static const float kTraceDiffuseCutoff = TRACE_TRANS_SPEC_DIFFUSEPART_CUTOFF;
static const bool kEmissionToCausticFilter = EMISSION_TO_CAUSTIC_FILTER;

/** Payload for photon collection. Collect all (48B) and Stochastic Collect (32 + X B) available
*/
#if USE_STOCHASTIC_COLLECT
struct RayData
{
    uint counter;                   //Counter for photons this pixel
    uint photonIdx[STOCH_NUM_PHOTONS];    //Num Photons, variable length. Should be 3 + 4*X for best fit.

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.counter = 0;
        for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
            this.photonIdx[i] = 0;
    }
};
#else //USE_STOCHASTIC_COLLECT
struct RayData
{
    float3 radiance; ///< Accumulated outgoing radiance from path.
    uint countPhotons; ///< Counts the photons
    PackedHitInfo packedHitInfo; ///< Hit info from vBuffer; Up to 16B

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.radiance = float3(0);
        this.countPhotons = 0;
    }
};
#endif //USE_STOCHASTIC_COLLECT

struct SphereAttribs
{
    uint pad;
};

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

uint index2Dto1D(uint2 idx, uint width)
{
    return idx.x + width * idx.y;
}

CausticSurface getSurface(uint2 idx, bool prev, uint frameWidth, float3 view)
{
    uint2 surfaceData;
    if (prev)
        surfaceData = gCausticSurfacePrev[idx];
    else
        surfaceData = gCausticSurface[idx];
    
    return CausticSurface(surfaceData);  
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}

//Photon Collection via anyHit
#if USE_STOCHASTIC_COLLECT
[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    const uint primIndex = PrimitiveIndex();
    //Get Photon data
    PhotonData pd = PhotonData(gPackedPhotonData[InstanceIndex()][primIndex]);

    //Check if first hit check is required
    if((rayData.counter >> 24 & 1) != 0)
    {
        if(!pd.isFirstHit)
            return;
    }

    //Check for different surfaces
    if (dot(pd.normal,  WorldRayDirection()) < 0.6)
    {
        return;
    }
    
    //Check if photon sample is in valid hemisphere
    if (dot(pd.dir, WorldRayDirection()) <= 0) 
    {
        return;
    }

    uint counter = rayData.counter & 0xFFFFFF;  //Mask out the flags
    uint idx = counter;
    counter++;
    rayData.counter++;

    //Reservoir sampling
    //If photon list is full we get a random index between 0 and number of elements (rayData.counter) to stochastically insert
    if (idx >= STOCH_NUM_PHOTONS)
    {
        idx = uint(sampleNext1D(rayData.sg) * counter);
    }
    //Insert if index is within maximum list size
    if (idx < STOCH_NUM_PHOTONS)
        rayData.photonIdx[idx] = primIndex;
}

#else //USE_STOCHASTIC_COLLECT

[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    const uint primIndex = PrimitiveIndex();

    //Get Photon data
    PhotonData pd = PhotonData(gPackedPhotonData[InstanceIndex()][primIndex]);
        
    //Get hit data from payload
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    let lod = ExplicitLodTextureSampler(0.f);
    //World Direction is set to the viewDir we get from the vBuffer
    ShadingData sd = loadShadingData(hit, WorldRayDirection(), lod);

    //Check if first hit check is required
    if((rayData.countPhotons >> 24 & 1) != 0)
    {
        if(!pd.isFirstHit)
            return;
    }
    
    //Check for different surfaces
    if (dot(pd.normal, sd.faceN) < 0.6)
    {
        return;
    }

    //Photon contribution
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_GREATERONE)
    rayData.countPhotons++;
    #endif
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);
    float3 f_r = bsdf.eval(sd, pd.dir, rayData.sg); //right sign?
    float NdotL = saturate(dot(bsdfProperties.guideNormal, pd.dir));
    if(NdotL > 0)
        rayData.radiance += max(0.f, (f_r * pd.flux) / NdotL);
}
#endif //USE_STOCHASTIC_COLLECT

//Checks if the ray start point is inside the sphere.
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    radiusTest = radiusTest * radiusTest;
    float radiusTestF = radiusTest.x + radiusTest.y + radiusTest.z;
    if (radiusTestF < radius * radius)
        return true;
    return false;
}

[shader("intersection")]
void intersection()
{
    //Get start point and prim index
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();

    //Get Photon AABB. Instance Index 0 is caustic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gPhotonRadius.x : gPhotonRadius.y;
    
    //Check for Sphere intersection
    bool tHit = hitSphere(photonAABB.center(), radius, origin);

    //Attribs are needed by the api. Because we have no need for additional attributes it is empty
    SphereAttribs attribs;
    attribs.pad = 0.f;

    //Report the hit if the point is in the sphere
    if (tHit)
    {
        ReportHit(RayTCurrent(), 0, attribs);
    }
}

//Lighting Calculation for each photon in the list. This is done seperatly for caustic and global photons
float3 stochPhotonContribution(in const ShadingData sd, in IMaterialInstance bsdf, float3 N , inout RayData rayData, bool isCaustic)
{
    #if USE_STOCHASTIC_COLLECT
    //Get the size of the list to loop through. 
    uint maxIdx = min(rayData.counter, STOCH_NUM_PHOTONS);
    float3 radiance = float3(0);

    //Return when there is no element
    if (maxIdx == 0)
        return radiance;

    //loop through the list and accumulate photons
    for (uint i = 0; i < maxIdx; i++)
    {
        //Get Photon info
        uint instanceIndex = isCaustic ? 1 : 0;
        PhotonData pd = PhotonData(gPackedPhotonData[instanceIndex][rayData.photonIdx[i]]);

        float3 f_r = bsdf.eval(sd, pd.dir, rayData.sg);
        float NdotL = saturate(dot(N, pd.dir));
        if(NdotL > 0)
            radiance += max(0.f, (f_r * pd.flux) / NdotL);
    }

    //Weight output radiance with number of photons for this pixel.
    //The weight equals 1 if there is less or equal than the maximum list size in the photon array. (Reservoir sampling)
    return radiance * (float(rayData.counter) / float(maxIdx));
    #else
    return float3(0);
    #endif
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    let lod = ExplicitLodTextureSampler(0.f);
    
    //Init reservoir
    Reservoir r = Reservoir(gReservoir[launchIndex]);
    r.M = 1; //set M to the right value
    
    //Prepare payload
    RayData rayData = RayData();
    rayData.sg = SampleGenerator(launchIndex, gFrameCount + 3 * 12);

    PackedHitInfo packedHit = gVBuffer[launchIndex];
    #if !USE_STOCHASTIC_COLLECT
    rayData.packedHitInfo = packedHit;
    #endif //!USE_STOCHASTIC_COLLECT
    HitInfo hit = HitInfo(packedHit);
    bool valid = hit.isValid(); //Check if the ray is valid

    //Get vertex data for the world position
    TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);

    float4 viewTmp =gView[launchIndex];
    float3 view = -viewTmp.xyz;
    bool collectDirect = viewTmp.w > 0.1;
    bool collectEmissive = viewTmp.w > 1.1;

    ShadingData fgSD = loadShadingData(hit, view, lod); //Shading Data for direct hit

    //Ray description 
    RayDesc ray;
    ray.Origin = v.posW;
    ray.TMin = 0.0;
    ray.TMax = 0.001;
#if USE_STOCHASTIC_COLLECT
    ray.Direction = fgSD.faceN; //we take the face Normal as direction to save payload space
#else
    ray.Direction = view; //we take the view dir as direction to save payload space
#endif //USE_STOCHASTIC_COLLECT

    //Closest hit shader can be skipped
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;
    float3 causticRadiance = float3(0);
    float3 thp = gThp[launchIndex].xyz;
    
    //Collect Caustic Photon
    if (valid && kCollectCaustics)
    {        
        TraceRay(gPhotonAS, rayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float w = 1 / (M_PI * gPhotonRadius.y * gPhotonRadius.y);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let mi = gScene.materials.getMaterialInstance(fgSD, lod, hints);
        let miProperties = mi.getProperties(fgSD);
        #if USE_STOCHASTIC_COLLECT
            causticRadiance += w * stochPhotonContribution(fgSD, mi, miProperties.guideNormal, rayData, true);
        #else // USE_STOCHASTIC_COLLECT
            #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_GREATERONE)
            if(rayData.countPhotons > 1)
            #endif // CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_GREATERONE
            causticRadiance += w * rayData.radiance;
        #endif // USE_STOCHASTIC_COLLECT

         //Collect the direct light on some metal surfaces
        if(collectDirect){
        //Clear Payload
        #if USE_STOCHASTIC_COLLECT
            rayData.counter = 0;
            for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
                rayData.photonIdx[i] = 0;
            rayData.counter += 1 << 24; //Flag that only the first hit should be collected
        #else
            rayData.radiance = float3(0);
            rayData.countPhotons += 1 << 24; //Flag that only the first hit should be collected
        #endif
            TraceRay(gPhotonAS, rayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
            float w = 1 / (M_PI * gPhotonRadius.x * gPhotonRadius.x);
        #if USE_STOCHASTIC_COLLECT
            rayData.counter &= 0xFFFFFF;    //Delete flag
            causticRadiance += w * stochPhotonContribution(fgSD, mi, miProperties.guideNormal, rayData, false);
        #else // USE_STOCHASTIC_COLLECT
            rayData.countPhotons = 0; //delete flag
            causticRadiance += w * rayData.radiance;
        #endif // USE_STOCHASTIC_COLLECT
            if((kEmissionToCausticFilter || MODE_FINAL_GATHER) && collectEmissive) causticRadiance += miProperties.emission;
            causticRadiance *= thp;
            
        }
    }

    //Temporal Caustic Filter
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_TEMPORAL)
    //Get Motion Vector and get reprojected pixel
    float2 motionOffset = gMVec[launchIndex] * float2(launchDim);
    float2 prevIdxF = motionOffset + float2(launchIndex);
    float2 rndOffset = sampleNext2D(rayData.sg) - 0.5f;
    prevIdxF += rndOffset;
    int2 tempIndex = int2(round(prevIdxF));

    uint history = 1;
    if (tempIndex.x >= 0 && tempIndex.y >= 0 && tempIndex.x < launchDim.x && tempIndex.y < launchDim.y)
    {
        //Load Surfaces
        CausticSurface currentSurface = getSurface(launchIndex, false, launchDim.x, view);
        CausticSurface prevSurface = getSurface(tempIndex, true, launchDim.x, view);
        
        if (currentSurface.isValidNeighbor(prevSurface, gDepthThreshold, gNormalThreshold))
        {
            //Because the caustic image can be RGBA16F, we need to store the history as float. As values are in a low range this should not introduce any errors
            float4 prev = gCausticPrev[tempIndex];

            uint limit = currentSurface.isDelta ? gTemporalFilterHistoryLimit/2 : gTemporalFilterHistoryLimit;  //Make reflections more reactive as the reprojection is often wrong
            history = min(limit, uint(round(prev.w)) + history);
            prev.xyz *= max(0, (history - 1));
            causticRadiance += prev.xyz;
            causticRadiance /= max(1u, history);
        }
    }
    
    gCausticOut[launchIndex] = float4(causticRadiance, float(history));
    
    #elif !MODE_FINAL_GATHER //END Caustic mode temporal; Store Caustic tex if not Final Gather mode
    gCausticOut[launchIndex] = float4(causticRadiance, 1.f);
    #endif

    //
    //Final Gather Sample Photon Collection
    //
    
    packedHit = gFinalGatherHit[launchIndex];
#if USE_STOCHASTIC_COLLECT
        rayData.counter = 0;
        for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
            rayData.photonIdx[i] = 0;
#else
    rayData.radiance = float3(0);
    rayData.packedHitInfo = packedHit;
#endif // USE_STOCHASTIC_COLLECT
    hit = HitInfo(packedHit);
    valid &= hit.isValid(); //Check if the final gather ray is valid

    triangleHit = hit.getTriangleHit();
    v = gScene.getVertexData(triangleHit);
    ray.Direction = v.posW - ray.Origin; //Sample dir is used here to save payload space
    float distToFG = length(ray.Direction); //Distance for Jacobian determinant
    ray.Direction /= distToFG; //Normalize
    ray.Origin = v.posW; //Update origin

    
#if MODE_FINAL_GATHER
    view = ray.Direction;   //Store view for later
#endif
    
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -ray.Direction, lod);
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);

    uint lobeTypes = bsdf.getLobeTypes(sd);
    bool hasDiffuseParts = true;
    if(kRejectDiffuseSurfaces)
    {
        float3 diffuseProb = bsdfProperties.diffuseReflectionAlbedo / (bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.specularReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo + bsdfProperties.specularTransmissionAlbedo);
        float meanDiffuseProb = (diffuseProb.x + diffuseProb.y + diffuseProb.z) / 3;
        hasDiffuseParts = (meanDiffuseProb > kTraceDiffuseCutoff) || bsdfProperties.roughness > kTraceRoughnessCutoff;
    }
    //Reject surfaces where no photons are stored anyway. The emissive case is handled by ReSTIR for Direct Lights
    bool isDiffuse = hasDiffuseParts && !any(bsdfProperties.emission > 0) && ((lobeTypes & (uint)LobeType::Delta) == 0);
    valid &= isDiffuse;  
    
    float3 fgRadiance = float3(0);
    float jacoDeter = -1.f;
    
    if (valid)
    {
        //Jacobian Determinant (only needed for resampling)
#if !MODE_FINAL_GATHER
        jacoDeter = vplAttenuation(distToFG, gAttenuationRadius) * saturate(dot(bsdfProperties.guideNormal,sd.V));
#endif

    #if USE_STOCHASTIC_COLLECT
        ray.Direction = sd.faceN;   //Store faceN in direction
    #endif // USE_STOCHASTIC_COLLECT
    
        TraceRay(gPhotonAS, rayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float w = 1 / (M_PI * gPhotonRadius.x * gPhotonRadius.x);
    #if USE_STOCHASTIC_COLLECT
        fgRadiance += w * stochPhotonContribution(sd, bsdf, bsdfProperties.guideNormal, rayData, false);
    #else
        fgRadiance += w * rayData.radiance;
    #endif 
        //Caustic photon collection for indirect light
        if(kCollectCaustics && kCollectCausticsForIndirect)
        {
        #if USE_STOCHASTIC_COLLECT
            rayData.counter = 0;
            for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
                rayData.photonIdx[i] = 0;
        #else
            rayData.radiance = float3(0);
        #endif
            //Add caustic indirect radiance
            TraceRay(gPhotonAS, rayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
            w = 1 / (M_PI * gPhotonRadius.y * gPhotonRadius.y);
        #if USE_STOCHASTIC_COLLECT
            fgRadiance += w * stochPhotonContribution(sd, bsdf, bsdfProperties.guideNormal, rayData, true);
        #else
            fgRadiance += w * rayData.radiance;
        #endif 
        }
        
    }
    
#if MODE_FINAL_GATHER
    //Final gather
    let bsdfFG = gScene.materials.getMaterialInstance(fgSD, lod, hints);
    float3 color = max(0.f, fgRadiance * r.weightSum);
    color *= bsdfFG.eval(fgSD, view, rayData.sg) * thp;

    gColor[launchIndex] = float4(color + causticRadiance, 1.f);
#else
   
    
    //ReSTIR Candidate
    uint lightIdx = index2Dto1D(launchIndex, launchDim.x);
    float Li = luminance(fgRadiance);

    r.targetPdf *= Li; //Add the missing parts of the targetPDF (Li)

     //Create the final gather sample
    FGData fgData = { };
    bool fgValid = !isnan(Li) && !isinf(Li);
    fgData.posW = sd.computeNewRayOrigin();
    fgData.normal = bsdfProperties.guideNormal;
    fgData.flux = fgRadiance;
    fgData.jacobianDeterminant = fgValid ? jacoDeter : -1.f;    //Negative jacobian determinant is an invalid sample

    if (!fgValid)
    {
        r.M = 0;
        r.weightSum = 0.f;
    }

#if USE_REDUCED_RESERVOIR_FORMAT
    gReservoir[launchIndex] = r.packReduced();
#else  //USE_REDUCED_RESERVOIR_FORMAT
    gReservoir[launchIndex] = r.pack();
#endif //USE_REDUCED_RESERVOIR_FORMAT
    gFGSampleData[index2Dto1D(launchIndex, launchDim.x)] = fgData;
    

#endif //!(MODE_FINAL_GATHER)
}
