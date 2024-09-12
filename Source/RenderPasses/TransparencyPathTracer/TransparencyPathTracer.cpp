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
#include "TransparencyPathTracer.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TransparencyPathTracer>();
}

namespace
{
    //shader
    const std::string kShaderFolder = "RenderPasses/TransparencyPathTracer/";
    const std::string kShaderFile = kShaderFolder + "TransparencyPathTracer.rt.slang";
    const std::string kShaderAVSMRay = kShaderFolder + "GenAdaptiveVolumetricSM.rt.slang";

    //RT shader constant settings
    const uint kMaxPayloadSizeBytes = 20u;
    const uint kMaxRecursionDepth = 1u;
    const uint kMaxPayloadSizeAVSM = 48u;

    const ChannelList kInputChannels = {
        {"vbuffer", "gVBuffer", "Visibility buffer in packed format"},
        {"viewW", "gViewW", "World-space view direction (xyz float format)", true /* optional */},
    };

    const ChannelList kOutputChannels = {
        {"color", "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float},
    };

}


TransparencyPathTracer::TransparencyPathTracer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);

    // Create sampler.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpPointSampler = Sampler::create(mpDevice, samplerDesc);
    FALCOR_ASSERT(mpShadowSamplerPoint);
}

Properties TransparencyPathTracer::getProperties() const
{
    return {};
}

RenderPassReflection TransparencyPathTracer::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void TransparencyPathTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    // If we have no scene, just clear the outputs and return.
    if (!mpScene)
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
        return;
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    generateAVSM(pRenderContext, renderData);

    traceScene(pRenderContext, renderData);  

    mFrameCount++;
}

void TransparencyPathTracer::generateAVSM(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate AVSM");

    // Get Analytic light data
    auto& lights = mpScene->getLights();
    // Nothing to do if there are no lights
    if (lights.size() == 0)
        return;

    //Check if resize is neccessary
    bool rebuildAll = false;
    if (mShadowMapMVP.size() != lights.size())
    {
        mShadowMapMVP.resize(lights.size());
        rebuildAll = true;
    }

    //Destroy resources if pass is not enabled or a resize is necessary
    if (rebuildAll) //TODO add global bool
    {
        mAVSM.clear();
        mAVSMLast.clear();
    }

    //Update matrices
    for (uint i = 0; i < lights.size(); i++)
    {
        auto changes = lights[i]->getChanges();
        bool rebuild = is_set(changes, Light::Changes::Position) || is_set(changes, Light::Changes::Direction) ||
                       is_set(changes, Light::Changes::SurfaceArea);
        rebuild |= rebuildAll;
        if (rebuild)
        {
            mShadowMapMVP[i].calculate(lights[i], mNearFar);
        }
    }

    // Create / Destroy resources
    //TODO MIPS and check formats
    {
        if (mAVSM.empty())
        {
            mAVSM.resize(lights.size() * 2);
            for (uint i = 0; i < lights.size() * 2; i++)
            {
                mAVSM[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mAVSM[i]->setName("AVSM_" + std::to_string(i));
            }
        }

        if (mAVSMLast.empty())
        {
            mAVSMLast.resize(lights.size());
            for (uint i = 0; i < lights.size(); i++)
            {
                mAVSMLast[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RG32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mAVSMLast[i]->setName("AVSMLastElement_" + std::to_string(i));
            }
        }
    }
    

    // Defines
    mGenVASMPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenVASMPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));


    //Create Program Vars
    if (!mGenVASMPip.pVars)
    {
        mGenVASMPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenVASMPip.pVars = RtProgramVars::create(mpDevice, mGenVASMPip.pProgram, mGenVASMPip.pBindingTable);
    }
        
    FALCOR_ASSERT(mGenVASMPip.pVars);

    //Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());
        // Bind Utility
        auto var = mGenVASMPip.pVars->getRootVar();
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;

        //var["gAVSM"] = mAVSM[i];
        var["gAVSMDepths"] = mAVSM[i];
        var["gAVSMTransparency"] = mAVSM[i + lights.size()];
        var["gAVSMLast"] = mAVSMLast[i];

        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenVASMPip.pProgram.get(), mGenVASMPip.pVars, uint3(targetDim, 1));
    }
}

