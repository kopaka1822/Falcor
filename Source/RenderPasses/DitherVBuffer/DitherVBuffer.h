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
#include "Utils/SampleGenerators/StratifiedSamplePattern.h"
#include "TransparencyWhitelist.h"


using namespace Falcor;

// use this define to enable the use of hash grids for surface attached noise and techniques
// the use of hash grids requires calculation of ray differentials in the any-hit, which are a measurable performance hit
#define ENABLE_HASH_GRIDS

class DitherVBuffer : public RenderPass
{
public:
    enum class DitherMode : uint32_t
    {
        PerPixel2x2,
        PerPixel3x3,
        PerPixel4x4,
        PerJitter,
        RussianRoulette,
        Periodic,
        HashGrid,
        FractalDithering,
        BlueNoise3D,
        PerPixel2x2x2,
        DitherTemporalAA,
        SpatioTemporalBlueNoise, // Spatiotemporal Blue Noise Masks, Wolfe 2022
        SurfaceSpatioTemporalBlueNoise, // STBN attahed to surface (with HashGrid technique)
        Disabled = 0xff,
    };

#ifdef ENABLE_HASH_GRIDS
    FALCOR_ENUM_INFO(DitherMode, {
        { DitherMode::Disabled, "Disabled" },
        { DitherMode::PerPixel3x3, "STD 3x3" },
        // { DitherMode::PerPixel4x4, "PerPixel4x4" }, deprecated: not up to date
        // { DitherMode::PerPixel2x2x2, "PerPixel2x2x2" }, // deprecated: too much flicker
        { DitherMode::DitherTemporalAA, "DitherTemporalAA" },
        //{ DitherMode::PerJitter, "PerJitter" }, // deprecated
        { DitherMode::RussianRoulette, "RussianRoulette" },
        { DitherMode::HashGrid, "HashGrid" },
        { DitherMode::FractalDithering, "FractalDithering" },
        { DitherMode::PerPixel2x2, "STD 2x2" },
        // the implementation of those work, but they have bad results:
        { DitherMode::Periodic, "Periodic" },
        { DitherMode::SpatioTemporalBlueNoise, "SpatioTemporalBlueNoise" },
        { DitherMode::SurfaceSpatioTemporalBlueNoise, "SurfaceSpatioTemporalBlueNoise" },
        { DitherMode::BlueNoise3D, "BlueNoise3D" },
        
    });
#else
    FALCOR_ENUM_INFO(DitherMode, {
    { DitherMode::Disabled, "Disabled" },
    { DitherMode::PerPixel3x3, "STD 3x3" },
    // { DitherMode::PerPixel4x4, "PerPixel4x4" }, deprecated: not up to date
    // { DitherMode::PerPixel2x2x2, "PerPixel2x2x2" }, // deprecated: too much flicker
    { DitherMode::DitherTemporalAA, "DitherTemporalAA" },
    //{ DitherMode::PerJitter, "PerJitter" }, // deprecated
    { DitherMode::RussianRoulette, "RussianRoulette" },
    //{ DitherMode::PerPixel2x2, "STD 2x2" },
    // the implementation of those work, but they have bad results:
    //{ DitherMode::Periodic, "Periodic" },
    //{ DitherMode::BlueNoise3D, "BlueNoise3D" },
});
#endif


    enum class DitherPattern : uint32_t
    {
        Dither2x2,
        Dither4x4,
        Dither8x8,
    };

