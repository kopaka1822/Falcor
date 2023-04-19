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
#include "RaytracedShadow.h"

const RenderPass::Info RaytracedShadow::kInfo { "RaytracedShadow", "Calculates a raytraced visibility map for the first light (only directional lights)" };

namespace 
{
    const std::string kPos = "posW";
    const std::string kVisibility = "visibility";

    const std::string kRasterShader = "RenderPasses/RaytracedShadow/Shadow.ps.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RaytracedShadow::kInfo, RaytracedShadow::create);
}

RaytracedShadow::SharedPtr RaytracedShadow::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new RaytracedShadow());
    return pPass;
}

RaytracedShadow::RaytracedShadow() : RenderPass(kInfo)
{
    mpFbo = Fbo::create();
}

Dictionary RaytracedShadow::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection RaytracedShadow::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kPos, "Pre-initialized scene depth buffer").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kVisibility, "Visibility map. Values are [0,1] where 0 means the pixel is completely shadowed and 1 means it's not shadowed at all")
        .format(ResourceFormat::R8Unorm)
        .texture2D(0, 0).
        bindFlags(ResourceBindFlags::RenderTarget);
    return reflector;
}

void RaytracedShadow::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pPos = renderData[kPos]->asTexture();
    auto pVisibility = renderData[kVisibility]->asTexture();

    // clear visibility texture
    pRenderContext->clearTexture(pVisibility.get(), glm::vec4(1, 1, 1, 1));
    if (!mpScene) return;

    if(!mpPass)
    {
        Program::DefineList defines;
        defines.add(mpScene->getSceneDefines());
        mpPass = FullScreenPass::create(kRasterShader, defines);
        mpPass->getProgram()->setTypeConformances(mpScene->getTypeConformances());
    }

    mpPass["gPos"] = pPos;

    // raytracing data
    auto var = mpPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);

    mpFbo->attachColorTarget(pVisibility, 0);
    mpPass->execute(pRenderContext, mpFbo);
}

void RaytracedShadow::renderUI(Gui::Widgets& widget)
{
}

void RaytracedShadow::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpPass.reset();
}