void TransparencyPathTracer::traceScene(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Trace Scene");
    auto& lights = mpScene->getLights();
    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mTracer.pProgram->addDefine("MAX_ALPHA_BOUNCES", std::to_string(mMaxAlphaTestPerBounce));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracer.pProgram->addDefine("LIGHT_SAMPLE_MODE", std::to_string((uint)mLightSampleMode));
    mTracer.pProgram->addDefine("USE_RUSSIAN_ROULETTE_FOR_ALPHA", mUseRussianRouletteForAlpha ? "1" : "0");
    mTracer.pProgram->addDefine("USE_RUSSIAN_ROULETTE_PATH", mUseRussianRoulettePath ? "1" : "0");
    mTracer.pProgram->addDefine("USE_AVSM", mUseAVSM ? "1" : "0");
    mTracer.pProgram->addDefine("COUNT_SM", std::to_string(lights.size()));

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    if (!mTracer.pVars)
        prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    // Set constants.
    auto var = mTracer.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount;
    
    //Shadow Map
    
    var["SMCB"]["gSMSize"] = mSMSize;
    var["SMCB"]["gNear"] = mNearFar.x;
    var["SMCB"]["gFar"] = mNearFar.y;
    
    
    for (uint i = 0; i < lights.size(); i++)
    {
        var["ShadowVPs"]["gShadowMapVP"][i] = mShadowMapMVP[i].viewProjection;
        var["gAVSMLast"][i] = mAVSMLast[i];
        var["gAVSMDepths"][i] = mAVSM[i];
        var["gAVSMTransmittance"][i] = mAVSM[i + lights.size()];
    }

    var["gPointSampler"] = mpPointSampler;

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

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
}

void TransparencyPathTracer::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.dropdown("Light Sample Mode", mLightSampleMode);
    widget.tooltip(
        "Changes light sample mode.\n"
        "RIS: Loops through all lights and resamples them. Recommended if there multiple lights that have not many overlaps \n"
        "Uniform: Randomly samples a light."
    );

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect illumination.\n0 = direct only\n1 = one indirect bounce etc.", true);

    dirty |= widget.var("Max Alpha Test bounces", mMaxAlphaTestPerBounce ,0u, 64u);
    widget.tooltip("Maximum number of alpha test bounces allowed", true);

    dirty |= widget.checkbox("Russian Roulette Path", mUseRussianRoulettePath);
    widget.tooltip("Russian Roulette for the path. Is used at the end of every bounce (after all alpha tests)");

    dirty |= widget.checkbox("Russian Roulette Transparency", mUseRussianRouletteForAlpha);
    widget.tooltip("Use Russian Roulette to abort alpha tested paths early (before reaching transparency == 0)");

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.checkbox("Use Adaptive Volumetric Shadow Maps", mUseAVSM);
    if (mUseAVSM)
    {
        if (auto group = widget.group("Adaptive Volumetric Shadow Maps Settings"))
        {
            group.var("Depth Bias", mDepthBias, 0.f, FLT_MAX, 0.0000001f, false, "%.7f");
            group.tooltip("Constant bias that is added to the depth");
            group.var("Normal Depth Bias", mNormalDepthBias, 0.f, FLT_MAX, 0.0000001f, false, "%.7f");
            group.tooltip("Bias that is added depending on the normal");
        }
    }

    mOptionsChanged |= dirty;
}

void TransparencyPathTracer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    // Clear data for previous scene.
    // After changing scene, the raytracing program should to be recreated.
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mFrameCount = 0;

    // Set new scene.
    mpScene = pScene;

    if (mpScene)
    {
        mpScene->setRtASAdditionalGeometryFlag(RtGeometryFlags::NoDuplicateAnyHitInvocation); //Add the NoDublicateAnyHitInvocation flag to this pass

        if (pScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("MinimalPathTracer: This render pass does not support custom primitives.");
        }

        // Create scene ray tracing program.
        {
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderFile);
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

            mTracer.pBindingTable = RtBindingTable::create(2, 2, mpScene->getGeometryCount());
            auto& sbt = mTracer.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("alphaMiss"));
            sbt->setMiss(1, desc.addMiss("shadowMiss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("alphaClosestHit", "alphaAnyHit")
                );
                sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowAnyHit"));
            }

            mTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }

        //Create AVSM trace program
        {
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderAVSMRay);
            desc.setMaxPayloadSize(kMaxPayloadSizeAVSM);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(1u);

            mGenVASMPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mGenVASMPip.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "anyHit")
                );
            }

            mGenVASMPip.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }
        
    }
}

void TransparencyPathTracer::prepareVars() {
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mTracer.pProgram);

    // Configure program.
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());

    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracer.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
}

void TransparencyPathTracer::LightMVP::calculate(ref<Light> light, float2 nearFar) {
    auto& data = light->getData();
    pos = data.posW;
    float3 lightTarget = pos + data.dirW;
    const float3 up = abs(data.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    view = math::matrixFromLookAt(data.posW, lightTarget, up);
    projection = math::perspective(data.openingAngle * 2, 1.f, nearFar.x, nearFar.y);
    viewProjection = math::mul(projection, view);
    invViewProjection = math::inverse(viewProjection);
}
