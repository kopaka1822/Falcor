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
#include "IntelXeSS.h"
#include "Core/API/NativeHandleTraits.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, IntelXeSS>();
}

namespace
{
    const std::string kColorIn = "color";
    const std::string kDepth = "depth";
    const std::string kMotion = "mvec";
    // const std::string kExposure = "exposure"; // Optional resource containing a 1x1 exposure value.
    const std::string kOutput = "output";
}

IntelXeSS::IntelXeSS(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
}

Properties IntelXeSS::getProperties() const
{
    return {};
}

RenderPassReflection IntelXeSS::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kColorIn, "Color buffer for the current frame (at render resolution)");
    reflector.addInput(kDepth, "32bit depth values for the current frame (at render resolution)");
    reflector.addInput(kMotion, "2-dimensional motion vectors");

    reflector.addOutput(kOutput, "Output texture").format(ResourceFormat::RGBA32Float).bindFlags(Resource::BindFlags::AllColorViews);
    return reflector;
}

void IntelXeSS::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    auto pNative = mpDevice->getNativeHandle().as<ID3D12Device*>();
}

void IntelXeSS::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    mTimer.update();
    auto pColorIn = renderData.getTexture(kColorIn);
    auto pDepth = renderData.getTexture(kDepth);
    auto pMotion = renderData.getTexture(kMotion);
    auto pOut = renderData.getTexture(kOutput);

    if (!mpScene || !mEnabled)
    {
        pRenderContext->blit(pColorIn->getSRV(), pOut->getRTV());
        return;
    }

    auto pCamera = mpScene->getCamera();

    ID3D12GraphicsCommandList* pCommandList = pRenderContext->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList*>();




    mReset = false;
    pRenderContext->setPendingCommands(true);
    pRenderContext->uavBarrier(pOut.get());
    pRenderContext->flush();
}

void IntelXeSS::renderUI(Gui::Widgets& widget)
{
}
