import Scene.Camera.Camera;
import VAOData;
import Scene.Raytracing;

#include "Scene/Material/MaterialDefines.slangh"

//#define TANGENT_SPACE_VIEW_SPACE 0
//#define TANGENT_SPACE_CAMERA_ORIENTED 1

// shoots only a small ray inside the sphere between [+sphereHeight, -sphereHeight]
#define RAY_MODE_SHORT_RAYS_APPROX 0
// shoots s small ray from the center, which is known to be unoccluded, to +sphereHeight, then continues with RAY_MODE_SHORT_RAYS_APPROX
// (this way, additional blocckers can be detected)
#define RAY_MODE_SHORT_RAYS_REF 1
// traces a single ray from the camera position to -sphereHeight
#define RAY_MODE_LONG_RAYS_REF 2
// traces a single ray from the camera position to -sphereHeight but stops at the first hit (should be very similar to rasterizer)
#define RAY_MODE_LONG_RAYS_FIRST_HIT 3

// removed and defaulted to tangent space
// this samples the hemisphere with the sphere center at the sampling position
#define SAMPLE_MODE_HEMISPHERE 0
// this samples a complete sphere, where the center is offsettet by normal*radius (VAO++)
#define SAMPLE_MODE_SPHERE 1
#define SAMPLE_MODE SAMPLE_MODE_HEMISPHERE

#define PREVENT_DARK_HALOS 1
#define FRONT_FACE_NORMALS true
// full radius of the halo
#define HALO_RADIUS (gData.radius * 3.0)
// area where the halo effect remains constant at 0.0
#if PREVENT_DARK_HALOS
#define CONST_RADIUS gData.radius
#else
#define CONST_RADIUS 0.0
#endif
#define RAY_MODE RAY_MODE_LONG_RAYS_REF

// indicates if the depth buffer should be used
#define USE_DEPTH_BUFFER

//#define ENABLED(value) (value != 0)

cbuffer StaticCB
{
    VAOData gData;
}

cbuffer PerFrameCB
{
    float4x4 invViewMat;
    Camera gCamera;
    uint frameIndex;
    uint guardBand;
}

SamplerState gNoiseSampler;
SamplerState gTextureSampler;

Texture2D<float> gDepthTex;

Texture2D gNormalTex;
Texture2D gNoiseTex;

static int gRaysTraced = 0;
static int gInvalid = 0;


// clamps uvend to screen space [0, 1]
// uvstart: uv coordinate that is in screen space
// uvend: target uv coordinate that might be outside of the screen space
// return: linear combination t*uvstart + (1-t)*uvend, that is as close to uvend as possible and is in the range of [0, 1]
float2 getScreenClampedUV(float2 uvstart, float2 uvend)
{
    return saturate(uvend); // this actually does not make much of a difference but costs a little bit more...
    /*float2 satuv = saturate(uvend);
    if (all(satuv == uvend))
        return uvend;

    // clip x
    float dist = abs(uvend.x - uvstart.x);
    if(dist > 0.0)
    {
        float t = abs(uvend.x - satuv.x) / dist;
        uvend.x = satuv.x;
        uvend.y = t * uvstart.y + (1.0 - t) * uvend.y;
    }

    // clip y
    satuv = saturate(uvend);
    dist = abs(uvend.y - uvstart.y);
    if(dist > 0.0)
    {
        float t = abs(uvend.y - satuv.y) / dist;
        uvend.y = satuv.y;
        uvend.x = t * uvstart.x + (1.0 - t) * uvend.x;
    }
    
    return uvend;*/
}

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

float4 getPosition(float2 uv)
{
    float4 pos;
    pos.x = uv.x * 2.0f - 1.0f;
    pos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    pos.z = gDepthTex.SampleLevel(gTextureSampler, uv, 0).r;
    pos.w = 1.0f;

    float4 posW = mul(pos, gCamera.data.invViewProj);
    posW /= posW.w;

    return posW;
}

