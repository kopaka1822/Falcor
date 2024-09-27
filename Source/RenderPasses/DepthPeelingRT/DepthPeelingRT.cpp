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
#include "DepthPeelingRT.h"

namespace
{
    const std::string kDepthIn = "linearZ";
    const std::string kDepthOut = "depthOut";
    const std::string kRayShader = "RenderPasses/DepthPeelingRT/DepthPeelingRT.rt.slang";

    const std::string kCullMode = "CullMode";
    const std::string kAlphaTest = "AlphaTest";
    const std::string kJitter = "Jitter";
    const std::string kGuardBand = "GuardBand";
    const std::string kMaxCount = "MaxCount";

    const Gui::DropdownList kCullModeList =
    {
        { (uint32_t)RasterizerState::CullMode::None, "None" },
        { (uint32_t)RasterizerState::CullMode::Back, "Back" },
        { (uint32_t)RasterizerState::CullMode::Front, "Front" },
    };
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, DepthPeelingRT>();
}

DepthPeelingRT::DepthPeelingRT(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    for(const auto& [key, value] : props)
    {
        if(key == kCullMode) mCullMode = value;
        else if(key == kAlphaTest) mAlphaTest = value;
        else if(key == kJitter) mJitter = value;
        else if(key == kGuardBand) mGuardBand = value;
        else if(key == kMaxCount) mMaxCount = value;
        else logWarning("Unknown field '" + key + "' in DepthPeelingRT pass");
    }
}

Properties DepthPeelingRT::getProperties() const
{
    Properties p;
    p[kCullMode] = mCullMode;
    p[kAlphaTest] = mAlphaTest;
    p[kJitter] = mJitter;
    p[kGuardBand] = mGuardBand;
    p[kMaxCount] = mMaxCount;
    return p;
}

RenderPassReflection DepthPeelingRT::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kDepthIn, "linear (primary) depth map").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addOutput(kDepthOut, "secondary depth map").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::R32Float);
    return reflector;
}

void DepthPeelingRT::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepthIn = renderData[kDepthIn]->asTexture();
    auto pDepthOut = renderData[kDepthOut]->asTexture();

    if(!mpRayProgram)
    {
        auto defines = mpScene->getSceneDefines();
        auto rayConeSpread = mpScene->getCamera()->computeScreenSpacePixelSpreadAngle(renderData.getDefaultTextureDims().y);
        defines.add("RAY_CONE_SPREAD", std::to_string(rayConeSpread));
        defines.add("USE_ALPHA_TEST", mAlphaTest ? "1" : "0");
        defines.add("SD_JITTER", mJitter ? "1" : "0");
        defines.add("CULL_MODE_RAY_FLAG", RasterizerState::CullModeToRayFlag(mCullMode));
        defines.add("GUARD_BAND", std::to_string(mGuardBand));
        defines.add("MAX_COUNT", std::to_string(mMaxCount));

        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kRayShader);
        desc.setMaxPayloadSize(4 * sizeof(float)); // TODO adjust payload size
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel("6_5");

        auto sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));
        sbt->setHitGroup(0, mpScene->getGeometryIDs(GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        // TODO add remaining primitives
        mpRayProgram = RtProgram::create(mpDevice, desc, defines);
        mRayVars = RtProgramVars::create(mpDevice, mpRayProgram, sbt);
        auto vars = mRayVars->getRootVar();
        // nearest sampler (interpolating depth is usually error prone near dicontinues)
        vars["S"] = Sampler::create(mpDevice, Sampler::Desc().setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point));
    }

    auto maxDepth = renderData.getDictionary().getValue("MAX_DEPTH", std::numeric_limits<float>::max());
    maxDepth = std::min(maxDepth, mpScene->getCamera()->getFarPlane());
    mRayVars->getRootVar()["PerFrameCB"]["maxDepth"] = maxDepth;
    mRayVars->getRootVar()["depthInTex"] = pDepthIn;
    mRayVars->getRootVar()["depthOutTex"] = pDepthOut;
    

    mpScene->raytrace(pRenderContext, mpRayProgram.get(), mRayVars, uint3(pDepthOut->getWidth(), pDepthOut->getHeight(), 1));
}

void DepthPeelingRT::renderUI(Gui::Widgets& widget)
{
    if (widget.checkbox("Alpha Test", mAlphaTest))
        requestRecompile();

    uint32_t cullMode = (uint32_t)mCullMode;
    if(widget.dropdown("Cull Mode", kCullModeList, cullMode))
    {
        mCullMode = (RasterizerState::CullMode)cullMode;
        requestRecompile();
    }
}

void DepthPeelingRT::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mpRayProgram.reset();
}
