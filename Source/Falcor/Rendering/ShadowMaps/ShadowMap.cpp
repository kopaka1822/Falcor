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
#include "Scene/Camera/Camera.h"
#include "Utils/Math/FalcorMath.h"

namespace Falcor
{
namespace
{
const std::string kDepthPassProgramFile = "Rendering/ShadowMaps/GenerateShadowMap.3d.slang";
const std::string kReflectTypesFile = "Rendering/ShadowMaps/ReflectTypesForParameterBlock.cs.slang";
const std::string kShaderModel = "6_5";

const Gui::DropdownList kShadowMapCullMode{
    {(uint)RasterizerState::CullMode::None, "None"},
    {(uint)RasterizerState::CullMode::Front, "Front"},
    {(uint)RasterizerState::CullMode::Back, "Back"},
};
} // namespace

ShadowMap::ShadowMap(ref<Device> device, ref<Scene> scene) : mpDevice{device}, mpScene{scene}
{
    FALCOR_ASSERT(mpScene);

    // Create FBO
    mpFbo = Fbo::create(mpDevice);
    mpFboCube = Fbo::create(mpDevice);

    // Create Light Mapping Buffer
    prepareShadowMapBuffers();

    prepareProgramms();

    setProjection();

    // Create sampler.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
    mpShadowSampler = Sampler::create(mpDevice, samplerDesc);

    // Set RasterizerStateDescription
    updateRasterizerStates();

    mFirstFrame = true;
}

void ShadowMap::prepareShadowMapBuffers()
{
    if ((mShadowResChanged || mResetShadowMapBuffers) && mpDepth)
        mpDepth.reset();

    // Reset existing shadow maps
    if (mpShadowMaps.size() > 0 || mpShadowMapsCube.size() > 0 || mShadowResChanged || mResetShadowMapBuffers)
    {
        for (auto shadowMap : mpShadowMaps)
            shadowMap.reset();
        for (auto shadowMap : mpShadowMapsCube)
            shadowMap.reset();

        mpShadowMaps.clear();
        mpShadowMapsCube.clear();
    }

    // Reset the light mapping
    if (mResetShadowMapBuffers)
    {
        mpLightMapping.reset();
        mpVPMatrixBuffer.reset();
    }

    // Set the Textures
    const std::vector<ref<Light>>& lights = mpScene->getLights();
    uint countPoint = 0;
    uint countMisc = 0;
    uint countCascade = 0;
    std::vector<uint> lightMapping;
    mPrevLightType.clear();
    lightMapping.reserve(lights.size());
    mPrevLightType.reserve(lights.size());

    for (ref<Light> light : lights)
    {
        ref<Texture> tex;
        auto lightType = getLightType(light);
        mPrevLightType.push_back(lightType);

        if (lightType & LightTypeSM::Point)
        {
            // Setup cube map tex
            auto format = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::R32Float : ResourceFormat::R16Unorm;

            auto bindFlags = ResourceBindFlags::ShaderResource;
            bindFlags |= ResourceBindFlags::RenderTarget;

            tex = Texture::createCube(mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, format, 1u, 1u, nullptr, bindFlags);
            tex->setName("ShadowMapCube" + std::to_string(countPoint));

            lightMapping.push_back(countPoint); // Push Back Point ID
            countPoint++;
            mpShadowMapsCube.push_back(tex);
        }
        else if (lightType & LightTypeSM::Spot)
        {
            tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, 1u, 1u, nullptr,
                ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource
            );
            tex->setName("ShadowMapMisc" + std::to_string(countMisc));

            lightMapping.push_back(countMisc); // Push Back Misc ID
            countMisc++;
            mpShadowMaps.push_back(tex);
        }
        else if (lightType & LightTypeSM::Directional)
        {
            tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, mCascadesLevelCount, 1u, nullptr,
                ResourceBindFlags::DepthStencil | ResourceBindFlags::ShaderResource
            );
            tex->setName("ShadowMapCascade" + std::to_string(countCascade));

            lightMapping.push_back(countCascade); // Push Back Cascade ID
            countCascade++;
            mpCascadedShadowMaps.push_back(tex);
        }
        else //Type not supported 
        {
            lightMapping.push_back(0); //Push back 0; Will be ignored in shader anyway
        }
    }

    // Light Mapping Buffer
    if (!mpLightMapping && lightMapping.size() > 0)
    {
        mpLightMapping = Buffer::createStructured(
            mpDevice, sizeof(uint), lightMapping.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, lightMapping.data(),
            false
        );
        mpLightMapping->setName("ShadowMapLightMapping");
    }

    if (!mpVPMatrixStangingBuffer && (mpShadowMaps.size() > 0 || mpCascadedShadowMaps.size() > 0))
    {
        size_t size = mpShadowMaps.size() + mpCascadedShadowMaps.size() * mCascadesLevelCount;
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
            mpDevice, sizeof(float4x4), mpShadowMaps.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false
        );
        mpVPMatrixBuffer->setName("ShadowMapViewProjectionBuffer");
    }

    mSpotDirViewProjMat.resize(mpShadowMaps.size());
    for (auto& vpMat : mSpotDirViewProjMat)
        vpMat = float4x4();

    mResetShadowMapBuffers = false;
    mShadowResChanged = false;
    mFirstFrame = true;
}

