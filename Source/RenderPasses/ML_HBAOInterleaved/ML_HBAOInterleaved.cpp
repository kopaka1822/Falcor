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
#include "ML_HBAOInterleaved.h"
#include <glm/gtc/random.hpp>
#include "../SSAO/scissors.h"

const RenderPass::Info ML_HBAOInterleaved::kInfo { "ML_HBAOInterleaved", "HBAO with machine learning and interleaved rendering" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regSSAO(pybind11::module& m)
{
    pybind11::class_<ML_HBAOInterleaved, RenderPass, ML_HBAOInterleaved::SharedPtr> pass(m, "ML_HBAOInterleaved");
    pass.def_property("sampleRadius", &ML_HBAOInterleaved::getSampleRadius, &ML_HBAOInterleaved::setSampleRadius);
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(ML_HBAOInterleaved::kInfo, ML_HBAOInterleaved::create);
    ScriptBindings::registerBinding(regSSAO);
}

namespace
{
    const std::string kEnabled = "enabled";
    const std::string kRadius = "radius";
    const std::string kGuardBand = "guardBand";

    const std::string kAmbientMap = "ambientMap";
    const std::string kAmbientArray = "ambientArray";
    const std::string kDepth = "depth";
    const std::string kDepthArray = "depthArray";
    const std::string kNormals = "normals";
    const std::string kMaterialData = "doubleSided";
    const std::string kMaterialArray = "materialArray";

    // interleaved data

    const std::string kSSAOShader = "RenderPasses/ML_HBAOInterleaved/Raster.ps.slang";
}

ML_HBAOInterleaved::ML_HBAOInterleaved()
    :
RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);
    setSampleRadius(mData.radius);

    mpAOFbo = Fbo::create();
    
    mpDeinterleave = DeinterleaveTexture::create();
    mpDeinterleaveMat = DeinterleaveTexture::create();
    mpDeinterleaveMat->setInputFormat(ResourceFormat::R8Uint);
    mpInterleave = InterleaveTexture::create();

    mNoiseTexture = genNoiseTexture();
}

ML_HBAOInterleaved::SharedPtr ML_HBAOInterleaved::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pSSAO = SharedPtr(new ML_HBAOInterleaved());
    for (const auto& [key, value] : dict)
    {
        if (key == kEnabled) pSSAO->mEnabled = value;
        else if (key == kRadius) pSSAO->mData.radius = value;
        else if (key == kGuardBand) pSSAO->mGuardBand = value;
        else logWarning("Unknown field '" + key + "' in a ML_HBAO dictionary");
    }
    return pSSAO;
}

Dictionary ML_HBAOInterleaved::getScriptingDictionary()
{
    Dictionary dict;
    dict[kEnabled] = mEnabled;
    dict[kRadius] = mData.radius;
    dict[kGuardBand] = mGuardBand;
    return dict;
}

RenderPassReflection ML_HBAOInterleaved::reflect(const CompileData& compileData)
{
    // set correct size of output resource
    auto srcWidth = compileData.defaultTexDims.x;
    auto srcHeight = compileData.defaultTexDims.y;

    auto dstWidth = (srcWidth + 4 - 1) / 4;
    auto dstHeight = (srcHeight + 4 - 1) / 4;

    auto inputFormat = ResourceFormat::R32Float;
    auto edge = compileData.connectedResources.getField(kDepth);
    if (edge)
    {
        inputFormat = edge->getFormat();
        if (isDepthFormat(inputFormat))
        {
            inputFormat = depthToRendertargetFormat(inputFormat);
        }
        mReady = true;
    }

    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kMaterialData, "Material Data (double sided flag)").bindFlags(ResourceBindFlags::ShaderResource);
    // internal interleave textures:
    reflector.addInternal(kDepthArray, "Depth Array").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget)
        .texture2D(dstWidth, dstHeight, 1, 1, 16).format(inputFormat);
    reflector.addInternal(kMaterialArray, "Material Array").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget)
        .texture2D(dstWidth, dstHeight, 1, 1, 16).format(ResourceFormat::R8Uint);

    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);

    reflector.addInternal(kAmbientArray, "Ambient Array").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget)
        .texture2D(dstWidth, dstHeight, 1, 1, 16).format(ResourceFormat::R8Unorm);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(ResourceFormat::R8Unorm);
    return reflector;
}

