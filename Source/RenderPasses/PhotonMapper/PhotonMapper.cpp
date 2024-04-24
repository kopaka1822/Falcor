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
#include "PhotonMapper.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "Utils/Math/FalcorMath.h"

namespace
{
    const std::string kShaderGeneratePhotons = "RenderPasses/PhotonMapper/PhotonMapperGenerate.rt.slang";
    const std::string kShaderCollectPhotons = "RenderPasses/PhotonMapper/PhotonMapperCollect.rt.slang";
    const std::string kShaderTraceTransmissionDelta = "RenderPasses/PhotonMapper/TraceTransmissionDelta.rt.slang";

    const std::string kShaderModel = "6_5";
    const uint kMaxPayloadBytes = 96u;

    const std::string kInputVBuffer = "vbuffer";

    const Falcor::ChannelList kInputChannels{
        {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
    };

    const std::string kOutputColor = "color";

    const Falcor::ChannelList kOutputChannels{
        {kOutputColor, "gOutColor", "Output Color (linear)", false /*optional*/, ResourceFormat::RGBA32Float},
    };

}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PhotonMapper>();
}

PhotonMapper::PhotonMapper(ref<Device> pDevice, const Properties& props)
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

    // TODO Handle Properties

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, mpSampleGenerator);
}

Properties PhotonMapper::getProperties() const
{
    return Properties();
}

RenderPassReflection PhotonMapper::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void PhotonMapper::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) // Return on empty scene
        return;

     auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mSPPMFramesCameraStill = 0;
        mOptionsChanged = false;
    }

    // Prepare used Datas and Buffers
    prepareLighting(pRenderContext);

    prepareBuffers(pRenderContext, renderData);

    prepareAccelerationStructure();

    // RenderPasses
    handlePhotonCounter(pRenderContext);

    traceTransmissiveDelta(pRenderContext, renderData);

    generatePhotonsPass(pRenderContext, renderData);

    collectPhotons(pRenderContext, renderData);

     // SPPM
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

    mFrameCount++;
}

