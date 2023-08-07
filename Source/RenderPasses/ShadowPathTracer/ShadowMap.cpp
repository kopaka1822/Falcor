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

    // Create FBO
    mpFbo = Fbo::create(mpDevice);
    mpFboCube = Fbo::create(mpDevice);
            
    //Create Light Mapping Buffer
    prepareShadowMapBuffers();
    
     // Create shadow cube pass program.
    {
        mShadowCubePass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDepthPassProgramFile).vsEntry("vsMain").psEntry("psMainCube");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowCubePass.pProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        mShadowCubePass.pState->setProgram(mShadowCubePass.pProgram);
    }
    //Create shadow misc pass program.
    {
        mShadowMiscPass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDepthPassProgramFile).vsEntry("vsMain").psEntry("psMainMisc");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowMiscPass.pProgram = GraphicsProgram::create(mpDevice, desc, mpScene->getSceneDefines());
        mShadowMiscPass.pState->setProgram(mShadowMiscPass.pProgram);
    }

    setProjection();

    // Create sampler.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpShadowSampler = Sampler::create(mpDevice, samplerDesc);

    mFirstFrame = true;
}

void ShadowMap::prepareShadowMapBuffers() {

    if (mShadowResChanged || !mpDepth)
    {
        auto format = mShadowMapCubeFormat == ResourceFormat::R32Float ? ResourceFormat::D32Float : ResourceFormat::D16Unorm;
        mpDepth =
            Texture::create2D(mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, format, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil);
        mpDepth->setName("ShadowMapPassDepthHelper");
    }

    //Reset existing shadow maps
    if (mpShadowMaps.size() > 0 || mpShadowMapsCube.size() > 0 || mShadowResChanged || mResetShadowMapBuffers)
    {
        for (auto shadowMap : mpShadowMaps)
            shadowMap.reset();
        for (auto shadowMap : mpShadowMapsCube)
            shadowMap.reset();

        mpShadowMaps.clear();
        mpShadowMapsCube.clear();
    }

    //Reset the light mapping
    if (mResetShadowMapBuffers)
    {
        mpLightMapping.reset();
        mpVPMatrixBuffer.reset();
    }
        

    // Set the Textures
    const std::vector<ref<Light>>& lights = mpScene->getLights();
    uint countPoint = 0;
    uint countMisc = 0;
    std::vector<uint> lightMapping;
    mIsCubeSM.clear();
    lightMapping.reserve(lights.size());
    mIsCubeSM.reserve(lights.size());

    for (ref<Light> light : lights)
    {
        ref<Texture> tex;
        if (isPointLight(light))
        {
            tex = Texture::createCube(
                mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, mShadowMapCubeFormat, 1u, 1u, nullptr,
                ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource
            );
            tex->setName("ShadowMapCube" + std::to_string(countPoint));
            lightMapping.push_back(countPoint); // Push Back Point ID
            mIsCubeSM.push_back(true);
            countPoint++;
            mpShadowMapsCube.push_back(tex);
        }
        else
        {
            tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, mShadowMap2DFormat, 1u, 1u, nullptr,
                ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource
            );
            tex->setName("ShadowMapMisc" + std::to_string(countMisc));
            lightMapping.push_back(countMisc); // Push Back Misc ID
            mIsCubeSM.push_back(false);
            countMisc++;
            mpShadowMaps.push_back(tex);
        }
    }

    //Light Mapping Buffer
    if (!mpLightMapping)
    {
        mpLightMapping = Buffer::createStructured(
            mpDevice, sizeof(uint), lightMapping.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, lightMapping.data(),
            false
        );
        mpLightMapping->setName("ShadowMapLightMapping");
    }

    if (!mpVPMatrixStangingBuffer && mpShadowMaps.size() > 0)
    {
        std::vector<float4x4> initData(mpShadowMaps.size());
        for (size_t i = 0; i < initData.size(); i++)
            initData[i] = float4x4();
        mpVPMatrixStangingBuffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), mpShadowMaps.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::Write, initData.data(),
            false
        );
        mpVPMatrixStangingBuffer->setName("ShadowMapViewProjectionStagingBuffer");
    }

    if (!mpVPMatrixBuffer && mpShadowMaps.size() > 0)
    {
        mpVPMatrixBuffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), mpShadowMaps.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false );
        mpVPMatrixBuffer->setName("ShadowMapViewProjectionBuffer");
    }
    

    mResetShadowMapBuffers = false;
    mShadowResChanged = false;
    mFirstFrame = true;
}