void ShadowMap::prepareRasterProgramms()
{
    mShadowCubePass.reset();
    mShadowMiscPass.reset();

    auto defines = getDefinesShadowMapGenPass();
    // Create Shadow Cube create rasterizer Program.
    {
        mShadowCubePass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDepthPassProgramFile).vsEntry("vsMain").psEntry("psMainCube");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowCubePass.pProgram = GraphicsProgram::create(mpDevice, desc, defines);
        mShadowCubePass.pState->setProgram(mShadowCubePass.pProgram);
    }
    // Create Shadow Map 2D create Program
    {
        mShadowMiscPass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kDepthPassProgramFile).vsEntry("vsMain").psEntry("psMain");
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowMiscPass.pProgram = GraphicsProgram::create(mpDevice, desc, defines);
        mShadowMiscPass.pState->setProgram(mShadowMiscPass.pProgram);
    }
}

void ShadowMap::prepareProgramms()
{
    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
    prepareRasterProgramms();
    auto definesPB = getDefines();
    definesPB.add("SAMPLE_GENERATOR_TYPE", "0");
    // Create dummy Compute pass for Parameter block
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addTypeConformances(globalTypeConformances);
        desc.setShaderModel(kShaderModel);
        desc.addShaderLibrary(kReflectTypesFile).csEntry("main");
        mpReflectTypes = ComputePass::create(mpDevice, desc, definesPB, false);

        mpReflectTypes->getProgram()->setDefines(definesPB);
        mpReflectTypes->setVars(nullptr);
    }
    // Create ParameterBlock
    {
        auto reflector = mpReflectTypes->getProgram()->getReflector()->getParameterBlock("gShadowMap");
        mpShadowMapParameterBlock = ParameterBlock::create(mpDevice, reflector);
        FALCOR_ASSERT(mpShadowMapParameterBlock);

        setShaderData();
    }
}

DefineList ShadowMap::getDefines() const
{
    DefineList defines;

    uint countShadowMapsCube = std::max(1u, getCountShadowMapsCube());
    uint countShadowMapsMisc = std::max(1u, getCountShadowMaps());
    bool multipleSMTypes = getCountShadowMapsCube() > 0 && getCountShadowMaps() > 0;

    defines.add("MULTIPLE_SHADOW_MAP_TYPES", multipleSMTypes ? "1" : "0");
    defines.add("NUM_SHADOW_MAPS_CUBE", std::to_string(countShadowMapsCube));
    defines.add("NUM_SHADOW_MAPS_MISC", std::to_string(countShadowMapsMisc));
    defines.add("SM_USE_PCF", mUsePCF ? "1" : "0");
    defines.add("SM_USE_POISSON_SAMPLING", mUsePoissonDisc ? "1" : "0");

    if (mpScene)
        defines.add(mpScene->getSceneDefines());

    return defines;
}

