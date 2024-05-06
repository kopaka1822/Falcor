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

/*
    Wrapper Module for Shadow Maps, which allow ShadowMaps to be easily integrated into every Render Pass.
*/
namespace Falcor
{
class RenderContext;

class FALCOR_API ShadowMapOracle
{
public:
    ShadowMapOracle(ref<Device> pDevice) : mpDevice {pDevice}{}

    // Gets the parameter block needed for shader usage
    ref<ParameterBlock> getParameterBlock() const { return mpShadowMapOracleParameterBlock; }

    DefineList getDefines() const;
    void createParameterBlock(ref<ComputePass> reflectProgram);
    void setShaderData(const uint2 frameDim, const CameraData& cameraData);
    bool renderUI(Gui::Widgets& widget);
    void resetBuffers() { mpNormalizedPixelSize.reset(); }
    void bindShaderData(ShaderVar rootVar) { rootVar["gShadowMapOracle"] = mpShadowMapOracleParameterBlock; }
    void handleNormalizedPixelSizeBuffer(
        ref<Scene> pScene,
        uint shadowMapSize,
        uint shadowCubeSize,
        uint shadowCascSize,
        uint cascadedLevelCount,
        std::vector<float2>& cascadedWidthHeight
    );

private:
    // Get Normalized pixel size used in oracle function
    float getNormalizedPixelSize(uint2 frameDim, float fovY, float aspect);
    float getNormalizedPixelSizeOrtho(uint2 frameDim, float width, float height); // Ortho case

    ref<Device> mpDevice;
    ref<ParameterBlock> mpShadowMapOracleParameterBlock; // Parameter Block
    ref<Buffer> mpNormalizedPixelSize;                   // Buffer with the normalized pixel size for each ShadowMap

    uint2 mNPSOffsets = uint2(0); // x = idx first spot; y = idx first cascade

    // Oracle
    bool mUseSMOracle = true; ///< Enables Shadow Map Oracle function
    bool mUseOracleDistFactor = true;  ///< Enables a lobe distance factor that is used in the oracle function
    OracleDistFunction mOracleDistanceFunctionMode = OracleDistFunction::None; // Distance functions used in Oracle
    bool mOracleAddRays = false; //Additionally adds rays if the oracle is over the upper bound value

    float mOracleCompaireValue = 1.f; ///< Compaire Value for the Oracle test. Tested against ShadowMapArea/CameraPixelArea.
    float mOracleCompaireUpperBound = 32.f; ///< Hybrid mode only. If oracle is over this value, shoot an ray

    bool mOracleIgnoreDirect = true; ///< Skip the Oracle function for direct hits and very specular (under the rougness thrs. below)
    float mOracleIgnoreDirectRoughness = 0.085f; ///< Roughness threshold for which hits are counted as very specular
};
} // namespace Falcor
