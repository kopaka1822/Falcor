import Scene.Raytracing;
import Rendering.Materials.TexLODHelpers;
import Rendering.Materials.TexLODTypes;
import Utils.Math.PackedFormats;

#include "../StochasticDepthMapRT/Jitter.slangh"

RWTexture2D<float> depthOutTex;
Texture2D<float> depthInTex;
SamplerState S; // point sampler

cbuffer PerFrameCB
{
    float maxDepth; // based on radius cutoff
};

float RayToViewDepth(float3 rayDir, float rayT)
{
    float cosTheta = dot(normalize(gScene.camera.data.cameraW), rayDir);
    return rayT * cosTheta;
}

float ViewDepthToRay(float3 rayDir, float viewDepth)
{
    float cosTheta = dot(normalize(gScene.camera.data.cameraW), rayDir);
    return viewDepth / cosTheta;
}

RayDesc initRayDesc(int2 pixel, int2 dim)
{
    dim = dim - 2 * GUARD_BAND; // remove guard band
    int2 signedPixel = pixel - GUARD_BAND; // pixel can be negative or bigger than dim, but thats fine for the ray direction
    RayDesc ray = gScene.camera.computeRayPinhole(signedPixel, dim).toRayDesc();
    ray.Direction = normalize(gScene.camera.computeNonNormalizedRayDirPinhole(signedPixel, dim, randomJitter(pixel)));
    
    //float epsilon = 0.1 * gScene.camera.data.nearZ; // small ray offset
    float epsilon = gScene.camera.data.nearZ; // small ray offset
    float depth = 0.0;
    if (all(signedPixel >= 0) && all(signedPixel < dim)) // use frame buffer depth if pixel is inside frame buffer
        depth = depthInTex.SampleLevel(S, (signedPixel + randomJitter(pixel) + 0.5) / dim, 0);

    ray.TMin = ViewDepthToRay(ray.Direction, depth) + epsilon; // start after first known hit
    ray.TMax = ViewDepthToRay(ray.Direction, maxDepth);
    
    return ray;
}

// Note: using lods (ray cones) is about 20% faster than sampling lod 0 (no lod)
ExplicitRayConesLodTextureSampler computeLod(VertexData v, float3 rayDir)
{
    RayCone rc = RayCone(0.0, RAY_CONE_SPREAD);
    rc = rc.propagateDistance(RayTCurrent());
    float lambda = rc.computeLOD(v.coneTexLODValue, rayDir, v.faceNormalW);
    return ExplicitRayConesLodTextureSampler(lambda);
}


struct RayData // cannot be compressed to half floats => no diff in rendering time + insufficient visual quality
{
    float depth;
    uint count; // number of processed samples
};

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.count += 1;
    if (rayData.count > MAX_COUNT)
        AcceptHitAndEndSearch(); // this saves a lot of time in alpha test dense regions (50% of time for emerald square)
#if USE_ALPHA_TEST
    { // ALPHA test

        const uint materialID = gScene.getMaterialID(getGeometryInstanceID());
        bool alphaTest = gScene.materials.materialData[materialID].header.getAlphaMode() == AlphaMode::Mask;
        
        // needs alpha testing?
        if (alphaTest)
        {
            TriangleHit hit;
            hit.instanceID = getGeometryInstanceID();
            hit.primitiveIndex = PrimitiveIndex();
            hit.barycentrics = attribs.barycentrics;
            
            const VertexData v = gScene.getVertexData(hit);
            if (gScene.materials.alphaTest(v, materialID, computeLod(v, WorldRayDirection())))
            {
                IgnoreHit();
            }
        }
    }
#endif
    //rayData.depth = RayTCurrent(); // will be done in closesthit
}

[shader("miss")]
void miss(inout RayData rayData)
{
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.depth = RayTCurrent();
    //if()
}

[shader("raygeneration")]
void rayGen()
{
    uint2 svPos = DispatchRaysIndex().xy;
    RayDesc ray = initRayDesc(svPos, DispatchRaysDimensions().xy);
    
    RayData rayData;
    rayData.count = 0;
    rayData.depth = 0;

    TraceRay(
        gScene.rtAccel,
        CULL_MODE_RAY_FLAG | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
        ray, rayData
    );

    if (rayData.count > MAX_COUNT)
        rayData.depth = 0.0; // invalid depth (ray was aborted)
    depthOutTex[svPos] = RayToViewDepth(ray.Direction, rayData.depth);
}
