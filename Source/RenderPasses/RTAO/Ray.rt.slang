import Scene.Camera.Camera;
import RTAOData;
import Scene.Intersection;
import Scene.Raytracing;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.MathHelpers;


Texture2D<float4> gWPosTex;
Texture2D<float3> gFaceNormalTex;
RWTexture2D<float> ambientOut;

Texture1D<float4> gSamples;

cbuffer StaticCB
{
    RTAOData gData;
}

cbuffer PerFrameCB
{
    Camera gCamera;
    uint frameIndex;
}

struct RayData
{
    float t; // ray intersection
};

[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.t = 1;
}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit(); // TODO proper lod
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.t = RayTCurrent();
}

float3 getTangentVector(float3 n)
{
    return abs(n.x) > abs(n.y) ? float3(-n.z, 0.0, n.x) : float3(0.0, n.z, -n.y); // from pbrt
}

[shader("raygeneration")]
void rayGen()
{
    const uint2 svPos = DispatchRaysIndex().xy;
    
    // non-linear depth
    float4 wPos = gWPosTex[svPos];
    if (wPos.w <= 0.0) //0 in w is miss
    {
        ambientOut[svPos] = 0;
        return;
    }

    // view space normal of current pixel
    float3 faceNormal = normalize(gFaceNormalTex[svPos]);
    // face forward
    if (dot(faceNormal, wPos.xyz - gCamera.data.posW) > 0)
    {
        faceNormal = -faceNormal;
    }

    // determine tangent space
    float3 bitangent = getTangentVector(faceNormal);
    float3 tangent = cross(bitangent, faceNormal);

    // Init sample generator
    SampleGenerator sg = SampleGenerator(svPos, frameIndex % 30u);

    RayDesc ray;
    ray.Origin = wPos.xyz;
    ray.Origin += faceNormal * gData.normalScale; // push origin in the direction of the face normal to avoid self intersection
    ray.TMin = 0.00;
    ray.TMax = gData.radius;

    RayData rayData;

    float tSum = 0.0;
    for (uint i = 0; i < gData.spp; i++)
    {
        float pdf; // needed by the sample function
        float3 randDir = sample_cosine_hemisphere_polar(sampleNext2D(sg), pdf);
        ray.Direction = normalize(tangent * randDir.x + bitangent * randDir.y + faceNormal * randDir.z);

        //rayData.t = gData.radius; // set to miss

        TraceRay(
            gScene.rtAccel,
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, // | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
            ray, rayData
        );

        // tSum += 1.0-saturate(rayData.t / gData.radius);
        tSum += saturate(rayData.t / gData.radius);
    }

    tSum /= gData.spp;
    ambientOut[svPos] = tSum;
}