    FALCOR_ENUM_INFO(DitherPattern, {
       { DitherPattern::Dither2x2, "Dither2x2" },
       { DitherPattern::Dither4x4, "Dither4x4" },
       { DitherPattern::Dither8x8, "Dither8x8" },
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

    enum class NoisePattern : uint32_t
    {
        White,
        Blue,
        Bayer,
        BlueBayer,
        Poisson,
        Perlin,
        Blue64
    };

    FALCOR_ENUM_INFO(NoisePattern, {
        {NoisePattern::White, "White"},
        {NoisePattern::Blue, "Blue"},
        {NoisePattern::Bayer, "Bayer"},
        {NoisePattern::BlueBayer, "BlueBayer"},
        {NoisePattern::Poisson, "Poisson"},
        {NoisePattern::Perlin, "Perlin"},
        {NoisePattern::Blue64, "Blue64"}
    });

    enum class NoiseTopPattern : uint32_t
    {
        Disabled,
        StaticWhite,
        DynamicWhite,
        StaticBlue,
        DynamicBlue,
        StaticBayer,
        DynamicBayer,
        SurfaceWhite,
    };

#ifdef ENABLE_HASH_GRIDS
    FALCOR_ENUM_INFO(NoiseTopPattern, {
        {NoiseTopPattern::Disabled, "Disabled"},
        {NoiseTopPattern::StaticWhite, "StaticWhite"},
        {NoiseTopPattern::DynamicWhite, "DynamicWhite"},
        {NoiseTopPattern::StaticBlue, "StaticBlue"},
        {NoiseTopPattern::DynamicBlue, "DynamicBlue"},
        {NoiseTopPattern::StaticBayer, "StaticBayer"},
        {NoiseTopPattern::SurfaceWhite, "Surface"},
        //{NoiseTopPattern::DynamicBayer, "DynamicBayer"},
    });
#else
    FALCOR_ENUM_INFO(NoiseTopPattern, {
    {NoiseTopPattern::Disabled, "Disabled"},
    {NoiseTopPattern::StaticWhite, "StaticWhite"},
    //{NoiseTopPattern::DynamicWhite, "DynamicWhite"},
    {NoiseTopPattern::StaticBlue, "StaticBlue"},
    //{NoiseTopPattern::DynamicBlue, "DynamicBlue"},
    //{NoiseTopPattern::StaticBayer, "StaticBayer"},
    //{NoiseTopPattern::DynamicBayer, "DynamicBayer"},
        });
#endif

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

    FALCOR_PLUGIN_CLASS(DitherVBuffer, "DitherVBuffer", "VBuffer with Dithering options for transparency");

    enum class STBNNoise : uint32_t
    {
        Scalar,
        Vector1D,
    };

    FALCOR_ENUM_INFO(STBNNoise, {
        {STBNNoise::Scalar, "Scalar (Default)"},
        {STBNNoise::Vector1D, "Vector1D"},
    });

    static ref<DitherVBuffer> create(ref<Device> pDevice, const Properties& props) { return make_ref<DitherVBuffer>(pDevice, props); }

    DitherVBuffer(ref<Device> pDevice, const Properties& props);

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
    
    void setFractalDitherPattern(DitherPattern pattern);

    void setupProgram();
    // returns true if at least one material was whitelisted (or scene was invalid)
    bool updateWhitelistBuffer();
    void createNoisePattern();

    ref<Scene> mpScene;
    
    ref<RtProgram> mpProgram;
    ref<RtProgramVars> mpVars;
    ref<SampleGenerator> mpSampleGenerator;
    ref<Buffer> mpTransparencyWhitelist;
    ref<Buffer> mpPermutations3x3Buffer;

    uint mFrameCount = 0;

    ref<CPUSampleGenerator> mpSamplePattern;

    DitherMode mDitherMode = DitherMode::PerPixel3x3;
    bool mUseAlphaTextureLOD = false; // use lod for alpha lookups
    bool mUseTransparencyWhitelist = false;
    whitelist_t mTransparencyWhitelist;
    CoverageCorrection mCoverageCorrection = CoverageCorrection::DLSS;
    float mDLSSCorrectionStrength = 1.0;
    DitherPattern mFractalDitherPattern = DitherPattern::Dither8x8;
    float mGridScale = 0.5f;
    ObjectHashType mObjectHashType = ObjectHashType::Geometry;

    ref<Texture> mpFracDitherTex;
    ref<Texture> mpFracDitherRampTex;
    ref<Sampler> mpFracSampler;
    ref<Texture> mpNoiseTex;
    ref<Sampler> mpNoiseSampler;
    ref<Texture> mpBlueNoise3DTex;
    ref<Texture> mpBlueNoise64Tex;
    ref<Texture> mpBayer64Tex;
    ref<Texture> mpSpatioTemporalBlueNoiseTex;
    ref<Texture> mpSpatioTemporalBlueNoiseTex2;
    NoisePattern mNoisePattern = NoisePattern::Blue;
    NoiseTopPattern mNoiseTopPattern = NoiseTopPattern::StaticBlue;
    STBNNoise mSTBNNoise = STBNNoise::Scalar;
    bool mCullBackFaces = false;
    float mMinVisibility = 1.0f;
    bool mAlignMotionVectors = false; // align when using pixel grid techniques
    bool mRotatePattern = true; // rotate pattern when using pixel grid techniques
    bool mDitherTAAPermutations = true;

    RenderScale mRenderScale = RenderScale::Full;

    std::vector<int> mPermutations3x3Scores;
    std::vector<Gui::DropdownValue> mPermutations3x3Dropdown;
    uint32_t mPermutations3x3Score = 0; 
};

FALCOR_ENUM_REGISTER(DitherVBuffer::DitherMode);
FALCOR_ENUM_REGISTER(DitherVBuffer::CoverageCorrection);
FALCOR_ENUM_REGISTER(DitherVBuffer::DitherPattern);
FALCOR_ENUM_REGISTER(DitherVBuffer::NoisePattern);
FALCOR_ENUM_REGISTER(DitherVBuffer::ObjectHashType);
FALCOR_ENUM_REGISTER(DitherVBuffer::NoiseTopPattern);
FALCOR_ENUM_REGISTER(DitherVBuffer::RenderScale);
FALCOR_ENUM_REGISTER(DitherVBuffer::STBNNoise);
