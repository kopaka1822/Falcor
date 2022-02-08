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
#include "VAONonInterleaved2.h"

#include <glm/gtc/random.hpp>


namespace
{
    const char kDesc[] = "Optimized Volumetric Ambient Occlusion (Non-Interleaved) 2nd pass";
    
    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kUseRays = "useRays";
    const std::string kExponent = "exponent";

    const std::string kAmbientMap = "ao";
    const std::string kAmbientMap2 = "ao2";
    const std::string kAoStencil = "stencil";
    const std::string kAccessStencil = "accessStencil";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";

    const std::string kRasterShader = "RenderPasses/VAONonInterleaved/Raster.ps.slang";
}

VAONonInterleaved2::SharedPtr VAONonInterleaved2::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VAONonInterleaved2(dict));
    return pPass;
}

std::string VAONonInterleaved2::getDesc() { return kDesc; }

VAONonInterleaved2::VAONonInterleaved2(const Dictionary& dict)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    //samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();

    mpNoiseTexture = genNoiseTexture();

    for (const auto& [key, value] : dict)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kDepthMode) mDepthMode = value;
        else if (key == kUseRays) mUseRays = value;
        else if (key == kExponent) mData.exponent = value;
        else logWarning("Unknown field '" + key + "' in a VAONonInterleaved dictionary");
    }
}

Dictionary VAONonInterleaved2::getScriptingDictionary()
{
    Dictionary d;
    d[kRadius] = mData.radius;
    d[kDepthMode] = mDepthMode;
    d[kUseRays] = mUseRays;
    d[kExponent] = mData.exponent;
    return d;
}

RenderPassReflection VAONonInterleaved2::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addInput(kAoStencil, "(Depth-) Stencil Buffer for the ao mask").format(ResourceFormat::D32FloatS8X24);
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(ksDepth, "Linear Stochastic Depth Map").texture2D(0, 0, 0).bindFlags(ResourceBindFlags::ShaderResource);

    reflector.addInputOutput(kAmbientMap, "Ambient Occlusion (primary)").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    return reflector;
}

void VAONonInterleaved2::compile(RenderContext* pContext, const CompileData& compileData)
{
    mDirty = true; // resolution changed probably
    mpRasterPass.reset(); // recompile raster pass
}

void VAONonInterleaved2::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    auto psDepth = renderData[ksDepth]->asTexture();

    auto pAoDst2 = renderData[kAmbientMap2]->asTexture();
    auto pStencil = renderData[kAoStencil]->asTexture();
    auto pAccessStencil = renderData[kAccessStencil]->asTexture();

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

    auto accessStencilUAV = pAccessStencil->getUAV(0);
    pRenderContext->clearUAV(accessStencilUAV.get(), uint4(0u));

    mpFbo->attachColorTarget(pAoDst, 0);
    mpFbo->attachColorTarget(pAoDst2, 1);
    mpFbo->attachColorTarget(pStencil, 2);

    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mpRasterPass["PerFrameCB"]["gCamera"]);
    mpRasterPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());
    mpRasterPass["gDepthTex"] = pDepth;
    mpRasterPass["gDepthTex2"] = pDepth2;
    mpRasterPass["gNormalTex"] = pNormal;
    mpRasterPass["gsDepthTex"] = psDepth;
    //mpRasterPass["gDepthAccess"] = pAccessStencil;
    mpRasterPass["gDepthAccess"].setUav(accessStencilUAV);

    mpRasterPass->execute(pRenderContext, mpFbo);
}

const Gui::DropdownList kDepthModeDropdown =
{
    { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
    { (uint32_t)DepthMode::DualDepth, "DualDepth" },
    { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
};

void VAONonInterleaved2::renderUI(Gui::Widgets& widget)
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

void VAONonInterleaved2::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpRasterPass.reset(); // new scene defines => recompile
}

Texture::SharedPtr VAONonInterleaved2::genNoiseTexture()
{
    std::vector<uint16_t> data;
    data.resize(16u);
    
    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < 16u; i++)
    {
        // Random directions on the XY plane
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        data[i] = uint16_t(glm::packSnorm4x8(float4(sin(theta), cos(theta), 0.0f, 0.0f)));
    }

    return Texture::create2D(4u, 4u, ResourceFormat::RG8Snorm, 1, 1, data.data());
}
