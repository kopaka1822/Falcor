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
    const std::string kShaderGenDebugRefFunction = kShaderFolder + "GenDebugRefFunction.rt.slang";
    const std::string kShaderStochSMRay = kShaderFolder + "GenStochasticSM.rt.slang";
    const std::string kShaderTemporalStochSMRay = kShaderFolder + "GenTmpStochSM.rt.slang";
    const std::string kShaderAccelShadowRay = kShaderFolder + "GenAccelShadow.rt.slang";

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
    FALCOR_ASSERT(mpPointSampler);
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

    bool sceneHasAnalyticLights = !mpScene->getLights().empty();

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    prepareDebugBuffers(pRenderContext);

    if (sceneHasAnalyticLights)
    {
        updateSMMatrices(pRenderContext, renderData);

        generateAVSM(pRenderContext, renderData);

        generateStochasticSM(pRenderContext, renderData);

        generateTmpStochSM(pRenderContext, renderData);

        generateAccelShadow(pRenderContext, renderData);
    }
    

    traceScene(pRenderContext, renderData);

    generateDebugRefFunction(pRenderContext, renderData);

    if (!mDebugFrameCount)
        mFrameCount++;

    mAVSMRebuildProgram = false;
    mAVSMTexResChanged = false;
}

void TransparencyPathTracer::updateSMMatrices(RenderContext* pRenderContext, const RenderData& renderData) {
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

void TransparencyPathTracer::generateAVSM(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate AVSM");

    if (mAVSMRebuildProgram)
    {
        mGenAVSMPip.pProgram.reset();
        mGenAVSMPip.pBindingTable.reset();
        mGenAVSMPip.pVars.reset();

        mTracer.pVars.reset(); //Recompile tracer program
        mAVSMTexResChanged = true; //Trigger texture reiinit
    }

    if (mAVSMTexResChanged)
    {
        mAVSMDepths.clear();
        mAVSMTransmittance.clear();
    }

    // Create AVSM trace program
    if(!mGenAVSMPip.pProgram){
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderAVSMRay);
        desc.setMaxPayloadSize(kMaxPayloadSizeAVSMPerK * mNumberAVSMSamples + 16);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1u);

        mGenAVSMPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mGenAVSMPip.pBindingTable;
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

        mGenAVSMPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    auto& lights = mpScene->getLights();

    // Create / Destroy resources
    //TODO MIPS and check formats
    {
        uint numTextures = lights.size() * (mNumberAVSMSamples / 4);
        if (mAVSMDepths.empty())
        {
            mAVSMDepths.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mAVSMDepths[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mAVSMDepths[i]->setName("AVSMDepth_" + std::to_string(i));
            }
        }

        if (mAVSMTransmittance.empty())
        {
            mAVSMTransmittance.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mAVSMTransmittance[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA8Unorm, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mAVSMTransmittance[i]->setName("AVSMTransmittance_" + std::to_string(i));
            }
        }
    }
    
    //Abort if disabled
    if (!mGenAVSM)
        return;

    // Defines
    mGenAVSMPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenAVSMPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));
    mGenAVSMPip.pProgram->addDefine("AVSM_UNDERESTIMATE", mAVSMUnderestimateArea ? "1" : "0");
    mGenAVSMPip.pProgram->addDefine("AVSM_REJECTION_MODE", std::to_string(mAVSMRejectionMode));
    mGenAVSMPip.pProgram->addDefine("USE_RANDOM_VARIANT", mAVSMUseRandomVariant ? "1" : "0");

    //Create Program Vars
    if (!mGenAVSMPip.pVars)
    {
        mGenAVSMPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenAVSMPip.pVars = RtProgramVars::create(mpDevice, mGenAVSMPip.pProgram, mGenAVSMPip.pBindingTable);
        mpSampleGenerator->setShaderData(mGenAVSMPip.pVars->getRootVar());
    }
        
    FALCOR_ASSERT(mGenAVSMPip.pVars);

    //Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        if (!lights[i]->isActive())
            break;
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());
        // Bind Utility
        auto var = mGenAVSMPip.pVars->getRootVar();
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;

        for (uint j = 0; j < mNumberAVSMSamples / 4; j++)
        {
            uint idx = i * (mNumberAVSMSamples/4) +j;
            var["gAVSMDepths"][j] = mAVSMDepths[idx];
            var["gAVSMTransmittance"][j] = mAVSMTransmittance[idx];
        }
        
        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenAVSMPip.pProgram.get(), mGenAVSMPip.pVars, uint3(targetDim, 1));
    }
}

