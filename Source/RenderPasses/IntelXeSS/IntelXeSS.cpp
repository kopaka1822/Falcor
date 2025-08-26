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
#include <xess/xess_d3d12.h>

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
    auto xr = xessD3D12CreateContext(pNative, &mContext);
    if(xr != XESS_RESULT_SUCCESS)
        throw std::runtime_error("Failed to create XeSS context");

    xess_d3d12_init_params_t ip = {};
    ip.outputResolution.x = compileData.defaultTexDims.x;
    ip.outputResolution.y = compileData.defaultTexDims.y;
    ip.qualitySetting = XESS_QUALITY_SETTING_AA; // highest quality?
    xr = xessD3D12Init(mContext, &ip);
    if(xr != XESS_RESULT_SUCCESS)
        throw std::runtime_error("Failed to initialize XeSS context");

    mReset = true;
}

static ID3D12Resource* getNativeResource(const ref<Texture>& pTex)
{
    if (!pTex) return nullptr;
    return pTex->getNativeHandle().as<ID3D12Resource*>();
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

    xess_d3d12_execute_params_t ep = {};
    ep.inputWidth = pColorIn->getWidth();
    ep.inputHeight = pColorIn->getHeight();
    float2 jitterOffset = float2(pCamera->getJitterX(), -pCamera->getJitterY()) * float2(pColorIn->getWidth(), pColorIn->getHeight());
    ep.jitterOffsetX = jitterOffset.x;
    ep.jitterOffsetY = jitterOffset.y;
    ep.resetHistory = mReset ? 1 : 0;
    ep.pColorTexture = getNativeResource(pColorIn);
    ep.pDepthTexture = getNativeResource(pDepth);
    ep.pVelocityTexture = getNativeResource(pMotion);
    ep.pOutputTexture = getNativeResource(pOut);
    ep.exposureScale = 1.0f;

    auto xr = xessD3D12Execute(mContext, pCommandList, &ep);
    if(xr != XESS_RESULT_SUCCESS)
    {
        logWarning("Failed xessD3D12Execute");
    }

    mReset = false;
    pRenderContext->setPendingCommands(true);
    pRenderContext->uavBarrier(pOut.get());
    pRenderContext->flush();
}

void IntelXeSS::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;

    if (widget.button("Reset"))
    {
        mReset = true;
    }
}