void PhotonMapper::renderUI(Gui::Widgets& widget)
{
    bool changed = false;

    if (auto group = widget.group("Specular Trace Options"))
    {
        widget.var("Specular/Transmissive Bounces", mTraceMaxBounces, 0u, 32u, 1u);
        widget.tooltip("Number of specular/transmissive bounces. 0 -> direct hit only");
        widget.checkbox("Require Diffuse Part", mTraceRequireDiffuseMat);
        widget.tooltip("Requires a diffuse part in addition to delta lobes");
    }

    if (auto group = widget.group("PhotonMapper"))
    {
        changed |= widget.checkbox("Enable dynamic photon dispatch", mUseDynamicePhotonDispatchCount);
        widget.tooltip("Changed the number of dispatched photons dynamically. Tries to fill the photon buffer");
        if (mUseDynamicePhotonDispatchCount)
        {
            changed |= widget.var("Max dispatched", mPhotonDynamicDispatchMax, mPhotonYExtent, 4000000u);
            widget.tooltip("Maximum number the dispatch can be increased to");
            changed |= widget.var("Guard Percentage", mPhotonDynamicGuardPercentage, 0.0f, 1.f, 0.001f);
            widget.tooltip(
                "If current fill rate is under PhotonBufferSize * (1-pGuard), the values are accepted. Reduces the changes every frame"
            );
            changed |= widget.var("Percentage Change", mPhotonDynamicChangePercentage, 0.01f, 10.f, 0.01f);
            widget.tooltip(
                "Increase/Decrease percentage from the Buffer Size. With current value a increase/decrease of :" +
                std::to_string(mPhotonDynamicChangePercentage * mNumMaxPhotons[0]) + "is expected"
            );
            widget.text("Dispatched Photons: " + std::to_string(mNumDispatchedPhotons));
        }
        else
        {
            uint dispatchedPhotons = mNumDispatchedPhotons;
            bool disPhotonChanged = widget.var("Dispatched Photons", dispatchedPhotons, mPhotonYExtent, 9984000u, (float)mPhotonYExtent);
            if (disPhotonChanged)
                mNumDispatchedPhotons = (uint)(dispatchedPhotons / mPhotonYExtent) * mPhotonYExtent;
        }

        // Buffer size
        widget.text("Photons: " + std::to_string(mCurrentPhotonCount[0]) + " / " + std::to_string(mNumMaxPhotons[0]));
        widget.text("Caustic photons: " + std::to_string(mCurrentPhotonCount[1]) + " / " + std::to_string(mNumMaxPhotons[1]));
        widget.var("Photon Buffer Size", mNumMaxPhotonsUI, 100u, 100000000u, 100);
        widget.tooltip("First -> Global, Second -> Caustic");
        mChangePhotonLightBufferSize = widget.button("Apply", true);

        changed |= widget.var("Light Store Probability", mPhotonRejection, 0.f, 1.f, 0.0001f);
        widget.tooltip("Probability a photon light is stored on diffuse hit. Flux is scaled up appropriately");

        changed |= widget.var("Max Bounces", mPhotonMaxBounces, 0u, 32u);
        changed |= widget.var("Max Caustic Bounces", mMaxCausticBounces, 0u, 32u);
        widget.tooltip("Maximum number of diffuse bounces that are allowed for a caustic photon.");

        changed |= widget.checkbox("Caustic from Delta lobes only", mGenerationDeltaRejection);
        widget.tooltip("Only stores ");
        if (mGenerationDeltaRejection)
        {
            widget.checkbox("Delta Lobes require diffuse parts", mGenerationDeltaRejectionRequireDiffPart);
            widget.tooltip("Requires a nonzero diffuse part for diffuse surfaces");
        }

        bool radiusChanged = widget.var("Collection Radius", mPhotonCollectionRadiusStart, 0.00001f, 1000.f, 0.00001f, false);
        mPhotonCollectionRadiusStart.y = std::min(mPhotonCollectionRadiusStart.y, mPhotonCollectionRadiusStart.x);
        widget.tooltip("Photon Radii for final gather and caustic collecton. First->Global, Second->Caustic");
        if (radiusChanged)
            mPhotonCollectRadius = mPhotonCollectionRadiusStart;

         changed |= group.checkbox("Enable SPPM", mUseSPPM);
        group.tooltip(
            "Stochastic Progressive Photon Mapping. Radius is reduced by a fixed sequence every frame. It is advised to use SPPM only for "
            "Offline Rendering"
        );
        if (mUseSPPM)
        {
            group.var("SPPM Alpha", mSPPMAlpha, 0.001f, 1.f, 0.001f);
            group.text(
                "Current Radius: Global = " + std::to_string(mPhotonCollectRadius.x) +
                "; Caustic = " + std::to_string(mPhotonCollectRadius.y)
            );
            changed |= group.button("Reset", true);
        }

        changed |= widget.checkbox("Use Photon Culling", mUsePhotonCulling);
        widget.tooltip("Enabled culling of photon based on a hash grid. Photons are only stored on cells that are collected");
        if (mUsePhotonCulling)
        {
            if (auto groupCulling = widget.group("CullingSettings", true))
            {
                widget.checkbox("Use fixed Culling Cell radius", mCullingUseFixedRadius);
                widget.tooltip("Use a fixed radius for the culling cell. Only used from the point where [Global Radius < Hash Radius]");
                if (mCullingUseFixedRadius)
                    changed |= widget.var("Hash Cell Radius", mCullingCellRadius, 0.0001f, 10000.f, 0.0001f);
                bool rebuildBuffer = widget.var("Culling Size Bytes", mCullingHashBufferSizeBits, 10u, 27u);
                widget.tooltip("Size of the culling buffer (2^x) and effective hash bytes used");

                if (rebuildBuffer)
                    mpPhotonCullingMask.reset();
                changed |= rebuildBuffer;
            }
        }
    }

    mOptionsChanged |= changed;
}

void PhotonMapper::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Reset Scene
    mpScene = pScene;

    mTraceTransmissionDelta = RayTraceProgramHelper::create();
    mGeneratePhotonPass = RayTraceProgramHelper::create();
    mCollectPhotonPass = RayTraceProgramHelper::create();
    mpEmissiveLightSampler.reset();

    if (mpScene)
    {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        prepareRayTracingShaders(pRenderContext);
    }
}

