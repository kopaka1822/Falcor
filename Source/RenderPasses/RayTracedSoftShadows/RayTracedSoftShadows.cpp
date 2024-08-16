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
#include "RayTracedSoftShadows.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

namespace
{
    const std::string kShaderTraceSoftShadows = "RenderPasses/RayTracedSoftShadows/RayTracedSoftShadows.rt.slang";

    // Ray tracing settings that affect the traversal stack size.
    const uint32_t kMaxPayloadSizeBytes = 64u;
    const uint32_t kMaxRecursionDepth = 1u;

    const ChannelList kInputChannels = {
        {"vBuffer", "gVBuffer", "V-Buffer"},
        {"viewW", "gView", "View vector"},
    };


    //Outputs
    const std::string kOutputColor = "color";
    const std::string kOutputEmission = "emission";
    const std::string kOutputDiffuseRadiance = "diffuseRadiance";
    const std::string kOutputSpecularRadiance = "specularRadiance";
    const std::string kOutputDiffuseReflectance = "diffuseReflectance";
    const std::string kOutputSpecularReflectance = "specularReflectance";

    const ChannelList kOutputChannels = {
        {kOutputColor, "gOutColor", "Output Color (linear)", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputEmission, "gOutEmission", "Output Emission", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputDiffuseRadiance, "gOutDiffuseRadiance", "Output demodulated diffuse color (linear)", true /*optional*/,ResourceFormat::RGBA16Float},
        {kOutputSpecularRadiance, "gOutSpecularRadiance", "Output demodulated specular color (linear)", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputDiffuseReflectance, "gOutDiffuseReflectance", "Output primary surface diffuse reflectance", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputSpecularReflectance, "gOutSpecularReflectance", "Output primary surface specular reflectance", true /*optional*/,ResourceFormat::RGBA16Float},
    };

} //Namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, RayTracedSoftShadows>();
}

RayTracedSoftShadows::RayTracedSoftShadows(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

Properties RayTracedSoftShadows::getProperties() const
{
    return {};
}

RenderPassReflection RayTracedSoftShadows::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void RayTracedSoftShadows::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "RayTracedSoftShadows");

    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        dict[Falcor::kRenderPassEnableNRD] = mEnableNRD ? NRDEnableFlags::NRDEnabled : NRDEnableFlags::NRDDisabled;
        mOptionsChanged = false;
    }

    // Clear Outputs Lamda
    auto clearOutputs = [&]()
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
    };

    // If we have no scene, clear the outputs and return.
    if (!mpScene)
    {
        clearOutputs();
        return;
    }
    //Check if emissive lights are enabled
    auto& pLights = mpScene->getLightCollection(pRenderContext);
    if (!mpScene->useEmissiveLights())
    {
        clearOutputs();
        return;
    }

    if (mClearDemodulationTextures) {
        clearOutputs(); // lazily clear all textures
        mClearDemodulationTextures = false;
    }
        

    prepareLighting(pRenderContext);

    //Shade
    shade(pRenderContext, renderData);

    mFrameCount++;
}

bool RayTracedSoftShadows::prepareLighting(RenderContext* pRenderContext) {
    bool lightingChanged = false;

    // Init light sampler if not set
    if (!mpEmissiveLightSampler || mRebuildLightSampler)
    {
        switch (mEmissiveType)
        {
        case EmissiveLightSamplerType::Uniform:
            mpEmissiveLightSampler = std::make_unique<EmissiveUniformSampler>(pRenderContext, mpScene);
            break;
        case EmissiveLightSamplerType::LightBVH:
            mpEmissiveLightSampler = std::make_unique<LightBVHSampler>(pRenderContext, mpScene, mLightBVHOptions);
            break;
        case EmissiveLightSamplerType::Power:
            mpEmissiveLightSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, mpScene);
            break;
        default:
            throw RuntimeError("Unknown emissive light sampler type");
        }
        lightingChanged = true;
        mRebuildLightSampler = false;
    }

    // Update Emissive light sampler
    if (mpEmissiveLightSampler)
    {
        lightingChanged |= mpEmissiveLightSampler->update(pRenderContext);
    }
    
    return lightingChanged;
}

