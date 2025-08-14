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
#include "RasterVBuffer.h"

#include "Utils/SampleGenerators/HaltonSamplePattern.h"

namespace
{
    const std::string kVbuffer = "vbuffer";
    const std::string kDepth = "depth";

    const std::string kProgramFile = "RenderPasses/RasterVBuffer/RasterVBuffer.3D.slang";

    const std::string kUseWhitelist = "useWhitelist";
    const std::string kWhitelist = "whitelist";
    const std::string kWhitelistBuffer = "whitelistBuffer"; // GPU Buffer for whitelist
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, RasterVBuffer>();
}

RasterVBuffer::RasterVBuffer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    // default graphics state
    mpState = GraphicsState::create(mpDevice);
    mpFbo = Fbo::create(mpDevice);

    mpSamplePattern = HaltonSamplePattern::create(16);
    
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

Properties RasterVBuffer::getProperties() const
{
    Properties props;
    props[kUseWhitelist] = mUseTransparencyWhitelist;
    // convert whitelist into a comma separated string
    std::stringstream ss;
    for (const auto& entry : mTransparencyWhitelist) ss << entry << ",";
    props[kWhitelist] = ss.str();
    return props;
}

RenderPassReflection RasterVBuffer::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    reflector.addOutput(kVbuffer, "V-buffer").format(HitInfo::kDefaultFormat);
    reflector.addOutput(kDepth, "Depth Bufer").format(ResourceFormat::D32FloatS8X24);
    return reflector;
}

void RasterVBuffer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pVbuffer = renderData.getTexture(kVbuffer);
    auto pDepth = renderData.getTexture(kDepth);

    if (!mpScene)
    {
        pRenderContext->clearUAV(pVbuffer->getUAV().get(), uint4(0));
        return;
    }

    assert(mpProgram);
    assert(mpVars);

    uint2 frameDim = uint2(pVbuffer->getWidth(), pVbuffer->getHeight());
    if(mCameraJitter)
        mpScene->getCamera()->setPatternGenerator(mpSamplePattern, 1.0f / float2(frameDim));
    else
        mpScene->getCamera()->setPatternGenerator(nullptr, 1.0f / float2(frameDim));

    mpProgram->addDefine("ALPHA_TEXTURE_LOD", mUseAlphaTextureLOD ? "1" : "0");

    // clear buffers
    pRenderContext->clearDsv(pDepth->getDSV().get(), 1.0f, 0);
    pRenderContext->clearUAV(pVbuffer->getUAV().get(), uint4(0));

    // framebuffer
    mpFbo->attachDepthStencilTarget(pDepth);
    mpFbo->attachColorTarget(pVbuffer, 0);
    mpState->setFbo(mpFbo);

    auto cullMode = mCullBackFaces ? RasterizerState::CullMode::Back : RasterizerState::CullMode::None;
    {
        FALCOR_PROFILE(pRenderContext, "Opaque");
        mpState->setProgram(mpOpaqueProgram);
        mpScene->rasterizeFrustumCulling(pRenderContext, mpState.get(), mpVars.get(), cullMode, RasterizerState::MeshRenderMode::SkipNonOpaque);
    }

    if (mUseTransparencyWhitelist) // if whitelist is not used, this means all alpha tests are treated as transparent (not handled by this pass)
    {
        FALCOR_PROFILE(pRenderContext, "Alpha Test");

        mpState->setProgram(mpProgram);

        auto camera = mpScene->getCamera();
        if (!mpCulling)
        {
            mpCulling = make_ref<FrustumCulling>(camera);
        }

        mpCulling->setUserCallback([&](const MeshDesc& mesh)
        {
            auto mat = mpScene->getMaterial(MaterialID::fromSlang(mesh.materialID));
            auto name = mat->getName();
            // draw if not in whitelist (=> alpha tested)
            return mTransparencyWhitelist.find(name) == mTransparencyWhitelist.end();
        });

        auto cameraChanges = camera->getChanges();
        auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
        if (((cameraChanges & ~excluded) != Camera::Changes::None))
        {
            mpCulling->updateFrustum(camera);
        }

        mpScene->rasterizeFrustumCulling(pRenderContext, mpState.get(), mpVars.get(), RasterizerState::CullMode::None, RasterizerState::MeshRenderMode::SkipOpaque, true, mpCulling);
    }

    // add whitelist to dict
    if (mUseTransparencyWhitelist)
    {
        renderData.getDictionary()[kWhitelist] = mTransparencyWhitelist;
        renderData.getDictionary()[kWhitelistBuffer] = mpTransparencyWhitelist;
    }
}

void RasterVBuffer::renderUI(Gui::Widgets& widget)
{
    if (auto g = widget.group("Scene"))
    {
        //widget.checkbox("Frustrum Culling", mFrustrumCulling);

        widget.checkbox("Camera Jitter", mCameraJitter);

        widget.checkbox("Alpha Texture LOD", mUseAlphaTextureLOD);

        widget.checkbox("Cull Back Faces (Opaque)", mCullBackFaces);
        widget.tooltip("Cull back faces for opaque objects. Culling is always disabled for transparent faces.");

        widget.checkbox("Transparency Whitelist", mUseTransparencyWhitelist);
        widget.tooltip("This pass only draws objects that are not marked as transparent by this pass. The transparent materials will be drawn by another pass.");
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
                    updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
                }
            }
        }
    }
}

void RasterVBuffer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    setupProgram();
    mUseTransparencyWhitelist = updateWhitelist(mpDevice, mpScene, mTransparencyWhitelist, mpTransparencyWhitelist);
    mpCulling = nullptr;
}

void RasterVBuffer::setupProgram()
{
    if (!mpScene) return;

    Program::Desc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kProgramFile).psEntry("psMain").vsEntry("vsMain");
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setShaderModel("6_5");

    mpProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    mpOpaqueProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines().add("OPAQUE_SHADER", "1"));
    mpState->setProgram(mpProgram);
    mpVars = GraphicsVars::create(mpDevice, mpProgram->getReflector());
}

bool RasterVBuffer::hasWhitelistMaterials()
{
    if (!mpScene) return true;
    bool any = false;

    uint32_t materialCount = mpScene->getMaterialCount();

    // Pack the boolean values into bits
    for (uint32_t mat = 0; mat < materialCount; ++mat)
    {
        std::string name = mpScene->getMaterial(MaterialID(mat))->getName();
        bool isTransparent = mTransparencyWhitelist.find(name) != mTransparencyWhitelist.end();
        any |= isTransparent;
    }


    return any;
}
