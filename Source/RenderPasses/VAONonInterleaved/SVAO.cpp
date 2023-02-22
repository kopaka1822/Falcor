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

#include "../SSAO/scissors.h"
#include "VAOSettings.h"


namespace
{
    const char kDesc[] = "Stenciled Volumetric Ambient Occlusion";

    const std::string kAmbientMap = "ao";
    const std::string kAoStencil = "stencil";
    const std::string kAccessStencil = "accessStencil";
    const std::string kGbufferDepth = "gbufferDepth";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string kNormals = "normals";
    const std::string kInternalStencil = "internalStencil";
    
    const std::string kRasterShader = "RenderPasses/VAONonInterleaved/Raster.ps.slang";

    const std::string kRasterShader2 = "RenderPasses/VAONonInterleaved/Raster2.ps.slang";
    const std::string kRayShader = "RenderPasses/VAONonInterleaved/Ray.rt.slang";
    const std::string kStencilShader = "RenderPasses/VAONonInterleaved/CopyStencil.ps.slang";

    const uint32_t kMaxPayloadSize = 4 * 4;
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
    
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();
    mpFbo2 = Fbo::create();

    mpNoiseTexture = VAOSettings::genNoiseTexture();

    VAOSettings::get().updateFromDict(dict);

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

Dictionary SVAO::getScriptingDictionary()
{
    return VAOSettings::get().getScriptingDictionary();
}

RenderPassReflection SVAO::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addInput(kAoStencil, "(Depth-) Stencil Buffer for the ao mask").format(ResourceFormat::D32FloatS8X24);
    reflector.addInput(kGbufferDepth, "Non-Linear Depth from the G-Buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion (primary)").bindFlags(ResourceBindFlags::UnorderedAccess |  ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    reflector.addOutput(kAoStencil, "Stencil Bitmask for primary / secondary ao").bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource).format(ResourceFormat::R8Uint);
    reflector.addOutput(kAccessStencil, "Stencil Bitmask for secondary depth map accesses").bindFlags(ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource).format(ResourceFormat::R8Uint);
    reflector.addInternal(kInternalStencil, "internal stencil mask").format(ResourceFormat::D24UnormS8);
    return reflector;
}

void SVAO::compile(RenderContext* pContext, const CompileData& compileData)
{
    VAOSettings::get().setResolution(compileData.defaultTexDims.x, compileData.defaultTexDims.y);
    mpRasterPass.reset(); // recompile passes
    mpRasterPass2.reset();
    mpRayProgram.reset();

    // create stochastic depth graph
    Dictionary sdDict;
    sdDict["SampleCount"] = msaa_sample;
    sdDict["Alpha"] = 0.2f;
    sdDict["linearize"] = true;
    sdDict["depthFormat"] = ResourceFormat::D24UnormS8;
    sdDict["CullMode"] = RasterizerState::CullMode::Back;
    mpStochasticDepthGraph = RenderGraph::create("Stochastic Depth");
    auto pStochasticDepthPass = RenderPassLibrary::instance().createPass(pContext, "StochasticDepthMap", sdDict);
    mpStochasticDepthGraph->addPass(pStochasticDepthPass, "StochasticDepthMap");
    mpStochasticDepthGraph->markOutput("StochasticDepthMap.stochasticDepth");

    // obtain SampleCount Variable from StochasticDepthMap pass
    //mpSampleCountVar = pStochasticDepthPass->getVars()->getConstantBuffer("PerFrameCB")->getVariable("SampleCount");
    //pStochasticDepthPass->getScriptingDictionary()["Alpha"] = 
}

void SVAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pNonLinearDepth = renderData[kGbufferDepth]->asTexture();
    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();

    auto pAoMask = renderData[kAoStencil]->asTexture();
    auto pAccessStencil = renderData[kAccessStencil]->asTexture();
    auto pInternalStencil = renderData[kInternalStencil]->asTexture();
    
    const auto& s = VAOSettings::get();

    if (!s.getEnabled())
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
        return;
    }

    if(!mpRasterPass || !mpRasterPass2 || !mpRayProgram) // this needs to be deferred because it needs the scene defines to compile
    {
        Program::DefineList defines;
        defines.add("PRIMARY_DEPTH_MODE", std::to_string(uint32_t(s.getPrimaryDepthMode())));
        defines.add("SECONDARY_DEPTH_MODE", std::to_string(uint32_t(s.getSecondaryDepthMode())));
        defines.add("MSAA_SAMPLES", std::to_string(msaa_sample)); // TODO update this from gui
        defines.add(mpScene->getSceneDefines());
        // raster pass 1
        mpRasterPass = FullScreenPass::create(kRasterShader, defines);
        mpRasterPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());

        // raster pass 2
        mpRasterPass2 = FullScreenPass::create(kRasterShader2, defines);
        mpRasterPass2->getProgram()->setTypeConformances(mpScene->getTypeConformances());
        mpRasterPass2->getState()->setDepthStencilState(mpDepthStencilState);

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
        // update data raster
        mpRasterPass["StaticCB"].setBlob(s.getData());
        mpRasterPass["gNoiseSampler"] = mpNoiseSampler;
        mpRasterPass["gTextureSampler"] = mpTextureSampler;
        mpRasterPass["gNoiseTex"] = mpNoiseTexture;
        // update data raster 2
        mpRasterPass2["StaticCB"].setBlob(s.getData());
        mpRasterPass2["gNoiseSampler"] = mpNoiseSampler;
        mpRasterPass2["gTextureSampler"] = mpTextureSampler;
        mpRasterPass2["gNoiseTex"] = mpNoiseTexture;
        // update data ray
        mRayVars["StaticCB"].setBlob(s.getData());
        mRayVars["gNoiseSampler"] = mpNoiseSampler;
        mRayVars["gTextureSampler"] = mpTextureSampler;
        mRayVars["gNoiseTex"] = mpNoiseTexture;

        // also clear ao texture if guard band changed
        pRenderContext->clearTexture(pAoDst.get(), float4(0.0f));
    }

    auto accessStencilUAV = pAccessStencil->getUAV(0);
    if(s.getSecondaryDepthMode() == DepthMode::StochasticDepth)
        pRenderContext->clearUAV(accessStencilUAV.get(), uint4(0u));

    mpFbo->attachColorTarget(pAoDst, 0);
    mpFbo->attachColorTarget(pAoMask, 1);

    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
    mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
    mpRasterPass["gDepthTex"] = pDepth;
    mpRasterPass["gDepthTex2"] = pDepth2;
    mpRasterPass["gNormalTex"] = pNormal;
    //mpRasterPass["gDepthAccess"] = pAccessStencil;
    mpRasterPass["gDepthAccess"].setUav(accessStencilUAV);

    setGuardBandScissors(*mpRasterPass->getState(), renderData.getDefaultTextureDims(), VAOSettings::get().getGuardBand());
    {
        FALCOR_PROFILE("AO 1");
        mpRasterPass->execute(pRenderContext, mpFbo, false);
    }

    Texture::SharedPtr pStochasticDepthMap;
    
    //  execute stochastic depth map
    if (s.getSecondaryDepthMode() == DepthMode::StochasticDepth)
    {
        FALCOR_PROFILE("Stochastic Depth");
        mpStochasticDepthGraph->setInput("StochasticDepthMap.depthMap", pNonLinearDepth); 
        mpStochasticDepthGraph->setInput("StochasticDepthMap.stencilMask", pAccessStencil);
        mpStochasticDepthGraph->execute(pRenderContext);
        pStochasticDepthMap = mpStochasticDepthGraph->getOutput("StochasticDepthMap.stochasticDepth")->asTexture();
    }

    if (VAOSettings::get().getRayPipeline()) // RAY PIPELINE
    {
        // set raytracing data
        //mpScene->setRaytracingShaderData(pRenderContext, mRayVars);

        // set camera data
        auto pCamera = mpScene->getCamera().get();
        pCamera->setShaderData(mRayVars["PerFrameCB"]["gCamera"]);
        mRayVars["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
        mRayVars["PerFrameCB"]["guardBand"] = s.getGuardBand();

        // set textures
        mRayVars["gDepthTex"] = pDepth;
        mRayVars["gDepthTex2"] = pDepth2;
        mRayVars["gNormalTex"] = pNormal;
        mRayVars["gsDepthTex"] = pStochasticDepthMap;
        mRayVars["aoMask"] = pAoMask;
        //mRayVars["aoPrev"] = pAoDst; // src view
        mRayVars["output"] = pAoDst; // uav view

        uint3 dims = uint3(pAoDst->getWidth() - 2 * s.getGuardBand(), pAoDst->getHeight() - 2 * s.getGuardBand(), 1);
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

        mpFbo2->attachDepthStencilTarget(pInternalStencil);
        mpFbo2->attachColorTarget(pAoDst, 0);

        // set raytracing data
        auto var = mpRasterPass2->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);

        // set camera data
        auto pCamera = mpScene->getCamera().get();
        pCamera->setShaderData(mpRasterPass2["PerFrameCB"]["gCamera"]);
        mpRasterPass2["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // set textures
        mpRasterPass2["gDepthTex"] = pDepth;
        mpRasterPass2["gDepthTex2"] = pDepth2;
        mpRasterPass2["gNormalTex"] = pNormal;
        mpRasterPass2["gsDepthTex"] = pStochasticDepthMap;
        mpRasterPass2["aoMask"] = pAoMask;
        mpRasterPass2["aoPrev"] = pAoDst;

        setGuardBandScissors(*mpRasterPass2->getState(), renderData.getDefaultTextureDims(), VAOSettings::get().getGuardBand());

        {
            FALCOR_PROFILE("AO 2 (raster)");
            mpRasterPass2->execute(pRenderContext, mpFbo2, false);
        }
    }
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
    mpRasterPass2.reset();
    mpRayProgram.reset();
}
