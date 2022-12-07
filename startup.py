# Graphs
from falcor import *

def render_graph_VAO():
    g = RenderGraph('VAO')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('RTVAO.dll')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    VAO = createPass('VAO', {'enabled': True, 'kernelSize': 8, 'noiseSize': uint2(4,4), 'radius': 0.5, 'distribution': SampleDistribution.VanDerCorput, 'depthMode': DepthMode.Raytraced, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(VAO, 'VAO')
    ConvertFormat__ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0)', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat__, 'ConvertFormat__')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('LinearizeDepth_.linearDepth', 'VAO.depth2')
    g.addEdge('LinearizeDepth_', 'VAO')
    g.addEdge('LinearizeDepth.linearDepth', 'VAO.depth')
    g.addEdge('StochasticDepthMap.stochasticDepth', 'VAO.stochasticDepth')
    g.addEdge('VAO.ambientMap', 'ConvertFormat__.I0')
    g.addEdge('ConvertFormat__.out', 'CrossBilateralBlur.color')
    g.addEdge('VAO', 'ConvertFormat__')
    g.addEdge('ConvertFormat__', 'CrossBilateralBlur')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'VAO.normals')
    g.addEdge('GBufferRaster.instanceID', 'VAO.instanceID')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    g.markOutput('VAO.ambientMap')
    return g
m.addGraph(render_graph_VAO())
from falcor import *

def render_graph_SVAO():
    g = RenderGraph('SVAO')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('RTVAO.dll')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    ConvertFormat__ = createPass('ConvertFormat', {'formula': 'float4(I0[xy].x + I0[xy].y + I0[xy].z,0,0,0)', 'format': ResourceFormat.R8Unorm})
    g.addPass(ConvertFormat__, 'ConvertFormat__')
    SVAO = createPass('SVAO', {'radius': 0.5, 'primaryDepthMode': DepthMode.SingleDepth, 'secondaryDepthMode': DepthMode.StochasticDepth, 'exponent': 1.2999999523162842, 'rayPipeline': False, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(SVAO, 'SVAO')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    StochasticDepthMap_ = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap_, 'StochasticDepthMap_')
    SVAO2 = createPass('SVAO2')
    g.addPass(SVAO2, 'SVAO2')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('ConvertFormat__.out', 'CrossBilateralBlur.color')
    g.addEdge('ConvertFormat__', 'CrossBilateralBlur')
    g.addEdge('LinearizeDepth_.linearDepth', 'SVAO.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'SVAO.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SVAO.normals')
    g.addEdge('LinearizeDepth_', 'SVAO')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap_.depthMap')
    g.addEdge('SVAO.accessStencil', 'StochasticDepthMap_.stencilMask')
    g.addEdge('StochasticDepthMap_.stochasticDepth', 'SVAO2.stochasticDepth')
    g.addEdge('SVAO.ao', 'SVAO2.ao')
    g.addEdge('LinearizeDepth_.linearDepth', 'SVAO2.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'SVAO2.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'SVAO2.normals')
    g.addEdge('SVAO2.ao', 'ConvertFormat__.I0')
    g.addEdge('SVAO.stencil', 'SVAO2.aoStencil')
    g.addEdge('StochasticDepthMap_', 'SVAO2')
    g.addEdge('SVAO2', 'ConvertFormat__')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    g.markOutput('StochasticDepthMap_.stochasticDepth')
    return g
m.addGraph(render_graph_SVAO())
from falcor import *

def render_graph_RTVAO():
    g = RenderGraph('RTVAO')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('RTVAO.dll')
    RTVAO = createPass('RTVAO', {'radius': 0.5, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(RTVAO, 'RTVAO')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].x * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.faceNormalW', 'RTVAO.normals')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('LinearizeDepth.linearDepth', 'RTVAO.depth')
    g.addEdge('RTVAO.ambientMap', 'CrossBilateralBlur.color')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_RTVAO())
from falcor import *

def render_graph_HBAOPlus():
    g = RenderGraph('HBAOPlus')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('RTVAO.dll')
    DeinterleaveTexture = createPass('DeinterleaveTexture')
    g.addPass(DeinterleaveTexture, 'DeinterleaveTexture')
    HBAOPlus = createPass('HBAOPlus', {'radius': 1.0, 'depthMode': DepthMode.SingleDepth, 'depthBias': 0.10000000149011612, 'exponent': 2.0})
    g.addPass(HBAOPlus, 'HBAOPlus')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    InterleaveTexture = createPass('InterleaveTexture')
    g.addPass(InterleaveTexture, 'InterleaveTexture')
    DeinterleaveTexture_ = createPass('DeinterleaveTexture')
    g.addPass(DeinterleaveTexture_, 'DeinterleaveTexture_')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].x * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('StochasticDepthMap.stochasticDepth', 'HBAOPlus.stochasticDepth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('HBAOPlus.ambientMap', 'InterleaveTexture.texIn')
    g.addEdge('LinearizeDepth.linearDepth', 'DeinterleaveTexture.texIn')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('DeinterleaveTexture.texOut', 'HBAOPlus.depth')
    g.addEdge('LinearizeDepth_.linearDepth', 'DeinterleaveTexture_.texIn')
    g.addEdge('LinearizeDepth_', 'DeinterleaveTexture')
    g.addEdge('DeinterleaveTexture_.texOut', 'HBAOPlus.depth2')
    g.addEdge('DeinterleaveTexture', 'DeinterleaveTexture_')
    g.addEdge('DeinterleaveTexture_', 'HBAOPlus')
    g.addEdge('InterleaveTexture.texOut', 'CrossBilateralBlur.color')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('HBAOPlus', 'InterleaveTexture')
    g.addEdge('InterleaveTexture', 'CrossBilateralBlur')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'HBAOPlus.normals')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_HBAOPlus())
