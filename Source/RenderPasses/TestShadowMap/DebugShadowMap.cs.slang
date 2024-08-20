import SMStructs;

#ifndef SM_FORMAT
#define SM_FORMAT float2
#endif

Texture2D<SM_FORMAT> gRayShadowMap;
Texture2D<uint> gRayReverseSM;
Texture2D<float> gRayNeededMask;

Texture2D<uint> gShadowAccessTex;

RWTexture2D<float4> gOut;

cbuffer CB
{
    uint gDebugMode;
    float gBlendVal;
    float gMaxValue;
    float2 gBrightnessIncrease;
}

// color tables from: https://www.kennethmoreland.com/color-advice/
static const float4 inferno[8] = {
    float4(0, 0, 0, 0.0),
    float4(40, 11, 84, 0.14),
    float4(101, 21, 110, 0.29),
    float4(159, 42, 99, 0.43),
    float4(212, 72, 66, 0.57),
    float4(245, 125, 21, 0.71),
    float4(250, 193, 39, 0.86),
    float4(252, 255, 164, 1.0),
};

// v is normalized; Converts v to inferno color
float3 getColorFromTableInferno(float v) {   
	float4 c1 = inferno[0]; float4 c2 = inferno[1];
    for(int i = 1; i < 8; i++) {              
        c2 = inferno[i];                                    
		if(v < c2.w)
            break;                               
		c1 = c2;                                          
    }                                                     
    float3 c = lerp(c1.rgb/255.0, c2.rgb/255.0, (v - c1.w) / max(c2.w - c1.w, 0.0001));	
    return c;                                             
}

float4 shadowMapAccessOverlay(uint2 pixel,float4 prevOut){
    uint accesses = gShadowAccessTex[pixel];
    float normalizedAcc = float(accesses)/ gMaxValue;
    normalizedAcc = saturate(normalizedAcc);
    float3 heatmap = getColorFromTableInferno(normalizedAcc);

    float3 output = lerp(heatmap * gBrightnessIncrease.x, prevOut.xyz * gBrightnessIncrease.y, gBlendVal);
    return float4(output,1);
}

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    float4 output = float4(0,0,0,1);

    //Copy SM
    #if FILTER_SM_VARIANCE
    output = float4(gRayShadowMap[pixel],0,1);
    #else //FILTER_SM_EVSM || FILTER_SM_MSM
    output = float4(gRayShadowMap[pixel]);
    #endif

    if (gDebugMode == (uint)PathSMDebugModes::ReverseSM) {
        float val = gRayReverseSM[pixel];
        output.yz = output.xy;
        output.x = 0;
        output.xyz = lerp(float3(val, 0, 0) * gBrightnessIncrease.x, output.xyz * gBrightnessIncrease.y, gBlendVal);
        gOut[pixel] = float4(output.xyz, 1);
        return;
    }

    if(gDebugMode == (uint)PathSMDebugModes::RayNeededMask){
        float mask = gRayNeededMask[pixel];
        float3 maskF3 = float3(mask,0,0);
        output.xyz = lerp(maskF3 * gBrightnessIncrease.x, output.xyz * gBrightnessIncrease.y, gBlendVal);
        gOut[pixel] = float4(output.xyz,1);
        return;
    }

    if(gDebugMode != (uint)PathSMDebugModes::ShadowMap){
        output = shadowMapAccessOverlay(pixel, output);
    }

    gOut[pixel] = float4(output.xyz,1);
}
