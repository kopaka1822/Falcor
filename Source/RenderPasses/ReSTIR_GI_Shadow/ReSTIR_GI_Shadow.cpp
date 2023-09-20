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
#include "ReSTIR_GI_Shadow.h"
#include "RenderGraph/RenderPassHelpers.h"

namespace
{
const std::string kTraceTransmissionDeltaShader = "RenderPasses/ReSTIR_GI_Shadow/Shader/TraceTransmissionDelta.rt.slang";
const std::string kPathSamplesShader = "RenderPasses/ReSTIR_GI_Shadow/Shader/GeneratePathSamples.rt.slang";
const std::string kResamplingPassShader = "RenderPasses/ReSTIR_GI_Shadow/Shader/ResamplingPass.cs.slang";
const std::string kFinalShadingPassShader = "RenderPasses/ReSTIR_GI_Shadow/Shader/FinalShading.cs.slang";

const std::string kShaderModel = "6_5";
const uint kMaxPayloadBytes = 96u;

// Render Pass inputs and outputs
const std::string kInputVBuffer = "vbuffer";
const std::string kInputMotionVectors = "mvec";
// const std::string kInputViewDir = "viewW";
// const std::string kInputRayDistance = "rayDist";

const Falcor::ChannelList kInputChannels{
    {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
    {kInputMotionVectors, "gMotionVectors", "Motion vector buffer (float format)", true /* optional */},
};

const std::string kOutputColor = "color";
const std::string kOutputEmission = "emission";
const std::string kOutputDiffuseRadiance = "diffuseRadiance";
const std::string kOutputSpecularRadiance = "specularRadiance";
const std::string kOutputDiffuseReflectance = "diffuseReflectance";
const std::string kOutputSpecularReflectance = "specularReflectance";
const std::string kOutputResidualRadiance = "residualRadiance"; // The rest (transmission, delta)

const Falcor::ChannelList kOutputChannels{
    {kOutputColor, "gOutColor", "Output Color (linear)", true /*optional*/, ResourceFormat::RGBA32Float},
    {kOutputEmission, "gOutEmission", "Output Emission", true /*optional*/, ResourceFormat::RGBA32Float},
    {kOutputDiffuseRadiance, "gOutDiffuseRadiance", "Output demodulated diffuse color (linear)", true /*optional*/,
     ResourceFormat::RGBA32Float},
    {kOutputSpecularRadiance, "gOutSpecularRadiance", "Output demodulated specular color (linear)", true /*optional*/,
     ResourceFormat::RGBA32Float},
    {kOutputDiffuseReflectance, "gOutDiffuseReflectance", "Output primary surface diffuse reflectance", true /*optional*/,
     ResourceFormat::RGBA16Float},
    {kOutputSpecularReflectance, "gOutSpecularReflectance", "Output primary surface specular reflectance", true /*optional*/,
     ResourceFormat::RGBA16Float},
    {kOutputResidualRadiance, "gOutResidualRadiance", "Output residual color (transmission/delta)", true /*optional*/,
     ResourceFormat::RGBA32Float},
};

const Gui::DropdownList kResamplingModeList{
    {(uint)ReSTIR_GI_Shadow::ResamplingMode::Temporal, "Temporal"},
    {(uint)ReSTIR_GI_Shadow::ResamplingMode::Spartial, "Spartial"},
    {(uint)ReSTIR_GI_Shadow::ResamplingMode::SpartioTemporal, "SpartioTemporal"},
};

const Gui::DropdownList kBiasCorrectionModeList{
    {(uint)ReSTIR_GI_Shadow::BiasCorrectionMode::Off, "Off"},
    {(uint)ReSTIR_GI_Shadow::BiasCorrectionMode::Basic, "Basic"},
    {(uint)ReSTIR_GI_Shadow::BiasCorrectionMode::RayTraced, "RayTraced"},
};

const Gui::DropdownList kDirectLightRenderModeList{
    {(uint)ReSTIR_GI_Shadow::DirectLightingMode::None, "None"},
    {(uint)ReSTIR_GI_Shadow::DirectLightingMode::RTXDI, "RTXDI"}};
} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ReSTIR_GI_Shadow>();
}

