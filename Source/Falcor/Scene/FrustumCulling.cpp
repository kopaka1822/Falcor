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
        float3 front = math::normalize(eye - center);
        float3 right = math::normalize(math::cross(up, front));
        float3 u = math::cross(front, right);
        createFrustum(eye, right, u, front, aspect, fovY, near, far);

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

        //TODO Check if these are right or if camera coord system is flipped in an axis
        frustum.top = {camPos, math::normalize(math::cross(camU, frontTimesFar - camV * halfVSide))};
        frustum.bottom = {camPos, math::normalize(math::cross(frontTimesFar + camV * halfVSide, camU))};

        frustum.right = {camPos, math::normalize(math::cross(frontTimesFar - camU * halfHSide, camV))};
        frustum.left = {camPos, math::normalize(math::cross(camV, frontTimesFar + camU * halfHSide))};

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
        
    void FrustumCulling::resetDrawBuffer(ref<Device> pDevice, const std::vector<ref<Buffer>>& drawBuffer, const std::vector<uint>& drawBufferCount)
    {
        //Clear
        mDraw.clear();
        mDrawCount.clear();
        mValidDrawBuffer.clear();

        // Resize
        size_t size = drawBuffer.size();
        mDraw.resize(size);
        mDrawCount.resize(size);
        mValidDrawBuffer.resize(size);

        //TODO add staging buffers if that results in problems
        //Initialize
        for (uint i = 0; i < size; i++)
        {
            auto elementCount = drawBuffer[i]->getElementCount();
            uint testCount = drawBufferCount[i];
            std::vector<char> tmpData;
            tmpData.resize(elementCount);
            mDraw[i] = Buffer::create(pDevice, elementCount, Resource::BindFlags::IndirectArg, Buffer::CpuAccess::Write, tmpData.data());
            mDraw[i]->setName("FrustumCullingBuffer");
        }
    }

    void FrustumCulling::invalidateAllDrawBuffers() {
        for (uint i = 0; i < mValidDrawBuffer.size(); i++)
            mValidDrawBuffer[i] = false;
    }

    void FrustumCulling::updateDrawBuffer(uint index, const std::vector<DrawIndexedArguments> drawArguments)
    {
        uint buffSize = drawArguments.size();
        DrawIndexedArguments* drawArgs = (DrawIndexedArguments*)mDraw[index]->map(Buffer::MapType::Write);
        for (uint i = 0; i < buffSize; i++)
        {
            drawArgs[i] = drawArguments[i];
        }
        mDraw[index]->unmap();
        mDrawCount[index] = buffSize;
        mValidDrawBuffer[index] = true;
    }
}
