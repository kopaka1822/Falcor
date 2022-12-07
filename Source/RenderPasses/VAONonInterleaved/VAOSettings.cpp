#include "VAOSettings.h"

#include <glm/gtc/random.hpp>

namespace
{
    const std::string kRadius = "radius";
    const std::string kGuardBand = "guardBand";
    const std::string kPrimaryDepthMode = "primaryDepthMode";
    const std::string kSecondaryDepthMode = "secondaryDepthMode";
    const std::string kExponent = "exponent";
    const std::string kUseRayPipeline = "rayPipeline";
    const std::string kThickness = "thickness";
}

VAOSettings& VAOSettings::get()
{
    static VAOSettings i;
    return i;
}

void VAOSettings::updateFromDict(const Dictionary& dict)
{
    for (const auto& [key, value] : dict)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kPrimaryDepthMode) mPrimaryDepthMode = value;
        else if (key == kSecondaryDepthMode) mSecondaryDepthMode = value;
        else if (key == kExponent) mData.exponent = value;
        else if (key == kUseRayPipeline) mUseRayPipeline = value;
        else if (key == kGuardBand) mGuardBand = value;
        else if (key == kThickness) mData.thickness = value;
        else logWarning("Unknown field '" + key + "' in a VAONonInterleaved dictionary");
    }
}

Dictionary VAOSettings::getScriptingDictionary()
{
    Dictionary d;
    d[kRadius] = mData.radius;
    d[kPrimaryDepthMode] = mPrimaryDepthMode;
    d[kSecondaryDepthMode] = mSecondaryDepthMode;
    d[kExponent] = mData.exponent;
    d[kUseRayPipeline] = mUseRayPipeline;
    d[kGuardBand] = mGuardBand;
    d[kThickness] = mData.thickness;
    return d;
}

const Gui::DropdownList kPrimaryDepthModeDropdown =
{
    { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
    { (uint32_t)DepthMode::DualDepth, "DualDepth" },
};

const Gui::DropdownList kSecondaryDepthModeDropdown =
{
    { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
    { (uint32_t)DepthMode::Raytraced, "Raytraced" },
};

void VAOSettings::renderUI(Gui::Widgets& widget)
{
    // use this as a synchronization point to mark things dirty
    mDirty = false;
    mReset = false;

    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256))
        mDirty = true;

    uint32_t primaryDepthMode = (uint32_t)mPrimaryDepthMode;
    if (widget.dropdown("Primary Depth Mode", kPrimaryDepthModeDropdown, primaryDepthMode)) {
        mPrimaryDepthMode = (DepthMode)primaryDepthMode;
        mReset = true;
    }

    uint32_t secondaryDepthMode = (uint32_t)mSecondaryDepthMode;
    if (widget.dropdown("Secondary Depth Mode", kSecondaryDepthModeDropdown, secondaryDepthMode)) {
        mSecondaryDepthMode = (DepthMode)secondaryDepthMode;
        mReset = true;
    }

    if (widget.checkbox("Ray Pipeline", mUseRayPipeline)) mReset = true;

    if (widget.var("Sample Radius", mData.radius, 0.01f, FLT_MAX, 0.01f)) mDirty = true;

    if (widget.var("Thickness", mData.thickness, 0.0f, 1.0f, 0.1f)) {
        mDirty = true;
        mData.exponent = glm::mix(1.6f, 1.0f, mData.thickness);
    }

    if (widget.var("Power Exponent", mData.exponent, 1.0f, 4.0f, 0.1f)) mDirty = true;
}

Texture::SharedPtr VAOSettings::genNoiseTexture()
{
    static const int NOISE_SIZE = 4;
    std::vector<uint8_t> data;
    data.resize(NOISE_SIZE * NOISE_SIZE);

    // https://en.wikipedia.org/wiki/Ordered_dithering
    const float ditherValues[] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < data.size(); i++)
    {
        data[i] = uint8_t(ditherValues[i] / 16.0f * 255.0f);
    }

    return Texture::create2D(NOISE_SIZE, NOISE_SIZE, ResourceFormat::R8Unorm, 1, 1, data.data());
}
