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
#include "PerlinNoise.h"
#include "RenderGraph/RenderPassHelpers.h"
#include <random>

namespace
{
    const std::string kShaderName = "RenderPasses/PerlinNoise/PerlinNoise.cs.slang";

    const std::string kShaderModel = "6_5";
    const std::string kOutputTex = "outPerlinTex";

    const ChannelList kOutputChannels{
        {kOutputTex, "gPerlin", "Perlin Noise Texture", false, ResourceFormat::RGBA32Float},
    };

    const Gui::DropdownList kSmoothModeList{
        {(uint)PerlinNoise::SmoothStep::None, "None"},
        {(uint)PerlinNoise::SmoothStep::Smooth, "Smooth"},
        {(uint)PerlinNoise::SmoothStep::Smoother, "Smoother"},
    };
} // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PerlinNoise>();
}

PerlinNoise::PerlinNoise(ref<Device> pDevice, const Dictionary& dict)
    : RenderPass(pDevice)
{
}

Dictionary PerlinNoise::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PerlinNoise::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addOutput("dst");
    //reflector.addInput("src");
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void PerlinNoise::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpSampleGenerator)
        mpSampleGenerator = SampleGenerator::create(mpDevice, mpSampleGenerator);

    //Create a grid with random vectors
    if (!mpGridTex || mRebuildGrid)
    {
        mGridSize = mGridSizeUI;
        createGrid(pRenderContext);
        mRebuildGrid = false;
    }

    FALCOR_PROFILE(pRenderContext, "PerlineNoise");

    if (!mpPerlinNoisePass)
    {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderName).csEntry("main").setShaderModel(kShaderModel);

        Program::DefineList defines;
        defines.add(mpSampleGenerator->getDefines());
       
        mpPerlinNoisePass = ComputePass::create(mpDevice, desc, defines, true);
    }

    FALCOR_ASSERT(mpPerlinNoisePass);

    auto var = mpPerlinNoisePass->getRootVar();
    mpSampleGenerator->setShaderData(var); // Sample generator

    var["uni"]["gGridSize"] = mGridSize;
    var["uni"]["gDim"] = renderData.getDefaultTextureDims();
    var["uni"]["gSmoothStep"] = (uint)mSmoothStep;
    var["uni"]["gBlackWhite"] = mBlackWhite;
    var["uni"]["gUseImproved"] = mSwitchVersion;
    var["uni"]["gUsePermutation"] = mUsePermutationArray;
    var["uni"]["gSeed"] = mSeed;

    var["gGrid"] = mpGridTex;
    var["gPerlin"] = renderData[kOutputTex]->asTexture();

     // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpPerlinNoisePass->execute(pRenderContext, uint3(targetDim, 1));
}

void PerlinNoise::renderUI(Gui::Widgets& widget)
{
    widget.var("Grid Size", mGridSizeUI, 1u, 4096u, 1u);
    mRebuildGrid |= widget.button("Apply Size Change");

    widget.dropdown("SmoothMode", kSmoothModeList, (uint&)mSmoothStep);
    widget.tooltip("Smooth = 3t^2-2t^3 ; Smoother = 6t^5 - 15t^4 + 10t^3");

    widget.checkbox("Map to [0,1]", mBlackWhite);
    widget.tooltip("If disabled it is from [-1,1]. Negative -> green, Positive -> red");

    widget.checkbox("Switch to improved", mSwitchVersion);

    widget.checkbox("Use Permutation array", mUsePermutationArray);
    if (!mUsePermutationArray)
        widget.var("RNG seed", mSeed, 0u, UINT_MAX, 1u);
}

void PerlinNoise::createGrid(RenderContext* pRenderContext) {
    uint size = (mGridSize.x + 1) * (mGridSize.y + 1);
    std::vector<float2> randomGradient;
    randomGradient.resize(size);

    //Init random number gen
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_real_distribution<float> dist(-1.0f, std::nextafter(1.0f, FLT_MAX));

    //Create N random vectors
    for (uint i = 0; i < size; i++)
    {
        float2 data = normalize(float2(dist(mt), dist(mt)));
        randomGradient[i] = data;
    }

    mpGridTex = Texture::create2D(mpDevice, mGridSize.x + 1, mGridSize.y + 1, ResourceFormat::RG32Float, 1u, 1u, randomGradient.data());
    mpGridTex->setName("PerlinNoise::GridTex");
}
