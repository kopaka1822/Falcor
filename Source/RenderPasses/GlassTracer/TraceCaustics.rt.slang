#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.RaytracingInline;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import StructsAndFunctions;

cbuffer CB
{
    uint gFrameCount;       //Current Frame Count for Sample Gen
    uint gLightPaths;       //Number of light Paths
    uint gMaxPathLength;    //Maximum path length
    uint gDiffuseBounces;   //Number of diffuse bounces a caustic is allowed to have

    uint2 gScreenDimensions;    //Screen dimensions for backprojection
    float gRoughnessThreshold;  //Roughness threshold for defining a diffuse surface
    float gEmissiveLightProb;   // Probablilty to create an emissive sample. 0->Analytic sample, 1->Emissive

    float gPhotonRadius;           // Caustic Photon radius
    float gNormalizedPixelDiagonal; // Approximated pixel diagonal at distance 1
}

RWTexture2D<int> gCausticHead;
RWStructuredBuffer<CausticData> gCausticData;
RWStructuredBuffer<AABB> gCausticAABB;
RWStructuredBuffer<uint> gCounter;

static const bool kAlphaTest = true;
static const bool kUseAdaptivePhotonRadius = USE_ADAPTIVE_PHOTON_RADIUS;

//Ray Payload using Falcors hit struct
struct RayPayload
{
    PackedHitInfo packedHitInfo; //< Hit Info

    /** Create ray payload with default parameters.
     */
    __init()
    {
        this.packedHitInfo = {};
    }
}

// A photon sampled from the scene
struct LightSample
{
    float3 posW;
    float normalX;
    float3 dirW;
    float normalY;
    float3 flux;
    float normalZ;

    property float3 normal {
        get { return float3(normalX, normalY, normalZ); }
        set {
            normalX = newValue.x;
            normalY = newValue.y;
            normalZ = newValue.z;
        }
    }
};

float getPhotonRadius(const float3 hitPos)
{
    float adaptiveRadius = 1.0;
    if (kUseAdaptivePhotonRadius)
    {
        float distToCamera = distance(hitPos, gScene.camera.getPosition());
        adaptiveRadius = (gNormalizedPixelDiagonal * distToCamera) / 2.0;
    }
    return adaptiveRadius * gPhotonRadius;
}

bool generateLightSample(bool isEmissive, inout SampleGenerator sg, out LightSample ls)
{
    ls = {};

    float3 woLocal = float3(0);

    if (isEmissive)
    {
        const uint lightCount = gScene.lightCollection.triangleCount;
        const uint selectedLight = min(uint(floor(lightCount * sampleNext1D(sg))), lightCount - 1);
        // Get emissive triangle data
        const EmissiveTriangle tri = gScene.lightCollection.getTriangle(selectedLight);
        const float3 barycentrics = sample_triangle(sampleNext2D(sg)); // Sample random triangle position
        ls.posW = tri.getPosition(barycentrics);
        ls.normal = tri.normal;

        // Sample emissive material of the triangle
        float2 uv = tri.getTexCoord(barycentrics);
        ls.flux = gScene.materials.evalEmissive(tri.materialID, uv) * tri.area;

        ls.posW = computeRayOrigin(ls.posW, ls.normal); // Offset as this light is real geometry

        // Sample random direction in a cosine lobe
        float lightDirPDF = 1.0;
        woLocal = sample_cosine_hemisphere_concentric(sampleNext2D(sg), lightDirPDF);
        if (lightDirPDF <= 0.) // Invalid direction
            return false;
        ls.flux *= M_PI;               // Lambert Emitter (or reduced cos/pdf)
        ls.flux *= lightCount;         // Inv light select pdf
        ls.flux /= gEmissiveLightProb; //Probability to select an emissive light
    } else
    {
        const uint lightCount = gScene.getLightCount();
        const uint selectedLight = min(uint(floor(lightCount * sampleNext1D(sg))), lightCount - 1);
        LightData lightData = gScene.getLight(selectedLight);
        // Only Point (and Spot) lights are supported
        if (lightData.type != uint(LightType::Point))
            return false;

        float spotAngle = lightData.cosOpeningAngle;
        float2 rndDir = sampleNext2D(sg);   //Random direction variable
        // Depending on the opening angle, the light is a spot or point light
        if (lightData.openingAngle < M_2PI) // Spot Light
        {
            woLocal = sample_cone(rndDir, spotAngle);
        } else // Point Light
        {
            spotAngle = 0; // Just to be sure
            woLocal = sample_sphere(rndDir);
        }
        float invLightPdf = M_2PI * (1.0 - spotAngle);
        ls.normal = lightData.dirW;
        ls.posW = lightData.posW;
        ls.flux = lightData.intensity * invLightPdf;
        ls.flux *= lightCount;               //Inv light select pdf
        ls.flux /= 1.0 - gEmissiveLightProb; //Probability to select an analytic light
    }

    // Local to world direction
    //  Convert direction local -> world
    float3 bitangent = perp_stark(ls.normal);
    float3 tangent = cross(bitangent, ls.normal);
    ls.dirW = woLocal.x * tangent + woLocal.y * bitangent + woLocal.z * ls.normal;

    // Divide by number of light paths
    ls.flux /= gLightPaths;
    return true;
}

