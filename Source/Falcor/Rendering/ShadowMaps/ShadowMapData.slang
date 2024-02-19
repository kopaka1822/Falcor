#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

enum class LightTypeSM : uint32_t
    {
        NotSupported = 0,
        Point = 1,
        Spot = 2,
        Directional = 3,
    };
FALCOR_ENUM_INFO(LightTypeSM, {
    { LightTypeSM::NotSupported, "NotSupported" },
    { LightTypeSM::Point, "Point" },
    { LightTypeSM::Spot, "Spot" },
    { LightTypeSM::Directional, "Directional" },
});
FALCOR_ENUM_REGISTER(LightTypeSM);

enum class OracleDistFunction : uint32_t
{
        None = 0,
        RoughnessLinear = 1,
        RoughnessSquare = 2,
        Lobe = 3,
        DiffSpecProp = 4,
};
FALCOR_ENUM_INFO(OracleDistFunction, {
    { OracleDistFunction::None, "None" },
    { OracleDistFunction::RoughnessLinear, "RoughnessLinear" },
    { OracleDistFunction::RoughnessSquare, "RoughnessSquare" },
    { OracleDistFunction::Lobe, "Lobe" },
    { OracleDistFunction::DiffSpecProp, "DiffSpecProp" },
});
FALCOR_ENUM_REGISTER(OracleDistFunction);

enum class ShadowMapType : uint32_t
{
        ShadowMap = 0,
        Variance = 1,
        Exponential = 2,
        ExponentialVariance = 3,
        MSMHamburger = 4,
        MSMHausdorff = 5,
        SDVariance = 6,
        SDExponentialVariance = 7,
        SDMSM = 8,
};
FALCOR_ENUM_INFO(ShadowMapType, {
    { ShadowMapType::ShadowMap, "ShadowMap" },
    { ShadowMapType::Variance, "Variance" },
    { ShadowMapType::Exponential, "Exponential" },
    { ShadowMapType::ExponentialVariance, "ExponentialVariance" },
    { ShadowMapType::MSMHamburger, "MSMHamburger" },
    { ShadowMapType::MSMHausdorff, "MSMHausdorff" },
    { ShadowMapType::SDVariance, "SDVariance" },
    { ShadowMapType::SDExponentialVariance, "SDExponentialVariance" },
    { ShadowMapType::SDMSM, "SDMSM" },
});
FALCOR_ENUM_REGISTER(ShadowMapType);

struct SMLightData{
    float3 posW;
    uint type;
    float3 dirW;
    uint index;
};

END_NAMESPACE_FALCOR
