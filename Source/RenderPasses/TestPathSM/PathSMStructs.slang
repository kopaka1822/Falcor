#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

//Debug Mask
enum class PathSMDebugModes : uint32_t
{
    LeakTracingMask = 0,
    LeakTracingMaskPerBounce = 1,
    RaySMDiff = 2,
    PathLengthValidLight = 3,
    //Shadow Map Modes
    ShadowMapFlag = 8,      //Bit for when the shadow map is set | 0b1000
    ShadowMap = 9,          //Shows Shadow Map
    ShadowMapAccess = 10,   //Shows Shadow Map with Access heat map
    ShadowMapRayDiff = 11,  //Shows Shadow Map with Ray diff heat map
};
FALCOR_ENUM_INFO(PathSMDebugModes, {
    { PathSMDebugModes::LeakTracingMask, "LeakTracingMask" },
    { PathSMDebugModes::LeakTracingMaskPerBounce, "LeakTracingMaskPerBounce" },
    { PathSMDebugModes::RaySMDiff, "RaySMDifference" },
    { PathSMDebugModes::PathLengthValidLight, "PathLengthValidLight"},
    {PathSMDebugModes::ShadowMap, "ShadowMap"},
    {PathSMDebugModes::ShadowMapAccess, "ShadowMapAccess"},
    {PathSMDebugModes::ShadowMapRayDiff, "ShadowMapRayDiff"},
});
FALCOR_ENUM_REGISTER(PathSMDebugModes);

enum class PathSMLightSampleMode : uint32_t
{
    Uniform = 0,
    RIS = 1,
};
FALCOR_ENUM_INFO(PathSMLightSampleMode, {
    { PathSMLightSampleMode::Uniform, "Uniform" },
    { PathSMLightSampleMode::RIS, "RIS" },
});
FALCOR_ENUM_REGISTER(PathSMLightSampleMode);

END_NAMESPACE_FALCOR
