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

    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kExponent = "exponent";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(SimpleSSAO::kInfo, SimpleSSAO::create);
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
    return d;
}

void SimpleSSAO::setRadius(float r)
{
    mData.radius = r;
    mData.negInvRsq = -1.0f / (r * r);
    mDirty = true;
}

void SimpleSSAO::setDepthMode(DepthMode m)
{
    mDepthMode = m;
    mpPass->getProgram()->addDefine("DEPTH_MODE", std::to_string(uint32_t(m)));
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
        else logWarning("Unknown field '" + key + "' in a SimpleSSAO dictionary");
    }

    setDepthMode(mDepthMode);
    setRadius(mData.radius);

    mpNoiseTexture = genNoiseTexture();
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

    if (widget.slider("Depth Bias", mData.NdotVBias, 0.0f, 0.5f)) mDirty = true;
    if (widget.slider("Power Exponent", mData.powerExponent, 1.0f, 4.0f)) mDirty = true;
    uint32_t depthMode = uint32_t(mDepthMode);
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode))
        setDepthMode(DepthMode(depthMode));
}

Texture::SharedPtr SimpleSSAO::genNoiseTexture()
{
    std::vector<uint32_t> data;
    data.resize(4u * 4u);

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < data.size(); i++)
    {
        // Random directions on the XY plane
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        auto r1 = glm::linearRand(0.0f, 1.0f);
        auto r2 = glm::linearRand(0.0f, 1.0f);
        data[i] = glm::packSnorm4x8(float4(sin(theta), cos(theta), r1, r2));
    }

    return Texture::create2D(4, 4, ResourceFormat::RGBA8Snorm, 1, 1, data.data());
}
