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
#include "GlassTracer.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "RenderGraph/RenderGraph.h"

namespace
{
    const std::string kVbuffer = "vbuffer";
    const std::string kMotion = "mvec";
    const std::string kColorOut = "color";
    const std::string kDepthOut = "depth";
    // iteration data
    const std::string kNewRayDir = "newRayDir";
    const std::string kLastRayDir = "lastRayDir";
    const std::string kLocalPathLength = "localPathLength";
    const std::string kReflectiveMask = "reflectiveMask";

    const uint32_t kMaxPayloadSizeBytes = 6 * sizeof(float);
    const std::string kProgramRaytraceFile = "RenderPasses/GlassTracer/GlassTracer.rt.slang";
    const std::string kIterationRaytraceFile = "RenderPasses/GlassTracer/IterateMV.rt.slang";
    const std::string kOpticalFlowFile = "RenderPasses/GlassTracer/OpticalFlow.cs.slang";

    const std::string kUseWhitelist = "useWhitelist";
    const std::string kWhitelist = "whitelist";
    const std::string kWhitelistBuffer = "whitelistBuffer"; // GPU Buffer for whitelist

    const std::string kDebug = "debug";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, GlassTracer>();
}

GlassTracer::GlassTracer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    mpSamplePattern = HaltonSamplePattern::create(16);
    mpOpticalFlowPass = ComputePass::create(mpDevice, kOpticalFlowFile, "main");
    mpOpticalFlowPass->getRootVar()["S"] = Sampler::create(mpDevice, Sampler::Desc().setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear));

    // load properties
    for (const auto& [key, value] : props)
    {
        if (key == kUseWhitelist) mUseTransparencyWhitelist = value;
        else if (key == kWhitelist)
        {
            std::stringstream ss;
            std::string svalue = value;
            ss << svalue;
            std::string entry;
            while (std::getline(ss, entry, ','))
            {
                mTransparencyWhitelist.insert(entry);
            }
        }
    }
}

Properties GlassTracer::getProperties() const
{
    Properties props;
    props[kUseWhitelist] = mUseTransparencyWhitelist;
    // convert whitelist into a comma separated string
    std::stringstream ss;
    for (const auto& entry : mTransparencyWhitelist) ss << entry << ",";
    props[kWhitelist] = ss.str();
    return props;
}

RenderPassReflection GlassTracer::reflect(const CompileData& compileData)
{
    uint2 dims = getRenderSize(compileData.defaultTexDims, mRenderScale);

    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addOutput(kVbuffer, "V-buffer").format(HitInfo::kDefaultFormat).texture2D(dims.x, dims.y);
    reflector.addOutput(kMotion, "Motion vector").format(ResourceFormat::RG32Float).flags(RenderPassReflection::Field::Flags::Optional).texture2D(dims.x, dims.y);
    reflector.addOutput(kColorOut, "Final color").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addOutput(kDepthOut, "Depth").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);

    reflector.addOutput(kNewRayDir, "New Ray Direction").format(ResourceFormat::RGBA32Float).texture2D(dims.x, dims.y);
    reflector.addOutput(kLastRayDir, "Last Ray Direction").format(ResourceFormat::RGBA32Float).texture2D(dims.x, dims.y);
    reflector.addOutput(kLocalPathLength, "Local Path Length").format(ResourceFormat::R32Uint).texture2D(dims.x, dims.y); 
    reflector.addOutput(kReflectiveMask, "Reflective Mask").format(ResourceFormat::R32Uint).texture2D(dims.x, dims.y); // TODO higher limit to support path length > 32

    reflector.addOutput(kDebug, "Debug output").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y, 1, 1, mPathLength);
    return reflector;
}

void GlassTracer::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mpVbufferToPosGraph = RenderGraph::create(mpDevice, "VBuffer to Position");
    ref<RenderPass> unpackPass = RenderPass::create("UnpackVBuffer", mpDevice);
    mpVbufferToPosGraph->addPass(unpackPass, "UnpackVBuffer");
    mpVbufferToPosGraph->markOutput("UnpackVBuffer.posW");
    mpVbufferToPosGraph->setScene(mpScene);

    uint2 dims = getRenderSize(compileData.defaultTexDims, mRenderScale);
    auto tmpFbo = Fbo::create2D(mpDevice, dims.x, dims.y, ResourceFormat::RGBA32Float);
    mpVbufferToPosGraph->onResize(tmpFbo.get());
}

void GlassTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (mOptionsChanged)
    {
        auto& dict = renderData.getDictionary();
        auto refreshFlags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        refreshFlags |= RenderPassRefreshFlags::RenderOptionsChanged;
        dict[kRenderPassRefreshFlags] = refreshFlags;
        mOptionsChanged = false;
    }

    auto pVbuffer = renderData.getTexture(kVbuffer);
    auto pMotion = renderData.getTexture(kMotion);
    auto pColor = renderData.getTexture(kColorOut);
    auto pDepth = renderData.getTexture(kDepthOut);
    auto pDebug = renderData.getTexture(kDebug);

    auto pNewRayDir = renderData.getTexture(kNewRayDir);
    auto pLastRayDir = renderData.getTexture(kLastRayDir);
    auto pLocalPathLength = renderData.getTexture(kLocalPathLength);
    auto pReflectiveMask = renderData.getTexture(kReflectiveMask);

    size_t requiredStack = pVbuffer->getWidth() * pVbuffer->getHeight() * std::max(1, mStackSize);
    // number of floats in the stack struct
    uint32_t structSize = 12;
    if (mUseTextureLOD) structSize += 12;
    if (mMotionVector == MotionVector::HalfwayReflection) structSize += 12;
    if (mMotionVector == MotionVector::RayDifferentials) structSize += 16;
    if (mMotionVector == MotionVector::ReverseRayDifferentials) structSize += 39;
    if (!mpStackBuffer || mpStackBuffer->getElementCount() != requiredStack || mpStackBuffer->getElementSize() != structSize * sizeof(float))
    {
        mpStackBuffer = Buffer::createStructured(mpDevice, sizeof(float) * structSize, requiredStack, ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false);
    }

    if (!mpScene)
    {
        pRenderContext->clearTexture(pColor.get(), float4(0, 0, 0, 0));
        return;
    }

    assert(mpProgram);
    assert(mpVars);

    uint2 frameDim = uint2(pVbuffer->getWidth(), pVbuffer->getHeight());
    mpScene->getCamera()->setPatternGenerator(mpSamplePattern, 1.0f / float2(frameDim));

    auto var = mpVars->getRootVar();
    var["gVBuffer"] = pVbuffer;
    var["gMotion"] = pMotion;
    var["gColor"] = pColor;
    var["gDepth"] = pDepth;
    var["gStack"] = mpStackBuffer;
    assert(mpTransparencyWhitelist);
    var["gTransparencyWhitelist"] = mpTransparencyWhitelist;

    // iteration buffers
    var["gNewRayDir"] = pNewRayDir;
    var["gLastRayDir"] = pLastRayDir;
    var["gLocalPathLength"] = pLocalPathLength;
    var["gReflectiveMask"] = pReflectiveMask;

    if (pDebug)
    {
        pRenderContext->clearTexture(pDebug.get(), float4(0, 0, 0, 0));
        var["gDebugTex"] = pDebug;
    }

    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gPathLength"] = mPathLength;
    var["PerFrame"]["gAmbientIntensity"] = mAmbientIntensity;
    var["PerFrame"]["gAnalyticIntensity"] = mAnalyticIntensity;
    var["PerFrame"]["gEmissionIntensity"] = mEmissionIntensity;
    var["PerFrame"]["gEnvmapIntensity"] = mEnvmapIntensity;
    var["PerFrame"]["gPointLightClip"] = mPointLightClip;
    var["PerFrame"]["gShadowLodBias"] = mShadowLodBias;
    var["PerFrame"]["gEnableShadows"] = mEnableShadows ? 1 : 0;
    var["PerFrame"]["gRoughnessCutoff"] = mRoughnessCutoff;
    var["PerFrame"]["gForceMotionVectorCalculation"] = mForceMotionVectorCalculation ? 1 : 0;
    var["PerFrame"]["gMaxStack"] = mStackSize;
    var["PerFrame"]["gTextureGradientScaling"] = std::pow(2.0f, mTextureLodBias);
    var["PerFrame"]["gForceZeroRoughness"] = mForceZeroRoughness ? 1 : 0;

    mpProgram->addDefine("TRANSPARENCY_WHITELIST", mUseTransparencyWhitelist ? "1" : "0");
    mpProgram->addDefine("CULL_BACK_FACES", mCullBackFaces ? "1" : "0");
    mpProgram->addDefine("MVEC", std::to_string(uint32_t(mMotionVector)));
    mpProgram->addDefine("USE_TEXTURE_LOD", mUseTextureLOD ? "1" : "0");
    mpProgram->addDefine("IGNORE_NORMAL_DIFFS", mIgnoreNormalDiffs ? "1" : "0");

    uint3 dispatch = uint3(1);
    dispatch.x = pVbuffer->getWidth();
    dispatch.y = pVbuffer->getHeight();
    {
        FALCOR_PROFILE(pRenderContext, "Primary");
        mpScene->raytrace(pRenderContext, mpProgram.get(), mpVars, dispatch);
    }

    if (mIterationTechnique != IterationTechnique::None && mIterations > 1)
    {
        FALCOR_PROFILE(pRenderContext, "Iterate");
        var = mpIterationVars->getRootVar();

        var["gVBuffer"] = pVbuffer;
        var["gMotion"] = pMotion;
        var["gTransparencyWhitelist"] = mpTransparencyWhitelist;
        var["gReflectiveMask"] = pReflectiveMask;
        var["gLocalPathLength"] = pLocalPathLength;
        var["gNewRayDir"] = pNewRayDir;
        var["gLastRayDir"] = pLastRayDir;

        mpIterationProgram->addDefine("TRANSPARENCY_WHITELIST", mUseTransparencyWhitelist ? "1" : "0");
        mpIterationProgram->addDefine("CULL_BACK_FACES", mCullBackFaces ? "1" : "0");
        mpIterationProgram->addDefine("USE_TEXTURE_LOD", mUseTextureLOD ? "1" : "0");
        mpIterationProgram->addDefine("IT_TECH", std::to_string(uint32_t(mIterationTechnique)));
        mpIterationProgram->addDefine("IGNORE_NORMAL_DIFFS", mIgnoreNormalDiffs ? "1" : "0");

        var["PerFrame"]["gIterations"] = mIterations;
        var["PerFrame"]["gForcePathLength"] = mForceIterationPathLength ? 1 : 0;

        mpScene->raytrace(pRenderContext, mpIterationProgram.get(), mpIterationVars, dispatch);
    }

    // jitter applied in Camera::computeRayPinhole
    const float2 curJitter = float2(-mpScene->getCamera()->getJitterX(), mpScene->getCamera()->getJitterY());
    if(mOpticalFlowTechnique != OpticalFlowTechnique::None)
    {
        // obtain current positions
        ref<Texture> pPosition;
        mpVbufferToPosGraph->setInput("UnpackVBuffer.vbuffer", pVbuffer);
        mpVbufferToPosGraph->execute(pRenderContext);
        pPosition = mpVbufferToPosGraph->getOutput("UnpackVBuffer.posW")->asTexture();
        bool usePrevPos = mpPrevPosition && 
            mpPrevPosition->getWidth() == pPosition->getWidth() && 
            mpPrevPosition->getHeight() == pPosition->getHeight();

        FALCOR_PROFILE(pRenderContext, "OpticalFlow");
        var = mpOpticalFlowPass->getRootVar();
        var["gMotion"] = pMotion;
        var["gCurPos"] = pPosition;
        var["gPrevPos"] = usePrevPos ? mpPrevPosition : pPosition;

        var["PerFrame"]["gFrameDim"] = uint2(dispatch.x, dispatch.y);
        var["PerFrame"]["gIterations"] = mOpticalIterations;
        var["PerFrame"]["gPrevJitter"] = mPrevJitter;
        var["PerFrame"]["gCurJitter"] = curJitter;

        mpOpticalFlowPass->execute(pRenderContext, dispatch);

        // blit cur pos to prev pos
        if (!usePrevPos) mpPrevPosition = Texture::create2D(mpDevice, pPosition->getWidth(), pPosition->getHeight(), pPosition->getFormat(), 1, 1, nullptr, ResourceBindFlags::AllColorViews);
        pRenderContext->blit(pPosition->getSRV(), mpPrevPosition->getRTV());
    }
    mPrevJitter = curJitter;



    // add whitelist to dict
    if (mUseTransparencyWhitelist)
    {
        renderData.getDictionary()[kWhitelist] = mTransparencyWhitelist;
        renderData.getDictionary()[kWhitelistBuffer] = mpTransparencyWhitelist;
    }
    mFrameCount++;
}

