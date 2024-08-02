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
#include "TestPathSM.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TestPathSM>();
}

namespace
{
const char kShaderFile[] = "RenderPasses/TestPathSM/TestPathSM.rt.slang";
const char kShaderSMGeneration[] = "RenderPasses/TestPathSM/GenerateShadowMap.rt.slang";
const char kShaderDebugSM[] = "RenderPasses/TestPathSM/DebugShadowMap.cs.slang";

// Ray tracing settings that affect the traversal stack size.
// These should be set as small as possible.
const uint32_t kMaxPayloadSizeBytes = 16u;
const uint32_t kMaxRecursionDepth = 1u;

const char kInputViewDir[] = "viewW";

const ChannelList kInputChannels = {
    {"vbuffer", "gVBuffer", "Visibility buffer in packed format"},
    {kInputViewDir, "gViewW", "World-space view direction (xyz float format)", true /* optional */},
};

const ChannelList kOutputChannels = {
    {"color", "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float},
    {"debug", "gDebug", "Debug", false, ResourceFormat::RGBA32Float},
};

//Properties Parsing
const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";
const char kShadowTracingMode[] = "shadowTracingMode";

const Gui::DropdownList kBlockSizes{
    {4, "4x4"},
    {8, "8x8"},
    {16, "16x16"},
    {32, "32x32"},
    {64, "64x64"},
};

const Gui::DropdownList kShadowMapSizes{
    {256, "256x256"},
    {512, "512x512"},
    {1024, "1024x1024"},
    {2048, "2048x2048"},
    {4096, "4096x4096"},
};

const Gui::DropdownList kSMGenerationRenderer{
    {0, "Rasterizer"},
    {1, "RayTracing"},
};

} // namespace

TestPathSM::TestPathSM(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);

    //Create Pixel Stats
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
    mpShadowMapOracle = std::make_unique<ShadowMapOracle>(false);
}

void TestPathSM::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces)
            mMaxBounces = value;
        else if (key == kComputeDirect)
            mComputeDirect = value;
        else if (key == kUseImportanceSampling)
            mUseImportanceSampling = value;
        else if (key == kShadowTracingMode)
            mShadowMode = value;
        else
            logWarning("Unknown property '{}' in TestPathSM properties.", key);
    }
}

Properties TestPathSM::getProperties() const
{
    Properties props;
    props[kMaxBounces] = mMaxBounces;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;
    props[kShadowTracingMode] = mShadowMode;
    return props;
}

RenderPassReflection TestPathSM::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget);

    return reflector;
}

void TestPathSM::execute(RenderContext* pRenderContext, const RenderData& renderData)
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
        throw RuntimeError("TestPathSM: This render pass does not support scene geometry changes.");
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

    //Handle Buffers
    prepareBuffers();

    //Debug Accumulate
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

    //Debug Access Tex
    if (mClearDebugAccessTex)
    {
        if (!mpShadowMapAccessTex.empty())
        {
            for (uint i = 0; i < mpShadowMapAccessTex.size(); i++)
                pRenderContext->clearUAV(mpShadowMapAccessTex[i]->getUAV(0,0,1u).get(), uint4(0));
        }
        mClearDebugAccessTex = false;
    }

    //Pixel Stats
    mpPixelStats->beginFrame(pRenderContext, renderData.getDefaultTextureDims());

    //Shadow Map Generation
    if (mSMGenerationUseRay) //Ray
    {
        generateShadowMap(pRenderContext, renderData);

        if (mpRasterShadowMap)
        {
            mpRasterShadowMap.reset();
            mTracer.pVars.reset();
        }
            
    }
    else //Raster
    {
        if (!mpRasterShadowMap)
        {
            mpRasterShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);
            mTracer.pVars.reset();
        }
        mpRasterShadowMap->update(pRenderContext);
    }
   

    //Oracle
    if (mpShadowMapOracle->isEnabled())
    {
        if (mSMGenerationUseRay)
        {
            std::vector<float2> empty;
            if (mpShadowMapOracle->update(
                    mpScene, renderData.getDefaultTextureDims(), mShadowMapSize, mShadowMapSize, mShadowMapSize, 0u, empty
                ))
                mTracer.pVars.reset();
        }
        else //Raster
        {
            if(mpShadowMapOracle->update(mpScene, renderData.getDefaultTextureDims(), mpRasterShadowMap.get()))
                mTracer.pVars.reset();
        }
        
    }
        
    traceScene(pRenderContext, renderData);

    //Debug pass
    if (is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag))
        debugShadowMapPass(pRenderContext, renderData);


    mpPixelStats->endFrame(pRenderContext);
    mFrameCount++;
    mIterationCount++;
}

