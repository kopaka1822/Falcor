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
#include "TestShadowMap.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TestShadowMap>();
}

namespace
{
const char kShaderFile[] = "RenderPasses/TestShadowMap/TestShadowMap.rt.slang";
const char kShaderSMGeneration[] = "RenderPasses/TestShadowMap/GenerateShadowMap.rt.slang";
const char kShaderReverseSMGen[] = "RenderPasses/TestShadowMap/ReverseSMGen.rt.slang";
const char kShaderSparseSMGen[] = "RenderPasses/TestShadowMap/SparseShadowMap.rt.slang";
const char kShaderDebugSM[] = "RenderPasses/TestShadowMap/DebugShadowMap.cs.slang";
const char kShaderMinMaxMips[] = "RenderPasses/TestShadowMap/GenMinMaxMips.cs.slang";
const char kShaderRayNeeded[] = "RenderPasses/TestShadowMap/CreateRayNeededMask.cs.slang";
const char kShaderLayeredVariance[] = "RenderPasses/TestShadowMap/LayeredVariance.cs.slang";

//Virtual Layers Shaders
const char kShaderVLVSMCreateLayers[] = "RenderPasses/TestShadowMap/VLVSM_CreateLayers.cs.slang";
const char kShaderEvaluateVirtualLayers[] = "RenderPasses/TestShadowMap/VLVSM_EvaluateLayers.cs.slang";
const char kShaderVLVSMBlur[] = "RenderPasses/TestShadowMap/VLVSM_Blur.cs.slang";

// Ray tracing settings that affect the traversal stack size.
// These should be set as small as possible.
const uint32_t kMaxPayloadSizeBytes = 16u;
const uint32_t kMaxRecursionDepth = 1u;

const char kInputVBuffer[] = "vbuffer";
const char kInputViewDir[] = "viewW";

const ChannelList kInputChannels = {
    {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
    {kInputViewDir, "gViewW", "World-space view direction (xyz float format)", true /* optional */},
};

const ChannelList kOutputChannels = {
    {"color", "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float},
    {"debug", "gDebug", "Debug", false, ResourceFormat::RGBA32Float},
};

// Properties Parsing
const char kShadowTracingMode[] = "shadowTracingMode";

const Gui::DropdownList kBlockSizes{
    {4, "4x4"}, {8, "8x8"}, {16, "16x16"}, {32, "32x32"}, {64, "64x64"},
};

const Gui::DropdownList kShadowMapSizes{
    {256, "256x256"}, {512, "512x512"}, {1024, "1024x1024"}, {2048, "2048x2048"}, {4096, "4096x4096"},
};

const Gui::DropdownList kSMGenerationRenderer{
    {0, "Rasterizer"},
    {1, "RayTracing"},
};

const Gui::DropdownList kLayeredVSMLayers{
    {2, "2"},
    {4, "4"},
    {8, "8"},
    {16, "16"},
};

} // namespace

TestShadowMap::TestShadowMap(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);

    mpGaussianBlur = std::make_unique<SMGaussianBlur>(mpDevice);

    // Create Pixel Stats
    mpPixelStats = std::make_unique<PixelStats>(mpDevice);

    // Create samplers.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpShadowSamplerPoint = Sampler::create(mpDevice, samplerDesc);
    FALCOR_ASSERT(mpShadowSamplerPoint);
    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    mpShadowSamplerLinear = Sampler::create(mpDevice, samplerDesc);
    FALCOR_ASSERT(mpShadowSamplerLinear);
}

void TestShadowMap::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kShadowTracingMode)
            mShadowMode = value;
        else
            logWarning("Unknown property '{}' in TestShadowMap properties.", key);
    }
}

Properties TestShadowMap::getProperties() const
{
    Properties props;
    props[kShadowTracingMode] = mShadowMode;
    return props;
}

RenderPassReflection TestShadowMap::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget);

    return reflector;
}

void TestShadowMap::execute(RenderContext* pRenderContext, const RenderData& renderData)
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

    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        throw RuntimeError("TestShadowMap: This render pass does not support scene geometry changes.");
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    // Configure depth-of-field.
    const bool useDOF = mpScene->getCamera()->getApertureRadius() > 0.f;
    if (useDOF && renderData[kInputViewDir] == nullptr)
    {
        logWarning("Depth-of-field requires the '{}' input. Expect incorrect shading.", kInputViewDir);
    }

    // Handle Buffers
    prepareBuffers();

    // Debug Accumulate
    auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
    bool refresh = is_set(RenderPassRefreshFlags::RenderOptionsChanged, flags);
    auto cameraChanges = mpScene->getCamera()->getChanges();
    auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
    refresh |= (cameraChanges & ~excluded) != Camera::Changes::None;
    if ((refresh || mResetDebugAccumulate) || !mAccumulateDebug)
    {
        mIterationCount = 0;
        mResetDebugAccumulate = false;
        mClearDebugAccessTex = true;
        ref<Texture> debugTex = renderData.getTexture("debug");
        pRenderContext->clearRtv(debugTex->getRTV().get(), float4(0, 0, 0, 1));
    }

    // Debug Access Tex
    if (mClearDebugAccessTex)
    {
        if (!mpShadowMapAccessTex.empty())
        {
            for (uint i = 0; i < mpShadowMapAccessTex.size(); i++)
                pRenderContext->clearUAV(mpShadowMapAccessTex[i]->getUAV(0, 0, 1u).get(), uint4(0));
        }
        mClearDebugAccessTex = false;
    }

    // Pixel Stats
    mpPixelStats->beginFrame(pRenderContext, renderData.getDefaultTextureDims());

    // Shadow Map Generation
    if (mSMGenerationUseRay) // Ray
    {
        generateShadowMap(pRenderContext, renderData);

        if (mUseReverseSM)
        {
            genReverseSM(pRenderContext, renderData);
        }

        if (mpRasterShadowMap)
        {
            mpRasterShadowMap.reset();
            mTracer.pVars.reset();
        }
    }
    else // Raster
    {
        if (!mpRasterShadowMap)
        {
            mpRasterShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);
            mTracer.pVars.reset();
        }
        mpRasterShadowMap->update(pRenderContext);
    }

    if (mFilterSMMode == FilterSMMode::LayeredVariance)
    {
        layeredVarianceSMPass(pRenderContext, renderData);
    }
    else if (mFilterSMMode == FilterSMMode::VirtualLayeredVariance)
    {
        virtualLayeredVarianceSMPass(pRenderContext, renderData);
    }
    else
    {
        traceScene(pRenderContext, renderData);
    }
        

    // Debug pass
    if (is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag))
        debugShadowMapPass(pRenderContext, renderData);

    mpPixelStats->endFrame(pRenderContext);
    mNearFarChanged = false;
    mFrameCount++;
    mIterationCount++;
}

