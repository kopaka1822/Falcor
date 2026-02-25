import Scene.Raster;
import Utils.Math.MathHelpers;

Texture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float4> gPosW;
RWTexture2D<float4> gPrevPosW;
RWTexture2D<float4> gNormalW;
RWTexture2D<float4> gNormalV;
RWTexture2D<float4> gFaceNormalW;
RWTexture2D<float> gRasterZ;
RWTexture2D<float> gLinearZ;
RWTexture2D<float> gRoughness;
RWTexture2D<float4> gDiffuse;
RWTexture2D<float4> gSpecular;
RWTexture2D<float4> gNormalWRoughnessMaterialID;

float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV) 
{ 
    NoV = abs(NoV); // [Ray Tracing Gems, Chapter 32] 
    float4 X; X.x = 1.f; 
    X.y = NoV; 
    X.z = NoV * NoV; 
    X.w = NoV * X.z; 
    float4 Y; 
    Y.x = 1.f; 
    Y.y = alpha; 
    Y.z = alpha * alpha; 
    Y.w = alpha * Y.z; 
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f); 
    float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f); 
    float2x2 M3 = float2x2(0.0365463f, 3.32707, 9.0632f, -9.04756); 
    float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f); 
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw)); 
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw)); // This is a hack for specular reflectance of 0 
    bias *= saturate(SpecularColor.g * 50); 
    return mad(SpecularColor, max(0, scale), max(0, bias)); 
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
    const uint2 pixel = id.xy;
    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (hit.isValid() && hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        let v = gScene.getVertexData(triangleHit);

        gPosW[pixel] = float4(v.posW, 1.0);
        gPrevPosW[pixel] = float4(v.prevPosW, 1.0);
        gNormalW[pixel] = float4(v.normalW, 0.0);
        gNormalV[pixel] = mul(gScene.camera.data.viewMat, float4(v.normalW, 0.0));
        gFaceNormalW[pixel] = float4(v.faceNormalW, 0.0);
        float4 posW = mul(gScene.camera.data.viewProjMat, float4(v.posW, 1.0));
        //float4 posW = mul(gScene.camera.data.viewProjMatNoJitter, float4(v.posW, 1.0));
        gRasterZ[pixel] = posW.z / posW.w;
        gLinearZ[pixel] = posW.w;
        //gLinearZ[pixel] = (posW.w - gScene.camera.data.nearZ) / (gScene.camera.data.farZ - gScene.camera.data.nearZ);

        float3 viewDir = normalize(gScene.camera.data.posW - v.posW);
        let materialID = gScene.getMaterialID(triangleHit.instanceID);
        let lod = ExplicitLodTextureSampler(0.0);
        ShadingData sd = gScene.materials.prepareShadingData(v, materialID, viewDir, lod);
        // Create material instance at shading point.
        let mi = gScene.materials.getMaterialInstance(sd, lod, (uint) MaterialInstanceHints::AdjustShadingNormal);
        let bsdf = mi.getProperties(sd);

        gRoughness[pixel] = saturate(bsdf.roughness);
        gDiffuse[pixel] = float4(bsdf.diffuseReflectionAlbedo + bsdf.emission, 1.0);
        //gSpecular[pixel] = float4(bsdf.specularReflectionAlbedo, 1.0);
        gSpecular[pixel] = float4(EnvBRDFApprox2(bsdf.specularReflectionAlbedo, bsdf.roughness * bsdf.roughness, abs(dot(v.normalW, viewDir))), 1.0);

        float3 normal = bsdf.guideNormal;
        float2 octNormal = ndir_to_oct_unorm(normal);
        float roughness = bsdf.roughness;
        gNormalWRoughnessMaterialID[pixel] = float4(octNormal, roughness, 0.0);
    }
    else // invalid hit => default values
    {
        if (hit.isValid() && hit.getType() == HitType::RayMiss)
        {
            gPosW[pixel] = float4(RayMissHit(hit.getData()).posW, 1.0);
            gPrevPosW[pixel] = float4(RayMissHit(hit.getData()).posW, 1.0);
        }
        else
        {
            gPosW[pixel] = 0.0;
            gPrevPosW[pixel] = 0.0;
        }
            
        gNormalW[pixel] = 0.0;
        gNormalV[pixel] = 0.0;
        gFaceNormalW[pixel] = 0.0;
        gRasterZ[pixel] = 1.0;
        gLinearZ[pixel] = gScene.camera.data.farZ;
        //gLinearZ[pixel] = 1.0;
        gRoughness[pixel] = 0.0;
        gDiffuse[pixel] = 0.0;
        gSpecular[pixel] = 0.0;

        const float2 octNormal = ndir_to_oct_unorm(float3(0.2215, 0.2215, 0.2215)); // UE4 default value at miss
        gNormalWRoughnessMaterialID[pixel] = float4(octNormal, 0.0, 0.0);
    }
}
