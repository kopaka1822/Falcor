#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"
import Rendering.Materials.TexLODHelpers;
import Rendering.Materials.TexLODTypes;
import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;


#ifndef MAX_ELEMENTS
#define MAX_ELEMENTS 256
#endif

//#define USE_LOD

/** Payload for shadow ray.
*/
struct RayData
{
    float visibility; // accumulated visibility
    float depth; // depth of last intersection (for the linked list)
    float rayT; // RayTCurrent of last intersection
    __init()
    {
        this.visibility = 1.0;
        this.depth = 0.0;
        this.rayT = 0.0;
    }
};

cbuffer CB
{
    uint        gFrameCount;       //Current frame count
    float3      gLightPos;     // Current Light Position
    float       gNear;
    float       gFar;
    float       gRayConeAngle; // spread angle
    uint        _pad;
    float4x4    gViewProj;      // View Projection for current light
    float4x4    gInvViewProj;   // Inverse view projection
    float4x4    gView;
}

struct ListNode
{
    float depth;
    float visibility;
    uint next;
};

struct ArrayNode
{
    float depth;
    float visibility;

    __init(ListNode n)
    {
        this.depth = n.depth;
        this.visibility = n.visibility;
    }
};

RWStructuredBuffer<ListNode> gLinkedList;
RWBuffer<uint> gCounter; // counter for the nodes

// array members
RWStructuredBuffer<ArrayNode> gArray;
RWTexture2D<uint2> gArrayOffsets; // stores offsets for the array: (start, count) pairs.
RWBuffer<uint> gCounter2; // counter for array storage

static const float kDepthBias = AVSM_DEPTH_BIAS;
static const float kNormalBias = AVSM_NORMAL_DEPTH_BIAS;
static const float kMinOpacity = 1e-7;

ExplicitRayConesLodTextureSampler computeLod(VertexData v, float3 rayDir)
{
    RayCone rc = RayCone(0.0, gRayConeAngle);
    rc = rc.propagateDistance(RayTCurrent());
    float lambda = rc.computeLOD(v.coneTexLODValue, rayDir, v.faceNormalW);
    return ExplicitRayConesLodTextureSampler(lambda);
}

/** Setup ShadingData based on loaded vertex/material attributes for a hit point.
    \param[in] hit Hit information.
    \param[in] rayOrigin Ray origin.
    \param[in] rayDir Normalized ray direction.
    \param[in] lod Method for computing texture level-of-detail.
    \return ShadingData struct.
*/
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    return sd;
}

//Shaders
[shader("miss")]
void miss(inout RayData rayData)
{
    rayData.depth = -1.0; // indicate miss
}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    // TODO lod
    #ifndef USE_LOD
    let lod = ExplicitLodTextureSampler(0.f);
    #else
    let lod = computeLod(v, WorldRayDirection());
    #endif
    if (gScene.materials.alphaTest(v, materialID, lod, kMinOpacity)) 
        IgnoreHit();
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    #ifndef USE_LOD
    let lod = ExplicitLodTextureSampler(0.f);
    #else
    let lod = computeLod(v, WorldRayDirection());
    #endif
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -WorldRayDirection(), lod);

    rayData.visibility = rayData.visibility * (1.0 - sd.opacity);
    rayData.depth = -1.0 * mul(gView, float4(sd.posW, 1.f)).z;
    float normalBias = (1.0 - abs(dot(WorldRayDirection(), sd.faceN))) * kNormalBias;
    rayData.depth += (kDepthBias + normalBias) * gFar;
    rayData.rayT = RayTCurrent();
}

//
// Shader entry point for ray generation shader.
//
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    uint numElements = 0;

    float2 pixelSample = pixel + 0.5;

    float2 relFramePos = pixelSample / frameDim;
    relFramePos = relFramePos * 2.0 - 1.0; // From [0,1] to [-1,1]
    relFramePos.y *= -1.f;

        
    float4 posWPixelNear = mul(gInvViewProj, float4(relFramePos, 0.f, 1.f));
    float4 posWPixelFar = mul(gInvViewProj, float4(relFramePos, 1.f, 1.f));

    posWPixelNear.xyz /= posWPixelNear.w;
    float3 toNear = posWPixelNear.xyz - gLightPos;

    RayDesc ray;
    ray.TMin = length(toNear);;                                             // Always start at depth 0
    ray.TMax = distance(gLightPos, posWPixelFar.xyz / posWPixelFar.w); // Real far for the pixel
    ray.Origin = gLightPos;
    ray.Direction = normalize(toNear); // World direction

    RayData rayData = RayData();

    ListNode prevNode;
    prevNode.depth = FLT_MAX;
    prevNode.visibility = 1.0;
    prevNode.next = 0;
    uint prevNodeIndex = pixel.y * frameDim.x + pixel.x;
    uint nodeIndex = prevNodeIndex;

    // Loop to get all possible samples
    for (uint i = 0; i < MAX_ELEMENTS; i++) {

        //Trace ray
        uint rayFlags = 0;
        TraceRay(gScene.rtAccel, rayFlags, kShadowRayInstanceMask /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        if(rayData.depth < 0) break; // miss shader

        // get storage for new point
        nodeIndex = prevNodeIndex;
        if(i > 0) // for first iteration, just overwrite the initial node
        {
            InterlockedAdd(gCounter[0], 1u, nodeIndex);

            if(nodeIndex >= MAX_INDEX) break; // cannot store further nodes, abort

            // complete and write last node
            prevNode.next = nodeIndex;
            gLinkedList[prevNodeIndex] = prevNode;
            numElements += 1;
        }

        // create new node storage
        ListNode node;
        node.depth = rayData.depth;
        node.visibility = rayData.visibility;
        node.next = 0;
        
        // update for next iteration
        prevNode = node;
        prevNodeIndex = nodeIndex;
        ray.TMin = rayData.rayT;  
        
        if(rayData.visibility <= kMinOpacity) break; // stop if visibility is too low)
    }

    // write last node
    gLinkedList[prevNodeIndex] = prevNode;
    numElements += 1;

#ifdef USE_LINKED_LIST_ARRAY
    uint arrayStartIndex = 0;
    InterlockedAdd(gCounter2[0], numElements, arrayStartIndex);
    // store start index and number of elements
    gArrayOffsets[pixel] = uint2(arrayStartIndex, numElements);

    // write linked list to array
    uint index = arrayStartIndex;
    nodeIndex = pixel.y * frameDim.x + pixel.x;
    for (uint index = arrayStartIndex; index < arrayStartIndex + numElements; index++)
    {
        ListNode node = gLinkedList[nodeIndex];
        gArray[index] = ArrayNode(node);
        nodeIndex = node.next;
    }
#endif
}
