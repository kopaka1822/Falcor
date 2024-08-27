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
#include "Falcor.h"
#include "Core/API/NativeHandleTraits.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include "NRDPass.h"
#include "RenderPasses/Shared/Denoising/NRDConstants.slang"

namespace
{
    const char kShaderPackRadiance[] = "RenderPasses/NRDPass/PackRadiance.cs.slang";

    // Input buffer names.
    const char kInputDiffuseRadianceHitDist[] = "diffuseRadianceHitDist";
    const char kInputSpecularRadianceHitDist[] = "specularRadianceHitDist";
    const char kInputSpecularHitDist[] = "specularHitDist";
    const char kInputMotionVectors[] = "mvec";
    const char kInputNormalRoughnessMaterialID[] = "normWRoughnessMaterialID";
    const char kInputViewZ[] = "viewZ";
    const char kInputDeltaPrimaryPosW[] = "deltaPrimaryPosW";
    const char kInputDeltaSecondaryPosW[] = "deltaSecondaryPosW";

    // Output buffer names.
    const char kOutputFilteredDiffuseRadianceHitDist[] = "filteredDiffuseRadianceHitDist";
    const char kOutputFilteredSpecularRadianceHitDist[] = "filteredSpecularRadianceHitDist";
    const char kOutputReflectionMotionVectors[] = "reflectionMvec";
    const char kOutputDeltaMotionVectors[] = "deltaMvec";
    const char kOutputValidation[] = "outValidation";

    // Serialized parameters.

    const char kEnabled[] = "enabled";
    const char kMethod[] = "method";
    const char kOutputSize[] = "outputSize";

    // Common settings.
    const char kWorldSpaceMotion[] = "worldSpaceMotion";
    const char kDisocclusionThreshold[] = "disocclusionThreshold";

    // Pack radiance settings.
    const char kMaxIntensity[] = "maxIntensity";

    // ReLAX diffuse/specular settings.
    const char kDiffusePrepassBlurRadius[] = "diffusePrepassBlurRadius";
    const char kSpecularPrepassBlurRadius[] = "specularPrepassBlurRadius";
    const char kDiffuseMaxAccumulatedFrameNum[] = "diffuseMaxAccumulatedFrameNum";
    const char kSpecularMaxAccumulatedFrameNum[] = "specularMaxAccumulatedFrameNum";
    const char kDiffuseMaxFastAccumulatedFrameNum[] = "diffuseMaxFastAccumulatedFrameNum";
    const char kSpecularMaxFastAccumulatedFrameNum[] = "specularMaxFastAccumulatedFrameNum";
    const char kDiffusePhiLuminance[] = "diffusePhiLuminance";
    const char kSpecularPhiLuminance[] = "specularPhiLuminance";
    const char kDiffuseLobeAngleFraction[] = "diffuseLobeAngleFraction";
    const char kSpecularLobeAngleFraction[] = "specularLobeAngleFraction";
    const char kRoughnessFraction[] = "roughnessFraction";
    const char kDiffuseHistoryRejectionNormalThreshold[] = "diffuseHistoryRejectionNormalThreshold";
    const char kSpecularVarianceBoost[] = "specularVarianceBoost";
    const char kSpecularLobeAngleSlack[] = "specularLobeAngleSlack";
    const char kDisocclusionFixEdgeStoppingNormalPower[] = "disocclusionFixEdgeStoppingNormalPower";
    const char kDisocclusionFixMaxRadius[] = "disocclusionFixMaxRadius";
    const char kDisocclusionFixNumFramesToFix[] = "disocclusionFixNumFramesToFix";
    const char kHistoryClampingColorBoxSigmaScale[] = "historyClampingColorBoxSigmaScale";
    const char kSpatialVarianceEstimationHistoryThreshold[] = "spatialVarianceEstimationHistoryThreshold";
    const char kAtrousIterationNum[] = "atrousIterationNum";
    const char kMinLuminanceWeight[] = "minLuminanceWeight";
    const char kDepthThreshold[] = "depthThreshold";
    const char kRoughnessEdgeStoppingRelaxation[] = "roughnessEdgeStoppingRelaxation";
    const char kNormalEdgeStoppingRelaxation[] = "normalEdgeStoppingRelaxation";
    const char kLuminanceEdgeStoppingRelaxation[] = "luminanceEdgeStoppingRelaxation";
    const char kEnableAntiFirefly[] = "enableAntiFirefly";
    const char kEnableReprojectionTestSkippingWithoutMotion[] = "enableReprojectionTestSkippingWithoutMotion";
    const char kEnableSpecularVirtualHistoryClamping[] = "enableSpecularVirtualHistoryClamping";
    const char kEnableRoughnessEdgeStopping[] = "enableRoughnessEdgeStopping";
    const char kEnableMaterialTestForDiffuse[] = "enableMaterialTestForDiffuse";
    const char kEnableMaterialTestForSpecular[] = "enableMaterialTestForSpecular";

    // Expose only togglable methods.
    // There is no reason to expose runtime toggle for other methods.
    const Gui::DropdownList kDenoisingMethod =
    {
        { (uint32_t)NRDPass::DenoisingMethod::RelaxDiffuseSpecular, "ReLAX" },
        { (uint32_t)NRDPass::DenoisingMethod::ReblurDiffuseSpecular, "ReBLUR" },
    };

    }

