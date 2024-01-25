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
#include "SMGaussianBlur.h"
#include "Scene/Camera/Camera.h"
#include "Utils/Math/FalcorMath.h"

namespace Falcor
{
namespace
{
const std::string kShaderFile = "Rendering/ShadowMaps/SMGaussianBlur.cs.slang";
const std::string kShaderModel = "6_5";

} // namespace

void SMGaussianBlur::execute(RenderContext* pRenderContext, ref<Texture>& pTexture, uint texArrayIndex)
{
    FALCOR_PROFILE(pRenderContext, "SM_GausBlur");

    //Check source texture and create work copy
    prepareBlurTexture(pTexture);

    //Update the Kernel if settings changed
    if (mKernelChanged)
    {
        updateKernel();
        mKernelChanged = false;
    }

    if (mIsCube)
    {
        for (uint i = 0; i < 6; i++)
            blur(pRenderContext, pTexture, i);  //TODO check if this works
    }
    else
    {
        blur(pRenderContext, pTexture, texArrayIndex);
    }
    
}

void SMGaussianBlur::blur(RenderContext* pRenderContext, ref<Texture>& pTexture, uint texArrayIndex) {
    // Horizontal Blur
    {
        if (!mpHorizontalBlur)
        {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderFile).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            defines.add("_HORIZONTAL_BLUR");
            if (mIsCube)
                defines.add("_IS_CUBE");
            defines.add("_KERNEL_WIDTH", std::to_string(mKernelWidth));
            defines.add("_TEX_WIDTH", mDimMaxDefineString);

            mpHorizontalBlur = ComputePass::create(mpDevice, desc, defines, true);
        }
        FALCOR_ASSERT(mpHorizontalBlur);
        // If defines change, refresh the program
        mpHorizontalBlur->getProgram()->addDefine("_KERNEL_WIDTH", std::to_string(mKernelWidth));
        mpHorizontalBlur->getProgram()->addDefine("_TEX_WIDTH", mDimMaxDefineString);

        // Set variables
        auto var = mpHorizontalBlur->getRootVar();

        var["weights"] = mpWeightBuffer;
        if (mIsCube)
            var["gSrcTex"].setUav(pTexture->getUAV(0, texArrayIndex, 1)); // SRV Cube is bugged in slang/falcor so UAV is needed
        else
            var["gSrcTex"].setSrv(pTexture->getSRV(0, 1, texArrayIndex, 1));

        var["gDstTex"] = mpBlurWorkTexture;

        mpHorizontalBlur->execute(pRenderContext, uint3(mTextureDims, 1));
    }

    // Vertical Blur
    {
        if (!mpVerticalBlur)
        {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderFile).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            defines.add("_VERTICAL_BLUR");
            defines.add("_TEX_WIDTH", mDimMaxDefineString);
            defines.add("_KERNEL_WIDTH", std::to_string(mKernelWidth));

            mpVerticalBlur = ComputePass::create(mpDevice, desc, defines, true);
        }
        FALCOR_ASSERT(mpVerticalBlur);
        // If defines change, refresh the program
        mpVerticalBlur->getProgram()->addDefine("_KERNEL_WIDTH", std::to_string(mKernelWidth));
        mpVerticalBlur->getProgram()->addDefine("_TEX_WIDTH", mDimMaxDefineString);

        // Set variables
        auto var = mpVerticalBlur->getRootVar();

        var["weights"] = mpWeightBuffer;
        var["gSrcTex"] = mpBlurWorkTexture;
        var["gDstTex"].setUav(pTexture->getUAV(0, texArrayIndex, 1));

        mpVerticalBlur->execute(pRenderContext, uint3(mTextureDims, 1));
    }
}

bool SMGaussianBlur::renderUI(Gui::Widgets& widget) {
    bool changed = false;
    if (widget.var("Kernel Width", (int&)mKernelWidth, 1, 15, 2))
        changed = true;
    if (widget.slider("Sigma", mSigma, 0.001f, mKernelWidth / 2.f))
        changed = true;

    mKernelChanged |= changed;
    return changed;
}

void SMGaussianBlur::prepareBlurTexture(ref<Texture> pTexture)
{
    bool createTexture = !mpBlurWorkTexture;
    const uint2 srcTexDims = uint2(pTexture->getWidth(), pTexture->getHeight());
    ResourceFormat srcTexFormat = pTexture->getFormat();
    //const uint srcTexArraySize = pTexture->getArraySize();
    //Convert depth formats to float formats for comparison
    switch (srcTexFormat)
    {
    case ResourceFormat::D32Float:
        srcTexFormat = ResourceFormat::R32Float;
        break;
    case ResourceFormat::D16Unorm:
        srcTexFormat = ResourceFormat::R16Float;
        break;
    default:
        break;
    }

    createTexture |= (srcTexDims.x != mTextureDims.x) || (srcTexDims.y != mTextureDims.y);  //Check Dims
    createTexture |= srcTexFormat != mTextureFormat;                                        //Check Format
    //bool arraySizeChanged =  srcTexArraySize != mArraySize;                                 //Check Array Size
    //createTexture |= arraySizeChanged;
    if (createTexture)
    {
        mTextureDims = srcTexDims;
        mTextureFormat = srcTexFormat;
        mDimMaxDefineString = "int2(" + std::to_string(mTextureDims.x - 1) + ", " + std::to_string(mTextureDims.y - 1) + ")";
        mArraySize = 1;

        if (mpBlurWorkTexture)
            mpBlurWorkTexture.reset();

        //Rebuild programs im the array size changed
        /*
        if (arraySizeChanged)
        {
            mpHorizontalBlur.reset();
            mpVerticalBlur.reset();
        }
        */

        mpBlurWorkTexture = Texture::create2D(
            mpDevice, mTextureDims.x, mTextureDims.y, mTextureFormat, mArraySize, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        mpBlurWorkTexture->setName("ShadowMap::GaussianBlurTex");
    }
}

float getCoefficient(float sigma, float kernelWidth, float x)
{
    float sigmaSquared = sigma * sigma;
    float p = -(x * x) / (2 * sigmaSquared);
    float e = std::exp(p);

    float a = 2 * (float)M_PI * sigmaSquared;
    return e / a;
}

void SMGaussianBlur::updateKernel()
{
    uint32_t center = mKernelWidth / 2;
    float sum = 0;
    std::vector<float> weights(center + 1);
    for (uint32_t i = 0; i <= center; i++)
    {
        weights[i] = getCoefficient(mSigma, (float)mKernelWidth, (float)i);
        sum += (i == 0) ? weights[i] : 2 * weights[i];
    }

    if (mpWeightBuffer)
        mpWeightBuffer.reset();

    mpWeightBuffer = Buffer::createTyped<float>(mpDevice, mKernelWidth, Resource::BindFlags::ShaderResource);
    mpWeightBuffer->setName("ShadowMap::GaussianWeightBuffer");

    for (uint32_t i = 0; i <= center; i++)
    {
        float w = weights[i] / sum;
        mpWeightBuffer->setElement(center + i, w);
        mpWeightBuffer->setElement(center - i, w);
    }

}

} // namespace Falcor
