#pragma once
#include "Falcor.h"
#include "Utils/Properties.h"

using namespace Falcor;

class FALCOR_API ShadowSettings
{
public:
    enum class RayConeShadow
    {
        Disabled,
        RawAlpha,
        AlphaTest,
        Saturated,
        OnlyOpaque
    };

    FALCOR_ENUM_INFO(
        RayConeShadow,
        {
            {RayConeShadow::Disabled, "Disabled" },
            { RayConeShadow::RawAlpha, "RawAlpha" },
            { RayConeShadow::AlphaTest, "AlphaTest" },
            { RayConeShadow::Saturated, "Saturated" },
            { RayConeShadow::OnlyOpaque, "OnlyOpaque" }
        }
    );

    enum class ShadowTechnique
    {
        DeepShadow,
        StochasticShadow,
        Dither2x2,
        Dither3x3,
        Dither4x4,
        SpatioTemporalBlueNoise
    };

    FALCOR_ENUM_INFO(
        ShadowTechnique,
        {
            { ShadowTechnique::DeepShadow, "Deep Shadow" },
            { ShadowTechnique::StochasticShadow, "Stochastic Shadow" },
            { ShadowTechnique::Dither2x2, "Dither 2x2" },
            { ShadowTechnique::Dither3x3, "Dither 3x3" },
            { ShadowTechnique::Dither4x4, "Dither 4x4" },
            { ShadowTechnique::SpatioTemporalBlueNoise, "STBN" }
        }
    );

    static ShadowSettings& get();

    void loadFromProperties(const Properties& props);
    Properties getProperties() const;
    void updateShaderVar(ref<Device> pDevice, ShaderVar& vars, uint frameIndex);
    void renderUI(Gui::Widgets& widget);
    DefineList getShaderDefines(Scene& scene, uint2 frameDim) const;
private:
    ShadowSettings();

    float mPointLightClip = 0.2f;
    bool mRayCones = false;
    bool mDiminishBorder = true;
    float mLodBias = 0.0f;
    RayConeShadow mRayConeShadow = RayConeShadow::RawAlpha;
    ShadowTechnique mShadowTechnique = ShadowTechnique::DeepShadow;

    ref<Sampler> mpSampler;
    ref<Texture> mpBlueNoise64Tex;
    ref<Texture> mpSpatioTemporalBlueNoiseTex;
    bool mRotatePattern = true;
    bool mCorrectCoverage = false;
};

FALCOR_ENUM_REGISTER(ShadowSettings::RayConeShadow);
FALCOR_ENUM_REGISTER(ShadowSettings::ShadowTechnique);
