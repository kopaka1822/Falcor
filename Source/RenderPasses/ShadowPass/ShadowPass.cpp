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
#include "ShadowPass.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

namespace
{
    const char kShaderFile[] = "RenderPasses/ShadowPass/ShadowPass.rt.slang";

    // Ray tracing settings that affect the traversal stack size.
    // These should be set as small as possible.
    const uint32_t kMaxPayloadSizeBytes = 8u;
    const uint32_t kMaxRecursionDepth = 1u;

    const ChannelList kInputChannels = {
        {"posW", "gPosW", "World Position"},
        
        {"faceNormalW", "gFaceNormalW", "Face Normal"},
        {"motionVector", "gMVec", "Motion Vector"},
        {"emissive", "gEmissive", "Emissive", true},
        {"guideNormalW", "gGuideNormalW", "World Normal from Textures"},
        {"diffuse", "gDiffuse", "Diffuse Reflection"},
        {"specularRoughness", "gSpecRough", "Specular Reflection (xyz) and Roughness (w)"},
    };

    const ChannelList kOutputChannels = {
        {"color", "gColor", "(Shadowed) Color for the direct light", false, ResourceFormat::RGBA8Unorm},
        {"debug", "gDebug", "Debug Image", true, ResourceFormat::RGBA8Unorm},
    };

    const Gui::DropdownList kDebugModes{
        {0, "Ray Shot"},
        {1, "Lod Level"},
        {2, "Cascaded Level"},
        {3, "LTT Mask Texture"},
        {4, "LTT Mask Texture per Light"},
    };

    const Gui::DropdownList kDistanceSettings{
        {0, "0"},
        {1, "Casc Far Level 0"},
        {2, "Casc Far Level 1"},
        {3, "Casc Far Level 2"},
        {4, "Casc Far Level 3"},
        {5, "Manual"},
    };

} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ShadowPass>();
}

ShadowPass::ShadowPass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

Properties ShadowPass::getProperties() const
{
    return {};
}

RenderPassReflection ShadowPass::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);

    addRenderPassOutputs(reflector, kOutputChannels, ResourceBindFlags::UnorderedAccess);

    return reflector;
}

void ShadowPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "DeferredShadingAndShadow");
    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    // Check if Input Size matches the output size
    const auto inTex = renderData.getTexture(kInputChannels[0].name);

    //Clear Outputs Lamda
    auto clearOutputs = [&]()
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
    };

    // If we have no scene or the scene has no active analytic lights, just clear the outputs and return.
    if (!mpScene)
    {
        clearOutputs();
        return;
    }
    if (mpScene->getActiveLightCount() == 0)
    {
        clearOutputs();
        return;
    }


    // Calculate and update the shadow map
    if (mShadowMode != SPShadowMode::RayShadows)
        if (!mpShadowMap->update(pRenderContext))
            return;

    //Handle hybrid mask textures
    handleHybridMaskData(pRenderContext, renderData.getDefaultTextureDims(), mpScene->getLightCount());
 
    shade(pRenderContext, renderData);
    mFrameCount++;
}

