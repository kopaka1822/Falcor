#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

//Debug Mask
enum class PathSMDebugModes : uint32_t
{
    LeakTracingMask = 0,
    RaySMDiff = 1,
    //Shadow Map Modes
    ShadowMapFlag = 8,      //Bit for when the shadow map is set | 0b1000
    ShadowMap = 9,          //Shows Shadow Map
    ShadowMapAccess = 10,   //Shows Shadow Map with Access heat map
    ShadowMapRayDiff = 11,  //Shows Shadow Map with Ray diff heat map
};

FALCOR_ENUM_INFO(PathSMDebugModes, {
    {PathSMDebugModes::LeakTracingMask, "LeakTracingMask"},
    {PathSMDebugModes::RaySMDiff, "RaySMDifference"},
    {PathSMDebugModes::ShadowMap, "ShadowMap"},
    {PathSMDebugModes::ShadowMapAccess, "ShadowMapAccess"},
    {PathSMDebugModes::ShadowMapRayDiff, "ShadowMapRayDiff"},
});
FALCOR_ENUM_REGISTER(PathSMDebugModes);

END_NAMESPACE_FALCOR
