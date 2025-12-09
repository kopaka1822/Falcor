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
#include "MotionVecVis.h"

namespace
{
    const std::string kMVecIn = "mvec";
    const std::string kColorOut = "color";

    const std::string kShaderFilename = "RenderPasses/MotionVecVis/MotionVecVis.ps.slang";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, MotionVecVis>();
}

MotionVecVis::MotionVecVis(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpPass = FullScreenPass::create(mpDevice, kShaderFilename);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpPass->getRootVar()["s"] = Sampler::create(mpDevice, samplerDesc);

    mpFbo = Fbo::create(mpDevice);
}

Properties MotionVecVis::getProperties() const
{
    return {};
}

RenderPassReflection MotionVecVis::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kMVecIn, "Motion Vectors").bindFlags(Resource::BindFlags::ShaderResource);
    reflector.addOutput(kColorOut, "Output color").bindFlags(Resource::BindFlags::AllColorViews).format(ResourceFormat::RGBA32Float);
    return reflector;
}

void MotionVecVis::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pMVec = renderData[kMVecIn]->asTexture();
    auto pOutput = renderData[kColorOut]->asTexture();

    mpFbo->attachColorTarget(pOutput, 0);
    mpPass->getRootVar()["gMotionVec"] = pMVec;

    auto vars = mpPass->getRootVar();
    vars["PerFrameCB"]["scale"] = mScale;

    mpPass->execute(pRenderContext, mpFbo);
}

void MotionVecVis::renderUI(Gui::Widgets& widget)
{
    widget.var("Scale", mScale, 0.0f, 100.0f);
}