void GlassTracer::renderUI(Gui::Widgets& widget)
{
    if (widget.dropdown("Render Scale", mRenderScale))
        requestRecompile();

    bool c = false; // changed

    c |= widget.dropdown("Motion Vectors", mMotionVector);
    c |= widget.checkbox("Force Motion Vector Calculation", mForceMotionVectorCalculation);
    widget.tooltip("Forces motion vector calculation even if neither camera nor vertex moved.");
    c |= widget.checkbox("Ignore Normal Differentials", mIgnoreNormalDiffs);
    widget.tooltip("Ignores normal differentials when computing ray differentials for motion vectors.");

    c |= widget.dropdown("Iteration Technique", mIterationTechnique);

    if (mIterationTechnique != IterationTechnique::None)
    {

        c |= widget.slider("Iterations", mIterations, 1, 20);
        if (mIterations > 1)
        {
            c |= widget.checkbox("Force It. Path Length", mForceIterationPathLength);
            widget.tooltip("If enabled, Paths must have the exact same path length as the original ray.");
        }
    }

    c |= widget.dropdown("OpticalFlow Technique", mOpticalFlowTechnique);
    if (mOpticalFlowTechnique != OpticalFlowTechnique::None)
    {
        c |= widget.slider("Iterations##1", mOpticalIterations, 1, 20);
    }


    if (auto g = widget.group("Scene"))
    {
        c |= widget.checkbox("Use Texture LOD", mUseTextureLOD);
        if (mUseTextureLOD)
        {
            c |= widget.var("Texture LOD Bias", mTextureLodBias, -8.0f, 8.0f, 0.25f);
        }

        c |= widget.checkbox("Cull Back Faces", mCullBackFaces);

        c |= widget.checkbox("Transparency Whitelist", mUseTransparencyWhitelist);
        widget.tooltip("Uses only whitelisted materials for dithering, when enabled. If not whitelisted, the material will use an alpha test.");
        if (mUseTransparencyWhitelist && mpScene)
        {
            auto g2 = widget.group("Whitelist");
            std::string removeEntry;
            // list all material names of the current scene
            for (uint mat = 0; mat < mpScene->getMaterialCount(); ++mat)
            {
                std::string name = mpScene->getMaterial(MaterialID(mat))->getName();
                bool isTransparent = mTransparencyWhitelist.find(name) != mTransparencyWhitelist.end();
                if (g2.checkbox(name.c_str(), isTransparent))
                {
                    if (isTransparent) mTransparencyWhitelist.insert(name);
                    else mTransparencyWhitelist.erase(name);
                    updateWhitelistBuffer();
                }
            }
        }
    }

    if (auto g = widget.group("Lighting"))
    {
        c |= widget.var("Ambient", mAmbientIntensity, 0.0f);
        c |= widget.var("Analytic", mAnalyticIntensity, 0.0f);
        c |= widget.var("Emission", mEmissionIntensity, 0.0f);
        c |= widget.var("Envmap", mEnvmapIntensity, 0.0f);
    }
    if (auto g = widget.group("Shadows"))
    {
        c |= widget.checkbox("Enable Shadows", mEnableShadows);
        c |= widget.var("Point Light Clip", mPointLightClip, 0.0f);
        c |= widget.var("Shadow LOD Bias", mShadowLodBias, -16.0f, 16.0f, 0.5f);
    }

    if (auto g = widget.group("Pathtracer"))
    {
        c |= widget.var("Path Length", mPathLength, 1, 64);
        c |= widget.var("Stack Size", mStackSize, 0, 16);

        c |= widget.var("Roughness Cutoff", mRoughnessCutoff, 0.0f, 1.0f);
        widget.tooltip("Surfaces with lower roughness will not reflect");
        c |= widget.checkbox("Force Zero Rougness", mForceZeroRoughness);
    }

    mOptionsChanged |= c;
}

void GlassTracer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    setupProgram();
    mUseTransparencyWhitelist = updateWhitelistBuffer();
}

void GlassTracer::setupProgram()
{
    if (!mpScene) return;

    auto setup = [&](const std::string& filename, ref<RtProgram>& dstProgram, ref<RtProgramVars>& dstVars)
    {
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());

        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(filename);
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        desc.setShaderModel("6_6");

        ref<RtBindingTable> sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));
        sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));

        dstProgram = RtProgram::create(mpDevice, desc, defines);
        dstVars = RtProgramVars::create(mpDevice, dstProgram, sbt);

        // Bind static resources.
        ShaderVar var = dstVars->getRootVar();
        mpSampleGenerator->setShaderData(var);
    };

    setup(kProgramRaytraceFile, mpProgram, mpVars);
    setup(kIterationRaytraceFile, mpIterationProgram, mpIterationVars);
}

bool GlassTracer::updateWhitelistBuffer()
{
    return updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
}

