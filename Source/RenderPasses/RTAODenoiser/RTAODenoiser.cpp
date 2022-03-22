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
#include "Utils/Math/FalcorMath.h"

const RenderPass::Info RTAODenoiser::kInfo { "RTAODenoiser", "Denoiser for noisy Ray Traced Ambient Occlusion." };

namespace {
    //Shader
    const std::string kTSSReverseReprojectShader = "RenderPasses/RTAODenoiser/TSSReverseReproject.cs.slang";
    const std::string kMeanVarianceShader = "RenderPasses/RTAODenoiser/CalculateMeanVariance.cs.slang";
    const std::string kTSSCacheBlendShader = "RenderPasses/RTAODenoiser/TSSCacheBlend.cs.slang";
    const std::string kSmoothVarianceShader = "RenderPasses/RTAODenoiser/SmoothVariance.cs.slang";
    const std::string kAtrousWaveletTransformShader = "RenderPasses/RTAODenoiser/AtrousWaveletTransformFilter.cs.slang";
    const std::string kBlurOcclusionShader = "RenderPasses/RTAODenoiser/BlurOcclusion.cs.slang";
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

    static const Gui::DropdownList kFilterDropdownList{
        {0 , "Gaussian 3x3"},
        {1 , "Gaussian 5x5"},
    };
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
        pRenderContext->copyResource(outputTex.get(), inputTex.get());
        return;
    }
           

    //check if resolution has changed
    {
        auto inputTex = renderData[kAOInputName]->asTexture();
        uint2 currTexDim = uint2(inputTex->getWidth(), inputTex->getHeight());
        if (mFrameDim.x != currTexDim.x || mFrameDim.y != currTexDim.y) {
            mFrameDim = currTexDim;
            resetTextures();
            mOptionsChange = true;
        }
    }

    /// Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
    
    mOptionsChange |= static_cast<uint>(flags & RenderPassRefreshFlags::RenderOptionsChanged) != 0;
    if (mOptionsChange) {
        //Set out refresh flags as formats could have changed
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;

        // adjust resolution
        mFrameDim = renderData.getDefaultTextureDims();

        mTSSRRData.resolution = float2(mFrameDim);
        mTSSRRData.invResolution = float2(1.0f) / mTSSRRData.resolution;

        //get depth format mantissa bits
        auto depthTex = renderData[kDepthInputName]->asTexture();
        auto dFormat = depthTex->getFormat();
        switch (dFormat) {
            case ResourceFormat::D32Float:
            case ResourceFormat::D32FloatS8X24:
                mMantissaBits = 23;
                break;
            case ResourceFormat::D16Unorm:
                mMantissaBits = 10;
                break;
            case ResourceFormat::D24UnormS8:
                mMantissaBits = 16;
                break;
            default:
                mMantissaBits = 10;
        }

        mOptionsChange = false;
        mSetConstantBuffers = true;
    }

    //Set indices for cached resources
    mCurrentCachedIndex = (mCurrentFrame + 1) % 2;
    mPrevCachedIndex = mCurrentFrame% 2;
    
    //All render passes
    TemporalSupersamplingReverseReproject(pRenderContext, renderData);

    CalculateMeanVariance(pRenderContext, renderData);

    TemporalCacheBlendWithCurrentFrame(pRenderContext, renderData);

    if(mUseSmoothedVariance)
        SmoothVariance(pRenderContext, renderData);

    ApplyAtrousWaveletTransformFilter(pRenderContext, renderData);

    if(mEnableDisocclusionBlur)
        BlurDisocclusions(pRenderContext, renderData);

    if (mSetConstantBuffers) mSetConstantBuffers = false;
    mCurrentFrame++;
}

