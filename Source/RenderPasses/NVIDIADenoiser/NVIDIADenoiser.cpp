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
#include "NVIDIADenoiser.h"


namespace
{
    const char kDesc[] = "NVIDIA Denoiser";
    const std::string kInput = "input";
    const std::string kOutput = "output";

    // more inputs
    const std::string kMotionVectors = "motion vector";
    const std::string kViewZ = "linear depth";
    const std::string kNormalRoughness = "normalRoughness";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("NVIDIADenoiser", kDesc, NVIDIADenoiser::create);
}

NVIDIADenoiser::SharedPtr NVIDIADenoiser::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new NVIDIADenoiser);
    return pPass;
}

std::string NVIDIADenoiser::getDesc() { return kDesc; }

Dictionary NVIDIADenoiser::getScriptingDictionary()
{
    return Dictionary();
}

NVIDIADenoiser::NVIDIADenoiser()
{
    //====================================================================================================================
    // STEP 2 - WRAP NATIVE DEVICE
    //====================================================================================================================

    nri::DeviceCreationD3D12Desc deviceDesc = {};
    deviceDesc.d3d12Device = gpDevice->getApiHandle();
    deviceDesc.d3d12PhysicalAdapter = nullptr; // sett Falcor/Core/API/D3D12 IDXGIAdapter
    deviceDesc.d3d12GraphicsQueue = gpDevice->getRenderContext()->getLowLevelData()->getCommandQueue();
    deviceDesc.enableNRIValidation = false;

    // Wrap the device
    nri::Result result = nri::CreateDeviceFromD3D12Device(deviceDesc, nriDevice);

    // Get needed functionality
    result = nri::GetInterface(*nriDevice, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&NRI);
    result = nri::GetInterface(*nriDevice, NRI_INTERFACE(nri::HelperInterface), (nri::HelperInterface*)&NRI);

    // Get needed "wrapper" extension, XXX - can be D3D11, D3D12 or VULKAN
    result = nri::GetInterface(*nriDevice, NRI_INTERFACE(nri::WrapperD3D12Interface), (nri::WrapperD3D12Interface*)&NRI);
}

NVIDIADenoiser::~NVIDIADenoiser()
{
    //====================================================================================================================
    // STEP 6 - CLEANUP
    //====================================================================================================================

    //for (uint32_t i = 0; i < N; i++)
    //    NRI.DestroyTexture(entryDescs[i].texture);

    //NRI.DestroyCommandBuffer(*cmdBuffer);

    //====================================================================================================================
    // STEP 7 - DESTROY
    //====================================================================================================================

    if(NRD) NRD->Destroy();
}

RenderPassReflection NVIDIADenoiser::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kInput, "noisy input image").format(ResourceFormat::R8Unorm).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kMotionVectors, "3D world space motion (RGBA16f+) or 2D screen space motion (RG16f+), MVs must be non-jittered, MV = previous - current").format(ResourceFormat::RG32Float).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormalRoughness, "RGBA8+ or R10G10B10A2+ depending on encoding (UNORM FORMAT)").format(ResourceFormat::RGBA8Unorm).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kViewZ, "Linear view depth for primary rays (R16f+)").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kOutput, "denoised output image").format(ResourceFormat::R8Unorm).bindFlags(ResourceBindFlags::UnorderedAccess);
    
    return reflector;
}

void NVIDIADenoiser::compile(RenderContext* pContext, const CompileData& compileData)
{
    //====================================================================================================================
    // STEP 3 - INITIALIZE NRD
    //====================================================================================================================
    if(NRD) NRD->Destroy();

    // initialize NRD
    NRD.emplace(2); // 2 = max number of in flight frames

    const nrd::MethodDesc methodDescs[] =
    {
        // put neeeded methods here, like:
        { nrd::Method::REBLUR_DIFFUSE_OCCLUSION, uint16_t(compileData.defaultTexDims.x), uint16_t(compileData.defaultTexDims.y) },
    };

    nrd::DenoiserCreationDesc denoiserCreationDesc = {};
    denoiserCreationDesc.requestedMethods = methodDescs;
    denoiserCreationDesc.requestedMethodNum = 1;
    
    bool bresult = NRD->Initialize(*nriDevice, NRI, NRI, denoiserCreationDesc);
    if(!bresult) throw std::runtime_error("could not initialize NRD");
    
    mLastTime = std::chrono::high_resolution_clock::now();
    mCommonSettings.frameIndex = 0;
}

