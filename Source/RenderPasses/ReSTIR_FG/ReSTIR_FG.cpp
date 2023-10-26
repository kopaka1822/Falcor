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
#include "ReSTIR_FG.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

namespace
{
    const std::string kTraceTransmissionDeltaShader = "RenderPasses/ReSTIR_FG/Shader/TraceTransmissionDelta.rt.slang";
    const std::string kFinalGatherSamplesShader = "RenderPasses/ReSTIR_FG/Shader/GenerateFinalGatherSamples.rt.slang";
    const std::string kGeneratePhotonsShader = "RenderPasses/ReSTIR_FG/Shader/GeneratePhotons.rt.slang";
    const std::string kCollectPhotonsFGShader = "RenderPasses/ReSTIR_FG/Shader/CollectPhotonsFinalGather.rt.slang";
    const std::string kCollectPhotonsShader = "RenderPasses/ReSTIR_FG/Shader/CollectPhotons.rt.slang";
    const std::string kResamplingPassShader = "RenderPasses/ReSTIR_FG/Shader/ResamplingPass.cs.slang";
    const std::string kFinalShadingPassShader = "RenderPasses/ReSTIR_FG/Shader/FinalShading.cs.slang";
    const std::string kDirectAnalyticPassShader = "RenderPasses/ReSTIR_FG/Shader/DirectAnalytic.cs.slang";

    const std::string kShaderModel = "6_5";
    const uint kMaxPayloadBytes = 96u;

    //Render Pass inputs and outputs
    const std::string kInputVBuffer = "vbuffer";
    const std::string kInputMotionVectors = "mvec";
    //const std::string kInputViewDir = "viewW";
    //const std::string kInputRayDistance = "rayDist";

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
    const std::string kOutputResidualRadiance = "residualRadiance";     //The rest (transmission, delta)

    const Falcor::ChannelList kOutputChannels{
        {kOutputColor,                  "gOutColor",                "Output Color (linear)", true /*optional*/, ResourceFormat::RGBA32Float},
        {kOutputEmission,               "gOutEmission",             "Output Emission", true /*optional*/, ResourceFormat::RGBA32Float},
        {kOutputDiffuseRadiance,        "gOutDiffuseRadiance",      "Output demodulated diffuse color (linear)", true /*optional*/, ResourceFormat::RGBA32Float},
        {kOutputSpecularRadiance,       "gOutSpecularRadiance",     "Output demodulated specular color (linear)", true /*optional*/, ResourceFormat::RGBA32Float},
        {kOutputDiffuseReflectance,     "gOutDiffuseReflectance",   "Output primary surface diffuse reflectance", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputSpecularReflectance,    "gOutSpecularReflectance",  "Output primary surface specular reflectance", true /*optional*/, ResourceFormat::RGBA16Float},
        {kOutputResidualRadiance,       "gOutResidualRadiance",     "Output residual color (transmission/delta)", true /*optional*/, ResourceFormat::RGBA32Float},
    };

    const Gui::DropdownList kResamplingModeList{
        {(uint)ReSTIR_FG::ResamplingMode::Temporal, "Temporal"},
        {(uint)ReSTIR_FG::ResamplingMode::Spartial, "Spartial"},
        {(uint)ReSTIR_FG::ResamplingMode::SpartioTemporal, "SpartioTemporal"},
    };

    const Gui::DropdownList kBiasCorrectionModeList{
        {(uint)ReSTIR_FG::BiasCorrectionMode::Off, "Off"},
        {(uint)ReSTIR_FG::BiasCorrectionMode::Basic, "Basic"},
        {(uint)ReSTIR_FG::BiasCorrectionMode::RayTraced, "RayTraced"},
    };

    const Gui::DropdownList kRenderModeList{
        {(uint)ReSTIR_FG::RenderMode::FinalGather, "Final Gather"},
        {(uint)ReSTIR_FG::RenderMode::ReSTIRFG, "ReSTIR_FG"},
    };

    const Gui::DropdownList kDirectLightRenderModeList{
        {(uint)ReSTIR_FG::DirectLightingMode::None, "None"},
        {(uint)ReSTIR_FG::DirectLightingMode::RTXDI, "RTXDI"},
        {(uint)ReSTIR_FG::DirectLightingMode::AnalyticDirect, "AnalyticDirect"}

    };

    const Gui::DropdownList kCausticCollectionModeList{
        {(uint)ReSTIR_FG::CausticCollectionMode::All, "All"},
        {(uint)ReSTIR_FG::CausticCollectionMode::None, "None"},
        {(uint)ReSTIR_FG::CausticCollectionMode::GreaterOne, "GreaterOne"},
        {(uint)ReSTIR_FG::CausticCollectionMode::Temporal, "Temporal"}
    };
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ReSTIR_FG>();
}

ReSTIR_FG::ReSTIR_FG(ref<Device> pDevice, const Properties& props)
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

    //TODO Handle Properties

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, mpSampleGenerator);
}

Properties ReSTIR_FG::getProperties() const
{
    return Properties();
}

RenderPassReflection ReSTIR_FG::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ReSTIR_FG::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if(!mpScene)    //Return on empty scene
        return;

    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mSPPMFramesCameraStill = 0;
        mOptionsChanged = false;
    }

    const auto& pMotionVectors = renderData[kInputMotionVectors]->asTexture();

    //Init RTXDI if it is enabled
    if (mDirectLightMode == DirectLightingMode::RTXDI && !mpRTXDI)
    {
        mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);
    }
    //Delete RTXDI if it is set and the mode changed
    if (mDirectLightMode != DirectLightingMode::RTXDI && mpRTXDI)
        mpRTXDI = nullptr;

    //Prepare used Datas and Buffers
    prepareLighting(pRenderContext);

    prepareBuffers(pRenderContext, renderData);

    prepareAccelerationStructure();

    //Clear the reservoir
    if (mClearReservoir)
    {
        for (uint i = 0; i < 2; i++)
            pRenderContext->clearUAV(mpReservoirBuffer[i]->getUAV().get(), uint4(0));
        mClearReservoir = false;
    }

    if (mpRTXDI) mpRTXDI->beginFrame(pRenderContext, mScreenRes);

    //RenderPasses
    traceTransmissiveDelta(pRenderContext, renderData);

    getFinalGatherHitPass(pRenderContext, renderData);

    generatePhotonsPass(pRenderContext, renderData);
    if (mMixedLights)
        generatePhotonsPass(pRenderContext, renderData, true);  //Secound pass. Always Analytic

    //Direct light resampling
    if (mpRTXDI) mpRTXDI->update(pRenderContext, pMotionVectors, mpViewDir, mpViewDirPrev);

    collectPhotons(pRenderContext, renderData);

    //Do resampling
    if (mReservoirValid && (mRenderMode == RenderMode::ReSTIRFG))
        resamplingPass(pRenderContext, renderData);

    if ((mRenderMode == RenderMode::ReSTIRFG) || mDirectLightMode == DirectLightingMode::RTXDI)
    {
        finalShadingPass(pRenderContext, renderData);

        copyViewTexture(pRenderContext, renderData);
    }

    if (mpRTXDI) mpRTXDI->endFrame(pRenderContext);

    //Add Direct Light at the end if this mode is enabled
    if (mDirectLightMode == DirectLightingMode::AnalyticDirect)
        directAnalytic(pRenderContext, renderData);

    //SPPM
    if (mUseSPPM)
    {
        if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::CameraMoved) || mSPPMFramesCameraStill == 0)
        {
            mSPPMFramesCameraStill = 0;
            mPhotonCollectRadius = mPhotonCollectionRadiusStart;
        }

        float itF = static_cast<float>(mSPPMFramesCameraStill);
        mPhotonCollectRadius *= sqrt((itF + mSPPMAlpha) / (itF + 1.0f));

        mSPPMFramesCameraStill++;
    }

    mReservoirValid = true;
    mFrameCount++;
}

