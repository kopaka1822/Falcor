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
#include "DitherVBuffer2.h"
#include "../DitherVBuffer/PermutationLookup.h"
#include "Scene/Lighting/ShadowSettings.h"

namespace
{
    const std::string kVbuffer = "vbuffer";
    const std::string kMotion = "mvec";
    const std::string kColorOut = "color";
    const std::string kDepthOut = "depth";

    const uint32_t kMaxPayloadSizeBytes = 6 * sizeof(float);
    const std::string kProgramRaytraceFile = "RenderPasses/DitherVBuffer2/DitherVBuffer2.rt.slang";

    const std::string kUseWhitelist = "useWhitelist";
    const std::string kWhitelist = "whitelist";
    const std::string kWhitelistBuffer = "whitelistBuffer"; // GPU Buffer for whitelist
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, DitherVBuffer2>();
}

DitherVBuffer2::DitherVBuffer2(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    mpSamplePattern = HaltonSamplePattern::create(16);

    //generatePermutations<3>();
    mpPermutations3x3Buffer = generatePermutations3x3(mpDevice);
    mpBlueNoise64Tex = Texture::createFromFile(mpDevice, "dither/bluenoise64.dds", false, false);
    mpSpatioTemporalBlueNoiseTex = Texture::createFromFile(mpDevice, "dither/spatiotemporal_bluenoise.dds", false, false);

    // load properties
    for (const auto& [key, value] : props)
    {
        if (key == kUseWhitelist) mUseTransparencyWhitelist = value;
        else if (key == kWhitelist)
        {
            std::stringstream ss;
            std::string svalue = value;
            ss << svalue;
            std::string entry;
            while (std::getline(ss, entry, ','))
            {
                mTransparencyWhitelist.insert(entry);
            }
        }
    }
}

Properties DitherVBuffer2::getProperties() const
{
    Properties props;
    props[kUseWhitelist] = mUseTransparencyWhitelist;
    // convert whitelist into a comma separated string
    std::stringstream ss;
    for (const auto& entry : mTransparencyWhitelist) ss << entry << ",";
    props[kWhitelist] = ss.str();
    return props;
}

RenderPassReflection DitherVBuffer2::reflect(const CompileData& compileData)
{
    uint2 dims = getRenderSize(compileData.defaultTexDims, mRenderScale);

    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addOutput(kVbuffer, "V-buffer").format(HitInfo::kDefaultFormat).texture2D(dims.x, dims.y);
    reflector.addOutput(kMotion, "Motion vector").format(ResourceFormat::RG32Float).flags(RenderPassReflection::Field::Flags::Optional).texture2D(dims.x, dims.y);
    reflector.addOutput(kColorOut, "Final color").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    reflector.addOutput(kDepthOut, "Depth").format(ResourceFormat::R32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    return reflector;
}

void DitherVBuffer2::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pVbuffer = renderData.getTexture(kVbuffer);
    auto pMotion = renderData.getTexture(kMotion);
    auto pColor = renderData.getTexture(kColorOut);
    auto pDepth = renderData.getTexture(kDepthOut);

    if (!mpScene)
    {
        pRenderContext->clearTexture(pColor.get(), float4(0, 0, 0, 0));
        return;
    }

    assert(mpProgram);
    assert(mpVars);

    uint2 frameDim = uint2(pVbuffer->getWidth(), pVbuffer->getHeight());
    mpScene->getCamera()->setPatternGenerator(mpSamplePattern, 1.0f / float2(frameDim));

    auto var = mpVars->getRootVar();
    var["gVBuffer"] = pVbuffer;
    var["gMotion"] = pMotion;
    var["gColor"] = pColor;
    var["gDepth"] = pDepth;
    assert(mpTransparencyWhitelist);
    var["gTransparencyWhitelist"] = mpTransparencyWhitelist;
    var["gPermutations3x3"] = mpPermutations3x3Buffer;
    var["gBlueNoise64x64Tex"] = mpBlueNoise64Tex;
    var["gSpatioTemporalBlueNoiseTex"] =mpSpatioTemporalBlueNoiseTex;

    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gDLSSCorrectionStrength"] = mDLSSCorrectionStrength;
    var["PerFrame"]["gAlignMotionVectors"] = 0;
    var["PerFrame"]["gPathLength"] = mPathLength;
    var["PerFrame"]["gAmbientIntensity"] = mAmbientIntensity;
    var["PerFrame"]["gAnalyticIntensity"] = mAnalyticIntensity;
    var["PerFrame"]["gEmissionIntensity"] = mEmissionIntensity;
    var["PerFrame"]["gEnvmapIntensity"] = mEnvmapIntensity;

    var["DitherConstants"]["gRotatePattern"] = 1;
    var["DitherConstants"]["gObjectHashType"] = uint(mObjectHashType);
    var["DitherConstants"]["gDitherTAAPermutations"] = 1;
    
    ShadowSettings::get().updateShaderVar(mpDevice, var, mFrameCount);

    mpProgram->addDefine("COVERAGE_CORRECTION", std::to_string(uint32_t(mCoverageCorrection)));
    mpProgram->addDefine("TRANSPARENCY_WHITELIST", mUseTransparencyWhitelist ? "1" : "0");
    mpProgram->addDefine("DITHER_MODE", std::to_string(uint32_t(mDitherMode)));
    mpProgram->addDefine("CULL_BACK_FACES", mCullBackFaces ? "1" : "0");
    mpProgram->addDefines(ShadowSettings::get().getShaderDefines(*mpScene, renderData.getDefaultTextureDims()));

    uint3 dispatch = uint3(1);
    dispatch.x = pVbuffer->getWidth();
    dispatch.y = pVbuffer->getHeight();
    mpScene->raytrace(pRenderContext, mpProgram.get(), mpVars, dispatch);

    // add whitelist to dict
    if (mUseTransparencyWhitelist)
    {
        renderData.getDictionary()[kWhitelist] = mTransparencyWhitelist;
        renderData.getDictionary()[kWhitelistBuffer] = mpTransparencyWhitelist;
    }
    mFrameCount++;
}

