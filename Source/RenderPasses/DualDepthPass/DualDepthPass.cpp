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
    const std::string kDepthFormat = "depthFormat";
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
        if (key == kDepthFormat) setDepthBufferFormat(value);
        else logWarning("Unknown field '" + key + "' in a DepthPass dictionary");
    }
}

Dictionary DualDepthPass::getScriptingDictionary()
{
    Dictionary d;
    d[kDepthFormat] = mDepthFormat;
    return d;
}

RenderPassReflection DualDepthPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    ResourceFormat stagingFormat = ResourceFormat::R32Float;
    if(mDepthFormat == ResourceFormat::D16Unorm) stagingFormat = ResourceFormat::R16Unorm;

    reflector.addOutput(kDepth, "Depth-buffer").bindFlags(Resource::BindFlags::DepthStencil).format(mDepthFormat);
    reflector.addOutput(kDepth2, "2nd Depth-buffer").bindFlags(Resource::BindFlags::DepthStencil).format(mDepthFormat);
    // unfortunately one can not write into a depth buffer as unordered access view
    reflector.addInternal(kInteralDepth, "staging resource for depth buffer").bindFlags(Resource::BindFlags::UnorderedAccess).format(stagingFormat);
    return reflector;
}

void DualDepthPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pDepth = renderData[kDepth]->asTexture();
    const auto& pDepth2 = renderData[kDepth2]->asTexture();

    // clear both depth textures
    pRenderContext->clearDsv(pDepth->getDSV().get(), 1, 0);
    pRenderContext->clearDsv(pDepth2->getDSV().get(), 1, 0);

    //mpFbo->attachDepthStencilTarget(pDepth2);
    mpFbo->attachDepthStencilTarget(pDepth);
    mpState->setFbo(mpFbo);

    // TODO bind pDepth as UAV

    if (mpScene) mpScene->rasterize(pRenderContext, mpState.get(), mpVars.get(), mCullMode);
}

void DualDepthPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    if (mpScene) mpState->getProgram()->addDefines(mpScene->getSceneDefines());
    mpVars = GraphicsVars::create(mpState->getProgram()->getReflector());
}

DualDepthPass& DualDepthPass::setDepthBufferFormat(ResourceFormat format)
{
    if (isDepthStencilFormat(format) == false)
    {
        logWarning("DepthPass buffer format must be a depth-stencil format");
    }
    else
    {
        mDepthFormat = format;
        mPassChangedCB();
    }
    return *this;
}

DualDepthPass& DualDepthPass::setDepthStencilState(const DepthStencilState::SharedPtr& pDsState)
{
    mpState->setDepthStencilState(pDsState);
    return *this;
}

static const Gui::DropdownList kDepthFormats =
{
    { (uint32_t)ResourceFormat::D16Unorm, "D16Unorm"},
    { (uint32_t)ResourceFormat::D32Float, "D32Float" },
    { (uint32_t)ResourceFormat::D24UnormS8, "D24UnormS8" },
    { (uint32_t)ResourceFormat::D32FloatS8X24, "D32FloatS8X24" },
};

void DualDepthPass::renderUI(Gui::Widgets& widget)
{
    uint32_t depthFormat = (uint32_t)mDepthFormat;
    if (widget.dropdown("Buffer Format", kDepthFormats, depthFormat)) setDepthBufferFormat(ResourceFormat(depthFormat));
}