from falcor import *

def render_graph_HBAOPlusNoninterleaved():
    g = RenderGraph('HBAOPlusNoninterleaved')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
    loadRenderPassLibrary('RTVAO.dll')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    HBAOPlusNonInterleaved = createPass('HBAOPlusNonInterleaved', {'radius': 1.0, 'depthMode': DepthMode.SingleDepth, 'depthBias': 0.10000000149011612, 'exponent': 2.0})
    g.addPass(HBAOPlusNonInterleaved, 'HBAOPlusNonInterleaved')
    GBufferRaster = createPass('GBufferRaster', {'outputSize': IOSize.Default, 'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    LinearizeDepth = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    Ambient = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Ambient, 'Ambient')
    LinearizeDepth_ = createPass('LinearizeDepth', {'depthFormat': ResourceFormat.R32Float})
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    Diffuse = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(Diffuse, 'Diffuse')
    CrossBilateralBlur = createPass('CrossBilateralBlur', {'guardBand': 64})
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    StochasticDepthMap = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.20000000298023224, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
    g.addPass(StochasticDepthMap, 'StochasticDepthMap')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('LinearizeDepth_.linearDepth', 'HBAOPlusNonInterleaved.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'HBAOPlusNonInterleaved.depth')
    g.addEdge('GBufferRaster.diffuseOpacity', 'Diffuse.I1')
    g.addEdge('HBAOPlusNonInterleaved.ambientMap', 'CrossBilateralBlur.color')
    g.addEdge('CrossBilateralBlur.color', 'Ambient.I0')
    g.addEdge('CrossBilateralBlur.color', 'Diffuse.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('LinearizeDepth_', 'HBAOPlusNonInterleaved')
    g.addEdge('HBAOPlusNonInterleaved', 'CrossBilateralBlur')
    g.addEdge('GBufferRaster.depth', 'StochasticDepthMap.depthMap')
    g.addEdge('StochasticDepthMap.stochasticDepth', 'HBAOPlusNonInterleaved.stochasticDepth')
    g.addEdge('DepthPass.depth', 'GBufferRaster.depth')
    g.addEdge('GBufferRaster.faceNormalW', 'HBAOPlusNonInterleaved.normals')
    g.markOutput('Ambient.out')
    g.markOutput('Diffuse.out')
    return g
m.addGraph(render_graph_HBAOPlusNoninterleaved())

# Scene
m.loadScene('Sponza/sponza.obj')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useGridVolumes=True)
m.scene.camera.position = float3(22.887949,4.794842,2.332704)
m.scene.camera.target = float3(21.943356,4.708484,2.016026)
m.scene.camera.up = float3(0.000738,1.000000,0.000248)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(1900, 1000)
m.ui = True

# Clock Settings
m.clock.time = 0
m.clock.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# m.clock.frame = 0
m.clock.pause()

# Frame Capture
m.frameCapture.outputDir = '.'
m.frameCapture.baseFilename = 'Mogwai'

