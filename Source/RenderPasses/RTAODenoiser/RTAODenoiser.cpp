/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
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
#include "RTAODenoiser.h"
#include <RenderGraph/RenderPassHelpers.h>

const RenderPass::Info RTAODenoiser::kInfo { "RTAODenoiser", "Denoiser for noisy Ray Traced Ambient Occlusion." };

namespace {
    //Shader
    const std::string kTSSReverseReprojectShader = "RenderPasses/RTAODenoiser/TSSReverseReproject.cs.slang";
    const std::string kMeanVarianceShader = "RenderPasses/RTAODenoiser/CalculateMeanVariance.cs.slang";
    const std::string kTSSCacheBlendShader = "RenderPasses/RTAODenoiser/TSSCacheBlend.cs.slang";

    //Inputs
    const std::string kAOInputName = "aoImage";
    const std::string kRayDistanceName = "rayDistance";
    const std::string kNormalInputName = "normal";
    const std::string kDepthInputName = "depth";
    const std::string kLinearDepthInputName = "linearDepth";
    const std::string kMotionVecInputName = "mVec";

    //Outputs
    const std::string kDenoisedOutputName = "denoisedOut";

    //Const
    static const float kInvalidAPCoefficientValue = -1;
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RTAODenoiser::kInfo, RTAODenoiser::create);
}

RTAODenoiser::SharedPtr RTAODenoiser::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new RTAODenoiser());
    return pPass;
}

Dictionary RTAODenoiser::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection RTAODenoiser::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kAOInputName, "Noisy Ambient Occlusion Image");
    reflector.addInput(kRayDistanceName, "Ray distance of the ambient occlusion ray");
    reflector.addInput(kNormalInputName, "World Space Normal");
    reflector.addInput(kDepthInputName, "Camera Depth");
    reflector.addInput(kLinearDepthInputName, "Linear Depth Derivatives");
    reflector.addInput(kMotionVecInputName, "Motion Vector");

    reflector.addOutput(kDenoisedOutputName, "Denoised output image").format(ResourceFormat::R8Unorm);

    return reflector;
}

void RTAODenoiser::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    
    if (!mpScene) {
        //empty output
        pRenderContext->clearTexture(renderData[kDenoisedOutputName]->asTexture().get(), float4(1.0f));
        return;
    }

    if (!mEnabled) {
        //Copy the input AO Image to the output
        //Input and output are on the 0 index for both channels lists
        auto inputTex = renderData[kAOInputName]->asTexture();
        auto outputTex = renderData[kDenoisedOutputName]->asTexture();
        pRenderContext->copyResource(inputTex.get(), outputTex.get());
        return;
    }

    if (mOptionsChange) {
        // adjust resolution
        mTSSRRData.resolution = float2(renderData.getDefaultTextureDims());
        mTSSRRData.invResolution = float2(1.0f) / mTSSRRData.resolution;

        mOptionsChange = false;
    }

    //Set indices for cached resources
    mCurrentCachedIndex = (mCurrentFrame + 1) % 2;
    mPrevCachedIndex = mCurrentFrame% 2;

    //TODO::Create all needed resources here instead

    TemporalSupersamplingReverseReproject(pRenderContext, renderData);

    CalculateMeanVariance(pRenderContext, renderData);

    TemporalCacheBlendWithCurrentFrame(pRenderContext, renderData);

    if(mUseSmoothedVariance)
        SmoothVariance(pRenderContext, renderData);

    ApplyAtrousWaveletTransformFilter(pRenderContext, renderData);

    //BlurDisocclusions(pRenderContext, renderData);

    mCurrentFrame++;
}

