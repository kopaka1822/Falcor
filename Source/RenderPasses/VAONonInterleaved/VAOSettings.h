#pragma once
#include "Falcor.h"
#include "../SSAO/DepthMode.h"
#include "VAOData.slang"

using namespace Falcor;

class VAOSettings // singleton of the VAO settings
{
public:
    static VAOSettings& get();

    void updateFromDict(const Dictionary& dict);
    Dictionary getScriptingDictionary();

    void renderUI(Gui::Widgets& widget);

    bool IsDirty() const { return mDirty; }
    bool IsReset() const { return mReset; }

    void setResolution(uint x, uint y)
    {
        mData.resolution = float2(x, y);
        mData.invResolution = float2(1.0f) / mData.resolution;
        mData.quarterResolution = float2((x + 3u) / 4u, (y + 3u) / 4u);
        mData.invQuarterResolution = float2(1.0f) / mData.quarterResolution;
;       mData.noiseScale = mData.resolution / 4.0f; // noise texture is 4x4 resolution
        mDirty = true;
    }

    bool getEnabled() const { return mEnabled; }
    VAOData getData() const { return mData; }
    DepthMode getPrimaryDepthMode() const { return mPrimaryDepthMode; }
    DepthMode getSecondaryDepthMode() const { return mSecondaryDepthMode; }
    bool getRayPipeline() const { return mUseRayPipeline; }
    int getGuardBand() const { return mGuardBand; }

    static Texture::SharedPtr genNoiseTexture();
    static std::vector<float2> genNoiseTextureCPU();
private: 
    VAOSettings() = default;

    bool mEnabled = true;

    VAOData mData;
    bool mDirty = true;
    bool mReset = false;
    int mGuardBand = 64;
    DepthMode mPrimaryDepthMode = DepthMode::DualDepth;
    DepthMode mSecondaryDepthMode = DepthMode::StochasticDepth;
    bool mUseRayPipeline = false;
};
