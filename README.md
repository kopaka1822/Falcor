![](docs/images/teaser.png)

# Fast Pseudo Caustics with Ray Differentials

Rendering shadows cast by refractive surfaces such as glass is challenging because direct visibility queries between surface points and light sources generally violate the laws of refraction.
Conventional hard shadows produce visually unappealing results, while semi-transparent fake shadows fail to capture important optical effects.
In particular, they do not reproduce caustics, the focused light patterns that are characteristic of refractive materials.
We present a method for approximating refractive caustics from direct light connections using ray differentials.
Although our approach does not compute physically exact caustics, it achieves visually similar results at orders-of-magnitude lower computational cost than a ground-truth simulation.
Furthermore, the method is deterministic, temporally stable, and free of Monte Carlo noise, making it well suited for real-time rendering.

VMV 2026 paper

## Contents:

* [Demo User Interface](#demo-user-interface)
* [Source Code](#source-code)
* [Falcor Prerequisites](#falcor-prerequisites)
* [Building Falcor](#building-falcor)

## Demo User Interface

This project was implemented in NVIDIAs Falcor rendering framework.

You can download the executable demo from the [Releases Page](https://github.com/kopaka1822/Falcor/releases/tag/Caustic), or build the project by following the instructions in [Building Falcor](#building-falcor).

After downloading the demo from the releases page, you can execute it with the RunFalcor.bat file. In the Demo, you can configure the renderer after expanding the **GlassTracer** tab. In the **GlassTracer** tab you can configure the Whitted Ray Tracer:
* Render Scale: Use this dropdown to change the render resolution for DLSS
* Shadow Test: Hard Shadows, Fresnel Shadows, McGuire's Methods, Our Pseudo Caustic and Final Gather for the reference image (Go to OutputSwitch->Accumulate to accumulate a reference image).

You can navigate the camera with WASD and dragging the mouse for rotation.
Hold shift for more camera speed
QE for camera up and down
Space to pause the animation

## Source Code

The important files can be found in `Source/RenderPasses/GlassTracer/`:
* `GlassTracer.cpp/.h`: Whitted Ray Tracer
* `GlassTracer.rt.slang`: Shader code for the Whitted Ray Tracer
* `ShadowRay.slang`: Shader code for the shadow methods

Pseudo Code from the paper:
```c++
float shadow(float3 L, float3 Ps, float3 Ns, 
             float3 dPsdx, Light light) {
  ray.Origin = L; // ray
  ray.D = normalize(Ps - L);
  ray.TMin = 0; 
  ray.TMax = distance(L, Ps);
  rd.dPdx = 0; // ray differential
  rd.dDdx = 0;
  if (light.type == POINT_LIGHT) 
    rd.dDdx = focus(Ps, L, dPsdx);
  if (light.type == DIR_LIGHT)
    rd.dPdx = ortho(ray.D, dPsdx);
  
  float visibility = 1.0;
  while(visibility > 0.0) {
    RayQuery q;
    q.TraceRayInline(ray);
    if (q.hit_status == NO_HIT) break;
    
    float eta = getEta(q.hit);
    float3 N = getShadingNormal(q.hit);
    float3 I = ray.D; // incoming
    if(isExitingMedium(q.hit)) 
      I = refract(ray.D, N, 1.0 / eta);
    
    visibility *= (1 - getFresnel(I, N, eta));
    // Ige99 Eq. 10, 20, 17:
    rd.Transfer(I,q.rayT-ray.TMin,N);
    float3 dNdx = getNormalDiff(q.hit, rd.dPdx);
    rd.Refraction(I, N, dNdx, eta);
    
    ray.TMin = q.rayT; // next ray
  }
  
  // final transfer to Ps
  rd.Transfer(ray.D, ray.TMax-ray.TMin, Ns);
  float A_M = length(cross(rd.dPdx, rd.dPdy));
  float A_D = length(cross(dPsdx, dPsdy));
  float tau = pow(A_D/A_M, gamma); //gamma=0.25
  return tau * visibility;
}

float3 focus(float3 P, float3 L, float3 dPdx) {
  float3 d = P - L;
  return (dot(d,d)*dPdx - dot(d,dPdx)*d) 
         / pow(dot(d, d), 1.5);
}
float3 ortho(float3 D, float3 dPdx) {
  return dPdx - dot(D, dPdx) * D;
}
float getFresnel(float3 I, float3 N, float eta){
  float F0 = pow((1 - eta) / (1 + eta), 2);
  return F0+(1 - F0)*pow(1-abs(dot(I, N)), 5);
}
```

## Falcor Prerequisites
- Windows 10 version 20H2 (October 2020 Update) or newer, OS build revision .789 or newer
- Visual Studio 2022
- [Windows 10 SDK (10.0.19041.0) for Windows 10, version 2004](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk/)
- A GPU which supports DirectX Raytracing, such as the NVIDIA Titan V or GeForce RTX
- NVIDIA driver 466.11 or newer

Optional:
- Windows 10 Graphics Tools. To run DirectX 12 applications with the debug layer enabled, you must install this. There are two ways to install it:
    - Click the Windows button and type `Optional Features`, in the window that opens click `Add a feature` and select `Graphics Tools`.
    - Download an offline package from [here](https://docs.microsoft.com/en-us/windows-hardware/test/hlk/windows-hardware-lab-kit#supplemental-content-for-graphics-media-and-mean-time-between-failures-mtbf-tests). Choose a ZIP file that matches the OS version you are using (not the SDK version used for building Falcor). The ZIP includes a document which explains how to install the graphics tools.
- NVAPI, CUDA, OptiX

## Building Falcor
Falcor uses the [CMake](https://cmake.org) build system. Additional information on how to use Falcor with CMake is available in the [CMake](docs/development/cmake.md) development documentation page.

### Visual Studio
If you are working with Visual Studio 2022, you can setup a native Visual Studio solution by running `setup_vs2022.bat` after cloning this repository. The solution files are written to `build/windows-vs2022` and the binary output is located in `build/windows-vs2022/bin`.