void ReSTIR_FG::renderUI(Gui::Widgets& widget)
{
    bool changed = false;

    widget.dropdown("Direct Light Mode", kDirectLightRenderModeList, (uint&)mDirectLightMode);
    widget.tooltip(
        "None: Direct Light is not calculated \n RTXDI: Optimized ReSTIR is used for direct light \n AnalyticDirect: All analytic lights "
        "are directly evaluated (Emissive light is ignored)"
    );

    widget.dropdown("(Indirect) Render Mode", kRenderModeList, (uint&)mRenderMode);

    if (auto group = widget.group("Specular Trace Options"))
    {
        group.var("Specular/Transmissive Bounces", mTraceMaxBounces, 0u, 32u, 1u);
        group.tooltip("Number of specular/transmissive bounces. 0 -> direct hit only");
        group.checkbox("Require Diffuse Part", mTraceRequireDiffuseMat);
        group.tooltip("Requires a diffuse part in addition to delta lobes");
        if (mTraceRequireDiffuseMat)
        {
            group.var("Roughness Cutoff", mTraceRoughnessCutoff, 0.0f, 1.0f, 0.01f);
            group.tooltip("Materials with roughness over this threshold are still considered diffuse");
        }
    }

    if (auto group = widget.group("PhotonMapper")) {
        if (mMixedLights)
        {
            changed |= group.var("Mixed Analytic Ratio", mPhotonAnalyticRatio, 0.f, 1.f , 0.01f);
            group.tooltip("Analytic photon distribution ratio in a mixed light case. E.g. 0.3 -> 30% analytic, 70% emissive");
        }
        changed |= group.checkbox("Enable dynamic photon dispatch", mUseDynamicePhotonDispatchCount);
        group.tooltip("Changed the number of dispatched photons dynamically. Tries to fill the photon buffer");
        if (mUseDynamicePhotonDispatchCount)
        {
            changed |= group.var("Max dispatched", mPhotonDynamicDispatchMax, mPhotonYExtent, 4000000u);
            group.tooltip("Maximum number the dispatch can be increased to");
            changed |= group.var("Guard Percentage", mPhotonDynamicGuardPercentage, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "If current fill rate is under PhotonBufferSize * (1-pGuard), the values are accepted. Reduces the changes every frame"
            );
            changed |= group.var("Percentage Change", mPhotonDynamicChangePercentage, 0.01f, 10.f, 0.01f);
            group.tooltip(
                "Increase/Decrease percentage from the Buffer Size. With current value a increase/decrease of :" +
                std::to_string(mPhotonDynamicChangePercentage * mNumMaxPhotons[0]) + "is expected"
            );
            group.text("Dispatched Photons: " + std::to_string(mNumDispatchedPhotons));
        }
        else
        {
            uint dispatchedPhotons = mNumDispatchedPhotons;
            bool disPhotonChanged = group.var("Dispatched Photons", dispatchedPhotons, mPhotonYExtent, 9984000u, (float)mPhotonYExtent);
            if (disPhotonChanged)
                mNumDispatchedPhotons = (uint)(dispatchedPhotons / mPhotonYExtent) * mPhotonYExtent;
        }

         // Buffer size
        group.text("Photons: " + std::to_string(mCurrentPhotonCount[0]) + " / " + std::to_string(mNumMaxPhotons[0]));
        group.text("Caustic photons: " + std::to_string(mCurrentPhotonCount[1]) + " / " + std::to_string(mNumMaxPhotons[1]));
        group.var("Photon Buffer Size", mNumMaxPhotonsUI, 100u, 100000000u, 100);
        group.tooltip("First -> Global, Second -> Caustic");
        mChangePhotonLightBufferSize = group.button("Apply", true);

        changed |= group.var("Light Store Probability", mPhotonRejection, 0.f, 1.f, 0.0001f);
        group.tooltip("Probability a photon light is stored on diffuse hit. Flux is scaled up appropriately");

        changed |= group.var("Max Bounces", mPhotonMaxBounces, 0u, 32u);
        changed |= group.var("Max Caustic Bounces", mMaxCausticBounces, 0u, 32u);
        group.tooltip("Maximum number of diffuse bounces that are allowed for a caustic photon.");

        bool radiusChanged = group.var("Collection Radius", mPhotonCollectionRadiusStart, 0.00001f, 1000.f, 0.00001f, false);
        mPhotonCollectionRadiusStart.y = std::min(mPhotonCollectionRadiusStart.y, mPhotonCollectionRadiusStart.x);
        group.tooltip("Photon Radii for final gather and caustic collecton. First->Global, Second->Caustic");
        if (radiusChanged)
            mPhotonCollectRadius = mPhotonCollectionRadiusStart;


        changed |= group.checkbox("Enable SPPM", mUseSPPM);
        group.tooltip("Stochastic Progressive Photon Mapping. Radius is reduced by a fixed sequence every frame. It is advised to use SPPM only for Offline Rendering");
        if (mUseSPPM)
        {
            group.var("SPPM Alpha", mSPPMAlpha, 0.001f, 1.f, 0.001f);
            group.text(
                "Current Radius: Global = " + std::to_string(mPhotonCollectRadius.x) +
                "; Caustic = " + std::to_string(mPhotonCollectRadius.y)
            );
            changed |= group.button("Reset", true);
        }
            

        if (auto causticGroup = group.group("Caustic Settings", true))
        {
            changed |= causticGroup.checkbox("Caustic from Delta lobes only", mGenerationDeltaRejection);
            causticGroup.tooltip("Only stores ");
            if (mGenerationDeltaRejection)
            {
                causticGroup.checkbox("Delta Lobes require diffuse parts", mGenerationDeltaRejectionRequireDiffPart);
                causticGroup.tooltip("Caustics requires a nonzero diffuse part for diffuse surfaces");
            }

            changed |= causticGroup.dropdown("Caustic Collection Mode", kCausticCollectionModeList, (uint32_t&)mCausticCollectMode);
            causticGroup.tooltip(
                "Collection Mode for Caustics \n All: Normal Collection \n GreaterOne: Radiance is only written if count>1. Adds Bias \n "
                "Mask: WIP"
            );

            if (mCausticCollectMode == CausticCollectionMode::Temporal)
            {
                changed |= causticGroup.var("Caustic Temporal History Limit", mCausticTemporalFilterHistoryLimit, 1u, 512u, 1u);
                causticGroup.tooltip("History Limit for the Temporal Caustic Filter");
            }
        }

        changed |= group.checkbox("Use Photon Culling", mUsePhotonCulling);
        group.tooltip("Enabled culling of photon based on a hash grid. Photons are only stored on cells that are collected");
        if (mUsePhotonCulling)
        {
            if (auto groupCulling = group.group("CullingSettings", true))
            {
                groupCulling.checkbox("Use Caustic Culling", mUseCausticCulling);
                groupCulling.tooltip("Enables Photon Culling for Caustic Photons");
                groupCulling.checkbox("Use fixed Culling Cell radius", mCullingUseFixedRadius);
                groupCulling.tooltip("Use a fixed radius for the culling cell. Only used from the point where [Global Radius < Hash Radius]"
                );
                if (mCullingUseFixedRadius)
                    changed |= groupCulling.var("Hash Cell Radius", mCullingCellRadius, 0.0001f, 10000.f, 0.0001f);
                bool rebuildBuffer = groupCulling.var("Culling Size Bytes", mCullingHashBufferSizeBits, 10u, 27u);
                groupCulling.tooltip("Size of the culling buffer (2^x) and effective hash bytes used");

                if (rebuildBuffer)
                    mpPhotonCullingMask.reset();
                changed |= rebuildBuffer;
            }
        }
            
    }

    if (mRenderMode == RenderMode::ReSTIRFG)
    {
        if (auto group = widget.group("ReSTIR_FG"))
        {
            changed |= group.dropdown("ResamplingMode", kResamplingModeList, (uint&)mResamplingMode);

            changed |= group.dropdown("BiasCorrection", kBiasCorrectionModeList, (uint&)mBiasCorrectionMode);

            changed |= group.var("Depth Threshold", mRelativeDepthThreshold, 0.0f, 1.0f, 0.0001f);
            group.tooltip("Relative depth threshold. 0.1 = within 10% of current depth (linZ)");

            changed |= group.var("Normal Threshold", mNormalThreshold, 0.0f, 1.0f, 0.0001f);
            group.tooltip("Maximum cosine of angle between normals. 1 = Exactly the same ; 0 = disabled");

            changed |= group.var("Material Threshold", mMaterialThreshold, 0.0f, 1.0f, 0.0001f);
            group.tooltip("Maximum absolute difference between the Diffuse probabilitys of the surfaces. 1 = Disabled ; 0 = Same surface");

            changed |= group.var("Jacobian Min/Max", mJacobianMinMax, 0.0f, 1000.f, 0.01f);
            group.tooltip("Min and Max values for the jacobian determinant. If smaller/higher the Reservoirs will not be combined");

            if (auto group2 = group.group("Temporal Options"))
            {
                changed |= group2.var("Temporal age", mTemporalMaxAge, 0u, 512u);
                group2.tooltip("Temporal age a sample should have");
            }

            if (auto group2 = group.group("Spartial Options"))
            {
                changed |= group2.var("Spartial Samples", mSpartialSamples, 0u, 8u);
                group2.tooltip("Number of spartial samples");

                changed |= group2.var("Disocclusion Sample Boost", mDisocclusionBoostSamples, 0u, 8u);
                group2.tooltip(
                    "Number of spartial samples if no temporal surface was found. Needs to be bigger than \"Spartial Samples\" + 1 to have "
                    "an effect"
                );

                changed |= group2.var("Sampling Radius", mSamplingRadius, 0.0f, 200.f);
                group2.tooltip("Radius for the Spartial Samples");
            }

            changed |= group.var("Sample Attenuation Radius", mSampleRadiusAttenuation, 0.0f, 500.f, 0.001f);
            group.tooltip(
                "The radius that is used for the non-linear sample attenuation(2/(d^2+r^2+d*sqrt(d^2+r^2))). At r=0 this leads to the "
                "normal 1/d^2"
            );

            mRebuildReservoirBuffer |= group.checkbox("Use reduced Reservoir format", mUseReducedReservoirFormat);
            group.tooltip(
                "If enabled uses RG32_UINT instead of RGBA32_UINT. In reduced format the targetPDF and M only have 16 bits while the "
                "weight still has full precision"
            );

            mClearReservoir = group.button("Clear Reservoirs");
            group.tooltip("Clears the reservoirs");
        }
    }

    if (mpRTXDI)
    {
        if (auto group = widget.group("RTXDI"))
        {
            bool rtxdiChanged = mpRTXDI->renderUI(group);
            if (rtxdiChanged)
                mRTXDIOptions = mpRTXDI->getOptions();
            changed |= rtxdiChanged;
        }
    }

    mOptionsChanged |= changed;
}

