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
//Accel
#include "Rendering/AccelerationStructure/CustomAccelerationStructure.h"

using namespace Falcor;

class PhotonMapper : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(PhotonMapper, "PhotonMapper", "A Photon Map Pass using the Ray Tracing API.");

    static ref<PhotonMapper> create(ref<Device> pDevice, const Properties& props) { return make_ref<PhotonMapper>(pDevice, props); }

    PhotonMapper(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

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

    /** Generate Photons
     */
    void generatePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData, bool clearBuffers = true);

     /** Handles the Photon Counter
     */
    void handlePhotonCounter(RenderContext* pRenderContext);

    /** Collect Photons at the final gather hit and scene hit
     */
    void collectPhotons(RenderContext* pRenderContext, const RenderData& renderData);

    /** Returs quad texture dimensions depending on the number of elements needed
     */
    void computeQuadTexSize(uint maxItems, uint& outWidth, uint& outHeight);

    /** Calcs the near plane for camera glints
    */
    void glintsCalcNearPlane();

    //
    // Constants
    //
    const ResourceFormat kViewDirFormat = ResourceFormat::RGBA32Float; // View Dir format

    //
    // Pointers
    //
    ref<Scene> mpScene;                     // Scene Pointer
    ref<SampleGenerator> mpSampleGenerator; // GPU Sample Gen
    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    std::unique_ptr<CustomAccelerationStructure> mpPhotonAS; // Accel Pointer

    //
    // Parameters
    //
    uint mFrameCount = 0;
    uint2 mScreenRes = uint2(0, 0); // Store screen res to react to changes
    

    // Specular Trace Options
    uint mTraceMaxBounces = 10;          // Number of Specular/Transmissive bounces allowed
    bool mTraceRequireDiffuseMat = true; // Requires a diffuse part in addition to delta lobes

    // Photon
    uint mPhotonMaxBounces = 10;                  // Number of Photon bounces
    uint mMaxCausticBounces = 10;                 // Number of diffuse bounces for a caustic
    float mPhotonRejection = 0.3f;                // Probability a global photon is stored
    uint mNumDispatchedPhotons = 2000000;         // Number of Photons dispatched
    uint mPhotonYExtent = 512;                    // Dispatch Y extend
    uint3 mNumMaxPhotons = uint3(400000, 100000, 100000); // Size of the photon buffer
    uint3 mNumMaxPhotonsUI = mNumMaxPhotons;
    uint3 mCurrentPhotonCount = uint3(1000000); // Gets data from GPU buffer
    float mASBuildBufferPhotonOverestimate = 1.15f;
    float2 mPhotonCollectionRadiusStart = float2(0.025f, 0.005f);
    float2 mPhotonCollectRadius = mPhotonCollectionRadiusStart; // Radius for collection
    bool mChangePhotonLightBufferSize = false;
    bool mPhotonUseAlphaTest = true;
    bool mPhotonAdjustShadingNormal = true;
    bool mEnableCausticPhotonCollection = true;
    bool mGenerationDeltaRejection = true;
    bool mGenerationDeltaRejectionRequireDiffPart = false;

    bool mUsePhotonCulling = true;
    uint mCullingHashBufferSizeBits = 20; // Number of Culling Hash bits
    bool mCullingUseFixedRadius = true;
    float mCullingCellRadius = 0.1f; // Radius used for the culling cells

    const uint kDynamicPhotonDispatchInitValue = 500224; // Start with 500 thousand photons
    bool mUseDynamicePhotonDispatchCount = true;         // Dynamically change the number of photons to fit the max photon number
    uint mPhotonDynamicDispatchMax = 2000000;            // Max value for dynamically dispatched photons
    float mPhotonDynamicGuardPercentage = 0.08f;  // Determines how much space of the buffer is used to guard against buffer overflows
    float mPhotonDynamicChangePercentage = 0.05f; // The percentage the buffer is increased/decreased per frame

    //Glints
    uint mGlintMaxPhotons = 10000;
    int2 mGlintTexRes = uint2(480, 270);
    int2 mGlintTexResUI = mGlintTexRes;
    float3x3 mCameraNearPlane;
    float3x3 mCameraNearPlaneDebug;
    float3 mGlintNormal = float3(0,1,0);
    float mCameraGlintNear = 0.8f;     //User defined near plane for the glints
    bool mCreateCamNearPlaneDebug = true;
    bool mShowDebugGlint = false;

    //
    // Buffer and Textures
    //

    ref<Buffer> mpPhotonAABB[3];    // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpPhotonData[3];    // Additional Photon data (flux, dir)
    ref<Buffer> mpPhotonCounter;    // Counter for the number of lights
    ref<Buffer> mpPhotonCounterCPU; // For showing the current number of photons in the UI
    ref<Texture> mpPhotonCullingMask; // Mask for photon culling
    ref<Texture> mpVBuffer;           // Work copy for VBuffer
    ref<Texture> mpViewDir;           // View dir tex (needed for highly specular and transparent materials)
    ref<Texture> mpThp;               // Throughput
    ref<Texture> mpGlintTex;          // Shows the glints
    ref<Buffer> mpGlintNumber;        //Number of glint hits

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

        void initRTCollectionProgram(
            ref<Device> device,
            ref<Scene> scene,
            const std::string& shaderName,
            uint maxPayloadBytes,
            const Program::TypeConformanceList& globalTypeConformances
        );

        void initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator);
    };

    RayTraceProgramHelper mTraceTransmissionDelta;
    RayTraceProgramHelper mGeneratePhotonPass;
    RayTraceProgramHelper mCollectPhotonPass;
};