void TestShadowMap::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.checkbox("Check for NaN and Inf", mCheckForNaN);
    widget.tooltip("Enable checks for NaN and Inf before writing into the output texture.");

    dirty |= widget.dropdown("Analytic Light Sample Mode", mPathLightSampleMode);
    widget.tooltip("Select the mode for sampling the analytic lights");

    dirty |= widget.dropdown("Shadow Render Mode", mShadowMode);
    widget.dropdown("SM Renderer", kSMGenerationRenderer, mSMGenerationUseRay);

    if (auto group = widget.group("ShadowMap Options"))
    {
        // Ray SM Options
        if (mSMGenerationUseRay)
        {
            mRebuildSMBuffers |= group.dropdown("Shadow Map Size", kShadowMapSizes, mShadowMapSize);
            mRebuildSMBuffers |= group.dropdown("Filter SM Mode", mFilterSMMode);
            group.tooltip("Filtered shadow map is always recreated from one depth");

            dirty |= group.checkbox("Always Render Shadow Map", mAlwaysRenderSM);

            dirty |= group.checkbox("Use SM for direct light", mUseSMForDirect);

            //Special Modes. Only one should be toggled on
            if (auto specGroup = group.group("SpecialModes"))
            {
                //Only one is allowed to be active
                bool oneActive = mUseReverseSM;

                if (!oneActive || mUseReverseSM)
                    dirty |= specGroup.checkbox("Use Reverse SM", mUseReverseSM);

            }

            if (mFilterSMMode == FilterSMMode::LayeredVariance)
            {
                if (auto layerGroup = group.group("Layered Variance Options"))
                {
                    dirty |= layerGroup.dropdown("Layers", kLayeredVSMLayers ,mLayeredVarianceData.layers);
                    dirty |= layerGroup.var("Layer Overlap", mLayeredVarianceData.overlap, 0.0f, 1.0f, 0.00001f);
                }
            }

            mRerenderSM |= group.checkbox("Use optimized near far", mUseOptimizedNearFarForShadowMap);
            group.tooltip("Optimized near far is calculated using an additional ray tracing depth pass");

            if (mUseOptimizedNearFarForShadowMap && mpScene)
            {
                group.slider("Selected Light", mUISelectedLight, 0u, mpScene->getLightCount() - 1);
                group.text(
                    "Near:" + std::to_string(mNearFarPerLight[mUISelectedLight].x) +
                    ", Far: " + std::to_string(mNearFarPerLight[mUISelectedLight].y)
                );
            }
            else
            {
                mNearFarChanged |= group.var("SM Near/Far", mNearFar, 0.f, FLT_MAX, 0.001f);
                group.tooltip("Sets the near and far for the shadow map");
            }
           
            group.checkbox("Use Ray outside of SM", mDistributeRayOutsideOfSM);

            if (auto blurGroup = group.group("Pre-Blur Options"))
            {
                blurGroup.checkbox("Enable", mEnableBlur);
                if (mEnableBlur)
                {
                    mpGaussianBlur->renderUI(blurGroup);
                }
            }

            mRerenderSM |= group.button("Reset Shadow Map");

            dirty |= mRebuildSMBuffers || mRerenderSM;
        }
        else if (mpRasterShadowMap) // Raster SM options
        {
            dirty |= mpRasterShadowMap->renderUILeakTracing(group, mShadowMode == ShadowMode::LeakTracing);
        }
    }

    if (auto group = widget.group("Debug"))
    {
        mResetDebugAccumulate |= group.checkbox("Enable", mEnableDebug);
        if (mEnableDebug)
        {
            bool debugModeChanged = group.dropdown("Debug Mode", mDebugMode);
            mResetDebugAccumulate |= debugModeChanged;

            if (mDebugMode != PathSMDebugModes::ShadowMap)
            {
                mResetDebugAccumulate |= group.checkbox("Accumulate Debug", mAccumulateDebug);
                mResetDebugAccumulate |= group.button("Reset Accumulate");
            }

            if (is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) && mpScene)
            {
                group.slider("Select Light", mUISelectedLight, 0u, mpScene->getLightCount() - 1);
                if (mFilterSMMode == FilterSMMode::LayeredVariance)
                {
                    group.slider("Selected Layer", mUISelectedVarianceLayer, 0u, mLayeredVarianceData.layers - 1);
                }
            }

            if (mDebugMode == PathSMDebugModes::ShadowMapAccess || mDebugMode == PathSMDebugModes::ShadowMapRayDiff ||
                mDebugMode == PathSMDebugModes::ReverseSM || mDebugMode == PathSMDebugModes::RayNeededMask)
            {
                if (debugModeChanged)
                {
                    if (mDebugMode == PathSMDebugModes::ReverseSM)
                        mDebugBrighnessMod = float2(1.0);
                    else
                        mDebugBrighnessMod = float2(5.f, 1.f);
                }
                group.var("Blend Val", mDebugAccessBlendVal, 0.f, 1.f, 0.001f);
                group.var("HeatMap/SM Brightness", mDebugBrighnessMod, 0.f, FLT_MAX, 0.001f);
                group.var(
                    "HeatMap max val",
                    mDebugMode == PathSMDebugModes::ShadowMapAccess ? mDebugHeatMapMaxCountAccess : mDebugHeatMapMaxCountDifference, 0.f,
                    FLT_MAX, mDebugMode == PathSMDebugModes::ShadowMapAccess ? 0.1f : 0.001f
                );
                group.checkbox("Use correct aspect", mDebugUseSMAspect);
            }

            
            mResetDebugAccumulate |= group.var("Debug Factor", mDebugMult, 0.f, FLT_MAX, 0.1f);
            group.tooltip("Multiplicator for the debug value");
        }
    }

    if (auto group = widget.group("Statistics"))
    {
        mpPixelStats->renderUI(group);
    }

    // If rendering options that modify the output have changed, set flag to indicate that.
    // In execute() we will pass the flag to other passes for reset of temporal data etc.
    mOptionsChanged |= dirty;
}

