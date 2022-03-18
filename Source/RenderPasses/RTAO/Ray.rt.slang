import Scene.Camera.Camera;
import RTAOData;
import Scene.Intersection;
import Scene.Raytracing;
import Utils.Geometry.GeometryHelpers;

Texture2D<float4> gWPosTex;
Texture2D<float3> gNormalTex;
Texture2D<float3> gFaceNormalTex;
RWTexture2D<float> ambientOut;
RWTexture2D<float> rayDistanceOut;

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
{}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
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

uint jenkins(uint a)
{
    a -= (a << 6);
    a ^= (a >> 17);
    a -= (a << 9);
    a ^= (a << 4);
    a -= (a << 3);
    a ^= (a << 10);
    a ^= (a >> 15);
    return a;
}

uint hash(uint3 c)
{
    // xy = screen position offset, z = frame index
    return jenkins(c.x * 449 + c.y * 2857 + jenkins(c.z));
}

float3 getWorldPosition(uint2 svPos, float depth)
{
    float2 texC = (float2(svPos) + 0.5) * gData.invResolution;
    texC.y = 1.0 - texC.y; // y is flipped
    float2 ndc = texC * 2.0 - 1.0;

    float4 pos;
    pos.x = ndc.x;
    pos.y = ndc.y;
    pos.z = depth;
    pos.w = 1.0f;

    float4 posW = mul(pos, gCamera.data.invViewProj);
    posW /= posW.w;

    return posW.xyz;
}

[shader("raygeneration")]
void rayGen()
{
    uint2 svPos = DispatchRaysIndex().xy;
    
    // non-linear depth
    float4 wPos = gWPosTex[svPos];
    if (wPos.w == 0.0) //0 in w is miss
    {
        ambientOut[svPos] = 0;
        return;
    }

    // view space normal of current pixel
    float3 normal = normalize(gFaceNormalTex[svPos]);
    float3 faceNormal = normalize(gFaceNormalTex[svPos]);

    //flip normal if backfacing
    float3 camDir = normalize(gScene.camera.computeNonNormalizedRayDirPinhole(svPos, DispatchRaysDimensions().xy));
    if (dot(camDir, faceNormal) > 0)
    {
        normal = -normal;
        faceNormal = -faceNormal;
    }
   
    // determine tangent space
    float3 bitangent = getTangentVector(normal);
    float3 tangent = cross(bitangent, normal);
    
    float visibility = 1.0f;

    // onbtain a single ray sample that is cosine distributed
    uint numSamples;
    gSamples.GetDimensions(numSamples);
    float3 randDir = gSamples[hash(uint3(svPos, frameIndex)) % numSamples].xyz;

    visibility = 0.f;

    RayDesc ray;
    ray.Origin = wPos.xyz;
    ray.Direction = normalize(tangent * randDir.x + bitangent * randDir.y + normal * randDir.z);
    ray.Origin += faceNormal * gData.rayNormalOffset; // push origin in the direction of the face normal to avoid self intersection
    ray.TMin = 0.0;
    ray.TMax = gData.radius;

    RayData rayData;
    rayData.t = gData.radius;
    TraceRay(
        gScene.rtAccel,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, // | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
        ray, rayData
    );

    rayDistanceOut[svPos] = rayData.t;
    visibility = saturate(rayData.t / gData.radius);
    ambientOut[svPos] = 1-visibility;
}
