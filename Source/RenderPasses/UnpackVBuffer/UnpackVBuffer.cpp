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
#include "UnpackVBuffer.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "RenderGraph/RenderPassHelpers.h"

namespace
{
    const std::string kVBuffer = "vbuffer";

    const ChannelList kVBufferExtraChannels =
    {
        { "posW","gPosW","World Space Position",true /* optional */, ResourceFormat::RGBA32Float},
        { "prevPosW", "gPrevPosW", "Previous World Space Position", true, ResourceFormat::RGBA32Float}, 
        { "normalW","gNormalW","World Space Normal",true /* optional */, ResourceFormat::RGBA16Float},
        {"normalV", "gNormalV", "View Space Normal", true, ResourceFormat::RGBA16Float},
        { "faceNormalW","gFaceNormalW","World Space Face Normal",true /* optional */, ResourceFormat::RGBA16Float},
        {"rasterZ", "gRasterZ", "Non-linear z values as in rasterization", true, ResourceFormat::R32Float},
        {"linearZ", "gLinearZ", "Linear z values from camera space (positive)", true, ResourceFormat::R32Float},
        {"roughness", "gRoughness", "0-1 material roughness", true, ResourceFormat::R8Unorm},
        {"diffuse", "gDiffuse", "diffuse material property", true, ResourceFormat::RGBA8Unorm},
        {"specular", "gSpecular", "specular material property", true, ResourceFormat::RGBA8Unorm},
        {"normalWRoughnessMaterialID", "gNormalWRoughnessMaterialID", "Guide normal in world space, roughness, and material ID", true, ResourceFormat::RGB10A2Unorm}
    };

    const std::string kProgramComputeFile = "RenderPasses/UnpackVBuffer/Unpack.cs.slang";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, UnpackVBuffer>();
}

UnpackVBuffer::UnpackVBuffer(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
}

Properties UnpackVBuffer::getProperties() const
{
    return {};
}

RenderPassReflection UnpackVBuffer::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kVBuffer, "vbuffer");
    // get input dim
    mLastDim = compileData.defaultTexDims;
    auto vbuffer = compileData.connectedResources.getField(kVBuffer);
    if (vbuffer)
        mLastDim = { vbuffer->getWidth(), vbuffer->getHeight() };

    addRenderPassOutputs(reflector, kVBufferExtraChannels, ResourceBindFlags::UnorderedAccess, mLastDim);
    return reflector;
}

void UnpackVBuffer::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    auto vbuffer = compileData.connectedResources.getField(kVBuffer);
    if (!vbuffer) throw std::runtime_error("UnpackVBuffer: VBuffer input not connected");

    if (mLastDim.x != vbuffer->getWidth() || mLastDim.y != vbuffer->getHeight())
        throw std::runtime_error("UnpackVBuffer: Output size mismatch");


    mpComputePass.reset();
}

void UnpackVBuffer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{

    if (mpScene == nullptr)
    {
        clearRenderPassChannels(pRenderContext, kVBufferExtraChannels, renderData);
        return;
    }

    if(!mpComputePass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kProgramComputeFile).csEntry("main").setShaderModel("6_5");
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        //defines.add(mpSampleGenerator->getDefines());
        defines.add(getShaderDefines(renderData));

        mpComputePass = ComputePass::create(mpDevice, desc, defines, true);

        // Bind static resources
        ShaderVar var = mpComputePass->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);
        //mpSampleGenerator->setShaderData(var);
    }

    mpComputePass->getProgram()->addDefines(getShaderDefines(renderData));

    ShaderVar var = mpComputePass->getRootVar();
    auto pVBuffer = renderData.getTexture(kVBuffer);
    var["gVBuffer"] = pVBuffer;
    for (const auto& channel : kVBufferExtraChannels)
    {
        var[channel.texname] = renderData.getTexture(channel.name);
    }

    mpComputePass->execute(pRenderContext, uint3(pVBuffer->getWidth(), pVBuffer->getHeight(), 1));
}

void UnpackVBuffer::renderUI(Gui::Widgets& widget)
{
}

void UnpackVBuffer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mpComputePass.reset();
}

DefineList UnpackVBuffer::getShaderDefines(const RenderData& renderData) const
{
    DefineList defines;
    defines.add(getValidResourceDefines(kVBufferExtraChannels, renderData));
    return defines;
}
