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
    reflector.addInput(kInput, "noisy input image");
    reflector.addOutput(kOutput, "denoised output image");
    
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
}

void NVIDIADenoiser::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    // auto& pTexture = renderData["src"]->asTexture();

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
    nri::TextureTransitionBarrierDesc entryDescs[N] = {};
    //nri::Format entryFormat[N] = {};

    for (uint32_t i = 0; i < N; i++)
    {
        nri::TextureTransitionBarrierDesc& entryDesc = entryDescs[i];
        const MyResource& myResource = GetMyResource(i);

        nri::TextureD3D12Desc textureDesc = {};
        textureDesc.d3d12Resource = myResource->GetNativePointer();
        NRI.CreateTextureD3D12(*nriDevice, textureDesc, (nri::Texture*&)entryDesc.texture );

        // You need to specify the current state of the resource here, after denoising NRD can modify
        // this state. Application must continue state tracking from this point.
        // Useful information:
        //    SRV = nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE
        //    UAV = nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL
        entryDesc.nextAccess = ConvertResourceStateToAccessBits( myResource->GetCurrentState() );
        entryDesc.nextLayout = ConvertResourceStateToLayout( myResource->GetCurrentState() );
    }

    //====================================================================================================================
    // STEP 5 - DENOISE
    //====================================================================================================================

    // Populate common settings
    //  - for the first time use defaults
    //  - currently NRD supports only the following view space: X - right, Y - top, Z - forward or backward
    nrd::CommonSettings commonSettings = {};
    //PopulateCommonSettings(commonSettings);

    // Set settings for each denoiser
    nrd::ReblurDiffuseSettings settings = {};
    //PopulateDenoiserSettings(settings);

    NRD->SetMethodSettings(nrd::Method::REBLUR_DIFFUSE_OCCLUSION, &settings);

    // Fill up the user pool
    NrdUserPool userPool =
    {{
        // Fill the required inputs and outputs in appropriate slots using entryDescs & entryFormat,
        // applying remapping if necessary. Unused slots can be {nullptr, nri::Format::UNKNOWN}
    }};

    NRD->Denoise(m_frameIndex, *cmdBuffer, commonSettings, userPool);
    m_frameIndex = 1 - m_frameIndex;
}


void NVIDIADenoiser::renderUI(Gui::Widgets& widget)
{

}
