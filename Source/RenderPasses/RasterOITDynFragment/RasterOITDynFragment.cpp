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
#include "RasterOITDynFragment.h"

#include "Scene/Lighting/LightSettings.h"
#include "Scene/Lighting/ShadowSettings.h"

namespace
{
    const std::string kDepth = "depth"; // input depth buffer
    const std::string kColor = "color";

    const std::string kWhitelist = "whitelist";

    const std::string kProgramFile = "RenderPasses/RasterOITDynFragment/BuildList.3D.slang";
    const std::string kSortFile = "RenderPasses/RasterOITDynFragment/Sort.slang";
    const std::string kScanFile = "RenderPasses/RasterOITDynFragment/scan.hlsl";
    const std::string kScanPushFile = "RenderPasses/RasterOITDynFragment/scanpush.hlsl";
}


extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, RasterOITDynFragment>();
}

RasterOITDynFragment::RasterOITDynFragment(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    // default graphics state
    mpState = GraphicsState::create(mpDevice);
    DepthStencilState::Desc dsDesc;
    dsDesc.setDepthWriteMask(false); // disable depth writes
    auto dsState = DepthStencilState::create(dsDesc);
    mpState->setDepthStencilState(dsState);
    mpFbo = Fbo::create(mpDevice);

    mpScanPass = ComputePass::create(mpDevice, kScanFile);
    mpScanPushPass = ComputePass::create(mpDevice, kScanPushFile);

    mpSortPass = ComputePass::create(mpDevice, kSortFile);

    // optimized sort passes
    // maximum fragment count for the resolve stage
    static constexpr auto resolveIntervals = std::array{ 256, 128, 64, 32, 16, 8, 4, 0 /*only for MIN_FRAGMENT*/ };

    DefineList s;
    for (size_t i = 0; i < resolveIntervals.size() - 1; ++i)
    {
        s["MAX_FRAGMENT"] = std::to_string(resolveIntervals[i]);
        s["MIN_FRAGMENT"] = std::to_string(resolveIntervals[i + 1]);
        mpOptimizedSortPasses.push_back(ComputePass::create(mpDevice, kSortFile, "main", s));
    }

    // share vars
    for(auto& pass : mpOptimizedSortPasses)
    {
        pass->setVars(mpSortPass->getVars());
    }
}

Properties RasterOITDynFragment::getProperties() const
{
    return {};
}

RenderPassReflection RasterOITDynFragment::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kDepth, "Depth buffer");

    reflector.addOutput(kColor, "Color buffer").format(ResourceFormat::RGBA16Float).bindFlags(ResourceBindFlags::AllColorViews);
    return reflector;
}

void RasterOITDynFragment::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    setupScanBuffer(compileData.defaultTexDims);
}

