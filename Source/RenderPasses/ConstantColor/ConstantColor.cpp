/***************************************************************************
 # Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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
#include "ConstantColor.h"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

namespace
{
    const char kColor[] = "color";
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("ConstantColor", "Render Pass Template", ConstantColor::create);
}

ConstantColor::SharedPtr ConstantColor::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new ConstantColor);

    // parse dictionary
    for (const auto& [key, value] : dict)
    {
        if (key == kColor) pPass->setColor(value);
        else logWarning("Unknown field '" + key + "' in a ConstantColor dictionary");
    }
    return pPass;
}

Dictionary ConstantColor::getScriptingDictionary()
{
    Dictionary d;
    d[kColor] = mColor;
    return d;
}

RenderPassReflection ConstantColor::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    // TODO make persistent?
    reflector.addOutput("texture", "output texture")
        .bindFlags(Resource::BindFlags::ShaderResource | Resource::BindFlags::RenderTarget)
        .format(ResourceFormat::RGBA32Float)
        .texture2D(0, 0);
    return reflector;
}

void ConstantColor::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    const auto& pTexture = renderData["texture"]->asTexture();
    if(!pTexture)
    {
        logWarning("ConstantColor::execute() - unused output texture");
        return;
    }

    // TODO custom clear color
    pRenderContext->clearTexture(pTexture.get(), mColor);
}

void ConstantColor::renderUI(Gui::Widgets& widget)
{
    float4 color = mColor;
    if (widget.rgbaColor("Color", color)) setColor(color);

}

void ConstantColor::setColor(float4 color)
{
    mColor = color;
    mPassChangedCB();
}