void TransparencyPathTracer::generateStochasticSM(RenderContext* pRenderContext, const RenderData& renderData) {
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
    mGenStochSMPip.pProgram->addDefine("AVSM_UNDERESTIMATE", mAVSMUnderestimateArea ? "1" : "0");
    mGenStochSMPip.pProgram->addDefine("AVSM_REJECTION_MODE", std::to_string(mAVSMRejectionMode));

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

void TransparencyPathTracer::generateTmpStochSM(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "Generate Temporal Stochastic SM");
    if (mAVSMRebuildProgram)
    {
        mGenTmpStochSMPip.pProgram.reset();
        mGenTmpStochSMPip.pBindingTable.reset();
        mGenTmpStochSMPip.pVars.reset();

        mTracer.pVars.reset();     // Recompile tracer program
        mAVSMTexResChanged = true; // Trigger texture reiinit
    }

    if (mAVSMTexResChanged)
    {
        mTmpStochDepths.clear();
        mTmpStochTransmittance.clear();
    }

    // Create AVSM trace program
    if (!mGenTmpStochSMPip.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTemporalStochSMRay);
        desc.setMaxPayloadSize(kMaxPayloadSizeAVSMPerK * mNumberAVSMSamples + 32); //+18 cause of the sampleGen (16) and confidence weight
                                                                                   //(4) + align(4)
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1u);

        mGenTmpStochSMPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mGenTmpStochSMPip.pBindingTable;
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

        mGenTmpStochSMPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    auto& lights = mpScene->getLights();

    // Create / Destroy resources
    // TODO MIPS and check formats
    {
        uint numTextures = lights.size() * (mNumberAVSMSamples / 4);
        if (mTmpStochDepths.empty())
        {
            mTmpStochDepths.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mTmpStochDepths[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mTmpStochDepths[i]->setName("TmpStochSMDepth_" + std::to_string(i));
                pRenderContext->clearTexture(mTmpStochDepths[i].get(), float4(1));
            }
        }

        if (mTmpStochTransmittance.empty())
        {
            mTmpStochTransmittance.resize(numTextures);
            for (uint i = 0; i < numTextures; i++)
            {
                mTmpStochTransmittance[i] = Texture::create2D(
                    mpDevice, mSMSize, mSMSize, ResourceFormat::RGBA8Unorm, 1u, 1u, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mTmpStochTransmittance[i]->setName("TmpStochSMTransmittance_" + std::to_string(i));
                pRenderContext->clearTexture(mTmpStochTransmittance[i].get(), float4(0));
            }
        }
    }

    // Abort early if disabled
    if (!mGenStochSM)
        return;

    // Defines
    mGenTmpStochSMPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenTmpStochSMPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));
    mGenTmpStochSMPip.pProgram->addDefine("AVSM_UNDERESTIMATE", mAVSMUnderestimateArea ? "1" : "0");
    mGenTmpStochSMPip.pProgram->addDefine("AVSM_REJECTION_MODE", std::to_string(mAVSMRejectionMode));

    // Create Program Vars
    if (!mGenTmpStochSMPip.pVars)
    {
        mGenTmpStochSMPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenTmpStochSMPip.pVars = RtProgramVars::create(mpDevice, mGenTmpStochSMPip.pProgram, mGenTmpStochSMPip.pBindingTable);
        mpSampleGenerator->setShaderData(mGenTmpStochSMPip.pVars->getRootVar());
    }

    FALCOR_ASSERT(mGenTmpStochSMPip.pVars);

    // Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        if (!lights[i]->isActive())
            break;
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());
        // Bind Utility
        auto var = mGenTmpStochSMPip.pVars->getRootVar();
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;

        for (uint j = 0; j < mNumberAVSMSamples / 4; j++)
        {
            uint idx = i * (mNumberAVSMSamples / 4) + j;
            var["gStochDepths"][j] = mTmpStochDepths[idx];
            var["gStochTransmittance"][j] = mTmpStochTransmittance[idx];
        }

        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenTmpStochSMPip.pProgram.get(), mGenTmpStochSMPip.pVars, uint3(targetDim, 1));
    }
}