ReSTIR_GI_Shadow::ReSTIR_GI_Shadow(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    if (!mpDevice->isShaderModelSupported(Device::ShaderModel::SM6_5))
    {
        throw RuntimeError("ReSTIR_GI_Shadow: Shader Model 6.5 is not supported by the current device");
    }
    if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::RaytracingTier1_1))
    {
        throw RuntimeError("ReSTIR_GI_Shadow: Raytracing Tier 1.1 is not supported by the current device");
    }

    // TODO Handle Properties

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, mpSampleGenerator);
}

Properties ReSTIR_GI_Shadow::getProperties() const
{
    return Properties();
}

RenderPassReflection ReSTIR_GI_Shadow::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ReSTIR_GI_Shadow::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) // Return on empty scene
        return;

    const auto& pMotionVectors = renderData[kInputMotionVectors]->asTexture();

    // Init RTXDI if it is enabled
    if (mDirectLightMode == DirectLightingMode::RTXDI && !mpRTXDI)
    {
        mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);
    }
    // Delete RTXDI if it is set and the mode changed
    if (mDirectLightMode != DirectLightingMode::RTXDI && mpRTXDI)
        mpRTXDI.reset();

    // Prepare used Datas and Buffers
    prepareLighting(pRenderContext);

    prepareBuffers(pRenderContext, renderData);

    // Calculate and update the shadow map
    if (mUseShadowMap)
    {
        if (!mpShadowMap->update(pRenderContext))
            return;
    }
        
    // Clear the reservoir
    if (mClearReservoir)
    {
        for (uint i = 0; i < 2; i++)
            pRenderContext->clearUAV(mpReservoirBuffer[i]->getUAV().get(), uint4(0));
        mClearReservoir = false;
    }

    if (mpRTXDI)
        mpRTXDI->beginFrame(pRenderContext, mScreenRes);

    traceTransmissiveDelta(pRenderContext, renderData);

    generatePathSamplesPass(pRenderContext, renderData);

    // Direct light resampling
    if (mpRTXDI)
        mpRTXDI->update(pRenderContext, pMotionVectors, mpViewDir, mpViewDirPrev);

    // Do resampling
    if (mReservoirValid)
        resamplingPass(pRenderContext, renderData);

    finalShadingPass(pRenderContext, renderData);

    copyViewTexture(pRenderContext, renderData);

    if (mpRTXDI)
        mpRTXDI->endFrame(pRenderContext);

    mReservoirValid = true;
    mFrameCount++;
}

