#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

enum class LightTypeSM : uint32_t
    {
        NotSupported = 0x00,
        Point = 0x01,
        Spot = 0x02,
        Directional = 0x04,
    };
FALCOR_ENUM_INFO(LightTypeSM, {
    { LightTypeSM::NotSupported, "NotSupported" },
    { LightTypeSM::Point, "Point" },
    { LightTypeSM::Spot, "Spot" },
    { LightTypeSM::Directional, "Directional" },
});
FALCOR_ENUM_REGISTER(LightTypeSM);

END_NAMESPACE_FALCOR