void ReSTIR_FG::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    //Reset Scene
    mpScene = pScene;

    mFinalGatherSamplePass = RayTraceProgramHelper::create();
    mGeneratePhotonPass = RayTraceProgramHelper::create();
    mCollectPhotonPass = RayTraceProgramHelper::create();
    mTraceTransmissionDelta = RayTraceProgramHelper::create();
    mpFinalShadingPass.reset();
    mpResamplingPass.reset();
    mpEmissiveLightSampler.reset();
    mpRTXDI.reset();
    mClearReservoir = true;

    if (mpScene)
    {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        prepareRayTracingShaders(pRenderContext);
    }
}

bool ReSTIR_FG::prepareLighting(RenderContext* pRenderContext)
{
    bool lightingChanged = false;
    // Make sure that the emissive light is up to date
    auto& pLights = mpScene->getLightCollection(pRenderContext);

    bool emissiveUsed = mpScene->useEmissiveLights();
    bool analyticUsed = mpScene->useAnalyticLights();

    mMixedLights = emissiveUsed && analyticUsed;

    if (mpScene->useEmissiveLights())
    {
        // Init light sampler if not set
        if (!mpEmissiveLightSampler)
        {
            // Ensure that emissive light struct is build by falcor
            FALCOR_ASSERT(pLights && pLights->getActiveLightCount(pRenderContext) > 0);
            // TODO: Support different types of sampler
            mpEmissiveLightSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, mpScene);
            lightingChanged = true;
        }
    }
    else
    {
        if (mpEmissiveLightSampler)
        {
            mpEmissiveLightSampler = nullptr;
            lightingChanged = true;
            mGeneratePhotonPass.pVars.reset();
        }
    }

    // Update Emissive light sampler
    if (mpEmissiveLightSampler)
    {
        lightingChanged |= mpEmissiveLightSampler->update(pRenderContext);
    }

    return lightingChanged;
}

