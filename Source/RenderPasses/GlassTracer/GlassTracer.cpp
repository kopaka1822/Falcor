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
    const std::string kMotionBackup = "mvecBackup";
    const std::string kMotionErrorMask = "mvecErrorMask"; // indicates where motion could not be reconstructed faithfully
    const std::string kMotionErrorTmp = "mvecErrorTmp"; // intermediate buffer
    const std::string kColorOut = "color";
    const std::string kDepthOut = "depth";
    const std::string kLinearDepthOut = "linearDepth";
    const std::string kPosDiff = "posDiff";
    // iteration data
    const std::string kLastRayDir = "lastRayDir";
    const std::string kLastRayDirPrev = "lastRayDirPrev";
    const std::string kPathLength = "pathLength";
    const std::string kLinearDepthPrev = "linearDepthPrev";
    // optical temporaray
    const std::string kMotionOptical = "mvecOptical"; // mvecs directly after optical flow, without post-processing
    // previous frame data for optical flow
    const std::string kPrevPosition = "prevPosition";
    const std::string kPrevPathLen = "prevPathLen";
    const std::string kIsBackupMotion = "isBackupMotion"; // indicates if mvec == bmvec
    const std::string kIsBackupMotionPong = "isBackupMotionPong";

    const uint32_t kMaxPayloadSizeBytes = 6 * sizeof(float);
    const std::string kProgramRaytraceFile = "RenderPasses/GlassTracer/GlassTracer.rt.slang";
    const std::string kOpticalFlowPosFile = "RenderPasses/GlassTracer/OpticalFlowPos.cs.slang";
    const std::string kOpticalBlurFile = "RenderPasses/GlassTracer/OpticalFlowBlur.cs.slang";
    const std::string kOpticalMedianFile = "RenderPasses/GlassTracer/OpticalFlowMedian.cs.slang";

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

    mpOpticalFlowPosPass = ComputePass::create(mpDevice, kOpticalFlowPosFile, "main");
    mpOpticalBlurPass = ComputePass::create(mpDevice, kOpticalBlurFile, "main");
    mpOpticalMedianPrePass = ComputePass::create(mpDevice, kOpticalMedianFile, "medianPreMain");

    auto linearSampler = Sampler::create(mpDevice, Sampler::Desc()
        .setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear)
        .setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp));

    mpOpticalFlowPosPass->getRootVar()["S"] = linearSampler;
    mpOpticalBlurPass->getRootVar()["S"] = linearSampler;
    mpOpticalMedianPrePass->getRootVar()["S"] = linearSampler;

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
    reflector.addOutput(kMotion, "Motion vector").format(ResourceFormat::RG32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addOutput(kColorOut, "Final color").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addOutput(kDepthOut, "Depth").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addOutput(kLinearDepthOut, "Linear Depth").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);

    reflector.addOutput(kMotionBackup, "Backup Motion vector (first hit)").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::RG32Float).texture2D(dims.x, dims.y);
    reflector.addOutput(kMotionErrorMask, "Motion vector error mask").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::R8Uint).texture2D(dims.x, dims.y);
    reflector.addInternal(kMotionErrorTmp, "Motion vector error mask").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::R8Uint).texture2D(dims.x, dims.y);
    reflector.addOutput(kIsBackupMotion, "mvec == bmvec").format(ResourceFormat::R8Unorm).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addInternal(kIsBackupMotionPong, "pong buffer").format(ResourceFormat::R8Unorm).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);

    // previous frame persistent
    reflector.addInternal(kPrevPosition, "Prev Position").format(ResourceFormat::RGBA32Float).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);
    reflector.addInternal(kPrevPathLen, "Prev Path Length").format(ResourceFormat::R32Uint).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);
    reflector.addInternal(kLastRayDirPrev, "Previous Ray Direction").format(ResourceFormat::RGBA32Float).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);
    reflector.addInternal(kLinearDepthPrev, "Previous Path Length").format(ResourceFormat::R32Float).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);
    // current frame persistent
    reflector.addOutput(kPosDiff, "Length of Position Differential").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);;
    reflector.addOutput(kLastRayDir, "Last Ray Direction").format(ResourceFormat::RGBA32Float).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);
    reflector.addOutput(kPathLength, "Local Path Length").format(ResourceFormat::R32Uint).texture2D(dims.x, dims.y).flags(RenderPassReflection::Field::Flags::Persistent);

    // actual output
    reflector.addOutput(kMotionOptical, "Motion vector (tmp from optical)").format(ResourceFormat::RG32Float).texture2D(dims.x, dims.y);

    reflector.addOutput(kDebug, "Debug output").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y, 1, 1, mPathLength).flags(RenderPassReflection::Field::Flags::Optional);
    return reflector;
}

