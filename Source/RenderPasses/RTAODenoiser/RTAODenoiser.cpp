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

    //TODO: Add toggle option for TSS
    TemporalSupersamplingReverseReproject(pRenderContext, renderData);

    CalculateMeanVariance(pRenderContext, renderData);

    TemporalCacheBlendWithCurrentFrame(pRenderContext, renderData);

    //SmoothVariance(pRenderContext, renderData);

    //ApplyAtrousWaveletTransformFilter(pRenderContext, renderData);

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
        //Rest of the options
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
    //bind internal textures (ping pong for some cached ones)
    uint currentCachedIndex = (mCurrentFrame + 1) % 2;
    uint prevCachedIndex = mCurrentFrame % 2;
    var["gInCachedTspp"] = mCachedTemporalTextures[prevCachedIndex].tspp;
    var["gInCachedValue"] = mCachedTemporalTextures[prevCachedIndex].value;
    var["gInCachedValueSquaredMean"] = mCachedTemporalTextures[prevCachedIndex].valueSqMean;
    var["gInCachedRayHitDepth"] = mCachedTemporalTextures[prevCachedIndex].rayHitDepth;
    var["gOutCachedTspp"] = mCachedTemporalTextures[currentCachedIndex].tspp;

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
    auto varianceOutTex = renderData[kDenoisedOutputName]->asTexture();

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
    }


    uint currentCachedIndex = (mCurrentFrame + 1) % 2;
    uint prevCachedIndex = mCurrentFrame % 2;

    // bind all input and output channels
    ShaderVar var = mpTCacheBlendPass->getRootVar();

    //set minSmoothingfactor here with  mTSS_MaxTspp

    //TODO set only if necessary
    var["CB"].setBlob(mTSSBlurData);

    var["gInVariance"] = aoInputTex;
    var["gLocalMeanVariance"] = mLocalMeanVariance;
    var["gInRayDistance"] = rayDistanceTex;
    var["gInReprojected_Tspp_Value_SquaredMeanValue_RayHitDistance"] = mCachedTsppValueSquaredValueRayHitDistance;

    var["gInOutTspp"] = mCachedTemporalTextures[currentCachedIndex].tspp;
    var["gInOutValue"] = mCachedTemporalTextures[currentCachedIndex].value;
    var["gInOutSquaredMeanValue"] = mCachedTemporalTextures[currentCachedIndex].valueSqMean;
    var["gInOutRayHitDistance"] = mCachedTemporalTextures[currentCachedIndex].rayHitDepth;

    var["gOutVariance"] = varianceOutTex;
    var["gOutBlurStrength"] = mDisocclusionBlurStrength;

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