void ReSTIR_FG::prepareBuffers(RenderContext* pRenderContext, const RenderData& renderData) {

    //Reset screen space depentend buffers if the resolution has changed
    if ((mScreenRes.x != renderData.getDefaultTextureDims().x) || (mScreenRes.y != renderData.getDefaultTextureDims().y))
    {
        mScreenRes = renderData.getDefaultTextureDims();
        for (size_t i = 0; i < 2; i++)
        {
            mpReservoirBuffer[i].reset();
            mpFGSampelDataBuffer[i].reset();
            mpSurfaceBuffer[i].reset();
            mpCausticRadiance[i].reset();
        }
        mpFinalGatherSampleHitData.reset();
        mpVBuffer.reset();
        mpViewDir.reset();
        mpViewDirPrev.reset();
        mpRayDist.reset();
        mpThp.reset();
    }

    //If reservoir format changed reset buffer
    if (mRebuildReservoirBuffer){
        mpReservoirBuffer[0].reset();
        mpReservoirBuffer[1].reset();
        mRebuildReservoirBuffer = false;
    }

    if (mChangePhotonLightBufferSize)
    {
        mNumMaxPhotons = mNumMaxPhotonsUI;
        for (uint i = 0; i < 2; i++)
        {
            mpPhotonAABB[i].reset();
            mpPhotonData[i].reset();
        }
    }


    //Per pixel Buffers/Textures
    for (uint i = 0; i < 2; i++){
        if (!mpReservoirBuffer[i]){
            mpReservoirBuffer[i] = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, mUseReducedReservoirFormat ? ResourceFormat::RG32Uint : ResourceFormat::RGBA32Uint, 
                                                     1u, 1u, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mpReservoirBuffer[i]->setName("ReSTIR_FG::Reservoir" + std::to_string(i));
        }

        if (!mpFGSampelDataBuffer[i])
        {
            mpFGSampelDataBuffer[i] = Buffer::createStructured(mpDevice, sizeof(uint) * 8, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                                                               Buffer::CpuAccess::None, nullptr, false );
            mpFGSampelDataBuffer[i]->setName("ReSTIR_FG::FGSampleData" + std::to_string(i));
        }

        if (!mpSurfaceBuffer[i]){
            mpSurfaceBuffer[i] = Buffer::createStructured(mpDevice, sizeof(uint) * 6, mScreenRes.x * mScreenRes.y,ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                                                          Buffer::CpuAccess::None, nullptr, false );
            mpSurfaceBuffer[i]->setName("ReSTIR_FG::SurfaceBuffer" + std::to_string(i));
        }
    }

    if (!mpFinalGatherSampleHitData)
    {
        mpFinalGatherSampleHitData = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, HitInfo::kDefaultFormat, 1u, 1u, nullptr,
                                                       ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpFinalGatherSampleHitData->setName("ReSTIR_FG::FGHitData");
    }

    if (!mpCausticRadiance[0])
    {
        mpCausticRadiance[0] = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA16Float, 1u, 1u, nullptr,
                                              ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpCausticRadiance[0]->setName("ReSTIR_FG::CausticRadiance");
    }

    if (mCausticCollectMode == CausticCollectionMode::Temporal && !mpCausticRadiance[1])
    {
        mpCausticRadiance[1] = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA16Float, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpCausticRadiance[1]->setName("ReSTIR_FG::CausticRadianceTemporal");
    }

    if (mpCausticRadiance[1] && (mCausticCollectMode != CausticCollectionMode::Temporal))
        mpCausticRadiance[1].reset();

    if (!mpVBuffer)
    {
        mpVBuffer = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, HitInfo::kDefaultFormat, 1u, 1u, nullptr,
                                      ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess );
        mpVBuffer->setName("ReSTIR_FG::VBufferWorkCopy");
    }

    if (!mpViewDir)
    {
        mpViewDir = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, kViewDirFormat, 1u, 1u, nullptr,
                                      ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpViewDir->setName("ReSTIR_FG::ViewDir");
    }

    if (!mpViewDirPrev)
    {
        mpViewDirPrev = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, kViewDirFormat, 1u, 1u, nullptr,
                                          ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpViewDirPrev->setName("ReSTIR_FG::ViewDirPrev");
    }

    if (!mpRayDist)
    {
        mpRayDist = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::R32Float, 1u, 1u, nullptr,
                                      ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpRayDist->setName("ReSTIR_FG::RayDist");
    }

    if (!mpThp)
    {
        mpThp = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA16Float, 1u, 1u, nullptr,
                                  ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpThp->setName("ReSTIR_FG::Throughput");
    }

    //Photon
    if (!mpPhotonCounter[0])
    {
        for (uint i = 0; i < kPhotonCounterCount; i++)
        {
            mpPhotonCounter[i] = Buffer::create(mpDevice, sizeof(uint) * 2);
            mpPhotonCounter[i]->setName("ReSTIR_FG::PhotonCounterGPU" + std::to_string(i));
        }
        
    }
    if (!mpPhotonCounterCPU[0])
    {
        for (uint i = 0; i < kPhotonCounterCount; i++)
        {
            mpPhotonCounterCPU[i] = Buffer::create(mpDevice, sizeof(uint) * 2, ResourceBindFlags::None, Buffer::CpuAccess::Read);
            mpPhotonCounterCPU[i]->setName("ReSTIR_FG::PhotonCounterCPU" + std::to_string(i));
        }
    }
    for (uint i = 0; i < 2; i++)
    {
        if (!mpPhotonAABB[i]) {
            mpPhotonAABB[i] = Buffer::createStructured(mpDevice, sizeof(AABB), mNumMaxPhotons[i]);
            mpPhotonAABB[i]->setName("ReSTIR_FG::PhotonAABB" + (i + 1));
        }
        if (!mpPhotonData[i]) {
            mpPhotonData[i] = Buffer::createStructured(mpDevice, sizeof(uint) * 4, mNumMaxPhotons[i]);
            mpPhotonData[i]->setName("ReSTIR_FG::PhotonData" + (i + 1));
        }
    }

    if (!mpPhotonCullingMask){
        uint bufferSize = 1 << mCullingHashBufferSizeBits;
        uint width, height;
        computeQuadTexSize(bufferSize, width, height);
        mpPhotonCullingMask = Texture::create2D(mpDevice, width, height, ResourceFormat::R8Uint, 1, 1, nullptr,ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
        mpPhotonCullingMask->setName("ReSTIR_FG::PhotonCullingMask");
    }

}

