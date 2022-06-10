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
#include "VAO.h"

#include "scissors.h"
#include "glm/gtc/random.hpp"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regSSAO(pybind11::module& m)
{
    pybind11::class_<VAO, RenderPass, VAO::SharedPtr> pass(m, "VAO");
    pass.def_property("enabled", &VAO::getEnabled, &VAO::setEnabled);
    pass.def_property("kernelRadius", &VAO::getKernelSize, &VAO::setKernelSize);
    pass.def_property("distribution", &VAO::getDistribution, &VAO::setDistribution);
    pass.def_property("sampleRadius", &VAO::getSampleRadius, &VAO::setSampleRadius);

    pybind11::enum_<VAO::SampleDistribution> sampleDistribution(m, "SampleDistribution");
    sampleDistribution.value("Random", VAO::SampleDistribution::Random);
    sampleDistribution.value("VanDerCorput", VAO::SampleDistribution::VanDerCorput);
    sampleDistribution.value("Poisson", VAO::SampleDistribution::Poisson);

    pybind11::enum_<Falcor::DepthMode> depthMode(m, "DepthMode");
    depthMode.value("SingleDepth", Falcor::DepthMode::SingleDepth);
    depthMode.value("DualDepth", Falcor::DepthMode::DualDepth);
    depthMode.value("StochasticDepth", Falcor::DepthMode::StochasticDepth);
    depthMode.value("Raytraced", Falcor::DepthMode::Raytraced);
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(VAO::kInfo, VAO::create);
    ScriptBindings::registerBinding(regSSAO);
}

const RenderPass::Info VAO::kInfo = { "VAO", "Screen-space ambient occlusion. Can be used with and without a normal-map" };

namespace
{
    const Gui::DropdownList kDistributionDropdown =
    {
        { (uint32_t)VAO::SampleDistribution::Random, "Random" },
        { (uint32_t)VAO::SampleDistribution::VanDerCorput, "Uniform VanDerCorput" },
        { (uint32_t)VAO::SampleDistribution::Poisson, "Poisson" },
    };

    const Gui::DropdownList kDepthModeDropdown =
    {
        { (uint32_t)DepthMode::SingleDepth, "SingleDepth" },
        { (uint32_t)DepthMode::DualDepth, "DualDepth" },
        { (uint32_t)DepthMode::StochasticDepth, "StochasticDepth" },
        { (uint32_t)DepthMode::Raytraced, "Raytraced" }
    };

    const std::string kEnabled = "enabled";
    const std::string kKernelSize = "kernelSize";
    const std::string kNoiseSize = "noiseSize";
    const std::string kDistribution = "distribution";
    const std::string kRadius = "radius";
    const std::string kDepthMode = "depthMode";
    const std::string kColorMap = "colorMap";
    const std::string kGuardBand = "guardBand";
    const std::string kThickness = "thickness";

    const std::string kAmbientMap = "ambientMap";
    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string ksDepth = "stochasticDepth";
    const std::string kNormals = "normals";

    const std::string kSSAOShader = "RenderPasses/SSAO/SSAO.ps.slang";
}

VAO::VAO()
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

ResourceFormat VAO::getAmbientMapFormat() const
{
    if (mColorMap) return ResourceFormat::RGBA8Unorm;
    return ResourceFormat::R8Unorm;
}

VAO::SharedPtr VAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pSSAO = SharedPtr(new VAO);
    Dictionary blurDict;
    for (const auto& [key, value] : dict)
    {
        if (key == kEnabled) pSSAO->mEnabled = value;
        else if (key == kKernelSize) pSSAO->mKernelSize = value;
        else if (key == kNoiseSize) pSSAO->mNoiseSize = value;
        else if (key == kDistribution) pSSAO->mHemisphereDistribution = value;
        else if (key == kRadius) pSSAO->mData.radius = value;
        else if (key == kDepthMode) pSSAO->mDepthMode = value;
        else if (key == kGuardBand) pSSAO->mGuardBand = value;
        else if (key == kThickness) pSSAO->mData.thickness = value;
        else logWarning("Unknown field '" + key + "' in a VAO dictionary");
    }
    return pSSAO;
}

