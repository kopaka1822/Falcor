from falcor import *

def render_graph_SVAO():
    g = RenderGraph('SVAO')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('VAOInterleaved.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('DLSSPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('NRDPass.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('ModulateIllumination.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('RTXDIPass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('VAORT.dll')
    loadRenderPassLibrary('SVAO.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('WriteStencil.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    loadRenderPassLibrary('VAO.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('RTAO.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('PathTracer.dll')
    loadRenderPassLibrary('RTAODenoiser.dll')
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
    SVAO = createPass('SVAO', {'radius': 0.5, 'primaryDepthMode': DepthMode.SingleDepth, 'secondaryDepthMode': DepthMode.StochasticDepth, 'exponent': 1.3, 'rayPipeline': False, 'guardBand': 64, 'thickness': 0.5})
    g.addPass(SVAO, 'SVAO')
    DepthPass = createPass('DepthPass', {'depthFormat': ResourceFormat.D32Float, 'useAlphaTest': True, 'cullMode': CullMode.CullBack})
    g.addPass(DepthPass, 'DepthPass')
    StochasticDepthMap_ = createPass('StochasticDepthMap', {'SampleCount': 4, 'Alpha': 0.2, 'CullMode': CullMode.CullBack, 'linearize': True, 'depthFormat': ResourceFormat.D24UnormS8})
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

SVAO = render_graph_SVAO()
try: m.addGraph(SVAO)
except NameError: None
