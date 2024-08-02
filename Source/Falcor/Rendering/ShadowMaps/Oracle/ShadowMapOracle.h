/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#pragma once
#include "../ShadowMapData.slang"
#include "../ShadowMap.h"
/*
    Wrapper Module for Shadow Maps, which allow ShadowMaps to be easily integrated into every Render Pass.
*/
namespace Falcor
{
class RenderContext;

class FALCOR_API ShadowMapOracle
{
public:
    ShadowMapOracle(bool enable = false) { mUseSMOracle = enable; }

    DefineList getDefines() const;
    void setVars(ShaderVar& var);
    bool renderUI(Gui::Widgets& widget);
    // Updates Oracle. Should be called once a frame. Returns if shader vars of the pass using the oracle should be reset
    bool update(ref<Scene> pScene, const uint2 frameDim, ShadowMap* pShadowMap);
    //Updates Oracle. Should be called once a frame. Returns if shader vars of the pass using the oracle should be reset
    bool update(
        ref<Scene> pScene,
        const uint2 frameDim,
        uint shadowMapSize,
        uint shadowCubeSize,
        uint shadowCascSize,
        uint cascadedLevelCount,
        std::vector<float2>& cascadedWidthHeight
    );
    bool isEnabled() { return mUseSMOracle; }

private:
    // Get Normalized pixel size used in oracle function
    float getNormalizedPixelSize(uint2 frameDim, float fovY, float aspect);
    float getNormalizedPixelSizeOrtho(uint2 frameDim, float width, float height); // Ortho case

    static constexpr float kNPSFactor = 1000.f; //For numerical stability

    uint2 mNPSOffsets = uint2(0); // x = idx first spot; y = idx first cascade
    float mCameraNPS = 0.f;
    std::vector<float> mLightNPS;

    // Oracle
    bool mUseSMOracle = true; ///< Enables Shadow Map Oracle function
    bool mUseOracleDistFactor = true;  ///< Enables a lobe distance factor that is used in the oracle function
    OracleDistFunction mOracleDistanceFunctionMode = OracleDistFunction::None; // Distance functions used in Oracle
    bool mOracleAddRays = false; //Additionally adds rays if the oracle is over the upper bound value
    bool mUseLeakTracing = true; //Uses Leak Tracing when shadow map should be used

    float mOracleCompaireValue = 0.25f; ///< Compaire Value for the Oracle test. Tested against ShadowMapArea/CameraPixelArea.
    float mOracleCompaireUpperBound = 32.f; ///< Hybrid mode only. If oracle is over this value, shoot an ray

    bool mOracleIgnoreDirect = true; ///< Skip the Oracle function for direct hits and very specular (under the rougness thrs. below)
    float mOracleIgnoreDirectRoughness = 0.085f; ///< Roughness threshold for which hits are counted as very specular

    //Runtime checks
    uint4 mShadowMapSizes = uint4(0); // Store changes the NPS was build with (Spot|Point|Cascaded|CascadedLevel)
};
} // namespace Falcor
