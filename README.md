# Stenciled Volumetric Ambient Occlusion (Falcor 5.0-preview)

## Setup

1. Follow the setup intructions for [Falcor](falcor_README.md).
2. In Visual Studio: Set Mogwai as startup project and use "-sC:/path_to_falcor/startup.py" as command arguments (right click mogwai in the solution explorer->properties->configuration properties/debugging/command arguments).
3. Start Mogwai with DebugD3D12 or ReleaseD3D12
4. Mogwai should automatically load the sponza scene and the default AO techniques (VAO and HBAO)

## Render Passes

In Mogwai you can select the active graph from the graph window:

| Active Graph | Renderers From Paper | Usage
|---|---|---|
| VAO | VAO, SD-VAO, RQ-VAO | Expand the VAO panel and change the depth mode to: SingleDepth (VAO), DualDepth, StochasticDepth (SD-VAO) and Raytraced (RQ-VAO).
| SVAO | SD-SVAO, RT-SVAO, RQ-SVAO | Expand the SVAO panel and change the secondary depth to: StochasticDepth (SD-SVAO) and Raytraced (RT/RQ-SVAO). Toggle the "Ray Pipeline" checkbox to select RT-SVAO (checked) or RQ-SVAO (unchecked).
| RTVAO | RT-VAO | 
| HBAOPlus | HBAO+, HBAO+SD | Expand the HBAOPlus panel and change the depth mode to: SingleDepth (HBAO+) or StochasticDepth (HBAO+SD)
| HBAOPlusNoninterleaved | (Not in Paper)  | This is an implementation of HBAO+ without the interleaved rendering. This version is slower and produced slightly different images because it does not work on downscaled versions of the depth buffer

There are a few additional parameters that can be changed:
| Renderer->Parameter | Description 
|---|---|
VAO->Thickness | Thickness model from the paper. On change this tries to match the power exponent accordingly
AO->Power Exponent | Artistic modification. The final AO will be AO^exponent
VAO->Color Map | When enabled, the AO pass outputs a colored map of valid (green), invalid (blue) and raytraced (red) samples. After enabling, change the graph output from Ambient.out to VAO.ambientMap and set the depth mode to Raytraced.

### Source Code

To browse the source code we recommennd to open the solution in Visual Studio 2022 and navigate to the respective render passes in the RenderPasses Folder.