void RTAODenoiser::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    dirty |= widget.checkbox("Enable", mEnabled);
    widget.tooltip("If deactivated the input AO image will only be copied to the output");
    //Only show rest if enabled
    if (mEnabled) {

        if (auto group = widget.group("TSS Reverse Reprojection")) {
            dirty |= widget.slider("DepthSigma", mTSSRRData.depthSigma, 0.0f, 10.f);
            widget.tooltip("Sigma used for depth weight");
            dirty |= widget.slider("Num Mantissa Bits Depth", mTSSRRData.numMantissaBits, 1U, 32U);     //Todo set that automaticly
            widget.tooltip("Number of Mantissa the input depth uses");
        }

        dirty |= widget.var("Mean Variance Kernel width", mBilateralFilterKernelWidth, 3U, 9U, 2U);
        widget.tooltip("Kernel width for the mean variance step");
        if (mBilateralFilterKernelWidth % 2 != 1) mBilateralFilterKernelWidth++;    //Make sure it is no invalid filter width

        if (auto group = widget.group("TSS Blur")) {
            dirty |= widget.slider("Max Tspp", mTSS_MaxTspp, 1u, 100u);
            widget.tooltip("Upper Limit for Tspp");
            dirty |= widget.slider("std Dev Gamma", mTSSBlurData.stdDevGamma, 0.1f, 10.f);
            widget.tooltip("Std dev gamma - scales std dev on clamping. Larger values give more clamp tolerance, lower values give less tolerance (i.e. clamp quicker, better for motion).");
            dirty |= widget.checkbox("Clamp Cached Values", mTSSBlurData.clampCachedValues);
            widget.tooltip("Enables or disables clamping for cached values");
            dirty |= widget.slider("min std dev tolerance", mTSSBlurData.clamping_minStdDevTolerance, 0.f, 1.f);
            widget.tooltip("Minimum std.dev used in clamping. Higher values helps prevent clamping, especially on checkerboard 1spp sampling values of ~0.1 prevent random clamping. higher values limit clamping due to true change and increase ghosting.");
            dirty |= widget.slider("Clamp Difference to Tspp Scale", mTSSBlurData.clampDifferenceToTsppScale, 0.f, 10.f);
            widget.tooltip("Higher values helps prevent clamping, especially on checkerboard 1spp sampling values of ~0.1 prevent random clamping. higher values limit clamping due to true change and increase ghosting.");
            dirty |= widget.slider("minTspp", mTSSBlurData.minTsppToUseTemporalVariance, 1u, 40u);
            widget.tooltip("Min tspp necessary for temporal variance to be used");

            dirty |= widget.slider("Blur Max Tspp", mTSSBlurData.blurStrength_MaxTspp, 1u, 100u);
            widget.tooltip("Max Tspp for the blur");
            dirty |= widget.slider("Blur decay strength", mTSSBlurData.blurDecayStrength, 0.1f, 32.f);
            widget.tooltip("Stength of the blur decay");
        }

        dirty |= widget.checkbox("Smooth Variance", mUseSmoothedVariance);
        widget.tooltip("Smoothes variance after TSS pass");
    }
    mOptionsChange = dirty;
}

void RTAODenoiser::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    reset();
}

void RTAODenoiser::reset()
{
    //reset all passes
    mCurrentFrame = 0;
    mpTSSReverseReprojectPass.reset();
    mTSSRRInternalTexReady = false;
    mOptionsChange = true;
}

