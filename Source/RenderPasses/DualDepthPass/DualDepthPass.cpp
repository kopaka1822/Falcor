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
    const std::string kDsvUavProgram = "RenderPasses/DualDepthPass/DsvUavPass.slang";
    const std::string kDepthPeelProgram1 = "RenderPasses/DualDepthPass/DepthPeel1.slang";
    const std::string kDepthPeelProgram2 = "RenderPasses/DualDepthPass/DepthPeel2.slang";
    const std::string kUavProgram = "RenderPasses/DualDepthPass/UavPass.slang";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string kInteralDepth = "internalDepth";
    const std::string kInteralDepth2 = "internalDepth2";

    const Gui::DropdownList kCullModeList =
    {
        { (uint32_t)RasterizerState::CullMode::None, "None" },
        { (uint32_t)RasterizerState::CullMode::Back, "Back" },
        { (uint32_t)RasterizerState::CullMode::Front, "Front" },
    };

    const Gui::DropdownList kVariantList =
    {
        {(uint32_t)DualDepthPass::Variant::DepthAndUav, "DsvUav"},
        //{(uint32_t)DualDepthPass::Variant::UavOnly, "UavOnly"},
        {(uint32_t)DualDepthPass::Variant::DepthPeel, "DepthPeel"},
    };
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
    {
        Program::Desc desc;
        desc.addShaderLibrary(kDsvUavProgram).psEntry("main");
        GraphicsProgram::SharedPtr pProgram = GraphicsProgram::create(desc);
        mpDsvUavState = GraphicsState::create();
        mpDsvUavState->setProgram(pProgram);
    }
    mpFbo = Fbo::create();

    {
        Program::Desc desc;
        desc.addShaderLibrary(kDepthPeelProgram1).psEntry("main");
        mpDepthPeelState1 = GraphicsState::create();
        mpDepthPeelState1->setProgram(GraphicsProgram::create(desc));
    }

    {
        Program::Desc desc;
        desc.addShaderLibrary(kDepthPeelProgram2).psEntry("main");
        mpDepthPeelState2 = GraphicsState::create();
        mpDepthPeelState2->setProgram(GraphicsProgram::create(desc));
    }

    {
        Program::Desc desc;
        desc.addShaderLibrary(kUavProgram).psEntry("main");
        mpUavState = GraphicsState::create();
        mpUavState->setProgram(GraphicsProgram::create(desc));
    }

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

    reflector.addOutput(kDepth, "Depth-buffer").bindFlags(Resource::BindFlags::AllDepthViews).format(depthFormat);
    reflector.addOutput(kDepth2, "2nd Depth-buffer").bindFlags(Resource::BindFlags::AllDepthViews).format(depthFormat);
    // unfortunately one can not write into a depth buffer as unordered access view
    reflector.addInternal(kInteralDepth, "staging resource for depth buffer").bindFlags(Resource::BindFlags::UnorderedAccess).format(stagingFormat);
    reflector.addInternal(kInteralDepth2, "2nd staging resource for depth buffer").bindFlags(Resource::BindFlags::UnorderedAccess).format(stagingFormat);
    return reflector;
}

void DualDepthPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    const auto& pDepth = renderData[kDepth]->asTexture();
    const auto& pDepth2 = renderData[kDepth2]->asTexture();
    const auto& pInternalDepth = renderData[kInteralDepth]->asTexture();
    const auto& pInternalDepth2 = renderData[kInteralDepth2]->asTexture();

    auto dsv1 = pDepth->getDSV();
    auto dsv2 = pDepth2->getDSV();
    auto depthUav = pInternalDepth->getUAV();
    auto depthUav2 = pInternalDepth2->getUAV();

    // clear both depth textures
    //pRenderContext->clearDsv(dsv1.get(), 1, 0);

    if (mVariant == Variant::DepthAndUav)
    {
        {
            PROFILE("clear textures");
            pRenderContext->clearDsv(dsv2.get(), 1.0f, 0, true, false);
            pRenderContext->clearUAV(depthUav.get(), float4(1.0f));
        }

        mpFbo->attachDepthStencilTarget(pDepth2);
        mpDsvUavState->setFbo(mpFbo);

        // bind the uav
        auto var = mpDsvUavVars->getRootVar();
        var["primaryDepth"] = pInternalDepth;

        // rasterize depth
        if (mpScene) mpScene->rasterize(pRenderContext, mpDsvUavState.get(), mpDsvUavVars.get(), mCullMode);

        // copy uav to dsv
        {
            PROFILE("copy depth to dsv");
            pRenderContext->copySubresource(pDepth.get(), 0, pInternalDepth.get(), 0);
        }
    }
    else if (mVariant == Variant::DepthPeel)
    {
        {
            PROFILE("clear textures");
            pRenderContext->clearDsv(dsv1.get(), 1.0f, 0, true, false);
            pRenderContext->clearDsv(dsv2.get(), 1.0f, 0, true, false);
        }

        {
            PROFILE("first layer");
            // render depth normally
            mpFbo->attachDepthStencilTarget(pDepth);
            mpDepthPeelState1->setFbo(mpFbo);
            if (mpScene) mpScene->rasterize(pRenderContext, mpDepthPeelState1.get(), mpDepthPeelVars1.get(), mCullMode);
        }

        {
            PROFILE("second layer");
            // render second layer with first layer as input
            auto var = mpDepthPeelVars2->getRootVar();
            var["prevDepth"] = pDepth;
            mpFbo->attachDepthStencilTarget(pDepth2);
            mpDepthPeelState2->setFbo(mpFbo);
            if (mpScene) mpScene->rasterize(pRenderContext, mpDepthPeelState2.get(), mpDepthPeelVars2.get(), mCullMode);
        }
    }
    else if(mVariant == Variant::UavOnly)
    {
        {
            PROFILE("clear textures");
            pRenderContext->clearUAV(depthUav.get(), float4(1.0f));
            pRenderContext->clearUAV(depthUav2.get(), float4(1.0f));
            pRenderContext->clearDsv(dsv2.get(), 1.0f, 0, true, false);
        }

        mpFbo->attachDepthStencilTarget(pDepth2);
        mpUavState->setFbo(mpFbo);

        // bind the uav
        auto var = mpUavVars->getRootVar();
        var["primaryDepth"] = pInternalDepth;
        var["secondaryDepth"] = pInternalDepth2;

        // rasterize depth
        if (mpScene) mpScene->rasterize(pRenderContext, mpUavState.get(), mpUavVars.get(), mCullMode);

        // copy uav to dsv
        {
            PROFILE("copy depth to dsv");
            pRenderContext->copySubresource(pDepth.get(), 0, pInternalDepth.get(), 0);
            // depth 2 already contains the correct depth
            //pRenderContext->copySubresource(pDepth2.get(), 0, pInternalDepth2.get(), 0);
        }
    }
}

void DualDepthPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    if (mpScene)
    {
        mpDsvUavState->getProgram()->addDefines(mpScene->getSceneDefines());
        mpDepthPeelState1->getProgram()->addDefines(mpScene->getSceneDefines());
        mpDepthPeelState2->getProgram()->addDefines(mpScene->getSceneDefines());
        mpUavState->getProgram()->addDefines(mpScene->getSceneDefines());
    }

    mpDsvUavVars = GraphicsVars::create(mpDsvUavState->getProgram()->getReflector());
    mpDepthPeelVars1 = GraphicsVars::create(mpDepthPeelState1->getProgram()->getReflector());
    mpDepthPeelVars2 = GraphicsVars::create(mpDepthPeelState2->getProgram()->getReflector());
    mpUavVars = GraphicsVars::create(mpUavState->getProgram()->getReflector());
}

DualDepthPass& DualDepthPass::setDepthStencilState(const DepthStencilState::SharedPtr& pDsState)
{
    mpDsvUavState->setDepthStencilState(pDsState);
    return *this;
}


void DualDepthPass::renderUI(Gui::Widgets& widget)
{
    uint32_t cullMode = (uint32_t)mCullMode;
    if (widget.dropdown("Cull mode", kCullModeList, cullMode))
        mCullMode = (RasterizerState::CullMode)cullMode;

    uint32_t variant = (uint32_t)mVariant;
    if (widget.dropdown("Variant", kVariantList, variant))
    {
        mVariant = (Variant)variant;
        mPassChangedCB();
    }
}
