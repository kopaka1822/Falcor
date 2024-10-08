import ErrorOverlayEnums;

Texture2D<float4> gImage;
Texture2D<float4> gRef;
Texture2D<float4> gMSE;
Texture2D<float4> gFLIP;

RWTexture2D<float4> gOut;

cbuffer CB
{
    uint4 gImageModes;      // Modes for images. 0: TopLeft, 1: TopRight, 2: BottomLeft, 3: BottomRight
    uint gValidImages;      // Number of valid images
    float3 gLineColor;      // Color of the line
    float2 gLinePosX;       // Line position; x = min , y = max
    float2 gLinePosY;       // Line position; x = min , y = max
}

float4 getValueFromType(uint overlayMode, uint2 pixel) {
    float4 val = float4(0);
    switch (overlayMode) {
    case (uint)ErrorOverlayMode::Image:
        val = gImage[pixel];
        break;
    case (uint)ErrorOverlayMode::Reference:
        val = gRef[pixel];
        break;
    case (uint)ErrorOverlayMode::MSE:
        val = gMSE[pixel];
        break;
    case (uint)ErrorOverlayMode::FLIP:
        val = gFLIP[pixel];
        break;
    case (uint)ErrorOverlayMode::None:
        val = float4(gLineColor, 1.0);
        break;
    }
    return val;
}

float4 oneActiveImage(uint2 pixel) {
    return getValueFromType(gImageModes.x, pixel);
}

float4 twoActiveImage(uint2 pixel) {
    uint type = (uint)ErrorOverlayMode::None; // None = Line Color
    if (pixel.x < gLinePosX.x) {
        type = gImageModes.x;
    } else if (pixel.x > gLinePosX.y) {
        type = gImageModes.y;
    }
    return getValueFromType(type, pixel);
}

float4 threeActiveImage(uint2 pixel) {
    uint type = (uint)ErrorOverlayMode::None; // None = Line Color
    if (pixel.x < gLinePosX.x && pixel.y < gLinePosY.x) {
        type = gImageModes.x;
    } else if (pixel.x > gLinePosX.y && pixel.y < gLinePosY.x) {
        type = gImageModes.y;
    } else if (pixel.y > gLinePosY.y) {
        type = gImageModes.z;
    }
    return getValueFromType(type, pixel);
}

float4 fourActiveImage(uint2 pixel) {
    uint type = (uint)ErrorOverlayMode::None; // None = Line Color
    if (pixel.x < gLinePosX.x && pixel.y < gLinePosY.x) {
        type = gImageModes.x;
    } else if (pixel.x > gLinePosX.y && pixel.y < gLinePosY.x) {
        type = gImageModes.y;
    } else if (pixel.x < gLinePosX.x && pixel.y > gLinePosY.y) {
        type = gImageModes.z;
    } else if (pixel.x > gLinePosX.y && pixel.y > gLinePosY.y) {
        type = gImageModes.w;
    }
    return getValueFromType(type, pixel);
}

[numthreads(16, 16, 1)]
void main(uint2 pixel: SV_DispatchThreadID)
{
    float4 output = float4(0.0);

    switch (gValidImages) {
    case 1:
        output = oneActiveImage(pixel);
        break;
    case 2:
        output = twoActiveImage(pixel);
        break;
    case 3:
        output = threeActiveImage(pixel);
        break;
    case 4:
        output = fourActiveImage(pixel);
        break;
    }

    gOut[pixel] = output;
}
