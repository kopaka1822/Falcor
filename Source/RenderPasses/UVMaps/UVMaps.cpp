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
#include "UVMaps.h"

namespace
{
    const std::string kTexIn = "texC";
    const std::string kMatIn = "material";
    const std::string kOut = "out";

    const std::string kShaderFilename = "RenderPasses/UVMaps/uvmaps.ps.slang";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, UVMaps>();
}

UVMaps::UVMaps(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpFbo = Fbo::create(mpDevice);
    mpPass = FullScreenPass::create(pDevice, kShaderFilename);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpSampler = Sampler::create(pDevice, samplerDesc);
}

Properties UVMaps::getProperties() const
{
    return {};
}

RenderPassReflection UVMaps::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kTexIn, "Texture coordinates");
    reflector.addInput(kMatIn, "Material id");

    for(uint i = 0; i < mMaterialCount; ++i)
    {
        reflector.addOutput(kOut + std::to_string(i), "Output texture").format(ResourceFormat::RGBA32Float);
    }
    return reflector;
}

void UVMaps::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pTex = renderData[kTexIn]->asTexture();
    const auto& pMat = renderData[kMatIn]->asTexture();

    auto var = mpPass->getRootVar();
    var["gTexC"] = pTex;
    var["gMaterialTex"] = pMat;

    for(uint i = 0; i < mMaterialCount; ++i)
    {
        var["PerFrameCB"]["curMaterial"] = i;
        mpFbo->attachColorTarget(renderData[kOut + std::to_string(i)]->asTexture(), 0);

        mpPass->execute(pRenderContext, mpFbo);
    }
}

void UVMaps::renderUI(Gui::Widgets& widget)
{
}