static void copyMatrix(float* dst, const glm::mat4& mat)
{
    memcpy(dst, &mat, sizeof(glm::mat4));
}

void NVIDIADenoiser::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if(!mpScene) return;

    auto pInput = renderData[kInput]->asTexture();
    auto pOutput = renderData[kOutput]->asTexture();
    auto pMotionVec = renderData[kMotionVectors]->asTexture();
    auto pViewZ = renderData[kViewZ]->asTexture();
    auto pNormalRoughness = renderData[kNormalRoughness]->asTexture();

    if(!mEnabled)
    {
        pRenderContext->copySubresource(pOutput.get(), 0, pInput.get(), 0);
        return;
    }

    // make sure all input resources have shader resource state
    pRenderContext->resourceBarrier(pInput.get(), Resource::State::ShaderResource);
    pRenderContext->resourceBarrier(pMotionVec.get(), Resource::State::ShaderResource);
    pRenderContext->resourceBarrier(pViewZ.get(), Resource::State::ShaderResource);
    pRenderContext->resourceBarrier(pNormalRoughness.get(), Resource::State::ShaderResource);

    // output should be in UAV state
    pRenderContext->resourceBarrier(pOutput.get(), Resource::State::UnorderedAccess);

    //====================================================================================================================
    // STEP 4 - WRAP NATIVE POINTERS
    //====================================================================================================================

    // Wrap the command buffer
    nri::CommandBufferD3D12Desc cmdDesc = {};
    cmdDesc.d3d12CommandList = pRenderContext->getLowLevelData()->getCommandList();
    cmdDesc.d3d12CommandAllocator = nullptr; // Not needed for NRD Integration layer

    nri::CommandBuffer* cmdBuffer = nullptr;
    NRI.CreateCommandBufferD3D12(*nriDevice, cmdDesc, cmdBuffer);

    // Wrap required textures
    nri::TextureTransitionBarrierDesc entryDescs[5] = {};
    nri::Format entryFormat[5] = {};

    //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
    //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL

    nri::TextureD3D12Desc textureDesc = {};
    // motion vec IN_MV
    textureDesc.d3d12Resource = pMotionVec->getApiHandle();
    NRI.CreateTextureD3D12(*nriDevice, textureDesc, entryDescs[0].texture);
    entryDescs[0].nextAccess = nri::AccessBits::SHADER_RESOURCE;
    entryDescs[0].nextLayout = nri::TextureLayout::SHADER_RESOURCE;
    // normal roughness IN_NORMAL_ROUGHNESS
    textureDesc.d3d12Resource = pNormalRoughness->getApiHandle();
    NRI.CreateTextureD3D12(*nriDevice, textureDesc, entryDescs[1].texture);
    entryDescs[1].nextAccess = nri::AccessBits::SHADER_RESOURCE;
    entryDescs[1].nextLayout = nri::TextureLayout::SHADER_RESOURCE;
    // IN_VIEWZ
    textureDesc.d3d12Resource = pViewZ->getApiHandle();
    NRI.CreateTextureD3D12(*nriDevice, textureDesc, entryDescs[2].texture);
    entryDescs[2].nextAccess = nri::AccessBits::SHADER_RESOURCE;
    entryDescs[2].nextLayout = nri::TextureLayout::SHADER_RESOURCE;
    // IN_DIFF_HITDIST
    textureDesc.d3d12Resource = pInput->getApiHandle();
    NRI.CreateTextureD3D12(*nriDevice, textureDesc, entryDescs[3].texture);
    entryDescs[3].nextAccess = nri::AccessBits::SHADER_RESOURCE;
    entryDescs[3].nextLayout = nri::TextureLayout::SHADER_RESOURCE;
    // OUT OUT_DIFF_HITDIST
    textureDesc.d3d12Resource = pOutput->getApiHandle();
    NRI.CreateTextureD3D12(*nriDevice, textureDesc, entryDescs[4].texture);
    entryDescs[4].nextAccess = nri::AccessBits::SHADER_RESOURCE_STORAGE;
    entryDescs[4].nextLayout = nri::TextureLayout::GENERAL;

    //====================================================================================================================
    // STEP 5 - DENOISE
    //====================================================================================================================

    // Populate common settings
    //  - for the first time use defaults
    //  - currently NRD supports only the following view space: X - right, Y - top, Z - forward or backward
    auto pCam = mpScene->getCamera();
    copyMatrix(mCommonSettings.viewToClipMatrix, pCam->getProjMatrix());
    copyMatrix(mCommonSettings.viewToClipMatrixPrev, pCam->getProjMatrix());
    copyMatrix(mCommonSettings.worldToViewMatrix, pCam->getViewMatrix());
    copyMatrix(mCommonSettings.worldToViewMatrixPrev, pCam->getPrevViewMatrix());
    //mCommonSettings.meterToUnitsMultiplier = 1.0f;
    auto nowTime = std::chrono::high_resolution_clock::now();
    mCommonSettings.timeDeltaBetweenFrames = std::chrono::duration<float>(nowTime - mLastTime).count() * 0.001f;
    mLastTime = nowTime;
    mCommonSettings.frameIndex += 1;
    mCommonSettings.isMotionVectorInWorldSpace = false; // here: provided in screen space
    mCommonSettings.isRadianceMultipliedByExposure = true; 

    // "Normalized hit distance" = saturate( "hit distance" / f ), where:
    // f = ( A + viewZ * B ) * lerp( 1.0, C, exp2( D * roughness ^ 2 ) ), see "NRD.hlsl/REBLUR_FrontEnd_GetNormHitDist"
    // HERE: simplify to: f = ( A + viewZ * B ) = viewZ * B
    mSettings.hitDistanceParameters.A = 0.0f;
    mSettings.hitDistanceParameters.B = 0.1f;
    mSettings.hitDistanceParameters.C = 1.0f; // this ignores the roughness parameter
    //mSettings.maxAccumulatedFrameNum = 16;
    mSettings.blurRadius = 30.0f;
    mSettings.checkerboardMode = nrd::CheckerboardMode::OFF;

    NRD->SetMethodSettings(nrd::Method::REBLUR_DIFFUSE_OCCLUSION, &mSettings);

    // Fill up the user pool
    NrdUserPool userPool = {};
    userPool[(size_t)nrd::ResourceType::IN_MV] = NrdIntegrationTexture{entryDescs, nri::Format::RG32_SFLOAT};
    userPool[(size_t)nrd::ResourceType::IN_NORMAL_ROUGHNESS] = NrdIntegrationTexture{entryDescs + 1, nri::Format::RGBA8_UNORM};
    userPool[(size_t)nrd::ResourceType::IN_VIEWZ] = NrdIntegrationTexture{entryDescs + 2, nri::Format::R32_SFLOAT};
    userPool[(size_t)nrd::ResourceType::IN_DIFF_HITDIST] = NrdIntegrationTexture{entryDescs + 3, nri::Format::R8_UNORM};
    userPool[(size_t)nrd::ResourceType::OUT_DIFF_HITDIST] = NrdIntegrationTexture{entryDescs + 4, nri::Format::R8_UNORM};

    NRD->Denoise(m_frameIndex, *cmdBuffer, mCommonSettings, userPool);
    m_frameIndex = 1 - m_frameIndex;

    NRI.DestroyCommandBuffer(*cmdBuffer);

    // restore descriptor heaps
    pRenderContext->bindDescriptorHeaps();
}


void NVIDIADenoiser::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if(!mEnabled) return;


    widget.var("Max Frame History", mSettings.maxAccumulatedFrameNum, uint32_t(0), nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
    widget.var("Meters To Units", mCommonSettings.meterToUnitsMultiplier, 0.0f);
}
