
import Utils.Sampling.SampleGenerator;

cbuffer uni
{
    uint2 gGridSize;    //Size of the grid tex
    uint2 gDim;         //Dimensions of tex
    uint gSmoothStep; //0->None, 1-> Smooth, 2-> Smoother
    bool gBlackWhite;
    bool gUseImproved;   //Changes perlin version
    bool gUsePermutation; //Use the permuatation array
    uint gSeed;
    uint3 _pad;
}

Texture2D<float2> gGrid;
RWTexture2D<float4> gPerlin;

/*****************************/
/** Perlin Improved Version **/
/*****************************/


// Hash lookup table as defined by Ken Perlin.  This is a randomly arranged array of all numbers from 0-255 inclusive.
const static uint p[512] =
{
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23,
    190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177, 33,
    88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166,
    77, 146, 158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244,
    102, 143, 54, 65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196,
    135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123,
    5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42,
    223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9,
    129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246, 97, 228,
    251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107,
    49, 192, 214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254,
    138, 236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
    //Repeat 0-256 starts here
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23,
    190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177, 33,
    88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166,
    77, 146, 158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244,
    102, 143, 54, 65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196,
    135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123,
    5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42,
    223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9,
    129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246, 97, 228,
    251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107,
    49, 192, 214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254,
    138, 236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180
};

float fade(float t)
{
    if (gSmoothStep == 2)
        return ((t * (t * 6.0 - 15.0) + 10.0) * t * t * t);
    else if (gSmoothStep == 1)
        return (3.0 - t * 2.0) * t * t;
    else
        return t;
}

// Source: http://riven8192.blogspot.com/2010/08/calculate-perlinnoise-twice-as-fast.html
float grad(int hash, float x, float y, float z)
{
    switch (hash & 0xF)
    {
        case 0x0:
            return x + y;
        case 0x1:
            return -x + y;
        case 0x2:
            return x - y;
        case 0x3:
            return -x - y;
        case 0x4:
            return x + z;
        case 0x5:
            return -x + z;
        case 0x6:
            return x - z;
        case 0x7:
            return -x - z;
        case 0x8:
            return y + z;
        case 0x9:
            return -y + z;
        case 0xA:
            return y - z;
        case 0xB:
            return -y - z;
        case 0xC:
            return y + x;
        case 0xD:
            return -y + z;
        case 0xE:
            return y - x;
        case 0xF:
            return -y - z;
        default:
            return 0; // never happens
    }
}

float perlinImproved(float x, float y, float z, uint2 pixel)
{
    
    uint xi = uint(x) & 255;
    uint yi = uint(y) & 255;
    uint zi = uint(z) & 255;
    double xf = x - xi;
    double yf = y - yi;
    double zf = z - zi;

    float u = fade(xf);
    float v = fade(yf);
    float w = fade(zf);
   
    int aaa, aba, baa, bba;
     
    
    if (!gUsePermutation)
    {
        uint maxNum = 0xFFFF;
        
        SampleGenerator sg0 = SampleGenerator(pixel, gSeed);
        SampleGenerator sg1 = SampleGenerator(pixel + uint2(0, 1), gSeed);
        SampleGenerator sg2 = SampleGenerator(pixel + uint2(1, 0), gSeed);
        SampleGenerator sg3 = SampleGenerator(pixel + uint2(1, 1), gSeed);
    
        aaa = int(sampleNext1D(sg0) * maxNum);
        aba = int(sampleNext1D(sg1) * maxNum);
        baa = int(sampleNext1D(sg2) * maxNum);
        bba = int(sampleNext1D(sg3) * maxNum);
    }
    else
    {
        aaa = p[p[p[xi] + yi] + zi];
        aba = p[p[p[xi] + yi + 1] + zi];
        baa = p[p[p[xi + 1] + yi] + zi];
        bba = p[p[p[xi + 1] + yi + 1] + zi];
    }
    
    
    float x1, x2, y1, y2;
    x1 = lerp(grad(aaa, xf, yf, zf),                    // The gradient function calculates the dot product between a pseudorandom
                grad(baa, xf - 1, yf, zf),              // gradient vector and the vector from the input coordinate to the 8
                u);                                     // surrounding points in its unit cube.
    x2 = lerp(grad(aba, xf, yf - 1, zf),                // This is all then lerped together as a sort of weighted average based on the faded (u,v,w)
                grad(bba, xf - 1, yf - 1, zf),          // values we made earlier.
                  u);
    y1 = lerp(x1, x2, v);

    return y1;    
}


/*****************************/
/*** Perlin Simple Version ***/
/*****************************/

// Computes the dot product of the distance and gradient vectors.
float dotGridGradient(int ix, int iy, float x, float y)
{
    // Get gradient from integer coordinates
    float2 gradient = normalize(gGrid[uint2(ix, iy)]);

    // Compute the distance vector
    float dx = x - (float) ix;
    float dy = y - (float) iy;

    // Compute the dot-product
    return (dx * gradient.x + dy * gradient.y);
}

float interpolate(float a0, float a1, float w)
{
    w = clamp(w, 0.0, 1.0);

    if (gSmoothStep == 2)
        return (a1 - a0) * ((w * (w * 6.0 - 15.0) + 10.0) * w * w * w) + a0;
    else if (gSmoothStep == 1)
        return (a1 - a0) * (3.0 - w * 2.0) * w * w + a0;
    else
        return (a1 - a0) * w + a0;
}

// x,y -> [0,1]
float perlinOrig(float x, float y)
{
    int x0 = int(floor(x));
    int x1 = x0 + 1;
    int y0 = int(floor(y));
    int y1 = y0 + 1;

    // Determine interpolation weights
    // Could also use higher order polynomial/s-curve here
    float sx = x - (float) x0;
    float sy = y - (float) y0;
    
    float n0 = dotGridGradient(x0, y0, x, y);
    float n1 = dotGridGradient(x1, y0, x, y);
    float ix0 = interpolate(n0, n1, sx);

    n0 = dotGridGradient(x0, y1, x, y);
    n1 = dotGridGradient(x1, y1, x, y);
    float ix1 = interpolate(n0, n1, sx);

    float value = interpolate(ix0, ix1, sy);
    
    return value;    //Value =  -1,1
}


[numthreads(16, 16, 1)]
void main(uint2 dTid : SV_DispatchThreadID)
{
    if (any(dTid > gDim))
        return;
    
    float2 relPos = float2(dTid) / gDim;
    float2 posF = relPos * gGridSize;

    float value;
    if (gUseImproved)
    {
        value = perlinImproved(posF.x, posF.y, 0.f, uint2(floor(posF)));
    }
    else
        value = perlinOrig(posF.x, posF.y);

    if (gBlackWhite)
    {
        value = value * 0.5f + 0.5f;
        gPerlin[dTid] = float4(value, value, value, 1.f);
    }
    else
    {
        float2 ret = value > 0 ? float2(value, 0.f) : float2(0, value * -1.f);
        gPerlin[dTid] = float4(ret, 0.f, 1.f);
    }

}
