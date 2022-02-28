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
#include "VAOInterleaved.h"
#include "../VAONonInterleaved/VAOSettings.h"

namespace
{
    const char kDesc[] = "Optimized Volumetric Ambient Occlusion (Interleaved Rendering)";

    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";
    const std::string kAmbientMap = "ambientMap";

    const std::string kProgram = "RenderPasses/VAOInterleaved/Raster.ps.slang";
}

const RenderPass::Info VAOInterleaved::kInfo { "VAOInterleaved", kDesc };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(VAOInterleaved::kInfo, VAOInterleaved::create);
}

VAOInterleaved::SharedPtr VAOInterleaved::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VAOInterleaved(dict));
    return pPass;
}

VAOInterleaved::VAOInterleaved(const Dictionary& dict)
    :
RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();

    mNoiseTexture = VAOSettings::get().genNoiseTextureCPU();

    VAOSettings::get().updateFromDict(dict);
}

Dictionary VAOInterleaved::getScriptingDictionary()
{
    return VAOSettings::get().getScriptingDictionary();
}

RenderPassReflection VAOInterleaved::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 1, 1, 16);;
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 1, 1, 16);;
    reflector.addInput(ksDepth, "linearized stochastic depths").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 0, 1, 1);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    auto& out = reflector.addOutput(kAmbientMap, "Ambient Occlusion (primary)").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    mReady = false;

    auto edge = compileData.connectedResources.getField(kDepth);
    if(edge)
    {
        // set correct size of output resource
        auto srcWidth = edge->getWidth();
        auto srcHeight = edge->getHeight();
        if (edge->getArraySize() != 16) throw std::runtime_error("HBAOPlusInterleaved expects deinterleaved depth with array size 16");
        out.texture2D(srcWidth, srcHeight, 1, 1, 16);
        mReady = true;
    }

    return reflector;
}

void VAOInterleaved::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    if (!mReady) throw std::runtime_error("VAOInterleaved::compile - missing incoming reflection information");

    VAOSettings::get().setResolution(compileData.defaultTexDims.x, compileData.defaultTexDims.y);
    mpRasterPass.reset();
}

void VAOInterleaved::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    auto psDepth = renderData[ksDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAmbient = renderData[kAmbientMap]->asTexture();

    const auto& s = VAOSettings::get();

    if (!s.getEnabled())
    {
        pRenderContext->clearTexture(pAmbient.get(), float4(1.0f));
        return;
    }

    if (!mpRasterPass) // this needs to be deferred because it needs the scene defines to compile
    {
        Program::DefineList defines;
        defines.add("PRIMARY_DEPTH_MODE", std::to_string(uint32_t(s.getPrimaryDepthMode())));
        defines.add("SECONDARY_DEPTH_MODE", std::to_string(uint32_t(s.getSecondaryDepthMode())));
        defines.add(mpScene->getSceneDefines());
        mpRasterPass = FullScreenPass::create(kProgram, defines);
        mpRasterPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
    }

    if (s.IsDirty() || s.IsReset())
    {
        // update data
        mpRasterPass["StaticCB"].setBlob(s.getData());
        mpRasterPass["gTextureSampler"] = mpTextureSampler;
    }

    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
    mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
    //mpRasterPass["gDepthTex"] = pDepth;
    //mpRasterPass["gDepthTex2"] = pDepth2;
    mpRasterPass["gNormalTex"] = pNormal;
    mpRasterPass["gsDepthTex"] = psDepth;

    for (UINT sliceIndex = 0; sliceIndex < 16; ++sliceIndex)
    {
        mpFbo->attachColorTarget(pAmbient, 0, 0, sliceIndex, 1);
        mpRasterPass["gDepthTexQuarter"].setSrv(pDepth->getSRV(0, 1, sliceIndex, 1));
        mpRasterPass["gDepthTex2Quarter"].setSrv(pDepth2->getSRV(0, 1, sliceIndex, 1));
        mpRasterPass["PerFrameCB"]["Rand"] = mNoiseTexture[sliceIndex];
        mpRasterPass["PerFrameCB"]["quarterOffset"] = uint2(sliceIndex % 4, sliceIndex / 4);
        mpRasterPass["PerFrameCB"]["sliceIndex"] = sliceIndex;

        mpRasterPass->execute(pRenderContext, mpFbo);
    }
}

void VAOInterleaved::renderUI(Gui::Widgets& widget)
{
    VAOSettings::get().renderUI(widget);
    if (VAOSettings::get().IsReset())
        mPassChangedCB();
}

void VAOInterleaved::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpRasterPass.reset();
}
