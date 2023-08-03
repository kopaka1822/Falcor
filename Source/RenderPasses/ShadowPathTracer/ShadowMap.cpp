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
#include "ShadowMap.h"

namespace
{
    const std::string kDepthPassProgramFile = "RenderPasses/ShadowPathTracer/Shaders/ShadowMap.3d.slang";
    const std::string kShaderModel = "6_5";
}

ShadowMap::ShadowMap(ref<Device> device, ref<Scene> scene) : mpDevice{ device }, mpScene{ scene } {
    FALCOR_ASSERT(mpScene);

     mShadowPass.pState = GraphicsState::create(mpDevice);
    // Create FBO
    mpFbo = Fbo::create(mpDevice);

    mpDepth = Texture::create2D(
        mpDevice, mShadowMapSize, mShadowMapSize, ResourceFormat::D32Float, 1u, 1u, nullptr,
        ResourceBindFlags::DepthStencil
    );
        
    //Set the Textures
    std::vector<ref<Light>> lights = mpScene->getLights();

    for (ref<Light> light : lights)
    {
        bool isPoint = (light->getType() == LightType::Point) && (light->getData().openingAngle > M_PI_2);

        ref<Texture> tex;
        if (isPoint)
        {
            tex = Texture::createCube(
                mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, 1u, 1u, nullptr,
                ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource
            );
            tex->setName("ShadowTex");
        }
        else
            tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, 1u, 1u, nullptr,
                ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource
            );

        mpShadowMaps.push_back(tex);
    }  
    

    //Create shadow pass program.
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDepthPassProgramFile).vsEntry("vsMain").psEntry("psMain");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowPass.pProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        mShadowPass.pState->setProgram(mShadowPass.pProgram);
    }

    setProjection();

    // Create sampler.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpShadowSampler = Sampler::create(mpDevice, samplerDesc);
}

void ShadowMap::setProjection(float near, float far) {
    if (near > 0)
        mNear = near;
    if (far > 0)
        mFar = far;

    mProjectionMatrix = math::perspective(float(M_PI_2), 1.f, mNear, mFar);
}

void ShadowMap::execute(RenderContext* pRenderContext) {

    //Return if there is no scene
    if (!mpScene)
        return;

    //Loop over all lights
    std::vector<ref<Light>> lights = mpScene->getLights();
    for (size_t i=0 ; i<lights.size(); i++)
    {
        ref<Light> light = lights[i];
        auto& lightData = light->getData();
        bool isPoint = (light->getType() == LightType::Point) && (lightData.openingAngle > M_PI_2);

        if (Light::Changes::Active == light->getChanges() && !light->isActive())
        {
            //TODO clear texture
        }
        if (!light->isActive() || !isPoint)
            return;

        //TODO set defines ? 

        if (!mShadowPass.pVars)
        {
            mShadowPass.pVars = GraphicsVars::create(mpDevice, mShadowPass.pProgram.get());
        }

        for (size_t j = 0; j < 6; j++)
        {
            //For Lights other than point lights only 1 iteration is needed
            if (j > 0 && !isPoint)
                break;

            // Clear depth buffer.
            pRenderContext->clearDsv(mpDepth->getDSV().get(), 1.f, 0);
            pRenderContext->clearRtv(mpShadowMaps[i]->getRTV(0, j, 1).get(), float4(1.f));

            //Get Light tex
            mpFbo->attachColorTarget(mpShadowMaps[i], 0, 0, j, 1);
            
            //pRenderContext->clearFbo(mpFbo.get(), float4(0), 1.f, 0, FboAttachmentType::Color);
            
            float3 lightTarget;
            float3 up;
            switch (j)
            {
            case 0: //+x (or dir) 
                lightTarget = float3(1, 0, 0);
                up = float3(0, -1, 0);
                break;
            case 1: //-x
                lightTarget = float3(-1, 0, 0);
                up = float3(0, -1, 0);
                break;
            case 2: //+y
                lightTarget = float3(0, -1, 0);
                up = float3(0, 0, -1);
                break;
            case 3: //-y
                lightTarget = float3(0, 1, 0);
                up = float3(0, 0, 1);
                break;
            case 4: //+z
                lightTarget = float3(0, 0, 1);
                up = float3(0, -1, 0);
                break;
            case 5://-z
                lightTarget = float3(0, 0, -1);
                up = float3(0, -1, 0);
                break;
            }
            lightTarget += lightData.posW;
            float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, up);
            

            auto vars = mShadowPass.pVars->getRootVar();
            vars["CB"]["VPMat"] = math::mul(mProjectionMatrix, viewMat);
            vars["CB"]["farPlane"] = mFar;
            vars["CB"]["lightPos"] = lightData.posW;
           

            mpFbo->attachDepthStencilTarget(mpDepth);
            mShadowPass.pState->setFbo(mpFbo);

            mpScene->rasterize(pRenderContext, mShadowPass.pState.get(), mShadowPass.pVars.get(), RasterizerState::CullMode::None);
        }
    }
}