void ShadowPass::shade(RenderContext* pRenderContext, const RenderData& renderData) {
    FALCOR_PROFILE(pRenderContext, "DeferredShading");
    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // I/O Resources could change at any time
    mShadowTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData)); // Emissive is the only optional channel (only
                                                                                             // used in simplified shading)
    mShadowTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData)); // Debug out images

    if (mShadowModeChanged)
    {
        mpShadowMap->setEnableRayTracing(mShadowMode != SPShadowMode::ShadowMap);
        mShadowModeChanged = false;
    }
    

    //Get the define for the alpha test range
    if (mCopyAlphaSettingsFromSM)
        mUseAlphaTestUntilDistance = mpShadowMap->getCascadedAlphaTestDistance();
    else
        mUseAlphaTestUntilDistance = 100000.f;

    //BlendRange
    if (mEnableHybridRTBlend)
    {
        float maxDist = mpShadowMap->getCascadedFarLastHybridLevel();
        if (maxDist > 0)
        {
            float range = maxDist * mHybridRTBlendDistancePercentage;
            float minDist = maxDist - range;
            mHybridRTBlend = float2(minDist, range);
        }else
            mHybridRTBlend = float2(100000.f, 1.f);
    }
    else
    {
        mHybridRTBlend = float2(100000.f, 1.f);
    }

    //Stoch cascaded level
    bool useStochCascLevel = false;
    if (mDebugMode == 2)
        useStochCascLevel = mpShadowMap->getIsStochasticCascadedLevelEnabled();

    // Add defines
    mShadowTracer.pProgram->addDefine("SP_SHADOW_MODE", std::to_string(uint32_t(mShadowMode)));
    mShadowTracer.pProgram->addDefine("ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    mShadowTracer.pProgram->addDefine("DISABLE_ALPHATEST_DISTANCE", std::to_string(mUseAlphaTestUntilDistance));    
    mShadowTracer.pProgram->addDefine("SP_AMBIENT", std::to_string(mAmbientFactor));
    mShadowTracer.pProgram->addDefine("SP_ENV_FACTOR", std::to_string(mEnvMapFactor));
    mShadowTracer.pProgram->addDefine("SP_EMISSIVE", std::to_string(mEmissiveFactor));
    mShadowTracer.pProgram->addDefine("USE_ENV_MAP", mpScene->useEnvBackground() ? "1" : "0");
    mShadowTracer.pProgram->addDefine("USE_EMISSIVE", mEmissiveFactor > 0.f ? "1" : "0");
    mShadowTracer.pProgram->addDefine("DEBUG_MODE", std::to_string(mDebugMode));
    mShadowTracer.pProgram->addDefine("DEBUG_LIGHT_INDEX", std::to_string(mLTTDebugLight));
    mShadowTracer.pProgram->addDefine("SHADOW_ONLY", mShadowOnly ? "1" : "0");
    mShadowTracer.pProgram->addDefine("SHADOW_MIPS_ENABLED", mpShadowMap->getMipMapsEnabled() ? "1" : "0");
    mShadowTracer.pProgram->addDefine("LTT_USE_BLENDING", mEnableHybridRTBlend ? "1" : "0");
    mShadowTracer.pProgram->addDefine(
        "LTT_BLENDING_RANGE", "float2(" + std::to_string(mHybridRTBlend.x) + "," + std::to_string(mHybridRTBlend.y) + ")"
    );
    mShadowTracer.pProgram->addDefine("DEBUG_STOCH_CASC_ENABLED", useStochCascLevel ? "1" : "0");
    mShadowTracer.pProgram->addDefine("LTT_ALPHA_ONLY", mpShadowMap->getRenderDoubleSidedOnly() ? "1" : "0");

    mShadowTracer.pProgram->addDefines(mpShadowMap->getDefines());
    mShadowTracer.pProgram->addDefines(hybridMaskDefines());

    // Prepare Vars
    if (!mShadowTracer.pVars)
    {
        mShadowTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mShadowTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
        mShadowTracer.pVars = RtProgramVars::create(mpDevice, mShadowTracer.pProgram, mShadowTracer.pBindingTable);
    }

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Bind Resources
    auto var = mShadowTracer.pVars->getRootVar();

    // Set Shadow Map per Iteration Shader Data
    mpShadowMap->setShaderDataAndBindBlock(var, targetDim);
    mpSampleGenerator->setShaderData(var);

    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gLTTMaskValid"] = !mHybridMaskFirstFrame && mpHybridMask;

    setHybridMaskVars(var, mFrameCount);
    
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

    // Execute shader
    mpScene->raytrace(pRenderContext, mShadowTracer.pProgram.get(), mShadowTracer.pVars, uint3(targetDim, 1));

    mHybridMaskFirstFrame = false;

    if (mClearHybridMask)
    {
        pRenderContext->clearUAV(mpHybridMask[0]->getUAV().get(), uint4(0));
        pRenderContext->clearUAV(mpHybridMask[1]->getUAV().get(), uint4(0));
        mHybridMaskFirstFrame = true;
        mClearHybridMask = false;
    }
}

