#include "VAOSettings.h"

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
