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
#include "MinimalPathTracerShadowMap.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, MinimalPathTracerShadowMap>();
}

namespace
{
    const char kShaderFile[] = "RenderPasses/MinimalPathTracerShadowMap/MinimalPathTracerShadowMap.rt.slang";

    // Ray tracing settings that affect the traversal stack size.
    // These should be set as small as possible.
    const uint32_t kMaxPayloadSizeBytes = 88u;
    const uint32_t kMaxRecursionDepth = 2u;

    const char kInputViewDir[] = "viewW";

    const ChannelList kInputChannels =
    {
        { "vbuffer",        "gVBuffer",     "Visibility buffer in packed format" },
        { kInputViewDir,    "gViewW",       "World-space view direction (xyz float format)", true /* optional */ },
    };

    const ChannelList kOutputChannels =
    {
        { "color",          "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float },
    };

    const char kMaxBounces[] = "maxBounces";
    const char kComputeDirect[] = "computeDirect";
    const char kUseImportanceSampling[] = "useImportanceSampling";
    const char kUseEmissiveLight[] = "useEmissiveLight";
    const char kUseAlphaTest[] = "useAlphaTest";
    const char kUseHybridSM[] = "useHybridSM";
    const char kShadowMapBounces[] = "shadowMapBounces";
    }

MinimalPathTracerShadowMap::MinimalPathTracerShadowMap(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    parseProperties(props);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

void MinimalPathTracerShadowMap::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces) mMaxBounces = value;
        else if (key == kComputeDirect) mComputeDirect = value;
        else if (key == kUseImportanceSampling) mUseImportanceSampling = value;
        else if (key == kUseEmissiveLight) mUseEmissiveLight = value;
        else if (key == kUseAlphaTest) mUseAlphaTest = value;
        else if (key == kUseHybridSM) mUseHybridSM = value;
        else if (key == kShadowMapBounces) mUseShadowMapBounce = value;
        else logWarning("Unknown property '{}' in MinimalPathTracerShadowMap properties.", key);
    }
}

Properties MinimalPathTracerShadowMap::getProperties() const
{
    Properties props;
    props[kMaxBounces] = mMaxBounces;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;
    props[kUseEmissiveLight] = mUseEmissiveLight;
    props[kUseAlphaTest] = mUseAlphaTest;
    props[kUseHybridSM] = mUseHybridSM;
    props[kShadowMapBounces] = mUseShadowMapBounce;
    return props;
}

RenderPassReflection MinimalPathTracerShadowMap::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void MinimalPathTracerShadowMap::execute(RenderContext* pRenderContext, const RenderData& renderData)
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
            if (pDst) pRenderContext->clearTexture(pDst);
        }
        return;
    }

    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        throw RuntimeError("MinimalPathTracerShadowMap: This render pass does not support scene geometry changes.");
    }

    // Calculate and update the shadow map
    if (!mpShadowMap->update(pRenderContext))
        return;

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

    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() && mUseEmissiveLight ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracer.pProgram->addDefine("ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    mTracer.pProgram->addDefine("USE_HYBRID_SM", mUseHybridSM ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ORACLE_FUNCTION", mUseSMOracle ? "1" : "0");
    mTracer.pProgram->addDefine("SHOW_ORACLE_INFLUENCE", mShowOracleFunc ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ORACLE_DISTANCE_FUNCTION", mUseOracleDistFactor ? "1" : "0");
    mTracer.pProgram->addDefine("ORACLE_DEBUG_SHOW_LIGHT_IDX", std::to_string(mShowOracleShowOnlyLightIdx));
    mTracer.pProgram->addDefine("TEX_LOD_MODE", std::to_string(static_cast<uint32_t>(mTexLODMode)));
    mTracer.pProgram->addDefine("RAY_CONES_MODE", std::to_string(static_cast<uint32_t>(mRayConeMode)));
    mTracer.pProgram->addDefines(mpShadowMap->getDefines());

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mTracer.pVars) prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Set constants.
    auto var = mTracer.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    var["CB"]["gUseShadowMap"] = mUseShadowMapBounce;
    var["CB"]["gOracleComp"] = mOracleCompaireValue;
    var["CB"]["gScreenSpacePixelSpreadAngle"] = mpScene->getCamera()->computeScreenSpacePixelSpreadAngle(targetDim.y);

    //Set Shadow Map per Iteration Shader Data
    mpShadowMap->setShaderDataAndBindBlock(var, renderData.getDefaultTextureDims());

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };
    for (auto channel : kInputChannels) bind(channel);
    for (auto channel : kOutputChannels) bind(channel);

    //Bind Debug Texture
    handleDebugOracleTexture(var, renderData);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));

    //Copy to out
    if (mShowOracleFunc)
    {
        pRenderContext->copyResource(renderData["color"]->asTexture().get(), mpOracleDebug[mShowOracleFuncLevel].get());
    }

    mFrameCount++;
}

