import LTTMaskSamplePatterns;

//Textures
Texture2D<uint> gLTTMaskLastFrame;
RWTexture2D<uint> gLTTMask;

// Reprojection
Texture2D<float> gPrevDepth;
RWTexture2D<float> gPrevDepthWrite;

//Sampler
SamplerState gLTTMaskSampler;    //Point sampler 

static const bool kUseLTTMask = USE_LTT_MASK;
static const uint2 kLTTMaskDims = LTT_MASK_DIMS;
static const uint kLTTSampleCount = LTT_MASK_SAMPLE_COUNT;
static const float kLTTMaskRemoveRaysGreaterAsDistance = LTT_MASK_REMOVE_RAYS_GREATER_AS_DISTANCE;
static const float kLTTMaskRemoveRaysSmallerAsDistance = LTT_MASK_REMOVE_RAYS_SMALLER_AS_DISTANCE;
static const float kLTTMaskExpandRaysMaxDistance = LTT_MASK_EXPAND_RAYS_MAX_DISTANCE;
static const float kLTTMaskDistanceDiffFactor = LTT_TEMPORAL_DEPTH_TEST_MAX_DEPTH_DIFF;
static const bool kUseRayWhenOutsideOfMask = LTT_MASK_USE_RAY_WHEN_OUTSIDE;

/** Struct for the LTT mask
*/
struct LTTMask{
    uint mask;

    __init(){
        mask = 0;
    }

    __init(uint mask){
        this.mask = mask;
    }

    __init(bool shadowTest, bool leakTracingTest, bool rayShadowed){
        mask = 0;
        if(shadowTest) mask += 1;
        if(leakTracingTest) mask += 2;
        if(rayShadowed) mask += 4;
    }

    property bool shadowTest{
        get { return (mask & 1) > 0; }
    }

    property bool leakTracingTest{
        get { return (mask & 2) > 0; }
    }

    property bool rayShadowed{
        get { return (mask & 4) > 0; }
    }

    uint maskOut(){
        return mask;
    }
}

//Evaluate the LTT mask
bool evaluateLTTMask(uint2 pixel, float2 mvec, bool ltt, bool shadowTest, out bool clampShadow, float linZ , uint frameCount = 0, bool isDynamic = false){
    
    clampShadow = false;
    //Reproject with motion vectors
    float2 pixelF = float2(DispatchRaysIndex().xy) + 0.5;
    const float2 dims = float2(kLTTMaskDims);
    float2 pixelOffset = mvec * dims;
    pixelF += pixelOffset;
    pixelF = floor(pixelF) + 0.5;

    //Is the reprojected pixel outside of the screen?
    if (any(pixelF < 0) || (pixelF.x >= dims.x) || (pixelF.y >= dims.y)) {
        return kUseRayWhenOutsideOfMask || ltt;
    }

    //Optional temporal depth test that disables Remove Rays 
#if LTT_MASK_USE_TEMPORAL_DEPTH_TEST
    float prevLinZ = gPrevDepth[uint2(pixelF)];
    float maxDistDiff = linZ * kLTTMaskDistanceDiffFactor;
    const bool tempDepthTest = abs(linZ - prevLinZ) < maxDistDiff;
#else
    bool tempDepthTest = true;
#endif // USE_TEMPORAL_DEPTH_TEST

    //Init mask parameters
    uint maskCount = 0;
    LTTMask mask[kLTTSampleCount];

    ///
    /// Sample with a 3x3 Sample Pattern
    ///

//Get the sample offsets for the responding pattern
#if LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_Plus
    const int2 indexOffsets[kLTTSampleCount] = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)}; //Count:5
#elif LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_Cross
    const int2 indexOffsets[kLTTSampleCount] = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#elif LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_Box_3x3
    const int2 indexOffsets[kLTTSampleCount] = {int2(-1,1),int2(0,1),int2(1,1),int2(-1,0),int2(0),int2(1,0),int2(-1,-1),int2(0,-1),int2(1,-1)};//Count:9
//Checkerboard switch between cross and and plus for neighboring pixels. Patter switches every frame
#elif LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_Checkerbord_3x3
    int2 indexOffsets[kLTTSampleCount];
    int2 projectedPixel = int2(pixelF);
    bool2 even = (projectedPixel % 2) == 0;
    bool usePlus = (frameCount%2 == 0) ? (even.x == even.y) : (even.x != even.y);
    if(usePlus)    //Plus
        indexOffsets = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};//Count:5
    else        //Cross
        indexOffsets = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#elif LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_PlusCross
    int2 indexOffsets[kLTTSampleCount];
    if((frameCount%2 == 0))    //Plus
        indexOffsets = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};//Count:5
    else        //Cross
        indexOffsets = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#endif //LTT_MASK_SAMPLE_PATTERN