void ML_HBAOInterleaved::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mDirty = true; // texture size may have changed => reupload data
    mpSSAOPass.reset();
}

void ML_HBAOInterleaved::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepthIn = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pMaterialDataIn = renderData[kMaterialData]->asTexture();
    auto pDepthArray = renderData[kDepthArray]->asTexture();
    auto pMaterialArray = renderData[kMaterialArray]->asTexture();
    auto pAoArray = renderData[kAmbientArray]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pCamera = mpScene->getCamera().get();

    if (!mEnabled)
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
        return;
    }

    if (mClearTexture)
    {
        pRenderContext->clearTexture(pAoArray.get(), float4(0.0f));
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
        mpSSAOPass["StaticCB"].setBlob(mData);
        mDirty = false;
    }

    // do deinterleave
    {
        FALCOR_PROFILE("DeinterleaveDepth");
        mpDeinterleave->execute(pRenderContext, pDepthIn, pDepthArray);
    }

    {
        FALCOR_PROFILE("DeinterleaveMaterial");
        mpDeinterleaveMat->execute(pRenderContext, pMaterialDataIn, pMaterialArray);
    }


    // bind dynamic resources
    auto var = mpSSAOPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);

    pCamera->setShaderData(mpSSAOPass["PerFrameCB"]["gCamera"]);
    mpSSAOPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

    // Update state/vars
    mpSSAOPass["gTextureSampler"] = mpTextureSampler;
    mpSSAOPass["gNormalTex"] = pNormals;


    // Generate AO
    mpAOFbo->attachColorTarget(pAoArray, 0);
    setGuardBandScissors(*mpSSAOPass->getState(), renderData.getDefaultTextureDims() / 4u, mGuardBand / 4);
    
    mpSSAOPass->execute(pRenderContext, mpAOFbo, false);

    {
        FALCOR_PROFILE("AO");
        for (UINT sliceIndex = 0; sliceIndex < 16; ++sliceIndex)
        {
            mpSSAOPass["gDepthTexQuarter"].setSrv(pDepthArray->getSRV(0, 1, sliceIndex, 1));
            mpSSAOPass["gMaterialDataQuarter"].setSrv(pMaterialArray->getSRV(0, 1, sliceIndex, 1));
            mpSSAOPass["PerFrameCB"]["Rand"] = mNoiseTexture[sliceIndex];
            mpSSAOPass["PerFrameCB"]["quarterOffset"] = uint2(sliceIndex % 4, sliceIndex / 4);
            mpSSAOPass["PerFrameCB"]["sliceIndex"] = sliceIndex;

            mpSSAOPass->execute(pRenderContext, mpAOFbo, false);
        }
    }

    {
        FALCOR_PROFILE("InterleaveAO");
        mpInterleave->execute(pRenderContext, pAoArray, pAoDst);
    }
}

void ML_HBAOInterleaved::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
}

void ML_HBAOInterleaved::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256)) mClearTexture = true;

    float radius = mData.radius;
    if (widget.var("Sample Radius", radius, 0.01f, FLT_MAX, 0.01f)) setSampleRadius(radius);

    if (widget.var("Power Exponent", mData.exponent, 1.0f, 4.0f, 0.1f)) mDirty = true;
}

void ML_HBAOInterleaved::setSampleRadius(float radius)
{
    mData.radius = radius;
    mDirty = true;
}



std::vector<float> ML_HBAOInterleaved::genNoiseTexture()
{
    std::vector<float> data;
    data.resize(4 * 4);

    // https://en.wikipedia.org/wiki/Ordered_dithering
    const float ditherValues[] = { 0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f };

    for (uint32_t i = 0; i < data.size(); i++)
    {
        data[i] = ditherValues[i] / 16.0f;
    }

    return data;
}
