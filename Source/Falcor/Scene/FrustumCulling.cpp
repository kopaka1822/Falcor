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
#include "FrustumCulling.h"
#include "Utils/Math/FalcorMath.h"

namespace Falcor
{
    FrustumCulling::Plane::Plane(const float3 p1, const float3 N)
    {
        normal = math::normalize(N);
        distance = math::dot(normal, p1);
    }  

    float FrustumCulling::Plane::getSignedDistanceToPlane(const float3 point) const
    {
        return math::dot(normal, point) - distance;
    }

    FrustumCulling::FrustumCulling(const ref<Camera>& camera)
    {
        updateFrustum(camera);
    }

    FrustumCulling::FrustumCulling(float3 eye, float3 center, float3 up, float aspect, float fovY, float near, float far)
    {
        updateFrustum(eye, center, up, aspect, fovY, near, far);
    }

    FrustumCulling::FrustumCulling(float3 eye, float3 center, float3 up, float left, float right, float bottom, float top, float near, float far)
    {
        updateFrustum(eye, center, up, left, right,bottom, top, near, far);
    }

    void FrustumCulling::updateFrustum(const ref<Camera>& camera)
    {
        const CameraData& data = camera->getData();
        const float fovY = focalLengthToFovY(data.focalLength, data.frameHeight);
        createFrustum(
            data.posW, normalize(data.cameraU), normalize(data.cameraV), normalize(data.cameraW), data.aspectRatio, fovY, data.nearZ,
            data.farZ
        );

        invalidateAllDrawBuffers();
    }

    void FrustumCulling::updateFrustum(float3 eye, float3 center, float3 up, float aspect, float fovY, float near, float far)
    {
        float3 front = math::normalize(center - eye);
        float3 right = math::normalize(math::cross(front,up));
        float3 u = math::cross(right,front);
        createFrustum(eye, right, u, front, aspect, fovY, near, far);

        invalidateAllDrawBuffers();
    }

    void FrustumCulling::updateFrustum(float3 eye, float3 center, float3 up, float left, float right, float bottom, float top, float near, float far)
    {
        float3 front = math::normalize(center - eye);
        float3 r = math::normalize(math::cross(front, up));
        float3 u = math::cross(r, front);
        createFrustum(eye, r, u, front, left, right, bottom, top, near, far);

        invalidateAllDrawBuffers();
    }

    void FrustumCulling::createFrustum(float3 camPos, float3 camU, float3 camV, float3 camW, float aspect, float fovY, float near, float far)
    {
        Frustum frustum;
        const float halfVSide = far * math::tan(fovY * 0.5f);
        const float halfHSide = halfVSide * aspect;
        const float3 frontTimesFar = camW * far;

        frustum.near = {camPos + near * camW, camW};
        frustum.far = {camPos + frontTimesFar, -camW};

        frustum.top = {camPos, math::normalize(math::cross(camU, frontTimesFar - camV * halfVSide))};
        frustum.bottom = {camPos, math::normalize(math::cross(frontTimesFar + camV * halfVSide, camU))};

        frustum.right = {camPos, math::normalize(math::cross(frontTimesFar - camU * halfHSide, camV))};
        frustum.left = {camPos, math::normalize(math::cross(camV, frontTimesFar + camU * halfHSide))};

        mFrustum = frustum;
    }

    void FrustumCulling::createFrustum(float3 camPos, float3 camU, float3 camV, float3 camW, float left, float right, float bottom, float top, float near, float far)
    {
        Frustum frustum;

        frustum.near = {camPos + camW * near, camW};
        frustum.far = {camPos + camW * far, -camW};

        frustum.top = {camPos + camV * top, -camV};
        frustum.bottom = {camPos + camV * bottom, camV};

        frustum.right = {camPos + camU * right, -camU};
        frustum.left = {camPos + camU * left, camU};

        mFrustum = frustum;
    }
        
    bool FrustumCulling::isInFrontOfPlane(const Plane& plane, const AABB& aabb) const
    {
        float3 c = aabb.center();
        float3 e = aabb.maxPoint - c; // Positive extends

        // Compute the projection interval radius of b onto L(t) = b.c + t * p.n
        float r = math::dot(e, math::abs(plane.normal));

        //A intersection would be if the signed distance is between [-r, r], but we only want to check if
        // any part of the box is in front of the plane, hence we test if the distance is [-inf, -r]
        return -r <= plane.getSignedDistanceToPlane(c);
    }

    bool FrustumCulling::isInFrustum(const AABB& aabb) const
    {
        bool inPlane = true;
        inPlane &= isInFrontOfPlane(mFrustum.near, aabb);
        inPlane &= isInFrontOfPlane(mFrustum.far, aabb);

        inPlane &= isInFrontOfPlane(mFrustum.top, aabb);
        inPlane &= isInFrontOfPlane(mFrustum.bottom, aabb);

        inPlane &= isInFrontOfPlane(mFrustum.left, aabb);
        inPlane &= isInFrontOfPlane(mFrustum.right, aabb);

        return inPlane;
    }
        