void TestShadowMap::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Clear data for previous scene.
    // After changing scene, the raytracing program should to be recreated.
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mGenerateSM.pProgram = nullptr;
    mGenerateSM.pBindingTable = nullptr;
    mGenerateSM.pVars = nullptr;
    mFrameCount = 0;
    mRebuildSMBuffers = true;
    mResetDebugAccumulate = true;
    // Set new scene.
    mpScene = pScene;

    if (mpScene)
    {
        if (pScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("TestShadowMap: This render pass does not support custom primitives.");
        }
        auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
        // Create ray tracing program.
        {
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderFile);
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

            mTracer.pBindingTable = RtBindingTable::create(1,1, mpScene->getGeometryCount());
            auto& sbt = mTracer.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
            sbt->setMiss(0, desc.addMiss("shadowMiss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
                );
            }

            mTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }

        {
            // Create SM gen program
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderSMGeneration);
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

            mGenerateSM.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mGenerateSM.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));

            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));

            mGenerateSM.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }

        {
            // Create SM gen program
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderReverseSMGen);
            desc.setShaderModel("6_6");
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

            mReverseSM.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mReverseSM.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
            sbt->setMiss(0, desc.addMiss("miss"));

            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));

            mReverseSM.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }

        {
            // Create SM gen program
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderSparseSMGen);
            desc.setShaderModel("6_6");
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

            mTraceVirtualLayers.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mTraceVirtualLayers.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
            sbt->setMiss(0, desc.addMiss("miss"));

            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));

            mTraceVirtualLayers.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        }

        // Check if the scene has more than 1 light
        if (mpScene->getLightCount() <= 1)
            mPathLightSampleMode = SMLightSampleMode::Uniform;
    }
}

void TestShadowMap::LightMVP::calculate(ref<Light> light, float2 nearFar)
{
    auto& data = light->getData();
    float3 lightTarget = data.posW + data.dirW;
    const float3 up = abs(data.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    view = math::matrixFromLookAt(data.posW, lightTarget, up);
    projection = math::perspective(data.openingAngle * 2, 1.f, nearFar.x, nearFar.y);
    viewProjection = math::mul(projection, view);
    invViewProjection = math::inverse(viewProjection);
}

void TestShadowMap::calculateShadowMapNearFar(RenderContext* pRenderContext, const RenderData& renderData, ShaderVar& var)
{
    FALCOR_PROFILE(pRenderContext, "OptimizeNearFar");
    // Get Analytic light data
    auto lights = mpScene->getLights();

    for (uint i = 0; i < lights.size(); i++)
    {
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = lights[i]->getData().posW;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;
        var["CB"]["gCalcNearFar"] = true;

        var["gRayShadowMapMinMax"] = mpShadowMapMinMaxOpti;

        const uint2 targetDim = uint2(mShadowMapSize);
        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenerateSM.pProgram.get(), mGenerateSM.pVars, uint3(targetDim, 1));

        // mpShadowMapMinMaxOpti->generateMips(pRenderContext, true,-1,true);
        generateMinMaxMips(pRenderContext, mpShadowMapMinMaxOpti);

        // CPU read back
        uint subresourceIndex = mpShadowMapMinMaxOpti->getSubresourceIndex(0, mpShadowMapMinMaxOpti->getMipCount() - 2);
        std::vector<uint8_t> texData = pRenderContext->readTextureSubresource(mpShadowMapMinMaxOpti.get(), subresourceIndex);
        FALCOR_ASSERT(texData.size() == 32); // There should be 4 values
        float2* minMaxData = reinterpret_cast<float2*>(texData.data());
        float2 minMax = float2(FLT_MAX, 0.f);
        for (uint j = 0; j < texData.size() / 8; j++)
        {
            minMax.x = std::min(minMaxData[j].x, minMax.x);
            minMax.y = std::max(minMaxData[j].y, minMax.y);
        }
        float constOffset = 0.f; //(minMax.y - minMax.x) * 0.01f;
        mNearFarPerLight[i] = float2(minMax.x - constOffset, minMax.y + constOffset);

        mShadowMapMVP[i].calculate(lights[i], mNearFarPerLight[i]);
    }
}

void TestShadowMap::generateMinMaxMips(RenderContext* pRenderContext, ref<Texture> pTexture)
{
    FALCOR_PROFILE(pRenderContext, "GenMinMaxMips");
    // Create Pass
    if (!mpGenMinMaxMipsPass)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderMinMaxMips).csEntry("main").setShaderModel("6_5");

        DefineList defines;

        mpGenMinMaxMipsPass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpGenMinMaxMipsPass);

    // Loop through the mip levels
    uint mipLevels = pTexture->getMipCount();
    uint2 texSize = uint2(pTexture->getWidth(), pTexture->getHeight());

    for (uint i = 0; i < mipLevels - 1; i++)
    {
        auto uavSrc = pTexture->getUAV(i, 0, 1);
        auto uavDst = pTexture->getUAV(i + 1, 0, 1);

        auto var = mpGenMinMaxMipsPass->getRootVar();
        uint2 texSizeDst = texSize / 2u;

        var["gMinMaxSrc"].setSrv(pTexture->getSRV(i, 1, 0, 1));
        var["gMinMaxDst"].setUav(pTexture->getUAV(i + 1, 0, 1));

        mpGenMinMaxMipsPass->execute(pRenderContext, uint3(texSizeDst, 1));

        // Reduce TexSize for the next pass
        texSize = texSizeDst;
    }
}

