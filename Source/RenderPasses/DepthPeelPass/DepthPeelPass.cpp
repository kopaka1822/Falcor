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
#include "DepthPeelPass.h"


namespace
{
    const char kDesc[] = "Peels one depth layer";

    const std::string kDepthIn = "depth";
    const std::string kDepthOut = "depth2";

    const std::string kDepthPeelProgram = "RenderPasses/DepthPeelPass/DepthPeel.slang";

    const std::string kCullMode = "cullMode";
    const std::string kDepthFormat = "depthFormat";

    const Gui::DropdownList kCullModeList =
    {
        { (uint32_t)RasterizerState::CullMode::None, "None" },
        { (uint32_t)RasterizerState::CullMode::Back, "Back" },
        { (uint32_t)RasterizerState::CullMode::Front, "Front" },
    };
}

const RenderPass::Info DepthPeelPass::kInfo{ "DepthPeelPass", kDesc };

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(DepthPeelPass::kInfo, DepthPeelPass::create);
}

DepthPeelPass::SharedPtr DepthPeelPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new DepthPeelPass(dict));
    return pPass;
}

Dictionary DepthPeelPass::getScriptingDictionary()
{
    Dictionary d;
    d[kCullMode] = mCullMode;
    d[kDepthFormat] = mDepthFormat;
    return Dictionary();
}

DepthPeelPass::DepthPeelPass(const Dictionary& dict)
    :
    RenderPass(kInfo)
{
    mpFbo = Fbo::create();
    Program::Desc desc;
    desc.addShaderLibrary(kDepthPeelProgram).psEntry("main");
    GraphicsProgram::SharedPtr pProgram = GraphicsProgram::create(desc);
    mpDepthPeelState = GraphicsState::create();
    mpDepthPeelState->setProgram(pProgram);

    // parse dictionary
    for (const auto& [key, value] : dict)
    {
        if (key == kCullMode) mCullMode = value;
        else if (key == kDepthFormat) mDepthFormat = value;
        else logWarning("Unknown field '" + key + "' in a DepthPeelPass dictionary");
    }
}

RenderPassReflection DepthPeelPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    auto& inField = reflector.addInput(kDepthIn, "non-linearized depth").bindFlags(ResourceBindFlags::ShaderResource);
    auto& outField = reflector.addOutput(kDepthOut, "next layer of depth (non-linear)").format(mDepthFormat).bindFlags(ResourceBindFlags::AllDepthViews); // set backup depth format


    return reflector;
}

void DepthPeelPass::compile(RenderContext* pContext, const CompileData& compileData)
{
}

void DepthPeelPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pDepthIn = renderData[kDepthIn]->asTexture();
    auto pDepthOut = renderData[kDepthOut]->asTexture();

    // clear depth
    auto dsvOut = pDepthOut->getDSV();
    pRenderContext->clearDsv(dsvOut.get(), 1.0f, 0, true, false);

    if (!mpScene) return;

    // attach depth buffer
    mpFbo->attachDepthStencilTarget(pDepthOut);
    mpDepthPeelState->setFbo(mpFbo);

    // set primary depth
    auto var = mpDepthPeelVars->getRootVar();
    var["prevDepth"] = pDepthIn;

    // rasterize
    mpScene->rasterize(pRenderContext, mpDepthPeelState.get(), mpDepthPeelVars.get(), mCullMode);
}

void DepthPeelPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;

    if (!pScene) return;

    // set scene defines and obtain vars
    mpDepthPeelState->getProgram()->addDefines(pScene->getSceneDefines());
    mpDepthPeelState->getProgram()->setTypeConformances(pScene->getTypeConformances());
    mpDepthPeelVars = GraphicsVars::create(mpDepthPeelState->getProgram()->getReflector());
}

static const Gui::DropdownList kDepthFormats =
{
    { (uint32_t)ResourceFormat::D16Unorm, "D16Unorm"},
    { (uint32_t)ResourceFormat::D32Float, "D32Float" },
    //{ (uint32_t)ResourceFormat::D24UnormS8, "D24UnormS8" },
    //{ (uint32_t)ResourceFormat::D32FloatS8X24, "D32FloatS8X24" },
};

void DepthPeelPass::renderUI(Gui::Widgets& widget)
{
    uint32_t depthFormat = (uint32_t)mDepthFormat;
    if (widget.dropdown("Buffer Format", kDepthFormats, depthFormat))
    {
        mDepthFormat = ResourceFormat(depthFormat);
        mPassChangedCB();
    }

    uint32_t cullMode = (uint32_t)mCullMode;
    if (widget.dropdown("Cull mode", kCullModeList, cullMode))
        mCullMode = (RasterizerState::CullMode)cullMode;


}
