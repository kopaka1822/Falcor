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
#include "HBAOPlus.h"

namespace
{
    const char kDesc[] = "Horizon Based Ambient Occlusion Plus from NVIDIAGameworks";
    const std::string kDepth = "depth";
    const std::string kNormal = "normals";
    const std::string kAmbientMap = "ambientMap";
}

// additional descriptors needed to pass in depth x2 and normals
static const uint32_t kNumAppDescriptors = 3;

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

static void regHBAOPlus(pybind11::module& m)
{
    pybind11::class_<HBAOPlus, RenderPass, HBAOPlus::SharedPtr> pass(m, "HBAOPlus");
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("HBAOPlus", kDesc, HBAOPlus::create);
    ScriptBindings::registerBinding(regHBAOPlus);
}

HBAOPlus::SharedPtr HBAOPlus::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new HBAOPlus);
    return pPass;
}

std::string HBAOPlus::getDesc() { return kDesc; }

Dictionary HBAOPlus::getScriptingDictionary()
{
    return Dictionary();
}

HBAOPlus::HBAOPlus()
{
    auto pDevice = gpDevice->getApiHandle();

    // create descriptor heaps for the ssao pass
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.NumDescriptors = kNumAppDescriptors + GFSDK_SSAO_NUM_DESCRIPTORS_CBV_SRV_UAV_HEAP_D3D12;
    descriptorHeapDesc.NodeMask = 0;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    pDevice->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&mSSAODescriptorHeapCBVSRVUAV));

    descriptorHeapDesc.NumDescriptors = GFSDK_SSAO_NUM_DESCRIPTORS_RTV_HEAP_D3D12;
    descriptorHeapDesc.NodeMask = 0;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    
    pDevice->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&mSSAODescriptorHeapRTV));

    mparams.Radius = 2.0f;
    mparams.Bias = 0.1f;
    mparams.PowerExponent = 2.0f;
    mparams.Blur.Enable = true;
    mparams.Blur.Radius = GFSDK_SSAO_BLUR_RADIUS_4;
    mparams.Blur.Sharpness = 16.0f;
    mparams.EnableDualLayerAO = false;
    // can be done to increase precision of linear depths:
    //mparams.DepthStorage = GFSDK_SSAO_DepthStorage::GFSDK_SSAO_FP32_VIEW_DEPTHS;
}

HBAOPlus::~HBAOPlus()
{
    if(mpAOContext) mpAOContext->Release();
    if(mSSAODescriptorHeapCBVSRVUAV) mSSAODescriptorHeapCBVSRVUAV->Release();
    if(mSSAODescriptorHeapRTV) mSSAODescriptorHeapRTV->Release();
}

RenderPassReflection HBAOPlus::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "non-linear depth map").bindFlags(Falcor::ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormal, "surface normals").bindFlags(Falcor::ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "ambient occlusion").bindFlags(Falcor::ResourceBindFlags::RenderTarget);//.format(ResourceFormat::R8Unorm);
    return reflector;
}