void TestPathSM::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, (1u << 8)-1);
    widget.tooltip("Maximum number of surface bounces (diffuse + specular + transmission)", true);

    dirty |= widget.var("Max diffuse bounces", mMaxDiffuseBounces, 0u, (1u << 8) - 1);
    widget.tooltip("Maximum number of diffuse bounces.\n0 = direct only\n1 = one indirect bounce etc.");

    dirty |= widget.var("Max specular bounces", mMaxSpecularBounces, 0u, (1u << 8) - 1);
    widget.tooltip("Maximum number of specular bounces.\n0 = direct only\n1 = one indirect bounce etc.");

    dirty |= widget.var("Max transmissive bounces", mMaxTransmissiveBounces, 0u, (1u << 8) - 1);
    widget.tooltip("Maximum number of tranmissive bounces.\n0 = direct only\n1 = one indirect bounce etc.");

    if (widget.button("Set all to Max Bounce"))
    {
        mMaxDiffuseBounces = mMaxBounces;
        mMaxSpecularBounces = mMaxBounces;
        mMaxTransmissiveBounces = mMaxBounces;
    }

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.checkbox("Use Russian Roulette", mUseRussianRoulette);
    widget.tooltip("Enables Russian Roulette to end path with low throuput prematurely", true);

    dirty |= widget.dropdown("Analytic Light Sample Mode", mPathLightSampleMode);
    widget.tooltip("Select the mode for sampling the analytic lights");

    dirty |= widget.checkbox("Use Seperate Light Sampler", mUseSeperateLightSampler);
    widget.tooltip("Seperate Light sampler that allows block sampling", true);

    if (mUseSeperateLightSampler)
    {
        dirty |= widget.dropdown("Light Sampler Block Sizes", kBlockSizes, mSeperateLightSamplerBlockSize);
    }

    dirty |= widget.dropdown("Shadow Render Mode", mShadowMode);
    widget.dropdown("SM Renderer", kSMGenerationRenderer, mSMGenerationUseRay);

    if (auto group = widget.group("ShadowMap Options"))
    {
        //Ray SM Options
        if (mSMGenerationUseRay)
        {
            mRebuildSMBuffers |= group.dropdown("Shadow Map Size", kShadowMapSizes, mShadowMapSize);
            dirty |= group.dropdown("Filter SM Mode", mFilterSMMode);
            group.tooltip("Filtered shadow map is always recreated from one depth");

            mRerenderSM |= group.checkbox("Use Min/Max SM", mUseMinMaxShadowMap);
            dirty |= group.checkbox("Always Render Shadow Map", mAlwaysRenderSM);

            dirty |= group.checkbox("Use SM for direct light", mUseSMForDirect);

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
                group.var("SM Near/Far", mNearFar, 0.f, FLT_MAX, 0.001f);
                group.tooltip("Sets the near and far for the shadow map");
            }
            group.var("LtBounds Start", mLtBoundsStart, 0.0f, 0.5f, 0.001f, false, "%.5f");
            group.var("LtBounds Max Reduction", mLtBoundsMaxReduction, 0.0f, 0.5f, 0.001f, false, "%.5f");

            group.checkbox("Use Ray outside of SM", mDistributeRayOutsideOfSM);

            if (mUseMinMaxShadowMap)
            {
                group.var("Shadow Map Samples", mShadowMapSamples, 1u, 4096u, 1u);
                group.tooltip("Sets the shadow map samples. Manual reset is necessary for it to take effect");
            }
            mRerenderSM |= group.button("Reset Shadow Map");

            dirty |= mRebuildSMBuffers || mRerenderSM;
        }
        else if (mpRasterShadowMap) //Raster SM options
        {
            dirty |= mpRasterShadowMap->renderUILeakTracing(group, mShadowMode == ShadowMode::LeakTracing);
        }
    }

    if (auto group = widget.group("Oracle"))
    {
        dirty |= mpShadowMapOracle->renderUI(group);
    }

    if (auto group = widget.group("Debug"))
    {
        mResetDebugAccumulate |= group.checkbox("Enable", mEnableDebug);
        if (mEnableDebug)
        {
            mResetDebugAccumulate |= group.dropdown("Debug Mode", mDebugMode);

            if (mDebugMode != PathSMDebugModes::ShadowMap)
            {
                mResetDebugAccumulate |= group.checkbox("Accumulate Debug", mAccumulateDebug);
                mResetDebugAccumulate |= group.button("Reset Accumulate");
            }

            if (is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) && mpScene)
            {
                group.slider("Select Light", mUISelectedLight, 0u, mpScene->getLightCount() - 1);
            }
            if (mDebugMode == PathSMDebugModes::ShadowMapAccess || mDebugMode == PathSMDebugModes::ShadowMapRayDiff)
            {
                group.var("Blend with SM", mDebugAccessBlendVal, 0.f, 1.f, 0.001f);
                group.var("HeatMap/SM Brightness", mDebugBrighnessMod, 0.f, FLT_MAX, 0.001f);
                group.var(
                    "HeatMap max val", mDebugMode == PathSMDebugModes::ShadowMapAccess ? mDebugHeatMapMaxCountAccess : mDebugHeatMapMaxCountDifference, 0.f, FLT_MAX,
                    mDebugMode == PathSMDebugModes::ShadowMapAccess ? 0.1f : 0.001f
                );
                group.checkbox("Use correct aspect", mDebugUseSMAspect);
            }
            if (mDebugMode == PathSMDebugModes::PathLengthValidLight || mDebugMode == PathSMDebugModes::LeakTracingMaskPerBounce)
                mResetDebugAccumulate |= group.slider("Shown Bounce", mDebugShowBounce, 0u, mMaxBounces);
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

void TestPathSM::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
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
            logWarning("TestPathSM: This render pass does not support custom primitives.");
        }

        // Create ray tracing program.
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
            sbt->setMiss(0, desc.addMiss("scatterMiss"));
            sbt->setMiss(1, desc.addMiss("shadowMiss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
                    desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
                );
                sbt->setHitGroup(
                    1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
                );
            }

            if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
                    desc.addHitGroup("scatterDisplacedTriangleMeshClosestHit", "", "displacedTriangleMeshIntersection")
                );
                sbt->setHitGroup(
                    1, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
                    desc.addHitGroup("", "", "displacedTriangleMeshIntersection")
                );
            }

            if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::Curve),
                    desc.addHitGroup("scatterCurveClosestHit", "", "curveIntersection")
                );
                sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("", "", "curveIntersection"));
            }

            if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid),
                    desc.addHitGroup("scatterSdfGridClosestHit", "", "sdfGridIntersection")
                );
                sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("", "", "sdfGridIntersection"));
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

        //Check if the scene has more than 1 light
        if (mpScene->getLightCount() <= 1)
            mPathLightSampleMode = PathSMLightSampleMode::Uniform;
    }
}