void TestShadowMap::generateShadowMap(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "Generate Shadow Map");

    // Get Analytic light data
    auto lights = mpScene->getLights();
    // Nothing to do if there are no lights
    if (lights.size() == 0)
        return;

    // Create MVP matrices
    bool rebuildAll = false;
    if (mShadowMapMVP.size() != lights.size())
    {
        mShadowMapMVP.resize(lights.size());
        mNearFarPerLight.resize(lights.size());
        rebuildAll = true;
    }
    for (uint i = 0; i < lights.size(); i++)
    {
        auto changes = lights[i]->getChanges();
        bool rebuild = is_set(changes, Light::Changes::Position) || is_set(changes, Light::Changes::Direction) ||
                       is_set(changes, Light::Changes::SurfaceArea);
        rebuild |= mNearFarChanged;
        rebuild |= rebuildAll;
        if (rebuild)
        {
            mShadowMapMVP[i].calculate(lights[i], mNearFar);
        }
        mRerenderSM |= rebuild;
    }

    mRerenderSM |= mAlwaysRenderSM;
    // If there are no changes compaired to last frame, rerendering is not necessary.
    if (!mRerenderSM)
        return;

    mGenerateSM.pProgram->addDefine("SM_NUM_LIGHTS", std::to_string(lights.size()));
    mGenerateSM.pProgram->addDefines(filterSMModesDefines());

    if (!mGenerateSM.pVars)
    {
        // Configure program.
        mGenerateSM.pProgram->setTypeConformances(mpScene->getTypeConformances());
        // Create program variables for the current program.
        // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
        mGenerateSM.pVars = RtProgramVars::create(mpDevice, mGenerateSM.pProgram, mGenerateSM.pBindingTable);
    }

    // Bind utility classes into shared data.
    auto var = mGenerateSM.pVars->getRootVar();

    if (mUseOptimizedNearFarForShadowMap)
        calculateShadowMapNearFar(pRenderContext, renderData, var);

    const uint2 targetDim = uint2(mShadowMapSize);
    // Generate one shadow map per light
    for (uint i = 0; i < lights.size(); i++)
    {
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = lights[i]->getData().posW;
        var["CB"]["gNear"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[i].x : mNearFar.x;
        var["CB"]["gFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[i].y : mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;
        var["CB"]["gCalcNearFar"] = false;

        var["gRayShadowMap"] = mpRayShadowMaps[i];

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenerateSM.pProgram.get(), mGenerateSM.pVars, uint3(targetDim, 1));

        if (mEnableBlur && mFilterSMMode != FilterSMMode::LayeredVariance)
        {
            mpGaussianBlur->execute(pRenderContext, mpRayShadowMaps[i]);
        }
    }

    mRebuildSMBuffers = false;
    mRerenderSM = false;
}

void TestShadowMap::computeRayNeededMask(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "RayNeededMask");
    // Create Pass
    if (!mpComputeRayNeededMask)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderRayNeeded).csEntry("main").setShaderModel("6_5");

        DefineList defines;
        defines.add(filterSMModesDefines());

        mpComputeRayNeededMask = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpComputeRayNeededMask);
    mpComputeRayNeededMask->getProgram()->addDefines(filterSMModesDefines());

    auto var = mpComputeRayNeededMask->getRootVar();
    var["CB"]["gSMSize"] = mShadowMapSize;

    var["gShadowMap"] = mpRayShadowMaps[0];
    var["gRayNeededMask"] = mpRayShadowNeededMask;

    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;

    uint2 dispatchSize = uint2(mShadowMapSize);

    mpComputeRayNeededMask->execute(pRenderContext, uint3(dispatchSize, 1));
}

void TestShadowMap::genReverseSM(RenderContext* pRenderContext,const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ReverserSM");

    pRenderContext->clearUAV(mpSpecialModeSMTex ->getUAV().get(), float4(0));

    computeRayNeededMask(pRenderContext, renderData);

    mReverseSM.pProgram->addDefines(filterSMModesDefines());
    if (!mReverseSM.pVars)
    {
        // Configure program.
        mReverseSM.pProgram->setTypeConformances(mpScene->getTypeConformances());
        // Create program variables for the current program.
        // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
        mReverseSM.pVars = RtProgramVars::create(mpDevice, mReverseSM.pProgram, mReverseSM.pBindingTable);
        //pRenderContext->clearUAV(mpSpecialModeSMTex ->getUAV().get(), float4(1, 1, 1, 1));
    }

    // Bind utility classes into shared data.
    auto var = mReverseSM.pVars->getRootVar();
    // Get Analytic light data
    auto lights = mpScene->getLights();
    const uint2 targetDim = renderData.getDefaultTextureDims();

    var["CB"]["gLightPos"] = lights[0]->getData().posW;
    var["CB"]["gNear"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0].x : mNearFar.x;
    var["CB"]["gFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0].y : mNearFar.y;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;
    var["CB"]["gInvViewProj"] = mShadowMapMVP[0].invViewProjection;
    var["CB"]["gSMSize"] = mShadowMapSize;

    var["gShadowMap"] = mpRayShadowMaps[0];
    var["gUseRayMask"] = mpRayShadowNeededMask;
    var["gReverseSM"] = mpSpecialModeSMTex ;

    // Bind Samplers
    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;
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

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mReverseSM.pProgram.get(), mReverseSM.pVars, uint3(targetDim, 1));

}

void TestShadowMap::traceScene(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "Path Tracer");

    auto& dict = renderData.getDictionary();
    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracer.pProgram->addDefine("COUNT_SM", std::to_string(mpRayShadowMaps.size()));
    mTracer.pProgram->addDefine("PATHSM_LIGHT_SAMPLE_MODE", std::to_string((uint32_t)mPathLightSampleMode));
    mTracer.pProgram->addDefine("USE_SHADOW_RAY", mShadowMode != ShadowMode::ShadowMap ? "1" : "0");
    mTracer.pProgram->addDefine("USE_DEBUG", mEnableDebug ? "1" : "0");
    mTracer.pProgram->addDefine("WRITE_TO_DEBUG", mEnableDebug && !is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) ? "1" : "0");
    mTracer.pProgram->addDefine("DEBUG_MODE", std::to_string((uint32_t)mDebugMode));
    mTracer.pProgram->addDefine("DEBUG_ACCUMULATE", mAccumulateDebug ? "1" : "0");
    mTracer.pProgram->addDefine("SM_GENERATION_RAYTRACING", std::to_string(mSMGenerationUseRay));
    mTracer.pProgram->addDefine("DISTRIBUTE_RAY_OUTSIDE_SM", mDistributeRayOutsideOfSM ? "1" : "0");
    mTracer.pProgram->addDefine("CHECK_FOR_NAN", mCheckForNaN ? "1" : "0");
    mTracer.pProgram->addDefine("USE_REVERSE_SM", mUseReverseSM ? "1" : "0");
    mTracer.pProgram->addDefines(filterSMModesDefines());

    if (mpRasterShadowMap)
        mTracer.pProgram->addDefines(mpRasterShadowMap->getDefines());

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mTracer.pVars)
        prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    auto var = mTracer.pVars->getRootVar();
    // Set constants.
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gUseShadowMap"] = mShadowMode != ShadowMode::RayShadows;
    var["CB"]["gRayShadowMapRes"] = mShadowMapSize;
    var["CB"]["gIterationCount"] = mIterationCount;
    var["CB"]["gDebugFactor"] = mDebugMult;
    var["CB"]["gUseRayForDirect"] = !mUseSMForDirect;

    // Bind Shadow MVPS and Shadow Map
    FALCOR_ASSERT(mpRayShadowMaps.size() == mShadowMapMVP.size() || mpRayShadowMapsMinMax.size() == mShadowMapMVP.size());
    for (uint i = 0; i < mShadowMapMVP.size(); i++)
    {
        var["ShadowNearFar"]["gSMNearFar"][i] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[i] : mNearFar;
        var["ShadowVPs"]["gRayShadowMapVP"][i] = mShadowMapMVP[i].viewProjection;
        var["gRayShadowMap"][i] = mpRayShadowMaps[i];

        bool validAccessTex =
            (mpShadowMapAccessTex.size() == mpRayShadowMaps.size()) || (mpShadowMapAccessTex.size() == mpRayShadowMapsMinMax.size());
        validAccessTex &= !mpShadowMapAccessTex.empty();
        if (validAccessTex)
            var["gShadowAccessDebugTex"][i] = mpShadowMapAccessTex[i];
    }
    var["gReverseShadowMap"] = mpSpecialModeSMTex ;
    var["gUseRayMask"] = mpRayShadowNeededMask;

    // Bind Samplers
    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;
    

    // RasterSM
    if (mpRasterShadowMap)
    {
        mpRasterShadowMap->setShaderDataAndBindBlock(var, renderData.getDefaultTextureDims());
    }

    // Pixel Stats
    mpPixelStats->prepareProgram(mTracer.pProgram, var);

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

