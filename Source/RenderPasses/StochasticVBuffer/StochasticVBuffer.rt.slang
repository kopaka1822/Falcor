#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;
import Utils.Sampling.UniformSampleGenerator;

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
}

RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float4> gRayDir;
RWTexture2D<float2> gMotion;
RWTexture2D<float4> gRayDiffs;
RWTexture2D<float> gRayDist;

struct TraceData
{
    PackedHitInfo packedHitInfo;
    float distance; // RayT
    
    static TraceData create()
    {
        TraceData d;
        d.packedHitInfo = {};
        d.distance = 0.f;
        return d;
    }
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

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    HitInfo hitInfo = { };
    rayData.packedHitInfo = hitInfo.getData(); // Invalid hit
    rayData.distance = FLT_MAX;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

[shader("closesthit")]
void closestHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    
    rayData.distance += RayTCurrent();
    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

float2 calcMotionVector(uint2 pixel, uint2 frameDim, HitInfo hitInfo)
{
    if(!hitInfo.isValid()) return 0.0;
    
    const TriangleHit triangleHit = hitInfo.getTriangleHit();
    //VertexData v = gScene.getVertexData(triangleHit);
    float3 prevPosW = gScene.getPrevPosW(triangleHit);

    float2 pixelPos = float2(pixel) + 0.5;
    float4 prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, float4(prevPosW, 1.0));
    return calcMotionVector(pixelPos, prevPosH, frameDim) + float2(gScene.camera.data.jitterX, -gScene.camera.data.jitterY); // Remove camera jitter from motion vector
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    Ray ray = gScene.camera.computeRayPinhole(pixel, frameDim);
    UniformSampleGenerator sg = UniformSampleGenerator( pixel, gFrameCount );

    TraceData data = TraceData::create();
    HitInfo hitInfo;
    float2 motionVector = 0.0;
    for (uint i = 0; i < 8; ++i)
    {

        TraceRay(gScene.rtAccel, RAY_FLAG_NONE, 0xff, 0, rayTypeCount, 0, ray.toRayDesc(), data);

        // for now take and write this hitpoint
        hitInfo = HitInfo(data.packedHitInfo);
        if(i == 0) motionVector = calcMotionVector(pixel, frameDim, hitInfo); // calc motion vector for the first hit

        if(!hitInfo.isValid()) break;
        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hitInfo, ray.dir, lod);

        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

        BSDFSample bsdfSample;
        bool absorb = !bsdf.sample(sd, sg, bsdfSample);
        if (absorb) break;

        bool isTransmission = bsdfSample.isLobe(LobeType::Transmission);
        bool isDiffuse =  bsdfSample.isLobe(LobeType::Diffuse);
        if(isDiffuse) break;

