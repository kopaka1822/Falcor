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
#pragma once
#include "Core/Macros.h"
#include "Core/Enum.h"
#include "Core/State/GraphicsState.h"
#include "Core/Program/ComputeProgram.h"
#include "Core/Program/GraphicsProgram.h"
#include "Core/Program/Program.h"
#include "Core/Program/ProgramReflection.h"
#include "Core/Program/ProgramVars.h"
#include "Core/Program/ProgramVersion.h"
#include "Core/Program/RtProgram.h"
#include "Utils/Properties.h"
#include "Utils/Debug/PixelDebug.h"
#include "Scene/Scene.h"

#include "ShadowMapData.slang"
#include "Blur/SMGaussianBlur.h"
#include "Oracle/ShadowMapOracle.h"

#include <memory>
#include <type_traits>
#include <vector>
#include <map>

/*
    Wrapper Module for Shadow Maps, which allow ShadowMaps to be easily integrated into every Render Pass.
*/
namespace Falcor
{
class RenderContext;

class FALCOR_API ShadowMap
{
public:
    ShadowMap(ref<Device> device, ref<Scene> scene, bool enableOracle = false);

    // Renders and updates the shadow maps if necessary
    bool update(RenderContext* pRenderContext);

    //Shadow map render UI, returns a boolean if the renderer should be refreshed
    bool renderUI(Gui::Widgets& widget);

    // Shadow map render UI for Leak Tracing paper, returns a boolean if the renderer should be refreshed
    bool renderUILeakTracing(Gui::Widgets& widget, bool leakTracingEnabled);

    // Returns a define List with all the defines. Need to be called once per frame to update defines
    DefineList getDefines() const;
    // Sets Shader data
    void setShaderData(const uint2 frameDim = uint2(1920u, 1080u));

    // Sets the shader data and binds the block to var "gShadowMap"
    void setShaderDataAndBindBlock(ShaderVar rootVar, const uint2 frameDim = uint2(1920u, 1080u));

    //Set if ray tracing is enabled in the used render
    void setEnableRayTracing(bool enableRayTracing)
    {
        mCanUseRayTracing = enableRayTracing;
        mUpdateShadowMap |= true;
    }

    // Gets the parameter block needed for shader usage
    ref<ParameterBlock> getParameterBlock() const { return mpShadowMapParameterBlock; }

    float getCascadedFarForLevel(uint level);
    float getCascadedFarLastHybridLevel() { return getCascadedFarForLevel(mCascadedLevelTrace); }
    float getCascadedAlphaTestDistance();
    const bool getMipMapsEnabled() const { return mUseShadowMipMaps; }
    const bool getIsStochasticCascadedLevelEnabled() const { return mUseStochasticCascadedLevels; }
    const bool getFullTracedCascadedUsed() const { return mCascadedLastLevelRayTrace; }
    const uint getCascadedLevelHybridIsUsed() const { return mCascadedLevelTrace; }
    const bool getRenderDoubleSidedOnly() const { return mSMDoubleSidedOnly; }

    enum class SMUpdateMode: uint
    {
        Static = 0,                 //Render once
        Dynamic = 1,              //Render every frame
    };

    enum class CascadedFrustumMode : uint32_t
    {
        Manual = 0u,
        AutomaticNvidia = 1u,
    };

private:
    const float kEVSM_ExponentialConstantMax = 42.f;    //Max exponential constant for Exponential Variance Shadow Maps
    const float kESM_ExponentialConstantMax = 84.f;     //Max exponential constant for Exponential Shadow Maps
    const static uint  kStagingBufferCount = 4;         // Number of staging buffers CPU buffers for GPU/CPU sync

    struct ShaderParameters
    {
        float4x4 viewProjectionMatrix = float4x4();

        float3 lightPosition = float3(0, 0, 0);
        bool disableAlpha = false;
        float nearPlane = 0.1f;
        float farPlane = 30.f;
    };
    struct VPMatrixBuffer
    {
        ref<Buffer> buffer = nullptr;
        ref<Buffer> staging = nullptr;
        std::array<uint64_t, kStagingBufferCount> stagingFenceWaitValues; // Fence wait values for staging cpu / gpu sync 
        uint32_t stagingCount = 0;

        void reset() {
            buffer.reset();
            staging.reset();
        }
    };
    struct CascadedTemporalReuse
    {
        bool valid = false;
        AABB aabb = AABB();
        float4x4 view = float4x4::identity();
        float4x4 ortho = float4x4::identity();
    };

    LightTypeSM getLightType(const ref<Light> light);
    void prepareShadowMapBuffers();
    void prepareRasterProgramms();
    void prepareProgramms();
    void prepareGaussianBlur();
    void setSMShaderVars(ShaderVar& var, ShaderParameters& params);
    void updateRasterizerStates();
    void updateSMVPBuffer(RenderContext* pRenderContext, VPMatrixBuffer& vpBuffer, std::vector<float4x4>& vpMatrix);