//Fetch the samples with for a 3x3 Pattern
#if LTT_MASK_SAMPLE_PATTERN <= LTT_MASK_SAMPLE_PATTERN_PlusCross
    for(uint i=0; i<kLTTSampleCount; i++){
        int2 idx = int2(pixelF + indexOffsets[i]);
        if(any(idx < 0) || idx.x >= dims.x || idx.y >= dims.y)
            continue;
        mask[maskCount] = HybridMask(gLTTMaskLastFrame[idx]);
        maskCount++;
    }
#endif

    ///
    /// Sample via Gather
    ///

//Sample via Gather
#if LTT_MASK_SAMPLE_PATTERN == LTT_MASK_SAMPLE_PATTERN_Gather
    const float2 gatherPixelOffsets[4] = {float2(0.25, -0.25), float2(-0.25, -0.25), float2(0.25, 0.25), float2(-0.25, 0.25)};
    float2 uvs = (pixelF - gatherPixelOffsets[frameCount%4]) / float2(dims);
    uint4 gatherMasks = gLTTMaskLastFrame.GatherRed(gLTTMaskSampler, uvs);
    for(maskCount = 0; maskCount < 4; maskCount++)
        mask[maskCount] = LTTMask(gatherMasks[maskCount]);
#endif

    ///
    /// Expand Rays
    ///

//Check if ray needs to be expanded (expands ray on shadow edges)
bool expandRay = false;
#if LTT_MASK_EXPAND_RAYS
    #if LTT_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        if(!ltt && (linZ < kLTTMaskExpandRaysMaxDistance))   //With distance constraint
    #else//LTT_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        if(!ltt)                                                //Without constraint
    #endif//LTT_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        {
            for(uint i=0; i<maskCount; i++){
                //expandRay |= (mask[i].leakTracingTest && mask[i].rayShadowed && !shadowTest) || (mask[i].leakTracingTest && !mask[i].rayShadowed && shadowTest) || (mask[i].shadowTest != shadowTest);    //Unsimplifed
                expandRay |= (mask[i].leakTracingTest && (mask[i].rayShadowed != shadowTest)) || (mask[i].shadowTest != shadowTest);     //Simplfied, does the same as the above with less operations
            }
        ltt |= expandRay;
    }
#endif //LTT_MASK_EXPAND_RAYS

    ///
    /// Remove Rays
    ///

    //Check if ray is necessary (removes ray from core shadow)
#if LTT_MASK_REMOVE_RAYS
    bool removeRayTest = ltt && !expandRay && !isDynamic && tempDepthTest;   //Check if ray was expanded or is a special case
    #if LTT_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        if (removeRayTest && ((linZ > kLTTMaskRemoveRaysGreaterAsDistance) || (linZ < kLTTMaskRemoveRaysSmallerAsDistance)))  //With distance constraint
    #else  //LTT_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        if (removeRayTest)                                                                                                          //Without constraint
    #endif //LTT_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        {
        bool removeRays = true;
            for(uint i=0; i<maskCount; i++){
                //removeRays &= (!mask[i].leakTracingTest && (shadowTest == mask[i].shadowTest)) || (mask[i].leakTracingTest && (shadowTest == mask[i].shadowTest) && (shadowTest == mask[i].shadowRay));
                removeRays &= (shadowTest == mask[i].shadowTest) && (!mask[i].leakTracingTest || (shadowTest == mask[i].rayShadowed)); //Simplyfied version of the above
            }
        clampShadow = removeRays;    //Clamp shadow to be consistent
        ltt = !removeRays;
        }
#endif //LTT_MASK_REMOVE_RAYS

    return ltt;
}

void writeLTTMask(const uint2 pixel, bool shadowTest, bool leakTracingTest, bool rayShadowed, float camDist) {
    LTTMask mask = LTTMask(shadowTest, leakTracingTest, rayShadowed);
    gLTTMask[pixel] = mask.maskOut();
#if LTT_MASK_USE_TEMPORAL_DEPTH_TEST
    gPrevDepthWrite[pixel] = camDist;
#endif // USE_TEMPORAL_DEPTH_TEST
}

void writeLTTMask(const uint2 pixel, bool shadowTest, bool leakTracingTest, bool rayShadowed){
    LTTMask mask = LTTMask(shadowTest, leakTracingTest, rayShadowed);
    gLTTMask[pixel] = mask.maskOut();
}

void writeLTTMask(const uint2 pixel, LTTMask mask){
    gLTTMask[pixel] = mask.maskOut();
}