void ReSTIR_FG::prepareAccelerationStructure() {
    //Delete the Photon AS if max Buffer size changes
    if (mChangePhotonLightBufferSize)
    {
        mpPhotonAS.reset();
        mChangePhotonLightBufferSize = false;
    }
       

    //Create the Photon AS
    if (!mpPhotonAS)
    {
        std::vector<uint64_t> aabbCount = {mNumMaxPhotons[0], mNumMaxPhotons[1]};
        std::vector<uint64_t> aabbGPUAddress = {mpPhotonAABB[0]->getGpuAddress(), mpPhotonAABB[1]->getGpuAddress()};
        mpPhotonAS = std::make_unique<CustomAccelerationStructure>(mpDevice, aabbCount, aabbGPUAddress);
    }
}

void ReSTIR_FG::prepareRayTracingShaders(RenderContext* pRenderContext) {
    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();

    //TODO specify the payload bytes for each pass
    mFinalGatherSamplePass.initRTProgram(mpDevice, mpScene, kFinalGatherSamplesShader, kMaxPayloadBytes, globalTypeConformances);
    mGeneratePhotonPass.initRTProgram(mpDevice, mpScene, kGeneratePhotonsShader, kMaxPayloadBytes, globalTypeConformances);
    mTraceTransmissionDelta.initRTProgram(mpDevice, mpScene, kTraceTransmissionDeltaShader, kMaxPayloadBytes, globalTypeConformances);

    //Special Program for the Photon Collection as the photon acceleration structure is used
    mCollectPhotonPass.initRTCollectionProgram(mpDevice, mpScene, kCollectPhotonsShader, kMaxPayloadBytes, globalTypeConformances);
}

void ReSTIR_FG::traceTransmissiveDelta(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "TraceDeltaTransmissive");

    mTraceTransmissionDelta.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));
    mTraceTransmissionDelta.pProgram->addDefine("TRACE_TRANS_SPEC_ROUGH_CUTOFF", std::to_string(mTraceRoughnessCutoff));

    if (!mTraceTransmissionDelta.pVars)
        mTraceTransmissionDelta.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mTraceTransmissionDelta.pVars);

    auto var = mTraceTransmissionDelta.pVars->getRootVar();

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gMaxBounces"] = mTraceMaxBounces;
    var[nameBuf]["gRequDiffParts"] = mTraceRequireDiffuseMat;
    var[nameBuf]["gAlphaTest"] = mPhotonUseAlphaTest; 

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

void ReSTIR_FG::getFinalGatherHitPass(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "FinalGatherSample");
    // Clear the buffer if photon culling is used TODO Compute pass to clear?
    if (mUsePhotonCulling)
    {
        pRenderContext->clearUAV(mpPhotonCullingMask->getUAV().get(), uint4(0));
    }

    mFinalGatherSamplePass.pProgram->addDefine("USE_PHOTON_CULLING", mUsePhotonCulling ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("USE_CAUSTIC_CULLING", (mCausticCollectMode != CausticCollectionMode::None) && mUseCausticCulling ? "1" : "0");
    mFinalGatherSamplePass.pProgram->addDefine("USE_RTXDI", mpRTXDI ? "1" : "0");
    if (mpRTXDI) mFinalGatherSamplePass.pProgram->addDefines(mpRTXDI->getDefines());
        
    if (!mFinalGatherSamplePass.pVars)
        mFinalGatherSamplePass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mFinalGatherSamplePass.pVars);

    auto var = mFinalGatherSamplePass.pVars->getRootVar();

    // Set Constant Buffers
    float hashRad = mCullingUseFixedRadius ? std::max(mPhotonCollectRadius.x, mCullingCellRadius) : mPhotonCollectRadius.x;

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gCollectionRadius"] = mPhotonCollectRadius;
    var[nameBuf]["gHashScaleFactor"] = 1.f / (2 * hashRad); // Hash Scale
    var[nameBuf]["gAttenuationRadius"] = mSampleRadiusAttenuation;

    nameBuf = "Constant";
    var[nameBuf]["gHashSize"] = 1 << mCullingHashBufferSizeBits;
    var[nameBuf]["gUseAlphaTest"] = mPhotonUseAlphaTest;
    var[nameBuf]["gDeltaRejection"] = mGenerationDeltaRejection;

    if (mpRTXDI) mpRTXDI->setShaderData(var);
    var["gVBuffer"] = mpVBuffer;
    var["gView"] = mpViewDir;
    var["gLinZ"] = mpRayDist;

    var["gReservoir"] = mpReservoirBuffer[mFrameCount % 2];
    var["gSurfaceData"] = mpSurfaceBuffer[mFrameCount % 2];
    var["gFinalGatherHit"] = mpFinalGatherSampleHitData;
    var["gPhotonCullingMask"] = mpPhotonCullingMask;

    FALCOR_ASSERT(mScreenRes.x > 0 && mScreenRes.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mFinalGatherSamplePass.pProgram.get(), mFinalGatherSamplePass.pVars, uint3(mScreenRes, 1));

    if (mpPhotonCullingMask)
        pRenderContext->uavBarrier(mpPhotonCullingMask.get());
}

