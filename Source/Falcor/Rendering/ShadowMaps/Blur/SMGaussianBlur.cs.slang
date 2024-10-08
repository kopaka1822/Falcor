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
#ifdef _IS_CUBE
    RWTexture2D gSrcTex;    //Workaround as TextureCube cannot be bound as an Texture2D for SRV
#else
    Texture2D gSrcTex;
#endif

RWTexture2D gDstTex;

Buffer<float> weights;

float4 blur(int2 pixel)
{
#ifdef _HORIZONTAL_BLUR
    const int2 dir = int2(1, 0);
#elif defined _VERTICAL_BLUR
    const int2 dir = int2(0, 1);
#else
#error Please define either _HORIZONTAL_BLUR or _VERTICAL_BLUR
#endif

   const int2 offset = -(_KERNEL_WIDTH / 2) * dir;

    float4 c = float4(0,0,0,0);
    $for(i in Range(_KERNEL_WIDTH))
    {
        int2 idx = pixel + offset + i * dir;
        c += gSrcTex[clamp(idx, int2(0), _TEX_WIDTH)] * weights[i];
    }
    return c;
}

[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    float4 fragColor = float4(1.f, 1.f, 1.f, 1.f);
    fragColor = blur(pixel);
    gDstTex[pixel] = fragColor;
}