bool PhotonMapper::prepareLighting(RenderContext* pRenderContext)
{
    bool lightingChanged = false;
    // Make sure that the emissive light is up to date
    auto& pLights = mpScene->getLightCollection(pRenderContext);

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

void PhotonMapper::prepareBuffers(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Reset screen space depentend buffers if the resolution has changed
    if ((mScreenRes.x != renderData.getDefaultTextureDims().x) || (mScreenRes.y != renderData.getDefaultTextureDims().y))
    {
        mScreenRes = renderData.getDefaultTextureDims();
        mpVBuffer.reset();
        mpViewDir.reset();
        mpThp.reset();
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

    // Per pixel Buffers/Textures
    if (!mpVBuffer)
    {
        mpVBuffer = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, HitInfo::kDefaultFormat, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpVBuffer->setName("PM::VBufferWorkCopy");
    }

    if (!mpViewDir)
    {
        mpViewDir = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, kViewDirFormat, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpViewDir->setName("PM::ViewDir");
    }

    if (!mpThp)
    {
        mpThp = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA16Float, 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpThp->setName("PM::Throughput");
    }

    // Photon
    if (!mpPhotonCounter)
    {
        mpPhotonCounter = Buffer::create(mpDevice, sizeof(uint) * 2);
        mpPhotonCounter->setName("PM::PhotonCounterGPU");
    }
    if (!mpPhotonCounterCPU)
    {
        mpPhotonCounterCPU = Buffer::create(mpDevice, sizeof(uint) * 2, ResourceBindFlags::None, Buffer::CpuAccess::Read);
        mpPhotonCounterCPU->setName("PM::PhotonCounterCPU");
    }
    for (uint i = 0; i < 2; i++)
    {
        if (!mpPhotonAABB[i])
        {
            mpPhotonAABB[i] = Buffer::createStructured(mpDevice, sizeof(AABB), mNumMaxPhotons[i]);
            mpPhotonAABB[i]->setName("PM::PhotonAABB" + (i + 1));
        }
        if (!mpPhotonData[i])
        {
            mpPhotonData[i] = Buffer::createStructured(mpDevice, sizeof(uint) * 4, mNumMaxPhotons[i]);
            mpPhotonData[i]->setName("PM::PhotonData" + (i + 1));
        }
    }

    if (!mpPhotonCullingMask)
    {
        uint bufferSize = 1 << mCullingHashBufferSizeBits;
        uint width, height;
        computeQuadTexSize(bufferSize, width, height);
        mpPhotonCullingMask = Texture::create2D(
            mpDevice, width, height, ResourceFormat::R8Uint, 1, 1, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        mpPhotonCullingMask->setName("PM::PhotonCullingMask");
    }
}

void PhotonMapper::prepareAccelerationStructure()
{
    // Delete the Photon AS if max Buffer size changes
    if (mChangePhotonLightBufferSize)
    {
        mpPhotonAS.reset();
        mChangePhotonLightBufferSize = false;
    }

    // Create the Photon AS
    if (!mpPhotonAS)
    {
        std::vector<uint64_t> aabbCount = {mNumMaxPhotons[0], mNumMaxPhotons[1]};
        std::vector<uint64_t> aabbGPUAddress = {
            mpPhotonAABB[0]->getGpuAddress(), mpPhotonAABB[1]->getGpuAddress()};
        mpPhotonAS = std::make_unique<CustomAccelerationStructure>(mpDevice, aabbCount, aabbGPUAddress);
    }
}

void PhotonMapper::prepareRayTracingShaders(RenderContext* pRenderContext)
{
    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();

    // TODO specify the payload bytes for each pass
    mGeneratePhotonPass.initRTProgram(mpDevice, mpScene, kShaderGeneratePhotons, kMaxPayloadBytes, globalTypeConformances);
    mTraceTransmissionDelta.initRTProgram(mpDevice, mpScene, kShaderTraceTransmissionDelta, kMaxPayloadBytes, globalTypeConformances);

    // Special Program for the Photon Collection as the photon acceleration structure is used
    mCollectPhotonPass.initRTCollectionProgram(mpDevice, mpScene, kShaderCollectPhotons, kMaxPayloadBytes, globalTypeConformances);
}

void PhotonMapper::traceTransmissiveDelta(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "TraceDeltaTransmissive");

    mTraceTransmissionDelta.pProgram->addDefine("USE_PHOTON_CULLING", mUsePhotonCulling ? "1" : "0");

    if (!mTraceTransmissionDelta.pVars)
        mTraceTransmissionDelta.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mTraceTransmissionDelta.pVars);

    auto var = mTraceTransmissionDelta.pVars->getRootVar();

    float hashRad = mCullingUseFixedRadius ? std::max(mPhotonCollectRadius.x, mCullingCellRadius) : mPhotonCollectRadius.x;

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gMaxBounces"] = mTraceMaxBounces;
    var[nameBuf]["gRequDiffParts"] = mTraceRequireDiffuseMat;
    var[nameBuf]["gHashSize"] = 1 << mCullingHashBufferSizeBits;
    var[nameBuf]["gCollectionRadius"] = mPhotonCollectRadius.x;
    var[nameBuf]["gHashScaleFactor"] = 1.f / (2 * hashRad); // Hash Scale
    var[nameBuf]["gAlphaTest"] = true; // TODO
    
    var["gInVBuffer"] = renderData[kInputVBuffer]->asTexture();
    
    var["gPhotonCullingMask"] = mpPhotonCullingMask;
    var["gOutThp"] = mpThp;
    var["gOutViewDir"] = mpViewDir;
    var["gOutVBuffer"] = mpVBuffer;

    // Create dimensions based on the number of VPLs
    FALCOR_ASSERT(mScreenRes.x > 0 && mScreenRes.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mTraceTransmissionDelta.pProgram.get(), mTraceTransmissionDelta.pVars, uint3(mScreenRes, 1));
}

void PhotonMapper::generatePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData, bool clearBuffers)
{
    FALCOR_PROFILE(pRenderContext, "PhotonGeneration");

    // TODO Clear via Compute pass?
    pRenderContext->clearUAV(mpPhotonCounter->getUAV().get(), uint4(0));
    pRenderContext->clearUAV(mpPhotonAABB[0]->getUAV().get(), uint4(0));
    pRenderContext->clearUAV(mpPhotonAABB[1]->getUAV().get(), uint4(0));

    // Defines
    mGeneratePhotonPass.pProgram->addDefine("USE_EMISSIVE_LIGHT", mpScene->useEmissiveLights() ? "1" : "0");
    mGeneratePhotonPass.pProgram->addDefine("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mNumMaxPhotons[0]));
    mGeneratePhotonPass.pProgram->addDefine("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mNumMaxPhotons[1]));
    mGeneratePhotonPass.pProgram->addDefine("USE_PHOTON_CULLING", mUsePhotonCulling ? "1" : "0");

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
    if (mPhotonUseAlphaTest)
        flags |= 0x01;
    if (mPhotonAdjustShadingNormal)
        flags |= 0x02;
    if (mEnableCausticPhotonCollection)
        flags |= 0x04;
    if (mGenerationDeltaRejection)
        flags |= 0x08;
    if (mGenerationDeltaRejectionRequireDiffPart)
        flags |= 0x10;
    if (!mpScene->useEmissiveLights())
        flags |= 0x20; // Analytic lights collect flag

    nameBuf = "CB";
    var[nameBuf]["gMaxRecursion"] = mPhotonMaxBounces;
    var[nameBuf]["gRejection"] = mPhotonRejection;
    var[nameBuf]["gFlags"] = flags;
    var[nameBuf]["gHashSize"] = 1 << mCullingHashBufferSizeBits; // Size of the Photon Culling buffer. 2^x
    var[nameBuf]["gCausticsBounces"] = mMaxCausticBounces;
    
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["Light"]["gEmissiveSampler"]);

    // Set the photon buffers
    for (uint32_t i = 0; i < 3; i++)
    {
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPackedPhotonData"][i] = mpPhotonData[i];
    }
    var["gPhotonCounter"] = mpPhotonCounter;
    var["gPhotonCullingMask"] = mpPhotonCullingMask;

    // Get dimensions of ray dispatch.
    uint dispatchedPhotons = mNumDispatchedPhotons;
    const uint2 targetDim = uint2(std::max(1u, dispatchedPhotons / mPhotonYExtent), mPhotonYExtent);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mGeneratePhotonPass.pProgram.get(), mGeneratePhotonPass.pVars, uint3(targetDim, 1));

    pRenderContext->uavBarrier(mpPhotonCounter.get());
    for (uint i = 0; i < 2; i++)
    {
        pRenderContext->uavBarrier(mpPhotonAABB[i].get());
        pRenderContext->uavBarrier(mpPhotonData[i].get());
    }

    // Build/Update Acceleration Structure
    uint2 currentPhotons = mFrameCount > 0 ? uint2(float2(mCurrentPhotonCount) * mASBuildBufferPhotonOverestimate) : mNumMaxPhotons;
    std::vector<uint64_t> photonBuildSize = {
        std::min(mNumMaxPhotons[0], currentPhotons[0]), std::min(mNumMaxPhotons[1], currentPhotons[1])};
    mpPhotonAS->update(pRenderContext, photonBuildSize);
}

