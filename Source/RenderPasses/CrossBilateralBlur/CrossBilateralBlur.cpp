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
#include "CrossBilateralBlur.h"


namespace
{
    const char kDesc[] = "Depth-Aware Cross Bilateral Blur";
    const char kShaderPath[] = "RenderPasses/CrossBilateralBlur/Blur.ps.slang";

    const std::string kColor = "color";
    const std::string kDepth = "linear depth";
    const std::string kPingPong = "pingpong";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("CrossBilateralBlur", kDesc, CrossBilateralBlur::create);
}

CrossBilateralBlur::SharedPtr CrossBilateralBlur::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new CrossBilateralBlur);
    return pPass;
}

std::string CrossBilateralBlur::getDesc() { return kDesc; }

Dictionary CrossBilateralBlur::getScriptingDictionary()
{
    return Dictionary();
}

void CrossBilateralBlur::setKernelRadius(uint32_t radius)
{
    mKernelRadius = radius;
    mPassChangedCB();
}

CrossBilateralBlur::CrossBilateralBlur()
{
    mpFbo = Fbo::create();
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpSampler = Sampler::create(samplerDesc);
}

RenderPassReflection CrossBilateralBlur::reflect(const CompileData& compileData)
{
    mReady = false;

    // Define the required resources here
    RenderPassReflection reflector;

    auto& colorField = reflector.addInputOutput(kColor, "color image to be blurred").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget);
    auto& depthField = reflector.addInput(kDepth, "linear depth").bindFlags(ResourceBindFlags::ShaderResource);
    auto& pingpongField = reflector.addInternal(kPingPong, "temporal result after first blur").bindFlags(ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget);

    // set correct input format and dimensions of the ping pong buffer
    auto edge = compileData.connectedResources.getField(kColor);
    if(edge)
    {
        const auto inputFormat = edge->getFormat();
        const auto srcWidth = edge->getWidth();
        const auto srcHeight = edge->getHeight();

        colorField.format(inputFormat).texture2D(srcWidth, srcHeight, 1, 1, 1);
        pingpongField.format(inputFormat).texture2D(srcWidth, srcHeight, 1, 1, 1);

        mReady = true;
    }
    
    return reflector;
}

void CrossBilateralBlur::compile(RenderContext* pContext, const CompileData& compileData)
{
    if(!mReady) throw std::runtime_error("CrossBilateralBlur::compile - missing incoming reflection information");

    Program::DefineList defines;
    defines.add("KERNEL_RADIUS", std::to_string(mKernelRadius));

    defines.add("DIR", "int2(1, 0)");
    mpBlurX = FullScreenPass::create(kShaderPath, defines);

    defines.add("DIR", "int2(0, 1)");
    mpBlurY = FullScreenPass::create(kShaderPath, defines);

    // share program vars
    mpBlurY->setVars(mpBlurX->getVars());

    mpBlurX["gSampler"] = mpSampler;
}

void CrossBilateralBlur::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    assert(mReady);

    auto pColor = renderData[kColor]->asTexture();
    auto pPingPong = renderData[kPingPong]->asTexture();
    auto pDepth = renderData[kDepth]->asTexture();

    assert(pColor->getFormat() == pPingPong->getFormat());

    // blur in x
    mpFbo->attachColorTarget(pPingPong, 0);
    mpBlurX["gSrcTex"] = pColor;
    mpBlurX["gDepthTex"] = pDepth;
    mpBlurX->execute(pRenderContext, mpFbo);

    // blur in y
    mpFbo->attachColorTarget(pColor, 0);
    mpBlurY["gSrcTex"] = pPingPong;
    mpBlurY->execute(pRenderContext, mpFbo);
}

void CrossBilateralBlur::renderUI(Gui::Widgets& widget)
{
    if (widget.var("Kernel Radius", mKernelRadius, uint32_t(1), uint32_t(20))) setKernelRadius(mKernelRadius);

}
