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
}

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

RenderPassReflection HBAOPlus::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput("depth", "non-linear depth map");
    reflector.addOutput("ambientMap", "ambient occlusion");
    return reflector;
}

void HBAOPlus::compile(RenderContext* pContext, const CompileData& compileData)
{
    // initialize the library
    GFSDK_SSAO_DescriptorHeaps_D3D12 descHeaps;
    //descHeaps.CBV_SRV_UAV 

    GFSDK_SSAO_Status status;
    GFSDK_SSAO_Context_D3D12* pAOContext;
    status = GFSDK_SSAO_CreateContext_D3D12(gpDevice->getApiHandle(), 1, descHeaps, &pAOContext);
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
    // renderData holds the requested resources
    // auto& pTexture = renderData["src"]->asTexture();
}

void HBAOPlus::renderUI(Gui::Widgets& widget)
{
}