static ResourceFormat getSMFormat(TestShadowMap::FilterSMMode filterMode) {
    switch (filterMode)
    {
    case TestShadowMap::FilterSMMode::Variance:
    case TestShadowMap::FilterSMMode::VirtualLayeredVariance:
        return ResourceFormat::RG32Float;
        break;
    case TestShadowMap::FilterSMMode::ESVM:
        return ResourceFormat::RGBA32Float;
        break;
    case TestShadowMap::FilterSMMode::MSM:
        return ResourceFormat::RGBA32Float;
        break;
    case TestShadowMap::FilterSMMode::LayeredVariance:
        return ResourceFormat::R32Float;
        break;
    default:
        FALCOR_UNREACHABLE();
        break;
    }
    return ResourceFormat::Unknown;
}

void TestShadowMap::prepareBuffers()
{
    // Shadow Map
    if (mRebuildSMBuffers)
    {
        mpRayShadowMaps.clear();
        mpRayShadowMapsMinMax.clear();
        mShadowMapMVP.clear();
        mpShadowMapBlit.reset();
        mpShadowMapAccessTex.clear();
        mpShadowMapMinMaxOpti.reset();

        //Temporary TODO implement properly
        mpSpecialModeSMTex .reset();
        mpRayShadowNeededMask.reset();

        mRerenderSM = true;
    }

    //TEMP test texture TODO
    if (!mpSpecialModeSMTex )
    {
        mpSpecialModeSMTex  = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::R32Float, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        mpSpecialModeSMTex ->setName("TestShadowMap::SpecialModeShadowMap");
    }
    // TEMP test texture TODO
    if (!mpRayShadowNeededMask)
    {
        mpRayShadowNeededMask = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::R8Unorm, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        mpRayShadowNeededMask->setName("TestShadowMap::RayNeededMask");
    }


    // Get Analytic light data
    auto lights = mpScene->getLights();
    uint numShadowMaps = mpRayShadowMaps.size();

    // Check if the shadow map is up to date
    if ((numShadowMaps != lights.size() && lights.size() > 0))
    {
        mpRayShadowMaps.clear();
        // Create a shadow map per light
        for (uint i = 0; i < lights.size(); i++)
        {
            ResourceFormat format = getSMFormat(mFilterSMMode);
            FALCOR_ASSERT(format != ResourceFormat::Unknown);
            ref<Texture> tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, format, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
            tex->setName("TestShadowMap::ShadowMap" + std::to_string(i));
            mpRayShadowMaps.push_back(tex);
        }
    }

    if (!mpRayShadowMapsMinMax.empty())
        mpRayShadowMapsMinMax.clear();
   
    // Update
    numShadowMaps = mpRayShadowMaps.size();

    // MinMax opti buffer
    if (!mpShadowMapMinMaxOpti && mUseOptimizedNearFarForShadowMap)
    {
        mpShadowMapMinMaxOpti = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1u, Texture::kMaxPossible, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget
        );
        mpShadowMapMinMaxOpti->setName("ShadowMap::MinMaxOpti");
    }
    else if (mpShadowMapMinMaxOpti && !mUseOptimizedNearFarForShadowMap)
        mpShadowMapMinMaxOpti.reset();

    // Debug Textures

    // Blit Tex
    if (mEnableDebug && is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) && numShadowMaps > 0)
    {
        // Reset if size is not right
        if (mpShadowMapBlit)
            if (mpShadowMapBlit->getWidth() != mShadowMapSize)
                mpShadowMapBlit.reset();
        if (!mpShadowMapBlit)
        {
            mpShadowMapBlit = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mpShadowMapBlit->setName("TestShadowMap::ShadowMapBlitTex");
        }
    }
    else if (mpShadowMapBlit)
    {
        mpShadowMapBlit.reset();
    }

    // SM accumulate tex
    if (mEnableDebug && (mDebugMode == PathSMDebugModes::ShadowMapAccess || mDebugMode == PathSMDebugModes::ShadowMapRayDiff) &&
        numShadowMaps > 0)
    {
        // Check if size and shadow map size is still right
        if (!mpShadowMapAccessTex.empty())
        {
            if ((mpShadowMapAccessTex.size() != numShadowMaps) || (mpShadowMapAccessTex[0]->getWidth() != mShadowMapSize))
            {
                mpShadowMapAccessTex.clear();
            }
        }

        // Create
        if (mpShadowMapAccessTex.empty())
        {
            mpShadowMapAccessTex.resize(numShadowMaps);
            for (uint i = 0; i < numShadowMaps; i++)
            {
                mpShadowMapAccessTex[i] = Texture::create2D(
                    mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::R32Uint, 1u, 1u, nullptr,
                    ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
                );
                mpShadowMapAccessTex[i]->setName("PathSM::ShadowMapAccessTex" + std::to_string(i));
            }
            mClearDebugAccessTex = true;
        }
    }
    else if (!mpShadowMapAccessTex.empty())
    {
        mpShadowMapAccessTex.clear();
    }
}