void MinimalPathTracerShadowMap::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect illumination.\n0 = direct only\n1 = one indirect bounce etc.", true);

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect is computed (when max bounces > 0).", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);
    widget.tooltip("Use importance sampling for materials", true);

    dirty |= widget.checkbox("Alpha Test", mUseAlphaTest);
    widget.tooltip("Alpha test for the rays", true);

    dirty |= widget.checkbox("Emissive Light", mUseEmissiveLight);
    widget.tooltip("Enable/Disable Emissive Light", true);

    if (auto mode = mTexLODMode; widget.dropdown("Texture LOD mode", mode))
    {
        setTexLODMode(mode);
        dirty = true;
    }
    widget.tooltip("The texture level-of-detail mode to use.", true);
    if (mTexLODMode == TexLODMode::RayCones)
    {
        if (auto mode = mRayConeMode; widget.dropdown("Ray cone mode", mode))
        {
            setRayConeMode(mode);
            dirty = true;
        }
        widget.tooltip("The variant of ray cones to use.");
    }

    if (auto group = widget.group("Shadow Map Options"))
    {
        group.checkbox("Use Oracle Function", mUseSMOracle);
        group.tooltip("Enables the oracle function for Shadow Mapping", true);
        if (mUseSMOracle)
        {
            if (Gui::Group group2 = widget.group("OracleOptions", true)) // TODO add oracle settings if too many factors appear
            {
                group2.var("Oracle Compaire Value", mOracleCompaireValue, 0.f, 64.f, 0.1f);
                group2.tooltip("Compaire Value for the Oracle function. Is basically compaired against ShadowMapPixelArea/CameraPixelArea.");
                group2.checkbox("Use Lobe factor", mUseOracleDistFactor);
                group2.tooltip("Uses a factor that increases the distance if a rough lobe was used (diffuse; specular WIP)"); // TODO change text if specular is implemented
            }
        }
        

        group.var("Use SM from Bounce", mUseShadowMapBounce, 0u, mMaxBounces + 1, 1u);
        group.tooltip(
            "Tells the renderer, at which bounces the shadow maps should be used. To disable shadow map usage set to \"Max bounces\" + 1 ",
            true
        );
        if (group.checkbox("Use Hybrid SM", mUseHybridSM))
        {
            if (mUseHybridSM)
                mOracleCompaireValue = 4.f;
        }
        group.tooltip("Enables Hybrid Shadow Maps, where the edge of the shadow map is traced", true);

       

        if (mpShadowMap)
            mpShadowMap->renderUI(group);
        else
            group.text("Further Shadow Map Options to appear \n when a scene is loaded in.");

        group.checkbox("Show Oracle Function", mShowOracleFunc);
        group.tooltip("Use SM = Red; Use RayTracing/Hybrid SM = green. Shows only the for the first SM bounce", true);
        if (mShowOracleFunc)
        {
            group.var("Show (Oracle) level", mShowOracleFuncLevel, 0u, mMaxBounces, 1u);
            group.var("Oracle Only Show Light", mShowOracleShowOnlyLightIdx, -1, 100000000, 1);
            group.tooltip("Only shows the light with the responding index. -1 Shows all lights. >NumLights will be black", true);
        }
    }

    // If rendering options that modify the output have changed, set flag to indicate that.
    // In execute() we will pass the flag to other passes for reset of temporal data etc.
    if (dirty)
    {
        mOptionsChanged = true;
    }
}

void MinimalPathTracerShadowMap::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
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
        if (pScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("MinimalPathTracerShadowMap: This render pass does not support custom primitives.");
        }

        //Init the shadow map
        mpShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);

        // Create ray tracing program.
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
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit"));
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit"));
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh), desc.addHitGroup("scatterDisplacedTriangleMeshClosestHit", "", "displacedTriangleMeshIntersection"));
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh), desc.addHitGroup("", "", "displacedTriangleMeshIntersection"));
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("scatterCurveClosestHit", "", "curveIntersection"));
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("", "", "curveIntersection"));
        }

        if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("scatterSdfGridClosestHit", "", "sdfGridIntersection"));
            sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("", "", "sdfGridIntersection"));
        }

        mTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    }
}

void MinimalPathTracerShadowMap::prepareVars()
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

void MinimalPathTracerShadowMap::handleDebugOracleTexture(ShaderVar& var, const RenderData& renderData)
{
    if (!mShowOracleFunc)
    {
        if (!mpOracleDebug.empty())
            mpOracleDebug.clear();
        return;
    }
        
    if (mpOracleDebug.empty() || mpOracleDebug.size() != mMaxBounces)
    {
        mpOracleDebug.clear();
        mpOracleDebug.resize(mMaxBounces+1);

        uint2 dims = renderData.getDefaultTextureDims();

        for (uint i = 0; i <= mMaxBounces; i++)
        {
            mpOracleDebug[i] = Texture::create2D(
                mpDevice, dims.x, dims.y, ResourceFormat::RGBA32Float, 1, 1, nullptr, Resource::BindFlags::UnorderedAccess
            );
            mpOracleDebug[i]->setName("MPTShadow::OracleDebug" + std::to_string(i));
        }
    }

    for (uint i = 0; i < mpOracleDebug.size(); i++)
    {
        var["gOracleDebug"][i] = mpOracleDebug[i];
    }
}

void MinimalPathTracerShadowMap::setTexLODMode(TexLODMode lodMode) {
    if (lodMode == TexLODMode::Stochastic || lodMode == TexLODMode::RayDiffs)
    {
        logWarning("(Minimal Path Tracer) Unsupported Lod Mode");
        return;
    }
    mTexLODMode = lodMode;
}
