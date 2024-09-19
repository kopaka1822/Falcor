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

    std::vector<float> kGraphTestX = {0.0f, 0.2f, 0.4f, 0.7f, 1.f};
    std::vector<float> kGraphTestY = {1.0f, 0.8f, 0.5f, 0.25f, 0.1f}; 

    //UI Graph
    // Colorblind friendly palette.
    const std::vector<uint32_t> kColorPalette = {
        IM_COL32(0x00, 0x49, 0x49, 0xff),
        IM_COL32(0x00, 0x92, 0x92, 0xff),
        IM_COL32(0xff, 0x6d, 0xb6, 0xff),
        IM_COL32(0xff, 0xb6, 0xdb, 0xff),
        IM_COL32(0x49, 0x00, 0x92, 0xff),
        IM_COL32(0x00, 0x6d, 0xdb, 0xff),
        IM_COL32(0xb6, 0x6d, 0xff, 0xff),
        IM_COL32(0x6d, 0xb6, 0xff, 0xff),
        IM_COL32(0xb6, 0xdb, 0xff, 0xff),
        IM_COL32(0x92, 0x00, 0x00, 0xff),
        // Yellow-ish colors don't work well with the highlight color.
        // IM_COL32(0x92, 0x49, 0x00, 0xff),
        // IM_COL32(0xdb, 0x6d, 0x00, 0xff),
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

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    generateAVSM(pRenderContext, renderData);

    traceScene(pRenderContext, renderData);

    generateDebugRefFunction(pRenderContext, renderData);

    mFrameCount++;
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
        mAVSMRebuildProgram = false;
    }

    if (mAVSMTexResChanged)
    {
        mAVSMDepths.clear();
        mAVSMTransmittance.clear();
        mAVSMTexResChanged = false;
    }

    // Create AVSM trace program
    if(!mGenAVSMPip.pProgram){
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderAVSMRay);
        desc.setMaxPayloadSize(kMaxPayloadSizeAVSMPerK * mNumberAVSMSamples);
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

        mGenAVSMPip.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    // Get Analytic light data
    auto& lights = mpScene->getLights();
    // Nothing to do if there are no lights
    if (lights.size() == 0)
        return;

    //Check if resize is neccessary
    bool rebuildAll = false;
    if (mShadowMapMVP.size() != lights.size())
    {
        mShadowMapMVP.resize(lights.size());
        rebuildAll = true;
    }

    //Destroy resources if pass is not enabled or a resize is necessary
    if (rebuildAll) //TODO add global bool
    {
        mAVSMDepths.clear();
        mAVSMTransmittance.clear();
    }

    //Update matrices
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
    

    // Defines
    mGenAVSMPip.pProgram->addDefine("AVSM_DEPTH_BIAS", std::to_string(mDepthBias));
    mGenAVSMPip.pProgram->addDefine("AVSM_NORMAL_DEPTH_BIAS", std::to_string(mNormalDepthBias));
    mGenAVSMPip.pProgram->addDefine("AVSM_USE_TRIANGLE_AREA", mAVSMUseRectArea ? "1" : "0");

    //Create Program Vars
    if (!mGenAVSMPip.pVars)
    {
        mGenAVSMPip.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mGenAVSMPip.pVars = RtProgramVars::create(mpDevice, mGenAVSMPip.pProgram, mGenAVSMPip.pBindingTable);
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
    mTracer.pProgram->addDefine("USE_AVSM", mUseAVSM ? "1" : "0");
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
    }

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

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));
}

