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

bool IsWithinBounds(in int2 index, in int2 dimensions)
{
    return index.x >= 0 && index.y >= 0 && index.x < dimensions.x && index.y < dimensions.y;
}

uint Float2ToHalf(in float2 val)
{
    uint result = 0;
    result = f32tof16(val.x);
    result |= f32tof16(val.y) << 16;
    return result;
}

float2 HalfToFloat2(in uint val)
{
    float2 result;
    result.x = f16tof32(val);
    result.y = f16tof32(val >> 16);
    return result;
}

uint SmallestPowerOf2GreaterThan(in uint x)
{
    // Set all the bits behind the most significant non-zero bit in x to 1.
    // Essentially giving us the largest value that is smaller than the
    // next power of 2 we're looking for.
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;

    // Return the next power of two value.
    return x + 1;
}


// Returns float precision for a given float value.
// Values within (value -precision, value + precision) map to the same value. 
// Precision = exponentRange/MaxMantissaValue = (2^e+1 - 2^e) / (2^NumMantissaBits)
// Ref: https://blog.demofox.org/2017/11/21/floating-point-precision/
float FloatPrecision(in float x, in uint NumMantissaBits)
{
    // Find the exponent range the value is in.
    uint nextPowerOfTwo = SmallestPowerOf2GreaterThan(x);
    float exponentRange = nextPowerOfTwo - (nextPowerOfTwo >> 1);

    float MaxMantissaValue = 1 << NumMantissaBits;

    return exponentRange / MaxMantissaValue;
}

// Remap partial depth derivatives at z0 from [1,1] pixel offset to a new pixel offset.
float2 RemapDdxy(in float z0, in float2 ddxy, in float2 pixelOffset)
{
    // Perspective correction for non-linear depth interpolation.
    // Ref: https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/visibility-problem-depth-buffer-depth-interpolation
    // Given a linear depth interpolation for finding z at offset q along z0 to z1
    //      z =  1 / (1 / z0 * (1 - q) + 1 / z1 * q)
    // and z1 = z0 + ddxy, where z1 is at a unit pixel offset [1, 1]
    // z can be calculated via ddxy as
    //
    //      z = (z0 + ddxy) / (1 + (1-q) / z0 * ddxy) 
    float2 z = (z0 + ddxy) / (1 + ((1 - pixelOffset) / z0) * ddxy);
    return sign(pixelOffset) * (z - z0);
}

// Returns an approximate surface dimensions covered in a pixel. 
// This is a simplified model assuming pixel to pixel view angles are the same.
// z - linear depth of the surface at the pixel
// ddxy - partial depth derivatives
// tan_a - tangent of a per pixel view angle 
float2 ApproximateProjectedSurfaceDimensionsPerPixel(in float z, in float2 ddxy, in float tan_a)
{
    // Surface dimensions for a surface parallel at z.
    float2 dx = tan_a * z;
    
    // Using Pythagorean theorem approximate the surface dimensions given the ddxy.
    float2 w = sqrt(dx * dx + ddxy * ddxy);

    return w;
}
