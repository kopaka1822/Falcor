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
#include "ML_HBAO.h"
#include <glm/gtc/random.hpp>
#include "../SSAO/scissors.h"

const RenderPass::Info ML_HBAO::kInfo { "ML_HBAO", "HBAO combined with machine learning heuristic" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regSSAO(pybind11::module& m)
{
    pybind11::class_<ML_HBAO, RenderPass, ML_HBAO::SharedPtr> pass(m, "ML_HBAO");
    pass.def_property("sampleRadius", &ML_HBAO::getSampleRadius, &ML_HBAO::setSampleRadius);
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(ML_HBAO::kInfo, ML_HBAO::create);
    ScriptBindings::registerBinding(regSSAO);
}

namespace
{
    const std::string kEnabled = "enabled";
    const std::string kRadius = "radius";
    const std::string kGuardBand = "guardBand";

    const std::string kAmbientMap = "ambientMap";
    const std::string kDepth = "depth";
    const std::string kNormals = "normals";
    const std::string kMaterialData = "materialData";

    const std::string kSSAOShader = "RenderPasses/ML_HBAO/Raster.ps.slang";

    static const int NOISE_SIZE = 4; // in each dimension: 4x4
}

ML_HBAO::ML_HBAO()
    :
    RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);
    setSampleRadius(mData.radius);

    mpAOFbo = Fbo::create();
}

ML_HBAO::SharedPtr ML_HBAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pSSAO = SharedPtr(new ML_HBAO());
    for (const auto& [key, value] : dict)
    {
        if (key == kEnabled) pSSAO->mEnabled = value;
        else if (key == kRadius) pSSAO->mData.radius = value;
        else if (key == kGuardBand) pSSAO->mGuardBand = value;
        else logWarning("Unknown field '" + key + "' in a ML_HBAO dictionary");
    }
    return pSSAO;
}

Dictionary ML_HBAO::getScriptingDictionary()
{
    Dictionary dict;
    dict[kEnabled] = mEnabled;
    dict[kRadius] = mData.radius;
    dict[kGuardBand] = mGuardBand;
    return dict;
}

RenderPassReflection ML_HBAO::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kMaterialData, "Material Data").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);

    return reflector;
}

void ML_HBAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setNoiseTexture();

    mDirty = true; // texture size may have changed => reupload data
    mpSSAOPass.reset();
}

void ML_HBAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pMaterialData = renderData[kMaterialData]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pCamera = mpScene->getCamera().get();

    if (mEnabled)
    {
        if (mClearTexture)
        {
            pRenderContext->clearTexture(pAoDst.get(), float4(0.0f));
            mClearTexture = false;
        }

        if (!mpSSAOPass)
        {
            // program defines
            Program::DefineList defines;
            defines.add(mpScene->getSceneDefines());

            mpSSAOPass = FullScreenPass::create(kSSAOShader, defines);
            mpSSAOPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
            mDirty = true;
        }

        if (mDirty)
        {
            // redefine defines

            // bind static resources
            mData.noiseScale = float2(pDepth->getWidth(), pDepth->getHeight()) / float2(NOISE_SIZE, NOISE_SIZE);
            mpSSAOPass["StaticCB"].setBlob(mData);
            mDirty = false;
        }

        // bind dynamic resources
        auto var = mpSSAOPass->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);

        pCamera->setShaderData(mpSSAOPass["PerFrameCB"]["gCamera"]);
        mpSSAOPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // Update state/vars
        mpSSAOPass["gNoiseSampler"] = mpNoiseSampler;
        mpSSAOPass["gTextureSampler"] = mpTextureSampler;
        mpSSAOPass["gDepthTex"] = pDepth;
        mpSSAOPass["gNoiseTex"] = mpNoiseTexture;
        mpSSAOPass["gNormalTex"] = pNormals;
        mpSSAOPass["gMaterialData"] = pMaterialData;

        // Generate AO
        mpAOFbo->attachColorTarget(pAoDst, 0);
        setGuardBandScissors(*mpSSAOPass->getState(), renderData.getDefaultTextureDims(), mGuardBand);
        mpSSAOPass->execute(pRenderContext, mpAOFbo, false);
    }
    else // ! enabled
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
    }
}

void ML_HBAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
}

void ML_HBAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256)) mClearTexture = true;

    float radius = mData.radius;
    if (widget.var("Sample Radius", radius, 0.01f, FLT_MAX, 0.01f)) setSampleRadius(radius);

    if (widget.var("Power Exponent", mData.exponent, 1.0f, 4.0f, 0.1f)) mDirty = true;
}

void ML_HBAO::setSampleRadius(float radius)
{
    mData.radius = radius;
    mDirty = true;
}

void ML_HBAO::setNoiseTexture()
{
    mDirty = true;

    std::vector<uint8_t> data;
    data.resize(NOISE_SIZE * NOISE_SIZE);

    // https://en.wikipedia.org/wiki/Ordered_dithering
    const float ditherValues[] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };

    for (uint32_t i = 0; i < data.size(); i++)
    {
        data[i] = uint8_t(ditherValues[i] / 16.0f * 255.0f);
    }

    mpNoiseTexture = Texture::create2D(NOISE_SIZE, NOISE_SIZE, ResourceFormat::R8Unorm, 1, 1, data.data());
}