void TransparencyPathTracer::generateDebugRefFunction(RenderContext* pRenderContext, const RenderData& renderData)
{
    static const uint elements = 256;
    //Early return
    if (!mGraphUISettings.genBuffers)
        return;

    // Create Debug buffers
    if (mGraphFunctionDatas.size() != mGraphUISettings.numberFunctions)
    {
        if (!mGraphFunctionDatas.empty())
            mGraphFunctionDatas.clear();

        mGraphFunctionDatas.resize(mGraphUISettings.numberFunctions);
        const size_t maxElements = elements * 8; // 256 (elements) x 2 (depth,opacity/transmittance) x 4 (element size)
        std::vector<float> initData(elements * 2, 1.0);
        for (uint i = 0; i < mGraphUISettings.numberFunctions; i++)
        {
            //Name
            if (i == 0)
                mGraphFunctionDatas[i].name = "Reference";
            else
                mGraphFunctionDatas[i].name = "Function" + std::to_string(i);

            //Create GPU buffers
            /*
            mGraphFunctionDatas[i].pPointsBuffer = Buffer::create(
                mpDevice, maxElements, ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None,
                initData.data()
            );*/
            mGraphFunctionDatas[i].pPointsBuffer = Buffer::createStructured(
                mpDevice, sizeof(float), elements * 2, ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, initData.data(), false
            );
            mGraphFunctionDatas[i].pPointsBuffer->setName("GraphDataBuffer_" + mGraphFunctionDatas[i].name);

            //Create Vector
            mGraphFunctionDatas[i].cpuData.resize(elements);
        }
    }

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

        mDebugGetRefFunction.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    }

    // Create Program Vars
    if (!mDebugGetRefFunction.pVars)
    {
        mDebugGetRefFunction.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mDebugGetRefFunction.pVars = RtProgramVars::create(mpDevice, mDebugGetRefFunction.pProgram, mDebugGetRefFunction.pBindingTable);
    }

    FALCOR_ASSERT(mGraphUISettings.selectedLight < mShadowMapMVP.size());
    auto var = mDebugGetRefFunction.pVars->getRootVar();
    var["CB"]["gSelectedPixel"] = mGraphUISettings.selectedPixel;
    var["CB"]["gNear"] = mNearFar.x;
    var["CB"]["gFar"] = mNearFar.y;
    var["CB"]["gSMRes"] = mSMSize;
    var["CB"]["gLightPos"] = mpScene->getLight(mGraphUISettings.selectedLight)->getData().posW;
    var["CB"]["gViewProj"] = mShadowMapMVP[mGraphUISettings.selectedLight].viewProjection;
    var["CB"]["gInvViewProj"] = mShadowMapMVP[mGraphUISettings.selectedLight].invViewProjection;

    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture(); //VBuffer to convert selected pixel to shadow map pixel
    var["gFuncData"] = mGraphFunctionDatas[0].pPointsBuffer;

    mpScene->raytrace(pRenderContext, mDebugGetRefFunction.pProgram.get(), mDebugGetRefFunction.pVars, uint3(1,1, 1));

    //TODO flush and copy the data to the cpu
    const float2* pBuf = static_cast<const float2*>(mGraphFunctionDatas[0].pPointsBuffer->map(Buffer::MapType::Read));
    FALCOR_ASSERT(pBuf);
    std::memcpy(mGraphFunctionDatas[0].cpuData.data(), pBuf, elements);
    mGraphFunctionDatas[0].pPointsBuffer->unmap();

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

void drawBackground(const ImVec2& graphOffset, const ImVec2& graphSize, const ImVec2& size, float borderThickness)
{
    const float textOffset = 9.f;
    // Draw border
    drawLine(ImVec2(graphOffset.x, 0.f), ImVec2(graphOffset.x, graphOffset.y + graphSize.y), borderThickness);
    drawLine(ImVec2(graphOffset.x, graphOffset.y + graphSize.y), ImVec2(size.x, graphOffset.y + graphSize.y), borderThickness);
    // Transparency lines and text
    float textPosY = graphOffset.y - textOffset;
    uint steps = 5;
    float stepSize = 1.0f / steps;
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
}

