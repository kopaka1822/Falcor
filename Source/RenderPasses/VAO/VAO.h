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
#include "VAOData.slang"
#include "DepthMode.h"
#include "Core/Pass/FullScreenPass.h"
#include "RenderGraph/RenderPass.h"


using namespace Falcor;

class VAO : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(VAO, "VAO", "Screen-space volumetric ambient occlusion");

    enum class SampleDistribution : uint32_t
    {
        Random,
        VanDerCorput,
        Poisson,
        Triangle
    };

    /** Create a new render pass object.
        \param[in] pDevice GPU device.
        \param[in] dict Dictionary of serialized parameters.
        \return A new object, or an exception is thrown if creation failed.
    */
    static ref<VAO> create(ref<Device> pDevice, const Dictionary& dict);

    virtual Dictionary getScriptingDictionary() override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    // indicates that ML depths will be saved in the next render iteration
    void saveDepths();
    VAO(ref<Device> pDevice);
private:
    void setNoiseTexture();
    void setKernel();
    std::vector<float> getSphereHeights() const;

    VAOData mData;
    bool mDirty = true;

    bool mEnabled = true;
    DepthMode mDepthMode = DepthMode::Raytraced;
    ref<Fbo> mpAOFbo;

    ref<Sampler> mpNoiseSampler;
    ref<Texture> mpNoiseTexture;

    ref<Sampler> mpTextureSampler;
    SampleDistribution mHemisphereDistribution = SampleDistribution::VanDerCorput;

    ref<FullScreenPass> mpSSAOPass;

    ref<Scene> mpScene;
    int mGuardBand = 64;
    bool mClearTexture = true;
    uint32_t mKernelSize = 8;

    bool mSaveDepths = false;
    bool mPreventDarkHalos = true;
    bool mIsTraining = true;
    int mTrainingIndex = 0;
};

inline void VAO::saveDepths()
{
    mSaveDepths = true;
}
