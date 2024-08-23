#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

//Debug Mask
enum class PathSMDebugModes : uint32_t
{
    LeakTracingMask = 0,
    LayeredVarianceLayers = 1,
    RaySMDiff = 2,
    // Shadow Map Modes
    ShadowMapFlag = 16,      //Bit for when the shadow map is set | 0b10000
    ShadowMap = 17,          //Shows Shadow Map
    ShadowMapAccess = 18,   //Shows Shadow Map with Access heat map
    ShadowMapRayDiff = 19,  //Shows Shadow Map with Ray diff heat map
    ReverseSM = 20,         //Shows Reverse SM
    RayNeededMask = 21,
    SparseShadowDepths = 22,
};
FALCOR_ENUM_INFO(PathSMDebugModes, {
    { PathSMDebugModes::LeakTracingMask, "LeakTracingMask" },
    { PathSMDebugModes::LayeredVarianceLayers, "LayeredVarianceLayers" },
    { PathSMDebugModes::RaySMDiff, "RaySMDifference" },
    { PathSMDebugModes::ShadowMap, "ShadowMap" },
    { PathSMDebugModes::ShadowMapAccess, "ShadowMapAccess" },
    { PathSMDebugModes::ShadowMapRayDiff, "ShadowMapRayDiff" },
    { PathSMDebugModes::ReverseSM, "ReverseSM" },
    { PathSMDebugModes::RayNeededMask, "RayNeededMask" },
    { PathSMDebugModes::SparseShadowDepths, "SparseShadowDepths" },
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