void ReSTIR_FG::generatePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData, bool secondPass) {

    std::string passName = mMixedLights ? (secondPass ? "PhotonGenAnalytic" : "PhotonGenEmissive") : "PhotonGeneration";
    FALCOR_PROFILE(pRenderContext, passName);

    //TODO Clear via Compute pass?
    if (!secondPass)
    {
        pRenderContext->clearUAV(mpPhotonCounter[mFrameCount % kPhotonCounterCount]->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mpPhotonAABB[0]->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mpPhotonAABB[1]->getUAV().get(), uint4(0));
    }

    // Get dimensions of ray dispatch.
    uint dispatchedPhotons = mNumDispatchedPhotons;
    bool traceScene = true;
    if (mMixedLights)
    {
        float dispatchedF = float(dispatchedPhotons);
        dispatchedF *= secondPass ? mPhotonAnalyticRatio : 1.f - mPhotonAnalyticRatio;
        dispatchedPhotons = uint(dispatchedF);
        if (dispatchedPhotons == 0)
            traceScene = false;
    }

    uint photonXExtent = std::max(1u, dispatchedPhotons / mPhotonYExtent);  //Divide total count by Y Extent
    photonXExtent += 32u - (photonXExtent % 32);                            //Round up to a multiple of 32
    const uint2 targetDim = uint2(photonXExtent, mPhotonYExtent);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);


    // Defines
    mGeneratePhotonPass.pProgram->addDefine("USE_EMISSIVE_LIGHT", mpScene->useEmissiveLights() ? "1" : "0");
    mGeneratePhotonPass.pProgram->addDefine("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mNumMaxPhotons[0]));
    mGeneratePhotonPass.pProgram->addDefine("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mNumMaxPhotons[1]));
    mGeneratePhotonPass.pProgram->addDefine("USE_PHOTON_CULLING", mUsePhotonCulling ? "1" : "0");
    mGeneratePhotonPass.pProgram->addDefine("USE_CAUSTIC_CULLING", mUseCausticCulling ? "1" : "0");

    
    if (!mGeneratePhotonPass.pVars)
    {
        FALCOR_ASSERT(mGeneratePhotonPass.pProgram);
        if (mpEmissiveLightSampler)
            mGeneratePhotonPass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

        mGeneratePhotonPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    };

    FALCOR_ASSERT(mGeneratePhotonPass.pVars);

    auto var = mGeneratePhotonPass.pVars->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);

    // Set constants (uniforms).
    //
    // PerFrame Constant Buffer
    float hashRad = mCullingUseFixedRadius ? std::max(mPhotonCollectRadius.x, mCullingCellRadius) : mPhotonCollectRadius.x;
    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gPhotonRadius"] = mPhotonCollectRadius;
    var[nameBuf]["gHashScaleFactor"] = 1.f / (2 * hashRad); // Hash scale factor. 1/diameter.

    // Upload constant buffer only if options changed

    // Fill flags
    uint flags = 0;
    if (mPhotonUseAlphaTest) flags |= 0x01;
    if (mPhotonAdjustShadingNormal) flags |= 0x02;
    if (mCausticCollectMode != CausticCollectionMode::None) flags |= 0x04;
    if (mGenerationDeltaRejection) flags |= 0x08;
    if (mGenerationDeltaRejectionRequireDiffPart) flags |= 0x10;
    if (!mpScene->useEmissiveLights() || secondPass) flags |= 0x20; // Analytic lights collect flag

    nameBuf = "CB";
    var[nameBuf]["gMaxRecursion"] = mPhotonMaxBounces;
    var[nameBuf]["gRejection"] = mPhotonRejection;
    var[nameBuf]["gFlags"] = flags;
    var[nameBuf]["gHashSize"] = 1 << mCullingHashBufferSizeBits; // Size of the Photon Culling buffer. 2^x
    var[nameBuf]["gCausticsBounces"] = mMaxCausticBounces;

     if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["Light"]["gEmissiveSampler"]);

     // Set the photon buffers
     for (uint32_t i = 0; i < 2; i++){
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPackedPhotonData"][i] = mpPhotonData[i];
     }
     var["gPhotonCounter"] = mpPhotonCounter[mFrameCount % kPhotonCounterCount];
     var["gPhotonCullingMask"] = mpPhotonCullingMask;

     // Trace the photons
     if (traceScene)
        mpScene->raytrace(pRenderContext, mGeneratePhotonPass.pProgram.get(), mGeneratePhotonPass.pVars, uint3(targetDim, 1));

     pRenderContext->uavBarrier(mpPhotonCounter[mFrameCount % kPhotonCounterCount].get());
     pRenderContext->uavBarrier(mpPhotonAABB[0].get());
     pRenderContext->uavBarrier(mpPhotonData[0].get());
     pRenderContext->uavBarrier(mpPhotonAABB[1].get());
     pRenderContext->uavBarrier(mpPhotonData[1].get());

     if (!mMixedLights || mMixedLights && secondPass)
     {
        handlePhotonCounter(pRenderContext);

        // Build/Update Acceleration Structure
        uint2 currentPhotons = mFrameCount > 0 ? uint2(float2(mCurrentPhotonCount) * mASBuildBufferPhotonOverestimate) : mNumMaxPhotons;
        std::vector<uint64_t> photonBuildSize = {
            std::min(mNumMaxPhotons[0], currentPhotons[0]), std::min(mNumMaxPhotons[1], currentPhotons[1])};
        mpPhotonAS->update(pRenderContext, photonBuildSize);
     }
    
}

void ReSTIR_FG::handlePhotonCounter(RenderContext* pRenderContext)
{
     // Copy the photonCounter to a CPU Buffer
     pRenderContext->copyBufferRegion(
         mpPhotonCounterCPU[mFrameCount % kPhotonCounterCount].get(), 0, mpPhotonCounter[mFrameCount % kPhotonCounterCount].get(), 0,
         sizeof(uint32_t) * 2
     );

     void* data = mpPhotonCounterCPU[mFrameCount % kPhotonCounterCount]->map(Buffer::MapType::Read);
     std::memcpy(&mCurrentPhotonCount, data, sizeof(uint) * 2);
     mpPhotonCounterCPU[mFrameCount % kPhotonCounterCount]->unmap();

     // Change Photon dispatch count dynamically.
     if (mUseDynamicePhotonDispatchCount)
     {
        // Only use global photons for the dynamic dispatch count
        uint globalPhotonCount = mCurrentPhotonCount[0];
        uint globalMaxPhotons = mNumMaxPhotons[0];
        // If counter is invalid, reset
        if (globalPhotonCount == 0)
        {
            mNumDispatchedPhotons = kDynamicPhotonDispatchInitValue;
        }
        uint bufferSizeCompValue = (uint)(globalMaxPhotons * (1.f - mPhotonDynamicGuardPercentage));
        uint changeSize = (uint)(globalMaxPhotons * mPhotonDynamicChangePercentage);

        // If smaller, increase dispatch size
        if (globalPhotonCount < bufferSizeCompValue)
        {
            uint newDispatched = (uint)((mNumDispatchedPhotons + changeSize) / mPhotonYExtent) * mPhotonYExtent; // mod YExtend == 0
            mNumDispatchedPhotons = std::min(newDispatched, mPhotonDynamicDispatchMax);
        }
        // If bigger, decrease dispatch size
        else if (globalPhotonCount >= globalMaxPhotons)
        {
            uint newDispatched = (uint)((mNumDispatchedPhotons - changeSize) / mPhotonYExtent) * mPhotonYExtent; // mod YExtend == 0
            mNumDispatchedPhotons = std::max(newDispatched, mPhotonYExtent);
        }
     }
}