    DefineList getDefinesShadowMapGenPass(bool addAlphaModeDefines = true) const;

    void rasterCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext);
    bool rasterSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered);
    bool rasterCascaded(ref<Light> light, RenderContext* pRenderContext, bool cameraMoved);
    float4x4 getProjViewForCubeFace(uint face, const LightData& lightData, const float4x4& projectionMatrix, float3& lightTarget, float3& up);
    float4x4 getProjViewForCubeFace(uint face, const LightData& lightData, const float4x4& projectionMatrix);
    void calcProjViewForCascaded(const LightData& lightData, std::vector<bool>& renderLevel, bool forceUpdate = false);
    void dummyProfileRaster(RenderContext* pRenderContext); // Shows the rasterizeSzene profile even if nothing was rendered

    // Getter
    std::vector<ref<Texture>>& getShadowMapsCube() { return mpShadowMapsCube; }
    std::vector<ref<Texture>>& getShadowMaps() { return mpShadowMaps; }
    ref<Buffer> getViewProjectionBuffer() { return mpVPMatrixBuffer.buffer; }
    ref<Buffer> getLightMapBuffer() { return mpLightMapping; }
    ref<Sampler> getSampler() { return mpShadowSamplerPoint; }
    float getFarPlane() { return mFar; }
    float getNearPlane() { return mNear; }
    uint getResolution() { return mShadowMapSize; }
    uint getCountShadowMapsCube() const { return mpShadowMapsCube.size(); }
    uint getCountShadowMaps() const { return mpShadowMaps.size(); }
    

    //Internal Refs
    ref<Device> mpDevice;                               ///< Graphics device
    ref<Scene> mpScene;                                 ///< Scene                       

    //FBOs
    ref<Fbo> mpFbo;
    ref<Fbo> mpFboCube;
    ref<Fbo> mpFboCascaded;  

    //Oracle
    std::unique_ptr<ShadowMapOracle> mpOracle;

    //Additional Cull states
    std::map<RasterizerState::CullMode, ref<RasterizerState>> mFrontClockwiseRS;
    std::map<RasterizerState::CullMode, ref<RasterizerState>> mFrontCounterClockwiseRS;

    //****************//
    //*** Settings ***//
    //****************//

    // Common Settings
    ShadowMapType mShadowMapType = ShadowMapType::SDExponentialVariance;     //Type

    uint mShadowMapSize = 2048;
    uint mShadowMapSizeCube = 1024;
    uint mShadowMapSizeCascaded = 2048;

    ResourceFormat mShadowMapFormat = ResourceFormat::D32Float;                 //Format D32 (F32 for most) and [untested] D16 (Unorm 16 for most) are supported
    RasterizerState::CullMode mCullMode = RasterizerState::CullMode::None;      //Cull mode. Double Sided Materials are not culled
    bool mUseFrustumCulling = true;

    float mNear = 0.1f;
    float mFar = 60.f;

    bool mUsePCF = false;
    bool mUsePoissonDisc = false;
    float mPoissonDiscRad = 0.5f;
    float mPoissonDiscRadCube = 0.015f;

    bool mUseAlphaTest = true;
    uint mAlphaMode = 1; // Mode for the alpha test ; 1 = Basic, 2 = HashedIsotropic, 3 = HashedAnisotropic

    bool mUseRayOutsideOfShadowMap = true;
    bool mSMDoubleSidedOnly = false;

    bool mUseShadowMipMaps = false; ///< Uses mip maps for applyable shadow maps
    float mShadowMipBias = 1.0f;    ///< Bias used in mips (cos theta)^bias

    //Cascaded
    CascadedFrustumMode mCascadedFrustumMode = CascadedFrustumMode::AutomaticNvidia;
    uint mCascadedLevelCount = 4;
    float mCascadedFrustumFix = 0.85f;
    uint mCascadedLevelTrace = 2;       //Trace until level
    bool mCascadedLastLevelRayTrace = true;  //Traces every cascaded level after the option above. Shadow map is still reserved in memory but is not used/rendered 
    float mCascadedReuseEnlargeFactor = 0.15f; // Increases box size by the factor on each side
    bool mEnableTemporalCascadedBoxTest = true; //Tests the cascaded level against the cascaded level from last frame. Only updates if box is outside
    std::vector<bool> mBlurForCascaded = {true, true, true, true};
    uint mCascadedDisableAlphaLevel = 4;

    // Hybrid Shadow Maps
    float2 mHSMFilteredThreshold = float2(0.01f, 0.99f); // Threshold for filtered shadow map variants

    //Animated Light
    bool mSceneIsDynamic = false;
    bool mRerenderStatic = false;
    SMUpdateMode mShadowMapUpdateMode = SMUpdateMode::Static;
    bool mStaticTexturesReady[2] = {false, false}; // Spot, Cube
    bool mUpdateShadowMap = true;

    //Shadow Map
    bool mBiasSettingsChanged = false;
    int32_t mBias = 0;
    float mSlopeBias = 0.f;
    float mSMCubeWorldBias = 0.f;
   
    //Exponential
    float mExponentialSMConstant = 80.f;            //Value used in the paper
    float mEVSMConstant = 20.f;                     //Exponential Variance Shadow Map constant. Needs to be lower than the exponential counterpart
    float mEVSMNegConstant = 5.f;                   //Exponential Variance Shadow Map negative constant. Usually lower than the positive counterpart
    bool mEVSMExtraTest = false;                    //Uses an extra test abs(posEXP - negEXP)<e

    //Variance and MSM
    bool mVarianceUseSelfShadowVariant = false;

    float mMSMDepthBias = 0.0f;     //Depth Bias (x1000)
    float mMSMMomentBias = 0.003f;  //Moment Bias (x1000)

    bool mMSMUseVarianceTest = false;
    float mMSMVarianceThreshold = 0.05f; //Threshold for additional variance test in hybrid moment shadow maps

    //Blur
    bool mUseGaussianBlur = false;

    //UI
    bool mApplyUiSettings = false;
    bool mResetShadowMapBuffers = false;
    bool mShadowResChanged = false;
    bool mRasterDefinesChanged = false;
    bool mTypeChanged = false;
    
    //
    //Internal
    //

    //General
    bool mCanUseRayTracing = true;  //RayTracing can be disabled for some settings
    bool mClearDynamicSM = false;

    //Frustum Culling
    uint2 mFrustumCullingVectorOffsets = uint2(0, 0);   //Cascaded / Point
    std::vector<ref<FrustumCulling>> mFrustumCulling;

    //Cascaded
    std::vector<float4x4> mCascadedVPMatrix;
    std::vector<CascadedTemporalReuse> mCascadedTemporalReuse;  //Data for the temporal cascaded reuse
    std::vector<float> mCascadedFrustumManualVals = {0.05f, 0.15f, 0.3f,1.f}; // Values for Manual set Cascaded frustum. Initialized for 3 Levels
    float mCascadedMaxFar = 1000000.f;
    bool mUseStochasticCascadedLevels = false;
    float mCascadedStochasticRange = 0.05f;
    std::vector<float> mCascadedZSlices;
    std::vector<float2> mCascadedWidthHeight;

    //Misc
    bool mMultipleSMTypes = false;
    
    std::vector<float4x4> mSpotDirViewProjMat;      //Spot matrices
    std::vector<LightTypeSM> mPrevLightType;   // Vector to check if the Shadow Map Type is still correct            

    //Blur 
    std::unique_ptr<SMGaussianBlur> mpBlurShadowMap;
    std::unique_ptr<SMGaussianBlur> mpBlurCascaded;
    std::unique_ptr<SMGaussianBlur> mpBlurCube;

    //Textures and Buffers
    ref<Texture> mpCascadedShadowMaps; //Cascaded Shadow Maps for Directional Lights
    std::vector<ref<Texture>> mpShadowMapsCube;     // Cube Shadow Maps (Point Lights)
    std::vector<ref<Texture>> mpShadowMaps;         // 2D Texture Shadow Maps (Spot Lights + (WIP) Area Lights)
    std::vector<ref<Texture>> mpShadowMapsCubeStatic;     // Static Cube Shadow Maps. Only used if scene has animations
    std::vector<ref<Texture>> mpShadowMapsStatic;     // Static 2D Texture Shadow Maps (Spot Lights + (WIP) Area Lights). Only used if scene has animations
    ref<Buffer> mpLightMapping;
    VPMatrixBuffer mpVPMatrixBuffer;
    VPMatrixBuffer mpCascadedVPMatrixBuffer;
    ref<Texture> mpDepthCascaded;                  //Depth texture needed for some types of cascaded (can be null)
    ref<Texture> mpDepthCube;                      //Depth texture needed for the cube map
    ref<Texture> mpDepth;                          //Depth texture needed for some types of 2D SM (can be null)
    std::vector<ref<Texture>> mpDepthCubeStatic;   // Static cube depth map copy per shadow map
    std::vector<ref<Texture>> mpDepthStatic;       // Static 2D depth map copy per shadow map

    //Samplers
    ref<Sampler> mpShadowSamplerPoint;
    ref<Sampler> mpShadowSamplerLinear;

    //Parameter block
    ref<ComputePass> mpReflectTypes;               // Dummy pass needed to create the parameter block
    ref<ParameterBlock> mpShadowMapParameterBlock; // Parameter Block

    //Render Passes
    struct RasterizerPass
    {
        ref<GraphicsState> pState = nullptr;
        ref<GraphicsProgram> pProgram = nullptr;
        ref<GraphicsVars> pVars = nullptr;

        void reset()
        {
            pState.reset();
            pProgram.reset();
            pVars.reset();
        }
    };

    RasterizerPass mShadowCubeRasterPass;
    RasterizerPass mShadowMapRasterPass;
    RasterizerPass mShadowMapCascadedRasterPass;
};

}