void HBAOPlus::compile(RenderContext* pContext, const CompileData& compileData)
{
    GFSDK_SSAO_DescriptorHeaps_D3D12 descHeaps;
    descHeaps.CBV_SRV_UAV.pDescHeap = mSSAODescriptorHeapCBVSRVUAV;
    descHeaps.CBV_SRV_UAV.BaseIndex = kNumAppDescriptors;
    descHeaps.RTV.pDescHeap = mSSAODescriptorHeapRTV;
    descHeaps.RTV.BaseIndex = 0;

    if(mpAOContext)
    {
        mpAOContext->Release();
        mpAOContext = nullptr;
    }
    
    GFSDK_SSAO_Status status;
    status = GFSDK_SSAO_CreateContext_D3D12(gpDevice->getApiHandle(), 1, descHeaps, &mpAOContext);
    assert(status == GFSDK_SSAO_OK);
    switch(status)
    {
        case GFSDK_SSAO_NULL_ARGUMENT: throw std::runtime_error("HBAO+: One of the required argument pointers is NULL");
        case GFSDK_SSAO_VERSION_MISMATCH: throw std::runtime_error("HBAO+: Invalid HeaderVersion (have you set HeaderVersion = GFSDK_SSAO_Version()?)");
        case GFSDK_SSAO_MEMORY_ALLOCATION_FAILED: throw std::runtime_error("HBAO+: Failed to allocate memory on the heap");
        case GFSDK_SSAO_D3D_FEATURE_LEVEL_NOT_SUPPORTED: throw std::runtime_error("HBAO+: The D3D feature level of pD3DDevice is lower than 11_0");
        case GFSDK_SSAO_D3D_RESOURCE_CREATION_FAILED: throw std::runtime_error("HBAO+: A resource-creation call has failed (running out of memory?)");
        case GFSDK_SSAO_D3D12_INVALID_HEAP_TYPE: throw std::runtime_error("HBAO+: One of the heaps provided to GFSDK_SSAO_CreateContext_D3D12 has an unexpected type");
        case GFSDK_SSAO_D3D12_INSUFFICIENT_DESCRIPTORS: throw std::runtime_error("HBAO+: One of the heaps described in pHeapInfo has an insufficient number of descriptors");
        case GFSDK_SSAO_D3D12_INVALID_NODE_MASK: throw std::runtime_error("HBAO+: NodeMask has more than one bit set, or is zero");
    }
}