void ShadowPass::renderUI(Gui::Widgets& widget)
{
    bool changed = false;
    mShadowModeChanged |= widget.dropdown("Shadow Mode", mShadowMode);
    changed |= mShadowModeChanged;
    if (mShadowMode != SPShadowMode::ShadowMap)
    {
        changed |= widget.checkbox("Ray Alpha Test", mUseAlphaTest);
        if (mUseAlphaTest)
        {
            changed |= widget.checkbox("Copy Alpha settings from SM", mCopyAlphaSettingsFromSM);
            widget.tooltip("Uses the alpha settings from the shadow map. Especially, at which distance the alpha test should be disabled");
        }
    }
        
    if (auto group = widget.group("Shading Settings"))
    {
        group.separator();
        changed |= group.checkbox("Shadow Only", mShadowOnly);
        group.tooltip("Disables shading. Guiding Normal (Textured normal) is used when using Simplified Shading");

        changed |= group.var("Ambient Factor", mAmbientFactor, 0.0f, 1.f, 0.01f);
        changed |= group.var("Env Map Factor", mEnvMapFactor, 0.f, 100.f, 0.01f);
        group.tooltip("Scale factor for the Enviroment Map.");
        changed |= group.var("Emissive Factor", mEmissiveFactor, 0.f, 100.f, 0.01f);
        group.tooltip("Scale factor for the Emissive materials.");
        group.separator();
    }
  
    changed |= lttMaskUI(widget);

    if (mShadowMode != SPShadowMode::RayShadows && mpShadowMap)
    {
        if (auto group = widget.group("Shadow Map Options", true))
        {
            group.separator();
            changed |= mpShadowMap->renderUILeakTracing(group, mShadowMode == SPShadowMode::LeakTracing);
            group.separator();
        }
            
    }

    changed |= widget.dropdown("Debug Mode", kDebugModes, mDebugMode);
    widget.tooltip("Changes the shown debug image for the debug texture. \"Show in Debug Window\" -> \"ShadowPass.debug\"");
    if (mDebugMode == 4)
    {
        widget.var("Choosen Light", mLTTDebugLight, 0u, mpScene->getLightCount()-1, 1u);
    }


    mOptionsChanged |= changed;
}

void ShadowPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    //Clear data from previous scene
    mShadowTracer.pBindingTable = nullptr;
    mShadowTracer.pProgram = nullptr;
    mShadowTracer.pVars = nullptr;

    //Set new scene
    mpScene = pScene;

    //Create Ray Tracing pass
    if (mpScene)
    {
        // Init the shadow map
        mpShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);

        // Create ray tracing program.
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mShadowTracer.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mShadowTracer.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(
                0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
                desc.addHitGroup("", "anyHit")
            );
        }

        mShadowTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());


        // Set starting values for the hybrid mask
        auto& cameraData = mpScene->getCamera()->getData();
        float maxDistance = cameraData.farZ - cameraData.nearZ;
        mHybridMaskRemoveRaysSmallerAsDistance = cameraData.nearZ + maxDistance * 0.005; // Dont Remove the first 0.5 percent
        mHybridMaskRemoveRaysGreaterAsDistance = cameraData.nearZ + maxDistance * 0.010; // Remove the next 0.5 percent
        mHybridMaskExpandRaysMaxDistance = cameraData.nearZ + maxDistance * 0.25;   //Until 25% of visible scene

    }
}