DefineList ShadowMap::getDefinesShadowMapGenPass() const
{
    DefineList defines;
    defines.add("USE_ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    if (mpScene)
        defines.add(mpScene->getSceneDefines());

    return defines;
}

void ShadowMap::setShaderData(const uint2 frameDim)
{
    FALCOR_ASSERT(mpShadowMapParameterBlock);

    auto var = mpShadowMapParameterBlock->getRootVar();

    auto& cameraData = mpScene->getCamera()->getData();

    // Parameters
    var["gShadowMapFarPlane"] = mFar;
    var["gDirectionalOffset"] = mDirLightPosOffset;
    var["gShadowMapRes"] = mShadowMapSize;
    var["gSceneCenter"] = mSceneCenter;
    var["gSMCubePixelSize"] = mSMCubePixelSize;
    var["gSMPixelSize"] = mSMPixelSize;
    var["gCamerPixelSize"] = getNormalizedPixelSize(frameDim, focalLengthToFovY(cameraData.focalLength, cameraData.frameHeight)  ,cameraData.aspectRatio);
    var["gPoissonDiscRad"] = gPoissonDiscRad;


    // Buffers and Textures
    for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
    {
        var["gShadowMapCube"][i] = mpShadowMapsCube[i]; // Can be Nullptr
    }
    for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
    {
        var["gShadowMap"][i] = mpShadowMaps[i]; // Can be Nullptr
    }

    var["gShadowMapVPBuffer"] = mpVPMatrixBuffer; // Can be Nullptr
    var["gShadowMapIndexMap"] = mpLightMapping;   // Can be Nullptr
    var["gShadowSampler"] = mpShadowSampler;
}

void ShadowMap::setShaderDataAndBindBlock(ShaderVar rootVar, const uint2 frameDim)
{
    setShaderData(frameDim);
    rootVar["gShadowMap"] = getParameterBlock();
}

void ShadowMap::setProjection(float near, float far)
{
    if (near > 0)
        mNear = near;
    if (far > 0)
        mFar = far;

    mProjectionMatrix = math::perspective(float(M_PI_2), 1.f, mNear, mFar);
    auto& sceneBounds = mpScene->getSceneBounds();
    mDirLightPosOffset = sceneBounds.radius();
    mSceneCenter = sceneBounds.center();
    mOrthoMatrix = math::ortho(
        -sceneBounds.radius(), sceneBounds.radius(), -sceneBounds.radius(), sceneBounds.radius(), near, sceneBounds.radius() * 2
    );

    //Set normalized pixel sizes
    mSMCubePixelSize = getNormalizedPixelSize(uint2(mShadowMapSizeCube), float(M_PI_2), 1.f);
    mSMPixelSize = getNormalizedPixelSize(uint2(mShadowMapSize), float(M_PI_2), 1.f);
}

void ShadowMap::updateRasterizerStates() {
    mFrontClockwiseRS[RasterizerState::CullMode::None] = RasterizerState::create(RasterizerState::Desc()
                                                                                     .setFrontCounterCW(false)
                                                                                     .setDepthBias(mBias, mSlopeBias)
                                                                                     .setDepthClamp(true)
                                                                                     .setCullMode(RasterizerState::CullMode::None));
    mFrontClockwiseRS[RasterizerState::CullMode::Back] = RasterizerState::create(RasterizerState::Desc()
                                                                                     .setFrontCounterCW(false)
                                                                                     .setDepthBias(mBias, mSlopeBias)
                                                                                     .setDepthClamp(true)
                                                                                     .setCullMode(RasterizerState::CullMode::Back));
    mFrontClockwiseRS[RasterizerState::CullMode::Front] = RasterizerState::create(RasterizerState::Desc()
                                                                                      .setFrontCounterCW(false)
                                                                                      .setDepthBias(mBias, mSlopeBias)
                                                                                      .setDepthClamp(true)
                                                                                      .setCullMode(RasterizerState::CullMode::Front));
    mFrontCounterClockwiseRS[RasterizerState::CullMode::None] = RasterizerState::create(RasterizerState::Desc()
                                                                                      .setFrontCounterCW(true)
                                                                                      .setDepthBias(mBias, mSlopeBias)
                                                                                      .setDepthClamp(true)
                                                                                      .setCullMode(RasterizerState::CullMode::None));
    mFrontCounterClockwiseRS[RasterizerState::CullMode::Back] = RasterizerState::create(RasterizerState::Desc()
                                                                                      .setFrontCounterCW(true)
                                                                                      .setDepthBias(mBias, mSlopeBias)
                                                                                      .setDepthClamp(true)
                                                                                      .setCullMode(RasterizerState::CullMode::Back));
    mFrontCounterClockwiseRS[RasterizerState::CullMode::Front] = RasterizerState::create(RasterizerState::Desc()
                                                                                      .setFrontCounterCW(true)
                                                                                      .setDepthBias(mBias, mSlopeBias)
                                                                                      .setDepthClamp(true)
                                                                                      .setCullMode(RasterizerState::CullMode::Front));
}

ShadowMap::LightTypeSM ShadowMap::getLightType(const ref<Light> light)
{
    const LightType& type = light->getType();
    if (type == LightType::Directional)
        return LightTypeSM::Directional;
    else if (type == LightType::Point)
    {
        if (light->getData().openingAngle > M_PI_2)
            return LightTypeSM::Point;
        else
            return LightTypeSM::Spot;
    }

    return LightTypeSM::NotSupported;
}

bool ShadowMap::isPointLight(const ref<Light> light)
{
    return (light->getType() == LightType::Point) && (light->getData().openingAngle > M_PI_2);
}

void ShadowMap::setSMShaderVars(ShaderVar& var, ShaderParameters& params)
{
    var["CB"]["gviewProjection"] = params.viewProjectionMatrix;
    var["CB"]["gLightPos"] = params.lightPosition;
    var["CB"]["gFarPlane"] = params.farPlane;
}

void ShadowMap::renderCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext)
{
    // Rendering per face with an array depth buffer is seemingly bugged, therefore a helper depth buffer is needed
    if (!mpDepth)
    {
        mpDepth = Texture::create2D(
            mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, mShadowMapFormat, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil
        );
        mpDepth->setName("ShadowMapCubePassDepthHelper");
    }

    if (index == 0)
    {
        mShadowCubePass.pState->getProgram()->addDefine("USE_ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    }

    // Create Program Vars
    if (!mShadowCubePass.pVars)
    {
        mShadowCubePass.pVars = GraphicsVars::create(mpDevice, mShadowCubePass.pProgram.get());
    }

    auto changes = light->getChanges();
    bool renderLight = changes == Light::Changes::Active || changes == Light::Changes::Position || mFirstFrame;

    if (!renderLight || !light->isActive())
        return;

    auto& lightData = light->getData();

    ShaderParameters params;
    params.lightPosition = lightData.posW;
    params.farPlane = mFar;

    pRenderContext->clearRtv(mpShadowMapsCube[index]->getRTV(0, 0, 6).get(), float4(1.f));
    for (size_t face = 0; face < 6; face++)
    {
        // Clear depth buffer.
        pRenderContext->clearDsv(mpDepth->getDSV().get(), 1.f, 0);
        // Attach Render Targets
        mpFboCube->attachColorTarget(mpShadowMapsCube[index], 0, 0, face, 1);
        mpFboCube->attachDepthStencilTarget(mpDepth);

        params.viewProjectionMatrix = getProjViewForCubeFace(face, lightData);

        auto vars = mShadowCubePass.pVars->getRootVar();
        setSMShaderVars(vars, params);

        mShadowCubePass.pState->setFbo(mpFboCube);
        mpScene->rasterize(pRenderContext, mShadowCubePass.pState.get(), mShadowCubePass.pVars.get(), RasterizerState::CullMode::Front);
    }
}

float4x4 ShadowMap::getProjViewForCubeFace(uint face, const LightData& lightData, bool useOrtho)
{
    float3 lightTarget;
    float3 up;
    switch (face)
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

    return math::mul(mProjectionMatrix, viewMat);
}

bool ShadowMap::renderSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered) {

    auto changes = light->getChanges();
    bool renderLight = (changes == Light::Changes::Active) || (changes == Light::Changes::Position) ||
                       (changes == Light::Changes::Direction) || mFirstFrame;

    auto& lightData = light->getData();

    if (!renderLight || !light->isActive())
    {
        wasRendered[index] = false;
        return false;
    }

    wasRendered[index] = true;

    // Clear depth buffer.
    pRenderContext->clearDsv(mpShadowMaps[index]->getDSV().get(), 1.f, 0);

    // Attach Render Targets
    mpFbo->attachDepthStencilTarget(mpShadowMaps[index]);

    ShaderParameters params;
  
    float3 lightTarget = lightData.posW + lightData.dirW;
    float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, float3(0, 1, 0));

    params.lightPosition = lightData.posW;
    params.farPlane = mFar;
    params.viewProjectionMatrix = math::mul(mProjectionMatrix, viewMat);     
  
    mSpotDirViewProjMat[index] = params.viewProjectionMatrix;

    auto vars = mShadowMiscPass.pVars->getRootVar();
    setSMShaderVars(vars, params);

    mShadowMiscPass.pState->setFbo(mpFbo);
    mpScene->rasterize(
        pRenderContext, mShadowMiscPass.pState.get(), mShadowMiscPass.pVars.get(), mFrontClockwiseRS[mCullMode],
        mFrontCounterClockwiseRS[mCullMode], mFrontCounterClockwiseRS[RasterizerState::CullMode::None]
    );
    return true;
}