void ShadowMap::setProjection(float near, float far) {
    if (near > 0)
        mNear = near;
    if (far > 0)
        mFar = far;

    mProjectionMatrix = math::perspective(float(M_PI_2), 1.f, mNear, mFar);
    auto& sceneBounds = mpScene->getSceneBounds();
    mDirLightPosOffset = sceneBounds.radius();
    mSceneCenter = sceneBounds.center();
    mOrthoMatrix =
        math::ortho(-sceneBounds.radius(), sceneBounds.radius(), -sceneBounds.radius(), sceneBounds.radius(), near, sceneBounds.radius() * 2);
}

bool ShadowMap::isPointLight(const ref<Light> light) {
    return (light->getType() == LightType::Point) && (light->getData().openingAngle > M_PI_2);
}

void ShadowMap::setSMShaderVars(ShaderVar& var, ShaderParameters& params)
{
    var["CB"]["gviewProjection"] = params.viewProjectionMatrix;
    var["CB"]["gLightPos"] = params.lightPosition;
    var["CB"]["gFarPlane"] = params.farPlane;
}

bool ShadowMap::update(RenderContext* pRenderContext)
{

    //Return if there is no scene
    if (!mpScene)
        return false;

    if (mAlwaysRenderSM)
        mFirstFrame = true;

    //Rebuild the Shadow Maps
    if (mResetShadowMapBuffers || mShadowResChanged)
        prepareShadowMapBuffers();

    //Loop over all lights
    const std::vector<ref<Light>>& lights = mpScene->getLights();   

    //Create Render List
    std::vector<ref<Light>> lightRenderListCube;    //Light List for cube render process
    std::vector<ref<Light>> lightRenderListMisc;    //Light List for 2D texture shadow maps
    for (size_t i = 0; i < lights.size(); i++)
    {
        ref<Light> light = lights[i];
        bool isPoint = isPointLight(light);

        // Check if the type has changed and end the pass if that is the case
        if (isPoint != mIsCubeSM[i])
        {
            mResetShadowMapBuffers = true;
            return false;
        }

        if (isPoint)
            lightRenderListCube.push_back(light);
        else
            lightRenderListMisc.push_back(light);
    }

    //Render all cube lights
    for (size_t i = 0; i < lightRenderListCube.size(); i++)
    {
        //Create Program Vars
        if (!mShadowCubePass.pVars)
        {
            mShadowCubePass.pVars = GraphicsVars::create(mpDevice, mShadowCubePass.pProgram.get());
        }

        auto light = lightRenderListCube[i];

        auto changes = light->getChanges();
        bool renderLight = changes == Light::Changes::Active || changes ==  Light::Changes::Position || mFirstFrame;

        if (!renderLight || !light->isActive())
            continue;        

        auto& lightData = light->getData();

        ShaderParameters params;
        params.lightPosition = lightData.posW;
        params.farPlane = mFar;
        
         for (size_t j = 0; j < 6; j++)
        {
            // Clear depth buffer.
            pRenderContext->clearDsv(mpDepth->getDSV().get(), 1.f, 0);

            //Attach Render Targets
            mpFboCube->attachColorTarget(mpShadowMapsCube[i], 0, 0, j, 1);
            mpFboCube->attachDepthStencilTarget(mpDepth);

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
            case 5: //-z
                lightTarget = float3(0, 0, -1);
                up = float3(0, -1, 0);
                break;
            }
            lightTarget += lightData.posW;
            float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, up);

            params.viewProjectionMatrix = math::mul(mProjectionMatrix, viewMat);
            
            auto vars = mShadowCubePass.pVars->getRootVar();
            setSMShaderVars(vars, params);

            mShadowCubePass.pState->setFbo(mpFboCube);
            mpScene->rasterize(pRenderContext, mShadowCubePass.pState.get(), mShadowCubePass.pVars.get(), RasterizerState::CullMode::None);
        }

    }

    std::vector<float4x4> viewProjectionMatrix(lightRenderListMisc.size());
    std::vector<bool> wasRendered(lightRenderListMisc.size());
    bool updateVPBuffer = false;

    //Render all spot / directional lights
    for (size_t i = 0; i < lightRenderListMisc.size(); i++)
    {
        // Create Program Vars
        if (!mShadowMiscPass.pVars)
        {
            mShadowMiscPass.pVars = GraphicsVars::create(mpDevice, mShadowMiscPass.pProgram.get());
        }

        auto light = lightRenderListMisc[i];

        auto changes = light->getChanges();
        bool renderLight = changes == Light::Changes::Active || changes == Light::Changes::Position || changes == Light::Changes::Direction || mFirstFrame;

        if (!renderLight || !light->isActive())
        {
            viewProjectionMatrix[i] = float4x4();
            wasRendered[i] = false;
            continue;
        }

        wasRendered[i] = true;
        updateVPBuffer |= true;

        auto& lightData = light->getData();

         // Clear depth buffer.
        pRenderContext->clearDsv(mpShadowMaps[i]->getDSV().get(), 1.f, 0);

        // Attach Render Targets
        mpFbo->attachDepthStencilTarget(mpShadowMaps[i]);

        ShaderParameters params;
        if (light->getType() == LightType::Directional)
        {
            float3 lightTarget = math::normalize(lightData.dirW);
            params.lightPosition = mSceneCenter - lightTarget * mDirLightPosOffset;
            params.farPlane = 2 * mDirLightPosOffset;
            lightTarget += params.lightPosition;

            float4x4 viewMat = math::matrixFromLookAt(params.lightPosition, lightTarget, float3(0, 1, 0));
            params.viewProjectionMatrix = math::mul(mOrthoMatrix, viewMat);
        }
        else
        {
            float3 lightTarget = lightData.posW + lightData.dirW;
            float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, float3(0, 1, 0));

            params.lightPosition = lightData.posW;
            params.farPlane = mFar;
            params.viewProjectionMatrix = math::mul(mProjectionMatrix, viewMat);
        }
        
        viewProjectionMatrix[i] = params.viewProjectionMatrix;

        auto vars = mShadowMiscPass.pVars->getRootVar();
        setSMShaderVars(vars, params);

        mShadowMiscPass.pState->setFbo(mpFbo);
        mpScene->rasterize(pRenderContext, mShadowMiscPass.pState.get(), mShadowMiscPass.pVars.get(), RasterizerState::CullMode::None);
    }

    //Write all ViewProjectionMatrix to the buffer
    //TODO optimize this depending on the number of active lights
    if (updateVPBuffer)
    {
        float4x4* mats = (float4x4*) mpVPMatrixStangingBuffer->map(Buffer::MapType::WriteDiscard);
        for (size_t i = 0; i < viewProjectionMatrix.size(); i++)
        {
            if (!wasRendered[i])
                continue;
            mats[i] = viewProjectionMatrix[i];
        }
        mpVPMatrixStangingBuffer->unmap();

        pRenderContext->copyBufferRegion(
            mpVPMatrixBuffer.get(), 0, mpVPMatrixStangingBuffer.get(), 0, sizeof(float4x4) * viewProjectionMatrix.size()
        );
    }

    mFirstFrame = false;
    return true;
}