void ReSTIR_FG::collectPhotons(RenderContext* pRenderContext, const RenderData& renderData) {
     FALCOR_PROFILE(pRenderContext, "CollectPhotons");

     bool finalGatherRenderMode = mRenderMode == RenderMode::FinalGather;

     //Defines TODO add
     mCollectPhotonPass.pProgram->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
     mCollectPhotonPass.pProgram->addDefine("MODE_FINAL_GATHER", finalGatherRenderMode ? "1" : "0");
     mCollectPhotonPass.pProgram->addDefine("CAUSTIC_COLLECTION_MODE", std::to_string((uint)mCausticCollectMode));

     if (!mCollectPhotonPass.pVars)
        mCollectPhotonPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
     FALCOR_ASSERT(mCollectPhotonPass.pVars);

     auto var = mCollectPhotonPass.pVars->getRootVar();

     // Set Constant Buffers
     std::string nameBuf = "PerFrame";
     var[nameBuf]["gFrameCount"] = mFrameCount;
     var[nameBuf]["gPhotonRadius"] = mPhotonCollectRadius;
     var[nameBuf]["gAttenuationRadius"] = mSampleRadiusAttenuation;

     //Set Temporal Constant Buffer if necessary
     if (mCausticCollectMode == CausticCollectionMode::Temporal)
     {
        nameBuf = "TemporalFilter";
        var[nameBuf]["gTemporalFilterHistoryLimit"] = mCausticTemporalFilterHistoryLimit;
        var[nameBuf]["gDepthThreshold"] = mRelativeDepthThreshold;
        var[nameBuf]["gNormalThreshold"] = mNormalThreshold;
        var[nameBuf]["gMatThreshold"] = mMaterialThreshold;

        //Bind necessary buffer and textures
        //Temporal Indices
        uint idxCurr = mFrameCount % 2;
        uint idxPrev = (mFrameCount + 1) % 2;

        var["gMVec"] = renderData[kInputMotionVectors]->asTexture();
        
        var["gSurface"] = mpSurfaceBuffer[idxCurr];
        var["gSurfacePrev"] = mpSurfaceBuffer[idxPrev];

        var["gCausticPrev"] = mpCausticRadiance[idxPrev];
        var["gCausticOut"] = mpCausticRadiance[idxCurr];
     }

     // Bind reservoir and light buffer depending on the boost buffer
     var["gReservoir"] = mpReservoirBuffer[mFrameCount % 2];
     if (!finalGatherRenderMode) //Only Bind when resampling is used
        var["gFGSampleData"] = mpFGSampelDataBuffer[mFrameCount % 2];
     for (uint32_t i = 0; i < 2; i++)
     {
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPackedPhotonData"][i] = mpPhotonData[i];
     }
     var["gFinalGatherHit"] = mpFinalGatherSampleHitData;

     var["gVBuffer"] = mpVBuffer;
     var["gView"] = mpViewDir;

     if (finalGatherRenderMode)
        var["gColor"] = renderData[kOutputColor]->asTexture();
     else if (mCausticCollectMode != CausticCollectionMode::Temporal)
        var["gCausticOut"] = mpCausticRadiance[0];


     mpPhotonAS->bindTlas(var, "gPhotonAS");

      // Create dimensions based on the number of VPLs
     uint2 targetDim = renderData.getDefaultTextureDims();
     FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

     // Trace the photons
     mpScene->raytrace(pRenderContext, mCollectPhotonPass.pProgram.get(), mCollectPhotonPass.pVars, uint3(targetDim, 1));
}

void ReSTIR_FG::resamplingPass(RenderContext* pRenderContext, const RenderData& renderData) {
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

     //If defines change, refresh the program
     mpResamplingPass->getProgram()->addDefine("MODE_SPATIOTEMPORAL", mResamplingMode == ResamplingMode::SpartioTemporal ? "1" : "0");
     mpResamplingPass->getProgram()->addDefine("MODE_TEMPORAL", mResamplingMode == ResamplingMode::Temporal ? "1" : "0");
     mpResamplingPass->getProgram()->addDefine("MODE_SPATIAL", mResamplingMode == ResamplingMode::Spartial ? "1" : "0");
     mpResamplingPass->getProgram()->addDefine("BIAS_CORRECTION_MODE", std::to_string((uint)mBiasCorrectionMode));
     mpResamplingPass->getProgram()->addDefine("USE_REDUCED_RESERVOIR_FORMAT" ,mUseReducedReservoirFormat ? "1" : "0");

     
    // Set variables
     auto var = mpResamplingPass->getRootVar();

     mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
     mpSampleGenerator->setShaderData(var);                    // Sample generator

     //Bind Reservoir and surfaces
     uint idxCurr = mFrameCount % 2;
     uint idxPrev = (mFrameCount + 1) % 2;

     var["gSurface"] = mpSurfaceBuffer[idxCurr];
     var["gSurfacePrev"] = mpSurfaceBuffer[idxPrev];

     //Swap the reservoir and sample indices for spatial resampling
     if (mResamplingMode == ResamplingMode::Spartial)
        std::swap(idxCurr, idxPrev);

     var["gReservoir"]          = mpReservoirBuffer[idxCurr];
     var["gReservoirPrev"]      = mpReservoirBuffer[idxPrev];
     var["gFGSampleData"]       = mpFGSampelDataBuffer[idxCurr];
     var["gFGSampleDataPrev"]   = mpFGSampelDataBuffer[idxPrev];
    

     //View
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
     pRenderContext->uavBarrier(mpFGSampelDataBuffer[idxCurr].get());
}