void ReSTIR_GI_Shadow::renderUI(Gui::Widgets& widget)
{
    bool changed = false;

    widget.dropdown("Direct Light Mode", kDirectLightRenderModeList, (uint&)mDirectLightMode);

    if (auto group = widget.group("Specular Trace Options"))
    {
        widget.var("Specular/Transmissive Bounces", mTraceMaxBounces, 0u, 32u, 1u);
        widget.tooltip("Number of specular/transmissive bounces. 0 -> direct hit only");
        widget.checkbox("Require Diffuse Part", mTraceRequireDiffuseMat);
        widget.tooltip("Requires a diffuse part in addition to delta lobes");
    }

    if (auto group = widget.group("ReSTIR_GI"))
    {
        group.checkbox("Enable ShadowMap", mUseShadowMap);
        if (mUseShadowMap)
        {
            if (auto group2 = widget.group("Shadow Map Options"))
            {
                if (mpShadowMap)
                    mpShadowMap->renderUI(group2);
            }
        }
        
        if (auto group2 = widget.group("Initial Sample Options"))
        {
            group2.var("GI Max Bounces", mGIMaxBounces, 1u, 32u, 1u);
            group2.tooltip("Number of Bounces the initial sample can have");
            group2.checkbox("GI Alpha Test", mAlphaTest);
            group2.tooltip("Enables the Alpha Test for the ray tracing operations");
            group2.checkbox("GI NEE", mGINEE);
            group2.tooltip("Enables NextEventEstimation for the initial Samples. Else only hits with the emissive geometry increase the radiance");
            if (mGINEE)
            {
                group2.checkbox("GI MIS", mGIMIS);
                group2.tooltip("Enables MIS for the Emissive light sources evaluated in NEE");
            }
            group2.checkbox("GI Russian Roulette", mGIRussianRoulette);
            group2.tooltip("Aborts a path early if the throughput is too low");
        }

        changed |= widget.dropdown("ResamplingMode", kResamplingModeList, (uint&)mResamplingMode);

        changed |= widget.dropdown("BiasCorrection", kBiasCorrectionModeList, (uint&)mBiasCorrectionMode);

        changed |= widget.var("Depth Threshold", mRelativeDepthThreshold, 0.0f, 1.0f, 0.0001f);
        widget.tooltip("Relative depth threshold. 0.1 = within 10% of current depth (linZ)");

        changed |= widget.var("Normal Threshold", mNormalThreshold, 0.0f, 1.0f, 0.0001f);
        widget.tooltip("Maximum cosine of angle between normals. 1 = Exactly the same ; 0 = disabled");

        changed |= widget.var("Material Threshold", mMaterialThreshold, 0.0f, 1.0f, 0.0001f);
        widget.tooltip("Maximum absolute difference between the Diffuse probabilitys of the surfaces. 1 = Disabled ; 0 = Same surface");

        changed |= widget.var("Jacobian Min/Max", mJacobianMinMax, 0.0f, 1000.f, 0.01f);
        widget.tooltip("Min and Max values for the jacobian determinant. If smaller/higher the Reservoirs will not be combined");

        if (auto group2 = widget.group("Temporal Options"))
        {
            changed |= widget.var("Temporal age", mTemporalMaxAge, 0u, 512u);
            widget.tooltip("Temporal age a sample should have");
        }

        if (auto group2 = widget.group("Spartial Options"))
        {
            changed |= widget.var("Spartial Samples", mSpartialSamples, 0u, 8u);
            widget.tooltip("Number of spartial samples");

            changed |= widget.var("Disocclusion Sample Boost", mDisocclusionBoostSamples, 0u, 8u);
            widget.tooltip(
                "Number of spartial samples if no temporal surface was found. Needs to be bigger than \"Spartial Samples\" + 1 to have "
                "an effect"
            );

            changed |= widget.var("Sampling Radius", mSamplingRadius, 0.0f, 200.f);
            widget.tooltip("Radius for the Spartial Samples");
        }

        changed |= widget.var("Sample Attenuation Radius", mSampleRadiusAttenuation, 0.0f, 500.f, 0.001f);
        widget.tooltip(
            "The radius that is used for the non-linear sample attenuation(2/(d^2+r^2+d*sqrt(d^2+r^2))). At r=0 this leads to the "
            "normal 1/d^2"
        );

        mRebuildReservoirBuffer |= widget.checkbox("Use reduced Reservoir format", mUseReducedReservoirFormat);
        widget.tooltip(
            "If enabled uses RG32_UINT instead of RGBA32_UINT. In reduced format the targetPDF and M only have 16 bits while the "
            "weight still has full precision"
        );

        mClearReservoir = widget.button("Clear Reservoirs");
        widget.tooltip("Clears the reservoirs");
    }

    if (mpRTXDI)
    {
        if (auto group = widget.group("RTXDI"))
        {
            bool rtxdiChanged = mpRTXDI->renderUI(widget);
            if (rtxdiChanged)
                mRTXDIOptions = mpRTXDI->getOptions();
            changed |= rtxdiChanged;
        }
    }
}

void ReSTIR_GI_Shadow::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Reset Scene
    mpScene = pScene;

    mFinalGatherSamplePass = RayTraceProgramHelper::create();
    mTraceTransmissionDelta = RayTraceProgramHelper::create();
    mpFinalShadingPass.reset();
    mpResamplingPass.reset();
    mpEmissiveLightSampler.reset();
    mpRTXDI.reset();
    mpShadowMap.reset();
    mClearReservoir = true;

    if (mpScene)
    {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        mpShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);

        prepareRayTracingShaders(pRenderContext);
    }
}