void RayTracedSoftShadows::resetLighting() {
    // Retain the options for the emissive sampler.
    if (auto lightBVHSampler = dynamic_cast<LightBVHSampler*>(mpEmissiveLightSampler.get()))
    {
        mLightBVHOptions = lightBVHSampler->getOptions();
    }

    mpEmissiveLightSampler = nullptr;
    mRecompile = true;
}

void RayTracedSoftShadows::shade(RenderContext* pRenderContext, const RenderData& renderData) {

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // I/O Resources could change at any time
    mSoftShadowPip.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));  // Emissive is the only optional channel (only
                                                                                              // used in simplified shading)
    mSoftShadowPip.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData)); // Debug out images

    //SetDefines
    mSoftShadowPip.pProgram->addDefine("ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    mSoftShadowPip.pProgram->addDefine("USE_ENV_MAP", mpScene->useEnvBackground() ? "1" : "0");
    mSoftShadowPip.pProgram->addDefine("NRD_DEMODULATION", mEnableNRD ? "1" : "0");

    if (mpEmissiveLightSampler)
        mSoftShadowPip.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

    // Prepare Vars
    if (!mSoftShadowPip.pVars || mRecompile)
    {
        mSoftShadowPip.pProgram->addDefines(mpSampleGenerator->getDefines());
        mSoftShadowPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mSoftShadowPip.pProgram->addDefines(mpSampleGenerator->getDefines());
        mSoftShadowPip.pVars = RtProgramVars::create(mpDevice, mSoftShadowPip.pProgram, mSoftShadowPip.pBindingTable);
        mRecompile = false;
    }

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Bind Resources
    auto var = mSoftShadowPip.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["gEmissiveSampler"]);

    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gSPP"] = mSPP;
    var["CB"]["gAmbientFactor"] = mAmbientFactor;
    var["CB"]["gEmissiveFactor"] = mEmissiveFactor;
    var["CB"]["gEnvMapFactor"] = mEnvMapFactor;

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };

    for (auto channel : kInputChannels)
        bind(channel);
    for (auto channel : kOutputChannels)
        bind(channel);

    // Execute shader
    mpScene->raytrace(pRenderContext, mSoftShadowPip.pProgram.get(), mSoftShadowPip.pVars, uint3(targetDim, 1));
}

void RayTracedSoftShadows::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.slider("SPP", mSPP,1u,32u);
    widget.tooltip("Number of light samples");
    dirty |= widget.checkbox("Alpha Test", mUseAlphaTest);
    widget.tooltip("Enable Alpha test");
    dirty |= widget.var("Ambient Factor", mAmbientFactor);
    widget.tooltip("Factor for ambient light");
    dirty |= widget.var("Emissive Factor", mEmissiveFactor);
    widget.tooltip("Factor for the emissive light strength");
    dirty |= widget.var("EnvMap Factor", mEnvMapFactor);
    widget.tooltip("Factor for the env map sample");

    mClearDemodulationTextures |= widget.checkbox("Enable NRD", mEnableNRD);
    dirty |= mClearDemodulationTextures;

    if (mpScene && mpScene->useEmissiveLights())
    {
        if (auto group = widget.group("Emissive sampler"))
        {
            if (widget.dropdown("Emissive sampler", mEmissiveType))
            {
                resetLighting();
                dirty = true;
            }
            widget.tooltip("Selects which light sampler to use for importance sampling of emissive geometry.", true);

            if (mpEmissiveLightSampler)
            {
                if (mpEmissiveLightSampler->renderUI(group))
                    mOptionsChanged = true;
            }
        }
    }

    mOptionsChanged = dirty;
}

void RayTracedSoftShadows::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Clear data from previous scene
    mSoftShadowPip.pBindingTable = nullptr;
    mSoftShadowPip.pProgram = nullptr;
    mSoftShadowPip.pVars = nullptr;
    resetLighting();

    // Set new scene
    mpScene = pScene;

    // Create Ray Tracing pass
    if (mpScene)
    {
        auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
        // Create ray tracing program.
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTraceSoftShadows);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mSoftShadowPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mSoftShadowPip.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "anyHit"));
        }

        mSoftShadowPip.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    }
}
