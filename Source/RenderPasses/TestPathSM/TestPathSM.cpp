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
};

const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";

const Gui::DropdownList kShadowMapSizes{
    {256, "256x256"},
    {512, "512x512"},
    {1024, "1024x1024"},
    {2048, "2048x2048"},
    {4096, "4096x4096"},
};
} // namespace

TestPathSM::TestPathSM(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);

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

    generateShadowMap(pRenderContext, renderData);

    if (mShowShadowMap)
    {
        ref<Texture> outTex = renderData.getTexture("color");
        pRenderContext->blit(mpShadowMaps[mShowLight]->getSRV(), outTex->getRTV());
    }else
        traceScene(pRenderContext, renderData);

    mFrameCount++;
}

void TestPathSM::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect illumination.\n0 = direct only\n1 = one indirect bounce etc.", true);

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.checkbox("Use Russian Roulette", mUseRussianRoulette);
    widget.tooltip("Enables Russian Roulette to end path with low throuput prematurely", true);

    dirty |= widget.checkbox("Enable Shadow Maps", mUseShadowMap);

    if (mUseShadowMap)
    {
        if (auto group = widget.group("ShadowMap Options"))
        {
            bool clearSM = group.dropdown("Shadow Map Size", kShadowMapSizes, mShadowMapSize);
            if (clearSM)
            {
                mpShadowMaps.clear();
                dirty = true;
            }

        }
    }

    if (mpScene)
    {
        dirty |= widget.checkbox("Show Shadow Map", mShowShadowMap);
        if (mShowShadowMap)
            dirty |= widget.var("Select Light", mShowLight, 0u, mpScene->getLightCount() - 1, 1u);
    }
       
    // If rendering options that modify the output have changed, set flag to indicate that.
    // In execute() we will pass the flag to other passes for reset of temporal data etc.
    if (dirty)
    {
        mOptionsChanged = true;
    }
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
        
    }
}

void TestPathSM::generateShadowMap(RenderContext* pRenderContext, const RenderData& renderData) {

    //Get Analytic light data
    auto lights = mpScene->getActiveLights();
    //Nothing to do if there are no lights
    if (lights.size() == 0)
        return;

    //Check if the buffer is up to date
    if (mpShadowMaps.size() != lights.size())
    {
        mpShadowMaps.clear();
        //Create a shadow map per light
        for (uint i = 0; i < lights.size(); i++)
        {
            ref<Texture> tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::RG32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
            tex->setName("TestPathSM::ShadowMap" + i);
            mpShadowMaps.push_back(tex);
        }
    }

    //Create MVP matrices
    bool rebuildAll = false;
    if (mShadowMapMVP.size() != lights.size())
    {
        mShadowMapMVP.resize(lights.size());
        rebuildAll = true;
    }
    for (uint i = 0; i < lights.size(); i++)
    {
        auto changes = lights[i]->getChanges();
        bool rebuild = is_set(changes, Light::Changes::Position) || is_set(changes, Light::Changes::Direction);
        rebuild |= rebuildAll;
        if (rebuild)
        {
            auto& data = lights[i]->getData();
            float3 lightTarget = data.posW + data.dirW;
            const float3 up = abs(data.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
            mShadowMapMVP[i].view = math::matrixFromLookAt(data.posW, lightTarget, up);
            mShadowMapMVP[i].projection = math::perspective(data.openingAngle * 2, 1.f, mNearFar.x, mNearFar.y);
            mShadowMapMVP[i].viewProjection = math::mul(mShadowMapMVP[i].projection, mShadowMapMVP[i].view);
            mShadowMapMVP[i].invViewProjection = math::inverse(mShadowMapMVP[i].viewProjection);
        }
    }

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

    const uint2 targetDim = uint2(mShadowMapSize);
    //Generate one shadow map per light
    for (uint i = 0; i < lights.size(); i++)
    {
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gLightPos"] = lights[i]->getData().posW;
        var["CB"]["gNear"] = mNearFar.x;
        var["CB"]["gFar"] = mNearFar.y;
        var["CB"]["gViewProj"] = mShadowMapMVP[i].viewProjection;
        var["CB"]["gInvViewProj"] = mShadowMapMVP[i].invViewProjection;


        var["gShadowMap"] = mpShadowMaps[i];

        // Spawn the rays.
        mpScene->raytrace(pRenderContext, mGenerateSM.pProgram.get(), mGenerateSM.pVars, uint3(targetDim, 1));
    }
}

void TestPathSM::traceScene(RenderContext* pRenderContext, const RenderData& renderData) {
    auto& dict = renderData.getDictionary();
    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_RUSSIAN_ROULETTE", mUseRussianRoulette ? "1" : "0");
    mTracer.pProgram->addDefine("COUNT_SM", std::to_string(mpShadowMaps.size()));

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mTracer.pVars)
        prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    // Set constants.
    auto var = mTracer.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    var["CB"]["gNear"] = mNearFar.x;
    var["CB"]["gFar"] = mNearFar.y;
    var["CB"]["gUseShadowMap"] = mUseShadowMap;
    

    //Bind Shadow MVPS and Shadow Map
    FALCOR_ASSERT(mpShadowMaps.size() == mShadowMapMVP.size());
    for (uint i = 0; i < mpShadowMaps.size(); i++)
    {
        var["ShadowVPs"]["gShadowMapVP"][i] = mShadowMapMVP[i].viewProjection;
        var["gShadowMap"][i] = mpShadowMaps[i];
    }

    //Bind Samplers
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
    for (auto channel : kOutputChannels)
        bind(channel);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
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