void RTAODenoiser::TemporalSupersamplingReverseReproject(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("TSSReverseReproject");
    //get render pass input tex
    auto aoInputTex = renderData[kAOInputName]->asTexture();
    auto normalTex = renderData[kNormalInputName]->asTexture();
    auto depthTex = renderData[kDepthInputName]->asTexture();
    auto linearDepthTex = renderData[kLinearDepthInputName]->asTexture();
    auto motionVectorTex = renderData[kMotionVecInputName]->asTexture();

    uint2 frameDim = uint2(aoInputTex->getWidth(), aoInputTex->getHeight());

    //Create compute program if not set
    if (!mpTSSReverseReprojectPass) {
        Program::Desc desc;
        desc.addShaderLibrary(kTSSReverseReprojectShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        //defines.add(mpScene->getSceneDefines());

        mpTSSReverseReprojectPass = ComputePass::create(desc, defines, true);
    }

    //create internal textures if not done before
    if(!mTSSRRInternalTexReady) {
        mPrevFrameNormalDepth = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::RGBA32Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mPrevFrameNormalDepth->setName("RTAODenoiser::CachedNormalDepth");
        FALCOR_ASSERT(mPrevFrameNormalDepth);

        for (uint i = 0; i < 2; i++) {
            mCachedTemporalTextures[i].tspp = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Uint,1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].tspp->setName("RTAODenoiser::Tspp" + std::to_string(i));
            mCachedTemporalTextures[i].value = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Float,1U,1U,nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].value->setName("RTAODenoiser::value" + std::to_string(i));
            mCachedTemporalTextures[i].valueSqMean = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].valueSqMean->setName("RTAODenoiser::valueSqMean" + std::to_string(i));
            mCachedTemporalTextures[i].rayHitDepth = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].rayHitDepth->setName("RTAODenoiser::rayHitDepth" + std::to_string(i));
        }

        //Fill value with invalid value
        for (uint i = 0; i < 2; i++) {
            pRenderContext->clearTexture(mCachedTemporalTextures[i].value.get(), float4(kInvalidAPCoefficientValue, 0, 0, 0));
        }

        mCachedTsppValueSquaredValueRayHitDistance = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::RGBA16Uint, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mCachedTsppValueSquaredValueRayHitDistance->setName("RTAODenoiser::CachedTsppValueSquaredValueRayDistance");
        FALCOR_ASSERT(mCachedTsppValueSquaredValueRayHitDistance);
        //Sampler
        Sampler::Desc desc;
        desc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
        desc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
        mClampSampler = Sampler::create(desc);
        FALCOR_ASSERT(mClampSampler);

        mTSSRRInternalTexReady = true;
    }

    // bind all input and output channels
    ShaderVar var = mpTSSReverseReprojectPass->getRootVar();
    var["StaticCB"].setBlob(mTSSRRData);    //TODO:: Set once and not every frame
    //render pass inputs
    var["gAOTex"] = aoInputTex;
    var["gNormalTex"] = normalTex;
    var["gDepthTex"] = depthTex;
    var["gLinearDepthTex"] = linearDepthTex;
    var["gMVecTex"] = motionVectorTex;

    var["gInCachedTspp"] = mCachedTemporalTextures[mPrevCachedIndex].tspp;
    var["gInCachedValue"] = mCachedTemporalTextures[mPrevCachedIndex].value;
    var["gInCachedValueSquaredMean"] = mCachedTemporalTextures[mPrevCachedIndex].valueSqMean;
    var["gInCachedRayHitDepth"] = mCachedTemporalTextures[mPrevCachedIndex].rayHitDepth;
    var["gOutCachedTspp"] = mCachedTemporalTextures[mCurrentCachedIndex].tspp;

    var["gInOutCachedNormalDepth"] = mPrevFrameNormalDepth;
    var["gOutReprojectedCachedValues"] = mCachedTsppValueSquaredValueRayHitDistance;
    //bind sampler
    var["gSampler"] = mClampSampler;
    
    mpTSSReverseReprojectPass->execute(pRenderContext, uint3(frameDim, 1));
}

