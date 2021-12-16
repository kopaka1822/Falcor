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
#include "SSAO.h"
#include "glm/gtc/random.hpp"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regSSAO(pybind11::module& m)
{
    pybind11::class_<SSAO, RenderPass, SSAO::SharedPtr> pass(m, "SSAO");
    pass.def_property("enabled", &SSAO::getEnabled, &SSAO::setEnabled);
    pass.def_property("halfResolution", &SSAO::getHalfResolution, &SSAO::setHalfResolution);
    pass.def_property("kernelRadius", &SSAO::getKernelSize, &SSAO::setKernelSize);
    pass.def_property("distribution", &SSAO::getDistribution, &SSAO::setDistribution);
    pass.def_property("sampleRadius", &SSAO::getSampleRadius, &SSAO::setSampleRadius);

    pybind11::enum_<SSAO::SampleDistribution> sampleDistribution(m, "SampleDistribution");
    sampleDistribution.value("Random", SSAO::SampleDistribution::Random);
    sampleDistribution.value("Hammersley", SSAO::SampleDistribution::Hammersley);

    pybind11::enum_<SSAO::ShaderVariant> shaderVariant(m, "ShaderVariant");
    shaderVariant.value("Raster", SSAO::ShaderVariant::Raster);
    shaderVariant.value("Raytracing", SSAO::ShaderVariant::Raytracing);
    shaderVariant.value("Hybrid", SSAO::ShaderVariant::Hybrid);
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("SSAO", "Screen-space ambient occlusion", SSAO::create);
    ScriptBindings::registerBinding(regSSAO);
}

const char* SSAO::kDesc = "Screen-space ambient occlusion. Can be used with and without a normal-map";

namespace
{
    const Gui::DropdownList kDistributionDropdown =
    {
        { (uint32_t)SSAO::SampleDistribution::Random, "Random" },
        { (uint32_t)SSAO::SampleDistribution::Hammersley, "Uniform Hammersley" },
    };

    const Gui::DropdownList kShaderVariantDropdown =
    {
        { (uint32_t)SSAO::ShaderVariant::Raster, "Raster" },
        { (uint32_t)SSAO::ShaderVariant::Raytracing, "Raytracing" },
        { (uint32_t)SSAO::ShaderVariant::Hybrid, "Hybrid" }
    };

    const std::string kEnabled = "enabled";
    const std::string kHalfResolution = "halfResolution";
    const std::string kKernelSize = "kernelSize";
    const std::string kNoiseSize = "noiseSize";
    const std::string kDistribution = "distribution";
    const std::string kRadius = "radius";
    const std::string kShaderVariant = "shaderVariant";

    const std::string kAmbientMap = "ambientMap";
    const std::string kDepth = "depth";
    const std::string kNormals = "normals";

    const std::string kSSAOShader = "RenderPasses/SSAO/SSAO.ps.slang";

    //const auto AMBIENT_MAP_FORMAT = ResourceFormat::RGBA8Unorm;
    const auto AMBIENT_MAP_FORMAT = ResourceFormat::RGBA32Float; // debugging
}

SSAO::SSAO()
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    //samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);
    setSampleRadius(mData.radius);
}

SSAO::SharedPtr SSAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pSSAO = SharedPtr(new SSAO);
    Dictionary blurDict;
    for (const auto& [key, value] : dict)
    {
        if(key == kEnabled) pSSAO->mEnabled = value;
        else if (key == kHalfResolution) pSSAO->mHalfResolution = value;
        else if (key == kKernelSize) pSSAO->mData.kernelSize = value;
        else if (key == kNoiseSize) pSSAO->mNoiseSize = value;
        else if (key == kDistribution) pSSAO->mHemisphereDistribution = value;
        else if (key == kRadius) pSSAO->mData.radius = value;
        else logWarning("Unknown field '" + key + "' in a SSAO dictionary");
    }
    return pSSAO;
}

Dictionary SSAO::getScriptingDictionary()
{
    Dictionary dict;
    dict[kEnabled] = mEnabled;
    dict[kHalfResolution] = mHalfResolution;
    dict[kKernelSize] = mData.kernelSize;
    dict[kNoiseSize] = mNoiseSize;
    dict[kRadius] = mData.radius;
    dict[kDistribution] = mHemisphereDistribution;
    return dict;
}

