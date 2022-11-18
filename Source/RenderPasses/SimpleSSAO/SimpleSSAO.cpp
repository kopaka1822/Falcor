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
#include "SimpleSSAO.h"
#include "../SSAO/scissors.h"
#include <glm/gtc/random.hpp>

const RenderPass::Info SimpleSSAO::kInfo { "SimpleSSAO", "Old SSAO techniques" };

namespace
{
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";

    const std::string kNormal = "normals";
    const std::string kAmbientMap = "ambientMap";

    const std::string kProgram = "RenderPasses/SimpleSSAO/AO.slang";

    const Gui::DropdownList kDepthModeDropdown =
    {
        { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
        { (uint32_t)DepthMode::DualDepth, "DualDepth" },
        //{ (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
    };

    const Gui::DropdownList kAoTechniqueDropdown =
    {
        { (uint32_t)AO_Algorithm::Mittring07, "Mittring07" },
        { (uint32_t)AO_Algorithm::Filion08, "Filion08" },
        { (uint32_t)AO_Algorithm::HBAO08, "HBAO08" },
        { (uint32_t)AO_Algorithm::HBAOPlus16, "HBAOPlus16" },
        { (uint32_t)AO_Algorithm::VAO10, "VAO10" },
    };

    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kExponent = "exponent";
    const std::string kAoAlgorithm = "aoAlgorithm";
    const std::string kNumSamples = "numSamples";
}

static void regPyBinding(pybind11::module& m)
{
    pybind11::enum_<AO_Algorithm> algo(m, "AO_Algorithm");
    algo.value("Mittring07", AO_Algorithm::Mittring07);
    algo.value("Filion08", AO_Algorithm::Filion08);
    algo.value("HBAO08", AO_Algorithm::HBAO08);
    algo.value("HBAOPlus16", AO_Algorithm::HBAOPlus16);
    algo.value("VAO10", AO_Algorithm::VAO10);
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(SimpleSSAO::kInfo, SimpleSSAO::create);
    ScriptBindings::registerBinding(regPyBinding);
}

SimpleSSAO::SharedPtr SimpleSSAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new SimpleSSAO(dict));
    return pPass;
}

Dictionary SimpleSSAO::getScriptingDictionary()
{
    Dictionary d;
    d[kRadius] = mData.radius;
    d[kDepthMode] = mDepthMode;
    //d[kDepthBias] = mData.NdotVBias;
    d[kExponent] = mData.powerExponent;
    d[kAoAlgorithm] = mAoAlgorithm;
    d[kNumSamples] = mData.numSamples;
    return d;
}

void SimpleSSAO::setRadius(float r)
{
    mData.radius = r;
    mData.negInvRsq = -1.0f / (r * r);
    mDirty = true;
}

void SimpleSSAO::setNumSamples(int n)
{
    assert(n > 0);
    mData.numSamples = n;
    mDirty = true;
}

void SimpleSSAO::setDepthMode(DepthMode m)
{
    mDepthMode = m;
    mpPass->getProgram()->addDefine("DEPTH_MODE", std::to_string(uint32_t(m)));
}

void SimpleSSAO::setAoAlgorithm(AO_Algorithm a)
{
    mAoAlgorithm = a;
    mpPass->getProgram()->addDefine("AO_ALGORITHM", std::to_string(uint32_t(a)));
}

SimpleSSAO::SimpleSSAO(const Dictionary& dict) : RenderPass(kInfo)
{
    mpFbo = Fbo::create();
    mpPass = FullScreenPass::create(kProgram);
    // create sampler
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    // load scripting data
    for (const auto& [key, value] : dict)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kDepthMode) mDepthMode = value;
        //else if (key == kDepthBias) mData.NdotVBias = value;
        else if (key == kExponent) mData.powerExponent = value;
        else if (key == kAoAlgorithm) mAoAlgorithm = value;
        else logWarning("Unknown field '" + key + "' in a SimpleSSAO dictionary");
    }

    setDepthMode(mDepthMode);
    setRadius(mData.radius);
    setAoAlgorithm(mAoAlgorithm);

    mpNoiseTexture = genNoiseTexture();
    mpSpherePositions = genSpherePositions(mMaxSamples);
    mpDiscPositions = genDiscPositions(mMaxSamples);
}

RenderPassReflection SimpleSSAO::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "linear-depth").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "linear-depth2").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormal, "normals").bindFlags(ResourceBindFlags::ShaderResource);
    //reflector.addInput(ksDepth, "linearized stochastic depths").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 0);
    reflector.addOutput(kAmbientMap, "ambient occlusion").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::R8Unorm);
    return reflector;
}

void SimpleSSAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mDirty = true;
    mClearTexture = true;

    // static defines
    //auto sdepths = compileData.connectedResources.getField(ksDepth);
    //if (!sdepths) return;

    //mpPass->getProgram()->addDefine("MSAA_SAMPLES", std::to_string(sdepths->getSampleCount()));
}

void SimpleSSAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    //auto psDepth = renderData[ksDepth]->asTexture();
    auto pNormal = renderData[kNormal]->asTexture();
    auto pAmbient = renderData[kAmbientMap]->asTexture();

    if (!mEnabled)
    {
        // clear and return
        pRenderContext->clearTexture(pAmbient.get(), float4(1.0f));
        return;
    }

    if (mClearTexture)
    {
        pRenderContext->clearTexture(pAmbient.get(), float4(0.0f));
        mClearTexture = false;
    }

    if (mDirty)
    {
        // static data
        mData.resolution = float2(renderData.getDefaultTextureDims().x, renderData.getDefaultTextureDims().y);
        mData.invResolution = float2(1.0f) / mData.resolution;
        mData.noiseScale = mData.resolution / 4.0f; // noise texture is 4x4 resolution
        mpPass["StaticCB"].setBlob(mData);

        mpPass["gNoiseSampler"] = mpNoiseSampler;
        mpPass["gTextureSampler"] = mpTextureSampler;
        mpPass["gNoiseTex"] = mpNoiseTexture;
        mpPass["gSpherePositions"] = mpSpherePositions;
        mpPass["gDiscPositions"] = mpDiscPositions;
        mDirty = false;
    }

    auto pCamera = mpScene->getCamera().get();

    mpFbo->attachColorTarget(pAmbient, 0);
    pCamera->setShaderData(mpPass["PerFrameCB"]["gCamera"]);
    mpPass["gDepthTex"] = pDepth;
    mpPass["gDepthTex2"] = pDepth2;
    mpPass["gNormalTex"] = pNormal;
    //mpPass["gsDepthTex"] = psDepth;

    setGuardBandScissors(*mpPass->getState(), renderData.getDefaultTextureDims(), mGuardBand);
    mpPass->execute(pRenderContext, mpFbo, false);
}

void SimpleSSAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256))
        mClearTexture = true;

    float radius = mData.radius;
    if (widget.var("Radius", radius, 0.01f, FLT_MAX, 0.01f))
        setRadius(radius);

    int nSamples = mData.numSamples;
    if (widget.var("Num Samples", nSamples, 1, mMaxSamples))
        setNumSamples(nSamples);

    if (widget.slider("Depth Bias", mData.NdotVBias, 0.0f, 0.5f)) mDirty = true;
    if (widget.slider("Power Exponent", mData.powerExponent, 1.0f, 4.0f)) mDirty = true;
    uint32_t depthMode = uint32_t(mDepthMode);
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode))
        setDepthMode(DepthMode(depthMode));

    uint32_t aoTechnique = uint32_t(mAoAlgorithm);
    if (widget.dropdown("AO Algorithm", kAoTechniqueDropdown, aoTechnique))
        setAoAlgorithm((AO_Algorithm)aoTechnique);
}

Texture::SharedPtr SimpleSSAO::genNoiseTexture()
{
    std::vector<uint32_t> data;
    data.resize(4u * 4u);

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < data.size(); i++)
    {
        // 4 random floats
        data[i] = glm::packUnorm4x8(float4(
            glm::linearRand(0.0f, 1.0f),
            glm::linearRand(0.0f, 1.0f),
            glm::linearRand(0.0f, 1.0f),
            glm::linearRand(0.0f, 1.0f)
        ));
    }

    return Texture::create2D(4, 4, ResourceFormat::RGBA8Unorm, 1, 1, data.data());
}

Texture::SharedPtr SimpleSSAO::genSpherePositions(int nSamples)
{
    std::vector<uint32_t> data;
    data.resize(nSamples);
    std::srand(26353); // same seed
    for(uint i = 0; i < data.size(); i++)
    {
        // generate random point in sphere
        glm::float3 pos;
        do
        {
            pos.x = glm::linearRand(-1.0f, 1.0f);
            pos.y = glm::linearRand(-1.0f, 1.0f);
            pos.z = glm::linearRand(-1.0f, 1.0f);
        } while (length(pos) > 1.0f || length(pos) < 0.01f);

        data[i] = glm::packSnorm4x8(float4(pos, 0.0f));
    }

    return Texture::create1D(nSamples, ResourceFormat::RGBA8Snorm, 1, 1, data.data());
}

Texture::SharedPtr SimpleSSAO::genDiscPositions(int nSamples)
{
    std::vector<uint16_t> data;
    data.resize(nSamples);
    std::srand(26353); // same seed
    for (uint i = 0; i < data.size(); i++)
    {
        // generate random point in sphere
        glm::float2 pos;
        do
        {
            pos.x = glm::linearRand(-1.0f, 1.0f);
            pos.y = glm::linearRand(-1.0f, 1.0f);
        } while (length(pos) > 1.0f || length(pos) < 0.01f);

        data[i] = (uint16_t)glm::packSnorm4x8(float4(pos, 0.0f, 0.0f));
    }

    return Texture::create1D(nSamples, ResourceFormat::RG8Snorm, 1, 1, data.data());
}
