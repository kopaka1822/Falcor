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
#include "WriteStencil.h"


namespace
{
    const char kDesc[] = "Copies the input texture into a stencil buffer";
    const std::string kInput = "input";
    const std::string kDepthStencil = "depthStencil";
}

const RenderPass::Info WriteStencil::kInfo{ "WriteStencil",  kDesc };

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(WriteStencil::kInfo, WriteStencil::create);
}

WriteStencil::SharedPtr WriteStencil::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new WriteStencil());
    return pPass;
}

Dictionary WriteStencil::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection WriteStencil::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kInput, "source").format(ResourceFormat::R8Uint);
    reflector.addInputOutput(kDepthStencil, "depth buffer with stencil attachment").format(ResourceFormat::D32FloatS8X24);
    return reflector;
}

void WriteStencil::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pInput = renderData[kInput]->asTexture();
    auto pDepthStencil = renderData[kDepthStencil]->asTexture();

    pRenderContext->copySubresource(pDepthStencil.get(), 1, pInput.get(), 0);
    logError("WriteStencil needs to be rewritten for better performance => clear stencil and copy mask");
}

void WriteStencil::renderUI(Gui::Widgets& widget)
{
}

WriteStencil::WriteStencil()
    :
    RenderPass(kInfo)
{

}
