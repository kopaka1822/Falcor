#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

// Modes
enum class ErrorOverlayMode : uint32_t
{
    None = 0,
    Input1 = 1,
    Input2 = 2,
    MSE = 3,
    FLIP = 4,
    NumElements = 5
};
FALCOR_ENUM_INFO(ErrorOverlayMode,
{
    { ErrorOverlayMode::None, "None" },
    { ErrorOverlayMode::Input1, "Input1" },
    { ErrorOverlayMode::Input2, "Input2" },
    { ErrorOverlayMode::MSE, "MSE" },
    { ErrorOverlayMode::FLIP, "FLIP" },
});

FALCOR_ENUM_REGISTER(ErrorOverlayMode);

END_NAMESPACE_FALCOR