void TransparencyPathTracer::generateAccelShadow(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate Shadow Acceleration Structure");

    if (mAVSMTexResChanged)
    {
        mAccelShadowAABB.clear();
        mAccelShadowData.clear();
        mpShadowAccelerationStrucure.reset();
    }

    // Create AVSM trace program
    if (!mGenAccelShadowPip.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderAccelShadowRay);
        desc.setMaxPayloadSize(96u); //
                                                                                   //(4) + align(4)
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1u);

        mGenAccelShadowPip.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mGenAccelShadowPip.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("AVSM_K", std::to_string(mNumberAVSMSamples));
        defines.add(mpSampleGenerator->getDefines());

        mGenAccelShadowPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    auto& lights = mpScene->getLights();

    // Create / Destroy resources
    // TODO MIPS and check formats
    {
        uint numBuffers = lights.size();
        const uint approxNumElementsPerPixel = 8u;
        if (mAccelShadowAABB.empty())
        {
            mAccelShadowAABB.resize(numBuffers);
            for (uint i = 0; i < numBuffers; i++)
            {
                mAccelShadowAABB[i] = Buffer::createStructured(
                    mpDevice, sizeof(AABB), mSMSize * mSMSize * approxNumElementsPerPixel,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false
                );
                mAccelShadowAABB[i]->setName("AccelShadowAABB_" + std::to_string(i));
            }
        }
        if (mAccelShadowCounter.empty())
        {
            mAccelShadowCounter.resize(numBuffers);
            for (uint i = 0; i < numBuffers; i++)
            {
                mAccelShadowCounter[i] = Buffer::createStructured(
                    mpDevice, sizeof(uint), 1u,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false
                );
                mAccelShadowCounter[i]->setName("AccelShadowAABBCounter_" + std::to_string(i));
            }
        }
        if (mAccelShadowData.empty())
        {
            mAccelShadowData.resize(numBuffers);
            for (uint i = 0; i < numBuffers; i++)
            {
                mAccelShadowData[i] = Buffer::createStructured(
                    mpDevice, sizeof(float), mSMSize * mSMSize * approxNumElementsPerPixel,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false
                );
                mAccelShadowData[i]->setName("AccelShadowData" + std::to_string(i));
            }
        }

        if (!mpShadowAccelerationStrucure)
        {
            std::vector<uint64_t> aabbCount;
            std::vector<uint64_t> aabbGPUAddress;
            for (uint i = 0; i < numBuffers; i++)
            {
                aabbCount.push_back(mSMSize * mSMSize * approxNumElementsPerPixel);
                aabbGPUAddress.push_back(mAccelShadowAABB[i]->getGpuAddress());
            }
            mpShadowAccelerationStrucure = std::make_unique<CustomAccelerationStructure>(mpDevice, aabbCount, aabbGPUAddress);
        }
    }

    // Abort early if disabled
    if (!mGenAccelShadow)
        return;

    //Clear Counter
    for (uint i = 0; i < mAccelShadowCounter.size(); i++)
        pRenderContext->clearUAV(mAccelShadowCounter[i]->getUAV(0u, 1u).get(), uint4(0));

    // Defines
    mGenAccelShadowPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenAccelShadowPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));

    // Create Program Vars
    if (!mGenAccelShadowPip.pVars)
    {
        mGenAccelShadowPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenAccelShadowPip.pVars = RtProgramVars::create(mpDevice, mGenAccelShadowPip.pProgram, mGenAccelShadowPip.pBindingTable);
        mpSampleGenerator->setShaderData(mGenAccelShadowPip.pVars->getRootVar());
    }

    FALCOR_ASSERT(mGenAccelShadowPip.pVars);

    // Trace the pass for every light
    for (uint i = 0; i < lights.size(); i++)
    {
        if (!lights[i]->isActive())
            break;
        FALCOR_PROFILE(pRenderContext, lights[i]->getName());
        // Bind Utility
        auto var = mGenAccelShadowPip.pVars->getRootVar();
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = mShadowMapMVP[i].pos;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;

        var["gAABB"] = mAccelShadowAABB[i];
        var["gCounter"] = mAccelShadowCounter[i];
        var["gData"] = mAccelShadowData[i];

        // Get dimensions of ray dispatch.
        const uint2 targetDim = uint2(mSMSize);
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenAccelShadowPip.pProgram.get(), mGenAccelShadowPip.pVars, uint3(targetDim, 1));
    }

    //Build the Acceleration structure
    std::vector<uint64_t> aabbCount;
    for (uint i = 0; i < lights.size(); i++)
    {
        // aabbCount.push_back(753190u);
        aabbCount.push_back(512 * 512 * 8);
    }
        

    mpShadowAccelerationStrucure->update(pRenderContext, aabbCount);
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
    var["CB"]["gOutputGraphDebug"] = mGraphUISettings.genBuffers;
    var["CB"]["gDebugSelectedPixel"] = mGraphUISettings.selectedPixel;
    
    //Shadow Map
    
    var["SMCB"]["gSMSize"] = mSMSize;
    var["SMCB"]["gNear"] = mNearFar.x;
    var["SMCB"]["gFar"] = mNearFar.y;
    
    
    for (uint i = 0; i < lights.size(); i++)
        var["ShadowVPs"]["gShadowMapVP"][i] = mShadowMapMVP[i].viewProjection;
    for (uint i = 0; i < lights.size() * (mNumberAVSMSamples / 4); i++)
    {
        var["gAVSMDepths"][i] = mAVSMDepths[i];
        var["gAVSMTransmittance"][i] = mAVSMTransmittance[i];
        var["gStochDepths"][i] = mStochDepths[i];
        var["gStochTransmittance"][i] = mStochTransmittance[i];
        var["gTmpStochDepths"][i] = mTmpStochDepths[i];
        var["gTmpStochTransmittance"][i] = mTmpStochTransmittance[i];
    }

    mpShadowAccelerationStrucure->bindTlas(var, "gShadowAS");
    var["gAccelShadowData"] = mAccelShadowData[0];

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

    //Bind graph debug buffers
    if (mGraphFunctionDatas.size() >= 2)
    {
        for (uint i = 0; i < mGraphFunctionDatas[1].pointsBuffers.size(); i++)
        {
            var["gGraphBuffer"][i] = mGraphFunctionDatas[1].pointsBuffers[i];
        }
    }

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
}

