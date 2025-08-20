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
#include "VBufferLighting.h"
#include "Scene/Lighting/LightSettings.h"
#include "Scene/Lighting/ShadowSettings.h"

namespace
{
    const char kShaderFile[] = "RenderPasses/VBufferLighting/VBufferLighting.ps.slang";

    const std::string kVBuffer = "vbuffer";
    const std::string kMotion = "mvec";
    const std::string kColor = "color";
    const std::string kTransparency = "transparency";
    const std::string kVisBuffer = "visibilityBuffer";
    const std::string kRayDir = "rayDir";
    const std::string kAmbient = "ambient";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, VBufferLighting>();
}

ref<VBufferLighting> VBufferLighting::create(ref<Device> pDevice, const Properties& props)
{
    return make_ref<VBufferLighting>(pDevice, props);
}

VBufferLighting::VBufferLighting(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    LightSettings::get().loadFromProperties(props);

    mpFbo = Fbo::create(mpDevice);
}

Properties VBufferLighting::getProperties() const
{
    return LightSettings::get().getProperties();
}

RenderPassReflection VBufferLighting::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addInput(kVBuffer, "vbuffer");
    reflector.addInput(kRayDir, "in view direction").flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kVisBuffer, "Visibility buffer used for shadowing. Range is [0,1] where 0 means the pixel is fully-shadowed and 1 means the pixel is not shadowed at all").flags(RenderPassReflection::Field::Flags::Optional).texture2D(0, 0, 1, 1, 0);
    reflector.addInput(kAmbient, "Ambient Occlusion (for opaque background)").bindFlags(ResourceBindFlags::ShaderResource).flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput(kTransparency, "Transparency RGB + Visibility").flags(RenderPassReflection::Field::Flags::Optional).bindFlags(ResourceBindFlags::ShaderResource);
    reflector.addOutput(kColor, "Color texture").format(ResourceFormat::RGBA32Float);
    reflector.addOutput(kMotion, "Motion vector").format(ResourceFormat::RG32Float);

    return reflector;
}

void VBufferLighting::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pColor = renderData[kColor]->asTexture();
    auto pVBuffer = renderData.getTexture(kVBuffer);
    auto pVisBuffer = renderData.getTexture(kVisBuffer);
    auto pRayDir = renderData.getTexture(kRayDir);
    auto pTransparency = renderData.getTexture(kTransparency);
    auto pAmbient = renderData.getTexture(kAmbient);
    auto pMvec = renderData[kMotion]->asTexture();

    mpFbo->attachColorTarget(pColor, 0);
    mpFbo->attachColorTarget(pMvec, 1);
    auto vars = mpPass->getRootVar();
    vars["vbuffer"] = pVBuffer;
    vars["rayDir"] = pRayDir;
    vars["visibilityBuffer"] = pVisBuffer;
    vars["ambient"] = pAmbient;
    vars["transparency"] = pTransparency;
    LightSettings::get().updateShaderVar(vars);

    mUseRayShadow = pVisBuffer == nullptr;
    ShadowSettings::get().updateShaderVar(mpDevice, vars, mFrameCount++);
    auto pProgram = mpPass->getProgram();
    pProgram->addDefines(ShadowSettings::get().getShaderDefines(*mpScene, renderData.getDefaultTextureDims()));
    pProgram->addDefine("USE_RAY_SHADOW", mUseRayShadow ? "1" : "0");
    pProgram->addDefine("USE_RAY_DIR", pRayDir ? "1" : "0");
    pProgram->addDefine("USE_AMBIENT", pAmbient ? "1" : "0");
    pProgram->addDefine("USE_TRANSPARENCY", pTransparency ? "1" : "0");

    mpScene->setRaytracingShaderData(pRenderContext, vars);

    mpPass->execute(pRenderContext, mpFbo);
}

void VBufferLighting::renderUI(Gui::Widgets& widget)
{
    LightSettings::get().renderUI(widget);
    if(mUseRayShadow)
    {
        ShadowSettings::get().renderUI(widget);
    }
}

void VBufferLighting::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;

    if (mpScene)
    {
        // create program
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile).psEntry("main");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel("6_5");
        mpPass = FullScreenPass::create(mpDevice, desc, mpScene->getSceneDefines());
    }
}