void RTAODenoiser::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    dirty |= widget.checkbox("Enable", mEnabled);
    widget.tooltip("If deactivated the input AO image will only be copied to the output");
    //Only show rest if enabled
    if (mEnabled) {
        dirty |= widget.var("Ray Max THit", mRadiusRTAO, 0.01f, FLT_MAX, 0.01f);
        widget.tooltip("The sample radius from RTAO. Has to be the same as RTAO pass for optimal results");


        if (auto group = widget.group("TSS Reverse Reprojection")) {
            dirty |= widget.slider("DepthSigma", mTSSRRData.depthSigma, 0.0f, 10.f);
            widget.tooltip("Sigma used for depth weight");
        }

        if (auto group = widget.group("Mean Variance")) {
            dirty |= widget.var("Mean Variance Kernel width", mBilateralFilterKernelWidth, 3U, 9U, 2U);
            widget.tooltip("Kernel width for the mean variance step");
            if (mBilateralFilterKernelWidth % 2 != 1) mBilateralFilterKernelWidth++;    //Make sure it is no invalid filter width
        }

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
        widget.tooltip("Smoothes variance after TSS Blur pass with a 3x3 Gaussian Filter using Bilinear Filtering");

        if (auto group = widget.group("AtrousWavleTransformFilter")) {
            bool changeFilter = widget.dropdown("Filter", kFilterDropdownList, mCurrentAtrousWaveletFilter);
            widget.tooltip("Changing filter leads to recompilation of the shader");
            if (changeFilter)
                mpAtrousWaveletTransformFilterPass.reset();
            dirty |= changeFilter;

            dirty |= widget.var("Depth weight cutoff", mAtrousWavletData.depthWeightCutoff, 0.0f, 2.0f, 0.1f);
            widget.tooltip("Cutoff for the depth weigths");
            dirty |= widget.checkbox("Adaptive Kernel Size", mAtrousWavletData.useAdaptiveKernelSize);
            widget.tooltip("Enables adaptive filter Kernel sizes");

            dirty |= widget.checkbox("Enable Rotating Kernel", mRotateKernelEnable);
            widget.tooltip("Enables a rotating kernel");
            if (mRotateKernelEnable) {
                dirty |= widget.var("Rotate Kernel Cycles", mRotateKernelNumCycles, 3u, 10u, 1u);
                widget.tooltip("Set the numbers the kernel needs for a full rotation");
            }

            dirty |= widget.var("Ray Hit Distance Scale Factor", mRayHitDistanceScaleFactor, 0.001f, 0.1f, 0.001f);
            widget.tooltip("");
            dirty |= widget.var("Ray Hit Distance Scale Exponent", mRayHitDistanceScaleExponent, 1.f, 5.f, 0.1f);
            widget.tooltip("");
            dirty |= widget.var("Minimum Kernel Width", mAtrousWavletData.minKernelWidth, 3u, 100u, 1u);
            widget.tooltip("Minimum Kernel width in pixel");
            dirty |= widget.var("Max Kernel Width %", mFilterMaxKernelWidthPercentage, 0.f, 100.f, 0.1f);
            widget.tooltip("Percentage how wide a kernel can get depending on the screen width.");

            dirty |= widget.var("Min Variance to denoise", mAtrousWavletData.minVarianceToDenoise, 0.0f, 1.0f, 0.01f);
            widget.tooltip("Minimum Variance needed to denoise for a pixel.");
            dirty |= widget.var("Value sigma", mAtrousWavletData.valueSigma, 0.0f, 30.0f, 0.1f);
            widget.tooltip("Sigma for the AO value weight");
            dirty |= widget.var("Depth sigma", mAtrousWavletData.depthSigma, 0.0f, 10.f, 0.1f);
            widget.tooltip("Sigma for the depth weight");
            dirty |= widget.var("Normal Sigma", mAtrousWavletData.normalSigma, 0.0f, 256.f, 4.f);
            widget.tooltip("Sigma for the normal weight");
        }

        if (auto group = widget.group("Blur Occlusion")) {
            dirty |= widget.checkbox("Enable", mEnableDisocclusionBlur);
            widget.tooltip("Enables the Disocclusion Blur pass");
            if (mEnableDisocclusionBlur) {
                bool changeFilter = widget.dropdown("Filter", kFilterDropdownList, mCurrentDisocclusionFilter);
                widget.tooltip("Changing filter leads to recompilation of the shader");
                if (changeFilter)
                    mpBlurDisocclusionsPass.reset();
                dirty |= changeFilter;

                dirty |= widget.var("Blur Passes", mNumBlurPasses, 0u,6u,1u);
                widget.tooltip("Number of blur passes. Each pass is a compute dispatch");
            }
        }

        //mResetTex |= widget.button("Reset textures");
    }

    mOptionsChange = dirty;
}

