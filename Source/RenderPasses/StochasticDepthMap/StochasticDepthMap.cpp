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
    const std::string kDepthIn = "depthMap";
    const std::string ksDepth = "stochasticDepth";
    const std::string ksLinearDepth = "linearSDepth";
    const std::string kDebug = "debugOutput";
    const std::string kProgramFile = "RenderPasses/StochasticDepthMap/StochasticDepth.ps.slang";
    const std::string kDebugFile = "RenderPasses/StochasticDepthMap/DebugLayer.ps.slang";
    const std::string kLinearizeFile = "RenderPasses/StochasticDepthMap/Linearize.ps.slang";
    const std::string kSampleCount = "SampleCount";
    const std::string kAlpha = "Alpha";
    const std::string kCullMode = "CullMode";

    const Gui::DropdownList kCullModeList =
    {
        { (uint32_t)RasterizerState::CullMode::None, "None" },
        { (uint32_t)RasterizerState::CullMode::Back, "Back" },
        { (uint32_t)RasterizerState::CullMode::Front, "Front" },
    };

    const Gui::DropdownList kSampleCountList =
    {
        { (uint32_t)1, "1" },
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

// code from stochastic-depth ambient occlusion:

// Based on Pascal's Triangle, 
// from https://www.geeksforgeeks.org/binomial-coefficient-dp-9/
static int Binomial(int n, int k)
{
    std::vector<int> C(k + 1);
    C[0] = 1;  // nC0 is 1 

    for (int i = 1; i <= n; i++)
    {
        // Compute next row of pascal triangle using 
        // the previous row 
        for (int j = std::min(i, k); j > 0; j--)
            C[j] = C[j] + C[j - 1];
    }
    return C[k];
}

static uint8_t count_bits(uint32_t v)
{
    uint8_t bits = 0;
    for (; v; ++bits) { v &= v - 1; }
    return bits;
}

void generateStratifiedLookupTable(int n, std::vector<int>& indices, std::vector<uint32_t>& lookUpTable) {

    uint32_t maxEntries = uint32_t(std::pow(2, n));
    //std::vector<int> indices(n + 1);
    //std::vector<uint32_t> lookUpTable(maxEntries);
    indices.resize(n + 1);
    lookUpTable.resize(maxEntries);

    // Generate index list
    indices[0] = 0;
    for (int i = 1; i <= n; i++) {
        indices[i] = Binomial(n, i - 1) + indices[i - 1];
    }

    // Generate lookup table
    std::vector<int> currentIndices(indices);
    lookUpTable[0] = 0;
    for (uint32_t i = 1; i < maxEntries; i++) {
        int popCount = count_bits(i);
        int index = currentIndices[popCount];
        lookUpTable[index] = i;
        currentIndices[popCount]++;
    }
}

StochasticDepthMap::SharedPtr StochasticDepthMap::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new StochasticDepthMap(dict));
    return pPass;
}

StochasticDepthMap::StochasticDepthMap(const Dictionary& dict)
{
    Program::DefineList defines;
    defines.add("NUM_SAMPLES", std::to_string(mSampleCount));
    defines.add("SAMPLE_INDEX", std::to_string(mDebugLayer));
    defines.add("ALPHA", std::to_string(mAlpha));

    Program::Desc desc;
    desc.addShaderLibrary(kProgramFile).psEntry("main");
    GraphicsProgram::SharedPtr pProgram = GraphicsProgram::create(desc, defines);
    mpState = GraphicsState::create();
    mpState->setProgram(pProgram);
    mpFbo = Fbo::create();

    mpDebugPass = FullScreenPass::create(kDebugFile, defines);
    mpDebugFbo = Fbo::create();

    mpLinearizePass = FullScreenPass::create(kLinearizeFile);
    mpLinearizeFbo = Fbo::create();

    parseDictionary(dict);
}

void StochasticDepthMap::parseDictionary(const Dictionary& dict)
{
    for (const auto& [key, value] : dict)
    {
        if (key == kSampleCount) mSampleCount = value;
        else if (key == kAlpha) mAlpha = value;
        else if (key == kCullMode) mCullMode = value;
        else logWarning("Unknown field '" + key + "' in a DepthPass dictionary");
    }
}

std::string StochasticDepthMap::getDesc() { return kDesc; }

Dictionary StochasticDepthMap::getScriptingDictionary()
{
    Dictionary d;
    d[kSampleCount] = mSampleCount;
    d[kAlpha] = mAlpha;
    d[kCullMode] = mCullMode;
    return d;
}

