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
#include "Lineart.h"

namespace
{
    const std::string kDiffuseIn = "diffuseIn";
    const std::string kPosIn = "wPos";
    const std::string kNormalIn = "wNormal";
    const std::string kOut = "out";

    const std::string kShaderFilename = "RenderPasses/Lineart/Lineart.ps.slang";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, Lineart>();
}

Lineart::Lineart(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpFbo = Fbo::create(mpDevice);
    mpPass = FullScreenPass::create(pDevice, kShaderFilename);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpSampler = Sampler::create(pDevice, samplerDesc);
}

Properties Lineart::getProperties() const
{
    return {};
}

RenderPassReflection Lineart::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDiffuseIn, "Diffuse texture");
    reflector.addInput(kPosIn, "World space position");
    reflector.addInput(kNormalIn, "World space normal");
    reflector.addOutput(kOut, "Output texture");
    return reflector;
}

void Lineart::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pDiffuse = renderData[kDiffuseIn]->asTexture();
    const auto& pPos = renderData[kPosIn]->asTexture();
    const auto& pNormal = renderData[kNormalIn]->asTexture();
    const auto& pOut = renderData[kOut]->asTexture();

    mpFbo->attachColorTarget(pOut, 0);

    auto var = mpPass->getRootVar();
    var["gDiffuseTex"] = pDiffuse;
    var["gPosTex"] = pPos;
    var["gNormalTex"] = pNormal;
    var["gSampler"] = mpSampler;

    var["StaticCB"]["gDiffuseStrength"] = mDiffuseStrength;
    var["StaticCB"]["gPosStrength"] = mPosStrength;

    mpPass->execute(pRenderContext, mpFbo);
}

void Lineart::renderUI(Gui::Widgets& widget)
{
    widget.var("Diffuse strength", mDiffuseStrength, 0.f, 1.f);
    widget.var("Position strength", mPosStrength, 0.f, 1.f);
}
