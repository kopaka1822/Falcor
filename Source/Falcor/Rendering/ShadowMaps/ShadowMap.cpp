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

#include "Utils/SampleGenerators/DxSamplePattern.h"
#include "Utils/SampleGenerators/HaltonSamplePattern.h"
#include "Utils/SampleGenerators/StratifiedSamplePattern.h"

namespace Falcor
{
namespace
{
const std::string kShadowGenRasterShader = "Rendering/ShadowMaps/GenerateShadowMap.3d.slang";
const std::string kReflectTypesFile = "Rendering/ShadowMaps/ReflectTypesForParameterBlock.cs.slang";
const std::string kShaderModel = "6_5";
const uint kRayPayloadMaxSize = 4u;

const Gui::DropdownList kShadowMapCullMode{
    {(uint)RasterizerState::CullMode::None, "None"},
    {(uint)RasterizerState::CullMode::Front, "Front"},
    {(uint)RasterizerState::CullMode::Back, "Back"},
};

const Gui::DropdownList kShadowMapRasterAlphaModeDropdown{
    {1, "Basic"},
    {2, "HashedIsotropic"},
    {3, "HashedAnisotropic"}
};

const Gui::DropdownList kShadowMapUpdateModeDropdownList{
    {(uint)ShadowMap::SMUpdateMode::Static, "Static"},
    {(uint)ShadowMap::SMUpdateMode::Dynamic, "Dynamic"},
};

const Gui::DropdownList kCascadedFrustumModeList{
    {(uint)ShadowMap::CascadedFrustumMode::Manual, "Manual"},
    {(uint)ShadowMap::CascadedFrustumMode::AutomaticNvidia, "AutomaticNvidia"},
};

const Gui::DropdownList kCascadedModeForEndOfLevels{
    {0u, "Shadow Map"},
    {1u, "Ray Shadow"},
};
} // namespace

ShadowMap::ShadowMap(ref<Device> device, ref<Scene> scene, bool enableOracle) : mpDevice{device}, mpScene{scene}
{
    FALCOR_ASSERT(mpScene);

    // Create FBO
    mpFbo = Fbo::create(mpDevice);
    mpFboCube = Fbo::create(mpDevice);
    mpFboCascaded = Fbo::create(mpDevice);

    //Create Oracle Pointer
    if (enableOracle)
        mpOracle = std::make_unique<ShadowMapOracle>(mpDevice);

    // Update all shadow maps every frame
    if (mpScene->hasDynamicGeometry())
    {
        mSceneIsDynamic = true;
        mShadowMapUpdateMode = SMUpdateMode::Dynamic;
    }  

    AABB sceneBounds = mpScene->getSceneBounds();
    float far = length(sceneBounds.extent());
    float near = 0.0005 * far;
    //Set camera near and far plane to usable values
    for (auto& camera : mpScene->getCameras())
    {
        camera->setNearPlane(std::max(camera->getNearPlane(), near));
        camera->setFarPlane(std::min(camera->getFarPlane(), far));
    }

    // Create Light Mapping Buffer
    prepareShadowMapBuffers();

    prepareProgramms();

    // Create samplers.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpShadowSamplerPoint = Sampler::create(mpDevice, samplerDesc);
    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    mpShadowSamplerLinear = Sampler::create(mpDevice, samplerDesc);

    //Init Fence values
    for (auto& waitVal : mpVPMatrixBuffer.stagingFenceWaitValues)
        waitVal = 0;
    for (auto& waitVal : mpCascadedVPMatrixBuffer.stagingFenceWaitValues)
        waitVal = 0;

    // Set RasterizerStateDescription
    updateRasterizerStates();

    mUpdateShadowMap = true;
}

void ShadowMap::prepareShadowMapBuffers()
{
    // Reset existing shadow maps
    if (mShadowResChanged || mResetShadowMapBuffers)
    {
        //Shadow Maps
        mpShadowMaps.clear();
        mpShadowMapsCube.clear();
        mpCascadedShadowMaps.reset();

        //Depth Buffers
        mpDepthCascaded.reset();
        mpDepthCube.reset();
        mpDepth.reset();

        //Static copys for animations
        mpShadowMapsCubeStatic.clear();
        mpDepthCubeStatic.clear();

        //Misc
        if (mpOracle)
            mpOracle->resetBuffers();
    }

    // Lighting Changed
    if (mResetShadowMapBuffers)
    {
        mpLightMapping.reset();
        mpVPMatrixBuffer.reset();
        mpCascadedVPMatrixBuffer.reset();
    }

    //Initialize the Shadow Map Textures
    const std::vector<ref<Light>>& lights = mpScene->getLights();
    
    uint countPoint = 0;
    mCountSpotShadowMaps = 0;
    uint countCascade = 0;

    std::vector<uint> lightMapping;
    mPrevLightType.clear();
    
    lightMapping.reserve(lights.size());
    mPrevLightType.reserve(lights.size());

    //Determine Shadow Map format and bind flags (can both change for cube case)
    ResourceFormat shadowMapFormat;
    ResourceBindFlags shadowMapBindFlags = ResourceBindFlags::ShaderResource;
    bool generateAdditionalDepthTextures = false;
    bool genMipMaps = false;
    switch (mShadowMapType)
    {
    case ShadowMapType::Variance:
    {
        shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::RG32Float : ResourceFormat::RG16Unorm;
        shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        generateAdditionalDepthTextures = true;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::Exponential:
    {
        shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::R32Float : ResourceFormat::R16Float;
        shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        generateAdditionalDepthTextures = true;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::ExponentialVariance:
    case ShadowMapType::MSMHamburger:
    case ShadowMapType::MSMHausdorff:
    {
        shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::RGBA32Float : ResourceFormat::RGBA16Float;
        shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        generateAdditionalDepthTextures = true;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::ShadowMap: // No special format needed
    case ShadowMapType::SDVariance:
    case ShadowMapType::SDExponentialVariance:
    case ShadowMapType::SDMSM:
    {
        shadowMapFormat = mShadowMapFormat;
        shadowMapBindFlags |= ResourceBindFlags::DepthStencil; 
    }
    }

    //
    //Create the Shadow Map Texture for every light
    //

    for (ref<Light> light : lights)
    {
        ref<Texture> tex;
        auto lightType = getLightType(light);
        mPrevLightType.push_back(lightType);

        if (lightType == LightTypeSM::Point)
        {
            // Setup cube map tex
            ResourceFormat shadowMapCubeFormat;
            switch (shadowMapFormat)
            {
            case ResourceFormat::D32Float:
            {
                shadowMapCubeFormat = ResourceFormat::R32Float;
                break;
            }
            case ResourceFormat::D16Unorm:
            {
                shadowMapCubeFormat = ResourceFormat::R16Unorm;
                break;
            }
            default:
            {
                shadowMapCubeFormat = shadowMapFormat;
            }
            }

            auto cubeBindFlags = ResourceBindFlags::ShaderResource | ResourceBindFlags::RenderTarget;
            if (mShadowMapType != ShadowMapType::ShadowMap)
                cubeBindFlags |= ResourceBindFlags::UnorderedAccess;

            //TODO fix mips
            tex = Texture::createCube(
                mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, shadowMapCubeFormat, 1u, 1u, nullptr,
                cubeBindFlags
            );
            tex->setName("ShadowMapCube" + std::to_string(countPoint));

            lightMapping.push_back(countPoint); // Push Back Point ID
            countPoint++;
            mpShadowMapsCube.push_back(tex);
        }
        else if (lightType == LightTypeSM::Spot)
        {
            lightMapping.push_back(mCountSpotShadowMaps); // Push Back Spot ID
            mCountSpotShadowMaps++;
        }
        else if (lightType == LightTypeSM::Directional)
        {
            lightMapping.push_back(0); // There is only one cascade so ID does not matter
            countCascade++;
        }
        else //Type not supported 
        {
            lightMapping.push_back(0); //Push back 0; Will be ignored in shader anyway
        }
    }

    //Create Textures for Spot lights
    uint loopCount = mSceneIsDynamic ? mCountSpotShadowMaps * 2 : mCountSpotShadowMaps;
    for (uint i = 0; i < loopCount; i++)
    {
        ref<Texture> tex = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, shadowMapFormat, 1u,
            (genMipMaps && i < mCountSpotShadowMaps) ? Texture::kMaxPossible : 1u,
            nullptr,
            shadowMapBindFlags
        );
        if (i >= mCountSpotShadowMaps)
            tex->setName("ShadowMapSpotDyn" + std::to_string(i - mCountSpotShadowMaps));
        else
            tex->setName("ShadowMapSpot" + std::to_string(i));

        mpShadowMaps.push_back(tex);
    }

    //Create Textures for cascade
    FALCOR_ASSERT(countCascade <= 1);
    if (countCascade > 0)
    {
        uint levelCount = mSceneIsDynamic ? mCascadedLevelCount * 2 : mCascadedLevelCount;
        mpCascadedShadowMaps = Texture::create2D(
            mpDevice, mShadowMapSizeCascaded, mShadowMapSizeCascaded, shadowMapFormat, levelCount, genMipMaps ? Texture::kMaxPossible : 1u,
            nullptr, shadowMapBindFlags
        );
        mpCascadedShadowMaps->setName("ShadowMapCascade");
    }

    //
    //Create additional Depth Textures (Filterable Shadow Maps)
    //
    if (!mpDepthCascaded && countCascade > 0 && generateAdditionalDepthTextures)
    {
        mpDepthCascaded = Texture::create2D(
            mpDevice, mShadowMapSizeCascaded, mShadowMapSizeCascaded, mShadowMapFormat, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil
        );
        mpDepthCascaded->setName("ShadowMapCascadedPassDepthHelper");
    }
    if (!mpDepthCube && countPoint > 0)
    {
        mpDepthCube = Texture::create2D(
            mpDevice, mShadowMapSizeCube, mShadowMapSizeCube, mShadowMapFormat, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil
        );
        mpDepthCube->setName("ShadowMapCubePassDepthHelper");
    }
    if (!mpDepth && mCountSpotShadowMaps > 0 && generateAdditionalDepthTextures)
    {
        mpDepth = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil
        );
        mpDepth->setName("ShadowMap2DPassDepthHelper");
    }

    //
    //Create Textures for scenes with dynamic geometry TODO move
    //
    if (mSceneIsDynamic)
    {
        for (size_t i = 0; i < mpShadowMapsCube.size(); i++)
        {
            //Create a copy Texture
            ref<Texture> tex = Texture::createCube(
                mpDevice, mpShadowMapsCube[i]->getWidth(), mpShadowMapsCube[i]->getHeight(), mpShadowMapsCube[i]->getFormat(), 1u,
                1u , nullptr, mpShadowMapsCube[i]->getBindFlags()
            );
            tex->setName("ShadowMapCubeStatic" + std::to_string(i));
            mpShadowMapsCubeStatic.push_back(tex);

            //Create a per face depth texture
            if (mpDepthCube)
            {
                for (uint face = 0; face < 6; face++)
                {
                    ref<Texture> depthTex = Texture::create2D(
                        mpDevice, mpDepthCube->getWidth(), mpDepthCube->getHeight(), mpDepthCube->getFormat(), 1u, 1u, nullptr,
                        mpDepthCube->getBindFlags()
                    );
                    depthTex->setName("ShadowMapCubePassDepthHelperStatic" + std::to_string(i) + "Face" + std::to_string(face));
                    mpDepthCubeStatic.push_back(depthTex);
                }
            }
        }
    }

    //
    //Create Frustum Culling Objects
    //
    if (mUseFrustumCulling)
    {
        //Calculate total number of Culling Objects needed
        mFrustumCullingVectorOffsets = uint2(mCountSpotShadowMaps, mCountSpotShadowMaps + mCascadedLevelCount);
        uint frustumCullingVectorSize = mCountSpotShadowMaps + mCascadedLevelCount + countPoint * 6;
        mFrustumCulling.resize(frustumCullingVectorSize);
        for (size_t i = 0; i < frustumCullingVectorSize; i++)
            mFrustumCulling[i] = make_ref<FrustumCulling>();
    }

    //
    // Light Mapping
    //

    // Check if multiple SM types are used
    LightTypeSM checkType = LightTypeSM::NotSupported;
    for (size_t i = 0; i < mPrevLightType.size(); i++)
    {
        if (i == 0)
            checkType = mPrevLightType[i];
        else if (checkType != mPrevLightType[i])
        {
            mMultipleSMTypes = true;
            break;
        }
    }

    //Create Light Mapping Buffer
    if (!mpLightMapping && lightMapping.size() > 0)
    {
        mpLightMapping = Buffer::createStructured(
            mpDevice, sizeof(uint), lightMapping.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, lightMapping.data(),
            false
        );
        mpLightMapping->setName("ShadowMapLightMapping");
    }

    //Create VP Matrices
    if ((!mpVPMatrixBuffer.buffer) && (mpShadowMaps.size() > 0))
    {
        size_t size = mpShadowMaps.size();
        std::vector<float4x4> initData(size * kStagingBufferCount);
        for (size_t i = 0; i < initData.size(); i++)
            initData[i] = float4x4::identity();

         mpVPMatrixBuffer.buffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size, ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false
        );
        mpVPMatrixBuffer.buffer->setName("ShadowMap_VP");

        mpVPMatrixBuffer.staging = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size * kStagingBufferCount, ResourceBindFlags::ShaderResource,
            Buffer::CpuAccess::Write, initData.data(), false
        );
        mpVPMatrixBuffer.staging->setName("ShadowMap_VPStaging");
    }

