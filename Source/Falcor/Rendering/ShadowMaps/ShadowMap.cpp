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
const std::string kShadowGenRayShader = "Rendering/ShadowMaps/GenerateShadowMap.rt.slang";
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

const Gui::DropdownList kJitterModeDropdownList{
    {(uint)ShadowMap::SamplePattern::None, "None"},
    {(uint)ShadowMap::SamplePattern::DirectX, "DirectX"},
    {(uint)ShadowMap::SamplePattern::Halton, "Halton"},
    {(uint)ShadowMap::SamplePattern::Stratified, "Stratified"},
};

const Gui::DropdownList kCascadedFrustumModeList{
    {(uint)ShadowMap::CascadedFrustumMode::Manual, "Manual"},
    {(uint)ShadowMap::CascadedFrustumMode::AutomaticNvidia, "AutomaticNvidia"},
};
} // namespace

ShadowMap::ShadowMap(ref<Device> device, ref<Scene> scene) : mpDevice{device}, mpScene{scene}
{
    FALCOR_ASSERT(mpScene);

    // Create FBO
    mpFbo = Fbo::create(mpDevice);
    mpFboCube = Fbo::create(mpDevice);
    mpFboCascaded = Fbo::create(mpDevice);

    // Update all shadow maps every frame
    if (mpScene->hasDynamicGeometry())
    {
        mSceneIsDynamic = true;
        mShadowMapUpdateMode = SMUpdateMode::Dynamic;
    }  

    // Create Light Mapping Buffer
    prepareShadowMapBuffers();

    prepareProgramms();

    // Create sampler.
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
    samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
    mpShadowSamplerPoint = Sampler::create(mpDevice, samplerDesc);

    //TODO add anisotropy ? Can this be used in ray tracing shaders?
    samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear);
    mpShadowSamplerLinear = Sampler::create(mpDevice, samplerDesc);

    //Init Fence values
    for (auto& waitVal : mStagingFenceWaitValues)
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
        mpCascadedShadowMaps.clear();

        //Depth Buffers
        mpDepthCascaded.reset();
        mpDepthCube.reset();
        mpDepth.reset();

        //Static copys for animations
        mpShadowMapsCubeStatic.clear();
        mpShadowMapsStatic.clear();
        mpDepthCubeStatic.clear();
        mpDepthStatic.clear();

        //Misc
        mpNormalizedPixelSize.reset();
    }

    // Lighting Changed
    if (mResetShadowMapBuffers)
    {
        mpLightMapping.reset();
        mpVPMatrixBuffer.reset();
        mpVPMatrixStangingBuffer.reset();
    }

    //Initialize the Shadow Map Textures
    const std::vector<ref<Light>>& lights = mpScene->getLights();
    
    uint countPoint = 0;
    uint countMisc = 0;
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
        generateAdditionalDepthTextures = !mUseRaySMGen;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::Exponential:
    {
        shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::R32Float : ResourceFormat::R16Float;
        shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        generateAdditionalDepthTextures = !mUseRaySMGen;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::ExponentialVariance:
    case ShadowMapType::MSMHamburger:
    case ShadowMapType::MSMHausdorff:
    {
        shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::RGBA32Float : ResourceFormat::RGBA16Float;
        shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        generateAdditionalDepthTextures = !mUseRaySMGen;
        genMipMaps = mUseShadowMipMaps;
        break;
    }
    case ShadowMapType::ShadowMap: // No special format needed
    {
        if (mUseRaySMGen)
        {
            shadowMapFormat = mShadowMapFormat == ResourceFormat::D32Float ? ResourceFormat::R32Float : ResourceFormat::R16Float;
            shadowMapBindFlags |= ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget;
        }
        else
        {
            shadowMapFormat = mShadowMapFormat;
            shadowMapBindFlags |= ResourceBindFlags::DepthStencil;
            if (mSceneIsDynamic)
                shadowMapBindFlags |= ResourceBindFlags::RenderTarget;  //Needed for copy (blit)
        }
        
    }
    }

    //Create the Shadow Map tex for every light
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
            if (mShadowMapType != ShadowMapType::ShadowMap || mUseRaySMGen)
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
            tex = Texture::create2D(
                mpDevice, mShadowMapSize, mShadowMapSize, shadowMapFormat, 1u, genMipMaps ? Texture::kMaxPossible : 1u, nullptr,
                shadowMapBindFlags
            );
            tex->setName("ShadowMapMisc" + std::to_string(countMisc));

            lightMapping.push_back(countMisc); // Push Back Misc ID
            countMisc++;
            mpShadowMaps.push_back(tex);
        }
        else if (lightType == LightTypeSM::Directional)
        {
            tex = Texture::create2D(
                mpDevice, mShadowMapSizeCascaded, mShadowMapSizeCascaded, shadowMapFormat, mCascadedLevelCount,
                genMipMaps ? Texture::kMaxPossible : 1u, nullptr, shadowMapBindFlags
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

    //Create Depth Textures
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
    if (!mpDepth && countMisc > 0 && generateAdditionalDepthTextures)
    {
        mpDepth = Texture::create2D(
            mpDevice, mShadowMapSize, mShadowMapSize, mShadowMapFormat, 1u, 1u, nullptr, ResourceBindFlags::DepthStencil
        );
        mpDepth->setName("ShadowMap2DPassDepthHelper");
    }

    //For Cascaded optimizations
    mPreviousCascades.resize(countCascade * mCascadedLevelCount);

    //Create Textures for scenes with dynamic geometry
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
        for (size_t i = 0; i < mpShadowMaps.size(); i++)
        {
            // Create a copy Texture
            ref<Texture> tex = Texture::create2D(
                mpDevice, mpShadowMaps[i]->getWidth(), mpShadowMaps[i]->getHeight(), mpShadowMaps[i]->getFormat(), 1u,
                1u, nullptr, mpShadowMaps[i]->getBindFlags()
            );
            tex->setName("ShadowMapStatic" + std::to_string(i));
            mpShadowMapsStatic.push_back(tex);

            // Create a per face depth texture
            if (mpDepth)
            {
                ref<Texture> depthTex = Texture::create2D(
                    mpDevice, mpDepth->getWidth(), mpDepth->getHeight(), mpDepth->getFormat(), 1u, 1u, nullptr, mpDepth->getBindFlags()
                );
                depthTex->setName("ShadowMapPassDepthHelperStatic" + std::to_string(i));
                mpDepthStatic.push_back(depthTex);
            }
        }
        for (size_t i = 0; i < mpCascadedShadowMaps.size(); i++)
        {
            // Create a copy Texture
            ref<Texture> tex = Texture::create2D(
                mpDevice, mpCascadedShadowMaps[i]->getWidth(), mpCascadedShadowMaps[i]->getHeight(), mpCascadedShadowMaps[i]->getFormat(),
                mCascadedLevelCount, 1u, nullptr, mpCascadedShadowMaps[i]->getBindFlags()
            );
            tex->setName("ShadowMapCascadedStatic" + std::to_string(i));
            mpShadowMapsCascadedStatic.push_back(tex);

            // Create a per cascaded level depth texture
            if (mpDepthCascaded)
            {
                for (uint cascLevel = 0; cascLevel < mCascadedLevelCount; cascLevel++)
                {
                    ref<Texture> depthTex = Texture::create2D(
                        mpDevice, mpDepthCascaded->getWidth(), mpDepthCascaded->getHeight(), mpDepthCascaded->getFormat(), 1u, 1u, nullptr,
                        mpDepthCascaded->getBindFlags()
                    );
                    depthTex->setName("ShadowMapPassCascadedDepthHelperStatic" + std::to_string(i));
                    mpDepthCascadedStatic.push_back(depthTex);
                }
            }
        }
    }

    //Create Frustum Culling Objects
    if (mUseFrustumCulling)
    {
        //Calculate total number of Culling Objects needed
        mFrustumCullingVectorOffsets = uint2(countMisc, countMisc + countCascade * mCascadedLevelCount);
        uint frustumCullingVectorSize = countMisc + countCascade * mCascadedLevelCount + countPoint * 6;
        mFrustumCulling.resize(frustumCullingVectorSize);
        for (size_t i = 0; i < frustumCullingVectorSize; i++)
            mFrustumCulling[i] = make_ref<FrustumCulling>();
    }

    //Check if multiple SM types are used
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

    // Light Mapping Buffer
    if (!mpLightMapping && lightMapping.size() > 0)
    {
        mpLightMapping = Buffer::createStructured(
            mpDevice, sizeof(uint), lightMapping.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, lightMapping.data(),
            false
        );
        mpLightMapping->setName("ShadowMapLightMapping");
    }

    if ((!mpVPMatrixBuffer) && (mpShadowMaps.size() > 0 || mpCascadedShadowMaps.size() > 0))
    {
        size_t size = mpShadowMaps.size() + mpCascadedShadowMaps.size() * mCascadedLevelCount;
        std::vector<float4x4> initData(size);
        for (size_t i = 0; i < initData.size(); i++)
            initData[i] = float4x4::identity();

         mpVPMatrixBuffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size, ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false
        );
        mpVPMatrixBuffer->setName("ShadowMapViewProjectionBuffer");

        mpVPMatrixStangingBuffer = Buffer::createStructured(
            mpDevice, sizeof(float4x4), size * kStagingBufferCount, ResourceBindFlags::ShaderResource,
            Buffer::CpuAccess::Write, initData.data(), false
        );
        mpVPMatrixStangingBuffer->setName("ShadowMapViewProjectionStagingBuffer");
       
        mCascadedMatrixStartIndex = mpShadowMaps.size();   //Set the start index for the cascaded VP Mats
    }

    mCascadedVPMatrix.resize(mpCascadedShadowMaps.size() * mCascadedLevelCount);
    mCascadedWidthHeight.resize(mpCascadedShadowMaps.size() * mCascadedLevelCount); //For Normalized Pixel Size
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

