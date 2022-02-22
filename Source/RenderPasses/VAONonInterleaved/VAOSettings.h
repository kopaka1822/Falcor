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
        float2 resolution = float2(x, y);
        mData.resolution = resolution;
        mData.invResolution = float2(1.0f) / resolution;
        mData.noiseScale = resolution / 4.0f; // noise texture is 4x4 resolution
        mDirty = true;
    }

    bool getEnabled() const { return mEnabled; }
    VAOData getData() const { return mData; }
    DepthMode getPrimaryDepthMode() const { return mPrimaryDepthMode; }
    DepthMode getSecondaryDepthMode() const { return mSecondaryDepthMode; }

    static Texture::SharedPtr genNoiseTexture();
private: 
    VAOSettings() = default;

    bool mEnabled = true;

    VAOData mData;
    bool mDirty = true;
    bool mReset = false;
    DepthMode mPrimaryDepthMode = DepthMode::DualDepth;
    DepthMode mSecondaryDepthMode = DepthMode::StochasticDepth;
};