    if ((!mpCascadedVPMatrixBuffer.buffer) && (mpCascadedShadowMaps))
    {
        size_t size = mCascadedLevelCount;
        std::vector<float4x4> initData(size * kStagingBufferCount);
        for (size_t i = 0; i < initData.size(); i++)
            initData[i] = float4x4::identity();

        mpCascadedVPMatrixBuffer.buffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size, ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false
        );
        mpCascadedVPMatrixBuffer.buffer->setName("SMCascaded_VP");

        mpCascadedVPMatrixBuffer.staging = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size * kStagingBufferCount, ResourceBindFlags::ShaderResource, Buffer::CpuAccess::Write,
            initData.data(), false
        );
        mpCascadedVPMatrixBuffer.staging->setName("SMCascaded_VPStaging");
    }

    mCascadedVPMatrix.resize(mCascadedLevelCount);
    mCascadedWidthHeight.resize(mCascadedLevelCount); //For Normalized Pixel Size
    mSpotDirViewProjMat.resize(mpShadowMaps.size());
    for (auto& vpMat : mSpotDirViewProjMat)
        vpMat = float4x4();

    mResetShadowMapBuffers = false;
    mShadowResChanged = false;
    mUpdateShadowMap = true;
}

void ShadowMap::prepareRasterProgramms()
{
    mShadowCubeRasterPass.reset();
    mShadowMapRasterPass.reset();
    mShadowMapCascadedRasterPass.reset();

    auto defines = getDefinesShadowMapGenPass();
    // Create Shadow Cube create rasterizer Program.
    {
        mShadowCubeRasterPass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());

        // Load in the Shaders depending on the Type
        switch (mShadowMapType)
        {
        case ShadowMapType::ShadowMap:
        case ShadowMapType::SDVariance:
        case ShadowMapType::SDExponentialVariance:
        case ShadowMapType::SDMSM:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMainCube");
            break;
        case ShadowMapType::Variance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psVarianceCube");
            break;
        case ShadowMapType::Exponential:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponentialCube");
            break;
        case ShadowMapType::ExponentialVariance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponentialVarianceCube");
            break;
        case ShadowMapType::MSMHamburger:
        case ShadowMapType::MSMHausdorff:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMSMCube");
            break;
        }

        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowCubeRasterPass.pProgram = GraphicsProgram::create(mpDevice, desc, defines);
        mShadowCubeRasterPass.pState->setProgram(mShadowCubeRasterPass.pProgram);
    }
    // Create Shadow Map 2D create Program
    {
        mShadowMapRasterPass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());

        //Load in the Shaders depending on the Type
        switch (mShadowMapType)
        {
        case ShadowMapType::ShadowMap:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMain");
            break;
        case ShadowMapType::SDVariance:
        case ShadowMapType::SDExponentialVariance:
        case ShadowMapType::SDMSM:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMainLinearDepth");
            break;
        case ShadowMapType::Variance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psVariance");
            break;
        case ShadowMapType::Exponential:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponential");
            break;
        case ShadowMapType::ExponentialVariance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponentialVariance");
            break;
        case ShadowMapType::MSMHamburger:
        case ShadowMapType::MSMHausdorff:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMSM");
            break;
        }
        
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowMapRasterPass.pProgram = GraphicsProgram::create(mpDevice, desc, defines);
        mShadowMapRasterPass.pState->setProgram(mShadowMapRasterPass.pProgram);
    }
    // Create Shadow Map 2D create Program
    {
        mShadowMapCascadedRasterPass.pState = GraphicsState::create(mpDevice);
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());

        // Load in the Shaders depending on the Type
        switch (mShadowMapType)
        {
        case ShadowMapType::ShadowMap:
        case ShadowMapType::SDVariance:
        case ShadowMapType::SDExponentialVariance:
        case ShadowMapType::SDMSM:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMain");
            break;
        case ShadowMapType::Variance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psVarianceCascaded");
            break;
        case ShadowMapType::Exponential:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponentialCascaded");
            break;
        case ShadowMapType::ExponentialVariance:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psExponentialVarianceCascaded");
            break;
        case ShadowMapType::MSMHamburger:
        case ShadowMapType::MSMHausdorff:
            desc.addShaderLibrary(kShadowGenRasterShader).vsEntry("vsMain").psEntry("psMSMCascaded");
            break;
        }

        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setShaderModel(kShaderModel);

        mShadowMapCascadedRasterPass.pProgram = GraphicsProgram::create(mpDevice, desc, defines);
        mShadowMapCascadedRasterPass.pState->setProgram(mShadowMapCascadedRasterPass.pProgram);
    }
}

void ShadowMap::prepareProgramms()
{
    mpShadowMapParameterBlock.reset();

    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
    prepareRasterProgramms();
    auto definesPB = getDefines();
    definesPB.add("SAMPLE_GENERATOR_TYPE", "0");
    if (mpOracle)
        definesPB.add("ORACLE_VALID");
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
        if (mpOracle)
            mpOracle->createParameterBlock(mpReflectTypes);


        setShaderData();
    }

    mpReflectTypes.reset();
}

void ShadowMap::prepareGaussianBlur() {
    bool blurChanged = false;

    bool filterableShadowMapType = true;
    filterableShadowMapType &= mShadowMapType != ShadowMapType::ShadowMap;
    filterableShadowMapType &= mShadowMapType != ShadowMapType::SDVariance;
    filterableShadowMapType &= mShadowMapType != ShadowMapType::SDExponentialVariance;
    filterableShadowMapType &= mShadowMapType != ShadowMapType::SDMSM;

    if (mUseGaussianBlur && filterableShadowMapType)
    {
        if (!mpBlurShadowMap && mpShadowMaps.size() > 0)
        {
            mpBlurShadowMap = std::make_unique<SMGaussianBlur>(mpDevice);
            blurChanged = true;
        }
        if (!mpBlurCascaded && mpCascadedShadowMaps)
        {
            mpBlurCascaded = std::make_unique<SMGaussianBlur>(mpDevice);
            blurChanged = true;
        }
        if (!mpBlurCube && mpShadowMapsCube.size() > 0)
        {
            mpBlurCube = std::make_unique<SMGaussianBlur>(mpDevice, true);
            blurChanged = true;
        }
    }
    else //Destroy the blur passes that are currently active
    {
        if (mpBlurShadowMap)
        {
            mpBlurShadowMap.reset();
            blurChanged = true;
        }
        if (mpBlurCascaded)
        {
            mpBlurCascaded.reset();
            blurChanged = true;
        }
        if (mpBlurCube)
        {
            mpBlurCube.reset();
            blurChanged = true;
        }
    }

    mUpdateShadowMap |= blurChanged; //Rerender if blur settings changed
}

