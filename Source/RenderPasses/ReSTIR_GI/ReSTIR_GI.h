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
#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
// Light samplers
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Rendering/Lights/EmissiveUniformSampler.h"

#include "Rendering/RTXDI/RTXDI.h"

using namespace Falcor;

class ReSTIR_GI : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_GI, "ReSTIR_GI", "Indirect light with Resampled Path Tracing");

    static ref<ReSTIR_GI> create(ref<Device> pDevice, const Properties& props) { return make_ref<ReSTIR_GI>(pDevice, props); }

    ReSTIR_GI(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    // GUI Structs and enum
    enum class ResamplingMode : uint
    {
        Temporal = 0u,
        Spartial = 1u,
        SpartioTemporal = 2u
    };

    enum class BiasCorrectionMode : uint
    {
        Off = 0,
        Basic = 1u,
        RayTraced = 2u
    };

    // TODO add analytic/ random mode
    enum class DirectLightingMode : uint
    {
        None = 0u,
        RTXDI = 1u
    };

private:
    /** Prepares the samplers etc needed for lighting. Returns true if lighting has changed
     */
    bool prepareLighting(RenderContext* pRenderContext);

    /** Prepare all buffers needed
     */
    void prepareBuffers(RenderContext* pRenderContext, const RenderData& renderData);

    /** Initializes all the ray tracing shaders
     */
    void prepareRayTracingShaders(RenderContext* pRenderContext);

    /** Trace Tranmissive and delta materials
     */
    void traceTransmissiveDelta(RenderContext* pRenderContext, const RenderData& renderData);

    /** Trace Scene for final gather hit
     */
    void generatePathSamplesPass(RenderContext* pRenderContext, const RenderData& renderData);

    /** Resampling pass, which resamples the generated sampled based on the resampling mode
     */
    void resamplingPass(RenderContext* pRenderContext, const RenderData& renderData);

    /** Final Shading
     */
    void finalShadingPass(RenderContext* pRenderContext, const RenderData& renderData);

    /** Copies the view direction for the previous view texture that is used next frame
     */
    void copyViewTexture(RenderContext* pRenderContext, const RenderData& renderData);

    /** Returs quad texture dimensions depending on the number of elements needed
     */
    void computeQuadTexSize(uint maxItems, uint& outWidth, uint& outHeight);

    //
    // Constants
    //
    const ResourceFormat kViewDirFormat = ResourceFormat::RGBA32Float; // View Dir format

    //
    // Pointers
    //
    ref<Scene> mpScene;                     // Scene Pointer
    ref<SampleGenerator> mpSampleGenerator; // GPU Sample Gen
    std::unique_ptr<RTXDI> mpRTXDI;         // Ptr to RTXDI for direct use
    RTXDI::Options mRTXDIOptions;           // Options for RTXDI

    EmissiveLightSamplerType mEmissiveType = EmissiveLightSamplerType::LightBVH;
    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    LightBVHSampler::Options mLightBVHOptions;

    //
    // Parameters
    //
    uint mFrameCount = 0;
    bool mReservoirValid = false;
    uint2 mScreenRes = uint2(0, 0); // Store screen res to react to changes
    ResamplingMode mResamplingMode = ResamplingMode::SpartioTemporal;
    DirectLightingMode mDirectLightMode = DirectLightingMode::RTXDI;

    // Specular Trace Options
    uint mTraceMaxBounces = 10;          // Number of Specular/Transmissive bounces allowed
    bool mTraceRequireDiffuseMat = false; // Requires a diffuse part in addition to delta lobes

    //Defines GI
    uint mGIMaxBounces = 3; // Max Bounces for GI
    bool mAlphaTest = true;
    bool mGINEE = true;                 //Next event estimation in GI
    bool mGIMIS = true;                 //Use Multiple Importance Sampling
    bool mGIRussianRoulette = true;     //Use Russian Roulette in GI
   

    // Reservoir
    bool mUseReducedReservoirFormat = false;         // Use a reduced reservoir format TODO: Add
    bool mRebuildReservoirBuffer = false;            // Rebuild the reservoir buffer
    bool mClearReservoir = false;                    // Clears both reservoirs
    float mSampleRadiusAttenuation = 0.05f;          // Radius for the better defined attenuation
    uint mTemporalMaxAge = 20;                       // Max age of an temporal reservoir
    uint mSpartialSamples = 1;                       // Number of spartial samples
    uint mDisocclusionBoostSamples = 2;              // Number of spartial samples if no temporal surface was found
    float mSamplingRadius = 20.f;                    // Sampling radius in pixel
    float mRelativeDepthThreshold = 0.15f;           // Realtive Depth threshold (is neighbor 0.1 = 10% as near as the current depth)
    float mMaterialThreshold = 0.2f;                 // Maximum absolute difference in diffuse material probability
    float mNormalThreshold = 0.6f;                   // Cosine of maximum angle between both normals allowed
    float2 mJacobianMinMax = float2(1 / 10.f, 10.f); // Min and Max values that are allowed for the jacobian determinant (Angle/dist too
                                                     // different if lower/higher)
    BiasCorrectionMode mBiasCorrectionMode = BiasCorrectionMode::Basic; // Bias Correction Mode

    //
    // Buffer and Textures
    //
    ref<Buffer> mpPathSampelDataBuffer[2];   // Per pixel path sample info
    ref<Texture> mpReservoirBuffer[2];       // Buffers for the reservoir
    ref<Buffer> mpSurfaceBuffer[2];          // Buffer for surface data
    ref<Texture> mpVBuffer;                  // Work copy for VBuffer
    ref<Texture> mpViewDir;                  // View dir tex (needed for highly specular and transparent materials)
    ref<Texture> mpViewDirPrev;              // Previous View dir
    ref<Texture> mpRayDist;                  // Ray distance (needed for highly specular and transparent materials)
    ref<Texture> mpThp;                      // Throughput

    //
    // Render Passes/Programms
    //

    struct RayTraceProgramHelper
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;

        static const RayTraceProgramHelper create()
        {
            RayTraceProgramHelper r;
            r.pProgram = nullptr;
            r.pBindingTable = nullptr;
            r.pVars = nullptr;
            return r;
        }

        void initRTProgram(
            ref<Device> device,
            ref<Scene> scene,
            const std::string& shaderName,
            uint maxPayloadBytes,
            const Program::TypeConformanceList& globalTypeConformances
        );

        void initRTProgramWithShadowTest(
            ref<Device> device,
            ref<Scene> scene,
            const std::string& shaderName,
            uint maxPayloadBytes,
            const Program::TypeConformanceList& globalTypeConformances
        );
        
        void initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator);
    };

    RayTraceProgramHelper mTraceTransmissionDelta;
    RayTraceProgramHelper mFinalGatherSamplePass;

    ref<ComputePass> mpResamplingPass;   // Resampling Pass for all resampling modes
    ref<ComputePass> mpFinalShadingPass; // Final Shading Pass
};
