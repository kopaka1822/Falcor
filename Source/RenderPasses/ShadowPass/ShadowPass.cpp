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
#include "ShadowPass.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

namespace
{
    const char kShaderFile[] = "RenderPasses/ShadowPass/ShadowPass.rt.slang";

    // Ray tracing settings that affect the traversal stack size.
    // These should be set as small as possible.
    const uint32_t kMaxPayloadSizeBytes = 4u;
    const uint32_t kMaxRecursionDepth = 1u;

    const ChannelList kInputChannels = {
        {"posW", "gPosW", "World Position"},
        {"normalW", "gNormalW", "World Normal"},
        {"faceNormalW", "gFaceNormalW", "Face Normal"},
    };

    const ChannelList kOptionalInputsShading = {
        {"tangentW", "gTangentW", "Tangent", true},
        {"texCoord", "gTexCoord", "Texture Coordinate", true},
        {"texGrads", "gTexGrads", "Texture Gradients (LOD)", true},
        {"MaterialInfo", "gMaterialInfo", "Material", true},
    };

    const ChannelList kOptionalInputsSimplifiedShading = {
        {"diffuse", "gDiffuse", "Diffuse Reflection", true},
        {"specularRoughness", "gSpecRough", "Specular Reflection (xyz) and Roughness (w)", true},
    };

    const ChannelList kOutputChannels = {
        {"color", "gColor", "(Shadowed) Color for the direct light", false, ResourceFormat::RGBA16Float},
    };

} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ShadowPass>();
}

ShadowPass::ShadowPass(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

Properties ShadowPass::getProperties() const
{
    return {};
}

RenderPassReflection ShadowPass::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassInputs(reflector, kOptionalInputsShading);
    addRenderPassInputs(reflector, kOptionalInputsSimplifiedShading);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void ShadowPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    //Clear Outputs Lamda
    auto clearOutputs = [&]()
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
    };

    // If we have no scene or the scene has no active analytic lights, just clear the outputs and return.
    if (!mpScene)
    {
        clearOutputs();
        return;
    }
    if (mpScene->getActiveLightCount() == 0)
    {
        clearOutputs();
        return;
    }

    // Calculate and update the shadow map
    if (!mpShadowMap->update(pRenderContext))
        return;

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // I/O Resources could change at any time
    bool ioChanged = false;
    ioChanged |= mShadowTracer.pProgram->addDefines(getValidResourceDefines(kOptionalInputsShading, renderData));
    ioChanged |= mShadowTracer.pProgram->addDefines(getValidResourceDefines(kOptionalInputsSimplifiedShading, renderData));

    // Check which shading model should be used
    if (ioChanged)
    {
        auto checkResources = [&](const ChannelList& list)
        {
            bool valid = true;
            for (const auto& desc : list)
                valid &= renderData[desc.name] != nullptr;
            return valid;
        };
        mSimplifiedShadingValid = checkResources(kOptionalInputsSimplifiedShading);
        mComplexShadingValid = checkResources(kOptionalInputsShading);

        if (!mComplexShadingValid && !mSimplifiedShadingValid)
            throw RuntimeError(
                "ShadowPass : Not enough input texture for shading. Either all Simplified or Complex Shading inputs need to be set!"
            );

        if (mSimplifiedShadingValid && !mComplexShadingValid)
            mUseSimplifiedShading = true;
        else if (mComplexShadingValid)
            mUseSimplifiedShading = false;
    }

    //Add defines
    mShadowTracer.pProgram->addDefine("SP_SHADOW_MODE", std::to_string(uint32_t(mShadowMode)));
    mShadowTracer.pProgram->addDefine("SIMPLIFIED_SHADING", mUseSimplifiedShading ? "1" : "0");
    mShadowTracer.pProgram->addDefine("ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    mShadowTracer.pProgram->addDefine("SP_NORMAL_TEST", mNormalTest ? "1" : "0");
    mShadowTracer.pProgram->addDefines(mpShadowMap->getDefines());

    //Prepare Vars
    if (!mShadowTracer.pVars)
    {
        mShadowTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());
        mShadowTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
        mShadowTracer.pVars = RtProgramVars::create(mpDevice, mShadowTracer.pProgram, mShadowTracer.pBindingTable);
    }

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    //Bind Resources
    auto var = mShadowTracer.pVars->getRootVar();

    // Set Shadow Map per Iteration Shader Data
    mpShadowMap->setShaderDataAndBindBlock(var, renderData.getDefaultTextureDims());
    mpSampleGenerator->setShaderData(var);

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };

    for (auto channel : kInputChannels)
        bind(channel);
    for (auto channel : kOptionalInputsShading)
        bind(channel);
    for (auto channel : kOptionalInputsSimplifiedShading)
        bind(channel);
    for (auto channel : kOutputChannels)
        bind(channel);

    //Execute shader
    mpScene->raytrace(pRenderContext, mShadowTracer.pProgram.get(), mShadowTracer.pVars, uint3(targetDim, 1));
}

void ShadowPass::renderUI(Gui::Widgets& widget)
{
    bool changed = false;
    changed |= widget.dropdown("Shadow Mode", mShadowMode);
    if (mShadowMode != SPShadowMode::ShadowMap)
        changed |= widget.checkbox("Ray Alpha Test", mUseAlphaTest);

    //Shading Model
    if (mSimplifiedShadingValid && mComplexShadingValid)
    {
        changed |= widget.checkbox("Use Simplified Shading", mUseSimplifiedShading);
        widget.tooltip(
            "Change the used shading model. For better overall performance please only bind the input textures for one shading model"
        );
    }
    else
    {
        if (mComplexShadingValid)
            widget.text("Complex Shading Model in use");
        else if (mSimplifiedShadingValid)
            widget.text("Simplified Shading Model in use");
        else
        {
            widget.text("Not enough resources bound for either shading model! (Not updated in RenderGraphEditor)");
            widget.text("Simplified Model: diffuse , specularRoughness");
            widget.text("Complex Model: tangentW, texCoords, texGrads, MaterialInfo");
        }
    }
        
    changed |= widget.checkbox("Light Normal Test", mNormalTest);
    widget.tooltip("Test if the surface is shaded using the normal. Requires a texture access to the normal texture");

    if (mShadowMode != SPShadowMode::RayTraced && mpShadowMap)
    {
        if (auto group = widget.group("Shadow Map Options", true))
            changed |= mpShadowMap->renderUI(group);
    }

    mOptionsChanged |= changed;
}

void ShadowPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    //Clear data from previous scene
    mShadowTracer.pBindingTable = nullptr;
    mShadowTracer.pProgram = nullptr;
    mShadowTracer.pVars = nullptr;

    //Set new scene
    mpScene = pScene;

    //Reset Shading
    mComplexShadingValid = false;
    mSimplifiedShadingValid = false;

    //Create Ray Tracing pass
    if (mpScene)
    {
        // Init the shadow map
        mpShadowMap = std::make_unique<ShadowMap>(mpDevice, mpScene);

        // Create ray tracing program.
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mShadowTracer.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mShadowTracer.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(
                0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
                desc.addHitGroup("", "anyHit")
            );
        }

        mShadowTracer.pProgram = RtProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    }

}