DefineList ShadowMap::getDefines() const
{
    DefineList defines;

    uint countShadowMapsCube = std::max(1u, getCountShadowMapsCube());
    uint countShadowMapsSpot = std::max(1u, getCountShadowMaps());

    uint cascadedSliceBufferSize = mCascadedLevelCount > 4 ? 8 : 4;

    defines.add("SHADOW_MAP_MODE", std::to_string((uint)mShadowMapType));
    defines.add("NUM_SHADOW_MAPS_CUBE", std::to_string(countShadowMapsCube));
    defines.add("BUFFER_SIZE_SPOT_SHADOW_MAPS", std::to_string(countShadowMapsSpot));
    defines.add("COUNT_SPOT_SM", std::to_string(mCountSpotShadowMaps));
    defines.add("MULTIPLE_SHADOW_MAP_TYPES", mMultipleSMTypes ? "1" : "0");
    defines.add("CASCADED_LEVEL", std::to_string(mCascadedLevelCount));
    defines.add("CASCADED_SLICE_BUFFER_SIZE", std::to_string(cascadedSliceBufferSize));
    defines.add("CASCADE_LEVEL_TRACE", std::to_string(mCascadedLevelTrace));
    defines.add("CASCADE_RAYTRACING_AFTER_HYBRID", mCascadedLastLevelRayTrace ? "1" : "0");
    defines.add("EVSM_EXTRA_TEST", mEVSMExtraTest ? "1" : "0");
    defines.add("SM_USE_PCF", mUsePCF ? "1" : "0");
    defines.add("SM_USE_POISSON_SAMPLING", mUsePoissonDisc ? "1" : "0");
    defines.add(
        "SM_EXPONENTIAL_CONSTANT",
        std::to_string(
            mShadowMapType == ShadowMapType::ExponentialVariance || mShadowMapType == ShadowMapType::SDExponentialVariance
                ? mEVSMConstant
                : mExponentialSMConstant
        )
    );
    defines.add("SM_NEGATIVE_EXPONENTIAL_CONSTANT", std::to_string(mEVSMNegConstant));
    defines.add("SM_NEAR", std::to_string(mNear));
    defines.add(
        "HYBRID_SMFILTERED_THRESHOLD",
        "float2(" + std::to_string(mHSMFilteredThreshold.x) + "," + std::to_string(mHSMFilteredThreshold.y) + ")"
    );
    defines.add("MSM_DEPTH_BIAS", std::to_string(mMSMDepthBias));
    defines.add("MSM_MOMENT_BIAS", std::to_string(mMSMMomentBias));
    defines.add("MSM_VARIANCE_TEST_THRESHOLD", mMSMUseVarianceTest ? std::to_string(mMSMVarianceThreshold) : "-1.0f");
    defines.add("CASC_USE_STOCH_LEVEL", mUseStochasticCascadedLevels ? "1" : "0");
    defines.add("CASC_STOCH_RANGE", std::to_string(mCascadedStochasticRange));
    defines.add("USE_RAY_OUTSIDE_SM", mUseRayOutsideOfShadowMap ? "1" : "0");
    defines.add("CASCADED_SM_RESOLUTION", std::to_string(mShadowMapSizeCascaded));
    defines.add("SM_RESOLUTION", std::to_string(mShadowMapSize));
    defines.add("CUBE_SM_RESOLUTION", std::to_string(mShadowMapSizeCube));
    defines.add("CUBE_WORLD_BIAS", std::to_string(mSMCubeWorldBias));
   
    defines.add("USE_SM_MIP", mUseShadowMipMaps ? "1" : "0");
    defines.add("SM_MIP_BIAS", std::to_string(mShadowMipBias));
    defines.add("USE_DYNAMIC_SM", mSceneIsDynamic ? "1" : "0");
    

    if (mpScene)
        defines.add(mpScene->getSceneDefines());

    if (mpOracle)
        defines.add(mpOracle->getDefines());

    return defines;
}