bool ReSTIR_GI_Shadow::prepareLighting(RenderContext* pRenderContext)
{
    bool lightingChanged = false;
    // Make sure that the emissive light is up to date
    auto& pLights = mpScene->getLightCollection(pRenderContext);

    if (mpScene->useEmissiveLights())
    {
        // Init light sampler if not set
        if (!mpEmissiveLightSampler)
        {
            FALCOR_ASSERT(pLights && pLights->getActiveLightCount(pRenderContext) > 0);
            FALCOR_ASSERT(!mpEmissiveLightSampler);

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
        }
    }
    else
    {
        if (mpEmissiveLightSampler)
        {
            // Retain the options for the emissive sampler.
            if (auto lightBVHSampler = dynamic_cast<LightBVHSampler*>(mpEmissiveLightSampler.get()))
            {
                mLightBVHOptions = lightBVHSampler->getOptions();
            }

            mpEmissiveLightSampler = nullptr;
            lightingChanged = true;
        }
    }

    // Update Emissive light sampler
    if (mpEmissiveLightSampler)
    {
        lightingChanged |= mpEmissiveLightSampler->update(pRenderContext);
    }

    return lightingChanged;
}

void ReSTIR_GI_Shadow::prepareBuffers(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Reset screen space depentend buffers if the resolution has changed
    if ((mScreenRes.x != renderData.getDefaultTextureDims().x) || (mScreenRes.y != renderData.getDefaultTextureDims().y))
    {
        mScreenRes = renderData.getDefaultTextureDims();
        for (size_t i = 0; i < 2; i++)
        {
            mpReservoirBuffer[i].reset();
            mpPathSampelDataBuffer[i].reset();
            mpSurfaceBuffer[i].reset();
        }
        mpVBuffer.reset();
        mpViewDir.reset();
        mpViewDirPrev.reset();
        mpRayDist.reset();
        mpThp.reset();
    }

    // If reservoir format changed reset buffer
    if (mRebuildReservoirBuffer)
    {
        mpReservoirBuffer[0].reset();
        mpReservoirBuffer[1].reset();
        mRebuildReservoirBuffer = false;
    }

    // Per pixel Buffers/Textures
    for (uint i = 0; i < 2; i++)
    {
        if (!mpReservoirBuffer[i])
        {
            mpReservoirBuffer[i] = Texture::create2D(
                mpDevice, mScreenRes.x, mScreenRes.y, mUseReducedReservoirFormat ? ResourceFormat::RG32Uint : ResourceFormat::RGBA32Uint,
                1u, 1u, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mpReservoirBuffer[i]->setName("ReSTIR_GI_Shadow::Reservoir" + std::to_string(i));
        }

        if (!mpPathSampelDataBuffer[i])
        {
            mpPathSampelDataBuffer[i] = Buffer::createStructured(
                mpDevice, sizeof(uint) * 8, mScreenRes.x * mScreenRes.y,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false
            );
            mpPathSampelDataBuffer[i]->setName("ReSTIR_GI_Shadow::GISampleData" + std::to_string(i));
        }

        if (!mpSurfaceBuffer[i])
        {
            mpSurfaceBuffer[i] = Buffer::createStructured(
                mpDevice, sizeof(uint) * 6, mScreenRes.x * mScreenRes.y,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false
            );
            mpSurfaceBuffer[i]->setName("ReSTIR_GI_Shadow::SurfaceBuffer" + std::to_string(i));
        }
    }

    if (!mpVBuffer)
    {
        mpVBuffer = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, HitInfo::kDefaultFormat, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpVBuffer->setName("ReSTIR_GI_Shadow::VBufferWorkCopy");
    }

    if (!mpViewDir)
    {
        mpViewDir = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, kViewDirFormat, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpViewDir->setName("ReSTIR_GI_Shadow::ViewDir");
    }

    if (!mpViewDirPrev)
    {
        mpViewDirPrev = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, kViewDirFormat, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpViewDirPrev->setName("ReSTIR_GI_Shadow::ViewDirPrev");
    }

    if (!mpRayDist)
    {
        mpRayDist = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::R32Float, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpRayDist->setName("ReSTIR_GI_Shadow::RayDist");
    }

    if (!mpThp)
    {
        mpThp = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA16Float, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpThp->setName("ReSTIR_GI_Shadow::Throughput");
    }
}

void ReSTIR_GI_Shadow::prepareRayTracingShaders(RenderContext* pRenderContext)
{
    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();

    // TODO specify the payload bytes for each pass
    mFinalGatherSamplePass.initRTProgramWithShadowTest(mpDevice, mpScene, kPathSamplesShader, kMaxPayloadBytes, globalTypeConformances);
    mTraceTransmissionDelta.initRTProgram(mpDevice, mpScene, kTraceTransmissionDeltaShader, kMaxPayloadBytes, globalTypeConformances);
}

void ReSTIR_GI_Shadow::traceTransmissiveDelta(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "TraceDeltaTransmissive");

    mTraceTransmissionDelta.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    if (!mTraceTransmissionDelta.pVars)
        mTraceTransmissionDelta.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mTraceTransmissionDelta.pVars);

    auto var = mTraceTransmissionDelta.pVars->getRootVar();

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gMaxBounces"] = mTraceMaxBounces;
    var[nameBuf]["gRequDiffParts"] = mTraceRequireDiffuseMat;
    var[nameBuf]["gAlphaTest"] = true;  //TODO add variable

    var["gInVBuffer"] = renderData[kInputVBuffer]->asTexture();

    var["gOutThp"] = mpThp;
    var["gOutViewDir"] = mpViewDir;
    var["gOutRayDist"] = mpRayDist;
    var["gOutVBuffer"] = mpVBuffer;
    if (renderData[kOutputDiffuseReflectance])
        var["gOutDiffuseReflectance"] = renderData[kOutputDiffuseReflectance]->asTexture();
    if (renderData[kOutputSpecularReflectance])
        var["gOutSpecularReflectance"] = renderData[kOutputSpecularReflectance]->asTexture();

    // Create dimensions based on the number of VPLs
    FALCOR_ASSERT(mScreenRes.x > 0 && mScreenRes.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mTraceTransmissionDelta.pProgram.get(), mTraceTransmissionDelta.pVars, uint3(mScreenRes, 1));
}

