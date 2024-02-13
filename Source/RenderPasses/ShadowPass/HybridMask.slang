/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
import HybridMaskSamplePatterns;

//Textures
Texture2D<uint> gHybridMaskLastFrame;
RWTexture2D<uint> gHybridMask;

//Sampler
SamplerState gHybridMaskSampler;    //Point sampler 

static const bool kUseHybridMask = USE_HYBRID_MASK;
static const uint2 kHybridMaskDims = HYBRID_MASK_DIMS;
static const uint kHMSampleCount = HYBRID_MASK_SAMPLE_COUNT;
static const float kHybridMaskRemoveRaysGreaterAsDistance = HYBRID_MASK_REMOVE_RAYS_GREATER_AS_DISTANCE;
static const float kHybridMaskRemoveRaysSmallerAsDistance = HYBRID_MASK_REMOVE_RAYS_SMALLER_AS_DISTANCE;
static const float kHybridMaskExpandRaysMaxDistance = HYBRID_MASK_EXPAND_RAYS_MAX_DISTANCE;
/** Struct for the hybrid mask
*/
struct HybridMask{
    uint mask;

    __init(){
        mask = 0;
    }

    __init(uint mask){
        this.mask = mask;
    }

    __init(bool shadowSM, bool shootRay, bool shadowRay){
        mask = 0;
        if(shadowSM) mask += 1;
        if(shootRay) mask += 2;
        if(shadowRay) mask += 4;
    }

    property bool shadowSM{
        get { return (mask & 1) > 0; }
    }

    property bool shootRay{
        get { return (mask & 2) > 0; }
    }

    property bool shadowRay{
        get { return (mask & 4) > 0; }
    }

    uint maskOut(){
        return mask;
    }
}

//Evaluate the hybrid mask
bool evaluateHybridMask(uint2 pixel, float2 mvec, bool smUseRay, bool smShadowed, float linZ , uint frameCount = 0){
    
    bool useRay = smUseRay;
   
    //Reproject with motion vectors
    float2 pixelF = float2(DispatchRaysIndex().xy) + 0.5;
    const float2 dims = float2(kHybridMaskDims);
    float2 pixelOffset = mvec * dims;
    pixelF += pixelOffset;
        
    //Get the masks
    uint maskCount = 0;
    HybridMask mask[kHMSampleCount];

//Fetch mask depending on sample pattern
#if HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_Plus
    const int2 indexOffsets[kHMSampleCount] = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)}; //Count:5
#elif HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_Cross
    const int2 indexOffsets[kHMSampleCount] = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#elif HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_Box_3x3
    const int2 indexOffsets[kHMSampleCount] = {int2(-1,1),int2(0,1),int2(1,1),int2(-1,0),int2(0),int2(1,0),int2(-1,-1),int2(0,-1),int2(1,-1)};//Count:9
//Checkerboard switch between cross and and plus for neighboring pixels. Patter switches every frame
#elif HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_Checkerbord_3x3
    int2 indexOffsets[kHMSampleCount];
    int2 projectedPixel = int2(pixelF);
    bool2 even = (projectedPixel % 2) == 0;
    bool usePlus = (frameCount%2 == 0) ? (even.x == even.y) : (even.x != even.y);
    if(usePlus)    //Plus
        indexOffsets = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};//Count:5
    else        //Cross
        indexOffsets = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#elif HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_PlusCross
    int2 indexOffsets[kHMSampleCount];
    if((frameCount%2 == 0))    //Plus
        indexOffsets = {int2(0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};//Count:5
    else        //Cross
        indexOffsets = {int2(0),int2(-1,-1),int2(-1,1),int2(1,-1),int2(1,1)};//Count:5
#endif //HYBRID_MASK_SAMPLE_PATTERN

//Fetch the samples with for a 3x3 Pattern
#if HYBRID_MASK_SAMPLE_PATTERN <= HYBRID_MASK_SAMPLE_PATTERN_PlusCross
    for(uint i=0; i<kHMSampleCount; i++){
        int2 idx = int2(pixelF + indexOffsets[i]);
        if(any(idx < 0) || idx.x >= dims.x || idx.y >= dims.y)
            continue;
        mask[maskCount] = HybridMask(gHybridMaskLastFrame[idx]);
        maskCount++;
    }
#endif

//Sample via Gather (TODO more Patterns?)
#if HYBRID_MASK_SAMPLE_PATTERN == HYBRID_MASK_SAMPLE_PATTERN_Gather
    const float2 gatherPixelOffsets[4] = {float2(-0.25), float2(0.25, -0.25), float2(0.25), float2(-0.25, 0.25)};
    float2 uvs = (pixelF - gatherPixelOffsets[frameCount%4]) / float2(dims);
    uint4 gatherMasks = gHybridMaskLastFrame.GatherRed(gHybridMaskSampler, uvs);
    for(maskCount = 0; maskCount < 4; maskCount++)
        mask[maskCount] = HybridMask(gatherMasks[maskCount]);
#endif


    //Check if ray is necessary (removes ray from core shadow)
#if HYBRID_MASK_REMOVE_RAYS
    #if HYBRID_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        if(useRay && ((linZ > kHybridMaskRemoveRaysGreaterAsDistance) || (linZ < kHybridMaskRemoveRaysSmallerAsDistance)))
    #else //HYBRID_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        if(useRay)
    #endif //HYBRID_MASK_REMOVE_RAYS_USE_MIN_DISTANCE
        {
            bool needRay = false;
            for(uint i=0; i<maskCount; i++){
                needRay |= smShadowed && (!mask[i].shadowSM || (mask[i].shootRay && !mask[i].shadowRay));
                needRay |= !smShadowed && (mask[i].shadowSM || (mask[i].shootRay && mask[i].shadowRay));
            }
        useRay = needRay;
        }
#endif //HYBRID_MASK_REMOVE_RAYS
    //Check if ray needs to be expanded (expands ray on shadow edges)
#if HYBRID_MASK_EXPAND_RAYS
    #if HYBRID_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        if(!useRay && (linZ < kHybridMaskExpandRaysMaxDistance))
    #else//HYBRID_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        if(!useRay)
    #endif//HYBRID_MASK_EXPAND_RAYS_USE_MAX_DISTANCE
        {
            bool expandRay = false;
            for(uint i=0; i<maskCount; i++){
                expandRay |= (mask[i].shootRay && mask[i].shadowRay && !smShadowed) || (mask[i].shootRay && !mask[i].shadowRay && smShadowed);
        }
        useRay |= expandRay;
    }
#endif //HYBRID_MASK_EXPAND_RAYS

    return useRay;
}

void writeHybridMask(const uint2 pixel, bool shadowSM, bool shootRay, bool shadowRay){
    HybridMask mask = HybridMask(shadowSM, shootRay, shadowRay);
    gHybridMask[pixel] = mask.maskOut();
}

void writeHybridMask(const uint2 pixel, HybridMask mask){
    gHybridMask[pixel] = mask.maskOut();
}
