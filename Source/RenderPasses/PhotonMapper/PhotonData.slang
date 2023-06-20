import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

struct PhotonData
{
    float3 flux;
    uint packedNormal;
    float3 dir;
    uint _pad;

    __init(){
        flux = float3(0);
        dir = float3(0);
        packedNormal = 0;
    }
    
    __init(
uint4 data){
        flux.xy = float2(f16tof32((data.x >> 16) & 0xFFFF), f16tof32(data.x & 0xFFFF));
        flux.z = f16tof32((data.y >> 16) & 0xFFFF);
        dir.x = f16tof32(data.y & 0xFFFF);
        dir.yz = float2(f16tof32((data.z >> 16) & 0xFFFF), f16tof32(data.z & 0xFFFF));
        packedNormal = data.
w;
    }

    property
float3 normal
{
    get{ return
    decodeNormal2x16(packedNormal);
}
        set { packedNormal = encodeNormal2x16(newValue); }
    }

uint4 pack()
{
    uint4 data;
    data.x = (f32tof16(flux.x) << 16u) | f32tof16(flux.y);
    data.y = (f32tof16(flux.z) << 16u) | f32tof16(dir.x);
    data.z = (f32tof16(dir.y) << 16u) | f32tof16(dir.z);
    data.w = packedNormal;
    return data;
}
};

//Helper functions for hash cell calculation

// "Explodes" an integer, i.e. inserts a 0 between each bit.  Takes inputs up to 16 bit wide.
//      For example, 0b11111111 -> 0b1010101010101010
uint PM_IntegerExplode(uint x)
{
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

// Reverse of ReSTIR_IntegerExplode, i.e. takes every other bit in the integer and compresses
// those bits into a dense bit field. Takes 32-bit inputs, produces 16-bit outputs.
//    For example, 0b'abcdefgh' -> 0b'0000bdfh'
uint PM_IntegerCompact(uint x)
{
    x = (x & 0x11111111) | ((x & 0x44444444) >> 1);
    x = (x & 0x03030303) | ((x & 0x30303030) >> 2);
    x = (x & 0x000F000F) | ((x & 0x0F000F00) >> 4);
    x = (x & 0x000000FF) | ((x & 0x00FF0000) >> 8);
    return x;
}

// Converts a 2D position to a linear index following a Z-curve pattern.
uint PM_ZCurveToLinearIndex(uint2 xy)
{
    return PM_IntegerExplode(xy[0]) | (PM_IntegerExplode(xy[1]) << 1);
}
// Converts a linear to a 2D position following a Z-curve pattern.
uint2 PM_LinearIndexToZCurve(uint index)
{
    return uint2(
        PM_IntegerCompact(index),
        PM_IntegerCompact(index >> 1));
}
