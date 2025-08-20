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
#include "DitherVBuffer.h"
#include "PermutationLookup.h"
#include "Scene/Lighting/LightSettings.h"
#include "Scene/Lighting/ShadowSettings.h"

namespace
{
    const std::string kVbuffer = "vbuffer";
    const std::string kMotion = "mvec";
    const std::string kOpacity = "opacity";
    const std::string kColorOut = "color";

    const uint32_t kMaxPayloadSizeBytes = 6 * sizeof(float); // could be optimized to 2*float for FULL_STOCHASTIC, but not measurable in performance
    const std::string kProgramRaytraceFile = "RenderPasses/DitherVBuffer/DitherVBuffer.rt.slang";

    const std::string kUseWhitelist = "useWhitelist";
    const std::string kWhitelist = "whitelist";
    const std::string kWhitelistBuffer = "whitelistBuffer"; // GPU Buffer for whitelist
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, DitherVBuffer>();
}

DitherVBuffer::DitherVBuffer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    mpSamplePattern = HaltonSamplePattern::create(16);
    createNoisePattern();
    setFractalDitherPattern(mFractalDitherPattern);
    Sampler::Desc sd;
    sd.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    sd.setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Clamp);
    mpFracSampler = Sampler::create(mpDevice, sd);
    sd.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    sd.setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpNoiseSampler = Sampler::create(mpDevice, sd);

    //generatePermutations<3>();
    mpPermutations3x3Buffer = generatePermutations3x3(mpDevice);
    mPermutations3x3Scores = getPermutationScores();
    mPermutations3x3Score = mPermutations3x3Scores.empty() ? 0 : mPermutations3x3Scores[0];
    mPermutations3x3Dropdown.clear();
    for (const auto& score : mPermutations3x3Scores)
    {
        mPermutations3x3Dropdown.push_back(Gui::DropdownValue{ (uint)score, std::to_string(score) });
    }

    mpBlueNoise3DTex = Texture::createFromFile(mpDevice, "dither/bluenoise3d_16.dds", false, false);
    mpBlueNoise64Tex = Texture::createFromFile(mpDevice, "dither/bluenoise64.dds", false, false);
    mpBayer64Tex = Texture::createFromFile(mpDevice, "dither/bayer64.dds", false, false);
    mpSpatioTemporalBlueNoiseTex = Texture::createFromFile(mpDevice, "dither/spatiotemporal_bluenoise.dds", false, false);
    mpSpatioTemporalBlueNoiseTex2 = Texture::createFromFile(mpDevice, "dither/spatiotemporal_bluenoise2.dds", false, false);

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

Properties DitherVBuffer::getProperties() const
{
    Properties props;
    props[kUseWhitelist] = mUseTransparencyWhitelist;
    // convert whitelist into a comma separated string
    std::stringstream ss;
    for(const auto& entry : mTransparencyWhitelist) ss << entry << ",";
    props[kWhitelist] = ss.str();
    return props;
}

RenderPassReflection DitherVBuffer::reflect(const CompileData& compileData)
{
    uint2 dims = getRenderSize(compileData.defaultTexDims, mRenderScale);

    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addOutput(kVbuffer, "V-buffer").format(HitInfo::kDefaultFormat).texture2D(dims.x, dims.y);
    reflector.addOutput(kMotion, "Motion vector").format(ResourceFormat::RG32Float).flags(RenderPassReflection::Field::Flags::Optional).texture2D(dims.x, dims.y);
    reflector.addOutput(kOpacity, "Opacity Mask (1 = any transparent)").format(ResourceFormat::R8Unorm).flags(RenderPassReflection::Field::Flags::Optional).texture2D(dims.x, dims.y);
    reflector.addOutput(kColorOut, "Final color").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::AllColorViews).texture2D(dims.x, dims.y);
    return reflector;
}