void TestShadowMap::prepareVars()
{
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

DefineList TestShadowMap::filterSMModesDefines()
{
    DefineList defines;
    defines.add("FILTER_SM_VARIANCE", mFilterSMMode == FilterSMMode::Variance ? "1" : "0");
    defines.add("FILTER_SM_ESVM", mFilterSMMode == FilterSMMode::ESVM ? "1" : "0");
    defines.add("FILTER_SM_MSM", mFilterSMMode == FilterSMMode::MSM ? "1" : "0");
    defines.add("FILTER_SM_LAYERED_VARIANCE", mFilterSMMode == FilterSMMode::LayeredVariance ? "1" : "0");
    defines.add("FILTER_SM_VIRTUAL_LAYERED_VARIANCE", mFilterSMMode == FilterSMMode::VirtualLayeredVariance ? "1" : "0");
    //Format
    std::string sFormat;
    switch (mFilterSMMode)
    {
    case TestShadowMap::FilterSMMode::Variance:
    case TestShadowMap::FilterSMMode::VirtualLayeredVariance:
        sFormat = "float2";
        break;
    case TestShadowMap::FilterSMMode::ESVM:
    case TestShadowMap::FilterSMMode::MSM:
        sFormat = "float4";
        break;
    case TestShadowMap::FilterSMMode::LayeredVariance:
        sFormat = "float";
        break;
    default:
        FALCOR_UNREACHABLE();
        sFormat = "float";
        break;
    }
    defines.add("SM_FORMAT", sFormat);

    return defines;
}

void TestShadowMap::debugShadowMapPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "DebugShadowMap");

    // Create Pass
    if (!mpDebugShadowMapPass)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderDebugSM).csEntry("main").setShaderModel("6_5");

        DefineList defines;
        defines.add(filterSMModesDefines());

        mpDebugShadowMapPass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpDebugShadowMapPass);

    //Return early if encoutering this pass
    if (mDebugMode == PathSMDebugModes::SparseShadowDepths)
        return;

    // Runtime defines
    mpDebugShadowMapPass->getProgram()->addDefines(filterSMModesDefines());

    // Set variables
    float debugHeatMapMaxCount =
        mDebugMode == PathSMDebugModes::ShadowMapAccess ? mDebugHeatMapMaxCountAccess : mDebugHeatMapMaxCountDifference;

    auto var = mpDebugShadowMapPass->getRootVar();
    var["CB"]["gDebugMode"] = (uint)mDebugMode;
    var["CB"]["gBlendVal"] = mDebugAccessBlendVal;
    var["CB"]["gMaxValue"] = debugHeatMapMaxCount * (mIterationCount + 1);
    var["CB"]["gBrightnessIncrease"] = mDebugBrighnessMod;

    if (!mLayeredVarianceData.pVarianceLayers.empty())
        var["gLayeredShadowMap"] = mLayeredVarianceData.pVarianceLayers[mUISelectedVarianceLayer];
    var["gRayShadowMap"] = mpRayShadowMaps[mUISelectedLight];
    var["gRayReverseSM"] = mpSpecialModeSMTex ;
    var["gRayNeededMask"] = mpRayShadowNeededMask;

    if (mpShadowMapAccessTex.size() > mUISelectedLight)
        var["gShadowAccessTex"] = mpShadowMapAccessTex[mUISelectedLight];
    var["gOut"] = mpShadowMapBlit;

    // Execute
    const uint2 targetDim = uint2(mShadowMapSize);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpDebugShadowMapPass->execute(pRenderContext, uint3(targetDim, 1));

    // Blit the shadow map into the Debug texture
    // Screen size
    uint2 screenSize = renderData.getDefaultTextureDims();
    uint sizeDiff = (screenSize.x - screenSize.y) / 2;
    uint4 outRect = mDebugUseSMAspect ? uint4(sizeDiff, 0, screenSize.x - sizeDiff, screenSize.y) : uint4(0, 0, screenSize.x, screenSize.y);

    ref<Texture> outTex = renderData.getTexture("debug");
    pRenderContext->blit(mpShadowMapBlit->getSRV(), outTex->getRTV(), uint4(0, 0, mShadowMapSize, mShadowMapSize), outRect);
}

void TestShadowMap::layeredVarianceSMPass(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Layered Variance");

    //Handle Buffers
    auto& data = mLayeredVarianceData;
    bool layerChanged = data.pVarianceLayers.size() != data.layers;
    bool sizeChanged = false;
    if (!data.pVarianceLayers.empty())
        sizeChanged |= data.pVarianceLayers[0]->getWidth() != mShadowMapSize;

    if (sizeChanged || layerChanged)
    {
        data.pVarianceLayers.clear();
        data.pVarianceLayers.resize(data.layers);
        for (uint i = 0; i < data.layers; i++)
        {
            data.pVarianceLayers[i] = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
            data.pVarianceLayers[i]->setName("LayeredVariance_Layer" + std::to_string(i));
        }

        //Reset both compute passes
        if (layerChanged)
        {
            data.pLayeredVarianceEvaluatePass.reset();
            data.pLayeredVarianceGeneratePass.reset();
        }
    }

    //Dispatch Shaders
    layeredVarianceSMGenerate(pRenderContext, renderData);
    layeredVarianceSMEvaluate(pRenderContext, renderData);
}