void TransparencyPathTracer::prepareDebugBuffers(RenderContext* pRenderContext) {
    // Early return
    if (!mGraphUISettings.genBuffers)
        return;

    const uint numLights = mpScene->getLightCount();
    // Create Graph Debug buffers
    if (mGraphFunctionDatas[0].pointsBuffers.empty())
    {
        const size_t maxElements = kGraphDataMaxSize * sizeof(float2);
        std::vector<float2> initData(kGraphDataMaxSize, float2(1.f, 0.f));
        for (uint i = 0; i < mGraphFunctionDatas.size(); i++)
        {
            // Name
            if (i == 0)
                mGraphFunctionDatas[i].name = "Reference";
            else
                mGraphFunctionDatas[i].name = "ShadowMap";

            // Create GPU buffers
            mGraphFunctionDatas[i].pointsBuffers.resize(numLights);
            for (uint j = 0; j < numLights; j++)
            {
                mGraphFunctionDatas[i].pointsBuffers[j] = Buffer::createStructured(
                    mpDevice, sizeof(float), kGraphDataMaxSize * 2, ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None,
                    initData.data(), false
                );
                mGraphFunctionDatas[i].pointsBuffers[j]->setName(
                    "GraphDataBuffer_Light_" + std::to_string(j) + "_" + mGraphFunctionDatas[i].name
                );
            }

            // Create Vectors
            mGraphFunctionDatas[i].cpuData.resize(numLights);
            for (uint j = 0; j < numLights; j++)
            {
                std::fill(std::begin(mGraphFunctionDatas[i].cpuData[j]), std::end(mGraphFunctionDatas[i].cpuData[j]), float2(1.f, 0.f));
            }
        }
    }
}

void TransparencyPathTracer::generateDebugRefFunction(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Early return
    if (!mGraphUISettings.genBuffers)
        return;

    const uint numLights = mpScene->getLightCount();

    //Create the ray tracing program
    if (!mDebugGetRefFunction.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderGenDebugRefFunction);
        desc.setMaxPayloadSize(16u);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);

        mDebugGetRefFunction.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mDebugGetRefFunction.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(
                0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit")
            );
        }

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("NUM_LIGHTS", std::to_string(mpScene->getLightCount()));
        defines.add("MAX_ELEMENTS", std::to_string(kGraphDataMaxSize));

        mDebugGetRefFunction.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    //Runtime defs
    mDebugGetRefFunction.pProgram->addDefine("AVSM_DEPTH_BIAS", mGraphUISettings.addDepthBias ? std::to_string(mDepthBias) : "0.0");
    mDebugGetRefFunction.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", mGraphUISettings.addDepthBias ? std::to_string(mNormalDepthBias) : "0.0");

    // Create Program Vars
    if (!mDebugGetRefFunction.pVars)
    {
        mDebugGetRefFunction.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mDebugGetRefFunction.pVars = RtProgramVars::create(mpDevice, mDebugGetRefFunction.pProgram, mDebugGetRefFunction.pBindingTable);
    }

    
    auto var = mDebugGetRefFunction.pVars->getRootVar();
    var["CB"]["gSelectedPixel"] = mGraphUISettings.selectedPixel;
    var["CB"]["gNear"] = mNearFar.x;
    var["CB"]["gFar"] = mNearFar.y;
    var["CB"]["gSMRes"] = mSMSize;
    for (uint i = 0; i < numLights; i++)
    {
        FALCOR_ASSERT(i < mShadowMapMVP.size());
        var["CB"]["gViewProj"][i] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"][i] = mShadowMapMVP[i].invViewProjection;
    }

    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture(); //VBuffer to convert selected pixel to shadow map pixel
    for (uint i=0; i<numLights; i++)
        var["gFuncData"][i] = mGraphFunctionDatas[0].pointsBuffers[i];

    mpScene->raytrace(pRenderContext, mDebugGetRefFunction.pProgram.get(), mDebugGetRefFunction.pVars, uint3(numLights,1, 1));

    //Copy (ref) to cpu buffer
    for (uint i = 0; i < numLights; i++)
    {
        const float2* pBuf = static_cast<const float2*>(mGraphFunctionDatas[0].pointsBuffers[i]->map(Buffer::MapType::Read));
        FALCOR_ASSERT(pBuf);
        std::memcpy(mGraphFunctionDatas[0].cpuData[i].data(), pBuf, kGraphDataMaxSize * sizeof(float2));
        mGraphFunctionDatas[0].pointsBuffers[i]->unmap();
    }

    //Change the selected light index to the one with the most valid samples
    uint mostValidSamples = 0;
    for (uint i = 0; i < numLights; i++){
        for (uint j = 0; j < kGraphDataMaxSize; j++) {
            if (mGraphFunctionDatas[0].cpuData[i][j].x >= 1.0){
                if (j > mostValidSamples){
                    mostValidSamples = j;
                    mGraphUISettings.selectedLight = i;
                }
                break;
            }
        }
    }

    //Copy (function) to cpu buffer
    for (uint i = 0; i < numLights; i++)
    {
        const float2* pBuf = static_cast<const float2*>(mGraphFunctionDatas[1].pointsBuffers[i]->map(Buffer::MapType::Read));
        FALCOR_ASSERT(pBuf);
        std::memcpy(mGraphFunctionDatas[1].cpuData[i].data(), pBuf, kGraphDataMaxSize * sizeof(float2));
        mGraphFunctionDatas[1].pointsBuffers[i]->unmap();
    }

    //Copy step function setting
    mGraphUISettings.asStepFuction = !mAVSMUseInterpolation;

    mGraphUISettings.genBuffers = false;
}

