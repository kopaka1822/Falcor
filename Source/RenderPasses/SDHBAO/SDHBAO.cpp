/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
#include "SDHBAO.h"
#include "../Utils/GuardBand/guardband.h"
#include <random>
#include "RenderGraph/RenderGraph.h"

namespace
{
    const char kDesc[] = "HBAO Plus von NVIDIA with interleaved texture access";

    const std::string kDepth = "depth";

    const std::string kNormal = "normals";

    const std::string kAoStencil = "aomask";
    const std::string kInternalRayMin = "internalRayMin";
    const std::string kInternalRayMax = "internalRayMax";


    const std::string kAmbientMap = "ambientMap";

    const std::string kProgram = "RenderPasses/SDHBAO/HBAO1.slang";
    const std::string kProgram2 = "RenderPasses/SDHBAO/HBAO2.slang";

    const Gui::DropdownList kDepthModeDropdown =
    {
        { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
        { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
    };

    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kDepthBias = "depthBias";
    const std::string kExponent = "exponent";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, SDHBAO>();
}

ref<SDHBAO> SDHBAO::create(ref<Device> pDevice, const Properties& props)
{
    auto pass = make_ref<SDHBAO>(pDevice, props);
    return pass;
}

SDHBAO::SDHBAO(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    for (const auto& [key, value] : props)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kDepthMode) mDepthMode = value;
        else if (key == kDepthBias) mData.NdotVBias = value;
        else if (key == kExponent) mData.powerExponent = value;
        else logWarning("Unknown field '" + key + "' in a HBAOPlus dictionary");
    }

    mpPass = ComputePass::create(mpDevice, kProgram);
    mpPass2 = ComputePass::create(mpDevice, kProgram2);
    // create sampler
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(mpDevice, samplerDesc);

    setDepthMode(mDepthMode);
    setRadius(mData.radius);

    mNoiseTexture = genNoiseTexture();
}

Properties SDHBAO::getProperties() const
{
    Properties d;
    d[kRadius] = mData.radius;
    d[kDepthMode] = mDepthMode;
    d[kDepthBias] = mData.NdotVBias;
    d[kExponent] = mData.powerExponent;
    return d;
}

RenderPassReflection SDHBAO::reflect(const CompileData& compileData)
{
    // set correct size of output resource
    auto srcWidth = compileData.defaultTexDims.x;
    auto srcHeight = compileData.defaultTexDims.y;

    auto dstWidth = (srcWidth + 4 - 1) / 4;
    auto dstHeight = (srcHeight + 4 - 1) / 4;
    auto quarterGuard = mData.sdGuard / 4;

    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "linear-depth (deinterleaved version)").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 1, 1, 16);

    reflector.addInput(kNormal, "normals").bindFlags(ResourceBindFlags::ShaderResource);//.texture2D(0, 0, 1, 1, 16);

    // ao mask and rayMin/rayMax texures
    reflector.addOutput(kAoStencil, "internal ao mask").format(ResourceFormat::R32Uint).texture2D(dstWidth, dstHeight, 1, 1, 16); // for 32 samples per pixel
    reflector.addOutput(kInternalRayMin, "internal ray min").format(ResourceFormat::R32Int).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dstWidth + quarterGuard * 2, dstHeight + quarterGuard * 2);
    reflector.addOutput(kInternalRayMax, "internal ray max").format(ResourceFormat::R32Int).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dstWidth + quarterGuard * 2, dstHeight + quarterGuard * 2);

    reflector.addOutput(kAmbientMap, "ambient occlusion (deinterleaved)").bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource)
        .texture2D(dstWidth, dstHeight, 1, 1, 16).format(ResourceFormat::RG8Unorm);

    return reflector;
}

void SDHBAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mDirty = true;

    Properties sdDict;
    sdDict["SampleCount"] = 4; // paper N
    sdDict["MaxCount"] = 8; // paper MAX_COUNT
    sdDict["CullMode"] = RasterizerState::CullMode::Back;
    sdDict["AlphaTest"] = true;
    sdDict["RayInterval"] = true;
    sdDict["normalize"] = true;
    sdDict["StoreNormals"] = false;
    sdDict["Jitter"] = true;
    sdDict["GuardBand"] = mData.sdGuard / 4;
    auto pStochasticDepthPass = RenderPass::create("StochasticDepthMapRT", mpDevice, sdDict);
    mpStochasticDepthGraph = RenderGraph::create(mpDevice, "Stochastic Depth");
    mpStochasticDepthGraph->addPass(pStochasticDepthPass, "StochasticDepthMap");
    mpStochasticDepthGraph->markOutput("StochasticDepthMap.stochasticDepth");
    mpStochasticDepthGraph->setScene(mpScene);
    mStochLastSize = uint2(0);
}

void SDHBAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepthIn = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormal]->asTexture();
    auto pAmbientOut = renderData[kAmbientMap]->asTexture();
    auto pAoMask = renderData[kAoStencil]->asTexture();

    auto pInternalRayMin = renderData[kInternalRayMin]->asTexture();
    auto pInternalRayMax = renderData[kInternalRayMax]->asTexture();

    if (!mEnabled)
    {
        // clear and return
        pRenderContext->clearTexture(pAmbientOut.get(), float4(1.0f));
        return;
    }

    auto vars = mpPass->getRootVar();
    auto vars2 = mpPass2->getRootVar();

    if (mDirty)
    {
        // static data
        mData.resolution = float2(renderData.getDefaultTextureDims().x, renderData.getDefaultTextureDims().y);
        mData.invResolution = float2(1.0f) / mData.resolution;
        mData.noiseScale = mData.resolution / 4.0f; // noise texture is 4x4 resolution
        mData.quarterResolution = float2(pDepthIn->getWidth(), pDepthIn->getHeight());
        mData.invQuarterResolution = float2(1.0f) / float2(pDepthIn->getWidth(), pDepthIn->getHeight());
        vars["StaticCB"].setBlob(mData);
        vars2["StaticCB"].setBlob(mData);

        vars["gTextureSampler"] = mpTextureSampler;
        vars2["gTextureSampler"] = mpTextureSampler;
        mDirty = false;
    }

    auto pCamera = mpScene->getCamera().get();

    pCamera->setShaderData(vars["PerFrameCB"]["gCamera"]);
    pCamera->setShaderData(vars2["PerFrameCB"]["gCamera"]);
    vars["gNormalTex"] = pNormal;
    vars2["gNormalTex"] = pNormal;

    auto& dict = renderData.getDictionary();
    auto guardBand = dict.getValue("guardBand", 0);
    auto quarterGuard = guardBand / 4;

    {
        FALCOR_PROFILE(pRenderContext, "AO 1");

        // rayMin/rayMax setup
        if (mDepthMode == DepthMode::StochasticDepth)
        {
            FALCOR_PROFILE(pRenderContext, "Clear RayMinMax");
            pRenderContext->clearUAV(pInternalRayMax->getUAV().get(), uint4(0u));
            pRenderContext->clearUAV(pInternalRayMin->getUAV().get(), uint4(asuint(std::numeric_limits<float>::max())));
        }

        vars["gRayMinAccess"] = pInternalRayMin;
        vars["gRayMaxAccess"] = pInternalRayMax;
        vars["PerFrameCB"]["guardBand"] = guardBand;

        // render
        for (int sliceIndex = 0; sliceIndex < 16; ++sliceIndex)
        {
            vars["gAmbientOut"].setUav(pAmbientOut->getUAV(0, sliceIndex, 1));
            vars["gAoMask"].setUav(pAoMask->getUAV(0, sliceIndex, 1));
            vars["gDepthTexQuarter"].setSrv(pDepthIn->getSRV(0, 1, sliceIndex, 1));
            vars["PerFrameCB"]["Rand"] = mNoiseTexture[sliceIndex];
            vars["PerFrameCB"]["quarterOffset"] = uint2(sliceIndex % 4, sliceIndex / 4);
            vars["PerFrameCB"]["sliceIndex"] = sliceIndex;

            mpPass->execute(pRenderContext, pDepthIn->getWidth() - quarterGuard * 2, pDepthIn->getHeight() - quarterGuard * 2);
        }
    }

    if (mDepthMode != DepthMode::StochasticDepth) return;

    mpStochasticDepthGraph->setInput("StochasticDepthMap.rayMin", pInternalRayMin);
    mpStochasticDepthGraph->setInput("StochasticDepthMap.rayMax", pInternalRayMax);

    auto stochSize = uint2(pDepthIn->getWidth(), pDepthIn->getHeight()) + uint2(mData.sdGuard / 4 * 2);
    if (any(mStochLastSize != stochSize))
    {
        auto stochFbo = Fbo::create2D(mpDevice, stochSize.x, stochSize.y, ResourceFormat::R32Float);
        mpStochasticDepthGraph->onResize(stochFbo.get());
        mStochLastSize = stochSize;
    }
    mpStochasticDepthGraph->execute(pRenderContext);
    auto pStochasticDepthMap = mpStochasticDepthGraph->getOutput("StochasticDepthMap.stochasticDepth")->asTexture();

    // second AO pass
    {
        FALCOR_PROFILE(pRenderContext, "AO 2");

        vars2["gsDepthTex"] = pStochasticDepthMap;
        vars2["PerFrameCB"]["guardBand"] = guardBand;

        // render
        for (int sliceIndex = 0; sliceIndex < 16; ++sliceIndex)
        {
            vars2["gAmbientOut"].setUav(pAmbientOut->getUAV(0, sliceIndex, 1));
            vars2["gDepthTexQuarter"].setSrv(pDepthIn->getSRV(0, 1, sliceIndex, 1));
            vars2["gAO1"].setSrv(pAmbientOut->getSRV(0, 1, sliceIndex, 1));
            vars2["gAoMask"].setSrv(pAoMask->getSRV(0, 1, sliceIndex, 1));
            vars2["PerFrameCB"]["Rand"] = mNoiseTexture[sliceIndex];
            vars2["PerFrameCB"]["quarterOffset"] = uint2(sliceIndex % 4, sliceIndex / 4);
            vars2["PerFrameCB"]["sliceIndex"] = sliceIndex;

            mpPass2->execute(pRenderContext, pDepthIn->getWidth() - quarterGuard * 2, pDepthIn->getHeight() - quarterGuard * 2);
        }
    }
}

void SDHBAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    float radius = mData.radius;
    if (widget.var("Radius", radius, 0.01f, FLT_MAX, 0.01f))
        setRadius(radius);

    if (widget.slider("Depth Bias", mData.NdotVBias, 0.0f, 0.5f)) mDirty = true;
    if (widget.slider("Power Exponent", mData.powerExponent, 1.0f, 4.0f)) mDirty = true;
    uint32_t depthMode = uint32_t(mDepthMode);
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode))
        setDepthMode(DepthMode(depthMode));
}


void SDHBAO::setRadius(float r)
{
    mData.radius = r;
    mData.negInvRsq = -1.0f / (r * r);
    mDirty = true;
}

void SDHBAO::setDepthMode(DepthMode m)
{
    mDepthMode = m;
    mpPass->getProgram()->addDefine("DEPTH_MODE", std::to_string(uint32_t(m)));
}

std::vector<float4> SDHBAO::genNoiseTexture()
{
    std::vector<float4> data;
    data.resize(4u * 4u);

    // https://en.wikipedia.org/wiki/Ordered_dithering
    //const float ditherValues[] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };

    auto linearRand = [](float min, float max)
        {
            static std::mt19937 generator(0);
            std::uniform_real_distribution<float> distribution(min, max);
            return distribution(generator);
        };

    for (uint32_t i = 0; i < data.size(); i++)
    {
        // Random directions on the XY plane
        auto theta = linearRand(0.0f, 2.0f * 3.141f);
        //auto theta = ditherValues[i] / 16.0f * 2.0f * glm::pi<float>();
        auto r1 = linearRand(0.0f, 1.0f);
        auto r2 = linearRand(0.0f, 1.0f);
        data[i] = float4(sin(theta), cos(theta), r1, r2);
    }

    return data;
}