void TestPathSM::LightMVP::calculate(ref<Light> light, float2 nearFar)
{
    auto& data = light->getData();
    float3 lightTarget = data.posW + data.dirW;
    const float3 up = abs(data.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    view = math::matrixFromLookAt(data.posW, lightTarget, up);
    projection = math::perspective(data.openingAngle * 2, 1.f, nearFar.x, nearFar.y);
    viewProjection = math::mul(projection, view);
    invViewProjection = math::inverse(viewProjection);
}

void TestPathSM::calculateShadowMapNearFar(RenderContext* pRenderContext, const RenderData& renderData, ShaderVar& var) {
    // Get Analytic light data
    auto lights = mpScene->getLights();

    for (uint i = 0; i < lights.size(); i++)
    {
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = lights[i]->getData().posW;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gSamples"] = 1;
        var["CB"]["gUseMinMaxSM"] = true;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;
        var["CB"]["gCalcNearFar"] = true;

        var["gRayShadowMapMinMax"] = mpShadowMapMinMaxOpti;

        const uint2 targetDim = uint2(mShadowMapSize);
        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenerateSM.pProgram.get(), mGenerateSM.pVars, uint3(targetDim, 1));

        mpShadowMapMinMaxOpti->generateMips(pRenderContext, true,-1,true);

        //CPU read back
        uint subresourceIndex = mpShadowMapMinMaxOpti->getSubresourceIndex(0, mpShadowMapMinMaxOpti->getMipCount()-2);
        std::vector<uint8_t> texData = pRenderContext->readTextureSubresource(mpShadowMapMinMaxOpti.get(), subresourceIndex);
        FALCOR_ASSERT(texData.size() == 32); //There should be 4 values
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

void TestPathSM::generateShadowMap(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Generate Shadow Map");

    //Get Analytic light data
    auto lights = mpScene->getLights();
    //Nothing to do if there are no lights
    if (lights.size() == 0)
        return;

    //Create MVP matrices
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
        rebuild |= rebuildAll;
        if (rebuild)
        {
            mShadowMapMVP[i].calculate(lights[i], mNearFar);
        }
        mRerenderSM |= rebuild;
    }

    mRerenderSM |= mAlwaysRenderSM;
    //If there are no changes compaired to last frame, rerendering is not necessary.
    if (!mRerenderSM)
        return;

    mGenerateSM.pProgram->addDefine("SM_NUM_LIGHTS", std::to_string(lights.size()));

    if (!mGenerateSM.pVars)
    {
        // Configure program.
        mGenerateSM.pProgram->addDefines(mpSampleGenerator->getDefines());
        mGenerateSM.pProgram->setTypeConformances(mpScene->getTypeConformances());
        // Create program variables for the current program.
        // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
        mGenerateSM.pVars = RtProgramVars::create(mpDevice, mGenerateSM.pProgram, mGenerateSM.pBindingTable);
    }

    // Bind utility classes into shared data.
    auto var = mGenerateSM.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);

    if (mUseOptimizedNearFarForShadowMap)
        calculateShadowMapNearFar(pRenderContext, renderData,var);

    const uint2 targetDim = uint2(mShadowMapSize);
    //Generate one shadow map per light
    for (uint i = 0; i < lights.size(); i++)
    {
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = lights[i]->getData().posW;
        var["CB"]["gNear"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[i].x : mNearFar.x;
        var["CB"]["gFar"] = mUseOptimizedNearFarForShadowMap ? mNearFarPerLight[i].y : mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gSamples"] = mShadowMapSamples;
        var["CB"]["gUseMinMaxSM"] = mUseMinMaxShadowMap;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;
        var["CB"]["gCalcNearFar"] = false;

        if(mUseMinMaxShadowMap)
            var["gRayShadowMapMinMax"] = mpRayShadowMapsMinMax[i];
        else
            var["gRayShadowMap"] = mpRayShadowMaps[i];

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenerateSM.pProgram.get(), mGenerateSM.pVars, uint3(targetDim, 1));
    }

    mRebuildSMBuffers = false;
    mRerenderSM = false;
}

