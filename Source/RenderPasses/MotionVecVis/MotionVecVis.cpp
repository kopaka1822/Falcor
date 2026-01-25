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
    const std::string kPosWIn = "posW"; // optional, to get position differences
    const std::string kPosDiffOut = "posDiff";

    const std::string kShaderFilename = "RenderPasses/MotionVecVis/MotionVecVis.ps.slang";
    const std::string kPosDiffShaderFilename = "RenderPasses/MotionVecVis/PosDiff.ps.slang";
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

    mpPosDiffPass = FullScreenPass::create(mpDevice, kPosDiffShaderFilename);
    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpPosDiffPass->getRootVar()["s"] = Sampler::create(mpDevice, samplerDesc);

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
    reflector.addInput(kPosWIn, "World Position (from VBuffer)").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);

    reflector.addOutput(kColorOut, "Output color").bindFlags(Resource::BindFlags::AllColorViews).format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kPosDiffOut, "World Position Diff with previous frame").bindFlags(Resource::BindFlags::AllColorViews).format(ResourceFormat::RGBA32Float);
    return reflector;
}

void MotionVecVis::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pMVec = renderData[kMVecIn]->asTexture();
    auto pOutput = renderData[kColorOut]->asTexture();
    auto pPosW = renderData.getTexture(kPosWIn);
    auto pPosDiff = renderData.getTexture(kPosDiffOut);

    // main visualization
    {
        mpFbo->attachColorTarget(pOutput, 0);
        mpPass->getRootVar()["gMotionVec"] = pMVec;

        auto vars = mpPass->getRootVar();
        vars["PerFrameCB"]["scale"] = mScale;
        vars["PerFrameCB"]["darkTheme"] = mDarkTheme;

        mpPass->execute(pRenderContext, mpFbo);
    }

    // optional pos difference
    if (pPosW && pPosDiff && mpScene)
    {
        bool usePrevPos = mpPrevPos && mpPrevPos->getWidth() == pPosW->getWidth() && mpPrevPos->getHeight() == pPosW->getHeight();
        const float2 curJitter = float2(-mpScene->getCamera()->getJitterX(), mpScene->getCamera()->getJitterY());

        auto vars = mpPosDiffPass->getRootVar();
        vars["PerFrameCB"]["scale"] = mScale;
        vars["PerFrameCB"]["prevJitter"] = mPrevJitter;
        vars["PerFrameCB"]["curJitter"] = curJitter;
        vars["gCurPosW"] = pPosW;
        vars["gPrevPosW"] = usePrevPos ? mpPrevPos : pPosW;
        vars["gMotionVec"] = pMVec;

        mpFbo->attachColorTarget(pPosDiff, 0);
        mpPosDiffPass->execute(pRenderContext, mpFbo);

        // create prev pos texture if not existing
        if(!usePrevPos) mpPrevPos = Texture::create2D(mpDevice, pPosW->getWidth(), pPosW->getHeight(), pPosW->getFormat(), 1, 1, nullptr, Resource::BindFlags::AllColorViews);
        // copy for next frame
        pRenderContext->blit(pPosW->getSRV(), mpPrevPos->getRTV());
        mPrevJitter = curJitter;
    }
}

void MotionVecVis::renderUI(Gui::Widgets& widget)
{
    widget.var("Scale", mScale, 0.0f, 10000.0f);
    widget.checkbox("Dark Theme", mDarkTheme);
}
