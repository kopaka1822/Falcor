// GetTangentSpace() function from the article with option for y-axis alignment
float3x3 GetBillboardTangentSpace(float3 normal, bool yAlign) {
	float3x3 planeBase;
	planeBase[2] = normal;
	
	// y axis alignment
	if(yAlign) planeBase[2] = normalize(float3(planeBase[2].x, 0.0, planeBase[2].z));

	float3 up = float3(0.0, 1.0, 0.0);
	planeBase[0] = normalize(cross(up, planeBase[2]));
	planeBase[1] = cross(planeBase[2], planeBase[0]);
	return planeBase;
}

// contrast function from soft particles (smooth gradient between 0 and 1)
float getSoftParticleContrast(float x)
{
    const float c = 2.0; // contrast in [1, 5]
    x = saturate(x);
    if(x <= 0.5) return 0.5 * pow(2.0 * x, c);
    return 1.0 - 0.5 * pow(2.0 * (1.0 - x), c);
}

// Returns vertex data for impostors and particle billboards
VertexData getBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 rayDirection, float rayTCurrent, float rayTMin, float rayTMax)
{
    VertexData v;
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 C = bbox.center();

    // billboard plane base
    const bool yAlign = BILLBOARD_TYPE == BILLBOARD_TYPE_IMPOSTOR;
    const float3x3 planeBase = GetBillboardTangentSpace(normalize(rayOrigin - C), yAlign);

    // fill out important vertex data
    v.posW = rayOrigin + rayDirection * rayTCurrent;
    v.normalW = planeBase[2];
    v.texC.x = dot(C - v.posW, planeBase[0]) / dim.x + 0.5;
    v.texC.y = dot(C - v.posW, planeBase[1]) / dim.y + 0.5;
    v.billboardStrength = 1.0;
    if(BILLBOARD_TYPE == BILLBOARD_TYPE_PARTICLE && USE_SOFT_PARTICLES)
    {
        float blendScale = 4.0 / dim.x;
        float outFade = getSoftParticleContrast((rayTMax - rayTCurrent) * blendScale);
        float inFade = getSoftParticleContrast((rayTCurrent - rayTMin) * blendScale);
        //inFade = 1.0;
        v.billboardStrength = min(outFade, inFade);
    }

    // other vertex data
    v.tangentW.xyz = planeBase[0];
    v.tangentW.w = 1.0;
    v.faceNormalW = planeBase[2];
    v.coneTexLODValue = 0.0;
    v.billboardDepth = (dim.x * 0.5 - length(C - v.posW)) * 2.0; // for soft shadows

    return v;
}

float2 CalcUVFromNormal(float3 n, uint aabbOffset)
{
    float2 uv;
    uv.x = (n.x == 0.0 ? 0.25 * sign(n.z) : atan2(n.z, n.x) / (2.0 * M_PI)) + 0.5;
    uv.y = acos(n.y) / M_PI;
    uv = clamp(uv, 0.0, 1.0);
    uv.x += aabbOffset * 0.172; // give each billboard a random texture rotation
    return uv;
}

// Returns vertex data for spherical billboards
VertexData getSphericalBillboardVertexData(uint aabbOffset, float3 rayOrigin, float3 rayDirection, float rayTCurrent, float rayTMin, float rayTMax)
{
    VertexData v;

    // reconstruct billboard plane
    const AABB bbox = gScene.proceduralPrimitiveAABBs[aabbOffset];
    const float2 dim = bbox.extent().xy;
    const float3 C = bbox.center();

    // determine depth
    const float r = 0.5 * dim.x;
    const float t = dot(rayDirection, C - rayOrigin); // rayT to closest point inside billboard
    const float l = length(rayOrigin + t * rayDirection - C); // smallest distance
    const float w = sqrt(r * r - l * l);
    const float ds = min(rayTMax, t + w) - rayTCurrent;

    // fill out vertex data
    v.billboardDepth = ds;
    v.billboardStrength = (1.0 - (l / r)) * (ds / (2.0 * r));
    v.posW = rayOrigin + rayDirection * rayTCurrent; // closest entry point
    v.normalW = normalize(rayOrigin + rayDirection * (t - w) - C); // direction to unclipped entry point (for consistent texture coordinates when traversing)
    v.texC = CalcUVFromNormal(v.normalW, aabbOffset);

    // other vertex data
    v.tangentW = 0.0; // does not really matter
    v.faceNormalW = v.normalW;
    v.coneTexLODValue = 0.0;

    return v;
}

// simple shading function that iterates though all lights and traces shadow rays for the primary light source
// returns rgb color and transmittance (in alpha channel)
float4 shade(ShadingData sd, bool useBillboardDepth = false, float billboardDepth = 0.0, float billboardOffset = 0.0)
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
            // obtain #BILLBOARD_SHADOW_SAMPLES shadow ray samples
            // Note: only for visualization purposes, please use fourier opacity maps or something similar in a real-time context
            float shadowSum = 0.0;
            const uint nSamples = BILLBOARD_SHADOW_SAMPLES;
            float scale = 1.0 / (nSamples - 1);
            for(uint j = 0; j < nSamples; ++j)
            {
                shadowSum += TraceShadowRay(sd.posW - sd.V * (billboardDepth * j * scale - billboardOffset), l);
            }
            curColor += color * (shadowSum / nSamples);
        }
        else
        {
            float shadowFac = 1.0;
            if(i == 0) shadowFac = TraceShadowRay(sd.posW, l); // single shadow ray sample for the primary light
            curColor += evalMaterial(sd, l, shadowFac).color;
        }
    }
    curColor += sd.diffuse * AMBIENT_LIGHT;
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