void RTAODenoiser::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    if (mpScene) {
        reset();
    }

    mpScene = pScene;
    
}

void RTAODenoiser::resetTextures()
{
    mPrevFrameNormalDepth.reset();
    mCachedTsppValueSquaredValueRayHitDistance.reset();
    mVarianceRawTex.reset();
    mVarianceSmoothTex.reset();
    mLocalMeanVariance.reset();
    mDisocclusionBlurStrength.reset();
    for (uint i = 0; i < 2; i++) {
        mCachedTemporalTextures[i].tspp.reset();
        mCachedTemporalTextures[i].rayHitDepth.reset();
        mCachedTemporalTextures[i].value.reset();
        mCachedTemporalTextures[i].valueSqMean.reset();
    }
    mTSSRRInternalTexReady = false;
}

void RTAODenoiser::reset()
{
    //reset all passes
    mCurrentFrame = 0;
    mpTSSReverseReprojectPass.reset();
    mpMeanVariancePass.reset();
    mpTCacheBlendPass.reset();
    mpGaussianSmoothPass.reset();
    mpAtrousWaveletTransformFilterPass.reset();
    mpBlurDisocclusionsPass.reset();
    resetTextures();
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
        mPrevFrameNormalDepth = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mPrevFrameNormalDepth->setName("RTAODenoiser::CachedNormalDepth");
        FALCOR_ASSERT(mPrevFrameNormalDepth);

        for (uint i = 0; i < 2; i++) {
            mCachedTemporalTextures[i].tspp = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Uint,1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].tspp->setName("RTAODenoiser::Tspp" + std::to_string(i));
            mCachedTemporalTextures[i].value = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float,1U,1U,nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].value->setName("RTAODenoiser::value" + std::to_string(i));
            mCachedTemporalTextures[i].valueSqMean = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].valueSqMean->setName("RTAODenoiser::valueSqMean" + std::to_string(i));
            mCachedTemporalTextures[i].rayHitDepth = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
            mCachedTemporalTextures[i].rayHitDepth->setName("RTAODenoiser::rayHitDepth" + std::to_string(i));
        }

        //Fill value with invalid value
        for (uint i = 0; i < 2; i++) {
            pRenderContext->clearTexture(mCachedTemporalTextures[i].value.get(), float4(kInvalidAPCoefficientValue, 0, 0, 0));
            pRenderContext->clearTexture(mCachedTemporalTextures[i].tspp.get(), uint4(mTSS_MaxTspp, 0, 0, 0));
        }

        mCachedTsppValueSquaredValueRayHitDistance = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::RGBA16Uint, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
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

    if (mSetConstantBuffers) {
        mTSSRRData.numMantissaBits = mMantissaBits;
        var["StaticCB"].setBlob(mTSSRRData);    //TODO:: Set once and not every frame
    }
    //render pass inputs
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
    
    mpTSSReverseReprojectPass->execute(pRenderContext, uint3(mFrameDim, 1));
}

//TODO: Add checkerboard sampling part. Must also be supported/ used by the RTAO
void RTAODenoiser::CalculateMeanVariance(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("MeanVariance");
    //get render pass input tex
    auto aoInputTex = renderData[kAOInputName]->asTexture();

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
        mLocalMeanVariance = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::RG16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mLocalMeanVariance->setName("RTAODenoiser::LocalMeanVariance");
        FALCOR_ASSERT(mLocalMeanVariance);
    }

    // bind all input and output channels
    ShaderVar var = mpMeanVariancePass->getRootVar();

    //update cb
    if(mSetConstantBuffers) {
        mMeanVarianceData.textureDim = mFrameDim;
        mMeanVarianceData.kernelWidth = mBilateralFilterKernelWidth;
        mMeanVarianceData.kernelRadius = mMeanVarianceData.kernelWidth >> 1;
        var["CB"].setBlob(mMeanVarianceData);
    }
    
    var["gInAOTex"] = aoInputTex;
    var["gMeanVar"] = mLocalMeanVariance;

    mpMeanVariancePass->execute(pRenderContext, uint3(mFrameDim, 1));
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
        mDisocclusionBlurStrength = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mDisocclusionBlurStrength->setName("RTAODenoiser::DisocclusionBlurStrength");
        FALCOR_ASSERT(mDisocclusionBlurStrength);
        mVarianceRawTex = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mVarianceRawTex->setName("RTAODenoiser::VarianceRaw");
    }

    // bind all input and output channels
    ShaderVar var = mpTCacheBlendPass->getRootVar();

    //set minSmoothingfactor here with  mTSS_MaxTspp
    mTSSBlurData.minSmoothingFactor = 1.f / mTSS_MaxTspp;

    //set constant buffer
    if(mSetConstantBuffers)
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

    mpTCacheBlendPass->execute(pRenderContext, uint3(mFrameDim, 1));

    pRenderContext->uavBarrier(mVarianceRawTex.get());  //Is needed in the next render pass (the optional and mandatary one)
}