RenderPassReflection SSAO::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Depth-buffer");
    reflector.addInput(kNormals, "World space normals, [0, 1] range").flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget).format(AMBIENT_MAP_FORMAT);
    
    return reflector;
}

void SSAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setKernel();
    setNoiseTexture();

    // reset framebufer
    mpAOFbo.reset();
}

void SSAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //pRenderContext->getLowLevelData()->getCommandList()->ResourceBarrier()
    //pRenderContext->resourceBarrier(, Resource::State::)


    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();

    //renderData["k"]->asBuffer();


    if(mEnabled)
    {
        auto pAoMap = generateAOMap(pRenderContext, mpScene->getCamera().get(), pDepth, pNormals);

        // copy to ao destination TODO optimize
        pRenderContext->copySubresource(pAoDst.get(), 0, pAoMap.get(), 0);
    }
    else // ! enabled
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
    }
}

void SSAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
}

Texture::SharedPtr SSAO::generateAOMap(RenderContext* pContext, const Camera* pCamera, const Texture::SharedPtr& pDepthTexture,
const Texture::SharedPtr& pNormalTexture)
{
    if(!mpAOFbo)
    {
        // create framebuffer
        Fbo::Desc fboDesc;
        fboDesc.setColorTarget(0, AMBIENT_MAP_FORMAT);

        uint divisor = mHalfResolution ? 2 : 1; // half resolution of the ambient occlusion map if required (use resolution of the depth map for reference)
        mpAOFbo = Fbo::create2D(pDepthTexture->getWidth() / divisor, pDepthTexture->getHeight() / divisor, fboDesc);
        assert(mpAOFbo);

        mData.noiseScale = float2(mpAOFbo->getWidth(), mpAOFbo->getHeight()) / float2(mNoiseSize.x, mNoiseSize.y);
        mDirty = true; // modified mData
    }

    if (mDirty || !mpSSAOPass)
    {
        // program defines
        Program::DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("SHADER_VARIANT", std::to_string(uint32_t(mShaderVariant)));

        mpSSAOPass = FullScreenPass::create(kSSAOShader, defines);

        // bind static resources
        auto var = mpSSAOPass->getRootVar();
        mpScene->setRaytracingShaderData(pContext, var);

        mpSSAOPass["StaticCB"].setBlob(mData);
        mDirty = false;
    }

   
    pCamera->setShaderData(mpSSAOPass["PerFrameCB"]["gCamera"]);
    mpSSAOPass["PerFrameCB"]["frameIndex"] = mFrameIndex++;

    // Update state/vars
    mpSSAOPass["gNoiseSampler"] = mpNoiseSampler;
    mpSSAOPass["gTextureSampler"] = mpTextureSampler;
    mpSSAOPass["gDepthTex"] = pDepthTexture;
    mpSSAOPass["gNoiseTex"] = mpNoiseTexture;
    mpSSAOPass["gNormalTex"] = pNormalTexture;

    // Generate AO
    mpSSAOPass->execute(pContext, mpAOFbo);
    return mpAOFbo->getColorTexture(0);
}

void SSAO::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if(!mEnabled) return;

    uint32_t shaderVariant = (uint32_t)mShaderVariant;
    if(widget.dropdown("Variant", kShaderVariantDropdown, shaderVariant)) setShaderVariant(shaderVariant);

    bool halfRes = mHalfResolution;
    if (widget.checkbox("Half Resolution", halfRes)) setHalfResolution(halfRes);

    uint32_t distribution = (uint32_t)mHemisphereDistribution;
    if (widget.dropdown("Kernel Distribution", kDistributionDropdown, distribution)) setDistribution(distribution);

    uint32_t size = mData.kernelSize;
    if (widget.var("Kernel Size", size, 1u, SSAOData::kMaxSamples)) setKernelSize(size);

    float radius = mData.radius;
    if (widget.var("Sample Radius", radius, 0.001f, FLT_MAX, 0.1f)) setSampleRadius(radius);

    widget.text("noise size"); widget.text(std::to_string(mNoiseSize.x), true);
}

void SSAO::setHalfResolution(bool halfRes)
{
    mHalfResolution = halfRes;
    // reset framebufer
    mpAOFbo.reset();
}

void SSAO::setSampleRadius(float radius)
{
    mData.radius = radius;
    mData.invRadiusSquared = 1.0f / (radius * radius);
    mDirty = true;
}

