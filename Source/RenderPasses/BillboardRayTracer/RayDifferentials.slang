// imported and modified ray differentials
#include "RenderPasses/Shared/PathTracer/RayFootprintModes.slangh"
#if (RAY_FOOTPRINT_MODE == RAY_FOOTPRINT_MODE_RAYDIFFS_ANISO)

bool SolveLinearSystem2x2(float2x2 A, float2 B, out float2 x) {
    float det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
    if (abs(det) < 1e-10f)
        return false;
    x[0] = (A[1][1] * B[0] - A[0][1] * B[1]) / det;
    x[1] = (A[0][0] * B[1] - A[1][0] * B[0]) / det;
    if (isnan(x[0]) || isnan(x[1]))
        return false;
    return true;
}

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
    void hitSurface(inout VertexData v, float rayT, float3 rayDir)
    {
        this.rayDiff = this.rayDiff.propagate(rayDir, rayT, v.faceNormalW);   // Propagate the ray differential to the current hit point.
    }

    [mutating]
    void hitSphere(float3 rayOrg, float3 rayDir, float3 sphereCenter, float sphereRadius)
    {
        this.rayDiff = this.rayDiff.propagateSphere(rayOrg, rayDir, sphereCenter, sphereRadius);   // Propagate the ray differential to the current hit point.
    }

    ShadingData computeShadingData(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float rayT, float3 rayDir)
    {
        // Get hit point adapted parameters.
        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        prepareRayDiffAtHitPoint(v, triangleVertices, hit.getBarycentricWeights(), rayDir, rayT, gScene.getWorldMatrix(hit.instanceID), gScene.getInverseTransposeWorldMatrix(hit.instanceID),
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
        const float r = dim.x * 0.5;
        const float3 P = bbox.center();
        const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;

        float2 dUVdx, dUVdy;    // Ray differential variables for the texture lookup.
        if(BILLBOARD_TYPE == BILLBOARD_TYPE_SPHERICAL)
        {
            // from pbrt http://www.pbr-book.org/3ed-2018/Texture/Sampling_and_Antialiasing.html#FindingtheTextureSamplingRate
            const float3 n = v.normalW;
            const float2 uv = v.texC;

            //float3 A = rayOrg + rayDir * dot(P - rayOrg, rayDir) - P;
            //float t = dot(P - rayOrg, rayDir) - sqrt(r * r - dot(A, A));
            //float3 p = rayOrg + t * rayDir;
            //
            //float3 px = p + this.rayDiff.dOdx;
            //float3 py = p + this.rayDiff.dOdy;
            //
            //float3 nx = (px - P) / r;
            //float3 ny = (py - P) / r;
            //
            //dUVdx = CalcUVFromNormal(nx, aabbIndex) - uv;
            //dUVdy = CalcUVFromNormal(ny, aabbIndex) - uv;
            int dim[2];
            if(abs(n.x) > abs(n.y) && abs(n.x) > abs(n.z)) {
                dim[0] = 1; dim[1] = 2;
            } else if (abs(n.y) > abs(n.z)) {
                dim[0] = 0; dim[1] = 2;
            } else {
                dim[0] = 0; dim[1] = 1;
            }
            
            // change of position per texture coordinate direction (UV-sphere)
            float theta = (uv.x - 0.5 - aabbIndex * 0.172) * 2.0 * M_PI;
            float phi = M_PI * uv.y;
            float sinphi = sin(phi);
            float cosphi = cos(phi);
            float sintheta = sin(theta);
            float costheta = cos(theta);
            const float3 dpdu = float3(-sinphi*sintheta,0.0,sinphi*costheta) * 2.0 * M_PI * r;
            const float3 dpdv = float3(cosphi*costheta,-sinphi,cosphi*sintheta) * M_PI * r;
            
            float2x2 A;
            A[0][0] = dpdu[dim[0]]; A[0][1] = dpdv[dim[0]];
            A[1][0] = dpdu[dim[1]]; A[1][1] = dpdv[dim[1]];
            
            float3 dx = this.rayDiff.dOdx;
            float3 dy = this.rayDiff.dOdy;
            float2 Bx = float2(this.rayDiff.dOdx[dim[0]], this.rayDiff.dOdx[dim[1]]);
            float2 By = float2(this.rayDiff.dOdy[dim[0]], this.rayDiff.dOdy[dim[1]]);
            
            if(!SolveLinearSystem2x2(A, Bx, dUVdx)) dUVdx = 0.0;
            if(!SolveLinearSystem2x2(A, By, dUVdy)) dUVdy = 0.0;
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
        //sd.diffuse = length(dUVdx);

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
    void hitSurface(inout VertexData v, float rayT, float3 rayDir)
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

    ShadingData computeShadingData(VertexData v, StaticVertexData triangleVertices[3], HitInfo hit, float rayT, float3 rayDir)
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
ShadingData prepareShadingData(VertexData v, RayFootprint rayFootprint, StaticVertexData triangleVertices[3], HitInfo hit, float rayT, float3 rayDir)
{
    return rayFootprint.computeShadingData(v, triangleVertices, hit, rayT, rayDir);
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
