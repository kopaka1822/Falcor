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
#include "RTHBAO.h"

#include <glm/gtc/random.hpp>

#include "../SSAO/scissors.h"

const RenderPass::Info RTHBAO::kInfo { "RTHBAO", "Screen-space ambient occlusion based on HBAO+ with ray tracing" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regSSAO(pybind11::module& m)
{
    pybind11::class_<RTHBAO, RenderPass, RTHBAO::SharedPtr> pass(m, "RTHBAO");
    pass.def_property("enabled", &RTHBAO::getEnabled, &RTHBAO::setEnabled);
    pass.def_property("sampleRadius", &RTHBAO::getSampleRadius, &RTHBAO::setSampleRadius);
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RTHBAO::kInfo, RTHBAO::create);
    ScriptBindings::registerBinding(regSSAO);
}

namespace
{
    const Gui::DropdownList kDepthModeDropdown =
    {
        { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
        //{ (uint32_t)DepthMode::DualDepth, "DualDepth" },
        //{ (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
        { (uint32_t)DepthMode::Raytraced, "Raytraced" }
    };

    const std::string kEnabled = "enabled";
    const std::string kKernelSize = "kernelSize";
    const std::string kNoiseSize = "noiseSize";
    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    //const std::string kColorMap = "colorMap";
    const std::string kGuardBand = "guardBand";
    const std::string kThickness = "thickness";

    const std::string kAmbientMap = "ambientMap";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";
    const std::string kInstanceID = "instanceID";

    const std::string kInternalRasterDepth = "iRasterDepth";
    const std::string kInternalRayDepth = "iRayDepth";
    const std::string kInternalInstanceID = "iInstanceID";

    const std::string kSSAOShader = "RenderPasses/RTHBAO/Raster.ps.slang";

    static const int NOISE_SIZE = 4; // in each dimension: 4x4
}

RTHBAO::RTHBAO()
    :
    RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);
    setSampleRadius(mData.radius);

    mpAOFbo = Fbo::create();
}

ResourceFormat RTHBAO::getAmbientMapFormat() const
{
    return ResourceFormat::R8Unorm;
}

RTHBAO::SharedPtr RTHBAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pSSAO = SharedPtr(new RTHBAO());
    Dictionary blurDict;
    for (const auto& [key, value] : dict)
    {
        if (key == kEnabled) pSSAO->mEnabled = value;
        else if (key == kRadius) pSSAO->mData.radius = value;
        else if (key == kDepthMode) pSSAO->mDepthMode = value;
        else if (key == kGuardBand) pSSAO->mGuardBand = value;
        else if (key == kThickness) pSSAO->mData.thickness = value;
        else logWarning("Unknown field '" + key + "' in a RTHBAO dictionary");
    }
    return pSSAO;
}

Dictionary RTHBAO::getScriptingDictionary()
{
    Dictionary dict;
    dict[kEnabled] = mEnabled;
    dict[kRadius] = mData.radius;
    dict[kDepthMode] = mDepthMode;
    dict[kGuardBand] = mGuardBand;
    dict[kThickness] = mData.thickness;
    return dict;
}

RenderPassReflection RTHBAO::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kInstanceID, "Instance ID").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(ksDepth, "Linear Stochastic Depth Map").texture2D(0, 0, 0).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(getAmbientMapFormat());

    auto numSamples = mNumDirections * mNumSteps;
    reflector.addInternal(kInternalRasterDepth, "internal raster depth").texture2D(0, 0, 1, 1, numSamples)
        .bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::R32Float);
    reflector.addInternal(kInternalRayDepth, "internal raster depth").texture2D(0, 0, 1, 1, numSamples)
        .bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::R32Float);
    reflector.addInternal(kInternalInstanceID, "internal instance ID").texture2D(0, 0, 1, 1, numSamples)
        .bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::R8Uint);

    return reflector;
}

void RTHBAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setNoiseTexture();

    mDirty = true; // texture size may have changed => reupload data
    mpSSAOPass.reset();
}


void RTHBAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //pRenderContext->getLowLevelData()->getCommandList()->ResourceBarrier()
    //pRenderContext->resourceBarrier(, Resource::State::)


    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    auto pInstanceID = renderData[kInstanceID]->asTexture();
    Texture::SharedPtr pDepth2;
    if (renderData[kDepth2]) pDepth2 = renderData[kDepth2]->asTexture();
    else if (mDepthMode == DepthMode::DualDepth) mDepthMode = DepthMode::SingleDepth;
    Texture::SharedPtr psDepth;
    if (renderData[ksDepth]) psDepth = renderData[ksDepth]->asTexture();
    else if (mDepthMode == DepthMode::StochasticDepth) mDepthMode = DepthMode::SingleDepth;

    auto pInternalRasterDepth = renderData[kInternalRasterDepth]->asTexture();
    auto pInternalRayDepth = renderData[kInternalRayDepth]->asTexture();
    auto pInternalInstanceID = renderData[kInternalInstanceID]->asTexture();

    auto pCamera = mpScene->getCamera().get();
    //renderData["k"]->asBuffer();


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
            defines.add("DEPTH_MODE", std::to_string(uint32_t(mDepthMode)));
            if (psDepth) defines.add("MSAA_SAMPLES", std::to_string(psDepth->getSampleCount()));

            mpSSAOPass = FullScreenPass::create(kSSAOShader, defines);
            mpSSAOPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
            mDirty = true;

            mpSSAOPass["gRasterDepth"] = pInternalRasterDepth;
            mpSSAOPass["gRayDepth"] = pInternalRayDepth;
            mpSSAOPass["gInstanceIDOut"] = pInternalInstanceID;
        }

        if (mDirty)
        {
            // redefine defines
            mpSSAOPass->getProgram()->addDefine("NUM_DIRECTIONS", std::to_string(mNumDirections));
            mpSSAOPass->getProgram()->addDefine("NUM_STEPS", std::to_string(mNumSteps));
            
            // bind static resources
            mData.noiseScale = float2(pDepth->getWidth(), pDepth->getHeight()) / float2(NOISE_SIZE, NOISE_SIZE);
            mpSSAOPass["StaticCB"].setBlob(mData);
            mDirty = false;
        }

        // bind dynamic resources
        auto var = mpSSAOPass->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);

        pCamera->setShaderData(mpSSAOPass["PerFrameCB"]["gCamera"]);
        mpSSAOPass["PerFrameCB"]["frameIndex"] = mFrameIndex++;
        mpSSAOPass["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // Update state/vars
        mpSSAOPass["gNoiseSampler"] = mpNoiseSampler;
        mpSSAOPass["gTextureSampler"] = mpTextureSampler;
        mpSSAOPass["gDepthTex"] = pDepth;
        mpSSAOPass["gDepthTex2"] = pDepth2;
        mpSSAOPass["gsDepthTex"] = psDepth;
        mpSSAOPass["gNoiseTex"] = mpNoiseTexture;
        mpSSAOPass["gNormalTex"] = pNormals;
        mpSSAOPass["gInstanceID"] = pInstanceID;

        // clear uav targets
        pRenderContext->clearTexture(pInternalRasterDepth.get());
        pRenderContext->clearTexture(pInternalRayDepth->asTexture().get());
        //pRenderContext->clearTexture(pInternalInstanceID->asTexture().get());
        pRenderContext->clearUAV(pInternalInstanceID->asTexture()->getUAV().get(), uint4(0));

        // Generate AO
        mpAOFbo->attachColorTarget(pAoDst, 0);
        setGuardBandScissors(*mpSSAOPass->getState(), renderData.getDefaultTextureDims(), mGuardBand);
        mpSSAOPass->execute(pRenderContext, mpAOFbo, false);

        if (mSaveDepths)
        {
            // write sample information
            pInternalRasterDepth->captureToFile(0, -1, "raster.dds", Bitmap::FileFormat::DdsFile);
            pInternalRayDepth->captureToFile(0, -1, "ray.dds", Bitmap::FileFormat::DdsFile);
            pInternalInstanceID->captureToFile(0, -1, "instance.dds", Bitmap::FileFormat::DdsFile);

            // write pixel information
            pInstanceID->captureToFile(0, -1, "instance_center.dds", Bitmap::FileFormat::DdsFile);
            mSaveDepths = false;
        }
    }
    else // ! enabled
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
    }
}

void RTHBAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
}


void RTHBAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256)) mClearTexture = true;

    uint32_t depthMode = (uint32_t)mDepthMode;
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode)) {
        mDepthMode = (DepthMode)depthMode;
        mpSSAOPass.reset();
    }

    if (widget.var("Num Directions", mNumDirections, 1, 32)) mPassChangedCB();
    if (widget.var("Num Steps", mNumSteps, 1, 32)) mPassChangedCB();

    float radius = mData.radius;
    if (widget.var("Sample Radius", radius, 0.01f, FLT_MAX, 0.01f)) setSampleRadius(radius);

    if (widget.var("Thickness", mData.thickness, 0.0f, 1.0f, 0.1f))
    {
        mDirty = true;
        mData.exponent = glm::mix(1.6f, 1.0f, mData.thickness);
    }

    if (widget.var("Power Exponent", mData.exponent, 1.0f, 4.0f, 0.1f)) mDirty = true;

    if (widget.button("Save Depths")) mSaveDepths = true;
}

void RTHBAO::setSampleRadius(float radius)
{
    mData.radius = radius;
    mDirty = true;
}

void RTHBAO::setNoiseTexture()
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
