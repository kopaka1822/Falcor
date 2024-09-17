#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

struct RTAOData
{
    float radius = 0.5f;
    uint spp = 1;
    float normalScale = 0.01f;

};

END_NAMESPACE_FALCOR