void RasterOITDynFragment::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pDepth = renderData.getTexture(kDepth);
    auto pColor = renderData.getTexture(kColor);

    uint2 dim = uint2(pDepth->getWidth(), pDepth->getHeight());

    // clear resources
    uint32_t uintmax = uint32_t(-1);
    pRenderContext->clearTexture(pColor.get(), float4(0, 0, 0, 1));
    pRenderContext->clearUAV(mpCountBuffer->getUAV().get(), uint4(0));

    pRenderContext->uavBarrier(mpCountBuffer.get());

    for(auto& buffer : m_scanAuxBuffer)
    {
        pRenderContext->clearUAV(buffer->getUAV().get(), uint4(0));
        pRenderContext->uavBarrier(buffer.get());
    }


    if (!mpScene)
    {
        return;
    }

    if (!mpDataBuffer || mpDataBuffer->getElementCount() != mDataBufferSize)
    {
        mpDataBuffer = Buffer::createStructured(mpDevice, sizeof(uint4), mDataBufferSize, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None);
    }
    pRenderContext->clearUAV(mpDataBuffer->getUAV().get(), uint4(0));
    pRenderContext->uavBarrier(mpDataBuffer.get());

    // obtain whitelist
    std::set<std::string> whitelist;
    const bool useWhitelist = renderData.getDictionary().keyExists(kWhitelist);
    if (useWhitelist)
        whitelist = renderData.getDictionary().getValue<decltype(whitelist)>(kWhitelist);

    // setup culling
    auto camera = mpScene->getCamera();
    if (!mpCulling)
    {
        mpCulling = make_ref<FrustumCulling>(camera);
    }

    mpCulling->setUserCallback([&](const MeshDesc& mesh)
        {
            if (!useWhitelist) return true; // draw all transparent objects
            auto mat = mpScene->getMaterial(MaterialID::fromSlang(mesh.materialID));
            auto name = mat->getName();
            return whitelist.find(name) != whitelist.end();
        });

    auto cameraChanges = camera->getChanges();
    auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
    if (((cameraChanges & ~excluded) != Camera::Changes::None))
    {
        mpCulling->updateFrustum(camera);
    }

    // framebuffer setup (only depth)
    mpFbo->attachDepthStencilTarget(pDepth);
    mpState->setFbo(mpFbo);

    // setup vars and program
    auto vars = mpVars->getRootVar();
    vars["gCounts"] = mpCountBuffer;

    vars["PerFrame"]["gFrameDim"] = dim;
    vars["PerFrame"]["maxElements"] = mpDataBuffer->getElementCount();

    // lighting settings
    LightSettings::get().updateShaderVar(vars);
    ShadowSettings::get().updateShaderVar(mpDevice, vars, mFrameCount++);
    mpProgram->addDefines(ShadowSettings::get().getShaderDefines(*mpScene, renderData.getDefaultTextureDims()));

    // count fragments
    {
        FALCOR_PROFILE(pRenderContext, "Count");

        mpProgram->addDefine("COUNT", "1");
        mpState->setProgram(mpProgram);

        mpScene->rasterizeFrustumCulling(pRenderContext, mpState.get(), mpVars.get(), RasterizerState::CullMode::None, RasterizerState::MeshRenderMode::SkipOpaque, true, mpCulling);

        pRenderContext->uavBarrier(mpCountBuffer.get()); // counts need to be finalized before scan
    }

    // scan counters
    performScan(pRenderContext);

    // record fragments
    {
        FALCOR_PROFILE(pRenderContext, "Record");
        mpProgram->removeDefine("COUNT");

        vars["gCounts"] = mpCountBuffer;
        vars["gBuffer"] = mpDataBuffer;
        vars["gPrefix"] = m_scanAuxBuffer.front();

        mpState->setProgram(mpProgram);

        mpScene->rasterizeFrustumCulling(pRenderContext, mpState.get(), mpVars.get(), RasterizerState::CullMode::None, RasterizerState::MeshRenderMode::SkipOpaque, true, mpCulling);
    }

    // sort fragments
    {
        FALCOR_PROFILE(pRenderContext, "Sort");
        pRenderContext->uavBarrier(mpDataBuffer.get());

        auto sortVars = mpSortPass->getRootVar();
        sortVars["gBuffer"] = mpDataBuffer;
        sortVars["gPrefix"] = m_scanAuxBuffer.front();
        sortVars["gColor"] = pColor;

        sortVars["PerFrame"]["gFrameDim"] = dim;
        sortVars["PerFrame"]["maxFragmentCount"] = mpDataBuffer->getElementCount();

        if(mOptimizeSort)
        {
            for(auto& sortPass : mpOptimizedSortPasses)
            {
                sortPass->execute(pRenderContext, dim.x, dim.y);
            }
        }
        else // single sort in global memory
        {
            mpSortPass->execute(pRenderContext, dim.x, dim.y);
        }

        
    }
    
}

void RasterOITDynFragment::renderUI(Gui::Widgets& widget)
{
    widget.var("Buffer Node Count", mDataBufferSize, 1024u, 1024u * 1024u * 1024u);
    auto sizeInBytes = size_t(mDataBufferSize) * size_t(16);
    widget.text("Size in MB: " + std::to_string(sizeInBytes / (1024u * 1024u)));

    widget.checkbox("Optimize Sort", mOptimizeSort);
}