void ShadowMap::prepareRayProgramms(const Program::TypeConformanceList& globalTypeConformances)
{
    mShadowCubeRayPass = RayTraceProgramHelper::create();
    mShadowMapRayPass = RayTraceProgramHelper::create();
    mShadowMapCascadedRayPass = RayTraceProgramHelper::create();

    auto defines = getDefinesShadowMapGenPass(false);

    mShadowCubeRayPass.initRTProgram(mpDevice, mpScene, kShadowGenRayShader, defines, globalTypeConformances);
    mShadowCubeRayPass.pProgram->addDefine("SMRAY_MODE", std::to_string((uint)LightTypeSM::Point));
    mShadowMapRayPass.initRTProgram(mpDevice, mpScene, kShadowGenRayShader, defines, globalTypeConformances);
    mShadowMapRayPass.pProgram->addDefine("SMRAY_MODE", std::to_string((uint)LightTypeSM::Spot));
    mShadowMapCascadedRayPass.initRTProgram(mpDevice, mpScene, kShadowGenRayShader, defines, globalTypeConformances);
    mShadowMapCascadedRayPass.pProgram->addDefine("SMRAY_MODE", std::to_string((uint)LightTypeSM::Directional));
}

void ShadowMap::prepareProgramms()
{
    mpShadowMapParameterBlock.reset();

    auto globalTypeConformances = mpScene->getMaterialSystem().getTypeConformances();
    prepareRasterProgramms();
    prepareRayProgramms(globalTypeConformances);
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

    mpReflectTypes.reset();
}