DefineList ShadowMap::getDefinesShadowMapGenPass(bool addAlphaModeDefines) const
{
    DefineList defines;
    defines.add("USE_ALPHA_TEST", mUseAlphaTest ? "1" : "0");
    defines.add("CASCADED_LEVEL", std::to_string(mCascadedLevelCount));
    defines.add(
        "SM_EXPONENTIAL_CONSTANT",
        std::to_string(mShadowMapType == ShadowMapType::ExponentialVariance ? mEVSMConstant : mExponentialSMConstant)
    );
    defines.add("SM_NEGATIVE_EXPONENTIAL_CONSTANT", std::to_string(mEVSMNegConstant));
    defines.add("SM_VARIANCE_SELFSHADOW", mVarianceUseSelfShadowVariant ? "1" : "0");
    if (addAlphaModeDefines)
        defines.add("_ALPHA_TEST_MODE", std::to_string(mAlphaMode)); 
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
    
    var["gPoissonDiscRad"] = mPoissonDiscRad;
    var["gPoissonDiscRadCube"] = mPoissonDiscRadCube;
    for (uint i = 0; i < mCascadedZSlices.size(); i++)
        var["gCascadedZSlices"][i] = mCascadedZSlices[i];
    
    // Buffers and Textures
    switch (mShadowMapType)
    {
    case Falcor::ShadowMapType::ShadowMap:
    case Falcor::ShadowMapType::SDVariance:
    case Falcor::ShadowMapType::SDExponentialVariance:
    case Falcor::ShadowMapType::SDMSM:
    case Falcor::ShadowMapType::Exponential:
        {
            for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
                var["gShadowMapCube"][i] = mpShadowMapsCube[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
                var["gShadowMap"][i] = mpShadowMaps[i]; // Can be Nullptr
            if (mpCascadedShadowMaps)
                var["gCascadedShadowMap"] = mpCascadedShadowMaps; // Can be Nullptr
        }
        break;
    case Falcor::ShadowMapType::Variance:
        {
            for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
                var["gShadowMapVarianceCube"][i] = mpShadowMapsCube[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
                var["gShadowMapVariance"][i] = mpShadowMaps[i]; // Can be Nullptr
            if (mpCascadedShadowMaps)
                var["gCascadedShadowMapVariance"] = mpCascadedShadowMaps; // Can be Nullptr
        }
        break;
    case Falcor::ShadowMapType::ExponentialVariance:
    case Falcor::ShadowMapType::MSMHamburger:
    case Falcor::ShadowMapType::MSMHausdorff:
        {
            for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
                var["gCubeShadowMapF4"][i] = mpShadowMapsCube[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
                var["gShadowMapF4"][i] = mpShadowMaps[i]; // Can be Nullptr
            if (mpCascadedShadowMaps)
                var["gCascadedShadowMapF4"] = mpCascadedShadowMaps; // Can be Nullptr
        }
        break;
    default:
        break;
    }
     
    var["gShadowMapVPBuffer"] = mpVPMatrixBuffer.buffer; // Can be Nullptr
    var["gSMCascadedVPBuffer"] = mpCascadedVPMatrixBuffer.buffer; // Can be Nullptr
    var["gShadowMapIndexMap"] = mpLightMapping;   // Can be Nullptr
    var["gShadowSamplerPoint"] = mpShadowSamplerPoint;
    var["gShadowSamplerLinear"] = mpShadowSamplerLinear;

    //Set Oracle Shader Data
    if (mpOracle)
        mpOracle->setShaderData(frameDim, cameraData);
}

void ShadowMap::setShaderDataAndBindBlock(ShaderVar rootVar, const uint2 frameDim)
{
    setShaderData(frameDim);
    rootVar["gShadowMap"] = getParameterBlock();
    if (mpOracle)
        mpOracle->bindShaderData(rootVar);
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

LightTypeSM ShadowMap::getLightType(const ref<Light> light)
{
    const LightType& type = light->getType();
    if (type == LightType::Directional)
        return LightTypeSM::Directional;
    else if (type == LightType::Point)
    {
        if (light->getData().openingAngle > M_PI_4)
            return LightTypeSM::Point;
        else
            return LightTypeSM::Spot;
    }

    return LightTypeSM::NotSupported;
}

void ShadowMap::setSMShaderVars(ShaderVar& var, ShaderParameters& params)
{
    var["CB"]["gviewProjection"] = params.viewProjectionMatrix;
    var["CB"]["gLightPos"] = params.lightPosition;
    var["CB"]["gDisableAlpha"] = params.disableAlpha;
    var["CB"]["gNearPlane"] = params.nearPlane;
    var["CB"]["gFarPlane"] = params.farPlane;
}

float4x4 ShadowMap::getProjViewForCubeFace(uint face,const LightData& lightData, const float4x4& projectionMatrix)
{
    float3 lightTarget, up;
    return getProjViewForCubeFace(face, lightData, projectionMatrix, lightTarget, up);
}

float4x4 ShadowMap::getProjViewForCubeFace(uint face, const LightData& lightData, const float4x4& projectionMatrix, float3& lightTarget, float3& up)
{
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

    return math::mul(projectionMatrix, viewMat);
}

void ShadowMap::rasterCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext)
{
    FALCOR_PROFILE(pRenderContext, "GenShadowMapPoint");
    if (index == 0)
    {
        mUpdateShadowMap |= mShadowCubeRasterPass.pState->getProgram()->addDefines(getDefinesShadowMapGenPass());
        dummyProfileRaster(pRenderContext);
    }

    // Create Program Vars
    if (!mShadowCubeRasterPass.pVars)
    {
        mShadowCubeRasterPass.pVars = GraphicsVars::create(mpDevice, mShadowCubeRasterPass.pProgram.get());
    }

    auto changes = light->getChanges();
    bool renderLight = false;
    if (mUpdateShadowMap)
        mStaticTexturesReady[1] = false;

    bool lightMoved = is_set(changes,Light::Changes::Position);
    if (mShadowMapUpdateMode == SMUpdateMode::Static)
        renderLight = is_set(changes , Light::Changes::Active) || lightMoved;
    else if (mShadowMapUpdateMode == SMUpdateMode::Dynamic)
        renderLight = true;

    renderLight |= mUpdateShadowMap;

    if (!renderLight || !light->isActive())
        return;

    auto& lightData = light->getData();

    ShaderParameters params;
    params.lightPosition = lightData.posW;
    params.farPlane = mFar;
    params.nearPlane = mNear;

    const float4x4 projMat = math::perspective(float(M_PI_2), 1.f, mNear, mFar); //Is the same for all 6 faces

    RasterizerState::MeshRenderMode meshRenderMode = RasterizerState::MeshRenderMode::All;

    // Render the static shadow map
    if (mShadowMapUpdateMode != SMUpdateMode::Static && !mStaticTexturesReady[1])
        meshRenderMode |= RasterizerState::MeshRenderMode::SkipDynamic;
    else if (mShadowMapUpdateMode != SMUpdateMode::Static)
        meshRenderMode |= RasterizerState::MeshRenderMode::SkipStatic;


    for (size_t face = 0; face < 6; face++)
    {
        if (is_set(meshRenderMode, RasterizerState::MeshRenderMode::SkipDynamic))
        {
            uint cubeDepthIdx = index * 6 + face;
            //  Attach Render Targets
            mpFboCube->attachColorTarget(mpShadowMapsCubeStatic[index], 0, 0, face, 1);
            mpFboCube->attachDepthStencilTarget(mpDepthCubeStatic[cubeDepthIdx]);
        }
        else if (is_set(meshRenderMode, RasterizerState::MeshRenderMode::SkipStatic))
        {
            uint cubeDepthIdx = index * 6 + face;
            // Copy the resources
            pRenderContext->copyResource(mpDepthCube.get(), mpDepthCubeStatic[cubeDepthIdx].get());
            if (face == 0)
                pRenderContext->copyResource(mpShadowMapsCube[index].get(), mpShadowMapsCubeStatic[index].get());
            mpFboCube->attachColorTarget(mpShadowMapsCube[index], 0, 0, face, 1);
            mpFboCube->attachDepthStencilTarget(mpDepthCube);
        }
        else
        {
            // Attach Render Targets
            mpFboCube->attachColorTarget(mpShadowMapsCube[index], 0, 0, face, 1);
            mpFboCube->attachDepthStencilTarget(mpDepthCube);
        }
        
        float3 lightTarget, up;
        params.viewProjectionMatrix = getProjViewForCubeFace(face, lightData, projMat,lightTarget, up);

        const uint cullingIndex = mFrustumCullingVectorOffsets.x + index * 6 + face;
         // Update frustum
        if ((lightMoved || mUpdateShadowMap) && mUseFrustumCulling)
        {
            mFrustumCulling[cullingIndex]->updateFrustum(lightData.posW, lightTarget, up, 1.f, float(M_PI_2), mNear, mFar);
        }

        auto vars = mShadowCubeRasterPass.pVars->getRootVar();
        setSMShaderVars(vars, params);

        mShadowCubeRasterPass.pState->setFbo(mpFboCube);
        if (!is_set(meshRenderMode, RasterizerState::MeshRenderMode::SkipStatic))
        {
            float4 clearColor = float4(1.f);
            if (mShadowMapType == ShadowMapType::Exponential)
                clearColor.x = exp(mExponentialSMConstant); //Set to highest possible
            else if (mShadowMapType == ShadowMapType::ExponentialVariance)
            {
                float2 expVarMax = float2(exp(mEVSMConstant), -exp(-mEVSMNegConstant)); 
                clearColor = float4(expVarMax.x, expVarMax.x * expVarMax.x, expVarMax.y, expVarMax.y * expVarMax.y); // Set to highest possible
            }
                
            pRenderContext->clearFbo(mShadowCubeRasterPass.pState->getFbo().get(), clearColor, 1.f, 0);
        }
        if (mUseFrustumCulling)
        {
            mpScene->rasterizeFrustumCulling(
                pRenderContext, mShadowCubeRasterPass.pState.get(), mShadowCubeRasterPass.pVars.get(),
                mCullMode, meshRenderMode,false, mFrustumCulling[cullingIndex]
            );
        }
        else
        {
            mpScene->rasterize(
                pRenderContext, mShadowCubeRasterPass.pState.get(), mShadowCubeRasterPass.pVars.get(), mCullMode, meshRenderMode, false
            );
        }
        
    }

    // Blur if it is activated/enabled
    if (mpBlurCube && (!is_set(meshRenderMode,  RasterizerState::MeshRenderMode::SkipDynamic)) )
        mpBlurCube->execute(pRenderContext, mpShadowMapsCube[index]);
    
    /* TODO doesnt work, needs fixing
    if (mShadowMapType != ShadowMapType::ShadowMap && mUseShadowMipMaps)
        mpShadowMapsCube[index]->generateMips(pRenderContext);
    */

     if (is_set(meshRenderMode, RasterizerState::MeshRenderMode::SkipDynamic))
        mStaticTexturesReady[1] = true;
}

bool ShadowMap::rasterSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext) {
    FALCOR_PROFILE(pRenderContext, "GenShadowMaps");
    if (index == 0)
    {
        mUpdateShadowMap |= mShadowMapRasterPass.pState->getProgram()->addDefines(getDefinesShadowMapGenPass()); // Update defines
        // Create Program Vars
        if (!mShadowMapRasterPass.pVars)
        {
            mShadowMapRasterPass.pVars = GraphicsVars::create(mpDevice, mShadowMapRasterPass.pProgram.get());
        }

        dummyProfileRaster(pRenderContext);
    }

    
    auto changes = light->getChanges();

    bool dynamicMode = (mShadowMapUpdateMode != SMUpdateMode::Static) || mClearDynamicSM;

    bool lightMoved = is_set(changes, Light::Changes::Position) || is_set(changes, Light::Changes::Direction);
    bool updateVP = is_set(changes, Light::Changes::Active) || lightMoved || mUpdateShadowMap; 

    if (!light->isActive())
    {
        return false;
    }

    auto& lightData = light->getData();

    //Update the ViewProjection and Frustum
    if (updateVP)
    {
        float3 lightTarget = lightData.posW + lightData.dirW;
        const float3 up = abs(lightData.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
        float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, up);
        float4x4 projMat = math::perspective(lightData.openingAngle * 2, 1.f, mNear, mFar);
        mSpotDirViewProjMat[index] = math::mul(projMat, viewMat);

        if (mUseFrustumCulling)
            mFrustumCulling[index]->updateFrustum(lightData.posW, lightTarget, up, 1.f, lightData.openingAngle * 2, mNear, mFar);
    }

    //Set Uniform
    ShaderParameters params;
    params.farPlane = mFar;
    params.nearPlane = mNear;
    params.viewProjectionMatrix = mSpotDirViewProjMat[index];


    auto vars = mShadowMapRasterPass.pVars->getRootVar();
    setSMShaderVars(vars, params);

    //Render Lamda
    auto bindAndRenderShadowMap = [&](uint idx, const RasterizerState::MeshRenderMode renderMode) {

        // If depth tex is set, Render to RenderTarget
        if (mpDepth)
        {
            //  Attach Render Targets
            mpFbo->attachColorTarget(mpShadowMaps[idx], 0, 0, 0, 1);
            mpFbo->attachDepthStencilTarget(mpDepth);
        }
        else // Else, rendering to depth texture is sufficient
        {
            // Attach Depth
            mpFbo->attachDepthStencilTarget(mpShadowMaps[idx]);
        }

        mShadowMapRasterPass.pState->setFbo(mpFbo);

        // Clear
        float4 clearColor = float4(1.f);
        if (mShadowMapType == ShadowMapType::Exponential)
            clearColor.x = exp(mExponentialSMConstant); // Set to highest possible
        else if (mShadowMapType == ShadowMapType::ExponentialVariance)
        {
            float2 expVarMax = float2(exp(mEVSMConstant), -exp(-mEVSMNegConstant));
            clearColor = float4(expVarMax.x, expVarMax.x * expVarMax.x, expVarMax.y, expVarMax.y * expVarMax.y); // Set to highest possible
        }
        pRenderContext->clearFbo(mShadowMapRasterPass.pState->getFbo().get(), clearColor, 1.f, 0);

        if (mUseFrustumCulling)
        {
            mpScene->rasterizeFrustumCulling(
                pRenderContext, mShadowMapRasterPass.pState.get(), mShadowMapRasterPass.pVars.get(), mFrontClockwiseRS[mCullMode],
                mFrontCounterClockwiseRS[mCullMode], mFrontCounterClockwiseRS[RasterizerState::CullMode::None], renderMode, false,
                mFrustumCulling[index]
            );
        }
        else
        {
            mpScene->rasterize(
                pRenderContext, mShadowMapRasterPass.pState.get(), mShadowMapRasterPass.pVars.get(), mFrontClockwiseRS[mCullMode],
                mFrontCounterClockwiseRS[mCullMode], mFrontCounterClockwiseRS[RasterizerState::CullMode::None], renderMode, false
            );
        }
    };

    //Static Pass
    if (updateVP)
    {
        auto meshRenderMode = RasterizerState::MeshRenderMode::All;
        if (dynamicMode)
            meshRenderMode = RasterizerState::MeshRenderMode::SkipDynamic;

        bindAndRenderShadowMap(index, meshRenderMode);

        //Blur
        if (mpBlurShadowMap)
            mpBlurShadowMap->execute(pRenderContext, mpShadowMaps[index]);

        //Generate Mips for shadow map modes that allow filter
        if (mUseShadowMipMaps)
            mpShadowMaps[index]->generateMips(pRenderContext);
    }

    //Render Dynamic Shadow Map
    if (dynamicMode)
    {
        uint dynIndex = mCountSpotShadowMaps + index; //Offset dynamic Index

        bindAndRenderShadowMap(dynIndex, RasterizerState::MeshRenderMode::SkipStatic);
    }

    return updateVP;
}

 //Calc based on https://learnopengl.com/Guest-Articles/2021/CSM
void ShadowMap::calcProjViewForCascaded(const LightData& lightData, std::vector<bool>& renderLevel, bool forceUpdate) {
   
    const auto& sceneBounds = mpScene->getSceneBounds();
    auto camera = mpScene->getCamera();
    const auto& cameraData = mpScene->getCamera()->getData();

    //Cascaded level calculations
    {
        //Calc the cascaded far value
        mCascadedMaxFar = std::min(sceneBounds.radius() * 2, camera->getFarPlane()); // Clamp Far to scene bounds

        //Check if the size of the array is still right
        if ((mCascadedZSlices.size() != mCascadedLevelCount))
        {
            mCascadedZSlices.clear();
            mCascadedZSlices.resize(mCascadedLevelCount);
        }

        //Temporal AABBs
        if (mEnableTemporalCascadedBoxTest)
        {
            if (mCascadedTemporalReuse.size() != mCascadedLevelCount)
            {
                mCascadedTemporalReuse.clear();
                mCascadedTemporalReuse.resize(mCascadedLevelCount);
            }
        }
        
        switch (mCascadedFrustumMode)
        {
        case Falcor::ShadowMap::CascadedFrustumMode::Manual:
            {
                const float near = camera->getNearPlane();
                const float distanceFarNear = camera->getFarPlane() - near;
                //If the manual array has the wrong size, create a new one
                if (mCascadedFrustumManualVals.size() != mCascadedLevelCount)
                {
                    mCascadedFrustumManualVals.resize(mCascadedLevelCount);
                    //TODO Replace with the Nvidia for init
                    const float equalLevel = 1.f / mCascadedLevelCount;
                    float partSum = equalLevel;
                    for (auto& vals : mCascadedFrustumManualVals)
                    {
                        vals = partSum;
                        partSum += equalLevel;
                    } 
                }

                //Update all zSlices
                for (uint i = 0; i < mCascadedLevelCount; i++)
                {
                    mCascadedZSlices[i] = near + distanceFarNear * mCascadedFrustumManualVals[i];
                }
            }
            break;
            case Falcor::ShadowMap::CascadedFrustumMode::AutomaticNvidia:
            {
                // Z slizes formula by:
                // https://developer.download.nvidia.com/SDK/10.5/opengl/src/cascaded_shadow_maps/doc/cascaded_shadow_maps.pdf
                std::vector<float> cascadedSlices(mCascadedLevelCount);
                const uint N = mCascadedLevelCount;
                for (uint i = 1; i <= N; i++)
                {
                    cascadedSlices[i - 1] = mCascadedFrustumFix * (cameraData.nearZ * pow((mCascadedMaxFar / cameraData.nearZ), float(i) / N));
                    cascadedSlices[i - 1] +=
                        (1.f - mCascadedFrustumFix) * (cameraData.nearZ + (float(i) / N) * (mCascadedMaxFar - cameraData.nearZ));
                }

                // Copy to used cascade levels
                for (uint i = 0; i < mCascadedZSlices.size(); i++)
                    mCascadedZSlices[i] = cascadedSlices[i];
            }
            break;
        }
    }

    //Set start near
    float near = cameraData.nearZ;
    float camFovY = focalLengthToFovY(cameraData.focalLength, cameraData.frameHeight);

    for (uint i = 0; i < mCascadedLevelCount; i++)
    {
        //Get the 8 corners of the frustum Part
        const float4x4 proj = math::perspective(camFovY, cameraData.aspectRatio, near, mCascadedZSlices[i]);
        const float4x4 inv = math::inverse(math::mul(proj, cameraData.viewMat));
        std::vector<float4> frustumCorners;
        for (uint x = 0; x <= 1; x++){
            for (uint y = 0; y <= 1; y++){
                for (uint z = 0; z <= 1; z++){
                    const float4 pt = math::mul(inv, float4(2.f * x - 1.f, 2.f * y - 1.f, z, 1.f));
                    frustumCorners.push_back(pt / pt.w);
                }
            }
        }

        //Get Centerpoint for view
        float3 center = float3(0);
        const float3 upVec = float3(0, 1, 0);
        for (const auto& p : frustumCorners)
            center += p.xyz();
        center /= 8.f;
        const float4x4 casView = math::matrixFromLookAt(center, center + lightData.dirW, upVec);

        //Create a view space AABB to clamp cascaded values
        AABB smViewAABB = sceneBounds.transform(casView);

        //Get Box for Orto
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        for (const float4& p : frustumCorners){
            float3 vp = math::mul(casView, p).xyz();
            vp = math::clamp(vp, smViewAABB.minPoint, smViewAABB.maxPoint); //Clamp to scene extends
            minX = std::min(minX, vp.x);
            maxX = std::max(maxX, vp.x);
            minY = std::min(minY, vp.y);
            maxY = std::max(maxY, vp.y);
            minZ = std::min(minZ, vp.z);
            maxZ = std::max(maxZ, vp.z);
        }

        // Set the Z values to min and max for the scene so that all geometry in the way is rendered
        maxZ = std::max(maxZ, smViewAABB.maxPoint.z);
        minZ = std::min(minZ, smViewAABB.minPoint.z);

        renderLevel[i] = !mEnableTemporalCascadedBoxTest;

        near = mCascadedZSlices[i];

        //Check the box from last frame and abourt rendering if current level is inside the last frames level
        if (mEnableTemporalCascadedBoxTest)
        {
            // Check if the cascaded from last frame is still valid
            if (mCascadedTemporalReuse[i].valid && !forceUpdate)
            {
                bool temporalValid = true;
                for (const float4& p : frustumCorners)
                {
                    //calc view and clamp to temporal view bounds
                    float3 viewP = math::mul(mCascadedTemporalReuse[i].view, p).xyz();
                    viewP = math::clamp(viewP, mCascadedTemporalReuse[i].aabb.minPoint, mCascadedTemporalReuse[i].aabb.maxPoint); 
                    //Projection
                    float3 projP = math::mul(mCascadedTemporalReuse[i].ortho, float4(viewP, 1.0f)).xyz();
                    if (projP.x < -1.f || projP.x > 1.f || projP.y < -1.f || projP.y > 1.f || projP.z < 0.f && projP.z > 1.f)
                        temporalValid = false;
                }

                if (temporalValid)
                    continue;
            }

            // Enlarge the box in x,y and set the previous cascade
            if (minX > 0)
                minX -= minX * mCascadedReuseEnlargeFactor;
            else
                minX += minX * mCascadedReuseEnlargeFactor;
            if (minY > 0)
                minY -= minY * mCascadedReuseEnlargeFactor;
            else
                minY += minY * mCascadedReuseEnlargeFactor;
            if (maxX < 0)
                maxX -= maxX * mCascadedReuseEnlargeFactor;
            else
                maxX += maxX * mCascadedReuseEnlargeFactor;
            if (maxY < 0)
                maxY -= maxY * mCascadedReuseEnlargeFactor;
            else
                maxY += maxY * mCascadedReuseEnlargeFactor;

            mCascadedTemporalReuse[i].valid = true;
            renderLevel[i] = true;
        }

        const float4x4 casProj = math::ortho(minX, maxX, minY, maxY, -1.f * maxZ, -1.f * minZ);

        //Set temporal data
        if (mEnableTemporalCascadedBoxTest)
        {
            mCascadedTemporalReuse[i].aabb = smViewAABB;
            mCascadedTemporalReuse[i].view = casView;
            mCascadedTemporalReuse[i].ortho = casProj;
        }

        mCascadedWidthHeight[i] = float2(abs(maxX - minX), abs(maxY - minY));
        mCascadedVPMatrix[i] = math::mul(casProj, casView);

        //Update Frustum Culling
        if (mUseFrustumCulling)
        {
            const uint cullingIndex = mFrustumCullingVectorOffsets.x + i; //i is cascaded level
            mFrustumCulling[cullingIndex]->updateFrustum(center, center + lightData.dirW, upVec, minX, maxX, minY, maxY, -1.f * maxZ, -1.f * minZ);
        }
    }        
}

bool ShadowMap::rasterCascaded(ref<Light> light, RenderContext* pRenderContext, bool cameraMoved)
{
    FALCOR_PROFILE(pRenderContext, "GenCascadedShadowMaps");
    
    mUpdateShadowMap |= mShadowMapCascadedRasterPass.pState->getProgram()->addDefines(getDefinesShadowMapGenPass()); // Update defines
    // Create Program Vars
    if (!mShadowMapCascadedRasterPass.pVars)
    {
        mShadowMapCascadedRasterPass.pVars = GraphicsVars::create(mpDevice, mShadowMapCascadedRasterPass.pProgram.get());
    }
    dummyProfileRaster(pRenderContext); // Show the render scene every frame

    bool dynamicMode = (mShadowMapUpdateMode != SMUpdateMode::Static) || mClearDynamicSM;
    
    auto changes = light->getChanges();

    bool directionChanged = is_set(changes, Light::Changes::Direction);

    if (!cameraMoved && !mUpdateShadowMap && !dynamicMode && !directionChanged)
        return false;

    auto& lightData = light->getData();

    if ( !light->isActive())
    {
        return false;
    } 

    // Update viewProj
    std::vector<bool> renderCascadedLevel(mCascadedLevelCount);
    calcProjViewForCascaded(lightData, renderCascadedLevel, mUpdateShadowMap || directionChanged);

    // Render each cascade
    const uint loopCount = dynamicMode ? mCascadedLevelCount * 2 : mCascadedLevelCount;
    for (uint i = 0; i < loopCount; i++)
    {
        const uint cascLevel = dynamicMode ? i / 2 : i;
        const bool isDynamic = dynamicMode ? (i % 2 == 1) : false; // Uneven number is the dynamic pass
        const uint cascRenderTargetLevel = isDynamic ? cascLevel + mCascadedLevelCount : cascLevel;

        //Skip static cascaded levels if no update is necessary
        if (!renderCascadedLevel[cascLevel] && !isDynamic)
            continue;

        //Check if the level is fully ray traced
        if (mCanUseRayTracing && mCascadedLastLevelRayTrace && (cascLevel > mCascadedLevelTrace))
            continue;

        // If depth tex is set, Render to RenderTarget
        if (mpDepthCascaded)
        { 
            mpFboCascaded->attachColorTarget(mpCascadedShadowMaps, 0, 0, cascRenderTargetLevel, 1);
            mpFboCascaded->attachDepthStencilTarget(mpDepthCascaded);
        }
        else //Else only render to DepthStencil
        {
            mpFboCascaded->attachDepthStencilTarget(mpCascadedShadowMaps, 0, cascRenderTargetLevel, 1);
        }
       
        ShaderParameters params;
        params.lightPosition = lightData.posW;
        params.farPlane = mFar;
        params.nearPlane = mNear;
        params.viewProjectionMatrix = mCascadedVPMatrix[cascLevel];
        params.disableAlpha = cascLevel >= mCascadedDisableAlphaLevel;

        auto vars = mShadowMapCascadedRasterPass.pVars->getRootVar();
        setSMShaderVars(vars, params);

        mShadowMapCascadedRasterPass.pState->setFbo(mpFboCascaded);

        float4 clearColor = float4(1.f);
        if (mShadowMapType == ShadowMapType::Exponential)
            clearColor.x = exp(mExponentialSMConstant); // Set to highest possible
        else if (mShadowMapType == ShadowMapType::ExponentialVariance)
        {
            float2 expVarMax = float2(exp(mEVSMConstant), -exp(-mEVSMNegConstant));
            clearColor = float4(expVarMax.x, expVarMax.x * expVarMax.x, expVarMax.y, expVarMax.y * expVarMax.y); // Set to highest possible
        }

        //Clear
        if (mpDepthCascaded)
            pRenderContext->clearFbo(mShadowMapCascadedRasterPass.pState->getFbo().get(), clearColor, 1.f, 0);
        else
            pRenderContext->clearDsv(mShadowMapCascadedRasterPass.pState->getFbo()->getDepthStencilView().get(), 1.f, 0.f, true, false);

        //Set mesh render mode
        auto meshRenderMode = RasterizerState::MeshRenderMode::All;
        if (dynamicMode)
        {
            meshRenderMode |= isDynamic ? RasterizerState::MeshRenderMode::SkipStatic : RasterizerState::MeshRenderMode::SkipDynamic;
            //When we want to clear the dynamic SM, jump out here
            if (mClearDynamicSM && isDynamic)
                continue;
        }
        if (mSMDoubleSidedOnly)
        {
            meshRenderMode |= RasterizerState::MeshRenderMode::SkipNonDoubleSided;
        }

        if (mUseFrustumCulling)
        {
            const uint cullingIndex = mFrustumCullingVectorOffsets.x + cascLevel;
            mpScene->rasterizeFrustumCulling(
                pRenderContext, mShadowMapCascadedRasterPass.pState.get(), mShadowMapCascadedRasterPass.pVars.get(),
                mFrontClockwiseRS[mCullMode], mFrontCounterClockwiseRS[mCullMode],
                mFrontCounterClockwiseRS[RasterizerState::CullMode::None], meshRenderMode, false, mFrustumCulling[cullingIndex]
            );
        }
        else
        {
            mpScene->rasterize(
                pRenderContext, mShadowMapCascadedRasterPass.pState.get(), mShadowMapCascadedRasterPass.pVars.get(),
                mFrontClockwiseRS[mCullMode], mFrontCounterClockwiseRS[mCullMode],
                mFrontCounterClockwiseRS[RasterizerState::CullMode::None], meshRenderMode, false
            );
        }
        
    }   

    // Blur all static shadow maps if it is enabled
    if (mpBlurCascaded)
    {
        //Check which if the blur enabled buffer is still valid
        if (mBlurForCascaded.size() != mCascadedLevelCount)
        {
            mBlurForCascaded.resize(mCascadedLevelCount);
            //Init default (disabled for the first two levels)
            for (uint i = 0; i < mBlurForCascaded.size(); i++)
            {
                //if (i < 2)
                //    mBlurForCascaded[i] = false;
                //else
                    mBlurForCascaded[i] = true;
            }
        }

        bool blurRendered = false;
        for (uint i = 0; i < mCascadedLevelCount; i++)
        {
            if (renderCascadedLevel[i] && mBlurForCascaded[i])
            {
                mpBlurCascaded->execute(pRenderContext, mpCascadedShadowMaps, i);
                blurRendered |= true;
            }
        }

        if (!blurRendered)
            mpBlurCascaded->profileDummy(pRenderContext);
    }

    //Determine if a static shadow map was rendered
    bool oneStaticIsRendered = false;
    for (uint i = 0; i < renderCascadedLevel.size(); i++)
        oneStaticIsRendered |= renderCascadedLevel[i];

    //Generate Mips for static shadow maps modes that allow filter
    if (mUseShadowMipMaps)
    {
        for (uint i = 0; (i < mCascadedLevelCount) && oneStaticIsRendered; i++)
        {
            if (renderCascadedLevel[i])
                mpCascadedShadowMaps->generateMips(pRenderContext,false, i);
        }
    }

    return oneStaticIsRendered; //Update VP when at least one was updated
}

bool ShadowMap::update(RenderContext* pRenderContext)
{
    // Return if there is no scene
    if (!mpScene)
        return false;

    // Return if there is no active light
    if (mpScene->getActiveLightCount() == 0)
        return true;

    if (mTypeChanged)
    {
        prepareProgramms();
        mResetShadowMapBuffers = true;
        mShadowResChanged = true;
        mBiasSettingsChanged = true;
        mTypeChanged = false;
    }

    if (mRasterDefinesChanged)
    {
        mUpdateShadowMap = true;
        prepareRasterProgramms();
        mRasterDefinesChanged = false;
    }

    // Rebuild the Shadow Maps
    if (mResetShadowMapBuffers || mShadowResChanged)
    {
        prepareShadowMapBuffers();
    }

    //Set Bias Settings for normal shadow maps
    if (mBiasSettingsChanged)
    {
        updateRasterizerStates(); // DepthBias is set here
        mUpdateShadowMap = true;       // Re render all SM
        mBiasSettingsChanged = false;
    }

    if (mRerenderStatic && mShadowMapUpdateMode == SMUpdateMode::Static)
        mUpdateShadowMap = true;

    //Handle Blur
    prepareGaussianBlur();

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
        rasterCubeEachFace(i, lightRenderListCube[i], pRenderContext);

    // Spot/Directional Lights
    bool updateVP = false;
    // Render all spot / directional lights
    for (size_t i = 0; i < lightRenderListMisc.size(); i++)
        updateVP |= rasterSpotLight(i, lightRenderListMisc[i], pRenderContext);

    //Update VP
    if (updateVP)
        updateSMVPBuffer(pRenderContext, mpVPMatrixBuffer, mSpotDirViewProjMat);

    //Render cascaded
    //updateVPBuffer |= lightRenderListCascaded.size() > 0;
    bool updateCascadedVPBuffer = false;
    bool cascFirstThisFrame = true;
    const auto& camera = mpScene->getCamera();
    auto cameraChanges = camera->getChanges();
    auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
    bool cameraMoved = (cameraChanges & ~excluded) != Camera::Changes::None;

    if (lightRenderListCascaded.size() > 0)
        updateCascadedVPBuffer |= rasterCascaded(lightRenderListCascaded[0], pRenderContext, cameraMoved);
    
    //Update VP
    if (updateCascadedVPBuffer)
        updateSMVPBuffer(pRenderContext, mpCascadedVPMatrixBuffer, mCascadedVPMatrix);

    if (mClearDynamicSM)
        mClearDynamicSM = false;

    if (mpOracle)
        mpOracle->handleNormalizedPixelSizeBuffer(
            mpScene, mShadowMapSize, mShadowMapSizeCube, mShadowMapSizeCascaded, mCascadedLevelCount, mCascadedWidthHeight
        );

    mUpdateShadowMap = false;
    return true;
}

void ShadowMap::updateSMVPBuffer(RenderContext* pRenderContext, VPMatrixBuffer& vpBuffer, std::vector<float4x4>& vpMatrix) {
    auto& stagingCount = vpBuffer.stagingCount;

    // Update staging values
    vpBuffer.stagingFenceWaitValues[stagingCount] = mpScene->getLastFrameFenceValue();
    stagingCount = (stagingCount + 1) % kStagingBufferCount;
    

    size_t totalSize = vpMatrix.size();
    auto& fenceWaitVal = vpBuffer.stagingFenceWaitValues[stagingCount];
    const uint stagingOffset = totalSize * stagingCount;

    // Wait for the GPU to finish copying from kStagingFramesInFlight frames back
    mpScene->getFence()->syncCpu(fenceWaitVal);
    float4x4* mats = (float4x4*)vpBuffer.staging->map(Buffer::MapType::Write);

    for (size_t i = 0; i < totalSize; i++)
    {
        mats[stagingOffset + i] = vpMatrix[i];
    }

    pRenderContext->copyBufferRegion(
        vpBuffer.buffer.get(), 0, vpBuffer.staging.get(), sizeof(float4x4) * stagingOffset, sizeof(float4x4) * totalSize
    );
}

bool ShadowMap::renderUILeakTracing(Gui::Widgets& widget, bool leakTracingEnabled)
{
    bool dirty = false;

    static uint classicBias = mBias;
    static float classicSlopeBias = mSlopeBias;
    static float cubeBias = mSMCubeWorldBias;
    if (widget.dropdown("Shadow Map Type", mShadowMapType))
    { // If changed, reset all buffers
        mTypeChanged = true;
        // Change Settings depending on type
        switch (mShadowMapType)
        {
        case Falcor::ShadowMapType::ShadowMap:
            mBias = classicBias;
            mSlopeBias = classicSlopeBias;
            mSMCubeWorldBias = cubeBias;
            break;
        default:
            mBias = 0;
            mSlopeBias = 0.f;
            mSMCubeWorldBias = 0.f;
            break;
        }
        dirty = true;
    }
    widget.tooltip("Changes the Shadow Map Type. SD indicates the optimized single-depth version", true);

    // Common options used in all shadow map variants
    if (auto group = widget.group("Common Settings"))
    {
        group.separator();

        static uint3 resolution = uint3(mShadowMapSize, mShadowMapSizeCube, mShadowMapSizeCascaded);
        if (mpShadowMaps.size() > 0)
            widget.var("Spot SM size", resolution.x, 32u, 16384u, 32u);
        if (mpCascadedShadowMaps)
            widget.var("Cascaded SM size", resolution.z, 32u, 16384u, 32u);
        if (mpShadowMapsCube.size() > 0)
            widget.var("Point SM size", resolution.y, 32u, 16384u, 32u);
        if (widget.button("Apply Change"))
        {
            mShadowMapSize = resolution.x;
            mShadowMapSizeCube = resolution.y;
            mShadowMapSizeCascaded = resolution.z;
            mShadowResChanged = true;
            dirty = true;
        }
        group.separator();

        if (mpShadowMaps.size() > 0 || mpShadowMapsCube.size() > 0)
        {
            widget.text("------- Point/Spot SM Range -------");
            widget.var("Point/Spot Near", mNear);
            widget.var("Point/Spot Far", mFar);
            widget.text("-----------------------------------------");
        }

        mRasterDefinesChanged |= group.checkbox("Alpha Test", mUseAlphaTest);
        if (mUseAlphaTest)
        {
            mRasterDefinesChanged |= group.dropdown("Alpha Test Mode", kShadowMapRasterAlphaModeDropdown, mAlphaMode);
            group.tooltip("Alpha Mode for the rasterized shadow map");
        }

        if (group.dropdown("Cull Mode", kShadowMapCullMode, (uint32_t&)mCullMode))
            mUpdateShadowMap = true; // Render all shadow maps again

        mResetShadowMapBuffers |= widget.checkbox("Use FrustumCulling", mUseFrustumCulling); 
        widget.tooltip("Enables Frustum Culling for the shadow map generation");

        if (mShadowMapUpdateMode == SMUpdateMode::Static) 
        {
            widget.checkbox("Render every frame", mRerenderStatic);
            widget.tooltip("Rerenders the shadow map every frame");
        }
        group.separator();
    }

    auto lTTThreshold = [&](Gui::Widgets& guiWidget) {
        dirty |= guiWidget.var("Leak Tracing Test Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
        guiWidget.tooltip(
            "Leak Tracing Test Threshold (epsilon). Ray is needed if shadow value between [TH.x, TH.y]", true
        );
        if (mHSMFilteredThreshold.x > mHSMFilteredThreshold.y)
            mHSMFilteredThreshold.y = mHSMFilteredThreshold.x;
    };

    //BlurMips
    auto blurMipUi = [&](Gui::Widgets& guiWidget) {
        dirty |= guiWidget.checkbox("Enable Blur", mUseGaussianBlur);
        guiWidget.tooltip("Enables a gaussian blur for filterable shadow maps. See \"Gaussian Blur Options\" for Settings.");
        mResetShadowMapBuffers |= guiWidget.checkbox("Use Mip Maps", mUseShadowMipMaps);
        guiWidget.tooltip("Uses MipMaps for applyable shadow map variants. Not recommended for LTT", true);
        if (mUseShadowMipMaps)
        {
            dirty |= guiWidget.var("MIP Bias", mShadowMipBias, 0.5f, 4.f, 0.001f);
            guiWidget.tooltip("Bias used in Shadow Map MIP Calculation. (cos theta)^bias", true);
        }
    };

    // Type specific UI group
    switch (mShadowMapType)
    {
    case ShadowMapType::ShadowMap:
    {
        if (auto group = widget.group("Shadow Map Options"))
        {
            group.separator();
            if (leakTracingEnabled)
            {
                group.text("Hybrid Shadows (AMD FideletyFX) with 2x2 PCF used!. LTT Mask settings still apply");
            }
            bool biasChanged = false;
            biasChanged |= group.var("Bias", mBias, 0, 256, 1);
            biasChanged |= group.var("Slope Bias", mSlopeBias, 0.f, 50.f, 0.001f);

            if (biasChanged)
            {
                classicBias = mBias;
                classicSlopeBias = mSlopeBias;
                cubeBias = mSMCubeWorldBias;
                mBiasSettingsChanged = true;
            }

            dirty |= biasChanged;

            if (!leakTracingEnabled)
            {
                dirty |= group.checkbox("Use PCF", mUsePCF);
                group.tooltip("Enable to use Percentage closer filtering");
                dirty |= group.checkbox("Use Poisson Disc Sampling", mUsePoissonDisc);
                group.tooltip("Use Poisson Disc Sampling, only enabled if rng of the eval function is filled");
                if (mUsePoissonDisc)
                {
                    if (mpCascadedShadowMaps)
                        dirty |= group.var("Poisson Disc Rad", mPoissonDiscRad, 0.f, 50.f, 0.001f);
                }
            }
            group.separator();
        }
        break;
    }
    case ShadowMapType::Variance:
    case ShadowMapType::SDVariance:
    {
        if (auto group = widget.group("Variance Shadow Map Options"))
        {
            group.separator();

            dirty |= group.checkbox("Variance SelfShadow Variant", mVarianceUseSelfShadowVariant);
            group.tooltip("From GPU Gems 3, Chapter 8. Uses part of ddx and ddy depth in variance calculation.");

            lTTThreshold(group);

            if (mShadowMapType == ShadowMapType::Variance)
            {
                blurMipUi(group);
            }
            group.separator();
        }

        break;
    }
    case ShadowMapType::Exponential:
    {
        if (auto group = widget.group("Exponential Shadow Map Options"))
        {
            group.separator();
            dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
            dirty |= group.var("Exponential Constant", mExponentialSMConstant, 1.f, kESM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for exponential shadow map");

            lTTThreshold(group);
            blurMipUi(group);

            group.separator();
        }
        break;
    }
    case ShadowMapType::ExponentialVariance:
    case ShadowMapType::SDExponentialVariance:
    {
        if (auto group = widget.group("Exponential Variance Shadow Map Options"))
        {
            group.separator();
            dirty |= group.var("Exponential Constant", mEVSMConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for exponential shadow map");
            dirty |= group.var("Exponential Negative Constant", mEVSMNegConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for the negative part");

            lTTThreshold(group);
            
            if (mShadowMapType == ShadowMapType::ExponentialVariance)
            {
                blurMipUi(group);
            }
            group.separator();
        }
        break;
    }
    case ShadowMapType::MSMHamburger:
    case ShadowMapType::MSMHausdorff:
    case ShadowMapType::SDMSM:
    {
        if (auto group = widget.group("Moment Shadow Maps Options"))
        {
            group.separator();
            dirty |= group.var("Depth Bias (x1000)", mMSMDepthBias, 0.f, 10.f, 0.001f);
            group.tooltip("Depth bias subtracted from the depth value the moment shadow map is tested against");
            dirty |= group.var("Moment Bias (x1000)", mMSMMomentBias, 0.f, 10.f, 0.001f);
            group.tooltip("Moment bias which pulls all values a bit to 0.5. Needs to be >0 for MSM to be stable");

            lTTThreshold(group);

            if (mShadowMapType != ShadowMapType::SDMSM)
            {
                blurMipUi(group);
            }
            group.separator();
        }
    }
    break;
    default:
        break;
    }

    if (mpCascadedShadowMaps)
    {
        if (auto group = widget.group("CascadedOptions"))
        {
            group.separator();
            if (group.var("Cacaded Level", mCascadedLevelCount, 1u, 8u, 1u))
            {
                mResetShadowMapBuffers = true;
                mShadowResChanged = true;
            }
            group.tooltip("Changes the number of cascaded levels");

            group.text("--- Cascaded Frustum Settings ---");

            group.dropdown("Cascaded Frustum Mode", kCascadedFrustumModeList, (uint32_t&)mCascadedFrustumMode);

            switch (mCascadedFrustumMode)
            {
            case Falcor::ShadowMap::CascadedFrustumMode::Manual:
                group.text("Set Cascaded Levels:");
                group.tooltip("Max Z-Level is set between 0 and 1. If last level has a Z-Value smaller than 1, it is ray traced");
                for (uint i = 0; i < mCascadedFrustumManualVals.size(); i++)
                {
                    const std::string name = "Level " + std::to_string(i);
                    group.var(name.c_str(), mCascadedFrustumManualVals[i], 0.f, 1.0f, 0.001f);
                }
                group.text("--------------------");
                break;
            case Falcor::ShadowMap::CascadedFrustumMode::AutomaticNvidia:
            {
                dirty |= group.var("Z Slize Exp influence", mCascadedFrustumFix, 0.f, 1.f, 0.001f);
                group.tooltip("Influence of the Exponentenial part in the zSlice calculation. (1-Value) is used for the linear part");
            }
            break;
            default:
                break;
            }

            if (leakTracingEnabled)
            {
                group.text("---- Cascaded LTT Settings ----");

                mUpdateShadowMap |= group.var("LTT: Use for cascaded levels:", mCascadedLevelTrace, 0u, mCascadedLevelCount - 1, 1u);
                group.tooltip("Uses LTT only for the first X levels, starting from 0. Only used when LTT is active");
                if (mCascadedLevelTrace < (mCascadedLevelCount - 1))
                {
                    uint lastLevelTraceSetting = mCascadedLastLevelRayTrace ? 1 : 0;
                    group.text("Shadow mode after:");
                    bool lastLevelTracedChanged = group.dropdown(" ", kCascadedModeForEndOfLevels, lastLevelTraceSetting);
                    group.tooltip("Mode for cascaded levels after LTT is not used.");
                    if (lastLevelTracedChanged)
                    {
                        mUpdateShadowMap = true;
                        mCascadedLastLevelRayTrace = lastLevelTraceSetting == 1;
                    }
                }
            }
            
            group.text("---- Cascaded Reuse ----");
            dirty |= group.checkbox("Enable Cascaded Reuse", mEnableTemporalCascadedBoxTest);
            group.tooltip("Enlarges the rendered cascade and reuses it in the next frame if cascaded level is still valid");
            if (mEnableTemporalCascadedBoxTest)
            {
                dirty |= group.var("Reuse Enlarge Factor", mCascadedReuseEnlargeFactor, 0.f, 10.f, 0.001f);
                group.tooltip("Factor by which the frustum of each cascaded level is enlarged by");
            }

            group.separator();
        }
    }

    if (mUseGaussianBlur && mpBlurCascaded)
    {
        bool blurSettingsChanged = false;
        if (auto group = widget.group("Gaussian Blur Options"))
        {
            group.separator();
            
            blurSettingsChanged |= mpBlurCascaded->renderUI(group);
            if (auto group3 = group.group("Enable Blur per Cascaded Level", true))
            {
                for (uint level = 0; level < mBlurForCascaded.size(); level++)
                {
                    // Bool vectors are very lovely, therefore this solution :)
                    bool currentLevel = mBlurForCascaded[level];
                    std::string blurLevelName = "Level " + std::to_string(level) + ":";
                    dirty |= group3.checkbox(blurLevelName.c_str(), currentLevel);
                    mBlurForCascaded[level] = currentLevel;
                }
            }
            group.separator();
        }

        dirty |= blurSettingsChanged;
        mUpdateShadowMap |= blurSettingsChanged; // Rerender Shadow maps if the blur settings changed
    }

    dirty |= mRasterDefinesChanged;
    dirty |= mResetShadowMapBuffers;

    return dirty;
}

bool ShadowMap::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    widget.tooltip("Uses a ray tracing shader to generate the shadow maps");
    mResetShadowMapBuffers |= widget.checkbox("Use FrustumCulling", mUseFrustumCulling);
    widget.tooltip("Enables Frustum Culling for the shadow map generation");
 
    static uint classicBias = mBias;
    static float classicSlopeBias = mSlopeBias;
    static float cubeBias = mSMCubeWorldBias;
    if (widget.dropdown("Shadow Map Type", mShadowMapType))
    {   //If changed, reset all buffers
        mTypeChanged = true;
        //Change Settings depending on type
        switch (mShadowMapType)
        {
        case Falcor::ShadowMapType::ShadowMap:
            mBias = classicBias;
            mSlopeBias = classicSlopeBias;
            mSMCubeWorldBias = cubeBias;
            break;
        default:
            mBias = 0;
            mSlopeBias = 0.f;
            mSMCubeWorldBias = 0.f;
            break;
        }
        dirty = true;
    }
    widget.tooltip("Changes the Shadow Map Type. For types other than Shadow Map, a extra depth texture is needed",true);

    if (mSceneIsDynamic)
    {
        mClearDynamicSM |= widget.dropdown("Update Mode", kShadowMapUpdateModeDropdownList, (uint&)mShadowMapUpdateMode);
        widget.tooltip("Specify the update mode for shadow maps"); // TODO add more detail to each mode

        if (mShadowMapUpdateMode != SMUpdateMode::Static)
        {
            bool resetStaticSM = widget.button("Reset Static SM");
            widget.tooltip("Rerenders all static shadow maps");
            if (resetStaticSM)
            {
                mStaticTexturesReady[0] = false;
                mStaticTexturesReady[1] = false;
            }
        }
    }

    if (mShadowMapUpdateMode == SMUpdateMode::Static)
    {
        widget.checkbox("Render every frame", mRerenderStatic);
        widget.tooltip("Rerenders the shadow map every frame");
    }

    static uint3 resolution = uint3(mShadowMapSize, mShadowMapSizeCube, mShadowMapSizeCascaded);
    widget.var("Shadow Map / Cube / Cascaded Res", resolution, 32u, 16384u, 32u);
    widget.tooltip("Change Resolution for the Shadow Map (x) or Shadow Cube Map (y) or Cascaded SM (z). Rebuilds all buffers!");
    if (widget.button("Apply Change"))
    {
        mShadowMapSize = resolution.x;
        mShadowMapSizeCube = resolution.y;
        mShadowMapSizeCascaded = resolution.z;
        mShadowResChanged = true;
        dirty = true;
    }

     widget.dummy("", float2(1.5f)); //Spacing

     //Common options used in all shadow map variants
     if (auto group = widget.group("Common Settings"))
     {
        mUpdateShadowMap |= group.checkbox("Render Double Sided Only", mSMDoubleSidedOnly);
        group.tooltip("Only renders materials flagged as double sided (often alpha tested). Can be used as an optimization");
        mRasterDefinesChanged |= group.checkbox("Alpha Test", mUseAlphaTest);
        if (mUseAlphaTest)
        {
            mRasterDefinesChanged |= group.dropdown("Alpha Test Mode", kShadowMapRasterAlphaModeDropdown, mAlphaMode);
            group.tooltip("Alpha Mode for the rasterized shadow map");
        }
            

        // Near Far option
        static float2 nearFar = float2(mNear, mFar);
        group.var("Near/Far", nearFar, 0.0f, 100000.f, 0.001f);
        group.tooltip("Changes the Near/Far values used for Point and Spotlights");
        if (nearFar.x != mNear || nearFar.y != mFar)
        {
            mNear = nearFar.x;
            mFar = nearFar.y;
            mUpdateShadowMap = true; // Rerender all shadow maps
        }

         if (group.dropdown("Cull Mode", kShadowMapCullMode, (uint32_t&)mCullMode))
            mUpdateShadowMap = true; // Render all shadow maps again

         

         dirty |= group.checkbox("Use Ray Outside of SM", mUseRayOutsideOfShadowMap);
         group.tooltip("Always uses a ray, when position is outside of the shadow map. Else the area is lit", true);
    }

    //Type specific UI group
    switch (mShadowMapType)
    {
    case ShadowMapType::ShadowMap:
    {
        if (auto group = widget.group("Shadow Map Options")){
            bool biasChanged = false;
            biasChanged |= group.var("Bias", mBias, 0, 256, 1);
            biasChanged |= group.var("Slope Bias", mSlopeBias, 0.f, 50.f, 0.001f);

            if (mpShadowMapsCube.size() > 0)
            {
                biasChanged |= group.var("Cube Bias", mSMCubeWorldBias, -10.f, 10.f, 0.0001f);
                group.tooltip("Bias for Cube shadow maps in World space");
            }

            if (biasChanged)
            {
                classicBias = mBias;
                classicSlopeBias = mSlopeBias;
                cubeBias = mSMCubeWorldBias;
                mBiasSettingsChanged = true;
            }

            dirty |= biasChanged;

            dirty |= group.checkbox("Use PCF", mUsePCF);
            group.tooltip("Enable to use Percentage closer filtering");
            dirty |= group.checkbox("Use Poisson Disc Sampling", mUsePoissonDisc);
            group.tooltip("Use Poisson Disc Sampling, only enabled if rng of the eval function is filled");
            if (mUsePoissonDisc)
            {
                if (mpCascadedShadowMaps || mpShadowMaps.size() > 0)
                    dirty |= group.var("Poisson Disc Rad", mPoissonDiscRad, 0.f, 50.f, 0.001f);
                else if (mpShadowMapsCube.size() > 0)
                {
                    dirty |= group.var("Poisson Disc Rad Cube", mPoissonDiscRadCube, 0.f, 20.f, 0.00001f);
                }
                    
            }
                
        }
        break;
    }
    case ShadowMapType::Variance:
    case ShadowMapType::SDVariance:
    {
        if (auto group = widget.group("Variance Shadow Map Options"))
        {
            dirty |= group.checkbox("Variance SelfShadow Variant", mVarianceUseSelfShadowVariant);
            group.tooltip("Uses part of ddx and ddy depth in variance calculation. Should not be used with Blur!. Only enabled in rasterize shadow map mode.");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip("Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [TH.x, TH.y]", true);
            if (mHSMFilteredThreshold.x > mHSMFilteredThreshold.y)
                mHSMFilteredThreshold.y = mHSMFilteredThreshold.x;

            if (mShadowMapType == ShadowMapType::Variance)
            {
                dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
                mResetShadowMapBuffers |= group.checkbox("Use Mip Maps", mUseShadowMipMaps);
                group.tooltip("Uses MipMaps for applyable shadow map variants", true);
                if (mUseShadowMipMaps)
                {
                    dirty |= group.var("MIP Bias", mShadowMipBias, 0.5f, 4.f, 0.001f);
                    group.tooltip("Bias used in Shadow Map MIP Calculation. (cos theta)^bias", true);
                }

                dirty |= group.checkbox("Use PCF", mUsePCF);
                group.tooltip("Enable to use Percentage closer filtering");
            }
        }
        
        break;
    }
    case ShadowMapType::Exponential:
    {
        if (auto group = widget.group("Exponential Shadow Map Options"))
        {
            dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
            dirty |= group.var("Exponential Constant", mExponentialSMConstant, 1.f, kESM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for exponential shadow map");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [TH, 1.f]", true
            );
            mResetShadowMapBuffers |= group.checkbox("Use Mip Maps", mUseShadowMipMaps);
            group.tooltip("Uses MipMaps for applyable shadow map variants", true);
            if (mUseShadowMipMaps)
            {
                dirty |= group.var("MIP Bias", mShadowMipBias, 0.5f, 4.f, 0.001f);
                group.tooltip("Bias used in Shadow Map MIP Calculation. (cos theta)^bias", true);
            }
        }
        break;
    }
    case ShadowMapType::ExponentialVariance:
    case ShadowMapType::SDExponentialVariance:
    {
        if (auto group = widget.group("Exponential Variance Shadow Map Options"))
        {          
            dirty |= group.var("Exponential Constant", mEVSMConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for exponential shadow map");
            dirty |= group.var("Exponential Negative Constant", mEVSMNegConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for the negative part");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [x, y]", true
            );
            group.checkbox("Enable extra hybrid test", mEVSMExtraTest);
            group.tooltip("Enables an additionall test on top of LTT. A ray is traced if LTT or abs(posEXP - negEXP)<e.");
            if (mShadowMapType == ShadowMapType::ExponentialVariance)
            {
                dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
                mResetShadowMapBuffers |= group.checkbox("Use Mip Maps", mUseShadowMipMaps);
                group.tooltip("Uses MipMaps for applyable shadow map variants", true);
                if (mUseShadowMipMaps)
                {
                    dirty |= group.var("MIP Bias", mShadowMipBias, 0.5f, 4.f, 0.001f);
                    group.tooltip("Bias used in Shadow Map MIP Calculation. (cos theta)^bias", true);
                }
            }
        }
        break;
    }
    case ShadowMapType::MSMHamburger:
    case ShadowMapType::MSMHausdorff:
    case ShadowMapType::SDMSM:
    {
        if (auto group = widget.group("Moment Shadow Maps Options"))
        {
            dirty |= group.var("Depth Bias (x1000)", mMSMDepthBias, 0.f, 10.f, 0.001f);
            group.tooltip("Depth bias subtracted from the depth value the moment shadow map is tested against");
            dirty |= group.var("Moment Bias (x1000)", mMSMMomentBias, 0.f, 10.f, 0.001f);
            group.tooltip("Moment bias which pulls all values a bit to 0.5. Needs to be >0 for MSM to be stable");
            
           
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [x, y]", true
            );
            dirty |= group.checkbox("HSM use additional variance test", mMSMUseVarianceTest);
            group.tooltip("Additional Variance test using the first two moments. Can help as both variance exhibit different artifacts");
            if (mMSMUseVarianceTest)
            {
                dirty |= group.var("HSM Variance Difference", mMSMVarianceThreshold, 0.f, 1.f, 0.001f);
                group.tooltip("Threshold difference for the additional variance test. A ray is shot if difference is bigger than the threshold");
            }

            if (mShadowMapType != ShadowMapType::SDMSM)
            {
                dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
                group.tooltip(
                    "Enables Gaussian Blur for shadow maps. For Cascaded, each level has a seperate checkbox (see Cascaded Options)"
                );
                mResetShadowMapBuffers |= group.checkbox("Use Mip Maps", mUseShadowMipMaps);
                group.tooltip("Uses MipMaps for applyable shadow map variants", true);
                if (mUseShadowMipMaps)
                {
                    dirty |= group.var("MIP Bias", mShadowMipBias, 0.5f, 4.f, 0.001f);
                    group.tooltip("Bias used in Shadow Map MIP Calculation. (cos theta)^bias", true);
                }
            }
            
        }
    }
    break;
    default:
        break;
    }
    
    if (mpCascadedShadowMaps)
    {
        if (auto group = widget.group("CascadedOptions"))
        {
            if (group.var("Cacaded Level", mCascadedLevelCount, 1u, 8u, 1u))
            {
                mResetShadowMapBuffers = true;
                mShadowResChanged = true;
            }
            group.tooltip("Changes the number of cascaded levels");

            group.dropdown("Cascaded Frustum Mode", kCascadedFrustumModeList, (uint32_t&)mCascadedFrustumMode);

            switch (mCascadedFrustumMode)
            {
            case Falcor::ShadowMap::CascadedFrustumMode::Manual:
                group.text("Set Cascaded Levels:");
                group.tooltip("Max Z-Level is set between 0 and 1. If last level has a Z-Value smaller than 1, it is ray traced");
                for (uint i = 0; i < mCascadedFrustumManualVals.size(); i++)
                {
                    const std::string name = "Level " + std::to_string(i);
                    group.var(name.c_str(), mCascadedFrustumManualVals[i], 0.f, 1.0f, 0.001f);
                }
                group.text("--------------------");
                break;
            case Falcor::ShadowMap::CascadedFrustumMode::AutomaticNvidia:
                {
                    dirty |= group.var("Z Slize Exp influence", mCascadedFrustumFix, 0.f, 1.f, 0.001f);
                    group.tooltip("Influence of the Exponentenial part in the zSlice calculation. (1-Value) is used for the linear part");
                }
                break;
            default:
                break;
            }

            mUpdateShadowMap |= group.var("Hybrid: Use for cascaded levels:", mCascadedLevelTrace, 0u, mCascadedLevelCount - 1, 1u);
            group.tooltip("Uses Hybrid for X levels, starting from 0. Only used when Hybrid is active");
            mUpdateShadowMap |= group.checkbox("Use full ray shadows after hybrid cutoff", mCascadedLastLevelRayTrace);
            group.tooltip("Uses ray traced shadows instead of the shadow map after the hybrid cutoff. Only used in hybrid mode");
            dirty |= group.checkbox("Use Temporal Cascaded Reuse", mEnableTemporalCascadedBoxTest);
            group.tooltip("Enlarges the rendered cascade and reuses it in the next frame if camera has not moved so much");
            if (mEnableTemporalCascadedBoxTest)
            {
                dirty |= group.var("Reuse Enlarge Factor", mCascadedReuseEnlargeFactor, 0.f, 10.f, 0.001f);
                group.tooltip("Factor by which the frustum of each cascaded level is enlarged by");
            }

            group.checkbox("Use Stochastic Cascaded Level", mUseStochasticCascadedLevels);
            if (mUseStochasticCascadedLevels)
            {
                dirty |= group.var("Stochastic Level Range", mCascadedStochasticRange, 0.f, 0.3f, 0.001f);
                group.tooltip("Stochastically shifts the cascaded level by percentage (values * 2). ");
            }
            
            dirty |= group.var("Use Alpha Test until level", mCascadedDisableAlphaLevel, 0u, mCascadedLevelCount, 1u);
            group.tooltip("Disables alpha test for shadow map generation starting from that level. Set to CascadedCount + 1 to use Alpha test for every level");
        }
    }

    if (mUseGaussianBlur)
    {
        bool blurSettingsChanged = false;
        if (auto group = widget.group("Gaussian Blur Options"))
        {
            if (mpBlurShadowMap)
            {
                if (auto group2 = group.group("ShadowMap"))
                    blurSettingsChanged |= mpBlurShadowMap->renderUI(group2);
            }
            if (mpBlurCascaded)
            {
                if (auto group2 = group.group("Cascaded"))
                {
                    blurSettingsChanged |= mpBlurCascaded->renderUI(group2);
                    if (auto group3 = group2.group("Enable Blur per Cascaded Level", true))
                    {
                        for (uint level = 0; level < mBlurForCascaded.size(); level++)
                        {
                            // Bool vectors are very lovely, therefore this solution :)
                            bool currentLevel = mBlurForCascaded[level];
                            std::string blurLevelName = "Level " + std::to_string(level) + ":";
                            dirty |= group3.checkbox(blurLevelName.c_str(), currentLevel);
                            mBlurForCascaded[level] = currentLevel;
                        }
                    }   
                }  
            }
            if (mpBlurCube)
            {
                if (auto group2 = group.group("PointLights"))
                    blurSettingsChanged |= mpBlurCube->renderUI(group2);
            }
        }

        dirty |= blurSettingsChanged;
        mUpdateShadowMap |= blurSettingsChanged; //Rerender Shadow maps if the blur settings changed
    }

    if (mpOracle)
    {
        if (auto group = widget.group("Oracle Options"))
        {
            mpOracle->renderUI(group);
        }
    }

    dirty |= mRasterDefinesChanged;
    dirty |= mResetShadowMapBuffers;

    return dirty;
}

float ShadowMap::getCascadedFarForLevel(uint level) {
    if (mCascadedZSlices.size() > level)
    {
        float range = mCascadedZSlices[level] - mNear;
        return mCascadedZSlices[level] + mCascadedStochasticRange * range;
    }
    return 0.f;
}

float ShadowMap::getCascadedAlphaTestDistance() {
    if (mCascadedDisableAlphaLevel < mCascadedLevelCount)
    {
        return getCascadedFarForLevel(mCascadedDisableAlphaLevel - 1);
    }
    return 100000.f;
}

void ShadowMap::dummyProfileRaster(RenderContext* pRenderContext) {
    FALCOR_PROFILE(pRenderContext, "rasterizeScene");
}

}
