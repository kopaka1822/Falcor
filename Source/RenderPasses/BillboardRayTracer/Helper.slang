struct VolumePayload
{
    float3 colorSum;
    float alphaSum;
    float transmit;
    float rayTMax;
    RayFootprint footprint; // up to 12 floats
}

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

float getSoftParticleContrast(float x)
{
    const float c = 2.0; // contrast in [1, 5]
    x = saturate(x);
    if(x <= 0.5) return 0.5 * pow(2.0 * x, c);
    return 1.0 - 0.5 * pow(2.0 * (1.0 - x), c);
}

VertexData getBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 rayDirection, float rayTCurrent, float rayTMin, float rayTMax)
{
    VertexData v;

    // reconstruct billboard plane
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 P = bbox.center();

    // billboard plane base
    const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;
    const float3x3 planeBase = GetBillboardPlane(normalize(rayOrigin - P), yAlign);

    v.posW = rayOrigin + rayDirection * rayTCurrent;
    v.normalW = planeBase[2];
    v.tangentW.xyz = planeBase[0];
    v.tangentW.w = 1.0;
    v.faceNormalW = planeBase[2];
    v.coneTexLODValue = 0.0;
    v.texC.x = dot(P - v.posW, planeBase[0]) / dim.x + 0.5;
    v.texC.y = dot(P - v.posW, planeBase[1]) / dim.y + 0.5;
    v.billboardStrength = 1.0;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE && USE_SOFT_PARTICLES)
    {
        float blendScale = 4.0 / dim.x;
        float outFade = getSoftParticleContrast((rayTMax - rayTCurrent) * blendScale);
        float inFade = getSoftParticleContrast((rayTCurrent - rayTMin) * blendScale);
        //float inFade = 1.0;
        v.billboardStrength = min(outFade, inFade);
    }

    return v;
}

VertexData getSphericalBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 rayDirection, float rayTCurrent, float rayTMin, float rayTMax)
{
    VertexData v;

    // reconstruct billboard plane
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 P = bbox.center();

    // determine depth
    const float r = 0.5 * dim.x;
    const float rayT = dot(rayDirection, P - rayOrigin); // rayT to closest point inside billboard
    const float d = length(rayOrigin + rayT * rayDirection - P); // smallest distance
    const float w = sqrt(r * r - d * d);
    const float ds = min(rayTMax, rayT + w) - rayTCurrent;

    v.billboardDepth = ds;
    v.billboardStrength = (1.0 - (d / r)) * (ds / (2.0 * r));

    v.posW = rayOrigin + rayDirection * rayTCurrent; // closest entry point
    v.normalW = normalize(rayOrigin + rayDirection * (rayT - w) - P); // direction to unclipped entry point (for consistent texture coordinates when traversing)
    v.tangentW = 0.0; // does not really matter
    v.faceNormalW = v.normalW;
    v.coneTexLODValue = 0.0;

    v.texC.x = v.normalW.x == 0.0 ? 0.25 * sign(v.normalW.z) : atan2(v.normalW.z, v.normalW.x) / (2.0 * M_PI);
    v.texC.y = acos(v.normalW.y) / M_PI;
    // give each bilboard a random texture rotation
    v.texC.x += aabbOffset * 0.172;

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
            float3x3 planeBase = GetBillboardPlane(normalize(rayOrg - P), yAlign);

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

// simple shading function that iterates though all lights and traces shadow rays for the primary light source
// \returns rgb color and transmittance
float4 shade(ShadingData sd, bool useBillboardDepth = false, float3 secondShadowPos = float3(0, 0, 0))
{
    float3 curColor = sd.emissive;
    const float curTransmit = 1.0 - sd.opacity;

    // forward render all light sources
    const uint lightCount = gScene.getLightCount();
    for(uint i = 0; i < lightCount; ++i)
    {
        LightData l = gScene.getLight(i);
        [branch] if(useBillboardDepth && (i == 0))
        {
            float3 color = evalMaterial(sd, l, 1.0).color;
            float shadowSum = 0.0;
            float scale = 1.0 / (BILLBOARD_SHADOW_SAMPLES - 1);
            for(uint j = 0; j < BILLBOARD_SHADOW_SAMPLES; ++j)
            {
                shadowSum += TraceShadowRay(lerp(sd.posW, secondShadowPos, j * scale), l);
            }
            curColor += color * (shadowSum / BILLBOARD_SHADOW_SAMPLES);
        }
        else
        {
            float shadowFac = 1.0;
            if(i == 0) shadowFac = TraceShadowRay(sd.posW, l);
            curColor += evalMaterial(sd, l, shadowFac).color;
        }
    }

    curColor *= sd.opacity;

    return float4(curColor, curTransmit);
}

// returns depth weight for Weighted-Blended OIT
float getDepthWeight(float alpha, float z)
{
    // transform z to non-linear
    // Phenomenological:
    const float near = gScene.camera.data.nearZ;
    const float far = gScene.camera.data.farZ;
    float f = ((near * far) / clamp(z, near, far) - far) / (near - far);
    
    float w = 10 * (1 - 0.99 * f); // modification: did not include alpha here (looks better for smoke)
    return clamp(w * w * w, 0.01, 3000.0) * alpha;
    //return 1.0;
}