void TestPathSM::traceScene(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "Path Tracer");

    auto& dict = renderData.getDictionary();
    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mTracer.pProgram->addDefine("MAX_DIFFUSE_BOUNCES", std::to_string(std::min(mMaxDiffuseBounces,mMaxBounces)));
    mTracer.pProgram->addDefine("MAX_SPECULAR_BOUNCES", std::to_string(std::min(mMaxSpecularBounces, mMaxBounces)));
    mTracer.pProgram->addDefine("MAX_TRANSMISSVE_BOUNCES", std::to_string(std::min(mMaxTransmissiveBounces, mMaxBounces)));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_RUSSIAN_ROULETTE", mUseRussianRoulette ? "1" : "0");
    mTracer.pProgram->addDefine(
        "COUNT_SM", mUseMinMaxShadowMap ? std::to_string(mpRayShadowMapsMinMax.size()) : std::to_string(mpRayShadowMaps.size())
    );
    mTracer.pProgram->addDefine("PATHSM_LIGHT_SAMPLE_MODE", std::to_string((uint32_t)mPathLightSampleMode));
    mTracer.pProgram->addDefine("USE_SEPERATE_LIGHT_SAMPLER", mUseSeperateLightSampler ? "1" : "0");
    mTracer.pProgram->addDefine("LIGHT_SAMPLER_BLOCK_SIZE", std::to_string(mSeperateLightSamplerBlockSize));
    mTracer.pProgram->addDefine("USE_SHADOW_RAY", mShadowMode != ShadowMode::ShadowMap ? "1" : "0");
    mTracer.pProgram->addDefine("USE_MIN_MAX_SM", mUseMinMaxShadowMap ? "1" : "0");
    mTracer.pProgram->addDefine("LT_BOUNDS_START", std::to_string(mLtBoundsStart));
    mTracer.pProgram->addDefine("USE_DEBUG", mEnableDebug ? "1" : "0");
    mTracer.pProgram->addDefine("WRITE_TO_DEBUG", mEnableDebug && !is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) ? "1" : "0");
    mTracer.pProgram->addDefine("DEBUG_MODE", std::to_string((uint32_t)mDebugMode));
    mTracer.pProgram->addDefine("DEBUG_ACCUMULATE", mAccumulateDebug ? "1" : "0");
    mTracer.pProgram->addDefine("SM_GENERATION_RAYTRACING", std::to_string(mSMGenerationUseRay));
    mTracer.pProgram->addDefine("DISTRIBUTE_RAY_OUTSIDE_SM", mDistributeRayOutsideOfSM ? "1" : "0");
    mTracer.pProgram->addDefines(filterSMModesDefines());
    mTracer.pProgram->addDefines(mpShadowMapOracle->getDefines());


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
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    var["CB"]["gUseShadowMap"] = mShadowMode != ShadowMode::RayShadows;
    var["CB"]["gRayShadowMapRes"] = mShadowMapSize;
    var["CB"]["gLtBoundsMaxReduction"] = mLtBoundsMaxReduction;
    var["CB"]["gIterationCount"] = mIterationCount;
    var["CB"]["gSelectedBounce"] = mDebugShowBounce;
    var["CB"]["gDebugFactor"] = mDebugMult;
    var["CB"]["gUseRayForDirect"] = !mUseSMForDirect;

    mpShadowMapOracle->setVars(var);

    //Bind Shadow MVPS and Shadow Map
    FALCOR_ASSERT(mpRayShadowMaps.size() == mShadowMapMVP.size() || mpRayShadowMapsMinMax.size() == mShadowMapMVP.size());
    for (uint i = 0; i < mShadowMapMVP.size(); i++)
    {
        var["ShadowNearFar"]["gSMNearFar"][i] = mUseOptimizedNearFarForShadowMap?  mNearFarPerLight[i] : mNearFar;
        var["ShadowVPs"]["gRayShadowMapVP"][i] = mShadowMapMVP[i].viewProjection;
        if (mUseMinMaxShadowMap)
            var["gRayShadowMapMinMax"][i] = mpRayShadowMapsMinMax[i];
        else
            var["gRayShadowMap"][i] = mpRayShadowMaps[i];

        bool validAccessTex = (mpShadowMapAccessTex.size() == mpRayShadowMaps.size()) || (mpShadowMapAccessTex.size() == mpRayShadowMapsMinMax.size());
        validAccessTex &= !mpShadowMapAccessTex.empty();
        if (validAccessTex)
            var["gShadowAccessDebugTex"][i] = mpShadowMapAccessTex[i];
    }
        
    //Bind Samplers
    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;

    //RasterSM
    if (mpRasterShadowMap)
    {
        mpRasterShadowMap->setShaderDataAndBindBlock(var, renderData.getDefaultTextureDims());
    }

    //Pixel Stats
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