void DitherVBuffer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pVbuffer = renderData.getTexture(kVbuffer);
    auto pMotion = renderData.getTexture(kMotion);
    auto pOpacity = renderData.getTexture(kOpacity);
    auto pColor = renderData.getTexture(kColorOut);

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
    var["gOpacity"] = pOpacity;
    var["gColor"] = pColor;
    var["gDitherTex"] = mpFracDitherTex;
    var["gDitherRampTex"] = mpFracDitherRampTex;
    var["gDitherSampler"] = mpFracSampler;
    var["gNoiseTex"] = mpNoiseTex;
    var["gNoiseSampler"] = mpNoiseSampler;
    assert(mpTransparencyWhitelist);
    var["gTransparencyWhitelist"] = mpTransparencyWhitelist;
    var["gPermutations3x3"] = mpPermutations3x3Buffer;
    var["gBlueNoise3DTex"] = mpBlueNoise3DTex;
    var["gBlueNoise64x64Tex"] = mpBlueNoise64Tex;
    var["gBayerNoise64Tex"] = mpBayer64Tex;
    var["gSpatioTemporalBlueNoiseTex"] = mSTBNNoise == STBNNoise::Scalar ? mpSpatioTemporalBlueNoiseTex : mpSpatioTemporalBlueNoiseTex2;

    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gDLSSCorrectionStrength"] = mDLSSCorrectionStrength;
    var["PerFrame"]["gMinVisibility"] = mMinVisibility;
    var["PerFrame"]["gAlignMotionVectors"] = mAlignMotionVectors ? 1 : 0;

    var["DitherConstants"]["gGridScale"] = mGridScale;
    var["DitherConstants"]["gNoiseScale"] = float2(1.0f / mpNoiseTex->getWidth(), 1.0f / mpNoiseTex->getHeight());
    var["DitherConstants"]["gRotatePattern"] = mRotatePattern ? 1 : 0;
    var["DitherConstants"]["gObjectHashType"] = uint(mObjectHashType);
    var["DitherConstants"]["gNoiseTop"] = uint(mNoiseTopPattern);
    var["DitherConstants"]["gDitherTAAPermutations"] = mDitherTAAPermutations ? 1 : 0;

    LightSettings::get().updateShaderVar(var);
    ShadowSettings::get().updateShaderVar(mpDevice, var, mFrameCount);

    mpProgram->addDefine("COVERAGE_CORRECTION", std::to_string(uint32_t(mCoverageCorrection)));
    mpProgram->addDefine("TRANSPARENCY_WHITELIST", mUseTransparencyWhitelist ? "1" : "0");
    mpProgram->addDefine("DITHER_MODE", std::to_string(uint32_t(mDitherMode)));
    mpProgram->addDefine("ALPHA_TEXTURE_LOD", mUseAlphaTextureLOD ? "1" : "0");
    mpProgram->addDefine("CULL_BACK_FACES", mCullBackFaces ? "1" : "0");
    mpProgram->addDefines(ShadowSettings::get().getShaderDefines(*mpScene, renderData.getDefaultTextureDims()));

    if(mMinVisibility >= 1.0f)
    {
        mpProgram->addDefine("FULL_STOCHASTIC"); // ~5% faster than hybrid
        mpProgram->removeDefine("FULL_BASELINE");
    }
    else if(mMinVisibility <= 0.0f)
    {
        mpProgram->removeDefine("FULL_STOCHASTIC");
        mpProgram->addDefine("FULL_BASELINE");
    }
    else
    {
        mpProgram->removeDefine("FULL_STOCHASTIC");
        mpProgram->removeDefine("FULL_BASELINE");
    }

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

