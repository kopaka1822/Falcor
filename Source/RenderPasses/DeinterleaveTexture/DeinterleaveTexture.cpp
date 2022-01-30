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
#include "DeinterleaveTexture.h"


namespace
{
    const char kDesc[] = "Converts a texture 2d into a 4x4 deinterleaved texture array";

    const std::string kTexIn = "texIn";
    const std::string kTexOut = "texOut";

    const std::string kProgram = "RenderPasses/DeinterleaveTexture/Deinterleave.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("DeinterleaveTexture", kDesc, DeinterleaveTexture::create);
}

DeinterleaveTexture::SharedPtr DeinterleaveTexture::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new DeinterleaveTexture);
    return pPass;
}

std::string DeinterleaveTexture::getDesc() { return kDesc; }

Dictionary DeinterleaveTexture::getScriptingDictionary()
{
    return Dictionary();
}

DeinterleaveTexture::DeinterleaveTexture()
    :
mMaxRenderTargetCount(Fbo::getMaxColorTargetCount())
{
    if (mMaxRenderTargetCount < 8)
        throw std::runtime_error("DeinterleaveTexture: At least 8 rendertargets need to be supported by the graphics card");

    mpFbo = Fbo::create();
    mpPass = FullScreenPass::create(kProgram);
}

RenderPassReflection DeinterleaveTexture::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kTexIn, "texture 2D").bindFlags(ResourceBindFlags::ShaderResource).texture2D(0, 0, 1, 1, 1);
    auto& outField = reflector.addOutput(kTexOut, "texture 2D array").bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource).texture2D(0, 0, 1, 1, mSize).format(mLastFormat);
    mReady = false;

    auto edge = compileData.connectedResources.getField(kTexIn);
    if(edge)
    {
        const auto inputFormat = edge->getFormat();
        auto srcWidth = edge->getWidth();
        if (srcWidth == 0) srcWidth = compileData.defaultTexDims.x;
        auto srcHeight = edge->getHeight();
        if (srcHeight == 0) srcHeight = compileData.defaultTexDims.y;
        mLastFormat = inputFormat;

        // reduce the size of the target texture
        auto dstWidth = (srcWidth + mWidth - 1) / mWidth;
        auto dstHeight = (srcHeight + mHeight - 1) / mHeight;

        outField.format(inputFormat).texture2D(dstWidth, dstHeight, 1, 1, mSize);
        mReady = true;
    }

    return reflector;
}

void DeinterleaveTexture::compile(RenderContext* pContext, const CompileData& compileData)
{
    if (!mReady) throw std::runtime_error("DeinterleaveTexture::compile - missing incoming reflection information");

    auto edge = compileData.connectedResources.getField(kTexIn);
    if (!edge) throw std::runtime_error("DeinterleaveTexture::compile - missing input information");

    auto inFormat = edge->getFormat();
    auto formatDesc = kFormatDesc[(uint32_t)inFormat];

    // set correct format type
    switch(formatDesc.channelCount)
    {
    case 1: mpPass->getProgram()->addDefine("type", "float"); break;
    case 2: mpPass->getProgram()->addDefine("type", "float2"); break;
    case 3: mpPass->getProgram()->addDefine("type", "float3"); break;
    case 4: mpPass->getProgram()->addDefine("type", "float4"); break;
    }

}

void DeinterleaveTexture::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pTexIn = renderData[kTexIn]->asTexture();
    auto pTexOut = renderData[kTexOut]->asTexture();

    mpPass["src"] = pTexIn;
    for(uint32_t slice = 0; slice < mSize; slice += 8)
    {
        for(uint slot = 0; slot < 8; ++slot)
        {
            // attach single slice of texture
            mpFbo->attachColorTarget(pTexOut, slot, 0, slice + slot, 1);
        }

        mpPass["PassData"]["offset"] = slice;
        mpPass->execute(pRenderContext, mpFbo);
    }
}

void DeinterleaveTexture::renderUI(Gui::Widgets& widget)
{
    widget.text(std::to_string(mMaxRenderTargetCount));
}
