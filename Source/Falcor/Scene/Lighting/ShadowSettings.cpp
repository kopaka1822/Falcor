#include "ShadowSettings.h"

namespace
{

}

ShadowSettings FALCOR_API_EXPORT & ShadowSettings::get()
{
    static ShadowSettings instance;
    return instance;
}

void ShadowSettings::loadFromProperties(const Properties& props)
{
    mRayCones = props.get("RayCones", mRayCones);
    mDiminishBorder = props.get("DiminishBorder", mDiminishBorder);
    mRayConeShadow = props.get("RayConeShadow", mRayConeShadow);
    mPointLightClip = props.get("PointLightClip", mPointLightClip);
}

Properties ShadowSettings::getProperties() const
{
    Properties d;
    d["RayCones"] = mRayCones;
    d["DiminishBorder"] = mDiminishBorder;
    d["RayConeShadow"] = mRayConeShadow;
    d["PointLightClip"] = mPointLightClip;
    return d;
}

void ShadowSettings::updateShaderVar(ref<Device> pDevice, ShaderVar& vars)
{
    if(!mpSampler)
    {
        Sampler::Desc d;
        d.setAddressingMode(Sampler::AddressMode::Border, Sampler::AddressMode::Border, Sampler::AddressMode::Border);
        d.setBorderColor(float4(0.0f));
        mpSampler = Sampler::create(pDevice, d);
    }
    vars["gSoftShadowSampler"] = mpSampler;

    vars["ShadowSettingBuffer"]["gPointLightClip"] = mPointLightClip;
    vars["ShadowSettingBuffer"]["gShadowLodBias"] = mLodBias;
    vars["ShadowSettingBuffer"]["gDiminishBorder"] = mDiminishBorder;
}

void ShadowSettings::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Ray Cones", mRayCones);
    if(mRayCones)
    {
        widget.checkbox("Diminish Border", mDiminishBorder);
        widget.tooltip("Blends out texture fetches near the border to prevent showing the quadratic shape of the texture in higher mip levels");
    }

    widget.dropdown("Shadow Type", mRayConeShadow);
    widget.var("LOD Bias", mLodBias, -16.0f, 16.0f, 0.5f);
    widget.tooltip("Bias applied to textures for the shadow alpha test.");
    widget.var("Point Light Clip", mPointLightClip, 0.0f);
    widget.tooltip("Radius around the point light source where intersections will be ignored. Useful if the point light is inside an object => can be used to ignore intersection with any geometry within that radius");
}

DefineList ShadowSettings::getShaderDefines(Scene& scene, uint2 frameDim) const
{
    DefineList defines;
    auto rayConeSpread = scene.getCamera()->computeScreenSpacePixelSpreadAngle(frameDim.y);
    defines.add("RAY_CONE_SPREAD", std::to_string(rayConeSpread));
    defines.add("USE_RAYCONES", mRayCones ? "1" : "0");
    defines.add("RAY_CONE_SHADOW", std::to_string(int(mRayConeShadow)));
    return defines;
}

ShadowSettings::ShadowSettings()
{

}