void drawText(const ImVec2& p, std::string text, uint32_t color = 0xffffffff) {
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cursorPos.x + p.x, cursorPos.y + p.y), color, text.c_str()
    );
}

void drawCircleFilled(const ImVec2& p, float radius, uint32_t color = 0xffffffff, const ImVec2& offset = ImVec2(0.f, 0.f))
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    cursorPos = ImVec2(cursorPos.x + offset.x, cursorPos.y + offset.y);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cursorPos.x + p.x, cursorPos.y + p.y), radius, color);
}

void drawLine(const ImVec2& p1, const ImVec2& p2, float thickness,uint32_t color = 0xffffffff, const ImVec2& offset = ImVec2(0.f,0.f))
{
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    cursorPos = ImVec2(cursorPos.x + offset.x,cursorPos.y + offset.y) ;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cursorPos.x + p1.x, cursorPos.y + p1.y), ImVec2(cursorPos.x + p2.x, cursorPos.y + p2.y), color, thickness
    );
};

void drawRectangle(const ImVec2& p1, const ImVec2& size, float thickness = 1.f ,uint32_t color = 0xffffffff) {
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRect(
        ImVec2(cursorPos.x + p1.x, cursorPos.y + p1.y), ImVec2(cursorPos.x + p1.x + size.x, cursorPos.y + p1.y + size.y), color, 0.f, 0, thickness
    );
}

void drawBackground(const ImVec2& graphOffset, const ImVec2& graphSize, const ImVec2& size, float2 minMaxDepth , float borderThickness)
{
    const float textOffset = 8.f;   //Y offset
    const float textOffsetD = 25.f; //X offset for depths
    // Draw border
    drawLine(ImVec2(graphOffset.x, 0.f), ImVec2(graphOffset.x, graphOffset.y + graphSize.y), borderThickness);
    drawLine(ImVec2(graphOffset.x, graphOffset.y + graphSize.y), ImVec2(size.x, graphOffset.y + graphSize.y), borderThickness);
    // Transparency lines and text
    float textPosY = graphOffset.y - textOffset;
    const uint steps = 5;
    const float stepSize = 1.0f / steps;
    float transparency = 1.f;
    for (uint i = 0; i < steps; i++)
    {
        float heightOffset = graphSize.y * (stepSize * i);
        std::string text = std::to_string(transparency);
        text = text.substr(0, text.find(".") + 3);
        drawText(ImVec2(0.f, textPosY + heightOffset), text);
        drawLine(
            ImVec2(graphOffset.x, graphOffset.y + heightOffset), ImVec2(graphOffset.x + graphSize.x, graphOffset.y + heightOffset),
            borderThickness / 2.f, kTransparentWhiteColor
        );
        transparency -= stepSize;
    }
    drawText(ImVec2(0.f, textPosY + graphSize.y), "0.0");

    //Scaled depth line and text
    textPosY = graphOffset.y + graphSize.y + textOffset * 2; //Fixed height
    const float depthRange = minMaxDepth.y - minMaxDepth.x;
    const float depthStep = stepSize * depthRange;
    for (uint i = 0; i <= steps; i++)
    {
        float depth = minMaxDepth.x + depthStep * i;
        std::string depthStr = std::to_string(depth);
        depthStr = depthStr.substr(0, depthStr.find(".") + 4);
        float xOffset = graphSize.x * stepSize * i;
        drawText(ImVec2(graphOffset.x - textOffsetD + xOffset, textPosY), depthStr);
        if (i > 0)
        {
            drawLine(
                ImVec2(graphOffset.x + xOffset, graphOffset.y + graphSize.y), ImVec2(graphOffset.x + xOffset, graphOffset.y),
                borderThickness / 2.f, kTransparentWhiteColor
            );
        }
    }
}