void GlassTracer::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mpVbufferToPosGraph = RenderGraph::create(mpDevice, "VBuffer to Position");
    ref<RenderPass> unpackPass = RenderPass::create("UnpackVBuffer", mpDevice);
    mpVbufferToPosGraph->addPass(unpackPass, "UnpackVBuffer");
    mpVbufferToPosGraph->markOutput("UnpackVBuffer.posW");
    mpVbufferToPosGraph->markOutput("UnpackVBuffer.prevPosW");
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
    auto pMotionBackup = renderData.getTexture(kMotionBackup);
    auto pMotionErrorMask = renderData.getTexture(kMotionErrorMask);
    auto pMotionErrorTmp = renderData.getTexture(kMotionErrorTmp);
    auto pIsBackupMotion = renderData.getTexture(kIsBackupMotion);
    auto pIsBackupMotionPong = renderData.getTexture(kIsBackupMotionPong);
    auto pColor = renderData.getTexture(kColorOut);
    auto pDepth = renderData.getTexture(kDepthOut);
    auto pLinearDepth = renderData.getTexture(kLinearDepthOut);
    auto pDebug = renderData.getTexture(kDebug);
    auto pPosDiff = renderData.getTexture(kPosDiff);
    auto pLastRayDir = renderData.getTexture(kLastRayDir);
    auto pLocalPathLength = renderData.getTexture(kPathLength);

    auto pMotionOptical = renderData.getTexture(kMotionOptical);

    size_t requiredStack = pVbuffer->getWidth() * pVbuffer->getHeight() * std::max(1, mStackSize);
    // number of floats in the stack struct
    uint32_t structSize = 12;
    if (mUseTextureLOD) structSize += 12;
    if (mMotionVector == MotionVector::HalfwayReflection || mMotionVector == MotionVector::FirstRefractiveHit || mBackupMotionVector == MotionVector::FirstRefractiveHit) structSize += 12;
    if (!mpStackBuffer || mpStackBuffer->getElementCount() != requiredStack || mpStackBuffer->getElementSize() != structSize * sizeof(float))
    {
        mpStackBuffer = Buffer::createStructured(mpDevice, sizeof(float) * structSize, requiredStack, ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false);
    }

    if (!mpScene)
    {
        pRenderContext->clearTexture(pColor.get(), float4(0, 0, 0, 0));
        return;
    }

    // copy resources from last frames before being overwritten
    ref<Texture> pPrevPathLen = renderData.getTexture(kPrevPathLen);
    ref<Texture> pPrevPosition = renderData.getTexture(kPrevPosition);
    ref<Texture> pLastRayDirPrev = renderData.getTexture(kLastRayDirPrev);
    ref<Texture> pLinearDepthPrev = renderData.getTexture(kLinearDepthPrev);
    pRenderContext->blit(pLocalPathLength->getSRV(), pPrevPathLen->getRTV());
    pRenderContext->blit(pLastRayDir->getSRV(), pLastRayDirPrev->getRTV());
    pRenderContext->blit(pLinearDepth->getSRV(), pLinearDepthPrev->getRTV());
    if(mpPositions && mpPositions->getWidth() == pPrevPosition->getWidth() && mpPositions->getHeight() == pPrevPosition->getHeight())
        pRenderContext->blit(mpPositions->getSRV(), pPrevPosition->getRTV());

    assert(mpProgram);
    assert(mpVars);

    uint2 frameDim = uint2(pVbuffer->getWidth(), pVbuffer->getHeight());
    mpScene->getCamera()->setPatternGenerator(mpSamplePattern, 1.0f / float2(frameDim));

    auto var = mpVars->getRootVar();
    var["gVBuffer"] = pVbuffer;
    var["gMotion"] = pMotion;
    var["gBackupMotion"] = pMotionBackup;
    var["gColor"] = pColor;
    var["gDepth"] = pDepth;
    var["gLinearDepth"] = pLinearDepth;
    var["gPosDiff"] = pPosDiff;
    var["gStack"] = mpStackBuffer;
    assert(mpTransparencyWhitelist);
    var["gTransparencyWhitelist"] = mpTransparencyWhitelist;
    var["gLastRayDir"] = pLastRayDir;
    var["gLocalPathLength"] = pLocalPathLength;
    var["gIsBackupMotion"] = pIsBackupMotion; 

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
    var["PerFrame"]["gPreventVbufferCurvedReflection"] = mPreventVbufferCurvedReflection ? 1 : 0;
    var["PerFrame"]["gPreventVbufferRefractiveReflection"] = mPreventVbufferRefractiveReflection ? 1 : 0;
    var["PerFrame"]["gMaxVbufferRefractions"] = mMaxVbufferRefractions;
    var["PerFrame"]["gShadowTransmissionMultiplier"] = mShadowTransmissionMultiplier;

    mpProgram->addDefine("TRANSPARENCY_WHITELIST", mUseTransparencyWhitelist ? "1" : "0");
    mpProgram->addDefine("CULL_BACK_FACES", mCullBackFaces ? "1" : "0");
    mpProgram->addDefine("MVEC", std::to_string(uint32_t(mMotionVector)));
    mpProgram->addDefine("BMVEC", std::to_string(uint32_t(mBackupMotionVector)));
    mpProgram->addDefine("USE_TEXTURE_LOD", mUseTextureLOD ? "1" : "0");
    mpProgram->addDefine("IGNORE_NORMAL_DIFFS", mIgnoreNormalDiffs ? "1" : "0");

    uint3 dispatch = uint3(1);
    dispatch.x = pVbuffer->getWidth();
    dispatch.y = pVbuffer->getHeight();
    {
        FALCOR_PROFILE(pRenderContext, "Primary");
        mpScene->raytrace(pRenderContext, mpProgram.get(), mpVars, dispatch);
    }

    // needs positions for iterations or optical flow?
    bool needPositions = false;
    needPositions |= mOpticalFlowTechnique == OpticalFlowTechnique::LucasKanadePos;
    ref<Texture> pCurPrevPosition; // while mpPosition contains the actual positions, pCurPrevPosition contains the locations where those positions have been in the previous frame
    if (needPositions)
    {
        // obtain current positions
        mpVbufferToPosGraph->setInput("UnpackVBuffer.vbuffer", pVbuffer);
        mpVbufferToPosGraph->execute(pRenderContext);
        mpPositions = mpVbufferToPosGraph->getOutput("UnpackVBuffer.posW")->asTexture();
        pCurPrevPosition = mpVbufferToPosGraph->getOutput("UnpackVBuffer.prevPosW")->asTexture();
    }

    // jitter applied in Camera::computeRayPinhole
    const float2 curJitter = float2(-mpScene->getCamera()->getJitterX(), mpScene->getCamera()->getJitterY());
    if(mOpticalFlowTechnique == OpticalFlowTechnique::LucasKanadePos)
    {
        FALCOR_PROFILE(pRenderContext, "OpticalFlow");

        var = mpOpticalFlowPosPass->getRootVar();
        var["gMotion"] = pMotion;
        var["gBackupMotion"] = pMotionBackup;
        var["gMotionError"] = pMotionErrorMask;
        var["gMotionOut"] = pMotionOptical;
        var["gCurPos"] = pCurPrevPosition;
        var["gPrevPos"] = pPrevPosition;
        var["gPosDiff"] = pPosDiff;
        var["gCurPathLen"] = pLocalPathLength;
        var["gPrevPathLen"] = pPrevPathLen;
        var["gLastRayDir"] = pLastRayDir;
        var["gLastRayDirPrev"] = pLastRayDirPrev;
        var["gLinearDepthPrev"] = pLinearDepthPrev;
        var["gLinearDepth"] = pLinearDepth;
        var["gDebugTex"] = pDebug;

        var["PerFrame"]["gFrameDim"] = uint2(dispatch.x, dispatch.y);
        var["PerFrame"]["gIterations"] = mOpticalIterations;
        var["PerFrame"]["gPrevJitter"] = mPrevJitter;
        var["PerFrame"]["gCurJitter"] = curJitter;
        var["PerFrame"]["gForceMotionVectorCalculation"] = mForceMotionVectorCalculation ? 1 : 0;

        {
            FALCOR_PROFILE(pRenderContext, "Iterations");
            mpOpticalFlowPosPass->execute(pRenderContext, dispatch);
            pRenderContext->uavBarrier(pMotionErrorMask.get());
        }
        // ouput is in pMotionOptical

        if (mUseOpticalMedianPrePass)
        {
            FALCOR_PROFILE(pRenderContext, "Median PrePass");

            // output is in pMotionOptical
            var = mpOpticalMedianPrePass->getRootVar();
            var["gMotion"] = pMotionOptical;
            var["gMotionOut"] = pMotion; // backup data and output
            var["gPathLength"] = pLocalPathLength;
            var["gBackupMotion"] = pMotionBackup;
            var["gMotionError"] = pMotionErrorMask;
            var["gMotionErrorOut"] = pMotionErrorTmp;


            var["PerFrame"]["gFrameDim"] = uint2(dispatch.x, dispatch.y);
            mpOpticalMedianPrePass->execute(pRenderContext, dispatch);

            pMotionErrorMask = pMotionErrorTmp; 
        }
        else pRenderContext->blit(pMotionOptical->getSRV(), pMotion->getRTV());

        // output is in pMotion
        
        // blur
        if (mOpticalBlurRadius > 0)
        {
            FALCOR_PROFILE(pRenderContext, "BilateralBlur");
            // output is in pMotionOptical
            var = mpOpticalBlurPass->getRootVar();
            var["gMotionIn"] = pMotion;
            var["gMotionOut"] = pMotionOptical;
            var["gIsBackupMotionIn"] = pIsBackupMotion;
            var["gIsBackupMotionOut"] = pIsBackupMotionPong;
            var["gMotionError"] = pMotionErrorMask;
            var["gPathLength"] = pLocalPathLength;
            var["gCurPos"] = pCurPrevPosition;
            var["gPrevPos"] = pPrevPosition;
            var["gPosDiff"] = pPosDiff;
            var["gLastRayDir"] = pLastRayDir;

            var["PerFrame"]["gFrameDim"] = int2(dispatch.x, dispatch.y);
            var["PerFrame"]["gDirection"] = int2(1, 0); // first pass must be X
            var["PerFrame"]["gRadius"] = mOpticalBlurRadius;
            var["PerFrame"]["gCompareBilateralOutput"] = mCompareBilateralOutput ? 1 : 0;
            var["PerFrame"]["gPrevJitter"] = mPrevJitter;
            var["PerFrame"]["gCurJitter"] = curJitter;

            pRenderContext->uavBarrier(pMotionErrorMask.get()); // required since previously RW
            mpOpticalBlurPass->execute(pRenderContext, dispatch);
            pRenderContext->uavBarrier(pMotionErrorMask.get());

            // vertical pass
            var["gMotionOut"].setUav(nullptr);
            var["gMotionIn"] = pMotionOptical;
            var["gMotionOut"] = pMotion;
            var["gIsBackupMotionOut"].setUav(nullptr);
            var["gIsBackupMotionIn"] = pIsBackupMotionPong;
            var["gIsBackupMotionOut"] = pIsBackupMotion;
            var["PerFrame"]["gDirection"] = int2(0, 1); // second pass must by Y
            mpOpticalBlurPass->execute(pRenderContext, dispatch);
        }

        // output is in pMotion
        
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

    bool prevFirstHit = mMotionVector == MotionVector::FirstHit;
    c |= widget.dropdown("Motion Vectors", mMotionVector);
    if (prevFirstHit && mMotionVector != MotionVector::FirstHit)
    {
        mBackupMotionVector = MotionVector::FirstRefractiveHit; // force better backup if possible
    }
    
    c |= widget.checkbox("Force Motion Vector Calculation", mForceMotionVectorCalculation);
    widget.tooltip("Forces motion vector calculation even if neither camera nor vertex moved.");

    c |= widget.dropdown("OpticalFlow Technique", mOpticalFlowTechnique);
    if (mOpticalFlowTechnique != OpticalFlowTechnique::None)
    {
        widget.separator();

        Gui::DropdownList backupDropdown = {
            { uint32_t(GlassTracer::MotionVector::FirstHit), "FirstHit" },
            { uint32_t(GlassTracer::MotionVector::FirstRefractiveHit), "FirstRefractiveHit" },
        };

        uint32_t backupSelection = uint32_t(mBackupMotionVector);
        c |= widget.dropdown("Backup Motion Vector", backupDropdown, backupSelection);
        mBackupMotionVector = GlassTracer::MotionVector(backupSelection);
        if (mMotionVector == MotionVector::FirstHit) mBackupMotionVector = MotionVector::FirstHit; // enforce valid selection

        c |= widget.slider("Iterations##1", mOpticalIterations, 1, 20);

        c |= widget.checkbox("Median Filter Pre-Blur", mUseOpticalMedianPrePass);
        c |= widget.slider("Bilateral Blur Radius", mOpticalBlurRadius, 0, 100);
        if(mOpticalBlurRadius > 0)
            c |= widget.checkbox("Compare Bilateral Output", mCompareBilateralOutput);

        widget.separator();
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
        c |= widget.var("Shadow Transmission Mult.", mShadowTransmissionMultiplier, 0.0f, 1.0f);
    }

    if (auto g = widget.group("Pathtracer"))
    {
        c |= widget.var("Path Length", mPathLength, 1, 64);
        c |= widget.var("Stack Size", mStackSize, 0, 16);

        c |= widget.var("Roughness Cutoff", mRoughnessCutoff, 0.0f, 1.0f);
        widget.tooltip("Surfaces with lower roughness will not reflect");
        c |= widget.checkbox("Force Zero Rougness", mForceZeroRoughness);
        c |= widget.checkbox("Disable V-buffer Curved Reflection", mPreventVbufferCurvedReflection);
        widget.tooltip("Prevents storing hit points after curved reflections to be stored in the V-Bffer.");
        c |= widget.checkbox("Disable V-buffer Refractive Reflection", mPreventVbufferRefractiveReflection);
        widget.tooltip("Prevents storing hit points after from reflections if the surface does also refract.");
        c |= widget.var("Max V-buffer Refractions", mMaxVbufferRefractions, 0, 128);
        widget.tooltip("Maximum number of refractions after which a v-buffer write is forced");
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

    auto linearSampler = Sampler::create(mpDevice, Sampler::Desc()
        .setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear)
        .setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp));

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
        var["S"] = linearSampler;
    };

    setup(kProgramRaytraceFile, mpProgram, mpVars);
}

bool GlassTracer::updateWhitelistBuffer()
{
    return updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
}

