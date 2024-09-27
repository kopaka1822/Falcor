#pragma once

namespace Falcor
{
    enum class DepthMode : uint32_t
    {
        SingleDepth,
        DualDepth,
        StochasticDepth,
        Raytraced,
        RaytracedDualDepth,
    };

    FALCOR_ENUM_INFO(
        DepthMode,
        {
            {DepthMode::SingleDepth, "SingleDepth"},
            {DepthMode::DualDepth, "DualDepth"},
            {DepthMode::StochasticDepth, "StochasticDepth"},
            {DepthMode::Raytraced, "Raytraced"},
            {DepthMode::RaytracedDualDepth, "RaytracedDualDepth"}
        }
    );

    FALCOR_ENUM_REGISTER(DepthMode);
}
