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
#include "VAONonInterleaved.h"

#include <glm/gtc/random.hpp>


namespace
{
    const char kDesc[] = "Optimized Volumetric Ambient Occlusion (Non-Interleaved)";
    
    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kUseRays = "useRays";
    const std::string kExponent = "exponent";

    const std::string kAmbientMap = "ao";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";

    const std::string kRasterShader = "RenderPasses/VAONonInterleaved/Raster.ps.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("VAONonInterleaved", kDesc, VAONonInterleaved::create);
}

VAONonInterleaved::SharedPtr VAONonInterleaved::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VAONonInterleaved(dict));
    return pPass;
}

std::string VAONonInterleaved::getDesc() { return kDesc; }

VAONonInterleaved::VAONonInterleaved(const Dictionary& dict)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    //samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();

    mpNoiseTexture = genNoiseTexture();
    initSampleDirections();

    for (const auto& [key, value] : dict)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kDepthMode) mDepthMode = value;
        else if (key == kUseRays) mUseRays = value;
        else if (key == kExponent) mData.exponent = value;
        else logWarning("Unknown field '" + key + "' in a VAONonInterleaved dictionary");
    }
}

Dictionary VAONonInterleaved::getScriptingDictionary()
{
    Dictionary d;
    d[kRadius] = mData.radius;
    d[kDepthMode] = mDepthMode;
    d[kUseRays] = mUseRays;
    d[kExponent] = mData.exponent;
    return d;
}

RenderPassReflection VAONonInterleaved::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(ksDepth, "Linear Stochastic Depth Map").texture2D(0, 0, 0).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    return reflector;
}

void VAONonInterleaved::compile(RenderContext* pContext, const CompileData& compileData)
{
    mDirty = true; // resolution changed probably
    mpRasterPass.reset(); // recompile raster pass
}

void VAONonInterleaved::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    auto psDepth = renderData[ksDepth]->asTexture();

    if (!mEnabled)
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
        return;
    }

    if(!mpRasterPass) // this needs to be deferred because it needs the scene defines to compile
    {
        Program::DefineList defines;
        defines.add("USE_RAYS", mUseRays ? "true" : "false");
        defines.add("DEPTH_MODE", std::to_string(uint32_t(mDepthMode)));
        defines.add("MSAA_SAMPLES", std::to_string(psDepth->getSampleCount()));
        defines.add(mpScene->getSceneDefines());
        mpRasterPass = FullScreenPass::create(kRasterShader, defines);
        mDirty = true;
    }

    if(mDirty)
    {
        // update data
        float2 resolution = float2(renderData.getDefaultTextureDims().x, renderData.getDefaultTextureDims().y);
        mData.resolution = resolution;
        mData.invResolution = float2(1.0f) / resolution;
        mData.noiseScale = resolution / 4.0f; // noise texture is 4x4 resolution
        mpRasterPass["StaticCB"].setBlob(mData);

        mpRasterPass["gNoiseSampler"] = mpNoiseSampler;
        mpRasterPass["gTextureSampler"] = mpTextureSampler;
        mpRasterPass["gNoiseTex"] = mpNoiseTexture;

        mDirty = false;
    }

    mpFbo->attachColorTarget(pAoDst, 0);

    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
    mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
    mpRasterPass["gDepthTex"] = pDepth;
    mpRasterPass["gDepthTex2"] = pDepth2;
    mpRasterPass["gNormalTex"] = pNormal;
    mpRasterPass["gsDepthTex"] = psDepth;

    mpRasterPass->execute(pRenderContext, mpFbo);
}

const Gui::DropdownList kDepthModeDropdown =
{
    { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
    { (uint32_t)DepthMode::DualDepth, "DualDepth" },
    { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
};

void VAONonInterleaved::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    uint32_t depthMode = (uint32_t)mDepthMode;
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode)) {
        mDepthMode = (DepthMode)depthMode;
        mpRasterPass.reset(); // changed defines
    }

    if (widget.checkbox("Use Rays", mUseRays)) mpRasterPass.reset(); // changed defines

    if (widget.var("Sample Radius", mData.radius, 0.01f, FLT_MAX, 0.01f)) mDirty = true;

    if (widget.slider("Power Exponent", mData.exponent, 1.0f, 4.0f)) mDirty = true;
}

void VAONonInterleaved::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpRasterPass.reset(); // new scene defines => recompile
}

Texture::SharedPtr VAONonInterleaved::genNoiseTexture()
{
    std::vector<uint8_t> data;
    data.resize(16u);
    /*
    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < 16u; i++)
    {
        // Random directions on the XY plane
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        data[i] = uint16_t(glm::packSnorm4x8(float4(sin(theta), cos(theta), 0.0f, 0.0f)));
    }

    return Texture::create2D(4u, 4u, ResourceFormat::RG8Snorm, 1, 1, data.data());*/

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < 16u; i++)
    {
        // random value in [0, 1]
        data[i] = uint8_t(glm::linearRand(0.0f, 256.0f));
    }

    return Texture::create2D(4u, 4u, ResourceFormat::R8Unorm, 1, 1, data.data());
}

void VAONonInterleaved::initSampleDirections()
{
    auto tst = radicalInverse(0);
    std::srand(5960372); // same seed for kernel
    for (uint32_t i = 0; i < NUM_DIRECTIONS; i++)
    {
        auto& s = mData.samples[i];
        float2 rand = float2((float)(i + 1) / (float)(NUM_DIRECTIONS + 1), radicalInverse(i + 1));
        float theta = rand.x * 2.0f * glm::pi<float>();
        float r = glm::sqrt(1.0f - glm::pow(rand.y, 2.0f / 3.0f));
        //s.x = r * sin(theta);
        //s.y = r * cos(theta);
        s.x = r;
    }
}