void TestPathSM::prepareBuffers() {

    //Shadow Map
    if (mRebuildSMBuffers)
    {
        mpRayShadowMaps.clear();
        mpRayShadowMapsMinMax.clear();
        mShadowMapMVP.clear();
        mpShadowMapBlit.reset();
        mpShadowMapAccessTex.clear();
        mpShadowMapMinMaxOpti.reset();

        mRerenderSM = true;
    }

    // Get Analytic light data
    auto lights = mpScene->getLights();
    uint numShadowMaps = mUseMinMaxShadowMap ? mpRayShadowMapsMinMax.size() : mpRayShadowMaps.size();

    // Check if the shadow map is up to date
    if (mUseMinMaxShadowMap)
    {
        if (numShadowMaps != lights.size() && lights.size() > 0)
        {
            mpRayShadowMapsMinMax.clear();
            // Create a shadow map per light
            for (uint i = 0; i < lights.size(); i++)
            {
                ref<Texture> tex = Texture::create2D(
                    mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
                );
                tex->setName("TestPathSM::ShadowMap" + i);
                mpRayShadowMapsMinMax.push_back(tex);
            }
        }

        if (!mpRayShadowMaps.empty())
            mpRayShadowMaps.clear();
    }
    else
    {
        if (numShadowMaps != lights.size() && lights.size() > 0)
        {
            mpRayShadowMaps.clear();
            // Create a shadow map per light
            for (uint i = 0; i < lights.size(); i++)
            {
                ref<Texture> tex = Texture::create2D(
                    mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::R32Float, 1u, 1u, nullptr,
                    ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
                );
                tex->setName("TestPathSM::ShadowMap" + i);
                mpRayShadowMaps.push_back(tex);
            }
        }

        if (!mpRayShadowMapsMinMax.empty())
            mpRayShadowMapsMinMax.clear();
    }

    //Update
    numShadowMaps = mUseMinMaxShadowMap ? mpRayShadowMapsMinMax.size() : mpRayShadowMaps.size();

    //MinMax opti buffer
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

    //Debug Textures

    //Blit Tex
    if (mEnableDebug && is_set(mDebugMode, PathSMDebugModes::ShadowMapFlag) && numShadowMaps > 0)
    {
        // Reset if size is not right
        if (mpShadowMapBlit)
            if (mpShadowMapBlit->getWidth() != mShadowMapSize)
                mpShadowMapBlit.reset();
        if (!mpShadowMapBlit)
        {
            mpShadowMapBlit = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RGBA32Float,
                1u, 1u, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mpShadowMapBlit->setName("TestPathSM::ShadowMapBlitTex");
        }
    }
    else if(mpShadowMapBlit)
    {
        mpShadowMapBlit.reset();
    }

    //SM accumulate tex
    if (mEnableDebug && (mDebugMode == PathSMDebugModes::ShadowMapAccess || mDebugMode == PathSMDebugModes::ShadowMapRayDiff) &&
        numShadowMaps > 0)
    {
        //Check if size and shadow map size is still right
        if (!mpShadowMapAccessTex.empty())
        {
            
            if ((mpShadowMapAccessTex.size() != numShadowMaps) || (mpShadowMapAccessTex[0]->getWidth() != mShadowMapSize))
            {
                mpShadowMapAccessTex.clear();
            }
        }

        //Create
        if (mpShadowMapAccessTex.empty())
        {
            mpShadowMapAccessTex.resize(numShadowMaps);
            for (uint i = 0; i < numShadowMaps; i++)
            {
                mpShadowMapAccessTex[i] = Texture::create2D(
                    mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::R32Uint,1u, 1u, nullptr,
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

void TestPathSM::prepareVars()
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

DefineList TestPathSM::filterSMModesDefines() {
    DefineList defines;
    defines.add("FILTER_SM_VARIANCE", mFilterSMMode == FilterSMMode::Variance ? "1" : "0");
    defines.add("FILTER_SM_ESVM", mFilterSMMode == FilterSMMode::ESVM ? "1" : "0");
    defines.add("FILTER_SM_MSM", mFilterSMMode == FilterSMMode::MSM ? "1" : "0");
    return defines;
}

void TestPathSM::debugShadowMapPass(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "DebugShadowMap");
 
    // Create Pass
    if (!mpDebugShadowMapPass)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderDebugSM).csEntry("main").setShaderModel("6_5");

        DefineList defines;
        defines.add("USE_MINMAX_SM", mUseMinMaxShadowMap ? "1" : "0");

        mpDebugShadowMapPass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpDebugShadowMapPass);

    //Runtime defines
    mpDebugShadowMapPass->getProgram()->addDefine("USE_MINMAX_SM", mUseMinMaxShadowMap ? "1" : "0");

    // Set variables
    float debugHeatMapMaxCount =
        mDebugMode == PathSMDebugModes::ShadowMapAccess ? mDebugHeatMapMaxCountAccess : mDebugHeatMapMaxCountDifference;

    auto var = mpDebugShadowMapPass->getRootVar();
    var["CB"]["gDebugMode"] = (uint)mDebugMode;
    var["CB"]["gBlendVal"] = mDebugAccessBlendVal;
    var["CB"]["gMaxValue"] = debugHeatMapMaxCount * (mIterationCount + 1);
    var["CB"]["gBrightnessIncrease"] = mDebugBrighnessMod;

    if (mUseMinMaxShadowMap)
        var["gRayShadowMapMinMax"] = mpRayShadowMapsMinMax[mUISelectedLight];
    else
        var["gRayShadowMap"] = mpRayShadowMaps[mUISelectedLight];

    if (mpShadowMapAccessTex.size() > mUISelectedLight)
        var["gShadowAccessTex"] = mpShadowMapAccessTex[mUISelectedLight];
    var["gOut"] = mpShadowMapBlit;

    // Execute
    const uint2 targetDim = uint2(mShadowMapSize);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpDebugShadowMapPass->execute(pRenderContext, uint3(targetDim, 1));

    //Blit the shadow map into the Debug texture
    //Screen size
    uint2 screenSize = renderData.getDefaultTextureDims();
    uint sizeDiff = (screenSize.x - screenSize.y)/2;
    uint4 outRect = mDebugUseSMAspect ? uint4(sizeDiff, 0, screenSize.x - sizeDiff, screenSize.y):
          uint4(0, 0, screenSize.x, screenSize.y);

    ref<Texture> outTex = renderData.getTexture("debug");
    pRenderContext->blit(mpShadowMapBlit->getSRV(), outTex->getRTV(), uint4(0, 0, mShadowMapSize, mShadowMapSize), outRect
    );
}