void RasterOITDynFragment::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    setupProgram();
    mpCulling = nullptr;
}

void RasterOITDynFragment::setupScanBuffer(uint2 res)
{
    auto size = size_t(res.x) * size_t(res.y);
    const auto alignment = s_scanWorkgroup * s_scanLocal;
    m_curScanSize = GetAligned(size, alignment);
    m_scanLastIdx = size - 1;

    mpCountBuffer = Buffer::createStructured(mpDevice, sizeof(uint), GetAligned(m_curScanSize, 4), ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpCountBuffer->setName("CountBuffer");

    m_scanAuxBuffer.clear();
    auto bs = m_curScanSize;
    while (bs > 1)
    {
        m_scanAuxBuffer.emplace_back(
            Buffer::createStructured(mpDevice, sizeof(uint32_t), GetAligned(bs, 4), ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource));
        bs /= alignment;
    }

    m_scanAuxBuffer.front()->setName("ScanAuxBuffer0");
}

void RasterOITDynFragment::performScan(RenderContext* pRenderContext)
{
    FALCOR_PROFILE(pRenderContext, "Scan");

    auto bs = m_curScanSize;
    int i = 0;
    const auto elemPerWk = s_scanWorkgroup * s_scanLocal;

    {
        auto scanVars = mpScanPass->getRootVar();
        // write to aux
        scanVars["BufferData"]["u_writeAux"] = 1;

        // hierarchical scan of blocks
        while (bs > 1)
        {
            // set source
            if (i == 0)
                scanVars["in_buffer"] = mpCountBuffer;
            else
                scanVars["in_buffer"] = m_scanAuxBuffer[i];

            // set destination
            scanVars["data"] = m_scanAuxBuffer[i];

            // Bind the auxiliary buffer for the next step 
            if (size_t(i + 1) < m_scanAuxBuffer.size())
                scanVars["aux"] = m_scanAuxBuffer[i + 1];
            else scanVars["BufferData"]["u_writeAux"] = 0; // don't write to aux

            scanVars["BufferData"]["u_bufferSize"] = m_scanAuxBuffer.at(i)->getElementCount();

            // perform scan
            mpScanPass->execute(pRenderContext, ((bs + elemPerWk - 1) / elemPerWk) * s_scanWorkgroup, 1, 1);

            pRenderContext->uavBarrier(m_scanAuxBuffer[i].get());
            if (size_t(i + 1) < m_scanAuxBuffer.size())
                pRenderContext->uavBarrier(m_scanAuxBuffer[i + 1].get());

            bs /= elemPerWk;
            ++i;
        }
    }

    auto pushVars = mpScanPushPass->getRootVar();

    pushVars["BufferData"]["u_stride"] = elemPerWk;
    //commands.SetComputeRoot32BitConstant(m_scanConstantsIdx, 0, 1);

    --i; bs = m_curScanSize;
    while (bs > elemPerWk) bs /= elemPerWk;
    while (bs < m_curScanSize)
    {
        bs *= elemPerWk;

        pushVars["aux_data"] = m_scanAuxBuffer[i];
        pushVars["data"] = m_scanAuxBuffer[i - 1];

        //if (i == 1) // last write
        //    commands.SetComputeRoot32BitConstant(m_scanConstantsIdx, UINT(m_scanLastIdx), 1);

        mpScanPushPass->execute(pRenderContext, ((bs - elemPerWk) / 64) * s_scanWorkgroup, 1, 1);

        --i;
        pRenderContext->uavBarrier(m_scanAuxBuffer[i].get());
    }
}

void RasterOITDynFragment::setupProgram()
{
    if (!mpScene) return;

    Program::Desc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kProgramFile).psEntry("psMain").vsEntry("vsMain");
    desc.addTypeConformances(mpScene->getTypeConformances());
    desc.setShaderModel("6_5");

    mpProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines());
    mpState->setProgram(mpProgram);
    mpVars = GraphicsVars::create(mpDevice, mpProgram->getReflector());
}
