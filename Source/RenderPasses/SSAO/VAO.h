/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
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
#include "SSAOData.slang"
#include "../Utils/GaussianBlur/GaussianBlur.h"
#include "DepthMode.h"

using namespace Falcor;

class VAO : public RenderPass
{
public:
    using SharedPtr = std::shared_ptr<VAO>;

    enum class SampleDistribution : uint32_t
    {
        Random,
        VanDerCorput,
        Poisson
    };

    static SharedPtr create(RenderContext* pRenderContext = nullptr, const Dictionary& dict = {});

    static const Info kInfo;
    virtual Dictionary getScriptingDictionary() override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene) override;
    virtual void renderUI(Gui::Widgets& widget) override;

    void setEnabled(bool enabled) {mEnabled = enabled;}
    void setSampleRadius(float radius);
    void setKernelSize(uint32_t kernelSize);
    void setDistribution(uint32_t distribution);
    void setShaderVariant(uint32_t variant);
    bool getEnabled() {return mEnabled;}
    float getSampleRadius() { return mData.radius; }
    uint32_t getKernelSize() { return mKernelSize; }
    uint32_t getDistribution() { return (uint32_t)mHemisphereDistribution; }

private:
    VAO();
    ResourceFormat getAmbientMapFormat() const;
    void setNoiseTexture();
    void setKernel();

    SSAOData mData;
    bool mDirty = true;

    bool mEnabled = true;
    DepthMode mDepthMode = DepthMode::SingleDepth;
    Fbo::SharedPtr mpAOFbo;
    uint mFrameIndex = 0;
    bool mColorMap = false;

    Sampler::SharedPtr mpNoiseSampler;
    Texture::SharedPtr mpNoiseTexture;

    Sampler::SharedPtr mpTextureSampler;
    SampleDistribution mHemisphereDistribution = SampleDistribution::VanDerCorput;

    FullScreenPass::SharedPtr mpSSAOPass;

    Scene::SharedPtr mpScene;
    int mGuardBand = 64;
    bool mClearTexture = true;
    uint32_t mKernelSize = 8;

    bool mSaveDepths = false;
};
