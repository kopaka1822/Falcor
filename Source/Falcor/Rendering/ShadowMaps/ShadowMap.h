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
#include "Core/API/GpuFence.h"
#include "Utils/Properties.h"
#include "Utils/Debug/PixelDebug.h"
#include "Scene/Scene.h"

#include "ShadowMapData.slang"
#include "SMGaussianBlur.h"

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
    ShadowMap(ref<Device> device, ref<Scene> scene);

    // Renders and updates the shadow maps if necessary
    bool update(RenderContext* pRenderContext);

    //Shadow map render UI, returns a boolean if the renderer should be refreshed
    bool renderUI(Gui::Widgets& widget);

    // Returns a define List with all the defines. Need to be called once per frame to update defines
    DefineList getDefines() const;
    // Sets Shader data
    void setShaderData(const uint2 frameDim = uint2(1920u, 1080u));

    // Sets the shader data and binds the block to var "gShadowMap"
    void setShaderDataAndBindBlock(ShaderVar rootVar, const uint2 frameDim = uint2(1920u, 1080u));

    // Gets the parameter block needed for shader usage
    ref<ParameterBlock> getParameterBlock() const { return mpShadowMapParameterBlock; }

    //Get Normalized pixel size used in oracle function
    float getNormalizedPixelSize(uint2 frameDim, float fovY, float aspect);
    float getNormalizedPixelSizeOrtho(uint2 frameDim, float width, float height);    //Ortho case

    enum class SMUpdateMode: uint
    {
        Static = 0,                 //Render once
        UpdateAll = 1,              //Render every frame
        UpdateOnePerFrame = 2,      //Render one light per frame
        UpdateInNFrames = 3         //Update all lights in N frames   
    };

    //Sample Pattern for Shadow Map Jitter
    enum class SamplePattern : uint32_t
    {
        None,
        DirectX,
        Halton,
        Stratified,
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
        float farPlane = 30.f;
    };
    struct RayShaderParameters
    {
        float4x4 viewProjectionMatrix = float4x4();
        float4x4 invViewProjectionMatrix = float4x4();

        float3 lightPosition = float3(0, 0, 0);
        float farPlane = 30.f;
        float nearPlane = 0.1f;
    };

    struct PreviousCascade
    {
        bool valid = false;
        float4x4 prevView;
        float3 min;
        float3 max;
    };

    LightTypeSM getLightType(const ref<Light> light);
    void prepareShadowMapBuffers();
    void prepareRasterProgramms();
    void prepareRayProgramms(const Program::TypeConformanceList& globalTypeConformances);
    void prepareProgramms();
    void prepareGaussianBlur();
    void setSMShaderVars(ShaderVar& var, ShaderParameters& params);
    void setSMRayShaderVars(ShaderVar& var, RayShaderParameters& params);
    void updateRasterizerStates();
    void handleNormalizedPixelSizeBuffer();
    void handleShadowMapUpdateMode();
    void updateJitterSampleGenerator();

    DefineList getDefinesShadowMapGenPass(bool addAlphaModeDefines = true) const;

    void rasterCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext);
    bool rasterSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered);
    bool rasterCascaded(uint index, ref<Light> light, RenderContext* pRenderContext, bool cameraMoved);
    void rayGenCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext);
    bool rayGenSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered);
    bool rayGenCascaded(uint index, ref<Light> light, RenderContext* pRenderContext, bool cameraMoved);
    float4x4 getProjViewForCubeFace(uint face, const LightData& lightData, const float4x4& projectionMatrix);
    void calcProjViewForCascaded(uint index, const LightData& lightData, std::vector<bool>& renderLevel);
    

    // Getter
    std::vector<ref<Texture>>& getShadowMapsCube() { return mpShadowMapsCube; }
    std::vector<ref<Texture>>& getShadowMaps() { return mpShadowMaps; }
    ref<Buffer> getViewProjectionBuffer() { return mpVPMatrixBuffer; }
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
    ref<CPUSampleGenerator> mpCPUJitterSampleGenerator; ///< Sample generator for shadow map jitter
    ref<GpuFence> mpFence;                              ///< Fence for GPU / CPU sync

    //FBOs
    ref<Fbo> mpFbo;
    ref<Fbo> mpFboCube;
    ref<Fbo> mpFboCascaded;  

    //Additional Cull states
    std::map<RasterizerState::CullMode, ref<RasterizerState>> mFrontClockwiseRS;
    std::map<RasterizerState::CullMode, ref<RasterizerState>> mFrontCounterClockwiseRS;

    //****************//
    //*** Settings ***//
    //****************//

    // Common Settings
    bool mUseRaySMGen = false;                                  //Generate Shadow Map with Ray Tracing
    ShadowMapType mShadowMapType = ShadowMapType::MSMHamburger;     //Type

    uint mShadowMapSize = 2048;
    uint mShadowMapSizeCube = 1024;
    uint mShadowMapSizeCascaded = 2048;

    ResourceFormat mShadowMapFormat = ResourceFormat::D32Float;                 //Format D32 (F32 for most) and [untested] D16 (Unorm 16 for most) are supported
    RasterizerState::CullMode mCullMode = RasterizerState::CullMode::Back;      //Cull mode. Double Sided Materials are not culled
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

    bool mUseShadowMipMaps = false; ///< Uses mip maps for applyable shadow maps
    float mShadowMipBias = 1.5f;    ///< Bias used in mips (cos theta)^bias

    //Cascaded
    CascadedFrustumMode mCascadedFrustumMode = CascadedFrustumMode::AutomaticNvidia;
    uint mCascadedLevelCount = 3;
    uint mCascadedTracedLevelsAtEnd = 1; //Adds N number of ray tracing only cascades at the end. Will only work on hybrid mode
    float mCascadedFrustumFix = 0.8f;
    float mCascZMult = 8.f; // Pushes the z Values apart
    float mCascadedReuseEnlargeFactor = 0.1f; // Increases box size by the factor on each side

    // Hybrid Shadow Maps
    bool mUseHybridSM = true; ///< Uses the Hybrid Shadow Maps; For "Classic" Shadow Maps based on: https://gpuopen.com/fidelityfx-hybrid-shadows/#details
    float2 mHSMFilteredThreshold = float2(0.02f, 0.98f); // Threshold for filtered shadow map variants

    //Animated Light
    bool mRerenderStatic = false;
    SMUpdateMode mShadowMapUpdateMode = SMUpdateMode::Static;
    bool mStaticTexturesReady[2] = {false, false}; // Spot, Cube
    uint mUpdateFrameCounter = 0;
    std::vector<bool> mShadowMapUpdateList[2];
    uint mUpdateEveryNFrame = 2;
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

    //Variance and MSM
    bool mVarianceUseSelfShadowVariant = false;

    bool mUseMinShadowValue = false; // Sets if there should be a minimum shadow value
    float mMinShadowValueVal = 0.4f; // The min allowed shadow value, else it is set to 0

    float mMSMDepthBias = 0.0f;     //Depth Bias (x1000)
    float mMSMMomentBias = 0.003f;  //Moment Bias (x1000)

    //Jitter And Blur
    SamplePattern mJitterSamplePattern = SamplePattern::None; // Sets the CPU Jitter generator
    uint mTemporalFilterLength = 10;                          // Temporal filter strength
    uint mJitterSampleCount = 16;                             // Number of Jitter samples

    bool mUseGaussianBlur = false;

    //Oracle
    bool mUseSMOracle = true;         ///< Enables Shadow Map Oracle function
    bool mUseOracleDistFactor = true; ///< Enables a lobe distance factor that is used in the oracle function 
    OracleDistFunction mOracleDistanceFunctionMode = OracleDistFunction::RoughnessSquare;   //Distance functions used in Oracle
   
    float mOracleCompaireValue = 1.f/9.f; ///< Compaire Value for the Oracle test. Tested against ShadowMapArea/CameraPixelArea.
    float mOracleCompaireUpperBound = 32.f;  ///< Hybrid mode only. If oracle is over this value, shoot an ray

    bool mOracleIgnoreDirect = true;            ///< Skip the Oracle function for direct hits and very specular (under the rougness thrs. below)
    float mOracleIgnoreDirectRoughness = 0.085f;  ///< Roughness threshold for which hits are counted as very specular    

    //UI
    bool mApplyUiSettings = false;
    bool mResetShadowMapBuffers = false;
    bool mShadowResChanged = false;
    bool mRasterDefinesChanged = false;
    bool mTypeChanged = false;
    
    //
    //Internal
    //

    //Frustum Culling
    uint2 mFrustumCullingVectorOffsets = uint2(0, 0);   //Cascaded / Point
    std::vector<ref<FrustumCulling>> mFrustumCulling;

    //Cascaded
    std::vector<float4x4> mCascadedVPMatrix;
    std::vector<PreviousCascade> mPreviousCascades; //Previous cascade for rendering optimizations
    std::vector<float> mCascadedFrustumManualVals = {0.1f, 0.3f, 0.6f}; // Values for Manual set Cascaded frustum. Initialized for 3 Levels
    uint mCascadedMatrixStartIndex = 0;         //Start index for the matrix buffer
    float mCascadedMaxFar = 1000000.f;
    bool mCascadedFirstThisFrame = true;
    std::vector<float> mCascadedZSlices;
    std::vector<float2> mCascadedWidthHeight;

    //Misc
    bool mMultipleSMTypes = false;
    uint2 mNPSOffsets = uint2(0);   //x = idx first spot; y = idx first cascade
    std::vector<float4x4> mSpotDirViewProjMat;      //Spot matrices
    std::vector<LightTypeSM> mPrevLightType;   // Vector to check if the Shadow Map Type is still correct
    std::array<uint, kStagingBufferCount> mStagingFenceWaitValues;  //Fence wait values for staging cpu / gpu sync               

    //Blur 
    std::unique_ptr<SMGaussianBlur> mpBlurShadowMap;
    std::unique_ptr<SMGaussianBlur> mpBlurCascaded;
    std::unique_ptr<SMGaussianBlur> mpBlurCube;

    //Textures and Buffers
    std::vector<ref<Texture>> mpCascadedShadowMaps; //Cascaded Shadow Maps for Directional Lights
    std::vector<ref<Texture>> mpShadowMapsCube;     // Cube Shadow Maps (Point Lights)
    std::vector<ref<Texture>> mpShadowMaps;         // 2D Texture Shadow Maps (Spot Lights + (WIP) Area Lights)
    std::vector<ref<Texture>> mpShadowMapsCubeStatic;     // Static Cube Shadow Maps. Only used if scene has animations
    std::vector<ref<Texture>> mpShadowMapsStatic;     // Static 2D Texture Shadow Maps (Spot Lights + (WIP) Area Lights). Only used if scene has animations
    ref<Buffer> mpLightMapping;
    ref<Buffer> mpVPMatrixBuffer;
    ref<Buffer> mpVPMatrixStangingBuffer;
    ref<Buffer> mpNormalizedPixelSize;             //Buffer with the normalized pixel size for each ShadowMap
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

    struct RayTraceProgramHelper
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;

        static const RayTraceProgramHelper create()
        {
            RayTraceProgramHelper r;
            r.pProgram = nullptr;
            r.pBindingTable = nullptr;
            r.pVars = nullptr;
            return r;
        }

        void initRTProgram(
            ref<Device> device,
            ref<Scene> scene,
            const std::string& shaderName,
            DefineList& defines,
            const Program::TypeConformanceList& globalTypeConformances
        );
    };

    RasterizerPass mShadowCubeRasterPass;
    RasterizerPass mShadowMapRasterPass;
    RasterizerPass mShadowMapCascadedRasterPass;
    RayTraceProgramHelper mShadowCubeRayPass;
    RayTraceProgramHelper mShadowMapRayPass;
    RayTraceProgramHelper mShadowMapCascadedRayPass;
};

}
