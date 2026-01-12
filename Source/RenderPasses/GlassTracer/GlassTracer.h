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
#include "Utils/SampleGenerators/HaltonSamplePattern.h"
#include "../DitherVBuffer/TransparencyWhitelist.h"

using namespace Falcor;

class GlassTracer : public RenderPass
{
public:
    // based on DLSS scales
    enum class RenderScale : uint32_t
    {
        Full,
        Quality,
        Balanced,
        Performance,
        UtraPerformance,
    };

    FALCOR_ENUM_INFO(RenderScale, {
        {RenderScale::Full, "Full (100%)"},
        {RenderScale::Quality, "Quality (66.7%)"},
        {RenderScale::Balanced, "Balanced (58%)"},
        {RenderScale::Performance, "Performance (50%)"},
        {RenderScale::UtraPerformance, "UltraPerformance (33.3%)"},
        });

    enum class MotionVector : uint32_t
    {
        FirstHit,
        HalfwayReflection,
        RayDifferentials,
        ReverseRayDifferentials
    };

    FALCOR_ENUM_INFO(MotionVector, {
        {MotionVector::FirstHit, "FirstHit"},
        {MotionVector::HalfwayReflection, "HalfwayReflection"},
        {MotionVector::RayDifferentials, "RayDifferentials"},
        {MotionVector::ReverseRayDifferentials, "ReverseRayDifferentials"},
    });

    FALCOR_PLUGIN_CLASS(GlassTracer, "GlassTracer", "Path Tracer specialized for noise-free glass rendering");

    enum class IterationTechnique : uint32_t
    {
        None,
        Forward,
        Reverse
    };

    FALCOR_ENUM_INFO(IterationTechnique, {
        {IterationTechnique::None, "None"},
        {IterationTechnique::Forward, "Forward"},
        {IterationTechnique::Reverse, "Reverse"},
    });

    enum class OpticalFlowTechnique : uint32_t
    {
        None,
        LucasKanadePos,
        LucasKanadeColor,
    };

    FALCOR_ENUM_INFO(OpticalFlowTechnique, {
        {OpticalFlowTechnique::None, "None"},
        {OpticalFlowTechnique::LucasKanadePos, "Lucas-Kanade (Position)"},
        {OpticalFlowTechnique::LucasKanadeColor, "Lucas-Kanade (Color)"},
    });

    static ref<GlassTracer> create(ref<Device> pDevice, const Properties& props) { return make_ref<GlassTracer>(pDevice, props); }

    GlassTracer(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    static uint2 getRenderSize(uint2 displaySize, RenderScale scale)
    {
        uint2 res = displaySize;
        switch (scale)
        {
        case RenderScale::Quality:
            res = uint2(ceil(float2(displaySize) * 0.667f));
            break;
        case RenderScale::Balanced:
            res = uint2(ceil(float2(displaySize) * 0.58f));
            break;
        case RenderScale::Performance:
            res = uint2(ceil(float2(displaySize) * 0.50f));
            break;
        case RenderScale::UtraPerformance:
            res = uint2(ceil(float2(displaySize) * 0.333f));
            break;
        }
        res = max(res, uint2(1));
        return res;
    }
private:

    void setupProgram();
    // returns true if at least one material was whitelisted (or scene was invalid)
    bool updateWhitelistBuffer();

    ref<Scene> mpScene;

    ref<RtProgram> mpProgram;
    ref<RtProgramVars> mpVars;
    ref<SampleGenerator> mpSampleGenerator;
    ref<Buffer> mpTransparencyWhitelist;

    ref<Buffer> mpStackBuffer;

    uint mFrameCount = 0;

    ref<CPUSampleGenerator> mpSamplePattern;
    bool mUseTransparencyWhitelist = false;
    whitelist_t mTransparencyWhitelist;

    bool mCullBackFaces = false;
    int mPathLength = 50;
    int mStackSize = 2;
    MotionVector mMotionVector = MotionVector::FirstHit;
    IterationTechnique mIterationTechnique = IterationTechnique::None;

    // iterations
    int mIterations = 1;
    bool mForceIterationPathLength = true;
    ref<RtProgram> mpIterationProgram;
    ref<RtProgramVars> mpIterationVars;

    // optical flow
    ref<Texture> mpPrevPosition;
    ref<Texture> mpPrevColor;
    ref<Texture> mpMotionPong;
    ref<ComputePass> mpOpticalFlowPosPass;
    ref<ComputePass> mpOpticalFlowColorPass;
    ref<ComputePass> mpOpticalBlurPass;
    OpticalFlowTechnique mOpticalFlowTechnique = OpticalFlowTechnique::None;
    int mOpticalIterations = 2;
    float mOpticalMaxMovement = 10.0f;
    int mOpticalRadius = 1;
    ref<RenderGraph> mpVbufferToPosGraph;
    float2 mPrevJitter;

    RenderScale mRenderScale = RenderScale::UtraPerformance;

    // lighting settings
    float mAmbientIntensity = 0.25f;
    float mAnalyticIntensity = 0.5f;
    float mEmissionIntensity = 1.0f;
    float mEnvmapIntensity = 1.0f;
    // shadow settings
    bool mEnableShadows = true;
    float mPointLightClip = 0.2f;
    float mShadowLodBias = 0.0f;

    float mRoughnessCutoff = 0.5f;
    bool mForceZeroRoughness = true;
    bool mForceMotionVectorCalculation = false;
    bool mIgnoreNormalDiffs = true; // more stable without normal diffs

    bool mUseTextureLOD = true;
    float mTextureLodBias = -1.5f;

    bool mOptionsChanged = true;
};

FALCOR_ENUM_REGISTER(GlassTracer::RenderScale);
FALCOR_ENUM_REGISTER(GlassTracer::MotionVector);
FALCOR_ENUM_REGISTER(GlassTracer::IterationTechnique);
FALCOR_ENUM_REGISTER(GlassTracer::OpticalFlowTechnique);