NRDPass::NRDPass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpDevice->requireD3D12();

    DefineList definesRelax;
    definesRelax.add("NRD_NORMAL_ENCODING", kNormalEncoding);
    definesRelax.add("NRD_ROUGHNESS_ENCODING", kRoughnessEncoding);
    definesRelax.add("NRD_METHOD", "0"); // NRD_METHOD_RELAX_DIFFUSE_SPECULAR
    definesRelax.add("GROUP_X", "16");
    definesRelax.add("GROUP_Y", "16"); 
    mpPackRadiancePassRelax = ComputePass::create(mpDevice, kShaderPackRadiance, "main", definesRelax);

    DefineList definesReblur;
    definesReblur.add("NRD_NORMAL_ENCODING", kNormalEncoding);
    definesReblur.add("NRD_ROUGHNESS_ENCODING", kRoughnessEncoding);
    definesReblur.add("NRD_METHOD", "1"); // NRD_METHOD_REBLUR_DIFFUSE_SPECULAR
    definesReblur.add("GROUP_X", "16");
    definesReblur.add("GROUP_Y", "16"); 
    mpPackRadiancePassReblur = ComputePass::create(mpDevice, kShaderPackRadiance, "main", definesReblur);

    // Override some defaults coming from the NRD SDK.
    /*TODO
    mRelaxDiffuseSpecularSettings.diffusePrepassBlurRadius = 16.0f;
    mRelaxDiffuseSpecularSettings.specularPrepassBlurRadius = 16.0f;
    mRelaxDiffuseSpecularSettings.diffuseMaxFastAccumulatedFrameNum = 2;
    mRelaxDiffuseSpecularSettings.specularMaxFastAccumulatedFrameNum = 2;
    mRelaxDiffuseSpecularSettings.diffuseLobeAngleFraction = 0.8f;
    mRelaxDiffuseSpecularSettings.disocclusionFixMaxRadius = 32.0f;
    mRelaxDiffuseSpecularSettings.enableSpecularVirtualHistoryClamping = false;
    mRelaxDiffuseSpecularSettings.disocclusionFixNumFramesToFix = 4;
    mRelaxDiffuseSpecularSettings.spatialVarianceEstimationHistoryThreshold = 4;
    mRelaxDiffuseSpecularSettings.atrousIterationNum = 6;
    mRelaxDiffuseSpecularSettings.depthThreshold = 0.02f;
    mRelaxDiffuseSpecularSettings.roughnessFraction = 0.5f;
    mRelaxDiffuseSpecularSettings.specularLobeAngleFraction = 0.9f;
    mRelaxDiffuseSpecularSettings.specularLobeAngleSlack = 10.0f;

    mRelaxDiffuseSettings.prepassBlurRadius = 16.0f;
    mRelaxDiffuseSettings.diffuseMaxFastAccumulatedFrameNum = 2;
    mRelaxDiffuseSettings.diffuseLobeAngleFraction = 0.8f;
    mRelaxDiffuseSettings.disocclusionFixMaxRadius = 32.0f;
    mRelaxDiffuseSettings.disocclusionFixNumFramesToFix = 4;
    mRelaxDiffuseSettings.spatialVarianceEstimationHistoryThreshold = 4;
    mRelaxDiffuseSettings.atrousIterationNum = 6;
    mRelaxDiffuseSettings.depthThreshold = 0.02f;
    */
    // Deserialize pass from dictionary.
    for (const auto& [key, value] : props)
    {
        if (key == kEnabled) mEnabled = value;
        else if (key == kMethod) mDenoisingMethod = value;
        else if (key == kOutputSize) mOutputSizeSelection = value;

        // Common settings.
        else if (key == kWorldSpaceMotion) mWorldSpaceMotion = value;
        else if (key == kDisocclusionThreshold) mDisocclusionThreshold = value;

        // Pack radiance settings.
        else if (key == kMaxIntensity) mMaxIntensity = value;

        // ReLAX diffuse/specular settings.
        else if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular || mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
        {
            /*TODO
            if (key == kDiffusePrepassBlurRadius) mRelaxDiffuseSpecularSettings.diffusePrepassBlurRadius = value;
            else if (key == kSpecularPrepassBlurRadius) mRelaxDiffuseSpecularSettings.specularPrepassBlurRadius = value;
            else if (key == kDiffuseMaxAccumulatedFrameNum) mRelaxDiffuseSpecularSettings.diffuseMaxAccumulatedFrameNum = value;
            else if (key == kSpecularMaxAccumulatedFrameNum) mRelaxDiffuseSpecularSettings.specularMaxAccumulatedFrameNum = value;
            else if (key == kDiffuseMaxFastAccumulatedFrameNum) mRelaxDiffuseSpecularSettings.diffuseMaxFastAccumulatedFrameNum = value;
            else if (key == kSpecularMaxFastAccumulatedFrameNum) mRelaxDiffuseSpecularSettings.specularMaxFastAccumulatedFrameNum = value;
            else if (key == kDiffusePhiLuminance) mRelaxDiffuseSpecularSettings.diffusePhiLuminance = value;
            else if (key == kSpecularPhiLuminance) mRelaxDiffuseSpecularSettings.specularPhiLuminance = value;
            else if (key == kDiffuseLobeAngleFraction) mRelaxDiffuseSpecularSettings.diffuseLobeAngleFraction = value;
            else if (key == kSpecularLobeAngleFraction) mRelaxDiffuseSpecularSettings.specularLobeAngleFraction = value;
            else if (key == kRoughnessFraction) mRelaxDiffuseSpecularSettings.roughnessFraction = value;
            else if (key == kDiffuseHistoryRejectionNormalThreshold) mRelaxDiffuseSpecularSettings.diffuseHistoryRejectionNormalThreshold = value;
            else if (key == kSpecularVarianceBoost) mRelaxDiffuseSpecularSettings.specularVarianceBoost = value;
            else if (key == kSpecularLobeAngleSlack) mRelaxDiffuseSpecularSettings.specularLobeAngleSlack = value;
            else if (key == kDisocclusionFixEdgeStoppingNormalPower) mRelaxDiffuseSpecularSettings.disocclusionFixEdgeStoppingNormalPower = value;
            else if (key == kDisocclusionFixMaxRadius) mRelaxDiffuseSpecularSettings.disocclusionFixMaxRadius = value;
            else if (key == kDisocclusionFixNumFramesToFix) mRelaxDiffuseSpecularSettings.disocclusionFixNumFramesToFix = value;
            else if (key == kHistoryClampingColorBoxSigmaScale) mRelaxDiffuseSpecularSettings.historyClampingColorBoxSigmaScale = value;
            else if (key == kSpatialVarianceEstimationHistoryThreshold) mRelaxDiffuseSpecularSettings.spatialVarianceEstimationHistoryThreshold = value;
            else if (key == kAtrousIterationNum) mRelaxDiffuseSpecularSettings.atrousIterationNum = value;
            else if (key == kMinLuminanceWeight) mRelaxDiffuseSpecularSettings.minLuminanceWeight = value;
            else if (key == kDepthThreshold) mRelaxDiffuseSpecularSettings.depthThreshold = value;
            else if (key == kLuminanceEdgeStoppingRelaxation) mRelaxDiffuseSpecularSettings.luminanceEdgeStoppingRelaxation = value;
            else if (key == kNormalEdgeStoppingRelaxation) mRelaxDiffuseSpecularSettings.normalEdgeStoppingRelaxation = value;
            else if (key == kRoughnessEdgeStoppingRelaxation) mRelaxDiffuseSpecularSettings.roughnessEdgeStoppingRelaxation = value;
            else if (key == kEnableAntiFirefly) mRelaxDiffuseSpecularSettings.enableAntiFirefly = value;
            else if (key == kEnableReprojectionTestSkippingWithoutMotion) mRelaxDiffuseSpecularSettings.enableReprojectionTestSkippingWithoutMotion = value;
            else if (key == kEnableSpecularVirtualHistoryClamping) mRelaxDiffuseSpecularSettings.enableSpecularVirtualHistoryClamping = value;
            else if (key == kEnableRoughnessEdgeStopping) mRelaxDiffuseSpecularSettings.enableRoughnessEdgeStopping = value;
            else if (key == kEnableMaterialTestForDiffuse) mRelaxDiffuseSpecularSettings.enableMaterialTestForDiffuse = value;
            else if (key == kEnableMaterialTestForSpecular) mRelaxDiffuseSpecularSettings.enableMaterialTestForSpecular = value;
            else
            {
                logWarning("Unknown property '{}' in NRD properties.", key);
            }
            */
        }
        else
        {
            logWarning("Unknown property '{}' in NRD properties.", key);
        }
    }
}

