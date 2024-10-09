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
#include "TransparencyLinkedList.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TransparencyLinkedList>();
}

namespace
{
    //shader
    const std::string kShaderFolder = "RenderPasses/TransparencyLinkedList/";
    const std::string kShaderFile = kShaderFolder + "TransparencyLinkedList.rt.slang";
    const std::string kShaderAVSMRay = kShaderFolder + "GenAdaptiveVolumetricSM.rt.slang";
    const std::string kShaderGenDebugRefFunction = kShaderFolder + "GenDebugRefFunction.rt.slang";
    const std::string kShaderStochSMRay = kShaderFolder + "GenStochasticSM.rt.slang";
    const std::string kShaderTemporalStochSMRay = kShaderFolder + "GenTmpStochSM.rt.slang";
    const std::string kShaderAccelShadowRay = kShaderFolder + "GenAccelShadow.rt.slang";
    const std::string kShaderLinkedList = kShaderFolder + "GenLinkedList.rt.slang";

    //RT shader constant settings
    const uint kMaxPayloadSizeBytes = 20u;
    const uint kMaxRecursionDepth = 1u;
    const uint kMaxPayloadSizeAVSMPerK = 8u;

    const std::string kInputVBuffer = "vbuffer";

    const ChannelList kInputChannels = {
        {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
        {"viewW", "gViewW", "World-space view direction (xyz float format)", true /* optional */},
    };

    const ChannelList kOutputChannels = {
        {"color", "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float},
    };

    const Gui::DropdownList kSMResolutionDropdown = {
        {256, "256x256"}, {512, "512x512"}, {768, "768x768"}, {1024, "1024x1024"}, {2048, "2048x2048"}, {4096, "4096x4096"},
    };

    const Gui::DropdownList kAVSMDropdownK = {
        {4, "4"},{8, "8"},{12, "12"},{16, "16"},{20, "20"}, {24, "24"},{28, "28"}, {32, "32"},
    };

    const Gui::DropdownList kAVSMRejectionMode = {{0, "TriangleArea"}, {1, "RectangeArea"}, {2, "Height"}, {3, "HeightErrorHeuristic"}
    };

    //UI Graph
    // Colorblind friendly palette.
    const std::vector<uint32_t> kColorPalette = {
        IM_COL32(0x00, 0x49, 0x49, 0xff), //Darker Cyan
        IM_COL32(0x00, 0x92, 0x92, 0xff), //Bright Cyan
        IM_COL32(0x49, 0x00, 0x92, 125), // Dark Purple
        IM_COL32(0x92, 0x4C, 0xD8, 0xff), //Bright Purple
        IM_COL32(0x00, 0x6d, 0xdb, 125),
        IM_COL32(0xb6, 0x6d, 0xff, 0xff),
        IM_COL32(0x6d, 0xb6, 0xff, 0xff),
        IM_COL32(0xb6, 0xdb, 0xff, 0xff),
        IM_COL32(0x92, 0x00, 0x00, 0xff),
        IM_COL32(0x24, 0xff, 0x24, 0xff),
        IM_COL32(0xff, 0xff, 0x6d, 0xff),
    };

    const uint32_t kHighlightColor = IM_COL32(0xff, 0x7f, 0x00, 0xcf);
    const uint32_t kTransparentWhiteColor = IM_COL32(0xff, 0xff, 0xff, 76);
    }


TransparencyLinkedList::TransparencyLinkedList(ref<Device> pDevice, const Properties& props)
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
    FALCOR_ASSERT(mpPointSampler);

    mpLinkedListCounter = Buffer::createStructured(mpDevice, sizeof(uint), 1, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false);
    mpLinkedListCounter->setName("LinkedListCounter");
}

Properties TransparencyLinkedList::getProperties() const
{
    return {};
}

RenderPassReflection TransparencyLinkedList::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void TransparencyLinkedList::execute(RenderContext* pRenderContext, const RenderData& renderData)
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

    bool sceneHasAnalyticLights = !mpScene->getLights().empty();

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    if (sceneHasAnalyticLights)
    {
        updateSMMatrices(pRenderContext, renderData);

        generateStochasticSM(pRenderContext, renderData);
        generateLinkedLists(pRenderContext, renderData);
    }
    

    traceScene(pRenderContext, renderData);

    if (!mDebugFrameCount)
        mFrameCount++;

    mAVSMRebuildProgram = false;
    mAVSMTexResChanged = false;
}

void TransparencyLinkedList::updateSMMatrices(RenderContext* pRenderContext, const RenderData& renderData) {
    auto& lights = mpScene->getLights();
    // Check if resize is neccessary
    bool rebuildAll = false;
    if (mShadowMapMVP.size() != lights.size())
    {
        mShadowMapMVP.resize(lights.size());
        rebuildAll = true;
    }

    // Update matrices
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
}