void ShadowMap::renderCascaded(uint index, ref<Light> light, RenderContext* pRenderContext) {
    //TODO
    //Matrix Calc see https://learnopengl.com/Guest-Articles/2021/CSM
    //Or https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps
}

bool ShadowMap::update(RenderContext* pRenderContext)
{
    // Return if there is no scene
    if (!mpScene)
        return false;

    // Return if there is no active light
    if (mpScene->getActiveLightCount() == 0)
        return true;

    if (mAlwaysRenderSM)
        mFirstFrame = true;

    if (mBiasSettingsChanged)
    {
        updateRasterizerStates();   //DepthBias is set here
        mFirstFrame = true; //Re render all SM
        mBiasSettingsChanged = false;
    }
        

    // Rebuild the Shadow Maps
    if (mResetShadowMapBuffers || mShadowResChanged)
    {
        prepareShadowMapBuffers();
        setProjection(mNear, mFar);
    }
        

    if (mRasterDefinesChanged)
    {
        mFirstFrame = true;
        prepareRasterProgramms();
        mRasterDefinesChanged = false;
    }

    // Loop over all lights
    const std::vector<ref<Light>>& lights = mpScene->getLights();

    // Create Render List
    std::vector<ref<Light>> lightRenderListCube; // Light List for cube render process
    std::vector<ref<Light>> lightRenderListMisc; // Light List for 2D texture shadow maps
    std::vector<ref<Light>> lightRenderListCascaded;    //Light List for the cascaded lights (directional)
    for (size_t i = 0; i < lights.size(); i++)
    {
        ref<Light> light = lights[i];
        LightTypeSM type = getLightType(light);
        //bool isPoint = isPointLight(light);

        // Check if the type has changed and end the pass if that is the case
        if (type != mPrevLightType[i])
        {
            mResetShadowMapBuffers = true;
            return false;
        }

        switch (type)
        {
        case LightTypeSM::Directional:
            lightRenderListCascaded.push_back(light);
            break;
        case LightTypeSM::Point:
            lightRenderListCube.push_back(light);
            break;
        case LightTypeSM::Spot:
            lightRenderListMisc.push_back(light);
            break;
        default:
            break;
        }
    }

    // Render all cube lights
    for (size_t i = 0; i < lightRenderListCube.size(); i++)
    {
        renderCubeEachFace(i, lightRenderListCube[i], pRenderContext);
    }

    //Spot/Directional Lights

    mShadowMiscPass.pState->getProgram()->addDefines(getDefinesShadowMapGenPass()); // Update defines
    // Create Program Vars
    if (!mShadowMiscPass.pVars)
    {
        mShadowMiscPass.pVars = GraphicsVars::create(mpDevice, mShadowMiscPass.pProgram.get());
    }

    std::vector<bool> wasRendered(lightRenderListMisc.size());
    bool updateVPBuffer = false;
    // Render all spot / directional lights
    for (size_t i = 0; i < lightRenderListMisc.size(); i++)
    {
        updateVPBuffer |= renderSpotLight(i, lightRenderListMisc[i], pRenderContext, wasRendered);
    }

    //Render cascaded
    updateVPBuffer |= lightRenderListCascaded.size() > 0;
    for (size_t i = 0; i < lightRenderListCascaded.size(); i++)
    {
        renderCascaded(i, lightRenderListCascaded[i], pRenderContext);
    }

    // Write all ViewProjectionMatrix to the buffer
    // TODO optimize this depending on the number of active lights
    // TODO Add cascaded
    if (updateVPBuffer)
    {
        float4x4* mats = (float4x4*)mpVPMatrixStangingBuffer->map(Buffer::MapType::Write);
        for (size_t i = 0; i < mSpotDirViewProjMat.size(); i++)
        {
            if (!wasRendered[i])
                continue;
            mats[i] = mSpotDirViewProjMat[i];
        }
        mpVPMatrixStangingBuffer->unmap();

        pRenderContext->copyBufferRegion(
            mpVPMatrixBuffer.get(), 0, mpVPMatrixStangingBuffer.get(), 0, sizeof(float4x4) * mSpotDirViewProjMat.size()
        );
    }

    mFirstFrame = false;
    return true;
}

