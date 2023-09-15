import Scene.Raster;

struct SDOut
{
    float4 posH : SV_Position;
    float3 posW : WORLD;
    nointerpolation uint materialID : MATERIAL_ID;
    float2 texC : TEXCOORD;
    float viewDepth : VIEW_DEPTH;
};

SDOut vsMain(VSIn vIn)
{
    //VSOut vOut;
    SDOut vOut;
    const GeometryInstanceID instanceID = { vIn.instanceID };

    float4x4 worldMat = gScene.getWorldMatrix(instanceID);
    float3 posW = mul(worldMat, float4(vIn.pos, 1.f)).xyz;
    vOut.posW = posW;
    vOut.viewDepth = length(mul(gScene.camera.data.viewMat, float4(posW, 1.0)).xyz);
    vOut.posH = mul(gScene.camera.getViewProj(), float4(posW, 1.f));

    //vOut.instanceID = instanceID;
    vOut.materialID = gScene.getMaterialID(instanceID);

    vOut.texC = vIn.texC;
    
    /*vOut.normalW = mul(gScene.getInverseTransposeWorldMatrix(instanceID), vIn.unpack().normal);
    float4 tangent = vIn.unpack().tangent;
    vOut.tangentW = float4(mul((float3x3)gScene.getWorldMatrix(instanceID), tangent.xyz), tangent.w);

    // Compute the vertex position in the previous frame.
    float3 prevPos = vIn.pos;
    GeometryInstanceData instance = gScene.getGeometryInstance(instanceID);
    if (instance.isDynamic())
    {
        uint prevVertexIndex = gScene.meshes[instance.geometryID].prevVbOffset + vIn.vertexID;
        prevPos = gScene.prevVertices[prevVertexIndex].position;
    }
    float3 prevPosW = mul(gScene.getPrevWorldMatrix(instanceID), float4(prevPos, 1.f)).xyz;
    vOut.prevPosH = mul(gScene.camera.data.prevViewProjMatNoJitter, float4(prevPosW, 1.f));
    */
    return vOut;
}

//static const uint stratifiedIndices[] = { 0, 1, 17, 137, 697, 2517, 6885, 14893, 26333, 39203, 50643, 58651, 63019, 64839, 65399, 65519, 65535, };
//static const uint stratifiedIndices[] = { 0, 1, 9, 37, 93, 163, 219, 247, 255 };
StructuredBuffer<uint> stratifiedIndices;
StructuredBuffer<uint> stratifiedLookUpTable;
Texture2D<float> depthBuffer;
Texture2D<uint> rayMin; // additional texture for a more precise depth range
Texture2D<uint> rayMax; // contains float values: use asfloat()

RWTexture2D<uint> counter; // for reservoir sampling

//#define ALPHA 0.375

cbuffer CameraCB
{
    float zNear;
    float zFar;
};

// Hash function from "Improved Alpha Testing Using Hashed Sampling"
float hash(float2 v)
{
    return frac(1.0e4 * sin(17.0 * v.x + 0.1 * v.y) * (0.1 + abs(sin(13.0 * v.y + v.x))));
}

// 3D hash function from "Improved Alpha Testing Using Hashed Sampling"
float hash3D(float3 v)
{
    return hash(float2(hash(v.xy), v.z));
}

// 4D hash function based on the 3D hash function
float hash4D(float4 v)
{
    return hash(float2(hash3D(v.xyz), v.w));
}

struct PsOut
{
    uint SampleMask[1] : SV_Coverage; // let this be an array to avoid compiler bugs
#ifdef LINEARIZE
    float depth : SV_Depth;
#endif
};

// [earlydepthstencil] <- do not force this, it makes it slower
PsOut psMain(SDOut vsOut, uint triangleIndex : SV_PrimitiveID)
{
    if (alphaTestFast(vsOut.texC, vsOut.materialID, ImplicitLodTextureSampler()))
        discard;
    //if (alphaTest(vsOut, triangleIndex, ImplicitLodTextureSampler()))
    //    discard;
    PsOut output;

    // assume that a depth-map of the first layer is available => discard fragments with the same depth
    uint2 svPos = uint2(vsOut.posH.xy);
    float firstDepth = depthBuffer[svPos];
    if (vsOut.posH.z <= firstDepth)
        discard;

    //float dist = distance(vsOut.posW.xyz, gScene.camera.getPosition());
    float dist = vsOut.posH.w;
    //float dist = vsOut.viewDepth;
    
    // use ray min from texture (can be bigger than depth buffer)
    uint iRayMin = rayMin[svPos];
    if (iRayMin != 0u && dist <= asfloat(iRayMin))
        discard;
    
    uint iRayMax = rayMax[svPos];
    if (iRayMax != 0u && dist >= asfloat(iRayMax))
        discard; // save bandwidth (cannot depth cull because of linearize...)
    
     #if NON_STOCHASTIC_FIRST_LAYER // Potentially, you could create the regular depth map in this same pass by never discarding the first sample
     	if (gl_SampleID == 0) { return; } 
     #endif

    float rng = hash4D(float4(vsOut.posW.xyz, 1.438943289));
    
    #if RESERVOIR_SAMPLING

    uint slot = 0;
    InterlockedAdd(counter[svPos], 1u, slot);
    if(slot >= NUM_SAMPLES)
        slot = uint(rng * slot);
    if(slot >= NUM_SAMPLES) // rejected
        discard;

    output.SampleMask[0] = 1u << slot;
    
    #else
    
    int R = int(floor(ALPHA * NUM_SAMPLES + rng));

    if (R >= NUM_SAMPLES)
    {
        output.SampleMask[0] = 0xffff; // This code assumes maximum 32 samples, otherwise further gl_SampleMask values need to be set
    }
    else if (R != 0)
    {
        float rng2 = hash3D(vsOut.posW.zyx);
        int index = int(lerp(stratifiedIndices[R], stratifiedIndices[R + 1], rng2));
        uint mask = stratifiedLookUpTable[index];
        output.SampleMask[0] = int(mask);
    }
    else
        discard;

    #endif
    
    #ifdef LINEARIZE
    output.depth = (vsOut.posH.w - zNear) / (zFar - zNear); // linear depth normalizedto [0,1] because a depth target cannot store higher values
    #endif
    
    return output;
}