void SSAO::setKernelSize(uint32_t kernelSize)
{
    kernelSize = glm::clamp(kernelSize, 1u, SSAOData::kMaxSamples);
    mData.kernelSize = kernelSize;
    setKernel();
}

void SSAO::setDistribution(uint32_t distribution)
{
    mHemisphereDistribution = (SampleDistribution)distribution;
    setKernel();
}

void SSAO::setShaderVariant(uint32_t variant)
{
    mShaderVariant = (ShaderVariant)variant;
    mDirty = true;
}

void SSAO::setKernel()
{
    for (uint32_t i = 0; i < mData.kernelSize; i++)
    {
        auto& s = mData.sampleKernel[i];
        float2 rand;
        switch (mHemisphereDistribution)
        {
        case SampleDistribution::Random:
            rand = float2(glm::linearRand(0.0f, 1.0f), glm::linearRand(0.0f, 1.0f));
            break;

        case SampleDistribution::Hammersley:
            // skip 0 because it will results in (0, 0) which results in sample point (0, 0)
            // => this means that we sample the same position for all tangent space rotations
            rand = float2((float)(i + 1) / (float)(mData.kernelSize + 1), radicalInverse(i + 1));
            break;

        default: throw std::runtime_error("unknown kernel distribution");
        }

        float theta = rand.x * 2.0f * glm::pi<float>();
        float r = glm::sqrt(1.0f - glm::pow(rand.y, 2.0f / 3.0f));
        s.x = r * sin(theta);
        s.y = r * cos(theta);
        s.z = glm::linearRand(0.0f, 1.0f);
        s.w = glm::linearRand(0.0f, 1.0f);


        // Hemisphere in the Z+ direction
        float3 p;
        /*switch (mHemisphereDistribution)
        {
        case SampleDistribution::Random:
            p = glm::normalize(glm::linearRand(float3(-1.0f, -1.0f, 0.0f), float3(1.0f, 1.0f, 1.0f)));
            break;

        case SampleDistribution::RandomCosine:
            {
                float phi = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
                float theta = glm::acos(glm::sqrt(glm::linearRand(0.0f, 1.0f)));

                p.z = cos(theta);
                p.x = sin(theta) * sin(phi);
                p.y = sin(theta) * cos(phi);
            }
            break;

        case SampleDistribution::UniformHammersley:
            p = hammersleyUniform(i + 1, mData.kernelSize + 1);
            break;

        case SampleDistribution::CosineHammersley:
            p = hammersleyCosine(i + 1, mData.kernelSize + 1);
            break;
        }*/

        //mData.sampleKernel[i] = float4(p, 0.0f);

        // Skew sample point distance on a curve so more cluster around the origin
        //float dist = (float)i / (float)mData.kernelSize;
        //dist = glm::mix(0.1f, 1.0f, dist * dist);
        //mData.sampleKernel[i] *= dist;
    }

    mDirty = true;
}

void SSAO::setNoiseTexture()
{
    std::vector<uint16_t> data;
    data.resize(mNoiseSize.x * mNoiseSize.y);

    for (uint32_t i = 0; i < mNoiseSize.x * mNoiseSize.y; i++)
    {
        // Random directions on the XY plane
        //float2 dir = glm::normalize(glm::linearRand(float2(-1), float2(1))) * 0.5f + 0.5f;
        //data[i] = glm::packUnorm4x8(float4(dir, 0.0f, 1.0f));
        auto theta = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        data[i] = uint16_t(glm::packSnorm4x8(float4(sin(theta), cos(theta), 0.0f, 0.0f)));

        //auto r1 = glm::linearRand(0.0f, 2.0f * glm::pi<float>());
        //auto r2 = glm::acos(1.0f - glm::linearRand(0.0f, 2.0f));
        //
        //float3 dir = float3(sin(r1) * sin(r2), sin(r1) * cos(r2), sin(r2));
        //data[i] = uint16_t(glm::packSnorm4x8(glm::vec4(dir * 0.5f + 0.5f, 0.0f)));
    }

    mpNoiseTexture = Texture::create2D(mNoiseSize.x, mNoiseSize.y, ResourceFormat::RG8Snorm, 1, Texture::kMaxPossible, data.data());

    mDirty = true;
}