void DitherVBuffer2::renderUI(Gui::Widgets& widget)
{
    if (widget.dropdown("Render Scale", mRenderScale))
        requestRecompile();

    widget.var("Path Length", mPathLength, 1, 64);

    widget.dropdown("Dither", mDitherMode);

    widget.dropdown("Correction", mCoverageCorrection);
    if (mCoverageCorrection != CoverageCorrection::Disabled)
    {
        widget.slider("Correction Strength", mDLSSCorrectionStrength, 0.0f, 4.0f);
    }


    if (auto g = widget.group("Scene"))
    {
        widget.dropdown("Object Hash", mObjectHashType);

        widget.checkbox("Cull Back Faces", mCullBackFaces);

        widget.checkbox("Transparency Whitelist", mUseTransparencyWhitelist);
        widget.tooltip("Uses only whitelisted materials for dithering, when enabled. If not whitelisted, the material will use an alpha test.");
        if (mUseTransparencyWhitelist && mpScene)
        {
            auto g2 = widget.group("Whitelist");
            std::string removeEntry;
            // list all material names of the current scene
            for (uint mat = 0; mat < mpScene->getMaterialCount(); ++mat)
            {
                std::string name = mpScene->getMaterial(MaterialID(mat))->getName();
                bool isTransparent = mTransparencyWhitelist.find(name) != mTransparencyWhitelist.end();
                if (g2.checkbox(name.c_str(), isTransparent))
                {
                    if (isTransparent) mTransparencyWhitelist.insert(name);
                    else mTransparencyWhitelist.erase(name);
                    updateWhitelistBuffer();
                }
            }
        }
    }

    if (auto g = widget.group("Lighting"))
    {
        widget.var("Ambient", mAmbientIntensity, 0.0f);
        widget.var("Analytic", mAnalyticIntensity, 0.0f);
        widget.var("Emission", mEmissionIntensity, 0.0f);
        widget.var("Envmap", mEnvmapIntensity, 0.0f);
    }
    if (auto g = widget.group("Shadows"))
    {
        ShadowSettings::get().renderUI(g);
    }
}

void DitherVBuffer2::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    setupProgram();
    mUseTransparencyWhitelist = updateWhitelistBuffer();
}

void DitherVBuffer2::setupProgram()
{
    if (!mpScene) return;

    DefineList defines;
    defines.add(mpScene->getSceneDefines());
    defines.add(mpSampleGenerator->getDefines());

    RtProgram::Desc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kProgramRaytraceFile);
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(1);
    desc.setShaderModel("6_6");

    ref<RtBindingTable> sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
    sbt->setRayGen(desc.addRayGen("rayGen"));
    sbt->setMiss(0, desc.addMiss("miss"));
    sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));

    mpProgram = RtProgram::create(mpDevice, desc, defines);
    mpVars = RtProgramVars::create(mpDevice, mpProgram, sbt);

    // Bind static resources.
    ShaderVar var = mpVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
}

bool DitherVBuffer2::updateWhitelistBuffer()
{
    return updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
}