void RTAODenoiser::SmoothVariance(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("SmoothVariance");
   

    //create compute pass if invalid
    if (!mpGaussianSmoothPass) {
        Program::Desc desc;
        desc.addShaderLibrary(kSmoothVarianceShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        //defines.add(mpScene->getSceneDefines());

        mpGaussianSmoothPass = ComputePass::create(desc, defines, true);
    }

    //Create variance tex and sampler if pass is called the first time
    if (!mVarianceSmoothTex) {
        mVarianceSmoothTex = Texture::create2D(mFrameDim.x, mFrameDim.y, ResourceFormat::R16Float, 1U, 1U, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mVarianceSmoothTex->setName("RTAODenoiser::VarianceSmooth");
        FALCOR_ASSERT(mVarianceSmoothTex);
        //Sampler
        Sampler::Desc desc;
        desc.setAddressingMode(Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror);
        desc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);
        mMirrorSampler = Sampler::create(desc);
        FALCOR_ASSERT(mMirrorSampler);
    }

    // bind all input and output channels
    ShaderVar var = mpGaussianSmoothPass->getRootVar();

    if (mSetConstantBuffers) {
        var["CB"]["gTextureDims"] = mFrameDim;
        var["CB"]["gInvTextureDims"] = 1.0f / float2(mFrameDim);
    }
    
    var["gSampler"] = mMirrorSampler;
    var["gInput"] = mVarianceRawTex;
    var["gOutput"] = mVarianceSmoothTex;


    mpGaussianSmoothPass->execute(pRenderContext, uint3(mFrameDim, 1));

    pRenderContext->uavBarrier(mVarianceSmoothTex.get());  //Is needed in the next render pass (the optional and mandatary one)
}

void RTAODenoiser::ApplyAtrousWaveletTransformFilter(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("AtrousWaveletTransform");
    //Barriers
    pRenderContext->uavBarrier(mCachedTemporalTextures[mCurrentCachedIndex].rayHitDepth.get());
    pRenderContext->uavBarrier(mCachedTemporalTextures[mCurrentCachedIndex].value.get());
    pRenderContext->uavBarrier(mCachedTemporalTextures[mCurrentCachedIndex].tspp.get());

    //Render Pass In/Out Tex
    auto normalTex = renderData[kNormalInputName]->asTexture();
    auto depthTex = renderData[kDepthInputName]->asTexture();
    auto linearDepthTex = renderData[kLinearDepthInputName]->asTexture();
    auto denoisedOutTex = renderData[kDenoisedOutputName]->asTexture();

    //create compute pass if invalid
    if (!mpAtrousWaveletTransformFilterPass) {
        Program::Desc desc;
        desc.addShaderLibrary(kAtrousWaveletTransformShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        if(mCurrentAtrousWaveletFilter == static_cast<uint>(Filter::Gauss3x3))
            defines.add("USE_GAUSSIAN_3X3");
        else {
            defines.add("USE_GAUSSIAN_5X5");
        }
        //defines.add(mpScene->getSceneDefines());

        mpAtrousWaveletTransformFilterPass = ComputePass::create(desc, defines, true);
    }
    
    //Calc buffers vars
    float kernelRadiusLerCoef = 0;
    if (mRotateKernelEnable) {
        uint i = mCurrentFrame % mRotateKernelNumCycles;
        kernelRadiusLerCoef = i / static_cast<float>(mRotateKernelNumCycles);
    }

    float rayHitDistanceScaleFactor = 22 / mRadiusRTAO * mRayHitDistanceScaleFactor;
    auto lerp = [](float a, float b, float t) {return a + t * (b - a); };
    auto relativeCoef = [](float a, float _min, float _max) {
        float _a = std::max(_min, std::min(_max, a));
        return (_a - _min) / (_max - _min);
    };
    float rayHitDistanceScaleExponent = lerp(1, mRayHitDistanceScaleExponent, relativeCoef(mRadiusRTAO, 4, 22));

    float fovY = focalLengthToFovY(mpScene->getCamera()->getFocalLength(), Camera::kDefaultFrameHeight);

    //Set uniform data (has to be set every frame)
    {
        mAtrousWavletData.textureDim = mFrameDim;
        mAtrousWavletData.fovy = fovY;
        mAtrousWavletData.kernelRadiusLerfCoef = kernelRadiusLerCoef;
        mAtrousWavletData.maxKernelWidth = static_cast<uint>((mFilterMaxKernelWidthPercentage/100) * mFrameDim.x);
        mAtrousWavletData.rayHitDistanceToKernelWidthScale = rayHitDistanceScaleFactor;
        mAtrousWavletData.rayHitDistanceToKernelSizeScaleExponent = rayHitDistanceScaleExponent;
        mAtrousWavletData.DepthNumMantissaBits = mMantissaBits;
    }
    
    // bind all input and output channels
    ShaderVar var = mpAtrousWaveletTransformFilterPass->getRootVar();

    var["CB"].setBlob(mAtrousWavletData);
    var["gInValue"] = mCachedTemporalTextures[mCurrentCachedIndex].value;
    var["gInNormal"] = normalTex;
    var["gInDepth"] = depthTex;
    var["gInVariance"] = mUseSmoothedVariance ? mVarianceSmoothTex : mVarianceRawTex;
    var["gInRayHitDistance"] = mCachedTemporalTextures[mCurrentCachedIndex].rayHitDepth;
    var["gInLinearZ"] = linearDepthTex;
    var["gInTspp"] = mCachedTemporalTextures[mCurrentCachedIndex].tspp;

    var["gOutValue"] = denoisedOutTex;

    mpAtrousWaveletTransformFilterPass->execute(pRenderContext, uint3(mFrameDim, 1));
    
}

void RTAODenoiser::BlurDisocclusions(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE("BlurDisocclusion");
    //Render Pass In/Out Tex
    auto depthTex = renderData[kDepthInputName]->asTexture();
    auto denoisedOutTex = renderData[kDenoisedOutputName]->asTexture();

    //create compute pass if invalid
    if (!mpBlurDisocclusionsPass) {
        Program::Desc desc;
        desc.addShaderLibrary(kBlurOcclusionShader).csEntry("main").setShaderModel("6_5");
        //desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add("INVALID_AO_COEFFICIENT_VALUE", std::to_string(kInvalidAPCoefficientValue));
        if (mCurrentDisocclusionFilter == static_cast<uint>(Filter::Gauss3x3))
            defines.add("USE_GAUSSIAN_3X3");
        else {
            defines.add("USE_GAUSSIAN_5X5");
        }

        mpBlurDisocclusionsPass = ComputePass::create(desc, defines, true);
    }

    // bind all input and output channels
    ShaderVar var = mpBlurDisocclusionsPass->getRootVar();
    if(mSetConstantBuffers)
        var["CB"]["gTextureDims"] = mFrameDim;

    var["gInDepth"] = depthTex;
    var["gInBlurStrength"] = mDisocclusionBlurStrength;
    var["gInOutValue"] = denoisedOutTex;
 
    uint filterStep = 1;
    uint numPasses = mNumBlurPasses;
    for (uint i = 0; i < numPasses; i++)
    {
        //Barriers for output as it is written every pass
        pRenderContext->uavBarrier(denoisedOutTex.get());
        var["CB"]["gFilterStep"] = filterStep;

        mpBlurDisocclusionsPass->execute(pRenderContext, uint3(mFrameDim, 1));

        filterStep *= 2;
    }

}
