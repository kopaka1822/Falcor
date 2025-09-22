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

class DitherVBuffer2 : public RenderPass
{
public:
    enum class DitherMode : uint32_t
    {
        PerPixel2x2 = 0,
        PerPixel3x3 = 1,
        RussianRoulette = 4,
        SpatioTemporalBlueNoise = 11,
        Disabled = 0xff,
    };

    FALCOR_ENUM_INFO(DitherMode, {
        { DitherMode::Disabled, "Disabled" },
        { DitherMode::RussianRoulette, "RussianRoulette" },
        { DitherMode::SpatioTemporalBlueNoise, "SpatioTemporalBlueNoise" },
        { DitherMode::PerPixel2x2, "PerPixel2x2" },
        { DitherMode::PerPixel3x3, "PerPixel3x3"}
    });

    enum class CoverageCorrection : uint32_t
    {
        Disabled,
        DLSS,
        FSR,
        XeSS
    };

    FALCOR_ENUM_INFO(CoverageCorrection, {
        { CoverageCorrection::Disabled, "Disabled" },
        { CoverageCorrection::DLSS, "DLSS" },
        { CoverageCorrection::FSR, "FSR" },
        { CoverageCorrection::XeSS, "XeSS" }
    });

    enum class ObjectHashType : uint32_t
    {
        Quads,
        Geometry,
    };

    FALCOR_ENUM_INFO(ObjectHashType, {
        {ObjectHashType::Quads, "Quads"},
        {ObjectHashType::Geometry, "Geometry"},
    });

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

    FALCOR_PLUGIN_CLASS(DitherVBuffer2, "DitherVBuffer2", "VBuffer with Dithering options for transparency and reflections");

    static ref<DitherVBuffer2> create(ref<Device> pDevice, const Properties& props) { return make_ref<DitherVBuffer2>(pDevice, props); }

    DitherVBuffer2(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
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
    ref<Buffer> mpPermutations3x3Buffer;
    ref<Texture> mpSpatioTemporalBlueNoiseTex;
    ref<Texture> mpBlueNoise64Tex;

    uint mFrameCount = 0;

    ref<CPUSampleGenerator> mpSamplePattern;

    DitherMode mDitherMode = DitherMode::RussianRoulette;
    bool mUseTransparencyWhitelist = false;
    whitelist_t mTransparencyWhitelist;
    CoverageCorrection mCoverageCorrection = CoverageCorrection::DLSS;
    float mDLSSCorrectionStrength = 1.0;
    ObjectHashType mObjectHashType = ObjectHashType::Geometry;
    bool mAlignMotionVectors = false;
    bool mRotatePattern = true;

    bool mCullBackFaces = false;
    int mPathLength = 10;
    //bool mAlignMotionVectors = false; // align when using pixel grid techniques
    //bool mRotatePattern = true; // rotate pattern when using pixel grid techniques

    RenderScale mRenderScale = RenderScale::Full;

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

    bool mOptionsChanged = true;
};

FALCOR_ENUM_REGISTER(DitherVBuffer2::DitherMode);
FALCOR_ENUM_REGISTER(DitherVBuffer2::CoverageCorrection);
FALCOR_ENUM_REGISTER(DitherVBuffer2::ObjectHashType);
FALCOR_ENUM_REGISTER(DitherVBuffer2::RenderScale);
