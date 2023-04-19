import Scene.RaytracingInline;
import Scene.Raster;
import Rendering.Lights.LightHelpers;

Texture2D<float> gDepth; // indicates depth accesses for the secondary pass

float3 loadPosition(float2 UV, float depth)
{
    float2 resolution;
    gDepth.GetDimensions(resolution.x, resolution.y);
    float2 invSize = 1.0f / resolution;
    // Recompute position by unprojecting the depth stored in the z-buffer
    float2 ndc = 2 * UV + invSize - 1;
#ifndef FALCOR_FLIP_Y
    // NDC Y is bottom-to-top
    ndc.y = -ndc.y;
#endif
    float4 wsPos = mul(float4(ndc.x, ndc.y, depth, 1.f), gScene.camera.data.invViewProj);
    return wsPos.xyz / wsPos.w;
}

float main(float2 texC : TEXCOORD, float4 svPos : SV_POSITION) : SV_Target
{
    float nonlinearDepth = gDepth[int2(svPos.xy)];
    float3 posW = loadPosition(texC, nonlinearDepth);

    if (gScene.getLightCount() == 0)
        return 1.0;
    
    LightData light = gScene.getLight(0);
    RayDesc ray;
    ray.Origin = posW;
    ray.TMin = gScene.camera.data.nearZ;
    
    if (light.type == uint(LightType::Point))
    {
        ray.Direction = normalize(light.posW - posW);
        ray.TMax = distance(light.posW, posW);
    }
    else if (light.type == uint(LightType::Directional))
    {
        ray.Direction = -light.dirW;
        ray.TMax = gScene.camera.data.farZ;
    }

    // TODO RAY_FLAG_CULL_BACK_FACING_TRIANGLES
    RayQuery < RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH > rayQuery;
    rayQuery.TraceRayInline(gScene.rtAccel, RAY_FLAG_NONE, 0xff, ray);
    while (rayQuery.Proceed())
    {
        if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            // alpha test
            const TriangleHit hit = getCandidateTriangleHit(rayQuery);
            const uint materialID = gScene.getMaterialID(hit.instanceID);
            const VertexData v = gScene.getVertexData(hit);
            if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
                continue; // alpha test failed => ignore this triangle

            rayQuery.CommitNonOpaqueTriangleHit();
        }
    }

    float visibility = 1.0;
    if (rayQuery.CommittedStatus() != COMMITTED_NOTHING)
        visibility = 0.0;

    visibility = (QuadReadAcrossX(visibility) + QuadReadAcrossY(visibility) + visibility * 2.0) / 4.0;
    
    return visibility;
}