void ShadowPass::handleHybridMaskData(RenderContext* pRenderContext,uint2 screenDims, uint numLights) {
    bool isHybridMode = mShadowMode == SPShadowMode::LeakTracing;
    //Create Textures if missing
    if (isHybridMode)
    {
        bool sizeChanged = false;

        if (mpHybridMask[0]) // testing one texture for this is sufficent
        {
            if (mpHybridMask[0]->getWidth() != screenDims.x || mpHybridMask[0]->getHeight() != screenDims.y)
                sizeChanged = true;
        }

        //Create the hybrid masks
        if (!mpHybridMask[0] || !mpHybridMask[1]|| sizeChanged)
        {
            auto format = ResourceFormat::R8Uint;
            if (numLights > 5)
                format = ResourceFormat::R32Uint;
            else if (numLights > 2)
                format = ResourceFormat::R16Uint;

            // Hybrid Mask
            for (uint i = 0; i < 2; i++)
            {
                mpHybridMask[i] = Texture::create2D(
                    mpDevice, screenDims.x, screenDims.y, format, 1, 1, nullptr,
                    ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
                );
                mpHybridMask[i]->setName("Hybrid Mask" + std::to_string(i));

                pRenderContext->clearUAV(mpHybridMask[i]->getUAV().get(), uint4(0));
            }
            mHybridMaskFirstFrame = true;
        }

        //Create prev depth textures
        if (mHybridUseTemporalDepthTest)
        {
            if (!mpPrevDepth[0] || !mpPrevDepth[1] || sizeChanged)
            {
                for (uint i = 0; i < 2; i++)
                {
                    // Prev Depth
                    mpPrevDepth[i] = Texture::create2D(
                        mpDevice, screenDims.x, screenDims.y, ResourceFormat::R16Float, 1u, 1u, nullptr,
                        ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
                    );
                    mpPrevDepth[i]->setName("Hybrid Mask Prev Depth" + std::to_string(i));
                    pRenderContext->clearUAV(mpPrevDepth[i]->getUAV().get(), uint4(0));
                }
                mHybridMaskFirstFrame = true;
            }
        }
        
        // Create the point sampler
        if (!mpHybridSampler)
        {
            Sampler::Desc samplerDesc;
            samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
            samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
            // samplerDesc.setBorderColor(float4(0.f));
            mpHybridSampler = Sampler::create(mpDevice, samplerDesc);
        }
    }
    else
    {
        // Free hybridMasks
        if (mpHybridMask[0] || mpHybridMask[1])
        {
            mpHybridMask[0].reset();
            mpHybridMask[1].reset();
        }
        // Free sampler
        if (mpHybridSampler)
            mpHybridSampler.reset();
    }

    if (!mHybridUseTemporalDepthTest || !isHybridMode)
    {
        //Free depth textures
        if (mpPrevDepth[0] || mpPrevDepth[1])
        {
            mpPrevDepth[0].reset();
            mpPrevDepth[1].reset();
        }
    }
}

