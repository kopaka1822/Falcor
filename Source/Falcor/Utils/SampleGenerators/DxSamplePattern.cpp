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
#include "DxSamplePattern.h"
#include "Utils/Logger.h"

namespace Falcor
{
const float2 DxSamplePattern::kPattern8x[] = {
    // clang-format off
    { 1.0f / 16.0f, -3.0f / 16.0f},
    {-1.0f / 16.0f,  3.0f / 16.0f},
    { 5.0f / 16.0f,  1.0f / 16.0f},
    {-3.0f / 16.0f, -5.0f / 16.0f},
    {-5.0f / 16.0f,  5.0f / 16.0f},
    {-7.0f / 16.0f, -1.0f / 16.0f},
    { 3.0f / 16.0f,  7.0f / 16.0f},
    { 7.0f / 16.0f, -7.0f / 16.0f},
    // clang-format on
};

const float2 DxSamplePattern::kPattern2x[] = {
    { 0.25f, -0.25f}, 
    {-0.25f, 0.25f}, 
};

/*const float2 DxSamplePattern::kPattern4x[] = {
    {0.375f, -0.125f},  
     {-0.125f, 0.375f},  
    {0.1250f, -0.375f}, 
     {-0.375, 0.125f}, 
};*/

const float2 DxSamplePattern::kPattern4x[] = {
    {0.375f, 0.125f},
     {-0.125f, 0.375f},
    {0.1250f, -0.375f},
     {-0.375, -0.125f},
};



DxSamplePattern::DxSamplePattern(uint32_t sampleCount)
{
    kSampleCount = sampleCount;
    if(sampleCount == 2)
    {
        kPattern = kPattern2x;
    }
    else if(sampleCount == 4)
    {
        kPattern = kPattern4x;
    }
    else
    {
        kPattern = kPattern8x;
        kSampleCount = 8; // fix sample count to 8 if no other match
    }
}
} // namespace Falcor
