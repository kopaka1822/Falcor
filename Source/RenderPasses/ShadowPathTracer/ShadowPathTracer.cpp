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
#include "ShadowPathTracer.h"
#include "RenderGraph/RenderPassHelpers.h"

namespace
{
    const std::string kPathTracingShader = "RenderPasses/ShadowPathTracer/Shaders/PathTrace.rt.slang";
    const std::string kShaderModel = "6_5";

     // Render Pass inputs and outputs
    const std::string kInputVBuffer = "vbuffer";

    const Falcor::ChannelList kInputChannels{
        {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"}
    };

    const std::string kOutputColor = "color";
    const Falcor::ChannelList kOutputChannels{
        {kOutputColor, "gOutColor", "Output Color (linear)", true /*optional*/, ResourceFormat::RGBA32Float},
    };
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ShadowPathTracer>();
}

ShadowPathTracer::ShadowPathTracer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    if (!mpDevice->isShaderModelSupported(Device::ShaderModel::SM6_5))
    {
        throw RuntimeError("ReSTIR_FG: Shader Model 6.5 is not supported by the current device");
    }
    if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::RaytracingTier1_1))
    {
        throw RuntimeError("ReSTIR_FG: Raytracing Tier 1.1 is not supported by the current device");
    }

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, mpSampleGenerator);
}

Properties ShadowPathTracer::getProperties() const
{
    return {};
}

RenderPassReflection ShadowPathTracer::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ShadowPathTracer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    mpScene = pScene;

    mPathProgram = RayTraceProgramHelper::create();

    if (mpScene)
    {
        auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
        mPathProgram.initRTProgram(mpDevice, mpScene, kPathTracingShader, 96u, globalTypeConformances);

        mpShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);

        //Set TMax depending on scene bounds
        auto& sceneBounds = mpScene->getSceneBounds();
        mRayTMax = sceneBounds.radius() * 2;
    }

   
}

void ShadowPathTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) //Return on empty scene
        return;

    if ((mScreenRes.x != renderData.getDefaultTextureDims().x) || (mScreenRes.y != renderData.getDefaultTextureDims().y))
    {
        mScreenRes = renderData.getDefaultTextureDims();
    }
 
    //Calculate the shadow map
    if (!mpShadowMap->update(pRenderContext))
        return;

    mPathProgram.pProgram->addDefine("USE_EMISSIVE_LIGHT", mUseEmissiveLight ? "1" : "0");
    mPathProgram.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mPathProgram.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mPathProgram.pProgram->addDefine("RAY_TMAX", std::to_string(mRayTMax));
    mPathProgram.pProgram->addDefine("EVALUATE_ALL_ANALYTIC_LIGHTS", mEvalAllAnalyticLights ? "1" : "0");
    mPathProgram.pProgram->addDefine("ADJUST_SHADING_NORMALS", mAdjustShadingNormals ? "1" : "0");
    mPathProgram.pProgram->addDefines(mpShadowMap->getDefines());

    if (!mPathProgram.pVars)
    {
        FALCOR_ASSERT(mPathProgram.pProgram);

        mPathProgram.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    };

    FALCOR_ASSERT(mPathProgram.pVars);

    auto var = mPathProgram.pVars->getRootVar();
    auto constBuff = var["CB"];

    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gMaxBounces"] = mMaxBounces;
    var["CB"]["gUseShadowMap"] = mUseShadowMapBounce;
        
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gColor"] = renderData[kOutputColor]->asTexture();

    mpShadowMap->setShaderData();
    var["gShadowMap"] = mpShadowMap->getParameterBlock();

    FALCOR_ASSERT(mScreenRes.x > 0 && mScreenRes.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mPathProgram.pProgram.get(), mPathProgram.pVars, uint3(mScreenRes, 1));

    mFrameCount++;
}

void ShadowPathTracer::renderUI(Gui::Widgets& widget)
{
    mpShadowMap->renderUI(widget);

    if (auto group = widget.group("Path Tracer"))
    {
        widget.var("Max Bounces", mMaxBounces, 0u, 32u, 1u);
        widget.tooltip("Number of Indirect Light Bounces");
        widget.var("Use SM from Bounce", mUseShadowMapBounce, 0u, mMaxBounces, 1u);
        widget.tooltip("Tells the renderer, at which bounces the shadow maps should be used");
        widget.checkbox("Eval all lights per hit", mEvalAllAnalyticLights);
        widget.checkbox("Enable Emissive Light", mUseEmissiveLight);
        widget.checkbox("Importance Sampling", mUseImportanceSampling);
        widget.checkbox("Adjust Shading Normals", mAdjustShadingNormals);
    }
}

void ShadowPathTracer::RayTraceProgramHelper::initRTProgram(
    ref<Device> device,
    ref<Scene> scene,
    const std::string& shaderName,
    uint maxPayloadBytes,
    const Program::TypeConformanceList& globalTypeConformances
)
{
    RtProgram::Desc desc;
    desc.addShaderModules(scene->getShaderModules());
    desc.addShaderLibrary(shaderName);
    desc.setMaxPayloadSize(maxPayloadBytes);
    desc.setMaxAttributeSize(scene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(2);  //2, to allow shadow rays in closest hit rays
    if (!scene->hasProceduralGeometry())
        desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

    pBindingTable = RtBindingTable::create(2, 2, scene->getGeometryCount());
    auto& sbt = pBindingTable;
    sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
    sbt->setMiss(0, desc.addMiss("miss"));
    sbt->setMiss(1, desc.addMiss("shadowMiss"));

    // TODO: Support more geometry types and more material conformances
    if (scene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(0, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", ""));
    }

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void ShadowPathTracer::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
{
    FALCOR_ASSERT(pProgram);

    // Configure program.
    pProgram->addDefines(pSampleGenerator->getDefines());
    pProgram->setTypeConformances(pScene->getTypeConformances());
    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    pVars = RtProgramVars::create(pDevice, pProgram, pBindingTable);

    // Bind utility classes into shared data.
    auto var = pVars->getRootVar();
    pSampleGenerator->setShaderData(var);
}