DefineList ShadowPass::hybridMaskDefines() {
    //Check if a cascaded level is fully traced
    if (mpShadowMap->getFullTracedCascadedUsed())
    {
        if (!mFullyTracedCascadedLevelsEnabled)
        {
            mFullyTracedCascadedLevelsEnabled = true;
            mEnableHybridRTBlend = false;
            mUseHybridMaskRemoveRaysDistance = true; // Disable for the last level
            mHybridMaskRemoveRaysSmallerAsDistanceMode = std::min(mpShadowMap->getCascadedLevelHybridIsUsed() + 1, 4u); // Until traced cascaded level
            mHybridMaskRemoveRaysGreaterAsDistanceMode = 4;                                                             // Disabled
        }
    }
    //Restore default settings
    else if (mFullyTracedCascadedLevelsEnabled)
    {
        mFullyTracedCascadedLevelsEnabled = false;
        mEnableHybridRTBlend = true;
        mUseHybridMaskRemoveRaysDistance = false;
        mHybridMaskRemoveRaysSmallerAsDistanceMode = 0;
        mHybridMaskRemoveRaysGreaterAsDistanceMode = 2;
    }

    // Set distances depending on cascaded level (had to be done here as UI code is only executed when open)
    if (mHybridMaskRemoveRaysGreaterAsDistanceMode == 0)
        mHybridMaskRemoveRaysGreaterAsDistance = 0.f;
    else if (mHybridMaskRemoveRaysGreaterAsDistanceMode < 5)
        mHybridMaskRemoveRaysGreaterAsDistance = mpShadowMap->getCascadedFarForLevel(mHybridMaskRemoveRaysGreaterAsDistanceMode - 1);

    if (mHybridMaskRemoveRaysSmallerAsDistanceMode == 0)
        mHybridMaskRemoveRaysSmallerAsDistance = 0.f;
    else if (mHybridMaskRemoveRaysSmallerAsDistanceMode < 5)
        mHybridMaskRemoveRaysSmallerAsDistance = mpShadowMap->getCascadedFarForLevel(mHybridMaskRemoveRaysSmallerAsDistanceMode - 1);

    if (mHybridMaskExpandRaysMaxDistanceMode == 0)
        mHybridMaskExpandRaysMaxDistance = 0.f;
    else if (mHybridMaskExpandRaysMaxDistanceMode < 5)
        mHybridMaskExpandRaysMaxDistance = mpShadowMap->getCascadedFarForLevel(mHybridMaskExpandRaysMaxDistanceMode - 1);

    DefineList defines;
    defines.add("USE_LTT_MASK", mpHybridMask[0] && mEnableHybridMask ? "1" : "0");
    if (mpHybridMask[0])
        defines.add(
            "LTT_MASK_DIMS",
            "uint2(" + std::to_string(mpHybridMask[0]->getWidth()) + "," + std::to_string(mpHybridMask[0]->getHeight()) + ")"
        );
    else
        defines.add("LTT_MASK_DIMS", "uint2(0)");

    defines.add("LTT_MASK_REMOVE_RAYS", mHybridMaskRemoveRays ? "1" : "0");
    defines.add("LTT_MASK_EXPAND_RAYS", mHybridMaskExpandRays ? "1" : "0");

    uint32_t samplePattern = uint32_t(mHybridMaskSamplePattern);
    defines.add("LTT_MASK_SAMPLE_PATTERN", std::to_string(samplePattern));
    uint32_t sampleCount = mHybridMaskSamplePattern == LTTMaskSamplePatterns::Box_3x3 ? 9 : 5;   //Box 3x3 is bigger
    sampleCount = (uint)mHybridMaskSamplePattern > (uint)LTTMaskSamplePatterns::PlusCross ? 4 : sampleCount; //Gather
    defines.add("LTT_MASK_SAMPLE_COUNT", std::to_string(sampleCount));

    defines.add("LTT_MASK_REMOVE_RAYS_USE_MIN_DISTANCE", mUseHybridMaskRemoveRaysDistance ? "1" : "0");
    defines.add("LTT_MASK_EXPAND_RAYS_USE_MAX_DISTANCE", mUseHybridMaskExpandRaysMaxDistance ? "1" : "0");
    defines.add("LTT_MASK_REMOVE_RAYS_SMALLER_AS_DISTANCE", std::to_string(mHybridMaskRemoveRaysSmallerAsDistance));
    defines.add("LTT_MASK_REMOVE_RAYS_GREATER_AS_DISTANCE", std::to_string(mHybridMaskRemoveRaysGreaterAsDistance));
    defines.add("LTT_MASK_EXPAND_RAYS_MAX_DISTANCE", std::to_string(mHybridMaskExpandRaysMaxDistance));
    defines.add("LTT_MASK_USE_TEMPORAL_DEPTH_TEST", mHybridUseTemporalDepthTest ? "1" : "0");
    defines.add("LTT_TEMPORAL_DEPTH_TEST_MAX_DEPTH_DIFF", std::to_string(mHybridTemporalDepthTestPercentage));
    defines.add("LTT_MASK_USE_RAY_WHEN_OUTSIDE", mHybridUseRayWhenOutsideMask ? "1" : "0");
    defines.add("DISABLE_DYNAMIC_GEOMETRY_CHECK", mHybridMaskDisableDynamicGeometryCheck ? "1" : "0");

    //Set all enums in a very hacky way
    std::string base = "LTT_MASK_SAMPLE_PATTERN_";
    const auto& samplePatternItems = EnumInfo<LTTMaskSamplePatterns>::items();
    for (const auto& samplePatternItem : samplePatternItems)
    {
        defines.add(base + samplePatternItem.second.c_str(), std::to_string(uint32_t(samplePatternItem.first)));
    }

    return defines;
}

void ShadowPass::setHybridMaskVars(ShaderVar& var, const uint frameCount) {
    if (mpHybridMask[0] && mpHybridMask[1])
    {
        var["gLTTMask"] = mpHybridMask[frameCount % 2];
        var["gLTTMaskLastFrame"] = mpHybridMask[(frameCount + 1) % 2];
    }
    if (mpPrevDepth[0] && mpPrevDepth[1])
    {
        var["gPrevDepth"] = mpPrevDepth[frameCount % 2];
        var["gPrevDepthWrite"] = mpPrevDepth[(frameCount + 1) % 2];
    }
        
    if (mpHybridSampler)
        var["gLTTMaskSampler"] = mpHybridSampler;
}