Dictionary VAO::getScriptingDictionary()
{
    Dictionary dict;
    dict[kEnabled] = mEnabled;
    dict[kKernelSize] = mKernelSize;
    dict[kNoiseSize] = mNoiseSize;
    dict[kRadius] = mData.radius;
    dict[kDistribution] = mHemisphereDistribution;
    dict[kDepthMode] = mDepthMode;
    dict[kGuardBand] = mGuardBand;
    dict[kThickness] = mData.thickness;
    return dict;
}

RenderPassReflection VAO::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "Linear Depth-buffer of second layer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(ksDepth, "Linear Stochastic Depth Map").texture2D(0, 0, 0).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(getAmbientMapFormat());
    
    return reflector;
}

void VAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setKernel();
    setNoiseTexture();

    mDirty = true; // texture size may have changed => reupload data
    mpSSAOPass.reset();
}

void VAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //pRenderContext->getLowLevelData()->getCommandList()->ResourceBarrier()
    //pRenderContext->resourceBarrier(, Resource::State::)


    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();
    Texture::SharedPtr pDepth2;
    if (renderData[kDepth2]) pDepth2 = renderData[kDepth2]->asTexture();
    else if (mDepthMode == DepthMode::DualDepth) mDepthMode = DepthMode::SingleDepth;
    Texture::SharedPtr psDepth;
    if (renderData[ksDepth]) psDepth = renderData[ksDepth]->asTexture();
    else if (mDepthMode == DepthMode::StochasticDepth) mDepthMode = DepthMode::SingleDepth;

    auto pCamera = mpScene->getCamera().get();
    //renderData["k"]->asBuffer();


    if(mEnabled)
    {
        if(mClearTexture)
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
            defines.add("KERNEL_SIZE", std::to_string(mKernelSize));
            if(mColorMap) defines.add("COLOR_MAP", "true");
            if (psDepth) defines.add("MSAA_SAMPLES", std::to_string(psDepth->getSampleCount()));

            mpSSAOPass = FullScreenPass::create(kSSAOShader, defines);
            mpSSAOPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
            mDirty = true;
        }

        if (mDirty)
        {
            // bind static resources
            mData.noiseScale = float2(pDepth->getWidth(), pDepth->getHeight()) / float2(mNoiseSize.x, mNoiseSize.y);
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

void VAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
}

void VAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if(!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256)) mClearTexture = true;

    if (widget.checkbox("Color Map", mColorMap)) mPassChangedCB();

    uint32_t depthMode = (uint32_t)mDepthMode;
    if (widget.dropdown("Depth Mode", kDepthModeDropdown, depthMode)) {
        mDepthMode = (DepthMode)depthMode;
        mpSSAOPass.reset();
    }

    uint32_t distribution = (uint32_t)mHemisphereDistribution;
    if (widget.dropdown("Kernel Distribution", kDistributionDropdown, distribution)) setDistribution(distribution);

    uint32_t size = mKernelSize;
    if (widget.var("Kernel Size", size, 1u, SSAOData::kMaxSamples)) setKernelSize(size);

    float radius = mData.radius;
    if (widget.var("Sample Radius", radius, 0.01f, FLT_MAX, 0.01f)) setSampleRadius(radius);

    if (widget.var("Thickness", mData.thickness, 0.0f, 1.0f, 0.1f))
    {
        mDirty = true;
        mData.exponent = glm::mix(1.6f, 1.0f, mData.thickness);
    }

    if (widget.var("Power Exponent", mData.exponent, 1.0f, 4.0f, 0.1f)) mDirty = true;

    
}

void VAO::setSampleRadius(float radius)
{
    mData.radius = radius;
    mDirty = true;
}

void VAO::setKernelSize(uint32_t kernelSize)
{
    kernelSize = glm::clamp(kernelSize, 1u, SSAOData::kMaxSamples);
    mKernelSize = kernelSize;
    setKernel();
    mPassChangedCB();
}

void VAO::setDistribution(uint32_t distribution)
{
    mHemisphereDistribution = (SampleDistribution)distribution;
    setKernel();
}

