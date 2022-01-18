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
#include "DualDepthPass.h"


namespace
{
    const char kDesc[] = "Creates two depth-buffers using the scene's active camera";
    const std::string kProgramFile = "RenderPasses/DualDepthPass/DualDepthPass.ps.slang";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string kInteralDepth = "internalDepth";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("DualDepthPass", kDesc, DualDepthPass::create);
}

DualDepthPass::SharedPtr DualDepthPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new DualDepthPass(dict));
    return pPass;
}

DualDepthPass::DualDepthPass(const Dictionary& dict)
{
    Program::Desc desc;
    desc.addShaderLibrary(kProgramFile).psEntry("main");
    GraphicsProgram::SharedPtr pProgram = GraphicsProgram::create(desc);
    mpState = GraphicsState::create();
    mpState->setProgram(pProgram);
    mpFbo = Fbo::create();

    parseDictionary(dict);
}

std::string DualDepthPass::getDesc() { return kDesc; }

void DualDepthPass::parseDictionary(const Dictionary& dict)
{
    for (const auto& [key, value] : dict)
    {

    }
}

Dictionary DualDepthPass::getScriptingDictionary()
{
    Dictionary d;
    return d;
}

RenderPassReflection DualDepthPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    ResourceFormat depthFormat = ResourceFormat::D32Float;
    ResourceFormat stagingFormat = ResourceFormat::R32Float;

    reflector.addOutput(kDepth, "Depth-buffer").bindFlags(Resource::BindFlags::DepthStencil).format(depthFormat);
    reflector.addOutput(kDepth2, "2nd Depth-buffer").bindFlags(Resource::BindFlags::DepthStencil).format(depthFormat);
    // unfortunately one can not write into a depth buffer as unordered access view
    reflector.addInternal(kInteralDepth, "staging resource for depth buffer").bindFlags(Resource::BindFlags::UnorderedAccess).format(stagingFormat);
    return reflector;
}

void DualDepthPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pDepth = renderData[kDepth]->asTexture();
    const auto& pDepth2 = renderData[kDepth2]->asTexture();
    const auto& pInternalDepth = renderData[kInteralDepth]->asTexture();

    //auto dsv1 = pDepth->getDSV();
    auto dsv2 = pDepth2->getDSV();
    auto depthUav = pInternalDepth->getUAV();

    // clear both depth textures
    //pRenderContext->clearDsv(dsv1.get(), 1, 0);
    pRenderContext->clearDsv(dsv2.get(), 1.0f, 0, true, false);
    pRenderContext->clearUAV(depthUav.get(), float4(1.0f));
    
    mpFbo->attachDepthStencilTarget(pDepth2);
    mpState->setFbo(mpFbo);

    // bind the uav
    auto var = mpVars->getRootVar();
    var["primaryDepth"] = pInternalDepth;

    // rasterize depth
    if (mpScene) mpScene->rasterize(pRenderContext, mpState.get(), mpVars.get(), mCullMode);

    // copy uav to dsv
    pRenderContext->copySubresource(pDepth.get(), 0, pInternalDepth.get(), 0);
}

void DualDepthPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    if (mpScene) mpState->getProgram()->addDefines(mpScene->getSceneDefines());
    mpVars = GraphicsVars::create(mpState->getProgram()->getReflector());
}

DualDepthPass& DualDepthPass::setDepthStencilState(const DepthStencilState::SharedPtr& pDsState)
{
    mpState->setDepthStencilState(pDsState);
    return *this;
}


void DualDepthPass::renderUI(Gui::Widgets& widget)
{
    
}
