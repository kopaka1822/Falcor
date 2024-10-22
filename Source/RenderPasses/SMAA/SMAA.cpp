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
#include "SMAA.h"
#include "AreaTex.h"
#include "SearchTex.h"

namespace
{
    const std::string kColorOut = "colorOut";
    const std::string kColorIn = "colorIn";
    const std::string kDepthIn = "linearDepth";
    const std::string kInternalStencil = "stencil";
    const std::string kVelocityIn = "mvec";

    const std::string kTmpColorOut = "kTempColorOut";
    const std::string kPrevColor = "kPrevColor";

    const std::string kEdgesTex = "edgesTex";
    const std::string kBlendTex = "blendTex";

    //const std::string kSmaaShaderFile = "RenderPasses/SMAA/SMAA.hlsl";
    const std::string kSmaaShader1 = "RenderPasses/SMAA/Pass1.hlsl";
    const std::string kSmaaShader2 = "RenderPasses/SMAA/Pass2.hlsl";
    const std::string kSmaaShader3 = "RenderPasses/SMAA/Pass3.hlsl";
    const std::string kSmaaShaderReproject = "RenderPasses/SMAA/Reproject.hlsl";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, SMAA>();
}

SMAA::SMAA(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpSearchTex = Texture::create2D(mpDevice, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, ResourceFormat::R8Unorm, 1, 1, searchTexBytes);
    mpSearchTex->setName("SMAASearchTex");
    mpAreaTex = Texture::create2D(mpDevice, AREATEX_WIDTH, AREATEX_HEIGHT, ResourceFormat::RG8Unorm, 1, 1, areaTexBytes);
    mpAreaTex->setName("SMAAAreaTex");

    Sampler::Desc s;
    s.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);
    s.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpLinearSampler = Sampler::create(mpDevice, s);
    s.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpPointSampler = Sampler::create(mpDevice, s);

    mpFbo = Fbo::create(mpDevice);

    DepthStencilState::Desc dsDesc;
    dsDesc.setDepthEnabled(false);
    dsDesc.setStencilEnabled(true);
    // always pass and increase the stencil value
    dsDesc.setStencilFunc(DepthStencilState::Face::FrontAndBack, ComparisonFunc::Always);
    dsDesc.setStencilOp(DepthStencilState::Face::FrontAndBack, DepthStencilState::StencilOp::Increase, DepthStencilState::StencilOp::Increase, DepthStencilState::StencilOp::Increase);
    mpStencilWriteMask = DepthStencilState::create(dsDesc);

    // modify desc to only accept non-zero stencil values
    dsDesc.setStencilFunc(DepthStencilState::Face::FrontAndBack, ComparisonFunc::NotEqual);
    dsDesc.setStencilRef(0);
    dsDesc.setStencilOp(DepthStencilState::Face::FrontAndBack, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Keep);
    mpStencilUseMask = DepthStencilState::create(dsDesc);
}

Properties SMAA::getProperties() const
{
    return {};
}

RenderPassReflection SMAA::reflect(const CompileData& compileData)
{
    auto outFormat = ResourceFormat::RGBA8UnormSrgb;

    RenderPassReflection reflector;
    reflector.addInput(kColorIn, "Color Input").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepthIn, "(Optional) Linear Depth Input").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kVelocityIn, "2D motion vectors").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);

    reflector.addOutput(kEdgesTex, "Top,Right Edges").format(ResourceFormat::RG8Unorm);
    reflector.addOutput(kBlendTex, "Blend Weights").format(ResourceFormat::RGBA8Unorm);

    reflector.addOutput(kColorOut, "Color Output").bindFlags(ResourceBindFlags::RenderTarget).format(outFormat);

    reflector.addInternal(kInternalStencil, "internal stencil").bindFlags(ResourceBindFlags::DepthStencil).format(ResourceFormat::D32FloatS8X24);
    reflector.addInternal(kTmpColorOut, "temporary storage for color").format(outFormat);
    reflector.addInternal(kPrevColor, "previous frame color").format(outFormat).flags(RenderPassReflection::Field::Flags::Persistent);

    return reflector;
}

void SMAA::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mpPass1.reset();
    mpPass2.reset();
    mpPass3.reset();
}

