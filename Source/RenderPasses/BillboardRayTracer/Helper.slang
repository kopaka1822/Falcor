

float3x3 GetBillboardPlane(float3 normal, bool yAlign) {
	float3x3 planeBase;
	planeBase[2] = normal;
	
	// y axis alignment
	if(yAlign) planeBase[2] = normalize(float3(planeBase[2].x, 0.0, planeBase[2].z));

	float3 up = float3(0.0, 1.0, 0.0);
	// x, y are plane tangents
	planeBase[0] = normalize(cross(up, planeBase[2]));
	planeBase[1] = cross(planeBase[2], planeBase[0]);
	return planeBase;
}

VertexData getBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 pointOnBillboard)
{
    VertexData v;

    // reconstruct billboard plane
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 P = bbox.center();
    const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;
    //const bool round = BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE;

    // billboard plane base
    const float3x3 planeBase = GetBillboardPlane(normalize(rayOrigin - P), yAlign);
    v.posW = pointOnBillboard;
    v.normalW = planeBase[2];
    v.tangentW.xyz = planeBase[0];
    v.tangentW.w = 1.0;
    v.faceNormalW = planeBase[2];
    v.coneTexLODValue = 0.0;
    v.texC.x = dot(P - v.posW, planeBase[0]) / dim.x + 0.5;
    v.texC.y = dot(P - v.posW, planeBase[1]) / dim.y + 0.5;
    return v;
}

float3 evalBillboardMaterial(ShadingData sd, LightData light, float shadowFactor)
{
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR) return evalMaterial(sd, light, shadowFactor).color;
    // particle
    LightSample ls = evalLight(light, sd);
    // ignore normals => use smooth gradient of light passing through the volume
    float occlusion = smoothstep(1.0 - sd.opacity, 1.0, (ls.NdotL + 1.0) * 0.5);
    return ls.diffuse * sd.diffuse * occlusion;
}


// imported and modified ray differentials
#include "RenderPasses/Shared/PathTracer/RayFootprintModes.slangh"
#if ((RAY_FOOTPRINT_MODE == RAY_FOOTPRINT_MODE_RAYDIFFS_ISO) || (RAY_FOOTPRINT_MODE == RAY_FOOTPRINT_MODE_RAYDIFFS_ANISO))

extension RayFootprint
{
    static RayFootprint create(uint2 pixel, uint2 frameDim)
    {
        RayFootprint res;
        const float3 nonNormalizedRayDir = gScene.camera.computeNonNormalizedRayDirPinhole(pixel, frameDim);
        float3 dDdx, dDdy;
        computeRayDirectionDifferentials(nonNormalizedRayDir, gScene.camera.data.cameraU, gScene.camera.data.cameraV, frameDim, dDdx, dDdy);
        res.rayDiff = RayDiff.create(float3(0.0), float3(0.0), dDdx, dDdy);
        return res;
    }

    [mutating]
    void hitSurface(inout VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        // Propagate to hit point.
        float hitT = length(v.posW - rayOrg);
        float3 geometricNormal = gScene.getFaceNormalW(hit.instanceID, hit.primitiveIndex);
        this.rayDiff = this.rayDiff.propagate(rayOrg, rayDir, hitT, geometricNormal);   // Propagate the ray differential to the current hit point.
    }

    ShadingData computeShadingData(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        float hitT = length(v.posW - rayOrg);

        // Get hit point adapted parameters.
        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        prepareRayDiffAtHitPoint(v, triangleVertices, hit.getBarycentricWeights(), rayDir, hitT, gScene.getWorldMatrix(hit.instanceID), gScene.getInverseTransposeWorldMatrix(hit.instanceID),
            this.rayDiff, dUVdx, dUVdy);

        // Compute shading data.
        float3 viewDir = -rayDir;
        ShadingData sd;
        const uint materialID = gScene.getMaterialID(hit.instanceID);
        if (RAY_FOOTPRINT_MODE == RAY_FOOTPRINT_MODE_RAYDIFFS_ISO)
        {
            // When using prepareShadingDataUsingRayDiffsLOD(), the texture sampler will compute a single lambda for texture LOD
            // using the dUVdx and dUVdy.
            sd = prepareShadingDataUsingRayDiffsLOD(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], viewDir, dUVdx, dUVdy);
        }
        else        // TexLODMode::RayDiffsAnisotropic.
        {
            // Use the dUVdx and dUVdy directly --> gives you anisotropic filtering.
            sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], viewDir, dUVdx, dUVdy);
        }

        return sd;
    }

    [mutating]
    void bounceOnSurface(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 normal, float3 rayDirIn, float3 rayDirOut, bool isTransmission)
    {
        // Check if scattered ray is refracted.
        if (isTransmission || dot(rayDirOut, normal) <= 0.0) return; // Leaves the footprint unchanged until refraction if properly handled.

        // TODO: Avoid recomputing everything (dUVdx, dUVdy and  dBarydx, dBarydy) by keeping intermediate variables live in registers.
        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        reflectRayDiffUsingVertexData(v, triangleVertices, hit.getBarycentricWeights(), rayDirIn, gScene.getWorldMatrix(hit.instanceID), gScene.getInverseTransposeWorldMatrix(hit.instanceID),
                                        this.rayDiff, dUVdx, dUVdy);
    }
};

/** Ray footprint for tex LOD.
    Force LOD0 version.
    Makes the interface consistant even when ray footprint is disabled.
*/
#else
extension RayFootprint
{
    static RayFootprint create(uint2 pixel, uint2 frameDim)
    {
        RayFootprint res;
        return res;
    }

    [mutating]
    void hitSurface(inout VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 rayOrg, float3 rayDir)
    {
    }

    [mutating]
    void bounceOnSurface(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 normal, float3 rayDirIn, float3 rayDirOut, bool isTransmission)
    {
    }

    ShadingData computeShadingData(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        const uint materialID = gScene.getMaterialID(hit.instanceID);
        return prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], -rayDir, 0.f);
    }
};
#endif


/** Prepare shading data using ray footprint and TexLOD.
   \param[in] v The mesh vertex data interpolated at hit point.
   \param[in] rayFootprint The ray footprint to use for TexLOD.
   \param[in] triangleVertices The vertex data of the 3 vertices of the triangle.
   \param[in] hit Ray hit info at hit point.
   \param[in] rayOrg Ray origin.
   \param[in] rayDir Ray direction.
   \return Shading data computed at hit point.
*/
ShadingData prepareShadingData(VertexData v, RayFootprint rayFootprint, StaticVertexData triangleVertices[3], HitInfo hit, float3 rayOrg, float3 rayDir)
{
    return rayFootprint.computeShadingData(v, triangleVertices, hit, rayOrg, rayDir);
}
