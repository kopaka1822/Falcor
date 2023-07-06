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
 //Light samplers
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Rendering/Lights/EmissiveUniformSampler.h"

#include "Rendering/RTXDI/RTXDI.h"

#include "Rendering/AccelerationStructure/CustomAccelerationStructure.h"

using namespace Falcor;

class ReSTIR_FG : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_FG, "ReSTIR_FG", "Indirect light with Resampled Final Gathering");

    static ref<ReSTIR_FG> create(ref<Device> pDevice, const Properties& props) { return make_ref<ReSTIR_FG>(pDevice, props); }

    ReSTIR_FG(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    //GUI Structs and enum
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

    enum class RenderMode : uint
    {
        FinalGather = 0,
        ReSTIRFG = 1u,
    };

    //TODO add analytic/ random mode
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

    /** Prepares the custom Acceleration Structure
    */
    void prepareAccelerationStructure();
    /** Initializes all the ray tracing shaders
    */
    void prepareRayTracingShaders(RenderContext* pRenderContext);

    /** Trace Tranmissive and delta materials
    */
    void traceTransmissiveDelta(RenderContext* pRenderContext, const RenderData& renderData);

     /** Trace Scene for final gather hit
     */
    void getFinalGatherHitPass(RenderContext* pRenderContext, const RenderData& renderData);

    /** Generate Photon lights
     */
    void generatePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData, bool clearBuffers = true);

    /** Handles the Photon Counter 
    */
    void handlePhotonCounter(RenderContext* pRenderContext);

    /** Collect Photons at the final gather hit and scene hit
     */
    void collectPhotons(RenderContext* pRenderContext, const RenderData& renderData);

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
    //Constants
    //
    const ResourceFormat kViewDirFormat = ResourceFormat::RGBA32Float;  //View Dir format

    //
    //Pointers
    //
    ref<Scene> mpScene;                                                 //Scene Pointer
    ref<SampleGenerator> mpSampleGenerator;                             //GPU Sample Gen
    std::unique_ptr<RTXDI> mpRTXDI;                                     //Ptr to RTXDI for direct use
    RTXDI::Options mRTXDIOptions;                                      //Options for RTXDI

    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler;       //Light Sampler
    std::unique_ptr<CustomAccelerationStructure> mpPhotonAS;            //Accel Pointer

    //
    //Parameters
    //
    uint mFrameCount = 0;
    bool mReservoirValid = false;
    uint2 mScreenRes = uint2(0, 0);                                 //Store screen res to react to changes
    RenderMode mRenderMode = RenderMode::ReSTIRFG;
    ResamplingMode mResamplingMode = ResamplingMode::SpartioTemporal;
    DirectLightingMode mDirectLightMode = DirectLightingMode::RTXDI;

    //Specular Trace Options
    uint mTraceMaxBounces = 10;                                          //Number of Specular/Transmissive bounces allowed
    bool mTraceRequireDiffuseMat = true;                            //Requires a diffuse part in addition to delta lobes

    //Reservoir
    bool mUseReducedReservoirFormat = false;                        //Use a reduced reservoir format TODO: Add
    bool mRebuildReservoirBuffer = false;                            //Rebuild the reservoir buffer
    bool mClearReservoir = false;                                   //Clears both reservoirs
    float mSampleRadiusAttenuation = 0.05f;                         //Radius for the better defined attenuation
    uint mTemporalMaxAge = 20;                                      // Max age of an temporal reservoir
    uint mSpartialSamples = 1;                                      // Number of spartial samples
    uint mDisocclusionBoostSamples = 2;                             // Number of spartial samples if no temporal surface was found
    float mSamplingRadius = 20.f;                                   // Sampling radius in pixel
    float mRelativeDepthThreshold = 0.15f;                          // Realtive Depth threshold (is neighbor 0.1 = 10% as near as the current depth)
    float mMaterialThreshold = 0.2f;                                // Maximum absolute difference in diffuse material probability
    float mNormalThreshold = 0.6f;                                  // Cosine of maximum angle between both normals allowed
    BiasCorrectionMode mBiasCorrectionMode = BiasCorrectionMode::Basic; // Bias Correction Mode


    //Photon
    uint mPhotonMaxBounces = 10;                                    //Number of Photon bounces
    uint mMaxCausticBounces = 10;                                   //Number of diffuse bounces for a caustic
    float mPhotonRejection = 0.3f;                                  //Probability a global photon is stored
    uint mNumDispatchedPhotons = 2000000;                           //Number of Photons dispatched
    uint mPhotonYExtent = 512;                                      //Dispatch Y extend
    uint2 mNumMaxPhotons = uint2(400000, 100000);                   // Size of the photon buffer
    uint2 mNumMaxPhotonsUI = mNumMaxPhotons;
    uint2 mCurrentPhotonCount = uint2(1000000, 1000000);            // Gets data from GPU buffer
    float mASBuildBufferPhotonOverestimate = 1.15f;
    float2 mPhotonCollectionRadiusStart = float2(0.025f, 0.005f);
    float2 mPhotonCollectRadius = mPhotonCollectionRadiusStart;     // Radius for collection
    bool mChangePhotonLightBufferSize = false;
    bool mPhotonUseAlphaTest = true;
    bool mPhotonAdjustShadingNormal = true;
    bool mEnableCausticPhotonCollection = true;
    bool mGenerationDeltaRejection = true;
    bool mGenerationDeltaRejectionRequireDiffPart = false;

    bool mUsePhotonCulling = true;
    uint mCullingHashBufferSizeBits = 20;                           //Number of Culling Hash bits
    bool mCullingUseFixedRadius = true;
    float mCullingCellRadius = 0.1f;                                //Radius used for the culling cells
    bool mUseAlternativeCulling = false;

    const uint kDynamicPhotonDispatchInitValue = 500224; // Start with 500 thousand photons
    bool mUseDynamicePhotonDispatchCount = true;  // Dynamically change the number of photons to fit the max photon number
    uint mPhotonDynamicDispatchMax = 2000000;     // Max value for dynamically dispatched photons
    float mPhotonDynamicGuardPercentage = 0.08f;  // Determines how much space of the buffer is used to guard against buffer overflows
    float mPhotonDynamicChangePercentage = 0.05f; // The percentage the buffer is increased/decreased per frame

    //
    // Buffer and Textures
    //
    ref<Texture> mpFinalGatherSampleHitData;    //V-Buffer of the Final Gather sample hits
    ref<Buffer> mpFGSampelDataBuffer[2]; // Per pixel final gather sample information
    ref<Texture> mpReservoirBuffer[2];  // Buffers for the reservoir
    ref<Buffer> mpSurfaceBuffer[2];     // Buffer for surface data
    ref<Buffer> mpPhotonAABB[2];        // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpPhotonData[2];        // Additional Photon data (flux, dir)
    ref<Buffer> mpPhotonCounter;        // Counter for the number of lights
    ref<Buffer> mpPhotonCounterCPU;     // For showing the current number of photons in the UI
    ref<Texture> mpPhotonCullingMask; // Mask for photon culling
    ref<Texture> mpCausticRadiance;     // Caustic Radiance from the Collection pass
    ref<Texture> mpVBuffer;             //Work copy for VBuffer
    ref<Texture> mpViewDir;             //View dir tex (needed for highly specular and transparent materials)
    ref<Texture> mpViewDirPrev;         //Previous View dir
    ref<Texture> mpRayDist;             //Ray distance (needed for highly specular and transparent materials)
    ref<Texture> mpThp;                 //Throughput

    //
    //Render Passes/Programms
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

        void initRTProgram(ref<Device> device,ref<Scene> scene, const std::string& shaderName,uint maxPayloadBytes,const Program::TypeConformanceList& globalTypeConformances);

        void initRTCollectionProgram(ref<Device> device,ref<Scene> scene,const std::string& shaderName,uint maxPayloadBytes,const Program::TypeConformanceList& globalTypeConformances);

        void initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator);
    };

    RayTraceProgramHelper mTraceTransmissionDelta;
    RayTraceProgramHelper mFinalGatherSamplePass;
    RayTraceProgramHelper mGeneratePhotonPass;
    RayTraceProgramHelper mCollectPhotonPass;

    ref<ComputePass> mpResamplingPass;                  // Resampling Pass for all resampling modes
    ref<ComputePass> mpFinalShadingPass;                // Final Shading Pass
};