        // reflection or refraction 
        ray.origin = sd.computeNewRayOrigin(!isTransmission);
        ray.dir = bsdfSample.wo;
        ray.tMin = 0.0;
    }

    gVBuffer[pixel] = data.packedHitInfo;
    gRayDir[pixel] = float4(ray.dir, 0.0);
    gMotion[pixel] = motionVector;
    
    // gRayDiffs;
    gRayDist[pixel] = data.distance;

    /*
    float4 diffuseReflectance = { };
    float4 specularReflectance = { };
    
    HitInfo hitInfo = HitInfo(gInVBuffer[launchIndex]);
    float3 viewDir = -gScene.camera.computeRayPinhole(launchIndex, launchDim).dir;
    float rayDist = 0.f;
    float3 thp = float3(1);

    //Direct illumination 
    HitInfo hitInfoDI = hitInfo;
    float4 viewDirDistDI = float4(viewDir, rayDist);
    float3 thpDI = thp;

    bool validSurfaceForDI = true;
    uint bouncesAfterValid = 0;
    bool invalidSample = false;
    bool isDiffuse = false;

    TraceData traceData = TraceData::create();
    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount);
    #else
        SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount + 2 * 32);
    #endif
    let lod = ExplicitLodTextureSampler(0.f);
    float hitT = 0.f;
    uint wasTracedMask = 0;
    #if CAUSTIC_TEMPORAL_FILTER_ENABLED
        CausticSurface causticSurface;
    #endif
    
    //Get first valid(diffuse) Surface
    for (uint i = 0; ((i < gMaxBounces) || gMaxBounces == 0) && hitInfo.isValid(); i++)
    {
        ShadingData sd = loadShadingData(hitInfo, -viewDir, lod);

        //Get ray distance
        if(i==0){
            rayDist = distance(gScene.camera.getPosition(), sd.posW);
            viewDirDistDI.w = rayDist;
        }
            

        //Get BSDF
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        
        //Write the reflectance on first hit


        bool validBSDF = bsdf.sample(sd, sg, bsdfSample);
        if (!validBSDF){
            invalidSample = true;
        }

        //Determine if surface is diffuse
        bool isTransmission = validBSDF ? bsdfSample.isLobe(LobeType::Transmission) : false;
        float3 diffuseProb = bsdfProperties.diffuseReflectionAlbedo / (bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.specularReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo + bsdfProperties.specularTransmissionAlbedo);
        float meanDiffuseProb = (diffuseProb.x + diffuseProb.y + diffuseProb.z) / 3;
        bool hasDiffuseParts = gRequDiffParts ? (meanDiffuseProb > kDiffuseCutoff) || bsdfProperties.roughness > kRoughnessMax : true; //Check if surface is diffuse or above the roughness threshold
        isDiffuse = hasDiffuseParts && bsdfProperties.roughness > kRoughnessMin && !isTransmission; //Material is only diffuse if it is over the roughness min
        
        if(isDiffuse || invalidSample)
            break;
        
        thp *= bsdfSample.weight;   //Update troughput
                
        RayDesc tRay;
        tRay.Origin = sd.computeNewRayOrigin(!isTransmission);
        tRay.TMin = 0.0001f;
        tRay.TMax = 10000.f;
        tRay.Direction = bsdfSample.wo;

        

        #if CAUSTIC_TEMPORAL_FILTER_ENABLED
            //Set is delta flag on caustic surface
            if(i == 0){
                causticSurface.isDelta = true;
            }
        #endif 
        
        //Update info
        hitInfo = HitInfo(traceData.packedHitInfo);
        viewDir = -bsdfSample.wo;
        rayDist += traceData.distance;
        hitT += traceData.distance;
        wasTracedMask = 1;
        
        //Write DirectHit surfaces hits only for transmission or delta reflection hits
        if(validSurfaceForDI){
            #if USE_RESTIR_GI
            if(true){
            #else
            if(isTransmission || bsdfSample.isLobe(LobeType::DeltaReflection)){
            #endif
                hitInfoDI = hitInfo;
                viewDirDistDI = float4(viewDir,rayDist);
                thpDI = thp;
            }else{
                validSurfaceForDI = false;
            }
        }else
            bouncesAfterValid++;

    }
            
    //Fill the RTXDI surface if it is enabled
#if USE_RTXDI
    if(hitInfoDI.isValid())
    {
        //load all needed data
        ShadingData sd = loadShadingData(hitInfoDI, -viewDirDistDI.xyz, lod);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
    
        gRTXDI.setSurfaceData(launchIndex, sd.computeNewRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness, viewDirDistDI.w);
    }else{
        gRTXDI.setInvalidSurfaceData(launchIndex);
    }
#endif

    #if CAUSTIC_TEMPORAL_FILTER_ENABLED
        gPackedCausticSurface[launchIndex] = causticSurface.pack();
    #endif

     //Write DI Buffer
    gOutVBufferDI[launchIndex] = hitInfoDI.getData();
    gOutViewDirRayDistDI[launchIndex] = viewDirDistDI;
    gOutThpDI[launchIndex] = float4(thpDI, 1.f);

    //Set surface to invalid if the specular surface did not produce a valid sample
    #if !USE_RESTIR_GI
        if(invalidSample && !isDiffuse)
            hitInfo = {};
    #endif
    //Write
    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(launchIndex, launchDim.x)] = sg;
    #endif //STORE_SAMPLE_GEN_STATE
    gOutVBuffer[launchIndex] = hitInfo.getData();
    gOutViewDir[launchIndex] = float4(viewDir, validSurfaceForDI ? 0 : bouncesAfterValid + 1);  //Store a flag if this surface has direct light
    gOutRayDist[launchIndex] = rayDist;
    gOutThp[launchIndex] = float4(thp, hitT);
    if (is_valid(gOutDiffuseReflectance)) gOutDiffuseReflectance[launchIndex] = float4(diffuseReflectance);
    if (is_valid(gOutSpecularReflectance)) gOutSpecularReflectance[launchIndex] = float4(specularReflectance);

    #if DEBUG_MASK
        float3 debugOut = float3(0,0,1);
        if(invalidSample && !isDiffuse)
            debugOut = float3(1);
        //Enable Red channel if it is not a direct hit
        if(hitT > 0){
            debugOut = float3(0.8,0,0);
            
            if(!validSurfaceForDI)
                debugOut = float3(0,0.6,0);
                
        }

        gDebugOut[launchIndex] = float4(debugOut, 1.f);
    #endif

    */
}