RenderPassReflection StochasticDepthMap::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepthIn, "non-linear (primary) depth map").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(ksDepth, "stochastic depths").bindFlags(ResourceBindFlags::AllDepthViews).format(Falcor::ResourceFormat::D32Float).texture2D(0, 0, mSampleCount);
    reflector.addOutput(ksLinearDepth, "stochastic depths linearized").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget).format(Falcor::ResourceFormat::R32Float).texture2D(0, 0, mSampleCount).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addOutput(kDebug, "single msaa layer").bindFlags(ResourceBindFlags::AllColorViews).format(Falcor::ResourceFormat::R32Float).texture2D(0, 0, 1).flags(RenderPassReflection::Field::Flags::Optional);
    return reflector;
}

void StochasticDepthMap::compile(RenderContext* pContext, const CompileData& compileData)
{
    mpState->getProgram()->addDefine("NUM_SAMPLES", std::to_string(mSampleCount));
    mpState->getProgram()->addDefine("ALPHA", std::to_string(mAlpha));

    // always sample at pixel centers for our msaa resource
    static std::array<Fbo::SamplePosition, 16> samplePos = {};
    mpFbo->setSamplePositions(mSampleCount, 1, samplePos.data());

    // generate data for stratified sampling
    std::vector<int> indices;
    std::vector<uint32_t> lookUpTable;
    generateStratifiedLookupTable(mSampleCount, indices, lookUpTable);

    mpStratifiedIndices = Buffer::createStructured(sizeof(indices[0]), uint32_t(indices.size()), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, indices.data(), false);
    mpStratifiedLookUpBuffer = Buffer::createStructured(sizeof(lookUpTable[0]), uint32_t(lookUpTable.size()), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, lookUpTable.data(), false);

    if (mNormalizeDepths) mpLinearizePass->addDefine("NORMALIZE");
    else mpLinearizePass->removeDefine("NORMALIZE");
}

void StochasticDepthMap::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pDepthIn = renderData[kDepthIn]->asTexture();
    auto psDepths = renderData[ksDepth]->asTexture();
    Texture::SharedPtr psLinearDepths;
    if (renderData[ksLinearDepth]) psLinearDepths = renderData[ksLinearDepth]->asTexture();
    Texture::SharedPtr pDebug;
    if (renderData[kDebug]) pDebug = renderData[kDebug]->asTexture();

    auto pCamera = mpScene->getCamera();

    {
        PROFILE("Non-Linear Depths");
        // rasterize non-linear depths
        mpFbo->attachDepthStencilTarget(psDepths);
        mpState->setFbo(mpFbo);
        pRenderContext->clearDsv(psDepths->getDSV().get(), 1.0f, 0, true, false);
        auto var = mpVars->getRootVar();
        var["stratifiedIndices"] = mpStratifiedIndices;
        var["stratifiedLookUpTable"] = mpStratifiedLookUpBuffer;
        var["depthBuffer"] = pDepthIn;

        if (mpScene) mpScene->rasterize(pRenderContext, mpState.get(), mpVars.get(), mCullMode);
    }

    // create linear depths
    if (psLinearDepths)
    {
        PROFILE("Linearize");
        mpLinearizeFbo->attachColorTarget(psLinearDepths, 0);
        mpLinearizePass["sdepth"] = psDepths;

        // set camera data
        float zNear = pCamera->getNearPlane();
        float zFar = pCamera->getFarPlane();
        if (mLastZNear != zNear || mLastZFar != zFar)
        {
            mpLinearizePass["CameraCB"]["zNear"] = zNear;
            mpLinearizePass["CameraCB"]["zFar"] = zFar;
            mLastZNear = zNear;
            mLastZFar = zFar;
        }

        mpLinearizePass->execute(pRenderContext, mpLinearizeFbo);
    }

    // if debug, resolve a single layer onto the debug texture
    if (pDebug)
    {
        PROFILE("Debug");
        mpDebugPass->addDefine("NUM_SAMPLES", std::to_string(psDepths->getSampleCount()));
        mpDebugPass->addDefine("SAMPLE_INDEX", std::to_string(mDebugLayer));

        mpDebugFbo->attachColorTarget(pDebug, 0);
        // bind linear depth if available, otherwise non linear
        if(psLinearDepths) mpDebugPass["sdepth"] = psLinearDepths;
        else mpDebugPass["sdepth"] = psDepths;
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

    if (widget.var("Alpha", mAlpha, 0.0f, 1.0f, 0.01f))
        mPassChangedCB();

    widget.var("Debug layer", mDebugLayer, 0u, mSampleCount - 1u);
    if (widget.checkbox("Normalize Linear Depths", mNormalizeDepths))
        mPassChangedCB();
}

void StochasticDepthMap::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    if (mpScene) mpState->getProgram()->addDefines(mpScene->getSceneDefines());
    mpVars = GraphicsVars::create(mpState->getProgram()->getReflector());
}