void TransparencyLinkedList::generateStochasticSM(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate Stochastic SM");
    if (mAVSMRebuildProgram)
    {
        mGenStochSMPip.pProgram.reset();
        mGenStochSMPip.pBindingTable.reset();
        mGenStochSMPip.pVars.reset();

        mTracer.pVars.reset();     // Recompile tracer program
        mAVSMTexResChanged = true; // Trigger texture reiinit
        
    }

    if (mAVSMTexResChanged)
    {
        mStochDepths.clear();
        mStochTransmittance.clear();
    }

    // Create AVSM trace program
    if (!mGenStochSMPip.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderStochSMRay);
        desc.setMaxPayloadSize(kMaxPayloadSizeAVSMPerK * mNumberAVSMSamples + 24); //+18 cause of the sampleGen (16) and confidence weight (4) + align(4)
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1u);

        mGenStochSMPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mGenStochSMPip.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "anyHit"));
        }

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("AVSM_K", std::to_string(mNumberAVSMSamples));
        defines.add(mpSampleGenerator->getDefines());

        mGenStochSMPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    auto& lights = mpScene->getLights();

    // Create / Destroy resources
    // TODO MIPS and check formats
    {
        uint numTextures = lights.size() * (mNumberAVSMSamples / 4);
        if (mStochDepths.empty())
        {
            mStochDepths.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mStochDepths[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mStochDepths[i]->setName("StochSMDepth_" + std::to_string(i));
            }
        }

        if (mStochTransmittance.empty())
        {
            mStochTransmittance.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mStochTransmittance[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA8Unorm, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mStochTransmittance[i]->setName("StochSMTransmittance_" + std::to_string(i));
            }
        }
    }

    //Abort early if disabled
    if (!mGenStochSM)
        return;

     // Defines
    mGenStochSMPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenStochSMPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));

    // Create Program Vars
    if (!mGenStochSMPip.pVars)
    {
        mGenStochSMPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenStochSMPip.pVars = RtProgramVars::create(mpDevice, mGenStochSMPip.pProgram, mGenStochSMPip.pBindingTable);
        mpSampleGenerator->setShaderData(mGenStochSMPip.pVars->getRootVar());
    }

    FALCOR_ASSERT(mGenStochSMPip.pVars);

    // Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        if (!lights[i]->isActive())
            break;
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());
        // Bind Utility
        auto var = mGenStochSMPip.pVars->getRootVar();
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;

        for (uint j = 0; j < mNumberAVSMSamples / 4; j++)
        {
            uint idx = i * (mNumberAVSMSamples / 4) + j;
            var["gStochDepths"][j] = mStochDepths[idx];
            var["gStochTransmittance"][j] = mStochTransmittance[idx];
        }

        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenStochSMPip.pProgram.get(), mGenStochSMPip.pVars, uint3(targetDim, 1));
    }
}

void TransparencyLinkedList::generateLinkedLists(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "Generate Linked List");
    if (!mGenLinkedListPip.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderLinkedList);
        desc.setMaxPayloadSize(4 * 4); // TODO adjust
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1u);

        mGenLinkedListPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mGenLinkedListPip.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("MAX_INDEX", std::to_string(mLinkedElementCount));
        
        defines.add(mpSampleGenerator->getDefines());

        mGenLinkedListPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    auto& lights = mpScene->getLights();

    // Create / Destroy resources
    {
        mpLinkedList.resize(lights.size());
        int i = 0;
        for(auto& pList : mpLinkedList)
        {
            if(!pList || pList->getElementCount() != mLinkedElementCount)
            {
                pList = Buffer::createStructured(mpDevice, sizeof(float) * 4, mLinkedElementCount);
                pList->setName("LinkedList_" + std::to_string(i));
            }
            ++i;
        }
    }

    //Abort early if disabled
    if (mShadowEvaluationMode != ShadowEvalMode::LinkedList)
        return;

    // Defines
    mGenLinkedListPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenLinkedListPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));

    // Create Program Vars
    if (!mGenLinkedListPip.pVars)
    {
        mGenLinkedListPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenLinkedListPip.pVars = RtProgramVars::create(mpDevice, mGenLinkedListPip.pProgram, mGenLinkedListPip.pBindingTable);
        mpSampleGenerator->setShaderData(mGenLinkedListPip.pVars->getRootVar());
    }

    FALCOR_ASSERT(mGenLinkedListPip.pVars);

    // Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        if (!lights[i]->isActive())
            break;
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());

        // clear to first free index after the head
        pRenderContext->clearUAV(mpLinkedListCounter->getUAV(0, 1).get(), uint4(mSMSize * mSMSize));

        // Bind Utility
        auto var = mGenLinkedListPip.pVars->getRootVar();
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;
        var["CB"]["gView"] = mShadowMapMVP[i].view;
        float rayConeAngle = std::atan(2.0f * std::tan(lights[i]->getData().openingAngle /*opening angle is already halved*/) / float(mSMSize));
        var["CB"]["gRayConeAngle"] = rayConeAngle;

        var["gLinkedList"] = mpLinkedList[i];
        var["gCounter"] = mpLinkedListCounter;

        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenLinkedListPip.pProgram.get(), mGenLinkedListPip.pVars, uint3(targetDim, 1));
    }
}