Properties NRDPass::getProperties() const
{
    Properties props;

    props[kEnabled] = mEnabled;
    props[kMethod] = mDenoisingMethod;
    props[kOutputSize] = mOutputSizeSelection;

    // Common settings.
    props[kWorldSpaceMotion] = mWorldSpaceMotion;
    props[kDisocclusionThreshold] = mDisocclusionThreshold;

    // Pack radiance settings.
    props[kMaxIntensity] = mMaxIntensity;

    // ReLAX diffuse/specular settings.
    if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular || mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
    {
        /*
        props[kDiffusePrepassBlurRadius] = mRelaxDiffuseSpecularSettings.diffusePrepassBlurRadius;
        props[kSpecularPrepassBlurRadius] = mRelaxDiffuseSpecularSettings.specularPrepassBlurRadius;
        props[kDiffuseMaxAccumulatedFrameNum] = mRelaxDiffuseSpecularSettings.diffuseMaxAccumulatedFrameNum;
        props[kSpecularMaxAccumulatedFrameNum] = mRelaxDiffuseSpecularSettings.specularMaxAccumulatedFrameNum;
        props[kDiffuseMaxFastAccumulatedFrameNum] = mRelaxDiffuseSpecularSettings.diffuseMaxFastAccumulatedFrameNum;
        props[kSpecularMaxFastAccumulatedFrameNum] = mRelaxDiffuseSpecularSettings.specularMaxFastAccumulatedFrameNum;
        props[kDiffusePhiLuminance] = mRelaxDiffuseSpecularSettings.diffusePhiLuminance;
        props[kSpecularPhiLuminance] = mRelaxDiffuseSpecularSettings.specularPhiLuminance;
        props[kDiffuseLobeAngleFraction] = mRelaxDiffuseSpecularSettings.diffuseLobeAngleFraction;
        props[kSpecularLobeAngleFraction] = mRelaxDiffuseSpecularSettings.specularLobeAngleFraction;
        props[kRoughnessFraction] = mRelaxDiffuseSpecularSettings.roughnessFraction;
        props[kDiffuseHistoryRejectionNormalThreshold] = mRelaxDiffuseSpecularSettings.diffuseHistoryRejectionNormalThreshold;
        props[kSpecularVarianceBoost] = mRelaxDiffuseSpecularSettings.specularVarianceBoost;
        props[kSpecularLobeAngleSlack] = mRelaxDiffuseSpecularSettings.specularLobeAngleSlack;
        props[kDisocclusionFixEdgeStoppingNormalPower] = mRelaxDiffuseSpecularSettings.disocclusionFixEdgeStoppingNormalPower;
        props[kDisocclusionFixMaxRadius] = mRelaxDiffuseSpecularSettings.disocclusionFixMaxRadius;
        props[kDisocclusionFixNumFramesToFix] = mRelaxDiffuseSpecularSettings.disocclusionFixNumFramesToFix;
        props[kHistoryClampingColorBoxSigmaScale] = mRelaxDiffuseSpecularSettings.historyClampingColorBoxSigmaScale;
        props[kSpatialVarianceEstimationHistoryThreshold] = mRelaxDiffuseSpecularSettings.spatialVarianceEstimationHistoryThreshold;
        props[kAtrousIterationNum] = mRelaxDiffuseSpecularSettings.atrousIterationNum;
        props[kMinLuminanceWeight] = mRelaxDiffuseSpecularSettings.minLuminanceWeight;
        props[kDepthThreshold] = mRelaxDiffuseSpecularSettings.depthThreshold;
        props[kLuminanceEdgeStoppingRelaxation] = mRelaxDiffuseSpecularSettings.luminanceEdgeStoppingRelaxation;
        props[kNormalEdgeStoppingRelaxation] = mRelaxDiffuseSpecularSettings.normalEdgeStoppingRelaxation;
        props[kRoughnessEdgeStoppingRelaxation] = mRelaxDiffuseSpecularSettings.roughnessEdgeStoppingRelaxation;
        props[kEnableAntiFirefly] = mRelaxDiffuseSpecularSettings.enableAntiFirefly;
        props[kEnableReprojectionTestSkippingWithoutMotion] = mRelaxDiffuseSpecularSettings.enableReprojectionTestSkippingWithoutMotion;
        props[kEnableSpecularVirtualHistoryClamping] = mRelaxDiffuseSpecularSettings.enableSpecularVirtualHistoryClamping;
        props[kEnableRoughnessEdgeStopping] = mRelaxDiffuseSpecularSettings.enableRoughnessEdgeStopping;
        props[kEnableMaterialTestForDiffuse] = mRelaxDiffuseSpecularSettings.enableMaterialTestForDiffuse;
        props[kEnableMaterialTestForSpecular] = mRelaxDiffuseSpecularSettings.enableMaterialTestForSpecular;
        */
    }

    return props;
}

RenderPassReflection NRDPass::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    const uint2 sz = RenderPassHelpers::calculateIOSize(mOutputSizeSelection, mScreenSize, compileData.defaultTexDims);

    if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular || mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
    {
        reflector.addInput(kInputDiffuseRadianceHitDist, "Diffuse radiance and hit distance");
        reflector.addInput(kInputSpecularRadianceHitDist, "Specular radiance and hit distance");
        reflector.addInput(kInputViewZ, "View Z");
        reflector.addInput(kInputNormalRoughnessMaterialID, "World normal, roughness, and material ID");
        reflector.addInput(kInputMotionVectors, "Motion vectors");

        reflector.addOutput(kOutputFilteredDiffuseRadianceHitDist, "Filtered diffuse radiance and hit distance").format(ResourceFormat::RGBA16Float).texture2D(sz.x, sz.y);
        reflector.addOutput(kOutputFilteredSpecularRadianceHitDist, "Filtered specular radiance and hit distance").format(ResourceFormat::RGBA16Float).texture2D(sz.x, sz.y);
        reflector.addOutput(kOutputValidation, "Validation Layer for debug purposes").format(ResourceFormat::RGBA32Float).texture2D(sz.x, sz.y);
    }
    else
    {
        FALCOR_UNREACHABLE();
    }

    return reflector;
}

void NRDPass::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mScreenSize = RenderPassHelpers::calculateIOSize(mOutputSizeSelection, mScreenSize, compileData.defaultTexDims);
    if (mScreenSize.x == 0 || mScreenSize.y == 0)
        mScreenSize = compileData.defaultTexDims;
    mFrameIndex = 0;
    reinit();
}

void NRDPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //Check if a dict NRD refresh flag was set and overwrite enabled
    auto& dict = renderData.getDictionary();
    auto nrdEnableFlag = dict.getValue(kRenderPassEnableNRD, NRDEnableFlags::None);
    if (mEnabled && (nrdEnableFlag == NRDEnableFlags::NRDDisabled))
        mEnabled = false;
    else if (!mEnabled && (nrdEnableFlag == NRDEnableFlags::NRDEnabled))
        mEnabled = true;

    if (mEnabled)
    {
        executeInternal(pRenderContext, renderData);
    }
    else
    {
        if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular || mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
        {
            pRenderContext->blit(renderData.getTexture(kInputDiffuseRadianceHitDist)->getSRV(), renderData.getTexture(kOutputFilteredDiffuseRadianceHitDist)->getRTV());
            pRenderContext->blit(renderData.getTexture(kInputSpecularRadianceHitDist)->getSRV(), renderData.getTexture(kOutputFilteredSpecularRadianceHitDist)->getRTV());
        }
    }

    //Update dict flag if options changed
    if (mOptionsChanged)
    {
        dict[Falcor::kRenderPassNRDOutputInYCoCg] = mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular ? NRDEnableFlags::NRDEnabled : NRDEnableFlags::NRDDisabled;
        dict[Falcor::kRenderPassUseNRDDebugLayer] = mEnableValidationLayer ? NRDEnableFlags::NRDEnabled : NRDEnableFlags::NRDDisabled;
        mOptionsChanged = false;
    }
    else
    {
        dict[Falcor::kRenderPassNRDOutputInYCoCg] = NRDEnableFlags::None;
        dict[Falcor::kRenderPassUseNRDDebugLayer] = NRDEnableFlags::None;
    }
}

nrd::HitDistanceReconstructionMode getNRDHitDistanceReconstructionMode(NRDPass::HitDistanceReconstructionMode& falcorHitDistMode)
{
    switch (falcorHitDistMode)
    {
    case NRDPass::HitDistanceReconstructionMode::OFF:
        return nrd::HitDistanceReconstructionMode::OFF;
    case NRDPass::HitDistanceReconstructionMode::AREA3X3:
        return nrd::HitDistanceReconstructionMode::AREA_3X3;
    case NRDPass::HitDistanceReconstructionMode::AREA5X5:
        return nrd::HitDistanceReconstructionMode::AREA_5X5;
    }
    //Should not happen
    return nrd::HitDistanceReconstructionMode::OFF;
}