void DitherVBuffer::renderUI(Gui::Widgets& widget)
{
    if (widget.dropdown("Render Scale", mRenderScale))
        requestRecompile();

    widget.slider("Hybrid Threshold", mMinVisibility, 0.0f, 1.0f);

    widget.dropdown("Dither", mDitherMode);
    if(mDitherMode == DitherMode::FractalDithering)
    {
        if(widget.dropdown("Pattern", mFractalDitherPattern))
        {
            setFractalDitherPattern(mFractalDitherPattern);
        }
    }

    const bool is2DDither = mDitherMode == DitherMode::PerPixel2x2 ||
        mDitherMode == DitherMode::PerPixel3x3 ||
        mDitherMode == DitherMode::PerPixel4x4;
    const bool is3DDither = mDitherMode == DitherMode::PerPixel2x2x2;
    const bool isDitherTAA = mDitherMode == DitherMode::DitherTemporalAA;

    if(is2DDither || is3DDither || isDitherTAA)
    {
        widget.checkbox("Align Motion Vector", mAlignMotionVectors);
        widget.tooltip("Align motion vector to grid size to prevent issues when moving camera");
    }

    bool useTopNoiseGrid = false;
    if(is2DDither || is3DDither || isDitherTAA)
    {
        widget.dropdown("Noise on Top", mNoiseTopPattern);
        if (mNoiseTopPattern == NoiseTopPattern::SurfaceWhite)
            useTopNoiseGrid = true;
    }

    if(is2DDither || mDitherMode == DitherMode::HashGrid)
    {
        widget.checkbox("Serpentine Pattern", mRotatePattern);
        widget.tooltip("Rotates the per-pixel dither pattern based on the frame index");
    }

    if (mDitherMode == DitherMode::FractalDithering || mDitherMode == DitherMode::HashGrid || useTopNoiseGrid || mDitherMode == DitherMode::SurfaceSpatioTemporalBlueNoise)
    {
        widget.var("Grid Scale", mGridScale, 0.0001f, 16.0f, 0.01f);
    }
    if (mDitherMode == DitherMode::HashGrid || useTopNoiseGrid)
    {
        if (widget.dropdown("Noise Pattern", mNoisePattern))
        {
            createNoisePattern();
        }
    }

    if(mDitherMode == DitherMode::DitherTemporalAA)
    {
        widget.checkbox("Enable DTAA*", mDitherTAAPermutations);
        widget.tooltip("Uses a permutations of the [0,1,2,3,4] sequence for creating the 5x5 mask. This will prevent objects from masking each other.");
    }

    if (mDitherMode == DitherMode::SpatioTemporalBlueNoise || mDitherMode == DitherMode::SurfaceSpatioTemporalBlueNoise)
    {
        widget.dropdown("STBN Variant", mSTBNNoise);
        widget.tooltip("Scalar is based on Void and Cluster algorithm. Vector is based on Georgiev et al.");
    }

    if (auto g = widget.group("Permutations"))
    {
        if(g.dropdown("Score: ", mPermutations3x3Dropdown, mPermutations3x3Score))
        {
            mpPermutations3x3Buffer = generatePermutations3x3(mpDevice, mPermutations3x3Score, mPermutations3x3Score);
        }
    }

    widget.dropdown("Correction", mCoverageCorrection);
    if (mCoverageCorrection != CoverageCorrection::Disabled)
    {
        widget.slider("Correction Strength", mDLSSCorrectionStrength, 0.0f, 4.0f);
    }


    if(auto g = widget.group("Scene"))
    {
        widget.dropdown("Object Hash", mObjectHashType);

        widget.checkbox("Alpha Texture LOD", mUseAlphaTextureLOD);

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

    if(auto g = widget.group("Lighting"))
    {
        LightSettings::get().renderUI(g);
    }
    if(auto g = widget.group("Shadows"))
    {
        ShadowSettings::get().renderUI(g);
    }
}

void DitherVBuffer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    setupProgram();
    mUseTransparencyWhitelist = updateWhitelistBuffer();
}

void DitherVBuffer::setFractalDitherPattern(DitherPattern pattern)
{
    mFractalDitherPattern = pattern;
    std::string texname;
    switch(pattern)
    {
    case DitherPattern::Dither2x2:
        texname = "dither/Dither3D_2x2";
        break;
    case DitherPattern::Dither4x4:
        texname = "dither/Dither3D_4x4";
        break;
    case DitherPattern::Dither8x8:
        texname = "dither/Dither3D_8x8";
        break;
    }
    mpFracDitherTex = Texture::createFromFile(mpDevice, texname + ".dds", false, false);
    mpFracDitherRampTex = Texture::createFromFile(mpDevice, texname + "_ramp.dds", false, false);
}

void DitherVBuffer::setupProgram()
{
    if (!mpScene) return;

    DefineList defines;
    defines.add(mpScene->getSceneDefines());
    defines.add(mpSampleGenerator->getDefines());
#ifdef ENABLE_HASH_GRIDS
    defines.add("ENABLE_HASH_GRIDS", "1");
#endif

    RtProgram::Desc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kProgramRaytraceFile);
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(1);

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

bool DitherVBuffer::updateWhitelistBuffer()
{
    return updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
}

void DitherVBuffer::createNoisePattern()
{
    std::string texname;
    switch (mNoisePattern)
    {
    case NoisePattern::White:
        texname = "dither/whitenoise1024.dds";
        break;
    case NoisePattern::Blue:
        texname = "dither/bluenoise1024.dds";
        break;
    case NoisePattern::Bayer:
        texname = "dither/bayer_matrix.dds";
        break;
    case NoisePattern::BlueBayer:
        texname = "dither/blue_bayer.dds";
        break;
    case NoisePattern::Poisson:
        texname = "dither/poisson1024.dds";
        break;
    case NoisePattern::Perlin:
        texname = "dither/perlin1024.dds";
        break;
    case NoisePattern::Blue64:
        texname = "dither/bluenoise64.dds";
        break;
    default:
        assert(false);
    }

    mpNoiseTex = Texture::createFromFile(mpDevice, texname, false, false);
}