void PhotonMapper::handlePhotonCounter(RenderContext* pRenderContext)
{
    // Copy the photonCounter to a CPU Buffer
    pRenderContext->copyBufferRegion(mpPhotonCounterCPU.get(), 0, mpPhotonCounter.get(), 0, sizeof(uint32_t) * 2);

    void* data = mpPhotonCounterCPU->map(Buffer::MapType::Read);
    std::memcpy(&mCurrentPhotonCount, data, sizeof(uint) * 2);
    mpPhotonCounterCPU->unmap();

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

void PhotonMapper::collectPhotons(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "CollectPhotons");

    if (!mCollectPhotonPass.pVars)
        mCollectPhotonPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mCollectPhotonPass.pVars);

    auto var = mCollectPhotonPass.pVars->getRootVar();

    // Set Constant Buffers
    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gPhotonRadius"] = mPhotonCollectRadius;

    for (uint32_t i = 0; i < 3; i++)
    {
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPackedPhotonData"][i] = mpPhotonData[i];
    }

    var["gVBufferFirstHit"] = renderData[kInputVBuffer]->asTexture(); 
    var["gVBuffer"] = mpVBuffer;
    var["gView"] = mpViewDir;
    var["gThp"] = mpThp;

    var["gColor"] = renderData[kOutputColor]->asTexture();

    mpPhotonAS->bindTlas(var, "gPhotonAS");

    // Create dimensions based on the number of VPLs
    uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mCollectPhotonPass.pProgram.get(), mCollectPhotonPass.pVars, uint3(targetDim, 1));
}