void ReSTIR_GI_Shadow::generatePathSamplesPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "FinalGatherSample");

    mFinalGatherSamplePass.pProgram->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("USE_RTXDI", mpRTXDI ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_USE_NEE", mGINEE ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_USE_ANALYTIC", mpScene && mpScene->useAnalyticLights() ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_USE_EMISSIVE", mpScene && mpScene->useEmissiveLights() ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_ALPHA_TEST", mAlphaTest ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_MIS", mGIMIS ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_RUSSIAN_ROULETTE", mGIRussianRoulette ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("GI_USE_SHADOW_MAP", mUseShadowMap ? "1" : "0");

    if (mpRTXDI)
        mFinalGatherSamplePass.pProgram->addDefines(mpRTXDI->getDefines());
    if (mpShadowMap)
        mFinalGatherSamplePass.pProgram->addDefines(mpShadowMap->getDefines());
    if (mpEmissiveLightSampler)
        mFinalGatherSamplePass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

    if (!mFinalGatherSamplePass.pVars)
        mFinalGatherSamplePass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mFinalGatherSamplePass.pVars);

    auto var = mFinalGatherSamplePass.pVars->getRootVar();

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gAttenuationRadius"] = mSampleRadiusAttenuation;
    var[nameBuf]["gBounces"] = mGIMaxBounces;

    if (mpRTXDI)
        mpRTXDI->setShaderData(var);

    if (mpShadowMap)
        mpShadowMap->setShaderDataAndBindBlock(var, renderData.getDefaultTextureDims());

    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["gEmissiveSampler"]);

    var["gVBuffer"] = mpVBuffer;
    var["gView"] = mpViewDir;
    var["gLinZ"] = mpRayDist;

    var["gReservoir"] = mpReservoirBuffer[mFrameCount % 2];
    var["gSurfaceData"] = mpSurfaceBuffer[mFrameCount % 2];
    var["gGISample"] = mpPathSampelDataBuffer[mFrameCount % 2];

    FALCOR_ASSERT(mScreenRes.x > 0 && mScreenRes.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mFinalGatherSamplePass.pProgram.get(), mFinalGatherSamplePass.pVars, uint3(mScreenRes, 1));
}