void ReSTIR_FG::finalShadingPass(RenderContext* pRenderContext, const RenderData& renderData) {
     FALCOR_PROFILE(pRenderContext,"FinalShading");

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
        if (mpRTXDI) defines.add(mpRTXDI->getDefines());
        defines.add("USE_RTXDI", mpRTXDI ? "1" : "0");
        defines.add("USE_RESTIRFG", mRenderMode == RenderMode::ReSTIRFG ? "1" : "0");

        mpFinalShadingPass = ComputePass::create(mpDevice, desc, defines, true);
     }
     FALCOR_ASSERT(mpFinalShadingPass);

     if (mpRTXDI) mpFinalShadingPass->getProgram()->addDefines(mpRTXDI->getDefines());  //TODO only set once?
     mpFinalShadingPass->getProgram()->addDefine("USE_RTXDI", mpRTXDI ? "1" : "0");
     mpFinalShadingPass->getProgram()->addDefine("USE_RESTIRFG", mRenderMode == RenderMode::ReSTIRFG ? "1" : "0");
     mpFinalShadingPass->getProgram()->addDefine("USE_REDUCED_RESERVOIR_FORMAT", mUseReducedReservoirFormat ? "1" : "0");
     mpFinalShadingPass->getProgram()->addDefine("USE_ENV_BACKROUND", mpScene->useEnvBackground() ? "1" : "0");
     // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
     mpFinalShadingPass->getProgram()->addDefines(getValidResourceDefines(kOutputChannels, renderData));

     // Set variables
     auto var = mpFinalShadingPass->getRootVar();

     mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
     mpSampleGenerator->setShaderData(var);                    // Sample generator

     if (mpRTXDI) mpRTXDI->setShaderData(var);

     uint reservoirIndex = mResamplingMode == ResamplingMode::Spartial ? (mFrameCount + 1) % 2 : mFrameCount % 2;

     var["gReservoir"] = mpReservoirBuffer[reservoirIndex];
     var["gFGSampleData"] = mpFGSampelDataBuffer[reservoirIndex];

     var["gThp"] = mpThp;
     var["gView"] = mpViewDir;
     var["gVBuffer"] = mpVBuffer;

     uint causticRadianceIdx = mCausticCollectMode == CausticCollectionMode::Temporal ? mFrameCount % 2 : 0;

     var["gCausticRadiance"] = mpCausticRadiance[causticRadianceIdx];

     //Bind all Output Channels
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
     //var[uniformName]["gEnableCaustics"] = mEnableCausticPhotonCollection;

     // Execute
     const uint2 targetDim = renderData.getDefaultTextureDims();
     FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
     mpFinalShadingPass->execute(pRenderContext, uint3(targetDim, 1));
}

void ReSTIR_FG::directAnalytic(RenderContext* pRenderContext, const RenderData& renderData) {
     FALCOR_PROFILE(pRenderContext, "DirectLight(Analytic)");

     // Create pass
     if (!mpDirectAnalyticPass)
     {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDirectAnalyticPassShader).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getValidResourceDefines(kOutputChannels, renderData));
        defines.add(getValidResourceDefines(kInputChannels, renderData));
       
        mpDirectAnalyticPass = ComputePass::create(mpDevice, desc, defines, true);
     }
     FALCOR_ASSERT(mpDirectAnalyticPass);

     // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
     mpDirectAnalyticPass->getProgram()->addDefines(getValidResourceDefines(kOutputChannels, renderData));

     // Set variables
     auto var = mpDirectAnalyticPass->getRootVar();

     mpScene->setRaytracingShaderData(pRenderContext, var, 1); // Set scene data
     mpSampleGenerator->setShaderData(var);                    // Sample generator

     var["gThp"] = mpThp;
     var["gView"] = mpViewDir;
     var["gVBuffer"] = mpVBuffer;

     // Bind all Output Channels
     for (uint i = 0; i < kOutputChannels.size(); i++)
     {
        if (renderData[kOutputChannels[i].name])
            var[kOutputChannels[i].texname] = renderData[kOutputChannels[i].name]->asTexture();
     }

     // Uniform
     std::string uniformName = "PerFrame";
     var[uniformName]["gFrameCount"] = mFrameCount;

     // Execute
     const uint2 targetDim = renderData.getDefaultTextureDims();
     FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
     mpDirectAnalyticPass->execute(pRenderContext, uint3(targetDim, 1));
}

void ReSTIR_FG::copyViewTexture(RenderContext* pRenderContext, const RenderData& renderData) {
     if (mpViewDir != nullptr)
     {
        pRenderContext->copyResource(mpViewDirPrev.get(), mpViewDir.get());
     }
}

void ReSTIR_FG::computeQuadTexSize(uint maxItems, uint& outWidth, uint& outHeight) {
     // Compute the size of a power-of-2 rectangle that fits all items, 1 item per pixel
     double textureWidth = std::max(1.0, ceil(sqrt(double(maxItems))));
     textureWidth = exp2(ceil(log2(textureWidth)));
     double textureHeight = std::max(1.0, ceil(maxItems / textureWidth));
     textureHeight = exp2(ceil(log2(textureHeight)));

     outWidth = uint(textureWidth);
     outHeight = uint(textureHeight);
}

void ReSTIR_FG::RayTraceProgramHelper::initRTProgram(ref<Device> device,ref<Scene> scene,const std::string& shaderName,
                                                     uint maxPayloadBytes,const Program::TypeConformanceList& globalTypeConformances)
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

    //TODO: Support more geometry types and more material conformances
    if (scene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(
            0, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit") );
    }

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void ReSTIR_FG::RayTraceProgramHelper::initRTCollectionProgram(ref<Device> device,ref<Scene> scene,const std::string& shaderName,
                                                               uint maxPayloadBytes,const Program::TypeConformanceList& globalTypeConformances)
{
    RtProgram::Desc desc;
    desc.addShaderModules(scene->getShaderModules());
    desc.addShaderLibrary(shaderName);
    desc.setMaxPayloadSize(maxPayloadBytes);
    desc.setMaxAttributeSize(scene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(1);

    pBindingTable = RtBindingTable::create(1, 1, scene->getGeometryCount()); //Geometry Count is still needed as the scenes AS is still bound
    auto& sbt = pBindingTable;
    sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances));   //Type conformances for material model
    sbt->setMiss(0, desc.addMiss("miss"));
    sbt->setHitGroup(0, 0, desc.addHitGroup("", "anyHit", "intersection", globalTypeConformances));

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void ReSTIR_FG::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice,ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
{
    FALCOR_ASSERT(pProgram);

    // Configure program.
    pProgram->addDefines(pSampleGenerator->getDefines());
    pProgram->setTypeConformances(pScene->getTypeConformances());
    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    pVars = RtProgramVars::create(pDevice ,pProgram, pBindingTable);

    // Bind utility classes into shared data.
    auto var = pVars->getRootVar();
    pSampleGenerator->setShaderData(var);
}
