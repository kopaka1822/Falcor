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
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Rendering/Utils/PixelStats.h"
#include "PathSMStructs.slang"
#include "Rendering/ShadowMaps/ShadowMap.h"
#include "Rendering/ShadowMaps/Oracle/ShadowMapOracle.h"

using namespace Falcor;

/** Minimal path tracer.

    This pass implements a minimal brute-force path tracer. It does purposely
    not use any importance sampling or other variance reduction techniques.
    The output is unbiased/consistent ground truth images, against which other
    renderers can be validated.

    Note that transmission and nested dielectrics are not yet supported.
*/
class TestPathSM : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(TestPathSM, "TestPathSM", "Minimal path tracer.");

    static ref<TestPathSM> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<TestPathSM>(pDevice, props);
    }

    TestPathSM(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    enum class ShadowMode : uint32_t
    {
        ShadowMap = 0,
        LeakTracing = 1,
        RayShadows = 2,
    };
    FALCOR_ENUM_INFO(
        TestPathSM::ShadowMode,
        {
            {ShadowMode::ShadowMap, "ShadowMap"},
            {ShadowMode::LeakTracing, "LeakTracing"},
            {ShadowMode::RayShadows, "RayShadows"},
        }
    );

    enum class FilterSMMode : uint32_t
    {
        Variance = 0,
        ESVM = 1,
        MSM = 2,
    };
    FALCOR_ENUM_INFO(
        TestPathSM::FilterSMMode,
        {
            {FilterSMMode::Variance, "Variance"},
            {FilterSMMode::ESVM, "ESVM"},
            {FilterSMMode::MSM, "MSM"},
        }
    );

private:  
    struct LightMVP
    {
        float4x4 view = float4x4();
        float4x4 projection = float4x4();
        float4x4 viewProjection = float4x4();
        float4x4 invViewProjection = float4x4();
    };


    void parseProperties(const Properties& props);
    void prepareBuffers();
    void prepareVars();
    DefineList filterSMModesDefines();

    void generateShadowMap(RenderContext* pRenderContext, const RenderData& renderData);
    void traceScene(RenderContext* pRenderContext, const RenderData& renderData);
    void debugShadowMapPass(RenderContext* pRenderContext, const RenderData& renderData);

    // Internal state
    ref<Scene> mpScene;                     ///< Current scene.
    ref<SampleGenerator> mpSampleGenerator; ///< GPU sample generator.
    std::unique_ptr<PixelStats> mpPixelStats;           ///< Stats for the current path
    std::unique_ptr<ShadowMapOracle> mpShadowMapOracle; ///< Shadow Map Oracle for tests
    std::unique_ptr<ShadowMap> mpRasterShadowMap;       //< Raster Shadow Map

    //Buffer and Samplers
    std::vector<ref<Texture>> mpRayShadowMaps;
    std::vector<ref<Texture>> mpRayShadowMapsMinMax;
    ref<Sampler> mpShadowSamplerPoint;
    ref<Sampler> mpShadowSamplerLinear;

    // Configuration Path Tracer
    uint mMaxBounces = 12;               ///< Max number of indirect bounces (0 = none).
    uint mMaxDiffuseBounces = 5;        ///< Max number of diffuse bounces
    uint mMaxSpecularBounces = 5;        ///< Max number of specular bounces
    uint mMaxTransmissiveBounces = 12;   ///< Max number of transmissive bounces
    bool mComputeDirect = true;         ///< Compute direct illumination (otherwise indirect only).
    bool mUseImportanceSampling = true; ///< Use importance sampling for materials.
    bool mUseRussianRoulette = false;
    bool mUseSeperateLightSampler = false;
    uint mSeperateLightSamplerBlockSize = 32;
    PathSMLightSampleMode mPathLightSampleMode = PathSMLightSampleMode::RIS;    //Mode for sampling the analytic lights 

    //Config Shadow Map
    ShadowMode mShadowMode = ShadowMode::LeakTracing;
    FilterSMMode mFilterSMMode = FilterSMMode::Variance;
    uint mSMGenerationUseRay = 1;       //Shadow Map Generation: 0-> Raster; 1-> Ray
    bool mRebuildSMBuffers = true;
    bool mRerenderSM = true;
    bool mAlwaysRenderSM = false;
    bool mShadowMapFlag = false;
    bool mUseShadowMap = true;
    bool mUseMinMaxShadowMap = true;
    uint mShadowMapSize = 2048;
    uint mShadowMapSamples = 32;
    std::vector<LightMVP> mShadowMapMVP;
    float2 mNearFar = float2(0.1, 60);
    float mLtBoundsStart = 0.05f;
    float mLtBoundsMaxReduction = 0.2f;

    //Debug
    bool mEnableDebug = false;
    PathSMDebugModes mDebugMode = PathSMDebugModes::LeakTracingMask;
    bool mAccumulateDebug = true;
    bool mResetDebugAccumulate = true;
    bool mClearDebugAccessTex = false;
    uint mDebugShowLight = 0;
    uint mIterationCount = 0;
    uint mDebugShowBounce = 0;
    float mDebugMult = 1.0f;
    float mDebugHeatMapMaxCountAccess = 4.f;
    float mDebugHeatMapMaxCountDifference = 0.25f;
    float mDebugAccessBlendVal = 0.3f;
    float2 mDebugBrighnessMod = float2(5.f,1.f);
    bool mDebugUseSMAspect = true;

    std::vector<ref<Texture>> mpShadowMapAccessTex;
    ref<Texture> mpShadowMapBlit;
    ref<ComputePass> mpDebugShadowMapPass;

    // Runtime data
    uint mFrameCount = 0; ///< Frame count since scene was loaded.
    bool mOptionsChanged = false;

    // Ray tracing program.
    struct RayTracingProgram
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    };

    RayTracingProgram mTracer;
    RayTracingProgram mGenerateSM;
};

FALCOR_ENUM_REGISTER(TestPathSM::ShadowMode);
FALCOR_ENUM_REGISTER(TestPathSM::FilterSMMode);
FALCOR_ENUM_CLASS_OPERATORS(PathSMDebugModes);
