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
#include "Core/Enum.h"
#include "RenderGraph/RenderPass.h"
#include "SharedTypes.slang"
#include "Rendering/AccelerationStructure/CustomAccelerationStructure.h"

using namespace Falcor;

class TransparencyLinkedList : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(TransparencyLinkedList, "TransparencyLinkedList", "Path Tracer with Transparencys");

    static ref<TransparencyLinkedList> create(ref<Device> pDevice, const Properties& props) { return make_ref<TransparencyLinkedList>(pDevice, props); }

    TransparencyLinkedList(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    enum class ShadowEvalMode : uint
    {
        RayTracing = 0u,
        LinkedList = 2u,
    };

    FALCOR_ENUM_INFO(ShadowEvalMode,{
        {ShadowEvalMode::RayTracing, "RayTracing"},
        {ShadowEvalMode::LinkedList, "LinkedList"},
    });

private:
    struct LightMVP
    {
        float3   pos = float3(0);
        uint     _pad = 0;
        float4x4 view = float4x4();
        float4x4 projection = float4x4();
        float4x4 viewProjection = float4x4();
        float4x4 invViewProjection = float4x4();

        void calculate(ref<Light> light, float2 nearFar);
    };


    void updateSMMatrices(RenderContext* pRenderContext, const RenderData& renderData);
    void generateLinkedLists(RenderContext* pRenderContext, const RenderData& renderData);
    void traceScene(RenderContext* pRenderContext, const RenderData& renderData);
    void prepareVars();

    // Internal state
    ref<Scene> mpScene;                     ///< Current scene.
    ref<SampleGenerator> mpSampleGenerator; ///< GPU sample generator.

    // Configuration Tracer
    TPTLightSampleMode mLightSampleMode = TPTLightSampleMode::RIS;
    uint mMaxBounces = 0;               ///< Max number of indirect bounces (0 = none).
    uint mMaxAlphaTestPerBounce = 32;   ///< Max number of allowed alpha tests per bounce
    bool mComputeDirect = true;         ///< Compute direct illumination (otherwise indirect only).
    bool mUseImportanceSampling = true; ///< Use importance sampling for materials.
    bool mUseRussianRoulettePath = false;   ///< Russian Roulett to abort the path early
    bool mUseRussianRouletteForAlpha = false; ///< Use Russian Roulette for transparent materials
    ShadowEvalMode mShadowEvaluationMode = ShadowEvalMode::LinkedList;

    // Runtime data Tracer
    uint mFrameCount = 0; ///< Frame count since scene was loaded.
    bool mDebugFrameCount = false;
    bool mOptionsChanged = false;
    ref<Sampler> mpPointSampler;
    std::vector<uint> mAccelShadowNumPoints;
    uint mAccelShadowMaxNumPoints = 0;

    //Configuration Shadow Map
    bool mAVSMRebuildProgram = false;
    bool mAVSMTexResChanged = false;
    bool mAVSMUsePCF = false;
    bool mAVSMUseInterpolation = false;
    uint mSMSize = 512;
    float2 mNearFar = float2(1.f, 40.f);
    float mDepthBias = 1e-6f;
    float mNormalDepthBias = 1e-3f;
    uint mNumberAVSMSamples = 8;    //< k for AVSM

    //Runtime Data DeepSM
    std::vector<LightMVP> mShadowMapMVP;

    // Ray tracing program.
    struct RayTracingPipeline
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    };

    RayTracingPipeline mTracer;

    // Linked list
    std::vector<ref<Buffer>> mpLinkedList;
    std::vector<ref<Buffer>> mpLinkedListNeighbors;
    std::vector<ref<Buffer>> mpLinkedListArray;
    std::vector<ref<Texture>> mpLinkedListArrayOffsets;
    bool mUseLinkedListPcf = false;
    bool mUseLinkedListArray = false;
    uint32_t mLinkedElementCount = 512 * 512 * 16; // (~70mb with 16 byte data => 4 floats)
    RayTracingPipeline mGenLinkedListPip;
    ref<Buffer> mpLinkedListCounter;
    ref<Buffer> mpLinkedListCounter2;
    ref<ComputePass> mpLinkedListNeighborsPass;
};

FALCOR_ENUM_REGISTER(TransparencyLinkedList::ShadowEvalMode);
