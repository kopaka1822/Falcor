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
#include "LinearizeDepth.h"


namespace
{
    const char kDesc[] = "Converts non-linear depth to linear view depth";
    const std::string kDepthIn = "depth";
    const std::string kDepthOut = "linearDepth";
    const std::string kShaderFilename = "RenderPasses/LinearizeDepth/linearize.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("LinearizeDepth", kDesc, LinearizeDepth::create);
}

LinearizeDepth::SharedPtr LinearizeDepth::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new LinearizeDepth);
    return pPass;
}

std::string LinearizeDepth::getDesc() { return kDesc; }

Dictionary LinearizeDepth::getScriptingDictionary()
{
    return Dictionary();
}

LinearizeDepth::LinearizeDepth()
{
    mpPass = FullScreenPass::create(kShaderFilename);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    mpPass["s"] = Sampler::create(samplerDesc);

    mpFbo = Fbo::create();
}

RenderPassReflection LinearizeDepth::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepthIn, "non-linear depth").bindFlags(Resource::BindFlags::ShaderResource);
    reflector.addOutput(kDepthOut, "linear view-depth").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews);
    return reflector;
}

void LinearizeDepth::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto depthIn = renderData[kDepthIn]->asTexture();
    auto depthOut = renderData[kDepthOut]->asTexture();

    auto pCamera = mpScene->getCamera();
    
    mpFbo->attachColorTarget(depthOut, 0);
    mpPass["depths"] = depthIn;

    float zNear = pCamera->getNearPlane();
    float zFar = pCamera->getFarPlane();
    if(mLastZNear != zNear || mLastZFar != zFar)
    {
        mpPass["StaticCB"]["zNear"] = zNear;
        mpPass["StaticCB"]["zFar"] = zFar;
        mLastZNear = zNear;
        mLastZFar = zFar;
    }

    mpPass->execute(pRenderContext, mpFbo);
}

void LinearizeDepth::renderUI(Gui::Widgets& widget)
{
}
