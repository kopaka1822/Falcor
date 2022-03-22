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
#include "VAORT.h"

#include <glm/gtc/random.hpp>

const RenderPass::Info VAORT::kInfo { "VAORT", "VAO in a dedicated ray tracing pipeline" };

namespace
{
    const std::string kAmbientMap = "ambientMap";
    const std::string kDepth = "depth";
    const std::string kNormals = "normals";

    const std::string kRadius = "radius";
    const std::string kGuardBand = "guardBand";
    const std::string kShader = "RenderPasses/VAORT/VAO.rt.slang";
    const uint32_t kMaxPayloadSize = 4 * 4;
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(VAORT::kInfo, VAORT::create);
}

VAORT::SharedPtr VAORT::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VAORT(dict));
    return pPass;
}

VAORT::VAORT(const Dictionary& dict) : RenderPass(kInfo)
{
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    //samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    for (const auto& [key, value] : dict)
    {
        if (key == kRadius) mData.radius = value;
        else if (key == kGuardBand) mGuardBand = value;
        else logWarning("Unknown field '" + key + "' in a SSAO dictionary");
    }
}

Dictionary VAORT::getScriptingDictionary()
{
    Dictionary dict;
    dict[kRadius] = mData.radius;
    dict[kGuardBand] = mGuardBand;
    return dict;
}

RenderPassReflection VAORT::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Linear Depth-buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormals, "World space normals, [0, 1] range").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "Ambient Occlusion").bindFlags(Falcor::ResourceBindFlags::AllColorViews).format(ResourceFormat::R8Unorm);

    return reflector;
}

void VAORT::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setKernel();
    setNoiseTexture();

    mDirty = true; // texture size may have changed => reupload data
    mpRayProgram.reset();
}

void VAORT::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //pRenderContext->getLowLevelData()->getCommandList()->ResourceBarrier()
    //pRenderContext->resourceBarrier(, Resource::State::)


    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormals = renderData[kNormals]->asTexture();
    auto pAoDst = renderData[kAmbientMap]->asTexture();

    auto pCamera = mpScene->getCamera().get();
    //renderData["k"]->asBuffer();

    if (mEnabled)
    {
        if (!mpRayProgram)
        {
            // program defines
            Program::DefineList defines;
            defines.add(mpScene->getSceneDefines());
            //defines.add("SHADER_VARIANT", std::to_string(uint32_t(mShaderVariant)));
            //defines.add("DEPTH_MODE", std::to_string(uint32_t(mDepthMode)));


            // ray pass
            RtProgram::Desc desc;
            desc.addShaderLibrary(kShader);
            desc.setMaxPayloadSize(kMaxPayloadSize);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(1);
            desc.addTypeConformances(mpScene->getTypeConformances());

            RtBindingTable::SharedPtr sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));
            sbt->setHitGroup(0, mpScene->getGeometryIDs(GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
            // TODO add remaining primitives
            mpRayProgram = RtProgram::create(desc, defines);
            mRayVars = RtProgramVars::create(mpRayProgram, sbt);
            mDirty = true;
        }

        if (mDirty)
        {
            // bind static resources
            mData.noiseScale = float2(pDepth->getWidth(), pDepth->getHeight()) / float2(mNoiseSize.x, mNoiseSize.y);
            mRayVars["StaticCB"].setBlob(mData);

            pRenderContext->clearTexture(pAoDst.get(), float4(0.0f));
            mDirty = false;
        }

        pCamera->setShaderData(mRayVars["PerFrameCB"]["gCamera"]);
        mRayVars["PerFrameCB"]["frameIndex"] = mFrameIndex++;
        mRayVars["PerFrameCB"]["guardBand"] = mGuardBand;
        mRayVars["PerFrameCB"]["invViewMat"] = glm::inverse(pCamera->getViewMatrix());

        // Update state/vars
        mRayVars["gNoiseSampler"] = mpNoiseSampler;
        mRayVars["gTextureSampler"] = mpTextureSampler;
        mRayVars["gDepthTex"] = pDepth;
        mRayVars["gNoiseTex"] = mpNoiseTexture;
        mRayVars["gNormalTex"] = pNormals;

        mRayVars["gOutput"] = pAoDst;
        uint3 dims = uint3(pAoDst->getWidth() - 2 * mGuardBand, pAoDst->getHeight() - 2 * mGuardBand, 1);
        mpScene->raytrace(pRenderContext, mpRayProgram.get(), mRayVars, dims);
    }
    else // ! enabled
    {
        pRenderContext->clearTexture(pAoDst.get(), float4(1.0f));
    }
}

void VAORT::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.var("Guard Band", mGuardBand, 0, 256)) mDirty = true;
    //uint32_t size = mData.kernelSize;
    //if (widget.var("Kernel Size", size, 1u, SSAOData::kMaxSamples)) setKernelSize(size);

    if (widget.var("Sample Radius", mData.radius, 0.01f, FLT_MAX, 0.01f)) mDirty = true;

    if (widget.slider("Power Exponent", mData.exponent, 1.0f, 4.0f)) mDirty = true;
}

void VAORT::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mDirty = true;
    mpRayProgram.reset();
}

void VAORT::setNoiseTexture()
{
    std::srand(5960372); // same seed for kernel
    for (uint32_t i = 0; i < mData.kernelSize; i++)
    {
        auto& s = mData.sampleKernel[i];
        float2 rand;
        rand = float2((float)(i) / (float)(mData.kernelSize), radicalInverse(i + 1));

        float theta = rand.x * 2.0f * glm::pi<float>();
        float r = glm::sqrt(1.0f - glm::pow(rand.y, 2.0f / 3.0f));
        s.x = r * sin(theta);
        s.y = r * cos(theta);
        s.z = glm::linearRand(0.0f, 1.0f);
        s.w = glm::linearRand(0.0f, 1.0f);
    }

    mDirty = true;
}

void VAORT::setKernel()
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
