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
#pragma once
#include "Core/Macros.h"
#include "Core/Enum.h"
#include "Core/State/GraphicsState.h"
#include "Core/Program/ComputeProgram.h"
#include "Core/Program/GraphicsProgram.h"
#include "Core/Program/Program.h"
#include "Core/Program/ProgramReflection.h"
#include "Core/Program/ProgramVars.h"
#include "Core/Program/ProgramVersion.h"
#include "Core/Program/RtProgram.h"
#include "Utils/Properties.h"
#include "Utils/Debug/PixelDebug.h"
#include "Scene/Scene.h"

#include <memory>
#include <type_traits>
#include <vector>
#include <map>
#include <string>

/*
    Gaussian Blur 
*/
namespace Falcor
{
class RenderContext;

class FALCOR_API SMGaussianBlur
{
public:
    SMGaussianBlur(ref<Device> pDevice);

    void execute(RenderContext* pRenderContext, ref<Texture> pTexture);

    bool renderUI(Gui::Widgets& widget);

private:
    void prepareBlurTexture(ref<Texture> pTexture);
    void updateKernel();

    ref<Device> mpDevice;

    uint2 mTextureDims = uint2(0);
    std::string mDimMaxDefineString = "int2(0,0)";
    ResourceFormat mTextureFormat = ResourceFormat::Unknown;
    uint mArraySize = 0;

    bool mKernelChanged = true;
    uint32_t mKernelWidth = 5;
    float mSigma = 2.0f;

    ref<ComputePass> mpHorizontalBlur;
    ref<ComputePass> mpVerticalBlur;
    ref<Buffer> mpWeightBuffer;
    ref<Texture> mpBlurWorkTexture;

};
} // namespace Falcor
