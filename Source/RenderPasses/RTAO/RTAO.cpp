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
#include "RTAO.h"

#include <glm/gtc/random.hpp>

namespace
{
    const char kDesc[] = "Ray Traced (noisy) AO";

    //In
    const std::string kWPos = "wPos";
    const std::string kFaceNormal = "faceNormal";
    
    //Out
    const std::string kAmbient = "ambient";
    const std::string kRayDistance = "rayDistance";

    const std::string kRayShader = "RenderPasses/RTAO/Ray.rt.slang";
    const uint32_t kMaxPayloadSize = 4;
}

const RenderPass::Info RTAO::kInfo { "RTAO", kDesc };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(RTAO::kInfo, RTAO::create);
}

RTAO::SharedPtr RTAO::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new RTAO());
    return pPass;
}

Dictionary RTAO::getScriptingDictionary()
{
    return Dictionary();
}

RTAO::RTAO() : RenderPass(kInfo)
{
    mpSamplesTex = genSamplesTexture(5312);
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

RenderPassReflection RTAO::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kWPos, "world position");
    reflector.addInput(kFaceNormal, "world space face normals");
    reflector.addOutput(kAmbient, "ambient map").format(ResourceFormat::R8Unorm);
    reflector.addOutput(kRayDistance, "distance of the ambient ray").format(ResourceFormat::R16Float);
    return reflector;
}

void RTAO::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mRayProgram.reset();
    mDirty = true;
}

void RTAO::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    auto pWPos = renderData[kWPos]->asTexture();
    auto pFaceNormal = renderData[kFaceNormal]->asTexture();
    auto pAmbient = renderData[kAmbient]->asTexture();
    auto pRayDistance = renderData[kRayDistance]->asTexture();

    if(!mEnabled)
    {
        pRenderContext->clearTexture(pAmbient.get(), float4(1.0f));
        return;
    }

    if(!mRayProgram)
    {
        Program::DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());

        RtProgram::Desc desc;
        desc.addShaderLibrary(kRayShader);
        desc.setMaxPayloadSize(kMaxPayloadSize);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        desc.addTypeConformances(mpScene->getTypeConformances());

        RtBindingTable::SharedPtr sbt = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));
        sbt->setHitGroup(0, mpScene->getGeometryIDs(GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        // TODO add remaining primitives
        mRayProgram = RtProgram::create(desc, defines);
        mRayVars = RtProgramVars::create(mRayProgram, sbt);
        mDirty = true;

        // set constants
        mpSampleGenerator->setShaderData(mRayVars->getRootVar());
        mRayVars["gSamples"] = mpSamplesTex;
    }

    if(mDirty)
    {
        // Calculate a theoretical max ray distance to be used in occlusion factor computation.
        // Occlusion factor of a ray hit is computed based of its ray hit time, falloff exponent and a max ray hit time.
        // By specifying a min occlusion factor of a ray, we can skip tracing rays that would have an occlusion 
        // factor less than the cutoff to save a bit of performance (generally 1-10% perf win without visible AO result impact).
        // Therefore the sample discerns between true maxRayHitTime, used in TraceRay, 
        // and a theoretical one used in calculating the occlusion factor on a hit.
        {
            float lambda = mData.exponentialFalloffDecayConstant;
            // Invert occlusionFactor = exp(-lambda * t * t), where t is tHit/tMax of a ray.
            float t = sqrt(logf(mMinOcclusionCutoff) / -lambda);

            mData.maxAORayTHit = mData.applyExponentialFalloff ? t * mMaxTHit : mMaxTHit;
            mData.maxTheoreticalTHit = mMaxTHit;
        }
        mRayVars["StaticCB"].setBlob(mData);
        mDirty = false;
    }

    // per frame data
    auto pCamera = mpScene->getCamera().get();
    pCamera->setShaderData(mRayVars["PerFrameCB"]["gCamera"]);
    mRayVars["PerFrameCB"]["frameIndex"] = frameIndex++;

    // resources
    mRayVars["gWPosTex"] = pWPos;
    mRayVars["gFaceNormalTex"] = pFaceNormal;
    mRayVars["ambientOut"] = pAmbient;
    mRayVars["rayDistanceOut"] = pRayDistance;

    mpScene->raytrace(pRenderContext, mRayProgram.get(), mRayVars, uint3(pAmbient->getWidth(), pAmbient->getHeight(), 1));
}

void RTAO::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    widget.checkbox("Enabled", mEnabled);
    if (!mEnabled) return;
    dirty |= widget.var("Ray Max THit", mMaxTHit, 0.01f, FLT_MAX, 0.01f);
    widget.tooltip("Max THit value. Real value is dependent on Occlusion Cutoff and Decay Constant if exponential falloff is enabled");
    dirty |= widget.checkbox("Exponential falloff", mData.applyExponentialFalloff);
    if (mData.applyExponentialFalloff) {
        dirty |= widget.var("Occlusion Cutoff", mMinOcclusionCutoff, 0.f, 1.f, 0.01f);
        widget.tooltip("Cutoff for occlusion. Cutts of the end of the ray, as they contribute little in exponential falloff");
        dirty |= widget.var("Falloff decay constant", mData.exponentialFalloffDecayConstant, 0.f, 20.f, 0.1f);
    }
    dirty |= widget.var("min Ambient Illumination", mData.minimumAmbientIllumination, 0.0f, 1.0f, 0.001f);

    dirty |= widget.var("spp", mData.spp, 1u, UINT32_MAX, 1u);
    widget.tooltip("Numbers of ray per pixel. If higher than 1 a slower sample generator is used");

    mDirty = dirty;
}

void RTAO::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mRayProgram.reset();
}

Texture::SharedPtr RTAO::genSamplesTexture(uint size)
{
    std::vector<uint32_t> data;
    data.resize(size);

    std::srand(89); // always use the same seed for the noise texture (linear rand uses std rand)
    for (auto& dat : data)
    {
        float2 uv = glm::linearRand(float2(0.0f), float2(1.0f));
        // Map to radius 1 hemisphere TODO verify
        float phi = uv.y * 2.0f * (float)M_PI;
        float t = std::sqrt(1.0f - uv.x);
        float s = std::sqrt(1.0f - t * t);
        float4 dir = float4(s * std::cos(phi), s * std::sin(phi), t, 0.0f);

        dat = glm::packSnorm4x8(dir);
    }

    return Texture::create1D(size, ResourceFormat::RGBA8Snorm, 1, 1, data.data());
}
