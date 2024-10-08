#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR


enum class TPTLightSampleMode : uint32_t
{
    Uniform = 0,
    RIS = 1,
};
FALCOR_ENUM_INFO(TPTLightSampleMode, {
    { TPTLightSampleMode::Uniform, "Uniform" },
    { TPTLightSampleMode::RIS, "RIS" },
});
FALCOR_ENUM_REGISTER(TPTLightSampleMode);

END_NAMESPACE_FALCOR