void ShadowMap::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Render every Frame", mAlwaysRenderSM);
    widget.tooltip("Renders all shadow maps every frame");

    // Near Far option
    static float2 nearFar = float2(mNear, mFar);
    widget.var("Near/Far", nearFar, 0.0f, 100000.f, 0.001f);
    widget.tooltip("Changes the Near/Far values used for Point and Spotlights");
    if (nearFar.x != mNear || nearFar.y != mFar)
    {
        mNear = nearFar.x;
        mFar = nearFar.y;
        setProjection(mNear, mFar);
        mFirstFrame = true; // Rerender all shadow maps
    }

    static uint2 resolution = uint2(mShadowMapSize, mShadowMapSizeCube);
    widget.var("Shadow Map / Cube Res", resolution, 32u, 16384u, 32u);
    widget.tooltip("Change Resolution for the Shadow Map (x) or Shadow Cube Map (y). Rebuilds all buffers!");
    if (widget.button("Apply Change"))
    {
        mShadowMapSize = resolution.x;
        mShadowMapSizeCube = resolution.y;
        mShadowResChanged = true;
    }

    if (widget.dropdown("Cull Mode", kShadowMapCullMode, (uint32_t&)mCullMode))
        mFirstFrame = true; // Render all shadow maps again

    mBiasSettingsChanged |= widget.var("Shadow Map Bias", mBias, 0, 256, 1);
    mBiasSettingsChanged |= widget.var("Shadow Slope Bias", mSlopeBias, 0.f, 50.f, 0.001f);

    mRasterDefinesChanged |= widget.checkbox("Alpha Test", mUseAlphaTest);
    widget.checkbox("Use PCF", mUsePCF);
    widget.tooltip("Enable to use Percentage closer filtering");
    widget.checkbox("Use Poisson Disc Sampling", mUsePoissonDisc);
    widget.tooltip("Use Poisson Disc Sampling, only enabled if rng of the eval function is filled");
    widget.var("Poisson Disc Rad", gPoissonDiscRad, 0.f, 50.f, 0.001f);
        
}

// Gets the pixel size at distance 1. Assumes every pixel has the same size.
float ShadowMap::getNormalizedPixelSize(uint2 frameDim, float fovY, float aspect)
{
    float h = tan(fovY / 2.f) * 2.f;
    float w = h * aspect;
    float wPix = w / frameDim.x;
    float hPix = h / frameDim.y;
    return wPix * hPix;
}

}