void SMAA::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pColorIn = renderData[kColorIn]->asTexture();
    ref<Texture> pDepthIn = renderData[kDepthIn] ? renderData[kDepthIn]->asTexture() : nullptr;
    auto pEdgesTex = renderData[kEdgesTex]->asTexture();
    auto pBlendTex = renderData[kBlendTex]->asTexture();
    auto pColorOut = renderData[kColorOut]->asTexture();
    auto pStencil = renderData[kInternalStencil]->asTexture();
    ref<Texture> pVelocityIn = renderData[kVelocityIn] ? renderData[kVelocityIn]->asTexture() : nullptr;

    auto pTmpColorOut = renderData[kTmpColorOut]->asTexture();
    auto pPrevColor = renderData[kPrevColor]->asTexture();

    if(!mEnabled)
    {
        pRenderContext->blit(pColorIn->getSRV(), pColorOut->getRTV());
        return;
    }

    const uint width = renderData.getDefaultTextureDims().x;
    const uint height = renderData.getDefaultTextureDims().y;

    if(!mpPass1 || !mpPass2 || !mpPass3)
    {
        DefineList defines;
        defines.add("SMAA_HLSL_4_1");
        defines.add("SMAA_RT_METRICS", "float4(1.0/" + std::to_string(width) + ", 1.0/" + std::to_string(height) + ", " + std::to_string(width) + ", " + std::to_string(height) + ")");
        //defines.add("SMAA_PRESET_LOW");
        //defines.add("SMAA_PRESET_MEDIUM");
        //defines.add("SMAA_PRESET_HIGH");
        defines.add("SMAA_PRESET_ULTRA");
        //defines.add("SMAA_PREDICATION", "1");
        defines.add("SMAA_REPROJECTION", "1");

        mpPass1 = FullScreenPass::create(mpDevice, kSmaaShader1, defines);
        mpPass1->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass1->getRootVar()["PointSampler"] = mpPointSampler;
        mpPass1->getState()->setDepthStencilState(mpStencilWriteMask);

        mpPass2 = FullScreenPass::create(mpDevice, kSmaaShader2, defines);
        mpPass2->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass2->getRootVar()["PointSampler"] = mpPointSampler;
        mpPass2->getState()->setDepthStencilState(mpStencilUseMask);

        mpPass3 = FullScreenPass::create(mpDevice, kSmaaShader3, defines);
        mpPass3->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass3->getRootVar()["PointSampler"] = mpPointSampler;

        mpPassReproject = FullScreenPass::create(mpDevice, kSmaaShaderReproject, defines);
        mpPassReproject->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPassReproject->getRootVar()["PointSampler"] = mpPointSampler;

        // do this only on resets:
        pRenderContext->blit(pColorIn->getSRV(), pPrevColor->getRTV());
    }

    // clear edges and blend textures
    pRenderContext->clearTexture(pEdgesTex.get(), float4(0.0f));
    pRenderContext->clearTexture(pBlendTex.get(), float4(0.0f));
    pRenderContext->clearDsv(pStencil->getDSV().get(), 0.0f, 0, false, true);

    {
        FALCOR_PROFILE(pRenderContext, "EdgeDetection");
        auto var = mpPass1->getRootVar();
        var["gColor"] = pColorIn;
        var["gDepth"] = pDepthIn;

        mpPass1->getProgram()->addDefine("EDGE_MODE", std::to_string(uint(mEdgeMode)));

        mpFbo->attachColorTarget(pEdgesTex, 0);
        mpFbo->attachDepthStencilTarget(pStencil);
        mpPass1->execute(pRenderContext, mpFbo);
    }

    {
        FALCOR_PROFILE(pRenderContext, "BlendingWeights");
        auto var = mpPass2->getRootVar();
        var["gEdgesTex"] = pEdgesTex;
        var["gAreaTex"] = mpAreaTex;
        var["gSearchTex"] = mpSearchTex;

        mpFbo->attachColorTarget(pBlendTex, 0);
        mpFbo->attachDepthStencilTarget(pStencil);
        mpPass2->execute(pRenderContext, mpFbo);
    }

    {
        FALCOR_PROFILE(pRenderContext, "Blend");
        auto var = mpPass3->getRootVar();
        var["gColor"] = pColorIn;
        var["gBlendTex"] = pBlendTex;
        var["gVelocity"] = pVelocityIn;
        mpPass3->getProgram()->addDefine("SMAA_REPROJECTION", mReprojection ? "1" : "0");

        if(mReprojection)
            mpFbo->attachColorTarget(pTmpColorOut, 0);
        else
            mpFbo->attachColorTarget(pColorOut, 0);
        
        mpFbo->attachDepthStencilTarget(nullptr);
        mpPass3->execute(pRenderContext, mpFbo);
    }

    if(mReprojection)
    {
        FALCOR_PROFILE(pRenderContext, "Reprojection");
        auto var = mpPassReproject->getRootVar();
        var["gColor"] = pTmpColorOut;
        var["gPrevColor"] = pPrevColor;
        var["gVelocity"] = pVelocityIn;

        mpFbo->attachColorTarget(pColorOut, 0);
        mpPassReproject->execute(pRenderContext, mpFbo);

        // copy color to prev color
        pRenderContext->blit(pColorOut->getSRV(), pPrevColor->getRTV());
    }
}

void SMAA::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if(!mEnabled) return;

    widget.checkbox("Reprojection", mReprojection);
    widget.dropdown("EdgeMode", mEdgeMode);
}
