

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
