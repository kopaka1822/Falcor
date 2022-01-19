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
#include "StochasticDepthMap.h"
#include <array>

namespace
{
    const char kDesc[] = "Captures mutliple depth layers stochastically inside a msaa texture";
    const std::string ksDepth = "stochasticDepth";
    const std::string kDebug = "debugOutput";
    const std::string kProgramFile = "RenderPasses/StochasticDepthMap/StochasticDepth.ps.slang";
    const std::string kDebugFile = "RenderPasses/StochasticDepthMap/DebugLayer.ps.slang";
    const std::string kSampleCount = "SampleCount";
    const std::string kLinearize = "LinearizeDepth";

    const Gui::DropdownList kCullModeList =
    {
        { (uint32_t)RasterizerState::CullMode::None, "None" },
        { (uint32_t)RasterizerState::CullMode::Back, "Back" },
        { (uint32_t)RasterizerState::CullMode::Front, "Front" },
    };

    const Gui::DropdownList kSampleCountList =
    {
        { (uint32_t)2, "2" },
        { (uint32_t)4, "4" },
        { (uint32_t)8, "8" },
        { (uint32_t)16, "16" },
    };
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("StochasticDepthMap", kDesc, StochasticDepthMap::create);
}

StochasticDepthMap::SharedPtr StochasticDepthMap::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new StochasticDepthMap(dict));
    return pPass;
}

StochasticDepthMap::StochasticDepthMap(const Dictionary& dict)
{
    Program::Desc desc;
    desc.addShaderLibrary(kProgramFile).psEntry("main");
    GraphicsProgram::SharedPtr pProgram = GraphicsProgram::create(desc);
    mpState = GraphicsState::create();
    mpState->setProgram(pProgram);
    mpFbo = Fbo::create();

    Program::DefineList defines;
    defines.add("NUM_SAMPLES", "4");
    defines.add("SAMPLE_INDEX", "0");
    mpDebugPass = FullScreenPass::create(kDebugFile, defines);
    mpDebugFbo = Fbo::create();

    parseDictionary(dict);
}

void StochasticDepthMap::parseDictionary(const Dictionary& dict)
{
    for (const auto& [key, value] : dict)
    {
        if (key == kSampleCount) mSampleCount = value;
        else if (key == kLinearize) mLinearDepth = value;
        else logWarning("Unknown field '" + key + "' in a DepthPass dictionary");
    }
}

std::string StochasticDepthMap::getDesc() { return kDesc; }

Dictionary StochasticDepthMap::getScriptingDictionary()
{
    Dictionary d;
    d[kSampleCount] = mSampleCount;
    d[kLinearize] = mLinearDepth;
    return d;
}

RenderPassReflection StochasticDepthMap::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addOutput(ksDepth, "stochastic depths. Values will always be in range [0, 1] even after linearization").bindFlags(ResourceBindFlags::AllDepthViews).format(Falcor::ResourceFormat::D32Float).texture2D(0, 0, mSampleCount);
    reflector.addOutput(kDebug, "single msaa layer").bindFlags(ResourceBindFlags::AllColorViews).format(Falcor::ResourceFormat::R32Float).texture2D(0, 0, 1).flags(RenderPassReflection::Field::Flags::Optional);
    return reflector;
}

void StochasticDepthMap::compile(RenderContext* pContext, const CompileData& compileData)
{
    if (mLinearDepth) mpState->getProgram()->addDefine("LINEARIZE_DEPTHS");
    else mpState->getProgram()->removeDefine("LINEARIZE_DEPTHS");

    // always sample at pixel centers for our msaa resource
    static std::array<Fbo::SamplePosition, 16> samplePos = {};
    mpFbo->setSamplePositions(mSampleCount, 1, samplePos.data());
}

void StochasticDepthMap::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto psDepths = renderData[ksDepth]->asTexture();
    Texture::SharedPtr pDebug;
    if (renderData[kDebug]) pDebug = renderData[kDebug]->asTexture();

    auto pCamera = mpScene->getCamera();

    mpFbo->attachDepthStencilTarget(psDepths);
    mpState->setFbo(mpFbo);
    pRenderContext->clearDsv(psDepths->getDSV().get(), 1.0f, 0, true, false);

    // set consant buffer data
    float zNear = pCamera->getNearPlane();
    float zFar = pCamera->getFarPlane();
    if (mLastZNear != zNear || mLastZFar != zFar)
    {
        auto var = mpVars->getRootVar();

        var["CameraCB"]["zNear"] = zNear;
        var["CameraCB"]["zFar"] = zFar;
        mLastZNear = zNear;
        mLastZFar = zFar;
    }
    
    if (mpScene) mpScene->rasterize(pRenderContext, mpState.get(), mpVars.get(), mCullMode);

    // if debug, resolve a single layer onto the debug texture
    if (pDebug)
    {
        mpDebugPass->addDefine("NUM_SAMPLES", std::to_string(psDepths->getSampleCount()));
        mpDebugPass->addDefine("SAMPLE_INDEX", std::to_string(mDebugLayer));

        mpDebugFbo->attachColorTarget(pDebug, 0);
        mpDebugPass["sdepth"] = psDepths;
        mpDebugPass->execute(pRenderContext, mpDebugFbo);
    }
}

void StochasticDepthMap::renderUI(Gui::Widgets& widget)
{
    uint32_t cullMode = (uint32_t)mCullMode;
    if (widget.dropdown("Cull mode", kCullModeList, cullMode))
        mCullMode = (RasterizerState::CullMode)cullMode;

    if (widget.dropdown("Sample Count", kSampleCountList, mSampleCount))
        mPassChangedCB(); // reload pass (recreate texture)

    widget.var("Debug layer", mDebugLayer, 0u, mSampleCount - 1u);
}

void StochasticDepthMap::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    if (mpScene) mpState->getProgram()->addDefines(mpScene->getSceneDefines());
    mpVars = GraphicsVars::create(mpState->getProgram()->getReflector());
}
