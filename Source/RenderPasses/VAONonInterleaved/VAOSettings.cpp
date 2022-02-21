#include "VAOSettings.h"

#include <glm/gtc/random.hpp>

namespace
{
    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kUseRays = "useRays";
    const std::string kExponent = "exponent";
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
        else if (key == kDepthMode) mDepthMode = value;
        else if (key == kUseRays) mUseRays = value;
        else if (key == kExponent) mData.exponent = value;
        else logWarning("Unknown field '" + key + "' in a VAONonInterleaved dictionary");
    }
}

Dictionary VAOSettings::getScriptingDictionary()
{
    Dictionary d;
    d[kRadius] = mData.radius;
    d[kDepthMode] = mDepthMode;
    d[kUseRays] = mUseRays;
    d[kExponent] = mData.exponent;
    return d;
}

const Gui::DropdownList kDepthModeDropdown =
{
    { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
    { (uint32_t)DepthMode::DualDepth, "DualDepth" },
    { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
};

void VAOSettings::renderUI(Gui::Widgets& widget)
{
    // use this as a synchronization point to mark things dirty
    mDirty = false;
    mReset = false;

    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    uint32_t depthMode = (uint32_t)mDepthMode;
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode)) {
        mDepthMode = (DepthMode)depthMode;
        mReset = true;
    }

    if (widget.checkbox("Use Rays", mUseRays)) mReset = true; // changed defines

    if (widget.var("Sample Radius", mData.radius, 0.01f, FLT_MAX, 0.01f)) mDirty = true;

    if (widget.slider("Power Exponent", mData.exponent, 1.0f, 4.0f)) mDirty = true;
}

Texture::SharedPtr VAOSettings::genNoiseTexture()
{
    std::vector<uint16_t> data;
    data.resize(16u);

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < 16u; i++)
    {
        // Random directions on the XY plane
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        data[i] = uint16_t(glm::packSnorm4x8(float4(sin(theta), cos(theta), 0.0f, 0.0f)));
    }

    return Texture::create2D(4u, 4u, ResourceFormat::RG8Snorm, 1, 1, data.data());
}