//Checks if the surface is visible by the camera
bool isVisibleByCamera(ShadingData sd, out uint2 backprojectedPixel)
{
    backprojectedPixel = uint2(0);

    // Check if position is in camera view
    float4 screenPos = mul(gScene.camera.data.viewProjMatNoJitter, float4(sd.posW, 1));
    screenPos /= screenPos.w;
    screenPos.y *= -1;
    screenPos.xy = screenPos.xy * 0.5 + 0.5; // UV from [-1,1] to [0,1]
    if (any(screenPos.xy < 0) || any(screenPos.xy >= 1) || screenPos.z < 0 || screenPos.z > 1)
        return false;

    backprojectedPixel = uint2(floor(screenPos.xy * gScreenDimensions));
    // Visibility ray
    float3 directionToCamera = gScene.camera.getPosition() - sd.posW;
    float distanceToCamera = length(directionToCamera);
    directionToCamera /= distanceToCamera;
    distanceToCamera -= gScene.camera.data.nearZ; // Remove near
    if (!dispatchShadowRay(sd, directionToCamera, distanceToCamera))
        return false;

    //Sample is valid
    return true;
}

[shader("miss")]
void miss(inout RayPayload rayData: SV_RayPayload)
{
    HitInfo hitInfo = {};
    rayData.packedHitInfo = hitInfo.getData(); // Invalid hit
}

// For alpha test
[shader("anyhit")]
void anyHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("closesthit")]
void closestHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes) {
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dispatchDims = DispatchRaysDimensions().xy;

    SampleGenerator sg = SampleGenerator(pixel, gFrameCount);

    // Generate a light sample
    bool isEmissive = (dispatchDims.x * gEmissiveLightProb) > pixel.x; // Use x dims for selection
    LightSample ls;
    if (!generateLightSample(isEmissive, sg, ls))
        return;

    // Prepare ray data
    RayPayload rayPayload = RayPayload();
    RayDesc ray = {};
    ray.Origin = ls.posW;
    ray.Direction = ls.dirW;
    ray.TMin = 0;
    ray.TMax = FLT_MAX;

    //Loop variables
    float3 flux = ls.flux;
    uint diffuseBounces = 0;
    bool lastHitSpecular = false;
    for (uint i = 0; i < gMaxPathLength; i++)
    {
        TraceRay(gScene.rtAccel, 0 /*Flags*/, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayPayload);

        HitInfo hit = HitInfo(rayPayload.packedHitInfo);
        if (!hit.isValid())
            return;

        // Get shading infos
        let lod = ExplicitLodTextureSampler(0);

        // Get Material properties
        uint hints = (uint)MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hit, -ray.Direction, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = mi.getProperties(sd);
        bool isDiffuse = bsdfProperties.roughness > gRoughnessThreshold;

        //Hit with emissive materials are undefined
        if (any(bsdfProperties.emission > 0))
            break;

        // Store caustic if this hit is diffuse and the last was specular
        if (isDiffuse && lastHitSpecular)
        {
            // Get insert index for the data
            uint causticInsertIndex = 0;
            InterlockedAdd(gCounter[kCounterLightPaths], 1u, causticInsertIndex);
            //Data
            CausticData cd = CausticData();
            cd.packedHit = hit.getData();
            cd.flux = flux;
            cd.pathLength = i;
            cd.wi = ray.Direction;

            // If the caustic is visible, it can be backprojected
            uint2 backprojectedPixel = uint2(0);
            if (isVisibleByCamera(sd, backprojectedPixel))
            {
                //Set Head Buffer and get next pointer
                int nodeNext = -1;
                InterlockedExchange(gCausticHead[backprojectedPixel], causticInsertIndex, nodeNext);

                //Store caustic data
                cd.next = nodeNext;
            }
            gCausticData[causticInsertIndex] = cd;

            // Store Caustic AABB
            float photonRadius = getPhotonRadius(sd.posW);
            AABB photonAABB = AABB(sd.posW - photonRadius, sd.posW + photonRadius);
            gCausticAABB[causticInsertIndex] = photonAABB;
        }

        //Sample bounce
        BSDFSample bsdfSample;
        bool validSample = mi.sample(sd, sg, bsdfSample, true /*ImportanceSampling*/);
        if (isDiffuse)
            diffuseBounces++;

        //Path ends here
        if(!validSample || (diffuseBounces > gDiffuseBounces))
            return;

        //Calc next ray and update flux
        bool transmission = bsdfSample.isLobe(LobeType::Transmission);
        ray.Origin = sd.computeNewRayOrigin(!transmission); // false is for transmissions
        ray.Direction = bsdfSample.wo;
        flux *= bsdfSample.weight;
        lastHitSpecular = !isDiffuse;
    }
}