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
#include "InterleaveTexture.h"


namespace
{
    const char kDesc[] = "Interleaves a 4x4 Texture2DArray back into a Texture2D";

    const std::string kTexIn = "texIn";
    const std::string kTexOut = "texOut";

    const std::string kProgram = "RenderPasses/InterleaveTexture/Interleave.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("InterleaveTexture", kDesc, InterleaveTexture::create);
}

InterleaveTexture::SharedPtr InterleaveTexture::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new InterleaveTexture);
    return pPass;
}

std::string InterleaveTexture::getDesc() { return kDesc; }

Dictionary InterleaveTexture::getScriptingDictionary()
{
    return Dictionary();
}

InterleaveTexture::InterleaveTexture()
{
    mpFbo = Fbo::create();
    mpPass = FullScreenPass::create(kProgram);
}

RenderPassReflection InterleaveTexture::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kTexIn, "Texture2DArray").texture2D(0, 0, 1, 1, 0);
    auto& outField = reflector.addOutput(kTexOut, "Texture2D").bindFlags(ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource).format(mLastFormat);
    mReady = false;

    auto edge = compileData.connectedResources.getField(kTexIn);
    if (edge)
    {
        const auto inputFormat = edge->getFormat();
        auto srcWidth = edge->getWidth();
        auto srcHeight = edge->getHeight();
        // TODO calculate output texture size based on input texture size

        mLastFormat = inputFormat;

        outField.format(inputFormat);
        mReady = true;
    }

    return reflector;
}

void InterleaveTexture::compile(RenderContext* pContext, const CompileData& compileData)
{
    if (!mReady) throw std::runtime_error("InterleaveTexture::compile - missing incoming reflection information");

    auto edge = compileData.connectedResources.getField(kTexIn);
    if (!edge) throw std::runtime_error("InterleaveTexture::compile - missing input information");

    auto inFormat = edge->getFormat();
    auto formatDesc = kFormatDesc[(uint32_t)inFormat];

    // set correct format type
    switch (formatDesc.channelCount)
    {
    case 1: mpPass->getProgram()->addDefine("type", "float"); break;
    case 2: mpPass->getProgram()->addDefine("type", "float2"); break;
    case 3: mpPass->getProgram()->addDefine("type", "float3"); break;
    case 4: mpPass->getProgram()->addDefine("type", "float4"); break;
    }
}

void InterleaveTexture::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pTexIn = renderData[kTexIn]->asTexture();
    auto pTexOut = renderData[kTexOut]->asTexture();

    mpFbo->attachColorTarget(pTexOut, 0);
    mpPass["src"] = pTexIn;
    mpPass->execute(pRenderContext, mpFbo);
}

void InterleaveTexture::renderUI(Gui::Widgets& widget)
{

}