//TODO: Add checkerboard sampling part. Must also be supported/ used by the RTAO
void RTAODenoiser::CalculateMeanVariance(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("MeanVariance");
    //get render pass input tex
    auto aoInputTex = renderData[kAOInputName]->asTexture();

    uint2 frameDim = uint2(aoInputTex->getWidth(), aoInputTex->getHeight());

    //create compute pass if invalid
    if (!mpMeanVariancePass) {
        Program::Desc desc;
        desc.addShaderLibrary(kMeanVarianceShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        //defines.add(mpScene->getSceneDefines());

        mpMeanVariancePass = ComputePass::create(desc, defines, true);
    }

    //Create the local mean variance texture
    if (!mLocalMeanVariance) {
        mLocalMeanVariance = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::RG16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mLocalMeanVariance->setName("RTAODenoiser::LocalMeanVariance");
        FALCOR_ASSERT(mLocalMeanVariance);
    }

    // bind all input and output channels
    ShaderVar var = mpMeanVariancePass->getRootVar();

    //update cb (TODO: set only once and if action changed)
    {
        mMeanVarianceData.textureDim = frameDim;
        mMeanVarianceData.kernelWidth = mBilateralFilterKernelWidth;
        mMeanVarianceData.kernelRadius = mMeanVarianceData.kernelWidth >> 1;
    }

    var["CB"].setBlob(mMeanVarianceData);
    var["gInAOTex"] = aoInputTex;
    var["gMeanVar"] = mLocalMeanVariance;

    mpMeanVariancePass->execute(pRenderContext, uint3(frameDim, 1));
}

void RTAODenoiser::TemporalCacheBlendWithCurrentFrame(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Barrier for resources from the last two passes.
    pRenderContext->uavBarrier(mCachedTsppValueSquaredValueRayHitDistance.get());
    pRenderContext->uavBarrier(mLocalMeanVariance.get());

    FALCOR_PROFILE("TSSBlend");
    //get render pass input tex
    auto aoInputTex = renderData[kAOInputName]->asTexture();
    auto rayDistanceTex = renderData[kRayDistanceName]->asTexture();

    auto debugOutScreenVal = renderData[kDenoisedOutputName]->asTexture();

    uint2 frameDim = uint2(aoInputTex->getWidth(), aoInputTex->getHeight());

    //create compute pass if invalid
    if (!mpTCacheBlendPass) {
        Program::Desc desc;
        desc.addShaderLibrary(kTSSCacheBlendShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        //defines.add(mpScene->getSceneDefines());

        mpTCacheBlendPass = ComputePass::create(desc, defines, true);
    }

    //Create texture if not set before
    if (!mDisocclusionBlurStrength) {
        mDisocclusionBlurStrength = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mDisocclusionBlurStrength->setName("RTAODenoiser::DisocclusionBlurStrength");
        FALCOR_ASSERT(mDisocclusionBlurStrength);
        mVarianceRawTex = Texture::create2D(frameDim.x, frameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mVarianceRawTex->setName("RTAODenoiser::VarianceRaw");
    }

    // bind all input and output channels
    ShaderVar var = mpTCacheBlendPass->getRootVar();

    //set minSmoothingfactor here with  mTSS_MaxTspp
    mTSSBlurData.minSmoothingFactor = 1.f / mTSS_MaxTspp;

    //TODO set only if necessary
    var["CB"].setBlob(mTSSBlurData);

    var["gInVariance"] = aoInputTex;
    var["gLocalMeanVariance"] = mLocalMeanVariance;
    var["gInRayDistance"] = rayDistanceTex;
    var["gInReprojected_Tspp_Value_SquaredMeanValue_RayHitDistance"] = mCachedTsppValueSquaredValueRayHitDistance;

    var["gInOutTspp"] = mCachedTemporalTextures[mCurrentCachedIndex].tspp;
    var["gInOutValue"] = mCachedTemporalTextures[mCurrentCachedIndex].value;
    var["gInOutSquaredMeanValue"] = mCachedTemporalTextures[mCurrentCachedIndex].valueSqMean;
    var["gInOutRayHitDistance"] = mCachedTemporalTextures[mCurrentCachedIndex].rayHitDepth;

    var["gOutVariance"] = mVarianceRawTex;
    var["gOutBlurStrength"] = mDisocclusionBlurStrength;

    var["gDebugOut"] = debugOutScreenVal;

    mpTCacheBlendPass->execute(pRenderContext, uint3(frameDim, 1));
}

void RTAODenoiser::SmoothVariance(RenderContext* pRenderContext, const RenderData& renderData)
{
    //TODO
}

void RTAODenoiser::ApplyAtrousWaveletTransformFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    //TODO
}

void RTAODenoiser::BlurDisocclusions(RenderContext* pRenderContext, const RenderData& renderData)
{
    //TODO
}
