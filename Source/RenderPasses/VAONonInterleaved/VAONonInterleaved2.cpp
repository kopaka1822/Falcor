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
#include "VAONonInterleaved2.h"
#include "../SSAO/scissors.h"
#include "VAOSettings.h"
#include <glm/gtc/random.hpp>

#include "Core/API/DepthStencilState.h"


namespace
{
    const char kDesc[] = "Optimized Volumetric Ambient Occlusion (Non-Interleaved) 2nd pass";

    const std::string kAmbientMap = "ao";
    const std::string kAOMask = "aoStencil";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";
    const std::string kInternalStencil = "internalStencil";

    const std::string kRasterShader = "RenderPasses/VAONonInterleaved/Raster2.ps.slang";
    const std::string kRayShader = "RenderPasses/VAONonInterleaved/Ray.rt.slang";
    const std::string kStencilShader = "RenderPasses/VAONonInterleaved/CopyStencil.ps.slang";

    const uint32_t kMaxPayloadSize = 4 * 4;
}

const RenderPass::Info VAONonInterleaved2::kInfo = { "VAONonInterleaved2", kDesc };

VAONonInterleaved2::SharedPtr VAONonInterleaved2::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VAONonInterleaved2(dict));
    return pPass;
}

VAONonInterleaved2::VAONonInterleaved2(const Dictionary& dict)
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

    // set stencil to succeed if not equal to zero
    DepthStencilState::Desc stencil;
    stencil.setDepthEnabled(false);
    stencil.setDepthWriteMask(false);
    stencil.setStencilEnabled(true);
    stencil.setStencilOp(DepthStencilState::Face::FrontAndBack, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Keep);
    stencil.setStencilFunc(DepthStencilState::Face::FrontAndBack, DepthStencilState::Func::NotEqual);
    stencil.setStencilRef(0);
    stencil.setStencilReadMask(1);
    stencil.setStencilWriteMask(0);
    mpDepthStencilState = DepthStencilState::create(stencil);

    // VAO settings will be loaded by first pass
    mpStencilPass = FullScreenPass::create(kStencilShader);
    stencil.setStencilOp(DepthStencilState::Face::FrontAndBack, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Keep, DepthStencilState::StencilOp::Replace);
    stencil.setStencilFunc(DepthStencilState::Face::FrontAndBack, DepthStencilState::Func::Always);
    stencil.setStencilRef(1);
    stencil.setStencilReadMask(0);
    stencil.setStencilWriteMask(1);
    mpStencilPass->getState()->setDepthStencilState(DepthStencilState::create(stencil));
    mpStencilFbo = Fbo::create();
}

Dictionary VAONonInterleaved2::getScriptingDictionary()
{
    Dictionary d; // will be set by first pass
    return d;
}

RenderPassReflection VAONonInterleaved2::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addInput(kAoStencil, "(Depth-) Stencil Buffer for the ao mask").format(ResourceFormat::D32FloatS8X24);
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(ksDepth, "Linear Stochastic Depth Map").texture2D(0, 0, 0).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kAOMask, "Mask where to recalculate ao with secondary information").format(ResourceFormat::R8Uint).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInputOutput(kAmbientMap, "Ambient Occlusion (primary)").bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::UnorderedAccess).format(ResourceFormat::R8Unorm);
    // internal stencil mask
    reflector.addInternal(kInternalStencil, "internal stencil mask").format(ResourceFormat::D24UnormS8); 
    return reflector;
}

void VAONonInterleaved2::compile(RenderContext* pContext, const CompileData& compileData)
{
    mpRasterPass.reset(); // recompile passes
    mpRayProgram.reset();
}