void PhotonMapper::computeQuadTexSize(uint maxItems, uint& outWidth, uint& outHeight)
{
    // Compute the size of a power-of-2 rectangle that fits all items, 1 item per pixel
    double textureWidth = std::max(1.0, ceil(sqrt(double(maxItems))));
    textureWidth = exp2(ceil(log2(textureWidth)));
    double textureHeight = std::max(1.0, ceil(maxItems / textureWidth));
    textureHeight = exp2(ceil(log2(textureHeight)));

    outWidth = uint(textureWidth);
    outHeight = uint(textureHeight);
}

void PhotonMapper::RayTraceProgramHelper::initRTProgram(
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

void PhotonMapper::RayTraceProgramHelper::initRTCollectionProgram(
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

    pBindingTable = RtBindingTable::create(1, 1, scene->getGeometryCount()); // Geometry Count is still needed as the scenes AS is still
                                                                             // bound
    auto& sbt = pBindingTable;
    sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances)); // Type conformances for material model
    sbt->setMiss(0, desc.addMiss("miss"));
    sbt->setHitGroup(0, 0, desc.addHitGroup("", "anyHit", "intersection", globalTypeConformances));

    pProgram = RtProgram::create(device, desc, scene->getSceneDefines());
}

void PhotonMapper::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
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


