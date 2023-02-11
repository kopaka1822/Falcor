/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
import Scene.Camera.Camera;
import SSAOData;
import Scene.Intersection;
import Scene.Shading;
import Scene.Raytracing;

//#include "Scene/Material/MaterialDefines.slangh"

#define ml_float float

// ml network
static const ml_float kernel0[4 * 5 * 5] =
{
    -2.1529474, -0.22074331, 2.6480758, -1.2584015, -0.28819174, 0.8789276, 0.38466936, 0.022578537, -0.004050717, 0.30297506, 0.035171174, 0.17631598, -0.05319165, -0.50373733, 0.14312294, -0.5837663, -0.29822022, 0.078120686, 1.0556755, -0.20848484, -0.12656504, -2.2454267, 0.059932016, 0.7978356, -2.2481554,
    3.8233576, -0.17893381, -1.9495546, 0.1253949, 0.004297561, -1.4594983, 3.7157764, 0.3517659, -1.800106, -0.7068048, 0.7926677, 0.025229696, 0.30072054, -0.92199486, 0.7403585, 0.15179537, 0.05578087, 0.28190252, -0.57500947, -0.38420802, 0.024217246, 0.08247212, 0.43449768, 0.009338685, -3.373533,
    0.19093709, -0.1358205, 0.30708405, -0.20670798, 0.14137848, -0.11035264, 3.8398652, -0.28542364, 4.095936, 2.6632288, -2.260531, -2.6075346, 2.9989827, -5.7030363, -1.1872946, 2.2835941, -0.11705348, 0.65230775, -0.121665195, 0.65373856, 0.011013254, -0.024134964, -0.019500528, -0.035975154, -0.026103381,
    0.16531761, 0.03233645, -0.0053912573, 0.06848293, -0.30268657, 0.99974805, 3.6845982, -0.31549686, 4.3015847, -1.7497748, -3.0175784, -1.6640717, 0.113875665, -1.0685353, 2.2669992, 1.9312453, 0.29030335, 4.2702923, 0.8817285, -0.7589611, 0.023578757, -0.008551796, -0.02089713, -0.03527413, -0.01471888
    
};
#define KERNEL0(step, row, col) kernel0[step * 5 * 5 + row * 5 + col]
static const ml_float bias0[4 * 5] =
{
    1.1631331, -0.06082821, -0.4667503, 0.5140993, 0.4035607,
    0.8670715, -0.46901023, 0.93886226, 1.6690806, 0.8498954,
    -0.15174386, 0.70832163, -1.1604751, 0.8655082, 1.130975,
    -0.015608429, 0.8526345, 0.60784566, 0.8707579, 0.9019055
};
#define BIAS0(step, col) bias0[step * 5 + col]
static const ml_float kernel1[4 * 5 * 5] =
{
    0.078057036, 0.2107422, -3.5479248, 0.10689201, 0.011195385, 2.4116623, -0.560882, 1.6831114, 0.6987789, 0.02600676, 0.0841675, -1.1471808, 0.2369262, 1.2171623, -0.9384125, -0.1052978, 0.095147796, -1.0771282, 0.11407696, -0.07878899, -2.9747589, 0.5397344, -1.73844, -0.47401133, -0.24260028,
    1.4168367, 0.36601797, -2.6717858, -2.8442566, -0.3486352, 0.13301066, -1.7021703, 1.3058927, 0.7409957, 0.12507373, -0.9577057, 0.12580922, -0.2573012, -0.048141334, -1.1746746, 0.43338102, -0.01224304, -18.936464, -0.08552319, -5.2429233, 0.03609894, -0.04451053, 0.08494705, -0.04828412, -3.1804955,
    1.6016544, 0.21521845, -1.7245837, 1.598831, 0.035999596, -2.873509, -1.4431279, 0.15967938, 1.5225425, -0.013412778, 0.40879735, -1.6779631, 0.9099941, 0.39787367, -1.0955284, 1.884562, 0.601181, -57.40482, -2.3850253, -0.14324912, -0.13524824, 0.9694758, -1.4737895, -0.34189898, -0.26169083,
     -0.03204945, 0.24805048, -14.072321, -0.7950415, -0.02584654, 2.2421587, -0.3348794, 1.7593398, 1.1568526, 0.0053971587, 0.6760102, -1.7839174, 0.7423849, -0.7128633, -0.9869542, -2.1151628, 0.82067335, -1.7320192, 1.1195401, -0.14110619, -3.1076295, 0.4169148, -0.34305027, 0.5337315, -0.31124455
};
#define KERNEL1(step, row, col) kernel1[step * 5 * 5 + row * 5 + col]
static const ml_float bias1[4 * 5] =
{
    1.0084023, 2.7913795, 1.8205782, -0.034690764, -0.21305476,
    -1.3357264, 4.302976, -1.2789307, 3.1032166, 2.9681976,
    -0.42658582, 3.107007, 2.3727345, -0.6127495, -0.24493241,
    2.4171095, 5.4000063, 1.2496324, 1.141482, -0.3966589
};
#define BIAS1(step, col) bias1[step * 5 + col]
static const ml_float kernel2[4 * 5 * 1] =
{
    1.1561285, -1.0737535, 0.99911916, 0.25504047, 0.11513423,
    0.27637112, -0.90580726, 0.21328549, 0.45614877, 1.4903924,
    -0.636092, -0.8628456, 0.649735, 0.69571066, 0.12947835,
    0.7513209, -0.93860745, 0.8664237, 0.31559163, 0.12267117
};
#define KERNEL2(step, row) kernel2[step * 5 * 1 + row]
static const ml_float bias2[4 * 1] =
{
    -2.0550632,
    -1.696745,
    -0.9425369,
    - 2.4453201
};
#define BIAS2(step) bias2[step]