void TestShadowMap::layeredVarianceSMGenerate(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate Layered Variance");
    auto& data = mLayeredVarianceData;
    auto& pComputePass = data.pLayeredVarianceGeneratePass;
    // Create Pass
    if (!pComputePass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderLayeredVariance).csEntry("generate").setShaderModel("6_6");
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(filterSMModesDefines());
        defines.add("LVSM_LAYERS",std::to_string(data.layers));
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());


        pComputePass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(pComputePass);
    pComputePass->getProgram()->addDefines(filterSMModesDefines());
    
    auto var = pComputePass->getRootVar();
    var["CB"]["gSMSize"] = mShadowMapSize;
    var["CB"]["gSMNearFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0] : mNearFar;
    var["CB"]["gOverlap"] = data.overlap;
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;

    var["gShadowMap"] = mpRayShadowMaps[0];
    for (uint i = 0; i < data.layers; i++)
    {
        var["gLayeredVarianceOut"][i] = data.pVarianceLayers[i];
    }

    uint2 dispatchSize = uint2(mShadowMapSize);

    pComputePass->execute(pRenderContext, uint3(dispatchSize, 1));

    if (mEnableBlur)
    {
        for (uint i = 0; i < data.layers; i++)
            mpGaussianBlur->execute(pRenderContext, data.pVarianceLayers[i]);
    }
}
void TestShadowMap::layeredVarianceSMEvaluate(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Evaluate Layered Variance");
    auto& data = mLayeredVarianceData;
    auto& pComputePass = data.pLayeredVarianceEvaluatePass;
    // Create Pass
    if (!pComputePass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderLayeredVariance).csEntry("evaluate").setShaderModel("6_5");
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(filterSMModesDefines());
        defines.add("LVSM_LAYERS", std::to_string(data.layers));
        defines.add("DEBUG_WRITE", mDebugMode == PathSMDebugModes::LayeredVarianceLayers ? "1" : "0");
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());

        pComputePass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(pComputePass);
    pComputePass->getProgram()->addDefines(filterSMModesDefines());
    pComputePass->getProgram()->addDefine("DEBUG_WRITE", mDebugMode == PathSMDebugModes::LayeredVarianceLayers ? "1" : "0");

    auto var = pComputePass->getRootVar();

    mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
    mpSampleGenerator->setShaderData(var);                    // Sample generator

    var["CB"]["gSMSize"] = mShadowMapSize;
    var["CB"]["gSMNearFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0] : mNearFar;
    var["CB"]["gOverlap"] = data.overlap;
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;

    for (uint i = 0; i < data.layers; i++)
    {
        var["gLayeredVariance"][i] = data.pVarianceLayers[i];
    }

    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;

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


    uint2 dispatchSize = renderData.getDefaultTextureDims();

    pComputePass->execute(pRenderContext, uint3(dispatchSize, 1));
}

