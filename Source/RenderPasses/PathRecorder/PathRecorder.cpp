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
#include "PathRecorder.h"

const RenderPass::Info PathRecorder::kInfo { "PathRecorder", "Helper pass to record camera positions" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(PathRecorder::kInfo, PathRecorder::create);
}

PathRecorder::SharedPtr PathRecorder::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new PathRecorder());
    return pPass;
}

Dictionary PathRecorder::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PathRecorder::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    // no inputs/outputs
    return reflector;
}

void PathRecorder::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // obtain camera
    
}

void PathRecorder::renderUI(Gui::Widgets& widget)
{
    if(!mpScene)
    {
        widget.text("No scene loaded");
        return;
    }

    auto pCamera = mpScene->getCamera();

    if (widget.button("Keyframe"))
    {
        CameraData d;
        d.pos = pCamera->getPosition();
        d.target = pCamera->getTarget();
        d.up = pCamera->getUpVector();
        mPath.push_back(d);
    }

    if (widget.button("Clear"))
    {
        mPath.clear();
    }

    widget.text("current path length: " + std::to_string(mPath.size()));

    if (widget.button("Save to path.txt"))
    {
        std::ofstream file;
        file.open("path.txt");

        file << "positions = [";
        for (auto& d : mPath)
            file << d.pos.x << "," << d.pos.y << "," << d.pos.z << ",";
        file << "]\n";
        file << "targets = [";
        for (auto& d : mPath)
            file << d.target.x << "," << d.target.y << "," << d.target.z << ",";
        file << "]\n";
        file << "ups = [";
        for (auto& d : mPath)
            file << d.up.x << "," << d.up.y << "," << d.up.z << ",";
        file << "]\n";
    }
}

void PathRecorder::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
}
