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
#include "HBAOPlusNonInterleaved.h"


namespace
{
    const char kDesc[] = "HBAO Plus von NVIDIA without deinterleaved texture acesses";

    const std::string kDepth = "depth";
    const std::string kDepth2 = "depth2";
    const std::string kNormal = "normals";
    const std::string kAmbientMap = "ambientMap";

    const std::string kProgram = "RenderPasses/HBAOPlusNonInterleaved/HBAOPlus.slang";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("HBAOPlusNonInterleaved", kDesc, HBAOPlusNonInterleaved::create);
}

HBAOPlusNonInterleaved::SharedPtr HBAOPlusNonInterleaved::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new HBAOPlusNonInterleaved);
    return pPass;
}

std::string HBAOPlusNonInterleaved::getDesc() { return kDesc; }

Dictionary HBAOPlusNonInterleaved::getScriptingDictionary()
{
    return Dictionary();
}

void HBAOPlusNonInterleaved::setRadius(float r)
{
    mData.radius = r;
    mData.negInvRsq = -1.0f / (r * r);
}

void HBAOPlusNonInterleaved::setDualLayer(bool dual)
{
    mDualLayer = dual;
    if(dual) mpPass->getProgram()->addDefine("DEPTH_MODE", "DEPTH_MODE_DUAL");
    else mpPass->getProgram()->addDefine("DEPTH_MODE", "DEPTH_MODE_SINGLE");
}

HBAOPlusNonInterleaved::HBAOPlusNonInterleaved()
{
    mpFbo = Fbo::create();
    mpPass = FullScreenPass::create(kProgram);
    // create sampler
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(samplerDesc);

    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point).setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpTextureSampler = Sampler::create(samplerDesc);

    setDualLayer(mDualLayer);
    setRadius(mData.radius);
}

RenderPassReflection HBAOPlusNonInterleaved::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "linear-depth").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kDepth2, "linear-depth2").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addInput(kNormal, "normals").bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kAmbientMap, "ambient occlusion").bindFlags(ResourceBindFlags::AllColorViews).format(ResourceFormat::R8Unorm);
    return reflector;
}

void HBAOPlusNonInterleaved::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pDepth = renderData[kDepth]->asTexture();
    auto pDepth2 = renderData[kDepth2]->asTexture();
    auto pNormal = renderData[kNormal]->asTexture();
    auto pAmbient = renderData[kAmbientMap]->asTexture();

    if (!mEnabled)
    {
        // clear and return
        pRenderContext->clearTexture(pAmbient.get(), float4(1.0f));
        return;
    }

    auto pCamera = mpScene->getCamera().get();

    mpFbo->attachColorTarget(pAmbient, 0);
    mData.resolution = float2(pDepth->getWidth(), pDepth->getHeight());
    mData.invResolution = float2(1.0f) / mData.resolution;
    mpPass["StaticCB"].setBlob(mData);
    pCamera->setShaderData(mpPass["PerFrameCB"]["gCamera"]);
    mpPass["gNoiseSampler"] = mpNoiseSampler;
    mpPass["gTextureSampler"] = mpTextureSampler;
    mpPass["gDepthTex"] = pDepth;
    mpPass["gDepthTex2"] = pDepth2;
    mpPass["gNormalTex"] = pNormal;

    mpPass->execute(pRenderContext, mpFbo);
}   

void HBAOPlusNonInterleaved::renderUI(Gui::Widgets& widget)
{
    float radius = mData.radius;
    if (widget.var("Radius", radius, 0.01f, FLT_MAX, 0.01f))
        setRadius(radius);

    widget.slider("Depth Bias", mData.NdotVBias, 0.0f, 0.5f);
    widget.slider("Power Exponent", mData.powerExponent, 1.0f, 4.0f);
    if (widget.checkbox("Dual Depth", mDualLayer)) setDualLayer(mDualLayer);
}
