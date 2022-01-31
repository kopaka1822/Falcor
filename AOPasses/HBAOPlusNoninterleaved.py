from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    loadRenderPassLibrary('CSM.dll')
    loadRenderPassLibrary('BSDFViewer.dll')
    loadRenderPassLibrary('MinimalPathTracer.dll')
    loadRenderPassLibrary('AccumulatePass.dll')
    loadRenderPassLibrary('Antialiasing.dll')
    loadRenderPassLibrary('BlitPass.dll')
    loadRenderPassLibrary('ConvertFormat.dll')
    loadRenderPassLibrary('CrossBilateralBlur.dll')
    loadRenderPassLibrary('DebugPasses.dll')
    loadRenderPassLibrary('DeinterleaveTexture.dll')
    loadRenderPassLibrary('ForwardLightingPass.dll')
    loadRenderPassLibrary('DepthPass.dll')
    loadRenderPassLibrary('DepthPeelPass.dll')
    loadRenderPassLibrary('DualDepthPass.dll')
    loadRenderPassLibrary('ErrorMeasurePass.dll')
    loadRenderPassLibrary('FLIPPass.dll')
    loadRenderPassLibrary('GBuffer.dll')
    loadRenderPassLibrary('TemporalDelayPass.dll')
    loadRenderPassLibrary('SSAO.dll')
    loadRenderPassLibrary('HBAOPlus.dll')
    loadRenderPassLibrary('HBAOPlusInterleaved.dll')
    loadRenderPassLibrary('ImageLoader.dll')
    loadRenderPassLibrary('HBAOPlusNonInterleaved.dll')
    loadRenderPassLibrary('InterleaveTexture.dll')
    loadRenderPassLibrary('LinearizeDepth.dll')
    loadRenderPassLibrary('MegakernelPathTracer.dll')
    loadRenderPassLibrary('NVIDIADenoiser.dll')
    loadRenderPassLibrary('OptixDenoiser.dll')
    loadRenderPassLibrary('PassLibraryTemplate.dll')
    loadRenderPassLibrary('PixelInspectorPass.dll')
    loadRenderPassLibrary('SceneDebugger.dll')
    loadRenderPassLibrary('SimplePostFX.dll')
    loadRenderPassLibrary('SkyBox.dll')
    loadRenderPassLibrary('StochasticDepthMap.dll')
    loadRenderPassLibrary('SVGFPass.dll')
    loadRenderPassLibrary('Texture2DArrayExtract.dll')
    loadRenderPassLibrary('TestPasses.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('Utils.dll')
    loadRenderPassLibrary('WhittedRayTracer.dll')
    HBAOPlusNonInterleaved = createPass('HBAOPlusNonInterleaved', {'radius': 0.10000000149011612, 'depthMode': DepthMode.DualDepth, 'depthBias': 0.10000000149011612, 'exponent': 2.0})
    g.addPass(HBAOPlusNonInterleaved, 'HBAOPlusNonInterleaved')
    DepthPeelPass = createPass('DepthPeelPass')
    g.addPass(DepthPeelPass, 'DepthPeelPass')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': SamplePattern.Center, 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': CullMode.CullBack})
    g.addPass(GBufferRaster, 'GBufferRaster')
    ConvertFormat = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat, 'ConvertFormat')
    LinearizeDepth = createPass('LinearizeDepth')
    g.addPass(LinearizeDepth, 'LinearizeDepth')
    ConvertFormat_ = createPass('ConvertFormat', {'formula': 'I0[xy].xxxx * I1[xy]', 'format': ResourceFormat.RGBA8UnormSrgb})
    g.addPass(ConvertFormat_, 'ConvertFormat_')
    LinearizeDepth_ = createPass('LinearizeDepth')
    g.addPass(LinearizeDepth_, 'LinearizeDepth_')
    CrossBilateralBlur = createPass('CrossBilateralBlur')
    g.addPass(CrossBilateralBlur, 'CrossBilateralBlur')
    g.addEdge('GBufferRaster.depth', 'DepthPeelPass.depth')
    g.addEdge('GBufferRaster.depth', 'LinearizeDepth.depth')
    g.addEdge('DepthPeelPass.depth2', 'LinearizeDepth_.depth')
    g.addEdge('LinearizeDepth_.linearDepth', 'HBAOPlusNonInterleaved.depth2')
    g.addEdge('LinearizeDepth.linearDepth', 'HBAOPlusNonInterleaved.depth')
    g.addEdge('GBufferRaster.normW', 'HBAOPlusNonInterleaved.normals')
    g.addEdge('GBufferRaster.diffuseOpacity', 'ConvertFormat_.I1')
    g.addEdge('HBAOPlusNonInterleaved.ambientMap', 'CrossBilateralBlur.color')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat.I0')
    g.addEdge('CrossBilateralBlur.color', 'ConvertFormat_.I0')
    g.addEdge('LinearizeDepth.linearDepth', 'CrossBilateralBlur.linear depth')
    g.addEdge('GBufferRaster', 'DepthPeelPass')
    g.addEdge('DepthPeelPass', 'LinearizeDepth')
    g.addEdge('LinearizeDepth', 'LinearizeDepth_')
    g.addEdge('LinearizeDepth_', 'HBAOPlusNonInterleaved')
    g.addEdge('HBAOPlusNonInterleaved', 'CrossBilateralBlur')
    g.markOutput('ConvertFormat.out')
    g.markOutput('ConvertFormat_.out')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)
except NameError: None