void TransparencyPathTracer::renderDebugGraph(const ImVec2& size) {
    const ImVec2 graphOffset = ImVec2(40.f, 18.f);
    const ImVec2 graphOffset2 = ImVec2(40.f, 75.f); // Somehow right and bottom needs a bigger offset
    const ImVec2 graphSize = ImVec2(size.x - (graphOffset.x + graphOffset2.x), size.y - (graphOffset.y + graphOffset2.y));
    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    mousePos.x -= screenPos.x;
    mousePos.y -= screenPos.y;

    //Find min and max depth for depth scaling
    float minDepth = 1.f;
    float maxDepth = 0.f;

    for (uint k = 0; k < mGraphFunctionDatas.size() && mGraphUISettings.enableDepthScaling; k++)
    {
        auto& graphVec = mGraphFunctionDatas[k].cpuData[mGraphUISettings.selectedLight];
        const int numElements = k > 0 ? mNumberAVSMSamples : graphVec.size();
        for (int i = 0; i < numElements; i++)
        {
            if (graphVec[i].x >= 1.0)
                break;
            minDepth = math::min(minDepth, graphVec[i].x);
            maxDepth = math::max(maxDepth, graphVec[i].x);
        }
    }

    bool depthsValid = (minDepth > maxDepth);
    minDepth = depthsValid ? 0.f : math::max(minDepth - mGraphUISettings.depthRangeOffset, 0.f);
    maxDepth = depthsValid ? 1.f : math::min(maxDepth + mGraphUISettings.depthRangeOffset, 1.f);

    drawBackground(graphOffset, graphSize, size, float2(minDepth, maxDepth), mGraphUISettings.borderThickness);

    // For point highlighting
    std::optional<float> highlightValueD;
    std::optional<float> highlightValueT;

    //Start loop over all functions
    for (int k = 0; k < kGraphMaxFunctions; k++)
    {
        const int colorIndex = k * 2;
        const int graphDataIdx = k > 0 ? 1 : 0;
        const int functionIdx = std::max(k - 1,0);      //Only valid if k>0
        if (((mGraphFunctionDatas[graphDataIdx].show >> functionIdx) & 0b1) == 0)
            continue;

        auto& origGraphVec = mGraphFunctionDatas[graphDataIdx].cpuData[mGraphUISettings.selectedLight];
        const int indexOffset = functionIdx * mNumberAVSMSamples; //offset for k > 1

        //Get last valid element for loop
        const int numElements = k > 0 ? mNumberAVSMSamples : origGraphVec.size();
        int lastElement = numElements;
        for (int i = 0; i < numElements; i++)
        {
            if (origGraphVec[i + indexOffset].x >= 1.0)
            {
                lastElement = i;
                break;
            }
            else if (origGraphVec[i + indexOffset].y <= 0)
            {
                lastElement = i + 1;
                break;
            }
        }

        //Nothing to render, abort
        if (lastElement == 0)
            continue;

        //Create a copy of the vector with scaled depths
        std::vector<float2> graphVec(lastElement);
        float depthRange = maxDepth - minDepth;
        for (int i = 0; i < lastElement; i++)
            graphVec[i] = float2((origGraphVec[i + indexOffset].x - minDepth) / depthRange, origGraphVec[i + indexOffset].y);

        // Connection from left side to element (same for both modes)
        if (graphVec[0].x > 0 && (lastElement != 0))
        {
            ImVec2 a = ImVec2(0.f, 0.f);
            ImVec2 b = ImVec2(graphVec[0].x * graphSize.x, 0.f);
            ImVec2 c = ImVec2(graphVec[0].x * graphSize.x, (1.f - graphVec[0].y) * graphSize.y);
            drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
            drawLine(b, c, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
        }
        // Draw the graph lines as either step or linear function
        if (mGraphUISettings.asStepFuction)
        {
            for (int i = 0; i < lastElement - 1; i++)
            {
                ImVec2 a = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
                ImVec2 b = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
                ImVec2 c = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i + 1].y) * graphSize.y);
                drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
                drawLine(b, c, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
            }
        }
        else // linear function
        {
            for (int i = 0; i < lastElement - 2; i++)
            {
                ImVec2 a = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
                ImVec2 b = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i + 1].y) * graphSize.y);
                drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
            }
            //Last element can be a step function if the last element has 0 transparency (is solid)
            int idx = lastElement - 2;
            if (graphVec[lastElement - 1].y > 0 && idx > 0) //Linear
            {
                ImVec2 a = ImVec2(graphVec[idx].x * graphSize.x, (1.f - graphVec[idx].y) * graphSize.y);
                ImVec2 b = ImVec2(graphVec[idx + 1].x * graphSize.x, (1.f - graphVec[idx + 1].y) * graphSize.y);
                drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
            }
            else if (idx > 0) //Step
            {
                ImVec2 a = ImVec2(graphVec[idx].x * graphSize.x, (1.f - graphVec[idx].y) * graphSize.y);
                ImVec2 b = ImVec2(graphVec[idx + 1].x * graphSize.x, (1.f - graphVec[idx].y) * graphSize.y);
                ImVec2 c = ImVec2(graphVec[idx + 1].x * graphSize.x, (1.f - graphVec[idx + 1].y) * graphSize.y);
                drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
                drawLine(b, c, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
            }
        }
        // Draw to right side if transparency is not zero (same for both modes)
        if ((lastElement > 0) && graphVec[lastElement - 1].y > 0)
        {
            ImVec2 a = ImVec2(graphVec[lastElement - 1].x * graphSize.x, (1.f - graphVec[lastElement - 1].y) * graphSize.y);
            ImVec2 b = ImVec2(graphSize.x, (1.f - graphVec[lastElement - 1].y) * graphSize.y);
            drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[colorIndex], graphOffset);
        }

        // Draw the sample points
        int highlightIndex = -1;
        for (size_t i = 0; i < lastElement; i++)
        {
            ImVec2 el = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
            el.x += graphOffset.x;
            el.y += graphOffset.y;
            drawCircleFilled(el, mGraphUISettings.radiusSize, kColorPalette[colorIndex + 1]);

            // Check if the mouse hovers over the point
            const float rad = mGraphUISettings.radiusSize;
            if (mousePos.x >= el.x - rad && mousePos.x < el.x + rad && mousePos.y >= el.y - rad && mousePos.y < el.y + rad)
            {
                highlightIndex = i;
                highlightValueD = origGraphVec[i + indexOffset].x; //unscaled depths
                highlightValueT = graphVec[i].y;
            }
        }
        // Draw highligh circle
        if (highlightIndex >= 0)
        {
            ImVec2 el = ImVec2(graphVec[highlightIndex].x * graphSize.x, (1.f - graphVec[highlightIndex].y) * graphSize.y);
            drawCircleFilled(el, mGraphUISettings.radiusSize, kHighlightColor, graphOffset);
        }
    }

    //Info for selected point
    if (highlightValueD && highlightValueT)
    {
        ImGui::BeginTooltip();
        ImGui::Text("Depth: %.3f\nTransmittance:%.1f%%", *highlightValueD, *highlightValueT * 100.f);
        ImGui::EndTooltip();
    }
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

    dirty |= widget.checkbox("Generate Adaptive Volumetric Shadow Maps (AVSM)", mGenAVSM);
    dirty |= widget.checkbox("Generate Stochastic Shadow Maps", mGenStochSM);
    dirty |= widget.checkbox("Generate (Test) Shadow Maps", mGenTmpStochSM);
    dirty |= widget.dropdown("Shadow Render Mode", mShadowEvaluationMode);

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
        bool modeChanged = group.dropdown("AVSM Rejection Mode", kAVSMRejectionMode, mAVSMRejectionMode);
        group.tooltip(
            "Triangle Area: First and last point is fix. Uses 3 neighboring points for the triangle area \n"
            "Rectangle Area: First point is fix. Uses two neighboring points for the rectangle area \n"
            "Height: Uses the height difference between two points \n"
            "Height Error Heuristic: Height mode but additionally weights the height with the expected error"
        );
        if (modeChanged)
            mAVSMUnderestimateArea = mAVSMRejectionMode > 0;
        group.checkbox("Underestimate Rejection", mAVSMUnderestimateArea);
        group.tooltip("Enables underestimation where the lowest transparacy is taken when a sample is removed");
        group.checkbox("Enable random rejection", mAVSMUseRandomVariant);
        group.tooltip("Enables random rejection based on the rejection mode weights");

        group.checkbox("Use PCF", mAVSMUsePCF); //TODO add other kernels
        group.tooltip("Enable 2x2 PCF using gather");
    }
    

    static bool graphOpenedFirstTime = true;
    
    if (mGraphUISettings.graphOpen && (!mGraphFunctionDatas.empty()))
    {
        if (graphOpenedFirstTime)
        {
            //This is set for full HD
            ImGui::SetNextWindowSize(ImVec2(1450.f, 900.f));
            ImGui::SetNextWindowPos(ImVec2(400.f, 125.f)); 
            graphOpenedFirstTime = false;
        }
        
        ImGui::Begin("Transmittance Function", &mGraphUISettings.graphOpen);
        renderDebugGraph(ImGui::GetWindowSize());
        ImGui::End();
    }

    
    if (auto group = widget.group("Graph Settings"))
    {
        if (mGraphUISettings.selectedPixel.x >= 0 && mGraphUISettings.selectedPixel.y >= 0 && !mGraphUISettings.selectPixelButton)
        {
            group.text(
                "Selected Pixel :" + std::to_string(mGraphUISettings.selectedPixel.x) + "," +
                std::to_string(mGraphUISettings.selectedPixel.y)
            );
        }

        if (!mGraphUISettings.graphOpen)
        {
            bool buttonInSameLine = false;
            if (mGraphUISettings.selectedPixel.x >= 0 && mGraphUISettings.selectedPixel.y >= 0 && !mGraphUISettings.selectPixelButton)
            {
                bool recreate = group.button("Recreate");
                bool reopen = group.button("Reopen",true);
                mGraphUISettings.genBuffers |= recreate;
                mGraphUISettings.graphOpen |= recreate | reopen;
                buttonInSameLine = true;
            }
            else
            {
                group.text("Please select a pixel");
            }

            if (mGraphUISettings.selectPixelButton)
                group.text("Press \"Right Click\" to select a pixel");
            else
                mGraphUISettings.selectPixelButton |= group.button("Select Pixel", buttonInSameLine);
                
        }

        if (mGraphUISettings.graphOpen)
        {
            if (mpScene && mpScene->getLightCount() > 0)
                group.slider("Selected Light", mGraphUISettings.selectedLight, 0u, mpScene->getLightCount() - 1);

            //Legend
            // Get color
            auto convertColorF4 = [&](const uint32_t& imColor) {
                float4 color = float4(0.f);
                color.r = (imColor & 0xFF) / 255.f;
                color.g = ((imColor>>8) & 0xFF) / 255.f;
                color.b = ((imColor>>16) & 0xFF) / 255.f;
                color.a = ((imColor>>24) & 0xFF) / 255.f;
                return color;
            };
            //Create tmp bools for checkbox
            bool showChanged = false;
            bool showRef = mGraphFunctionDatas[0].show & 0b1;
            bool showAVSM = mGraphFunctionDatas[1].show & 0b1;
            bool showStochSM = mGraphFunctionDatas[1].show >> 1 & 0b1;
            group.text("Graphs:");
            showChanged |= group.checkbox("Reference", showRef);
            group.rect(float2(20.f), convertColorF4(kColorPalette[0]), true,true);
            group.dummy("", float2(0.f));
            showChanged |= group.checkbox("AVSM", showAVSM);
            group.rect(float2(20.f), convertColorF4(kColorPalette[2]), true, true);
            group.dummy("", float2(0.f));
            showChanged |= group.checkbox("StochSM", showStochSM);
            group.rect(float2(20.f), convertColorF4(kColorPalette[4]), true, true);
            group.dummy("", float2(0.f));

            //Write back into the bitmask
            if (showChanged)
            {
                //Clear and set the n th bit of data. bit should either be 0 or 1
                auto clearAndSetBit = [](uint& data, uint n, uint bit) {
                    data = (data & ~(1u << n)) | (bit << n);
                };
                clearAndSetBit(mGraphFunctionDatas[0].show, 0, showRef ? 1 : 0);
                clearAndSetBit(mGraphFunctionDatas[1].show, 0, showAVSM ? 1 : 0);
                clearAndSetBit(mGraphFunctionDatas[1].show, 1, showStochSM ? 1 : 0);
            }
            

            group.separator();
        }
        group.text("Settings");
        group.checkbox("As step function", mGraphUISettings.asStepFuction);
        group.checkbox("Add depth bias to ref", mGraphUISettings.addDepthBias);
        group.checkbox("Scale Depth Range", mGraphUISettings.enableDepthScaling);
        if (mGraphUISettings.enableDepthScaling)
            group.var("Depth Scaling Offset", mGraphUISettings.depthRangeOffset, 0.f, 1.f, 0.001f);
        group.var("Point Radius", mGraphUISettings.radiusSize, 1.f, 128.f, 0.1f);
        group.var("Line Thickness", mGraphUISettings.lineThickness, 1.f, 128.f, 0.1f);
        group.var("Border Thickness", mGraphUISettings.borderThickness, 1.f, 128.f, 0.1f);
    }

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

            mTracer.pBindingTable = RtBindingTable::create(3, 3, mpScene->getGeometryCount());
            auto& sbt = mTracer.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("alphaMiss"));
            sbt->setMiss(1, desc.addMiss("shadowMiss"));
            sbt->setMiss(2, desc.addMiss("shadowAccelMiss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("alphaClosestHit", "alphaAnyHit")
                );
                sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowAnyHit"));
                sbt->setHitGroup(2, 0, desc.addHitGroup("", "shadowAccelAnyHit", "shadowAccelIntersection"));
            }

            mTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }        
    }
}

bool TransparencyPathTracer::onMouseEvent(const MouseEvent& mouseEvent)
{
    static uint2 mouseScreenPos = uint2(0);

    if (mGraphUISettings.selectPixelButton)
    {
        if (mouseEvent.type == MouseEvent::Type::Move)
        {
            mouseScreenPos = mouseEvent.screenPos;
            return true;
        }

        if (mouseEvent.type == MouseEvent::Type::ButtonDown && mouseEvent.button == Input::MouseButton::Right)
        {
            mGraphUISettings.selectedPixel = mouseScreenPos;
            mGraphUISettings.genBuffers = true;
            mGraphUISettings.graphOpen = true;
            mGraphUISettings.selectPixelButton = false;
            mGraphUISettings.mouseDown = false;
            return false;
        }
    }

    return false;
}

void TransparencyPathTracer::prepareVars() {
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