void TransparencyPathTracer::renderDebugGraph(const ImVec2& size) {

    const ImVec2 graphOffset = ImVec2(35.f, 18.f);
    const ImVec2 graphOffset2 = ImVec2(35.f, 55.f); // Somehow right bottom needs a bigger offset
    const ImVec2 graphSize = ImVec2(size.x - (graphOffset.x + graphOffset2.x), size.y - (graphOffset.y + graphOffset2.y));
    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    mousePos.x -= screenPos.x;
    mousePos.y -= screenPos.y;

    drawBackground(graphOffset, graphSize, size, mGraphUISettings.borderThickness);

    auto& graphVec = mGraphFunctionDatas[0].cpuData;

    const size_t numElements = graphVec.size();
    size_t lastElement = graphVec.size();
    for (uint i = 0; i < graphVec.size(); i++)
    {
        if (graphVec[i].x >= 1.0)
        {
            lastElement = i;
            break;
        }
    }

    //Draw the graph lines as either step or linear function
    if (mGraphUISettings.asStepFuction)
    {
        for (size_t i = 0; i < lastElement - 1; i++)
        {
            ImVec2 a = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
            ImVec2 b = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
            ImVec2 c = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i + 1].y) * graphSize.y);
            drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[0], graphOffset);
            drawLine(b, c, mGraphUISettings.lineThickness, kColorPalette[0], graphOffset);
        }
    }
    else // linear function
    {
        for (size_t i = 0; i < lastElement - 1; i++)
        {
            ImVec2 a = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
            ImVec2 b = ImVec2(graphVec[i + 1].x * graphSize.x, (1.f - graphVec[i + 1].y) * graphSize.y);
            drawLine(a, b, mGraphUISettings.lineThickness, kColorPalette[0], graphOffset);
        }
    }

    //For point highlighting
    std::optional<float> highlightValueD;
    std::optional<float> highlightValueT;
    int highlightIndex = -1;
    //Draw the points
    for (size_t i = 0; i < lastElement; i++)
    {
        ImVec2 el = ImVec2(graphVec[i].x * graphSize.x, (1.f - graphVec[i].y) * graphSize.y);
        el.x += graphOffset.x;
        el.y += graphOffset.y;
        drawCircleFilled(el, mGraphUISettings.radiusSize, kColorPalette[1]);
       
        //Check if the mouse hovers over the point
        const float rad = mGraphUISettings.radiusSize;
        if (mousePos.x >= el.x - rad && mousePos.x < el.x + rad && mousePos.y >= el.y - rad && mousePos.y < el.y + rad)
        {
            highlightIndex = i;
            highlightValueD = graphVec[i].x;
            highlightValueT = graphVec[i].y;
        }
    }
    //Draw highligh circle
    if (highlightIndex >= 0)
    {
        ImVec2 el = ImVec2(graphVec[highlightIndex].x * graphSize.x, (1.f - graphVec[highlightIndex].y) * graphSize.y);
        drawCircleFilled(el, mGraphUISettings.radiusSize, kHighlightColor, graphOffset);
    }
    ImGui::Dummy(size);
    if (ImGui::IsItemHovered() && highlightValueD && highlightValueT)
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

    dirty |= widget.checkbox("Use Adaptive Volumetric Shadow Maps", mUseAVSM);
    if (mUseAVSM)
    {
        if (auto group = widget.group("Adaptive Volumetric Shadow Maps Settings"))
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
            group.checkbox("Use Rect Area Rejection", mAVSMUseRectArea);
            group.tooltip(
                "Uses Rectange Area rejection from Adaptive Transparency with underestimation instead of the triangle rejection of the "
                "original paper."
            );
            group.checkbox("Use PCF", mAVSMUsePCF); //TODO add other kernels
            group.tooltip("Enable 2x2 PCF using gather");
        }
    }

    static bool graphOpenedFirstTime = true;
    static bool graphOpen = false;
    if (!graphOpen)
        graphOpen |= widget.button("Open Transmittance Graph");
    if (graphOpen && (!mGraphFunctionDatas.empty()))
    {
        if (graphOpenedFirstTime)
        {
            ImGui::SetNextWindowSize(ImVec2(300.f, 300.f));
            graphOpenedFirstTime = false;
        }
        
        ImGui::Begin("Test", &graphOpen);
        renderDebugGraph(ImGui::GetWindowSize());
        ImGui::End();
    }

    if (graphOpen)
    {
        if (auto group = widget.group("Graph Settings", true))
        {
            //TODO add mode for shadow map pixel select
            if (mpScene && mpScene->getLightCount() > 0)
                group.slider("Selected Light", mGraphUISettings.selectedLight, 0u, mpScene->getLightCount() - 1);

            if (mGraphUISettings.selectedPixel.x >= 0 && mGraphUISettings.selectedPixel.y >= 0)
                group.text("Selected Pixel :" + std::to_string(mGraphUISettings.selectedPixel.x) + "," + std::to_string(mGraphUISettings.selectedPixel.y));
            else
                group.text("Please select a pixel");

            if (mGraphUISettings.selectPixelButton)
                group.text("Press \"Left Click\" to select a pixel");
            else
                mGraphUISettings.selectPixelButton |= group.button("Select Pixel");

            // TODO legend
            group.text("Legend:");

            group.separator();
            group.text("Settings");
            group.checkbox("As step function", mGraphUISettings.asStepFuction);
            group.var("Point Radius", mGraphUISettings.radiusSize, 1.f, 128.f, 0.1f);
            group.var("Line Thickness", mGraphUISettings.lineThickness, 1.f, 128.f, 0.1f);
            group.var("Border Thickness", mGraphUISettings.borderThickness, 1.f, 128.f, 0.1f);
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

            mTracer.pBindingTable = RtBindingTable::create(2, 2, mpScene->getGeometryCount());
            auto& sbt = mTracer.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("alphaMiss"));
            sbt->setMiss(1, desc.addMiss("shadowMiss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(
                    0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("alphaClosestHit", "alphaAnyHit")
                );
                sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowAnyHit"));
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

        if (mouseEvent.type == MouseEvent::Type::ButtonDown && mouseEvent.button == Input::MouseButton::Left)
        {
            mGraphUISettings.selectedPixel = mouseScreenPos;
            mGraphUISettings.genBuffers = true;
            mGraphUISettings.selectPixelButton = false;
            mGraphUISettings.mouseDown = false;
            return true;
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