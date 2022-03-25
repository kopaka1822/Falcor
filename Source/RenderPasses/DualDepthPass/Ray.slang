#include "Scene/ScenePrimitiveDefines.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Scene.Scene;
import Scene.Shading;
import Scene.HitInfo;
import Utils.Math.Ray;
import Utils.Math.MathHelpers;
import Experimental.Scene.Material.TexLODTypes;
import Experimental.Scene.Material.TexLODHelpers;

Texture2D<float> primaryDepth;
RWTexture2D<float> secondaryDepth;

struct RayData
{
    float invCos;
};

[shader("miss")]
void miss(inout RayData rayData)
{
    // write nothing
}

/** Any hit shader for triangles.*/
[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.0f))
        IgnoreHit();
}

/** Closest hit shader for triangles.
*/
[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    float zNear = gScene.camera.data.nearZ;
    float zFar = gScene.camera.data.farZ;

    // write depth
    float linearDepth = RayTCurrent() / rayData.invCos;
    // make non linear
    float nonLinearDepth = ((zNear * zFar / linearDepth) - zFar) / (zNear - zFar);
    secondaryDepth[DispatchRaysIndex().xy] = nonLinearDepth;
    //secondaryDepth[DispatchRaysIndex().xy] = linearDepth * 0.01;
}

/** ******************************** RayGen ******************************** */

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    // Trace ray.
    RayDesc ray;
    ray.Origin = gScene.camera.data.posW;
    ray.Direction = normalize(gScene.camera.computeNonNormalizedRayDirPinhole(pixel, frameDim));

    float invCos = 1.f / dot(normalize(gScene.camera.data.cameraW), ray.Direction);
    float zNear = gScene.camera.data.nearZ;
    float zFar = gScene.camera.data.farZ;
    ray.TMin = zNear * invCos;
    ray.TMax = zFar * invCos;
    // include primary depth buffer depth in tMin
    float linearPrimaryDepth = zNear * zFar / (zFar + primaryDepth[pixel] * (zNear - zFar));
    ray.TMin = max(ray.TMin, linearPrimaryDepth * invCos + 0.001);
    //w = n * f / (f + z * (n - f)
    RayData rayData;
    rayData.invCos = invCos;
    TraceRay(gScene.rtAccel, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}
