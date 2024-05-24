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
#include "DepthGuide.h"

namespace
{
    const std::string kDepthIn = "linearZ";
    const std::string kOut = "out";

    const std::string kShaderFilename = "RenderPasses/DepthGuide/DepthGuide.ps.slang";
}


extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, DepthGuide>();
}

DepthGuide::DepthGuide(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpFbo = Fbo::create(mpDevice);
    mpPass = FullScreenPass::create(pDevice, kShaderFilename);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpSampler = Sampler::create(pDevice, samplerDesc);

    //mpMinMaxBuffer = Buffer::createStructured(pDevice, sizeof(float2), 1, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false);
    mpMinMaxBuffer = Buffer::create(pDevice, sizeof(float2), ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr);


    mpRangePass = FullScreenPass::create(pDevice, "RenderPasses/DepthGuide/MinMaxRange.ps.slang");
}

Properties DepthGuide::getProperties() const
{
    return {};
}

RenderPassReflection DepthGuide::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepthIn, "Linear Z texture");
    reflector.addOutput(kOut, "Output texture").format(ResourceFormat::RGBA32Float);
    return reflector;
}

void DepthGuide::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pDepth = renderData[kDepthIn]->asTexture();
    const auto& pOut = renderData[kOut]->asTexture();

    // clear min max buffer
    float2 minMax = float2(FLT_MAX, 0.0f);
    mpMinMaxBuffer->setElement(0, minMax);

    // determine min max
    auto mmVar = mpRangePass->getRootVar();
    mmVar["gMinMaxDepth"] = mpMinMaxBuffer;
    mmVar["gSampler"] = mpSampler;
    mmVar["gDepthTex"] = pDepth;
    mpRangePass->execute(pRenderContext, mpFbo);

    //mpMinMaxBuffer->readData(&minMax, 0, sizeof(float2));
    

    // render based on min max
    mpFbo->attachColorTarget(pOut, 0);

    auto var = mpPass->getRootVar();
    var["gDepthTex"] = pDepth;
    var["gSampler"] = mpSampler;
    var["gMinMaxDepth"] = mpMinMaxBuffer;

    mpPass->execute(pRenderContext, mpFbo);
}

void DepthGuide::renderUI(Gui::Widgets& widget)
{
    widget.var("Min Depth", mMinDepth);
    widget.var("Max Depth", mMaxDepth);
}
