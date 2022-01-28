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
#pragma once
#include "Falcor.h"
#include "FalcorExperimental.h"

using namespace Falcor;

class DualDepthPass : public RenderPass
{
public:
    using SharedPtr = std::shared_ptr<DualDepthPass>;

    static SharedPtr create(RenderContext* pRenderContext = nullptr, const Dictionary& dict = {});

    enum class Variant
    {
        UavOnly,
        DepthAndUav,
        DepthPeel,
        DepthAndRay
    };

    virtual std::string getDesc() override;
    virtual Dictionary getScriptingDictionary() override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    DualDepthPass& setDepthStencilState(const DepthStencilState::SharedPtr& pDsState);
    void setCullMode(RasterizerState::CullMode cullMode) { mCullMode = cullMode; }

private:
    DualDepthPass(const Dictionary& dict);
    void parseDictionary(const Dictionary& dict);

    Fbo::SharedPtr mpFbo;

    GraphicsState::SharedPtr mpDsvUavState;
    GraphicsVars::SharedPtr mpDsvUavVars;

    GraphicsState::SharedPtr mpDepthPeelState1;
    GraphicsState::SharedPtr mpDepthPeelState2;
    GraphicsVars::SharedPtr mpDepthPeelVars1;
    GraphicsVars::SharedPtr mpDepthPeelVars2;

    GraphicsState::SharedPtr mpUavState;
    GraphicsVars::SharedPtr mpUavVars;

    RtProgram::SharedPtr mpRayProgram;
    RtProgramVars::SharedPtr mpRayVars;

    RasterizerState::CullMode mCullMode = RasterizerState::CullMode::Back;
    Scene::SharedPtr mpScene;
    Variant mVariant = Variant::DepthPeel;
};
