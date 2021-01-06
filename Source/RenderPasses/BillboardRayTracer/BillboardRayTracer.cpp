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
#include "BillboardRayTracer.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "Experimental/Scene/Material/TexLODTypes.slang"
#include "Scene/HitInfo.h"

 // Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary & lib)
{
    lib.registerClass("BillboardRayTracer", "Ray Tracer for Billboard Rendering", BillboardRayTracer::create);
}

namespace
{
    const char kShaderFile[] = "RenderPasses/BillboardRayTracer/BillboardRayTracer.rt.slang";

    // Ray tracing settings that affect the traversal stack size.
    // These should be set as small as possible.
    const uint32_t kMaxPayloadSizeBytes = std::max<uint32_t>(/* default RayPayload */HitInfo::kMaxPackedSizeInBytes + 4, /* VolumePayload */18 * 5);
    const uint32_t kMaxAttributesSizeBytes = 8u;
    const uint32_t kMaxRecursionDepth = 1u;

    const ChannelList kInputChannels =
    {
        // TODO add shadow map here
    };

    const ChannelList kOutputChannels =
    {
        { "color",          "gOutputColor",               "Output color (sum of direct and indirect)"                },
    };

    const Gui::DropdownList kRayFootprintModeList =
    {
        { (uint32_t)TexLODMode::Mip0, "Disabled" },
        //{ (uint32_t)TexLODMode::RayCones, "Ray Cones" },
        //{ (uint32_t)TexLODMode::RayDiffsIsotropic, "Ray diffs (isotropic)" },
        { (uint32_t)TexLODMode::RayDiffsAnisotropic, "Ray diffs (anisotropic)" },
    };

    const Gui::DropdownList kBillboardTypeList =
    {
        { (uint32_t)BillboardRayTracer::BillboardType::Impostor, "Impostor" },
        { (uint32_t)BillboardRayTracer::BillboardType::Particle, "Particle" },
        { (uint32_t)BillboardRayTracer::BillboardType::Spherical, "Spherical" },
    };
};

BillboardRayTracer::SharedPtr BillboardRayTracer::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    return SharedPtr(new BillboardRayTracer(dict));
}

BillboardRayTracer::BillboardRayTracer(const Dictionary& dict)
    :
mFootprintMode((uint32_t)TexLODMode::RayDiffsAnisotropic)
{
    // Deserialize pass from dictionary.
    serializePass<true>(dict);

    // Create ray tracing program.
    RtProgram::Desc progDesc;
    progDesc.addShaderLibrary(kShaderFile).setRayGen("rayGen");
    progDesc.addIntersection(0, "boxIntersect");
    progDesc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    // hit group 0
    progDesc.addHitGroup(0, "triangleClosestHit", "triangleAnyHit").addMiss(0, "miss");
    progDesc.addAABBHitGroup(0, "boxClosestHit", "boxAnyHit");

    mTracer.pProgram = RtProgram::create(progDesc, kMaxPayloadSizeBytes, kMaxAttributesSizeBytes);

    // Create a sample generator.
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    assert(mpSampleGenerator);
}

Dictionary BillboardRayTracer::getScriptingDictionary()
{
    Dictionary dict;
    serializePass<false>(dict);
    return dict;
}

RenderPassReflection BillboardRayTracer::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    mOptionsChanged = true;
    return reflector;
}

void BillboardRayTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;

        // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
        Program::DefineList defines;
        defines.add(getValidResourceDefines(kInputChannels, renderData));
        defines.add(getValidResourceDefines(kOutputChannels, renderData));
        // footprints
        defines.add("RAY_FOOTPRINT_MODE", std::to_string(mFootprintMode));
        defines.add("RAY_CONE_MODE", "1");
        defines.add("RAY_FOOTPRINT_USE_MATERIAL_ROUGHNESS", "1");
        // billboard data
        defines.add("BILLBOARD_TYPE", std::to_string(mBillboardType));
        defines.add("BILLBOARD_TYPE_IMPOSTOR", std::to_string((uint)BillboardType::Impostor));
        defines.add("BILLBOARD_TYPE_PARTICLE", std::to_string((uint)BillboardType::Particle));
        defines.add("BILLBOARD_TYPE_SPHERICAL", std::to_string((uint)BillboardType::Spherical));
        defines.add("USE_REFLECTION_CORRECTION", mReflectionCorrection ? "true" : "false");
        defines.add("USE_REFRACTION_CORRECTION", mRefractionCorrection ? "true" : "false");
        defines.add("USE_SOFT_PARTICLES", mSoftParticles ? "true" : "false");
        defines.add("BILLBOARD_MATERIAL_ID", std::to_string(mLastMaterialId));
        defines.add("USE_SHADOWS", mShadows ? "true" : "false");
        defines.add("USE_RANDOM_BILLBOARD_COLORS", mRandomColors ? "true" : "false");
        defines.add("BILLBOARD_SHADOW_SAMPLES", std::to_string(mDeepBillboardSamples));

        mTracer.pProgram->addDefines(defines);

        mOptionsChanged = false;
    }

    // If we have no scene, just clear the outputs and return.
    if (!mpScene)
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData[it.name]->asTexture().get();
            if (pDst) pRenderContext->clearTexture(pDst);
        }
        return;
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mTracer.pVars) prepareVars();
    assert(mTracer.pVars);

    // Set constants.
    auto pVars = mTracer.pVars;
    pVars["CB"]["gFrameCount"] = mFrameCount;

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            auto pGlobalVars = mTracer.pVars;
            pGlobalVars[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    for (auto channel : kInputChannels) bind(channel);
    for (auto channel : kOutputChannels) bind(channel);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    assert(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));

    mFrameCount++;
}

void BillboardRayTracer::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    if(widget.dropdown("Ray footprint mode", kRayFootprintModeList, mFootprintMode))
        dirty = true;
    widget.tooltip("The ray footprint (texture LOD) mode to use.");

    if (widget.dropdown("Billboard type", kBillboardTypeList, mBillboardType))
        dirty = true;
    widget.tooltip("Impostor: y-aligned, Particle: origin-oriented, Spherical: Spherical shaped volume");

    if (widget.checkbox("Reflection correction", mReflectionCorrection)) dirty = true;
    widget.tooltip("Ray origin correction for impostors and particles");
    if (widget.checkbox("Refraction correction", mRefractionCorrection)) dirty = true;
    widget.tooltip("Ray origin correction for impostors and particles");

    if (mBillboardType == (uint)BillboardType::Particle &&
        widget.checkbox("Soft particles", mSoftParticles)) dirty = true;

    if (mBillboardType == (uint)BillboardType::Spherical)
    {
        //if(widget.checkbox("Deep billboard samples", mDeepBillboardSamples)) dirty = true;
        if (widget.var("Deep shadow samples", mDeepBillboardSamples, 1, 32)) dirty = true;
        widget.tooltip("Shadow samples will be taken from front- and backface of the billboard");
    }

    if (widget.checkbox("Shadows", mShadows)) dirty = true;
    if (widget.checkbox("Random Colors", mRandomColors)) dirty = true;
    widget.tooltip("Multiplies billboard colors with some random color");

    if (dirty)
    {
        mOptionsChanged = true;
    }
}

void BillboardRayTracer::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    // Clear data for previous scene.
    // After changing scene, the program vars should to be recreated.
    mTracer.pVars = nullptr;
    mFrameCount = 0;

    // Set new scene.
    mpScene = pScene;

    if (pScene)
    {
        mLastMaterialId = pScene->getMaterialCount() - 1;
        mTracer.pProgram->addDefines(pScene->getSceneDefines());

        // get last material name for hint about billboard type
        auto matName = pScene->getMaterial(mLastMaterialId)->getName();
        if(matName == "Impostor") mBillboardType = (uint)BillboardType::Impostor;
        else if(matName == "Particle") mBillboardType = (uint)BillboardType::Particle;
        else if(matName == "Spherical") mBillboardType = (uint)BillboardType::Spherical;
    }

    mOptionsChanged = true;
}

void BillboardRayTracer::prepareVars()
{
    assert(mpScene);
    assert(mTracer.pProgram);

    // Configure program.
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());

    // Create program variables for the current program/scene.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mTracer.pProgram, mpScene);

    // Bind utility classes into shared data.
    auto pGlobalVars = mTracer.pVars->getRootVar();
    bool success = mpSampleGenerator->setShaderData(pGlobalVars);
    if (!success) throw std::exception("Failed to bind sample generator");
}
