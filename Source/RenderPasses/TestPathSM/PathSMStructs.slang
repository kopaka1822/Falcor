#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

//Debug Mask
enum class PathSMDebugModes : uint32_t
{
    LeakTracingMask = 0,
    RaySMDiff = 1
};

FALCOR_ENUM_INFO(PathSMDebugModes, {
    {PathSMDebugModes::LeakTracingMask, "LeakTracingMask"},
    {PathSMDebugModes::RaySMDiff, "RaySMDifference"},
});
FALCOR_ENUM_REGISTER(PathSMDebugModes);

END_NAMESPACE_FALCOR