void NRDPass::renderUI(Gui::Widgets& widget)
{
    const nrd::LibraryDesc& nrdLibraryDesc = nrd::GetLibraryDesc();
    char name[256];
    _snprintf_s(name, 255, "NRD Library v%u.%u.%u", nrdLibraryDesc.versionMajor, nrdLibraryDesc.versionMinor, nrdLibraryDesc.versionBuild);
    widget.text(name);

    widget.checkbox("Enabled", mEnabled);

    widget.text("Common:");
    widget.checkbox("Motion : world space", mWorldSpaceMotion);
    widget.tooltip("Else 2.5D Motion Vectors are assumed");
    widget.var("Disocclusion threshold (%) x 100", mDisocclusionThreshold, 1.0f, 2.0f, 0.01f, false, "%.2f");
    mOptionsChanged |= widget.checkbox("Enable Debug Layer", mEnableValidationLayer);
    widget.checkbox("Enable Debug Split Screen", mEnableSplitScreen);
    widget.tooltip("Enables \" noisy input / denoised output \" comparison [0; 1]");
    if (mEnableSplitScreen)
        widget.var("Split Screen Value", mSplitScreenValue, 0.0f, 1.0f, 0.01f, false, "%.2f");

    widget.text("Pack radiance:");
    widget.var("Max intensity", mMaxIntensity, 0.f, 100000.f, 1.f, false, "%.0f");

    if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular || mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
    {
        mRecreateDenoiser = widget.dropdown("Denoising method", mDenoisingMethod);
    }

    if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular)
    {
        
        // ReLAX diffuse/specular settings.
        if (auto group = widget.group("ReLAX Diffuse/Specular"))
        {
            if (auto group2 = group.group("Antilag Settings"))
            {
                group2.text(
                    "IMPORTANT: History acceleration and reset amounts for specular are made 2x-3x weaker than values for diffuse below \n"
                    "due to specific specular logic that does additional history acceleration and reset"
                );
                group2.var("Acceleration Amount", mRelaxSettings.antilagSettings.accelerationAmount, 0.f, 1.f, 0.01f, false, "%.2f");
                group2.tooltip("[0; 1] - amount of history acceleration if history clamping happened in pixel");
                group2.var("Spatial Sigma Scale", mRelaxSettings.antilagSettings.spatialSigmaScale, 0.f, 256.f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - history is being reset if delta between history and raw input is larger than spatial sigma + temporal sigma");
                group2.var("Temporal Sigma Scale", mRelaxSettings.antilagSettings.temporalSigmaScale, 0.f, 256.f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - history is being reset if delta between history and raw input is larger than spatial sigma + temporal sigma");
                group2.var("Reset Amount", mRelaxSettings.antilagSettings.resetAmount, 0.f, 1.f, 0.01f, false, "%.2f");
                group2.tooltip("[0; 1] - amount of history reset, 0.0 - no reset, 1.0 - full reset");
            }
            group.text("Prepass:");
            group.var("Diffuse blur radius", mRelaxSettings.diffusePrepassBlurRadius, 0.0f, 100.0f, 1.0f, false, "%.0f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of probabilistic sampling)");
            group.var("Specular blur radius", mRelaxSettings.specularPrepassBlurRadius, 0.0f, 100.0f,1.0f, false, "%.0f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of probabilistic sampling)");

            group.text("Reprojection:");
            group.var("Diffuse max accumulated frames", mRelaxSettings.diffuseMaxAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Diffuse responsive max accumulated frames", mRelaxSettings.diffuseMaxFastAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Specular max accumulated frames", mRelaxSettings.specularMaxAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Specular responsive max accumulated frames", mRelaxSettings.specularMaxFastAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.slider("History Fix Frames", mRelaxSettings.historyFixFrameNum, 0u, 3u);
            group.tooltip("[0; 3] - number of reconstructed frames after history reset (less than \" maxFastAccumulatedFrameNum \")"); 

            group.text("A-trous edge stopping:");
            group.var("Diffuse Phi Luminance", mRelaxSettings.diffusePhiLuminance, 0.f, 256.f, 0.01f, false, "%.2f");
            group.var("Specular Phi Luminance", mRelaxSettings.specularPhiLuminance, 0.f, 256.f, 0.01f, false, "%.2f");            
            group.var("Diffuse Lobe Angle Fraction", mRelaxSettings.diffuseLobeAngleFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Specular Lobe Angle Fraction", mRelaxSettings.specularLobeAngleFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Roughness Fraction", mRelaxSettings.roughnessFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of center roughness used to drive roughness based rejection");
            group.var("Specular Variance Boost", mRelaxSettings.specularVarianceBoost, 0.f, 64.f, 0.01f, false, "%.2f");
            group.tooltip("(>= 0) - how much variance we inject to specular if reprojection confidence is low");
            group.var("Specular Lobe Angle Slack", mRelaxSettings.specularLobeAngleSlack, 0.f, 0.9f, 0.01f, false, "%.2f");
            group.tooltip("(degrees) - slack for the specular lobe angle used in normal based rejection of specular during A-Trous passes");
            group.var("History Fix Edge Stopping Normal Power", mRelaxSettings.historyFixEdgeStoppingNormalPower, 0.01f, 64.f, 0.01f, false, "%.2f");
            group.tooltip("(> 0) - normal edge stopper for history reconstruction pass");
            group.var("History Fix Color Box Sigma Scale", mRelaxSettings.historyClampingColorBoxSigmaScale, 1.f, 3.0f, 0.01f, false, "%.2f");
            group.tooltip("[1; 3] - standard deviation scale of color box for clamping main \" slow \" history to responsive \" fast \" history");
            group.var("Spatial Variance Estimation History Threshold", mRelaxSettings.spatialVarianceEstimationHistoryThreshold, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.tooltip("(>= 0) - history length threshold below which spatial variance estimation will be executed");
            group.var("A-trous Iterations", mRelaxSettings.atrousIterationNum, 2u, 8u);
            group.tooltip("[2; 8] - number of iterations for A-Trous wavelet transform");
            group.var("Diffuse Min Luminance Weight", mRelaxSettings.diffuseMinLuminanceWeight, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - A-trous edge stopping Luminance weight minimum");
            group.var("Specular Min Luminance Weight", mRelaxSettings.specularMinLuminanceWeight, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - A-trous edge stopping Luminance weight minimum");
            group.var("Depth Threshold", mRelaxSettings.depthThreshold, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - Depth threshold for spatial passes");

            group.text("Relaxation Settings:");
            group.var("CD Relaxation Multiplier", mRelaxSettings.confidenceDrivenRelaxationMultiplier, 0.f, 1.0f, 0.01f, false, "%.2f"); //TODO range?
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");
            group.var("CD Relaxation Luminance Edge Stopping", mRelaxSettings.confidenceDrivenLuminanceEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");
            group.var("CD Relaxation Normal Edge Stopping", mRelaxSettings.confidenceDrivenNormalEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");

            group.var("Relaxation Luminance Edge Stopping", mRelaxSettings.luminanceEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax roughness based rejection for spatial filter in areas where specular reprojection is low");
            group.var("Relaxation Normal Edge Stopping", mRelaxSettings.normalEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax roughness based rejection for spatial filter in areas where specular reprojection is low");
            group.var("Relaxation Roughness Edge Stopping", mRelaxSettings.roughnessEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax rejection for spatial filter based on roughness and view vector");
            group.checkbox("Enable Roughness Edge Stopping", mRelaxSettings.enableRoughnessEdgeStopping);

            group.text("Misc:");
            group.dropdown("Hit Distance Reconstruction", mHitDistanceReconstructionMode);
            getNRDHitDistanceReconstructionMode(mHitDistanceReconstructionMode); // Set NRD setting
            group.checkbox("Anti-Firefly Filter", mRelaxSettings.enableAntiFirefly);
            group.checkbox("Material test for diffuse", mRelaxSettings.enableMaterialTestForDiffuse);
            group.checkbox("Material test for specular", mRelaxSettings.enableMaterialTestForSpecular);
        }
    }
    else if (mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
    {
        if (auto group = widget.group("ReBLUR Diffuse/Specular"))
        {
            
            const float kEpsilon = 0.0001f;
            if (auto group2 = group.group("Hit distance"))
            {
                group2.text(
                    "Normalized hit distance = saturate( \"hit distance\" / f ), where: \n f = ( A + viewZ * B ) * lerp( 1.0, C, exp2( D * "
                    "roughness ^ 2 ) ), see \"NRD.hlsl/REBLUR_FrontEnd_GetNormHitDist\""
                );
                group2.var("A", mReblurSettings.hitDistanceParameters.A, 0.01f, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(units > 0) - constant value");
                group2.var("B", mReblurSettings.hitDistanceParameters.B, kEpsilon, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - viewZ based linear scale (1 m - 10 cm, 10 m - 1 m, 100 m - 10 m)");
                group2.var("C", mReblurSettings.hitDistanceParameters.C, 1.0f, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(>= 1) - roughness based scale, use values > 1 to get bigger hit distance for low roughness");
                group2.var("D", mReblurSettings.hitDistanceParameters.D, -256.0f, 0.0f, 0.01f, false, "%.2f");
                group2.tooltip(
                    "(<= 0) - absolute value should be big enough to collapse \" exp2(D * roughness ^ 2) \" to \" ~0 \" for roughness = 1"
                );
            }

            if (auto group2 = group.group("Antilag settings"))
            {
                group2.var("Luminance Sigma Scale", mReblurSettings.antilagSettings.luminanceSigmaScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
                group2.var("hit Distance Sigma Scale", mReblurSettings.antilagSettings.hitDistanceSigmaScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
                group2.var("Luminance Antilag Power", mReblurSettings.antilagSettings.luminanceAntilagPower, kEpsilon, 1.0f, 0.0001f, false, "%.4f");
                group2.var("hit Distance Antilag Power", mReblurSettings.antilagSettings.hitDistanceAntilagPower, kEpsilon, 1.0f, 0.0001f,false, "%.4f");
            }

            group.var("Max accumulated frame num", mReblurSettings.maxAccumulatedFrameNum, 0u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
            group.var("Max fast accumulated frame num", mReblurSettings.maxFastAccumulatedFrameNum, 0u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
            group.slider("History Fix frame num", mReblurSettings.historyFixFrameNum, 0u, 3u);
            group.var("Prepass Diffuse Blur radius", mReblurSettings.diffusePrepassBlurRadius, 0.f, 256.f, 0.01f, false, "%.2f");

            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of badly defined signals and probabilistic sampling)");
            group.var("Prepass Specular Blur radius", mReblurSettings.specularPrepassBlurRadius, 0.f, 256.f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of badly defined signals and probabilistic sampling)");

            group.var("Min Blur radius", mReblurSettings.minBlurRadius, 0.0f, 256.0f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - min denoising radius (for converged state)");
            group.var("Max Blur radius", mReblurSettings.maxBlurRadius, 0.0f, 256.0f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - base (max) denoising radius (gets reduced over time)");

            group.var("Normal weight (Lobe Angle Fraction)", mReblurSettings.lobeAngleFraction, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Roughness Fraction", mReblurSettings.roughnessFraction, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of center roughness used to drive roughness based rejection");

            group.var("Responsive Accumulation Roughness Threshold", mReblurSettings.responsiveAccumulationRoughnessThreshold, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - if roughness < this, temporal accumulation becomes responsive and driven by roughness (useful for animated water)");

            group.var("Stabilization Strength", mReblurSettings.stabilizationStrength, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip(
                "(normalized %) - stabilizes output, but adds temporal lag, at the same time more stabilization improves antilag (clean signals can use lower values)\n"
                "= N / (1 + N), where N is the number of accumulated frames \n"
                "0 - disables the stabilization pass"
            );

            group.var("hit Distance Stabilization Strength", mReblurSettings.hitDistanceStabilizationStrength, 0.0f, mReblurSettings.stabilizationStrength, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - same as \" stabilizationStrength \", but for hit distance (can't be > \" stabilizationStrength \", 0 - allows to reach parity with REBLUR_OCCLUSION) \n"
                "= N / (1 + N), where N is the number of accumulated frames "
            );

            group.var("Plane Distance Sensitivity", mReblurSettings.planeDistanceSensitivity, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - represents maximum allowed deviation from local tangent plane");

            float2 specularProbabilityThresholdsForMvModification = float2(mReblurSettings.specularProbabilityThresholdsForMvModification[0],mReblurSettings.specularProbabilityThresholdsForMvModification[1]);
            if (group.var("Specular Probability Thresholds For MV Modification", specularProbabilityThresholdsForMvModification, 0.0f, 1.f , 0.01f, false,"%.2f"))
            {
                mReblurSettings.specularProbabilityThresholdsForMvModification[0] = specularProbabilityThresholdsForMvModification.x;
                mReblurSettings.specularProbabilityThresholdsForMvModification[1] = specularProbabilityThresholdsForMvModification.y;
            }
            group.tooltip("IN_MV = lerp(IN_MV, specularMotion, smoothstep(this[0], this[1], specularProbability))");

            group.var("Firefly Suppressore Min Relative Scale", mReblurSettings.fireflySuppressorMinRelativeScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
            group.tooltip("[1; 3] - undesired sporadic outliers suppression to keep output stable (smaller values maximize suppression in exchange of bias)");

            group.checkbox("Antifirefly", mReblurSettings.enableAntiFirefly);
            group.checkbox("Performance mode", mReblurSettings.enablePerformanceMode);
            group.dropdown("Hit Distance Reconstruction", mHitDistanceReconstructionMode);
            mReblurSettings.hitDistanceReconstructionMode = getNRDHitDistanceReconstructionMode(mHitDistanceReconstructionMode); //Set NRD setting
            group.checkbox("Material test for diffuse", mReblurSettings.enableMaterialTestForDiffuse);
            group.checkbox("Material test for specular", mReblurSettings.enableMaterialTestForSpecular);
            group.checkbox("Use Prepass Only For Specular Motion Estimation", mReblurSettings.usePrepassOnlyForSpecularMotionEstimation);
        }
    }
}

void NRDPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
}

static void* nrdAllocate(void* userArg, size_t size, size_t alignment)
{
    return malloc(size);
}

static void* nrdReallocate(void* userArg, void* memory, size_t size, size_t alignment)
{
    return realloc(memory, size);
}

static void nrdFree(void* userArg, void* memory)
{
    free(memory);
}

static ResourceFormat getFalcorFormat(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:             return ResourceFormat::R8Unorm;
    case nrd::Format::R8_SNORM:             return ResourceFormat::R8Snorm;
    case nrd::Format::R8_UINT:              return ResourceFormat::R8Uint;
    case nrd::Format::R8_SINT:              return ResourceFormat::R8Int;
    case nrd::Format::RG8_UNORM:            return ResourceFormat::RG8Unorm;
    case nrd::Format::RG8_SNORM:            return ResourceFormat::RG8Snorm;
    case nrd::Format::RG8_UINT:             return ResourceFormat::RG8Uint;
    case nrd::Format::RG8_SINT:             return ResourceFormat::RG8Int;
    case nrd::Format::RGBA8_UNORM:          return ResourceFormat::RGBA8Unorm;
    case nrd::Format::RGBA8_SNORM:          return ResourceFormat::RGBA8Snorm;
    case nrd::Format::RGBA8_UINT:           return ResourceFormat::RGBA8Uint;
    case nrd::Format::RGBA8_SINT:           return ResourceFormat::RGBA8Int;
    case nrd::Format::RGBA8_SRGB:           return ResourceFormat::RGBA8UnormSrgb;
    case nrd::Format::R16_UNORM:            return ResourceFormat::R16Unorm;
    case nrd::Format::R16_SNORM:            return ResourceFormat::R16Snorm;
    case nrd::Format::R16_UINT:             return ResourceFormat::R16Uint;
    case nrd::Format::R16_SINT:             return ResourceFormat::R16Int;
    case nrd::Format::R16_SFLOAT:           return ResourceFormat::R16Float;
    case nrd::Format::RG16_UNORM:           return ResourceFormat::RG16Unorm;
    case nrd::Format::RG16_SNORM:           return ResourceFormat::RG16Snorm;
    case nrd::Format::RG16_UINT:            return ResourceFormat::RG16Uint;
    case nrd::Format::RG16_SINT:            return ResourceFormat::RG16Int;
    case nrd::Format::RG16_SFLOAT:          return ResourceFormat::RG16Float;
    case nrd::Format::RGBA16_UNORM:         return ResourceFormat::RGBA16Unorm;
    case nrd::Format::RGBA16_SNORM:         return ResourceFormat::Unknown; // Not defined in Falcor
    case nrd::Format::RGBA16_UINT:          return ResourceFormat::RGBA16Uint;
    case nrd::Format::RGBA16_SINT:          return ResourceFormat::RGBA16Int;
    case nrd::Format::RGBA16_SFLOAT:        return ResourceFormat::RGBA16Float;
    case nrd::Format::R32_UINT:             return ResourceFormat::R32Uint;
    case nrd::Format::R32_SINT:             return ResourceFormat::R32Int;
    case nrd::Format::R32_SFLOAT:           return ResourceFormat::R32Float;
    case nrd::Format::RG32_UINT:            return ResourceFormat::RG32Uint;
    case nrd::Format::RG32_SINT:            return ResourceFormat::RG32Int;
    case nrd::Format::RG32_SFLOAT:          return ResourceFormat::RG32Float;
    case nrd::Format::RGB32_UINT:           return ResourceFormat::RGB32Uint;
    case nrd::Format::RGB32_SINT:           return ResourceFormat::RGB32Int;
    case nrd::Format::RGB32_SFLOAT:         return ResourceFormat::RGB32Float;
    case nrd::Format::RGBA32_UINT:          return ResourceFormat::RGBA32Uint;
    case nrd::Format::RGBA32_SINT:          return ResourceFormat::RGBA32Int;
    case nrd::Format::RGBA32_SFLOAT:        return ResourceFormat::RGBA32Float;
    case nrd::Format::R10_G10_B10_A2_UNORM: return ResourceFormat::RGB10A2Unorm;
    case nrd::Format::R10_G10_B10_A2_UINT:  return ResourceFormat::RGB10A2Uint;
    case nrd::Format::R11_G11_B10_UFLOAT:   return ResourceFormat::R11G11B10Float;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return ResourceFormat::RGB9E5Float;
    default:
        throw RuntimeError("Unsupported NRD format.");
    }
}

static nrd::Denoiser getNrdDenoiser(NRDPass::DenoisingMethod denoisingMethod)
{
    switch (denoisingMethod)
    {
    case NRDPass::DenoisingMethod::RelaxDiffuseSpecular:
        return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    case NRDPass::DenoisingMethod::ReblurDiffuseSpecular:
        return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    default:
        FALCOR_UNREACHABLE();
        return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    }
}

/// Copies into col-major layout, as the NRD library works in column major layout,
/// while Falcor uses row-major layout
static void copyMatrix(float* dstMatrix, const float4x4& srcMatrix)
{
    float4x4 col_major = transpose(srcMatrix);
    memcpy(dstMatrix, static_cast<const float*>(col_major.data()), sizeof(float4x4));
}


void NRDPass::reinit()
{
    // Create a new denoiser instance.
    if (mpInstance)
        nrd::DestroyInstance(*mpInstance);

    const nrd::LibraryDesc& libraryDesc = nrd::GetLibraryDesc();

    const nrd::DenoiserDesc denoiserDescs[] = {
        {nrd::Identifier(getNrdDenoiser(mDenoisingMethod)), getNrdDenoiser(mDenoisingMethod)}
    };
    nrd::InstanceCreationDesc instanceCreationDesc = {};
    instanceCreationDesc.denoisers = denoiserDescs;
    instanceCreationDesc.denoisersNum = 1; //Only 1 denoiser is used at a time

    nrd::Result res = nrd::CreateInstance(instanceCreationDesc, mpInstance);
    if (res != nrd::Result::SUCCESS)
        throw RuntimeError("NRDPass: Failed to create NRD denoiser");

    createResources();
    createPipelines();
}

void NRDPass::createPipelines()
{
    mpPasses.clear();
    mpCachedProgramKernels.clear();
    mpCSOs.clear();
    mCBVSRVUAVdescriptorSetLayouts.clear();
    mpRootSignatures.clear();

    // Get denoiser desc for currently initialized denoiser implementation.
    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);

    // Create samplers descriptor layout and set.
    D3D12DescriptorSetLayout SamplersDescriptorSetLayout;
    
    for (uint32_t j = 0; j < instanceDesc.samplersNum; j++)
    {
        SamplersDescriptorSetLayout.addRange(ShaderResourceType::Sampler, instanceDesc.samplersBaseRegisterIndex + j, 1);
    }
    mpSamplersDescriptorSet = D3D12DescriptorSet::create(mpDevice, SamplersDescriptorSetLayout, D3D12DescriptorSetBindingUsage::ExplicitBind);

    // Set sampler descriptors right away.
    for (uint32_t j = 0; j < instanceDesc.samplersNum; j++)
    {
        mpSamplersDescriptorSet->setSampler(0, j, mpSamplers[j].get());
    }

    // Go over NRD passes and creating descriptor sets, root signatures and PSOs for each.
    for (uint32_t i = 0; i < instanceDesc.pipelinesNum; i++)
    {
        const nrd::PipelineDesc& nrdPipelineDesc = instanceDesc.pipelines[i];
        const nrd::ComputeShaderDesc& nrdComputeShader = nrdPipelineDesc.computeShaderDXIL;

        // Initialize descriptor set.
        D3D12DescriptorSetLayout CBVSRVUAVdescriptorSetLayout;

        // Add constant buffer to descriptor set.
        CBVSRVUAVdescriptorSetLayout.addRange(ShaderResourceType::Cbv, instanceDesc.constantBufferRegisterIndex, 1);

        for (uint32_t j = 0; j < nrdPipelineDesc.resourceRangesNum; j++)
        {
            const nrd::ResourceRangeDesc& nrdResourceRange = nrdPipelineDesc.resourceRanges[j];

            ShaderResourceType descriptorType = nrdResourceRange.descriptorType == nrd::DescriptorType::TEXTURE
                                                    ?
                ShaderResourceType::TextureSrv :
                ShaderResourceType::TextureUav;

            CBVSRVUAVdescriptorSetLayout.addRange(descriptorType, nrdResourceRange.baseRegisterIndex, nrdResourceRange.descriptorsNum);
        }

        mCBVSRVUAVdescriptorSetLayouts.push_back(CBVSRVUAVdescriptorSetLayout);

        // Create root signature for the NRD pass.
        D3D12RootSignature::Desc rootSignatureDesc;
        rootSignatureDesc.addDescriptorSet(SamplersDescriptorSetLayout);
        rootSignatureDesc.addDescriptorSet(CBVSRVUAVdescriptorSetLayout);

        const D3D12RootSignature::Desc& desc = rootSignatureDesc;

        ref<D3D12RootSignature> pRootSig = D3D12RootSignature::create(mpDevice, desc);

        mpRootSignatures.push_back(pRootSig);

        // Create Compute PSO for the NRD pass.
        {
            std::string shaderFileName = "nrd/Shaders/Source/" + std::string(nrdPipelineDesc.shaderFileName) + ".hlsl";

            Program::Desc programDesc;
            programDesc.addShaderLibrary(shaderFileName).csEntry(nrdPipelineDesc.shaderEntryPointName);
            programDesc.setCompilerFlags(Program::CompilerFlags::MatrixLayoutColumnMajor);
            DefineList defines;
            defines.add("NRD_COMPILER_DXC");
            defines.add("NRD_NORMAL_ENCODING", kNormalEncoding);
            defines.add("NRD_ROUGHNESS_ENCODING", kRoughnessEncoding);
            defines.add("GROUP_X", "16");
            defines.add("GROUP_Y", "16");

            ref<ComputePass> pPass = ComputePass::create(mpDevice, programDesc, defines);

            ref<ComputeProgram> pProgram = pPass->getProgram();
            ref<const ProgramKernels> pProgramKernels = pProgram->getActiveVersion()->getKernels(mpDevice.get(), pPass->getVars().get());

            ComputeStateObject::Desc csoDesc;
            csoDesc.setProgramKernels(pProgramKernels);
            csoDesc.setD3D12RootSignatureOverride(pRootSig);

            ref<ComputeStateObject> pCSO = ComputeStateObject::create(mpDevice, csoDesc);

            mpPasses.push_back(pPass);
            mpCachedProgramKernels.push_back(pProgramKernels);
            mpCSOs.push_back(pCSO);
        }
    }
}

static inline uint16_t NRD_DivideUp(uint32_t x, uint16_t y)
{
    return uint16_t((x + y - 1) / y);
}

void NRDPass::createResources()
{
    // Destroy previously created resources.
    mpSamplers.clear();
    mpPermanentTextures.clear();
    mpTransientTextures.clear();
    mpConstantBuffer = nullptr;

    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);
    const uint32_t poolSize = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;

    // Create samplers.
    for (uint32_t i = 0; i < instanceDesc.samplersNum; i++)
    {
        const nrd::Sampler& nrdStaticsampler = instanceDesc.samplers[i];
        Sampler::Desc samplerDesc;
        samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);

        if (nrdStaticsampler == nrd::Sampler::NEAREST_CLAMP || nrdStaticsampler == nrd::Sampler::LINEAR_CLAMP)
        {
            samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
        }
        else
        {
            samplerDesc.setAddressingMode(Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror);
        }

        if (nrdStaticsampler == nrd::Sampler::NEAREST_CLAMP)
        {
            samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
        }
        else
        {
            samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);
        }

        mpSamplers.push_back(Sampler::create(mpDevice, samplerDesc));
    }

    // Texture pool.
    for (uint32_t i = 0; i < poolSize; i++)
    {
        const bool isPermanent = (i < instanceDesc.permanentPoolSize);

        // Get texture desc.
        const nrd::TextureDesc& nrdTextureDesc =
            isPermanent ? instanceDesc.permanentPool[i] : instanceDesc.transientPool[i - instanceDesc.permanentPoolSize];

        // Create texture.
        ResourceFormat textureFormat = getFalcorFormat(nrdTextureDesc.format);
        uint w = NRD_DivideUp(mScreenSize.x, nrdTextureDesc.downsampleFactor);
        uint h = NRD_DivideUp(mScreenSize.y, nrdTextureDesc.downsampleFactor);
        ref<Texture> pTexture = Texture::create2D(
            mpDevice, w, h,
            textureFormat, 1u, 1,
            nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);

        if (isPermanent)
            mpPermanentTextures.push_back(pTexture);
        else
            mpTransientTextures.push_back(pTexture);
    }

    // Constant buffer.
    mpConstantBuffer = Buffer::create(
        mpDevice,
        instanceDesc.constantBufferMaxDataSize,
        ResourceBindFlags::Constant,
        Buffer::CpuAccess::Write,
        nullptr);
}

void NRDPass::executeInternal(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_ASSERT(mpScene);

    if (mRecreateDenoiser)
    {
        reinit();
        mOptionsChanged = true;
    }

    if (mDenoisingMethod == DenoisingMethod::RelaxDiffuseSpecular)
    {
        // Run classic Falcor compute pass to pack radiance.
        {
            FALCOR_PROFILE(pRenderContext, "PackRadiance");
            auto perImageCB = mpPackRadiancePassRelax->getRootVar()["PerImageCB"];

            perImageCB["gMaxIntensity"] = mMaxIntensity;
            perImageCB["gDiffuseRadianceHitDist"] = renderData.getTexture(kInputDiffuseRadianceHitDist);
            perImageCB["gSpecularRadianceHitDist"] = renderData.getTexture(kInputSpecularRadianceHitDist);
            mpPackRadiancePassRelax->execute(pRenderContext, uint3(mScreenSize.x, mScreenSize.y, 1u));
        }

        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(nrd::Denoiser::RELAX_DIFFUSE_SPECULAR), static_cast<void*>(&mRelaxSettings));
    }
    else if (mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular)
    {
        // Run classic Falcor compute pass to pack radiance and hit distance.
        {
            FALCOR_PROFILE(pRenderContext, "PackRadianceHitDist");
            auto perImageCB = mpPackRadiancePassReblur->getRootVar()["PerImageCB"];

            perImageCB["gHitDistParams"].setBlob(mReblurSettings.hitDistanceParameters);
            perImageCB["gMaxIntensity"] = mMaxIntensity;
            perImageCB["gDiffuseRadianceHitDist"] = renderData.getTexture(kInputDiffuseRadianceHitDist);
            perImageCB["gSpecularRadianceHitDist"] = renderData.getTexture(kInputSpecularRadianceHitDist);
            perImageCB["gNormalRoughness"] = renderData.getTexture(kInputNormalRoughnessMaterialID);
            perImageCB["gViewZ"] = renderData.getTexture(kInputViewZ);
            mpPackRadiancePassReblur->execute(pRenderContext, uint3(mScreenSize.x, mScreenSize.y, 1u));
        }

        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR), static_cast<void*>(&mReblurSettings));
    }
    else
    {
        FALCOR_UNREACHABLE();
        return;
    }

    // Initialize common settings.
    float4x4 viewMatrix = mpScene->getCamera()->getViewMatrix();
    float4x4 projMatrix = mpScene->getCamera()->getData().projMatNoJitter;
    // NRD's convention for the jitter is: [-0.5; 0.5] sampleUv = pixelUv + cameraJitter. Falcors jitter is in subpixel size divided by screen res
    float2 cameraJitter = float2(-mpScene->getCamera()->getJitterX() * mScreenSize.x, mpScene->getCamera()->getJitterY() * mScreenSize.y);
    if (mFrameIndex == 0)
    {
        mPrevViewMatrix = viewMatrix;
        mPrevProjMatrix = projMatrix;
        mPrevCameraJitter = cameraJitter;
    }

    copyMatrix(mCommonSettings.viewToClipMatrix, projMatrix);
    copyMatrix(mCommonSettings.viewToClipMatrixPrev, mPrevProjMatrix);
    copyMatrix(mCommonSettings.worldToViewMatrix, viewMatrix);
    copyMatrix(mCommonSettings.worldToViewMatrixPrev, mPrevViewMatrix);
    
    mCommonSettings.cameraJitter[0] = cameraJitter.x;
    mCommonSettings.cameraJitter[1] = cameraJitter.y;
    mCommonSettings.cameraJitterPrev[0] = mPrevCameraJitter.x;
    mCommonSettings.cameraJitterPrev[1] = mPrevCameraJitter.y;
    mCommonSettings.denoisingRange = kNRDDepthRange;
    mCommonSettings.disocclusionThreshold = mDisocclusionThreshold * 0.01f;
    mCommonSettings.frameIndex = mFrameIndex;
    mCommonSettings.isMotionVectorInWorldSpace = mWorldSpaceMotion;
    if (!mWorldSpaceMotion)
        mCommonSettings.motionVectorScale[2] = 1.f; //Enable 2.5D motion
    mCommonSettings.resourceSize[0] = mScreenSize.x;
    mCommonSettings.resourceSize[1] = mScreenSize.y;
    mCommonSettings.resourceSizePrev[0] = mScreenSize.x;
    mCommonSettings.resourceSizePrev[1] = mScreenSize.y;
    mCommonSettings.rectSize[0] = mScreenSize.x;
    mCommonSettings.rectSize[1] = mScreenSize.y;
    mCommonSettings.rectSizePrev[0] = mScreenSize.x;
    mCommonSettings.rectSizePrev[1] = mScreenSize.y;
    mCommonSettings.enableValidation = mEnableValidationLayer;
    mCommonSettings.splitScreen = mEnableSplitScreen ? mSplitScreenValue : 0.0f;

    mPrevViewMatrix = viewMatrix;
    mPrevProjMatrix = projMatrix;
    mPrevCameraJitter = cameraJitter;
    mFrameIndex++;

    nrd::Result result = nrd::SetCommonSettings(*mpInstance, mCommonSettings);
    FALCOR_ASSERT(result == nrd::Result::SUCCESS)

    // Run NRD dispatches.
    const nrd::DispatchDesc* dispatchDescs = nullptr;
    uint32_t dispatchDescNum = 0;
    nrd::Identifier denoiser = nrd::Identifier(getNrdDenoiser(mDenoisingMethod));   
    result = nrd::GetComputeDispatches(*mpInstance, &denoiser, 1, dispatchDescs, dispatchDescNum);
    FALCOR_ASSERT(result == nrd::Result::SUCCESS);

    for (uint32_t i = 0; i < dispatchDescNum; i++)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[i];
        FALCOR_PROFILE(pRenderContext, dispatchDesc.name);
        dispatch(pRenderContext, renderData, dispatchDesc);
    }

    // Submit the existing command list and start a new one.
    pRenderContext->flush();
}

void NRDPass::dispatch(RenderContext* pRenderContext, const RenderData& renderData, const nrd::DispatchDesc& dispatchDesc)
{
    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);
    const nrd::PipelineDesc& pipelineDesc = instanceDesc.pipelines[dispatchDesc.pipelineIndex];

    // Set root signature.
    mpRootSignatures[dispatchDesc.pipelineIndex]->bindForCompute(pRenderContext);

    // Upload constants.
    mpConstantBuffer->setBlob(dispatchDesc.constantBufferData, 0, dispatchDesc.constantBufferDataSize);

    // Create descriptor set for the NRD pass.
    ref<D3D12DescriptorSet> CBVSRVUAVDescriptorSet = D3D12DescriptorSet::create(mpDevice, mCBVSRVUAVdescriptorSetLayouts[dispatchDesc.pipelineIndex], D3D12DescriptorSetBindingUsage::ExplicitBind);

    // Set CBV.
    mpCBV = D3D12ConstantBufferView::create(mpDevice, mpConstantBuffer);
    CBVSRVUAVDescriptorSet->setCbv(0 /* NB: range #0 is CBV range */, instanceDesc.constantBufferRegisterIndex, mpCBV.get());

    uint32_t resourceIndex = 0;
    for (uint32_t resourceRangeIndex = 0; resourceRangeIndex < pipelineDesc.resourceRangesNum; resourceRangeIndex++)
    {
        const nrd::ResourceRangeDesc& nrdResourceRange = pipelineDesc.resourceRanges[resourceRangeIndex];

        for (uint32_t resourceOffset = 0; resourceOffset < nrdResourceRange.descriptorsNum; resourceOffset++)
        {
            FALCOR_ASSERT(resourceIndex < dispatchDesc.resourcesNum);
            const nrd::ResourceDesc& resourceDesc = dispatchDesc.resources[resourceIndex];
            
            FALCOR_ASSERT(resourceDesc.descriptorType == nrdResourceRange.descriptorType);

            ref<Texture> texture;

            switch (resourceDesc.type)
            {
            case nrd::ResourceType::IN_MV:
                texture = renderData.getTexture(kInputMotionVectors);
                break;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                texture = renderData.getTexture(kInputNormalRoughnessMaterialID);
                break;
            case nrd::ResourceType::IN_VIEWZ:
                texture = renderData.getTexture(kInputViewZ);
                break;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                texture = renderData.getTexture(kInputDiffuseRadianceHitDist);
                break;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                texture = renderData.getTexture(kInputSpecularRadianceHitDist);
                break;
            case nrd::ResourceType::IN_SPEC_HITDIST:
                texture = renderData.getTexture(kInputSpecularHitDist);
                break;
            case nrd::ResourceType::IN_DELTA_PRIMARY_POS:
                texture = renderData.getTexture(kInputDeltaPrimaryPosW);
                break;
            case nrd::ResourceType::IN_DELTA_SECONDARY_POS:
                texture = renderData.getTexture(kInputDeltaSecondaryPosW);
                break;
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                texture = renderData.getTexture(kOutputFilteredDiffuseRadianceHitDist);
                break;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                texture = renderData.getTexture(kOutputFilteredSpecularRadianceHitDist);
                break;
            case nrd::ResourceType::OUT_VALIDATION:
                texture = renderData.getTexture(kOutputValidation);
                break;
            /*
            case nrd::ResourceType::OUT_REFLECTION_MV:
                texture = renderData.getTexture(kOutputReflectionMotionVectors);
                break;
            case nrd::ResourceType::OUT_DELTA_MV:
                texture = renderData.getTexture(kOutputDeltaMotionVectors);
                break;
            */
            case nrd::ResourceType::TRANSIENT_POOL:
                texture = mpTransientTextures[resourceDesc.indexInPool];
                break;
            case nrd::ResourceType::PERMANENT_POOL:
                texture = mpPermanentTextures[resourceDesc.indexInPool];
                break;
            default:
                FALCOR_ASSERT(!"Unavailable resource type");
                break;
            }

            FALCOR_ASSERT(texture);

            // Set up resource barriers.
            Resource::State newState = resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE ? Resource::State::ShaderResource
                                                                                                   : Resource::State::UnorderedAccess;
            {
                const ResourceViewInfo viewInfo = ResourceViewInfo(0, 1, 0, 1);
                pRenderContext->resourceBarrier(texture.get(), newState, &viewInfo);
            }

            // Set the SRV and UAV descriptors.
            if (nrdResourceRange.descriptorType == nrd::DescriptorType::TEXTURE)
            {
                ref<ShaderResourceView> pSRV = texture->getSRV(0, 1, 0, 1);
                CBVSRVUAVDescriptorSet->setSrv(
                    resourceRangeIndex + 1 /* NB: range #0 is CBV range */, nrdResourceRange.baseRegisterIndex + resourceOffset, pSRV.get()
                );
            }
            else
            {
                ref<UnorderedAccessView> pUAV = texture->getUAV(0, 0, 1);
                CBVSRVUAVDescriptorSet->setUav(
                    resourceRangeIndex + 1 /* NB: range #0 is CBV range */, nrdResourceRange.baseRegisterIndex + resourceOffset, pUAV.get()
                );
            }

            resourceIndex++;
        }
    }

    FALCOR_ASSERT(resourceIndex == dispatchDesc.resourcesNum);

    // Set descriptor sets.
    mpSamplersDescriptorSet->bindForCompute(pRenderContext, mpRootSignatures[dispatchDesc.pipelineIndex].get(), 0);
    CBVSRVUAVDescriptorSet->bindForCompute(pRenderContext, mpRootSignatures[dispatchDesc.pipelineIndex].get(), 1);

    // Set pipeline state.
    ref<ComputePass> pPass = mpPasses[dispatchDesc.pipelineIndex];
    ref<ComputeProgram> pProgram = pPass->getProgram();
    ref<const ProgramKernels> pProgramKernels = pProgram->getActiveVersion()->getKernels(mpDevice.get(), pPass->getVars().get());

    // Check if anything changed.
    bool newProgram = (pProgramKernels.get() != mpCachedProgramKernels[dispatchDesc.pipelineIndex].get());
    if (newProgram)
    {
        mpCachedProgramKernels[dispatchDesc.pipelineIndex] = pProgramKernels;

        ComputeStateObject::Desc desc;
        desc.setProgramKernels(pProgramKernels);
        desc.setD3D12RootSignatureOverride(mpRootSignatures[dispatchDesc.pipelineIndex]);

        ref<ComputeStateObject> pCSO = ComputeStateObject::create(mpDevice, desc);
        mpCSOs[dispatchDesc.pipelineIndex] = pCSO;
    }
    ID3D12GraphicsCommandList* pCommandList = pRenderContext->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList*>();
    ID3D12PipelineState* pPipelineState = mpCSOs[dispatchDesc.pipelineIndex]->getNativeHandle().as<ID3D12PipelineState*>();

    pCommandList->SetPipelineState(pPipelineState);

    // Dispatch.
    pCommandList->Dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);
}

extern "C" FALCOR_API_EXPORT void registerPlugin(PluginRegistry& registry)
{
    registry.registerClass<RenderPass, NRDPass>();
}