void VAONonInterleaved2::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    auto psDepth = renderData[ksDepth]->asTexture();

    auto pAoMask = renderData[kAOMask]->asTexture();

    auto pInternalStencil = renderData[kInternalStencil]->asTexture();

    const auto& s = VAOSettings::get();

    if (!s.getEnabled())
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
        return;
    }

    if(!mpRasterPass || !mpRayProgram) // this needs to be deferred because it needs the scene defines to compile
    {
        Program::DefineList defines;
        defines.add("PRIMARY_DEPTH_MODE", std::to_string(uint32_t(s.getPrimaryDepthMode())));
        defines.add("SECONDARY_DEPTH_MODE", std::to_string(uint32_t(s.getSecondaryDepthMode())));
        defines.add("MSAA_SAMPLES", std::to_string(psDepth->getSampleCount()));
        defines.add(mpScene->getSceneDefines());

        // raster pass
        mpRasterPass = FullScreenPass::create(kRasterShader, defines);
        mpRasterPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
        mpRasterPass->getState()->setDepthStencilState(mpDepthStencilState);

        // ray pass
        RtProgram::Desc desc;
        desc.addShaderLibrary(kRayShader);
        desc.setMaxPayloadSize(kMaxPayloadSize);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        desc.addTypeConformances(mpScene->getTypeConformances());

        RtBindingTable::SharedPtr sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));
        sbt->setHitGroup(0, mpScene->getGeometryIDs(GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        // TODO add remaining primitives
        mpRayProgram = RtProgram::create(desc, defines);
        mRayVars = RtProgramVars::create(mpRayProgram, sbt);
    }

    if(s.IsDirty() || s.IsReset())
    {
        // update data
        mpRasterPass["StaticCB"].setBlob(s.getData());
        mpRasterPass["gNoiseSampler"] = mpNoiseSampler;
        mpRasterPass["gTextureSampler"] = mpTextureSampler;
        mpRasterPass["gNoiseTex"] = mpNoiseTexture;

        mRayVars["StaticCB"].setBlob(s.getData());
        mRayVars["gNoiseSampler"] = mpNoiseSampler;
        mRayVars["gTextureSampler"] = mpTextureSampler;
        mRayVars["gNoiseTex"] = mpNoiseTexture;
    }

    if(VAOSettings::get().getRayPipeline()) // RAY PIPELINE
    {
        // set raytracing data
        //mpScene->setRaytracingShaderData(pRenderContext, mRayVars);
        
        // set camera data
        auto pCamera = mpScene->getCamera().get();
        pCamera->setShaderData(mRayVars["PerFrameCB"]["gCamera"]);
        mRayVars["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // set textures
        mRayVars["gDepthTex"] = pDepth;
        mRayVars["gDepthTex2"] = pDepth2;
        mRayVars["gNormalTex"] = pNormal;
        mRayVars["gsDepthTex"] = psDepth;
        mRayVars["aoMask"] = pAoMask;
        //mRayVars["aoPrev"] = pAoDst; // src view
        mRayVars["output"] = pAoDst; // uav view

        // TODO add guard band
        if(VAOSettings::get().getGuardBand() != 0)
            Logger::log(Logger::Level::Warning, "guard band was not implemented for raytracing pipeline");

        mpScene->raytrace(pRenderContext, mpRayProgram.get(), mRayVars, uint3{ pAoDst->getWidth(), pAoDst->getHeight(), 1 });
    }
    else // RASTER PIPELINE
    {
        // copy stencil
        {
            FALCOR_PROFILE("copy stencil");
            auto dsv = pInternalStencil->getDSV();
            // clear stencil
            pRenderContext->clearDsv(dsv.get(), 0.0f, 0, false, true);
            mpStencilFbo->attachDepthStencilTarget(pInternalStencil);
            mpStencilPass["aoMask"] = pAoMask;
            mpStencilPass->execute(pRenderContext, mpStencilFbo);
            //pRenderContext->copySubresource(pInternalStencil.get(), 1, pAoMask.get(), 0); // <= don't do this, this results in a slow stencil
        }

        mpFbo->attachDepthStencilTarget(pInternalStencil);
        mpFbo->attachColorTarget(pAoDst, 0);

        // set raytracing data
        auto var = mpRasterPass->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);

        // set camera data
        auto pCamera = mpScene->getCamera().get();
        pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
        mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // set textures
        mpRasterPass["gDepthTex"] = pDepth;
        mpRasterPass["gDepthTex2"] = pDepth2;
        mpRasterPass["gNormalTex"] = pNormal;
        mpRasterPass["gsDepthTex"] = psDepth;
        mpRasterPass["aoMask"] = pAoMask;
        mpRasterPass["aoPrev"] = pAoDst;

        setGuardBandScissors(*mpRasterPass->getState(), renderData.getDefaultTextureDims(), VAOSettings::get().getGuardBand());

        {
            FALCOR_PROFILE("rasterize");
            mpRasterPass->execute(pRenderContext, mpFbo, false);
        }
    }
}

const Gui::DropdownList kDepthModeDropdown =
{
    { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
    { (uint32_t)DepthMode::DualDepth, "DualDepth" },
    { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
};

void VAONonInterleaved2::renderUI(Gui::Widgets& widget)
{
    // will be rendered by first pass
    if (VAOSettings::get().IsReset())
        mPassChangedCB();
}

void VAONonInterleaved2::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpRasterPass.reset(); // new scene defines => recompile
    mpRayProgram.reset();
}