void VAO::setKernel()
{
    std::srand(5960372); // same seed for kernel
    int vanDerCorputOffset = mKernelSize; // (only correct for power of two numbers => offset 8 results in 1/16, 9/16, 5/16... which are 8 different uniformly dstributed numbers, see https://en.wikipedia.org/wiki/Van_der_Corput_sequence)
    bool isPowerOfTwo = std::_Popcount(uint32_t(vanDerCorputOffset)) == 1;//std::has_single_bit(uint32_t(vanDerCorputOffset));
    if (mHemisphereDistribution == SampleDistribution::VanDerCorput && !isPowerOfTwo)
        logWarning("VanDerCorput sequence only works properly if the sample count is a power of two!");

    if (mHemisphereDistribution == SampleDistribution::Poisson)
    {
        // brute force algorithm to generate poisson samples
        float r = 0.28f; // for kernelSize = 8
        if (mKernelSize >= 16) r = 0.19f;
        if (mKernelSize >= 24) r = 0.15f;
        if (mKernelSize >= 32) r = 0.13f;

        auto pow2 = [](float x) {return x * x; };

        uint i = 0; // current length of list
        uint cur_attempt = 0;
        while (i < mKernelSize)
        {
            i = 0; // reset list length
            const uint max_retries = 10000;
            uint cur_retries = 0;
            while (i < mKernelSize && cur_retries < max_retries)
            {
                cur_retries += 1;
                float2 point = float2(glm::linearRand(-1.0f, 1.0f), glm::linearRand(-1.0f, 1.0f));
                if (point.x * point.x + point.y * point.y > pow2(1.0f - r))
                    continue;

                bool too_close = false;
                for (uint j = 0; j < i; ++j)
                    if (pow2(point.x - mData.sampleKernel[j].x) + pow2(point.y - mData.sampleKernel[j].y) < pow2(2.0f * r))
                    {
                        too_close = true;
                        break;
                    }


                if (too_close) continue;

                mData.sampleKernel[i++] = float4(point.x, point.y, glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
            }

            std::cerr << "\rpoisson attempt " << ++cur_attempt;

            if (cur_attempt % 1000 == 0)
                r = r - 0.01f; // shrink radius every 1000 attempts
        }

        // succesfully found points

    }
    else // random or hammersly
    {
        std::string nums;
        for (uint32_t i = 0; i < mKernelSize; i++)
        {
            auto& s = mData.sampleKernel[i];
            float2 rand;
            switch (mHemisphereDistribution)
            {
            case SampleDistribution::Random:
                rand = float2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
                break;

            case SampleDistribution::VanDerCorput:
                // skip 0 because it will results in (0, 0) which results in sample point (0, 0)
                // => this means that we sample the same position for all tangent space rotations
                rand = float2((float)(i) / (float)(mKernelSize), radicalInverse(vanDerCorputOffset + i));
                break;

            default: throw std::runtime_error("unknown kernel distribution");
            }

            float theta = rand.x * 2.0f * glm::pi<float>();
            float r = glm::sqrt(1.0f - glm::pow(rand.y, 2.0f / 3.0f));
            nums += std::to_string(r) + ", ";
            s.x = r * sin(theta);
            s.y = r * cos(theta);
            s.z = glm::linearRand(0.0f, 1.0f);
            s.w = glm::linearRand(0.0f, 1.0f);
        }
    }


    mDirty = true;
}

void VAO::setNoiseTexture()
{
    std::vector<uint16_t> data;
    data.resize(mNoiseSize.x * mNoiseSize.y);

    std::srand(2346); // always use the same seed for the noise texture (linear rand uses std rand)
    for (uint32_t i = 0; i < mNoiseSize.x * mNoiseSize.y; i++)
    {
        // Random directions on the XY plane
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        data[i] = uint16_t(glm::packSnorm4x8(float4(sin(theta), cos(theta), 0.0f, 0.0f)));
    }

    mpNoiseTexture = Texture::create2D(mNoiseSize.x, mNoiseSize.y, ResourceFormat::RG8Snorm, 1, Texture::kMaxPossible, data.data());

    mDirty = true;
}