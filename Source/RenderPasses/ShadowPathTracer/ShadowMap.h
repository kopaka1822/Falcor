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
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include <vector>

using namespace Falcor;

class ShadowMap
{
public:
    ShadowMap(ref<Device> device, ref<Scene> scene);

    void setProjection(float near =-1.f , float far = -1.f);

    void execute(RenderContext* pRenderContext);

    ref<Texture> getTexture(uint lightNum) { return mpShadowMaps[lightNum]; }
    ref<Sampler> getSampler() { return mpShadowSampler; }

private:
    ref<Device> mpDevice;
    ref<Scene> mpScene;
    ref<Fbo> mpFbo;
    

    uint mShadowMapSize = 1024;
    ResourceFormat mShadowMapFormat = ResourceFormat::R32Float;

    float4x4 mProjectionMatrix = float4x4();
    float mNear = 0.01f;
    float mFar = 30.f;

    std::vector<ref<Texture>> mpShadowMaps;
    ref<Sampler> mpShadowSampler;
    ref<Texture> mpDepth;
    ref<Texture> mpTestTex;

    struct
    {
        ref<GraphicsState> pState;
        ref<GraphicsProgram> pProgram;
        ref<GraphicsVars> pVars;
    } mShadowPass;
};