#define NUM_STEPS 4
#define NUM_DIRECTIONS 8

cbuffer StaticCB
{
    SSAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
    uint guardBand;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

// inputs
Texture2D<float> gDepthTex;
Texture2D<float> gNoiseTex;
Texture2D gNormalTex;

Texture2D<uint> gStencil;
RWTexture2D<float> gOutput;

struct RayData
{
    float tLastFaceOutside;
    float tFirstFaceInside;
    uint occlusionStack;
    float sphereStart;
};

float2 getSnappedUV(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelCoord = floor(uv * float2(width, height));
    return float2((pixelCoord.x + 0.5f) / width, (pixelCoord.y + 0.5f) / height);
}

bool isSamePixel(float2 uv1, float2 uv2)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    float2 pixelSize = float2(rcp(width), rcp(height));
    return all(abs(uv1 - uv2) < pixelSize);

}

// uv: uv coordinates [0, 1]
// viewDepth: linear depth in view space (positive z)
// return: view space position (negative z)
float3 UVToViewSpace(float2 uv, float viewDepth)
{
    float2 ndc = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0; // normalized device coordinates [-1, 1]
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    return float3(ndc * viewDepth * imageScale, -viewDepth);
}

// posV: view space position (negative z)
// return: texture uv [0, 1]
float2 ViewSpaceToUV(float3 posV)
{
    const float2 imageScale = 0.5 * float2(gCamera.data.frameWidth / gCamera.data.focalLength, gCamera.data.frameHeight / gCamera.data.focalLength);
    float2 ndc = posV.xy / (imageScale * posV.z);
    return ndc * float2(-0.5, 0.5) + 0.5; // since posV.z is negative, the sign order is inversed
}

int2 UVToPixel(float2 uv)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    return int2(floor(uv * float2(width, height)));
}

float2 PixelToUV(int2 pixel)
{
    float width, height;
    gDepthTex.GetDimensions(width, height);
    return float2((pixel.x + 0.5f) / width, (pixel.y + 0.5f) / height);
}

float makeNonZero(float value, float epsilon)
{
    float absValue = max(abs(value), epsilon);
    return value >= 0 ? absValue : -absValue;
}

// like sign, but returns 1 for v=0
float signBinary(float v)
{
    if (v < 0.0)
        return -1.0;
    return 1.0;
}

float2 getRaySphereIntersections(float3 rayStart, /*normalized*/ float3 rayDir, float3 sphereCenter, float radius)
{
    // distance to the closest point near the sphere center (if d > radius, there are no intersections)
    float d = dot(rayDir, sphereCenter - rayStart);
    float delta = d * d + radius * radius - dot(sphereCenter - rayStart, sphereCenter - rayStart);
    delta = max(delta, 0.0); // this is important to have a valid square root (negative values would mean no intersection, but we just assume that this will only be the case for precision reasons)
    return float2(d - delta, d + delta);
}

struct PSOut
{
    float ao : SV_Target0;
    uint rayMask : SV_Target1;
};

[shader("miss")]
void miss(inout RayData rayData)
{
}

[shader("anyhit")]
void anyHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // extract hit properties
    float t = RayTCurrent();

    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    TriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    const uint materialID = gScene.getMaterialID(hit.instanceID);
    const MaterialHeader header = gScene.materials.materialData[materialID].header;

    bool isAlphaTested = header.getAlphaMode() == AlphaMode::Mask;
    frontFace = frontFace || isAlphaTested || header.isDoubleSided();
                    
    if (t < rayData.sphereStart) // in front of the sphere
    {
        // intersection in front of the trusted area
        if (isAlphaTested || header.isDoubleSided()) // ignore alpha tested materials (too thin for occlusion at this distance)
        {
            IgnoreHit();
            return;
        }
        rayData.occlusionStack += frontFace ? 1 : -1; // update occlusion stack
        rayData.tLastFaceOutside = max(rayData.tLastFaceOutside, t);
    }
    else // inside of the trusted area
    {
                // needs alpha testing?
        if (isAlphaTested)
        {
            const VertexData v = gScene.getVertexData(hit);
            if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
            {
                IgnoreHit();
                return;
            }
        }

        rayData.tFirstFaceInside = min(rayData.tFirstFaceInside, t);
        return; // since we save the min, we can commit TMax here
    }
    
    IgnoreHit(); // continue traversal
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // is skipped
}

