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
#include "SVAO.h"

#include <glm/gtc/random.hpp>

#include "SVAO2.h"
#include "../SSAO/scissors.h"
#include "VAOSettings.h"


namespace
{
    const char kDesc[] = "Stenciled Volumetric Ambient Occlusion";

    const std::string kAmbientMap = "ao";
    const std::string kAoStencil = "stencil";
    const std::string kAccessStencil = "accessStencil";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string kNormals = "normals";

    const std::string kRasterShader = "RenderPasses/VAONonInterleaved/Raster.ps.slang";
}

const RenderPass::Info SVAO::kInfo = { "SVAO", kDesc };

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(SVAO::kInfo, SVAO::create);
    lib.registerPass(SVAO2::kInfo, SVAO2::create);
}

SVAO::SharedPtr SVAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new SVAO(dict));
    return pPass;
}

SVAO::SVAO(const Dictionary& dict)
    :
    RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    //samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();

    mpNoiseTexture = VAOSettings::genNoiseTexture();

    VAOSettings::get().updateFromDict(dict);
}

Dictionary SVAO::getScriptingDictionary()
{
    return VAOSettings::get().getScriptingDictionary();
}

RenderPassReflection SVAO::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addInput(kAoStencil, "(Depth-) Stencil Buffer for the ao mask").format(ResourceFormat::D32FloatS8X24);
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion (primary)").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    reflector.addOutput(kAoStencil, "Stencil Bitmask for primary / secondary ao").bindFlags(ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Uint);
    reflector.addOutput(kAccessStencil, "Stencil Bitmask for secondary depth map accesses").bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::R8Uint);
    return reflector;
}

void SVAO::compile(RenderContext* pContext, const CompileData& compileData)
{
    VAOSettings::get().setResolution(compileData.defaultTexDims.x, compileData.defaultTexDims.y);
    mpRasterPass.reset(); // recompile raster pass
}

void SVAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();

    auto pStencil = renderData[kAoStencil]->asTexture();
    auto pAccessStencil = renderData[kAccessStencil]->asTexture();

    const auto& s = VAOSettings::get();

    if (!s.getEnabled())
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
        return;
    }

    if(!mpRasterPass) // this needs to be deferred because it needs the scene defines to compile
    {
        Program::DefineList defines;
        defines.add("PRIMARY_DEPTH_MODE", std::to_string(uint32_t(s.getPrimaryDepthMode())));
        defines.add("SECONDARY_DEPTH_MODE", std::to_string(uint32_t(s.getSecondaryDepthMode())));
        defines.add(mpScene->getSceneDefines());
        mpRasterPass = FullScreenPass::create(kRasterShader, defines);
        mpRasterPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
    }

    if(s.IsDirty() || s.IsReset())
    {
        // update data
        mpRasterPass["StaticCB"].setBlob(s.getData());

        mpRasterPass["gNoiseSampler"] = mpNoiseSampler;
        mpRasterPass["gTextureSampler"] = mpTextureSampler;
        mpRasterPass["gNoiseTex"] = mpNoiseTexture;

        // also clear ao texture if guard band changed
        pRenderContext->clearTexture(pAoDst.get(), float4(0.0f));
    }

    auto accessStencilUAV = pAccessStencil->getUAV(0);
    if(s.getSecondaryDepthMode() == DepthMode::StochasticDepth)
        pRenderContext->clearUAV(accessStencilUAV.get(), uint4(0u));

    mpFbo->attachColorTarget(pAoDst, 0);
    mpFbo->attachColorTarget(pStencil, 1);

    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
    mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
    mpRasterPass["gDepthTex"] = pDepth;
    mpRasterPass["gDepthTex2"] = pDepth2;
    mpRasterPass["gNormalTex"] = pNormal;
    //mpRasterPass["gDepthAccess"] = pAccessStencil;
    mpRasterPass["gDepthAccess"].setUav(accessStencilUAV);

    setGuardBandScissors(*mpRasterPass->getState(), renderData.getDefaultTextureDims(), VAOSettings::get().getGuardBand());
    mpRasterPass->execute(pRenderContext, mpFbo, false);
}

void SVAO::renderUI(Gui::Widgets& widget)
{
    VAOSettings::get().renderUI(widget);
    if (VAOSettings::get().IsReset())
        mPassChangedCB();
}

void SVAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpRasterPass.reset(); // new scene defines => recompile
}