void TestShadowMap::virtualLayeredVarianceSMPass(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "VirtualLayeredVariance");
    //Handle Buffers
    auto& data = mVirtualLayeredVarianceData;

    //Reset if size changed
    if (data.pVirtualLayers && data.pVirtualLayers->getWidth() != mShadowMapSize)
    {
        data.pVirtualLayers.reset();
        data.pLayersNeededMax.reset();
        data.pLayersNeededMin.reset();
        data.pLayersMinMax.reset();
    }

    uint layerMaskSize = mShadowMapSize / 2;
    if (!data.pLayersNeededMax)
    {
        data.pLayersNeededMax = Texture::create2D(
            mpDevice, layerMaskSize, layerMaskSize, ResourceFormat::R32Uint, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        data.pLayersNeededMax->setName("VirtualLayersNeededMax");
    }
    if (!data.pLayersNeededMin)
    {
        data.pLayersNeededMin = Texture::create2D(
            mpDevice, layerMaskSize, layerMaskSize, ResourceFormat::R32Uint, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        data.pLayersNeededMin->setName("VirtualLayersNeededMin");
    }

    if (!data.pLayersMinMax)
    {
        data.pLayersMinMax = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG8Uint, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        data.pLayersMinMax->setName("VirtualLayersNeededMinMax");
    }

    if (!data.pVirtualLayers)
    {
        data.pVirtualLayers = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        data.pVirtualLayers->setName("VirtualLayerSM");
    }



    //createLayersNeeded(pRenderContext, renderData);
    //traceVirtualLayers(pRenderContext, renderData);
    virtualLayersBlur(pRenderContext, renderData);
    evaluateVirtualLayers(pRenderContext, renderData);
}

void TestShadowMap::createLayersNeeded(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Virtual Layers Mask");

    auto& data = mVirtualLayeredVarianceData;
    auto& pComputePass = data.pCreateLayersNeededPass;
    //Clear the Layers Texture
    pRenderContext->clearUAV(data.pLayersNeededMin->getUAV(0u, 0u,1u).get(), uint4(255u));
    pRenderContext->clearUAV(data.pLayersNeededMax->getUAV(0u, 0u, 1u).get(), uint4(0u));

    // Create Pass
    if (!pComputePass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderVLVSMCreateLayers).csEntry("main").setShaderModel("6_5");
        desc.addTypeConformances(mpScene->getTypeConformances());
        
        DefineList defines;
        defines.add("LVSM_LAYERS", std::to_string(data.layers));
        defines.add(mpScene->getSceneDefines());

        pComputePass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(pComputePass);
    pComputePass->getProgram()->addDefines(filterSMModesDefines());
    pComputePass->getProgram()->addDefine("LVSM_LAYERS", std::to_string(data.layers));

    auto var = pComputePass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data

    var["CB"]["gSMSize"] = mShadowMapSize / 2;
    var["CB"]["gSMNearFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0] : mNearFar;
    var["CB"]["gOverlap"] = data.overlap;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;

    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gOutLayersNeededMin"] = data.pLayersNeededMin;
    var["gOutLayersNeededMax"] = data.pLayersNeededMax;

    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;

    uint2 dispatchSize = renderData.getDefaultTextureDims();

    pComputePass->execute(pRenderContext, uint3(dispatchSize, 1));
}

void TestShadowMap::traceVirtualLayers(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "TraceVirtualLayers");
    auto& data = mVirtualLayeredVarianceData;
    // computeRayNeededMask(pRenderContext, renderData);

    mTraceVirtualLayers.pProgram->addDefines(filterSMModesDefines());
    mTraceVirtualLayers.pProgram->addDefine("LVSM_LAYERS", std::to_string(data.layers));
    if (!mTraceVirtualLayers.pVars)
    {
        // Configure program.
        mTraceVirtualLayers.pProgram->setTypeConformances(mpScene->getTypeConformances());
        // Create program variables for the current program.
        // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
        mTraceVirtualLayers.pVars = RtProgramVars::create(mpDevice, mTraceVirtualLayers.pProgram, mTraceVirtualLayers.pBindingTable);
    }

    // Bind utility classes into shared data.
    auto var = mTraceVirtualLayers.pVars->getRootVar();
    // Get Analytic light data
    auto lights = mpScene->getLights();
    const uint2 targetDim = uint2(mShadowMapSize);

    var["CB"]["gLightPos"] = lights[0]->getData().posW;
    var["CB"]["gNear"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0].x : mNearFar.x;
    var["CB"]["gFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0].y : mNearFar.y;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;
    var["CB"]["gInvViewProj"] = mShadowMapMVP[0].invViewProjection;
    var["CB"]["gSMSize"] = mShadowMapSize;
    var["CB"]["gOverlap"] = data.overlap;

    var["gLayersNeededMin"] = data.pLayersNeededMin;
    var["gLayersNeededMax"] = data.pLayersNeededMax;
    var["gVirtualLayers"] = data.pVirtualLayers;
    var["gShadowMap"] = mpRayShadowMaps[0];

    // Bind Samplers
    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTraceVirtualLayers.pProgram.get(), mTraceVirtualLayers.pVars, uint3(targetDim, 1));
}

void TestShadowMap::evaluateVirtualLayers(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Evaluate Virtual Layers");
    auto& data = mVirtualLayeredVarianceData;
    auto& pComputePass = data.pEvaluateVirtualLayers;

    // Create Pass
    if (!pComputePass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderEvaluateVirtualLayers).csEntry("main").setShaderModel("6_5");
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add("LVSM_LAYERS", std::to_string(data.layers));
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());

        pComputePass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(pComputePass);
    pComputePass->getProgram()->addDefines(filterSMModesDefines());
    pComputePass->getProgram()->addDefine("LVSM_LAYERS", std::to_string(data.layers));

    auto var = pComputePass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
    mpSampleGenerator->setShaderData(var);

    var["CB"]["gSMSize"] = mShadowMapSize;
    var["CB"]["gSMNearFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[0] : mNearFar;
    var["CB"]["gOverlap"] = data.overlap;
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gViewProj"] = mShadowMapMVP[0].viewProjection;

    var["gOutLayersNeededMin"] = data.pLayersNeededMin;
    var["gOutLayersNeededMax"] = data.pLayersNeededMax;
    var["gLayersMinMax"] = data.pLayersMinMax;

    var["gShadowMap"] = mpRayShadowMaps[0];
    var["gVirtualLayers"] = data.pVirtualLayers;

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

    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;

    uint2 dispatchSize = renderData.getDefaultTextureDims();

    pComputePass->execute(pRenderContext, uint3(dispatchSize, 1));

}

float getCoefficient(float sigma, float kernelWidth, float x)
{
    float sigmaSquared = sigma * sigma;
    float p = -(x * x) / (2 * sigmaSquared);
    float e = std::exp(p);

    float a = 2 * (float)M_PI * sigmaSquared;
    return e / a;
}

void TestShadowMap::virtualLayersBlur(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "LayersAndBlur");
    auto& data = mVirtualLayeredVarianceData;
    auto& pComputePasses = data.pBlurAndVirtualLayers;

    //Create and fill blur buffer
    if (data.resetBlur)
    {
        uint center = data.blurRadius / 2;
        float sum = 0;
        std::vector<float> weights(center + 1);
        for (uint i = 0; i <= center; i++)
        {
            weights[i] = data.useBoxFilter ? 1.0f / data.blurRadius : getCoefficient(data.sigma, (float)data.blurRadius, (float)i);
            sum += (i == 0) ? weights[i] : 2 * weights[i];
        }
        std::vector<float> weightsCPU(data.blurRadius);
        for (uint i = 0; i <= center; i++)
        {
            float w = weights[i] / sum;
            weightsCPU[center + i] = w;
            weightsCPU[center - i] = w;
        }

        if (data.pBlurWeights)
            data.pBlurWeights.reset();

        data.pBlurWeights = Buffer::createStructured(
            mpDevice, sizeof(float), data.blurRadius, ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, weightsCPU.data(), false
        );
        data.pBlurWeights->setName("VirtualLayersBlurWeights");


        data.resetBlur = false;
    }

    // Create Pass
    if (!pComputePasses[0])
    {
        for (uint i = 0; i < 3; i++)
        {
            Program::Desc desc;
            //desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderVLVSMBlur).csEntry("main").setShaderModel("6_5");

            DefineList defines;
            defines.add("LVSM_LAYERS", std::to_string(data.layers));
            defines.add("_KERNEL_WIDTH", std::to_string(data.blurRadius));
            defines.add("_TEX_SIZE", std::to_string(mShadowMapSize));
            if ((i%2) == 0)
                defines.add("_HORIZONTAL_DIR");
            else
                defines.add("_VERTICAL_DIR");

            if (i < 2)
                defines.add("_MINMAX_PASS");
            else
                defines.add("_BLUR_PASS");

            pComputePasses[i] = ComputePass::create(mpDevice, desc, defines, true);
        }
        
    }

    FALCOR_ASSERT(pComputePasses[0] && pComputePasses[1]);

    //Do both blur passes
    for (uint i = 0; i < 3; i++)
    {
        std::string profileString = (i < 2 ? "LayerMinMax" : "Blur") + std::to_string(i % 2);
        FALCOR_PROFILE(pRenderContext, profileString);
        //Update defines
        pComputePasses[i]->getProgram()->addDefines(filterSMModesDefines());
        pComputePasses[i]->getProgram()->addDefine("LVSM_LAYERS", std::to_string(data.layers));
        pComputePasses[i]->getProgram()->addDefine("_KERNEL_WIDTH", std::to_string(data.blurRadius));
        pComputePasses[i]->getProgram()->addDefine("_TEX_SIZE", std::to_string(mShadowMapSize));

        auto var = pComputePasses[i]->getRootVar();

        //Ping-Pong Tex (Reuse virtual layers as tmp tex)
        var["gSrcTex"] = ((i % 2) == 0) ? mpRayShadowMaps[0] : data.pVirtualLayers; 
        var["gDstTex"] = ((i % 2) == 0) ? data.pVirtualLayers : mpRayShadowMaps[0];

        var["gWeights"] = data.pBlurWeights;
        if (i < 2)
            var["gLayersMinMaxOut"] = data.pLayersMinMax;
        else
            var["gLayersMinMax"] = data.pLayersMinMax;

        uint2 dispatchSize = uint2(mShadowMapSize);

        pComputePasses[i]->execute(pRenderContext, uint3(dispatchSize, 1));
    }    
}
