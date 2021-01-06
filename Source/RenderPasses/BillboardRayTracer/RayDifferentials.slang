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
    void hitSurface(inout VertexData v, float3 rayOrg, float3 rayDir)
    {
        // Propagate to hit point.
        float hitT = length(v.posW - rayOrg);
        this.rayDiff = this.rayDiff.propagate(rayOrg, rayDir, hitT, v.faceNormalW);   // Propagate the ray differential to the current hit point.
    }

    [mutating]
    void hitSphere(float3 rayOrg, float3 rayDir, float3 sphereCenter, float sphereRadius)
    {
        this.rayDiff = this.rayDiff.propagateSphere(rayOrg, rayDir, sphereCenter, sphereRadius);   // Propagate the ray differential to the current hit point.
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

    ShadingData computeBillboardShadingData(VertexData v, uint aabbIndex, float3 rayOrg, float3 rayDir)
    {
        // TODO store data somewhere instead of recomputing
        // reconstruct billboard plane
        const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbIndex];
        const float2 dim = bbox.extent().xy;
        const float3 P = bbox.center();
        const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;

        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        if(BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL)
        {
            const float3 d = v.normalW;
            float3 dNdx = this.rayDiff.dOdx / dim.x;
            float3 dNdy = this.rayDiff.dOdy / dim.x;

            dUVdx.x = (-d.z * dNdx.x  + d.x * dNdx.z) / (2.0 * M_PI * (d.x * d.x + d.z * d.z));
            dUVdx.y = -dNdx.y / (sqrt(1.0 - d.y * d.y) * M_PI);
            dUVdy.x = (-d.z * dNdy.x  + d.x * dNdy.z) / (2.0 * M_PI * (d.x * d.x + d.z * d.z));
            dUVdy.y = -dNdy.y / (sqrt(1.0 - d.y * d.y) * M_PI);
        }
        else
        {
            // billboard plane base
            float3x3 planeBase = GetBillboardTangentSpace(normalize(rayOrg - P), yAlign);

            // Get hit point adapted parameters.
            dUVdx.x = dot(this.rayDiff.dOdx, planeBase[0]) / dim[0];
            dUVdx.y = dot(this.rayDiff.dOdx, planeBase[1]) / dim[1];
            dUVdy.x = dot(this.rayDiff.dOdy, planeBase[0]) / dim[0];
            dUVdy.y = dot(this.rayDiff.dOdy, planeBase[1]) / dim[1];
        }

        

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
    void hitSurface(inout VertexData v, float3 rayOrg, float3 rayDir)
    {
    }

    [mutating]
    void hitSphere(float3 rayOrg, float3 rayDir, float3 sphereCenter, float sphereRadius)
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

    ShadingData computeBillboardShadingData(VertexData v, uint aabbIndex, float3 rayOrg, float3 rayDir)
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

static const float3 gRandomColors[] = {float3(1.0, 0, 0), float3(1.0, 0.1, 0.9), float3(0.1, 1, 0.1), float3(0, 1.0, 0.95), float3(1.0, 0.95, 0.0), float3(0.1, 0.1, 1)};

ShadingData prepareBillboardShadingData(VertexData v, RayFootprint rayFootprint, uint aabbIndex, /*long version*/ float3 rayOrg,  float3 rayDir)
{
    ShadingData sd = rayFootprint.computeBillboardShadingData(v, aabbIndex, rayOrg, rayDir);
    if(BILLBOARD_TYPE != BILLBOARD_TYPE_IMPOSTOR) sd.isVolume = true;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE) sd.opacity *= v.billboardStrength;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL && sd.opacity < 1.0) sd.opacity = 1.0 - pow(1.0 - sd.opacity, v.billboardStrength);
    if(USE_RANDOM_BILLBOARD_COLORS)
    {
        sd.diffuse *= gRandomColors[aabbIndex % 6];
    }
    return sd;
}