void ShadowMap::prepareGaussianBlur() {
    bool blurChanged = false;

    if (mUseGaussianBlur && mShadowMapType != ShadowMapType::ShadowMap)
    {
        if (!mpBlurShadowMap && mpShadowMaps.size() > 0)
        {
            mpBlurShadowMap = std::make_unique<SMGaussianBlur>(mpDevice);
            blurChanged = true;
        }
        if (!mpBlurCascaded && mpCascadedShadowMaps.size() > 0)
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
    uint countShadowMapsMisc = std::max(1u, getCountShadowMaps());
    uint countShadowMapsCascade = std::max(1u, (uint) mpCascadedShadowMaps.size());

    uint cascadedSliceBufferSize = mCascadedLevelCount > 4 ? 8 : 4;

    defines.add("MULTIPLE_SHADOW_MAP_TYPES", mMultipleSMTypes ? "1" : "0");
    defines.add("SHADOW_MAP_MODE", std::to_string((uint)mShadowMapType));
    defines.add("NUM_SHADOW_MAPS_CUBE", std::to_string(countShadowMapsCube));
    defines.add("NUM_SHADOW_MAPS_MISC", std::to_string(countShadowMapsMisc));
    defines.add("NUM_SHADOW_MAPS_CASCADE", std::to_string(countShadowMapsCascade));
    defines.add("CASCADED_MATRIX_OFFSET", std::to_string(mCascadedMatrixStartIndex));
    defines.add("CASCADED_LEVEL", std::to_string(mCascadedLevelCount));
    defines.add("CASCADED_SLICE_BUFFER_SIZE", std::to_string(cascadedSliceBufferSize));
    defines.add("SM_USE_PCF", mUsePCF ? "1" : "0");
    defines.add("SM_USE_POISSON_SAMPLING", mUsePoissonDisc ? "1" : "0");
    defines.add("NPS_OFFSET_SPOT", std::to_string(mNPSOffsets.x));
    defines.add("NPS_OFFSET_CASCADED", std::to_string(mNPSOffsets.y));
    defines.add("ORACLE_DIST_FUNCTION_MODE", std::to_string((uint)mOracleDistanceFunctionMode));
    defines.add("SM_EXPONENTIAL_CONSTANT", std::to_string(mShadowMapType == ShadowMapType::ExponentialVariance ? mEVSMConstant : mExponentialSMConstant));
    defines.add("SM_NEGATIVE_EXPONENTIAL_CONSTANT", std::to_string(mEVSMNegConstant));
    defines.add("SM_NEAR", std::to_string(mNear));
    defines.add(
        "HYBRID_SMFILTERED_THRESHOLD",
        "float2(" + std::to_string(mHSMFilteredThreshold.x) + "," + std::to_string(mHSMFilteredThreshold.y) + ")"
    );
    defines.add("MSM_DEPTH_BIAS", std::to_string(mMSMDepthBias));
    defines.add("MSM_MOMENT_BIAS", std::to_string(mMSMMomentBias));
    defines.add("USE_RAY_OUTSIDE_SM", mUseRayOutsideOfShadowMap ? "1" : "0");
    defines.add("CASCADED_SM_RESOLUTION", std::to_string(mShadowMapSizeCascaded));
    defines.add("SM_RESOLUTION", std::to_string(mShadowMapSize));
    defines.add("CUBE_SM_RESOLUTION", std::to_string(mShadowMapSizeCube));
    defines.add("CUBE_WORLD_BIAS", std::to_string(mSMCubeWorldBias));

    defines.add("USE_HYBRID_SM", mUseHybridSM ? "1" : "0");
    defines.add("USE_SM_MIP", mUseShadowMipMaps ? "1" : "0");
    defines.add("SM_MIP_BIAS", std::to_string(mShadowMipBias));
    defines.add("MIN_SHADOW_VALUE_FILTERED", mUseMinShadowValue ? std::to_string(mMinShadowValueVal) : "-1.f");
    defines.add("USE_ORACLE_FUNCTION", mUseSMOracle ? "1" : "0");
    defines.add("ORACLE_COMP_VALUE", std::to_string(mOracleCompaireValue));
    defines.add("ORACLE_UPPER_BOUND", std::to_string(mOracleCompaireUpperBound));
    defines.add("USE_ORACLE_DISTANCE_FUNCTION", mOracleDistanceFunctionMode == OracleDistFunction::None ? "0" : "1");
    defines.add("USE_ORACLE_FOR_DIRECT", mOracleIgnoreDirect ? "1" : "0");
    defines.add("USE_ORACLE_FOR_DIRECT_ROUGHNESS", std::to_string(mOracleIgnoreDirectRoughness));
    

    if (mpScene)
        defines.add(mpScene->getSceneDefines());

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
    var["gCameraNPS"] = getNormalizedPixelSize(frameDim, focalLengthToFovY(cameraData.focalLength, cameraData.frameHeight)  ,cameraData.aspectRatio);
    var["gPoissonDiscRad"] = mPoissonDiscRad;
    var["gPoissonDiscRadCube"] = mPoissonDiscRadCube;
    for (uint i = 0; i < mCascadedZSlices.size(); i++)
        var["gCascadedZSlices"][i] = mCascadedZSlices[i];
    
    // Buffers and Textures
    switch (mShadowMapType)
    {
    case Falcor::ShadowMapType::ShadowMap:
    case Falcor::ShadowMapType::Exponential:
        {
            for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
                var["gShadowMapCube"][i] = mpShadowMapsCube[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
                var["gShadowMap"][i] = mpShadowMaps[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpCascadedShadowMaps.size(); i++)
                var["gCascadedShadowMap"][i] = mpCascadedShadowMaps[i]; // Can be Nullptr
        }
        break;
    case Falcor::ShadowMapType::Variance:
        {
            for (uint32_t i = 0; i < mpShadowMapsCube.size(); i++)
                var["gShadowMapVarianceCube"][i] = mpShadowMapsCube[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpShadowMaps.size(); i++)
                var["gShadowMapVariance"][i] = mpShadowMaps[i]; // Can be Nullptr
            for (uint32_t i = 0; i < mpCascadedShadowMaps.size(); i++)
                var["gCascadedShadowMapVariance"][i] = mpCascadedShadowMaps[i]; // Can be Nullptr
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
            for (uint32_t i = 0; i < mpCascadedShadowMaps.size(); i++)
                var["gCascadedShadowMapF4"][i] = mpCascadedShadowMaps[i]; // Can be Nullptr
        }
        break;
    default:
        break;
    }
     
    var["gShadowMapNPSBuffer"] = mpNormalizedPixelSize; //Can be Nullptr on init
    var["gShadowMapVPBuffer"] = mpVPMatrixBuffer; // Can be Nullptr
    var["gShadowMapIndexMap"] = mpLightMapping;   // Can be Nullptr
    var["gShadowSampler"] = mShadowMapType == ShadowMapType::ShadowMap ? mpShadowSamplerPoint : mpShadowSamplerLinear;
}

void ShadowMap::setShaderDataAndBindBlock(ShaderVar rootVar, const uint2 frameDim)
{
    setShaderData(frameDim);
    rootVar["gShadowMap"] = getParameterBlock();
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
    var["CB"]["gFarPlane"] = params.farPlane;
}

void ShadowMap::setSMRayShaderVars(ShaderVar& var, RayShaderParameters& params)
{
    var["CB"]["gViewProjection"] = params.viewProjectionMatrix;
    var["CB"]["gInvViewProjection"] = params.invViewProjectionMatrix;
    var["CB"]["gLightPos"] = params.lightPosition;
    var["CB"]["gFarPlane"] = params.farPlane;
    var["CB"]["gNearPlane"] = params.nearPlane;

    //Set Jitter
    if (mJitterSamplePattern != SamplePattern::None || !mpCPUJitterSampleGenerator)
    {
        var["CB"]["gJitter"] = mpCPUJitterSampleGenerator->next(); // Jitter
        var["CB"]["gJitterTemporalFilter"] = mTemporalFilterLength; // Temporal filter length
    }
    else
    {
        var["CB"]["gJitter"] = float2(0);  // Disable Jitter
        var["CB"]["gJitterTemporalFilter"] = 1; //No Filter
    }
    
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

    bool lightMoved = changes == Light::Changes::Position;
    if (mShadowMapUpdateMode == SMUpdateMode::Static)
        renderLight = (changes == Light::Changes::Active) || lightMoved;
    else if (mShadowMapUpdateMode == SMUpdateMode::Dynamic)
        renderLight = true;

    renderLight |= mUpdateShadowMap;

    if (!renderLight || !light->isActive())
        return;

    auto& lightData = light->getData();

    ShaderParameters params;
    params.lightPosition = lightData.posW;
    params.farPlane = mFar;

    const float4x4 projMat = math::perspective(float(M_PI_2), 1.f, mNear, mFar); //Is the same for all 6 faces

    RasterizerState::MeshRenderMode meshRenderMode = RasterizerState::MeshRenderMode::All;

    // Render the static shadow map
    if (mShadowMapUpdateMode != SMUpdateMode::Static && !mStaticTexturesReady[1])
        meshRenderMode = RasterizerState::MeshRenderMode::Static;
    else if (mShadowMapUpdateMode != SMUpdateMode::Static)
        meshRenderMode = RasterizerState::MeshRenderMode::Dynamic;


    for (size_t face = 0; face < 6; face++)
    {
        if (meshRenderMode == RasterizerState::MeshRenderMode::Static)
        {
            uint cubeDepthIdx = index * 6 + face;
            //  Attach Render Targets
            mpFboCube->attachColorTarget(mpShadowMapsCubeStatic[index], 0, 0, face, 1);
            mpFboCube->attachDepthStencilTarget(mpDepthCubeStatic[cubeDepthIdx]);
        }
        else if (meshRenderMode == RasterizerState::MeshRenderMode::Dynamic)
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
        if (meshRenderMode != RasterizerState::MeshRenderMode::Dynamic)
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
    if (mpBlurCube && (meshRenderMode != RasterizerState::MeshRenderMode::Static) )
        mpBlurCube->execute(pRenderContext, mpShadowMapsCube[index]);
    
    /* TODO doesnt work, needs fixing
    if (mShadowMapType != ShadowMapType::ShadowMap && mUseShadowMipMaps)
        mpShadowMapsCube[index]->generateMips(pRenderContext);
    */

     if (meshRenderMode == RasterizerState::MeshRenderMode::Static)
        mStaticTexturesReady[1] = true;
}

void ShadowMap::rayGenCubeEachFace(uint index, ref<Light> light, RenderContext* pRenderContext) {
     FALCOR_PROFILE(pRenderContext, "GenShadowMapPoint");

     if (index == 0)
     {
        mUpdateShadowMap |= mShadowCubeRayPass.pProgram->addDefines(getDefinesShadowMapGenPass(false));                // Update defines
        mUpdateShadowMap |= mShadowCubeRayPass.pProgram->addDefine("SMRAY_TYPE", std::to_string((uint)mShadowMapType)); // SM type define
        mUpdateShadowMap |=
            mShadowCubeRayPass.pProgram->addDefine("USE_JITTER", mJitterSamplePattern == SamplePattern::None ? "0" : "1"); // Jitter define
        // Create Program Vars
        if (!mShadowCubeRayPass.pVars)
        {
            mShadowCubeRayPass.pProgram->setTypeConformances(mpScene->getTypeConformances());
            // Create program variables for the current program.
            // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
            mShadowCubeRayPass.pVars = RtProgramVars::create(mpDevice, mShadowCubeRayPass.pProgram, mShadowCubeRayPass.pBindingTable);
        }

        dummyProfileRayTrace(pRenderContext);
     }

     auto changes = light->getChanges();
     bool renderLight = false;

     if (mShadowMapUpdateMode == SMUpdateMode::Static)
        renderLight = (changes == Light::Changes::Active) || (changes == Light::Changes::Position);
     else if (mShadowMapUpdateMode == SMUpdateMode::Dynamic)
        renderLight = true;

     renderLight |= mUpdateShadowMap;

     if (!renderLight || !light->isActive())
        return;

     auto& lightData = light->getData();

     RayShaderParameters params;
     params.lightPosition = lightData.posW;
     params.farPlane = mFar;
     params.nearPlane = mNear;

     const float4x4 projMat = math::perspective(float(M_PI_2), 1.f, mNear, mFar); // Is the same for all 6 faces

     for (size_t face = 0; face < 6; face++)
     {
        params.viewProjectionMatrix = getProjViewForCubeFace(face, lightData, projMat);
        params.invViewProjectionMatrix = math::inverse(params.viewProjectionMatrix);

        auto vars = mShadowCubeRayPass.pVars->getRootVar();
        setSMRayShaderVars(vars, params);

         // Set UAV's
        switch (mShadowMapType)
        {
        case Falcor::ShadowMapType::ShadowMap:
        case Falcor::ShadowMapType::Exponential:
            vars["gOutSM"].setUav(mpShadowMapsCube[index]->getUAV(0,face,1));
            break;
        case Falcor::ShadowMapType::Variance:
            vars["gOutSMF2"].setUav(mpShadowMapsCube[index]->getUAV(0, face, 1));
            break;
        case Falcor::ShadowMapType::ExponentialVariance:
        case Falcor::ShadowMapType::MSMHamburger:
        case Falcor::ShadowMapType::MSMHausdorff:
            vars["gOutSMF4"].setUav(mpShadowMapsCube[index]->getUAV(0, face, 1));
            break;
        }
        uint2 dispatchDims = uint2(mShadowMapSizeCube);

        mpScene->raytrace(pRenderContext, mShadowCubeRayPass.pProgram.get(), mShadowCubeRayPass.pVars, uint3(dispatchDims, 1));
     }

     // Blur if it is activated/enabled

     if (mpBlurCube)
        mpBlurCube->execute(pRenderContext, mpShadowMapsCube[index]);

     /* TODO doesnt work, needs fixing
     if (mShadowMapType != ShadowMapType::ShadowMap && mUseShadowMipMaps)
         mpShadowMapsCube[index]->generateMips(pRenderContext);
     */
}

bool ShadowMap::rasterSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered) {
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
    bool renderLight = false;
    if (mUpdateShadowMap)
        mStaticTexturesReady[0] = false;

    //Handle updates
    bool lightMoved = (changes == Light::Changes::Position) || (changes == Light::Changes::Direction);
    if (mShadowMapUpdateMode == SMUpdateMode::Static)
        renderLight = (changes == Light::Changes::Active) || lightMoved;
    else if (mShadowMapUpdateMode == SMUpdateMode::Dynamic)
        renderLight = true;

    renderLight |= mUpdateShadowMap;

    auto& lightData = light->getData();

    if (!renderLight || !light->isActive())
    {
        wasRendered[index] = false;
        return false;
    }
    wasRendered[index] = true;

    RasterizerState::MeshRenderMode meshRenderMode = RasterizerState::MeshRenderMode::All;

    //Render the static shadow map
    if (mShadowMapUpdateMode != SMUpdateMode::Static && !mStaticTexturesReady[0])
        meshRenderMode = RasterizerState::MeshRenderMode::Static;
    else if (mShadowMapUpdateMode != SMUpdateMode::Static)
        meshRenderMode = RasterizerState::MeshRenderMode::Dynamic;

    //If depth tex is set, Render to RenderTarget
    if (mpDepth)
    {
        if (meshRenderMode == RasterizerState::MeshRenderMode::Static)
        {
            // Attach Render Targets
            mpFbo->attachColorTarget(mpShadowMapsStatic[index], 0, 0, 0, 1);
            mpFbo->attachDepthStencilTarget(mpDepthStatic[index]);
        }
        else if (meshRenderMode == RasterizerState::MeshRenderMode::Dynamic)
        {
            //Copy the resources
            pRenderContext->copyResource(mpDepth.get(), mpDepthStatic[index].get());
            pRenderContext->blit(mpShadowMapsStatic[index]->getSRV(0, 1, 0, 1), mpShadowMaps[index]->getRTV(0, 0, 1));
            mpFbo->attachColorTarget(mpShadowMaps[index], 0, 0, 0, 1);
            mpFbo->attachDepthStencilTarget(mpDepth);
        }
        else
        {
            //  Attach Render Targets
            mpFbo->attachColorTarget(mpShadowMaps[index], 0, 0, 0, 1);
            mpFbo->attachDepthStencilTarget(mpDepth);
        }
        
    }
    else //Else only render to DepthStencil
    {
        if (meshRenderMode == RasterizerState::MeshRenderMode::Static)
        {
            // Attach Render Targets
            mpFbo->attachDepthStencilTarget(mpShadowMapsStatic[index]);
        }
        else if (meshRenderMode == RasterizerState::MeshRenderMode::Dynamic)
        {
            // Copy the resources
            pRenderContext->copyResource(mpShadowMaps[index].get(), mpShadowMapsStatic[index].get());
            mpFbo->attachDepthStencilTarget(mpShadowMaps[index]);
        }
        else
        {
            // Attach Render Targets
            mpFbo->attachDepthStencilTarget(mpShadowMaps[index]);
        }
    }
    
    ShaderParameters params;

    //TODO only recalculate if lightMoved
    float3 lightTarget = lightData.posW + lightData.dirW;
    const float3 up = abs(lightData.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, up);
    float4x4 projMat = math::perspective(lightData.openingAngle * 2, 1.f, mNear, mFar);

    params.lightPosition = float3(mNear, 0.f,0.f);
    params.farPlane = mFar;
    params.viewProjectionMatrix = math::mul(projMat, viewMat);     
  
    mSpotDirViewProjMat[index] = params.viewProjectionMatrix;

     // Update frustum
    if ((lightMoved || mUpdateShadowMap) && mUseFrustumCulling)
    {
        mFrustumCulling[index]->updateFrustum(lightData.posW, lightTarget, up, 1.f, lightData.openingAngle * 2, mNear, mFar);
    }

    auto vars = mShadowMapRasterPass.pVars->getRootVar();
    setSMShaderVars(vars, params);

    mShadowMapRasterPass.pState->setFbo(mpFbo);
    if (meshRenderMode != RasterizerState::MeshRenderMode::Dynamic)
    {
        float4 clearColor = float4(1.f);
        if (mShadowMapType == ShadowMapType::Exponential)
            clearColor.x = exp(mExponentialSMConstant); // Set to highest possible
        else if (mShadowMapType == ShadowMapType::ExponentialVariance)
        {
            float2 expVarMax = float2(exp(mEVSMConstant), -exp(-mEVSMNegConstant));
            clearColor = float4(expVarMax.x, expVarMax.x * expVarMax.x, expVarMax.y, expVarMax.y * expVarMax.y); // Set to highest possible
        }
        pRenderContext->clearFbo(mShadowMapRasterPass.pState->getFbo().get(), clearColor, 1.f, 0);
    }

    if (mUseFrustumCulling)
    {
        mpScene->rasterizeFrustumCulling(
            pRenderContext, mShadowMapRasterPass.pState.get(), mShadowMapRasterPass.pVars.get(), mFrontClockwiseRS[mCullMode],
            mFrontCounterClockwiseRS[mCullMode], mFrontCounterClockwiseRS[RasterizerState::CullMode::None], meshRenderMode, false,
            mFrustumCulling[index]
        );
    }else{
        mpScene->rasterize(
            pRenderContext, mShadowMapRasterPass.pState.get(), mShadowMapRasterPass.pVars.get(), mFrontClockwiseRS[mCullMode],
            mFrontCounterClockwiseRS[mCullMode], mFrontCounterClockwiseRS[RasterizerState::CullMode::None], meshRenderMode, false
        );
    }
    

    //Blur if it is activated/enabled
    if (mpBlurShadowMap && (meshRenderMode != RasterizerState::MeshRenderMode::Static))
        mpBlurShadowMap->execute(pRenderContext, mpShadowMaps[index]);

    // generate Mips for shadow map modes that allow filter
    if (mUseShadowMipMaps && (meshRenderMode != RasterizerState::MeshRenderMode::Static))
        mpShadowMaps[index]->generateMips(pRenderContext);

    if (meshRenderMode == RasterizerState::MeshRenderMode::Static)
        mStaticTexturesReady[0] = true;

    return true;
}

bool ShadowMap::rayGenSpotLight(uint index, ref<Light> light, RenderContext* pRenderContext, std::vector<bool>& wasRendered) {
    FALCOR_PROFILE(pRenderContext, "GenShadowMaps");
    if (index == 0)
    {
        mUpdateShadowMap |= mShadowMapRayPass.pProgram->addDefines(getDefinesShadowMapGenPass(false)); // Update defines
        mUpdateShadowMap |= mShadowMapRayPass.pProgram->addDefine("SMRAY_TYPE", std::to_string((uint)mShadowMapType)); // SM type define
        mUpdateShadowMap |=
            mShadowMapRayPass.pProgram->addDefine("USE_JITTER", mJitterSamplePattern == SamplePattern::None ? "0" : "1"); // Jitter define
        // Create Program Vars
        if (!mShadowMapRayPass.pVars)
        {
            mShadowMapRayPass.pProgram->setTypeConformances(mpScene->getTypeConformances());
            // Create program variables for the current program.
            // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
            mShadowMapRayPass.pVars = RtProgramVars::create(mpDevice, mShadowMapRayPass.pProgram, mShadowMapRayPass.pBindingTable);
        }
        dummyProfileRayTrace(pRenderContext);
    }
    
    auto changes = light->getChanges();
    bool renderLight = false;
    bool lightMoved = (changes == Light::Changes::Position) || (changes == Light::Changes::Direction);

    // Handle updates
    if (mShadowMapUpdateMode == SMUpdateMode::Static)
        renderLight = (changes == Light::Changes::Active) || lightMoved;
    else if (mShadowMapUpdateMode == SMUpdateMode::Dynamic)
        renderLight = true;

    renderLight |= mUpdateShadowMap;

    auto& lightData = light->getData();

    if (!renderLight || !light->isActive())
    {
        wasRendered[index] = false;
        return false;
    }
    wasRendered[index] = true;

    RayShaderParameters params;

    float3 lightTarget = lightData.posW + lightData.dirW;
    const float3 up = abs(lightData.dirW.y) == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
    float4x4 viewMat = math::matrixFromLookAt(lightData.posW, lightTarget, up);
    float4x4 projMat = math::perspective(lightData.openingAngle * 2, 1.f, mNear, mFar);

    params.lightPosition = lightData.posW;
    params.farPlane = mFar;
    params.nearPlane = mNear;
    params.viewProjectionMatrix = math::mul(projMat, viewMat);
    params.invViewProjectionMatrix = math::inverse(params.viewProjectionMatrix);

    mSpotDirViewProjMat[index] = params.viewProjectionMatrix;

    auto vars = mShadowMapRayPass.pVars->getRootVar();
    setSMRayShaderVars(vars, params);
    
    //Set UAV's
    switch (mShadowMapType)
    {
    case Falcor::ShadowMapType::ShadowMap:
    case Falcor::ShadowMapType::Exponential:
        vars["gOutSM"] = mpShadowMaps[index];
        break;
    case Falcor::ShadowMapType::Variance:
        vars["gOutSMF2"] = mpShadowMaps[index];
        break;    
    case Falcor::ShadowMapType::ExponentialVariance:
    case Falcor::ShadowMapType::MSMHamburger:
    case Falcor::ShadowMapType::MSMHausdorff:
        vars["gOutSMF4"] = mpShadowMaps[index];
        break;
    }

    uint2 dispatchDims = uint2(mShadowMapSize);

    mpScene->raytrace(pRenderContext, mShadowMapRayPass.pProgram.get(), mShadowMapRayPass.pVars, uint3(dispatchDims, 1));

    // Blur if it is activated/enabled
    if (mpBlurShadowMap)
        mpBlurShadowMap->execute(pRenderContext, mpShadowMaps[index]);

    // generate Mips for shadow map modes that allow filter
    if (mUseShadowMipMaps)
        mpShadowMaps[index]->generateMips(pRenderContext);

    return true;
}

 //Calc based on https://learnopengl.com/Guest-Articles/2021/CSM
void ShadowMap::calcProjViewForCascaded(uint index ,const LightData& lightData, std::vector<bool>& renderLevel) {
   
    auto& sceneBounds = mpScene->getSceneBounds();
    auto camera = mpScene->getCamera();
    const auto& cameraData = mpScene->getCamera()->getData();

    if (mCascadedFirstThisFrame)
    {
        //Calc the cascaded far value
        mCascadedMaxFar = std::min(sceneBounds.radius() * 2, camera->getFarPlane()); // Clamp Far to scene bounds

        //Check if the size of the array is still right
        if ((mCascadedZSlices.size() != mCascadedLevelCount))
        {
            mCascadedZSlices.clear();
            mCascadedZSlices.resize(mCascadedLevelCount);
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
                std::vector<float> cascadedSlices(mCascadedLevelCount + mCascadedTracedLevelsAtEnd);
                const uint N = mCascadedLevelCount + mCascadedTracedLevelsAtEnd;
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
        
        mCascadedFirstThisFrame = false;
    }

    
    uint startIdx = index * mCascadedLevelCount;    //Get start index in vector

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

        //Get Box for Orto
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        for (const float4& p : frustumCorners){
            const float4 vp = math::mul(casView, p);
            minX = std::min(minX, vp.x);
            maxX = std::max(maxX, vp.x);
            minY = std::min(minY, vp.y);
            maxY = std::max(maxY, vp.y);
            minZ = std::min(minZ, vp.z);
            maxZ = std::max(maxZ, vp.z);
        }

        if (minZ < 0)
        {
            minZ *= mCascZMult;
        }
        else
        {
            minZ /= mCascZMult;
        }
        if (maxZ < 0)
        {
            maxZ /= mCascZMult;
        }
        else
        {
            maxZ *= mCascZMult;
        }

        renderLevel[i] = false;

        //Check if the current cascade box fit into the box from last frame
        if (mPreviousCascades[startIdx + i].valid){
            //Get the AABB from the stored view
            float3 minRepro = float3(std::numeric_limits<float>::max());
            float3 maxRepro = float3(std::numeric_limits<float>::min());
            for (const float4& p : frustumCorners)
            {
                const float4 vp = math::mul(mPreviousCascades[i].prevView, p);
                minRepro.x = std::min(minRepro.x, vp.x);
                maxRepro.x = std::max(maxRepro.x, vp.x);
                minRepro.y = std::min(minRepro.y, vp.y);
                maxRepro.y = std::max(maxRepro.y, vp.y);
                minRepro.z = std::min(minRepro.z, vp.z);
                maxRepro.z = std::max(maxRepro.z, vp.z);
            }

            //Pull back based on ZMult
            if (minRepro.z < 0)
            {
                minRepro.z *= mCascZMult;
            }
            else
            {
                minRepro.z /= mCascZMult;
            }
            if (maxRepro.z < 0)
            {
                maxRepro.z /= mCascZMult;
            }
            else
            {
                maxRepro.z *= mCascZMult;
            }

            //Test both points against the enlarged box. If the box is inside, skip calculation for this level
            const float3& minPrev = mPreviousCascades[startIdx + i].min;
            const float3& maxPrev = mPreviousCascades[startIdx + i].max;
            if (math::all(minRepro >= minPrev && minRepro <= maxPrev) && math::all(maxRepro >= minPrev && maxRepro <= maxPrev))
                continue;
        }

        //Enlarge the box and set the previous cascade
        minX += minX * mCascadedReuseEnlargeFactor;
        maxX += maxX * mCascadedReuseEnlargeFactor;
        minY += minY * mCascadedReuseEnlargeFactor;
        maxY += maxY * mCascadedReuseEnlargeFactor;
        minZ += minZ * mCascadedReuseEnlargeFactor;
        maxZ += maxZ * mCascadedReuseEnlargeFactor;

        mPreviousCascades[startIdx + i].valid = true;
        mPreviousCascades[startIdx + i].prevView = casView;
        mPreviousCascades[startIdx + i].min = float3(minX, minY, minZ);
        mPreviousCascades[startIdx + i].max = float3(maxX, maxY, maxZ);
        renderLevel[i] = true;

        const float4x4 casProj = math::ortho(minX, maxX,  minY, maxY, minZ, maxZ);

        mCascadedWidthHeight[startIdx * mCascadedLevelCount + i] = float2(abs(maxX - minX), abs(maxY - minY));
        mCascadedVPMatrix[startIdx * mCascadedLevelCount + i] = math::mul(casProj, casView);

        //Update Frustum Culling
        if (mUseFrustumCulling)
        {
            const uint cullingIndex = mFrustumCullingVectorOffsets.x + index * mCascadedLevelCount + i; //i is cascaded level
            mFrustumCulling[cullingIndex]->updateFrustum(center, center + lightData.dirW, upVec, minX, maxX, minY, maxY, minZ, maxZ);
        }

        near = mCascadedZSlices[i];
    }        
}

bool ShadowMap::rasterCascaded(uint index, ref<Light> light, RenderContext* pRenderContext, bool cameraMoved)
{
    FALCOR_PROFILE(pRenderContext, "GenCascadedShadowMaps");
    if (index == 0)
    {
        mUpdateShadowMap |= mShadowMapCascadedRasterPass.pState->getProgram()->addDefines(getDefinesShadowMapGenPass()); // Update defines
        // Create Program Vars
        if (!mShadowMapCascadedRasterPass.pVars)
        {
            mShadowMapCascadedRasterPass.pVars = GraphicsVars::create(mpDevice, mShadowMapCascadedRasterPass.pProgram.get());
        }

        dummyProfileRaster(pRenderContext); // Show the render scene every frame
    }

    bool dynamicMode = mShadowMapUpdateMode != SMUpdateMode::Static;

    if (!cameraMoved && !mUpdateShadowMap && !dynamicMode)
        return false;

    auto& lightData = light->getData();

    if (index == 0)
        mCascadedFirstThisFrame = true;

    if ( !light->isActive())
    {
        return false;
    } 

    // Update viewProj
    std::vector<bool> renderCascadedLevel(mCascadedLevelCount);
    calcProjViewForCascaded(index, lightData, renderCascadedLevel);

    uint casMatIdx = index * mCascadedLevelCount;

    // Render each cascade
    const uint loopCount = dynamicMode ? mCascadedLevelCount * 2 : mCascadedLevelCount;
    for (uint i = 0; i < loopCount; i++)
    {
        const uint cascLevel = dynamicMode ? i / 2 : i;
        const bool isDynamic = dynamicMode ? (i % 2 == 1) : false; // Uneven number is the dynamic pass

        //Skip static cascaded levels if no update is necessary
        if (!renderCascadedLevel[cascLevel] && !isDynamic)
            continue;

        // If depth tex is set, Render to RenderTarget
        if (mpDepthCascaded)
        {
            if (dynamicMode)
            {
                if (isDynamic)
                {
                    //Copy static tex
                    pRenderContext->copyResource(mpDepthCascaded.get(), mpDepthCascadedStatic[casMatIdx + cascLevel].get());
                    pRenderContext->blit(
                        mpShadowMapsCascadedStatic[index]->getSRV(0, 1, cascLevel, 1), mpCascadedShadowMaps[index]->getRTV(0, cascLevel, 1)
                    );
                    //Bind FBO
                    mpFboCascaded->attachColorTarget(mpCascadedShadowMaps[index], 0, 0, cascLevel, 1);
                    mpFboCascaded->attachDepthStencilTarget(mpDepthCascaded);
                }
                else
                {
                    mpFboCascaded->attachColorTarget(mpShadowMapsCascadedStatic[index], 0, 0, cascLevel, 1);
                    mpFboCascaded->attachDepthStencilTarget(mpDepthCascadedStatic[casMatIdx + cascLevel]);
                }
            }
            else //Static Geometry only mode
            {
                mpFboCascaded->attachColorTarget(mpCascadedShadowMaps[index], 0, 0, cascLevel, 1);
                mpFboCascaded->attachDepthStencilTarget(mpDepthCascaded);
            }
        }
        else //Else only render to DepthStencil
        {
            if (dynamicMode)
            {
                if (isDynamic)
                {
                    // Copy static tex
                    pRenderContext->blit(
                        mpShadowMapsCascadedStatic[index]->getSRV(0, 1, cascLevel, 1), mpCascadedShadowMaps[index]->getRTV(0, cascLevel, 1)
                    );
                    mpFboCascaded->attachDepthStencilTarget(mpCascadedShadowMaps[index], 0, cascLevel, 1);
                }
                else
                {
                    mpFboCascaded->attachDepthStencilTarget(mpShadowMapsCascadedStatic[index], 0, cascLevel, 1);
                }
            }
            else //Static Geometry only mode
            {
                mpFboCascaded->attachDepthStencilTarget(mpCascadedShadowMaps[index], 0, cascLevel, 1);
            }
        }
       
        ShaderParameters params;
        params.lightPosition = lightData.posW;
        params.farPlane = mFar;
        params.viewProjectionMatrix = mCascadedVPMatrix[casMatIdx + cascLevel];

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

        if (!isDynamic)
            pRenderContext->clearFbo(mShadowMapCascadedRasterPass.pState->getFbo().get(), clearColor, 1.f, 0);

        //Set mesh render mode
        auto meshRenderMode = RasterizerState::MeshRenderMode::All;
        if (dynamicMode)
            meshRenderMode = isDynamic ? RasterizerState::MeshRenderMode::Dynamic : RasterizerState::MeshRenderMode::Static;

        if (mUseFrustumCulling)
        {
            const uint cullingIndex = mFrustumCullingVectorOffsets.x + index * mCascadedLevelCount + cascLevel;
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

    // Blur if it is activated/enabled
    if (mpBlurCascaded)
        mpBlurCascaded->execute(pRenderContext, mpCascadedShadowMaps[index]);

    // generate Mips for shadow map modes that allow filter
    if (mUseShadowMipMaps)
        mpCascadedShadowMaps[index]->generateMips(pRenderContext);

    return true;
}

bool ShadowMap::rayGenCascaded(uint index, ref<Light> light, RenderContext* pRenderContext, bool cameraMoved)
{
    FALCOR_PROFILE(pRenderContext, "GenCascadedShadowMaps");
    if (index == 0)
    {
        mUpdateShadowMap |= mShadowMapCascadedRayPass.pProgram->addDefines(getDefinesShadowMapGenPass(false));                 // Update defines
        mUpdateShadowMap |= mShadowMapCascadedRayPass.pProgram->addDefine("SMRAY_TYPE", std::to_string((uint)mShadowMapType)); // SM type define
        mUpdateShadowMap |=
            mShadowMapRayPass.pProgram->addDefine("USE_JITTER", mJitterSamplePattern == SamplePattern::None ? "0" : "1"); // Jitter define
        // Create Program Vars
        if (!mShadowMapCascadedRayPass.pVars)
        {
            mShadowMapCascadedRayPass.pProgram->setTypeConformances(mpScene->getTypeConformances());
            // Create program variables for the current program.
            // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
            mShadowMapCascadedRayPass.pVars =
                RtProgramVars::create(mpDevice, mShadowMapCascadedRayPass.pProgram, mShadowMapCascadedRayPass.pBindingTable);
        }
        dummyProfileRayTrace(pRenderContext);
    }

    if (!cameraMoved && !mUpdateShadowMap)
        return false;

    auto& lightData = light->getData();

    if (index == 0)
        mCascadedFirstThisFrame = true;

    if (!light->isActive())
    {
        return false;
    }

    // Update viewProj
    std::vector<bool> renderCascadedLevel(mCascadedLevelCount);
    calcProjViewForCascaded(index, lightData, renderCascadedLevel);

    uint casMatIdx = index * mCascadedLevelCount;

    RayShaderParameters params;
    params.lightPosition = lightData.dirW;
    params.farPlane = mFar;
    params.nearPlane = mNear;

    // Render each cascade
    for (uint cascLevel = 0; cascLevel < mCascadedLevelCount; cascLevel++)
    {
        if (!renderCascadedLevel[cascLevel])
            continue;

        params.viewProjectionMatrix = mCascadedVPMatrix[casMatIdx + cascLevel];
        params.invViewProjectionMatrix = math::inverse(mCascadedVPMatrix[casMatIdx + cascLevel]);

        auto vars = mShadowMapCascadedRayPass.pVars->getRootVar();
        setSMRayShaderVars(vars, params);

        // Set UAV's
        switch (mShadowMapType)
        {
        case Falcor::ShadowMapType::ShadowMap:
        case Falcor::ShadowMapType::Exponential:
            vars["gOutSM"].setUav(mpCascadedShadowMaps[index]->getUAV(0,cascLevel,1));
            break;
        case Falcor::ShadowMapType::Variance:
            vars["gOutSMF2"].setUav(mpCascadedShadowMaps[index]->getUAV(0, cascLevel, 1));
            break;
        case Falcor::ShadowMapType::ExponentialVariance:
        case Falcor::ShadowMapType::MSMHamburger:
        case Falcor::ShadowMapType::MSMHausdorff:
            vars["gOutSMF4"].setUav(mpCascadedShadowMaps[index]->getUAV(0, cascLevel, 1));
            break;
        }

        uint2 dispatchDims = uint2(mShadowMapSizeCascaded);

        mpScene->raytrace(
            pRenderContext, mShadowMapCascadedRayPass.pProgram.get(), mShadowMapCascadedRayPass.pVars, uint3(dispatchDims, 1)
        );
    }

    // Blur if it is activated/enabled
    if (mpBlurCascaded)
        mpBlurCascaded->execute(pRenderContext, mpCascadedShadowMaps[index]);

    // generate Mips for shadow map modes that allow filter
    if (mUseShadowMipMaps)
        mpCascadedShadowMaps[index]->generateMips(pRenderContext);

    return true;
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

    //Create a jitter sample generator
    if (!mpCPUJitterSampleGenerator)
    {
        updateJitterSampleGenerator();
    }

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
    {
        if (mUseRaySMGen)
            rayGenCubeEachFace(i, lightRenderListCube[i], pRenderContext);
        else
            rasterCubeEachFace(i, lightRenderListCube[i], pRenderContext);
    }

    // Spot/Directional Lights
    std::vector<bool> wasRendered(lightRenderListMisc.size());
    bool updateVPBuffer = false;
    // Render all spot / directional lights
    for (size_t i = 0; i < lightRenderListMisc.size(); i++)
    {
        if (mUseRaySMGen)
            updateVPBuffer |= rayGenSpotLight(i, lightRenderListMisc[i], pRenderContext, wasRendered);
        else
            updateVPBuffer |= rasterSpotLight(i, lightRenderListMisc[i], pRenderContext, wasRendered);
    }

    //Render cascaded
    updateVPBuffer |= lightRenderListCascaded.size() > 0;
    bool cascFirstThisFrame = true;
    const auto& camera = mpScene->getCamera();
    auto cameraChanges = camera->getChanges();
    auto excluded = Camera::Changes::Jitter | Camera::Changes::History;
    bool cameraMoved = (cameraChanges & ~excluded) != Camera::Changes::None;

    for (size_t i = 0; i < lightRenderListCascaded.size(); i++)
    {
        if (mUseRaySMGen)
            updateVPBuffer |= rayGenCascaded(i, lightRenderListCascaded[i], pRenderContext,cameraMoved);
        else
            updateVPBuffer |= rasterCascaded(i, lightRenderListCascaded[i], pRenderContext, cameraMoved);
    }
    

    // Write all ViewProjectionMatrix to the buffer
    // TODO optimize this depending on the number of active lights
    if (updateVPBuffer)
    {
        static uint stagingCount = 0;

        //Update staging values
        mStagingFenceWaitValues[stagingCount] = mpScene->getLastFrameFenceValue();
        stagingCount = (stagingCount + 1) % kStagingBufferCount;

        size_t totalSize = mpShadowMaps.size() + mpCascadedShadowMaps.size() * mCascadedLevelCount;
        auto& fenceWaitVal = mStagingFenceWaitValues[stagingCount];
        const uint stagingOffset = totalSize * stagingCount;

        //Wait for the GPU to finish copying from kStagingFramesInFlight frames back
        mpScene->getFence()->syncCpu(fenceWaitVal);

        float4x4* mats = (float4x4*)mpVPMatrixStangingBuffer->map(Buffer::MapType::Write);
        for (size_t i = 0; i < mSpotDirViewProjMat.size(); i++)
        {
            if (!wasRendered[i])
                continue;
            mats[stagingOffset + i] = mSpotDirViewProjMat[i];
        }

        for (size_t i = 0; i < mpCascadedShadowMaps.size() * mCascadedLevelCount; i++)
        {
            mats[stagingOffset + mCascadedMatrixStartIndex + i] = mCascadedVPMatrix[i];
        }

        pRenderContext->copyBufferRegion(
            mpVPMatrixBuffer.get(), 0, mpVPMatrixStangingBuffer.get(), sizeof(float4x4) * stagingOffset, sizeof(float4x4) * totalSize
        );
    }

    handleNormalizedPixelSizeBuffer();

    mUpdateShadowMap = false;
    return true;
}

bool ShadowMap::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    mResetShadowMapBuffers |= widget.checkbox("Generate: Use Ray Tracing", mUseRaySMGen);
    widget.tooltip("Uses a ray tracing shader to generate the shadow maps");
    if (!mUseRaySMGen)
    {
        mResetShadowMapBuffers |= widget.checkbox("Use FrustumCulling", mUseFrustumCulling);
        widget.tooltip("Enables Frustum Culling for the shadow map generation");
    }
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
        case Falcor::ShadowMapType::Variance:
        case Falcor::ShadowMapType::Exponential:
            mBias = 0;
            mSlopeBias = 0.f;
            mSMCubeWorldBias = 0.f;
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
        widget.dropdown("Update Mode", kShadowMapUpdateModeDropdownList, (uint&)mShadowMapUpdateMode);
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

    dirty |= widget.checkbox("Use Hybrid SM", mUseHybridSM);
    widget.tooltip("Enables Hybrid Shadow Maps, where the edge of the shadow map is traced", true);

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
                if (mpCascadedShadowMaps.size() > 0 || mpShadowMaps.size() > 0)
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
    {
        if (auto group = widget.group("Variance Shadow Map Options"))
        {
            dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
            dirty |= group.checkbox("Use Min Shadow Value", mUseMinShadowValue);
            group.tooltip("Enables a minimum allowed shadow value. Every shadow value gets reduced to 0. Prevents some light leaking with the cost of reducing the soft shadow effect");
            dirty |= group.var("Min Shadow Value", mMinShadowValueVal, 0.f , 1.f, 0.0001f);
            group.tooltip("Minimal Shadow Value");
            dirty |= group.checkbox("Variance SelfShadow Variant", mVarianceUseSelfShadowVariant);
            group.tooltip("Uses part of ddx and ddy depth in variance calculation. Should not be used with Blur!. Only enabled in rasterize shadow map mode.");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip("Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [TH.x, TH.y]", true);
            if (mHSMFilteredThreshold.x > mHSMFilteredThreshold.y)
                mHSMFilteredThreshold.y = mHSMFilteredThreshold.x;
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
    {
        if (auto group = widget.group("Exponential Variance Shadow Map Options"))
        {
            dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
            dirty |= group.var("Exponential Constant", mEVSMConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for exponential shadow map");
            dirty |= group.var("Exponential Negative Constant", mEVSMNegConstant, 1.f, kEVSM_ExponentialConstantMax, 0.1f);
            group.tooltip("Constant for the negative part");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [x, y]", true
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
    case ShadowMapType::MSMHamburger:
    case ShadowMapType::MSMHausdorff:
    {
        if (auto group = widget.group("Moment Shadow Maps Options"))
        {
            dirty |= group.var("Depth Bias (x1000)", mMSMDepthBias, 0.f, 10.f, 0.0001f);
            group.tooltip("Depth bias subtracted from the depth value the moment shadow map is tested against");
            dirty |= group.var("Moment Bias (x1000)", mMSMMomentBias, 0.f, 10.f, 0.0001f);
            group.tooltip("Moment bias which pulls all values a bit to 0.5");
            dirty |= group.checkbox("Enable Blur", mUseGaussianBlur);
            dirty |= group.checkbox("Use Min Shadow Value", mUseMinShadowValue);
            group.tooltip(
                "Enables a minimum allowed shadow value. Every shadow value gets reduced to 0. Prevents some light leaking with the cost "
                "of reducing the soft shadow effect"
            );
            dirty |= group.var("Min Shadow Value", mMinShadowValueVal, 0.f, 1.f, 0.0001f);
            group.tooltip("Minimal Shadow Value");
            dirty |= group.var("HSM Filterd Threshold", mHSMFilteredThreshold, 0.0f, 1.f, 0.001f);
            group.tooltip(
                "Threshold used for filtered SM variants when a ray is needed. Ray is needed if shadow value between [x, y]", true
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
    default:;
    }
    
    if (mpCascadedShadowMaps.size() > 0)
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
                    group.var("Virtual RayTraced Casc Level at end", mCascadedTracedLevelsAtEnd, 0u, 8u, 1u);
                    group.tooltip(
                        "Adds N virtual cascaded levels that will be fully ray traced. Does not generate extra shadow maps.\n \" Use Ray "
                        "outside "
                        "Shadow Map \" needs to be activated for the virtual cascaded levels to work"
                    );
                    dirty |= group.var("Z Slize Exp influence", mCascadedFrustumFix, 0.f, 1.f, 0.001f);
                    group.tooltip("Influence of the Exponentenial part in the zSlice calculation. (1-Value) is used for the linear part");
                }
                break;
            default:
                break;
            }
           
            dirty |= group.var("Z Value Multi", mCascZMult, 1.f, 1000.f, 0.1f);
            group.tooltip("Pulls the Z-Values of each cascaded level apart. Is needed as not all Geometry is in the View-Frustum");
            dirty |= group.var("Reuse Enlarge Factor", mCascadedReuseEnlargeFactor, 0.f, 10.f, 0.001f);
            group.tooltip("Factor by which the frustum of each cascaded level is enlarged by");
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
                    blurSettingsChanged |= mpBlurCascaded->renderUI(group2);
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

    if (auto group = widget.group("Jitter Options"))
    {
        bool jitterChanged = group.dropdown("Jitter Sample Mode", kJitterModeDropdownList, (uint&)mJitterSamplePattern);
        if (mJitterSamplePattern != SamplePattern::None)
        {
            if ((mJitterSamplePattern != SamplePattern::DirectX))
            {
                jitterChanged |= group.var("Sample Count", mJitterSampleCount, 1u, 1024u, 1u);
                group.tooltip("Sets the sample count for the jitter generator");
            }
            
            jitterChanged |= group.var("Temporal Filter Strength", mTemporalFilterLength, 1u, 64u, 1u);
            group.tooltip("Number of frames used for the temporal filter");
        }

        if (jitterChanged)
            updateJitterSampleGenerator();
    }

    if (auto group = widget.group("Oracle Options"))
    {
        dirty |= group.checkbox("Use Oracle Function", mUseSMOracle);
        group.tooltip("Enables the oracle function for Shadow Mapping", true);
        if (mUseSMOracle)
        {
            dirty |= group.var("Oracle Compaire Value", mOracleCompaireValue, 0.f, 64.f, 0.1f);
            group.tooltip("Compaire Value for the Oracle function. Is basically compaired against ShadowMapPixelArea/CameraPixelArea.");
            if (mUseHybridSM)
            {
                dirty |= group.var("Oracle Upper Bound", mOracleCompaireUpperBound, mOracleCompaireValue, 2048.f, 0.1f);
                group.tooltip(
                    "Upper Bound for the oracle value. If oracle is above this value the shadow test is skipped and an ray is shot."
                );
            }
            dirty |= group.dropdown("Oracle Distance Mode", mOracleDistanceFunctionMode);
            group.tooltip("Mode for the distance factor applied on bounces.");

            if (mUseHybridSM)
            {
                dirty |= group.checkbox("Ignore Oracle on direct hit", mOracleIgnoreDirect);
                group.tooltip("Ignores the oracle on direct and very specular hits");
                if (mOracleIgnoreDirect)
                {
                    dirty |= group.var("Ignore Oracle Roughness", mOracleIgnoreDirectRoughness, 0.f, 1.f, 0.0001f);
                    group.tooltip("The roughness that defines the very specular hits for the ignore oracle function");
                }                 
            }
        }
    }

    dirty |= mRasterDefinesChanged;
    dirty |= mResetShadowMapBuffers;

    return dirty;
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

float ShadowMap::getNormalizedPixelSizeOrtho(uint2 frameDim, float width, float height)
{

    float wPix = width / frameDim.x;
    float hPix = height / frameDim.y;
    return wPix * hPix;
}

void ShadowMap::handleNormalizedPixelSizeBuffer()
{
    //If buffer is build, no need to rebuild it
    if (mpNormalizedPixelSize)
        return;

    //Get the NPS(Normalized Pixel Size) for each light type
    std::vector<float> pointNPS;
    std::vector<float> spotNPS;
    std::vector<float> dirNPS;

    uint cascadedCount = 0;
    for (ref<Light> light : mpScene->getLights())
    {
        auto type = getLightType(light);
        switch (type)
        {
        case LightTypeSM::Point:
            pointNPS.push_back(getNormalizedPixelSize(uint2(mShadowMapSizeCube), float(M_PI_2), 1.f));
            break;
        case LightTypeSM::Spot:
            spotNPS.push_back(getNormalizedPixelSize(uint2(mShadowMapSize), light->getData().openingAngle * 2, 1.f));
            break;
        case LightTypeSM::Directional:
            for (uint i = 0; i < mCascadedLevelCount; i++)
            {
                float2 wh = mCascadedWidthHeight[cascadedCount * mCascadedLevelCount + i];
                dirNPS.push_back(getNormalizedPixelSizeOrtho(uint2(mShadowMapSizeCascaded), wh.x, wh.y)
                );
            }
            cascadedCount++;
            break;
        }
    }

    mNPSOffsets = uint2(pointNPS.size(), pointNPS.size() + spotNPS.size());
    size_t totalSize = mNPSOffsets.y + dirNPS.size();

    std::vector<float> npsData;
    npsData.reserve(totalSize);
    for (auto nps : pointNPS)
        npsData.push_back(nps);
    for (auto nps : spotNPS)
        npsData.push_back(nps);
    for (auto nps : dirNPS)
        npsData.push_back(nps);

    //Create the buffer
    mpNormalizedPixelSize = Buffer::createStructured(
        mpDevice, sizeof(float), npsData.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, npsData.data(), false
    );
    mpNormalizedPixelSize->setName("ShadowMap::NPSBuffer");
}

void ShadowMap::dummyProfileRaster(RenderContext* pRenderContext) {
    FALCOR_PROFILE(pRenderContext, "rasterizeScene");
}

void ShadowMap::dummyProfileRayTrace(RenderContext* pRenderContext) {
    FALCOR_PROFILE(pRenderContext, "raytraceScene");
}

void ShadowMap::updateJitterSampleGenerator() {
    switch (mJitterSamplePattern)
    {
    case Falcor::ShadowMap::SamplePattern::None:
        mpCPUJitterSampleGenerator = HaltonSamplePattern::create(1);    //So a sample generator is intitialized
        break;
    case Falcor::ShadowMap::SamplePattern::DirectX:
        mpCPUJitterSampleGenerator = DxSamplePattern::create();     //Has a fixed sample size of 8
        break;
    case Falcor::ShadowMap::SamplePattern::Halton:
        mpCPUJitterSampleGenerator = HaltonSamplePattern::create(mJitterSampleCount); // So a sample generator is intitialized
        break;
    case Falcor::ShadowMap::SamplePattern::Stratified:
        mpCPUJitterSampleGenerator = StratifiedSamplePattern::create(mJitterSampleCount);
        break;
    default:
        FALCOR_UNREACHABLE();
        break;
    }
}

void ShadowMap::RayTraceProgramHelper::initRTProgram(ref<Device> device,ref<Scene> scene, const std::string& shaderName, DefineList& defines, const Program::TypeConformanceList& globalTypeConformances)
{
    RtProgram::Desc desc;
    desc.addShaderModules(scene->getShaderModules());
    desc.addShaderLibrary(shaderName);
    desc.setMaxPayloadSize(kRayPayloadMaxSize);
    desc.setMaxAttributeSize(scene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(1);
    if (!scene->hasProceduralGeometry())
        desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

    pBindingTable = RtBindingTable::create(1, 1, scene->getGeometryCount());
    auto& sbt = pBindingTable;
    sbt->setRayGen(desc.addRayGen("rayGen", globalTypeConformances)); //TODO globalTypeConformances necessary? 
    sbt->setMiss(0, desc.addMiss("miss"));

    if (scene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(0, scene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
    }

    pProgram = RtProgram::create(device, desc, defines);
}

}
