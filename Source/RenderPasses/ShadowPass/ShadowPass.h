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
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Rendering/ShadowMaps/ShadowMap.h"
#include "ShadowPassData.slang"
#include "LTTMaskSamplePatterns.slang"

using namespace Falcor;

class ShadowPass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ShadowPass, "ShadowPass", "An shadow pass for analytic shadow using shadow maps");

    static ref<ShadowPass> create(ref<Device> pDevice, const Properties& props) { return make_ref<ShadowPass>(pDevice, props); }

    ShadowPass(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {};
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    //Dispatches the shaders
    void shade(RenderContext* pRenderContext, const RenderData& renderData);

    //Hybrid mask functions
    void handleHybridMaskData(RenderContext* pRenderContext, uint2 screenDims, uint numLights); ///< Handles Textures for the hybrid mask
    DefineList hybridMaskDefines();
    void setHybridMaskVars(ShaderVar& var, const uint frameCount);
    bool lttMaskUI(Gui::Widgets& widget);

    // Internal state
    ref<Scene> mpScene;                     ///< Current scene.
    std::unique_ptr<ShadowMap> mpShadowMap; ///< Shadow Map
    ref<SampleGenerator> mpSampleGenerator; ///< GPU sample generator.
    uint mFrameCount = 0;
    bool mShadowModeChanged = false;

    //Configuration
    bool mUseAlphaTest = true; ///< Alpha Test for ray tracing
    bool mCopyAlphaSettingsFromSM = true;   ///< Copies settings when the alpha test should be used from the shadow map
    float mUseAlphaTestUntilDistance = 1000000.f;
    bool mShadowOnly = false;               ///< Only output shadow
    float mAmbientFactor = 0.1f; //<Ambient light factor
    float mEnvMapFactor = 0.3f; //< Env Map factor
    float mEmissiveFactor = 1.f; //< Emissive Factor
    uint mDebugMode = 3;            //< Mode for the debug view
    bool mOptionsChanged = false;
    SPShadowMode mShadowMode = SPShadowMode::LeakTracing;
    bool mEnableHybridRTBlend = true;
    float2 mHybridRTBlend = float2(100000.f, 1.f);
    float mHybridRTBlendDistancePercentage = 0.05f;
    bool mFullyTracedCascadedLevelsEnabled = false;
    
    //Hybrid Mask
    LTTMaskSamplePatterns mHybridMaskSamplePattern = LTTMaskSamplePatterns::Gather;

    bool mHybridMaskFirstFrame = false; //< Marks if this is the first frame for the hybrid mask and all values are invalid
    bool mHybridUseTemporalDepthTest = false;
    float mHybridTemporalDepthTestPercentage = 0.1f;
    bool mClearHybridMask = false;
    bool mEnableHybridMask = true;
    bool mHybridMaskRemoveRays = true;
    bool mUseHybridMaskRemoveRaysDistance = false;
    uint mHybridMaskRemoveRaysGreaterAsDistanceMode = 2; // UI mode
    float mHybridMaskRemoveRaysGreaterAsDistance = 20.f;
    uint mHybridMaskRemoveRaysSmallerAsDistanceMode = 0; // UI mode
    float mHybridMaskRemoveRaysSmallerAsDistance = 7.f;
    bool mHybridMaskExpandRays = true;
    bool mUseHybridMaskExpandRaysMaxDistance = true;
    uint mHybridMaskExpandRaysMaxDistanceMode = 3;      //UI mode
    float mHybridMaskExpandRaysMaxDistance = 70.f;
    bool mHybridMaskDisableDynamicGeometryCheck = false;   //Disable the geometry check
    bool mHybridUseRayWhenOutsideMask = true;
    uint mLTTDebugLight = 0;

    ref<Texture> mpHybridMask[2];   //Ping Pong temporal hybrid mask
    ref<Sampler> mpHybridSampler;   //Sampler for the hybrid mask
    ref<Texture> mpPrevDepth[2];       //Previous depth

    // Ray tracing program.
    struct
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    } mShadowTracer;
};