void HBAOPlus::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if(!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pNormal = renderData[kNormal]->asTexture();
    auto pAmientMap = renderData[kAmbientMap]->asTexture();
    auto ambientRtv = pAmientMap->getRTV();

    // transition resources into expected state
    pRenderContext->resourceBarrier(pDepth.get(), Falcor::Resource::State::ShaderResource);
    pRenderContext->resourceBarrier(pNormal.get(), Falcor::Resource::State::ShaderResource);
    pRenderContext->resourceBarrier(pAmientMap.get(), Falcor::Resource::State::RenderTarget);


    // initialize srv description for depth buffer
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSRVDesc = {};
    switch(pDepth->getFormat())
    {
    case Falcor::ResourceFormat::D16Unorm:
        depthSRVDesc.Format = DXGI_FORMAT_R16_UNORM;
        break;
    case Falcor::ResourceFormat::D32Float:
        depthSRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        break;
    case Falcor::ResourceFormat::D24UnormS8:
        depthSRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case Falcor::ResourceFormat::D32FloatS8X24:
        depthSRVDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    default: throw std::runtime_error("depth input for HBAOPlus is not a depth format!");
    }
    depthSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MipLevels = 1;
    depthSRVDesc.Texture2D.MostDetailedMip = 0; // No MIP
    depthSRVDesc.Texture2D.PlaneSlice = 0;
    depthSRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    auto curSrvHeapAddress = mSSAODescriptorHeapCBVSRVUAV->GetCPUDescriptorHandleForHeapStart();
    gpDevice->getApiHandle()->CreateShaderResourceView(pDepth->getApiHandle(), &depthSRVDesc, curSrvHeapAddress);

    // initialize srv description for normal
    D3D12_SHADER_RESOURCE_VIEW_DESC normalSrvDesc = {};
    assert(pNormal->getFormat() == Falcor::ResourceFormat::RGBA32Float);
    normalSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    normalSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    normalSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    normalSrvDesc.Texture2D.MipLevels = 1;
    normalSrvDesc.Texture2D.MostDetailedMip = 0; // No MIP
    normalSrvDesc.Texture2D.PlaneSlice = 0;
    normalSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    curSrvHeapAddress.ptr += 2 * gpDevice->getApiHandle()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gpDevice->getApiHandle()->CreateShaderResourceView(pNormal->getApiHandle(), &normalSrvDesc, curSrvHeapAddress);

    // set depth texture in descirptor 
    GFSDK_SSAO_InputData_D3D12 input;
    input.DepthData.DepthTextureType = GFSDK_SSAO_HARDWARE_DEPTHS;
    input.DepthData.FullResDepthTexture2ndLayerSRV = {}; // no second texture
    // set depth texture to first slot in descriptor heap
    input.DepthData.FullResDepthTextureSRV.pResource = pDepth->getApiHandle();
    input.DepthData.FullResDepthTextureSRV.GpuHandle = mSSAODescriptorHeapCBVSRVUAV->GetGPUDescriptorHandleForHeapStart().ptr;

    const auto& projMatrix = mpScene->getCamera()->getProjMatrix();
    const auto& viewMatrix =  mpScene->getCamera()->getViewMatrix();
    static_assert(sizeof(projMatrix) == sizeof(GFSDK_SSAO_Float4x4));

    input.DepthData.MetersToViewSpaceUnits = 1.0f;
    input.DepthData.ProjectionMatrix.Data = GFSDK_SSAO_Float4x4(reinterpret_cast<const float*>(&projMatrix));
    input.DepthData.ProjectionMatrix.Layout = GFSDK_SSAO_ROW_MAJOR_ORDER;
    input.DepthData.Viewport.Enable = false; // use default texture viewport

    input.NormalData.Enable = m_useNormalData; // do not use input normals
    input.NormalData.WorldToViewMatrix.Data = GFSDK_SSAO_Float4x4(reinterpret_cast<const float*>(&viewMatrix));
    input.NormalData.WorldToViewMatrix.Layout = GFSDK_SSAO_ROW_MAJOR_ORDER; 
    input.NormalData.FullResNormalTextureSRV.pResource = pNormal->getApiHandle();
    input.NormalData.FullResNormalTextureSRV.GpuHandle = mSSAODescriptorHeapCBVSRVUAV->GetGPUDescriptorHandleForHeapStart().ptr + 2 * gpDevice->getApiHandle()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);;
    input.NormalData.DecodeScale = -1.0f;
    input.NormalData.DecodeBias = 0.0f;

    // set render target
    GFSDK_SSAO_Output_D3D12 output;
    GFSDK_SSAO_RenderTargetView_D3D12 outputRtv;
    outputRtv.pResource = pAmientMap->getApiHandle();
    outputRtv.CpuHandle = ambientRtv->getApiHandle()->getCpuHandle(0).ptr;
    output.pRenderTargetView = &outputRtv;
    output.Blend.Mode = GFSDK_SSAO_OVERWRITE_RGB;
    //output.Blend.Mode = GFSDK_SSAO_MULTIPLY_RGB;

    // render
    ID3D12CommandQueue* commandQueue = pRenderContext->getLowLevelData()->getCommandQueue();
    ID3D12GraphicsCommandList* commandList = pRenderContext->getLowLevelData()->getCommandList();

    // set ssao descriptor heaps
    commandList->SetDescriptorHeaps(1, &mSSAODescriptorHeapCBVSRVUAV);

    auto status = mpAOContext->RenderAO(commandQueue, commandList, input, mparams, output);
    assert(status == GFSDK_SSAO_OK);

    // restore descriptor heaps
    pRenderContext->bindDescriptorHeaps();
}

void HBAOPlus::renderUI(Gui::Widgets& widget)
{
    widget.slider("Radius", mparams.Radius, 1.0f, 32.0f);
    widget.slider("Bias", mparams.Bias, 0.0f, 0.5f);
    widget.slider("Small Scale Factor", mparams.SmallScaleAO, 0.0f, 2.0f);
    widget.slider("Large Scale Factor", mparams.LargeScaleAO, 0.0f, 2.0f);
    widget.slider("Power Exponent", mparams.PowerExponent, 1.0f, 4.0f);

    widget.checkbox("Use Normals", m_useNormalData);

    widget.checkbox("Blur", *reinterpret_cast<bool*>(&mparams.Blur.Enable));
    if(mparams.Blur.Enable)
    {
        widget.slider("Sharpness", mparams.Blur.Sharpness, 0.0f, 16.0f);
    }
}

void HBAOPlus::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
}
