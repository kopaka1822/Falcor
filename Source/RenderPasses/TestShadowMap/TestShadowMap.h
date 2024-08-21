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
#include "SMStructs.slang"
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
class TestShadowMap : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(TestShadowMap, "TestShadowMap", "Minimal path tracer.");

    static ref<TestShadowMap> create(ref<Device> pDevice, const Properties& props) { return make_ref<TestShadowMap>(pDevice, props); }

    TestShadowMap(ref<Device> pDevice, const Properties& props);

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
        TestShadowMap::ShadowMode,
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
        TestShadowMap::FilterSMMode,
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

        void calculate(ref<Light> light, float2 nearFar);
    };

    void parseProperties(const Properties& props);
    void prepareBuffers();
    void prepareVars();
    DefineList filterSMModesDefines();

    void generateShadowMap(RenderContext* pRenderContext, const RenderData& renderData);
    void computeRayNeededMask(RenderContext* pRenderContext, const RenderData& renderData);
    void genReverseSM(RenderContext* pRenderContext, const RenderData& renderData);
    void genSparseShadowMap(RenderContext* pRenderContext, const RenderData& renderData);
    void calculateShadowMapNearFar(RenderContext* pRenderContext, const RenderData& renderData, ShaderVar& var);
    void traceScene(RenderContext* pRenderContext, const RenderData& renderData);
    void debugShadowMapPass(RenderContext* pRenderContext, const RenderData& renderData);

    // Takes a float2 input tex and writes the min max to the mip maps of the texture
    void generateMinMaxMips(RenderContext* pRenderContext, ref<Texture> texture);

    // Internal state
    ref<Scene> mpScene;                                 ///< Current scene.
    ref<SampleGenerator> mpSampleGenerator;             ///< GPU sample generator.
    std::unique_ptr<PixelStats> mpPixelStats;           ///< Stats for the current path
    std::unique_ptr<ShadowMap> mpRasterShadowMap;       //< Raster Shadow Map

    // Buffer and Samplers
    std::vector<ref<Texture>> mpRayShadowMaps;
    std::vector<ref<Texture>> mpRayShadowMapsMinMax;
    ref<Texture> mpShadowMapMinMaxOpti;
    ref<Sampler> mpShadowSamplerPoint;
    ref<Sampler> mpShadowSamplerLinear;
    ref<Texture> mpReverseSMTex;
    ref<Texture> mpRayShadowNeededMask;

    // Configuration Path Tracer
    bool mCheckForNaN = true; ///< Checks for NaN before writing to texture
    SMLightSampleMode mPathLightSampleMode = SMLightSampleMode::All; // Mode for sampling the analytic lights

    // Config Shadow Map
    ShadowMode mShadowMode = ShadowMode::ShadowMap;
    FilterSMMode mFilterSMMode = FilterSMMode::Variance;
    uint mSMGenerationUseRay = 1; // Shadow Map Generation: 0-> Raster; 1-> Ray
    bool mUseSMForDirect = true;
    bool mRebuildSMBuffers = true;
    bool mRerenderSM = true;
    bool mAlwaysRenderSM = true;
    bool mShadowMapFlag = false;
    bool mUseShadowMap = true;
    uint mShadowMapSize = 1024;
    std::vector<LightMVP> mShadowMapMVP;
    float2 mNearFar = float2(0.1, 60);
    std::vector<float2> mNearFarPerLight;
    bool mUseOptimizedNearFarForShadowMap = false;
    uint mUISelectedLight = 0;
    bool mDistributeRayOutsideOfSM = false;
    bool mNearFarChanged = false;

    // Debug
    bool mEnableDebug = false;
    PathSMDebugModes mDebugMode = PathSMDebugModes::LeakTracingMask;
    bool mAccumulateDebug = true;
    bool mResetDebugAccumulate = true;
    bool mClearDebugAccessTex = false;
    uint mIterationCount = 0;
    uint mDebugShowBounce = 0;
    float mDebugMult = 1.0f;
    float mDebugHeatMapMaxCountAccess = 4.f;
    float mDebugHeatMapMaxCountDifference = 0.25f;
    float mDebugAccessBlendVal = 0.3f;
    float2 mDebugBrighnessMod = float2(5.f, 1.f);
    bool mDebugUseSMAspect = true;
    bool mUseReverseSM = false;

    std::vector<ref<Texture>> mpShadowMapAccessTex;
    ref<Texture> mpShadowMapBlit;
    ref<ComputePass> mpDebugShadowMapPass;
    ref<ComputePass> mpGenMinMaxMipsPass;
    ref<ComputePass> mpComputeRayNeededMask;

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
    RayTracingProgram mReverseSM;
    RayTracingProgram mSparseDepthSM;
   
};

FALCOR_ENUM_REGISTER(TestShadowMap::ShadowMode);
FALCOR_ENUM_REGISTER(TestShadowMap::FilterSMMode);
FALCOR_ENUM_CLASS_OPERATORS(PathSMDebugModes);
