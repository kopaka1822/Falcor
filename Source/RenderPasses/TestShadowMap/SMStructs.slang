#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

//Debug Mask
enum class PathSMDebugModes : uint32_t
{
    LeakTracingMask = 0,
    RaySMDiff = 2,
    //Shadow Map Modes
    ShadowMapFlag = 8,      //Bit for when the shadow map is set | 0b1000
    ShadowMap = 9,          //Shows Shadow Map
    ShadowMapAccess = 10,   //Shows Shadow Map with Access heat map
    ShadowMapRayDiff = 11,  //Shows Shadow Map with Ray diff heat map
    ReverseSM = 12,         //Shows Reverse SM
    RayNeededMask = 13,
};
FALCOR_ENUM_INFO(PathSMDebugModes, {
    { PathSMDebugModes::LeakTracingMask, "LeakTracingMask" },
    { PathSMDebugModes::RaySMDiff, "RaySMDifference" },
    {PathSMDebugModes::ShadowMap, "ShadowMap"},
    {PathSMDebugModes::ShadowMapAccess, "ShadowMapAccess"},
    {PathSMDebugModes::ShadowMapRayDiff, "ShadowMapRayDiff"},
    {PathSMDebugModes::ReverseSM, "ReverseSM"},
     {PathSMDebugModes::RayNeededMask, "RayNeededMask"},
});
FALCOR_ENUM_REGISTER(PathSMDebugModes);

enum class SMLightSampleMode : uint32_t
{
    Uniform = 0,
    RIS = 1,
    All = 2,
};
FALCOR_ENUM_INFO(SMLightSampleMode, {
    { SMLightSampleMode::Uniform, "Uniform" },
    { SMLightSampleMode::RIS, "RIS" },
    { SMLightSampleMode::All, "All" },
});
FALCOR_ENUM_REGISTER(SMLightSampleMode);

enum class SMFormat : uint32_t
{
    Float = 0,
    Float2 = 1,
    Float4 = 2,
};

FALCOR_ENUM_INFO(SMFormat, {
    { SMFormat::Float, "Float" },
    { SMFormat::Float2, "Float2" },
    { SMFormat::Float4, "Float4" },
});
FALCOR_ENUM_REGISTER(SMFormat);

END_NAMESPACE_FALCOR