    void FrustumCulling::createDrawBuffer(ref<Device> pDevice, RenderContext* pRenderContext, const std::vector<ref<Buffer>>& drawBuffer)
    {
        //Clear
        mDraw.clear();
        mStagingBuffer.clear();
        mDrawCount.clear();
        mValidDrawBuffer.clear();

        mpStagingFence = GpuFence::create(pDevice);
        mpStagingFence->breakStrongReferenceToDevice();

        // Resize
        size_t size = drawBuffer.size();
        mDraw.resize(size);
        mStagingBuffer.resize(size);
        mDrawCount.resize(size);
        mValidDrawBuffer.resize(size);

        //Initialize
        for (uint i = 0; i < size; i++)
        {
            auto elementCount = drawBuffer[i]->getElementCount(); // Byte size of original buffer

            mStagingBuffer[i].count = 0;
            mStagingBuffer[i].maxElementsBytes = elementCount;
            for (uint j = 0; j < kStagingFramesInFlight; j++)
            {
                mStagingBuffer[i].fenceWaitValue[j] = 0;
            }

            //CPU Buffers need initial data or they are not initialized properly
            std::vector<char> tmpData;
            tmpData.resize(elementCount * kStagingFramesInFlight);

            //Create Staging
            mStagingBuffer[i].buffer = Buffer::create(
                pDevice, elementCount * kStagingFramesInFlight, Resource::BindFlags::IndirectArg, Buffer::CpuAccess::Write, tmpData.data()
            );
            mStagingBuffer[i].buffer->setName("FrustumCullingBufferStaging");

            //Create Draw buffer
            mDraw[i] =
                Buffer::create(pDevice, elementCount, Resource::BindFlags::IndirectArg, Buffer::CpuAccess::None);
            mDraw[i]->setName("FrustumCullingBuffer");
            pRenderContext->copyBufferRegion(mDraw[i].get(), 0, drawBuffer[i].get(), 0, elementCount);  //Copy the original buffer for now
        }
    }

    void FrustumCulling::invalidateAllDrawBuffers() {
        for (uint i = 0; i < mValidDrawBuffer.size(); i++)
            mValidDrawBuffer[i] = false;
    }

    void FrustumCulling::updateDrawBuffer(RenderContext* pRenderContext, uint index, const std::vector<DrawIndexedArguments> drawArguments)
    {
        FALCOR_ASSERT(mStagingBuffer[index].buffer);
        FALCOR_ASSERT(mDraw[index]);

        uint buffSize = drawArguments.size();
        mValidDrawBuffer[index] = true;
        mDrawCount[index] = buffSize;

        if (buffSize <= 0)
            return;

        auto& stagingBuffer = mStagingBuffer[index].buffer;
        auto& stagingCount = mStagingBuffer[index].count;
        auto& fenceWaitVal = mStagingBuffer[index].fenceWaitValue[stagingCount];
        const auto& maxElements = mStagingBuffer[index].maxElementsBytes / sizeof(DrawIndexedArguments);
        const uint stagingOffset = maxElements * stagingCount;

        if (buffSize > maxElements)
            throw RuntimeError("Test");

        //Wait for the GPU to finish copying from kStagingFramesInFlight frames back
        mpStagingFence->syncCpu(fenceWaitVal);

        DrawIndexedArguments* drawArgs = (DrawIndexedArguments*)stagingBuffer->map(Buffer::MapType::Write);
        for (uint i = 0; i < buffSize; i++)
        {
            drawArgs[stagingOffset + i] = drawArguments[i];
        }

        //mpStagingFence->syncGpu(pRenderContext->getLowLevelData()->getCommandQueue());
        pRenderContext->copyBufferRegion(
            mDraw[index].get(), 0, stagingBuffer.get(), sizeof(DrawIndexedArguments) * stagingOffset,
            sizeof(DrawIndexedArguments) * buffSize
        );
        
        pRenderContext->flush(); //Sumbit pending commands before adding the Fence Signal

        fenceWaitVal = mpStagingFence->gpuSignal(pRenderContext->getLowLevelData()->getCommandQueue()); // Signal GPU for next wait
        
        stagingCount = (stagingCount + 1) % kStagingFramesInFlight;
    }

    void FrustumCulling::updateDrawBuffer(RenderContext* pRenderContext, uint index, const std::vector<DrawArguments> drawArguments)
    {
        FALCOR_ASSERT(mStagingBuffer[index].buffer);
        FALCOR_ASSERT(mDraw[index]);

        uint buffSize = drawArguments.size();
        mValidDrawBuffer[index] = true;
        mDrawCount[index] = buffSize;
        if (buffSize <= 0)
            return;

        auto& stagingBuffer = mStagingBuffer[index].buffer;
        auto& stagingCount = mStagingBuffer[index].count;
        auto& fenceWaitVal = mStagingBuffer[index].fenceWaitValue[stagingCount];
        const auto& maxElements = mStagingBuffer[index].maxElementsBytes / sizeof(DrawArguments);
        const uint stagingOffset = maxElements * stagingCount;

        // Wait for the GPU to finish copying from kStagingFramesInFlight frames back
        mpStagingFence->syncCpu(fenceWaitVal);

        DrawArguments* drawArgs = (DrawArguments*)stagingBuffer->map(Buffer::MapType::Write);
        for (uint i = 0; i < buffSize; i++)
        {
            drawArgs[stagingOffset + i] = drawArguments[i];
        }

        pRenderContext->copyBufferRegion(
            mDraw[index].get(), 0, stagingBuffer.get(), sizeof(DrawArguments) * stagingOffset,
            sizeof(DrawArguments) * buffSize
        );
        pRenderContext->flush(); //Sumbit pending commands before adding the Fence Signal

        fenceWaitVal = mpStagingFence->gpuSignal(pRenderContext->getLowLevelData()->getCommandQueue()); //Signal GPU for next wait

        stagingCount = (stagingCount + 1) % kStagingFramesInFlight;
    }

}
