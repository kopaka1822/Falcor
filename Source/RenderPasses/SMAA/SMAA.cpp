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

    const std::string kEdgesTex = "edgesTex";
    const std::string kBlendTex = "blendTex";

    //const std::string kSmaaShaderFile = "RenderPasses/SMAA/SMAA.hlsl";
    const std::string kSmaaShader1 = "RenderPasses/SMAA/Pass1.hlsl";
    const std::string kSmaaShader2 = "RenderPasses/SMAA/Pass2.hlsl";
    const std::string kSmaaShader3 = "RenderPasses/SMAA/Pass3.hlsl";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, SMAA>();
}

SMAA::SMAA(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpSearchTex = Texture::create2D(mpDevice, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, ResourceFormat::R8Unorm, 1, 1, searchTexBytes);
    mpAreaTex = Texture::create2D(mpDevice, AREATEX_WIDTH, AREATEX_HEIGHT, ResourceFormat::RG8Unorm, 1, 1, areaTexBytes);

    Sampler::Desc s;
    s.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    s.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpLinearSampler = Sampler::create(mpDevice, s);
    s.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpPointSampler = Sampler::create(mpDevice, s);

    mpFbo = Fbo::create(mpDevice);
}

Properties SMAA::getProperties() const
{
    return {};
}

RenderPassReflection SMAA::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kColorIn, "Color Input").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepthIn, "(Optional) Linear Depth Input").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);

    reflector.addOutput(kEdgesTex, "Top,Right Edges").format(ResourceFormat::RG8Unorm);
    reflector.addOutput(kBlendTex, "Blend Weights").format(ResourceFormat::RGBA8Unorm);

    reflector.addOutput(kColorOut, "Color Output").bindFlags(ResourceBindFlags::RenderTarget).format(ResourceFormat::RGBA8UnormSrgb);

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

    if(!mEnabled)
    {
        pRenderContext->blit(pColorIn->getSRV(), pColorOut->getRTV());
        return;
    }

    const uint width = renderData.getDefaultTextureDims().x;
    const uint height = renderData.getDefaultTextureDims().x;

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
        //defines.add("SMAA_REPROJECTION", "1");

        mpPass1 = FullScreenPass::create(mpDevice, kSmaaShader1, defines);
        mpPass1->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass1->getRootVar()["PointSampler"] = mpPointSampler;

        mpPass2 = FullScreenPass::create(mpDevice, kSmaaShader2, defines);
        mpPass2->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass2->getRootVar()["PointSampler"] = mpPointSampler;

        mpPass3 = FullScreenPass::create(mpDevice, kSmaaShader3, defines);
        mpPass3->getRootVar()["LinearSampler"] = mpLinearSampler;
        mpPass3->getRootVar()["PointSampler"] = mpPointSampler;
    }

    // clear edges and blend textures
    pRenderContext->clearTexture(pEdgesTex.get());
    pRenderContext->clearTexture(pBlendTex.get());

    {
        FALCOR_PROFILE(pRenderContext, "EdgeDetection");
        auto var = mpPass1->getRootVar();
        var["gColor"] = pColorIn;

        mpFbo->attachColorTarget(pEdgesTex, 0);
        mpPass1->execute(pRenderContext, mpFbo);
    }

    {
        FALCOR_PROFILE(pRenderContext, "BlendingWeights");
        auto var = mpPass2->getRootVar();
        var["gEdgesTex"] = pEdgesTex;
        var["gAreaTex"] = mpAreaTex;
        var["gSearchTex"] = mpSearchTex;

        mpFbo->attachColorTarget(pBlendTex, 0);
        mpPass2->execute(pRenderContext, mpFbo);
    }

    {
        FALCOR_PROFILE(pRenderContext, "Blend");
        auto var = mpPass3->getRootVar();
        var["gColor"] = pColorIn;
        var["gBlendTex"] = pBlendTex;

        mpFbo->attachColorTarget(pColorOut, 0);
        mpPass3->execute(pRenderContext, mpFbo);
    }
}

void SMAA::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if(!mEnabled) return;


}
