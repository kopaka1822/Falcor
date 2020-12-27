

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

VertexData getBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 rayDirection, float rayTCurrent, float rayTMax)
{
    VertexData v;
    const float3 pointOnBillboard = rayOrigin + rayDirection * rayTCurrent;

    // reconstruct billboard plane
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 P = bbox.center();

    switch(BILLBOARD_TYPE)
    {
    case BILLBOARD_TYPE_IMPOSTOR:
    case BILLBOARD_TYPE_PARTICLE: {
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
        v.billboardStrength = 1.0;
        if(BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE && USE_SOFT_PARTICLES)
            v.billboardStrength = saturate((rayTMax - rayTCurrent) * dim.x);
    } break;
    case BILLBOARD_TYPE_SPHERICAL: {
        v.posW = pointOnBillboard; // closest entry point
        v.normalW = normalize(v.posW - P);
        v.tangentW = 0.0; // does not really matter
        v.faceNormalW = v.normalW;
        v.coneTexLODValue = 0.0;

        v.texC.x = v.normalW.x == 0.0 ? 0.25 * sign(v.normalW.z) : atan2(v.normalW.z, v.normalW.x) / (2.0 * M_PI);
        v.texC.y = acos(v.normalW.y) / M_PI;
        // give each bilboard a random texture rotation
        v.texC.x += aabbOffset + 0.172;

        // determine depth
        const float r = 0.5 * dim.x;
        const float rayT = dot(rayDirection, P - rayOrigin); // rayT to closest point inside billboard
        const float d = length(rayOrigin + rayT * rayDirection - P); // smallest distance
        const float w = sqrt(r * r - d * d);
        const float ds = min(rayTMax, rayT + w) - rayTCurrent;

        v.billboardDepth = ds / (2.0 * r);
        v.billboardStrength = 1.0 - (d / r);
    } break;
    }
    return v;
}

// imported and modified ray differentials
#include "RenderPasses/Shared/PathTracer/RayFootprintModes.slangh"
#if (RAY_FOOTPRINT_MODE == RAY_FOOTPRINT_MODE_RAYDIFFS_ANISO)

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
    void hitSurface(inout VertexData v, HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        // Propagate to hit point.
        float hitT = length(v.posW - rayOrg);
        this.rayDiff = this.rayDiff.propagate(rayOrg, rayDir, hitT, v.faceNormalW);   // Propagate the ray differential to the current hit point.
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
        // Use the dUVdx and dUVdy directly --> gives you anisotropic filtering.
        sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], viewDir, dUVdx, dUVdy);

        return sd;
    }

    ShadingData computeBillboardShadingData(VertexData v, HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        // TODO store data somewhere instead of recomputing
        // reconstruct billboard plane
        const AABB bbox = gScene.proceduralPrimitiveAABBs[hit.primitiveIndex];
        const float2 dim = bbox.extent().xy;
        const float3 P = bbox.center();
        const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;

        // billboard plane base
        float3x3 planeBase = GetBillboardPlane(normalize(rayOrg - P), yAlign);

        // Get hit point adapted parameters.
        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        dUVdx.x = dot(this.rayDiff.dOdx, planeBase[0]) / dim[0];
        dUVdx.y = dot(this.rayDiff.dOdx, planeBase[1]) / dim[1];
        dUVdy.x = dot(this.rayDiff.dOdy, planeBase[0]) / dim[0];
        dUVdy.y = dot(this.rayDiff.dOdy, planeBase[1]) / dim[1];

        // Compute shading data.
        float3 viewDir = -rayDir;
        ShadingData sd;
        const uint materialID = BILLBOARD_MATERIAL_ID;
        // Use the dUVdx and dUVdy directly --> gives you anisotropic filtering.
        sd = prepareShadingData(v, materialID, gScene.materials[materialID], gScene.materialResources[materialID], viewDir, dUVdx, dUVdy);

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
    void hitSurface(inout VertexData v, HitInfo hit, float3 rayOrg, float3 rayDir)
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

    ShadingData computeBillboardShadingData(VertexData v, HitInfo hit, float3 rayOrg, float3 rayDir)
    {
        const uint materialID = BILLBOARD_MATERIAL_ID;
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

ShadingData prepareBillboardShadingData(VertexData v, RayFootprint rayFootprint, HitInfo hit, /*long version*/ float3 rayOrg,  float3 rayDir)
{
    ShadingData sd = rayFootprint.computeBillboardShadingData(v, hit, rayOrg, rayDir);
    if(BILLBOARD_TYPE != BILLBOARD_TYPE_IMPOSTOR) sd.isVolume = true;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE) sd.opacity *= v.billboardStrength;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL && sd.opacity < 1.0)
    {
        // determine correct opacity
        const float nt = log(1.0 - sd.opacity);
        sd.opacity = 1.0 - exp(nt * v.billboardDepth * v.billboardStrength);
    }
    return sd;
}