void ReSTIR_GI_Shadow::resamplingPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    std::string profileName = "SpatiotemporalResampling";
    if (mResamplingMode == ResamplingMode::Temporal)
        profileName = "TemporalResampling";
    else if (mResamplingMode == ResamplingMode::Spartial)
        profileName = "SpatialResampling";

    FALCOR_PROFILE(pRenderContext, profileName);

    if (!mpResamplingPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kResamplingPassShader).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
        defines.add("MODE_SPATIOTEMPORAL", mResamplingMode == ResamplingMode::SpartioTemporal ? "1" : "0");
        defines.add("MODE_TEMPORAL", mResamplingMode == ResamplingMode::Temporal ? "1" : "0");
        defines.add("MODE_SPATIAL", mResamplingMode == ResamplingMode::Spartial ? "1" : "0");
        defines.add("BIAS_CORRECTION_MODE", std::to_string((uint)mBiasCorrectionMode));

        mpResamplingPass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpResamplingPass);

    // If defines change, refresh the program
    mpResamplingPass->getProgram()->addDefine("MODE_SPATIOTEMPORAL", mResamplingMode == ResamplingMode::SpartioTemporal ? "1" : "0");
    mpResamplingPass->getProgram()->addDefine("MODE_TEMPORAL", mResamplingMode == ResamplingMode::Temporal ? "1" : "0");
    mpResamplingPass->getProgram()->addDefine("MODE_SPATIAL", mResamplingMode == ResamplingMode::Spartial ? "1" : "0");
    mpResamplingPass->getProgram()->addDefine("BIAS_CORRECTION_MODE", std::to_string((uint)mBiasCorrectionMode));
    mpResamplingPass->getProgram()->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");

    // Set variables
    auto var = mpResamplingPass->getRootVar();

    mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
    mpSampleGenerator->setShaderData(var);                    // Sample generator

    // Bind Reservoir and surfaces
    uint idxCurr = mFrameCount % 2;
    uint idxPrev = (mFrameCount + 1) % 2;

    var["gSurface"] = mpSurfaceBuffer[idxCurr];
    var["gSurfacePrev"] = mpSurfaceBuffer[idxPrev];

    // Swap the reservoir and sample indices for spatial resampling
    if (mResamplingMode == ResamplingMode::Spartial)
        std::swap(idxCurr, idxPrev);

    var["gReservoir"] = mpReservoirBuffer[idxCurr];
    var["gReservoirPrev"] = mpReservoirBuffer[idxPrev];
    var["gGIPathData"] = mpPathSampelDataBuffer[idxCurr];
    var["gGIPathDataPrev"] = mpPathSampelDataBuffer[idxPrev];

    // View
    var["gView"] = mpViewDir;
    var["gPrevView"] = mpViewDirPrev;
    var["gMVec"] = renderData[kInputMotionVectors]->asTexture();

    std::string uniformName = "PerFrame";
    var[uniformName]["gFrameCount"] = mFrameCount;

    uniformName = "Constant";
    var[uniformName]["gFrameDim"] = renderData.getDefaultTextureDims();
    var[uniformName]["gMaxAge"] = mTemporalMaxAge;
    var[uniformName]["gSpatialSamples"] = mSpartialSamples;
    var[uniformName]["gSamplingRadius"] = mSamplingRadius;
    var[uniformName]["gDepthThreshold"] = mRelativeDepthThreshold;
    var[uniformName]["gNormalThreshold"] = mNormalThreshold;
    var[uniformName]["gMatThreshold"] = mMaterialThreshold;
    var[uniformName]["gDisocclusionBoostSamples"] = mDisocclusionBoostSamples;
    var[uniformName]["gAttenuationRadius"] = mSampleRadiusAttenuation;
    var[uniformName]["gJacobianMinMax"] = mJacobianMinMax;

    // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpResamplingPass->execute(pRenderContext, uint3(targetDim, 1));

    // Barrier for written buffer
    pRenderContext->uavBarrier(mpReservoirBuffer[idxCurr].get());
    pRenderContext->uavBarrier(mpPathSampelDataBuffer[idxCurr].get());
}

