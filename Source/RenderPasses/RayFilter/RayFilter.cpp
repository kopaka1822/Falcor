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
#include "RayFilter.h"

const RenderPass::Info RayFilter::kInfo { "RayFilter", "Filters the ray bitmask to improve performance" };

namespace
{
    const char kShaderPath[] = "RenderPasses/RayFilter/Blur.ps.hlsl";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RayFilter::kInfo, RayFilter::create);
}

RayFilter::SharedPtr RayFilter::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new RayFilter());
    return pPass;
}

Dictionary RayFilter::getScriptingDictionary()
{
    return Dictionary();
}



RayFilter::RayFilter() : RenderPass(kInfo)
{
    mpFbo = Fbo::create();
}

RenderPassReflection RayFilter::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addOutput("dst");
    //reflector.addInput("src");
    return reflector;
}

void RayFilter::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    // auto& pTexture = renderData["src"]->asTexture();
    throw std::runtime_error("RayFilter::execute() is not implemented");
}

void RayFilter::execute(RenderContext* pRenderContext, const Texture::SharedPtr& pInput,
    const Texture::SharedPtr& pOutput)
{
    if(!mpPass)
    {
        Program::DefineList defines;
        defines.add("KERNEL_RADIUS", std::to_string(mKernelRadius));
        defines.add("MIN_COUNT", std::to_string(mMinCount));
        mpPass = FullScreenPass::create(kShaderPath, defines);
    }

    mpPass["gInput"] = pInput;
    mpFbo->attachColorTarget(pOutput, 0);
    mpPass->execute(pRenderContext, mpFbo);
}

void RayFilter::renderUI(Gui::Widgets& widget)
{
    widget.var("Kernel Radius", mKernelRadius, 1, 10);
    widget.var("Min Count", mMinCount, 1, 100);
}