void TransparencyLinkedList::traceScene(RenderContext* pRenderContext, const RenderData& renderData) {
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
    mTracer.pProgram->addDefine("SHADOW_EVAL_MODE", std::to_string((uint)mShadowEvaluationMode));
    mTracer.pProgram->addDefine("USE_AVSM_PCF", mAVSMUsePCF ? "1" : "0");
    mTracer.pProgram->addDefine("USE_AVSM_INTERPOLATION", mAVSMUseInterpolation ? "1" : "0");

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
        var["ShadowView"]["gShadowMapView"][i] = mShadowMapMVP[i].view;
        var["gLinkedList"][i] = mpLinkedList[i];
    }
        
    for (uint i = 0; i < lights.size() * (mNumberAVSMSamples / 4); i++)
    {
        var["gStochDepths"][i] = mStochDepths[i];
        var["gStochTransmittance"][i] = mStochTransmittance[i];
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

void TransparencyLinkedList::renderUI(Gui::Widgets& widget)
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

    dirty |= widget.checkbox("Generate Stochastic Shadow Maps", mGenStochSM);
    dirty |= widget.dropdown("Shadow Render Mode", mShadowEvaluationMode);

    if(mShadowEvaluationMode == ShadowEvalMode::LinkedList)
    {
        dirty |= widget.var("Max Elements", mLinkedElementCount, 1u, std::numeric_limits<uint32_t>::max());
    }

    if (auto group = widget.group("Deep Shadow Maps Settings"))
    {
        mAVSMRebuildProgram |= group.dropdown("K", kAVSMDropdownK, mNumberAVSMSamples);
        mAVSMTexResChanged |= group.dropdown("Resolution", kSMResolutionDropdown, mSMSize);
        group.var("Near/Far", mNearFar, 0.000001f, FLT_MAX, 0.000001f, false, "%.6f");
        group.var("Depth Bias", mDepthBias, 0.f, FLT_MAX, 0.0000001f, false, "%.7f");
        group.tooltip("Constant bias that is added to the depth");
        group.var("Normal Depth Bias", mNormalDepthBias, 0.f, FLT_MAX, 0.0000001f, false, "%.7f");
        group.tooltip("Bias that is added depending on the normal");
        group.checkbox("Use Interpolation", mAVSMUseInterpolation);
        group.tooltip("Use interpolation for the evaluation.");
        group.checkbox("Use 2x2 PCF", mAVSMUsePCF); //TODO add other kernels
        group.tooltip("Enable 2x2 PCF using gather");
    }

    static bool graphOpenedFirstTime = true;
    
    //Debug options
    if (auto group = widget.group("Debug Options"))
    {
        bool changedFramecount = group.checkbox("Set fixed frame count", mDebugFrameCount);
        if (mDebugFrameCount)
        {
            if (changedFramecount)
                mFrameCount = 0;
            group.var("Frame Count", mFrameCount, 0u, UINT_MAX);
        }
    }

    mOptionsChanged |= dirty;
}

void TransparencyLinkedList::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    // Clear data for previous scene.
    // After changing scene, the raytracing program should to be recreated.
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mFrameCount = 0;

    mGenLinkedListPip.pProgram = nullptr;
    mGenLinkedListPip.pBindingTable = nullptr;
    mGenLinkedListPip.pVars = nullptr;

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
    }
}

void TransparencyLinkedList::prepareVars() {
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mTracer.pProgram);

    // Configure program.
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());
    mTracer.pProgram->addDefine("AVSM_K", std::to_string(mNumberAVSMSamples));
    mTracer.pProgram->addDefine("COUNT_LIGHTS",  std::to_string(mpScene->getLightCount()));

    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracer.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
}

void TransparencyLinkedList::LightMVP::calculate(ref<Light> light, float2 nearFar) {
    auto& data = light->getData();
    pos = data.posW;
    float3 lightTarget = pos + data.dirW;
    const float3 up = abs(data.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    view = math::matrixFromLookAt(data.posW, lightTarget, up);
    projection = math::perspective(data.openingAngle * 2, 1.f, nearFar.x, nearFar.y);
    viewProjection = math::mul(projection, view);
    invViewProjection = math::inverse(viewProjection);
}