// "Explodes" an integer, i.e. inserts a 0 between each bit. 
//      For example, 0b11111111 -> 0b1010101010101010
uint IntegerExplode(uint x)
{
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

// helper math for zcurve
uint ZCurveToLinearIndex(uint2 xy)
{
    return IntegerExplode(xy[0]) | (IntegerExplode(xy[1]) << 1);
}

uint JenkinsHash(uint a)
{
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

float2 Rotate2D(float2 vec, float theta)
{
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    return float2(
        vec.x * cosTheta - vec.y * sinTheta,
        vec.x * sinTheta + vec.y * cosTheta
    );
}

float2 Rotate90(float2 vec) // CCW
{
    return float2(-vec.y, vec.x);
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

float makeNonZero(float value, float epsilon)
{
    float absValue = max(abs(value), epsilon);
    return value >= 0 ? absValue : -absValue;
}

// get rid of shadowing around edges
// introduce a linear falloff function that starts with 0.0 when the sample depth intersects the front sphere exactly,
// and falls of to 1.0 when it gets further away from the sphere but closer to the camera
float calcHaloVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    if (!PREVENT_DARK_HALOS)
        return 0.0;
    
    return saturate((objectSpaceZ - sphereStart - CONST_RADIUS) / HALO_RADIUS)
        * (sphereStart - sphereEnd); // this adjust the visibility to the sampling (hemi-)sphere
}

float calcSphereVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    float sampleRange = max(sphereStart - max(sphereEnd, objectSpaceZ), 0.0);
    return sampleRange;
}

float calcVisibility(float objectSpaceZ, float sphereStart, float sphereEnd)
{
    return calcSphereVisibility(objectSpaceZ, sphereStart, sphereEnd)
         + calcHaloVisibility(objectSpaceZ, sphereStart, sphereEnd);
}

float calcObjectSpaceZ(float3 posV, float3 normal, float2 uv, Texture2D<float> depthTex)
{
    float linearSampleDepth = depthTex.SampleLevel(gTextureSampler, uv, 0);
    float3 samplePosV = UVToViewSpace(uv, linearSampleDepth);
            // the object is the sphere centered at posV with the above tangent space (positive values are closer to the camera)
    float objectSpaceZ = dot(samplePosV - posV, normal);
    return objectSpaceZ;
}

///////// start ray shaders /////////////

struct RayData // cannot be compressed to half floats => no diff in rendering time + insufficient visual quality
{
    float tLastFrontFaceHalo; // ray min
    float tFirstFrontFaceInside; // ray max
    float tConstRadiusStart;
    float tSphereStart;
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
    if (t < rayData.tLastFrontFaceHalo)
        IgnoreHit(); // we can skip this since it would not contribute anyways (unfortunately there is no option to set the ray.min afterwards)

    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    TriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    const uint materialID = gScene.getMaterialID(hit.instanceID);
    const MaterialHeader header = gScene.materials.materialData[materialID].header;

    bool isAlphaTested = header.getAlphaMode() == AlphaMode::Mask;

    // needs alpha testing?
    if (isAlphaTested)
    {
        const VertexData v = gScene.getVertexData(hit);
        if (gScene.materials.alphaTest(v, materialID, 0.0)) // TODO correct lod?   
            IgnoreHit(); // alpha test failed => ignore this triangle
    }

    frontFace = frontFace || isAlphaTested || header.isDoubleSided();
    if (!frontFace)
        IgnoreHit(); // this is just for rasterizer compability

    if (t <= rayData.tSphereStart)
    {
        rayData.tLastFrontFaceHalo = max(rayData.tLastFrontFaceHalo, t);
        if (t >= rayData.tConstRadiusStart)
            AcceptHitAndEndSearch(); // we can stop the query, because this will set the visibility to zero
    }
    else // inside sphere
    {
        rayData.tFirstFrontFaceInside = min(rayData.tFirstFrontFaceInside, t);
        return; // since we save the min, we can commit TMax here
    }
    IgnoreHit(); // continue traversal
}

[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // is skipped
}

RWTexture2D<float> gOutput;

[shader("raygeneration")]
void rayGen()
{
    float2 resolution;
    gDepthTex.GetDimensions(resolution.x, resolution.y);
    float2 invResolution = float2(1.0) / resolution;

    uint2 svPos = DispatchRaysIndex().xy + uint2(guardBand);
    float2 texC = (float2(svPos) + 0.5) * invResolution;

    float linearDepth = gDepthTex.SampleLevel(gTextureSampler, texC, 0);
    if (linearDepth >= gCamera.data.farZ * 0.99)
    {
        gOutput[svPos] = 1.0;
        return;
    }
        

    // view space position of current pixel
    float3 posV = UVToViewSpace(texC, linearDepth);

    // view space normal of current pixel
    float3 normalW = gNormalTex.SampleLevel(gTextureSampler, texC, 0).xyz;
    float3 normalV = mul(normalW, float3x3(gCamera.data.viewMat));
    if (FRONT_FACE_NORMALS && dot(posV, normalV) > 0.0)
        normalV = -normalV;

    // move sampling sphere
    if (SAMPLE_MODE == SAMPLE_MODE_SPHERE)
    {
        posV += normalize(normalV) * gData.radius;
    }
    const float posVLength = length(posV);

    // Calculate tangent space (use random direction for tangent orientation)
    float2 randDir = gNoiseTex.SampleLevel(gNoiseSampler, texC * gData.noiseScale, 0).xy;
    randDir = normalize(randDir); // should be normalized by default, but precision is lost in texture format
    //randDir = float2(1.0f, 0.0f);
    
    // determine tangent space
    float3 normal = -posV / posVLength;
    float3 bitangent = normalize(cross(normal, float3(randDir, 0.0f)));
    float3 tangent = cross(bitangent, normal);
    

    // transfer view space normal to normal in object coordinates of the sampling sphere
    float3 normalO = float3(dot(normalV, tangent), dot(normalV, bitangent), dot(normalV, normal));
    
    float visibility = 0.0f;
    //uint zCurveIndex = ZCurveToLinearIndex(uint2(svPos.xy));
    //uint i = (zCurveIndex + (frameIndex)) % gData.kernelSize; // JenkinsHash
    for (uint i = 0; i < gData.kernelSize; i++)
    {
        // obtain sample position on disc around view space position
        float4 rand = gData.sampleKernel[i]; // xy = random location on unit disc, zw = uniform in 0,1
        rand.xy *= gData.radius; // multiply 2D position with sample radius
        
        // height of the sphere at the requested sample position (not at the actual sampling position)
        const float sphereHeight = sqrt(gData.radius * gData.radius - dot(rand.xy, rand.xy));
        // probability for choosing this sample
        const float pdf = 2.0 * sphereHeight;
        //float adaptiveHaloHeight = sqrt((gData.radius + HALO_RADIUS) * (gData.radius + HALO_RADIUS) - dot(rand.xy, rand.xy));

        // determine distance within [-sphereHeight, +sphereHeight]
        float sphereStart = sphereHeight; // in object coordinates (bigger is closer to the camera)
        float sphereEnd = -sphereHeight; // in object coordinates (smaller is futher from the camera)
        //float haloStart = sphereStart;
        //if (PREVENT_DARK_HALOS)
        //    haloStart = sphereStart + HALO_RADIUS;

        if (SAMPLE_MODE == SAMPLE_MODE_HEMISPHERE) // determine correct sphereStart and sphereEnd for hemisphere
        {
            //float zIntersect = -dot(rand.xy, normalO.xy) / normalO.z;
            float zIntersect = -dot(rand.xy, normalO.xy) / makeNonZero(normalO.z, 0.0001);
            float zIntersectClamped = clamp(zIntersect, -sphereHeight, sphereHeight);
            if (normalO.z >= 0.0)
                sphereEnd = zIntersectClamped;
            else
                sphereStart = zIntersectClamped;
        }

        // if the sample range is too small, skip calculation (sample could be entirely below the surface hemisphere when looking from grazing angles)
        if (sphereStart - sphereEnd < 0.01)
        {
            gInvalid += 1;
            continue;
        }

        // calculate view position of sample and project to uv coordinates
        float3 initialSamplePosV = posV + tangent * rand.x + bitangent * rand.y;
        float2 samplePosUV = ViewSpaceToUV(initialSamplePosV);
        float curVisibility = 1.0f;

        #ifdef USE_DEPTH_BUFFER
        // clip sample position uv and snap to pixel center
        bool hybridRay = false;
        float2 screenUv = getScreenClampedUV(texC, samplePosUV); // clip to screen border
        const bool isInScreen = all(samplePosUV == screenUv);
        if (!isInScreen)
            hybridRay = true; // always shoot rays for screen border

        float2 rasterSamplePosUV = screenUv;
        rasterSamplePosUV = getSnappedUV(rasterSamplePosUV); // snap to pixel center

        // object space z of the primary depth buffer or the secondary depth buffer if available (will be set if raster or hybrid)
        float objectSpaceZ = sphereHeight + CONST_RADIUS + HALO_RADIUS;
        //if (SHADER_VARIANT == SHADER_VARIANT_RASTER || SHADER_VARIANT == SHADER_VARIANT_HYBRID)
        {      
            objectSpaceZ = calcObjectSpaceZ(posV, normal, rasterSamplePosUV, gDepthTex);
            curVisibility = calcVisibility(objectSpaceZ, sphereStart, sphereEnd);

            //if (DEPTH_MODE == DEPTH_MODE_SINGLE)
            
            // require hybrid ray if intersection is outside of sphere radius
            if (objectSpaceZ > sphereStart + CONST_RADIUS)
                hybridRay = true;
            
        }
        #else
        bool hybridRay = true;
        const bool isInScreen = false;
        float objectSpaceZ = 0.0;
        #endif

        if (hybridRay)
        {
            gRaysTraced += 1;

            if (!isInScreen) // reset visibility to 1 if the raster sample was not in screen
                curVisibility = 1.0;

            // to be consistent with the rasterizer, we snap the uv coordinate as well to the pixel center,
            // but we do not clip it since we can shoot outside of the screen space
            //samplePosUV = getScreenClampedUV(texC, samplePosUV);
            samplePosUV = getSnappedUV(samplePosUV); // snap to pixel center
            
            float3 sampleDirV = normalize(UVToViewSpace(samplePosUV, 1.0)); // get sample direction in view space
            float initialSamplePosLength = length(initialSamplePosV);

            bool occluded = false;

            RayDesc ray;
            ray.Origin = gCamera.data.posW;
            ray.Direction = mul(sampleDirV, float3x3(invViewMat));

            float tSphereStart = (posVLength - sphereStart) * initialSamplePosLength / posVLength;
            float tSphereEnd = (posVLength - sphereEnd) * initialSamplePosLength / posVLength;
            float tHaloStart = (posVLength - sphereHeight - CONST_RADIUS - HALO_RADIUS) * initialSamplePosLength / posVLength;
            float tConstRadiusStart = (posVLength - sphereHeight - CONST_RADIUS) * initialSamplePosLength / posVLength;
            ray.TMin = max(tHaloStart, 0.0);
            ray.TMax = tSphereEnd;

            // include the value of the depth buffer when choosing TMin to save some traversal time
            if (isInScreen)
            {
                const float epsilon = gData.radius * 0.01;
                ray.TMin = max(ray.TMin, (posVLength - objectSpaceZ) * initialSamplePosLength / posVLength + epsilon);
            }

            RayData rayData;
            rayData.tLastFrontFaceHalo = tHaloStart; // min
            rayData.tFirstFrontFaceInside = tSphereEnd; // max
            rayData.tConstRadiusStart = tConstRadiusStart;
            rayData.tSphereStart = tSphereStart;
            TraceRay(
                gScene.rtAccel,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                0xff /* instanceInclusionMask */, 0 /* hitIdx */, 1 /*ray type count*/, 0 /* missIdx */,
                ray, rayData
            );

            // calculate visibility inside and outside of sphere
            float sphereVisibility = calcSphereVisibility(posVLength - rayData.tFirstFrontFaceInside * posVLength / initialSamplePosLength, sphereStart, sphereEnd);
            float haloVisibility = calcHaloVisibility(posVLength - rayData.tLastFrontFaceHalo * posVLength / initialSamplePosLength, sphereStart, sphereEnd);

            curVisibility = min(curVisibility, min(sphereVisibility, haloVisibility));
        }

        visibility += curVisibility / pdf;
    }

    float AO = visibility / float(gData.kernelSize);

    // values should range from 0 for occluded, to 1 for visible
    if (SAMPLE_MODE == SAMPLE_MODE_SPHERE) 
        AO = saturate(AO);

    // since fully visibile are all values in [0.5, 1.0], scale accordingly
    if (SAMPLE_MODE == SAMPLE_MODE_HEMISPHERE)
        AO = saturate(AO * 2.0);

    // do artistic modifications
    AO = pow(AO, gData.exponent);

    gOutput[svPos] = AO;
}