bool ShadowPass::lttMaskUI(Gui::Widgets& widget) {
    bool changed = false;

    if (!mFullyTracedCascadedLevelsEnabled)
    {
        changed |= widget.checkbox("Use Hybrid Blend", mEnableHybridRTBlend);
        if (mEnableHybridRTBlend)
        {
            changed |= widget.var("Blend Percentage", mHybridRTBlendDistancePercentage);
        }
    }

    if (auto group = widget.group("LTT Mask"))
    {
        group.separator();
        changed |= group.checkbox("Enable", mEnableHybridMask);
        if (mEnableHybridMask)
        {
            changed |= group.dropdown("Sample Pattern", mHybridMaskSamplePattern);
            changed |= group.checkbox("Expand Rays", mHybridMaskExpandRays);
            group.tooltip("Expands rays on shadow edges");
            
            changed |= group.checkbox("Remove Rays", mHybridMaskRemoveRays);
            group.tooltip("Removes ray from core shadow. Can lead to temporal artifacts on dynamic objects");   //TODO fix dynamic
                        
            if (auto group2 = widget.group("Settings"))
            {
                group.separator();
                if (mHybridMaskExpandRays)
                {
                    group2.text("---------- Expand Ray Settings ----------");
                    changed |= group2.checkbox("Expand Rays until Max distance", mUseHybridMaskExpandRaysMaxDistance);
                    if (mUseHybridMaskExpandRaysMaxDistance)
                    {
                        changed |= group2.dropdown("Max Distance", kDistanceSettings, mHybridMaskExpandRaysMaxDistanceMode);
                        if (mHybridMaskExpandRaysMaxDistanceMode >= 5)
                            group2.var("Max Distance", mHybridMaskExpandRaysMaxDistance, 0.0f);
                        // The auto set happens in hybridMaskDefines()
                    }
                }
                if (mHybridMaskRemoveRays)
                {
                    group2.text("---------- Remove Ray Settings ----------");
                    changed |= group2.checkbox("Remove Rays at Min distance", mUseHybridMaskRemoveRaysDistance);
                    if (mUseHybridMaskRemoveRaysDistance)
                    {
                        changed |= group2.dropdown("Smaller As Distance", kDistanceSettings, mHybridMaskRemoveRaysSmallerAsDistanceMode);
                        if (mHybridMaskRemoveRaysSmallerAsDistanceMode >= 5)
                            group2.var("Manual Distance", mHybridMaskRemoveRaysSmallerAsDistance, 0.0f);

                        changed |= group2.dropdown("Greater As Distance", kDistanceSettings, mHybridMaskRemoveRaysGreaterAsDistanceMode);
                        if (mHybridMaskRemoveRaysGreaterAsDistanceMode >= 5)
                            group2.var("Manual Distance", mHybridMaskRemoveRaysGreaterAsDistance, 0.0f);
                        // The auto set happens in hybridMaskDefines()
                    }
                    group2.checkbox("Use additional Temporal Depth test", mHybridUseTemporalDepthTest);
                    group2.tooltip("Uses depth from last frame and disables remove rays if the difference is too big");
                    if (mHybridUseTemporalDepthTest)
                    {
                        group2.var("Max depth difference", mHybridTemporalDepthTestPercentage, 0.0f, 1.f, 0.001f);
                        group2.tooltip("Max depth difference. Test: abs(linZ - prevLinZ) < (linZ * maxDepthDiff).");
                    }
                    group2.checkbox("Disable Dynamic Geometry Check", mHybridMaskDisableDynamicGeometryCheck);
                    group2.tooltip(
                        "Remove ray is used on static and dynamic shadows if enables. Not adversed as this produces visible discretization "
                        "artifacts."
                    );
                }
                else
                {
                    if (mHybridUseTemporalDepthTest)
                        mHybridUseTemporalDepthTest = false;
                }

                group2.text("---------- General ----------");
                changed |= group2.checkbox("Use Ray when sample is outside of the mask", mHybridUseRayWhenOutsideMask);
                group2.tooltip("Always uses a ray when the sample is outside of the mask");
                group2.separator();
            }
            mClearHybridMask |= group.button("Clear HybridMask");
            group.tooltip("Clears the mask");
        }
        group.separator();
    }

    return changed;
}