[shader("raygeneration")]
void rayGen()
{
    uint2 svPos = DispatchRaysIndex().xy + uint2(guardBand);
    float2 texC = PixelToUV(svPos);
    
    uint mask = gStencil[svPos];
    if (mask == 0u)
    {
        //gOutput[svPos] = 0.0;
        return;
    }
    
    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);
    
    // view space position of current pixel
    float3 posV = UVToViewSpace(texC, linearDepth);
    float3 posW = mul(float4(posV, 1.0), invViewMat).xyz;
    
    // view space normal of current pixel
    float3 normalW = normalize(gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz);
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (dot(posV, normalV) > 0.0) // front face normals
    {
        normalW = -normalW;
        normalV = -normalV;
    }

    // obtain current pixels XY coordinate          
    float width, height;
    gDepthTex.GetDimensions(width, height);
    const int2 XY = UVToPixel(texC);
    
    const float posVLength = length(posV);

    // Calculate tangent space (use random direction for tangent orientation)
    float randJitter = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0); 
    float randRotation = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0) * 2.0 * 3.141; // range [0, 2pi]
    float2 randDir = float2(sin(randRotation), cos(randRotation));
    randDir = normalize(randDir); // should be normalized by default, but precision is lost in texture format
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 V = -posV / posVLength;
    float3 bitangent = normalize(cross(V, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, V);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, V));

    //float visibility = gOutput[svPos];
    float visibility = 0.0;
    
    uint index = 0;
    while(mask != 0u)
    {
        while ((mask & 1u) == 0)
        {
            index++;
            mask = mask >> 1;
        }

        uint idir = index / 8;
        int step = (index % 8) - 4;

        float angle = (2.0 * 3.141 * idir) / NUM_DIRECTIONS;
        float2 direction = float2(sin(angle), cos(angle));
            
        // calc 's' sampling pos
        float jitterSign = step < 0 ? -1 : 1;
        float2 s = direction * (step + 0.5 + jitterSign * randJitter) / (NUM_STEPS + 1);
        s *= gData.radius; // multiply 2D position with sample radius

        // calculate view position of sample and project to uv coordinates
        float3 S = tangent * s.x + bitangent * s.y; // sample position relative to posV
        float3 initialSamplePosV = posV + S; // absolute sample position in view space
        float initialSampleT = length(initialSamplePosV); // serves as reference for ray/raster depths
        float2 sphereStartEnd = getRaySphereIntersections(0.0, normalize(initialSamplePosV), posV, gData.radius);
            
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
            
        samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center

        float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space

        RayDesc ray;
        ray.Origin = gCamera.data.posW;
        ray.Direction = mul(sampleDirV, float3x3(invViewMat));
        ray.TMin = 0.0;
        ray.TMax = gCamera.data.farZ;
        
        RayData rayData;
        rayData.tLastFaceOutside = ray.TMin;
        rayData.tFirstFaceInside = ray.TMax;
        rayData.occlusionStack = 0; // > 1 means occluded (counted for outside front faces)
        rayData.sphereStart = sphereStartEnd.x;

        TraceRay(
            gScene.rtAccel,
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
            ray, rayData
        );
       

        float tFinalHit = rayData.tLastFaceOutside;
        if (rayData.occlusionStack <= 0)
            tFinalHit = rayData.tFirstFaceInside;

        //gRayDepth[uint3(XY, sampleIndex)] = (initialSampleT - tFinalHit) / gData.radius;
        float3 hitV = mul(float4(ray.Origin + ray.Direction * tFinalHit, 1.0), gCamera.data.viewMat).xyz;
        float3 P = normalize(hitV - posV);
        float alpha = 1 - max(dot(normalV, P), 0.0);
        float curVisibility = alpha;
                
        if (tFinalHit > sphereStartEnd.y) // intersection behind the sphere (assume not occluded)
        {
            curVisibility = 1.0;
        }
        visibility += curVisibility;

        // prepare for next iteration
        index++;
        mask = mask >> 1;
    }
    
    float AO = visibility / float(NUM_DIRECTIONS * NUM_STEPS);
    AO += gOutput[svPos];
    
    //AO = saturate(2 * AO - 1);

    // do artistic modifications
    AO = pow(AO, gData.exponent);
    gOutput[svPos] = AO;
}
