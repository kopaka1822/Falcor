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

class LightTrace : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(LightTrace, "LightTrace", "Test for Ligth Trace");

    static ref<LightTrace> create(ref<Device> pDevice, const Properties& props) { return make_ref<LightTrace>(pDevice, props); }

    LightTrace(ref<Device> pDevice, const Properties& props);

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


    //
    // Pointers
    //
    ref<Scene> mpScene;                                           // Scene Pointer
    ref<SampleGenerator> mpSampleGenerator;                       // GPU Sample Gen
    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    std::unique_ptr<CustomAccelerationStructure> mpLightTraceAS;      // Accel Pointer

    //
    // Parameters
    //
    uint mFrameCount = 0;
    uint2 mScreenRes = uint2(0, 0); // Store screen res to react to changes
    bool mOptionsChanged = false;

    uint mLightMaxBounces = 5; // Number of Light bounces
    uint mNumDispatchedPhotons = 100000; // Number of Photons dispatched
    uint mPhotonYExtent = 512;            // Dispatch Y extend
    uint mNumMaxPhotons = 2000000;
    uint mNumMaxPhotonsUI = mNumMaxPhotons;
    uint mCurrentPhotonCount = 1000000; // Gets data from GPU buffer
    float mASBuildBufferPhotonOverestimate = 1.15f;
    bool mChangePhotonLightBufferSize = false;


    ref<Buffer> mpLightTraceAABB;    // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpLightTraceData; // Additional Photon data (L)
    ref<Buffer> mpPhotonCounter;     // Counter for the number of lights
    ref<Buffer> mpPhotonCounterCPU;  // For showing the current number of photons in the UI

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

    RayTraceProgramHelper mGeneratePhotonPass;
    RayTraceProgramHelper mCollectPhotonPass;
};