void ReSTIR_GI_Shadow::finalShadingPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "FinalShading");

    // Create pass
    if (!mpFinalShadingPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kFinalShadingPassShader).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getValidResourceDefines(kOutputChannels, renderData));
        defines.add(getValidResourceDefines(kInputChannels, renderData));
        defines.add("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
        defines.add("USE_ENV_BACKROUND", mpScene->useEnvBackground() ? "1" : "0");
        if (mpRTXDI)
            defines.add(mpRTXDI->getDefines());
        defines.add("USE_RTXDI", mpRTXDI ? "1" : "0");

        mpFinalShadingPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpFinalShadingPass);

    if (mpRTXDI)
        mpFinalShadingPass->getProgram()->addDefines(mpRTXDI->getDefines()); // TODO only set once?
    mpFinalShadingPass->getProgram()->addDefine("USE_RTXDI", mpRTXDI ? "1" : "0");
    mpFinalShadingPass->getProgram()->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
    mpFinalShadingPass->getProgram()->addDefine("USE_ENV_BACKROUND", mpScene->useEnvBackground() ? "1" : "0");
    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    mpFinalShadingPass->getProgram()->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Set variables
    auto var = mpFinalShadingPass->getRootVar();

    mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
    mpSampleGenerator->setShaderData(var);                    // Sample generator

    if (mpRTXDI)
        mpRTXDI->setShaderData(var);

    uint reservoirIndex = mResamplingMode == ResamplingMode::Spartial ? (mFrameCount + 1) % 2 : mFrameCount % 2;

    var["gReservoir"] = mpReservoirBuffer[reservoirIndex];
    var["gGISampleData"] = mpPathSampelDataBuffer[reservoirIndex];

    var["gThp"] = mpThp;
    var["gView"] = mpViewDir;
    var["gVBuffer"] = mpVBuffer;
    var["gMVec"] = renderData[kInputMotionVectors]->asTexture();

    // Bind all Output Channels
    for (uint i = 0; i < kOutputChannels.size(); i++)
    {
        if (renderData[kOutputChannels[i].name])
            var[kOutputChannels[i].texname] = renderData[kOutputChannels[i].name]->asTexture();
    }

    // Uniform
    std::string uniformName = "PerFrame";
    var[uniformName]["gFrameCount"] = mFrameCount;
    var[uniformName]["gAttenuationRadius"] = mSampleRadiusAttenuation; // Attenuation radius
    var[uniformName]["gFrameDim"] = renderData.getDefaultTextureDims();
    // var[uniformName]["gEnableCaustics"] = mEnableCausticPhotonCollection;

    // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpFinalShadingPass->execute(pRenderContext, uint3(targetDim, 1));
}

void ReSTIR_GI_Shadow::copyViewTexture(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (mpViewDir != nullptr)
    {
        pRenderContext->copyResource(mpViewDirPrev.get(), mpViewDir.get());
    }
}

void ReSTIR_GI_Shadow::computeQuadTexSize(uint maxItems, uint& outWidth, uint& outHeight)
{
    // Compute the size of a power-of-2 rectangle that fits all items, 1 item per pixel
    double textureWidth = std::max(1.0, ceil(sqrt(double(maxItems))));
    textureWidth = exp2(ceil(log2(textureWidth)));
    double textureHeight = std::max(1.0, ceil(maxItems / textureWidth));
    textureHeight = exp2(ceil(log2(textureHeight)));

    outWidth = uint(textureWidth);
    outHeight = uint(textureHeight);
}

void ReSTIR_GI_Shadow::RayTraceProgramHelper::initRTProgram(
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
    desc.setMaxTraceRecursionDepth(1);
    if (!scene->hasProceduralGeometry())
        desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

    pBindingTable = RtBindingTable::create(1, 1, scene->getGeometryCount());
    auto& sbt = pBindingTable;
    sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));
    sbt->setMiss(0, desc.addMiss("miss"));

    // TODO: Support more geometry types and more material conformances
    if (scene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(0, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
    }

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void ReSTIR_GI_Shadow::RayTraceProgramHelper::initRTProgramWithShadowTest(
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
    desc.setMaxTraceRecursionDepth(1);
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
        sbt->setHitGroup(0, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        sbt->setHitGroup(1, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowAnyHit"));
    }

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void ReSTIR_GI_Shadow::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
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
