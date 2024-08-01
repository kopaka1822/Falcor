#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

// Modes
enum class ErrorOverlayMode : uint32_t
{
    None = 0,
    Image = 1,
    Reference = 2,
    MSE = 3,
    FLIP = 4,
    NumElements = 5
};
FALCOR_ENUM_INFO(ErrorOverlayMode,
{
    { ErrorOverlayMode::None, "None" },
    { ErrorOverlayMode::Image, "Image" },
    { ErrorOverlayMode::Reference, "Reference" },
    { ErrorOverlayMode::MSE, "MSE" },
    { ErrorOverlayMode::FLIP, "FLIP" },
});

FALCOR_ENUM_REGISTER(ErrorOverlayMode);

END_NAMESPACE_FALCOR
